// Test-only heap profiler (cargo feature "heapprof"): tracks every Rust allocation of at least MIN bytes with its call stack and, every
// 20 s, writes the stacks holding the most live bytes to $T2R_HEAPPROF (default heapprof.txt). Stack entries are offsets into the
// executable (subtract the load base) so `addr2line -f -C -e <unstripped t2r-admin>` names them. Also counts all live Rust bytes.
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::io::Write;
use std::sync::atomic::{AtomicIsize, AtomicUsize, Ordering};
use std::sync::Mutex;

const MIN: usize = 16 * 1024;
const DEPTH: usize = 28;

pub struct Prof;

static TOTAL: AtomicIsize = AtomicIsize::new(0);
static COUNT: AtomicIsize = AtomicIsize::new(0);
static BASE: AtomicUsize = AtomicUsize::new(0);

struct Tab {
    live: HashMap<usize, (usize, u32)>,
    site_of: HashMap<[usize; DEPTH], u32>,
    sites: Vec<[usize; DEPTH]>,
}

static TAB: Mutex<Option<Tab>> = Mutex::new(None);

thread_local! {
    static INSIDE: Cell<bool> = const { Cell::new(false) };
}

#[repr(C)]
struct UnwindCtx {
    _p: [u8; 0],
}
type TraceFn = extern "C" fn(*mut UnwindCtx, *mut c_void) -> i32;
extern "C" {
    fn _Unwind_Backtrace(f: TraceFn, arg: *mut c_void) -> i32;
    fn _Unwind_GetIP(ctx: *mut UnwindCtx) -> usize;
}

struct Walk {
    ips: [usize; DEPTH],
    n: usize,
    skip: usize,
}

extern "C" fn step(ctx: *mut UnwindCtx, arg: *mut c_void) -> i32 {
    let w = unsafe { &mut *(arg as *mut Walk) };
    if w.skip > 0 {
        w.skip -= 1;
        return 0;
    }
    if w.n >= DEPTH {
        return 5; // _URC_END_OF_STACK
    }
    w.ips[w.n] = unsafe { _Unwind_GetIP(ctx) };
    w.n += 1;
    0
}

fn guard<R>(f: impl FnOnce() -> R) -> Option<R> {
    INSIDE.try_with(|c| {
        if c.get() {
            return None;
        }
        c.set(true);
        let r = f();
        c.set(false);
        Some(r)
    })
    .ok()
    .flatten()
}

fn note_alloc(p: *mut u8, size: usize) {
    if p.is_null() {
        return;
    }
    TOTAL.fetch_add(size as isize, Ordering::Relaxed);
    COUNT.fetch_add(1, Ordering::Relaxed);
    if size < MIN {
        return;
    }
    guard(|| {
        let mut w = Walk { ips: [0; DEPTH], n: 0, skip: 2 };
        unsafe {
            _Unwind_Backtrace(step, &mut w as *mut Walk as *mut c_void);
        }
        let base = BASE.load(Ordering::Relaxed);
        for ip in w.ips.iter_mut().take(w.n) {
            *ip = ip.wrapping_sub(base);
        }
        let mut g = TAB.lock().unwrap_or_else(|e| e.into_inner());
        let t = g.get_or_insert_with(|| Tab { live: HashMap::new(), site_of: HashMap::new(), sites: Vec::new() });
        let id = match t.site_of.get(&w.ips) {
            Some(id) => *id,
            None => {
                let id = t.sites.len() as u32;
                t.sites.push(w.ips);
                t.site_of.insert(w.ips, id);
                id
            }
        };
        t.live.insert(p as usize, (size, id));
    });
}

fn note_free(p: *mut u8, size: usize) {
    TOTAL.fetch_sub(size as isize, Ordering::Relaxed);
    COUNT.fetch_sub(1, Ordering::Relaxed);
    if size < MIN {
        return;
    }
    guard(|| {
        let mut g = TAB.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(t) = g.as_mut() {
            t.live.remove(&(p as usize));
        }
    });
}

unsafe impl GlobalAlloc for Prof {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let p = System.alloc(l);
        note_alloc(p, l.size());
        p
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        let p = System.alloc_zeroed(l);
        note_alloc(p, l.size());
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        note_free(p, l.size());
        System.dealloc(p, l)
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        let q = System.realloc(p, l, new);
        if !q.is_null() {
            note_free(p, l.size());
            note_alloc(q, new);
        }
        q
    }
}

fn exe_base() -> usize {
    let exe = std::fs::read_link("/proc/self/exe").unwrap_or_default();
    let maps = std::fs::read_to_string("/proc/self/maps").unwrap_or_default();
    for l in maps.lines() {
        if l.ends_with(&*exe.to_string_lossy()) {
            if let Some(a) = l.split('-').next() {
                return usize::from_str_radix(a, 16).unwrap_or(0);
            }
        }
    }
    0
}

fn rss_kb() -> u64 {
    let s = std::fs::read_to_string("/proc/self/status").unwrap_or_default();
    s.lines().find(|l| l.starts_with("VmRSS:")).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok()).unwrap_or(0)
}

pub fn start() {
    BASE.store(exe_base(), Ordering::Relaxed);
    let path = std::env::var("T2R_HEAPPROF").unwrap_or_else(|_| "heapprof.txt".into());
    std::thread::spawn(move || loop {
        std::thread::sleep(std::time::Duration::from_secs(20));
        let rows = guard(|| {
            let g = TAB.lock().unwrap_or_else(|e| e.into_inner());
            let Some(t) = g.as_ref() else { return Vec::new() };
            let mut by: HashMap<u32, (usize, usize)> = HashMap::new();
            for (sz, id) in t.live.values() {
                let e = by.entry(*id).or_default();
                e.0 += sz;
                e.1 += 1;
            }
            let mut v: Vec<(usize, usize, [usize; DEPTH])> = by.into_iter().map(|(id, (b, n))| (b, n, t.sites[id as usize])).collect();
            v.sort_by(|a, b| b.0.cmp(&a.0));
            v.truncate(25);
            v
        })
        .unwrap_or_default();
        let _ = guard(|| {
            let mut out = String::new();
            let tracked: usize = rows.iter().map(|r| r.0).sum();
            out.push_str(&format!(
                "ts {} rss_kb {} rust_live_bytes {} rust_live_allocs {} top25_big_bytes {}\n",
                crate::timeutil::now() as u64,
                rss_kb(),
                TOTAL.load(Ordering::Relaxed),
                COUNT.load(Ordering::Relaxed),
                tracked
            ));
            for (b, n, ips) in &rows {
                out.push_str(&format!("site bytes {} count {} ips", b, n));
                for ip in ips.iter().filter(|x| **x != 0) {
                    out.push_str(&format!(" {:x}", ip));
                }
                out.push('\n');
            }
            if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&path) {
                let _ = f.write_all(out.as_bytes());
            }
        });
    });
}
