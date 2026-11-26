//! `t2r-daemon selftest clan_chronicle` (also `clan_arcs`): the Rust side of verify/py/clan_chronicle_harness.py (same script, same output bytes).
use crate::c_util::script;
use crate::clan::clan::int_of;
use crate::clan::ext_clan_chronicle as ext;
use crate::clan::{clan_arcs as arcs, clan_chronicle as ch};
use crate::py;
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::io::{BufRead, Write};
use t2r_clanstory::pyrand::PyRandom;

fn js<'a>(o: &'a Value, k: &str) -> &'a str {
    o.get(k).and_then(|x| x.as_str()).unwrap_or("")
}
fn ji(o: &Value, k: &str) -> i64 {
    o.get(k).map(int_of).unwrap_or(0)
}

fn persona_json(p: &ch::Persona) -> Value {
    json!({"temper": p.temper, "habit": p.habit, "past": p.past, "bond": p.bond, "bt": p.bt})
}

fn lg(l: &str) -> &'static str {
    if l == "pl" {
        "pl"
    } else {
        "en"
    }
}

fn set_path(st: &mut Value, path: &[Value], v: &Value) {
    let mut cur = st;
    for k in &path[..path.len().saturating_sub(1)] {
        cur = match k {
            Value::String(s) => &mut cur[s.as_str()],
            Value::Number(n) => &mut cur[n.as_u64().unwrap_or(0) as usize],
            _ => return,
        };
    }
    if let Some(Value::String(s)) = path.last() {
        cur[s.as_str()] = v.clone();
    }
}

pub(crate) struct Run {
    pub(crate) c: Connection,
    pub(crate) sts: Vec<(String, Value)>,
}

impl Run {
    pub(crate) fn st(&mut self, k: &str) -> Option<&mut Value> {
        self.sts.iter_mut().find(|(n, _)| n == k).map(|(_, v)| v)
    }
}

/// One call: `Ok(Some(r))` = the call has a result, `Ok(None)` = no "r" key.
pub(crate) fn op(run: &mut Run, o: &Value) -> Result<Option<Value>, String> {
    let k = js(o, "k").to_string();
    let fk_of = |st: &Value| if st.get("fac").map(int_of) == Some(2) { "S" } else { "W" };
    match js(o, "op") {
        "load" => {
            run.sts.retain(|(n, _)| *n != k);
            run.sts.push((k, o["st"].clone()));
            Ok(Some(json!(true)))
        }
        "set" => {
            if let Some(st) = run.st(&k) {
                set_path(st, o["path"].as_array().map(|a| a.as_slice()).unwrap_or(&[]), &o["v"]);
            }
            Ok(None)
        }
        "pset" => {
            if let Some(st) = run.st(&k) {
                if let Some(ppl) = st.get_mut("people").and_then(|x| x.as_array_mut()) {
                    if let Some(p) = ppl.iter_mut().find(|p| ji(p, "i") == ji(o, "i")) {
                        p[js(o, "f")] = o["v"].clone();
                    }
                }
            }
            Ok(None)
        }
        "kill" => {
            if let Some(st) = run.st(&k) {
                let ix = st.get("people").and_then(|x| x.as_array()).and_then(|a| a.iter().position(|p| ji(p, "i") == ji(o, "i")));
                if let Some(ix) = ix {
                    let p = st["people"].as_array_mut().map(|a| a.remove(ix)).unwrap_or(Value::Null);
                    let rec = json!({"i": p["i"], "n": p["n"], "g": p["g"], "r": p["r"], "d": o["d"], "c": o["c"], "il": p["il"], "k": 0});
                    if let Some(d) = st.get_mut("dead").and_then(|x| x.as_array_mut()) {
                        d.push(rec);
                    }
                }
            }
            Ok(None)
        }
        "add" => {
            if let Some(st) = run.st(&k) {
                if let Some(a) = st.get_mut("people").and_then(|x| x.as_array_mut()) {
                    a.push(o["p"].clone());
                }
            }
            Ok(None)
        }
        "day" => {
            let seed = o.get("seed").filter(|v| !v.is_null()).map(int_of);
            let matchv = o.get("match").filter(|v| !v.is_null());
            let mut used = o.get("used").filter(|v| !v.is_null()).cloned();
            let newc = o["newc"].clone();
            let r = o["r"].clone();
            let st = run.st(&k).ok_or("no state")?;
            let mut rng = seed.map(PyRandom::from_i64);
            let rows = ch::day_rows(st, &newc, &r, matchv, used.as_mut(), rng.as_mut())?;
            let mut en: Vec<Value> = Vec::new();
            let mut pl: Vec<Value> = Vec::new();
            for x in rows.iter() {
                en.push(ch::render_msg_res(js(x, "lid"), &x["params"], "en")?.map(Value::from).unwrap_or(Value::Null));
            }
            for x in rows.iter() {
                pl.push(ch::render_msg_res(js(x, "lid"), &x["params"], "pl")?.map(Value::from).unwrap_or(Value::Null));
            }
            let res = json!({"rows": rows, "en": en, "pl": pl});
            if let Some(u) = used {
                return Ok(Some(json!({"__r": res, "__used": u})));
            }
            Ok(Some(res))
        }
        "save" => {
            let s = py::dumps_compact(run.st(&k).ok_or("no state")?);
            run.c.execute("INSERT OR REPLACE INTO clan_story(token_id,state,updated) VALUES(?,?,?)", rusqlite::params![k, s, 0.0f64]).map_err(|e| e.to_string())?;
            Ok(None)
        }
        "public" => Ok(Some(ch::public(&run.c, &k, js(o, "lang"), 40)?)),
        "persona" | "tagline" | "assign" | "inventory" | "match_log" | "portrait" | "head" | "bond" => {
            let opn = js(o, "op").to_string();
            let n = ji(o, "n") as usize;
            let lang = js(o, "lang").to_string();
            let st = run.st(&k).ok_or("no state")?;
            let fk = fk_of(st);
            let ppl: Vec<Value> = st["people"].as_array().cloned().unwrap_or_default();
            let p = if ppl.is_empty() { None } else { Some(ppl[n % ppl.len()].clone()) };
            let members: Option<Vec<Value>> = if o["members"].as_bool() == Some(true) {
                let mut m = ppl.clone();
                m.extend(st.get("dead").and_then(|x| x.as_array()).cloned().unwrap_or_default());
                Some(m)
            } else {
                None
            };
            let members_s: Option<&[Value]> = members.as_deref().filter(|m| !m.is_empty());
            if p.is_none() && matches!(opn.as_str(), "persona" | "tagline" | "portrait" | "head" | "bond") {
                return Ok(Some(Value::Null));
            }
            let pv = p.unwrap_or(Value::Null);
            Ok(Some(match opn.as_str() {
                "persona" => persona_json(&ch::persona(&pv, &ppl, fk, if n % 2 != 0 { Some(st) } else { None })),
                "tagline" => json!(ch::tagline(&pv, &ppl, fk, if lang.starts_with("pl") { "pl" } else { "en" }, if n % 2 != 0 { Some(st) } else { None })?),
                "assign" => ch::assign_habits(st, fk),
                "inventory" => {
                    let (a, b) = ch::inventory(st, lg(&lang))?;
                    json!([a, b])
                }
                "match_log" => json!(ch::match_log(st, lg(&lang), n + 1)?),
                "portrait" => json!(ch::portrait_id(&pv, fk, o.get("state").and_then(|x| x.as_str()), ji(o, "morale"), ji(o, "models"), members_s)),
                "head" => {
                    let (s, m) = ch::head_model(&pv, fk, ji(o, "models"), members_s, true);
                    json!([s, m])
                }
                _ => {
                    let q = ppl[(n + 1) % ppl.len()].clone();
                    let mut v = Vec::new();
                    for bt in ["kin", "mentor", "rival", "sweet", ""] {
                        v.push(json!(ch::bond_label(&q, &pv, bt, lg(&lang))?));
                    }
                    Value::Array(v)
                }
            }))
        }
        "qty" => Ok(Some(json!(ch::qty(js(o, "key"), ji(o, "n"), js(o, "lang"))?))),
        "loot_phrase" => Ok(Some(json!(ch::loot_phrase(js(o, "key"), ji(o, "n"), js(o, "lang"))?))),
        "join" => {
            let items: Vec<String> = o["items"].as_array().map(|a| a.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect()).unwrap_or_default();
            Ok(Some(json!(ch::join_list(&items, js(o, "lang")))))
        }
        "map" => {
            // str(en or "")
            let en = match o.get("en") {
                Some(Value::String(s)) => s.clone(),
                Some(Value::Number(n)) => n.to_string(),
                _ => String::new(),
            };
            Ok(Some(json!(ch::map_name(&en, js(o, "lang")))))
        }
        "habit_pref" => Ok(Some(json!(ch::habit_pref(&o["p"], js(o, "fk"))))),
        "roll_loot" => {
            let mut rng = PyRandom::from_i64(ji(o, "seed"));
            Ok(Some(Value::Array(ch::roll_loot(&mut rng, &o["r"], &o["match"], &o["newc"], &o["have"])?)))
        }
        "render" => Ok(Some(match ch::render_msg_res(js(o, "lid"), &o["params"], js(o, "lang"))? {
            Some(s) => json!(s),
            None => Value::Null,
        })),
        "thread" => {
            let st = run.st(&k).ok_or("no state")?;
            Ok(Some(json!(arcs::thread(st, js(o, "id")))))
        }
        other => Err(format!("unknown op {}", other)),
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    if let Err(k) = ch::consts_ok() {
        eprintln!("clan_chronicle: constant {} differs from the generated table", k);
        return 3;
    }
    if let Err(k) = arcs::consts_ok() {
        eprintln!("clan_arcs: constant {} differs from the generated table", k);
        return 3;
    }
    if !ch::reg_ok() {
        eprintln!("clan_chronicle: the piece table differs from the generated REG");
        return 3;
    }
    if let Err(k) = arcs::arcs_ok() {
        eprintln!("clan_arcs: the hand-ported ARCS table differs from the generated one: {}", k);
        return 3;
    }
    ext::doubles(true);
    let dbp = std::env::var("T2R_CL_DB").unwrap_or_else(|_| "/tmp/clan_chron_rs.db".into());
    for p in [dbp.clone(), format!("{}-journal", dbp)] {
        let _ = std::fs::remove_file(p);
    }
    let c = match Connection::open(&dbp) {
        Ok(c) => c,
        Err(x) => {
            eprintln!("clan_chronicle: cannot open {}: {}", dbp, x);
            return 2;
        }
    };
    if c.execute("CREATE TABLE IF NOT EXISTS clan_story (token_id TEXT PRIMARY KEY, state TEXT, updated REAL)", []).is_err() {
        return 2;
    }
    let mut run = Run { c, sts: Vec::new() };
    for (i, o) in script(input).iter().enumerate() {
        let opn = js(o, "op").to_string();
        let res = op(&mut run, o);
        let mut m = Map::new();
        m.insert("i".into(), json!(i as i64));
        let mut errf = false;
        match res {
            Ok(Some(r)) => {
                if let Some(obj) = r.as_object().filter(|x| x.contains_key("__r")) {
                    m.insert("r".into(), obj["__r"].clone());
                    m.insert("used".into(), obj["__used"].clone());
                } else {
                    m.insert("r".into(), r);
                }
            }
            Ok(None) => {}
            Err(e) => {
                eprintln!("clan_chronicle: call {} ({}): {}", i, opn, e);
                m.insert("r".into(), Value::Null);
                errf = true;
            }
        }
        if errf {
            m.insert("err".into(), json!(true));
        }
        if opn == "day" || opn == "assign" || errf {
            let k = js(o, "k").to_string();
            if let Some(st) = run.st(&k) {
                m.insert("st".into(), st.clone());
            }
        }
        let _ = writeln!(out, "{}", py::dumps_compact(&Value::Object(m)));
    }
    let mut fin = Map::new();
    for (k, v) in &run.sts {
        fin.insert(k.clone(), v.clone());
    }
    let _ = writeln!(out, "{}", py::dumps_compact(&json!({"i": -1, "final": Value::Object(fin)})));
    ext::doubles(false);
    0
}
