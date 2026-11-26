//! `t2r-daemon selftest clan_golden`: the Rust side of verify/py/clan_golden_harness.py (same script, same output bytes) = the GOLDEN test of the clan engine:
//! the whole day pipeline with the REAL modules (no doubles), in the order and with the arguments of daemon.py (the line of each call is cited).
//! One JSON event per input line -> `{"i", "r"[, "p" = printed lines]}` (or `{"i","err"}`); at the end a dump of every table and of the reset backups.
use crate::c_util::{jb, jf, ji, js, script};
use crate::clan::achievements as ach;
use crate::clan::clan::{self as cl, int_of, vref_to_json, Counts};
use crate::clan::{clan_ingame as ing, clan_perks as perks, clan_reset as rst, clan_status as status, clan_story as cs, clan_story_pl as pln};
use crate::{clock, py};
use indexmap::IndexMap;
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::io::{BufRead, Write};

const ACH_E2_ONLY: [i64; 11] = [1, 2, 3, 4, 5, 6, 21, 22, 23, 24, 33];
const CLAN_WEAPONS: [&str; 12] = ["", "bow", "revolver", "shorty", "hunting", "semi", "burst", "pistol", "crossbow", "melee", "bomb", "molotov"];
const FEED_LINES: usize = 6;
const CLAN_GRACE: f64 = 20.0;

type Cnt = IndexMap<i64, i64>;
type Key2 = (String, String);

struct DayRec {
    e2: Cnt,
    win: Option<bool>,
    left: bool,
    late: bool,
}

struct Run {
    c: Connection,
    full: bool,
    rdir: String,
    pl: HashMap<String, (String, String)>, // token -> (key, name)
    ach_done: HashMap<Key2, HashMap<i64, i64>>,
    ach_pend: HashMap<Key2, Cnt>,
    ach_day: HashMap<Key2, DayRec>,
    ach_live_at: HashMap<String, f64>,
    clan_match: HashMap<String, String>,
    clan_last_match: HashMap<String, String>,
    clan_team: HashMap<String, HashMap<String, i64>>,
    clan_map: HashMap<String, String>,
    clan_ended: IndexMap<String, f64>,
    clan_pending: HashMap<String, (String, i64, i64, Cnt)>,
    clan_due: IndexMap<String, f64>,
    clan_credited: HashSet<Key2>,
    clan_rebook: HashSet<Key2>,
    clan_done: HashMap<String, String>,
    clan_win: HashMap<Key2, Option<bool>>,
    quit: HashSet<Key2>,
    kick: HashSet<Key2>,
    exc: HashSet<Key2>,
    late_mult: HashMap<Key2, f64>,
    late_bonus: HashMap<Key2, i64>,
    boost_pick: HashMap<String, i64>,
    boost_active: HashMap<String, i64>,
}

type R<T> = Result<T, String>;

fn de(e: rusqlite::Error) -> String {
    e.to_string()
}

fn sha(s: &str) -> String {
    let mut h = Sha256::new();
    h.update(s.as_bytes());
    let d = h.finalize();
    let mut o = String::new();
    for b in d.iter().take(12) {
        o.push_str(&format!("{:02x}", b));
    }
    o
}

fn to_counts(m: &Cnt) -> Counts {
    m.iter().map(|(k, v)| (*k, *v)).collect()
}

/// `{int(k): int(v) for k, v in d.items()}` of a script dict.
fn counts_in(v: Option<&Value>) -> Cnt {
    let mut m = Cnt::new();
    if let Some(Value::Object(o)) = v {
        for (k, x) in o {
            if let Ok(l) = k.trim().parse::<i64>() {
                m.insert(l, int_of(x));
            }
        }
    }
    m
}

fn cnt_json(m: &Cnt) -> Value {
    let mut o = Map::new();
    for (k, v) in m {
        o.insert(k.to_string(), json!(v));
    }
    Value::Object(o)
}

fn strs(v: &[String]) -> Value {
    Value::Array(v.iter().map(|s| json!(s)).collect())
}

fn truthy(v: Option<&Value>) -> bool {
    match v {
        None => false,
        Some(x) => cl::truthy(x),
    }
}

/// Python `bytes[:n].decode("utf-8", "ignore")`.
fn cut_ignore(b: &[u8], n: usize) -> String {
    let b = &b[..b.len().min(n)];
    let mut out = String::new();
    let mut rest = b;
    loop {
        match std::str::from_utf8(rest) {
            Ok(s) => {
                out.push_str(s);
                break;
            }
            Err(e) => {
                let (ok, bad) = rest.split_at(e.valid_up_to());
                out.push_str(std::str::from_utf8(ok).unwrap_or(""));
                let skip = e.error_len().unwrap_or(bad.len());
                rest = &bad[skip.min(bad.len())..];
                if rest.is_empty() {
                    break;
                }
            }
        }
    }
    out
}

fn id_str(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        other => other.to_string(),
    }
}

impl Run {
    fn key_of(&self, tok: &str) -> R<String> {
        self.pl.get(tok).map(|x| x.0.clone()).ok_or_else(|| "KeyError".to_string())
    }

    // clan_op, daemon.py:2594
    fn state_reply(&self, tok: &str, op: i64) -> R<String> {
        let key = self.key_of(tok)?;
        let st = cl::load(&self.c, &key).map_err(de)?;
        let mut txt = cl::state_text(&st);
        if op == 0xE0 || op == 0xE2 {
            let story = cs::_load(&self.c, &key); // daemon.py:2629
            let tail = perks::wire_text(&perks::active(&st, story.as_ref()), perks::loss_streak(story.as_ref()));
            if !tail.is_empty() {
                txt.push(' ');
                txt.push_str(&tail);
            }
            let story = cs::_load(&self.c, &key); // daemon.py:2642
            let tail = match &story {
                Some(s) => status::wire_text(s),
                None => String::new(),
            };
            if !tail.is_empty() {
                txt.push(' ');
                txt.push_str(&tail);
            }
        }
        Ok(txt)
    }

    // ach_read, daemon.py:1866
    fn ach_read(&self, tok: &str, op: i64) -> Value {
        let txt = (|| -> R<String> {
            let key = self.key_of(tok)?;
            let row = cl::load(&self.c, &key).map_err(de)?;
            let rank = row.get("days_total").map(int_of).unwrap_or(0).div_euclid(7);
            let mut t = ach::wire_text(&self.c, &key, Some(rank));
            t.push_str("LIVE 1\n");
            Ok(t)
        })()
        .unwrap_or_default();
        json!([op, txt])
    }

    // ach_merge, daemon.py:1763
    fn ach_merge(e2: &Cnt, a9: Option<&Cnt>) -> Cnt {
        let mut m = e2.clone();
        if let Some(a) = a9 {
            for (k, v) in a {
                if ACH_E2_ONLY.contains(k) {
                    continue;
                }
                let old = m.get(k).copied().unwrap_or(0);
                m.insert(*k, old.max(*v));
            }
        }
        m
    }

    // ach_book_day, daemon.py:1771
    fn ach_book_day(&mut self, tok: &str, mid: &str, counts: &Cnt, win: Option<bool>, left: bool, late: bool, before: &Value, after: &Value, r: Option<&Value>) -> Value {
        let key = match self.key_of(tok) {
            Ok(k) => k,
            Err(_) => return Value::Null,
        };
        let k2 = (tok.to_string(), mid.to_string());
        let mut done = self.ach_done.remove(&k2).unwrap_or_default();
        let first = !self.ach_day.contains_key(&k2);
        self.ach_day.insert(k2.clone(), DayRec { e2: counts.clone(), win, left, late });
        ach::settle(&self.c, &key, &to_counts(counts), &mut done, &ACH_E2_ONLY);
        let pend = self.ach_pend.remove(&k2);
        let merged = Self::ach_merge(counts, pend.as_ref());
        let mut got = ach::book_match(&self.c, &key, &merged, &mut done, win, left, late, first, None);
        if r.map(|x| cl::truthy(x)).unwrap_or(false) {
            for a in ach::book_clan(&self.c, &key, Some(before), Some(after), r, None, None) {
                if !got.contains(&a) {
                    got.push(a);
                }
            }
        }
        self.ach_done.insert(k2, done);
        strs(&got)
    }

    // clan_match_facts, daemon.py:1692
    fn match_facts(&mut self, tok: &str, mid: &str, r: &Value, parts: i64, counts: &Cnt) -> Value {
        let mut m = Map::new();
        m.insert("map".into(), json!(self.clan_map.get(mid).cloned().unwrap_or_default()));
        m.insert("supplies".into(), json!(r.get("supplies").map(int_of).unwrap_or(0)));
        m.insert("parts".into(), json!(parts));
        m.insert("kills".into(), json!(counts.get(&1).copied().unwrap_or(0)));
        m.insert("mid".into(), json!(mid));
        if !counts.is_empty() {
            for (lab, key) in [(2, "execs"), (3, "revives"), (4, "headshots"), (5, "marked"), (21, "downs"), (22, "deaths"), (23, "streak")] {
                if counts.contains_key(&lab) || lab <= 5 {
                    m.insert(key.into(), json!(counts.get(&lab).copied().unwrap_or(0)));
                }
            }
            let w = counts.get(&24).copied().unwrap_or(0);
            if 0 < w && (w as usize) < CLAN_WEAPONS.len() {
                m.insert("weapon".into(), json!(CLAN_WEAPONS[w as usize]));
            }
        }
        if let Some(Some(b)) = self.clan_win.remove(&(tok.to_string(), mid.to_string())) {
            m.insert("win".into(), json!(b));
        }
        Value::Object(m)
    }

    // clan_finalize, daemon.py:1888
    fn finalize(&mut self, tok: &str, res: &mut Vec<Value>) -> R<()> {
        let Some((mid, parts0, flags, counts0)) = self.clan_pending.remove(tok) else { return Ok(()) };
        let k2 = (tok.to_string(), mid.clone());
        self.clan_done.insert(tok.to_string(), mid.clone()); // daemon.py:1935
        if self.clan_credited.contains(&k2) {
            res.push(json!(["credited"]));
            return Ok(());
        }
        let mut left = (!self.clan_ended.contains_key(&mid) && (flags & 2) == 0) || self.quit.contains(&k2); // daemon.py:1941
        let mut forgive_excused = false;
        if self.kick.contains(&k2) {
            // daemon.py:1973
            if parts0 <= 0 {
                res.push(json!(["kick0"]));
                return Ok(());
            }
            left = false;
            forgive_excused = true;
        }
        if left && self.exc.contains(&k2) && !self.quit.contains(&k2) {
            // daemon.py:1989
            if parts0 <= 0 {
                res.push(json!(["exc0"]));
                return Ok(());
            }
            left = false;
            forgive_excused = true;
        }
        let key = self.key_of(tok)?;
        let dup = cl::load(&self.c, &key).map_err(de)?.get("last_mid").and_then(|x| x.as_str()) == Some(mid.as_str()); // daemon.py:1996
        let prev = cl::snap_get(&self.c, &key, &mid).map_err(de)?;
        let stint_ok = prev.as_ref().map(|p| truthy(p.get("stint"))).unwrap_or(false);
        let rebook = dup && prev.is_some() && stint_ok && (self.clan_rebook.contains(&k2) || truthy(prev.as_ref().and_then(|p| p.get("rejoined"))));
        if dup && !rebook {
            res.push(json!(["dup"]));
            return Ok(());
        }
        let mut parts = parts0;
        let mut counts = counts0;
        let mut late: i64 = 0;
        let mut mult0 = 1.0f64;
        if rebook {
            // daemon.py:2003
            let prev = prev.as_ref().ok_or("prev")?;
            let st1 = &prev["stint"];
            let mut merged = Cnt::new();
            if let Some(o) = st1["counts"].as_object() {
                for (k, v) in o {
                    merged.insert(k.trim().parse::<i64>().map_err(|e| e.to_string())?, int_of(v));
                }
            }
            for (k, v) in &counts {
                if *k == 23 {
                    let o = merged.get(k).copied().unwrap_or(0);
                    merged.insert(*k, o.max(*v));
                } else if *k == 24 {
                    let o = merged.get(k).copied().unwrap_or(0);
                    merged.insert(*k, if o != 0 { o } else { *v });
                } else {
                    let o = merged.get(k).copied().unwrap_or(0);
                    merged.insert(*k, o + *v);
                }
            }
            parts = (parts + int_of(&st1["parts"])).min(100000).max(0);
            counts = merged;
            late = st1.get("late").map(int_of).unwrap_or(0);
            mult0 = st1.get("mult").and_then(|x| x.as_f64()).unwrap_or(1.0);
            forgive_excused = forgive_excused || truthy(st1.get("forgive"));
            self.late_mult.remove(&k2);
            self.late_bonus.remove(&k2);
            cl::snap_restore(&self.c, &key, tok, prev).map_err(de)?;
        } else {
            let _ = cl::snap_take(&self.c, &key, &mid, tok); // daemon.py:2021 (a failure never blocks a day)
        }
        let raw_parts = parts;
        let raw_counts = counts.clone();
        self.clan_credited.insert(k2.clone());
        {
            // daemon.py:2030
            let nm = self.pl.get(tok).map(|x| x.1.clone()).unwrap_or_default();
            let nm = if nm.is_empty() { "?".to_string() } else { nm };
            let team = self.clan_team.get(&mid).and_then(|m| m.get(tok)).copied().unwrap_or(-1);
            let cc = to_counts(&counts);
            let _ = ing::stats_report(&self.c, &mid, tok, &nm, team, Some(&cc), left);
            ing::stats_parts(&self.c, &mid, tok, parts);
        }
        let mult = if rebook { mult0 } else { self.late_mult.remove(&k2).unwrap_or(1.0) }; // daemon.py:2038
        if !left {
            late = if rebook { late } else { self.late_bonus.remove(&k2).unwrap_or(0) };
            if late != 0 {
                parts += late;
            }
        }
        let ach_win: Option<bool> = self.clan_win.get(&k2).copied().flatten(); // daemon.py:2049
        let ach_late = late != 0 || mult > 1.0;
        let before = cl::load(&self.c, &key).map_err(de)?; // daemon.py:2051
        let cc = to_counts(&counts);
        let bottles = if left { 0 } else { counts.get(&33).copied().unwrap_or(0).min(200).max(0) };
        let (st, r) = cl::apply_match(&self.c, &key, &mid, parts, left, Some(&cc), if !left { mult } else { 1.0 }, (mult > 1.0 && !left) || forgive_excused, bottles).map_err(de)?;
        self.clan_rebook.remove(&k2);
        let _ = cl::snap_update(
            &self.c,
            &key,
            &mid,
            &[
                ("rejoined", json!(false)),
                (
                    "stint",
                    json!({"parts": raw_parts, "counts": cnt_json(&raw_counts), "late": if left { 0 } else { late }, "mult": mult,
                           "forgive": forgive_excused || (mult > 1.0 && !left)}),
                ),
            ],
        );
        let mut r = r;
        let r_truthy = r.as_ref().map(|x| cl::truthy(x)).unwrap_or(false);
        if r_truthy && !left && self.clan_map.get(&mid).map(|m| !m.is_empty()).unwrap_or(false) {
            // daemon.py:2061
            let mf = self.match_facts(tok, &mid, r.as_ref().unwrap(), parts, &counts);
            r.as_mut().unwrap()["match"] = mf;
        }
        let mut ex: Vec<&str> = Vec::new();
        if r_truthy && cs::on_day(&self.c, &key, &st, r.as_ref().unwrap()).is_err() {
            // daemon.py:2068
            ex.push("story");
        }
        let mut drawn: Value = Value::Null;
        if r_truthy {
            let rr = r.as_ref().unwrap();
            let cc = to_counts(&counts);
            match perks::on_day(&self.c, &key, &st, rr, Some(&cc), rr.get("match")) {
                // daemon.py:2073
                Ok(d) => drawn = Value::Array(d.into_iter().map(|(c, n)| json!([c, n])).collect()),
                Err(_) => ex.push("perks"),
            }
        }
        let got = self.ach_book_day(tok, &mid, &counts, ach_win, left, ach_late, &before, &st, if r_truthy { r.as_ref() } else { None });
        res.push(json!(["day", left, parts, mult, cl::state_text(&st), r.unwrap_or(Value::Null), drawn, got, ex]));
        Ok(())
    }

    // boost_start, daemon.py:2389
    fn boost_start(&mut self, tok: &str) -> R<Value> {
        self.boost_active.remove(tok);
        let bid = self.boost_pick.remove(tok).unwrap_or(0);
        if bid == 0 {
            return Ok(Value::Null);
        }
        let ok = cl::booster_consume(&self.c, &self.key_of(tok)?, bid).map_err(de)?;
        if ok {
            self.boost_active.insert(tok.to_string(), bid);
        }
        Ok(json!([bid, ok]))
    }

    fn meta(&self, tok: &str) -> R<String> {
        cl::meta_text(&self.c, &self.key_of(tok)?, self.boost_pick.get(tok).copied().unwrap_or(0), self.boost_active.get(tok).copied().unwrap_or(0)).map_err(de)
    }

    fn dump_all(&self) -> R<Vec<(String, Vec<String>, Vec<String>)>> {
        let names: Vec<String> = {
            let mut st = self.c.prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").map_err(de)?;
            let it = st.query_map([], |r| r.get::<_, String>(0)).map_err(de)?;
            it.collect::<Result<_, _>>().map_err(de)?
        };
        let mut out = Vec::new();
        for t in names {
            let mut st = self.c.prepare(&format!("SELECT * FROM \"{}\"", t)).map_err(de)?;
            let cols: Vec<String> = st.column_names().iter().map(|s| s.to_string()).collect();
            let n = cols.len();
            let it = st
                .query_map([], |r| {
                    let mut v = Vec::new();
                    for i in 0..n {
                        let x = r.get_ref(i)?;
                        v.push(match x {
                            rusqlite::types::ValueRef::Blob(b) => Value::String(format!("x'{}'", b.iter().map(|c| format!("{:02x}", c)).collect::<String>())),
                            o => vref_to_json(o),
                        });
                    }
                    Ok(py::dumps_compact(&Value::Array(v)))
                })
                .map_err(de)?;
            let mut rows: Vec<String> = it.collect::<Result<_, _>>().map_err(de)?;
            rows.sort();
            out.push((t, cols, rows));
        }
        Ok(out)
    }

    fn call(&mut self, o: &Value, seed: i64) -> R<Value> {
        let tok = js(o, "tok").to_string();
        let tok = tok.as_str();
        let r: Value = match js(o, "op") {
            "init" => json!(ji(o, "seed") == seed),
            "ensure" => {
                cl::ensure(&self.c).map_err(de)?;
                cs::ensure(&self.c).map_err(de)?;
                ach::ensure(&self.c);
                self.c.execute("CREATE TABLE IF NOT EXISTS players(token TEXT PRIMARY KEY, steamid INTEGER)", []).map_err(de)?;
                json!(true)
            }
            "player" => {
                self.pl.insert(tok.to_string(), (js(o, "key").to_string(), js(o, "name").to_string()));
                let sid: i64 = js(o, "key")[2..].parse().map_err(|_| "sid".to_string())?;
                self.c.execute("INSERT OR REPLACE INTO players(token, steamid) VALUES(?,?)", rusqlite::params![tok, sid]).map_err(de)?;
                json!(true)
            }
            "side" => {
                // clan_side + clan_op 0xE4, daemon.py:1672 / 2600
                let key = self.key_of(tok)?;
                if jb(o, "only_over") && cl::load(&self.c, &key).map_err(de)?.get("status").map(int_of) == Some(cl::ALIVE) {
                    json!("alive")
                } else {
                    let (st, ok) = cl::choose_side(&self.c, &key, ji(o, "fac")).map_err(de)?;
                    let mut okv = json!(ok);
                    let mut got = Value::Null;
                    if ok {
                        if cs::on_new_run(&self.c, &key, ji(o, "fac")).is_err() {
                            okv = json!([true, "story"]);
                        }
                        got = strs(&ach::book_side(&self.c, &key, None));
                    }
                    json!([cl::state_text(&st), okv, got])
                }
            }
            "begin" => {
                // clan_match_started, daemon.py:2337
                let mid = js(o, "mid").to_string();
                self.clan_map.insert(mid.clone(), js(o, "map").to_string());
                let mut team = HashMap::new();
                let ps: Vec<Value> = o["players"].as_array().cloned().unwrap_or_default();
                for p in &ps {
                    team.insert(js(p, "tok").to_string(), ji(p, "team"));
                }
                self.clan_team.insert(mid.clone(), team);
                let mut bs = Vec::new();
                for p in &ps {
                    let t = js(p, "tok").to_string();
                    self.clan_match.insert(t.clone(), mid.clone());
                    self.clan_last_match.insert(t.clone(), mid.clone());
                    let m = jf(p, "mult");
                    if m != 1.0 {
                        self.late_mult.insert((t.clone(), mid.clone()), m);
                    }
                    if ji(p, "late") != 0 {
                        self.late_bonus.insert((t.clone(), mid.clone()), ji(p, "late"));
                    }
                    bs.push(self.boost_start(&t)?);
                }
                Value::Array(bs)
            }
            "live" => {
                // ach_live_op, daemon.py:1836
                let cnt = counts_in(o.get("counts"));
                let mid = self.clan_match.get(tok).cloned();
                let now = clock::now();
                let mut got = Value::Array(vec![]);
                if let Some(mid) = mid {
                    let k2 = (tok.to_string(), mid);
                    if !self.ach_day.contains_key(&k2) && now - self.ach_live_at.get(tok).copied().unwrap_or(0.0) >= 2.0 {
                        self.ach_live_at.insert(tok.to_string(), now);
                        let key = self.key_of(tok)?;
                        let mut done = self.ach_done.remove(&k2).unwrap_or_default();
                        got = strs(&ach::book_live(&self.c, &key, &cnt, &mut done, None));
                        self.ach_done.insert(k2, done);
                    }
                }
                json!([got, self.ach_read(tok, 0xAB)])
            }
            "a9" => {
                // ach_report_op, daemon.py:1790
                let a9 = counts_in(o.get("counts"));
                let mid = self.clan_match.get(tok).cloned().or_else(|| self.clan_last_match.get(tok).cloned());
                let mut got = Value::Array(vec![]);
                if let Some(mid) = mid {
                    let k2 = (tok.to_string(), mid);
                    if !self.ach_day.contains_key(&k2) {
                        let old = self.ach_pend.get(&k2).cloned().unwrap_or_default();
                        let keys: BTreeSet<i64> = a9.keys().chain(old.keys()).copied().collect();
                        let mut m = Cnt::new();
                        for k in keys {
                            m.insert(k, a9.get(&k).copied().unwrap_or(0).max(old.get(&k).copied().unwrap_or(0)));
                        }
                        self.ach_pend.insert(k2, m);
                    } else {
                        let key = self.key_of(tok)?;
                        let day = &self.ach_day[&k2];
                        let merged = Self::ach_merge(&day.e2, Some(&a9));
                        let (win, left, late) = (day.win, day.left, day.late);
                        let mut done = self.ach_done.remove(&k2).unwrap_or_default();
                        got = strs(&ach::book_match(&self.c, &key, &merged, &mut done, win, left, late, false, None));
                        self.ach_done.insert(k2, done);
                    }
                }
                json!([got, self.ach_read(tok, 0xA9)])
            }
            "report" => {
                // clan_report, daemon.py:2203
                let mid = self.clan_match.get(tok).cloned();
                let mut fin = Vec::new();
                if let Some(mid) = mid {
                    if self.clan_done.get(tok) != Some(&mid) {
                        let parts = ji(o, "parts").max(0).min(100000);
                        let counts = counts_in(o.get("counts"));
                        let flags = ji(o, "flags");
                        let k2 = (tok.to_string(), mid.clone());
                        if jb(o, "quit") {
                            self.quit.insert(k2.clone());
                        }
                        if jb(o, "kick") {
                            self.kick.insert(k2.clone());
                        }
                        if jb(o, "exc") {
                            self.exc.insert(k2.clone());
                        }
                        self.clan_pending.insert(tok.to_string(), (mid.clone(), parts, flags, counts));
                        if self.clan_ended.contains_key(&mid) || (flags & 2) != 0 {
                            self.finalize(tok, &mut fin)?;
                        } else {
                            self.clan_due.insert(tok.to_string(), clock::now() + CLAN_GRACE);
                        }
                    }
                }
                json!([fin, self.state_reply(tok, 0xE2)?])
            }
            "tick" => {
                // clan_scheduler, daemon.py:2090
                let mut fin: Vec<Value> = Vec::new();
                let now = clock::now();
                let due: Vec<String> = self.clan_due.iter().filter(|(_, d)| **d <= now).map(|(t, _)| t.clone()).collect();
                for t2 in due {
                    self.clan_due.shift_remove(&t2);
                    fin.push(json!(t2.chars().take(6).collect::<String>()));
                    self.finalize(&t2, &mut fin)?;
                }
                Value::Array(fin)
            }
            "end" => {
                // record_result + clan_match_ended, daemon.py:8330 / 2368
                let mid = js(o, "mid").to_string();
                let winner = ji(o, "winner");
                let players: Vec<String> = o["players"].as_array().map(|a| a.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect()).unwrap_or_default();
                let dep: HashSet<String> = o["departed"].as_array().map(|a| a.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect()).unwrap_or_default();
                if (0..=2).contains(&winner) {
                    for t2 in &players {
                        if !dep.contains(t2) {
                            let tm = self.clan_team[&mid][t2];
                            self.clan_win.insert((t2.clone(), mid.clone()), if winner == 2 { None } else { Some(tm == winner) });
                        }
                    }
                    let ros: Vec<(String, String, i64, bool)> = players
                        .iter()
                        .map(|t2| {
                            let nm = self.pl[t2].1.clone();
                            (t2.clone(), if nm.is_empty() { "?".to_string() } else { nm }, self.clan_team[&mid][t2], dep.contains(t2))
                        })
                        .collect();
                    ing::stats_roster(&self.c, &mid, self.clan_map.get(&mid).map(|s| s.as_str()).unwrap_or(""), &ros, winner).map_err(de)?;
                }
                for p in &players {
                    self.boost_active.remove(p);
                }
                self.clan_ended.insert(mid.clone(), clock::now());
                let mut fin = Vec::new();
                for p in &players {
                    if self.clan_pending.get(p).map(|x| x.0 == mid).unwrap_or(false) {
                        self.finalize(p, &mut fin)?;
                    }
                }
                let now = clock::now();
                let old: Vec<String> = self.clan_ended.iter().filter(|(_, t2)| now - **t2 > 3600.0).map(|(m, _)| m.clone()).collect();
                for m in old {
                    self.clan_ended.shift_remove(&m);
                }
                Value::Array(fin)
            }
            "rejoin" => {
                // clan_rejoin_after_booking, daemon.py:6963
                let mut r = Value::Null;
                if let Some(mid) = self.clan_match.get(tok).cloned() {
                    let key = self.key_of(tok)?;
                    let k2 = (tok.to_string(), mid.clone());
                    if self.clan_done.get(tok) != Some(&mid) {
                        let _ = cl::snap_update(&self.c, &key, &mid, &[("rejoined", json!(true))]);
                        r = json!("plain");
                    } else {
                        self.clan_done.remove(tok);
                        self.clan_credited.remove(&k2);
                        self.exc.remove(&k2);
                        self.quit.remove(&k2);
                        self.clan_rebook.insert(k2);
                        let _ = cl::snap_update(&self.c, &key, &mid, &[("rejoined", json!(true))]);
                        r = json!("rebook");
                    }
                }
                r
            }
            "pick" => {
                // meta_op, daemon.py:2400
                let key = self.key_of(tok)?;
                if js(o, "what") == "booster" {
                    let mut bid = ji(o, "id");
                    if bid != 0 && cl::booster_counts(&self.c, &key).map_err(de)?.get(&bid).copied().unwrap_or(0) <= 0 {
                        bid = 0;
                    }
                    if bid != 0 {
                        self.boost_pick.insert(tok.to_string(), bid);
                    } else {
                        self.boost_pick.remove(tok);
                    }
                } else {
                    cl::mission_choose(&self.c, &key, ji(o, "id")).map_err(de)?;
                }
                json!(self.meta(tok)?)
            }
            "read" => {
                let lang = pln::norm_lang_b(&[(ji(o, "lang") & 255) as u8]);
                let key = self.key_of(tok)?;
                match js(o, "kind") {
                    "E0" => json!(self.state_reply(tok, 0xE0)?),
                    "E8" => json!(self.meta(tok)?),
                    "AA" => self.ach_read(tok, 0xAA),
                    "RANK" => {
                        // rank_of_token, daemon.py:2495
                        let (a, b) = ing::player_rank(&self.c, tok);
                        json!([a, b])
                    }
                    "B4" => {
                        // feed_op, daemon.py:2457 / 2437
                        let feed = cs::live_for_widget(&self.c, &key, FEED_LINES, lang, None);
                        let mut lines: Vec<String> = Vec::new();
                        for (mid, text) in feed.iter().take(FEED_LINES) {
                            let tx = text.replace('\n', " ").replace('|', "/");
                            let tx = cut_ignore(tx.as_bytes(), 150);
                            let idp: String = id_str(mid).replace('|', "").replace('\n', "").chars().take(23).collect();
                            lines.push(format!("{}|{}", idp, tx));
                        }
                        json!(format!("n={}\n{}", lines.len(), lines.join("\n")))
                    }
                    "B6" => {
                        // clan13_op, daemon.py:2533
                        let text = {
                            let _ = cs::ensure_roster(&self.c, &key, true);
                            ing::build(&self.c, &key, lang, ji(o, "section"), ji(o, "seen_id"), ji(o, "seen_day"), Some(&tok.to_lowercase())).unwrap_or_default()
                        };
                        let mut raw: &[u8] = text.as_bytes();
                        let cap: usize = if ji(o, "big") != 0 { 60000 } else { 24000 };
                        if raw.len() > cap {
                            let cut = raw[..cap].iter().rposition(|b| *b == b'\n');
                            raw = match cut {
                                Some(p) if p > 0 => &raw[..p + 1],
                                _ => &raw[..cap],
                            };
                        }
                        let s = String::from_utf8_lossy(raw).into_owned();
                        let mut v = vec![json!(raw.len()), json!(sha(&s))];
                        if self.full {
                            v.push(json!(s));
                        }
                        Value::Array(v)
                    }
                    _ => return Err("read kind".into()),
                }
            }
            "reset" => {
                // admin_clan_reset, daemon.py:10866
                let sid: i64 = self.key_of(tok)?[2..].parse().map_err(|_| "sid".to_string())?;
                let nm = self.pl[tok].1.clone();
                rst::reset(&self.c, &self.rdir, &nm, sid, &[], jb(o, "names"), None)?
            }
            "restore" => {
                // admin_clan_restore, daemon.py:10893
                let sid: i64 = self.key_of(tok)?[2..].parse().map_err(|_| "sid".to_string())?;
                let bl = rst::list_backups(&self.rdir, if sid != 0 { Some(sid) } else { None });
                let arr = bl.as_array().cloned().unwrap_or_default();
                if arr.is_empty() {
                    Value::Null
                } else {
                    let f = js(&arr[(ji(o, "idx") as usize) % arr.len()], "file").to_string();
                    let rr = rst::restore(&self.c, &self.rdir, &f, None)?;
                    json!([rr, f])
                }
            }
            "mark" => {
                let d = self.dump_all()?;
                let mut tabs = Map::new();
                for (t, _cols, rows) in &d {
                    tabs.insert(t.clone(), json!([rows.len(), sha(&rows.join("\n"))]));
                }
                let mut m = Map::new();
                m.insert("day".into(), json!(ji(o, "day")));
                m.insert("tables".into(), Value::Object(tabs));
                if self.full {
                    let mut rr = Map::new();
                    for (t, _cols, rows) in &d {
                        rr.insert(t.clone(), strs(rows));
                    }
                    m.insert("rows".into(), Value::Object(rr));
                    return Ok(json!({"__mark": Value::Object(m)}));
                }
                Value::Object(m)
            }
            other => return Err(format!("unknown op {}", other)),
        };
        Ok(r)
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let dbp = std::env::var("T2R_CL_DB").unwrap_or_else(|_| "/tmp/clan_golden_rs.db".into());
    let _ = std::fs::remove_file(&dbp);
    let _ = std::fs::remove_file(format!("{}-journal", dbp));
    let c = match Connection::open(&dbp) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("selftest clan_golden: {}", e);
            return 1;
        }
    };
    let seed: i64 = std::env::var("T2R_TEST_SEED").ok().and_then(|s| s.trim().parse().ok()).unwrap_or(0);
    let rdir = format!("{}/clan-resets", std::env::var("T2R_GOLD_DIR").unwrap_or_else(|_| "/tmp/clan_golden_rs".into()));
    let mut run = Run {
        c,
        full: std::env::var("T2R_GOLD_FULL").is_ok(),
        rdir: rdir.clone(),
        pl: HashMap::new(),
        ach_done: HashMap::new(),
        ach_pend: HashMap::new(),
        ach_day: HashMap::new(),
        ach_live_at: HashMap::new(),
        clan_match: HashMap::new(),
        clan_last_match: HashMap::new(),
        clan_team: HashMap::new(),
        clan_map: HashMap::new(),
        clan_ended: IndexMap::new(),
        clan_pending: HashMap::new(),
        clan_due: IndexMap::new(),
        clan_credited: HashSet::new(),
        clan_rebook: HashSet::new(),
        clan_done: HashMap::new(),
        clan_win: HashMap::new(),
        quit: HashSet::new(),
        kick: HashSet::new(),
        exc: HashSet::new(),
        late_mult: HashMap::new(),
        late_bonus: HashMap::new(),
        boost_pick: HashMap::new(),
        boost_active: HashMap::new(),
    };
    let _ = std::fs::remove_dir_all(&rdir);
    cl::capture_prints(true);
    let timing = std::env::var("T2R_GOLD_TIME").is_ok();
    let mut spent: HashMap<String, (f64, u64)> = HashMap::new();
    let mut i = 0usize;
    for op in script(input) {
        if let Some(t) = op.get("t").and_then(|x| x.as_f64()) {
            clock::freeze(t);
        }
        let mut m = Map::new();
        m.insert("i".into(), Value::from(i as i64));
        let t0 = std::time::Instant::now();
        let res = run.call(&op, seed);
        if timing {
            let e = spent.entry(format!("{}:{}", js(&op, "op"), js(&op, "kind"))).or_insert((0.0, 0));
            e.0 += t0.elapsed().as_secs_f64();
            e.1 += 1;
        }
        let prints = cl::take_prints();
        match res {
            Ok(r) => {
                if let Some(mk) = r.get("__mark") {
                    // full mark: the python side prints "rows" after "r"
                    let mk = mk.clone();
                    let mut rr = Map::new();
                    rr.insert("day".into(), mk["day"].clone());
                    rr.insert("tables".into(), mk["tables"].clone());
                    m.insert("r".into(), Value::Object(rr));
                    m.insert("rows".into(), mk["rows"].clone());
                } else {
                    m.insert("r".into(), r);
                }
                if !prints.is_empty() {
                    m.insert("p".into(), strs(&prints));
                }
            }
            Err(e) => {
                if std::env::var("T2R_TRACE").is_ok() {
                    eprintln!("call {} {}: {}", i, js(&op, "op"), e);
                }
                m.clear();
                m.insert("i".into(), Value::from(i as i64));
                m.insert("err".into(), json!(js(&op, "op")));
            }
        }
        let _ = writeln!(out, "{}", py::dumps_compact(&Value::Object(m)));
        i += 1;
    }
    cl::capture_prints(false);
    if timing {
        let mut v: Vec<_> = spent.into_iter().collect();
        v.sort_by(|a, b| b.1 .0.partial_cmp(&a.1 .0).unwrap_or(std::cmp::Ordering::Equal));
        for (k, (t, n)) in v {
            eprintln!("time {:<12} {:9.3} s  {:6} calls  {:8.2} ms/call", k, t, n, t * 1000.0 / n as f64);
        }
    }
    if let Ok(d) = run.dump_all() {
        for (t, cols, rows) in d {
            let _ = writeln!(out, "{}", py::dumps_compact(&json!({"table": t, "cols": cols, "rows": rows})));
        }
    }
    let mut files = Vec::new();
    if let Ok(rd) = std::fs::read_dir(&rdir) {
        let mut fs: Vec<String> = rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        fs.sort();
        for f in fs {
            let b = std::fs::read(std::path::Path::new(&rdir).join(&f)).unwrap_or_default();
            files.push(json!([f, sha(&String::from_utf8_lossy(&b))]));
        }
    }
    let _ = writeln!(out, "{}", py::dumps_compact(&json!({"files": files})));
    0
}
