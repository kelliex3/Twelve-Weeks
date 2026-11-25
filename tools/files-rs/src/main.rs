mod api;
mod ban;
mod daemonlog;
mod files;
mod http;
mod reports;

use api::{Api, Reply};
use files::{is_blob_name, is_sha, serve_file, FileReply};
use http::{is_loopback, pct_decode, read_head, send_json, Request};
use reports::{Limits, RateLimiter};
use serde_json::json;
use std::io::{self, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

const READ_TIMEOUT: Duration = Duration::from_secs(30);
const WRITE_TIMEOUT: Duration = Duration::from_secs(60);

pub struct Config {
    listen: String,
    data: PathBuf,
    db: Option<PathBuf>,
    status_file: Option<PathBuf>,
    public_url: String,
    reports: Option<PathBuf>,
    daemon_log_dir: PathBuf,
    archive_days: f64,
    max_conns: usize,
    pub rate_mbit: u64,
    stats_secs: u64,
    window: f64,
    limits: Limits,
}

#[derive(Default)]
pub struct Stats {
    active: AtomicUsize,
    pub file_reqs: AtomicU64,
    pub file_full: AtomicU64,
    pub file_404: AtomicU64,
    api: AtomicU64,
    api_err: AtomicU64,
    reports: AtomicU64,
    refused: AtomicU64,
    pub sent: AtomicU64,
}

pub struct Shared {
    pub cfg: Config,
    pub stats: Stats,
    pub bucket: Option<Mutex<(f64, Instant)>>,
    api: Api,
    report_limit: RateLimiter,
    clip_limit: RateLimiter,
    clip_lock: Mutex<()>,
    cache: Mutex<std::collections::HashMap<String, (Instant, Arc<Encoded>)>>,
}

fn client_ip(req: &Request, peer: &SocketAddr) -> (String, bool) {
    if is_loopback(&peer.ip()) {
        match req.header("cf-connecting-ip").map(str::trim).filter(|s| !s.is_empty()) {
            Some(ip) => (ip.to_string(), true),
            None => (peer.ip().to_string(), false),
        }
    } else {
        (peer.ip().to_string(), true)
    }
}

fn base_url(sh: &Shared, req: &Request, peer: &SocketAddr) -> String {
    if !sh.cfg.public_url.is_empty() {
        return sh.cfg.public_url.trim_end_matches('/').to_string();
    }
    let https = is_loopback(&peer.ip()) && req.header("x-forwarded-proto").map_or(false, |p| p.trim().eq_ignore_ascii_case("https"));
    format!("{}://{}", if https { "https" } else { "http" }, req.header("host").unwrap_or("localhost"))
}

fn send_reply(s: &mut TcpStream, r: &Reply, head: bool) -> io::Result<()> {
    send_json(s, r.code, &r.body, head)
}

/// An API reply encoded once: the plain JSON and, from 1024 bytes up, its gzip (server.py gzip_cached: level 6, header mtime 0).
pub struct Encoded {
    code: u16,
    plain: Vec<u8>,
    gz: Option<Vec<u8>>,
}

fn encode(r: Reply) -> Encoded {
    let plain = r.body.into_bytes();
    let gz = if plain.len() >= 1024 {
        let mut e = flate2::GzBuilder::new().mtime(0).write(Vec::with_capacity(plain.len() / 3), flate2::Compression::new(6));
        e.write_all(&plain).ok().and_then(|_| e.finish().ok())
    } else {
        None
    };
    Encoded { code: r.code, plain, gz }
}

/// (11-24 bandwidth, live server.py _json) JSON goes out gzipped when the caller accepts it: Cloudflare and Caddy always do. Every reply
/// says Vary: Accept-Encoding so a cache in between keeps the two forms apart.
fn send_encoded(s: &mut TcpStream, req: &Request, e: &Encoded, head: bool) -> io::Result<()> {
    let gz = e.gz.is_some() && req.header("accept-encoding").map_or(false, |v| v.to_ascii_lowercase().contains("gzip"));
    let body: &[u8] = if gz { e.gz.as_deref().unwrap_or(&[]) } else { &e.plain };
    let extra = if gz { "Cache-Control: no-cache\r\nVary: Accept-Encoding\r\nContent-Encoding: gzip\r\n" } else { "Cache-Control: no-cache\r\nVary: Accept-Encoding\r\n" };
    http::send_body(s, e.code, "application/json", extra, body, head)
}

/// (11-25 live server.py reply cache) the polled replies every open launcher asks for again and again, encoded once per TTL. Key = the
/// request target (path + query) plus the base URL the manifest embeds.
fn cached(sh: &Shared, key: String, ttl: f64, build: impl FnOnce() -> Reply) -> Arc<Encoded> {
    let now = Instant::now();
    if let Some((t, e)) = sh.cache.lock().unwrap().get(&key) {
        if now.duration_since(*t).as_secs_f64() < ttl {
            return e.clone();
        }
    }
    let e = Arc::new(encode(guarded(sh, build)));
    if e.code < 500 {
        let mut c = sh.cache.lock().unwrap();
        if c.len() > 4000 {
            c.retain(|_, (t, _)| now.duration_since(*t).as_secs_f64() < 30.0);
        }
        c.insert(key, (now, e.clone()));
    }
    e
}

fn guarded(sh: &Shared, f: impl FnOnce() -> Reply) -> Reply {
    sh.stats.api.fetch_add(1, Ordering::Relaxed);
    let r = f();
    if r.code >= 500 {
        sh.stats.api_err.fetch_add(1, Ordering::Relaxed);
    }
    r
}

struct Body<'a> {
    pending: &'a mut Vec<u8>,
    s: &'a mut TcpStream,
}

impl Read for Body<'_> {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if !self.pending.is_empty() {
            let n = out.len().min(self.pending.len());
            out[..n].copy_from_slice(&self.pending[..n]);
            self.pending.drain(..n);
            return Ok(n);
        }
        self.s.read(out)
    }
}

fn expect_continue(s: &mut TcpStream, req: &Request) -> io::Result<()> {
    if req.header("expect").map_or(false, |e| e.eq_ignore_ascii_case("100-continue")) {
        s.write_all(b"HTTP/1.1 100 Continue\r\n\r\n")?;
    }
    Ok(())
}

fn post_report(sh: &Shared, s: &mut TcpStream, req: &Request, peer: &SocketAddr, pending: &mut Vec<u8>) -> io::Result<bool> {
    let root = match &sh.cfg.reports {
        Some(r) => r.clone(),
        None => {
            send_json(s, 503, &json!({"ok": false, "error": "reports are not enabled on this server"}).to_string(), false)?;
            return Ok(false);
        }
    };
    let (ip, _) = client_ip(req, peer);
    let n = req.content_length().unwrap_or(0);
    if n == 0 || n > reports::MAX_BODY {
        let msg = format!("bad size (max {} MB)", reports::MAX_BODY >> 20);
        send_json(s, if n > 0 { 413 } else { 400 }, &json!({"ok": false, "error": msg}).to_string(), false)?;
        return Ok(false);
    }
    if !sh.report_limit.allow(&ip) {
        send_json(s, 429, &json!({"ok": false, "error": "too many reports, try again later"}).to_string(), false)?;
        return Ok(false);
    }
    expect_continue(s, req)?;
    let mut body = Vec::with_capacity(n as usize);
    let mut rd = Body { pending, s: &mut *s }.take(n);
    if rd.read_to_end(&mut body).is_err() {
        return Ok(false);
    }
    if body.len() as u64 != n {
        send_json(s, 400, &json!({"ok": false, "error": "short body"}).to_string(), false)?;
        return Ok(false);
    }
    sh.stats.reports.fetch_add(1, Ordering::Relaxed);
    match reports::save_report(&root, &body, &ip, &sh.clip_limit, &sh.cfg.limits) {
        Ok(r) => {
            log!("report {} stored in {}", r.code, r.dir.display());
            daemonlog::snapshot_later(r.dir.clone(), sh.cfg.daemon_log_dir.clone(), sh.cfg.archive_days);
            send_json(s, 200, &json!({"ok": true, "code": r.code, "clipKey": r.clip_key, "clipRefused": r.clip_refused}).to_string(), false)?;
            Ok(true)
        }
        Err(e) => {
            send_json(s, e.status, &json!({"ok": false, "error": e.msg}).to_string(), false)?;
            Ok(true)
        }
    }
}

fn put_clip(sh: &Shared, s: &mut TcpStream, req: &Request, peer: &SocketAddr, pending: &mut Vec<u8>) -> io::Result<bool> {
    let root = match &sh.cfg.reports {
        Some(r) => r.clone(),
        None => {
            send_json(s, 503, &json!({"ok": false, "error": "reports are not enabled on this server"}).to_string(), false)?;
            return Ok(false);
        }
    };
    let n = req.content_length().unwrap_or(0);
    if n == 0 || n > reports::CLIP_MAX_EACH {
        let msg = if n > 0 { format!("Clip too big - trim it to under {} MB", reports::CLIP_MAX_EACH >> 20) } else { "empty clip".into() };
        send_json(s, if n > 0 { 413 } else { 400 }, &json!({"ok": false, "error": msg}).to_string(), false)?;
        return Ok(false);
    }
    let q = req.query();
    let code = q.first("code").unwrap_or("").to_string();
    let key = q.first("key").unwrap_or("").to_string();
    let (ip, _) = client_ip(req, peer);
    expect_continue(s, req)?;
    let r = {
        let mut rd = Body { pending, s: &mut *s }.take(n);
        reports::save_clip(&root, &code, &key, n, &mut rd, &sh.clip_lock, &sh.cfg.limits)
    };
    match r {
        Ok(c) => {
            log!("report {} clip {} ({} bytes) from {}", code, c.name, c.size, ip);
            send_json(s, 200, &json!({"ok": true, "name": c.name, "size": c.size}).to_string(), false)?;
            Ok(true)
        }
        Err(e) => {
            send_json(s, e.status, &json!({"ok": false, "error": e.msg}).to_string(), false)?;
            Ok(false)
        }
    }
}

fn get(sh: &Shared, s: &mut TcpStream, req: &Request, peer: &SocketAddr) -> io::Result<()> {
    let head = req.method == "HEAD";
    let path = req.path();
    let data = &sh.cfg.data;
    let nofile = "{\"error\":\"no such file\"}";
    if let Some(n) = path.strip_prefix("/files/") {
        if !is_blob_name(n) {
            sh.stats.file_404.fetch_add(1, Ordering::Relaxed);
            return send_json(s, 404, nofile, head);
        }
        return serve_file(sh, s, req, &data.join("blobs").join(n), &plain(n, nofile));
    }
    if let Some(n) = path.strip_prefix("/download/") {
        let missing = "{\"error\":\"no such download\"}";
        let name = pct_decode(n, false);
        let sha = name.as_deref().and_then(|x| api::downloads_sha(data, x));
        return match (name, sha) {
            (Some(name), Some(h)) => {
                let r = FileReply { etag: &h, name: Some(&name), ctype: "application/octet-stream", cache: None, missing, max_size: None };
                serve_file(sh, s, req, &data.join("blobs").join(&h), &r)
            }
            _ => {
                sh.stats.file_404.fetch_add(1, Ordering::Relaxed);
                send_json(s, 404, missing, head)
            }
        };
    }
    if let Some(n) = path.strip_prefix("/media/") {
        return match api::media_type(n) {
            Some(ct) => {
                let p = data.join("media").join(n);
                let etag = std::fs::metadata(&p)
                    .map(|m| {
                        let mt = m.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map(|d| d.as_secs()).unwrap_or(0);
                        format!("m-{}-{}", mt, m.len())
                    })
                    .unwrap_or_default();
                let r = FileReply { etag: &etag, name: None, ctype: ct, cache: Some("public, max-age=86400"), missing: nofile, max_size: Some(6 << 20) };
                serve_file(sh, s, req, &p, &r)
            }
            None => send_json(s, 404, nofile, head),
        };
    }
    let q = req.query();
    if path == "/part1-maps.json" || path.starts_with("/part1-files/") {
        let sid = match sh.api.part1_sid(q.first("sid").unwrap_or("")) {
            Some(x) => x,
            None => return send_json(s, 404, "{\"error\":\"no Part I map pack published\"}", head),
        };
        if let Some(n) = path.strip_prefix("/part1-files/") {
            if !is_sha(n) {
                return send_json(s, 404, nofile, head);
            }
            return serve_file(sh, s, req, &data.join("part1").join("blobs").join(n), &plain(n, nofile));
        }
        let base = base_url(sh, req, peer);
        let r = guarded(sh, || sh.api.part1_maps(&base, &sid));
        return send_reply(s, &r, head);
    }
    let e = match path {
        "/manifest.json" => {
            let base = base_url(sh, req, peer);
            cached(sh, format!("{} {}", req.target, base), 10.0, || sh.api.manifest(&q, &base))
        }
        "/news.json" => cached(sh, "/news.json".into(), 10.0, || sh.api.news()),
        "/clanfeed.json" => cached(sh, req.target.clone(), 20.0, || sh.api.clanfeed(&q)),
        "/status.json" => {
            let (ip, real) = client_ip(req, peer);
            Arc::new(encode(guarded(sh, || sh.api.status(if real { Some(&ip) } else { None }))))
        }
        "/versions.json" => Arc::new(encode(guarded(sh, || sh.api.versions(&q)))),
        "/clan.json" => Arc::new(encode(guarded(sh, || sh.api.clan(&q)))),
        "/ban-check" => Arc::new(encode(guarded(sh, || sh.api.ban_check(&q, &req.target)))),
        _ => Arc::new(encode(api::reply(404, &json!({"error": "not found"})))),
    };
    send_encoded(s, req, &e, head)
}

fn plain<'a>(etag: &'a str, missing: &'a str) -> FileReply<'a> {
    FileReply { etag, name: None, ctype: "application/octet-stream", cache: None, missing, max_size: None }
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
                    let _ = send_json(&mut s, 400, "{\"error\":\"bad request\"}", false);
                }
                break;
            }
        };
        if req.header("transfer-encoding").is_some() {
            let _ = send_json(&mut s, 411, "{\"error\":\"send a Content-Length\"}", false);
            break;
        }
        let clen = match req.content_length() {
            Ok(n) => n,
            Err(()) => {
                let _ = send_json(&mut s, 400, "{\"error\":\"bad Content-Length\"}", false);
                break;
            }
        };
        let keep = match (req.method.as_str(), req.path()) {
            ("GET", _) | ("HEAD", _) => {
                if clen > 0 {
                    if clen > buf.len() as u64 {
                        break;
                    }
                    buf.drain(..clen as usize);
                }
                get(&sh, &mut s, &req, &peer).is_ok()
            }
            ("POST", "/report") => post_report(&sh, &mut s, &req, &peer, &mut buf).unwrap_or(false),
            ("PUT", "/report-clip") => put_clip(&sh, &mut s, &req, &peer, &mut buf).unwrap_or(false),
            ("POST", _) | ("PUT", _) => {
                let _ = send_json(&mut s, 404, "{\"error\":\"not found\"}", false);
                false
            }
            _ => {
                let _ = send_json(&mut s, 501, "{\"error\":\"unsupported method\"}", false);
                false
            }
        };
        if !keep || !req.keep_alive() {
            break;
        }
    }
    let _ = s.shutdown(Shutdown::Both);
}

fn env_num(k: &str, d: f64) -> f64 {
    std::env::var(k).ok().and_then(|v| v.trim().parse().ok()).unwrap_or(d)
}

fn parse_args() -> Config {
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    let mut c = Config {
        listen: "0.0.0.0:7002".into(),
        data: PathBuf::from("update-data"),
        db: None,
        status_file: None,
        public_url: String::new(),
        reports: None,
        daemon_log_dir: std::env::var("T2R_DAEMON_LOG_DIR").map(PathBuf::from).unwrap_or_else(|_| PathBuf::from(&home).join("t2r-live").join("host186")),
        archive_days: env_num("T2R_DAEMON_ARCHIVE_DAYS", 3.0),
        max_conns: 1500,
        rate_mbit: 0,
        stats_secs: 60,
        window: env_num("T2R_LAUNCHER_WINDOW_S", 75.0),
        limits: Limits {
            disk_max: (env_num("T2R_REPORTS_MAX_GB", 20.0) as u64) << 30,
            min_free: (env_num("T2R_REPORTS_MIN_FREE_GB", 10.0) as u64) << 30,
        },
    };
    let mut reports: Option<String> = None;
    let (mut host, mut port): (Option<String>, Option<String>) = (None, None);
    let a: Vec<String> = std::env::args().skip(1).collect();
    let mut i = 0;
    while i < a.len() {
        let v = a.get(i + 1).cloned().unwrap_or_default();
        let opt = |x: &str| if x.is_empty() { None } else { Some(PathBuf::from(x)) };
        match a[i].as_str() {
            "--listen" => c.listen = v,
            "--host" => host = Some(v),
            "--port" => port = Some(v),
            "--data" => c.data = PathBuf::from(v),
            "--db" => c.db = opt(&v),
            "--status-file" => c.status_file = opt(&v),
            "--public-url" => c.public_url = v,
            "--reports" => reports = Some(v),
            "--daemon-log-dir" => c.daemon_log_dir = PathBuf::from(v),
            "--max-conns" => c.max_conns = v.parse().unwrap_or(c.max_conns),
            "--rate-mbit" => c.rate_mbit = v.parse().unwrap_or(0),
            "--stats-secs" => c.stats_secs = v.parse().unwrap_or(60),
            "--backend" | "--daemon" => log!("{} {} ignored: nothing is proxied or probed any more", a[i], v),
            x => {
                eprintln!("unknown argument {}", x);
                std::process::exit(2);
            }
        }
        i += 2;
    }
    if host.is_some() || port.is_some() {
        c.listen = format!("{}:{}", host.unwrap_or_else(|| "0.0.0.0".into()), port.unwrap_or_else(|| "7002".into()));
    }
    c.reports = match reports {
        Some(r) if r.is_empty() => None,
        Some(r) => Some(PathBuf::from(r)),
        None => Some(c.data.join("reports")),
    };
    c
}

fn main() {
    let cfg = parse_args();
    let listener = TcpListener::bind(&cfg.listen).unwrap_or_else(|e| {
        eprintln!("bind {}: {}", cfg.listen, e);
        std::process::exit(3);
    });
    log!(
        "t2r-files on {} | data {} | db {} | status {} | reports {} | max conns {} | rate {}",
        cfg.listen,
        cfg.data.display(),
        cfg.db.as_ref().map_or("-".into(), |p| p.display().to_string()),
        cfg.status_file.as_ref().map_or("-".into(), |p| p.display().to_string()),
        cfg.reports.as_ref().map_or("off".into(), |p| p.display().to_string()),
        cfg.max_conns,
        if cfg.rate_mbit > 0 { format!("{} Mbit/s", cfg.rate_mbit) } else { "unlimited".into() }
    );
    let bucket = if cfg.rate_mbit > 0 { Some(Mutex::new((0.0, Instant::now()))) } else { None };
    let api = Api::new(cfg.data.clone(), cfg.db.clone(), cfg.status_file.clone(), cfg.window);
    let sh = Arc::new(Shared {
        cfg,
        stats: Stats::default(),
        bucket,
        api,
        report_limit: RateLimiter::new(reports::RATE_N, reports::RATE_WINDOW),
        clip_limit: RateLimiter::new(reports::CLIP_RATE_N, reports::RATE_WINDOW),
        clip_lock: Mutex::new(()),
        cache: Mutex::new(std::collections::HashMap::new()),
    });
    if let Some(sf) = sh.cfg.status_file.clone() {
        let out = sf.parent().map(|p| p.to_path_buf()).unwrap_or_default().join("t2r-launchers.json");
        let sh = sh.clone();
        std::thread::spawn(move || loop {
            if let Err(e) = api::write_launchers(&sh.api, &out) {
                log!("launchers file: {}", e);
            }
            std::thread::sleep(Duration::from_secs(15));
        });
    }
    if sh.cfg.db.is_some() {
        std::thread::spawn(|| {
            if !t2r_clan::warm() {
                log!("clan templates did not load: /clanfeed.json menus stay empty");
            }
        });
    }
    if sh.cfg.reports.is_some() {
        daemonlog::archive_loop(sh.cfg.daemon_log_dir.clone(), sh.cfg.archive_days);
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
                    "stats: conns {} | file reqs {} (full {}, 404 {}) | api {} (errors {}) | reports {} | launchers {} | refused {} | sent {:.1} MB, {:.0} Mbit/s",
                    st.active.load(Ordering::Relaxed),
                    st.file_reqs.load(Ordering::Relaxed),
                    st.file_full.load(Ordering::Relaxed),
                    st.file_404.load(Ordering::Relaxed),
                    st.api.load(Ordering::Relaxed),
                    st.api_err.load(Ordering::Relaxed),
                    st.reports.load(Ordering::Relaxed),
                    sh.api.launchers_open(),
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
                std::thread::sleep(Duration::from_millis(20));
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
            let _ = send_json(&mut s, 503, "{\"error\":\"server busy, try again in a moment\"}", false);
            continue;
        }
        sh.stats.active.fetch_add(1, Ordering::Relaxed);
        let sh2 = sh.clone();
        let r = std::thread::Builder::new().stack_size(1 << 20).spawn(move || {
            handle(sh2.clone(), s, peer);
            sh2.stats.active.fetch_sub(1, Ordering::Relaxed);
        });
        if r.is_err() {
            sh.stats.active.fetch_sub(1, Ordering::Relaxed);
        }
    }
}
