mod clan;
mod http;
mod news;
mod players;
mod session;
mod site;
mod util;

use http::{Conn, Head, Request, Response};
use news::{Fail, News};
use players::{Ctx, Players};
use serde_json::{json, Value};
use session::{cookie_domain, csrf_for, hostname, session_info, sign, valid_next, SESSION_DAYS, STEAM_OPENID};
use site::send;
use std::collections::HashSet;
use std::fs;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::os::unix::fs::OpenOptionsExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime};
use util::{ct_eq, local_clock, local_stamp, now, parse_qs, py_strip, qget, quote_plus, take_chars, urlencode};

const ADMIN_BODY_MAX: i64 = 300_000;
const MAX_CONNS: usize = 256;
const BOARDS: [&str; 3] = ["elo", "kd", "wins"];
const MEDIA_EXTS: [&str; 7] = [".gif", ".webp", ".png", ".jpg", ".jpeg", ".mp4", ".webm"];

struct Config {
    db: PathBuf,
    news: PathBuf,
    site: PathBuf,
    public_url: String,
    allow_next: Vec<String>,
    host: String,
    port: u16,
}

struct IdFile {
    path: PathBuf,
    parse: fn(&str) -> HashSet<i64>,
    state: Mutex<(Option<SystemTime>, Arc<HashSet<i64>>)>,
}

impl IdFile {
    fn new(path: PathBuf, parse: fn(&str) -> HashSet<i64>) -> IdFile {
        IdFile { path, parse, state: Mutex::new((None, Arc::new(HashSet::new()))) }
    }

    fn get(&self) -> Arc<HashSet<i64>> {
        let mt = fs::metadata(&self.path).and_then(|m| m.modified()).ok();
        let mut g = self.state.lock().unwrap_or_else(|e| e.into_inner());
        match mt {
            None => {
                *g = (None, Arc::new(HashSet::new()));
            }
            Some(t) if g.0 != Some(t) => {
                let ids = util::read_lossy(&self.path).map(|s| (self.parse)(&s)).unwrap_or_default();
                *g = (Some(t), Arc::new(ids));
            }
            _ => {}
        }
        g.1.clone()
    }
}

struct App {
    cfg: Config,
    secret: Vec<u8>,
    players: Players,
    news: News,
    admins: IdFile,
    leads: IdFile,
    csp: String,
    origins: Vec<String>,
    public_host: String,
    cookie_domain: String,
    data_dir: PathBuf,
    active: AtomicUsize,
}

impl App {
    fn is_admin(&self, sid: i64) -> bool {
        sid != 0 && self.admins.get().contains(&sid)
    }

    fn cookie(&self, value: &str, max_age: i64, domain: Option<&str>) -> String {
        let secure = if self.cfg.public_url.starts_with("https://") { "; Secure" } else { "" };
        let d = domain.unwrap_or(&self.cookie_domain);
        let dom = if d.is_empty() { String::new() } else { format!("; Domain={}", d) };
        format!("tw_s={}; Path=/; Max-Age={}; HttpOnly; SameSite=Lax{}{}", value, max_age, secure, dom)
    }

    fn log_admin(&self, sid: i64, line: &str) {
        let msg = format!("{} admin {}: {}\n", local_stamp(now() as i64), sid, line);
        let _ = std::io::stderr().write_all(msg.as_bytes());
        if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(self.data_dir.join("web-admin.log")) {
            let _ = f.write_all(msg.as_bytes());
        }
    }
}

fn json_resp(code: u16, v: &Value, cache: &str) -> Response {
    send(code, serde_json::to_vec(v).unwrap_or_default(), "application/json; charset=utf-8", cache, Vec::new())
}

fn json_err(code: u16, msg: &str) -> Response {
    json_resp(code, &json!({ "error": msg }), "no-store")
}

fn db_err(e: String) -> Response {
    json_resp(503, &json!({"error": "database unavailable", "detail": take_chars(&e, 120)}), "no-store")
}

fn redirect(url: &str, cookies: Vec<String>) -> Response {
    let mut extra = vec![("Location", url.to_string())];
    extra.extend(cookies.into_iter().map(|c| ("Set-Cookie", c)));
    send(302, Vec::new(), "text/plain", "no-store", extra)
}

fn get(app: &App, req: &Request) -> Response {
    let p = req.path();
    let q = parse_qs(req.query());
    let leads;
    match p {
        "/api/news" => {
            site::send_shared(app.news.public_bytes(), "application/json; charset=utf-8", "public, max-age=120")
        }
        "/api/me" => {
            let sid = match session_info(&app.secret, req.header("cookie")) {
                Some((sid, _)) if sid != 0 => sid,
                _ => return json_resp(200, &json!({"loggedIn": false}), "no-store"),
            };
            let snap = match app.players.snapshot() {
                Ok(s) => s,
                Err(e) => return db_err(e),
            };
            leads = app.leads.get();
            let cx = Ctx { snap: &snap, leads: &leads, site: &app.cfg.site };
            let id = format!("s{}", sid);
            let player = snap.get(&id).map_or(Value::Null, |pl| Value::Object(players::public(&cx, pl, true)));
            json_resp(200, &json!({"loggedIn": true, "id": id, "player": player, "admin": app.is_admin(sid)}), "no-store")
        }
        "/api/admin/news" => {
            let (sid, mac) = match session_info(&app.secret, req.header("cookie")) {
                Some((sid, mac)) if app.is_admin(sid) => (sid, mac),
                _ => return json_err(403, "admin only"),
            };
            let _ = sid;
            let v = json!({
                "csrf": csrf_for(&app.secret, &mac),
                "posts": app.news.raw(),
                "icons": news::asset_files(&app.cfg.site, "", &[".png"]),
                "media": news::asset_files(&app.cfg.site, "media", &MEDIA_EXTS),
                "kinds": news::KINDS,
            });
            json_resp(200, &v, "no-store")
        }
        "/api/players" => {
            let s = qget(&q, "q").unwrap_or("");
            let s2 = take_chars(&py_strip(s).to_lowercase(), 32).to_string();
            if s2.chars().count() < 2 {
                return json_resp(200, &json!([]), "no-store");
            }
            let snap = match app.players.snapshot() {
                Ok(s) => s,
                Err(e) => return db_err(e),
            };
            leads = app.leads.get();
            let cx = Ctx { snap: &snap, leads: &leads, site: &app.cfg.site };
            json_resp(200, &players::search(&cx, s), "no-store")
        }
        "/api/player" => {
            let pid = qget(&q, "id").unwrap_or("");
            if !pid_ok(pid) {
                return json_err(404, "not found");
            }
            let snap = match app.players.snapshot() {
                Ok(s) => s,
                Err(e) => return db_err(e),
            };
            leads = app.leads.get();
            let cx = Ctx { snap: &snap, leads: &leads, site: &app.cfg.site };
            match snap.get(pid) {
                Some(pl) if !pl.banned => json_resp(200, &Value::Object(players::public(&cx, pl, true)), "no-store"),
                _ => json_err(404, "not found"),
            }
        }
        "/api/leaderboard" => {
            let by = qget(&q, "by").unwrap_or("elo");
            if !BOARDS.contains(&by) {
                return json_err(400, "unknown board");
            }
            let snap = match app.players.snapshot() {
                Ok(s) => s,
                Err(e) => return db_err(e),
            };
            leads = app.leads.get();
            let cx = Ctx { snap: &snap, leads: &leads, site: &app.cfg.site };
            let rows = players::leaderboard(&cx, by);
            let min = if by == "elo" || by == "kd" { players::MIN_RATE_MATCHES } else { 1 };
            json_resp(200, &json!({"by": by, "minMatches": min, "rows": rows, "statsTracked": snap.stats_tracked}), "public, max-age=60")
        }
        "/auth/steam" => {
            let mut ret = format!("{}/auth/steam/return", app.cfg.public_url);
            if let Some(n) = qget(&q, "next").and_then(|n| valid_next(n, &app.public_host, &app.origins)) {
                ret.push_str("?next=");
                ret.push_str(&quote_plus(&n));
            }
            let params: Vec<(String, String)> = vec![
                ("openid.ns".into(), "http://specs.openid.net/auth/2.0".into()),
                ("openid.mode".into(), "checkid_setup".into()),
                ("openid.return_to".into(), ret),
                ("openid.realm".into(), format!("{}/", app.cfg.public_url)),
                ("openid.identity".into(), "http://specs.openid.net/auth/2.0/identifier_select".into()),
                ("openid.claimed_id".into(), "http://specs.openid.net/auth/2.0/identifier_select".into()),
            ];
            redirect(&format!("{}?{}", STEAM_OPENID, urlencode(&params)), Vec::new())
        }
        "/auth/steam/return" => {
            let sid = match session::steam_verify(&q, &app.cfg.public_url) {
                Some(s) => s,
                None => return redirect("/?login=failed", Vec::new()),
            };
            let exp = now() as i64 + SESSION_DAYS * 86400;
            let rt = session::return_next(qget(&q, "openid.return_to").unwrap_or(""));
            let nxt = if !rt.is_empty() && Some(rt.as_str()) == qget(&q, "next") { valid_next(&rt, &app.public_host, &app.origins) } else { None };
            let to = nxt.unwrap_or_else(|| format!("/player.html?id=s{}", sid));
            let value = format!("{}.{}.{}", sid, exp, sign(&app.secret, &sid.to_string(), exp));
            redirect(&to, vec![app.cookie(&value, SESSION_DAYS * 86400, None)])
        }
        "/auth/logout" => redirect("/", vec![app.cookie("", 0, None), app.cookie("", 0, Some(""))]),
        _ => site::serve(req, &app.cfg.site, &app.csp),
    }
}

fn pid_ok(pid: &str) -> bool {
    if let Some(d) = pid.strip_prefix("s7656") {
        return d.len() == 13 && d.bytes().all(|b| b.is_ascii_digit());
    }
    if let Some(h) = pid.strip_prefix('p') {
        return h.len() == 16 && h.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b));
    }
    false
}

fn io_name(e: &Fail) -> &'static str {
    match e {
        Fail::Io(n) => n,
        _ => "OSError",
    }
}

fn post(app: &App, req: &Request, conn: &mut Conn, consumed: &mut bool) -> Response {
    if req.path() != "/api/admin/news" {
        return json_err(404, "not found");
    }
    let (sid, mac) = match session_info(&app.secret, req.header("cookie")) {
        Some((sid, mac)) if app.is_admin(sid) => (sid, mac),
        _ => return json_err(403, "admin only"),
    };
    let origin = req.header("origin").unwrap_or("");
    let host_hdr = req.header("host").unwrap_or("");
    let host = host_hdr.split(':').next().unwrap_or("");
    let loopback = |h: &str| h == "127.0.0.1" || h == "localhost";
    let ok = origin == app.cfg.public_url || (loopback(host) && loopback(&app.cfg.host) && origin == format!("http://{}", host_hdr));
    if !ok {
        return json_err(403, "bad origin");
    }
    if !ct_eq(req.header("x-csrf").unwrap_or("").as_bytes(), csrf_for(&app.secret, &mac).as_bytes()) {
        return json_err(403, "bad csrf token (reload the page)");
    }
    if !req.header("content-type").unwrap_or("").to_lowercase().starts_with("application/json") {
        return json_err(415, "json only");
    }
    let n: i64 = match req.header("content-length") {
        None => 0,
        Some(v) => util::parse_py_int(v).unwrap_or(-1),
    };
    if n <= 0 || n > ADMIN_BODY_MAX {
        return json_err(413, "body size");
    }
    if req.header("expect").is_some_and(|e| e.eq_ignore_ascii_case("100-continue")) {
        let _ = conn.stream.write_all(b"HTTP/1.1 100 Continue\r\n\r\n");
    }
    let raw = match conn.read_body(n as usize) {
        Ok(b) => b,
        Err(_) => {
            let mut r = json_err(400, "bad json");
            r.close = true;
            return r;
        }
    };
    *consumed = req.header("transfer-encoding").is_none();
    let body = match serde_json::from_slice::<Value>(&raw) {
        Ok(Value::Object(m)) => m,
        _ => return json_err(400, "bad json"),
    };
    let who = sid.to_string();
    match app.news.apply(body.get("action"), &who, body.get("id"), body.get("post"), body.get("big")) {
        Ok((id, line)) => {
            app.log_admin(sid, &line);
            json_resp(200, &json!({"ok": true, "id": id, "posts": app.news.raw()}), "no-store")
        }
        Err(Fail::Invalid(m)) => json_err(400, &m),
        Err(Fail::Corrupt) => json_resp(500, &json!({"error": "could not read the news file", "detail": "JSONDecodeError"}), "no-store"),
        Err(e) => json_resp(500, &json!({"error": "could not write the news file", "detail": io_name(&e)}), "no-store"),
    }
}

fn route(app: &App, req: &Request, conn: &mut Conn, consumed: &mut bool) -> Response {
    match req.method.as_str() {
        "GET" | "HEAD" => get(app, req),
        "POST" => post(app, req, conn, consumed),
        m => {
            let body = format!(
                "<!DOCTYPE HTML>\n<html lang=\"en\">\n    <head>\n        <meta charset=\"utf-8\">\n        <title>Error response</title>\n    </head>\n    <body>\n        <h1>Error response</h1>\n        <p>Error code: 501</p>\n        <p>Message: Unsupported method ('{}').</p>\n        <p>Error code explanation: 501 - Server does not support this operation.</p>\n    </body>\n</html>\n",
                m.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('\'', "&#x27;")
            );
            let mut r = Response::new(501, vec![("Content-Type", "text/html;charset=utf-8".into()), ("Content-Length", body.len().to_string())], body.into_bytes());
            r.close = true;
            r
        }
    }
}

fn handle(app: &App, stream: TcpStream) {
    let _ = stream.set_write_timeout(Some(Duration::from_secs(60)));
    let _ = stream.set_nodelay(true);
    let mut conn = Conn { stream, buf: Vec::new() };
    loop {
        let req = match conn.read_head() {
            Head::Req(r) => r,
            Head::Closed => return,
            Head::Bad => {
                let body = b"Bad request".to_vec();
                let r = Response::new(400, vec![("Content-Type", "text/plain".into()), ("Content-Length", body.len().to_string())], body);
                let _ = conn.write(r, false, false);
                return;
            }
        };
        let mut consumed = !req.has_body();
        let resp = route(app, &req, &mut conn, &mut consumed);
        let keep = req.keep_alive() && consumed && !resp.close;
        let code = resp.code;
        let ok = conn.write(resp, req.method == "HEAD", keep).is_ok();
        let line = format!("{} \"{}\" {} -\n", local_clock(), req.line, code);
        let _ = std::io::stderr().write_all(line.as_bytes());
        if !ok {
            return;
        }
        if !keep {
            if !consumed {
                linger(&mut conn.stream);
            }
            return;
        }
    }
}

fn linger(s: &mut TcpStream) {
    let _ = s.shutdown(std::net::Shutdown::Write);
    let _ = s.set_read_timeout(Some(Duration::from_secs(2)));
    let mut tmp = [0u8; 16384];
    let mut left: usize = 1 << 20;
    while left > 0 {
        match s.read(&mut tmp) {
            Ok(0) | Err(_) => break,
            Ok(n) => left = left.saturating_sub(n),
        }
    }
}

fn usage() -> ! {
    eprintln!("usage: t2r-web --db DB --news NEWS [--site SITE] [--public-url URL] [--allow-next ORIGIN]... [--host HOST] [--port PORT]");
    std::process::exit(2);
}

fn parse_args() -> Config {
    let mut db = None;
    let mut news = None;
    let mut site = PathBuf::from("site");
    let mut public_url = "https://twelve-weeks.com".to_string();
    let mut allow_next: Vec<String> = Vec::new();
    let mut host = "127.0.0.1".to_string();
    let mut port: u16 = 7020;
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        let (k, inline) = match a.split_once('=') {
            Some((k, v)) if k.starts_with("--") => (k.to_string(), Some(v.to_string())),
            _ => (a.clone(), None),
        };
        if k == "-h" || k == "--help" {
            usage();
        }
        let v = match inline.or_else(|| args.next()) {
            Some(v) => v,
            None => usage(),
        };
        match k.as_str() {
            "--db" => db = Some(PathBuf::from(v)),
            "--news" => news = Some(PathBuf::from(v)),
            "--site" => site = PathBuf::from(v),
            "--public-url" => public_url = v,
            "--allow-next" => allow_next.push(v),
            "--host" => host = v,
            "--port" => port = v.parse().unwrap_or_else(|_| usage()),
            _ => usage(),
        }
    }
    let (db, news) = match (db, news) {
        (Some(d), Some(n)) => (d, n),
        _ => usage(),
    };
    if allow_next.is_empty() {
        allow_next.push("https://admin.twelve-weeks.com".into());
    }
    Config {
        db: std::path::absolute(&db).unwrap_or(db),
        news,
        site: std::path::absolute(&site).unwrap_or(site),
        public_url: public_url.trim_end_matches('/').to_string(),
        allow_next: allow_next.iter().map(|o| o.trim_end_matches('/').to_string()).collect(),
        host,
        port,
    }
}

fn load_secret(dir: &Path) -> std::io::Result<Vec<u8>> {
    let sec = dir.join("web-secret");
    if !sec.exists() {
        let mut rnd = [0u8; 32];
        fs::File::open("/dev/urandom")?.read_exact(&mut rnd)?;
        let mut f = fs::OpenOptions::new().write(true).create_new(true).mode(0o600).open(&sec)?;
        f.write_all(util::hex(&rnd).as_bytes())?;
    }
    let text = fs::read_to_string(&sec)?;
    Ok(py_strip(&text).as_bytes().to_vec())
}

fn main() {
    let cfg = parse_args();
    let data_dir = cfg.db.parent().map(Path::to_path_buf).unwrap_or_default();
    let secret = match load_secret(&data_dir) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("web-secret: {}", e);
            std::process::exit(1);
        }
    };
    let origins: Vec<String> = cfg.allow_next.iter().map(|o| o.to_lowercase()).collect();
    let csp = format!("frame-ancestors 'self' {}", origins.join(" "));
    let app = Arc::new(App {
        players: Players::new(cfg.db.clone()),
        news: News::new(cfg.news.clone(), &cfg.site),
        admins: IdFile::new(data_dir.join("web-admins.txt"), util::admin_ids_parse),
        leads: IdFile::new(data_dir.join("devs.txt"), util::lead_ids_parse),
        csp,
        origins,
        public_host: hostname(&cfg.public_url),
        cookie_domain: cookie_domain(&cfg.public_url),
        data_dir,
        secret,
        active: AtomicUsize::new(0),
        cfg,
    });
    let listener = match TcpListener::bind((app.cfg.host.as_str(), app.cfg.port)) {
        Ok(l) => l,
        Err(e) => {
            eprintln!("bind {}:{}: {}", app.cfg.host, app.cfg.port, e);
            std::process::exit(1);
        }
    };
    println!("twelve-weeks web on {}:{} (public {})", app.cfg.host, app.cfg.port, app.cfg.public_url);
    for s in listener.incoming() {
        let s = match s {
            Ok(s) => s,
            Err(_) => {
                std::thread::sleep(Duration::from_millis(50));
                continue;
            }
        };
        if app.active.load(Ordering::Relaxed) >= MAX_CONNS {
            drop(s);
            continue;
        }
        app.active.fetch_add(1, Ordering::Relaxed);
        let a = app.clone();
        let spawned = std::thread::Builder::new().stack_size(256 * 1024).spawn(move || {
            handle(&a, s);
            a.active.fetch_sub(1, Ordering::Relaxed);
        });
        if spawned.is_err() {
            app.active.fetch_sub(1, Ordering::Relaxed);
        }
    }
}
