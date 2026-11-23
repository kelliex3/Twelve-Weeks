use crate::http::Query;
use sha2::{Digest, Sha256};
use rusqlite::{Connection, OpenFlags};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::path::Path;
use std::sync::OnceLock;
use std::time::Duration;

const HWID_HEX: usize = 24;
const HWID_MIN_MATCH: usize = 2;
const MAX_HASHES: usize = 8;
const MAX_IDS: usize = 4;

fn is_hex(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

fn is_digits(s: &str) -> bool {
    !s.is_empty() && s.chars().all(|c| c.is_ascii_digit())
}

fn hw_hash(tag: u8, data: &[u8]) -> String {
    let mut m = b"T2R-HWID1".to_vec();
    m.push(tag);
    m.extend_from_slice(data);
    Sha256::digest(&m)[..12].iter().map(|b| format!("{:02x}", b)).collect()
}

fn hex_bytes(s: &str) -> Vec<u8> {
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap_or(0)).collect()
}

fn placeholders() -> &'static HashSet<String> {
    static P: OnceLock<HashSet<String>> = OnceLock::new();
    P.get_or_init(|| {
        let mut out = HashSet::new();
        for s in [
            "", "To Be Filled By O.E.M.", "To be filled by O.E.M.", "Default string", "Default String", "None", "none", "Unknown", "unknown", "N/A", "NA",
            "Not Specified", "Not Applicable", "System Serial Number", "SerialNumber", "Serial Number", "123456789", "1234567890", "0123456789", "00000000",
            "0000000000", "0", "000000000000", "0000_0000_0000_0000", "0000_0000_0000_0000.", "XXXXXXXX", "xxxxxxxx", "INVALID", "Invalid", "OEM", "O.E.M.",
            "ChangeMe", "default", "Default", "VMware Virtual disk", "QEMU HARDDISK",
        ] {
            out.insert(hw_hash(3, s.as_bytes()));
        }
        for n in [8usize, 16, 20, 32, 40] {
            for c in [b'0', b'F', b'f', b' '] {
                out.insert(hw_hash(3, &vec![c; n]));
            }
        }
        for u in [
            "00000000000000000000000000000000", "ffffffffffffffffffffffffffffffff", "00020003000400050006000700080009", "03000200040005000006000700080009",
            "11111111111111111111111111111111", "0123456789abcdef0123456789abcdef", "fedcba9876543210fedcba9876543210",
        ] {
            out.insert(hw_hash(2, &hex_bytes(u)));
        }
        out
    })
}

#[derive(Default)]
struct Bans {
    ids: HashMap<String, String>,
    names: Vec<(String, String)>,
    hw: HashMap<String, String>,
    tok: HashMap<String, String>,
}

impl Bans {
    fn is_empty(&self) -> bool {
        self.ids.is_empty() && self.names.is_empty() && self.hw.is_empty() && self.tok.is_empty()
    }
}

fn parse_bans(path: &Path) -> Bans {
    let mut out = Bans::default();
    let raw = match std::fs::read(path) {
        Ok(b) => b,
        Err(_) => return out,
    };
    let text = String::from_utf8_lossy(&raw).replace('\u{fffd}', "");
    for line in text.lines() {
        let (body, comment) = line.split_once('#').unwrap_or((line, ""));
        let e = body.trim();
        if e.is_empty() {
            continue;
        }
        static RE: OnceLock<regex::Regex> = OnceLock::new();
        let re = RE.get_or_init(|| regex::Regex::new(r"(?i)reason\s*:\s*(.*)$").unwrap());
        let reason = re.captures(comment.trim()).map(|m| m[1].trim().chars().take(200).collect()).unwrap_or_default();
        let low = e.to_lowercase();
        if let Some(t) = low.strip_prefix("token:") {
            let t = t.trim();
            if t.len() == 64 && is_hex(t) {
                out.tok.insert(t.to_string(), reason);
            }
        } else if let Some(h) = low.strip_prefix("hwid:") {
            let h = h.trim();
            if h.len() >= HWID_HEX && is_hex(h) {
                out.hw.insert(h[..HWID_HEX].to_string(), reason);
            }
        } else if is_digits(e) && e.chars().count() >= 15 {
            out.ids.insert(e.to_string(), reason);
        } else if let Some(x) = out.names.iter_mut().find(|(n, _)| *n == low) {
            x.1 = reason;
        } else {
            out.names.push((low, reason));
        }
    }
    out
}

fn verdict(banned: bool, reason: &str) -> Value {
    json!({"banned": banned, "reason": reason})
}

fn check(db: Option<&Path>, ban_path: Option<&Path>, hashes: &[String], steamids: &[String], tokens: &[String]) -> Value {
    let bans = match ban_path {
        Some(p) => parse_bans(p),
        None => Bans::default(),
    };
    if bans.is_empty() {
        return verdict(false, "");
    }
    for t in tokens {
        if let Some(r) = bans.tok.get(t) {
            return verdict(true, r);
        }
    }
    for s in steamids {
        if let Some(r) = bans.ids.get(s) {
            return verdict(true, r);
        }
    }
    let ph = placeholders();
    let mut mine: Vec<String> = Vec::new();
    for h in hashes {
        if !ph.contains(h) && !mine.contains(h) {
            mine.push(h.clone());
        }
    }
    for h in &mine {
        if let Some(r) = bans.hw.get(h) {
            return verdict(true, r);
        }
    }
    let db = match db {
        Some(d) if !bans.ids.is_empty() || !bans.names.is_empty() => d,
        _ => return verdict(false, ""),
    };
    let c = match Connection::open_with_flags(db, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX) {
        Ok(c) => c,
        Err(_) => return verdict(false, ""),
    };
    let _ = c.busy_timeout(Duration::from_secs(2));
    db_check(&c, &bans, steamids, mine).unwrap_or_else(|| verdict(false, ""))
}

fn col(c: &Connection, sql: &str, p: impl rusqlite::Params) -> Option<Vec<rusqlite::types::Value>> {
    let mut st = c.prepare_cached(sql).ok()?;
    let rows = st.query_map(p, |r| r.get::<_, rusqlite::types::Value>(0)).ok()?;
    rows.collect::<Result<Vec<_>, _>>().ok()
}

fn text_of(v: &rusqlite::types::Value) -> Option<String> {
    match v {
        rusqlite::types::Value::Text(s) => Some(s.clone()),
        rusqlite::types::Value::Integer(i) => Some(i.to_string()),
        rusqlite::types::Value::Blob(b) => Some(String::from_utf8_lossy(b).into_owned()),
        rusqlite::types::Value::Real(f) => Some(f.to_string()),
        rusqlite::types::Value::Null => None,
    }
}

fn sid_int(s: &str) -> Option<i64> {
    s.parse::<i64>().ok()
}

fn db_check(c: &Connection, bans: &Bans, steamids: &[String], mut mine: Vec<String>) -> Option<Value> {
    let ph = placeholders();
    let mut banned_ids: Vec<(String, String)> = bans.ids.iter().map(|(k, v)| (k.clone(), v.clone())).collect();
    let mut seen_ids: HashSet<String> = bans.ids.keys().cloned().collect();
    for (n, why) in &bans.names {
        for v in col(c, "SELECT steamid FROM players WHERE lower(name)=? AND steamid", [n])? {
            if let Some(sid) = text_of(&v) {
                if seen_ids.insert(sid.clone()) {
                    banned_ids.push((sid, why.clone()));
                }
            }
        }
    }
    for s in steamids {
        if let Some((_, r)) = banned_ids.iter().find(|(k, _)| k == s) {
            return Some(verdict(true, r));
        }
    }
    let mut derived: HashMap<String, String> = HashMap::new();
    let mut toks: Vec<(String, String)> = Vec::new();
    let mut tok_seen: HashSet<String> = HashSet::new();
    for (sid, why) in &banned_ids {
        let n = sid_int(sid)?;
        for v in col(c, "SELECT token FROM players WHERE steamid=?", [n])? {
            if let Some(t) = text_of(&v) {
                if tok_seen.insert(t.clone()) {
                    toks.push((t, why.clone()));
                }
            }
        }
        for v in col(c, "SELECT hash FROM hwids WHERE steamid=?", [n])? {
            if let Some(h) = text_of(&v) {
                derived.entry(h).or_insert_with(|| why.clone());
            }
        }
    }
    for (n, why) in &bans.names {
        for v in col(c, "SELECT token FROM hwids WHERE lower(name)=?", [n])? {
            if let Some(t) = text_of(&v) {
                if tok_seen.insert(t.clone()) {
                    toks.push((t, why.clone()));
                }
            }
        }
    }
    for (t, why) in &toks {
        for v in col(c, "SELECT hash FROM hwids WHERE token=?", [t])? {
            if let Some(h) = text_of(&v) {
                derived.entry(h).or_insert_with(|| why.clone());
            }
        }
    }
    for s in steamids {
        let n = sid_int(s)?;
        for v in col(c, "SELECT hash FROM hwids WHERE steamid=?", [n])? {
            if let Some(h) = text_of(&v) {
                if !mine.contains(&h) {
                    mine.push(h);
                }
            }
        }
    }
    mine.retain(|h| !ph.contains(h));
    for h in &mine {
        if let Some(r) = bans.hw.get(h) {
            return Some(verdict(true, r));
        }
    }
    let hits: Vec<&String> = mine.iter().filter(|h| derived.contains_key(*h) && !bans.hw.contains_key(*h)).collect();
    if hits.len() >= HWID_MIN_MATCH {
        return Some(verdict(true, &derived[hits[0]]));
    }
    Some(verdict(false, ""))
}

pub fn route(db: Option<&Path>, q: &Query) -> Value {
    let vals = |k: &str| -> Vec<String> {
        q.all(k).iter().flat_map(|v| v.split(',')).filter(|x| !x.is_empty()).take(16).map(|x| x.to_string()).collect()
    };
    let tokens: Vec<String> = vals("t").into_iter().take(2).map(|t| t.to_lowercase()).filter(|t| t.len() == 64 && is_hex(t)).collect();
    let mut hashes: Vec<String> = Vec::new();
    for v in vals("h").into_iter().take(MAX_HASHES) {
        let v = v.trim().to_lowercase();
        if v.len() == HWID_HEX && is_hex(&v) && !hashes.contains(&v) {
            hashes.push(v);
        }
    }
    let mut ids: Vec<String> = Vec::new();
    for v in vals("s").into_iter().take(MAX_IDS) {
        let v = v.trim().to_string();
        if is_digits(&v) && (15..=20).contains(&v.len()) && !ids.contains(&v) {
            ids.push(v);
        }
    }
    let bans = db.map(|d| d.parent().unwrap_or(Path::new(".")).join("bans.txt"));
    check(db, bans.as_deref(), &hashes, &ids, &tokens)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn placeholder_hashes_match_python() {
        assert_eq!(hw_hash(3, b""), "77d792b560b8c9cf9b656aab");
        assert_eq!(placeholders().len(), 62);
    }

    #[test]
    fn explicit_entries() {
        let d = std::env::temp_dir().join(format!("t2r-ban-{}", std::process::id()));
        std::fs::create_dir_all(&d).unwrap();
        let b = d.join("bans.txt");
        std::fs::write(&b, "76561190000000001 # reason: cheating\nhwid:AABBCCDDEEFF001122334455 # private note\nSomeName\n").unwrap();
        let v = check(None, Some(&b), &[], &["76561190000000001".into()], &[]);
        assert_eq!(v, json!({"banned": true, "reason": "cheating"}));
        let v = check(None, Some(&b), &["aabbccddeeff001122334455".into()], &[], &[]);
        assert_eq!(v, json!({"banned": true, "reason": ""}));
        let v = check(None, Some(&b), &["aabbccddeeff001122334456".into()], &["76561190000000002".into()], &[]);
        assert_eq!(v, json!({"banned": false, "reason": ""}));
        let _ = std::fs::remove_dir_all(&d);
    }
}
