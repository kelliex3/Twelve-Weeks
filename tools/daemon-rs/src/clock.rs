use std::sync::OnceLock;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

static BASE: OnceLock<(f64, Instant)> = OnceLock::new();

fn wall() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

static FROZEN: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// Tests only (selftest): freeze `now()` at `t` (0.0 = run free again).
pub fn freeze(t: f64) {
    FROZEN.store(t.to_bits(), std::sync::atomic::Ordering::Relaxed);
}

pub fn now() -> f64 {
    let f = FROZEN.load(std::sync::atomic::Ordering::Relaxed);
    if f != 0 {
        return f64::from_bits(f);
    }
    let (t0, i0) = BASE.get_or_init(|| (test_epoch().unwrap_or_else(wall), Instant::now()));
    t0 + i0.elapsed().as_secs_f64()
}

struct Tm {
    sec: i32,
    min: i32,
    hour: i32,
    mday: i32,
    mon: i32,
    year: i32,
    wday: i32,
}

#[allow(deprecated)]
fn local(t: i64) -> Tm {
    let tt = t as libc::time_t;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    let ok = unsafe { !libc::localtime_r(&tt, &mut tm).is_null() };
    if !ok {
        return Tm { sec: 0, min: 0, hour: 0, mday: 1, mon: 0, year: 70, wday: 4 };
    }
    Tm { sec: tm.tm_sec, min: tm.tm_min, hour: tm.tm_hour, mday: tm.tm_mday, mon: tm.tm_mon, year: tm.tm_year, wday: tm.tm_wday }
}

pub fn hms(t: f64) -> String {
    let tm = local(t.floor() as i64);
    format!("{:02}:{:02}:{:02}", tm.hour, tm.min, tm.sec)
}

pub fn hms_now() -> String {
    hms(wall())
}

const DAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
const MONTHS: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

pub fn ctime(t: f64) -> String {
    let tm = local(t.floor() as i64);
    format!(
        "{} {} {:2} {:02}:{:02}:{:02} {}",
        DAYS[tm.wday.rem_euclid(7) as usize],
        MONTHS[tm.mon.rem_euclid(12) as usize],
        tm.mday,
        tm.hour,
        tm.min,
        tm.sec,
        tm.year + 1900
    )
}

pub fn ctime_now() -> String {
    ctime(wall())
}

static MONO0: OnceLock<Instant> = OnceLock::new();

/// `time.monotonic()` (seconds since an arbitrary start).
pub fn mono() -> f64 {
    let f = FROZEN.load(std::sync::atomic::Ordering::Relaxed);
    if f != 0 {
        return f64::from_bits(f); // (selftest: the Python harness maps time.monotonic to the same frozen time)
    }
    MONO0.get_or_init(Instant::now).elapsed().as_secs_f64() + 1000.0
}

/// The real wall clock (`wall_time()` in daemon.py): for values other programs compare with their own clock.
pub fn wall_time() -> f64 {
    if test_epoch().is_some() {
        return now();
    }
    wall()
}

/// `T2R_TEST_EPOCH=<unix seconds>` (test hook, verify/py/seedhook.py): the wall clock starts at that value and runs on the monotonic clock.
fn test_epoch() -> Option<f64> {
    static E: OnceLock<Option<f64>> = OnceLock::new();
    *E.get_or_init(|| std::env::var("T2R_TEST_EPOCH").ok().and_then(|v| v.trim().parse::<f64>().ok()))
}
