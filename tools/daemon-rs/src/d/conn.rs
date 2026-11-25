//! TCP plumbing of daemon.py: framing, OutConn (bounded, non-blocking writes), the accept loop with the connection caps,
//! and tcp_conn (the per-connection read loop). One OS thread per connection like Python, without the GIL.
// py: daemon.py:2731-2880 (OutConn), 7984-8217 (parse_login .. tcp_conn), 9080-9160 (bind_exclusive, tcp_server)
use super::*;

use parking_lot::Mutex;
use std::collections::{HashMap, VecDeque};
use std::io::{self, Read};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::os::fd::AsRawFd;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, LazyLock};
use std::time::{Duration, Instant};

use crate::{dlog, env_parse, env_str, evt};

pub type Addr = SocketAddr;

/// Python's `str(addr)` of an (ip, port) tuple: `('1.2.3.4', 5678)`.
pub struct PA(pub Addr);
impl std::fmt::Display for PA {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "('{}', {})", self.0.ip(), self.0.port())
    }
}
pub fn pa(a: Addr) -> PA {
    PA(a)
}
pub fn ip_of(a: &Addr) -> String {
    a.ip().to_string()
}

// ---------------------------------------------------------------- framing
// py: daemon.py:410 msg
pub fn msg(op: u8, payload: &[u8]) -> Vec<u8> {
    let n = payload.len() + 1;
    let mut v = Vec::with_capacity(n + 2);
    v.extend_from_slice(&(n as u16).to_le_bytes());
    v.push(op);
    v.extend_from_slice(payload);
    v
}

pub type Batch = Vec<(u8, Vec<u8>)>;

pub const MAX_TCP_BUF: usize = 256 * 1024;

// py: daemon.py:8011 split_frames -> (frames, rest); rest None = hopeless stream
pub fn split_frames(buf: &[u8]) -> (Batch, Option<Vec<u8>>) {
    let mut out = Vec::new();
    let mut i = 0usize;
    while buf.len() - i >= 2 {
        let ln = u16::from_le_bytes([buf[i], buf[i + 1]]) as usize;
        if ln == 0 {
            i += 2;
            continue;
        }
        if buf.len() - i < ln + 2 {
            break;
        }
        out.push((buf[i + 2], buf[i + 3..i + 2 + ln].to_vec()));
        i += 2 + ln;
    }
    let rest = buf[i..].to_vec();
    if rest.len() <= MAX_TCP_BUF {
        (out, Some(rest))
    } else {
        (out, None)
    }
}

// py: daemon.py:7984 parse_login -> (token, loadout)
pub fn parse_login(payload: &[u8]) -> (Option<String>, Option<Vec<u8>>) {
    let mut token = None;
    let mut loadout = None;
    if payload.len() >= 65 && payload[0] == 0x1c {
        let t = &payload[1..65];
        if t.iter().all(|c| c.is_ascii_hexdigit()) {
            token = Some(t.iter().map(|&c| c as char).collect());
        }
    }
    if payload.len() > 100 {
        loadout = Some(payload.to_vec());
    }
    (token, loadout)
}

/// latin1 decode (every byte is one char)
pub fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

/// `b.decode("latin1", "ignore")` on the first 64 bytes of a payload (the token field of the ext's short requests).
pub fn tok64(pl: &[u8]) -> String {
    latin1(crate::util::sl(pl, 0, 64))
}

// py: daemon.py:7999 name_from_session
pub fn name_from_session(payload: &[u8]) -> Option<String> {
    if payload.len() >= 41 {
        let f = &payload[9..41];
        let end = f.iter().position(|&c| c == 0).unwrap_or(f.len());
        let s = latin1(&f[..end]);
        if s.is_empty() {
            None
        } else {
            Some(s)
        }
    } else {
        None
    }
}

// py: daemon.py:8005 NOISY_OPS
pub const NOISY_OPS: [u8; 24] = [
    0xBC, 0xCC, 0xBA, 0xB4, 0xB6, 0x05, 0x15, 0xE3, 0xE6, 0xDC, 0xEA, 0xE8, 0xE0, 0xD0, 0xF3, 0xF4, 0xF6, 0xDA, 0xC6, 0xC8, 0xCA, 0x13, 0xDE, 0xBF,
];
pub static NOISY_COUNT: AtomicU64 = AtomicU64::new(0);
// py: POLL_DEADLINE_OPS
pub const POLL_DEADLINE_OPS: [u8; 6] = [0xB4, 0xBF, 0xE0, 0xB6, 0xD0, 0xE8];

pub struct ConnCfg {
    pub host: String,
    pub port: u16,
    pub max_conns: i64,
    pub max_per_ip: i64,
    pub per_ip_loopback: bool,
    pub partial_frame_s: f64,
    pub flood_max_recvs: i64,
    pub first_frame_s: f64,
    pub poll_deadline_s: f64,
    pub sendq_max_bytes: usize,
    pub sendq_max_frames: usize,
    pub sendq_stall_s: f64,
    pub tcp_sndbuf: i64,
    pub bind_retry_s: f64,
}
pub const FLOOD_WINDOW_S: f64 = 10.0;
pub const SENDQ_LINGER_S: f64 = 5.0;

pub static CONN_CFG: LazyLock<ConnCfg> = LazyLock::new(|| ConnCfg {
    host: env_str("T2R_HOST", "0.0.0.0"),
    port: env_parse("T2R_PORT", 7001u16),
    max_conns: env_parse("T2R_MAX_CONNS", 512i64),
    max_per_ip: env_parse("T2R_MAX_PER_IP", 64i64),
    per_ip_loopback: env_str("T2R_PER_IP_LOOPBACK", "0") == "1",
    partial_frame_s: env_parse("T2R_PARTIAL_FRAME_S", 30.0),
    flood_max_recvs: env_parse("T2R_FLOOD_MAX_RECVS", 3000i64),
    first_frame_s: env_parse("T2R_FIRST_FRAME_S", 15.0),
    poll_deadline_s: env_parse("T2R_POLL_DEADLINE_S", 6.0),
    sendq_max_bytes: env_parse("T2R_SENDQ_BYTES", 256 * 1024usize),
    sendq_max_frames: env_parse("T2R_SENDQ_FRAMES", 512usize),
    sendq_stall_s: env_parse("T2R_SENDQ_STALL_S", 15.0),
    tcp_sndbuf: env_parse("T2R_TCP_SNDBUF", 0i64),
    bind_retry_s: env_parse("T2R_BIND_RETRY_S", 20.0),
});

// ---------------------------------------------------------------- OutConn
/// py: SENDQ_STATS {"dropped", "killed"}
pub static SENDQ_DROPPED: AtomicU64 = AtomicU64::new(0);
pub static SENDQ_KILLED: AtomicU64 = AtomicU64::new(0);

struct OutState {
    q: VecDeque<(Vec<u8>, bool)>,
    qbytes: usize,
    writer: bool,
    dead: bool,
    closing: bool,
    closed: bool,
}

/// py: daemon.py:2738 OutConn. Writes never block the caller: inline when the kernel takes everything, else queued and drained
/// by a short-lived writer thread. Bounded queue; droppable frames are shed first; a stalled peer is closed.
pub struct OutConn {
    sock: TcpStream,
    pub addr: Addr,
    id: u64,
    st: Mutex<OutState>,
}

static CONN_IDS: AtomicU64 = AtomicU64::new(1);

pub type Conn = Arc<OutConn>;

fn brokenpipe(s: &str) -> io::Error {
    io::Error::new(io::ErrorKind::BrokenPipe, s.to_string())
}

impl OutConn {
    pub fn new(sock: TcpStream, addr: Addr) -> Conn {
        Arc::new(OutConn {
            sock,
            addr,
            id: CONN_IDS.fetch_add(1, Ordering::Relaxed),
            st: Mutex::new(OutState { q: VecDeque::new(), qbytes: 0, writer: false, dead: false, closing: false, closed: false }),
        })
    }
    /// identity (Python compared connection objects with `is`)
    pub fn id(&self) -> u64 {
        self.id
    }
    pub fn same(&self, other: &OutConn) -> bool {
        self.id == other.id
    }
    pub fn queued(&self) -> usize {
        self.st.lock().qbytes
    }
    pub fn is_dead(&self) -> bool {
        let s = self.st.lock();
        s.dead || s.closed
    }
    fn kill(&self, why: &str) {
        {
            let mut s = self.st.lock();
            if s.dead {
                return;
            }
            s.dead = true;
            s.q.clear();
            s.qbytes = 0;
        }
        SENDQ_KILLED.fetch_add(1, Ordering::Relaxed);
        dlog!("SENDQ", "closing connection:", why);
        let _ = self.sock.shutdown(std::net::Shutdown::Both);
    }
    /// send without blocking: Ok(n) bytes taken (0 when the kernel buffer is full)
    fn send_nb(&self, data: &[u8]) -> io::Result<usize> {
        let fd = self.sock.as_raw_fd();
        let n = unsafe { libc::send(fd, data.as_ptr() as *const libc::c_void, data.len(), libc::MSG_DONTWAIT | libc::MSG_NOSIGNAL) };
        if n >= 0 {
            return Ok(n as usize);
        }
        let e = io::Error::last_os_error();
        match e.raw_os_error() {
            Some(libc::EAGAIN) | Some(libc::EINTR) => Ok(0),
            _ => Err(e),
        }
    }
    pub fn sendall(self: &Arc<Self>, data: &[u8]) -> io::Result<()> {
        self.sendall_d(data, false)
    }
    pub fn sendall_d(self: &Arc<Self>, data: &[u8], drop_ok: bool) -> io::Result<()> {
        if data.is_empty() {
            return Ok(());
        }
        let cfg = &*CONN_CFG;
        let mut data = data.to_vec();
        let mut drop_ok = drop_ok;
        let (overflow, start, qb, ql);
        {
            let mut s = self.st.lock();
            if s.dead || s.closing {
                return Err(brokenpipe("connection closed"));
            }
            if s.q.is_empty() && !s.writer {
                match self.send_nb(&data) {
                    Ok(sent) => {
                        if sent >= data.len() {
                            return Ok(());
                        }
                        data.drain(..sent);
                        drop_ok = false;
                    }
                    Err(e) => {
                        s.dead = true;
                        return Err(brokenpipe(&format!("send failed: {}", e)));
                    }
                }
            }
            s.qbytes += data.len();
            s.q.push_back((data, drop_ok));
            if s.qbytes > cfg.sendq_max_bytes || s.q.len() > cfg.sendq_max_frames {
                Self::shed(&mut s, cfg);
            }
            overflow = s.qbytes > cfg.sendq_max_bytes || s.q.len() > cfg.sendq_max_frames;
            start = !s.writer && !overflow;
            if start {
                s.writer = true;
            }
            qb = s.qbytes;
            ql = s.q.len();
        }
        if overflow {
            self.kill(&format!("send queue overflow ({} B / {} frames)", qb, ql));
            return Err(brokenpipe("send queue overflow"));
        }
        if start {
            let me = Arc::clone(self);
            if std::thread::Builder::new().name("sendq".into()).stack_size(64 * 1024).spawn(move || me.run()).is_err() {
                self.st.lock().writer = false;
                self.kill("cannot start writer thread");
                return Err(brokenpipe("no writer thread"));
            }
        }
        Ok(())
    }
    fn shed(s: &mut OutState, cfg: &ConnCfg) {
        let mut nbytes: usize = s.q.iter().map(|i| i.0.len()).sum();
        let mut n = s.q.len();
        let mut out = VecDeque::with_capacity(n);
        for (i, it) in s.q.drain(..).enumerate() {
            if i > 0 && it.1 && (nbytes > cfg.sendq_max_bytes || n > cfg.sendq_max_frames) {
                nbytes -= it.0.len();
                n -= 1;
                SENDQ_DROPPED.fetch_add(1, Ordering::Relaxed);
            } else {
                out.push_back(it);
            }
        }
        s.q = out;
        s.qbytes = nbytes;
    }
    fn poll_w(&self, ms: i32) -> bool {
        let mut p = libc::pollfd { fd: self.sock.as_raw_fd(), events: libc::POLLOUT, revents: 0 };
        let r = unsafe { libc::poll(&mut p, 1, ms) };
        r > 0
    }
    fn run(self: Arc<Self>) {
        let cfg = &*CONN_CFG;
        let mut last_progress = Instant::now();
        let mut linger_end: Option<Instant> = None;
        let mut released = false;
        loop {
            let head;
            {
                let mut s = self.st.lock();
                if s.dead || s.q.is_empty() {
                    s.writer = false;
                    released = true;
                    break;
                }
                if s.closing && linger_end.is_none() {
                    linger_end = Some(Instant::now() + Duration::from_secs_f64(SENDQ_LINGER_S));
                }
                head = s.q[0].0.clone();
            }
            let now = Instant::now();
            if now.duration_since(last_progress).as_secs_f64() > cfg.sendq_stall_s {
                let qb = self.queued();
                self.kill(&format!("peer not reading for {:.0} s ({} B queued)", cfg.sendq_stall_s, qb));
                break;
            }
            if let Some(le) = linger_end {
                if now > le {
                    self.kill(&format!("close(): queue not flushed in {:.0} s", SENDQ_LINGER_S));
                    break;
                }
            }
            if !self.poll_w(500) {
                continue;
            }
            match self.send_nb(&head) {
                Ok(0) => continue,
                Ok(n) => {
                    last_progress = Instant::now();
                    let mut s = self.st.lock();
                    if let Some(f) = s.q.front_mut() {
                        if f.0 == head {
                            if n >= f.0.len() {
                                s.q.pop_front();
                            } else {
                                f.0.drain(..n);
                            }
                            s.qbytes = s.qbytes.saturating_sub(n);
                        }
                    }
                }
                Err(e) => {
                    self.kill(&format!("send failed {}", e));
                    break;
                }
            }
        }
        let done = {
            let mut s = self.st.lock();
            if !released {
                s.writer = false;
            }
            s.closing && !s.writer
        };
        if done {
            self.really_close();
        }
    }
    fn really_close(&self) {
        {
            let mut s = self.st.lock();
            if s.closed {
                return;
            }
            s.closed = true;
        }
        let _ = self.sock.shutdown(std::net::Shutdown::Both);
    }
    /// py: OutConn.close() - close once what is queued is flushed (bounded by SENDQ_LINGER_S)
    pub fn close(&self) {
        {
            let mut s = self.st.lock();
            s.closing = true;
            if s.writer {
                return;
            }
        }
        self.really_close();
    }
    /// py: socket.shutdown(SHUT_RDWR) (used by the RST / session reset paths)
    pub fn shutdown(&self) {
        let _ = self.sock.shutdown(std::net::Shutdown::Both);
    }
    /// py: the RST close used by final_exit / reset_running_sessions (SO_LINGER 0 + close)
    pub fn reset(&self) {
        let fd = self.sock.as_raw_fd();
        let lg = libc::linger { l_onoff: 1, l_linger: 0 };
        unsafe {
            libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_LINGER, &lg as *const _ as *const libc::c_void, std::mem::size_of::<libc::linger>() as u32);
        }
        self.really_close();
    }
    pub fn set_timeout(&self, s: f64) {
        let _ = self.sock.set_read_timeout(if s > 0.0 { Some(Duration::from_secs_f64(s)) } else { None });
    }
    pub fn recv(&self, buf: &mut [u8]) -> io::Result<usize> {
        (&self.sock).read(buf)
    }
}

/// py: daemon.py:2872 push(c, data, drop) - a cross-thread push to another player's connection
pub fn push(c: &Conn, data: &[u8], drop_ok: bool) -> io::Result<()> {
    c.sendall_d(data, drop_ok)
}

// ---------------------------------------------------------------- per-connection state + handler errors
/// py: the `st` dict of tcp_conn
#[derive(Debug, Clone)]
pub struct ConnSt {
    pub token: Option<String>,
    pub name: String,
    pub authed: bool,
    pub session: bool,
    pub is_bridge: bool,
}

/// What a handler can raise (tcp_conn's except clauses).
#[derive(Debug)]
pub enum HErr {
    /// RequestAbandoned: close the connection
    Abandoned,
    /// ConnectionError / socket.timeout: the outer handler logs "ERR" and closes
    Conn(String),
    /// any other exception: "HANDLER ERR (conn kept)"
    Other(String),
}

impl From<io::Error> for HErr {
    fn from(e: io::Error) -> HErr {
        HErr::Conn(e.to_string())
    }
}

impl From<db::DbErr> for HErr {
    fn from(e: db::DbErr) -> HErr {
        match e {
            db::DbErr::Abandoned => HErr::Abandoned,
            db::DbErr::Sql(e) => HErr::Other(format!("OperationalError('{}')", e)),
        }
    }
}

pub type HResult = Result<(), HErr>;

// ---------------------------------------------------------------- new-identity gate
/// py: daemon.py:8043 AddrLimiter - sliding window per key
pub struct AddrLimiter {
    n: usize,
    window: f64,
    hits: Mutex<HashMap<String, VecDeque<f64>>>,
}

impl AddrLimiter {
    pub fn new(n: usize, window: f64) -> AddrLimiter {
        AddrLimiter { n, window, hits: Mutex::new(HashMap::new()) }
    }
    pub fn allow(&self, key: &str, now: Option<f64>) -> bool {
        let now = now.unwrap_or_else(crate::clock::now);
        let mut h = self.hits.lock();
        if h.len() > 4096 {
            let w = self.window;
            h.retain(|_, v| v.back().map(|t| now - t < w).unwrap_or(false));
        }
        let q = h.entry(key.to_string()).or_default();
        while let Some(&f) = q.front() {
            if now - f > self.window {
                q.pop_front();
            } else {
                break;
            }
        }
        if q.len() >= self.n {
            return false;
        }
        q.push_back(now);
        true
    }
    pub fn prune(&self, now: Option<f64>) -> usize {
        let now = now.unwrap_or_else(crate::clock::now);
        let mut h = self.hits.lock();
        let before = h.len();
        let w = self.window;
        h.retain(|_, v| v.back().map(|t| now - t <= w).unwrap_or(false));
        before - h.len()
    }
}

pub static NEW_TOKENS: LazyLock<AddrLimiter> = LazyLock::new(|| AddrLimiter::new(env_parse("T2R_NEW_TOKENS_PER_10MIN", 12usize), 600.0));

// py: daemon.py:8071 is_token
pub fn is_token(t: &str) -> bool {
    t.len() == 64 && t.bytes().all(|c| c.is_ascii_hexdigit())
}

// py: daemon.py:8073 token_known
pub fn token_known(t: &str) -> bool {
    db::db_read(|c| {
        use rusqlite::OptionalExtension;
        c.query_row("SELECT 1 FROM players WHERE token=?", [t], |_| Ok(())).optional()
    })
    .map(|o| o.is_some())
    .unwrap_or(false)
}

fn is_loopback_exempt(addr: Option<&Addr>) -> bool {
    match addr {
        None => true,
        Some(a) => ip_of(a) == "127.0.0.1" && !CONN_CFG.per_ip_loopback,
    }
}

// py: daemon.py:8078 new_identity_ok
pub fn new_identity_ok(token: &str, addr: Option<&Addr>) -> bool {
    if is_loopback_exempt(addr) {
        return true;
    }
    if !is_token(token) {
        return false;
    }
    let ip = addr.map(ip_of).unwrap_or_default();
    token_known(token) || NEW_TOKENS.allow(&ip, None)
}

// py: daemon.py:8089 _creates_rows
pub fn _creates_rows(op: u8, pl: &[u8], addr: &Addr) -> bool {
    if matches!(op, 0xD2 | 0xE4 | 0xEE | 0xEF | 0xE2) && pl.len() >= 64 && !new_identity_ok(&tok64(pl), Some(addr)) {
        dlog!("NEWID", pa(*addr), format!("op 0x{:02x} refused: too many new identities from this address", op));
        return true;
    }
    false
}

pub const STEAMID_MIN: u64 = 76561197960265728;
pub const STEAMID_MAX: u64 = 76561202255233024;

// ---------------------------------------------------------------- tcp_conn
fn ops_subset(ops: &[u8], set: &[u8]) -> bool {
    !ops.is_empty() && ops.iter().all(|o| set.contains(o))
}

/// py: hexcut(b, n=LOG_HEX)
pub fn hexcut(b: &[u8]) -> String {
    let n = *LOG_HEX;
    let cut = crate::util::sl(b, 0, n);
    let mut s = crate::py::hex_sp(cut);
    if b.len() > n {
        s.push_str(&format!(" ...(+{}B)", b.len() - n));
    }
    s
}
pub static LOG_HEX: LazyLock<usize> = LazyLock::new(|| env_parse("T2R_LOG_HEX", 48usize));

enum Rd {
    Data(usize),
    Fin,
    Timeout,
    Err(io::Error),
}

fn rd(conn: &Conn, buf: &mut [u8]) -> Rd {
    match conn.recv(buf) {
        Ok(0) => Rd::Fin,
        Ok(n) => Rd::Data(n),
        Err(e) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut) => Rd::Timeout,
        Err(e) => Rd::Err(e),
    }
}

// py: daemon.py:8099 tcp_conn
pub fn tcp_conn(conn: Conn, addr: Addr) {
    let cfg = &*CONN_CFG;
    conn.set_timeout(cfg.first_frame_s);
    let t_conn = Instant::now();
    let mut got_frame = false;
    let mut st = ConnSt { token: None, name: "PLAYER".into(), authed: false, session: false, is_bridge: false };
    let mut buf: Vec<u8> = Vec::new();
    let mut close_why = "handler ended".to_string();
    let mut partial_since: Option<Instant> = None;
    let mut recv_n: i64 = 0;
    let mut recv_t0 = Instant::now();
    {
        let fd = conn.sock.as_raw_fd();
        let one: libc::c_int = 1;
        unsafe {
            libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_KEEPALIVE, &one as *const _ as *const libc::c_void, 4);
            if cfg.tcp_sndbuf > 0 {
                let v = cfg.tcp_sndbuf as libc::c_int;
                libc::setsockopt(fd, libc::SOL_SOCKET, libc::SO_SNDBUF, &v as *const _ as *const libc::c_void, 4);
            }
        }
    }
    let mut rbuf = vec![0u8; 8192];
    'outer: loop {
        let n = match rd(&conn, &mut rbuf) {
            Rd::Data(n) => n,
            Rd::Fin => {
                close_why = "peer closed (FIN)".into();
                break;
            }
            Rd::Timeout => {
                close_why = "error TimeoutError('timed out')".into();
                dlog!("TCP", pa(addr), "ERR", "timed out");
                break;
            }
            Rd::Err(e) => {
                let r = crate::pyfmt::os_error_repr(&e);
                close_why = format!("error {}", r);
                dlog!("TCP", pa(addr), "ERR", e);
                break;
            }
        };
        let data = rbuf[..n].to_vec();
        let mono = Instant::now();
        if mono.duration_since(recv_t0).as_secs_f64() >= FLOOD_WINDOW_S {
            recv_n = 0;
            recv_t0 = mono;
        }
        recv_n += 1;
        if recv_n > cfg.flood_max_recvs {
            dlog!("TCP", pa(addr), format!("flood: {} reads in {:.0} s, closing", recv_n, FLOOD_WINDOW_S));
            break;
        }
        buf.extend_from_slice(&data);
        let (mut batch, rest) = split_frames(&buf);
        let rest_none = rest.is_none();
        buf = rest.unwrap_or_default();
        if !rest_none && !buf.is_empty() {
            match partial_since {
                None => partial_since = Some(mono),
                Some(_) if !batch.is_empty() => partial_since = Some(mono),
                Some(p) if mono.duration_since(p).as_secs_f64() > cfg.partial_frame_s => {
                    dlog!("TCP", pa(addr), format!("incomplete frame for {:.0} s, closing", cfg.partial_frame_s));
                    break;
                }
                _ => {}
            }
        } else if !rest_none {
            partial_since = None;
        }
        if !batch.is_empty() && !got_frame {
            got_frame = true;
            conn.set_timeout(120.0);
        } else if !got_frame && t_conn.elapsed().as_secs_f64() > cfg.first_frame_s {
            dlog!("TCP", pa(addr), format!("no complete frame in {:.0} s, closing", cfg.first_frame_s));
            break;
        }
        if rest_none {
            dlog!("TCP", pa(addr), "garbage / oversized stream, closing");
            break;
        }
        let mut ops: Vec<u8> = batch.iter().map(|x| x.0).collect();
        ops.sort_unstable();
        ops.dedup();
        if ops_subset(&ops, &NOISY_OPS) {
            NOISY_COUNT.fetch_add(1, Ordering::Relaxed);
        } else {
            dlog!("TCP", pa(addr), format!("recv {}B", data.len()), hexcut(&data));
        }
        if ops.contains(&0x0f) && !ops.iter().any(|o| matches!(o, 0x01 | 0xfc | 0xF0)) {
            conn.set_timeout(0.3);
            let more = rd(&conn, &mut rbuf);
            conn.set_timeout(120.0);
            match more {
                Rd::Data(m) => {
                    let more = rbuf[..m].to_vec();
                    dlog!("TCP", pa(addr), format!("recv {}B (joined)", more.len()), hexcut(&more));
                    buf.extend_from_slice(&more);
                    let (mb, rest) = split_frames(&buf);
                    match rest {
                        None => break 'outer,
                        Some(r) => buf = r,
                    }
                    batch.extend(mb);
                }
                Rd::Fin => {
                    let _ = handle_batch(&conn, addr, &mut st, &mut batch);
                    break 'outer;
                }
                Rd::Timeout => {}
                Rd::Err(e) => {
                    close_why = format!("error {}", crate::pyfmt::os_error_repr(&e));
                    dlog!("TCP", pa(addr), "ERR", e);
                    break 'outer;
                }
            }
        }
        if !batch.is_empty() {
            if cfg.poll_deadline_s > 0.0 && ops_subset(&ops, &POLL_DEADLINE_OPS) && !st.session && !st.is_bridge {
                db::set_deadline(Some(mono + Duration::from_secs_f64(cfg.poll_deadline_s)));
            }
            let r = handle_batch(&conn, addr, &mut st, &mut batch);
            db::set_deadline(None);
            match r {
                Ok(()) => {}
                Err(HErr::Abandoned) => {
                    close_why = "request abandoned (client timed out waiting for the DB)".into();
                    break;
                }
                Err(HErr::Conn(e)) => {
                    close_why = format!("error {}", e);
                    dlog!("TCP", pa(addr), "ERR", e);
                    break;
                }
                Err(HErr::Other(e)) => {
                    dlog!("TCP", pa(addr), "HANDLER ERR (conn kept)", e);
                }
            }
        }
    }
    db::set_deadline(None);
    if let Some(tok) = st.token.clone() {
        if st.session {
            let q = conn.queued();
            let searching = MM.lock.lock().queue.contains_key(&tok);
            let inm = MM.in_match(&tok);
            dlog!(
                "SESSION conn closed",
                tag(&tok),
                format!("after {:.0} s:", t_conn.elapsed().as_secs_f64()),
                close_why,
                if searching { "| searching" } else { "" },
                if inm { "| in match" } else { "" },
                format!("| {} B unsent", q)
            );
        }
        if st.is_bridge && BRIDGES.unregister(&tok, &conn) {
            evt!("bridge-gone", tag(&tok), "Steam helper disconnected");
            if HM.enabled && HM.has(&tok) {
                HM.bridge_gone(&tok);
            }
        }
        if st.session {
            note_session_lost(&tok, &conn);
            SESSIONS.unregister(&tok, &conn);
        }
    }
    conn.close();
}

// ---------------------------------------------------------------- accept loop
struct Live {
    n: i64,
    per_ip: HashMap<String, i64>,
}
static TCP_LIVE: LazyLock<Mutex<Live>> = LazyLock::new(|| Mutex::new(Live { n: 0, per_ip: HashMap::new() }));
pub static TCP_REFUSED: AtomicU64 = AtomicU64::new(0);

/// py: _tcp_live[0] (live TCP connections, the STATUS line `tcp=`)
pub fn tcp_live() -> i64 {
    TCP_LIVE.lock().n
}

fn release(ip: &str, capped: bool) {
    let mut l = TCP_LIVE.lock();
    l.n -= 1;
    if capped {
        let gone = match l.per_ip.get_mut(ip) {
            Some(v) => {
                *v -= 1;
                *v <= 0
            }
            None => false,
        };
        if gone {
            l.per_ip.remove(ip);
        }
    }
}

// py: daemon.py:9080 bind_exclusive + 9109 tcp_server
pub fn tcp_server() {
    let cfg = &*CONN_CFG;
    let listener = match bind_tcp(&cfg.host, cfg.port, cfg.bind_retry_s) {
        Some(l) => l,
        None => std::process::exit(3),
    };
    dlog!("TCP LISTEN", format!("{}:{}", cfg.host, cfg.port));
    loop {
        let (c, a) = match listener.accept() {
            Ok(x) => x,
            Err(e) => {
                dlog!("TCP-ACCEPT-ERR", crate::pyfmt::os_error_repr(&e));
                std::thread::sleep(Duration::from_millis(50));
                continue;
            }
        };
        let ip = ip_of(&a);
        let capped = cfg.per_ip_loopback || ip != "127.0.0.1";
        let (full, ipfull) = {
            let mut l = TCP_LIVE.lock();
            let full = l.n >= cfg.max_conns;
            let ipfull = capped && l.per_ip.get(&ip).copied().unwrap_or(0) >= cfg.max_per_ip;
            if !full && !ipfull {
                l.n += 1;
                if capped {
                    *l.per_ip.entry(ip.clone()).or_insert(0) += 1;
                }
            }
            (full, ipfull)
        };
        if full || ipfull {
            if TCP_REFUSED.load(Ordering::Relaxed) % 50 == 0 {
                let why = if full {
                    format!("too many connections ({})", cfg.max_conns)
                } else {
                    format!("{} live connections from this address", cfg.max_per_ip)
                };
                dlog!("TCP", pa(a), format!("refused: {}", why));
            }
            TCP_REFUSED.fetch_add(1, Ordering::Relaxed);
            drop(c);
            continue;
        }
        let conn = OutConn::new(c, a);
        let ip2 = ip.clone();
        let spawned = std::thread::Builder::new().name("tcp".into()).stack_size(256 * 1024).spawn(move || {
            tcp_conn(conn, a);
            release(&ip2, capped);
        });
        if let Err(e) = spawned {
            dlog!("TCP-THREAD-ERR", format!("RuntimeError('{}')", e));
            release(&ip, capped);
            std::thread::sleep(Duration::from_millis(50));
        }
    }
}

fn bind_tcp(host: &str, port: u16, retry_s: f64) -> Option<TcpListener> {
    use socket2::{Domain, Protocol, Socket, Type};
    let t0 = Instant::now();
    let mut nxt = 1.0f64;
    let addr: SocketAddr = format!("{}:{}", host, port).parse().ok()?;
    loop {
        let r = (|| -> io::Result<TcpListener> {
            let s = Socket::new(Domain::IPV4, Type::STREAM, Some(Protocol::TCP))?;
            s.set_reuse_address(true)?;
            s.bind(&addr.into())?;
            s.listen(256)?;
            Ok(s.into())
        })();
        match r {
            Ok(l) => {
                let el = t0.elapsed().as_secs_f64();
                if el > 0.3 {
                    println!("BIND TCP {}:{} ok after {:.1}s of retries", host, port, el);
                }
                return Some(l);
            }
            Err(e) => {
                let el = t0.elapsed().as_secs_f64();
                if el >= retry_s {
                    println!("FATAL TCP {}:{} already in use after {:.0}s (is another server running?): {}", host, port, retry_s, e);
                    return None;
                }
                if el >= nxt {
                    println!("BIND TCP {}:{} busy ({}); retrying for up to {:.0}s more", host, port, e, retry_s - el);
                    nxt = el + 5.0;
                }
                std::thread::sleep(Duration::from_millis(250));
            }
        }
    }
}
