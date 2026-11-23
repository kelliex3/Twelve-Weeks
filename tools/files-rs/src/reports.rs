use crate::http::{gmtime, now_secs};
use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet, VecDeque};
use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::Mutex;

pub const MAX_BODY: u64 = 80 * 1024 * 1024;
const MAX_ENTRIES: usize = 60;
const MAX_ENTRY: u64 = 256 * 1024 * 1024;
const MAX_UNPACKED: u64 = 600 * 1024 * 1024;
pub const RATE_N: usize = 30;
pub const RATE_WINDOW: f64 = 3600.0;
const CODE_ALPHABET: &[u8] = b"23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
const TEXT_FIELDS: [(&str, usize); 12] = [
    ("text", 4000),
    ("when", 200),
    ("whenText", 400),
    ("mapMode", 100),
    ("version", 60),
    ("name", 40),
    ("nameSource", 60),
    ("steamId", 30),
    ("launcherVersion", 30),
    ("os", 200),
    ("gpu", 300),
    ("game", 200),
];
const SHOT_MAX_FILES: usize = 5;
const SHOT_MAX_EACH: u64 = 8 * 1024 * 1024;
const CLIP_MAX_FILES: usize = 2;
pub const CLIP_MAX_EACH: u64 = 200 * 1024 * 1024;
const CLIP_MAX_TOTAL: u64 = 250 * 1024 * 1024;
pub const CLIP_RATE_N: usize = 3;

pub struct ReportError {
    pub status: u16,
    pub msg: String,
}

fn err(status: u16, msg: impl Into<String>) -> ReportError {
    ReportError { status, msg: msg.into() }
}

pub struct Limits {
    pub disk_max: u64,
    pub min_free: u64,
}

pub struct RateLimiter {
    n: usize,
    window: f64,
    hits: Mutex<HashMap<String, VecDeque<f64>>>,
}

impl RateLimiter {
    pub fn new(n: usize, window: f64) -> Self {
        RateLimiter { n, window, hits: Mutex::new(HashMap::new()) }
    }

    pub fn allow(&self, ip: &str) -> bool {
        let now = now_secs();
        let mut g = self.hits.lock().unwrap_or_else(|e| e.into_inner());
        if g.len() > 2048 {
            let w = self.window;
            g.retain(|_, q| q.back().map_or(false, |t| now - t <= w));
        }
        let q = g.entry(ip.to_string()).or_default();
        while q.front().map_or(false, |t| now - t > self.window) {
            q.pop_front();
        }
        if q.len() >= self.n {
            return false;
        }
        q.push_back(now);
        true
    }
}

fn sniff_image(h: &[u8]) -> Option<&'static str> {
    if h.starts_with(b"\x89PNG\r\n\x1a\n") {
        Some("png")
    } else if h.starts_with(&[255, 216, 255]) {
        Some("jpg")
    } else if h.len() >= 12 && &h[..4] == b"RIFF" && &h[8..12] == b"WEBP" {
        Some("webp")
    } else if h.starts_with(b"GIF87a") || h.starts_with(b"GIF89a") {
        Some("gif")
    } else if h.starts_with(b"BM") && h.len() >= 14 {
        Some("bmp")
    } else {
        None
    }
}

fn sniff_video(h: &[u8]) -> Option<&'static str> {
    if h.len() >= 12 && &h[4..8] == b"ftyp" {
        return Some(if &h[8..12] == b"qt  " { "mov" } else { "mp4" });
    }
    if h.starts_with(&[0x1A, 0x45, 0xDF, 0xA3]) {
        let w = &h[..h.len().min(64)];
        return Some(if w.windows(4).any(|x| x == b"webm") { "webm" } else { "mkv" });
    }
    None
}

fn shot_type(k: &str) -> &'static str {
    match k {
        "png" => "image/png",
        "jpg" => "image/jpeg",
        "webp" => "image/webp",
        "gif" => "image/gif",
        _ => "image/bmp",
    }
}

fn clip_type(k: &str) -> &'static str {
    match k {
        "mp4" => "video/mp4",
        "mov" => "video/quicktime",
        "webm" => "video/webm",
        _ => "video/x-matroska",
    }
}

fn folder_size(root: &Path) -> u64 {
    let mut n = 0;
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        if let Ok(rd) = fs::read_dir(&d) {
            for e in rd.flatten() {
                match e.file_type() {
                    Ok(t) if t.is_dir() => stack.push(e.path()),
                    Ok(t) if t.is_file() => n += e.metadata().map(|m| m.len()).unwrap_or(0),
                    _ => {}
                }
            }
        }
    }
    n
}

#[cfg(unix)]
fn free_space(p: &Path) -> Option<u64> {
    use std::os::unix::ffi::OsStrExt;
    let c = std::ffi::CString::new(p.as_os_str().as_bytes()).ok()?;
    let mut s: libc::statvfs = unsafe { std::mem::zeroed() };
    if unsafe { libc::statvfs(c.as_ptr(), &mut s) } != 0 {
        return None;
    }
    Some(s.f_bavail as u64 * s.f_frsize as u64)
}

#[cfg(not(unix))]
fn free_space(_: &Path) -> Option<u64> {
    Some(u64::MAX / 4)
}

fn disk_ok(root: &Path, extra: u64, lim: &Limits) -> bool {
    if fs::create_dir_all(root).is_err() {
        return false;
    }
    if folder_size(root) + extra > lim.disk_max {
        return false;
    }
    free_space(root).map_or(false, |f| f.saturating_sub(extra) >= lim.min_free && f >= extra)
}

fn safe_sub(s: &str) -> String {
    s.chars().map(|c| if c.is_ascii_alphanumeric() || c == '.' || c == '_' || c == '-' { c } else { '_' }).collect()
}

pub fn flat_name(name: &str) -> String {
    let base = name.rsplit(['/', '\\']).next().unwrap_or("");
    let s = safe_sub(base);
    let s = s.trim_matches('.');
    let s = if s.is_empty() { "file" } else { s };
    s.chars().take(80).collect()
}

fn shot_match(name: &str) -> Option<(String, String)> {
    let low = name.to_ascii_lowercase();
    let rest = low.strip_prefix("screenshot-")?;
    let (num, ext) = rest.split_once('.')?;
    let ok_num = matches!(num.len(), 1 | 2) && num.as_bytes()[0] != b'0' && num.bytes().all(|c| c.is_ascii_digit());
    if !ok_num || !["png", "jpg", "jpeg", "webp", "gif", "bmp"].contains(&ext) {
        return None;
    }
    Some((name["screenshot-".len().."screenshot-".len() + num.len()].to_string(), ext.to_string()))
}

fn rand_bytes(n: usize) -> Vec<u8> {
    let mut b = vec![0u8; n];
    if File::open("/dev/urandom").and_then(|mut f| f.read_exact(&mut b)).is_err() {
        let mut x = (now_secs() * 1e9) as u64 ^ (std::process::id() as u64) << 32;
        for v in b.iter_mut() {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            *v = x as u8;
        }
    }
    b
}

fn token_hex(n: usize) -> String {
    rand_bytes(n).iter().map(|b| format!("{:02x}", b)).collect()
}

fn new_code(root: &Path) -> Result<String, ReportError> {
    let names: Vec<String> = fs::read_dir(root).map(|rd| rd.flatten().filter_map(|e| e.file_name().into_string().ok()).collect()).unwrap_or_default();
    for _ in 0..50 {
        let code: String = "R-".to_string()
            + &rand_bytes(4).iter().map(|b| CODE_ALPHABET[(*b as usize) % CODE_ALPHABET.len()] as char).collect::<String>();
        let tag = format!("_{}_", code);
        if !names.iter().any(|d| d.contains(&tag)) {
            return Ok(code);
        }
    }
    Err(err(500, "could not allocate a report code"))
}

fn py_str(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        other => py_repr(other),
    }
}

fn py_float(f: f64) -> String {
    if f.is_finite() && f == f.trunc() && f.abs() < 1e16 {
        format!("{:.1}", f)
    } else {
        format!("{}", f)
    }
}

fn py_repr(v: &Value) -> String {
    match v {
        Value::Null => "None".into(),
        Value::Bool(b) => if *b { "True" } else { "False" }.into(),
        Value::Number(n) => n.as_i64().map(|i| i.to_string()).or_else(|| n.as_u64().map(|u| u.to_string())).unwrap_or_else(|| py_float(n.as_f64().unwrap_or(0.0))),
        Value::String(s) => {
            let q = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
            let mut o = String::new();
            o.push(q);
            for c in s.chars() {
                match c {
                    '\\' => o.push_str("\\\\"),
                    '\n' => o.push_str("\\n"),
                    '\r' => o.push_str("\\r"),
                    '\t' => o.push_str("\\t"),
                    c if c == q => {
                        o.push('\\');
                        o.push(c);
                    }
                    c if (c as u32) < 0x20 || c as u32 == 0x7f => o.push_str(&format!("\\x{:02x}", c as u32)),
                    c => o.push(c),
                }
            }
            o.push(q);
            o
        }
        Value::Array(a) => format!("[{}]", a.iter().map(py_repr).collect::<Vec<_>>().join(", ")),
        Value::Object(m) => format!("{{{}}}", m.iter().map(|(k, x)| format!("{}: {}", py_repr(&Value::String(k.clone())), py_repr(x))).collect::<Vec<_>>().join(", ")),
    }
}

pub fn truthy(v: &Value) -> bool {
    truthy_opt(Some(v))
}

pub fn truthy_opt(v: Option<&Value>) -> bool {
    match v {
        None | Some(Value::Null) => false,
        Some(Value::Bool(b)) => *b,
        Some(Value::Number(n)) => n.as_f64().map_or(false, |f| f != 0.0),
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
    }
}

fn py_int(v: Option<&Value>) -> Option<i64> {
    if !truthy_opt(v) {
        return Some(0);
    }
    match v? {
        Value::Bool(b) => Some(*b as i64),
        Value::Number(n) => n.as_i64().or_else(|| n.as_f64().filter(|f| f.is_finite()).map(|f| f.trunc() as i64)),
        Value::String(s) => {
            let t = s.trim().replace('_', "");
            t.parse::<i64>().ok()
        }
        _ => None,
    }
}

fn utc_stamp(t: i64, iso: bool) -> String {
    let (y, mo, d, hh, mm, ss) = gmtime(t);
    if iso {
        format!("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, mo, d, hh, mm, ss)
    } else {
        format!("{:04}{:02}{:02}-{:02}{:02}{:02}", y, mo, d, hh, mm, ss)
    }
}

fn write_json(path: &Path, v: &Value) -> io::Result<()> {
    let mut buf = Vec::new();
    let fmt = serde_json::ser::PrettyFormatter::with_indent(b" ");
    let mut ser = serde_json::Serializer::with_formatter(&mut buf, fmt);
    serde::Serialize::serialize(v, &mut ser).map_err(io::Error::other)?;
    let mut f = File::create(path)?;
    f.write_all(&buf)
}

pub struct Saved {
    pub code: String,
    pub dir: PathBuf,
    pub clip_key: String,
    pub clip_refused: String,
}

pub fn save_report(root: &Path, body: &[u8], ip: &str, clip_limiter: &RateLimiter, lim: &Limits) -> Result<Saved, ReportError> {
    if body.len() as u64 > MAX_BODY {
        return Err(err(413, "report too large"));
    }
    let mut z = zip::ZipArchive::new(io::Cursor::new(body)).map_err(|_| err(400, "not a zip file"))?;
    if z.len() > MAX_ENTRIES {
        return Err(err(400, "too many files"));
    }
    let mut infos = Vec::new();
    let mut total = 0u64;
    for i in 0..z.len() {
        let f = z.by_index_raw(i).map_err(|_| err(400, "not a zip file"))?;
        total += f.size();
        if f.size() > MAX_ENTRY {
            return Err(err(413, "unpacked size too large"));
        }
        infos.push((i, f.name().to_string(), f.is_dir(), f.unix_mode().map_or(false, |m| m & 0o170000 == 0o120000), f.size()));
    }
    if total > MAX_UNPACKED {
        return Err(err(413, "unpacked size too large"));
    }
    let meta_idx = infos.iter().rev().find(|x| x.1 == "meta.json").map(|x| x.0);
    let meta_in: Map<String, Value> = (|| {
        let mut f = z.by_index(meta_idx?).ok()?;
        let mut raw = Vec::new();
        f.read_to_end(&mut raw).ok()?;
        match serde_json::from_slice::<Value>(&raw).ok()? {
            Value::Object(m) => Some(m),
            _ => None,
        }
    })()
    .ok_or_else(|| err(400, "meta.json missing or not an object"))?;
    let mut meta = Map::new();
    for (k, lim) in TEXT_FIELDS {
        let s = meta_in.get(k).map(py_str).unwrap_or_default();
        meta.insert(k.to_string(), Value::String(s.chars().take(lim).collect()));
    }
    if meta["text"].as_str().unwrap_or("").trim().chars().count() < 5 {
        return Err(err(400, "the description is missing"));
    }
    meta.insert("includeDumps".into(), Value::Bool(truthy_opt(meta_in.get("includeDumps"))));
    let mut want_clips = py_int(meta_in.get("clips")).unwrap_or(0).clamp(0, CLIP_MAX_FILES as i64);
    let mut clip_refused = String::new();
    if want_clips > 0 {
        if !clip_limiter.allow(ip) {
            clip_refused = "too many reports with video clips from this address, try again later".into();
        } else if !disk_ok(root, 0, lim) {
            clip_refused = "the server cannot take video clips right now (storage is full)".into();
        }
        if !clip_refused.is_empty() {
            want_clips = 0;
        }
    }
    meta.insert("clipsExpected".into(), json!(want_clips));
    if want_clips > 0 {
        meta.insert("clipKey".into(), Value::String(token_hex(16)));
    }
    fs::create_dir_all(root).map_err(|_| err(500, "could not store the report"))?;
    let code = new_code(root)?;
    let t = now_secs() as i64;
    let player = safe_sub(meta["name"].as_str().unwrap_or("")).chars().take(15).collect::<String>();
    let player = if player.is_empty() { "player".to_string() } else { player };
    let d = root.join(format!("{}_{}_{}", utc_stamp(t, false), code, player));
    fs::create_dir_all(d.join("files")).map_err(|_| err(500, "could not store the report"))?;
    match store(&mut z, &infos, &d, meta, &code, t, ip) {
        Ok(meta) => Ok(Saved {
            code,
            dir: d,
            clip_key: meta.get("clipKey").and_then(Value::as_str).unwrap_or("").to_string(),
            clip_refused,
        }),
        Err(e) => {
            let _ = fs::remove_dir_all(&d);
            Err(e)
        }
    }
}

type Info = (usize, String, bool, bool, u64);

fn store(z: &mut zip::ZipArchive<io::Cursor<&[u8]>>, infos: &[Info], d: &Path, mut meta: Map<String, Value>, code: &str, t: i64, ip: &str) -> Result<Map<String, Value>, ReportError> {
    let mut used: HashSet<String> = HashSet::new();
    let mut files = Vec::new();
    let mut total = 0u64;
    let mut shots = 0usize;
    let io_err = |_| err(500, "could not store the report");
    for (idx, zname, is_dir, is_link, size) in infos {
        if *is_dir || zname == "meta.json" || *is_link {
            continue;
        }
        let mut name = flat_name(zname);
        let shot = shot_match(&name);
        if shot.is_some() {
            shots += 1;
            if shots > SHOT_MAX_FILES {
                return Err(err(400, format!("too many screenshots (max {})", SHOT_MAX_FILES)));
            }
            if *size > SHOT_MAX_EACH {
                return Err(err(413, format!("screenshot too large (max {} MB each)", SHOT_MAX_EACH >> 20)));
            }
        }
        let base = name.clone();
        let mut n = 1;
        while used.contains(&name) {
            n += 1;
            name = format!("{}_{}", n, base);
        }
        used.insert(name.clone());
        let fp = d.join("files").join(&name);
        {
            let mut src = z.by_index(*idx).map_err(|_| err(400, "not a zip file"))?;
            let mut dst = File::create(&fp).map_err(io_err)?;
            let mut left = MAX_ENTRY as i64;
            let mut buf = vec![0u8; 1 << 20];
            loop {
                let k = src.read(&mut buf).map_err(|_| err(400, "not a zip file"))?;
                if k == 0 {
                    break;
                }
                left -= k as i64;
                total += k as u64;
                if left < 0 {
                    return Err(err(413, "entry too large"));
                }
                if total > MAX_UNPACKED {
                    return Err(err(413, "unpacked size too large"));
                }
                dst.write_all(&buf[..k]).map_err(io_err)?;
            }
        }
        let fsize = fs::metadata(&fp).map(|m| m.len()).unwrap_or(0);
        let mut entry = Map::new();
        entry.insert("name".into(), json!(name));
        entry.insert("size".into(), json!(fsize));
        entry.insert("zipName".into(), json!(zname.chars().take(200).collect::<String>()));
        if let Some((num, ext)) = shot {
            let mut head = [0u8; 16];
            let got = File::open(&fp).and_then(|mut f| f.read(&mut head)).unwrap_or(0);
            let kind = sniff_image(&head[..got]);
            let kind = match kind {
                Some(k) if fsize <= SHOT_MAX_EACH => k,
                _ => return Err(err(400, format!("{} is not a valid png/jpg/webp/gif/bmp image (max {} MB)", name, SHOT_MAX_EACH >> 20))),
            };
            let want = if ext == "jpeg" { "jpg" } else { ext.as_str() };
            if kind != want {
                let mut newn = format!("screenshot-{}.{}", num, kind);
                while used.contains(&newn) {
                    newn = format!("x{}", newn);
                }
                fs::rename(&fp, d.join("files").join(&newn)).map_err(io_err)?;
                used.remove(&name);
                used.insert(newn.clone());
                entry.insert("name".into(), json!(newn));
            }
            entry.insert("kind".into(), json!("screenshot"));
            entry.insert("contentType".into(), json!(shot_type(kind)));
        }
        files.push(Value::Object(entry));
    }
    meta.insert("code".into(), json!(code));
    meta.insert("received".into(), json!(utc_stamp(t, true)));
    meta.insert("ip".into(), json!(ip));
    meta.insert("files".into(), Value::Array(files));
    meta.insert("handled".into(), json!(false));
    write_json(&d.join("meta.json"), &Value::Object(meta.clone())).map_err(io_err)?;
    Ok(meta)
}

pub fn find_report_dir(root: &Path, code: &str) -> Option<PathBuf> {
    let b = code.as_bytes();
    if b.len() != 6 || !code.starts_with("R-") || !b[2..].iter().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()) {
        return None;
    }
    let tag = format!("_{}_", code);
    for e in fs::read_dir(root).ok()?.flatten() {
        let n = e.file_name().into_string().unwrap_or_default();
        if n.contains(&tag) && e.path().join("meta.json").is_file() {
            return Some(e.path());
        }
    }
    None
}

fn read_meta(d: &Path) -> Result<Map<String, Value>, ReportError> {
    let b = fs::read(d.join("meta.json")).map_err(|_| err(404, "no such report"))?;
    match serde_json::from_slice::<Value>(&b) {
        Ok(Value::Object(m)) => Ok(m),
        _ => Err(err(500, "report meta.json unreadable")),
    }
}

fn videos(meta: &Map<String, Value>) -> (usize, u64) {
    let v: Vec<&Value> = meta.get("files").and_then(Value::as_array).map(|a| a.iter().filter(|x| x.get("kind").and_then(Value::as_str) == Some("video")).collect()).unwrap_or_default();
    (v.len(), v.iter().filter_map(|x| x.get("size").and_then(Value::as_u64)).sum())
}

pub struct Clip {
    pub name: String,
    pub size: u64,
}

pub fn save_clip(root: &Path, code: &str, key: &str, length: u64, body: &mut dyn Read, lock: &Mutex<()>, lim: &Limits) -> Result<Clip, ReportError> {
    let d = find_report_dir(root, code).ok_or_else(|| err(404, "no such report"))?;
    if length == 0 {
        return Err(err(400, "empty clip"));
    }
    if length > CLIP_MAX_EACH {
        return Err(err(413, format!("Clip too big - trim it to under {} MB", CLIP_MAX_EACH >> 20)));
    }
    let tmp = {
        let _g = lock.lock().unwrap_or_else(|e| e.into_inner());
        let meta = read_meta(&d)?;
        let ck = meta.get("clipKey").and_then(Value::as_str).unwrap_or("");
        if ck.is_empty() || !const_eq(key.as_bytes(), ck.as_bytes()) {
            return Err(err(403, "bad clip key"));
        }
        let (have, have_size) = videos(&meta);
        let pending = meta.get("clipsExpected").and_then(Value::as_u64).unwrap_or(0) as usize;
        if have >= CLIP_MAX_FILES.min(pending) {
            return Err(err(400, format!("too many clips for this report (max {})", CLIP_MAX_FILES)));
        }
        if have_size + length > CLIP_MAX_TOTAL {
            return Err(err(413, format!("clips together too big (max {} MB)", CLIP_MAX_TOTAL >> 20)));
        }
        if !disk_ok(root, length, lim) {
            return Err(err(507, "the server cannot take video clips right now (storage is full)"));
        }
        d.join("files").join(format!(".clip-{}.part", token_hex(4)))
    };
    let r = write_clip(&d, &tmp, length, body, lock);
    if r.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    r
}

fn write_clip(d: &Path, tmp: &Path, length: u64, body: &mut dyn Read, lock: &Mutex<()>) -> Result<Clip, ReportError> {
    let io_err = |_| err(400, "short upload");
    let mut out = File::create(tmp).map_err(|_| err(500, "could not store the clip"))?;
    let mut first = vec![0u8; length.min(64) as usize];
    let mut got = 0usize;
    while got < first.len() {
        let k = body.read(&mut first[got..]).map_err(io_err)?;
        if k == 0 {
            break;
        }
        got += k;
    }
    first.truncate(got);
    let kind = sniff_video(&first).ok_or_else(|| err(400, "not a valid mp4/mov/webm/mkv video"))?;
    out.write_all(&first).map_err(|_| err(500, "could not store the clip"))?;
    let mut total = got as u64;
    let mut buf = vec![0u8; 1 << 20];
    while total < length {
        let want = ((length - total) as usize).min(buf.len());
        let k = body.read(&mut buf[..want]).map_err(io_err)?;
        if k == 0 {
            break;
        }
        total += k as u64;
        out.write_all(&buf[..k]).map_err(|_| err(500, "could not store the clip"))?;
    }
    drop(out);
    if total != length {
        return Err(err(400, "short upload"));
    }
    let _g = lock.lock().unwrap_or_else(|e| e.into_inner());
    let mut meta = read_meta(d)?;
    let (have, have_size) = videos(&meta);
    if have >= CLIP_MAX_FILES || have_size + total > CLIP_MAX_TOTAL {
        return Err(err(413, "clips together too big"));
    }
    let name = format!("clip-{}.{}", have + 1, kind);
    fs::rename(tmp, d.join("files").join(&name)).map_err(|_| err(500, "could not store the clip"))?;
    let entry = json!({"name": name, "size": total, "kind": "video", "contentType": clip_type(kind), "zipName": ""});
    match meta.get_mut("files").and_then(Value::as_array_mut) {
        Some(a) => a.push(entry),
        None => {
            meta.insert("files".into(), Value::Array(vec![entry]));
        }
    }
    let mp = d.join("meta.json");
    let mt = d.join("meta.json.tmp");
    write_json(&mt, &Value::Object(meta)).and_then(|_| fs::rename(&mt, &mp)).map_err(|_| err(500, "could not store the clip"))?;
    Ok(Clip { name, size: total })
}

fn const_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn zip_of(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let mut buf = io::Cursor::new(Vec::new());
        {
            let mut w = zip::ZipWriter::new(&mut buf);
            for (n, d) in entries {
                w.start_file(*n, zip::write::SimpleFileOptions::default()).unwrap();
                w.write_all(d).unwrap();
            }
            w.finish().unwrap();
        }
        buf.into_inner()
    }

    fn tmp(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("t2r-rep-{}-{}", tag, std::process::id()));
        let _ = fs::remove_dir_all(&d);
        d
    }

    const LIM: Limits = Limits { disk_max: 1 << 40, min_free: 0 };

    #[test]
    fn names() {
        assert_eq!(flat_name("../../etc/passwd"), "passwd");
        assert_eq!(flat_name("a\\b\\..."), "file");
        assert_eq!(flat_name("weird name!.log"), "weird_name_.log");
        assert_eq!(shot_match("screenshot-1.PNG"), Some(("1".into(), "png".into())));
        assert_eq!(shot_match("screenshot-0.png"), None);
        assert_eq!(py_repr(&json!([1, "a", null, true, 2.0])), "[1, 'a', None, True, 2.0]");
    }

    #[test]
    fn report_and_clip() {
        let root = tmp("ok");
        let png = b"\x89PNG\r\n\x1a\n0000000000";
        let jpg_named_png: &[u8] = &[255, 216, 255, 0, 1, 2, 3, 4];
        let body = zip_of(&[
            ("meta.json", br#"{"text":"it crashed on load","name":"Bob <x>","clips":"1","steamId":76561190000000001}"#),
            ("files/t2r-play.log", b"log"),
            ("files/screenshot-1.png", png),
            ("files/screenshot-2.png", jpg_named_png),
            ("../evil/t2r-play.log", b"dup"),
        ]);
        let lim = RateLimiter::new(3, 3600.0);
        let s = save_report(&root, &body, "1.2.3.4", &lim, &LIM).ok().unwrap();
        assert!(s.dir.file_name().unwrap().to_str().unwrap().ends_with(&format!("_{}_Bob__x_", s.code)));
        assert_eq!(s.clip_key.len(), 32);
        let meta: Value = serde_json::from_slice(&fs::read(s.dir.join("meta.json")).unwrap()).unwrap();
        assert_eq!(meta["steamId"], "76561190000000001");
        assert_eq!(meta["clipsExpected"], 1);
        let names: Vec<&str> = meta["files"].as_array().unwrap().iter().map(|f| f["name"].as_str().unwrap()).collect();
        assert_eq!(names, vec!["t2r-play.log", "screenshot-1.png", "screenshot-2.jpg", "2_t2r-play.log"]);
        let lock = Mutex::new(());
        let mut clip = b"\x00\x00\x00\x18ftypisom0000000000".to_vec();
        clip.resize(100, 7);
        assert_eq!(save_clip(&root, &s.code, "bad", 100, &mut &clip[..], &lock, &LIM).err().unwrap().status, 403);
        let c = save_clip(&root, &s.code, &s.clip_key, 100, &mut &clip[..], &lock, &LIM).ok().unwrap();
        assert_eq!((c.name.as_str(), c.size), ("clip-1.mp4", 100));
        assert_eq!(save_clip(&root, &s.code, &s.clip_key, 100, &mut &clip[..], &lock, &LIM).err().unwrap().status, 400);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn refusals_leave_nothing() {
        let root = tmp("bad");
        let lim = RateLimiter::new(3, 3600.0);
        assert_eq!(save_report(&root, b"nope", "", &lim, &LIM).err().unwrap().msg, "not a zip file");
        let short = zip_of(&[("meta.json", br#"{"text":"hi"}"#)]);
        assert_eq!(save_report(&root, &short, "", &lim, &LIM).err().unwrap().msg, "the description is missing");
        let fake = zip_of(&[("meta.json", br#"{"text":"long enough"}"#), ("screenshot-1.png", b"not an image")]);
        assert_eq!(save_report(&root, &fake, "", &lim, &LIM).err().unwrap().status, 400);
        assert_eq!(fs::read_dir(&root).map(|r| r.count()).unwrap_or(0), 0);
        let _ = fs::remove_dir_all(&root);
    }
}
