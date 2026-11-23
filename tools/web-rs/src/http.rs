use crate::util::{http_date, now};
use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::net::TcpStream;
use std::sync::Arc;
use std::time::{Duration, Instant};

const HEAD_MAX: usize = 64 * 1024;
const IDLE_TIMEOUT: Duration = Duration::from_secs(120);
const REQ_TIMEOUT: Duration = Duration::from_secs(30);

pub struct Request {
    pub method: String,
    pub target: String,
    pub version: String,
    pub line: String,
    pub headers: Vec<(String, String)>,
}

impl Request {
    pub fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(n, _)| n == name).map(|(_, v)| v.as_str())
    }

    fn split(&self) -> (&str, &str) {
        let t = self.target.split_once('#').map_or(self.target.as_str(), |x| x.0);
        t.split_once('?').unwrap_or((t, ""))
    }

    pub fn path(&self) -> &str {
        self.split().0
    }

    pub fn query(&self) -> &str {
        self.split().1
    }

    pub fn keep_alive(&self) -> bool {
        let c = self.header("connection").unwrap_or("").to_ascii_lowercase();
        if self.version == "HTTP/1.1" {
            !c.contains("close")
        } else {
            false
        }
    }

    pub fn has_body(&self) -> bool {
        self.header("transfer-encoding").is_some() || self.header("content-length").is_some_and(|v| v.trim() != "0")
    }
}

pub enum Body {
    Bytes(Vec<u8>),
    Shared(Arc<Vec<u8>>),
    File(File, u64, u64),
}

pub struct Response {
    pub code: u16,
    pub headers: Vec<(&'static str, String)>,
    pub body: Body,
    pub close: bool,
}

impl Response {
    pub fn new(code: u16, headers: Vec<(&'static str, String)>, body: Vec<u8>) -> Response {
        Response { code, headers, body: Body::Bytes(body), close: false }
    }
}

pub struct Conn {
    pub stream: TcpStream,
    pub buf: Vec<u8>,
}

fn head_end(buf: &[u8]) -> Option<(usize, usize)> {
    let mut i = 0;
    while i < buf.len() {
        if buf[i] == b'\n' {
            if buf.get(i + 1) == Some(&b'\n') {
                return Some((i, i + 2));
            }
            if buf.get(i + 1) == Some(&b'\r') && buf.get(i + 2) == Some(&b'\n') {
                return Some((i, i + 3));
            }
        }
        i += 1;
    }
    None
}

pub enum Head {
    Req(Request),
    Closed,
    Bad,
}

impl Conn {
    pub fn read_head(&mut self) -> Head {
        let mut tmp = [0u8; 8192];
        let mut started: Option<Instant> = None;
        loop {
            let lead = self.buf.iter().take_while(|b| **b == b'\r' || **b == b'\n').count();
            if lead > 0 {
                self.buf.drain(..lead);
            }
            if let Some((end, next)) = head_end(&self.buf) {
                let head = self.buf[..end].to_vec();
                self.buf.drain(..next);
                return match parse_head(&head) {
                    Some(r) => Head::Req(r),
                    None => Head::Bad,
                };
            }
            if self.buf.len() > HEAD_MAX {
                return Head::Bad;
            }
            let wait = match started {
                None if self.buf.is_empty() => IDLE_TIMEOUT,
                None => {
                    started = Some(Instant::now());
                    REQ_TIMEOUT
                }
                Some(t) => match REQ_TIMEOUT.checked_sub(t.elapsed()) {
                    Some(left) if !left.is_zero() => left,
                    _ => return Head::Closed,
                },
            };
            let _ = self.stream.set_read_timeout(Some(wait));
            match self.stream.read(&mut tmp) {
                Ok(0) => return Head::Closed,
                Ok(n) => self.buf.extend_from_slice(&tmp[..n]),
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => return Head::Closed,
            }
        }
    }

    pub fn read_body(&mut self, n: usize) -> io::Result<Vec<u8>> {
        let _ = self.stream.set_read_timeout(Some(REQ_TIMEOUT));
        let mut out = Vec::with_capacity(n);
        let take = n.min(self.buf.len());
        out.extend(self.buf.drain(..take));
        let mut tmp = [0u8; 16384];
        while out.len() < n {
            let want = (n - out.len()).min(tmp.len());
            match self.stream.read(&mut tmp[..want]) {
                Ok(0) => return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "short body")),
                Ok(k) => out.extend_from_slice(&tmp[..k]),
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(e),
            }
        }
        Ok(out)
    }

    pub fn write(&mut self, resp: Response, head_only: bool, keep: bool) -> io::Result<()> {
        let mut h = format!("HTTP/1.1 {} {}\r\nServer: twelve-weeks\r\nDate: {}\r\n", resp.code, reason(resp.code), http_date(now() as i64));
        for (k, v) in &resp.headers {
            h.push_str(k);
            h.push_str(": ");
            h.push_str(v);
            h.push_str("\r\n");
        }
        if !keep {
            h.push_str("Connection: close\r\n");
        }
        h.push_str("\r\n");
        let mut out = h.into_bytes();
        match resp.body {
            Body::Bytes(b) => {
                if !head_only {
                    out.extend_from_slice(&b);
                }
                self.stream.write_all(&out)?;
            }
            Body::Shared(b) => {
                self.stream.write_all(&out)?;
                if !head_only {
                    self.stream.write_all(&b)?;
                }
            }
            Body::File(mut f, off, len) => {
                self.stream.write_all(&out)?;
                if !head_only && len > 0 {
                    f.seek(SeekFrom::Start(off))?;
                    let sent = io::copy(&mut f.take(len), &mut self.stream)?;
                    if sent != len {
                        return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "file shrank"));
                    }
                }
            }
        }
        self.stream.flush()
    }
}

fn parse_head(head: &[u8]) -> Option<Request> {
    let text = std::str::from_utf8(head).ok()?;
    let mut lines = text.split('\n').map(|l| l.strip_suffix('\r').unwrap_or(l));
    let line = lines.next()?.to_string();
    let mut words = line.split_ascii_whitespace();
    let method = words.next()?.to_string();
    let target = words.next()?.to_string();
    let version = words.next()?.to_string();
    if words.next().is_some() || !version.starts_with("HTTP/1.") || !target.starts_with('/') {
        return None;
    }
    let mut headers = Vec::new();
    for l in lines {
        if l.is_empty() {
            continue;
        }
        if l.starts_with(' ') || l.starts_with('\t') {
            continue;
        }
        let (n, v) = l.split_once(':')?;
        headers.push((n.trim().to_ascii_lowercase(), v.trim().to_string()));
    }
    Some(Request { method, target, version, line, headers })
}

pub fn reason(code: u16) -> &'static str {
    match code {
        200 => "OK",
        206 => "Partial Content",
        302 => "Found",
        304 => "Not Modified",
        400 => "Bad Request",
        403 => "Forbidden",
        404 => "Not Found",
        413 => "Request Entity Too Large",
        415 => "Unsupported Media Type",
        416 => "Requested Range Not Satisfiable",
        500 => "Internal Server Error",
        501 => "Not Implemented",
        503 => "Service Unavailable",
        _ => "Unknown",
    }
}
