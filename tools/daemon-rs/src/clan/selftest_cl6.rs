//! `t2r-daemon selftest clan_perks | clan_status | clan_ingame | achievements` (one script format, all four): the Rust side of verify/py/clan_cl6_harness.py
//! (same script, same output bytes). One call per input line -> `{"i", "r"[, "h"][, "err"]}`; at the end the final states and a dump of every table.
//! clan_journal / clan_journal_entries are the deterministic doubles of ext_clan_perks (the same ones the Python harness installs).
use crate::c_util::{jb, ji, js, script};
use crate::clan::achievements as ach;
use crate::clan::clan::{self as cl, int_of, vref_to_json, Counts};
use crate::clan::{clan_chronicle as ch, clan_ingame as ing, clan_perks as perks, clan_status as status, clan_story as cs, ext_clan_perks as extp};
use crate::{clock, py};
use indexmap::IndexMap;
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::collections::HashMap;
use std::io::{BufRead, Write};

fn crc_of(v: &Value) -> u32 {
    t2r_clanstory::crc::crc32(py::dumps_compact(v).as_bytes())
}

/// Python `d = st; for k in path[:-1]: d = d[k]; d[path[-1]] = v` (a missing key raises).
fn set_path(st: &mut Value, path: &[Value], v: &Value) -> Result<(), String> {
    let mut cur = st;
    for k in &path[..path.len().saturating_sub(1)] {
        cur = match k {
            Value::String(s) => cur.get_mut(s.as_str()).ok_or("KeyError")?,
            Value::Number(n) => cur.get_mut(n.as_u64().unwrap_or(0) as usize).ok_or("IndexError")?,
            _ => return Err("TypeError".into()),
        };
    }
    match (path.last(), cur.as_object_mut()) {
        (Some(Value::String(s)), Some(o)) => {
            o.insert(s.clone(), v.clone());
            Ok(())
        }
        _ => Err("TypeError".into()),
    }
}

/// {"label": n} of a script -> Counts (non-integer labels dropped like the Python harness).
fn counts_of(v: Option<&Value>) -> Counts {
    let mut m = Counts::new();
    if let Some(Value::Object(o)) = v {
        for (k, x) in o {
            if let Ok(l) = k.trim().parse::<i64>() {
                m.insert(l, int_of(x));
            }
        }
    }
    m
}

/// Ordered {"label": n} (the caller's order is the DB row order).
fn counts_ordered(v: Option<&Value>) -> IndexMap<i64, i64> {
    let mut m = IndexMap::new();
    if let Some(Value::Object(o)) = v {
        for (k, x) in o {
            if let Ok(l) = k.trim().parse::<i64>() {
                m.insert(l, int_of(x));
            }
        }
    }
    m
}

fn done_json(d: &HashMap<i64, i64>) -> Value {
    let mut ks: Vec<&i64> = d.keys().collect();
    ks.sort();
    let mut m = Map::new();
    for k in ks {
        m.insert(k.to_string(), json!(d[k]));
    }
    Value::Object(m)
}

fn opt_f(o: &Value, k: &str) -> Option<f64> {
    o.get(k).and_then(|x| x.as_f64())
}

fn opt_b(o: &Value, k: &str) -> Option<bool> {
    o.get(k).and_then(|x| x.as_bool())
}

struct Run {
    c: Connection,
    sts: Vec<(String, Value)>,
    done: HashMap<String, HashMap<i64, i64>>,
}

impl Run {
    fn st(&mut self, k: &str) -> Option<&mut Value> {
        self.sts.iter_mut().find(|(n, _)| n == k).map(|(_, v)| v)
    }
    fn st_ref(&self, k: &str) -> Option<&Value> {
        self.sts.iter().find(|(n, _)| n == k).map(|(_, v)| v)
    }
}

type Res = Result<(Value, Option<u32>), String>;

fn call(run: &mut Run, o: &Value) -> Res {
    let k = js(o, "k").to_string();
    let lang = js(o, "lang").to_string();
    let r = match js(o, "op") {
        "load" => {
            if let Some(x) = run.sts.iter_mut().find(|(n, _)| *n == k) {
                x.1 = o["st"].clone();
            } else {
                run.sts.push((k, o["st"].clone()));
            }
            return Ok((json!(true), None));
        }
        "set" => {
            if let Some(st) = run.st(&k) {
                set_path(st, o["path"].as_array().map(|a| a.as_slice()).unwrap_or(&[]), &o["v"])?;
                return Ok((Value::Null, Some(crc_of(st))));
            }
            return Ok((Value::Null, None));
        }
        "pset" => {
            if let Some(st) = run.st(&k) {
                if let Some(ppl) = st.get_mut("people").and_then(|x| x.as_array_mut()) {
                    if let Some(p) = ppl.iter_mut().find(|p| ji(p, "i") == ji(o, "i")) {
                        p[js(o, "f")] = o["v"].clone();
                    }
                }
                return Ok((Value::Null, Some(crc_of(st))));
            }
            return Ok((Value::Null, None));
        }
        "kill" => {
            if let Some(st) = run.st(&k) {
                let pos = st.get("people").and_then(|x| x.as_array()).and_then(|a| a.iter().position(|p| ji(p, "i") == ji(o, "i")));
                if let Some(ix) = pos {
                    let p = st["people"].as_array_mut().map(|a| a.remove(ix)).unwrap_or(Value::Null);
                    let rec = json!({"i": p["i"], "n": p["n"], "g": p["g"], "r": p["r"], "d": o["d"], "c": o["c"], "il": p["il"], "k": 0});
                    if let Some(d) = st.get_mut("dead").and_then(|x| x.as_array_mut()) {
                        d.push(rec);
                    }
                }
                return Ok((Value::Null, Some(crc_of(st))));
            }
            return Ok((Value::Null, None));
        }
        "add" => {
            if let Some(st) = run.st(&k) {
                if let Some(a) = st.get_mut("people").and_then(|x| x.as_array_mut()) {
                    a.push(o["p"].clone());
                }
                return Ok((Value::Null, Some(crc_of(st))));
            }
            return Ok((Value::Null, None));
        }
        "sql" => {
            let args: Vec<rusqlite::types::Value> = o.get("args").and_then(|a| a.as_array()).map(|a| a.iter().map(cl::to_sql).collect()).unwrap_or_default();
            run.c.execute(js(o, "sql"), rusqlite::params_from_iter(args)).map_err(|e| e.to_string())?;
            return Ok((json!(true), None));
        }
        "rows" => {
            let cols: Vec<&str> = o["cols"].as_array().map(|a| a.iter().filter_map(|x| x.as_str()).collect()).unwrap_or_default();
            let sql = format!("INSERT OR REPLACE INTO {}({}) VALUES({})", js(o, "table"), cols.join(","), vec!["?"; cols.len()].join(","));
            for row in o["rows"].as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
                let args: Vec<rusqlite::types::Value> = row.as_array().map(|a| a.iter().map(cl::to_sql).collect()).unwrap_or_default();
                run.c.execute(&sql, rusqlite::params_from_iter(args)).map_err(|e| e.to_string())?;
            }
            return Ok((json!(true), None));
        }
        "ensure" => {
            cl::ensure(&run.c).map_err(|e| e.to_string())?;
            cs::ensure(&run.c).map_err(|e| e.to_string())?;
            return Ok((json!(true), None));
        }
        // ------------------------------------------------------------ clan_status
        "s_code" => {
            let st = run.st_ref(&k).ok_or("no state")?;
            let ppl = st.get("people").and_then(|x| x.as_array()).cloned().unwrap_or_default();
            Value::Array(ppl.iter().map(|p| json!(status::code_of(st, p))).collect())
        }
        "s_observe" => {
            let rows: Vec<Value> = o["rows"].as_array().cloned().unwrap_or_default();
            let st = run.st(&k).ok_or("no state")?;
            let new = status::observe(st, &rows)?;
            let h = crc_of(st);
            return Ok((Value::Array(new), Some(h)));
        }
        "s_pending" => Value::Array(status::pending(run.st_ref(&k).ok_or("no state")?)),
        "s_public" => status::public(run.st_ref(&k).ok_or("no state")?, js(o, "fk"), &lang)?,
        "s_wire" => json!(status::wire_text(run.st_ref(&k).ok_or("no state")?)),
        "s_line" => json!(status::story_line(&o["e"], &lang)),
        "s_images" => {
            let st = if jb(o, "nost") { None } else { run.st_ref(&k) };
            let e = &o["e"];
            json!([status::images(e, js(o, "fk"), st), status::sex_of(e), status::look_of(e), status::event_id(e)])
        }
        // ------------------------------------------------------------ clan_perks
        "p_active" => {
            let story = if jb(o, "nostory") { None } else { run.st_ref(&k) };
            Value::Array(perks::active(&o["clan"], story).iter().map(|p| p.to_json()).collect())
        }
        "p_wire" => {
            let story = if jb(o, "nostory") { None } else { run.st_ref(&k) };
            let pk = perks::active(&o["clan"], story);
            let streak = if o.get("streak").map(|x| x.is_null()).unwrap_or(true) { perks::loss_streak(story) } else { ji(o, "streak") };
            json!([perks::wire_text(&pk, streak), perks::loss_streak(story)])
        }
        "p_need" => {
            let c = &o["clan"];
            json!([perks::need_of(c), perks::stock_of(c), perks::surplus(c)])
        }
        "p_uses" => {
            let u = perks::uses_of(Some(&counts_of(o.get("counts"))));
            let mut m = Map::new();
            for (c, n) in u {
                m.insert(c.to_string(), json!(n));
            }
            Value::Object(m)
        }
        "p_credit_day" => {
            let st = run.st(&k).ok_or("no state")?;
            perks::credit_day(st, o.get("got").filter(|x| !x.is_null()), ji(o, "day"));
            return Ok((Value::Null, Some(crc_of(st))));
        }
        "p_credit_of" => {
            let st = run.st_ref(&k);
            perks::credit_of(st, js(o, "cat")).unwrap_or(Value::Null)
        }
        "p_line" => json!(perks::line(js(o, "cat"), &o["who"], js(o, "map"), &lang, ji(o, "day"))?),
        "p_render" => match perks::render_msg(js(o, "lid"), o.get("params").unwrap_or(&Value::Null), &lang) {
            Some(s) => json!(s),
            None => Value::Null,
        },
        "p_on_day" => {
            let drawn = perks::on_day(&run.c, js(o, "tid"), &o["newc"], &o["r"], Some(&counts_of(o.get("counts"))), o.get("match").filter(|x| !x.is_null()))?;
            Value::Array(drawn.into_iter().map(|(c, n)| json!([c, n])).collect())
        }
        // ------------------------------------------------------------ achievements
        "a_table" => {
            ach::check_table()?;
            json!([ach::ach().len(), ach::live_rules().len(), ach::ext_header()])
        }
        "a_book" => {
            let mut done = run.done.remove(js(o, "d")).unwrap_or_default();
            let r = ach::book_match(&run.c, js(o, "key"), &counts_ordered(o.get("counts")), &mut done, opt_b(o, "win"), jb(o, "left"), jb(o, "late"), jb(o, "first"), opt_f(o, "now"));
            let dj = done_json(&done);
            run.done.insert(js(o, "d").to_string(), done);
            json!([r, dj])
        }
        "a_live" => {
            let mut done = run.done.remove(js(o, "d")).unwrap_or_default();
            let r = ach::book_live(&run.c, js(o, "key"), &counts_ordered(o.get("counts")), &mut done, opt_f(o, "now"));
            let dj = done_json(&done);
            run.done.insert(js(o, "d").to_string(), done);
            json!([r, dj])
        }
        "a_settle" => {
            let mut done = run.done.remove(js(o, "d")).unwrap_or_default();
            let labels: Vec<i64> = o["labels"].as_array().map(|a| a.iter().map(int_of).collect()).unwrap_or_default();
            ach::settle(&run.c, js(o, "key"), &counts_of(o.get("auth")), &mut done, &labels);
            let dj = done_json(&done);
            run.done.insert(js(o, "d").to_string(), done);
            dj
        }
        "a_side" => json!(ach::book_side(&run.c, js(o, "key"), opt_f(o, "now"))),
        "a_clan" => json!(ach::book_clan(&run.c, js(o, "key"), o.get("before").filter(|x| !x.is_null()), o.get("after").filter(|x| !x.is_null()), o.get("r").filter(|x| !x.is_null()), o.get("rank").and_then(|x| x.as_i64()), opt_f(o, "now"))),
        "a_san" => {
            let s = ach::sanitize(&counts_ordered(o.get("counts")));
            let mut m = Map::new();
            for (k, v) in s {
                m.insert(k.to_string(), json!(v));
            }
            Value::Object(m)
        }
        "a_wire" => json!(ach::wire_text(&run.c, js(o, "key"), o.get("rank").and_then(|x| x.as_i64()))),
        "a_pub" => Value::Array(ach::public_rows(&run.c, js(o, "key"), o.get("rank").and_then(|x| x.as_i64()))),
        "a_rows" => Value::Array(ach::rows(&run.c, js(o, "key"), o.get("rank").and_then(|x| x.as_i64())).into_iter().map(|(a, p, t)| json!([a.id, p, t])).collect()),
        // ------------------------------------------------------------ clan_ingame
        "i_build" => {
            let tok = o.get("token").and_then(|x| x.as_str());
            json!(ing::build(&run.c, js(o, "tid"), &lang, ji(o, "section"), ji(o, "seen_id"), ji(o, "seen_day"), tok)?)
        }
        "i_roster" => {
            let rows: Vec<(String, String, i64, bool)> = o["rows"].as_array().map(|a| a.iter().map(|r| (js(r, "tok").to_string(), js(r, "name").to_string(), ji(r, "team"), jb(r, "left"))).collect()).unwrap_or_default();
            ing::stats_roster(&run.c, js(o, "mid"), js(o, "map"), &rows, ji(o, "winner")).map_err(|e| e.to_string())?;
            json!(true)
        }
        "i_report" => {
            let cnt = counts_of(o.get("counts"));
            ing::stats_report(&run.c, js(o, "mid"), js(o, "token"), js(o, "name"), ji(o, "team"), Some(&cnt), jb(o, "left")).map_err(|e| e.to_string())?;
            json!(true)
        }
        "i_parts" => {
            ing::stats_parts(&run.c, js(o, "mid"), js(o, "token"), ji(o, "parts"));
            json!(true)
        }
        "i_heads" => {
            let rows: Vec<(String, i64)> = o["rows"].as_array().map(|a| a.iter().map(|r| (r.get(0).and_then(|x| x.as_str()).unwrap_or("").to_string(), r.get(1).map(int_of).unwrap_or(0))).collect()).unwrap_or_default();
            let h = ing::stats_heads(&rows);
            let mut ks: Vec<&String> = h.keys().collect();
            ks.sort();
            let mut m = Map::new();
            for k in ks {
                m.insert(k.clone(), json!(h[k]));
            }
            json!([Value::Object(m), ing::stats_head_pool().len()])
        }
        "i_feed" => {
            let feed = cs::read_feed(&run.c, js(o, "tid"), ji(o, "limit"), 0, &lang, jb(o, "cb"), false, jb(o, "cb"));
            let rows = if !feed.is_empty() && feed[0].get("card").is_none() { cs::story_rows(&feed) } else { feed.clone() };
            json!([feed, rows])
        }
        "i_rank" => {
            let (a, b) = ing::player_rank(&run.c, js(o, "token"));
            json!([a, b])
        }
        "portrait" => json!(ch::fit_expr(ji(o, "model"), js(o, "expr"))),
        other => return Err(format!("unknown op {}", other)),
    };
    Ok((r, None))
}

fn dump_table(c: &Connection, name: &str, order: &str) -> Value {
    let sql = format!("SELECT * FROM {} ORDER BY {}", name, order);
    let mut rows = Vec::new();
    if let Ok(mut st) = c.prepare(&sql) {
        let n = st.column_count();
        if let Ok(it) = st.query_map([], |r| {
            let mut v = Vec::new();
            for i in 0..n {
                v.push(vref_to_json(r.get_ref(i)?));
            }
            Ok(Value::Array(v))
        }) {
            for r in it.flatten() {
                rows.push(r);
            }
        }
    }
    json!({"table": name, "rows": rows})
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let dbp = std::env::var("T2R_CL_DB").unwrap_or_else(|_| "/tmp/clan_cl6_rs.db".into());
    let _ = std::fs::remove_file(&dbp);
    let c = match Connection::open(&dbp) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("selftest clan_cl6: {}", e);
            return 1;
        }
    };
    extp::doubles(true);
    let mut run = Run { c, sts: Vec::new(), done: HashMap::new() };
    let mut i = 0usize;
    for op in script(input) {
        if let Some(t) = op.get("t").and_then(|x| x.as_f64()) {
            clock::freeze(t);
        }
        let mut m = Map::new();
        m.insert("i".into(), Value::from(i as i64));
        match call(&mut run, &op) {
            Ok((r, h)) => {
                m.insert("r".into(), r);
                if let Some(h) = h {
                    m.insert("h".into(), json!(h));
                    if std::env::var("T2R_FULLST").is_ok() {
                        m.insert("st".into(), run.st_ref(js(&op, "k")).cloned().unwrap_or(Value::Null));
                    }
                }
            }
            Err(e) => {
                if std::env::var("T2R_TRACE").is_ok() {
                    eprintln!("call {} {}: {}", i, js(&op, "op"), e);
                }
                m.insert("r".into(), Value::Null);
                m.insert("err".into(), json!(true));
            }
        }
        let _ = writeln!(out, "{}", py::dumps_compact(&Value::Object(m)));
        i += 1;
    }
    let mut fin = Map::new();
    for (k, v) in &run.sts {
        fin.insert(k.clone(), v.clone());
    }
    let _ = writeln!(out, "{}", py::dumps_compact(&json!({"i": -1, "final": Value::Object(fin)})));
    for (t, o) in [("clans", "token_id"), ("clan_story", "token_id"), ("clan_feed", "token_id,id"), ("match_stats", "mid,token"), ("ach_count", "key,lab"), ("ach", "key,aid")] {
        let _ = writeln!(out, "{}", py::dumps_compact(&dump_table(&run.c, t, o)));
    }
    0
}
