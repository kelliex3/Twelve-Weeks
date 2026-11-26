//! The global allocator (the libc malloc; mimalloc with --features mimalloc), wrapped to count the live bytes the daemon's Rust code holds, so the status loop can log the live heap
//! next to SQLite's own memory (C malloc, not counted here) and the process RSS ("MEM" line once a minute): it tells a growing data set
//! apart from memory the allocator keeps mapped after it was freed.
#[cfg(feature = "mimalloc")]
use mimalloc::MiMalloc as System;
#[cfg(not(feature = "mimalloc"))]
use std::alloc::System;
use std::alloc::{GlobalAlloc, Layout};
use std::cell::Cell;
use std::sync::atomic::{AtomicI64, Ordering};

pub struct Counting;

pub static LIVE: AtomicI64 = AtomicI64::new(0);

/// Net bytes per kind of work (allocated minus freed on threads doing it): 0..=255 = the op of the TCP batch being handled, then the
/// named threads. A kind whose total keeps climbing holds what it allocated.
const KINDS: [&str; 14] = ["tcp-idle", "status", "matchmaker", "voice-room", "clan-scheduler", "presence", "admin", "log", "clan-timer", "sendq", "named", "main", "?", "memwatch"];
pub const K_TCP: u16 = 256;
const K_UNKNOWN: u16 = 256 + 12;
static NET: [AtomicI64; 256 + 14] = [const { AtomicI64::new(0) }; 256 + 14];

thread_local! {
    static KIND: Cell<u16> = const { Cell::new(K_UNKNOWN) };
}

#[inline]
fn charge(n: i64) {
    LIVE.fetch_add(n, Ordering::Relaxed);
    let k = KIND.try_with(|c| c.get()).unwrap_or(K_UNKNOWN);
    NET[k as usize].fetch_add(n, Ordering::Relaxed);
}

/// The kind the current thread's allocations count to (an op 0..=255 or K_TCP).
pub fn set_kind(k: u16) {
    let _ = KIND.try_with(|c| c.set(k.min(256 + 13)));
}

/// Kind of a named thread (its name as std::thread::Builder::name got it).
pub fn set_kind_name(name: &str) {
    let i = match name {
        "status" => 1,
        "matchmaker" => 2,
        "voice-room" => 3,
        "clan-scheduler" => 4,
        "presence" => 5,
        "admin" | "admin-conn" => 6,
        "log" => 7,
        "clan-timer" => 8,
        "sendq" => 9,
        "main" => 11,
        "memwatch" => 13,
        _ => 10,
    };
    set_kind(256 + i);
}

fn kind_name(k: usize) -> String {
    if k < 256 {
        format!("op{:02x}", k)
    } else {
        KINDS[k - 256].to_string()
    }
}

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc(l) };
        if !p.is_null() {
            charge(l.size() as i64);
        }
        p
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc_zeroed(l) };
        if !p.is_null() {
            charge(l.size() as i64);
        }
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        unsafe { System.dealloc(p, l) };
        charge(-(l.size() as i64));
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        let q = unsafe { System.realloc(p, l, new) };
        if !q.is_null() {
            charge(new as i64 - l.size() as i64);
        }
        q
    }
}

/// Process start, before the threads: on glibc at most T2R_MALLOC_ARENAS (default 2) malloc arenas. musl's malloc kept freed memory
/// mapped (live 11-26: heap 68 MB, RSS 424 MB and climbing); glibc's can hand it back (trim).
pub fn tune() {
    #[cfg(all(target_env = "gnu", not(feature = "mimalloc")))]
    {
        let n: i32 = std::env::var("T2R_MALLOC_ARENAS").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(2);
        if n > 0 {
            unsafe { libc::mallopt(libc::M_ARENA_MAX, n) };
        }
    }
}

/// glibc malloc_trim(0): free memory at the top of every arena and the free pages inside them go back to the kernel. "" elsewhere.
pub fn trim() -> &'static str {
    #[cfg(all(target_env = "gnu", not(feature = "mimalloc")))]
    {
        if std::env::var("T2R_MALLOC_TRIM").map(|v| v.trim() == "0").unwrap_or(false) {
            return "";
        }
        return if unsafe { libc::malloc_trim(0) } != 0 { " trimmed" } else { " trim-none" };
    }
    #[allow(unreachable_code)]
    ""
}

fn statm_rss_mb() -> i64 {
    let s = std::fs::read_to_string("/proc/self/statm").unwrap_or_default();
    s.split_whitespace().nth(1).and_then(|n| n.parse::<i64>().ok()).unwrap_or(0) * 4096 / 1048576
}

/// "memwatch" thread: every 200 ms the heap, SQLite and RSS against their lowest value of the last 2 s; a rise of T2R_MEMWATCH_MB
/// (default 48) or more logs "MEM-JUMP" (also on stderr, so it is there when the kernel kills the process right after) with the
/// kinds that grew in those 2 s. At most one line per 2 s.
pub fn watch() {
    set_kind_name("memwatch");
    let lim: i64 = std::env::var("T2R_MEMWATCH_MB").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(48);
    if lim <= 0 {
        return;
    }
    const W: usize = 10;
    let mb = |b: i64| b / 1048576;
    let snap = || -> Vec<i64> { NET.iter().map(|a| a.load(Ordering::Relaxed)).collect() };
    let mut ring: std::collections::VecDeque<(i64, i64, i64, Vec<i64>)> = std::collections::VecDeque::with_capacity(W + 1);
    let mut quiet = 0usize;
    loop {
        std::thread::sleep(std::time::Duration::from_millis(200));
        let heap = LIVE.load(Ordering::Relaxed);
        let sq = unsafe { rusqlite::ffi::sqlite3_memory_used() };
        let rss = statm_rss_mb();
        let now = snap();
        if quiet > 0 {
            quiet -= 1;
        } else if let Some(i) = (0..ring.len()).min_by_key(|&i| ring[i].2) {
            let (h0, q0, r0, ref k0) = ring[i];
            let (hmin, qmin) = (ring.iter().map(|x| x.0).min().unwrap_or(h0), ring.iter().map(|x| x.1).min().unwrap_or(q0));
            if rss - r0 >= lim || mb(heap - hmin) >= lim || mb(sq - qmin) >= lim {
                let mut d: Vec<(usize, i64)> = now.iter().zip(k0.iter()).enumerate().map(|(k, (a, b))| (k, a - b)).filter(|x| x.1.abs() >= 1048576).collect();
                d.sort_by_key(|x| -x.1.abs());
                let kinds = d.iter().take(8).map(|(k, n)| format!("{} {:+.1}", kind_name(*k), *n as f64 / 1048576.0)).collect::<Vec<_>>().join(", ");
                let line = format!("heap {}->{} MB sqlite {}->{} MB rss {}->{} MB in 2 s; grew: {}", mb(hmin), mb(heap), mb(qmin), mb(sq), r0, rss, kinds);
                eprintln!("MEM-JUMP {}", line);
                crate::dlog!("MEM-JUMP", line);
                quiet = W;
            }
        }
        ring.push_back((heap, sq, rss, now));
        if ring.len() > W {
            ring.pop_front();
        }
    }
}

fn status_kb(s: &str, key: &str) -> i64 {
    s.lines().find(|l| l.starts_with(key)).and_then(|l| l.split_whitespace().nth(1)).and_then(|n| n.parse().ok()).unwrap_or(0)
}

/// "kinds op b6 +12.3, status -3.1, ..." (net MB since start, the 10 largest by size)
pub fn kinds_line() -> String {
    let mut v: Vec<(usize, i64)> = NET.iter().enumerate().map(|(i, a)| (i, a.load(Ordering::Relaxed))).filter(|x| x.1 != 0).collect();
    v.sort_by_key(|x| -x.1.abs());
    v.iter().take(12).map(|(k, n)| format!("{} {:+.1}", kind_name(*k), *n as f64 / 1048576.0)).collect::<Vec<_>>().join(", ")
}

/// "heap 151 MB sqlite 20 MB (high 31 MB) rss 300 MB anon 290 MB threads 210"
pub fn line() -> String {
    let mb = |b: i64| b / (1024 * 1024);
    let st = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
    let (sq, sq_hi) = unsafe { (rusqlite::ffi::sqlite3_memory_used(), rusqlite::ffi::sqlite3_memory_highwater(1)) };
    format!(
        "heap {} MB sqlite {} MB (high {} MB) rss {} MB anon {} MB threads {}",
        mb(LIVE.load(Ordering::Relaxed)),
        mb(sq),
        mb(sq_hi),
        status_kb(&st, "VmRSS:") / 1024,
        status_kb(&st, "RssAnon:") / 1024,
        status_kb(&st, "Threads:")
    )
}
