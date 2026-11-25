//! Player roles for the rank badge colour and the lead developer's nametag.
//!
//! The Rust version of roles.py. Codes: 0 none, 1 OG, 2 admin, 3 dev, 4 lead dev; a player with several roles gets the highest.
//! Sources (Steam64 ids, one per line, '#' starts a comment, re-read when the file changes): og-steamids.txt, web-admins.txt
//! (a line whose comment contains TEMP gets no role), devs.txt ('<steamid> [lead] # name').
use parking_lot::Mutex;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

pub const NONE: i64 = 0;
pub const OG: i64 = 1;
pub const ADMIN: i64 = 2;
pub const DEV: i64 = 3;
pub const LEAD: i64 = 4;

const KINDS: [(&str, &str); 3] = [("og", "og-steamids.txt"), ("admin", "web-admins.txt"), ("dev", "devs.txt")];

/// `FILES[kind]`
pub fn file_name(kind: &str) -> Option<&'static str> {
    KINDS.iter().find(|(k, _)| *k == kind).map(|(_, f)| *f)
}

/// `str.splitlines()` (all of Python's line boundaries).
fn splitlines(s: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0usize;
    let b: Vec<(usize, char)> = s.char_indices().collect();
    let mut i = 0usize;
    while i < b.len() {
        let (pos, c) = b[i];
        let brk = matches!(c, '\n' | '\r' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}' | '\u{2029}');
        if brk {
            out.push(&s[start..pos]);
            let mut next = pos + c.len_utf8();
            if c == '\r' && i + 1 < b.len() && b[i + 1].1 == '\n' {
                i += 1;
                next += 1;
            }
            start = next;
        }
        i += 1;
    }
    if start < s.len() {
        out.push(&s[start..]);
    }
    out
}

/// `bytes.decode("utf-8", "ignore")`.
pub fn utf8_ignore(b: &[u8]) -> String {
    let mut out = String::new();
    let mut rest = b;
    loop {
        match std::str::from_utf8(rest) {
            Ok(s) => {
                out.push_str(s);
                break;
            }
            Err(e) => {
                let v = e.valid_up_to();
                out.push_str(std::str::from_utf8(&rest[..v]).unwrap_or(""));
                match e.error_len() {
                    Some(n) => rest = &rest[v + n..],
                    None => break,
                }
            }
        }
    }
    out
}

// py: roles.py:28 parse_ids
/// {steamid: [lower-cased words after the id]} from a list file; bad lines are ignored. Insertion order is kept (a repeated id keeps its first position).
pub fn parse_ids(text: &str, skip_temp: bool) -> indexmap::IndexMap<u128, Vec<String>> {
    let mut out = indexmap::IndexMap::new();
    for line in splitlines(text) {
        let (body, comment) = match line.find('#') {
            Some(p) => (&line[..p], &line[p + 1..]),
            None => (line, ""),
        };
        let words: Vec<&str> = body.split(crate::py::is_py_space).filter(|w| !w.is_empty()).collect();
        let Some(first) = words.first() else { continue };
        if first.is_empty() || !first.chars().all(|c| c.is_ascii_digit()) {
            continue;
        }
        if skip_temp && comment.contains("TEMP") {
            continue;
        }
        let Ok(sid) = first.parse::<u128>() else { continue };
        if sid > 0 {
            out.insert(sid, words[1..].iter().map(|w| w.to_lowercase()).collect());
        }
    }
    out
}

type Stamp = Option<(i128, u64)>;

struct Inner {
    stamp: HashMap<&'static str, Stamp>,
    lists: HashMap<&'static str, indexmap::IndexMap<u128, Vec<String>>>,
}

pub struct Roles {
    data_dir: PathBuf,
    inner: Mutex<Inner>,
}

impl Roles {
    // py: roles.py:44 Roles.__init__
    pub fn new(data_dir: &Path) -> Roles {
        let mut lists = HashMap::new();
        for (k, _) in KINDS {
            lists.insert(k, indexmap::IndexMap::new());
        }
        Roles { data_dir: data_dir.to_path_buf(), inner: Mutex::new(Inner { stamp: HashMap::new(), lists }) }
    }

    fn stat(p: &Path) -> Stamp {
        use std::time::UNIX_EPOCH;
        let md = std::fs::metadata(p).ok()?;
        let ns = match md.modified().ok()?.duration_since(UNIX_EPOCH) {
            Ok(d) => d.as_nanos() as i128,
            Err(e) => -(e.duration().as_nanos() as i128),
        };
        Some((ns, md.len()))
    }

    // py: roles.py:49 Roles._refresh
    fn refresh(&self) {
        let mut g = self.inner.lock();
        for (k, fn_) in KINDS {
            let p = self.data_dir.join(fn_);
            let stamp = Self::stat(&p);
            if g.stamp.get(k) == Some(&stamp) {
                continue;
            }
            g.stamp.insert(k, stamp);
            let mut text = String::new();
            if stamp.is_some() {
                text = match std::fs::read(&p) {
                    Ok(b) => utf8_ignore(&b),
                    Err(_) => String::new(),
                };
            }
            g.lists.insert(k, parse_ids(&text, k == "admin"));
        }
    }

    // py: roles.py:68 Roles.role_of
    /// Role code of a Steam64 id (0 for none / unknown / 0).
    pub fn role_of(&self, steamid: i64) -> i64 {
        if steamid <= 0 {
            return NONE;
        }
        self.refresh();
        let sid = steamid as u128;
        let g = self.inner.lock();
        if let Some(dev) = g.lists["dev"].get(&sid) {
            return if dev.iter().any(|w| w == "lead") { LEAD } else { DEV };
        }
        if g.lists["admin"].contains_key(&sid) {
            return ADMIN;
        }
        if g.lists["og"].contains_key(&sid) {
            return OG;
        }
        NONE
    }
}

// py: roles.py:88 freeze_og
/// Write every Steam id in the players table (optionally only last_seen < before) to out_path. Returns the count.
pub fn freeze_og(db_path: &str, out_path: &str, before: Option<f64>) -> std::io::Result<usize> {
    let c = rusqlite::Connection::open_with_flags(db_path, rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY)
        .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e.to_string()))?;
    let mut q = "SELECT DISTINCT steamid FROM players WHERE steamid > 0".to_string();
    if before.is_some() {
        q.push_str(" AND last_seen < ?");
    }
    let mut stmt = c.prepare(&q).map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e.to_string()))?;
    let map = |r: &rusqlite::Row| r.get::<_, i64>(0);
    let rows: Vec<i64> = match before {
        Some(b) => stmt.query_map([b], map),
        None => stmt.query_map([], map),
    }
    .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e.to_string()))?
    .flatten()
    .collect();
    let mut ids = rows;
    ids.sort();
    let tmp = format!("{}.tmp", out_path);
    let mut s = String::from("# OG: every Steam id that played before the public release (tools/roles.py freeze-og)\n");
    for sid in &ids {
        s.push_str(&format!("{}\n", sid));
    }
    std::fs::write(&tmp, s)?;
    std::fs::rename(&tmp, out_path)?;
    Ok(ids.len())
}

// ---------------------------------------------------------------- differential test
// verify/py/small_harness.py is the Python side: JSON lines {"m":"roles","op":..,..}; the list files live in $T2R_VERIFY_DIR/roles.
pub fn selftest(input: &mut dyn std::io::BufRead, out: &mut dyn std::io::Write) -> i32 {
    use crate::py::dumps_compact;
    use serde_json::{json, Value};
    let dir = crate::selftest::tmp_dir("roles");
    let r = Roles::new(&dir);
    let mut line = String::new();
    loop {
        line.clear();
        match input.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let Ok(v) = serde_json::from_str::<Value>(line.trim()) else { continue };
        if v["m"].as_str() != Some("roles") {
            continue;
        }
        let res: Value = match v["op"].as_str().unwrap_or("") {
            "write" => {
                // {"kind","text_hex"|null}: the file content as hex bytes (so invalid UTF-8 can be tested); null removes the file
                let fnm = file_name(v["kind"].as_str().unwrap_or("")).unwrap_or("x.txt");
                let p = dir.join(fnm);
                match v["hex"].as_str() {
                    Some(h) => {
                        let b: Vec<u8> = (0..h.len() / 2).filter_map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).ok()).collect();
                        let _ = std::fs::write(&p, b);
                        // a fixed write time per script line: the stamp must not depend on the file system's clock granularity
                        if let (Some(mt), Ok(f)) = (v["mt"].as_u64(), std::fs::OpenOptions::new().write(true).open(&p)) {
                            let _ = f.set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(mt));
                        }
                    }
                    None => {
                        let _ = std::fs::remove_file(&p);
                    }
                }
                Value::Null
            }
            "role_of" => json!(r.role_of(v["sid"].as_i64().unwrap_or(0))),
            "parse" => {
                let m = parse_ids(v["text"].as_str().unwrap_or(""), v["skip_temp"].as_bool().unwrap_or(false));
                let mut o = serde_json::Map::new();
                for (k, w) in m {
                    o.insert(k.to_string(), json!(w));
                }
                Value::Object(o)
            }
            _ => json!("?"),
        };
        let _ = writeln!(out, "{}", dumps_compact(&res));
    }
    0
}
