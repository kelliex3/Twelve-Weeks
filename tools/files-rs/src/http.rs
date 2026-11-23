use std::io::{self, Read, Write};
use std::net::{IpAddr, TcpStream};
use std::time::{SystemTime, UNIX_EPOCH};

const HEAD_MAX: usize = 32 * 1024;

pub struct Request {
    pub method: String,
    pub target: String,
    pub version: String,
    pub headers: Vec<(String, String)>,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(n, _)| n == name).map(|(_, v)| v.as_str())
    }

    pub fn keep_alive(&self) -> bool {
        let c = self.header("connection").unwrap_or("").to_ascii_lowercase();
        if self.version == "HTTP/1.0" {
            c.contains("keep-alive")
        } else {
            !c.contains("close")
        }
    }

    pub fn path(&self) -> &str {
        self.target.split_once('?').map(|x| x.0).unwrap_or(&self.target)
    }

    pub fn query(&self) -> Query {
        Query::parse(self.target.split_once('?').map(|x| x.1).unwrap_or(""))
    }

    pub fn content_length(&self) -> Result<u64, ()> {
        match self.header("content-length") {
            None => Ok(0),
            Some(v) => v.trim().parse().map_err(|_| ()),
        }
    }
}

pub fn now_secs() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub fn now_ts() -> String {
    let s = now_secs() as u64;
    format!("{:02}:{:02}:{:02}", (s / 3600) % 24, (s / 60) % 60, s % 60)
}

#[macro_export]
macro_rules! log {
    ($($a:tt)*) => {{ eprintln!("[{}] {}", $crate::http::now_ts(), format!($($a)*)); }};
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

pub fn gmtime(s: i64) -> (i64, u32, u32, i64, i64, i64) {
    let (y, m, d) = civil(s.div_euclid(86400));
    let rem = s.rem_euclid(86400);
    (y, m, d, rem / 3600, (rem / 60) % 60, rem % 60)
}

pub fn http_date() -> String {
    let s = now_secs() as i64;
    let wd = ["Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"][s.div_euclid(86400).rem_euclid(7) as usize];
    let (y, m, d, hh, mm, ss) = gmtime(s);
    let mon = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"][(m - 1) as usize];
    format!("{}, {:02} {} {} {:02}:{:02}:{:02} GMT", wd, d, mon, y, hh, mm, ss)
}

fn find_head_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

pub fn read_head(s: &mut TcpStream, buf: &mut Vec<u8>) -> io::Result<Option<Request>> {
    let mut tmp = [0u8; 8192];
    loop {
        if let Some(e) = find_head_end(buf) {
            let head: Vec<u8> = buf[..e].to_vec();
            buf.drain(..e + 4);
            return parse_head(&head).map(Some);
        }
        if buf.len() > HEAD_MAX {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "head too large"));
        }
        let n = match s.read(&mut tmp) {
            Ok(n) => n,
            Err(e) if buf.is_empty() && matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) => return Ok(None),
            Err(e) => return Err(e),
        };
        if n == 0 {
            return if buf.is_empty() { Ok(None) } else { Err(io::Error::new(io::ErrorKind::UnexpectedEof, "eof in head")) };
        }
        buf.extend_from_slice(&tmp[..n]);
    }
}

pub fn parse_head(head: &[u8]) -> io::Result<Request> {
    let bad = || io::Error::new(io::ErrorKind::InvalidData, "bad request");
    let text = std::str::from_utf8(head).map_err(|_| bad())?;
    let mut lines = text.split("\r\n");
    let first = lines.next().ok_or_else(bad)?;
    let mut parts = first.split(' ');
    let method = parts.next().ok_or_else(bad)?.to_string();
    let target = parts.next().ok_or_else(bad)?.to_string();
    let version = parts.next().ok_or_else(bad)?.to_string();
    if parts.next().is_some() || !version.starts_with("HTTP/1.") || method.is_empty() || !target.starts_with('/') {
        return Err(bad());
    }
    let mut headers = Vec::new();
    for l in lines {
        let (n, v) = l.split_once(':').ok_or_else(bad)?;
        headers.push((n.trim().to_ascii_lowercase(), v.trim().to_string()));
    }
    Ok(Request { method, target, version, headers })
}

pub fn parse_range(header: Option<&str>, size: u64) -> Result<Option<(u64, u64)>, ()> {
    let h = match header {
        Some(h) => h.trim(),
        None => return Ok(None),
    };
    let spec = match h.strip_prefix("bytes=") {
        Some(s) => s,
        None => return Ok(None),
    };
    let (a, b) = match spec.split_once('-') {
        Some(x) => x,
        None => return Ok(None),
    };
    let digits = |x: &str| x.chars().all(|c| c.is_ascii_digit());
    if !digits(a) || !digits(b) || (a.is_empty() && b.is_empty()) {
        return Ok(None);
    }
    if a.is_empty() {
        let n: u64 = b.parse().map_err(|_| ())?;
        if n == 0 || size == 0 {
            return Err(());
        }
        return Ok(Some((size.saturating_sub(n), size - 1)));
    }
    let start: u64 = a.parse().map_err(|_| ())?;
    let end: u64 = if b.is_empty() { size.wrapping_sub(1) } else { b.parse().map_err(|_| ())? };
    if start >= size || end < start {
        return Err(());
    }
    Ok(Some((start, end.min(size - 1))))
}

pub fn pct_decode(s: &str, plus: bool) -> Option<String> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'%' => {
                let h = b.get(i + 1..i + 3).and_then(|h| std::str::from_utf8(h).ok()).and_then(|h| u8::from_str_radix(h, 16).ok());
                match h {
                    Some(v) => {
                        out.push(v);
                        i += 3;
                    }
                    None if plus => {
                        out.push(b'%');
                        i += 1;
                    }
                    None => return None,
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
    if plus {
        Some(String::from_utf8_lossy(&out).into_owned())
    } else {
        String::from_utf8(out).ok()
    }
}

pub struct Query(Vec<(String, String)>);

impl Query {
    pub fn parse(q: &str) -> Query {
        let mut v = Vec::new();
        for kv in q.split('&') {
            if kv.is_empty() {
                continue;
            }
            let (k, val) = match kv.split_once('=') {
                Some(x) => x,
                None => continue,
            };
            if val.is_empty() {
                continue;
            }
            if let (Some(k), Some(val)) = (pct_decode(k, true), pct_decode(val, true)) {
                v.push((k, val));
            }
        }
        Query(v)
    }

    pub fn first(&self, key: &str) -> Option<&str> {
        self.0.iter().find(|(k, _)| k == key).map(|(_, v)| v.as_str())
    }

    pub fn all(&self, key: &str) -> Vec<&str> {
        self.0.iter().filter(|(k, _)| k == key).map(|(_, v)| v.as_str()).collect()
    }
}

pub fn is_loopback(ip: &IpAddr) -> bool {
    match ip {
        IpAddr::V4(a) => a.is_loopback(),
        IpAddr::V6(a) => a.is_loopback() || a.to_ipv4_mapped().map(|x| x.is_loopback()).unwrap_or(false),
    }
}

pub fn reason(code: u16) -> &'static str {
    match code {
        200 => "OK",
        206 => "Partial Content",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        411 => "Length Required",
        413 => "Payload Too Large",
        416 => "Requested Range Not Satisfiable",
        429 => "Too Many Requests",
        500 => "Internal Server Error",
        501 => "Not Implemented",
        503 => "Service Unavailable",
        507 => "Insufficient Storage",
        _ => "Unknown",
    }
}

pub fn send_body(s: &mut TcpStream, code: u16, ctype: &str, extra: &str, body: &[u8], head_only: bool) -> io::Result<()> {
    let h = format!(
        "HTTP/1.1 {} {}\r\nServer: t2r-files/2\r\nDate: {}\r\nContent-Type: {}\r\nContent-Length: {}\r\n{}\r\n",
        code,
        reason(code),
        http_date(),
        ctype,
        body.len(),
        extra
    );
    if head_only || body.is_empty() || body.len() > 64 * 1024 {
        s.write_all(h.as_bytes())?;
        if !head_only {
            s.write_all(body)?;
        }
        return Ok(());
    }
    let mut all = Vec::with_capacity(h.len() + body.len());
    all.extend_from_slice(h.as_bytes());
    all.extend_from_slice(body);
    s.write_all(&all)
}

pub fn send_json(s: &mut TcpStream, code: u16, body: &str, head_only: bool) -> io::Result<()> {
    send_body(s, code, "application/json", "Cache-Control: no-cache\r\n", body.as_bytes(), head_only)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn range_matches_server_py() {
        assert_eq!(parse_range(None, 100), Ok(None));
        assert_eq!(parse_range(Some("bytes=0-9"), 100), Ok(Some((0, 9))));
        assert_eq!(parse_range(Some("bytes=90-"), 100), Ok(Some((90, 99))));
        assert_eq!(parse_range(Some("bytes=-10"), 100), Ok(Some((90, 99))));
        assert_eq!(parse_range(Some("bytes=-500"), 100), Ok(Some((0, 99))));
        assert_eq!(parse_range(Some("bytes=50-500"), 100), Ok(Some((50, 99))));
        assert_eq!(parse_range(Some("bytes=100-"), 100), Err(()));
        assert_eq!(parse_range(Some("bytes=9-3"), 100), Err(()));
        assert_eq!(parse_range(Some("bytes=-0"), 100), Err(()));
        assert_eq!(parse_range(Some("bytes=-"), 100), Ok(None));
        assert_eq!(parse_range(Some("bytes=0-1,5-6"), 100), Ok(None));
        assert_eq!(parse_range(Some("items=0-1"), 100), Ok(None));
        assert_eq!(parse_range(Some("bytes=-5"), 0), Err(()));
    }

    #[test]
    fn head_parse() {
        let r = parse_head(b"GET /files/x?y=1 HTTP/1.1\r\nHost: a\r\nRange: bytes=1-2").unwrap();
        assert_eq!(r.method, "GET");
        assert_eq!(r.header("range"), Some("bytes=1-2"));
        assert_eq!(r.path(), "/files/x");
        assert!(r.keep_alive());
        assert!(parse_head(b"GET http://x/ HTTP/1.1").is_err());
        assert!(!parse_head(b"GET / HTTP/1.0").unwrap().keep_alive());
    }

    #[test]
    fn query_like_parse_qs() {
        let q = Query::parse("channel=stable&h=a%2Cb&h=c&empty=&s=1+2&bad=%zz");
        assert_eq!(q.first("channel"), Some("stable"));
        assert_eq!(q.all("h"), vec!["a,b", "c"]);
        assert_eq!(q.first("empty"), None);
        assert_eq!(q.first("s"), Some("1 2"));
        assert_eq!(q.first("bad"), Some("%zz"));
        assert_eq!(pct_decode("TwelveWeeks%20L.exe", false).as_deref(), Some("TwelveWeeks L.exe"));
        assert_eq!(pct_decode("%2", false), None);
    }

    #[test]
    fn dates() {
        let d = http_date();
        assert!(d.ends_with(" GMT") && d.len() == 29, "{}", d);
        assert_eq!(gmtime(1795409305), (2026, 11, 23, 4, 48, 25));
    }
}
