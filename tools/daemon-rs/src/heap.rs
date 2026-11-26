//! The global allocator (mimalloc; libc malloc with --features sysalloc), wrapped to count the live bytes the daemon's Rust code holds, so the status loop can log the live heap
//! next to SQLite's own memory (C malloc, not counted here) and the process RSS ("MEM" line once a minute): it tells a growing data set
//! apart from memory the allocator keeps mapped after it was freed.
#[cfg(not(feature = "sysalloc"))]
use mimalloc::MiMalloc as System;
#[cfg(feature = "sysalloc")]
use std::alloc::System; // (diagnostic build: libc malloc as before, only counted)
use std::alloc::{GlobalAlloc, Layout};
use std::cell::Cell;
use std::sync::atomic::{AtomicI64, Ordering};

pub struct Counting;

pub static LIVE: AtomicI64 = AtomicI64::new(0);

/// Net bytes per kind of work (allocated minus freed on threads doing it): 0..=255 = the op of the TCP batch being handled, then the
/// named threads. A kind whose total keeps climbing holds what it allocated.
const KINDS: [&str; 13] = ["tcp-idle", "status", "matchmaker", "voice-room", "clan-scheduler", "presence", "admin", "log", "clan-timer", "sendq", "named", "main", "?"];
pub const K_TCP: u16 = 256;
const K_UNKNOWN: u16 = 256 + 12;
static NET: [AtomicI64; 256 + 13] = [const { AtomicI64::new(0) }; 256 + 13];

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
    let _ = KIND.try_with(|c| c.set(k.min(K_UNKNOWN)));
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
