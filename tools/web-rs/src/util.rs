use serde_json::Value;
use std::collections::HashSet;
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

pub fn now() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub fn civil(days: i64) -> (i64, u32, u32) {
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

pub fn utc_date(secs: i64) -> String {
    let (y, m, d) = civil(secs.div_euclid(86400));
    format!("{:04}-{:02}-{:02}", y, m, d)
}

pub fn http_date(secs: i64) -> String {
    let wd = ["Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"][secs.div_euclid(86400).rem_euclid(7) as usize];
    let (y, m, d) = civil(secs.div_euclid(86400));
    let rem = secs.rem_euclid(86400);
    let mon = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"][(m as usize).clamp(1, 12) - 1];
    format!("{}, {:02} {} {} {:02}:{:02}:{:02} GMT", wd, d, mon, y, rem / 3600, (rem / 60) % 60, rem % 60)
}

pub struct Local {
    pub year: i64,
    pub mon: u32,
    pub day: u32,
    pub hour: u32,
    pub min: u32,
    pub sec: u32,
}

#[allow(deprecated)]
pub fn local(secs: i64) -> Local {
    let t = secs as libc::time_t;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    let ok = unsafe { !libc::localtime_r(&t, &mut tm).is_null() };
    if ok {
        Local {
            year: tm.tm_year as i64 + 1900,
            mon: (tm.tm_mon + 1) as u32,
            day: tm.tm_mday as u32,
            hour: tm.tm_hour as u32,
            min: tm.tm_min as u32,
            sec: tm.tm_sec as u32,
        }
    } else {
        let (year, mon, day) = civil(secs.div_euclid(86400));
        let rem = secs.rem_euclid(86400);
        Local { year, mon, day, hour: (rem / 3600) as u32, min: ((rem / 60) % 60) as u32, sec: (rem % 60) as u32 }
    }
}

pub fn local_stamp(secs: i64) -> String {
    let l = local(secs);
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", l.year, l.mon, l.day, l.hour, l.min, l.sec)
}

pub fn local_clock() -> String {
    let l = local(now() as i64);
    format!("{:02}:{:02}:{:02}", l.hour, l.min, l.sec)
}

pub fn py_space(c: char) -> bool {
    c.is_whitespace() || ('\x1c'..='\x1f').contains(&c)
}

pub fn py_strip(s: &str) -> &str {
    s.trim_matches(py_space)
}

pub fn is_digits(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit())
}

pub fn take_chars(s: &str, n: usize) -> &str {
    match s.char_indices().nth(n) {
        Some((i, _)) => &s[..i],
        None => s,
    }
}

pub fn round_half_even(x: f64) -> i64 {
    x.round_ties_even() as i64
}

pub fn round2(x: f64) -> f64 {
    format!("{:.2}", x).parse().unwrap_or(x)
}

pub fn py_int(v: &Value) -> Option<i64> {
    match v {
        Value::Bool(b) => Some(*b as i64),
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                Some(i)
            } else if let Some(u) = n.as_u64() {
                i64::try_from(u).ok()
            } else {
                n.as_f64().filter(|f| f.is_finite()).map(|f| f.trunc() as i64)
            }
        }
        Value::String(s) => parse_py_int(s),
        _ => None,
    }
}

pub fn parse_py_int(s: &str) -> Option<i64> {
    let t = py_strip(s);
    let (neg, digits) = match t.as_bytes().first() {
        Some(b'-') => (true, &t[1..]),
        Some(b'+') => (false, &t[1..]),
        _ => (false, t),
    };
    if !is_digits(digits) {
        return None;
    }
    let v: i64 = digits.parse().ok()?;
    Some(if neg { -v } else { v })
}

pub fn py_truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64() != Some(0.0),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

pub fn py_float_repr(f: f64) -> String {
    if f.is_nan() {
        return "nan".into();
    }
    if f.is_infinite() {
        return if f > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if f == f.trunc() && f.abs() < 1e16 {
        return format!("{:.1}", f);
    }
    let s = format!("{:e}", f);
    let (m, e) = match s.split_once('e') {
        Some(x) => x,
        None => return s,
    };
    let exp: i32 = e.parse().unwrap_or(0);
    if (-4..16).contains(&exp) {
        return format!("{}", f);
    }
    format!("{}e{}{:02}", m, if exp < 0 { '-' } else { '+' }, exp.abs())
}

pub fn py_str(v: &Value) -> String {
    match v {
        Value::Null => "None".into(),
        Value::Bool(true) => "True".into(),
        Value::Bool(false) => "False".into(),
        Value::Number(n) => {
            if n.is_f64() {
                py_float_repr(n.as_f64().unwrap_or(0.0))
            } else {
                n.to_string()
            }
        }
        Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

pub fn py_num(v: &Value) -> Option<f64> {
    match v {
        Value::Bool(b) => Some(*b as i64 as f64),
        Value::Number(n) => n.as_f64(),
        _ => None,
    }
}

pub fn py_eq(a: &Value, b: &Value) -> bool {
    match (py_num(a), py_num(b)) {
        (Some(x), Some(y)) => x == y,
        (None, None) => a == b,
        _ => false,
    }
}

pub fn hex(b: &[u8]) -> String {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push(H[(x >> 4) as usize] as char);
        s.push(H[(x & 15) as usize] as char);
    }
    s
}

pub fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

pub fn quote_plus(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 8);
    for &b in s.as_bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'_' | b'.' | b'-' | b'~' => o.push(b as char),
            b' ' => o.push('+'),
            _ => o.push_str(&format!("%{:02X}", b)),
        }
    }
    o
}

pub fn urlencode(pairs: &[(String, String)]) -> String {
    pairs.iter().map(|(k, v)| format!("{}={}", quote_plus(k), quote_plus(v))).collect::<Vec<_>>().join("&")
}

fn hexval(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

pub fn unquote(s: &str, plus: bool) -> String {
    let b = s.as_bytes();
    if !b.contains(&b'%') && !(plus && b.contains(&b'+')) {
        return s.to_string();
    }
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'%' if i + 2 < b.len() => {
                match (hexval(b[i + 1]), hexval(b[i + 2])) {
                    (Some(h), Some(l)) => {
                        out.push(h * 16 + l);
                        i += 3;
                    }
                    _ => {
                        out.push(b'%');
                        i += 1;
                    }
                }
            }
            b'+' if plus => {
                out.push(b' ');
                i += 1;
            }
            c => {
                out.push(c);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

pub fn parse_qs(q: &str) -> Vec<(String, String)> {
    let mut out: Vec<(String, String)> = Vec::new();
    for part in q.split('&') {
        if part.is_empty() {
            continue;
        }
        let (k, v) = match part.split_once('=') {
            Some(x) => x,
            None => continue,
        };
        if v.is_empty() {
            continue;
        }
        let k = unquote(k, true);
        if out.iter().any(|(x, _)| *x == k) {
            continue;
        }
        out.push((k, unquote(v, true)));
    }
    out
}

pub fn qget<'a>(q: &'a [(String, String)], k: &str) -> Option<&'a str> {
    q.iter().find(|(x, _)| x == k).map(|(_, v)| v.as_str())
}

pub fn file_lines(text: &str) -> impl Iterator<Item = &str> {
    text.split(['\n', '\r'])
}

pub fn read_lossy(p: &Path) -> Option<String> {
    std::fs::read(p).ok().map(|b| String::from_utf8_lossy(&b).into_owned())
}

pub fn admin_ids_parse(text: &str) -> HashSet<i64> {
    let mut ids = HashSet::new();
    for line in file_lines(text) {
        let e = py_strip(line.split('#').next().unwrap_or(""));
        if is_digits(e) {
            if let Ok(v) = e.parse() {
                ids.insert(v);
            }
        }
    }
    ids
}

pub fn lead_ids_parse(text: &str) -> HashSet<i64> {
    let mut ids = HashSet::new();
    for line in file_lines(text) {
        let e: Vec<&str> = line.split('#').next().unwrap_or("").split(py_space).filter(|x| !x.is_empty()).collect();
        if e.len() >= 2 && is_digits(e[0]) && e[1].to_lowercase() == "lead" {
            if let Ok(v) = e[0].parse() {
                ids.insert(v);
            }
        }
    }
    ids
}

pub fn mtime_secs(p: &Path) -> i64 {
    std::fs::metadata(p)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}
