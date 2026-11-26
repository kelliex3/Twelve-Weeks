//! One scripted game client: a Steam helper connection (0xF0), a held session connection (0x0f + 0x0e, then 0x01 / polls), and short
//! RPC connections for every other op, as the real client (winmm + t2r-ext + helper) uses them.
use crate::proto::{ascii, frame, hex, op_name, pad, Splitter};
use crate::rec::Rec;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

pub struct Ctx {
    pub host: String,
    pub port: u16,
    pub admin_port: u16,
    pub workdir: PathBuf,
    pub rec: Arc<Rec>,
    pub version: String,
    /// directional ping (ms) a helper reports for a pair: (reporter idx, other idx); default PING_DEFAULT
    pub pings: Mutex<std::collections::HashMap<(usize, usize), i32>>,
    /// what the daemon's client_versions.json / linux_clients.json must hold once saved (every valid 0xB0 report, last one wins)
    pub versions: Mutex<std::collections::BTreeMap<String, String>>,
    pub linux: Mutex<std::collections::BTreeSet<String>>,
}

pub const PING_DEFAULT: i32 = 30;

impl Ctx {
    pub fn ping(&self, a: usize, b: usize) -> i32 {
        *self.pings.lock().unwrap().get(&(a, b)).unwrap_or(&PING_DEFAULT)
    }
    pub fn set_ping(&self, a: usize, b: usize, ms: i32) {
        self.pings.lock().unwrap().insert((a, b), ms);
    }
}

#[derive(Clone)]
pub struct Hist {
    pub stream: String,
    pub op: u8,
    pub pl: Vec<u8>,
}

pub struct Shared {
    pub name: String,
    pub rec: Arc<Rec>,
    pub hist: Mutex<Vec<Hist>>,
    pub cv: Condvar,
    /// record 0x03 / 0x17 on the session stream (otherwise they are poll replies: kept for waiting, not in the transcript)
    pub rec_all: AtomicBool,
    pub paused: AtomicBool,
    pub closing: AtomicBool,
}

impl Shared {
    pub fn mark(&self) -> usize {
        self.hist.lock().unwrap().len()
    }
    pub fn wait<F: Fn(&Hist) -> bool>(&self, from: usize, timeout: Duration, f: F) -> Option<(usize, Hist)> {
        let end = Instant::now() + timeout;
        let mut g = self.hist.lock().unwrap();
        let mut i = from;
        loop {
            while i < g.len() {
                if f(&g[i]) {
                    return Some((i, g[i].clone()));
                }
                i += 1;
            }
            let now = Instant::now();
            if now >= end {
                return None;
            }
            let (ng, _) = self.cv.wait_timeout(g, end - now).unwrap();
            g = ng;
        }
    }
    pub fn wait_op(&self, from: usize, stream: &str, op: u8, timeout: Duration) -> Option<(usize, Hist)> {
        self.wait(from, timeout, |h| h.op == op && (stream.is_empty() || h.stream == stream))
    }
    /// every frame of `op` seen on a stream since `from`
    pub fn all_of(&self, from: usize, stream: &str, op: u8) -> Vec<Vec<u8>> {
        let g = self.hist.lock().unwrap();
        g[from.min(g.len())..].iter().filter(|h| h.op == op && (stream.is_empty() || h.stream == stream)).map(|h| h.pl.clone()).collect()
    }
}

pub struct Bot {
    pub idx: usize,
    pub name: String,
    pub token: String,
    pub sid: u64,
    pub hport: u16,
    pub ctx: Arc<Ctx>,
    pub sh: Arc<Shared>,
    bridge: Option<Arc<Mutex<TcpStream>>>,
    sess: Option<Arc<Mutex<TcpStream>>>,
    /// the session socket the poller writes to (replaced on a new login)
    sess_cell: Arc<Mutex<Option<Arc<Mutex<TcpStream>>>>>,
    stop: Arc<AtomicBool>,
    poller: Option<std::thread::JoinHandle<()>>,
}

pub const STEAMID_BASE: u64 = 76561197960265728;

/// `hex(b"T2N:" + name + b"\0" + pad)`: the token layout of the real client (the name rides inside), padding fixed per bot.
pub fn make_token(name: &str, idx: usize) -> String {
    let mut raw = b"T2N:".to_vec();
    raw.extend_from_slice(&name.as_bytes()[..name.len().min(15)]);
    raw.push(0);
    let mut k = 0u8;
    while raw.len() < 32 {
        raw.push(0x40u8.wrapping_add((idx as u8).wrapping_mul(7)).wrapping_add(k));
        k = k.wrapping_add(13);
    }
    hex(&raw)
}

/// Payload column of a reply: hex and text; the friends reply (JSON with ages and seconds) is printed normalised instead.
fn show(op: u8, p: &[u8]) -> String {
    if op == 0xD9 {
        if let Ok(mut v) = serde_json::from_slice::<serde_json::Value>(p) {
            crate::norm::norm_json(&mut v);
            return format!("json {}", serde_json::to_string(&crate::admin::sorted(&v)).unwrap_or_default());
        }
    }
    if op == 0xBB {
        // notice lines "id|kind|param|text": the param of a restart notice (kind r) is the seconds left
        let t = String::from_utf8_lossy(p).to_string();
        let masked: Vec<String> = t
            .split("\n")
            .map(|l| {
                let f: Vec<&str> = l.splitn(4, '|').collect();
                if f.len() == 4 && f[1] == "r" {
                    format!("{}|r|N|{}", f[0], f[3])
                } else {
                    l.to_string()
                }
            })
            .collect();
        return format!("text {}", masked.join(" / "));
    }
    if op == 0xA9 || op == 0xAA || op == 0xAB {
        // the achievement table carries unix times (unlock stamps): text with those masked, no hex
        let t = String::from_utf8_lossy(p).to_string();
        return format!("text {}", crate::norm::mask_epoch(&t).replace("\n", " / "));
    }
    format!("{} ; {}", hex(p), ascii(p))
}

fn connect(ctx: &Ctx) -> std::io::Result<TcpStream> {
    let s = TcpStream::connect((ctx.host.as_str(), ctx.port))?;
    let _ = s.set_nodelay(true);
    Ok(s)
}

impl Bot {
    pub fn new(ctx: &Arc<Ctx>, idx: usize, name: &str) -> Bot {
        Bot {
            idx,
            name: name.to_string(),
            token: make_token(name, idx),
            sid: STEAMID_BASE + 1000 + idx as u64,
            hport: 40000 + idx as u16,
            ctx: ctx.clone(),
            sh: Arc::new(Shared {
                name: name.to_string(),
                rec: ctx.rec.clone(),
                hist: Mutex::new(Vec::new()),
                cv: Condvar::new(),
                rec_all: AtomicBool::new(false),
                paused: AtomicBool::new(false),
                closing: AtomicBool::new(false),
            }),
            bridge: None,
            sess: None,
            sess_cell: Arc::new(Mutex::new(None)),
            stop: Arc::new(AtomicBool::new(false)),
            poller: None,
        }
    }

    pub fn log(&self, s: &str) {
        self.ctx.rec.script(format!("[{}] {}", self.name, s));
    }

    fn spawn_reader(&self, sock: TcpStream, stream: &'static str, helper: Option<Arc<Mutex<TcpStream>>>) {
        let sh = self.sh.clone();
        let ctx = self.ctx.clone();
        let me = self.idx;
        std::thread::spawn(move || {
            let mut sock = sock;
            let mut sp = Splitter::default();
            let mut buf = [0u8; 8192];
            loop {
                match sock.read(&mut buf) {
                    Ok(0) | Err(_) => {
                        if !sh.closing.load(Ordering::SeqCst) {
                            sh.rec.note(&sh.name, stream, "closed by the server");
                        }
                        let mut g = sh.hist.lock().unwrap();
                        g.push(Hist { stream: stream.to_string(), op: 0xFF, pl: b"EOF".to_vec() });
                        sh.cv.notify_all();
                        return;
                    }
                    Ok(n) => {
                        for (op, pl) in sp.push(&buf[..n]) {
                            if op == 0xF5 && pl.len() >= 8 {
                                // the helper's ping estimate to the player behind that Steam id (the real helper asks Steam's relay network)
                                if let Some(w) = &helper {
                                    let sid = u64::from_le_bytes(pl[0..8].try_into().unwrap());
                                    let other = sid.wrapping_sub(STEAMID_BASE + 1000) as usize;
                                    let mut rp = sid.to_le_bytes().to_vec();
                                    rp.extend_from_slice(&ctx.ping(me, other).to_le_bytes());
                                    sh.rec.frame(&sh.name, stream, "<-", op, &pl);
                                    sh.rec.frame(&sh.name, stream, "->", 0xF6, &rp);
                                    let _ = w.lock().unwrap().write_all(&frame(0xF6, &rp));
                                    let mut g = sh.hist.lock().unwrap();
                                    g.push(Hist { stream: stream.to_string(), op, pl });
                                    sh.cv.notify_all();
                                    continue;
                                }
                            }
                            let pollish = stream == "sess" && (op == 0x03 || op == 0x17);
                            if !pollish || sh.rec_all.load(Ordering::SeqCst) {
                                sh.rec.frame(&sh.name, stream, "<-", op, &pl);
                            }
                            let mut g = sh.hist.lock().unwrap();
                            g.push(Hist { stream: stream.to_string(), op, pl });
                            sh.cv.notify_all();
                        }
                    }
                }
            }
        });
    }

    fn send(&self, sock: &Arc<Mutex<TcpStream>>, data: &[u8]) -> bool {
        sock.lock().unwrap().write_all(data).is_ok()
    }

    /// 0xF0 on its own connection, kept open: the Steam-only gate asks for a live helper with a Steam id and a port.
    pub fn helper_up(&mut self) -> bool {
        let Ok(s) = connect(&self.ctx) else {
            self.log("helper: connect failed");
            return false;
        };
        let mut pl = Vec::new();
        pl.extend_from_slice(self.token.as_bytes());
        pl.extend_from_slice(&self.sid.to_le_bytes());
        pl.push(self.name.len() as u8);
        pl.extend_from_slice(self.name.as_bytes());
        pl.extend_from_slice(&self.hport.to_le_bytes());
        let m = self.sh.mark();
        let rd = s.try_clone().unwrap();
        let sock = Arc::new(Mutex::new(s));
        self.spawn_reader(rd, "bridge", Some(sock.clone()));
        self.log(&format!("helper -> 0xf0 helper-register sid={} port={}", self.sid, self.hport));
        self.send(&sock, &frame(0xF0, &pl));
        self.bridge = Some(sock);
        let ok = self.sh.wait_op(m, "bridge", 0xF1, Duration::from_secs(5)).is_some();
        self.log(&format!("helper <- 0xf1 ack={}", ok));
        self.ping_location();
        ok
    }

    /// 0xF4: the helper's Steam ping location (data centre = ms+jitter), as the real helper reports it.
    pub fn ping_location(&self) {
        let loc = format!("fra={}+2,ams={}+2,lhr={}+3,par={}+3", 18 + self.idx, 22 + self.idx, 30 + self.idx, 28 + self.idx);
        self.log(&format!("helper -> 0xf4 ping-location {}", loc));
        if let Some(b) = &self.bridge {
            self.send(b, &frame(0xF4, loc.as_bytes()));
        }
        std::thread::sleep(Duration::from_millis(250));
    }

    /// 0xB0 version report (own short connection). `ver` is the build string; `linux` appends the 'L' marker.
    pub fn version(&self, ver: &str, linux: bool) -> Vec<(u8, Vec<u8>)> {
        let mut pl = vec![ver.len() as u8];
        pl.extend_from_slice(ver.as_bytes());
        pl.extend_from_slice(self.token.as_bytes());
        if linux {
            pl.push(b'L');
        }
        let valid = !ver.is_empty() && ver.len() <= 40 && ver.chars().all(|c| c.is_ascii_alphanumeric() || "._-+".contains(c));
        if valid {
            self.ctx.versions.lock().unwrap().insert(self.token.clone(), ver.to_string());
            if linux {
                self.ctx.linux.lock().unwrap().insert(self.token.clone());
            }
        }
        self.rpc(0xB0, &pl, Some(0xB1))
    }

    fn login_frame(&self) -> Vec<u8> {
        let mut pl = vec![0x1c];
        pl.extend_from_slice(self.token.as_bytes());
        frame(0x0f, &pl)
    }

    fn name_frame(&self, op: u8, total: usize) -> Vec<u8> {
        let mut pl = vec![0x1c];
        pl.extend_from_slice(&[self.idx as u8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77]);
        pl.extend_from_slice(&pad(&self.name, 32));
        pl.resize(total, 0);
        frame(op, &pl)
    }

    /// Menu session: 0x0f login + 0x0e announce in one segment; the daemon keeps the socket open. Starts the keepalive poller.
    pub fn boot(&mut self) -> bool {
        let Ok(s) = connect(&self.ctx) else {
            self.log("boot: connect failed");
            return false;
        };
        let m = self.sh.mark();
        self.spawn_reader(s.try_clone().unwrap(), "sess", None);
        let sock = Arc::new(Mutex::new(s));
        let mut data = self.login_frame();
        data.extend_from_slice(&self.name_frame(0x0e, 41));
        self.log("sess -> 0x0f login + 0x0e announce");
        self.send(&sock, &data);
        *self.sess_cell.lock().unwrap() = Some(sock.clone());
        self.sess = Some(sock);
        let ok = self.sh.wait_op(m, "sess", 0x14, Duration::from_secs(6)).is_some();
        self.log(&format!("sess boot reply={}", ok));
        self.start_poller();
        ok
    }

    pub fn start_poller(&mut self) {
        if self.poller.is_some() {
            return;
        }
        let sess_cell = self.sess_cell.clone();
        let bridge = self.bridge.clone();
        let stop = self.stop.clone();
        let sh = self.sh.clone();
        self.poller = Some(std::thread::spawn(move || {
            let mut n = 0u32;
            while !stop.load(Ordering::SeqCst) {
                for _ in 0..15 {
                    std::thread::sleep(Duration::from_millis(100));
                    if stop.load(Ordering::SeqCst) {
                        return;
                    }
                }
                n += 1;
                if !sh.paused.load(Ordering::SeqCst) {
                    let cur = sess_cell.lock().unwrap().clone();
                    if let Some(s) = cur {
                        let _ = s.lock().unwrap().write_all(&frame(0x05, &[]));
                    }
                }
                if n % 12 == 0 {
                    if let Some(b) = &bridge {
                        let _ = b.lock().unwrap().write_all(&frame(0xF3, &[]));
                    }
                }
            }
        }));
    }

    pub fn pause_poll(&self, on: bool) {
        self.sh.paused.store(on, Ordering::SeqCst);
        if on {
            std::thread::sleep(Duration::from_millis(300)); // let a poll reply that is already on its way land
        }
    }

    /// Run `f` with the poller paused and every frame recorded (including the 0x03 / 0x17 searching lists).
    pub fn explicit<R>(&self, f: impl FnOnce() -> R) -> R {
        self.pause_poll(true);
        self.sh.rec_all.store(true, Ordering::SeqCst);
        let r = f();
        self.sh.rec_all.store(false, Ordering::SeqCst);
        self.sh.paused.store(false, Ordering::SeqCst);
        r
    }

    /// Frame on the held session connection (recorded in the script).
    pub fn sess_send(&self, op: u8, pl: &[u8]) -> bool {
        self.log(&format!("sess -> 0x{:02x} {} len={} {}", op, op_name(op), pl.len(), hex(pl)));
        match &self.sess {
            Some(s) => self.send(s, &frame(op, pl)),
            None => false,
        }
    }

    /// FIND MATCH (0x01 on the held session): the ack 0x03 + 0x17 is recorded.
    pub fn search(&self) -> bool {
        self.explicit(|| {
            let m = self.sh.mark();
            self.log("sess -> 0x01 find-match");
            let ok = match &self.sess {
                Some(s) => self.send(s, &self.name_frame(0x01, 67)),
                None => false,
            };
            let got = self.sh.wait_op(m, "sess", 0x17, Duration::from_secs(5)).is_some();
            std::thread::sleep(Duration::from_millis(200));
            ok && got
        })
    }

    /// One explicit keepalive poll; its reply (searching list / nothing) is recorded.
    pub fn poll(&self) {
        self.explicit(|| {
            let m = self.sh.mark();
            self.log("sess -> 0x05 keepalive (explicit)");
            if let Some(s) = &self.sess {
                self.send(s, &frame(0x05, &[]));
            }
            let _ = self.sh.wait_op(m, "sess", 0x17, Duration::from_millis(1200));
        })
    }

    /// 0x15 online-count poll on the session socket (reply 0x16).
    pub fn poll_online(&self) {
        let m = self.sh.mark();
        self.sess_send(0x15, &[]);
        let _ = self.sh.wait_op(m, "sess", 0x16, Duration::from_secs(3));
    }

    pub fn leave(&self) {
        self.explicit(|| {
            self.sess_send(0x02, &[]);
            std::thread::sleep(Duration::from_millis(800));
        });
    }

    pub fn loaded(&self) {
        self.sess_send(0x09, &[]);
    }

    pub fn match_end(&self, winner: u8) {
        self.sess_send(0x12, &[winner]);
    }

    /// Short connection: send one frame, collect the reply frames until `expect` arrives (or 5 s), close. Recorded in the script.
    pub fn rpc(&self, op: u8, pl: &[u8], expect: Option<u8>) -> Vec<(u8, Vec<u8>)> {
        self.rpc_raw(&frame(op, pl), op, pl, expect)
    }

    pub fn rpc_raw(&self, data: &[u8], op: u8, pl: &[u8], expect: Option<u8>) -> Vec<(u8, Vec<u8>)> {
        self.log(&format!("rpc -> 0x{:02x} {} len={} {} ; {}", op, op_name(op), pl.len(), hex(pl), ascii(pl)));
        let mut out = Vec::new();
        let Ok(mut s) = connect(&self.ctx) else {
            self.log("rpc <- connect failed");
            return out;
        };
        let _ = s.set_read_timeout(Some(Duration::from_millis(200)));
        if s.write_all(data).is_err() {
            self.log("rpc <- write failed");
            return out;
        }
        let mut sp = Splitter::default();
        let end = Instant::now() + Duration::from_millis(if expect.is_some() { 5000 } else { 600 });
        let mut buf = [0u8; 8192];
        let mut eof = false;
        'o: while Instant::now() < end {
            match s.read(&mut buf) {
                Ok(0) => {
                    eof = true;
                    break;
                }
                Ok(n) => {
                    for (o, p) in sp.push(&buf[..n]) {
                        let hit = Some(o) == expect;
                        out.push((o, p));
                        if hit {
                            break 'o;
                        }
                    }
                }
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock || e.kind() == std::io::ErrorKind::TimedOut => {}
                Err(_) => {
                    eof = true;
                    break;
                }
            }
        }
        for (o, p) in &out {
            self.log(&format!("rpc <- 0x{:02x} {} len={} {}", o, op_name(*o), p.len(), show(*o, p)));
        }
        if out.is_empty() {
            self.log(&format!("rpc <- (no reply{})", if eof { ", closed" } else { "" }));
        }
        out
    }

    /// Client-side close of the session connection (the game quits / the link drops): the daemon sees a FIN.
    pub fn drop_sess(&mut self) {
        self.log("sess: closed by the client");
        self.sh.closing.store(true, Ordering::SeqCst);
        *self.sess_cell.lock().unwrap() = None;
        if let Some(s) = self.sess.take() {
            let _ = s.lock().unwrap().shutdown(std::net::Shutdown::Both);
        }
        std::thread::sleep(Duration::from_millis(300));
        self.sh.closing.store(false, Ordering::SeqCst);
    }

    pub fn drop_bridge(&mut self) {
        self.log("helper: closed by the client");
        self.sh.closing.store(true, Ordering::SeqCst);
        if let Some(s) = self.bridge.take() {
            let _ = s.lock().unwrap().shutdown(std::net::Shutdown::Both);
        }
        std::thread::sleep(Duration::from_millis(300));
        self.sh.closing.store(false, Ordering::SeqCst);
    }

    /// Drop every connection (the whole client is gone).
    pub fn kill(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        self.sh.closing.store(true, Ordering::SeqCst);
        if let Some(s) = self.sess.take() {
            let _ = s.lock().unwrap().shutdown(std::net::Shutdown::Both);
        }
        if let Some(s) = self.bridge.take() {
            let _ = s.lock().unwrap().shutdown(std::net::Shutdown::Both);
        }
    }

    pub fn mark(&self) -> usize {
        self.sh.mark()
    }

    /// Wait for a frame `op` on `stream` after `from`.
    pub fn wait_op(&self, from: usize, stream: &str, op: u8, secs: u64) -> Option<Vec<u8>> {
        self.sh.wait_op(from, stream, op, Duration::from_secs(secs)).map(|x| x.1.pl)
    }

    pub fn seen(&self, from: usize, stream: &str, op: u8) -> bool {
        !self.sh.all_of(from, stream, op).is_empty()
    }

    /// Slot number of the bot in its (latest) 0x04 match record (0 = host).
    pub fn my_slot(&self) -> Option<u8> {
        let g = self.sh.hist.lock().unwrap();
        g.iter().rev().find(|h| h.op == 0x04).and_then(|h| h.pl.get(0x17).copied())
    }
}

impl Drop for Bot {
    fn drop(&mut self) {
        self.kill();
    }
}
