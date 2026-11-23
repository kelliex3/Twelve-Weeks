use serde::de::DeserializeOwned;
use serde::Serialize;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

#[macro_export]
macro_rules! log {
    ($($arg:tt)*) => {{
        println!("[{}] {}", $crate::util::stamp(), format!($($arg)*));
    }};
}

pub fn now_secs() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub fn civil(secs: i64) -> (i64, u32, u32, u32, u32, u32) {
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    let y = yoe + era * 400 + i64::from(m <= 2);
    (y, m, d, (rem / 3600) as u32, (rem % 3600 / 60) as u32, (rem % 60) as u32)
}

pub fn stamp() -> String {
    let (y, mo, d, h, mi, s) = civil(now_secs() as i64);
    format!("{y:04}-{mo:02}-{d:02} {h:02}:{mi:02}:{s:02}")
}

pub fn iso_utc() -> String {
    let (y, mo, d, h, mi, s) = civil(now_secs() as i64);
    format!("{y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{s:02}")
}

pub fn today() -> String {
    let (y, mo, d, ..) = civil(now_secs() as i64);
    format!("{y:04}-{mo:02}-{d:02}")
}

pub fn read_json<T: DeserializeOwned>(path: &Path) -> Option<T> {
    let data = fs::read(path).ok()?;
    serde_json::from_slice(&data).ok()
}

pub fn to_pretty<T: Serialize>(value: &T) -> io::Result<Vec<u8>> {
    let mut out = Vec::new();
    let fmt = serde_json::ser::PrettyFormatter::with_indent(b" ");
    let mut ser = serde_json::Serializer::with_formatter(&mut out, fmt);
    value.serialize(&mut ser).map_err(io::Error::other)?;
    Ok(out)
}

pub fn write_json<T: Serialize>(path: &Path, value: &T) -> io::Result<()> {
    let data = to_pretty(value)?;
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    fs::write(&tmp, &data)?;
    fs::rename(&tmp, path)
}

pub fn home() -> Option<PathBuf> {
    if let Some(h) = std::env::var_os("HOME").filter(|h| !h.is_empty()) {
        return Some(PathBuf::from(h));
    }
    let status = fs::read_to_string("/proc/self/status").ok()?;
    let uid = status.lines().find_map(|l| l.strip_prefix("Uid:"))?.split_whitespace().next()?.to_string();
    let passwd = fs::read_to_string("/etc/passwd").ok()?;
    passwd.lines().find_map(|l| {
        let f: Vec<&str> = l.split(':').collect();
        (f.len() >= 6 && f[2] == uid).then(|| PathBuf::from(f[5]))
    })
}

pub fn expand_user(p: &str) -> PathBuf {
    if p == "~" || p.starts_with("~/") {
        if let Some(h) = home() {
            return h.join(p.trim_start_matches('~').trim_start_matches('/'));
        }
    }
    PathBuf::from(p)
}

pub fn truncate_chars(s: &str, n: usize) -> String {
    s.chars().take(n).collect()
}

pub fn parse_id(v: &serde_json::Value) -> Option<u64> {
    match v {
        serde_json::Value::Number(n) => n.as_u64(),
        serde_json::Value::String(s) => s.trim().parse().ok(),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn civil_dates() {
        assert_eq!(civil(0), (1970, 1, 1, 0, 0, 0));
        assert_eq!(civil(1795410474), (2026, 11, 23, 5, 7, 54));
        assert_eq!(civil(951782400), (2000, 2, 29, 0, 0, 0));
    }

    #[test]
    fn pretty_matches_python_indent_1() {
        let v: serde_json::Value = serde_json::from_str(r#"{"a":1,"b":{"c":[1,2]},"d":[],"e":{}}"#).unwrap();
        let s = String::from_utf8(to_pretty(&v).unwrap()).unwrap();
        assert_eq!(s, "{\n \"a\": 1,\n \"b\": {\n  \"c\": [\n   1,\n   2\n  ]\n },\n \"d\": [],\n \"e\": {}\n}");
    }
}
