use crate::files::is_sha;
use crate::http::{now_secs, Query};
use crate::log;
use rusqlite::{Connection, OpenFlags};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const DB_POOL: usize = 2;
const DB_CACHE_KIB: i64 = 512;
const DB_MAX_AGE: Duration = Duration::from_secs(60);

pub struct Reply {
    pub code: u16,
    pub body: String,
}

pub fn reply(code: u16, v: &Value) -> Reply {
    Reply { code, body: v.to_string() }
}

fn error(code: u16, msg: &str) -> Reply {
    reply(code, &json!({"error": msg}))
}

pub fn valid_name(s: &str) -> bool {
    (1..=40).contains(&s.len()) && s.bytes().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'+' | b'-'))
}

pub fn read_json(path: &Path) -> Option<Value> {
    for i in 0..4 {
        match fs::read(path) {
            Ok(b) => match serde_json::from_slice(&b) {
                Ok(v) => return Some(v),
                Err(_) if i < 3 => std::thread::sleep(Duration::from_millis(20 * (i + 1))),
                Err(_) => return None,
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return None,
            Err(_) if i < 3 => std::thread::sleep(Duration::from_millis(20 * (i + 1))),
            Err(_) => return None,
        }
    }
    None
}

fn mtime_of(m: &fs::Metadata) -> f64 {
    m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map(|d| d.as_secs_f64()).unwrap_or(0.0)
}

pub struct Api {
    pub data: PathBuf,
    pub db: Option<PathBuf>,
    pub status_file: Option<PathBuf>,
    news: Mutex<Option<(SystemTime, u64, String)>>,
    status: Mutex<Option<(f64, String)>>,
    elig: Mutex<(Option<SystemTime>, HashSet<String>)>,
    pub seen: Mutex<HashMap<String, f64>>,
    pool: Mutex<Vec<(Instant, Connection)>>,
    pub window: f64,
}

impl Api {
    pub fn new(data: PathBuf, db: Option<PathBuf>, status_file: Option<PathBuf>, window: f64) -> Api {
        Api {
            data,
            db,
            status_file,
            news: Mutex::new(None),
            status: Mutex::new(None),
            elig: Mutex::new((None, HashSet::new())),
            seen: Mutex::new(HashMap::new()),
            pool: Mutex::new(Vec::new()),
            window,
        }
    }

    pub fn manifest(&self, q: &Query, base: &str) -> Reply {
        let ch = q.first("channel").unwrap_or("stable");
        let m = self.load_manifest(ch, q.first("version"));
        let mut m = match m {
            Some(Value::Object(m)) => m,
            _ => return error(404, "no such channel/version"),
        };
        let blobs = self.data.join("blobs");
        let remote = |f: &Value| f.get("url").and_then(Value::as_str).map_or(false, |u| u.starts_with("https://"));
        let empty = Vec::new();
        let files = m.get("files").and_then(Value::as_array).unwrap_or(&empty);
        let gone: Vec<&str> = files
            .iter()
            .filter(|f| !remote(f))
            .filter(|f| !f.get("sha256").and_then(Value::as_str).map_or(false, |s| is_sha(s) && blobs.join(s).is_file()))
            .map(|f| f.get("path").and_then(Value::as_str).unwrap_or("?"))
            .collect();
        if !gone.is_empty() {
            log!("manifest {}/{}: {} blob(s) missing ({} ...)", ch, m.get("version").map(|v| v.to_string()).unwrap_or_default(), gone.len(), gone[0]);
            return error(503, "this version is not fully published yet, try again in a minute");
        }
        if let Some(Value::Array(files)) = m.get_mut("files") {
            for f in files.iter_mut() {
                if remote(f) {
                    continue;
                }
                let o = match f.as_object_mut() {
                    Some(o) => o,
                    None => continue,
                };
                let sha = o.get("sha256").and_then(Value::as_str).unwrap_or("").to_string();
                o.insert("url".into(), json!(format!("{}/files/{}", base, sha)));
                let ext = match o.get("enc").and_then(Value::as_str) {
                    Some("gzip") => Some("gz"),
                    Some("br") => Some("br"),
                    _ => None,
                };
                let has_z = o.get("zsize").map_or(false, |z| !matches!(z, Value::Null | Value::Bool(false)) && z.as_f64() != Some(0.0) && z.as_str() != Some(""));
                match ext {
                    Some(e) if has_z && blobs.join(format!("{}.{}", sha, e)).is_file() => {
                        o.insert("zurl".into(), json!(format!("{}/files/{}.{}", base, sha, e)));
                    }
                    _ => {
                        o.shift_remove("zsize");
                        o.shift_remove("enc");
                    }
                }
            }
        }
        m.insert("launcher".into(), self.launcher_entry("launcher.json", base));
        m.insert("launcher_linux".into(), self.launcher_entry("launcher-linux.json", base));
        reply(200, &Value::Object(m))
    }

    fn load_manifest(&self, ch: &str, version: Option<&str>) -> Option<Value> {
        if !valid_name(ch) {
            return None;
        }
        let v = match version {
            Some(v) => v.to_string(),
            None => match read_json(&self.data.join("channels.json"))?.get(ch)? {
                Value::String(s) => s.clone(),
                _ => return None,
            },
        };
        if !valid_name(&v) {
            return None;
        }
        read_json(&self.data.join("manifests").join(ch).join(format!("{}.json", v)))
    }

    fn launcher_entry(&self, name: &str, base: &str) -> Value {
        let la = match read_json(&self.data.join(name)) {
            Some(Value::Object(o)) if !o.is_empty() => o,
            _ => return Value::Null,
        };
        let sha = la.get("sha256").cloned().unwrap_or(Value::Null);
        let url = match la.get("url").and_then(Value::as_str) {
            Some(u) if u.starts_with("https://") => u.to_string(),
            _ => format!("{}/files/{}", base, sha.as_str().unwrap_or("")),
        };
        json!({"version": la.get("version").cloned().unwrap_or(Value::Null), "sha256": sha, "size": la.get("size").cloned().unwrap_or(Value::Null), "url": url})
    }

    pub fn news(&self) -> Reply {
        let p = self.data.join("news.json");
        let meta = fs::metadata(&p).ok();
        let key = meta.as_ref().and_then(|m| Some((m.modified().ok()?, m.len())));
        let mut g = self.news.lock().unwrap_or_else(|e| e.into_inner());
        if let (Some((t, n)), Some((ct, cn, body))) = (key, g.as_ref()) {
            if t == *ct && n == *cn {
                return Reply { code: 200, body: body.clone() };
            }
        }
        let body = read_json(&p).unwrap_or_else(|| json!([])).to_string();
        if let Some((t, n)) = key {
            *g = Some((t, n, body.clone()));
        }
        Reply { code: 200, body }
    }

    pub fn status(&self, ip: Option<&str>) -> Reply {
        let now = now_secs();
        if let Some(ip) = ip {
            self.seen.lock().unwrap_or_else(|e| e.into_inner()).insert(ip.to_string(), now);
        }
        let mut g = self.status.lock().unwrap_or_else(|e| e.into_inner());
        if let Some((t, body)) = g.as_ref() {
            if now - t < 3.0 {
                return Reply { code: 200, body: body.clone() };
            }
        }
        let off = json!({"online": false, "server_version": "", "matches": null});
        let v = match &self.status_file {
            Some(p) => match read_json(p) {
                Some(Value::Object(d)) => {
                    let ts = match d.get("ts") {
                        None => Some(0.0),
                        Some(Value::Number(n)) => n.as_f64(),
                        Some(Value::String(s)) => s.trim().parse::<f64>().ok(),
                        Some(Value::Bool(b)) => Some(*b as i64 as f64),
                        _ => None,
                    };
                    match ts {
                        Some(ts) => {
                            let get = |k: &str, dflt: Value| d.get(k).cloned().unwrap_or(dflt);
                            let or_empty = |k: &str| match d.get(k) {
                                Some(v) if crate::reports::truthy(v) => v.clone(),
                                _ => json!([]),
                            };
                            json!({"online": now - ts < 30.0, "server_version": get("server_version", json!("")), "matches": get("matches", Value::Null),
                                   "players_online": get("players_online", Value::Null), "queue": get("queue", Value::Null), "notices": or_empty("notices"),
                                   "target_version": get("target_version", json!("")), "roster": or_empty("roster")})
                        }
                        None => off,
                    }
                }
                _ => off,
            },
            None => off,
        };
        let body = v.to_string();
        *g = Some((now, body.clone()));
        Reply { code: 200, body }
    }

    pub fn launchers_open(&self) -> usize {
        let now = now_secs();
        let mut g = self.seen.lock().unwrap_or_else(|e| e.into_inner());
        let w = self.window;
        g.retain(|_, t| now - *t <= w);
        g.len()
    }

    pub fn versions(&self, q: &Query) -> Reply {
        let ch = q.first("channel").unwrap_or("stable");
        let current = read_json(&self.data.join("channels.json")).and_then(|c| c.get(ch).cloned()).unwrap_or(Value::Null);
        let mut vs: Vec<(f64, String)> = Vec::new();
        if valid_name(ch) {
            if let Ok(rd) = fs::read_dir(self.data.join("manifests").join(ch)) {
                for e in rd.flatten() {
                    let n = e.file_name().into_string().unwrap_or_default();
                    if let Some(v) = n.strip_suffix(".json") {
                        vs.push((e.metadata().map(|m| mtime_of(&m)).unwrap_or(0.0), v.to_string()));
                    }
                }
            }
        }
        vs.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
        reply(200, &json!({"channel": ch, "current": current, "versions": vs.into_iter().map(|x| x.1).collect::<Vec<_>>()}))
    }

    pub fn part1_sid(&self, raw: &str) -> Option<String> {
        let p = self.data.join("part1-eligible.txt");
        let mt = fs::metadata(&p).and_then(|m| m.modified()).ok()?;
        let mut g = self.elig.lock().unwrap_or_else(|e| e.into_inner());
        if g.0 != Some(mt) {
            let mut ids = HashSet::new();
            if let Ok(t) = fs::read_to_string(&p) {
                for line in t.lines() {
                    let l = line.split('#').next().unwrap_or("").trim();
                    if !l.is_empty() && l.chars().all(|c| c.is_ascii_digit()) {
                        ids.insert(l.to_string());
                    }
                }
            }
            *g = (Some(mt), ids);
        }
        raw.split(',').take(8).map(str::trim).find(|x| !x.is_empty() && x.chars().all(|c| c.is_ascii_digit()) && g.1.contains(*x)).map(str::to_string)
    }

    /// GET /part1-importer.json (live part1_importer.py public()): importer.json with an absolute url per file, public like the manifest.
    /// Complete and sane only: a 32-hex build id, the recipe and at least one converter (win32 / linux), every file a 64-hex sha256 with a
    /// positive size and either an https url (a GitHub release asset, passed through) or a blob of exactly that size here.
    pub fn part1_importer(&self, base: &str) -> Reply {
        let none = || error(404, "no Part I importer published");
        let root = self.data.join("part1-importer");
        let m = match read_json(&root.join("importer.json")) {
            Some(Value::Object(m)) => m,
            _ => return none(),
        };
        let bid = m.get("build_id").and_then(Value::as_str).unwrap_or("");
        if bid.len() != 32 || !bid.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c)) {
            return none();
        }
        let file = |f: Option<&Value>| -> Option<Value> {
            let o = f?.as_object()?;
            let sha = o.get("sha256")?.as_str()?;
            let size = o.get("size")?.as_i64()?;
            if !is_sha(sha) || size <= 0 || o.get("size")?.is_f64() {
                return None;
            }
            let remote = o.get("url").and_then(Value::as_str).filter(|u| u.starts_with("https://"));
            let url = match remote {
                Some(u) => u.to_string(),
                None => {
                    if fs::metadata(root.join("blobs").join(sha)).ok().map(|x| x.len() as i64) != Some(size) {
                        return None;
                    }
                    format!("{}/part1-importer-files/{}", base.trim_end_matches('/'), sha)
                }
            };
            Some(json!({"sha256": sha, "size": size, "url": url}))
        };
        let recipe = match file(m.get("recipe")) {
            Some(r) => r,
            None => return none(),
        };
        let conv = match m.get("converter").and_then(Value::as_object) {
            Some(c) if c.keys().any(|k| k == "win32" || k == "linux") => c,
            _ => return none(),
        };
        let mut out = serde_json::Map::new();
        for (k, v) in conv {
            if k != "win32" && k != "linux" {
                return none();
            }
            match file(Some(v)) {
                Some(f) => {
                    out.insert(k.clone(), f);
                }
                None => return none(),
            }
        }
        reply(200, &json!({"build_id": bid, "published": m.get("published").cloned().unwrap_or(json!("")), "recipe": recipe, "converter": Value::Object(out)}))
    }

    pub fn part1_maps(&self, base: &str, sid: &str) -> Reply {
        let none = || error(404, "no Part I map pack published");
        let root = self.data.join("part1");
        let mut m = match read_json(&root.join("manifest.json")) {
            Some(Value::Object(m)) => m,
            _ => return none(),
        };
        let files = match m.get_mut("files") {
            Some(Value::Array(a)) if !a.is_empty() => a,
            _ => return none(),
        };
        let mut total: i64 = 0;
        for f in files.iter_mut() {
            let o = match f.as_object_mut() {
                Some(o) => o,
                None => return none(),
            };
            let path = o.get("path").and_then(Value::as_str).unwrap_or("");
            let sha = o.get("sha256").and_then(Value::as_str).unwrap_or("").to_string();
            let size = match o.get("size") {
                Some(Value::Number(n)) if n.is_i64() && n.as_i64().unwrap_or(-1) >= 0 => n.as_i64().unwrap_or(0),
                _ => return none(),
            };
            if !part1_name(path) || !is_sha(&sha) {
                return none();
            }
            if fs::metadata(root.join("blobs").join(&sha)).ok().map(|x| x.len() as i64) != Some(size) {
                return none();
            }
            total += size;
            o.insert("url".into(), json!(format!("{}/part1-files/{}?sid={}", base.trim_end_matches('/'), sha, sid)));
        }
        m.insert("total".into(), json!(total));
        reply(200, &Value::Object(m))
    }

    fn ro_db(&self) -> Option<Connection> {
        let c = Connection::open_with_flags(self.db.as_ref()?, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX).ok()?;
        let _ = c.busy_timeout(Duration::from_secs(2));
        let _ = c.pragma_update(None, "query_only", true);
        let _ = c.pragma_update(None, "cache_size", -DB_CACHE_KIB);
        Some(c)
    }

    fn with_db(&self, f: impl FnOnce(&Connection) -> Value) -> Reply {
        if self.db.is_none() {
            return reply(503, &json!({"ok": false, "error": "no database configured (--db)"}));
        }
        let pooled = {
            let mut g = self.pool.lock().unwrap_or_else(|e| e.into_inner());
            g.retain(|(t, _)| t.elapsed() < DB_MAX_AGE);
            g.pop()
        };
        let (born, c) = match pooled.or_else(|| self.ro_db().map(|c| (Instant::now(), c))) {
            Some(x) => x,
            None => return reply(200, &json!({"ok": false, "error": "database unavailable"})),
        };
        let v = f(&c);
        let mut g = self.pool.lock().unwrap_or_else(|e| e.into_inner());
        if g.len() < DB_POOL && born.elapsed() < DB_MAX_AGE {
            g.push((born, c));
        }
        reply(200, &v)
    }

    pub fn clan(&self, q: &Query) -> Reply {
        self.with_db(|c| t2r_clan::clan_view(c, q.first("player").unwrap_or(""), None))
    }

    pub fn clanfeed(&self, q: &Query) -> Reply {
        self.with_db(|c| {
            t2r_clan::clanfeed(
                c,
                q.first("player").unwrap_or(""),
                q.first("limit").unwrap_or("30"),
                q.first("since").unwrap_or("0"),
                q.first("lang").unwrap_or("en"),
                now_secs(),
                None,
            )
        })
    }

    pub fn ban_check(&self, q: &Query, path: &str) -> Reply {
        let v = crate::ban::route(self.db.as_deref(), q);
        if v["banned"] == json!(true) {
            log!("ban-check: BANNED {}", path.chars().take(200).collect::<String>());
        }
        reply(200, &v)
    }
}

fn part1_name(p: &str) -> bool {
    let segs: Vec<&str> = p.split('/').collect();
    segs.len() <= 6
        && segs.iter().all(|s| {
            let b = s.as_bytes();
            !b.is_empty() && b.len() <= 80 && b[0].is_ascii_alphanumeric() && b.iter().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'.' | b'_' | b'+' | b'-'))
        })
}

pub fn media_type(name: &str) -> Option<&'static str> {
    let (stem, ext) = name.rsplit_once('.')?;
    let sb = stem.as_bytes();
    if sb.is_empty() || sb.len() > 81 || !(3..=4).contains(&ext.len()) {
        return None;
    }
    if !(sb[0].is_ascii_alphanumeric() || sb[0] == b'_' || sb[0] == b'-') || !sb.iter().all(|c| c.is_ascii_alphanumeric() || matches!(c, b'_' | b'.' | b'-')) {
        return None;
    }
    if !ext.bytes().all(|c| c.is_ascii_alphanumeric()) {
        return None;
    }
    match ext.to_ascii_lowercase().as_str() {
        "gif" => Some("image/gif"),
        "webp" => Some("image/webp"),
        "png" => Some("image/png"),
        "jpg" | "jpeg" => Some("image/jpeg"),
        "mp4" => Some("video/mp4"),
        "webm" => Some("video/webm"),
        _ => None,
    }
}

pub fn write_launchers(api: &Api, path: &Path) -> std::io::Result<()> {
    let v = json!({"ts": now_secs(), "open": api.launchers_open(), "window_s": api.window});
    let tmp = path.with_extension("json.tmp");
    fs::write(&tmp, v.to_string())?;
    fs::rename(&tmp, path)
}

pub fn downloads_sha(data: &Path, name: &str) -> Option<String> {
    match read_json(&data.join("downloads.json"))?.get(name)? {
        Value::String(s) if is_sha(s) => Some(s.clone()),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn media_names() {
        assert_eq!(media_type("n186-choose.jpg"), Some("image/jpeg"));
        assert_eq!(media_type("a.b.webm"), Some("video/webm"));
        assert_eq!(media_type(".x.png"), None);
        assert_eq!(media_type("x.exe"), None);
        assert_eq!(media_type("../x.png"), None);
        assert_eq!(media_type("x.PNG"), Some("image/png"));
    }

    #[test]
    fn part1_paths() {
        assert!(part1_name("pak68/x.pak"));
        assert!(!part1_name("../x"));
        assert!(!part1_name("/abs"));
        assert!(!part1_name("a/b/c/d/e/f/g"));
    }

    #[test]
    fn manifest_rewrites_urls() {
        let d = std::env::temp_dir().join(format!("t2r-api-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(d.join("manifests/stable")).unwrap();
        fs::create_dir_all(d.join("blobs")).unwrap();
        let a = "a".repeat(64);
        let b = "b".repeat(64);
        fs::write(d.join("blobs").join(&a), "x").unwrap();
        fs::write(d.join("blobs").join(format!("{}.gz", a)), "z").unwrap();
        fs::write(d.join("channels.json"), r#"{"stable":"7"}"#).unwrap();
        let m = json!({"version":"7","channel":"stable","files":[
            {"path":"a.bin","size":1,"sha256":a,"enc":"gzip","zsize":1},
            {"path":"big.slim","size":9,"sha256":b,"url":"https://github.com/x/y/releases/download/build-7/b"}]});
        fs::write(d.join("manifests/stable/7.json"), m.to_string()).unwrap();
        fs::write(d.join("launcher.json"), json!({"version":"0.1.51","sha256":b,"size":5,"url":"https://github.com/l.exe"}).to_string()).unwrap();
        let api = Api::new(d.clone(), None, None, 75.0);
        let r = api.manifest(&Query::parse(""), "http://h:7002");
        assert_eq!(r.code, 200);
        let v: Value = serde_json::from_str(&r.body).unwrap();
        assert_eq!(v["files"][0]["url"], format!("http://h:7002/files/{}", a));
        assert_eq!(v["files"][0]["zurl"], format!("http://h:7002/files/{}.gz", a));
        assert_eq!(v["files"][1]["url"], "https://github.com/x/y/releases/download/build-7/b");
        assert_eq!(v["launcher"]["url"], "https://github.com/l.exe");
        assert_eq!(v["launcher_linux"], Value::Null);
        fs::remove_file(d.join("blobs").join(&a)).unwrap();
        assert_eq!(api.manifest(&Query::parse(""), "http://h").code, 503);
        assert_eq!(api.manifest(&Query::parse("channel=nope"), "http://h").code, 404);
        assert_eq!(api.manifest(&Query::parse("channel=../x"), "http://h").code, 404);
        let _ = fs::remove_dir_all(&d);
    }
}
