use crate::http::{Body, Request, Response};
use crate::util::{http_date, mtime_secs, unquote};
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::UNIX_EPOCH;

const MIME: &[(&str, &str)] = &[
    ("html", "text/html"),
    ("htm", "text/html"),
    ("css", "text/css"),
    ("js", "text/javascript"),
    ("mjs", "text/javascript"),
    ("json", "application/json"),
    ("webmanifest", "application/manifest+json"),
    ("xml", "application/xml"),
    ("txt", "text/plain"),
    ("md", "text/markdown"),
    ("csv", "text/csv"),
    ("py", "text/x-python"),
    ("png", "image/png"),
    ("jpg", "image/jpeg"),
    ("jpeg", "image/jpeg"),
    ("gif", "image/gif"),
    ("webp", "image/webp"),
    ("avif", "image/avif"),
    ("svg", "image/svg+xml"),
    ("ico", "image/vnd.microsoft.icon"),
    ("bmp", "image/bmp"),
    ("mp4", "video/mp4"),
    ("webm", "video/webm"),
    ("mp3", "audio/mpeg"),
    ("ogg", "audio/ogg"),
    ("wav", "audio/x-wav"),
    ("woff", "font/woff"),
    ("woff2", "font/woff2"),
    ("ttf", "font/ttf"),
    ("otf", "font/otf"),
    ("wasm", "application/wasm"),
    ("pdf", "application/pdf"),
    ("zip", "application/zip"),
    ("bin", "application/octet-stream"),
];

fn mime(name: &str) -> &'static str {
    let stem = name.trim_start_matches('.');
    let ext = match stem.rsplit_once('.') {
        Some((_, e)) => e.to_ascii_lowercase(),
        None => return "application/octet-stream",
    };
    MIME.iter().find(|(e, _)| *e == ext).map_or("application/octet-stream", |m| m.1)
}

fn common(ctype: &str, len: usize, cache: &str) -> Vec<(&'static str, String)> {
    vec![
        ("Content-Type", ctype.to_string()),
        ("Content-Length", len.to_string()),
        ("Cache-Control", cache.to_string()),
        ("X-Content-Type-Options", "nosniff".into()),
        ("Referrer-Policy", "same-origin".into()),
    ]
}

pub fn send(code: u16, body: Vec<u8>, ctype: &str, cache: &str, extra: Vec<(&'static str, String)>) -> Response {
    let mut h = common(ctype, body.len(), cache);
    h.extend(extra);
    Response::new(code, h, body)
}

pub fn send_shared(body: Arc<Vec<u8>>, ctype: &str, cache: &str) -> Response {
    Response { code: 200, headers: common(ctype, body.len(), cache), body: Body::Shared(body), close: false }
}

fn not_found(root: &Path) -> Response {
    let body = fs::read(root.join("404.html")).unwrap_or_else(|_| b"Not found".to_vec());
    send(404, body, "text/html; charset=utf-8", "no-store", Vec::new())
}

fn resolve(root: &Path, raw: &str) -> Option<PathBuf> {
    let mut p = raw.to_string();
    if p.ends_with('/') {
        p.push_str("index.html");
    }
    let dec = unquote(&p, false);
    if dec.contains('\0') {
        return None;
    }
    let mut parts: Vec<&str> = Vec::new();
    let mut escaped = false;
    for seg in dec.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                if parts.pop().is_none() {
                    escaped = true;
                }
            }
            s => parts.push(s),
        }
    }
    if escaped || parts.is_empty() {
        return None;
    }
    let base = parts[parts.len() - 1];
    if base.starts_with('_') || base.starts_with('.') {
        return None;
    }
    let mut f = root.to_path_buf();
    for s in parts {
        f.push(s);
    }
    Some(f)
}

fn modified(m: &fs::Metadata) -> i64 {
    m.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).map_or(0, |d| d.as_secs() as i64)
}

fn rewrite_versions(root: &Path, body: &[u8]) -> (Vec<u8>, i64) {
    let mut out = Vec::with_capacity(body.len() + 256);
    let mut newest = 0;
    let mut i = 0;
    let name_ch = |c: u8| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'_' || c == b'-';
    while i < body.len() {
        let rest = &body[i..];
        let attr = if rest.starts_with(b"href=\"") {
            6
        } else if rest.starts_with(b"src=\"") {
            5
        } else {
            0
        };
        if attr > 0 {
            let s = i + attr;
            let mut j = s;
            while j < body.len() && name_ch(body[j]) {
                j += 1;
            }
            if j > s && body.get(j) == Some(&b'.') {
                let after = &body[j + 1..];
                let ext = if after.starts_with(b"css\"") {
                    3
                } else if after.starts_with(b"js\"") {
                    2
                } else {
                    0
                };
                if ext > 0 {
                    let end = j + 1 + ext;
                    let name = String::from_utf8_lossy(&body[s..end]).into_owned();
                    let v = mtime_secs(&root.join(&name));
                    newest = newest.max(v);
                    out.extend_from_slice(&body[i..end]);
                    out.extend_from_slice(format!("?v={}", v).as_bytes());
                    out.push(b'"');
                    i = end + 1;
                    continue;
                }
            }
        }
        out.push(body[i]);
        i += 1;
    }
    (out, newest)
}

pub fn serve(req: &Request, root: &Path, csp: &str) -> Response {
    let f = match resolve(root, req.path()) {
        Some(f) => f,
        None => return not_found(root),
    };
    let meta = match fs::metadata(&f) {
        Ok(m) if m.is_file() => m,
        _ => return not_found(root),
    };
    let fname = f.file_name().and_then(|n| n.to_str()).unwrap_or("");
    let mut ctype = mime(fname).to_string();
    if ctype.starts_with("text/") || ctype == "application/javascript" {
        ctype.push_str("; charset=utf-8");
    }
    let cache = if f.to_string_lossy().contains("/assets/") {
        "public, max-age=86400"
    } else {
        "no-cache"
    };
    if fname.ends_with(".mp4") || fname.ends_with(".webm") {
        return media(req, f, meta.len(), &ctype, cache);
    }
    let mut mtime = modified(&meta);
    if fname.ends_with(".html") {
        let raw = match fs::read(&f) {
            Ok(b) => b,
            Err(_) => return not_found(root),
        };
        let (body, newest) = rewrite_versions(root, &raw);
        mtime = mtime.max(newest);
        let lm = http_date(mtime);
        if req.header("if-modified-since") == Some(lm.as_str()) {
            return not_modified(cache, &lm);
        }
        return send(200, body, &ctype, cache, vec![("Content-Security-Policy", csp.to_string()), ("Last-Modified", lm)]);
    }
    let lm = http_date(mtime);
    if req.header("if-modified-since") == Some(lm.as_str()) {
        return not_modified(cache, &lm);
    }
    let file = match File::open(&f) {
        Ok(h) => h,
        Err(_) => return not_found(root),
    };
    let len = meta.len();
    let mut h = common(&ctype, len as usize, cache);
    h.push(("Last-Modified", lm));
    Response { code: 200, headers: h, body: Body::File(file, 0, len), close: false }
}

fn not_modified(cache: &str, lm: &str) -> Response {
    Response::new(304, vec![("Cache-Control", cache.to_string()), ("Last-Modified", lm.to_string())], Vec::new())
}

fn parse_range(h: Option<&str>) -> Option<(String, String)> {
    let s = h?.trim().strip_prefix("bytes=")?;
    let (a, b) = s.split_once('-')?;
    if !a.bytes().all(|c| c.is_ascii_digit()) || !b.bytes().all(|c| c.is_ascii_digit()) || (a.is_empty() && b.is_empty()) {
        return None;
    }
    Some((a.to_string(), b.to_string()))
}

fn media(req: &Request, f: PathBuf, size: u64, ctype: &str, cache: &str) -> Response {
    let mut start: u64 = 0;
    let mut end: i128 = size as i128 - 1;
    let mut code = 200;
    if let Some((a, b)) = parse_range(req.header("range")) {
        let big = |x: &str| x.parse::<u128>().unwrap_or(u128::MAX);
        if !a.is_empty() {
            start = big(&a).min(u64::MAX as u128) as u64;
            if !b.is_empty() {
                end = (big(&b).min(i128::MAX as u128) as i128).min(size as i128 - 1);
            }
        } else {
            start = (size as u128).saturating_sub(big(&b)) as u64;
        }
        if start as i128 > end || start >= size {
            return Response::new(416, vec![("Content-Range", format!("bytes */{}", size)), ("Content-Length", "0".into())], Vec::new());
        }
        code = 206;
    }
    let len = (end - start as i128 + 1).max(0) as u64;
    let file = match File::open(&f) {
        Ok(h) => h,
        Err(_) => return Response::new(404, vec![("Content-Length", "0".into())], Vec::new()),
    };
    let mut h = vec![("Content-Type", ctype.to_string()), ("Accept-Ranges", "bytes".into()), ("Content-Length", len.to_string())];
    if code == 206 {
        h.push(("Content-Range", format!("bytes {}-{}/{}", start, end, size)));
    }
    h.push(("Cache-Control", cache.to_string()));
    h.push(("X-Content-Type-Options", "nosniff".into()));
    Response { code, headers: h, body: Body::File(file, start, len), close: false }
}
