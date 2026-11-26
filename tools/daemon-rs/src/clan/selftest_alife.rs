//! `t2r-daemon selftest clan_alife` / `selftest clan_journal`: the Rust side of verify/py/clan_alife_harness.py / clan_journal_harness.py (same script, same output
//! bytes). The clan days run through the real pipeline (clan_story::on_day -> clan_alife::on_day -> clan_journal::on_day); the read functions are extra ops.
use crate::c_util::{jb, ji, js, script};
use crate::clan::clan::{self as cl, vref_to_json};
use crate::clan::clan_alife as al;
use crate::clan::clan_journal as jn;
use crate::clan::clan_journal_entries as je;
use crate::clan::clan_story as cs;
use crate::clan::ext_clan_story as ext;
use crate::{clock, py};
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::io::{BufRead, Write};

fn lines_value(v: Vec<(Value, String)>) -> Value {
    Value::Array(v.into_iter().map(|(i, t)| Value::Array(vec![i, Value::String(t)])).collect())
}

fn exc() -> Value {
    Value::String("EXC".into())
}

fn opt_str(o: Option<String>) -> Value {
    o.map(Value::String).unwrap_or(Value::Null)
}

fn call(c: &Connection, op: &Value) -> Value {
    let tid = js(op, "tid");
    let lang = js(op, "lang");
    let now = op.get("now").and_then(|x| x.as_f64());
    match js(op, "op") {
        "cfg" => {
            std::env::set_var("T2R_CLAN_ALIFE", if jb(op, "alife") { "1" } else { "0" });
            Value::Bool(true)
        }
        "clan" => match cl::save(c, tid, op.get("clan").unwrap_or(&Value::Null)) {
            Ok(()) => Value::Bool(true),
            Err(_) => exc(),
        },
        "new_run" => match cs::on_new_run(c, tid, ji(op, "fac")) {
            Ok(v) => Value::Array(v),
            Err(_) => exc(),
        },
        "day" => match cs::on_day(c, tid, op.get("newc").unwrap_or(&Value::Null), op.get("r").unwrap_or(&Value::Null)) {
            Ok(v) => Value::Array(v),
            Err(_) => exc(),
        },
        "patch" => match cs::_load(c, tid) {
            Some(mut st) => {
                let key = js(op, "key");
                match op.get("value") {
                    Some(v) if !v.is_null() => st[key] = v.clone(),
                    _ => {
                        if let Some(o) = st.as_object_mut() {
                            o.shift_remove(key);
                        }
                    }
                }
                let _ = cs::_save(c, tid, &st);
                Value::Bool(true)
            }
            None => Value::Bool(false),
        },
        "stat" => match cs::_load(c, tid) {
            Some(mut st) => {
                if st.get("ch").is_none() {
                    st["ch"] = json!({});
                }
                if st["ch"].get("stat").is_none() {
                    st["ch"]["stat"] = json!({});
                }
                st["ch"]["stat"]["ev"] = op.get("ev").cloned().unwrap_or(Value::Null);
                let _ = cs::_save(c, tid, &st);
                Value::Bool(true)
            }
            None => Value::Bool(false),
        },
        "menu" => lines_value(cs::menu_lines(c, tid, ji(op, "n") as usize, lang, now, jb(op, "save"))),
        "live" => match cs::live_lines(c, tid, ji(op, "n") as usize, lang, now, jb(op, "save")) {
            Ok(v) => lines_value(v),
            Err(()) => exc(),
        },
        "slot" => lines_value(cs::pick_slot(c, tid, ji(op, "slot"), ji(op, "n") as usize, lang, now)),
        "feed" => Value::Array(cs::read_feed(c, tid, ji(op, "limit"), ji(op, "since"), lang, jb(op, "cb"), jb(op, "who"), jb(op, "fold"))),
        "latest" => Value::Array(cs::latest(c, tid, ji(op, "n"), ji(op, "since"), lang)),
        "people" => cs::read_people(c, tid, ji(op, "limit") as usize, lang),
        "journal" => {
            let rows = cs::read_feed(c, tid, 100, 0, lang, true, false, true);
            Value::Array(cs::journal(&rows, ji(op, "max") as usize))
        }
        "state" => cs::_load(c, tid).unwrap_or(Value::Null),
        "roster" => cs::ensure_roster(c, tid, jb(op, "save")).unwrap_or(Value::Null),
        "al_ticker" => match cs::_load(c, tid) {
            Some(st) => match al::ticker_lines(c, tid, &st, ji(op, "slot"), ji(op, "n") as usize, lang, None, jb(op, "save")) {
                Ok(v) => lines_value(v),
                Err(_) => exc(),
            },
            None => Value::Null,
        },
        "al_state" => al::read_state(c, tid, lang).unwrap_or_else(|_| exc()),
        "al_public" => al::public(c, tid, lang),
        "al_summary" => al::summary(c, tid),
        "al_view" => al::view(c, tid),
        "al_feed_render" => {
            let mut res = Vec::new();
            let rows: Vec<(String, Option<String>)> = match c.prepare("SELECT lid, params FROM clan_feed WHERE token_id=? AND lid LIKE 'al/%' ORDER BY id DESC LIMIT 8") {
                Ok(mut s) => match s.query_map([tid], |r| Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?))) {
                    Ok(it) => it.flatten().collect(),
                    Err(_) => Vec::new(),
                },
                Err(_) => Vec::new(),
            };
            for (lid, params) in rows {
                let p = params.filter(|s| !s.is_empty()).and_then(|s| serde_json::from_str::<Value>(&s).ok()).unwrap_or(Value::Null);
                res.push(json!([opt_str(al::render_msg(&lid, &p, "en")), opt_str(al::render_msg(&lid, &p, "pl")), opt_str(al::render_msg(&lid, &p, lang))]));
            }
            for (lid, p) in [("al/zz", json!({})), ("al/", json!({"p": {}})), ("nope", json!({})), ("al/wx_rain~00000000", json!({"p": {}})), ("", Value::Null)] {
                res.push(opt_str(al::render_msg(lid, &p, lang)));
            }
            Value::Array(res)
        }
        "jn_entries" => Value::Array(jn::entries(c, tid, lang, op.get("dd").and_then(|d| d.as_i64()))),
        "jn_perk" => match cs::_load(c, tid) {
            Some(mut st) => {
                let people = st.get("people").and_then(|p| p.as_array()).cloned().unwrap_or_default();
                if people.is_empty() {
                    return Value::Null;
                }
                let who = people[py::pymod(ji(op, "who"), people.len() as i64) as usize].clone();
                let day = (cl::int_of(&st["day"]) + ji(op, "doff")).max(0);
                let res = jn::add_perk(c, tid, &mut st, day, js(op, "cat"), &who, js(op, "map"));
                let _ = cs::_save(c, tid, &st);
                match res {
                    Ok(b) => Value::Bool(b),
                    Err(_) => exc(),
                }
            }
            None => Value::Null,
        },
        "jn_render" => {
            let mut res: Vec<Value> = Vec::new();
            let rows: Vec<(i64, String)> = match c.prepare("SELECT day, data FROM clan_journal WHERE token_id=? ORDER BY day DESC LIMIT 3") {
                Ok(mut s) => match s.query_map([tid], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?))) {
                    Ok(it) => it.flatten().collect(),
                    Err(_) => Vec::new(),
                },
                Err(_) => Vec::new(),
            };
            for (_d, data) in rows {
                let Ok(rec) = serde_json::from_str::<Value>(&data) else { return exc() };
                if let Some(pa) = rec.get("pa").and_then(|p| p.as_array()) {
                    for p in pa {
                        for q in p.get(2).and_then(|s| s.as_array()).map(|a| a.as_slice()).unwrap_or(&[]) {
                            res.push(json!([jn::render(q, lang), jn::render(q, "en")]));
                        }
                    }
                }
                if let Some(ti) = rec.get("ti").filter(|t| cl::truthy(t)) {
                    res.push(Value::String(jn::render(ti, lang)));
                }
            }
            res.push(Value::String(jn::render(&json!(["jn/zz.1", {}]), lang)));
            res.push(Value::String(jn::render(&json!(["jn/raid_win.9999", {"p": {}, "s": {}}]), lang)));
            res.push(Value::String(jn::render(&json!("bad"), lang)));
            Value::Array(res)
        }
        "je" => match cs::_load(c, tid) {
            Some(st) => {
                let rows = cs::story_rows(&cs::read_feed(c, tid, 100, 0, lang, true, false, true));
                let roster: Vec<Value> = st["people"].as_array().cloned().unwrap_or_default().iter().map(|p| json!({"name": p["n"].clone(), "portrait": format!("p{}", cl::int_of(&p["i"]))})).collect();
                Value::Array(je::compose_entries(&rows, &roster, ji(op, "seen")))
            }
            None => Value::Null,
        },
        _ => Value::Null,
    }
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
    let dbp = std::env::var("T2R_CL_DB").unwrap_or_else(|_| "/tmp/clan_alife_rs.db".into());
    let _ = std::fs::remove_file(&dbp);
    let c = match Connection::open(&dbp) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("selftest clan_alife: {}", e);
            return 1;
        }
    };
    std::env::set_var("T2R_CLAN_ALIFE", "1");
    std::env::set_var("T2R_CLAN_CHRONICLE", "0"); // the Python harness switches the chronicle off too (it is not part of this module's test)
    ext::set_test_real(true);
    crate::clan::ext_clan_alife::set_test_fake(true);
    let mut i = 0usize;
    for op in script(input) {
        if let Some(t) = op.get("t").and_then(|x| x.as_f64()) {
            clock::freeze(t);
        }
        let r = call(&c, &op);
        let mut m = Map::new();
        m.insert("i".into(), Value::from(i as i64));
        m.insert("r".into(), r);
        let _ = writeln!(out, "{}", py::dumps(&Value::Object(m)));
        i += 1;
    }
    for (t, o) in [("clans", "token_id"), ("clan_story", "token_id"), ("clan_feed", "token_id,id"), ("clan_alife", "token_id"), ("clan_journal", "token_id,day")] {
        let _ = writeln!(out, "{}", py::dumps(&dump_table(&c, t, o)));
    }
    0
}
