//! The launcher polls answered here instead of in Python (with ~100 players the main box sat at 100 % CPU; every open launcher polls /status.json,
//! /manifest.json and /news.json, ~18 requests / s at 100 players, and each one was a new loopback connection + a new Python thread +
//! a reply rebuilt from the files). Same bytes as tools/update_server/server.py (json.dumps(obj, separators=(",", ":")), gzip when the
//! client accepts it and the body is >= 1 KB, the same headers and error bodies). Replies are kept until a file they come from changes.
//!
//! /status.json also counts the open launchers (distinct caller addresses in the last 75 s, written to <status dir>/t2r-launchers.json
//! every 15 s for the Discord bot), as server.py's LauncherSeen did.

use crate::json::{self, Json};
use std::collections::HashMap;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Instant, SystemTime, UNIX_EPOCH};

pub const LAUNCHER_WINDOW_S: f64 = 75.0;
const STATUS_TTL_S: f64 = 3.0;

pub struct Reply {
    pub code: u16,
    pub plain: Vec<u8>,
    pub gz: Option<Vec<u8>>, // only for bodies >= 1 KB (server.py: len(body) >= 1024)
}

pub struct Poll {
    data: PathBuf,
    status_file: Option<PathBuf>,
    cache: Mutex<HashMap<String, (Instant, Vec<u64>, std::sync::Arc<Reply>)>>,
    pub launchers: Mutex<HashMap<String, f64>>,
}

fn now_f() -> f64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

fn mtime(p: &Path) -> u64 {
    std::fs::metadata(p)
        .and_then(|m| m.modified())
        .ok()
        .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
        .map(|d| d.as_nanos() as u64)
        .unwrap_or(0)
}

fn name_ok(s: &str) -> bool {
    // server.py NAME_RE = ^[A-Za-z0-9._+-]{1,40}$
    !s.is_empty() && s.len() <= 40 && s.bytes().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'+' | b'-'))
}

fn gzip6(b: &[u8]) -> Vec<u8> {
    // like Python gzip.compress(body, 6, mtime=0): header mtime 0
    let mut e = flate2::GzBuilder::new().mtime(0).write(Vec::with_capacity(b.len() / 3), flate2::Compression::new(6));
    let _ = e.write_all(b);
    e.finish().unwrap_or_default()
}

pub fn reply(code: u16, v: &Json) -> Reply {
    let plain = json::dumps(v).into_bytes();
    let gz = if plain.len() >= 1024 { Some(gzip6(&plain)) } else { None };
    Reply { code, plain, gz }
}

fn read_json(p: &Path) -> Option<Json> {
    // server.py Store.read_json: a missing / unreadable / bad file = the default
    let t = std::fs::read_to_string(p).ok()?;
    json::parse(&t)
}

impl Poll {
    pub fn new(data: PathBuf, status_file: Option<PathBuf>) -> Poll {
        Poll { data, status_file, cache: Mutex::new(HashMap::new()), launchers: Mutex::new(HashMap::new()) }
    }

    /// The cached reply for key when its file signature is unchanged (and, with ttl, it is younger than ttl s); else build() and keep it.
    fn cached(&self, key: String, sig: Vec<u64>, ttl: Option<f64>, build: impl FnOnce() -> Reply) -> std::sync::Arc<Reply> {
        {
            let g = self.cache.lock().unwrap_or_else(|e| e.into_inner());
            if let Some((t, s, r)) = g.get(&key) {
                if *s == sig && ttl.map(|x| t.elapsed().as_secs_f64() < x).unwrap_or(true) {
                    return r.clone();
                }
            }
        }
        let r = std::sync::Arc::new(build());
        if r.code == 200 {
            let mut g = self.cache.lock().unwrap_or_else(|e| e.into_inner());
            if g.len() >= 256 {
                g.clear();
            }
            g.insert(key, (Instant::now(), sig, r.clone()));
        }
        r
    }

    // ---------------------------------------------------------------- /status.json
    pub fn hit_launcher(&self, addr: &str) {
        if addr.is_empty() {
            return;
        }
        let mut g = self.launchers.lock().unwrap_or_else(|e| e.into_inner());
        g.insert(addr.to_string(), now_f());
        if g.len() > 20000 {
            let now = now_f();
            g.retain(|_, t| now - *t <= LAUNCHER_WINDOW_S);
        }
    }

    pub fn launcher_count(&self) -> usize {
        let now = now_f();
        let mut g = self.launchers.lock().unwrap_or_else(|e| e.into_inner());
        g.retain(|_, t| now - *t <= LAUNCHER_WINDOW_S);
        g.len()
    }

    /// server.py LauncherSeen.write: {"ts", "open", "window_s"} next to the status file (tmp + rename).
    pub fn write_launchers(&self) -> std::io::Result<()> {
        let sf = match &self.status_file {
            Some(s) => s,
            None => return Ok(()),
        };
        let path = sf.parent().unwrap_or(Path::new(".")).join("t2r-launchers.json");
        let tmp = path.with_extension("json.tmp");
        let body = format!("{{\"ts\": {}, \"open\": {}, \"window_s\": {}}}", py_float(now_f()), self.launcher_count(), py_float(LAUNCHER_WINDOW_S));
        std::fs::write(&tmp, body)?;
        std::fs::rename(&tmp, &path)
    }

    pub fn has_status(&self) -> bool {
        self.status_file.is_some()
    }

    pub fn status(&self) -> std::sync::Arc<Reply> {
        let sf = self.status_file.clone().unwrap_or_default();
        let sig = vec![mtime(&sf)];
        self.cached("status".into(), sig, Some(STATUS_TTL_S), || {
            let v = match read_json(&sf) {
                Some(d) if matches!(d, Json::Obj(_)) => {
                    let ts = match d.get("ts") {
                        Some(Json::Num(n)) => n.parse::<f64>().unwrap_or(0.0),
                        _ => 0.0,
                    };
                    let g = |k: &str| d.get(k).cloned().unwrap_or(Json::Null);
                    let or = |k: &str, dflt: Json| match d.get(k) {
                        Some(x) if x.truthy() => x.clone(),
                        _ => dflt,
                    };
                    Json::Obj(vec![
                        ("online".into(), Json::Bool(now_f() - ts < 30.0)),
                        ("server_version".into(), d.get("server_version").cloned().unwrap_or(Json::str(""))),
                        ("matches".into(), g("matches")),
                        ("players_online".into(), g("players_online")),
                        ("queue".into(), g("queue")),
                        ("notices".into(), or("notices", Json::Arr(vec![]))),
                        ("target_version".into(), d.get("target_version").cloned().unwrap_or(Json::str(""))),
                        ("roster".into(), or("roster", Json::Arr(vec![]))),
                    ])
                }
                _ => Json::Obj(vec![("online".into(), Json::Bool(false)), ("server_version".into(), Json::str("")), ("matches".into(), Json::Null)]),
            };
            reply(200, &v)
        })
    }

    // ---------------------------------------------------------------- /news.json
    pub fn news(&self) -> std::sync::Arc<Reply> {
        let p = self.data.join("news.json");
        self.cached("news".into(), vec![mtime(&p)], None, || reply(200, &read_json(&p).unwrap_or(Json::Arr(vec![]))))
    }

    // ---------------------------------------------------------------- /manifest.json
    /// base = "http(s)://host" (server.py _base). Ok(reply) for 200 / 404 / 503.
    pub fn manifest(&self, channel: &str, version: Option<&str>, base: &str) -> std::sync::Arc<Reply> {
        let d = &self.data;
        if !name_ok(channel) {
            return std::sync::Arc::new(reply(404, &err("no such channel/version")));
        }
        let sig = vec![
            mtime(&d.join("channels.json")),
            mtime(&d.join("manifests").join(channel)),
            mtime(&d.join("blobs")),
            mtime(&d.join("launcher.json")),
            mtime(&d.join("launcher-linux.json")),
        ];
        let key = format!("manifest\u{0}{}\u{0}{}\u{0}{}", channel, version.unwrap_or("\u{1}"), base);
        self.cached(key, sig, None, || self.build_manifest(channel, version, base))
    }

    fn build_manifest(&self, channel: &str, version: Option<&str>, base: &str) -> Reply {
        let d = &self.data;
        let ver: String = match version {
            Some(v) if !v.is_empty() => v.to_string(),
            _ => match read_json(&d.join("channels.json")).and_then(|c| c.get(channel).and_then(|x| x.as_str().map(String::from))) {
                Some(v) => v,
                None => return reply(404, &err("no such channel/version")),
            },
        };
        if !name_ok(&ver) {
            return reply(404, &err("no such channel/version"));
        }
        let mut m = match read_json(&d.join("manifests").join(channel).join(format!("{}.json", ver))) {
            Some(m) if m.truthy() => m,
            _ => return reply(404, &err("no such channel/version")),
        };
        let remote = |f: &Json| f.get("url").and_then(|u| u.as_str()).map(|u| u.starts_with("https://")).unwrap_or(false);
        let blobs = d.join("blobs");
        let files_empty = Json::Arr(vec![]);
        let files = m.get("files").unwrap_or(&files_empty);
        let gone: Vec<String> = match files {
            Json::Arr(a) => a
                .iter()
                .filter(|f| !remote(f) && !blobs.join(f.get("sha256").and_then(|x| x.as_str()).unwrap_or("")).is_file())
                .map(|f| f.get("path").and_then(|x| x.as_str()).unwrap_or("").to_string())
                .collect(),
            _ => vec![],
        };
        if !gone.is_empty() {
            eprintln!("manifest {}/{}: {} blob(s) missing ({} ...)", channel, json::dumps(m.get("version").unwrap_or(&Json::Null)), gone.len(), gone[0]);
            return reply(503, &err("this version is not fully published yet, try again in a minute"));
        }
        if let Json::Obj(o) = &mut m {
            if let Some((_, Json::Arr(files))) = o.iter_mut().find(|(k, _)| k == "files") {
                for f in files.iter_mut() {
                    if remote(f) {
                        continue;
                    }
                    let sha = f.get("sha256").and_then(|x| x.as_str()).unwrap_or("").to_string();
                    f.set("url", Json::Str(format!("{}/files/{}", base, sha)));
                    let ext = match f.get("enc").and_then(|x| x.as_str()) {
                        Some("gzip") => Some("gz"),
                        Some("br") => Some("br"),
                        _ => None,
                    };
                    let has_z = f.get("zsize").map(|z| z.truthy()).unwrap_or(false);
                    match ext {
                        Some(e) if has_z && blobs.join(format!("{}.{}", sha, e)).is_file() => {
                            f.set("zurl", Json::Str(format!("{}/files/{}.{}", base, sha, e)));
                        }
                        _ => {
                            f.remove("zsize");
                            f.remove("enc");
                        }
                    }
                }
            }
        }
        let entry = |la: Option<Json>| -> Json {
            let la = match la {
                Some(x) if x.truthy() => x,
                _ => return Json::Null,
            };
            let url = match la.get("url").and_then(|u| u.as_str()) {
                Some(u) if u.starts_with("https://") => u.to_string(),
                _ => format!("{}/files/{}", base, la.get("sha256").and_then(|x| x.as_str()).unwrap_or("")),
            };
            Json::Obj(vec![
                ("version".into(), la.get("version").cloned().unwrap_or(Json::Null)),
                ("sha256".into(), la.get("sha256").cloned().unwrap_or(Json::Null)),
                ("size".into(), la.get("size").cloned().unwrap_or(Json::Null)),
                ("url".into(), Json::Str(url)),
            ])
        };
        let l = entry(read_json(&d.join("launcher.json")));
        m.set("launcher", l);
        let ll = entry(read_json(&d.join("launcher-linux.json")));
        m.set("launcher_linux", ll);
        reply(200, &m)
    }
}

fn err(msg: &str) -> Json {
    Json::Obj(vec![("error".into(), Json::str(msg))])
}

/// Python repr of a float (enough for the launchers file: a timestamp and 75.0).
fn py_float(x: f64) -> String {
    let s = format!("{}", x);
    if s.contains('.') || s.contains('e') {
        s
    } else {
        format!("{}.0", s)
    }
}
