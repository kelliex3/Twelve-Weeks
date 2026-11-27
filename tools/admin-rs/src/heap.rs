//! The global allocator (the libc malloc), wrapped to count the live bytes the admin site's Rust code holds, and the glibc tuning the
//! daemon uses: at most 2 malloc arenas and a malloc_trim each minute. musl's malloc kept the memory of the big short-lived lists
//! (player list, clans, JSON answers) mapped after they were freed: RSS climbed by ~13 MB a minute while the live data stayed flat.
//! Once a minute a "MEM" line (live heap, RSS, trim result) goes to stderr (admin.out), which tells a growing data set apart from
//! memory the allocator keeps.
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicI64, Ordering};

pub struct Counting;

pub static LIVE: AtomicI64 = AtomicI64::new(0);

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc(l) };
        if !p.is_null() {
            LIVE.fetch_add(l.size() as i64, Ordering::Relaxed);
        }
        p
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        let p = unsafe { System.alloc_zeroed(l) };
        if !p.is_null() {
            LIVE.fetch_add(l.size() as i64, Ordering::Relaxed);
        }
        p
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) {
        unsafe { System.dealloc(p, l) };
        LIVE.fetch_sub(l.size() as i64, Ordering::Relaxed);
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, new: usize) -> *mut u8 {
        let q = unsafe { System.realloc(p, l, new) };
        if !q.is_null() {
            LIVE.fetch_add(new as i64 - l.size() as i64, Ordering::Relaxed);
        }
        q
    }
}

/// Process start, before the threads: on glibc at most T2R_MALLOC_ARENAS (default 2) malloc arenas.
pub fn tune() {
    #[cfg(target_env = "gnu")]
    {
        let n: i32 = std::env::var("T2R_MALLOC_ARENAS").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(2);
        if n > 0 {
            unsafe { libc::mallopt(libc::M_ARENA_MAX, n) };
        }
    }
}

/// glibc malloc_trim(0): the free pages of every arena go back to the kernel. "" elsewhere.
fn trim() -> &'static str {
    #[cfg(target_env = "gnu")]
    {
        if std::env::var("T2R_MALLOC_TRIM").map(|v| v.trim() == "0").unwrap_or(false) {
            return "";
        }
        return if unsafe { libc::malloc_trim(0) } != 0 { " trimmed" } else { " trim-none" };
    }
    #[allow(unreachable_code)]
    ""
}

fn rss_mb() -> i64 {
    let s = std::fs::read_to_string("/proc/self/statm").unwrap_or_default();
    s.split_whitespace().nth(1).and_then(|n| n.parse::<i64>().ok()).unwrap_or(0) * 4096 / 1048576
}

/// The "mem" thread: once a minute trim, then log the live heap and RSS.
pub fn start() {
    let _ = std::thread::Builder::new().name("mem".into()).spawn(|| loop {
        std::thread::sleep(std::time::Duration::from_secs(60));
        let before = rss_mb();
        let t = trim();
        eprintln!("MEM live {} MB rss {} -> {} MB{}", LIVE.load(Ordering::Relaxed) / 1048576, before, rss_mb(), t);
    });
}
