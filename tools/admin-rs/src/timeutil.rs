use std::time::{SystemTime, UNIX_EPOCH};

pub fn now() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub fn mtime(md: &std::fs::Metadata) -> f64 {
    md.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub fn mktime(y: i32, mo: i32, d: i32, h: i32, mi: i32, s: i32) -> f64 {
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    tm.tm_isdst = -1;
    unsafe { libc::mktime(&mut tm) as f64 }
}

pub struct Local {
    pub year: i32,
    pub month: i32,
    pub day: i32,
    pub hour: i32,
    pub min: i32,
    pub sec: i32,
}

pub fn localtime(t: f64) -> Local {
    let secs: i64 = t.floor() as i64;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe {
        libc::localtime_r(&secs, &mut tm);
    }
    Local { year: tm.tm_year + 1900, month: tm.tm_mon + 1, day: tm.tm_mday, hour: tm.tm_hour, min: tm.tm_min, sec: tm.tm_sec }
}

pub fn date_str(t: f64) -> String {
    let l = localtime(t);
    format!("{:04}-{:02}-{:02}", l.year, l.month, l.day)
}

pub fn datetime_str(t: f64) -> String {
    let l = localtime(t);
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", l.year, l.month, l.day, l.hour, l.min, l.sec)
}

/// "30 Nov 18:00 UTC" (the end of a timed ban, as the player reads it: one zone for everyone)
pub fn utc_short(t: f64) -> String {
    let secs: i64 = t.floor() as i64;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    unsafe {
        libc::gmtime_r(&secs, &mut tm);
    }
    const M: [&str; 12] = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];
    format!("{} {} {:02}:{:02} UTC", tm.tm_mday, M[(tm.tm_mon.clamp(0, 11)) as usize], tm.tm_hour, tm.tm_min)
}

pub fn stamp_str(t: f64) -> String {
    let l = localtime(t);
    format!("{:04}{:02}{:02}-{:02}{:02}{:02}", l.year, l.month, l.day, l.hour, l.min, l.sec)
}

const MONTHS: [&str; 12] = ["jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"];
const DAYS: [&str; 7] = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"];

fn days_in(y: i32, m: i32) -> i32 {
    match m {
        2 if (y % 4 == 0 && y % 100 != 0) || y % 400 == 0 => 29,
        2 => 28,
        4 | 6 | 9 | 11 => 30,
        _ => 31,
    }
}

pub fn parse_ctime(s: &str) -> Option<(i32, i32, i32, i32, i32, i32)> {
    let mut it = s.split_whitespace();
    let wd = it.next()?.to_ascii_lowercase();
    if !DAYS.contains(&wd.as_str()) {
        return None;
    }
    let mon = it.next()?.to_ascii_lowercase();
    let mo = MONTHS.iter().position(|m| *m == mon)? as i32 + 1;
    let d: i32 = it.next()?.parse().ok()?;
    let hms: Vec<&str> = it.next()?.split(':').collect();
    if hms.len() != 3 {
        return None;
    }
    let h: i32 = hms[0].parse().ok()?;
    let mi: i32 = hms[1].parse().ok()?;
    let sec: i32 = hms[2].parse().ok()?;
    let y: i32 = it.next()?.parse().ok()?;
    if it.next().is_some() || !(1..=12).contains(&mo) || d < 1 || d > days_in(y, mo) || h > 23 || mi > 59 || sec > 61 {
        return None;
    }
    Some((y, mo, d, h, mi, sec))
}
