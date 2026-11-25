//! `t2r-daemon selftest clan_story`: the Rust side of verify/py/clan_story_harness.py (same script, same output bytes).
//! One JSON call per input line -> one result line `{"i": n, "r": result}`; at the end a dump of every table the story writes.
//! The other clan modules (alife, chronicle, journal, arcs, perks, saga) are the deterministic fakes of ext_clan_story.
use crate::c_util::{canon, jb, ji, js, script};
use crate::clan::clan::{self as cl, vref_to_json};
use crate::clan::clan_story as cs;
use crate::clan::clan_story_pl as pln;
use crate::clan::ext_clan_story as ext;
use crate::{clock, py};
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::collections::HashSet;
use std::io::{BufRead, Write};

fn lines_value(v: Vec<(Value, String)>) -> Value {
    Value::Array(v.into_iter().map(|(i, t)| Value::Array(vec![i, Value::String(t)])).collect())
}

fn opt_str(o: Option<String>) -> Value {
    match o {
        Some(s) => Value::String(s),
        None => Value::Null,
    }
}

fn vars_value(v: Option<cs::Vars>) -> Value {
    match v {
        Some(m) => canon(Value::Object(m.into_iter().map(|(k, x)| (k, Value::String(x))).collect())),
        None => Value::Null,
    }
}

fn call(c: &Connection, op: &Value, seen: &mut HashSet<i64>) -> Value {
    let tid = js(op, "tid");
    let lang = js(op, "lang");
    let now = op.get("now").and_then(|x| x.as_f64());
    match js(op, "op") {
        "cfg" => {
            ext::set_test_switches(jb(op, "alife"), jb(op, "chron"));
            Value::Bool(true)
        }
        "clan" => match cl::save(c, tid, op.get("clan").unwrap_or(&Value::Null)) {
            Ok(()) => Value::Bool(true),
            Err(_) => Value::String("EXC".into()),
        },
        "new_run" => match cs::on_new_run(c, tid, ji(op, "fac")) {
            Ok(v) => Value::Array(v),
            Err(_) => Value::String("EXC".into()),
        },
        "day" => match cs::on_day(c, tid, op.get("newc").unwrap_or(&Value::Null), op.get("r").unwrap_or(&Value::Null)) {
            Ok(v) => Value::Array(v),
            Err(_) => Value::String("EXC".into()),
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
        "menu" => lines_value(cs::menu_lines(c, tid, ji(op, "n") as usize, lang, now, jb(op, "save"))),
        "live" => match cs::live_lines(c, tid, ji(op, "n") as usize, lang, now, jb(op, "save")) {
            Ok(v) => lines_value(v),
            Err(()) => Value::String("EXC".into()),
        },
        "slot" => lines_value(cs::pick_slot(c, tid, ji(op, "slot"), ji(op, "n") as usize, lang, now)),
        "feed" => Value::Array(cs::read_feed(c, tid, ji(op, "limit"), ji(op, "since"), lang, jb(op, "cb"), jb(op, "who"), jb(op, "fold"))),
        "latest" => Value::Array(cs::latest(c, tid, ji(op, "n"), ji(op, "since"), lang)),
        "people" => cs::read_people(c, tid, ji(op, "limit") as usize, lang),
        "journal" => {
            let rows = cs::read_feed(c, tid, 100, 0, lang, true, false, true);
            Value::Array(cs::journal(&rows, ji(op, "max") as usize))
        }
        "render" => opt_str(cs::render_msg(js(op, "lid"), op.get("params").unwrap_or(&Value::Null), lang)),
        "short" => {
            let t = js(op, "text");
            json!({"short": cs::short_text(t, ji(op, "limit") as usize), "widget": cs::widget_text(t)})
        }
        "xp" => {
            let sym = op.get("sym").unwrap_or(&Value::Null);
            json!({"en": vars_value(cs::expand_en(sym)), "pl": vars_value(pln::expand(sym))})
        }
        "ins" => {
            let names = py::dumps(op.get("names").unwrap_or(&Value::Null));
            let params = op.get("params").and_then(|x| x.as_str()).map(|s| s.to_string());
            let ok = c.execute(
                "INSERT OR REPLACE INTO clan_feed(token_id,id,ts,day,week,kind,mood,faction,text,names,lid,params) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                rusqlite::params![tid, ji(op, "id"), op.get("ts").and_then(|x| x.as_f64()).unwrap_or(0.0), ji(op, "day"), ji(op, "week"), js(op, "kind"), js(op, "mood"), ji(op, "faction"), js(op, "text"), names, js(op, "lid"), params],
            );
            Value::Bool(ok.is_ok())
        }
        "state" => cs::_load(c, tid).unwrap_or(Value::Null),
        "roster" => cs::ensure_roster(c, tid, jb(op, "save")).unwrap_or(Value::Null),
        "pfw" => {
            let rows = cs::pick_for_widget(c, tid, seen, ji(op, "n") as usize, ji(op, "window"), lang);
            let mut s: Vec<i64> = seen.iter().copied().collect();
            s.sort();
            json!({"rows": rows.into_iter().map(|(i, t)| json!([i, t])).collect::<Vec<_>>(), "seen": s})
        }
        "lang" => json!(pln::norm_lang(js(op, "x"))),
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
    let dbp = std::env::var("T2R_CL_DB").unwrap_or_else(|_| "/tmp/clan_story_rs.db".into());
    let _ = std::fs::remove_file(&dbp);
    let c = match Connection::open(&dbp) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("selftest clan_story: {}", e);
            return 1;
        }
    };
    ext::set_test_fake(true);
    let mut seen: HashSet<i64> = HashSet::new();
    let mut i = 0usize;
    for op in script(input) {
        if let Some(t) = op.get("t").and_then(|x| x.as_f64()) {
            clock::freeze(t);
        }
        let r = call(&c, &op, &mut seen);
        let mut m = Map::new();
        m.insert("i".into(), Value::from(i as i64));
        m.insert("r".into(), r);
        let _ = writeln!(out, "{}", py::dumps(&Value::Object(m)));
        i += 1;
    }
    for (t, o) in [("clans", "token_id"), ("clan_story", "token_id"), ("clan_feed", "token_id,id"), ("fake_notes", "seq")] {
        let _ = writeln!(out, "{}", py::dumps(&dump_table(&c, t, o)));
    }
    0
}
