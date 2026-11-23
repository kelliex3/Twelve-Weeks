use crate::http::{gmtime, now_secs};
use crate::log;
use flate2::read::GzDecoder;
use flate2::write::GzEncoder;
use flate2::Compression;
use regex::Regex;
use serde_json::Value;
use std::collections::HashSet;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::sync::OnceLock;
use std::time::{Duration, UNIX_EPOCH};

const HOURS: f64 = 6.0;
const MAX_OUT: usize = 6 * 1024 * 1024;

struct Res {
    skip: Regex,
    pair: Regex,
    mid: Regex,
    ipv4: Regex,
}

fn res() -> &'static Res {
    static R: OnceLock<Res> = OnceLock::new();
    R.get_or_init(|| Res {
        skip: Regex::new(r"^\[\d\d:\d\d:\d\d\] (TCP \(|PING |STATUS )").unwrap(),
        pair: Regex::new(r"\('(\d{1,3}(?:\.\d{1,3}){3})', (\d+)\)").unwrap(),
        mid: Regex::new(r"\b(?:match|mid) ([0-9a-f]{8})\b").unwrap(),
        ipv4: Regex::new(r"\b\d{1,3}(?:\.\d{1,3}){3}\b").unwrap(),
    })
}

fn mtime(p: &Path) -> Option<f64> {
    fs::metadata(p).ok()?.modified().ok()?.duration_since(UNIX_EPOCH).ok().map(|d| d.as_secs_f64())
}

fn archive_dir(log_dir: &Path) -> PathBuf {
    log_dir.join("log-archive")
}

fn list(dir: &Path, keep: impl Fn(&str) -> bool) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(rd) = fs::read_dir(dir) {
        for e in rd.flatten() {
            if let Some(n) = e.file_name().to_str() {
                if keep(n) {
                    out.push(e.path());
                }
            }
        }
    }
    out
}

fn gz_copy(src: &Path, dst: &Path) -> io::Result<()> {
    let mut i = File::open(src)?;
    let mut o = GzEncoder::new(File::create(dst)?, Compression::new(6));
    io::copy(&mut i, &mut o)?;
    o.finish()?.sync_all()
}

pub fn archive_rotated(log_dir: &Path, days: f64) -> usize {
    let ad = archive_dir(log_dir);
    if fs::create_dir_all(&ad).is_err() {
        return 0;
    }
    let mut made = 0;
    let rotated = list(log_dir, |n| n.strip_prefix("daemon.log.").map_or(false, |r| r.starts_with(|c: char| c.is_ascii_digit())));
    for p in rotated {
        let mt = match mtime(&p) {
            Some(m) => m,
            None => continue,
        };
        let (y, mo, d, hh, mm, ss) = gmtime(mt as i64);
        let out = ad.join(format!("daemon-{:04}{:02}{:02}-{:02}{:02}{:02}.log.gz", y, mo, d, hh, mm, ss));
        if out.exists() {
            continue;
        }
        let tmp = out.with_extension("gz.tmp");
        match gz_copy(&p, &tmp).and_then(|_| fs::rename(&tmp, &out)) {
            Ok(()) => {
                let t = UNIX_EPOCH + Duration::from_secs_f64(mt);
                if let Ok(f) = File::options().write(true).open(&out) {
                    let _ = f.set_times(fs::FileTimes::new().set_accessed(t).set_modified(t));
                }
                made += 1;
            }
            Err(_) => {
                let _ = fs::remove_file(&tmp);
            }
        }
    }
    let now = now_secs();
    for p in list(&ad, |n| n.starts_with("daemon-") && n.ends_with(".log.gz")) {
        if mtime(&p).map_or(false, |m| now - m > days * 86400.0) {
            let _ = fs::remove_file(&p);
        }
    }
    made
}

fn sources(log_dir: &Path, since: f64) -> Vec<PathBuf> {
    let mut files: Vec<(f64, PathBuf)> = Vec::new();
    let mut archived = HashSet::new();
    for p in list(&archive_dir(log_dir), |n| n.starts_with("daemon-") && n.ends_with(".log.gz")) {
        if let Some(mt) = mtime(&p) {
            archived.insert(mt as i64);
            if mt >= since {
                files.push((mt, p));
            }
        }
    }
    let live = log_dir.join("daemon.log");
    for p in list(log_dir, |n| n.starts_with("daemon.log")) {
        if let Some(mt) = mtime(&p) {
            if mt < since || (p != live && archived.contains(&(mt as i64))) {
                continue;
            }
            files.push((mt, p));
        }
    }
    files.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal).then_with(|| a.1.cmp(&b.1)));
    files.into_iter().map(|x| x.1).collect()
}

fn for_lines(path: &Path, mut f: impl FnMut(&str) -> bool) {
    let file = match File::open(path) {
        Ok(f) => f,
        Err(_) => return,
    };
    let rd: Box<dyn Read> = if path.extension().map_or(false, |e| e == "gz") { Box::new(GzDecoder::new(file)) } else { Box::new(file) };
    let mut r = BufReader::with_capacity(1 << 16, rd);
    let mut buf = Vec::new();
    loop {
        buf.clear();
        match r.read_until(b'\n', &mut buf) {
            Ok(0) | Err(_) => return,
            Ok(_) => {}
        }
        if !f(&String::from_utf8_lossy(&buf)) {
            return;
        }
    }
}

fn py_repr(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut out = String::new();
    out.push(q);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == q => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => out.push_str(&format!("\\x{:02x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push(q);
    out
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

fn has_word(line: &str, name: &str) -> bool {
    let mut from = 0;
    while let Some(i) = line[from..].find(name) {
        let s = from + i;
        let e = s + name.len();
        let before = line[..s].chars().next_back().map_or(false, is_word);
        let after = line[e..].chars().next().map_or(false, is_word);
        if !before && !after {
            return true;
        }
        from = s + line[s..].chars().next().map_or(1, |c| c.len_utf8());
    }
    false
}

pub fn snapshot(report_dir: &Path, meta: &Value, log_dir: &Path, now: f64) -> io::Result<usize> {
    let r = res();
    let name = meta.get("name").and_then(Value::as_str).unwrap_or("").trim().to_string();
    let steam = meta.get("steamId").and_then(Value::as_str).unwrap_or("").trim().to_string();
    let use_name = name.chars().count() >= 2;
    let use_steam = steam.len() >= 15 && steam.chars().all(|c| c.is_ascii_digit());
    if !use_name && !use_steam {
        return Ok(0);
    }
    let named = |l: &str| (use_name && has_word(l, &name)) || (use_steam && l.contains(steam.as_str()));
    let pairs_of = |l: &str| -> Vec<(String, String)> { r.pair.captures_iter(l).map(|c| (c[1].to_string(), c[2].to_string())).collect() };
    let mids_of = |l: &str| -> Vec<String> { r.mid.captures_iter(l).map(|c| c[1].to_string()).collect() };
    let srcs = sources(log_dir, now - HOURS * 3600.0);
    let mut pairs: HashSet<(String, String)> = HashSet::new();
    let mut mids: HashSet<String> = HashSet::new();
    for p in &srcs {
        for_lines(p, |l| {
            if r.skip.is_match(l) {
                return true;
            }
            if named(l) {
                pairs.extend(pairs_of(l));
                mids.extend(mids_of(l));
            } else if !pairs.is_empty() && r.mid.is_match(l) && pairs_of(l).iter().any(|x| pairs.contains(x)) {
                mids.extend(mids_of(l));
            }
            true
        });
    }
    let mut sorted: Vec<&String> = mids.iter().collect();
    sorted.sort();
    let mid_text = if sorted.is_empty() { "-".to_string() } else { sorted.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(" ") };
    let mut out = format!(
        "# daemon lines for {} (steam {}), {} file(s) from the last {:.0} h; {} session address(es), match ids: {}\n",
        name.replace('\\', "\\\\").replace('\'', "\\'"),
        if steam.is_empty() { "-" } else { &steam },
        srcs.len(),
        HOURS,
        pairs.len(),
        mid_text
    );
    let mut size = 0usize;
    let mut n = 0usize;
    let mut cut = false;
    for p in &srcs {
        out += &format!("# --- {}\n", p.file_name().and_then(|x| x.to_str()).unwrap_or(""));
        for_lines(p, |l| {
            if r.skip.is_match(l) {
                return true;
            }
            let hit = named(l)
                || (!pairs.is_empty() && pairs_of(l).iter().any(|x| pairs.contains(x)))
                || (!mids.is_empty() && mids_of(l).iter().any(|m| mids.contains(m)));
            if !hit {
                return true;
            }
            let line = r.ipv4.replace_all(l, "IP");
            size += line.len();
            if size > MAX_OUT {
                out += &format!("# ... cut at {} MB\n", MAX_OUT >> 20);
                cut = true;
                return false;
            }
            out += &line;
            n += 1;
            true
        });
        if cut {
            break;
        }
    }
    let fd = report_dir.join("files");
    fs::create_dir_all(&fd)?;
    let tmp = fd.join("daemon-lines.txt.tmp");
    let mut f = File::create(&tmp)?;
    f.write_all(out.as_bytes())?;
    drop(f);
    fs::rename(&tmp, fd.join("daemon-lines.txt"))?;
    Ok(n)
}

pub fn snapshot_later(report_dir: PathBuf, log_dir: PathBuf, days: f64) {
    std::thread::spawn(move || {
        std::thread::sleep(Duration::from_secs(5));
        archive_rotated(&log_dir, days);
        let meta = fs::read(report_dir.join("meta.json")).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok());
        match meta {
            Some(m) => {
                if let Err(e) = snapshot(&report_dir, &m, &log_dir, now_secs()) {
                    log!("daemon lines for {} failed: {}", report_dir.display(), e);
                }
            }
            None => log!("daemon lines for {} failed: meta.json unreadable", report_dir.display()),
        }
    });
}

pub fn archive_loop(log_dir: PathBuf, days: f64) {
    std::thread::spawn(move || loop {
        archive_rotated(&log_dir, days);
        std::thread::sleep(Duration::from_secs(300));
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn word_boundaries() {
        assert!(has_word("[12:00:00] BRIDGE token Bob joined", "Bob"));
        assert!(!has_word("[12:00:00] Bobby joined", "Bob"));
        assert!(has_word("x Bobby Bob", "Bob"));
        assert!(!has_word("_Bob", "Bob"));
        assert!(has_word("(Bob)", "Bob"));
    }

    #[test]
    fn snapshot_follows_sessions() {
        let d = std::env::temp_dir().join(format!("t2r-dl-{}", std::process::id()));
        let logs = d.join("logs");
        let rep = d.join("rep");
        fs::create_dir_all(&logs).unwrap();
        fs::create_dir_all(&rep).unwrap();
        fs::write(
            logs.join("daemon.log"),
            "[10:00:00] BRIDGE ('1.2.3.4', 5000) token Bob\n[10:00:01] MATCHMAKE ('1.2.3.4', 5000) ok match 0badf00d\n[10:00:02] host view match 0badf00d down\n[10:00:03] PING ('1.2.3.4', 5000)\n[10:00:04] unrelated ('9.9.9.9', 1)\n",
        )
        .unwrap();
        let meta = serde_json::json!({"name": "Bob", "steamId": ""});
        let n = snapshot(&rep, &meta, &logs, now_secs()).unwrap();
        assert_eq!(n, 3);
        let t = fs::read_to_string(rep.join("files").join("daemon-lines.txt")).unwrap();
        assert!(t.starts_with("# daemon lines for 'Bob' (steam -), 1 file(s) from the last 6 h; 1 session address(es), match ids: 0badf00d\n"));
        assert!(t.contains("('IP', 5000)") && !t.contains("PING") && !t.contains("unrelated"));
        let _ = fs::remove_dir_all(&d);
    }
}
