//! wslink: the game server link over a WebSocket (the Rust version of wslink.py).
//!
//! Some networks let a TCP connection to our host open and then drop every byte, while Cloudflare traffic passes. The launcher then
//! carries each game-link TCP connection as one WebSocket:
//!
//!   client 127.0.0.1:<local port> -> wss://<cloudflare host>/link -> cloudflared -> this bridge (127.0.0.1:7004) -> daemon 127.0.0.1:7001
//!
//! One WebSocket = one TCP connection; binary frames carry the raw bytes both ways. The bridge listens on loopback only (the tunnel is
//! the one way in, so Cloudflare's CF-Connecting-IP header is the player's address). The daemon learns it from the first line the
//! bridge sends ("T2RFWD <ip>\n", accepted only from a loopback peer).
//!
//! `t2r-daemon wslink [--listen 127.0.0.1:7004] [--daemon 127.0.0.1:7001] [--max 300]` runs the bridge.
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::io::{Read, Write};
use std::net::{Shutdown, TcpListener, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::Arc;
use std::time::Duration;

use crate::py::strip;

// py: wslink.py:28 GUID
pub const GUID: &[u8] = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
// py: wslink.py:30 MAX_FRAME
/// One frame never carries more than 1 MiB (the game link sends a few KB at most).
pub const MAX_FRAME: u64 = 1 << 20;
// py: wslink.py:31 IDLE_S
/// No byte either way for 3 min = dead (the daemon's own keepalive runs every few seconds).
pub const IDLE_S: u64 = 180;

/// `PROXY_LINE % ip`: the first line the daemon gets.
// py: wslink.py:29 PROXY_LINE
pub fn proxy_line(ip: &str) -> Vec<u8> {
    format!("T2RFWD {}\n", ip).into_bytes()
}

/// The Python exception a failed step raised: its class and `str(e)`.
#[derive(Debug, Clone, PartialEq)]
pub enum WsErr {
    /// ConnectionError("...")
    Conn(String),
    /// ValueError("...")
    Value(String),
    /// OSError (socket error): the text `str(e)` has
    Os(String),
}

impl WsErr {
    pub fn text(&self) -> &str {
        match self {
            WsErr::Conn(s) | WsErr::Value(s) | WsErr::Os(s) => s,
        }
    }
    pub fn class(&self) -> &'static str {
        match self {
            WsErr::Conn(_) => "ConnectionError",
            WsErr::Value(_) => "ValueError",
            WsErr::Os(_) => "OSError",
        }
    }
}

/// `str(OSError)` of a socket error: "[Errno 104] Connection reset by peer", a timeout = "timed out".
pub fn os_err(e: &std::io::Error) -> WsErr {
    match e.raw_os_error() {
        Some(libc::EAGAIN) => WsErr::Os("timed out".into()),
        Some(code) => WsErr::Os(format!("[Errno {}] {}", code, crate::pyfmt::strerror(code))),
        None if e.kind() == std::io::ErrorKind::TimedOut => WsErr::Os("timed out".into()),
        None => WsErr::Os(e.to_string()),
    }
}

/// What a socket gives: `recv(n)` = up to n bytes, empty = closed.
pub trait Recv {
    fn recv(&mut self, n: usize) -> Result<Vec<u8>, WsErr>;
}

impl Recv for TcpStream {
    fn recv(&mut self, n: usize) -> Result<Vec<u8>, WsErr> {
        let mut b = vec![0u8; n];
        loop {
            match self.read(&mut b) {
                Ok(k) => {
                    b.truncate(k);
                    return Ok(b);
                }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => return Err(os_err(&e)),
            }
        }
    }
}

// py: wslink.py:44 recv_exact
pub fn recv_exact(s: &mut dyn Recv, n: usize) -> Result<Vec<u8>, WsErr> {
    let mut buf: Vec<u8> = Vec::new();
    while buf.len() < n {
        let part = s.recv(n - buf.len())?;
        if part.is_empty() {
            return Err(WsErr::Conn("closed".into()));
        }
        buf.extend_from_slice(&part);
    }
    Ok(buf)
}

fn latin1(b: &[u8]) -> String {
    b.iter().map(|&x| x as char).collect()
}

/// Python `repr(str)` for a text of Latin-1 characters.
fn repr_latin1(s: &str) -> String {
    let quote = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut out = String::new();
    out.push(quote);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == quote => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 || (0x7f..=0xa0).contains(&(c as u32)) || c as u32 == 0xad => out.push_str(&format!("\\x{:02x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push(quote);
    out
}

/// The parsed head of a request: (path, headers with lower-cased names, bytes read past the head).
pub type Request = (String, IndexMap<String, String>, Vec<u8>);

fn find_sub(h: &[u8], n: &[u8]) -> Option<usize> {
    h.windows(n.len()).position(|w| w == n)
}

// py: wslink.py:54 read_request
/// HTTP request head -> (path, headers lower-cased, rest); an error on garbage / over 8 KB.
pub fn read_request(s: &mut dyn Recv) -> Result<Request, WsErr> {
    let mut data: Vec<u8> = Vec::new();
    while find_sub(&data, b"\r\n\r\n").is_none() {
        let part = s.recv(4096)?;
        if part.is_empty() {
            return Err(WsErr::Conn("closed before the request head".into()));
        }
        data.extend_from_slice(&part);
        if data.len() > 8192 {
            return Err(WsErr::Value("request head over 8 KB".into()));
        }
    }
    let at = find_sub(&data, b"\r\n\r\n").unwrap_or(0);
    let head = latin1(&data[..at]);
    let rest = data[at + 4..].to_vec();
    let lines: Vec<&str> = head.split("\r\n").collect();
    let parts: Vec<&str> = lines[0].split(' ').collect();
    if parts.len() < 2 || parts[0] != "GET" {
        let first: String = lines[0].chars().take(80).collect();
        return Err(WsErr::Value(format!("not a GET: {}", repr_latin1(&first))));
    }
    let mut hdr: IndexMap<String, String> = IndexMap::new();
    for ln in &lines[1..] {
        if let Some((k, v)) = ln.split_once(':') {
            hdr.insert(strip(k).to_lowercase(), strip(v).to_string());
        }
    }
    Ok((parts[1].to_string(), hdr, rest))
}

// py: wslink.py:76 send_frame
/// The bytes of one unmasked frame (FIN set).
pub fn frame_bytes(opcode: u8, payload: &[u8]) -> Vec<u8> {
    let n = payload.len();
    let mut b = Vec::with_capacity(n + 10);
    if n < 126 {
        b.push(0x80 | opcode);
        b.push(n as u8);
    } else if n < 65536 {
        b.push(0x80 | opcode);
        b.push(126);
        b.extend_from_slice(&(n as u16).to_be_bytes());
    } else {
        b.push(0x80 | opcode);
        b.push(127);
        b.extend_from_slice(&(n as u64).to_be_bytes());
    }
    b.extend_from_slice(payload);
    b
}

pub fn send_frame(s: &mut dyn Write, opcode: u8, payload: &[u8]) -> Result<(), WsErr> {
    s.write_all(&frame_bytes(opcode, payload)).map_err(|e| os_err(&e))
}

/// One decoded client frame: (opcode, unmasked payload, the FIN bit (0x80 or 0), bytes read past it).
pub type Frame = (u8, Vec<u8>, u8, Vec<u8>);

// py: wslink.py:87 read_frame
/// Client frames must be masked (RFC 6455 5.1). `pending` = bytes already read past the request head.
pub fn read_frame(s: &mut dyn Recv, pending: Vec<u8>) -> Result<Frame, WsErr> {
    let mut pending = pending;
    fn take(s: &mut dyn Recv, pending: &mut Vec<u8>, n: usize) -> Result<Vec<u8>, WsErr> {
        if pending.len() >= n {
            let rest = pending.split_off(n);
            return Ok(std::mem::replace(pending, rest));
        }
        let mut out = std::mem::take(pending);
        let more = recv_exact(s, n - out.len())?;
        out.extend_from_slice(&more);
        Ok(out)
    }
    let h = take(s, &mut pending, 2)?;
    let (b1, b2) = (h[0], h[1]);
    let fin = b1 & 0x80;
    let opcode = b1 & 0x0F;
    let masked = b2 & 0x80;
    let mut n = (b2 & 0x7F) as u64;
    if n == 126 {
        let x = take(s, &mut pending, 2)?;
        n = u16::from_be_bytes([x[0], x[1]]) as u64;
    } else if n == 127 {
        let x = take(s, &mut pending, 8)?;
        n = u64::from_be_bytes([x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]]);
    }
    if n > MAX_FRAME {
        return Err(WsErr::Value(format!("frame of {} bytes", n)));
    }
    if masked == 0 {
        return Err(WsErr::Value("unmasked client frame".into()));
    }
    let mask = take(s, &mut pending, 4)?;
    let mut data = take(s, &mut pending, n as usize)?;
    for (i, b) in data.iter_mut().enumerate() {
        *b ^= mask[i & 3];
    }
    Ok((opcode, data, fin, pending))
}

/// Python `str(ipaddress.ip_address(s))`, None when it raises ValueError.
// py: wslink.py:114 client_ip (ipaddress.ip_address)
pub fn ip_text(s: &str) -> Option<String> {
    fn v4(s: &str) -> Option<[u8; 4]> {
        if s.contains('/') {
            return None;
        }
        let parts: Vec<&str> = s.split('.').collect();
        if parts.len() != 4 {
            return None;
        }
        let mut o = [0u8; 4];
        for (i, p) in parts.iter().enumerate() {
            if p.is_empty() || !p.chars().all(|c| c.is_ascii_digit()) || p.len() > 3 || (*p != "0" && p.starts_with('0')) {
                return None;
            }
            let x: u32 = p.parse().ok()?;
            if x > 255 {
                return None;
            }
            o[i] = x as u8;
        }
        Some(o)
    }
    fn v6(s: &str) -> Option<String> {
        if s.contains('/') {
            return None;
        }
        let (addr, scope) = match s.split_once('%') {
            Some((a, sc)) => {
                if sc.is_empty() || sc.contains('%') {
                    return None;
                }
                (a, Some(sc))
            }
            None => (s, None),
        };
        if addr.is_empty() {
            return None;
        }
        let mut parts: Vec<String> = addr.split(':').map(|x| x.to_string()).collect();
        if parts.len() < 3 {
            return None;
        }
        if parts[parts.len() - 1].contains('.') {
            let last = parts.pop()?;
            let o = v4(&last)?;
            let n = u32::from_be_bytes(o);
            parts.push(format!("{:x}", (n >> 16) & 0xFFFF));
            parts.push(format!("{:x}", n & 0xFFFF));
        }
        if parts.len() > 9 {
            return None;
        }
        let mut skip: Option<usize> = None;
        for i in 1..parts.len() - 1 {
            if parts[i].is_empty() {
                if skip.is_some() {
                    return None;
                }
                skip = Some(i);
            }
        }
        let (hi, lo): (usize, usize);
        if let Some(si) = skip {
            let mut parts_hi = si as i64;
            let mut parts_lo = (parts.len() - si - 1) as i64;
            if parts[0].is_empty() {
                parts_hi -= 1;
                if parts_hi != 0 {
                    return None;
                }
            }
            if parts[parts.len() - 1].is_empty() {
                parts_lo -= 1;
                if parts_lo != 0 {
                    return None;
                }
            }
            let skipped = 8 - (parts_hi + parts_lo);
            if skipped < 1 {
                return None;
            }
            hi = parts_hi as usize;
            lo = parts_lo as usize;
        } else {
            if parts.len() != 8 || parts[0].is_empty() || parts[parts.len() - 1].is_empty() {
                return None;
            }
            hi = 8;
            lo = 0;
        }
        let hex = |h: &str| -> Option<u16> {
            if !h.chars().all(|c| c.is_ascii_hexdigit()) || h.len() > 4 {
                return None;
            }
            if h.is_empty() {
                return Some(0);
            }
            u16::from_str_radix(h, 16).ok()
        };
        let mut hextets: Vec<u16> = Vec::new();
        for p in parts.iter().take(hi) {
            hextets.push(hex(p)?);
        }
        if skip.is_some() {
            for _ in 0..(8 - (hi + lo)) {
                hextets.push(0);
            }
            for p in parts.iter().skip(parts.len() - lo) {
                hextets.push(hex(p)?);
            }
        }
        if hextets.len() != 8 {
            return None;
        }
        // _compress_hextets: the first longest run of two or more zeros becomes "::"
        let (mut best_start, mut best_len, mut start, mut len) = (-1i64, 0i64, -1i64, 0i64);
        for (i, h) in hextets.iter().enumerate() {
            if *h == 0 {
                len += 1;
                if start == -1 {
                    start = i as i64;
                }
                if len > best_len {
                    best_len = len;
                    best_start = start;
                }
            } else {
                len = 0;
                start = -1;
            }
        }
        let mut out: Vec<String> = hextets.iter().map(|h| format!("{:x}", h)).collect();
        if best_len > 1 {
            let best_end = (best_start + best_len) as usize;
            if best_end == out.len() {
                out.push(String::new());
            }
            out.splice(best_start as usize..best_end, [String::new()]);
            if best_start == 0 {
                out.insert(0, String::new());
            }
        }
        let mut t = out.join(":");
        if let Some(sc) = scope {
            t.push('%');
            t.push_str(sc);
        }
        Some(t)
    }
    if let Some(o) = v4(s) {
        return Some(format!("{}.{}.{}.{}", o[0], o[1], o[2], o[3]));
    }
    v6(s)
}

// py: wslink.py:114 client_ip
pub fn client_ip(hdr: &IndexMap<String, String>, peer0: &str) -> String {
    let h = hdr.get("cf-connecting-ip").map(|s| s.as_str()).unwrap_or("");
    let ip = if h.is_empty() { peer0 } else { h };
    ip_text(strip(ip)).unwrap_or_else(|| peer0.to_string())
}

/// `base64.b64encode`
fn b64(b: &[u8]) -> String {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::new();
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        out.push(A[(n >> 18) as usize & 63] as char);
        out.push(A[(n >> 12) as usize & 63] as char);
        out.push(if c.len() > 1 { A[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if c.len() > 2 { A[n as usize & 63] as char } else { '=' });
    }
    out
}

/// `base64(sha1(key.encode() + GUID))`
pub fn accept_key(key: &str) -> String {
    let mut d = key.as_bytes().to_vec();
    d.extend_from_slice(GUID);
    b64(&crate::social::sha1(&d))
}

/// The 101 answer of the handshake.
pub fn handshake_response(key: &str) -> Vec<u8> {
    format!("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n", accept_key(key)).into_bytes()
}

// ---------------------------------------------------------------- the socket loop
struct Stats {
    live: AtomicI64,
    opened: AtomicI64,
    refused: AtomicI64,
    bytes_up: AtomicI64,
    bytes_down: AtomicI64,
}

static STATS: Stats = Stats { live: AtomicI64::new(0), opened: AtomicI64::new(0), refused: AtomicI64::new(0), bytes_up: AtomicI64::new(0), bytes_down: AtomicI64::new(0) };
static LOCK: Mutex<()> = Mutex::new(());

// py: wslink.py:36 log
fn log(msg: &str) {
    let mut o = std::io::stdout().lock();
    let _ = writeln!(o, "[{}] {}", crate::clock::hms_now(), msg);
    let _ = o.flush();
}

fn set_nodelay(s: &TcpStream) {
    let _ = s.set_nodelay(true);
}

// py: wslink.py:127 serve
fn serve(mut c: TcpStream, peer_ip: String, daemon_addr: (String, u16), max_live: i64) {
    let mut d: Option<TcpStream> = None;
    let mut ip = peer_ip.clone();
    let r: Result<(), WsErr> = (|| {
        let _ = c.set_read_timeout(Some(Duration::from_secs(15)));
        let _ = c.set_write_timeout(Some(Duration::from_secs(15)));
        let (path, hdr, rest) = read_request(&mut c)?;
        ip = client_ip(&hdr, &peer_ip);
        let up = hdr.get("upgrade").map(|s| s.to_lowercase()).unwrap_or_default();
        if up != "websocket" || !hdr.contains_key("sec-websocket-key") || !path.starts_with("/link") {
            c.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").map_err(|e| os_err(&e))?;
            return Ok(());
        }
        {
            let _g = LOCK.lock();
            if STATS.live.load(Ordering::Relaxed) >= max_live {
                STATS.refused.fetch_add(1, Ordering::Relaxed);
                c.write_all(b"HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").map_err(|e| os_err(&e))?;
                log(&format!("refused {}: {} links open", ip, STATS.live.load(Ordering::Relaxed)));
                return Ok(());
            }
            STATS.live.fetch_add(1, Ordering::Relaxed);
            STATS.opened.fetch_add(1, Ordering::Relaxed);
        }
        let r2: Result<(), WsErr> = (|| {
            let addrs = (daemon_addr.0.as_str(), daemon_addr.1).to_socket_addrs().map_err(|e| os_err(&e))?;
            let mut last = WsErr::Os("getaddrinfo returns an empty list".into());
            let mut conn: Option<TcpStream> = None;
            for a in addrs {
                match TcpStream::connect_timeout(&a, Duration::from_secs(10)) {
                    Ok(s) => {
                        conn = Some(s);
                        break;
                    }
                    Err(e) => last = os_err(&e),
                }
            }
            let mut ds = conn.ok_or(last)?;
            set_nodelay(&ds);
            ds.write_all(&proxy_line(&ip)).map_err(|e| os_err(&e))?;
            let key = hdr.get("sec-websocket-key").cloned().unwrap_or_default();
            c.write_all(&handshake_response(&key)).map_err(|e| os_err(&e))?;
            set_nodelay(&c);
            let idle = Some(Duration::from_secs(IDLE_S));
            let _ = c.set_read_timeout(idle);
            let _ = c.set_write_timeout(idle);
            let _ = ds.set_read_timeout(idle);
            let _ = ds.set_write_timeout(idle);
            d = ds.try_clone().ok();
            pipe(&c, ds, rest);
            Ok(())
        })();
        STATS.live.fetch_sub(1, Ordering::Relaxed);
        r2
    })();
    if let Err(e) = r {
        log(&format!("link {} ended: {}", ip, e.text()));
    }
    let _ = c.shutdown(Shutdown::Both);
    if let Some(d) = d {
        let _ = d.shutdown(Shutdown::Both);
    }
}

// py: wslink.py:178 pipe
/// WebSocket c <-> TCP d until either side closes.
fn pipe(c: &TcpStream, d: TcpStream, pending: Vec<u8>) {
    let done = Arc::new(AtomicBool::new(false));
    let wlock = Arc::new(Mutex::new(()));
    let (tx, rx) = std::sync::mpsc::channel::<()>();
    let (dc, cc, done2, wl2) = (d.try_clone(), c.try_clone(), done.clone(), wlock.clone());
    if let (Ok(mut dr), Ok(mut cw)) = (dc, cc) {
        let spawned = std::thread::Builder::new().name("wslink-down".into()).stack_size(512 * 1024).spawn(move || {
            // daemon -> player
            let mut buf = vec![0u8; 65536];
            while !done2.load(Ordering::Relaxed) {
                match dr.read(&mut buf) {
                    Ok(0) => break,
                    Ok(k) => {
                        let r = {
                            let _g = wl2.lock();
                            send_frame(&mut cw, 0x2, &buf[..k])
                        };
                        if r.is_err() {
                            break;
                        }
                        STATS.bytes_down.fetch_add(k as i64, Ordering::Relaxed);
                    }
                    Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                    Err(_) => break,
                }
            }
            done2.store(true, Ordering::Relaxed);
            {
                let _g = wl2.lock();
                let _ = send_frame(&mut cw, 0x8, b"");
            }
            let _ = cw.shutdown(Shutdown::Both);
            let _ = tx.send(());
        });
        if spawned.is_err() {
            done.store(true, Ordering::Relaxed);
        }
    }
    let mut cr = match c.try_clone() {
        Ok(x) => x,
        Err(_) => return,
    };
    let mut cw = match c.try_clone() {
        Ok(x) => x,
        Err(_) => return,
    };
    let mut dw = match d.try_clone() {
        Ok(x) => x,
        Err(_) => return,
    };
    let mut pending = pending;
    while !done.load(Ordering::Relaxed) {
        match read_frame(&mut cr, std::mem::take(&mut pending)) {
            Ok((op, data, _fin, p)) => {
                pending = p;
                if op == 0x0 || op == 0x1 || op == 0x2 {
                    if !data.is_empty() {
                        if dw.write_all(&data).is_err() {
                            break;
                        }
                        STATS.bytes_up.fetch_add(data.len() as i64, Ordering::Relaxed);
                    }
                } else if op == 0x8 {
                    break;
                } else if op == 0x9 {
                    let _g = wlock.lock();
                    if send_frame(&mut cw, 0xA, &data).is_err() {
                        break;
                    }
                }
            }
            Err(_) => break,
        }
    }
    done.store(true, Ordering::Relaxed);
    let _ = d.shutdown(Shutdown::Both);
    let _ = rx.recv_timeout(Duration::from_secs(5));
}

// py: wslink.py:226 main
/// `wslink [--listen H:P] [--daemon H:P] [--max N]`; returns the exit code (never returns while serving).
pub fn run_cli(args: &[String]) -> i32 {
    let (mut listen, mut daemon, mut max) = ("127.0.0.1:7004".to_string(), "127.0.0.1:7001".to_string(), 300i64);
    let mut i = 0;
    while i < args.len() {
        let (k, v) = match args[i].split_once('=') {
            Some((k, v)) => (k.to_string(), Some(v.to_string())),
            None => (args[i].clone(), None),
        };
        let val = match v {
            Some(v) => v,
            None => {
                i += 1;
                match args.get(i) {
                    Some(x) => x.clone(),
                    None => {
                        eprintln!("usage: wslink [--listen H:P] [--daemon H:P] [--max N]");
                        return 2;
                    }
                }
            }
        };
        match k.as_str() {
            "--listen" => listen = val,
            "--daemon" => daemon = val,
            "--max" => match val.trim().parse() {
                Ok(n) => max = n,
                Err(_) => {
                    eprintln!("wslink: --max: invalid int value: {}", crate::py::repr_str(&val));
                    return 2;
                }
            },
            _ => {
                eprintln!("usage: wslink [--listen H:P] [--daemon H:P] [--max N]");
                return 2;
            }
        }
        i += 1;
    }
    let (Some((lh, lp)), Some((dh, dp))) = (listen.rsplit_once(':'), daemon.rsplit_once(':')) else {
        eprintln!("wslink: need host:port");
        return 2;
    };
    if !["127.0.0.1", "::1", "localhost"].contains(&lh) {
        eprintln!("wslink listens on loopback only (the tunnel is the way in; CF-Connecting-IP is trusted)");
        return 1;
    }
    let Ok(lport) = lp.parse::<u16>() else {
        eprintln!("wslink: bad port");
        return 2;
    };
    let Ok(dport) = dp.parse::<u16>() else {
        eprintln!("wslink: bad port");
        return 2;
    };
    let host = if lh == "localhost" { "127.0.0.1" } else { lh };
    let srv = match TcpListener::bind((host, lport)) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("{}", os_err(&e).text());
            return 1;
        }
    };
    log(&format!("wslink on {} -> daemon {}:{} (max {} links)", listen, dh, dport, max));
    let mut last = crate::clock::wall_time();
    loop {
        let (c, peer) = match srv.accept() {
            Ok(x) => x,
            Err(_) => continue,
        };
        let da = (dh.to_string(), dport);
        let pip = peer.ip().to_string();
        let _ = std::thread::Builder::new().name("wslink-conn".into()).stack_size(512 * 1024).spawn(move || serve(c, pip, da, max));
        if crate::clock::wall_time() - last > 600.0 {
            last = crate::clock::wall_time();
            log(&format!(
                "stats: {} open, {{'opened': {}, 'refused': {}, 'bytes_up': {}, 'bytes_down': {}}}",
                STATS.live.load(Ordering::Relaxed),
                STATS.opened.load(Ordering::Relaxed),
                STATS.refused.load(Ordering::Relaxed),
                STATS.bytes_up.load(Ordering::Relaxed),
                STATS.bytes_down.load(Ordering::Relaxed)
            ));
        }
    }
}

// ---------------------------------------------------------------- differential test
/// A socket that hands out scripted chunks (`recv(n)` = the next chunk cut to n bytes; no chunk left = closed).
struct Fake {
    chunks: std::collections::VecDeque<Vec<u8>>,
}

impl Recv for Fake {
    fn recv(&mut self, n: usize) -> Result<Vec<u8>, WsErr> {
        match self.chunks.pop_front() {
            None => Ok(Vec::new()),
            Some(mut c) => {
                if c.len() > n {
                    let rest = c.split_off(n);
                    self.chunks.push_front(rest);
                }
                Ok(c)
            }
        }
    }
}

fn unhex(h: &str) -> Vec<u8> {
    (0..h.len() / 2).filter_map(|k| u8::from_str_radix(&h[2 * k..2 * k + 2], 16).ok()).collect()
}

fn hexs(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}

/// `t2r-daemon selftest wslink`: the Rust side of verify/py/wslink_harness.py (the pure parts: request head, frames, address, handshake).
pub fn selftest(input: &mut dyn std::io::BufRead, out: &mut dyn std::io::Write) -> i32 {
    use crate::c_util::js;
    use serde_json::{json, Value};
    let errv = |e: WsErr| json!(["ERR", e.class(), e.text()]);
    for (i, c) in crate::c_util::script(input).iter().enumerate() {
        let chunks: std::collections::VecDeque<Vec<u8>> = c.get("chunks").and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|x| x.as_str().map(unhex)).collect()).unwrap_or_default();
        let mut fake = Fake { chunks };
        let r: Value = match js(c, "op") {
            "head" => match read_request(&mut fake) {
                Ok((p, h, rest)) => {
                    let m: serde_json::Map<String, Value> = h.into_iter().map(|(k, v)| (k, json!(v))).collect();
                    json!([p, Value::Object(m), hexs(&rest), fake.chunks.len()])
                }
                Err(e) => errv(e),
            },
            "frame" => match read_frame(&mut fake, unhex(js(c, "pending"))) {
                Ok((op, data, fin, pend)) => json!([op, hexs(&data), fin, hexs(&pend), fake.chunks.len()]),
                Err(e) => errv(e),
            },
            "out" => json!(hexs(&frame_bytes(c.get("opcode").and_then(|x| x.as_u64()).unwrap_or(0) as u8, &unhex(js(c, "payload"))))),
            "ip" => {
                let mut h: IndexMap<String, String> = IndexMap::new();
                if let Some(v) = c.get("hdr").and_then(|x| x.as_str()) {
                    h.insert("cf-connecting-ip".into(), v.to_string());
                }
                json!([client_ip(&h, js(c, "peer")), ip_text(js(c, "text"))])
            }
            "accept" => json!([accept_key(js(c, "key")), hexs(&handshake_response(js(c, "key"))), hexs(&proxy_line(js(c, "ip")))]),
            "errno" => {
                let code = c.get("code").and_then(|x| x.as_i64()).unwrap_or(0) as i32;
                json!(os_err(&std::io::Error::from_raw_os_error(code)).text())
            }
            _ => json!("?"),
        };
        crate::c_util::emit(out, i, r, Value::Null);
    }
    0
}
