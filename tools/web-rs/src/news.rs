use crate::util::{is_digits, local, local_stamp, now, py_eq, py_space, py_str, py_strip, py_truthy, take_chars};
use serde_json::ser::{Formatter, PrettyFormatter, Serializer};
use serde_json::{json, Map, Value};
use std::fs;
use std::io::{self, Write};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

const NEWS_KEEP: [&str; 11] = ["id", "date", "build", "kind", "headline", "title", "body", "text", "sections", "blocks", "big"];
pub const KINDS: [&str; 4] = ["build", "hotfix", "patch", "update"];
const SEC_KEYS: [&str; 6] = ["new", "changed", "why", "watch", "known", "fixed"];
const BLOCK_TYPES: [&str; 6] = ["heading", "text", "note", "media", "stats", "sections"];
const KEEP_BACKUPS: usize = 50;
const MAX_BLOCKS: usize = 80;
const MAX_POST_BYTES: usize = 120_000;
const MAX_MD: usize = 10_000;

type StatKey = Option<(i64, i64, u64, u64)>;
type BigIds = Arc<Vec<(String, bool)>>;

fn stat_key(p: &Path) -> StatKey {
    fs::metadata(p).ok().map(|m| (m.mtime(), m.mtime_nsec(), m.size(), m.ino()))
}

struct Cached {
    key: (StatKey, StatKey),
    bytes: Arc<Vec<u8>>,
    big: BigIds,
}

pub struct News {
    path: PathBuf,
    big_path: PathBuf,
    cache: Mutex<Option<Cached>>,
    write: Mutex<()>,
}

pub enum Fail {
    Invalid(String),
    Io(&'static str),
    Corrupt,
}

fn invalid<T>(s: String) -> Result<T, Fail> {
    Err(Fail::Invalid(s))
}

impl News {
    pub fn new(path: PathBuf, site: &Path) -> News {
        News { path, big_path: site.join("news-big.json"), cache: Mutex::new(None), write: Mutex::new(()) }
    }

    fn compute(&self) -> Cached {
        let key = (stat_key(&self.path), stat_key(&self.big_path));
        let items = fs::read(&self.path).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok());
        let big_ids: Vec<Value> = match fs::read(&self.big_path).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok()) {
            Some(Value::Object(m)) => match m.get("ids") {
                Some(Value::Array(a)) => a.clone(),
                _ => Vec::new(),
            },
            _ => Vec::new(),
        };
        let mut out: Vec<Value> = Vec::new();
        let mut big = Vec::new();
        if let Some(Value::Array(items)) = items {
            for n in items.iter() {
                if out.len() >= 60 {
                    break;
                }
                if let Value::Object(n) = n {
                    let mut o = Map::new();
                    for k in NEWS_KEEP {
                        if let Some(v) = n.get(k) {
                            o.insert(k.into(), v.clone());
                        }
                    }
                    let b = match n.get("big") {
                        Some(v) => py_truthy(v),
                        None => {
                            let id = n.get("id").unwrap_or(&Value::Null);
                            big_ids.iter().any(|x| py_eq(x, id))
                        }
                    };
                    o.insert("big".into(), Value::Bool(b));
                    big.push((py_str(n.get("id").unwrap_or(&Value::Null)), b));
                    out.push(Value::Object(o));
                }
            }
        }
        let bytes = serde_json::to_vec(&out).unwrap_or_else(|_| b"[]".to_vec());
        Cached { key, bytes: Arc::new(bytes), big: Arc::new(big) }
    }

    fn cached(&self) -> (Arc<Vec<u8>>, BigIds) {
        let key = (stat_key(&self.path), stat_key(&self.big_path));
        let mut g = self.cache.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(c) = &*g {
            if c.key == key {
                return (c.bytes.clone(), c.big.clone());
            }
        }
        let c = self.compute();
        let r = (c.bytes.clone(), c.big.clone());
        *g = Some(c);
        r
    }

    pub fn public_bytes(&self) -> Arc<Vec<u8>> {
        self.cached().0
    }

    pub fn raw(&self) -> Value {
        let items = match load(&self.path) {
            Ok(v) => v,
            Err(_) => return Value::Array(Vec::new()),
        };
        let big = self.cached().1;
        let mut out = Vec::new();
        for n in items {
            if let Value::Object(mut n) = n {
                let id = py_str(n.get("id").unwrap_or(&Value::Null));
                let b = match big.iter().rev().find(|(k, _)| *k == id) {
                    Some((_, b)) => *b,
                    None => n.get("big").is_some_and(py_truthy),
                };
                n.insert("big".into(), Value::Bool(b));
                out.push(Value::Object(n));
            }
        }
        Value::Array(out)
    }

    pub fn apply(&self, action: Option<&Value>, who: &str, pid: Option<&Value>, post: Option<&Value>, big: Option<&Value>) -> Result<(String, String), Fail> {
        let _g = self.write.lock().unwrap_or_else(|e| e.into_inner());
        let mut items = load(&self.path)?;
        let null = Value::Null;
        let pidv = pid.unwrap_or(&null);
        let (what, rid) = match action.and_then(Value::as_str) {
            Some("create") => {
                let c = clean_post(post.unwrap_or(&null))?;
                let nid = next_id(&items);
                let all_int = items.iter().all(|n| match n {
                    Value::Object(m) => match m.get("id") {
                        None => true,
                        Some(Value::Bool(_)) => true,
                        Some(Value::Number(x)) => !x.is_f64(),
                        Some(_) => false,
                    },
                    _ => true,
                });
                let mut ent = Map::new();
                ent.insert("id".into(), if all_int { nid.parse::<u64>().map(Value::from).unwrap_or_else(|_| Value::from(nid.clone())) } else { Value::from(nid.clone()) });
                let headline = c.get("headline").and_then(Value::as_str).unwrap_or("").to_string();
                for (k, v) in c {
                    ent.insert(k, v);
                }
                items.insert(0, Value::Object(ent));
                (format!("create id={} '{}'", nid, take_chars(&headline, 60)), nid)
            }
            Some("update") => {
                let i = find(&items, pidv).ok_or_else(|| Fail::Invalid("no such post".into()))?;
                let c = clean_post(post.unwrap_or(&null))?;
                let headline = c.get("headline").and_then(Value::as_str).unwrap_or("").to_string();
                let no_blocks = c.get("blocks").and_then(Value::as_array).is_none_or(|a| a.is_empty());
                let empty_pl = c.get("pl").map(|p| p.as_object().is_some_and(|o| o.is_empty()));
                if let Value::Object(new) = &mut items[i] {
                    for k in ["sections", "blocks", "pl"] {
                        new.shift_remove(k);
                    }
                    for (k, v) in c {
                        new.insert(k, v);
                    }
                    if no_blocks {
                        new.shift_remove("blocks");
                    }
                    if empty_pl == Some(true) {
                        new.shift_remove("pl");
                    }
                }
                (format!("update id={} '{}'", py_str(pidv), take_chars(&headline, 60)), py_str(pidv))
            }
            Some("big") => {
                let i = find(&items, pidv).ok_or_else(|| Fail::Invalid("no such post".into()))?;
                let on = big.is_some_and(py_truthy);
                if let Value::Object(m) = &mut items[i] {
                    m.insert("big".into(), Value::Bool(on));
                }
                (format!("big id={} {}", py_str(pidv), if on { "on" } else { "off" }), py_str(pidv))
            }
            Some("delete") => {
                let i = find(&items, pidv).ok_or_else(|| Fail::Invalid("no such post".into()))?;
                let gone = items.remove(i);
                let tp = self.path.parent().map(Path::to_path_buf).unwrap_or_default().join("news-trash.json");
                let mut trash = load(&tp).unwrap_or_default();
                let mut g = match gone {
                    Value::Object(m) => m,
                    _ => Map::new(),
                };
                let headline = py_str(g.get("headline").unwrap_or(&Value::from("")));
                g.insert("deletedAt".into(), Value::from(local_stamp(now() as i64)));
                g.insert("deletedBy".into(), Value::from(who));
                trash.insert(0, Value::Object(g));
                atomic(&tp, &trash).map_err(|_| Fail::Io("OSError"))?;
                (format!("delete id={} '{}' (to news-trash.json)", py_str(pidv), take_chars(&headline, 60)), py_str(pidv))
            }
            _ => return invalid("unknown action".into()),
        };
        backup(&self.path).map_err(|_| Fail::Io("OSError"))?;
        atomic(&self.path, &items).map_err(|_| Fail::Io("OSError"))?;
        Ok((rid, format!("{} by {}", what, who)))
    }
}

fn load(path: &Path) -> Result<Vec<Value>, Fail> {
    let b = match fs::read(path) {
        Ok(b) => b,
        Err(e) if e.kind() == io::ErrorKind::NotFound => return Ok(Vec::new()),
        Err(_) => return Err(Fail::Io("OSError")),
    };
    match serde_json::from_slice::<Value>(&b) {
        Ok(Value::Array(a)) => Ok(a),
        Ok(_) => invalid("news file is not a list".into()),
        Err(_) => Err(Fail::Corrupt),
    }
}

fn find(items: &[Value], pid: &Value) -> Option<usize> {
    let want = py_str(pid);
    items.iter().position(|n| match n {
        Value::Object(m) => py_str(m.get("id").unwrap_or(&Value::Null)) == want,
        _ => false,
    })
}

fn next_id(items: &[Value]) -> String {
    let mut best: Option<u128> = None;
    for n in items {
        if let Value::Object(m) = n {
            let s = py_str(m.get("id").unwrap_or(&Value::from("")));
            if is_digits(&s) {
                if let Ok(v) = s.parse::<u128>() {
                    best = Some(best.map_or(v, |b| b.max(v)));
                }
            }
        }
    }
    best.map_or(1, |b| b + 1).to_string()
}

struct PyFormatter;

impl Formatter for PyFormatter {
    fn begin_array_value<W: ?Sized + Write>(&mut self, w: &mut W, first: bool) -> io::Result<()> {
        if first {
            Ok(())
        } else {
            w.write_all(b", ")
        }
    }

    fn begin_object_key<W: ?Sized + Write>(&mut self, w: &mut W, first: bool) -> io::Result<()> {
        if first {
            Ok(())
        } else {
            w.write_all(b", ")
        }
    }

    fn begin_object_value<W: ?Sized + Write>(&mut self, w: &mut W) -> io::Result<()> {
        w.write_all(b": ")
    }
}

fn py_dumps_len(v: &Value) -> usize {
    let mut buf = Vec::new();
    let mut ser = Serializer::with_formatter(&mut buf, PyFormatter);
    match serde::Serialize::serialize(v, &mut ser) {
        Ok(()) => buf.len(),
        Err(_) => usize::MAX,
    }
}

pub fn pretty(v: &impl serde::Serialize) -> io::Result<Vec<u8>> {
    let mut buf = Vec::new();
    let mut ser = Serializer::with_formatter(&mut buf, PrettyFormatter::with_indent(b"  "));
    v.serialize(&mut ser).map_err(io::Error::other)?;
    buf.push(b'\n');
    Ok(buf)
}

fn atomic(path: &Path, items: &[Value]) -> io::Result<()> {
    let data = pretty(&items)?;
    let tmp = PathBuf::from(format!("{}.tmp.{}", path.display(), std::process::id()));
    let res = (|| {
        let mut f = fs::File::create(&tmp)?;
        f.write_all(&data)?;
        f.flush()?;
        f.sync_all()?;
        fs::rename(&tmp, path)
    })();
    if res.is_err() {
        let _ = fs::remove_file(&tmp);
    }
    res
}

fn backup(path: &Path) -> io::Result<()> {
    if !path.is_file() {
        return Ok(());
    }
    let d = path.parent().map(Path::to_path_buf).unwrap_or_default().join("news-backups");
    fs::create_dir_all(&d)?;
    let t = now();
    let l = local(t.floor() as i64);
    let ms = ((t - t.floor()) * 1000.0) as u32;
    let name = format!("news-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.json", l.year, l.mon, l.day, l.hour, l.min, l.sec, ms.min(999));
    fs::copy(path, d.join(name))?;
    let mut old: Vec<String> = fs::read_dir(&d)?
        .filter_map(|e| e.ok())
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|f| f.starts_with("news-") && f.ends_with(".json"))
        .collect();
    old.sort();
    if old.len() > KEEP_BACKUPS {
        for f in &old[..old.len() - KEEP_BACKUPS] {
            let _ = fs::remove_file(d.join(f));
        }
    }
    Ok(())
}

fn s_field(v: Option<&Value>, what: &str, maxlen: usize, required: bool) -> Result<String, Fail> {
    let s = match v {
        None | Some(Value::Null) => "",
        Some(Value::String(s)) => s.as_str(),
        Some(_) => return invalid(format!("{} must be text", what)),
    };
    if s.chars().count() > maxlen {
        return invalid(format!("{} is too long (max {})", what, maxlen));
    }
    if s.contains('\0') {
        return invalid(format!("{} has a control character", what));
    }
    if required && s.chars().all(py_space) {
        return invalid(format!("{} is required", what));
    }
    Ok(s.to_string())
}

fn list<'a>(v: Option<&'a Value>, what: &str, maxn: usize) -> Result<&'a [Value], Fail> {
    match v {
        None | Some(Value::Null) => Ok(&[]),
        Some(Value::Array(a)) => {
            if a.len() > maxn {
                invalid(format!("{} has too many entries (max {})", what, maxn))
            } else {
                Ok(a.as_slice())
            }
        }
        Some(_) => invalid(format!("{} must be a list", what)),
    }
}

fn clean_sections(v: Option<&Value>, what: &str) -> Result<Map<String, Value>, Fail> {
    let secs = match v {
        None | Some(Value::Null) => return Ok(Map::new()),
        Some(Value::Object(m)) => m,
        Some(_) => return invalid(format!("{} must be an object", what)),
    };
    let mut out = Map::new();
    for (k, lines) in secs {
        if !SEC_KEYS.contains(&k.as_str()) {
            return invalid(format!("{}: unknown section '{}'", what, take_chars(k, 20)));
        }
        let mut ls = Vec::new();
        for (i, x) in list(Some(lines), &format!("{}.{}", what, k), 60)?.iter().enumerate() {
            let s = s_field(Some(x), &format!("{}.{} line {}", what, k, i + 1), 2000, false)?;
            if !py_strip(&s).is_empty() {
                ls.push(Value::from(s));
            }
        }
        if !ls.is_empty() {
            out.insert(k.clone(), Value::Array(ls));
        }
    }
    Ok(out)
}

fn media_ok(src: &str) -> bool {
    let s = src.strip_prefix("media/").unwrap_or(src);
    let (stem, ext) = match s.rsplit_once('.') {
        Some(x) => x,
        None => return false,
    };
    let ext = ext.to_ascii_lowercase();
    if !["gif", "webp", "png", "jpg", "jpeg", "mp4", "webm"].contains(&ext.as_str()) {
        return false;
    }
    let b = stem.as_bytes();
    let head = |c: u8| c.is_ascii_alphanumeric() || c == b'_' || c == b'-';
    !b.is_empty() && b.len() <= 81 && head(b[0]) && b[1..].iter().all(|c| head(*c) || *c == b'.')
}

fn icon_ok(icon: &str) -> bool {
    let stem = match icon.strip_suffix(".png") {
        Some(s) => s,
        None => return false,
    };
    stem.split('/').all(|seg| !seg.is_empty() && seg.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_' || c == b'-'))
}

fn clean_block(b: &Value, i: usize) -> Result<Value, Fail> {
    let w = format!("block {}", i + 1);
    let b = match b {
        Value::Object(m) => m,
        _ => return invalid(format!("{} must be an object", w)),
    };
    let t = b.get("type").and_then(Value::as_str).filter(|t| BLOCK_TYPES.contains(t));
    let t = match t {
        Some(t) => t,
        None => return invalid(format!("{}: unknown type '{}'", w, take_chars(&py_str(b.get("type").unwrap_or(&Value::Null)), 20))),
    };
    Ok(match t {
        "heading" => json!({"type": t, "text": s_field(b.get("text"), &format!("{} text", w), 200, true)?}),
        "text" => json!({"type": t, "md": s_field(b.get("md"), &format!("{} text", w), MAX_MD, true)?}),
        "note" => {
            let title = s_field(b.get("title"), &format!("{} title", w), 120, false)?;
            json!({"type": t, "title": title, "md": s_field(b.get("md"), &format!("{} text", w), MAX_MD, true)?})
        }
        "sections" => json!({"type": t, "sections": clean_sections(b.get("sections"), &w)?}),
        "media" => {
            let mut items = Vec::new();
            for it in list(b.get("items"), &format!("{} items", w), 3)? {
                let it = match it {
                    Value::Object(m) => m,
                    _ => return invalid(format!("{}: media item must be an object", w)),
                };
                let src = s_field(it.get("src"), &format!("{} file name", w), 90, true)?;
                if !media_ok(&src) {
                    return invalid(format!("{}: bad media file name '{}'", w, take_chars(&src, 40)));
                }
                let caption = s_field(it.get("caption"), &format!("{} caption", w), 200, false)?;
                items.push(json!({"src": src, "caption": caption}));
            }
            if items.is_empty() {
                return invalid(format!("{}: media needs at least one file", w));
            }
            json!({"type": t, "items": items})
        }
        _ => {
            let mut cols = Vec::new();
            for c in list(b.get("cols"), &format!("{} columns", w), 8)? {
                cols.push(s_field(Some(c), &format!("{} column", w), 40, false)?);
            }
            if cols.is_empty() {
                return invalid(format!("{}: stats needs at least one column", w));
            }
            let mut rows = Vec::new();
            for r in list(b.get("rows"), &format!("{} rows", w), 80)? {
                let r = match r {
                    Value::Object(m) => m,
                    _ => return invalid(format!("{}: row must be an object", w)),
                };
                let icon = s_field(r.get("icon"), &format!("{} icon", w), 80, false)?;
                if !icon.is_empty() && !icon_ok(&icon) {
                    return invalid(format!("{}: bad icon '{}'", w, take_chars(&icon, 40)));
                }
                let mut vals = Vec::new();
                for v in list(r.get("values"), &format!("{} values", w), 8)? {
                    vals.push(s_field(Some(v), &format!("{} value", w), 60, false)?);
                }
                if vals.len() > cols.len() {
                    return invalid(format!("{}: a row has more values than columns", w));
                }
                let name = s_field(r.get("name"), &format!("{} row name", w), 80, false)?;
                vals.resize(cols.len(), String::new());
                rows.push(json!({"icon": icon, "name": name, "values": vals}));
            }
            json!({"type": t, "cols": cols, "rows": rows})
        }
    })
}

fn clean_blocks(v: Option<&Value>, what: &str) -> Result<Vec<Value>, Fail> {
    list(v, what, MAX_BLOCKS)?.iter().enumerate().map(|(i, b)| clean_block(b, i)).collect()
}

fn real_date(d: &str) -> bool {
    let b = d.as_bytes();
    if b.len() != 10 || b[4] != b'-' || b[7] != b'-' {
        return false;
    }
    let (y, m, dd) = (&d[..4], &d[5..7], &d[8..]);
    is_digits(y) && is_digits(m) && is_digits(dd)
}

fn calendar_ok(d: &str) -> bool {
    let y: i64 = d[..4].parse().unwrap_or(0);
    let m: u32 = d[5..7].parse().unwrap_or(0);
    let dd: u32 = d[8..].parse().unwrap_or(0);
    if y < 1 || !(1..=12).contains(&m) || dd < 1 {
        return false;
    }
    let leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    let days = [31, if leap { 29 } else { 28 }, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31][(m - 1) as usize];
    dd <= days
}

fn clean_post(p: &Value) -> Result<Map<String, Value>, Fail> {
    let p = match p {
        Value::Object(m) => m,
        _ => return invalid("post must be an object".into()),
    };
    let mut out = Map::new();
    out.insert("headline".into(), Value::from(s_field(p.get("headline"), "headline", 200, true)?));
    let date = s_field(p.get("date"), "date", 10, true)?;
    if !real_date(&date) {
        return invalid("date must be YYYY-MM-DD".into());
    }
    if !calendar_ok(&date) {
        return invalid("date is not a real date".into());
    }
    out.insert("date".into(), Value::from(date));
    out.insert("build".into(), Value::from(s_field(p.get("build"), "build", 16, false)?));
    let kind = match p.get("kind") {
        Some(v) if py_truthy(v) => v,
        _ => &Value::Null,
    };
    let kind = match kind {
        Value::Null => "build",
        Value::String(s) if KINDS.contains(&s.as_str()) => s.as_str(),
        _ => return invalid(format!("kind must be one of {}", KINDS.join(", "))),
    };
    out.insert("kind".into(), Value::from(kind));
    out.insert("big".into(), Value::Bool(p.get("big").is_some_and(py_truthy)));
    out.insert("sections".into(), Value::Object(clean_sections(p.get("sections"), "sections")?));
    out.insert("blocks".into(), Value::Array(clean_blocks(p.get("blocks"), "blocks")?));
    match p.get("pl") {
        None | Some(Value::Null) => {}
        Some(Value::Object(pl)) => {
            let mut o = Map::new();
            let h = s_field(pl.get("headline"), "pl headline", 200, false)?;
            if !py_strip(&h).is_empty() {
                o.insert("headline".into(), Value::from(h));
            }
            let s = clean_sections(pl.get("sections"), "pl sections")?;
            if !s.is_empty() {
                o.insert("sections".into(), Value::Object(s));
            }
            let b = clean_blocks(pl.get("blocks"), "pl blocks")?;
            if !b.is_empty() {
                o.insert("blocks".into(), Value::Array(b));
            }
            out.insert("pl".into(), Value::Object(o));
        }
        Some(_) => return invalid("pl must be an object".into()),
    }
    if py_dumps_len(&Value::Object(out.clone())) > MAX_POST_BYTES {
        return invalid("post is too large".into());
    }
    Ok(out)
}

pub fn asset_files(site: &Path, sub: &str, exts: &[&str]) -> Vec<String> {
    let d = site.join("assets").join("news").join(sub);
    let mut v: Vec<String> = match fs::read_dir(&d) {
        Ok(rd) => rd
            .filter_map(|e| e.ok())
            .filter_map(|e| e.file_name().into_string().ok())
            .filter(|f| {
                let low = f.to_lowercase();
                exts.iter().any(|x| low.ends_with(x)) && !f.starts_with('.') && !f.starts_with('_')
            })
            .collect(),
        Err(_) => Vec::new(),
    };
    v.sort();
    v
}
