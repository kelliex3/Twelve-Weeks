//! Custom player names + clan tags.
//!
//! The Rust version of names.py: validation, the `names` table and the 0xD1 reply text.
use rusqlite::{params, Connection, OptionalExtension};

use crate::py::strip;

pub const NAME_MAX: usize = 15;
pub const TAG_MAX: usize = 5;

/// What `save` returns: `(name, tag, error)` exactly like the Python triple.
pub type Saved = (Option<String>, Option<String>, Option<String>);

// py: names.py:19 token_id
pub fn token_id(token: &str) -> String {
    let t = token.to_lowercase();
    let ch: Vec<char> = t.chars().collect();
    if ch.len() == 64 {
        ch[40..64].iter().collect()
    } else {
        t
    }
}

fn name_ok(s: &str) -> bool {
    let mut it = s.chars();
    match it.next() {
        Some(c) if c.is_ascii_alphanumeric() => {}
        _ => return false,
    }
    it.all(|c| c.is_ascii_alphanumeric() || c == ' ' || c == '_' || c == '.' || c == '-')
}

fn tag_ok(s: &str) -> bool {
    !s.is_empty() && s.chars().all(|c| c.is_ascii_alphanumeric())
}

/// `re.sub(r" +", " ", s)`
fn collapse_spaces(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut prev = false;
    for c in s.chars() {
        if c == ' ' {
            if !prev {
                out.push(' ');
            }
            prev = true;
        } else {
            out.push(c);
            prev = false;
        }
    }
    out
}

// py: names.py:25 clean
/// -> `Ok((name, tag))` (both empty = clear the custom name; the tag is upper-cased) or `Err(error text)`.
pub fn clean(name: &str, tag: &str) -> Result<(String, String), String> {
    let name = collapse_spaces(strip(name));
    let tag = strip(tag).to_uppercase();
    if name.is_empty() && tag.is_empty() {
        return Ok((String::new(), String::new()));
    }
    if name.is_empty() {
        return Err("name needed".to_string());
    }
    if name.chars().count() > NAME_MAX {
        return Err(format!("name too long (max {})", NAME_MAX));
    }
    if !name_ok(&name) {
        return Err("name: letters, digits, space . _ - only".to_string());
    }
    if !tag.is_empty() && tag.chars().count() > TAG_MAX {
        return Err(format!("tag too long (max {})", TAG_MAX));
    }
    if !tag.is_empty() && !tag_ok(&tag) {
        return Err("tag: letters and digits only".to_string());
    }
    Ok((name, tag))
}

// py: names.py:41 init
pub fn init(c: &Connection) -> rusqlite::Result<()> {
    c.execute("CREATE TABLE IF NOT EXISTS names(token_id TEXT PRIMARY KEY, name TEXT, tag TEXT, updated REAL)", [])?;
    Ok(())
}

// py: names.py:45 load
pub fn load(c: &Connection, tid: &str) -> rusqlite::Result<(String, String)> {
    init(c)?;
    let r: Option<(Option<String>, Option<String>)> =
        c.query_row("SELECT name,tag FROM names WHERE token_id=?", params![tid], |r| Ok((r.get(0)?, r.get(1)?))).optional()?;
    Ok(match r {
        Some((n, t)) => (n.unwrap_or_default(), t.unwrap_or_default()),
        None => (String::new(), String::new()),
    })
}

// py: names.py:51 save
/// Validated store (`time.time()` = `crate::clock::now()`). Returns `(name, tag, error)` like Python; an empty name + tag deletes the row.
/// `taken(name)` = another player shows that name.
pub fn save(c: &Connection, tid: &str, name: &str, tag: &str, taken: Option<&dyn Fn(&str) -> bool>) -> rusqlite::Result<Saved> {
    save_at(c, tid, name, tag, taken, crate::clock::now())
}

/// `save` with an explicit clock value (the differential test freezes it).
pub fn save_at(c: &Connection, tid: &str, name: &str, tag: &str, taken: Option<&dyn Fn(&str) -> bool>, now: f64) -> rusqlite::Result<Saved> {
    let (name, tag) = match clean(name, tag) {
        Ok(x) => x,
        Err(e) => return Ok((None, None, Some(e))),
    };
    if !name.is_empty() {
        if let Some(t) = taken {
            if t(&name) {
                return Ok((None, None, Some("name taken".to_string())));
            }
        }
    }
    init(c)?;
    if name.is_empty() {
        c.execute("DELETE FROM names WHERE token_id=?", params![tid])?;
    } else {
        c.execute(
            "INSERT INTO names(token_id,name,tag,updated) VALUES(?,?,?,?) \
             ON CONFLICT(token_id) DO UPDATE SET name=excluded.name,tag=excluded.tag,updated=excluded.updated",
            params![tid, name, tag, now],
        )?;
    }
    Ok((Some(name), Some(tag), None))
}

// py: names.py:67 display
pub fn display(name: &str, tag: &str) -> String {
    if tag.is_empty() {
        name.to_string()
    } else {
        format!("[{}] {}", tag, name)
    }
}

// py: names.py:71 state_text
/// Reply text of 0xD1: ASCII "k=v ..." (spaces in the name -> _).
pub fn state_text(name: &str, tag: &str, err: Option<&str>) -> String {
    let err = err.filter(|e| !e.is_empty()); // Python truthiness: "" = no error
    let n = name.replace(' ', "_");
    let s = format!("ok={} name={} tag={}", if err.is_some() { 0 } else { 1 }, if n.is_empty() { "-" } else { &n }, if tag.is_empty() { "-" } else { tag });
    match err {
        Some(e) => format!("{} err={}", s, e.replace(' ', "_")),
        None => s,
    }
}

// ---------------------------------------------------------------- differential test
// verify/py/small_harness.py is the Python side: JSON lines {"m":"names","op":..,"t": frozen time, ..}.
pub fn selftest(input: &mut dyn std::io::BufRead, out: &mut dyn std::io::Write) -> i32 {
    use crate::py::dumps_compact;
    use serde_json::{json, Value};
    let path = crate::selftest::tmp_db("names");
    let c = match Connection::open(&path) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("db: {}", e);
            return 2;
        }
    };
    let mut line = String::new();
    loop {
        line.clear();
        match input.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let Ok(v) = serde_json::from_str::<Value>(line.trim()) else { continue };
        if v["m"].as_str() != Some("names") {
            continue;
        }
        let s = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
        let t = v.get("t").and_then(|x| x.as_f64()).unwrap_or(0.0);
        let res: Value = match v["op"].as_str().unwrap_or("") {
            "token_id" => json!(token_id(&s("token"))),
            "clean" => match clean(&s("name"), &s("tag")) {
                Ok((n, tg)) => json!([n, tg, null]),
                Err(e) => json!([null, null, e]),
            },
            "load" => match load(&c, &s("tid")) {
                Ok((n, tg)) => json!([n, tg]),
                Err(e) => json!(format!("ERR {}", e)),
            },
            "save" => {
                let taken_list: Vec<String> =
                    v.get("taken").and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default();
                let has = v.get("taken").map(|x| !x.is_null()).unwrap_or(false);
                let f = |n: &str| taken_list.iter().any(|x| x == n);
                let r = save_at(&c, &s("tid"), &s("name"), &s("tag"), if has { Some(&f) } else { None }, t);
                match r {
                    Ok((n, tg, e)) => json!([n, tg, e]),
                    Err(e) => json!(format!("ERR {}", e)),
                }
            }
            "display" => json!(display(&s("name"), &s("tag"))),
            "state_text" => {
                let e = v.get("err").and_then(|x| x.as_str());
                json!(state_text(&s("name"), &s("tag"), e))
            }
            _ => json!("?"),
        };
        let _ = writeln!(out, "{}", dumps_compact(&res));
    }
    if let Ok(mut stmt) = c.prepare("SELECT token_id,name,tag,updated FROM names ORDER BY token_id") {
        let rows = stmt.query_map([], |r| {
            Ok(json!([r.get::<_, Option<String>>(0)?, r.get::<_, Option<String>>(1)?, r.get::<_, Option<String>>(2)?, r.get::<_, Option<f64>>(3)?]))
        });
        if let Ok(rows) = rows {
            for r in rows.flatten() {
                let _ = writeln!(out, "ROW {}", dumps_compact(&r));
            }
        }
    }
    0
}
