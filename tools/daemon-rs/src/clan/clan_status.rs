//! clan_status.py: clan member STATUS UPDATES (who changed state since the player last looked). Read-only observer of the story state;
//! events and the snapshot live in st["ch"]["stat"] = {"snap": {id: code}, "ev": [event, ...]}.
use crate::clan::clan::{int_of, pystr, truthy};
use crate::clan::data;
use crate::clan::{clan_chronicle as ch, clan_continuity as cont, ext_clan_chronicle as ext, ext_clan_perks as extp};
use regex::Regex;
use serde_json::{json, Map, Value};
use std::collections::HashMap;
use std::sync::LazyLock;

pub const HEALTHY: i64 = 0;
pub const HUNGRY: i64 = 1;
pub const SAD: i64 = 2;
pub const SICK: i64 = 3;
pub const DEAD: i64 = 4;
pub const INJURED: i64 = 5;
pub const RECOVERED: i64 = 6;
pub const NEW: i64 = 7;
pub const DEPRESSED: i64 = 8;
pub const SHOCK: i64 = 9;
pub const NAMES: [&str; 10] = ["healthy", "hungry", "sad", "sick", "dead", "injured", "recovered", "new", "depressed", "shock"];
pub const EXPR: [&str; 10] = ["neutral", "hungry", "sad", "sick", "dead", "sick", "neutral", "neutral", "depressed", "shock"];
pub const DEP_AT: i64 = 4;
pub const EV_DAYS: i64 = 3;
pub const EV_MAX: usize = 6;

// py: clan_status.py:20 LINES
fn lines_row(lang: &str, code: i64) -> Option<&'static Value> {
    data::dict_get_int(data::t("clan_status", "LINES").get(lang)?, code)
}

// py: clan_status.py:36 enabled
pub fn enabled() -> bool {
    let v = std::env::var("T2R_CLAN_STATUS").unwrap_or_default();
    !matches!(crate::py::strip(&v).to_lowercase().as_str(), "0" | "off" | "no")
}

fn vget<'a>(v: &'a Value, k: &str) -> Option<&'a Value> {
    v.get(k)
}

/// `(x or {})`: the value when it is a truthy object, else an empty one.
fn or_obj<'a>(v: Option<&'a Value>) -> Option<&'a Value> {
    v.filter(|x| truthy(x))
}

// py: clan_status.py:46 code_of
/// Status code of a living member: sick > hungry > sad (grief memory still running) > injured > healthy.
pub fn code_of(st: &Value, p: &Value) -> i64 {
    let s = p.get("s").map(pystr).unwrap_or_else(|| "h".to_string());
    let day = st.get("day").map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
    let key = p.get("i").map(pystr).unwrap_or_default();
    let ch = or_obj(st.get("ch"));
    let mem = ch.and_then(|c| or_obj(c.get("mem"))).and_then(|m| or_obj(m.get(&key)));
    let dep = ch.and_then(|c| or_obj(c.get("dep"))).and_then(|m| or_obj(m.get(&key)));
    let dep_n = dep.and_then(|d| d.get("n")).map(int_of).unwrap_or(0);
    if dep_n >= DEP_AT {
        return DEPRESSED;
    }
    let g0 = mem.and_then(|m| or_obj(m.get("grief")));
    if let Some(g) = g0 {
        if g.get("d").map(int_of).unwrap_or(-1) == day {
            return SHOCK;
        }
    }
    if s == "s" {
        return SICK;
    }
    if s == "u" {
        return HUNGRY;
    }
    if let Some(g) = g0 {
        if g.get("until").map(int_of).unwrap_or(-1) >= day {
            return SAD;
        }
    }
    if dep.and_then(|d| d.get("rec")).map(truthy).unwrap_or(false) && dep_n > 0 {
        return SAD; // on the way back from depression
    }
    let inj = mem.and_then(|m| m.get("inj")).map(truthy).unwrap_or(false) || p.get("il").map(truthy).unwrap_or(false);
    if inj && p.get("r").map(pystr).as_deref() != Some("kid") {
        return INJURED;
    }
    HEALTHY
}

static LINE_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^(?:DAY|DZIEŃ) \d+: ([^.]*)\.\s*([\s\S]*)$").expect("LINE_RE"));

/// Python `str.capitalize()`.
fn capitalize(s: &str) -> String {
    let mut it = s.chars();
    match it.next() {
        Some(c) => c.to_uppercase().collect::<String>() + &it.as_str().to_lowercase(),
        None => String::new(),
    }
}

// py: clan_status.py:68 _line
/// The headline of a chronicle row 'DAY n: HEAD. Body' as the short story line ("" when the row has no headline).
fn line_of(row_text: &str, names: &[String]) -> String {
    let Some(m) = LINE_RE.captures(row_text) else { return String::new() };
    let head = m.get(1).map(|x| x.as_str()).unwrap_or("");
    if crate::py::strip(head).is_empty() {
        return String::new();
    }
    let t = capitalize(crate::py::strip(head));
    let t = cont::restore_case(&t, &cont::proper_nouns(names));
    t + "."
}

fn people_names(st: &Value, k: &str) -> Vec<String> {
    st.get(k).and_then(|v| v.as_array()).map(|a| a.iter().map(|x| x.get("n").map(pystr).unwrap_or_default()).collect()).unwrap_or_default()
}

// py: clan_status.py:85 observe
/// Compare every member with the snapshot of the last call; new events go to st["ch"]["stat"]["ev"]. The first call only seeds the snapshot.
/// Returns the new events. `Err` = where Python raises (a member without "i" / "n").
pub fn observe(st: &mut Value, rows: &[Value]) -> Result<Vec<Value>, String> {
    if !enabled() || !truthy(st) {
        return Ok(vec![]);
    }
    if !st.is_object() {
        return Err("AttributeError: setdefault".into());
    }
    if !st.get("ch").map(|c| c.is_object()).unwrap_or(false) {
        if st.get("ch").is_some() && !st["ch"].is_null() {
            return Err("AttributeError: setdefault on non-dict".into());
        }
        st["ch"] = json!({});
    }
    let _ = ext::loot::update_moods(st); // no-op when the chronicle already did it today; the mood chain never breaks the status events
    if !st["ch"].get("stat").map(|x| x.is_object()).unwrap_or(false) {
        st["ch"]["stat"] = json!({"snap": {}, "ev": []});
    }
    let day = st.get("day").map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
    let seeded = st["ch"]["stat"].get("seeded").map(truthy).unwrap_or(false);
    let mut names: HashMap<String, String> = HashMap::new();
    for r in rows {
        if let Some(ns) = r.get("names").filter(|x| truthy(x)).and_then(|x| x.as_array()) {
            for n in ns {
                names.entry(pystr(n)).or_insert_with(|| r.get("text").filter(|x| truthy(x)).map(pystr).unwrap_or_default());
            }
        }
    }
    let mut all_names = people_names(st, "people");
    all_names.extend(people_names(st, "dead"));
    let people: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let dead: Vec<Value> = st.get("dead").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let mut new: Vec<Value> = Vec::new();
    let mut add = |p: &Value, c: i64, f: i64| -> Result<(), String> {
        let n = p.get("n").ok_or("KeyError: n")?;
        let ln = line_of(names.get(&pystr(n)).map(|s| s.as_str()).unwrap_or(""), &all_names);
        new.push(json!({"i": p.get("i").ok_or("KeyError: i")?, "n": n, "g": p.get("g").cloned().unwrap_or(json!("n")), "r": p.get("r").cloned().unwrap_or(json!("")), "c": c, "f": f, "d": day, "ln": ln}));
        Ok(())
    };
    let mut snap: Map<String, Value> = match st["ch"]["stat"].get("snap") {
        Some(Value::Object(m)) => m.clone(),
        _ => return Err("KeyError: snap".into()),
    };
    let mut alive: std::collections::HashSet<String> = std::collections::HashSet::new();
    for p in &people {
        let k = p.get("i").map(pystr).ok_or("KeyError: i")?;
        alive.insert(k.clone());
        let c = code_of(st, p);
        let was = snap.get(&k).map(int_of);
        if seeded {
            match was {
                None => add(p, NEW, 0)?,
                Some(w) if c != w => add(p, if c == HEALTHY { RECOVERED } else { c }, w)?,
                _ => {}
            }
        }
        snap.insert(k, json!(c));
    }
    for dd in &dead {
        let k = dd.get("i").map(pystr).ok_or("KeyError: i")?;
        if snap.get(&k).map(int_of) != Some(DEAD) {
            if seeded && snap.contains_key(&k) {
                let was = snap.get(&k).map(int_of).unwrap_or(0);
                add(&json!({"i": dd["i"], "n": dd.get("n").ok_or("KeyError: n")?, "g": dd.get("g").cloned().unwrap_or(json!("n")), "r": dd.get("r").cloned().unwrap_or(json!(""))}), DEAD, was)?;
            }
            snap.insert(k, json!(DEAD));
        }
    }
    let gone: Vec<String> = snap.iter().filter(|(k, v)| !alive.contains(*k) && int_of(v) != DEAD).map(|(k, _)| k.clone()).collect();
    for k in gone {
        snap.shift_remove(&k);
    }
    let dead_keys: Vec<String> = snap.iter().filter(|(_, v)| int_of(v) == DEAD).map(|(k, _)| k.clone()).collect();
    if dead_keys.len() > 30 {
        for k in &dead_keys[..dead_keys.len() - 30] {
            snap.shift_remove(k);
        }
    }
    let stat = &mut st["ch"]["stat"];
    stat["snap"] = Value::Object(snap);
    stat["seeded"] = json!(1);
    let old: Vec<Value> = stat.get("ev").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let mut ev: Vec<Value> = Vec::new();
    for e in old {
        if day - e.get("d").map(int_of).ok_or("KeyError: d")? < EV_DAYS {
            ev.push(e);
        }
    }
    ev.extend(new.iter().cloned());
    let keep_from = ev.len().saturating_sub(EV_MAX);
    stat["ev"] = Value::Array(ev.split_off(keep_from));
    Ok(new)
}

// py: clan_status.py:131 pending
/// The events still on offer (oldest first).
pub fn pending(st: &Value) -> Vec<Value> {
    if !enabled() || !truthy(st) {
        return vec![];
    }
    let day = st.get("day").map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
    let evs = st.get("ch").and_then(|c| or_obj(c.get("stat"))).and_then(|s| s.get("ev")).and_then(|e| e.as_array());
    evs.map(|a| a.iter().filter(|e| day - e.get("d").map(int_of).unwrap_or(0) < EV_DAYS).cloned().collect()).unwrap_or_default()
}

// py: clan_status.py:140 event_id
pub fn event_id(e: &Value) -> String {
    format!("{}:{}:{}", e.get("i").map(pystr).unwrap_or_default(), e.get("c").map(pystr).unwrap_or_default(), e.get("d").map(pystr).unwrap_or_default())
}

fn name_of(e: &Value) -> String {
    e.get("n").map(pystr).unwrap_or_default()
}

// py: clan_status.py:144 sex_of
pub fn sex_of(e: &Value) -> &'static str {
    match e.get("g").and_then(|g| g.as_str()) {
        Some("f") => "f",
        Some("m") => "m",
        _ => {
            if ch::h(&["sex".to_string(), name_of(e)]) & 1 != 0 {
                "f"
            } else {
                "m"
            }
        }
    }
}

// py: clan_status.py:149 look_of
pub fn look_of(e: &Value) -> i64 {
    (ch::h(&["model".to_string(), name_of(e)]) % 4) as i64
}

// py: clan_status.py:154 images
/// Per member + expression image ids {neutral, hungry, sad, sick, dead, depressed, shock} (the SAME head as the roster card).
pub fn images(e: &Value, fk: &str, st: Option<&Value>) -> Value {
    let members: Option<Vec<Value>> = st.filter(|s| truthy(s)).map(|s| {
        let mut v: Vec<Value> = s.get("people").and_then(|x| x.as_array()).cloned().unwrap_or_default();
        v.extend(s.get("dead").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default());
        v
    });
    let p = json!({"n": e["n"], "g": e.get("g").cloned().unwrap_or(json!("n")), "r": e.get("r").cloned().unwrap_or(json!(""))});
    let (sex, model) = ch::head_model(&p, fk, ch::PORTRAIT_MODELS, members.as_deref(), true);
    let mut m = Map::new();
    for x in ["neutral", "hungry", "sad", "sick", "dead", "depressed", "shock"] {
        m.insert(x.to_string(), json!(format!("{}/{}/{}/{}", fk, sex, model, ch::fit_expr(model, x))));
    }
    Value::Object(m)
}

// py: clan_status.py:160 story_line
pub fn story_line(e: &Value, lang: &str) -> String {
    if e.get("jl").map(truthy).unwrap_or(false) {
        // the journal's own sentence about the member: no two cards read the same
        let t = extp::journal::render(&e["jl"], lang);
        if !t.is_empty() {
            return t;
        }
    }
    if e.get("ln").map(truthy).unwrap_or(false) {
        return pystr(&e["ln"]);
    }
    let code = e.get("c").map(int_of).unwrap_or(0);
    let row = lines_row(if lang == "pl" { "pl" } else { "en" }, code);
    let t: String = match row {
        Some(Value::Array(a)) => a.get(if sex_of(e) == "f" { 1 } else { 0 }).map(pystr).unwrap_or_default(),
        Some(v) => pystr(v),
        None => "%s".to_string(),
    };
    t.replacen("%s", &name_of(e), 1)
}

// py: clan_status.py:183 public
/// For the launcher / clanfeed 'chron.status': the pending events with image ids, story line and the colour state names (a JSON array).
pub fn public(st: &Value, fk: &str, lang: &str) -> Result<Value, String> {
    let mut out = Vec::new();
    for e in pending(st) {
        let c = e.get("c").map(int_of).ok_or("KeyError: c")?;
        let idx = |v: i64| -> Result<usize, String> { if (0..10).contains(&v) { Ok(v as usize) } else { Err("IndexError".to_string()) } };
        let f = e.get("f").map(int_of).unwrap_or(0);
        let fi = if f < 0 { f + 10 } else { f };
        out.push(json!({"id": event_id(&e), "name": e["n"], "role": e.get("r").cloned().unwrap_or(json!("")), "code": c, "state": NAMES[idx(c)?], "from": NAMES[idx(fi)?], "day": e["d"],
                        "expr": EXPR[idx(c)?], "img": images(&e, fk, Some(st)), "line": story_line(&e, lang), "ln": e.get("ln").map(truthy).unwrap_or(false)}));
    }
    Ok(Value::Array(out))
}

// py: clan_status.py:192 wire_text
/// The tail for the 0xE1 state text ('' when nothing is pending).
pub fn wire_text(st: &Value) -> String {
    let all = pending(st);
    let ev: &[Value] = &all[all.len().saturating_sub(4)..];
    if ev.is_empty() {
        return String::new();
    }
    let mut f: Vec<String> = vec![format!("sc={}", ev.len())];
    for (k, e) in ev.iter().enumerate() {
        let c = e.get("c").map(int_of).unwrap_or(0);
        f.push(format!("sci{}={}", k, if c < 8 { c } else { SAD }));
        if c >= 8 {
            f.push(format!("scx{}={}", k, c));
        }
        f.push(format!("scd{}={}", k, e.get("d").map(int_of).unwrap_or(0)));
        f.push(format!("scg{}={}", k, if sex_of(e) == "f" { 1 } else { 0 }));
        f.push(format!("scl{}={}", k, look_of(e)));
    }
    for (k, e) in ev.iter().enumerate() {
        let n: String = name_of(e).chars().map(|c| if (c.is_ascii_alphanumeric()) || c == '-' || c == '\'' { c } else { '_' }).take(20).collect();
        f.push(format!("scm{}={}", k, n));
    }
    f.join(" ")
}
