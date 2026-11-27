use std::collections::{HashMap, HashSet};
use std::net::IpAddr;
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};

use hmac::{Hmac, Mac};
use regex::Regex;
use serde_json::{json, Map, Value};
use sha2::Sha256;

use crate::py;
use crate::store::Admin;
use crate::timeutil;

const CSP: &str = "default-src 'self'; img-src 'self' data:; media-src 'self'; style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; font-src 'self' https://fonts.gstatic.com; script-src 'self'; connect-src 'self'; frame-ancestors 'self'";
const STEAM_SVG: &str = r#"<svg viewBox="0 0 24 24" aria-hidden="true"><path fill="currentColor" d="M12 2a10 10 0 0 0-9.96 9.2l5.35 2.21a2.8 2.8 0 0 1 1.6-.5h.16l2.38-3.45v-.05a3.77 3.77 0 1 1 3.77 3.77h-.09l-3.4 2.42v.13a2.83 2.83 0 0 1-5.6.57L2.4 15.1A10 10 0 1 0 12 2zM8.28 17.17l-1.22-.5a2.12 2.12 0 1 0 1.16-2.9l1.27.52a1.56 1.56 0 1 1-1.2 2.88zm9.51-7.74a2.51 2.51 0 1 0-2.51 2.51 2.51 2.51 0 0 0 2.51-2.51zm-4.4 0a1.89 1.89 0 1 1 1.89 1.89 1.89 1.89 0 0 1-1.89-1.89z"/></svg>"#;
const STATIC_FILES: [&str; 6] = ["admin.css", "admin.js", "clan.html", "clanframe.js", "news.html", "newsframe.js"];

pub struct Config {
    pub port: u16,
    pub users_file: PathBuf,
    pub allow_hosts: Vec<String>,
    pub tunnel_hosts: Vec<String>,
    pub web_secret: PathBuf,
    pub web_admins: PathBuf,
    pub site_url: String,
    pub static_dir: PathBuf,
    pub launcher_root: Option<PathBuf>,
}

type FileSig = Option<(i64, i64, u64, u64)>;

pub struct App {
    pub cfg: Config,
    pub live: Admin,
    pub test: Option<Admin>,
    csp: String,
    fails: Mutex<HashMap<String, (u32, f64)>>,
    users: Mutex<(FileSig, Arc<HashMap<String, String>>)>,
    secret: Mutex<(FileSig, Arc<Vec<u8>>)>,
    admins: Mutex<(FileSig, Arc<HashSet<i64>>)>,
}

pub struct Req {
    pub method: String,
    pub path: String,
    pub query: String,
    pub headers: Vec<(String, String)>,
    pub peer: IpAddr,
    pub body: Option<Vec<u8>>,
}

impl Req {
    fn header(&self, name: &str) -> Option<&str> {
        self.headers.iter().find(|(k, _)| k.eq_ignore_ascii_case(name)).map(|(_, v)| v.as_str())
    }
}

pub enum Body {
    Bytes(Vec<u8>),
    File { path: PathBuf, start: u64, len: u64 },
}

pub struct Resp {
    pub code: u16,
    pub headers: Vec<(String, String)>,
    pub body: Body,
    pub delay: bool,
}

#[derive(Clone)]
enum Ident {
    Local,
    Steam(i64),
    Token(String),
}

fn fsig(p: &Path) -> FileSig {
    std::fs::metadata(p).ok().map(|m| (m.mtime(), m.mtime_nsec(), m.size(), m.ino()))
}

fn read_text(p: &Path) -> String {
    std::fs::read(p).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default()
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;").replace('\'', "&#x27;")
}

fn quote_all(s: &str) -> String {
    let mut out = String::new();
    for b in s.bytes() {
        if b.is_ascii_alphanumeric() || b"_.-~".contains(&b) {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{:02X}", b));
        }
    }
    out
}

fn unquote_plus(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'+' => {
                out.push(b' ');
                i += 1;
            }
            b'%' if i + 2 < b.len() && b[i + 1].is_ascii_hexdigit() && b[i + 2].is_ascii_hexdigit() => {
                let hex = |c: u8| (c as char).to_digit(16).unwrap_or(0) as u8;
                out.push(hex(b[i + 1]) * 16 + hex(b[i + 2]));
                i += 3;
            }
            c => {
                out.push(c);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn parse_qs(q: &str) -> HashMap<String, String> {
    let mut out = HashMap::new();
    for part in q.split('&') {
        let Some((k, v)) = part.split_once('=') else { continue };
        if v.is_empty() {
            continue;
        }
        out.entry(unquote_plus(k)).or_insert_with(|| unquote_plus(v));
    }
    out
}

fn netloc(url: &str) -> String {
    let rest = match url.find(':') {
        Some(i) if i > 0 && url[..i].chars().next().map(|c| c.is_ascii_alphabetic()).unwrap_or(false)
            && url[..i].chars().all(|c| c.is_ascii_alphanumeric() || "+-.".contains(c)) =>
        {
            &url[i + 1..]
        }
        _ => url,
    };
    match rest.strip_prefix("//") {
        Some(r) => r.split(['/', '?', '#']).next().unwrap_or("").to_string(),
        None => String::new(),
    }
}

fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

fn b64decode(s: &str) -> Option<Vec<u8>> {
    let mut vals = Vec::new();
    for c in s.bytes() {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' => break,
            _ => continue,
        };
        vals.push(v as u32);
    }
    if vals.len() % 4 == 1 {
        return None;
    }
    let mut out = Vec::with_capacity(vals.len() * 3 / 4);
    for ch in vals.chunks(4) {
        let mut n = 0u32;
        for (i, v) in ch.iter().enumerate() {
            n |= v << (18 - 6 * i);
        }
        out.push((n >> 16) as u8);
        if ch.len() > 2 {
            out.push((n >> 8) as u8);
        }
        if ch.len() > 3 {
            out.push(n as u8);
        }
    }
    Some(out)
}

fn is_private(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v) => {
            let o = v.octets();
            let n = u32::from(v);
            let inn = |base: [u8; 4], bits: u32| {
                let b = u32::from_be_bytes(base);
                let mask = if bits == 0 { 0 } else { u32::MAX << (32 - bits) };
                n & mask == b & mask
            };
            o[0] == 0
                || o[0] == 10
                || o[0] == 127
                || inn([169, 254, 0, 0], 16)
                || inn([172, 16, 0, 0], 12)
                || inn([192, 0, 0, 0], 29)
                || inn([192, 0, 0, 170], 31)
                || inn([192, 0, 2, 0], 24)
                || inn([192, 168, 0, 0], 16)
                || inn([198, 18, 0, 0], 15)
                || inn([198, 51, 100, 0], 24)
                || inn([203, 0, 113, 0], 24)
                || o[0] >= 240
        }
        IpAddr::V6(v) => {
            let s = v.segments();
            v.is_loopback()
                || v.is_unspecified()
                || (s[0..5] == [0, 0, 0, 0, 0] && s[5] == 0xffff)
                || (s[0] & 0xfe00) == 0xfc00
                || (s[0] & 0xffc0) == 0xfe80
                || (s[0] == 0x2001 && s[1] == 0x0db8)
                || (s[0] == 0x2001 && s[1] < 0x0200)
                || (s[0] == 0x0100 && s[1..4] == [0, 0, 0])
        }
    }
}

fn mime(p: &str) -> &'static str {
    let ext = p.rsplit('.').next().unwrap_or("").to_ascii_lowercase();
    match ext.as_str() {
        "html" | "htm" => "text/html",
        "css" => "text/css",
        "js" | "mjs" => "text/javascript",
        "json" => "application/json",
        "png" => "image/png",
        "jpg" | "jpeg" => "image/jpeg",
        "ico" => "image/vnd.microsoft.icon",
        "svg" => "image/svg+xml",
        "webp" => "image/webp",
        "gif" => "image/gif",
        _ => "application/octet-stream",
    }
}

fn launcher_re() -> &'static Regex {
    static R: OnceLock<Regex> = OnceLock::new();
    R.get_or_init(|| {
        Regex::new(r"^/(?:launcher/)?(src/(?:(?:app/)?[a-z0-9-]+\.(?:js|css)|i18n/[a-z-]+\.json)|assets/(?:portraits|icons|bg)/[A-Za-z0-9_-]+\.(?:png|jpg))\z").expect("launcher re")
    })
}

fn test_cookie_re() -> &'static Regex {
    static R: OnceLock<Regex> = OnceLock::new();
    R.get_or_init(|| Regex::new(r"(?:^|;\s*)tw_srv=test(?:;|$)").expect("cookie re"))
}

fn truthy_str(o: &Map<String, Value>, k: &str) -> String {
    o.get(k).filter(|v| py::truthy(v)).map(py::to_str).unwrap_or_default()
}

impl App {
    pub fn new(cfg: Config, live: Admin, test: Option<Admin>) -> Self {
        let csp = match Self::site_netloc(&cfg.site_url) {
            Some((scheme, host)) => format!("{}; frame-src 'self' {}://{}", CSP, scheme, host),
            None => CSP.to_string(),
        };
        App {
            cfg,
            live,
            test,
            csp,
            fails: Mutex::new(HashMap::new()),
            users: Mutex::new((None, Arc::new(HashMap::new()))),
            secret: Mutex::new((None, Arc::new(Vec::new()))),
            admins: Mutex::new((None, Arc::new(HashSet::new()))),
        }
    }

    fn site_netloc(site: &str) -> Option<(String, String)> {
        let n = netloc(site);
        if n.is_empty() {
            return None;
        }
        let scheme = site.split_once("://").map(|(s, _)| s.to_string()).filter(|s| !s.is_empty()).unwrap_or_else(|| "https".into());
        Some((scheme, n))
    }

    fn site_host(&self) -> String {
        let n = netloc(&self.cfg.site_url);
        let h = n.rsplit_once('@').map(|(_, h)| h).unwrap_or(&n);
        let h = if h.starts_with('[') { h.split(']').next().map(|x| x.trim_start_matches('[')).unwrap_or("") } else { h.split(':').next().unwrap_or("") };
        h.to_lowercase()
    }

    fn users(&self) -> Arc<HashMap<String, String>> {
        let s = fsig(&self.cfg.users_file);
        let mut g = self.users.lock().unwrap_or_else(|e| e.into_inner());
        if g.0 != s || s.is_none() {
            let mut m = HashMap::new();
            for ln in py::splitlines(&read_text(&self.cfg.users_file)) {
                let ln = ln.trim();
                if ln.is_empty() || ln.starts_with('#') {
                    continue;
                }
                if let Some((name, tok)) = ln.split_once(char::is_whitespace) {
                    m.insert(name.to_string(), tok.trim().to_string());
                }
            }
            *g = (s, Arc::new(m));
        }
        g.1.clone()
    }

    fn web_secret(&self) -> Arc<Vec<u8>> {
        let s = fsig(&self.cfg.web_secret);
        let mut g = self.secret.lock().unwrap_or_else(|e| e.into_inner());
        if g.0 != s || s.is_none() {
            *g = (s, Arc::new(read_text(&self.cfg.web_secret).trim().as_bytes().to_vec()));
        }
        g.1.clone()
    }

    fn web_admins(&self) -> Arc<HashSet<i64>> {
        let s = fsig(&self.cfg.web_admins);
        let mut g = self.admins.lock().unwrap_or_else(|e| e.into_inner());
        if g.0 != s || s.is_none() {
            let ids = py::splitlines(&read_text(&self.cfg.web_admins))
                .into_iter()
                .filter_map(|ln| {
                    let e = ln.split('#').next().unwrap_or("").trim();
                    if py::is_digits(e) {
                        e.parse::<i64>().ok()
                    } else {
                        None
                    }
                })
                .collect();
            *g = (s, Arc::new(ids));
        }
        g.1.clone()
    }

    fn cookie_sid(&self, cookie: Option<&str>) -> Option<i64> {
        let secret = self.web_secret();
        if secret.is_empty() {
            return None;
        }
        let now = timeutil::now();
        for part in cookie.unwrap_or("").split(';') {
            let part = part.trim();
            let (k, v) = part.split_once('=').unwrap_or((part, ""));
            if k != "tw_s" {
                continue;
            }
            let bits: Vec<&str> = v.split('.').collect();
            if bits.len() != 3 {
                return None;
            }
            let (sid, exp, mac) = (bits[0], bits[1], bits[2]);
            let Ok(exp) = exp.trim().replace('_', "").parse::<i64>() else {
                return None;
            };
            let Ok(mut h) = Hmac::<Sha256>::new_from_slice(&secret) else {
                return None;
            };
            h.update(format!("{}.{}", sid, exp).as_bytes());
            let want: String = h.finalize().into_bytes().iter().map(|b| format!("{:02x}", b)).collect();
            if py::is_digits(sid) && (exp as f64) > now && ct_eq(mac.as_bytes(), want.as_bytes()) {
                return sid.parse::<i64>().ok();
            }
        }
        None
    }

    fn host(&self, r: &Req) -> String {
        r.header("host").unwrap_or("").to_lowercase()
    }

    fn via_tunnel(&self, r: &Req) -> bool {
        let h = self.host(r);
        let h = h.split(':').next().unwrap_or("");
        let lo = match r.peer {
            IpAddr::V4(v) => v.octets() == [127, 0, 0, 1],
            IpAddr::V6(v) => v.is_loopback(),
        };
        !self.cfg.tunnel_hosts.is_empty() && self.cfg.tunnel_hosts.iter().any(|t| t == h) && lo
    }

    fn is_loopback(&self, r: &Req) -> bool {
        !self.via_tunnel(r) && r.peer.is_loopback()
    }

    fn host_ok(&self, r: &Req) -> bool {
        if self.via_tunnel(r) {
            return true;
        }
        let h = self.host(r);
        let p = self.cfg.port;
        if self.is_loopback(r) {
            return h == format!("127.0.0.1:{}", p) || h == format!("localhost:{}", p) || h == format!("[::1]:{}", p);
        }
        let Some((name, port)) = h.rsplit_once(':') else {
            return false;
        };
        if name.is_empty() || port != p.to_string() {
            return false;
        }
        if self.cfg.allow_hosts.iter().any(|a| a == name) {
            return true;
        }
        name.trim_matches(|c| c == '[' || c == ']').parse::<IpAddr>().is_ok()
    }

    fn admin_origin(&self, r: &Req) -> String {
        let h = self.host(r);
        let h = h.split(':').next().unwrap_or("");
        if self.via_tunnel(r) {
            return format!("https://{}", h);
        }
        if let Some(t) = self.cfg.tunnel_hosts.first() {
            return format!("https://{}", t);
        }
        let sh = self.site_host();
        format!("https://admin.{}", if sh.is_empty() { "twelve-weeks.com".into() } else { sh })
    }

    fn gate(&self, r: &Req, code: u16, kind: &str, sid: i64) -> Resp {
        let tpl = std::fs::read(self.cfg.static_dir.join("login.html"))
            .map(|b| String::from_utf8_lossy(&b).into_owned())
            .unwrap_or_else(|_| "<!doctype html><title>Admin</title><!--BODY-->".into());
        let site = html_escape(self.cfg.site_url.trim_end_matches('/'));
        let body = if kind == "notadmin" {
            format!(
                "<h1>Not an admin</h1><p class=\"lead\">You are logged in with Steam, but this account is not on the admin list of this server.</p><div class=\"idbox\"><span>Your Steam ID</span><b>{}</b></div><p class=\"hint\">Send this number to the server owner. They add it to <code>data/web-admins.txt</code>, then reload this page.</p><div class=\"gate-actions\"><a class=\"btn\" href=\"{}/auth/logout\">Log out</a></div>",
                sid, site
            )
        } else {
            let nxt = quote_all(&format!("{}/", self.admin_origin(r).trim_end_matches('/')));
            format!(
                "<h1>Admin login</h1><p class=\"lead\">Log in with the Steam account of a server admin.</p><div class=\"gate-actions\"><a class=\"btn main\" href=\"{}/auth/steam?next={}\">{}Log in with Steam</a></div><p class=\"hint\"><a href=\"/login-token\">Use an access token</a></p>",
                site, nxt, STEAM_SVG
            )
        };
        self.bytes(code, tpl.replace("<!--BODY-->", &body).into_bytes(), "text/html; charset=utf-8", vec![])
    }

    fn bytes(&self, code: u16, body: Vec<u8>, ctype: &str, extra: Vec<(&str, String)>) -> Resp {
        let mut h: Vec<(String, String)> = vec![("Content-Type".into(), ctype.into())];
        if !extra.iter().any(|(k, _)| k.eq_ignore_ascii_case("cache-control")) {
            h.push(("Cache-Control".into(), "no-store".into()));
        }
        h.push(("Content-Security-Policy".into(), self.csp.clone()));
        h.push(("X-Content-Type-Options".into(), "nosniff".into()));
        h.push(("Referrer-Policy".into(), "no-referrer".into()));
        for (k, v) in extra {
            h.push((k.to_string(), v));
        }
        Resp { code, headers: h, body: Body::Bytes(body), delay: false }
    }

    fn text(&self, code: u16, msg: &str) -> Resp {
        self.bytes(code, msg.as_bytes().to_vec(), "text/plain", vec![])
    }

    fn json(&self, code: u16, v: &Value) -> Resp {
        self.bytes(code, serde_json::to_vec(v).unwrap_or_default(), "application/json; charset=utf-8", vec![])
    }

    fn result(&self, r: Result<Value, String>) -> Resp {
        match r {
            Ok(v) => self.json(200, &v),
            Err(e) => self.json(500, &json!({"error": e})),
        }
    }

    fn static_file(&self, rel: &str) -> Resp {
        match std::fs::read(self.cfg.static_dir.join(rel)) {
            Ok(b) => self.bytes(200, b, mime(rel), vec![]),
            Err(_) => self.text(404, "not found"),
        }
    }

    fn auth(&self, r: &Req) -> Result<Ident, Resp> {
        if self.is_loopback(r) {
            return Ok(Ident::Local);
        }
        if r.path == "/admin.css" {
            return Ok(Ident::Local);
        }
        let tunnel = self.via_tunnel(r);
        let mut ip = r.peer.to_string();
        if tunnel {
            ip = r.header("cf-connecting-ip").map(|s| s.trim().to_string()).unwrap_or_else(|| "tunnel".into());
        } else if !is_private(r.peer) {
            return Err(self.text(403, "LAN only"));
        }
        let hdr = r.header("authorization").unwrap_or("");
        let basic = hdr.len() >= 6 && hdr[..6].eq_ignore_ascii_case("basic ");
        if !basic {
            if let Some(sid) = self.cookie_sid(r.header("cookie")) {
                if self.web_admins().contains(&sid) {
                    return Ok(Ident::Steam(sid));
                }
                return Err(self.gate(r, 403, "notadmin", sid));
            }
        }
        let now = timeutil::now();
        let (cnt, t0) = self.fails.lock().unwrap_or_else(|e| e.into_inner()).get(&ip).copied().unwrap_or((0, 0.0));
        if cnt >= 10 && now - t0 < 600.0 {
            return Err(self.text(429, "too many failed logins, wait 10 minutes"));
        }
        let mut user = String::new();
        let mut ok = false;
        if basic {
            if let Some(dec) = b64decode(&hdr[6..]).and_then(|b| String::from_utf8(b).ok()) {
                let (u, pw) = dec.split_once(':').unwrap_or((dec.as_str(), ""));
                user = u.to_string();
                if let Some(want) = self.users().get(u) {
                    ok = !want.is_empty() && ct_eq(want.as_bytes(), pw.as_bytes());
                }
            }
        }
        if ok {
            self.fails.lock().unwrap_or_else(|e| e.into_inner()).remove(&ip);
            return Ok(Ident::Token(user));
        }
        let mut delay = false;
        if !hdr.is_empty() {
            let recent = now - t0 < 600.0;
            let mut f = self.fails.lock().unwrap_or_else(|e| e.into_inner());
            f.retain(|_, (_, t)| now - *t < 600.0);
            f.insert(ip, (if recent { cnt + 1 } else { 1 }, if cnt > 0 && recent { t0 } else { now }));
            delay = true;
        }
        let mut resp = if !hdr.is_empty() || r.path == "/login-token" {
            self.bytes(401, b"login required".to_vec(), "text/plain", vec![("WWW-Authenticate", "Basic realm=\"T2R admin\", charset=\"UTF-8\"".into())])
        } else if r.path.starts_with("/api/") {
            self.bytes(401, br#"{"error": "login required"}"#.to_vec(), "application/json; charset=utf-8", vec![])
        } else {
            self.gate(r, 401, "login", 0)
        };
        resp.delay = delay;
        Err(resp)
    }

    fn srv(&self, r: &Req) -> &Admin {
        match &self.test {
            Some(t) if test_cookie_re().is_match(r.header("cookie").unwrap_or("")) => t,
            _ => &self.live,
        }
    }

    fn whoami(&self, ident: &Ident) -> Map<String, Value> {
        let site = self.cfg.site_url.trim_end_matches('/').to_string();
        let mut m = Map::new();
        match ident {
            Ident::Steam(sid) => {
                m.insert("kind".into(), json!("steam"));
                m.insert("id".into(), json!(sid.to_string()));
                m.insert("name".into(), json!(self.live.name_for_steam(*sid).unwrap_or_default()));
                m.insert("logout".into(), json!(format!("{}/auth/logout", site)));
            }
            Ident::Token(u) => {
                m.insert("kind".into(), json!("token"));
                m.insert("id".into(), json!(""));
                m.insert("name".into(), json!(u));
                m.insert("logout".into(), json!(""));
            }
            Ident::Local => {
                m.insert("kind".into(), json!("local"));
                m.insert("id".into(), json!(""));
                m.insert("name".into(), json!(""));
                m.insert("logout".into(), json!(""));
            }
        }
        m.insert("site".into(), json!(site));
        m
    }

    fn who_label(&self, ident: &Ident) -> String {
        let w = self.whoami(ident);
        for k in ["name", "id", "kind"] {
            let v = truthy_str(&w, k);
            if !v.is_empty() {
                return v;
            }
        }
        String::new()
    }

    pub fn handle(&self, r: Req) -> Resp {
        match r.method.as_str() {
            "GET" | "HEAD" => self.get(&r),
            "POST" => self.post(&r),
            _ => self.text(501, "Unsupported method"),
        }
    }

    fn range(&self, r: &Req, path: PathBuf, ctype: &str, size: u64) -> Resp {
        let mut start = 0u64;
        let mut end = size as i128 - 1;
        let mut code = 200;
        let rng = r.header("range").unwrap_or("").trim();
        if let Some(spec) = rng.strip_prefix("bytes=") {
            if let Some((a, b)) = spec.split_once('-') {
                let digits = |s: &str| s.bytes().all(|c| c.is_ascii_digit());
                if digits(a) && digits(b) && (!a.is_empty() || !b.is_empty()) {
                    if !a.is_empty() {
                        start = a.parse().unwrap_or(u64::MAX);
                        if !b.is_empty() {
                            end = b.parse::<i128>().unwrap_or(i128::MAX);
                        }
                    } else {
                        start = size.saturating_sub(b.parse().unwrap_or(u64::MAX));
                    }
                    end = end.min(size as i128 - 1);
                    if (start as i128) > end || start >= size {
                        return self.bytes(416, b"bad range".to_vec(), "text/plain", vec![("Content-Range", format!("bytes */{}", size))]);
                    }
                    code = 206;
                }
            }
        }
        let n = (end - start as i128 + 1).max(0) as u64;
        let mut h: Vec<(String, String)> = vec![("Content-Type".into(), ctype.into()), ("Accept-Ranges".into(), "bytes".into())];
        if code == 206 {
            h.push(("Content-Range".into(), format!("bytes {}-{}/{}", start, end, size)));
        }
        h.push(("Cache-Control".into(), "private, max-age=3600".into()));
        h.push(("Content-Security-Policy".into(), self.csp.clone()));
        h.push(("X-Content-Type-Options".into(), "nosniff".into()));
        Resp { code, headers: h, body: Body::File { path, start, len: n }, delay: false }
    }

    fn get(&self, r: &Req) -> Resp {
        if !self.host_ok(r) {
            return self.text(403, "bad host");
        }
        let p = r.path.as_str();
        if p == "/icon.png" || p == "/favicon.ico" {
            return self.static_file(&p[1..]);
        }
        let ident = match self.auth(r) {
            Ok(i) => i,
            Err(resp) => return resp,
        };
        let a = self.srv(r);
        let q = parse_qs(&r.query);
        let qs = |k: &str, d: &str| q.get(k).cloned().unwrap_or_else(|| d.to_string());
        match p {
            "/login-token" => return self.bytes(302, Vec::new(), "text/plain", vec![("Location", "/".into())]),
            "/api/whoami" => return self.json(200, &Value::Object(self.whoami(&ident))),
            "/api/server" => {
                let is_test = self.test.as_ref().map(|t| std::ptr::eq(t, a)).unwrap_or(false);
                return self.json(200, &json!({"server": if is_test { "test" } else { "live" }, "test": self.test.is_some()}));
            }
            "/" | "/index.html" => return self.static_file("index.html"),
            _ => {}
        }
        if let Some(f) = STATIC_FILES.iter().find(|f| p.strip_prefix('/') == Some(**f)) {
            return self.static_file(f);
        }
        if let (Some(c), Some(root)) = (launcher_re().captures(p), &self.cfg.launcher_root) {
            if let Some(rel) = c.get(1) {
                let fp = root.join(rel.as_str());
                if fp.is_file() {
                    if let Ok(b) = std::fs::read(&fp) {
                        return self.bytes(200, b, mime(p), vec![("Cache-Control", "max-age=300".into())]);
                    }
                }
            }
        }
        match p {
            "/api/overview" => self.result(a.overview()),
            "/api/online" => self.result(a.players_online()),
            "/api/players" => self.result(a.players_all(&qs("q", ""))),
            "/api/player" => self.result(a.player(&qs("id", ""))),
            "/api/notices" => self.json(200, &a.notices()),
            "/api/matches" => self.result(a.matches()),
            "/api/teams" => self.json(200, &a.teams()),
            "/api/clanview" => self.result(a.clanview(&qs("id", ""), &qs("lang", "en"))),
            "/api/clans" => self.result(a.clans()),
            "/api/reports" => self.json(200, &a.reports_list()),
            "/api/clan-backups" => self.result(a.clan_backups(&qs("id", ""))),
            "/api/report" => self.json(200, &a.report(&qs("code", ""))),
            "/api/report-file" => match a.report_file(&qs("code", ""), &qs("name", "")) {
                Some(v) => self.json(200, &v),
                None => self.json(404, &json!({"error": "no such file"})),
            },
            "/api/report-image" => match a.report_image(&qs("code", ""), &qs("name", "")) {
                Some((ct, data)) => self.bytes(200, data, ct, vec![("Content-Disposition", "inline".into()), ("Cache-Control", "private, max-age=3600".into())]),
                None => self.text(404, "no such image"),
            },
            "/api/report-video" => match a.report_video(&qs("code", ""), &qs("name", "")) {
                Some((path, ct, size)) => self.range(r, path, ct, size),
                None => self.text(404, "no such video"),
            },
            "/api/match" => self.result(a.match_detail(&qs("id", ""))),
            "/api/chat" => {
                let int = |k: &str, d: &str| -> Result<i64, String> {
                    let raw = qs(k, d);
                    let raw = if raw.is_empty() { d.to_string() } else { raw };
                    py::to_int(&Value::String(raw.clone())).ok_or_else(|| format!("ValueError: invalid literal for int() with base 10: {}", py::repr_str(&raw)))
                };
                match (int("before", "0"), int("n", "200")) {
                    (Ok(before), Ok(n)) => self.result(a.chat(&qs("q", ""), &qs("kind", ""), &qs("id", ""), before, n)),
                    (Err(e), _) | (_, Err(e)) => self.json(500, &json!({"error": e})),
                }
            }
            "/api/news" => self.json(200, &self.live.news_get()),
            "/api/logs" => {
                let raw = qs("n", "300");
                let n = match py::to_int(&Value::String(raw.clone())) {
                    Some(n) => n.min(1000),
                    None => return self.json(500, &json!({"error": format!("ValueError: invalid literal for int() with base 10: {}", py::repr_str(&raw))})),
                };
                self.json(200, &a.logs(&qs("q", ""), &qs("level", "all"), &qs("file", "0"), n, qs("quiet", "1") != "0"))
            }
            _ => self.text(404, "not found"),
        }
    }

    fn post(&self, r: &Req) -> Resp {
        if !self.host_ok(r) {
            return self.text(403, "bad host");
        }
        let ident = match self.auth(r) {
            Ok(i) => i,
            Err(resp) => return resp,
        };
        let a = self.srv(r);
        let origin = r.header("origin");
        let host = self.host(r);
        let forbidden = || self.json(403, &json!({"ok": false, "error": "forbidden"}));
        if r.header("x-admin-request") != Some("1") || origin.map(|o| netloc(o).to_lowercase() != host).unwrap_or(false) {
            return forbidden();
        }
        let tunnel = self.via_tunnel(r);
        if tunnel || matches!(ident, Ident::Steam(_)) {
            match origin {
                None => return forbidden(),
                Some(o) if tunnel && o.to_lowercase() != format!("https://{}", host) => return forbidden(),
                _ => {}
            }
        }
        let bad = || self.json(400, &json!({"ok": false, "error": "bad request"}));
        let Some(raw) = &r.body else {
            return bad();
        };
        let body: Map<String, Value> = if raw.is_empty() {
            Map::new()
        } else {
            match serde_json::from_slice::<Value>(raw) {
                Ok(Value::Object(o)) => o,
                _ => return bad(),
            }
        };
        let s = |k: &str| py::get_str(&body, k, "");
        let s_or = |k: &str| truthy_str(&body, k);
        let flag = |k: &str, d: bool| body.get(k).map(py::truthy).unwrap_or(d);
        match r.path.as_str() {
            "/api/news" => self.result(self.live.news_save(body.get("entries"))),
            "/api/ban" => self.result(a.ban(&s("id"), body.get("minutes").unwrap_or(&Value::Null), &s_or("reason"), &self.who_label(&ident))),
            "/api/unban-entry" => self.result(a.unban_entry(&s("entry"), &self.who_label(&ident))),
            "/api/unban" => self.result(a.unban(&s("id"), &self.who_label(&ident))),
            "/api/mute" => self.result(a.mute(&s("id"), body.get("minutes").unwrap_or(&Value::Null), &s_or("reason"), &self.who_label(&ident))),
            "/api/unmute" => self.result(a.unmute(&s("id"), &self.who_label(&ident))),
            "/api/kick" => self.json(200, &a.kick(&s("pid"), &s_or("reason"))),
            "/api/chat-say" => self.json(200, &a.chat_say(&s("mid"), &s("name"), &s("text"), &self.who_label(&ident))),
            "/api/clan-reset" => self.result(a.clan_reset(&s("id"), &s("confirm"), flag("names", false))),
            "/api/clan-restore" => self.result(a.clan_restore(&s("id"), &s("file"))),
            "/api/notice" => {
                let sec = match body.get("seconds") {
                    Some(v) if py::truthy(v) => py::to_float(v),
                    _ => Some(8.0),
                };
                match sec {
                    Some(sec) => self.json(200, &a.notice_send(&s("text"), sec)),
                    None => bad(),
                }
            }
            "/api/notice-clear" => match body.get("id").map(py::to_int).unwrap_or(Some(-1)) {
                Some(id) => self.json(200, &a.notice_clear(id)),
                None => bad(),
            },
            "/api/shutdown" => match body.get("minutes").map(py::to_float).unwrap_or(Some(5.0)) {
                Some(m) => self.json(200, &a.shutdown_schedule(m, &s("reason"), flag("restart", true))),
                None => bad(),
            },
            "/api/shutdown-cancel" => self.json(200, &a.shutdown_cancel()),
            "/api/teams" => self.json(200, &a.teams_set(&s("rules"))),
            "/api/teams-move" => self.json(200, &a.teams_move(&s("pid"))),
            "/api/report-handled" => self.result(a.mark_handled(&s("code"), flag("handled", true))),
            "/api/report-state" => self.result(a.mark_state(&s("code"), &s("state"))),
            _ => self.json(404, &json!({"ok": false, "error": "unknown action"})),
        }
    }
}

pub fn body_limit(path: &str) -> usize {
    if path == "/api/news" {
        2 << 20
    } else {
        8192
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn qs() {
        let q = parse_qs("q=a+b&q=c&x=&id=%41%zz&lang=pl");
        assert_eq!(q.get("q").map(String::as_str), Some("a b"));
        assert_eq!(q.get("x"), None);
        assert_eq!(q.get("id").map(String::as_str), Some("A%zz"));
    }

    #[test]
    fn origins() {
        assert_eq!(netloc("https://admin.twelve-weeks.com"), "admin.twelve-weeks.com");
        assert_eq!(netloc("http://127.0.0.1:7010/x"), "127.0.0.1:7010");
        assert_eq!(netloc("null"), "");
    }

    #[test]
    fn base64() {
        assert_eq!(b64decode("YWRtaW46c2VjcmV0").as_deref(), Some(&b"admin:secret"[..]));
        assert_eq!(b64decode("YQ==").as_deref(), Some(&b"a"[..]));
        assert_eq!(b64decode("Y"), None);
    }

    #[test]
    fn privacy() {
        assert!(is_private("192.168.1.5".parse().unwrap()));
        assert!(!is_private("8.8.8.8".parse().unwrap()));
        assert!(is_private("fe80::1".parse().unwrap()));
    }
}
