use std::io::{Read, Write};
use std::net::{TcpStream, ToSocketAddrs};
use std::time::Duration;

use serde_json::Value;

pub enum CallErr {
    NoSecret,
    Net(String),
}

pub const NO_SECRET: &str = "no admin secret (the daemon creates admin-secret.txt next to the database when it starts with this build)";

impl CallErr {
    pub fn reason(&self) -> String {
        match self {
            CallErr::NoSecret => NO_SECRET.to_string(),
            CallErr::Net(r) => r.clone(),
        }
    }
}

pub fn io_reason(e: &std::io::Error) -> String {
    match e.kind() {
        std::io::ErrorKind::TimedOut | std::io::ErrorKind::WouldBlock => "timed out".to_string(),
        _ => match e.raw_os_error() {
            Some(code) => {
                let msg = std::io::Error::from_raw_os_error(code).to_string();
                let msg = msg.split(" (os error").next().unwrap_or(&msg).to_string();
                format!("[Errno {}] {}", code, msg)
            }
            None => e.to_string(),
        },
    }
}

fn split_url(url: &str) -> Option<(String, String)> {
    let rest = url.strip_prefix("http://")?;
    let (hostport, prefix) = match rest.find('/') {
        Some(i) => (&rest[..i], &rest[i..]),
        None => (rest, ""),
    };
    let hostport = if hostport.contains(':') && !hostport.ends_with(']') { hostport.to_string() } else { format!("{}:80", hostport) };
    Some((hostport, prefix.trim_end_matches('/').to_string()))
}

fn reason_phrase(code: u16) -> &'static str {
    match code {
        400 => "Bad Request",
        401 => "Unauthorized",
        403 => "Forbidden",
        404 => "Not Found",
        405 => "Method Not Allowed",
        409 => "Conflict",
        500 => "Internal Server Error",
        501 => "Not Implemented",
        502 => "Bad Gateway",
        503 => "Service Unavailable",
        _ => "error",
    }
}

fn dechunk(mut b: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::new();
    loop {
        let i = b.windows(2).position(|w| w == b"\r\n")?;
        let line = std::str::from_utf8(&b[..i]).ok()?;
        let n = usize::from_str_radix(line.split(';').next()?.trim(), 16).ok()?;
        b = &b[i + 2..];
        if n == 0 {
            return Some(out);
        }
        if b.len() < n + 2 {
            return None;
        }
        out.extend_from_slice(&b[..n]);
        b = &b[n + 2..];
    }
}

pub fn call(admin_url: &str, secret: &str, path: &str, data: Option<&Value>, timeout: f64) -> Result<Value, CallErr> {
    if secret.is_empty() {
        return Err(CallErr::NoSecret);
    }
    let (hostport, prefix) = split_url(admin_url).ok_or_else(|| CallErr::Net(format!("unknown url type: {}", admin_url)))?;
    let to = Duration::from_secs_f64(timeout.max(0.01));
    let addrs: Vec<_> = hostport.to_socket_addrs().map_err(|e| CallErr::Net(io_reason(&e)))?.collect();
    let mut last = CallErr::Net("[Errno -2] Name or service not known".into());
    let mut stream = None;
    for a in addrs {
        match TcpStream::connect_timeout(&a, to) {
            Ok(s) => {
                stream = Some(s);
                break;
            }
            Err(e) => last = CallErr::Net(io_reason(&e)),
        }
    }
    let mut s = stream.ok_or(last)?;
    let net = |e: std::io::Error| CallErr::Net(io_reason(&e));
    s.set_read_timeout(Some(to)).map_err(net)?;
    s.set_write_timeout(Some(to)).map_err(net)?;
    let body = data.map(|d| d.to_string().into_bytes());
    let host = hostport.clone();
    let mut req = format!(
        "{} {}{} HTTP/1.1\r\nHost: {}\r\nX-Admin-Secret: {}\r\nContent-Type: application/json\r\nConnection: close\r\nAccept-Encoding: identity\r\n",
        if body.is_some() { "POST" } else { "GET" },
        prefix,
        path,
        host,
        secret
    );
    if let Some(b) = &body {
        req.push_str(&format!("Content-Length: {}\r\n", b.len()));
    }
    req.push_str("\r\n");
    s.write_all(req.as_bytes()).map_err(net)?;
    if let Some(b) = &body {
        s.write_all(b).map_err(net)?;
    }
    let mut raw = Vec::new();
    s.take(32 << 20).read_to_end(&mut raw).map_err(net)?;
    let he = raw.windows(4).position(|w| w == b"\r\n\r\n").ok_or_else(|| CallErr::Net("Remote end closed connection without response".into()))?;
    let head = String::from_utf8_lossy(&raw[..he]).into_owned();
    let mut lines = head.split("\r\n");
    let status_line = lines.next().unwrap_or("");
    let code: u16 = status_line.split_whitespace().nth(1).and_then(|c| c.parse().ok()).unwrap_or(0);
    let mut chunked = false;
    let mut clen: Option<usize> = None;
    for l in lines {
        if let Some((k, v)) = l.split_once(':') {
            let k = k.trim().to_ascii_lowercase();
            if k == "transfer-encoding" && v.to_ascii_lowercase().contains("chunked") {
                chunked = true;
            } else if k == "content-length" {
                clen = v.trim().parse().ok();
            }
        }
    }
    if !(200..300).contains(&code) {
        let phrase = status_line.splitn(3, ' ').nth(2).map(str::trim).filter(|p| !p.is_empty()).unwrap_or(reason_phrase(code));
        return Err(CallErr::Net(phrase.to_string()));
    }
    let mut payload = raw[he + 4..].to_vec();
    if chunked {
        payload = dechunk(&payload).ok_or_else(|| CallErr::Net("bad chunked response".into()))?;
    } else if let Some(n) = clen {
        payload.truncate(n);
    }
    serde_json::from_slice(&payload).map_err(|e| CallErr::Net(e.to_string()))
}
