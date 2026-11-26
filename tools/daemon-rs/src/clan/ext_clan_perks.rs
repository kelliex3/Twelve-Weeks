//! TEMPORARY: removed at integration. The functions of clan_journal / clan_journal_entries (CL3, not merged here) that clan_perks, clan_status and
//! clan_ingame call. In production they are `unimplemented!()` until CL3's port lands; `doubles(true)` (selftest only) runs the deterministic test
//! doubles that verify/py/clan_ingame_harness.py installs on the Python side (same specs).
use crate::clan::clan::{int_of, pystr};
use rusqlite::Connection;
use serde_json::{json, Value};
use std::sync::atomic::{AtomicBool, Ordering};

static DOUBLES: AtomicBool = AtomicBool::new(false);

/// Test doubles on (selftest only).
pub fn doubles(on: bool) {
    DOUBLES.store(on, Ordering::SeqCst);
}

fn need_doubles(what: &str) {
    if !DOUBLES.load(Ordering::SeqCst) {
        unimplemented!("{} is not merged yet (src/clan/ext_clan_perks.rs)", what);
    }
}

pub mod journal {
    use super::*;

    // py: clan_journal.py render  (TEST DOUBLE)  "" = unreadable
    pub fn render(sent: &Value, lang: &str) -> String {
        need_doubles("clan_journal::render");
        match sent.as_str() {
            Some(s) if s != "x" => format!("JL:{}:{}", s, lang),
            _ => String::new(),
        }
    }

    // py: clan_journal.py add_perk  (TEST DOUBLE)
    pub fn add_perk(c: &Connection, tid: &str, st: &mut Value, day: i64, cat: &str, who: &Value, mapname: &str) -> Result<bool, String> {
        need_doubles("clan_journal::add_perk");
        let _ = (c, tid);
        if day % 11 == 0 {
            return Err("ValueError: double".into());
        }
        if !st["ch"].is_object() {
            st["ch"] = json!({});
        }
        let cs = &mut st["ch"];
        if !cs.get("pkj").map(|x| x.is_array()).unwrap_or(false) {
            cs["pkj"] = json!([]);
        }
        if let Some(a) = cs["pkj"].as_array_mut() {
            a.push(json!([day, cat, who.get("n").map(pystr).unwrap_or_default(), mapname]));
        }
        Ok(cat == "med" || cat == "ammo")
    }

    // py: clan_journal.py entries  (TEST DOUBLE)  oldest first; fields the in-game page reads
    pub fn entries(c: &Connection, tid: &str, lang: &str, days_done: Option<i64>) -> Vec<Value> {
        need_doubles("clan_journal::entries");
        let _ = (c, tid);
        let dd = days_done.unwrap_or(0);
        let n = if dd % 5 == 0 { 0 } else { dd.clamp(0, 90) }; // (no entries every 5th day: the older feed rows then fill the page)
        let tones = ["good", "warn", "bad", "info"];
        let wins = [json!(1), json!(0), Value::Null];
        (0..n)
            .map(|d| {
                let (wk, dow) = crate::clan::clan::week_day(d);
                json!({"day": d, "week": wk, "dow": if d > 0 { dow } else { 0 }, "id": 100 + d, "title": format!("Title {} {}|x", d, lang), "tone": tones[(d % 4) as usize],
                       "map": format!("Map{}", d % 5), "win": wins[(d % 3) as usize], "sup": d, "need": d + 1, "h": d % 7, "u": (d * 3) % 5, "s": d % 2, "dn": d % 3,
                       "paras": [["RAID", format!("Para {} a", d)], ["CAMP", format!("Para {} b\tx", d)]]})
            })
            .collect()
    }
}

pub mod journal_entries {
    use super::*;

    // py: clan_journal_entries.py compose_entries  (TEST DOUBLE)  rows = the story rows, newest first
    pub fn compose_entries(rows: &[Value], roster: &[Value], seen_id: i64) -> Vec<Value> {
        need_doubles("clan_journal_entries::compose_entries");
        let _ = roster;
        rows.iter()
            .map(|m| {
                let text = match m.get("text") {
                    Some(x) if crate::clan::clan::truthy(x) => pystr(x),
                    _ => String::new(),
                };
                let head: String = text.split('.').next().unwrap_or("").to_string();
                let id = m.get("id").map(|v| if crate::clan::clan::truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
                let authors: Vec<Value> = m.get("names").and_then(|x| x.as_array()).map(|a| a.iter().take(2).cloned().collect()).unwrap_or_default();
                json!({"prim": m, "head": head, "body": text, "authors": authors, "id": id, "new": if id > seen_id { 1 } else { 0 }})
            })
            .collect()
    }
}
