//! TEMPORARY: the functions of modules that are not ported yet and that clan_chronicle / clan_arcs call (clan_alife_lex, clan_story, clan_saga,
//! clan_loot, clan_status). Each module of the clan engine replaces its part when it lands (delete the block, point the calls at the real module).
//!
//! `lex` and `story::{_load, finish_member}` are REAL ports (small, needed by every row); the saga / loot / status / spare_sick blocks are
//! `unimplemented!()` in production and run deterministic TEST DOUBLES (the same ones verify/py/clan_chronicle_harness.py installs on the Python
//! side) only while `doubles(true)` is set by `selftest clan_chronicle`. Conventions: `cs` (= `st["ch"]`) is never passed, the callee takes it
//! from `st`; a Python exception is `Err(String)`.
use crate::clan::clan::{int_of, pystr, truthy};
use crate::clan::data;
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use t2r_clanstory::pyrand::PyRandom;

static DOUBLES: AtomicBool = AtomicBool::new(false);
static REAL_SAGA: AtomicBool = AtomicBool::new(false);

/// Selftest only: with the doubles on, the saga and loot calls still go to the REAL clan_saga / clan_loot (`selftest clan_saga`).
pub fn real_saga(on: bool) {
    REAL_SAGA.store(on, Ordering::SeqCst);
}

/// The saga / loot functions are real in production and in `selftest clan_saga`; only `selftest clan_chronicle` runs their doubles.
fn real() -> bool {
    !DOUBLES.load(Ordering::SeqCst) || REAL_SAGA.load(Ordering::SeqCst)
}

/// Test doubles on (selftest only).
pub fn doubles(on: bool) {
    DOUBLES.store(on, Ordering::SeqCst);
}

fn need_doubles(what: &str) {
    if !DOUBLES.load(Ordering::SeqCst) {
        unimplemented!("{} is not ported yet (src/clan/ext_clan_chronicle.rs)", what);
    }
}

/// Python `int(x)` of a value in a template variable.
pub type Vars = HashMap<String, String>;

// ---------------------------------------------------------------------------------------------------- clan_alife_lex.py (the needed part)
pub mod lex {
    use super::*;
    use regex::Regex;
    use std::collections::BTreeSet;
    use std::sync::LazyLock;

    const W: &str = r"\p{L}\p{N}_";

    // py: clan_alife_lex.py:312 _KEY
    static KEY: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"\{{([{w}]+)\}}", w = W)).expect("KEY"));
    // py: clan_alife_lex.py:313 _SW
    static SW: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{([A-D]+)\?([^|{}]*)\|([^|{}]*)\}").expect("SW"));
    // py: clan_alife_lex.py:314 _PERSON   ^([ABCD])_(\w+)$
    static PERSON: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"^([ABCD])_([{w}]+)$", w = W)).expect("PERSON"));
    static PERSON_ONLY: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"^[A-D](?:_[{w}]+)?$", w = W)).expect("PERSON_ONLY"));
    static LETTER: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"\{{([A-D])(?:_[{w}]+)?\}}", w = W)).expect("LETTER"));

    fn data_roles() -> &'static Value {
        data::t("clan_story_data", "ROLES")
    }

    // py: clan_alife_lex.py:317 role_word
    /// `Err` = the KeyError of an unknown role.
    pub fn role_word(person: &Value, fk: &str, lang: &str) -> Result<String, String> {
        let r = match person.get("r") {
            Some(v) if truthy(v) => pystr(v),
            _ => "scavenger".to_string(),
        };
        let i = if fk == "W" { 0 } else { 1 };
        if lang == "pl" {
            let pair = data::t("clan_story_pl", "ROLES").get(&r).and_then(|x| x.get(i)).ok_or_else(|| format!("KeyError: {}", r))?;
            let k = if person.get("g").and_then(|v| v.as_str()) == Some("f") { 1 } else { 0 };
            Ok(pair.get(k).map(pystr).unwrap_or_default())
        } else {
            Ok(data_roles().get(&r).and_then(|x| x.get(i)).map(pystr).ok_or_else(|| format!("KeyError: {}", r))?)
        }
    }

    /// `CS.PRON[g][word]`.
    pub fn pron(g: &str, word: &str) -> Result<String, String> {
        data::t("clan_story", "PRON")
            .get(g)
            .and_then(|x| x.get(word))
            .map(pystr)
            .ok_or_else(|| format!("KeyError: PRON[{}][{}]", g, word))
    }

    fn cap_first(v: &str) -> String {
        let mut it = v.chars();
        match it.next() {
            Some(c) => c.to_uppercase().collect::<String>() + it.as_str(),
            None => String::new(),
        }
    }

    // py: clan_alife_lex.py:325 render
    /// Fill a template: variables first (their values may hold switches), then the {A?m|f} switches, then names / pronouns / roles.
    /// `Err` = a Python exception (a letter missing from `people`, an unknown pronoun or role).
    pub fn render(tpl: &str, people: &Value, vars: &Vars, lang: &str, fk: &str) -> Result<String, String> {
        // 1. variables (the sentence-start test looks at the ORIGINAL template before the key)
        let mut s = String::with_capacity(tpl.len() + 16);
        let mut last = 0;
        for m in KEY.captures_iter(tpl) {
            let whole = m.get(0).map(|x| x).ok_or("re")?;
            s.push_str(&tpl[last..whole.start()]);
            let k = m.get(1).map(|x| x.as_str()).unwrap_or("");
            match vars.get(k) {
                Some(val) => {
                    let before = tpl[..whole.start()].trim_end_matches(crate::py::is_py_space);
                    let lower = val.chars().next().map(|c| c.is_lowercase()).unwrap_or(false);
                    if lower && (before.is_empty() || before.chars().next_back().map(|c| ".!?\"'„”".contains(c)).unwrap_or(false)) {
                        s.push_str(&cap_first(val));
                    } else {
                        s.push_str(val);
                    }
                }
                None => s.push_str(whole.as_str()),
            }
            last = whole.end();
        }
        s.push_str(&tpl[last..]);
        // 2. {AB?m|f} switches
        let mut s2 = String::with_capacity(s.len());
        let mut last = 0;
        for m in SW.captures_iter(&s) {
            let whole = m.get(0).ok_or("re")?;
            s2.push_str(&s[last..whole.start()]);
            let mut fem = true;
            for ch in m.get(1).map(|x| x.as_str()).unwrap_or("").chars() {
                let p = people.get(ch.to_string()).ok_or_else(|| format!("KeyError: {}", ch))?;
                if p.get("g").and_then(|v| v.as_str()) != Some("f") {
                    fem = false;
                    break; // Python all() stops at the first non-f: later letters need not exist
                }
            }
            s2.push_str(m.get(if fem { 3 } else { 2 }).map(|x| x.as_str()).unwrap_or(""));
            last = whole.end();
        }
        s2.push_str(&s[last..]);
        // 3. names, pronouns, roles
        let mut out = String::with_capacity(s2.len());
        let mut last = 0;
        for m in KEY.captures_iter(&s2) {
            let whole = m.get(0).ok_or("re")?;
            out.push_str(&s2[last..whole.start()]);
            let k = m.get(1).map(|x| x.as_str()).unwrap_or("");
            if let Some(pm) = PERSON.captures(k) {
                let letter = pm.get(1).map(|x| x.as_str()).unwrap_or("");
                let p = people.get(letter).ok_or_else(|| format!("KeyError: {}", letter))?;
                let w = pm.get(2).map(|x| x.as_str()).unwrap_or("");
                if w == "role" {
                    out.push_str(&role_word(p, fk, lang)?);
                } else {
                    let g = p.get("g").map(pystr).unwrap_or_default();
                    let r = pron(&g, &w.to_lowercase())?;
                    if w.chars().next().map(|c| c.is_uppercase()).unwrap_or(false) {
                        out.push_str(&cap_first(&r));
                    } else {
                        out.push_str(&r);
                    }
                }
            } else if let Some(p) = people.get(k) {
                out.push_str(&p.get("n").map(pystr).unwrap_or_default());
            } else {
                out.push_str(whole.as_str());
            }
            last = whole.end();
        }
        out.push_str(&s2[last..]);
        Ok(out)
    }

    // py: clan_alife_lex.py:359 letters_of
    pub fn letters_of(tpl: &str) -> BTreeSet<char> {
        let mut out: BTreeSet<char> = LETTER.captures_iter(tpl).filter_map(|m| m.get(1)?.as_str().chars().next()).collect();
        for m in SW.captures_iter(tpl) {
            out.extend(m.get(1).map(|x| x.as_str()).unwrap_or("").chars());
        }
        out
    }

    // py: clan_alife_lex.py:366 vars_of
    /// Variable keys (not people) of a template.
    pub fn vars_of(tpl: &str) -> BTreeSet<String> {
        KEY.captures_iter(tpl).filter_map(|m| m.get(1)).map(|x| x.as_str().to_string()).filter(|k| !PERSON_ONLY.is_match(k)).collect()
    }
}

// ---------------------------------------------------------------------------------------------------- clan_story.py
pub mod story {
    use super::*;

    // py: clan_story.py:880 _load
    pub fn _load(c: &Connection, tid: &str) -> Option<Value> {
        let row: Option<String> = c.query_row("SELECT state FROM clan_story WHERE token_id=?", [tid], |r| r.get::<_, String>(0)).ok();
        let st: Value = serde_json::from_str(&row?).ok()?;
        if st.is_object() && st.get("v").map(int_of) == Some(1) && st.get("v").map(|v| v.is_number()).unwrap_or(false) {
            Some(st)
        } else {
            None
        }
    }

    fn by_id_idx(st: &Value, i: i64) -> Option<usize> {
        st.get("people")?.as_array()?.iter().position(|p| p.get("i").map(int_of) == Some(i))
    }

    fn set_field(st: &mut Value, idx: usize, k: &str, v: Value) {
        if let Some(p) = st.get_mut("people").and_then(|x| x.as_array_mut()).and_then(|a| a.get_mut(idx)) {
            if let Some(o) = p.as_object_mut() {
                o.insert(k.to_string(), v);
            }
        }
    }

    // py: clan_story.py:463 finish_member  (+ _remove at :486)
    /// A member leaves the roster for good (a story arc's end). `p` and `helper` are looked up in `st["people"]` by their id (Python mutates the very objects).
    pub fn finish_member(st: &mut Value, p: &Value, day: i64, cause: &str, helper: Option<&Value>) {
        let pid = p.get("i").map(int_of).unwrap_or(0);
        // _remove(st, p, day, cause)
        if let Some(idx) = by_id_idx(st, pid) {
            if let Some(a) = st.get_mut("people").and_then(|x| x.as_array_mut()) {
                a.remove(idx);
            }
        }
        let pk = p.get("k").map(int_of).unwrap_or(0);
        let k_id = if pk != 0 { by_id_idx(st, pk).and_then(|ix| st["people"][ix].get("i").map(int_of)) } else { None };
        if let Some(kid) = k_id {
            if let Some(ix) = by_id_idx(st, kid) {
                set_field(st, ix, "k", json!(0));
            }
        }
        let rec = json!({"i": p.get("i").cloned().unwrap_or(Value::Null), "n": p.get("n").cloned().unwrap_or(Value::Null), "g": p.get("g").cloned().unwrap_or(Value::Null),
            "r": p.get("r").cloned().unwrap_or(Value::Null), "d": day, "c": cause, "il": p.get("il").cloned().unwrap_or(Value::Null), "k": k_id.unwrap_or(0)});
        if let Some(d) = st.get_mut("dead").and_then(|x| x.as_array_mut()) {
            d.push(rec);
            let n = d.len();
            if n > 30 {
                d.drain(..n - 30);
            }
        }
        // k = _by_id(st, p["k"]) was taken BEFORE the kin link was cleared in Python's finish_member? No: _remove() cleared k["k"], then `_by_id(st, p["k"])` runs again.
        let k2 = if pk != 0 { by_id_idx(st, pk) } else { None };
        let mut ids: Vec<usize> = Vec::new();
        if let Some(ix) = k2 {
            ids.push(ix);
        }
        let hix = helper.and_then(|h| h.get("i")).map(int_of).and_then(|i| by_id_idx(st, i));
        if let Some(ix) = hix {
            if !ids.contains(&ix) {
                ids.push(ix);
            }
        }
        for ix in ids {
            let cur = st["people"][ix].get("gr").map(int_of).unwrap_or(0);
            set_field(st, ix, "gr", json!(cur.max(day + 6)));
        }
        if matches!(cause, "mercy" | "bite_kin" | "turned") {
            if let Some(ix) = hix {
                set_field(st, ix, "gl", json!(day + 12));
            }
        }
    }

    // py: clan_story.py:474 spare_sick   (TEST DOUBLE: the real one draws a name from the pools)
    pub fn spare_sick(rng: &mut PyRandom, st: &mut Value, day: i64) -> Value {
        need_doubles("clan_story::spare_sick");
        let _ = rng.random();
        let pid = st.get("pid").map(int_of).unwrap_or(0) + 1;
        st["pid"] = json!(pid);
        let q = json!({"i": pid, "n": format!("Spare{}", pid), "l": "Xx", "g": "n", "r": "scavenger", "s": "s", "il": "flu", "is": 0, "id": day, "k": 0, "jd": 0, "gr": 0, "gl": 0});
        if let Some(a) = st.get_mut("people").and_then(|x| x.as_array_mut()) {
            a.push(q.clone());
        }
        q
    }
}

// ---------------------------------------------------------------------------------------------------- clan_saga.py
pub mod saga {
    use super::*;
    use crate::clan::clan_chronicle::Day;

    // py: clan_saga.py:140 fit  (TEST DOUBLE)
    pub fn fit(cs: &Value, kind: &str, p: &Value) -> i64 {
        if real() {
            return crate::clan::clan_saga::fit(cs, kind, p);
        }
        need_doubles("clan_saga::fit");
        let _ = cs;
        (p.get("i").map(int_of).unwrap_or(0) * 7 + kind.len() as i64) % 5 - 2
    }

    // py: clan_saga.py:922 run  (TEST DOUBLE). `cs` = st["ch"] is taken from `st`; `ch` (the chronicle module) is this crate's clan_chronicle.
    pub fn run(d: &mut Day, st: &mut Value, newc: &Value, r: &Value, matchv: &Value, rng: &mut PyRandom, loot: &[Value], died_now: &[Value], arc_rows: &mut [Value]) -> Result<Vec<Value>, String> {
        if real() {
            return crate::clan::clan_saga::run(d, st, newc, r, matchv, rng, loot, died_now, arc_rows);
        }
        need_doubles("clan_saga::run");
        let _ = (newc, r, matchv, loot, died_now, arc_rows);
        let _ = rng.random();
        let tid = st.get("tid").map(pystr).unwrap_or_default();
        let day = d.day;
        let h = crate::clan::clan_chronicle::h(&["fakesaga".to_string(), tid, day.to_string()]);
        let n = st["ch"].get("sg_n").map(int_of).unwrap_or(0) + 1;
        st["ch"]["sg_n"] = json!(n);
        if d.alive.is_empty() {
            return Ok(vec![]);
        }
        let first = d.alive[0].clone();
        let last = d.alive[d.alive.len() - 1].clone();
        d.used.insert(format!("P{}", first.get("i").map(int_of).unwrap_or(0)), day);
        d.spot.insert(last.get("i").map(int_of).unwrap_or(0));
        if h % 3 == 0 {
            Ok(vec![json!({"kind": "chron", "mood": "calm", "text": format!("SAGA {} {}", day, first.get("n").map(pystr).unwrap_or_default()), "names": [], "lid": "sg/fake", "params": {}})])
        } else {
            Ok(vec![])
        }
    }

    // py: clan_saga.py render_msg  (TEST DOUBLE)
    pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
        if real() {
            return crate::clan::clan_saga::render_msg(lid, params, lang);
        }
        need_doubles("clan_saga::render_msg");
        let _ = (params, lang);
        Some(format!("SAGA-RENDER {}", lid))
    }
}

// ---------------------------------------------------------------------------------------------------- clan_loot.py
pub mod loot {
    use super::*;

    // py: clan_loot.py:125 fortune  (TEST DOUBLE)  `fo_st` = dict(st, morale=...) ; updates st["ch"]["fort"] (here: `cs` = the real st's ch)
    pub fn fortune(fo_st: &Value, cs: &mut Value, hist: &[Value]) -> Result<Value, String> {
                if real() {
            return crate::clan::clan_loot::fortune(fo_st, cs, hist);
        }
        need_doubles("clan_loot::fortune");
        let _ = fo_st;
        let s = hist.len() as i64 - 3;
        cs["fort"] = json!({"s": s});
        Ok(json!(s))
    }

    // py: clan_loot.py:353 roll_finds  (TEST DOUBLE)
    pub fn roll_finds(rng: &mut PyRandom, st: &mut Value, r: &Value, matchv: &Value, score: &Value, day: i64) -> Result<Vec<Value>, String> {
                if real() {
            return crate::clan::clan_loot::roll_finds(rng, st, r, matchv, score, Some(day));
        }
        need_doubles("clan_loot::roll_finds");
        let _ = (st, r, matchv, score, day);
        let n = rng.randrange(0, 3).map_err(|e| format!("{:?}", e))?;
        let keys = ["pine", "beans", "bandage", "chess", "teddy"];
        let mut out = Vec::new();
        for _ in 0..n {
            let k = *rng.choice(&keys).map_err(|e| format!("{:?}", e))?;
            let c = rng.randrange(1, 4).map_err(|e| format!("{:?}", e))?;
            out.push(json!({"k": k, "n": c, "c": -1}));
        }
        Ok(out)
    }

    // py: clan_loot.py:552 legacy_loot  (TEST DOUBLE)
    pub fn legacy_loot(finds: &[Value], have: &Value) -> Vec<Value> {
                if real() {
            return crate::clan::clan_loot::legacy_loot(finds, have);
        }
        need_doubles("clan_loot::legacy_loot");
        let _ = have;
        finds.iter().map(|f| json!([f["k"], if crate::clan::clan_chronicle::is_keep(f["k"].as_str().unwrap_or("")) { 1 } else { int_of(&f["n"]) }])).collect()
    }

    // py: clan_loot.py:157 update_moods  (TEST DOUBLE)
    pub fn update_moods(st: &mut Value) -> Result<(), String> {
                if real() {
            return crate::clan::clan_loot::update_moods(st);
        }
        need_doubles("clan_loot::update_moods");
        let n = st["ch"].get("moods").map(int_of).unwrap_or(0) + 1;
        st["ch"]["moods"] = json!(n);
        Ok(())
    }

    // py: clan_loot.py:509 items_text  (TEST DOUBLE)
    pub fn items_text(finds: &Value, lang: &str) -> String {
                if real() {
            return crate::clan::clan_loot::items_text(finds, lang);
        }
        need_doubles("clan_loot::items_text");
        let _ = lang;
        finds.as_array().map(|a| a.iter().map(|f| format!("{}x{}", f["n"], f["k"].as_str().unwrap_or(""))).collect::<Vec<_>>().join(", ")).unwrap_or_default()
    }

    // py: clan_loot.py:514 public  (TEST DOUBLE)
    pub fn public(finds: &Value, lang: &str) -> Value {
                if real() {
            return crate::clan::clan_loot::public(finds, lang);
        }
        need_doubles("clan_loot::public");
        json!(finds.as_array().map(|a| a.iter().map(|f| json!({"k": f["k"], "lang": lang})).collect::<Vec<_>>()).unwrap_or_default())
    }
}

// ---------------------------------------------------------------------------------------------------- clan_status.py
pub mod status {
    use super::*;

    pub const HEALTHY: i64 = 0;
    pub const NAMES: [&str; 10] = ["healthy", "hungry", "sad", "sick", "dead", "injured", "recovered", "new", "depressed", "shock"];
    pub const EXPR: [&str; 10] = ["neutral", "hungry", "sad", "sick", "dead", "sick", "neutral", "neutral", "depressed", "shock"];

    // py: clan_status.py:46 code_of  (TEST DOUBLE)
    pub fn code_of(st: &Value, p: &Value) -> i64 {
        need_doubles("clan_status::code_of");
        let day = st.get("day").map(int_of).unwrap_or(0);
        let i = p.get("i").map(int_of).unwrap_or(0);
        [0, 1, 2, 3, 5, 8, 9][((i * 5 + day) % 7) as usize]
    }

    // py: clan_status.py:85 observe  (TEST DOUBLE)
    pub fn observe(st: &mut Value, rows: &[Value]) -> Result<(), String> {
        need_doubles("clan_status::observe");
        let n = st["ch"].get("obs").map(int_of).unwrap_or(0) + rows.len() as i64;
        st["ch"]["obs"] = json!(n);
        Ok(())
    }

    // py: clan_status.py:183 public  (TEST DOUBLE)
    pub fn public(st: &Value, fk: &str, lang: &str) -> Result<Value, String> {
        need_doubles("clan_status::public");
        if st.get("day").map(int_of).unwrap_or(0) % 2 != 0 {
            return Ok(json!({}));
        }
        let n = st.get("people").and_then(|x| x.as_array()).map(|a| a.len()).unwrap_or(0);
        Ok(json!({"events": [{"k": n, "lang": lang, "f": fk}]}))
    }
}

/// An empty JSON object.
pub fn obj() -> Value {
    Value::Object(Map::new())
}
