// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K2 (clan glue) chunk calls, with the signatures they will have.
// The CL6 modules (clan_perks, clan_status, clan_ingame, achievements) are not merged yet: they are DETERMINISTIC DOUBLES here (the same doubles as
// verify/py/k2_harness.py injects as Python modules): they log their arguments and write one note row, so the differential test sees exactly what the glue
// passed and the transaction it ran in. At integration: `use crate::clan::{achievements, clan_ingame, clan_perks, clan_status};` replaces the four
// `pub mod` blocks and the harness runs with T2R_K2_DOUBLES=0 (real modules on both sides).
#![allow(dead_code, unused_variables)]
use super::*;
use crate::clock;
use indexmap::IndexMap;
use parking_lot::Mutex;
use rusqlite::Connection;
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::AtomicBool;
use std::sync::LazyLock;

fn note(c: &Connection, what: &str) {
    let _ = c.execute("CREATE TABLE IF NOT EXISTS dbl_notes(seq INTEGER PRIMARY KEY AUTOINCREMENT, note TEXT)", []);
    let _ = c.execute("INSERT INTO dbl_notes(note) VALUES(?)", [what]);
}

fn counts_str<'a>(it: impl Iterator<Item = (&'a i64, &'a i64)>) -> String {
    format!("{{{}}}", it.map(|(k, v)| format!("{}: {}", k, v)).collect::<Vec<_>>().join(", "))
}

fn sorted_counts(m: &HashMap<i64, i64>) -> String {
    let mut v: Vec<(&i64, &i64)> = m.iter().collect();
    v.sort();
    counts_str(v.into_iter())
}

fn ob(v: Option<bool>) -> &'static str {
    match v {
        Some(true) => "True",
        Some(false) => "False",
        None => "None",
    }
}

// ---------------------------------------------------------------- CL6: clan_perks (double)
pub mod clan_perks {
    use super::*;
    use crate::clan::clan::Counts;
    pub struct Who {
        pub n: String,
        pub r: String,
        pub g: i64,
        pub look: i64,
        pub i: i64,
        pub pid: Option<String>,
    }
    pub struct Perk {
        pub cat: &'static str,
        pub wire: i64,
        pub key: &'static str,
        pub cap: i64,
        pub who: Option<Who>,
    }
    /// (double) 0..2 perks by the clan's day, cap = size of the story roster
    pub fn active(clan_row: &Value, story: Option<&Value>) -> Vec<Perk> {
        let day = clan_row.get("day").map(crate::clan::clan::int_of).unwrap_or(0);
        let cap = story.map(|s| s.get("people").and_then(|p| p.as_array()).map(|a| a.len() as i64).unwrap_or(0)).unwrap_or(-1);
        let cats = [("food", "f"), ("water", "w"), ("med", "m")];
        (0..(day % 3)).map(|i| Perk { cat: cats[i as usize].0, wire: i + 1, key: cats[i as usize].1, cap, who: None }).collect()
    }
    pub fn loss_streak(story: Option<&Value>) -> i64 {
        story.map(|s| s.get("people").and_then(|p| p.as_array()).map(|a| a.len() as i64).unwrap_or(0) % 3).unwrap_or(0)
    }
    pub fn wire_text(perks: &[Perk], streak: i64) -> String {
        if perks.is_empty() {
            return String::new();
        }
        format!("pk={} ls={}", perks.iter().map(|p| format!("{}:{}:{}", p.cat, p.wire, p.cap)).collect::<Vec<_>>().join(","), streak)
    }
    pub fn on_day(c: &Connection, tid: &str, newc: &Value, r: &Value, counts: Option<&Counts>, matchv: Option<&Value>) -> Result<Vec<(&'static str, i64)>, String> {
        let n = counts.and_then(|m| m.get(&31)).copied().unwrap_or(0);
        super::note(c, &format!("perks.on_day {} day={} counts={} match={}", tid, crate::clan::clan::gi(newc, "day", 0), counts.map(sorted_counts).unwrap_or_default(), crate::py::b(matchv.is_some())));
        let _ = r;
        if n > 7 {
            return Err("perks double: boom".to_string());
        }
        Ok(if n > 0 { vec![("food", n), ("med", 1)] } else { Vec::new() })
    }
}

// ---------------------------------------------------------------- CL6: clan_status (double)
pub mod clan_status {
    use super::*;
    pub fn wire_text(st: &Value) -> String {
        if st.is_null() {
            return String::new();
        }
        let n = st.get("people").and_then(|p| p.as_array()).map(|a| a.len() as i64).unwrap_or(0) % 4;
        if n == 0 {
            String::new()
        } else {
            format!("st={}", n)
        }
    }
}

// ---------------------------------------------------------------- CL6: clan_ingame (double)
pub mod clan_ingame {
    use super::*;
    use crate::clan::clan::Counts;
    pub fn build(c: &Connection, tid: &str, lang: &str, section: i64, seen_id: i64, seen_day: i64, token: Option<&str>) -> Result<String, String> {
        if section == 7 {
            return Err("ingame double: boom".to_string());
        }
        let n = (seen_id % 4000) as usize;
        let mut s = format!("HEAD {} {} {} {} {} {}\n", tid, lang, section, seen_id, seen_day, token.unwrap_or("-"));
        for i in 0..n {
            s.push_str(&format!("R{}|{}|{}\n", i, tid, lang));
        }
        if section == 8 {
            s.push_str(&"x".repeat(30000)); // one row longer than the cap
        }
        Ok(s)
    }
    pub fn stats_report(c: &Connection, mid: &str, token: &str, name: &str, team: i64, counts: Option<&Counts>, left: bool) -> rusqlite::Result<()> {
        super::note(c, &format!("ingame.stats_report {} {} {} team={} counts={} left={}", mid, token, name, team, counts.map(sorted_counts).unwrap_or_else(|| "None".into()), crate::py::b(left)));
        Ok(())
    }
    pub fn stats_parts(c: &Connection, mid: &str, token: &str, parts: i64) {
        super::note(c, &format!("ingame.stats_parts {} {} {}", mid, token, parts));
    }
    pub fn player_rank(c: &Connection, token: &str) -> (i64, i64) {
        let h = |a: usize| i64::from_str_radix(token.get(a..a + 2).unwrap_or("0"), 16).unwrap_or(0);
        (h(0) % 10, h(2) % 1000)
    }
}

// ---------------------------------------------------------------- CL6: achievements (double)
pub mod achievements {
    use super::*;
    pub struct Ach {
        pub id: &'static str,
        pub title: &'static str,
    }
    static BY: LazyLock<Mutex<HashMap<String, &'static Ach>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
    pub fn by_id(id: &str) -> Option<&'static Ach> {
        let mut m = BY.lock();
        if let Some(a) = m.get(id) {
            return Some(a);
        }
        let a: &'static Ach = Box::leak(Box::new(Ach { id: Box::leak(id.to_string().into_boxed_str()), title: Box::leak(format!("T-{}", id).into_boxed_str()) }));
        m.insert(id.to_string(), a);
        Some(a)
    }
    fn done_str(d: &HashMap<i64, i64>) -> String {
        sorted_counts(d)
    }
    fn ids(counts: &IndexMap<i64, i64>) -> Vec<String> {
        counts.iter().filter(|(_, v)| **v >= 5).map(|(k, _)| format!("m{}", k)).take(3).collect()
    }
    pub fn settle(c: &Connection, key: &str, auth: &HashMap<i64, i64>, done: &mut HashMap<i64, i64>, labels: &[i64]) {
        super::note(c, &format!("ach.settle {} auth={} done={} labels={}", key, sorted_counts(auth), done_str(done), labels.len()));
        for l in labels {
            if let Some(v) = auth.get(l) {
                let e = done.entry(*l).or_insert(0);
                *e = (*e).max(*v);
            }
        }
    }
    pub fn book_match(c: &Connection, key: &str, counts: &IndexMap<i64, i64>, done: &mut HashMap<i64, i64>, win: Option<bool>, left: bool, late: bool, first: bool, now: Option<f64>) -> Vec<String> {
        super::note(c, &format!("ach.book_match {} counts={} done={} win={} left={} late={} first={}", key, counts_str(counts.iter()), done_str(done), ob(win), crate::py::b(left), crate::py::b(late), crate::py::b(first)));
        for (k, v) in counts {
            let e = done.entry(*k).or_insert(0);
            *e = (*e).max(*v);
        }
        ids(counts)
    }
    pub fn book_live(c: &Connection, key: &str, counts: &IndexMap<i64, i64>, done: &mut HashMap<i64, i64>, now: Option<f64>) -> Vec<String> {
        super::note(c, &format!("ach.book_live {} counts={} done={}", key, counts_str(counts.iter()), done_str(done)));
        for (k, v) in counts {
            let e = done.entry(*k).or_insert(0);
            *e = (*e).max(*v);
        }
        ids(counts).into_iter().map(|s| format!("l{}", s)).collect()
    }
    pub fn book_side(c: &Connection, key: &str, now: Option<f64>) -> Vec<String> {
        super::note(c, &format!("ach.book_side {}", key));
        vec!["side".to_string()]
    }
    pub fn book_clan(c: &Connection, key: &str, before: Option<&Value>, after: Option<&Value>, r: Option<&Value>, rank_weeks: Option<i64>, now: Option<f64>) -> Vec<String> {
        let day = |v: Option<&Value>| v.map(|x| crate::clan::clan::gi(x, "day", -1)).unwrap_or(-2);
        super::note(c, &format!("ach.book_clan {} before={} after={} r={}", key, day(before), day(after), crate::py::b(r.is_some())));
        vec![format!("c{}", day(after)), "m1".to_string()]
    }
    pub fn wire_text(c: &Connection, key: &str, rank_weeks: Option<i64>) -> String {
        super::note(c, &format!("ach.wire_text {} weeks={}", key, rank_weeks.map(|w| w.to_string()).unwrap_or_else(|| "None".into())));
        format!("ACH {} {}\n", key, rank_weeks.unwrap_or(-1))
    }
}

// ---------------------------------------------------------------- K5 batch
/// `loaded_clear_match(mid, players)` (double: no-op)
pub fn loaded_clear_match(mid: &str, players: &[String]) {}

// ---------------------------------------------------------------- K6 loop
pub static OFFLINE_GRACE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_OFFLINE_GRACE", 60.0));
/// daemon.py:9943 RESTORED_T: mid -> time the match was brought back after a daemon restart
pub static RESTORED_T: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static RESTORE_EXCUSE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_RESTORE_EXCUSE_S", 600.0));
#[allow(unused_imports)]
use HashSet as _HS;
#[allow(unused_imports)]
use AtomicBool as _AB;
#[allow(unused_imports)]
use clock as _clk;

// ---------------------------------------------------------------- test hooks (selftest k2 only; empty in production)
/// token -> seconds of silence (`silence(token, now)` of K6 reads the presence table)
pub static SILENCE: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// mids the PRIV double reports as private matches (K1's PrivStub.is_private_mid reads it)
pub static PRIV_MIDS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
