//! t2r-files: the public front of the Twelve Weeks update server.
//!
//! Why: the Python update server (tools/update_server/server.py, a ThreadingHTTPServer) sends every file body through `read()` + `write()`
//! in Python. When a new build goes out and every launcher downloads several GB at once, that holds a small server's CPU at 100 %, the
//! box swaps, the game server beside it stops answering and every player gets a slow download.
//!
//! What it does:
//!   * GET / HEAD of the big immutable files is served HERE, with Linux `sendfile` (no copy through user space, no Python):
//!       /files/<sha256>[.gz|.br]        -> <data>/blobs/<name>
//!       /part1-files/<sha256>?sid=...   -> <data>/part1/blobs/<sha256>   (only for a Steam id listed in <data>/part1-eligible.txt)
//!       /download/<name>                -> <data>/blobs/<sha of name in <data>/downloads.json>
//!     Same answers as server.py (_file / _download / parse_range): 200 / 206 / 416 / 404, Accept-Ranges, ETag, Content-Range,
//!     Content-Disposition for /download, the same JSON 404 bodies.
//!   * Every other request (manifest, news, status, clan, ban check, reports, report clips, Part I list, media) is passed unchanged to the
//!     Python server on the backend address (127.0.0.1:7003). The client's address goes along as CF-Connecting-IP (the header server.py
//!     reads for loopback callers; a loopback caller, e.g. a Cloudflare tunnel, keeps the header it sent).
//!
//!   * The launcher polls /status.json, /manifest.json and /news.json are answered HERE too (poll.rs, same bytes as server.py),
//!     with keep-alive: ~18 of the ~21 requests / s at 100 players no longer reach Python. /status.json counts the open launchers into
//!     <status dir>/t2r-launchers.json. --status-file is needed for /status.json (else it is passed on); --no-poll passes all three on.
//!
//! One crate (flate2, pure Rust, for the gzip replies), one thread per connection with a cap, keep-alive for file and poll requests,
//! optional global rate cap (--rate-mbit).
//!
//! Usage: t2r-files --listen 0.0.0.0:7002 --backend 127.0.0.1:7003 --data <update-data folder> [--max-conns 1500]
//!                  [--rate-mbit 0] [--stats-secs 60] [--status-file <data folder>/t2r-status.json] [--no-poll]
//! Run the Python server behind it with: server.py --host 127.0.0.1 --port 7003 ...
//! Linux build: cargo build --release --target x86_64-unknown-linux-musl (static binary)

mod json;
mod poll;

use std::collections::HashSet;
use std::fs::File;
use std::io::{self, Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpListener, TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const HEAD_MAX: usize = 32 * 1024;
const READ_TIMEOUT: Duration = Duration::from_secs(30);
const WRITE_TIMEOUT: Duration = Duration::from_secs(60);
const CHUNK: u64 = 1 << 20;

struct Config {
    listen: String,
    backend: String,
    data: PathBuf,
    max_conns: usize,
    rate_mbit: u64,
    stats_secs: u64,
    status_file: Option<PathBuf>,
    no_poll: bool,
}

#[derive(Default)]
struct Stats {
    active: AtomicUsize,
    file_reqs: AtomicU64,
    file_full: AtomicU64,
    file_404: AtomicU64,
    proxied: AtomicU64,
    proxy_fail: AtomicU64,
    refused: AtomicU64,
    sent: AtomicU64,
    polls: AtomicU64,
}

struct Shared {
    cfg: Config,
    stats: Stats,
    elig: Mutex<(Option<SystemTime>, HashSet<String>)>,
    bucket: Option<Mutex<(f64, Instant)>>,
    poll: poll::Poll,
}

fn now_ts() -> String {
    let s = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let (h, m, sec) = ((s / 3600) % 24, (s / 60) % 60, s % 60);
    format!("{:02}:{:02}:{:02}", h, m, sec)
}

macro_rules! log {
    ($($a:tt)*) => {{ eprintln!("[{}] {}", now_ts(), format!($($a)*)); }};
}

/// RFC 7231 date, e.g. "Sun, 22 Nov 2026 05:40:00 GMT".
fn http_date() -> String {
    let s = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) as i64;
    let days = s.div_euclid(86400);
    let rem = s.rem_euclid(86400);
    let wd = ["Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"][(days.rem_euclid(7)) as usize];
    // civil from days (H. Hinnant)
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let mut y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    if m <= 2 {
        y += 1;
    }
    let mon = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"][(m - 1) as usize];
    format!("{}, {:02} {} {} {:02}:{:02}:{:02} GMT", wd, d, mon, y, rem / 3600, (rem / 60) % 60, rem % 60)
}

// ------------------------------------------------------------------ request parsing

struct Request {
    method: String,
    target: String,
    version: String,
    headers: Vec<(String, String)>, // (lower-case name, value)
    raw_head: Vec<u8>,              // the head as received (request line + header lines, without the blank line)
}

impl Request {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(n, _)| n == name).map(|(_, v)| v.as_str())
    }
    fn keep_alive(&self) -> bool {
        let c = self.header("connection").unwrap_or("").to_ascii_lowercase();
        if self.version == "HTTP/1.0" {
            c.contains("keep-alive")
        } else {
            !c.contains("close")
        }
    }
}

fn find_head_end(buf: &[u8]) -> Option<usize> {
    buf.windows(4).position(|w| w == b"\r\n\r\n")
}

/// Reads one request head. Ok(None) = the client closed (or timed out) between requests. `buf` keeps bytes read past the head.
fn read_head(s: &mut TcpStream, buf: &mut Vec<u8>) -> io::Result<Option<Request>> {
    let mut tmp = [0u8; 8192];
    loop {
        if let Some(e) = find_head_end(buf) {
            let head: Vec<u8> = buf[..e].to_vec();
            buf.drain(..e + 4);
            return parse_head(head).map(Some);
        }
        if buf.len() > HEAD_MAX {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "head too large"));
        }
        let n = match s.read(&mut tmp) {
            Ok(n) => n,
            Err(e) if buf.is_empty() && (e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut) => return Ok(None),
            Err(e) => return Err(e),
        };
        if n == 0 {
            return if buf.is_empty() { Ok(None) } else { Err(io::Error::new(io::ErrorKind::UnexpectedEof, "eof in head")) };
        }
        buf.extend_from_slice(&tmp[..n]);
    }
}

fn parse_head(head: Vec<u8>) -> io::Result<Request> {
    let bad = || io::Error::new(io::ErrorKind::InvalidData, "bad request");
    let text = std::str::from_utf8(&head).map_err(|_| bad())?;
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
    Ok(Request { method, target, version, headers, raw_head: head })
}

/// server.py parse_range: Ok(None) = no / ignored header, Err(()) = 416, Ok(Some((start, end_inclusive))).
fn parse_range(header: Option<&str>, size: u64) -> Result<Option<(u64, u64)>, ()> {
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

fn is_sha(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

fn is_blob_name(s: &str) -> bool {
    if is_sha(s) {
        return true;
    }
    match s.split_once('.') {
        Some((h, e)) => is_sha(h) && (e == "gz" || e == "br"),
        None => false,
    }
}

fn pct_decode(s: &str) -> Option<String> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' {
            if i + 2 >= b.len() {
                return None;
            }
            let h = std::str::from_utf8(&b[i + 1..i + 3]).ok()?;
            out.push(u8::from_str_radix(h, 16).ok()?);
            i += 3;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

fn query_param<'a>(query: &'a str, key: &str) -> Option<&'a str> {
    query.split('&').find_map(|kv| {
        let (k, v) = kv.split_once('=').unwrap_or((kv, ""));
        if k == key {
            Some(v)
        } else {
            None
        }
    })
}

/// Value of "name" in a flat JSON object of strings ({"a": "b", ...}, downloads.json). Handles the usual escapes.
fn json_flat_lookup(text: &str, name: &str) -> Option<String> {
    let b = text.as_bytes();
    let mut i = 0;
    let mut strings: Vec<(String, usize)> = Vec::new(); // (string, index of the byte after it)
    while i < b.len() {
        if b[i] == b'"' {
            let mut s = Vec::new();
            i += 1;
            while i < b.len() && b[i] != b'"' {
                if b[i] == b'\\' && i + 1 < b.len() {
                    i += 1;
                    match b[i] {
                        b'n' => s.push(b'\n'),
                        b't' => s.push(b'\t'),
                        b'u' if i + 4 < b.len() => {
                            let cp = u32::from_str_radix(std::str::from_utf8(&b[i + 1..i + 5]).ok()?, 16).ok()?;
                            let mut tmp = [0u8; 4];
                            s.extend_from_slice(char::from_u32(cp).unwrap_or('?').encode_utf8(&mut tmp).as_bytes());
                            i += 4;
                        }
                        c => s.push(c),
                    }
                } else {
                    s.push(b[i]);
                }
                i += 1;
            }
            i += 1;
            strings.push((String::from_utf8(s).ok()?, i));
        } else {
            i += 1;
        }
    }
    // a key is a string followed by ':' (whitespace allowed); its value is the next string
    for w in strings.windows(2) {
        let (k, after) = (&w[0].0, w[0].1);
        let rest = &text[after..];
        if k == name && rest.trim_start().starts_with(':') {
            return Some(w[1].0.clone());
        }
    }
    None
}

// ------------------------------------------------------------------ responses

fn send_json(s: &mut TcpStream, code: u16, reason: &str, body: &str, head_only: bool) -> io::Result<()> {
    let h = format!(
        "HTTP/1.1 {} {}\r\nServer: t2r-files/1\r\nDate: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nCache-Control: no-cache\r\n\r\n",
        code,
        reason,
        http_date(),
        body.len()
    );
    s.write_all(h.as_bytes())?;
    if !head_only {
        s.write_all(body.as_bytes())?;
    }
    Ok(())
}

enum PollKind {
    Status,
    Manifest,
    News,
}

enum Route {
    Poll(PollKind),
    File { path: PathBuf, etag: String, name: Option<String> },
    NotFound(&'static str),
    Proxy,
}

fn route(sh: &Shared, method: &str, target: &str) -> Route {
    if method != "GET" && method != "HEAD" {
        return Route::Proxy;
    }
    let (path, query) = target.split_once('?').unwrap_or((target, ""));
    let data = &sh.cfg.data;
    if !sh.cfg.no_poll {
        match path {
            "/status.json" if sh.poll.has_status() => return Route::Poll(PollKind::Status),
            "/manifest.json" => return Route::Poll(PollKind::Manifest),
            "/news.json" => return Route::Poll(PollKind::News),
            _ => {}
        }
    }
    if let Some(n) = path.strip_prefix("/files/") {
        if !is_blob_name(n) {
            return Route::NotFound("{\"error\":\"no such file\"}");
        }
        return Route::File { path: data.join("blobs").join(n), etag: n.to_string(), name: None };
    }
    if let Some(n) = path.strip_prefix("/part1-files/") {
        let sid = pct_decode(query_param(query, "sid").unwrap_or("")).unwrap_or_default();
        if !part1_eligible(sh, &sid) {
            return Route::NotFound("{\"error\":\"no Part I map pack published\"}");
        }
        if !is_sha(n) {
            return Route::NotFound("{\"error\":\"no such file\"}");
        }
        return Route::File { path: data.join("part1").join("blobs").join(n), etag: n.to_string(), name: None };
    }
    if let Some(n) = path.strip_prefix("/download/") {
        let name = match pct_decode(n) {
            Some(x) => x,
            None => return Route::NotFound("{\"error\":\"no such download\"}"),
        };
        let sha = std::fs::read_to_string(data.join("downloads.json")).ok().and_then(|t| json_flat_lookup(&t, &name));
        return match sha {
            Some(h) if is_sha(&h) => Route::File { path: data.join("blobs").join(&h), etag: h, name: Some(name) },
            _ => Route::NotFound("{\"error\":\"no such download\"}"),
        };
    }
    Route::Proxy
}

/// server.py _part1_sid: the first of up to 8 comma-separated Steam ids that is listed in <data>/part1-eligible.txt (re-read on change).
fn part1_eligible(sh: &Shared, raw: &str) -> bool {
    let p = sh.cfg.data.join("part1-eligible.txt");
    let mt = match std::fs::metadata(&p).and_then(|m| m.modified()) {
        Ok(t) => t,
        Err(_) => return false,
    };
    let mut g = sh.elig.lock().unwrap_or_else(|e| e.into_inner());
    if g.0 != Some(mt) {
        let mut ids = HashSet::new();
        if let Ok(t) = std::fs::read_to_string(&p) {
            for line in t.lines() {
                let l = line.split('#').next().unwrap_or("").trim();
                if !l.is_empty() && l.bytes().all(|c| c.is_ascii_digit()) {
                    ids.insert(l.to_string());
                }
            }
        }
        *g = (Some(mt), ids);
    }
    raw.split(',').take(8).map(|x| x.trim()).any(|x| !x.is_empty() && x.bytes().all(|c| c.is_ascii_digit()) && g.1.contains(x))
}

fn throttle(sh: &Shared, n: u64) {
    let b = match &sh.bucket {
        Some(b) => b,
        None => return,
    };
    let rate = sh.cfg.rate_mbit as f64 * 1e6 / 8.0; // bytes per second
    loop {
        let wait = {
            let mut g = b.lock().unwrap_or_else(|e| e.into_inner());
            let now = Instant::now();
            let el = now.duration_since(g.1).as_secs_f64();
            g.0 = (g.0 + el * rate).min(rate); // at most one second of burst
            g.1 = now;
            if g.0 >= n as f64 {
                g.0 -= n as f64;
                return;
            }
            (n as f64 - g.0) / rate
        };
        std::thread::sleep(Duration::from_secs_f64(wait.min(0.5)));
    }
}

#[cfg(unix)]
fn send_range(sh: &Shared, s: &mut TcpStream, f: &File, start: u64, len: u64) -> io::Result<()> {
    use std::os::unix::io::AsRawFd;
    extern "C" {
        fn sendfile(out_fd: i32, in_fd: i32, offset: *mut i64, count: usize) -> isize;
    }
    let mut off = start as i64;
    let mut left = len;
    while left > 0 {
        let want = left.min(CHUNK);
        throttle(sh, want);
        let r = unsafe { sendfile(s.as_raw_fd(), f.as_raw_fd(), &mut off, want as usize) };
        if r < 0 {
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(e);
        }
        if r == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "file shrank"));
        }
        left -= r as u64;
        sh.stats.sent.fetch_add(r as u64, Ordering::Relaxed);
    }
    Ok(())
}

#[cfg(not(unix))]
fn send_range(sh: &Shared, s: &mut TcpStream, f: &File, start: u64, len: u64) -> io::Result<()> {
    use std::io::{Seek, SeekFrom};
    let mut f = f;
    f.seek(SeekFrom::Start(start))?;
    let mut buf = vec![0u8; CHUNK as usize];
    let mut left = len;
    while left > 0 {
        let want = left.min(CHUNK) as usize;
        throttle(sh, want as u64);
        let n = f.read(&mut buf[..want])?;
        if n == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "file shrank"));
        }
        s.write_all(&buf[..n])?;
        left -= n as u64;
        sh.stats.sent.fetch_add(n as u64, Ordering::Relaxed);
    }
    Ok(())
}

/// Ok(true) = the connection may serve another request.
fn serve_file(sh: &Shared, s: &mut TcpStream, req: &Request, path: &Path, etag: &str, name: Option<&str>) -> io::Result<bool> {
    let head_only = req.method == "HEAD";
    sh.stats.file_reqs.fetch_add(1, Ordering::Relaxed);
    let f = match File::open(path) {
        Ok(f) if f.metadata().map(|m| m.is_file()).unwrap_or(false) => f,
        _ => {
            sh.stats.file_404.fetch_add(1, Ordering::Relaxed);
            let body = if name.is_some() { "{\"error\":\"no such download\"}" } else { "{\"error\":\"no such file\"}" };
            send_json(s, 404, "Not Found", body, head_only)?;
            return Ok(true);
        }
    };
    let size = f.metadata()?.len();
    let rng = match parse_range(req.header("range"), size) {
        Ok(r) => r,
        Err(()) => {
            let h = format!(
                "HTTP/1.1 416 Requested Range Not Satisfiable\r\nServer: t2r-files/1\r\nDate: {}\r\nContent-Range: bytes */{}\r\nContent-Length: 0\r\n\r\n",
                http_date(),
                size
            );
            s.write_all(h.as_bytes())?;
            return Ok(true);
        }
    };
    let (start, end) = rng.unwrap_or((0, size.saturating_sub(1)));
    let len = if size == 0 { 0 } else { end - start + 1 };
    let mut h = format!(
        "HTTP/1.1 {}\r\nServer: t2r-files/1\r\nDate: {}\r\nContent-Type: application/octet-stream\r\nAccept-Ranges: bytes\r\nETag: \"{}\"\r\n",
        if rng.is_some() { "206 Partial Content" } else { "200 OK" },
        http_date(),
        etag
    );
    if let Some(n) = name {
        h += &format!("Content-Disposition: attachment; filename=\"{}\"\r\n", n.replace('"', ""));
    }
    h += &format!("Content-Length: {}\r\n", len);
    if rng.is_some() {
        h += &format!("Content-Range: bytes {}-{}/{}\r\n", start, end, size);
    }
    h += "\r\n";
    if rng.is_none() {
        sh.stats.file_full.fetch_add(1, Ordering::Relaxed);
    }
    s.write_all(h.as_bytes())?;
    if head_only || len == 0 {
        return Ok(true);
    }
    send_range(sh, s, &f, start, len)?;
    Ok(true)
}

fn reason(code: u16) -> &'static str {
    match code {
        200 => "OK",
        404 => "Not Found",
        503 => "Service Unavailable",
        _ => "Error",
    }
}

/// One launcher poll (poll.rs), answered like server.py's _json: gzip when asked and the body is >= 1 KB, Vary: Accept-Encoding.
fn serve_poll(sh: &Shared, s: &mut TcpStream, peer: &SocketAddr, req: &Request, kind: &PollKind) -> io::Result<()> {
    sh.stats.polls.fetch_add(1, Ordering::Relaxed);
    let loop_peer = is_loopback(&peer.ip());
    let query = req.target.split_once('?').map(|x| x.1).unwrap_or("");
    let r = match kind {
        PollKind::Status => {
            // server.py: the caller's address (CF-Connecting-IP from a loopback front: Caddy / the tunnel); a loopback caller without it is a
            // local tool, not a launcher
            let ip = if loop_peer { req.header("cf-connecting-ip").unwrap_or("").trim().to_string() } else { peer.ip().to_string() };
            let ip = if ip == "127.0.0.1" || ip == "::1" { String::new() } else { ip };
            sh.poll.hit_launcher(&ip);
            sh.poll.status()
        }
        PollKind::News => sh.poll.news(),
        PollKind::Manifest => {
            let q = |k: &str| query_param(query, k).and_then(|v| pct_decode(&v.replace('+', " "))).filter(|v| !v.is_empty());
            let ch = q("channel").unwrap_or_else(|| "stable".into());
            let ver = q("version");
            // server.py _base: https only when the front that called us said so (X-Forwarded-Proto from a loopback peer)
            let https = loop_peer && req.header("x-forwarded-proto").map(|v| v.trim().eq_ignore_ascii_case("https")).unwrap_or(false);
            let base = format!("{}://{}", if https { "https" } else { "http" }, req.header("host").unwrap_or("localhost"));
            sh.poll.manifest(&ch, ver.as_deref(), &base)
        }
    };
    let gz = r.gz.is_some() && req.header("accept-encoding").map(|v| v.to_ascii_lowercase().contains("gzip")).unwrap_or(false);
    let body: &[u8] = if gz { r.gz.as_deref().unwrap_or(&[]) } else { &r.plain };
    let h = format!(
        "HTTP/1.1 {} {}\r\nServer: t2r-files/1\r\nDate: {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nCache-Control: no-cache\r\nVary: Accept-Encoding\r\n{}\r\n",
        r.code,
        reason(r.code),
        http_date(),
        body.len(),
        if gz { "Content-Encoding: gzip\r\n" } else { "" }
    );
    s.write_all(h.as_bytes())?;
    if req.method != "HEAD" {
        s.write_all(body)?;
    }
    Ok(())
}

fn is_loopback(ip: &IpAddr) -> bool {
    match ip {
        IpAddr::V4(a) => a.is_loopback(),
        IpAddr::V6(a) => a.is_loopback() || a.to_ipv4_mapped().map(|x| x.is_loopback()).unwrap_or(false),
    }
}

/// Passes the request to the Python server and streams its answer back; the client connection closes afterwards.
fn proxy(sh: &Shared, s: &mut TcpStream, peer: &SocketAddr, req: &Request, mut pending: Vec<u8>) -> io::Result<()> {
    sh.stats.proxied.fetch_add(1, Ordering::Relaxed);
    if req.header("transfer-encoding").is_some() {
        return send_json(s, 411, "Length Required", "{\"error\":\"send a Content-Length\"}", false);
    }
    let clen: u64 = match req.header("content-length") {
        Some(v) => match v.parse() {
            Ok(n) => n,
            Err(_) => return send_json(s, 400, "Bad Request", "{\"error\":\"bad Content-Length\"}", false),
        },
        None => 0,
    };
    let addr = match sh.cfg.backend.to_socket_addrs().ok().and_then(|mut a| a.next()) {
        Some(a) => a,
        None => return send_json(s, 502, "Bad Gateway", "{\"error\":\"update server busy, try again in a moment\"}", false),
    };
    let mut b = match TcpStream::connect_timeout(&addr, Duration::from_secs(5)) {
        Ok(b) => b,
        Err(_) => {
            sh.stats.proxy_fail.fetch_add(1, Ordering::Relaxed);
            return send_json(s, 502, "Bad Gateway", "{\"error\":\"update server busy, try again in a moment\"}", false);
        }
    };
    b.set_read_timeout(Some(Duration::from_secs(120)))?;
    b.set_write_timeout(Some(WRITE_TIMEOUT))?;
    let loop_peer = is_loopback(&peer.ip());
    let text = String::from_utf8_lossy(&req.raw_head);
    let mut lines = text.split("\r\n");
    let mut out = String::with_capacity(req.raw_head.len() + 96);
    out += lines.next().unwrap_or("");
    out += "\r\n";
    for l in lines {
        let n = l.split(':').next().unwrap_or("").trim().to_ascii_lowercase();
        let hop = matches!(n.as_str(), "connection" | "keep-alive" | "proxy-connection" | "te" | "upgrade" | "trailer");
        if hop || (n == "cf-connecting-ip" && !loop_peer) {
            continue;
        }
        out += l;
        out += "\r\n";
    }
    if !loop_peer {
        out += &format!("CF-Connecting-IP: {}\r\n", peer.ip());
    }
    out += "Connection: close\r\n\r\n";
    b.write_all(out.as_bytes())?;
    // the body: what came with the head, then the rest straight from the client
    let first = (pending.len() as u64).min(clen) as usize;
    b.write_all(&pending[..first])?;
    pending.drain(..first);
    let mut left = clen - first as u64;
    let mut buf = vec![0u8; 64 * 1024];
    while left > 0 {
        let want = left.min(buf.len() as u64) as usize;
        let n = s.read(&mut buf[..want])?;
        if n == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "client body ended early"));
        }
        b.write_all(&buf[..n])?;
        left -= n as u64;
    }
    // the answer, as the backend sends it, until it closes
    loop {
        let n = match b.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        };
        s.write_all(&buf[..n])?;
    }
    Ok(())
}

fn handle(sh: Arc<Shared>, mut s: TcpStream, peer: SocketAddr) {
    let _ = s.set_nodelay(true);
    let _ = s.set_read_timeout(Some(READ_TIMEOUT));
    let _ = s.set_write_timeout(Some(WRITE_TIMEOUT));
    let mut buf: Vec<u8> = Vec::new();
    loop {
        let req = match read_head(&mut s, &mut buf) {
            Ok(Some(r)) => r,
            Ok(None) => break,
            Err(e) => {
                if e.kind() == io::ErrorKind::InvalidData {
                    let _ = send_json(&mut s, 400, "Bad Request", "{\"error\":\"bad request\"}", false);
                }
                break;
            }
        };
        match route(&sh, &req.method, &req.target) {
            Route::Poll(kind) => {
                if let Some(n) = req.header("content-length").and_then(|v| v.parse::<usize>().ok()) {
                    if n > 0 {
                        if n > buf.len() {
                            break;
                        }
                        buf.drain(..n);
                    }
                }
                if serve_poll(&sh, &mut s, &peer, &req, &kind).is_err() {
                    break;
                }
            }
            Route::Proxy => {
                let pending = std::mem::take(&mut buf);
                if let Err(e) = proxy(&sh, &mut s, &peer, &req, pending) {
                    if e.kind() != io::ErrorKind::BrokenPipe && e.kind() != io::ErrorKind::ConnectionReset {
                        log!("proxy {} {} {}: {}", peer.ip(), req.method, req.target.chars().take(80).collect::<String>(), e);
                    }
                }
                break;
            }
            Route::NotFound(body) => {
                sh.stats.file_404.fetch_add(1, Ordering::Relaxed);
                if send_json(&mut s, 404, "Not Found", body, req.method == "HEAD").is_err() {
                    break;
                }
            }
            Route::File { path, etag, name } => {
                // a GET body (never sent by the launcher) is skipped so the next request parses
                if let Some(n) = req.header("content-length").and_then(|v| v.parse::<usize>().ok()) {
                    if n > 0 {
                        if n > buf.len() {
                            break;
                        }
                        buf.drain(..n);
                    }
                }
                match serve_file(&sh, &mut s, &req, &path, &etag, name.as_deref()) {
                    Ok(true) => {}
                    _ => break,
                }
            }
        }
        if !req.keep_alive() {
            break;
        }
    }
    let _ = s.shutdown(Shutdown::Both);
}

fn parse_args() -> Config {
    let mut c = Config {
        listen: "0.0.0.0:7002".into(),
        backend: "127.0.0.1:7003".into(),
        data: PathBuf::from("update-data"),
        max_conns: 1500,
        rate_mbit: 0,
        stats_secs: 60,
        status_file: None,
        no_poll: false,
    };
    let a: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < a.len() {
        let v = a.get(i + 1).cloned().unwrap_or_default();
        if a[i] == "--no-poll" {
            c.no_poll = true;
            i += 1;
            continue;
        }
        match a[i].as_str() {
            "--status-file" => c.status_file = Some(PathBuf::from(v)),
            "--listen" => c.listen = v,
            "--backend" => c.backend = v,
            "--data" => c.data = PathBuf::from(v),
            "--max-conns" => c.max_conns = v.parse().unwrap_or(c.max_conns),
            "--rate-mbit" => c.rate_mbit = v.parse().unwrap_or(0),
            "--stats-secs" => c.stats_secs = v.parse().unwrap_or(60),
            x => {
                eprintln!("unknown argument {}", x);
                std::process::exit(2);
            }
        }
        i += 2;
    }
    c
}

fn main() {
    let cfg = parse_args();
    let bucket = if cfg.rate_mbit > 0 { Some(Mutex::new((0.0, Instant::now()))) } else { None };
    let listener = TcpListener::bind(&cfg.listen).unwrap_or_else(|e| {
        eprintln!("bind {}: {}", cfg.listen, e);
        std::process::exit(1);
    });
    log!(
        "t2r-files on {} -> backend {} | data {} | max conns {} | rate {}",
        cfg.listen,
        cfg.backend,
        cfg.data.display(),
        cfg.max_conns,
        if cfg.rate_mbit > 0 { format!("{} Mbit/s", cfg.rate_mbit) } else { "unlimited".into() }
    );
    let poll = poll::Poll::new(cfg.data.clone(), cfg.status_file.clone());
    let sh = Arc::new(Shared { cfg, stats: Stats::default(), elig: Mutex::new((None, HashSet::new())), bucket, poll });
    if sh.poll.has_status() && !sh.cfg.no_poll {
        // the open-launcher count for the Discord bot (server.py LauncherSeen: every 15 s)
        let sh = sh.clone();
        std::thread::spawn(move || loop {
            if let Err(e) = sh.poll.write_launchers() {
                log!("launchers file: {}", e);
            }
            std::thread::sleep(Duration::from_secs(15));
        });
    }
    {
        let sh = sh.clone();
        std::thread::spawn(move || {
            let mut last_sent = 0u64;
            let mut last = Instant::now();
            loop {
                std::thread::sleep(Duration::from_secs(sh.cfg.stats_secs.max(5)));
                let st = &sh.stats;
                let sent = st.sent.load(Ordering::Relaxed);
                let el = last.elapsed().as_secs_f64().max(0.001);
                log!(
                    "stats: conns {} | polls {} | file reqs {} (full {}, 404 {}) | proxied {} (backend down {}) | refused {} | sent {:.1} MB, {:.0} Mbit/s",
                    st.active.load(Ordering::Relaxed),
                    st.polls.load(Ordering::Relaxed),
                    st.file_reqs.load(Ordering::Relaxed),
                    st.file_full.load(Ordering::Relaxed),
                    st.file_404.load(Ordering::Relaxed),
                    st.proxied.load(Ordering::Relaxed),
                    st.proxy_fail.load(Ordering::Relaxed),
                    st.refused.load(Ordering::Relaxed),
                    (sent - last_sent) as f64 / 1e6,
                    (sent - last_sent) as f64 * 8.0 / 1e6 / el
                );
                last_sent = sent;
                last = Instant::now();
            }
        });
    }
    for conn in listener.incoming() {
        let s = match conn {
            Ok(s) => s,
            Err(_) => {
                std::thread::sleep(Duration::from_millis(20)); // EMFILE etc.: do not spin
                continue;
            }
        };
        let peer = match s.peer_addr() {
            Ok(p) => p,
            Err(_) => continue,
        };
        if sh.stats.active.load(Ordering::Relaxed) >= sh.cfg.max_conns {
            sh.stats.refused.fetch_add(1, Ordering::Relaxed);
            let mut s = s;
            let _ = s.set_write_timeout(Some(Duration::from_secs(2)));
            let _ = send_json(&mut s, 503, "Service Unavailable", "{\"error\":\"server busy, try again in a moment\"}", false);
            continue;
        }
        sh.stats.active.fetch_add(1, Ordering::Relaxed);
        let sh2 = sh.clone();
        let r = std::thread::Builder::new().stack_size(128 * 1024).spawn(move || {
            handle(sh2.clone(), s, peer);
            sh2.stats.active.fetch_sub(1, Ordering::Relaxed);
        });
        if r.is_err() {
            sh.stats.active.fetch_sub(1, Ordering::Relaxed);
        }
    }
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
        assert_eq!(parse_range(Some("bytes=0-1,5-6"), 100), Ok(None)); // multi-range: ignored like server.py's regex
        assert_eq!(parse_range(Some("items=0-1"), 100), Ok(None));
        assert_eq!(parse_range(Some("bytes=-5"), 0), Err(()));
    }

    #[test]
    fn names() {
        let h = "a".repeat(64);
        assert!(is_blob_name(&h));
        assert!(is_blob_name(&format!("{}.gz", h)));
        assert!(!is_blob_name(&format!("{}.zip", h)));
        assert!(!is_blob_name("../../etc/passwd"));
        assert!(!is_blob_name(&"A".repeat(64)));
        assert!(!is_sha(&format!("{}/x", "a".repeat(62))));
    }

    #[test]
    fn downloads_json() {
        let t = "{\n \"TwelveWeeks-Launcher-0.1.44.exe\": \"ad7c\",\n \"a \\\"q\\\".exe\": \"ff\"\n}";
        assert_eq!(json_flat_lookup(t, "TwelveWeeks-Launcher-0.1.44.exe").as_deref(), Some("ad7c"));
        assert_eq!(json_flat_lookup(t, "a \"q\".exe").as_deref(), Some("ff"));
        assert_eq!(json_flat_lookup(t, "ad7c"), None); // a value is not a key
        assert_eq!(pct_decode("TwelveWeeks%20L.exe").as_deref(), Some("TwelveWeeks L.exe"));
        assert_eq!(pct_decode("%2"), None);
    }

    #[test]
    fn head_parse() {
        let r = parse_head(b"GET /files/x?y=1 HTTP/1.1\r\nHost: a\r\nRange: bytes=1-2".to_vec()).unwrap();
        assert_eq!(r.method, "GET");
        assert_eq!(r.header("range"), Some("bytes=1-2"));
        assert!(r.keep_alive());
        assert!(parse_head(b"GET http://x/ HTTP/1.1".to_vec()).is_err());
        let r10 = parse_head(b"GET / HTTP/1.0".to_vec()).unwrap();
        assert!(!r10.keep_alive());
    }

    #[test]
    fn date_format() {
        let d = http_date();
        assert!(d.ends_with(" GMT") && d.len() == 29, "{}", d);
    }
}
