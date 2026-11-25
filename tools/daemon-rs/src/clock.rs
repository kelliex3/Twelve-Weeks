use std::sync::OnceLock;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

static BASE: OnceLock<(f64, Instant)> = OnceLock::new();

fn wall() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub fn now() -> f64 {
    let (t0, i0) = BASE.get_or_init(|| (wall(), Instant::now()));
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
