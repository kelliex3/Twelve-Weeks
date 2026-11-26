//! TEMPORARY: stand-ins for the clan modules clan_story calls that are ported by other agents (clan_alife, clan_chronicle, clan_journal, clan_arcs,
//! clan_perks, clan_saga). Replace each function by the real port (same signature) and delete this file. `enabled()` is false here (the A-Life and the
//! chronicle are optional switches that default to off); every other stand-in is `unimplemented!()` - except in the differential test, where
//! `set_test_fake(true)` makes them deterministic fakes (the Python harness injects the same fakes as modules: verify/py/clan_story_harness.py).
use rusqlite::Connection;
use serde_json::{Map, Value};
use std::sync::atomic::{AtomicBool, Ordering};

static FAKE: AtomicBool = AtomicBool::new(false);
static FAKE_ALIFE: AtomicBool = AtomicBool::new(false);
static FAKE_CHRON: AtomicBool = AtomicBool::new(false);

/// Test only: the stand-ins answer with the deterministic fakes of the harness.
pub fn set_test_fake(on: bool) {
    FAKE.store(on, Ordering::SeqCst);
}

/// Test only: switch the fake A-Life / chronicle on or off.
pub fn set_test_switches(alife: bool, chron: bool) {
    FAKE_ALIFE.store(alife, Ordering::SeqCst);
    FAKE_CHRON.store(chron, Ordering::SeqCst);
}

fn fake() -> bool {
    FAKE.load(Ordering::SeqCst)
}

fn vi(v: &Value, k: &str) -> i64 {
    v.get(k).and_then(|x| x.as_i64()).unwrap_or(0)
}

fn first_person(st: &Value) -> Option<(String, String)> {
    let p = st.get("people")?.as_array()?.first()?;
    Some((p.get("n")?.as_str()?.to_string(), p.get("g")?.as_str()?.to_string()))
}

/// The fake journal / A-Life log their calls into this table (the harness compares the rows).
fn note(c: &Connection, tid: &str, what: &str) {
    let _ = c.execute("CREATE TABLE IF NOT EXISTS fake_notes(seq INTEGER PRIMARY KEY AUTOINCREMENT, token_id TEXT, note TEXT)", []);
    let _ = c.execute("INSERT INTO fake_notes(token_id,note) VALUES(?,?)", rusqlite::params![tid, what]);
}

fn fake_row(kind: &str, text: String, st: &Value, lid: &str, day: i64) -> Value {
    let mut m = Map::new();
    m.insert("kind".into(), Value::String(kind.into()));
    m.insert("mood".into(), Value::String("calm".into()));
    m.insert("text".into(), Value::String(text));
    let mut p = Map::new();
    let mut names = Vec::new();
    if let Some((n, g)) = first_person(st) {
        names.push(Value::String(n.clone()));
        p.insert("A".into(), Value::Array(vec![Value::String(n), Value::String(g)]));
    }
    m.insert("names".into(), Value::Array(names));
    m.insert("lid".into(), Value::String(lid.into()));
    let mut s = Map::new();
    s.insert("day".into(), Value::from(day));
    let mut pm = Map::new();
    pm.insert("p".into(), Value::Object(p));
    pm.insert("s".into(), Value::Object(s));
    m.insert("params".into(), Value::Object(pm));
    Value::Object(m)
}

/// py: clan_alife.py
pub mod alife {
    use super::*;

    /// of the menu's 6 ticker lines this many come from the A-Life facts
    pub const TICKER_SHARE: usize = 3;

    // py: clan_alife.py:90 enabled
    pub fn enabled() -> bool {
        fake() && FAKE_ALIFE.load(Ordering::SeqCst)
    }

    // py: clan_alife.py:156 reset
    pub fn reset(c: &Connection, tid: &str) -> Result<(), String> {
        if fake() {
            note(c, tid, "alife.reset");
            return Ok(());
        }
        unimplemented!("clan_alife::reset")
    }

    // py: clan_alife.py:886 on_day
    pub fn on_day(c: &Connection, tid: &str, st: &mut Value, newc: &Value, r: &Value, msgs: &[Value]) -> Result<Vec<Value>, String> {
        if fake() {
            let _ = (newc, r);
            let day = vi(st, "day");
            note(c, tid, &format!("alife.on_day {} {}", day, msgs.len()));
            st["al"] = Value::from(day);
            return Ok(vec![fake_row("alife", format!("AL day {} n{}", day, msgs.len()), st, "al/fake", day)]);
        }
        let _ = (c, tid, st, newc, r, msgs);
        unimplemented!("clan_alife::on_day")
    }

    // py: clan_alife.py:1072 ticker_lines
    pub fn ticker_lines(c: &Connection, tid: &str, st: &Value, slot: i64, n: usize, lang: &str, now: Option<f64>, save: bool) -> Result<Vec<(Value, String)>, String> {
        if fake() {
            let _ = (c, tid, st, now, save);
            return Ok((0..n).map(|i| (Value::String(format!("a{}{:02}", slot.rem_euclid(100000), i)), format!("AL tick {} {} {}", slot, i, lang))).collect());
        }
        let _ = (c, tid, st, slot, n, lang, now, save);
        unimplemented!("clan_alife::ticker_lines")
    }

    // py: clan_alife.py:239 render_msg
    pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
        if fake() {
            let _ = params;
            return if lid == "al/none" { None } else { Some(format!("AL[{}|{}]", lid, lang)) };
        }
        let _ = (lid, params, lang);
        unimplemented!("clan_alife::render_msg")
    }
}

/// py: clan_chronicle.py
pub mod chronicle {
    use super::*;

    // py: clan_chronicle.py:187 enabled
    pub fn enabled() -> bool {
        if fake() {
            return FAKE_CHRON.load(Ordering::SeqCst);
        }
        crate::clan::clan_chronicle::enabled() // (the chronicle is ported: its real switch outside the clan_story selftest)
    }

    // py: clan_chronicle.py:492 day_rows
    pub fn day_rows(st: &mut Value, newc: &Value, r: &Value) -> Result<Vec<Value>, String> {
        if fake() {
            let _ = (newc, r);
            let day = vi(st, "day");
            return Ok(vec![fake_row("chron", format!("CH day {}", day), st, "ch/fake+x", day)]);
        }
        let _ = (st, newc, r);
        unimplemented!("clan_chronicle::day_rows")
    }

    // py: clan_chronicle.py:318 render_msg
    pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
        if fake() {
            let _ = params;
            return Some(format!("CH[{}|{}]", lid, lang));
        }
        crate::clan::clan_chronicle::render_msg(lid, params, lang)
    }
}

/// py: clan_journal.py
pub mod journal {
    use super::*;

    // py: clan_journal.py:470 purge
    pub fn purge(c: &Connection, tid: &str) -> Result<(), String> {
        if fake() {
            note(c, tid, "journal.purge");
            return Ok(());
        }
        unimplemented!("clan_journal::purge")
    }

    // py: clan_journal.py:493 on_new_run
    pub fn on_new_run(c: &Connection, tid: &str, st: &Value) -> Result<(), String> {
        if fake() {
            note(c, tid, &format!("journal.on_new_run {}", st.get("people").and_then(|p| p.as_array()).map_or(0, |a| a.len())));
            return Ok(());
        }
        let _ = (c, tid, st);
        unimplemented!("clan_journal::on_new_run")
    }

    // py: clan_journal.py:178 snapshot
    pub fn snapshot(st: &Value) -> Result<Value, String> {
        if fake() {
            let mut m = Map::new();
            m.insert("people".into(), Value::from(st.get("people").and_then(|p| p.as_array()).map_or(0, |a| a.len()) as i64));
            m.insert("day".into(), Value::from(vi(st, "day")));
            return Ok(Value::Object(m));
        }
        let _ = st;
        unimplemented!("clan_journal::snapshot")
    }

    // py: clan_journal.py:508 on_day
    pub fn on_day(c: &Connection, tid: &str, st: &mut Value, before: &Value, newc: &Value, r: &Value) -> Result<(), String> {
        if fake() {
            let _ = (newc, r);
            let after = st.get("people").and_then(|p| p.as_array()).map_or(0, |a| a.len());
            note(c, tid, &format!("journal.on_day {} before={} after={}", vi(st, "day"), vi(before, "people"), after));
            return Ok(());
        }
        let _ = (c, tid, st, before, newc, r);
        unimplemented!("clan_journal::on_day")
    }
}

/// clan_arcs / clan_saga / clan_chronicle are ported: the real render, the stand-in text only under the clan_story selftest's `set_test_fake`.
macro_rules! render_real {
    ($m:ident, $py:expr, $tag:expr, $real:path) => {
        #[doc = $py]
        pub mod $m {
            use super::*;

            pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
                if fake() {
                    let _ = params;
                    return Some(format!("{}[{}|{}]", $tag, lid, lang));
                }
                $real(lid, params, lang)
            }
        }
    };
}

render_real!(arcs, "py: clan_arcs.py:521 render_msg", "ARC", crate::clan::clan_arcs::render_msg);
/// clan_perks is ported (CL6): the real render, the stand-in text only under the clan_story selftest's `set_test_fake`.
pub mod perks {
    use super::*;

    pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
        if fake() {
            let _ = params;
            return Some(format!("PK[{}|{}]", lid, lang));
        }
        crate::clan::clan_perks::render_msg(lid, params, lang)
    }
}
render_real!(saga, "py: clan_saga.py:929 render_msg", "SG", crate::clan::clan_saga::render_msg);
