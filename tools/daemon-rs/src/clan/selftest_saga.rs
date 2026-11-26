//! `t2r-daemon selftest clan_saga` (also `clan_loot`): the Rust side of verify/py/clan_saga_harness.py (same script, same output bytes).
//! Whole chronicle days run with the REAL saga / Polish saga / loot (ext_clan_chronicle::real_saga); clan_status and spare_sick stay the doubles.
use crate::c_util::script;
use crate::clan::clan::{int_of, pystr};
use crate::clan::ext_clan_chronicle as ext;
use crate::clan::selftest_chronicle::{op as chron_op, Run};
use crate::clan::{clan_arcs as arcs, clan_chronicle as ch, clan_kids as kd, clan_loot as lt, clan_saga as sg, clan_saga_pl as pls, data};
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
fn arr<'a>(o: &'a Value, k: &str) -> &'a [Value] {
    o.get(k).and_then(|x| x.as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

fn pairs_sorted(m: std::collections::HashMap<String, String>) -> Value {
    let mut v: Vec<(String, String)> = m.into_iter().collect();
    v.sort();
    Value::Array(v.into_iter().map(|(k, x)| json!([k, x])).collect())
}

/// `str(x or "")` of a JSON value.
fn str_or_empty(v: Option<&Value>) -> String {
    match v {
        Some(x) if crate::clan::clan::truthy(x) => pystr(x),
        _ => String::new(),
    }
}

/// Ops of this test; `None` = not mine (the chronicle test ops).
fn mine(run: &mut Run, o: &Value) -> Option<Result<Option<Value>, String>> {
    let k = js(o, "k").to_string();
    let r: Result<Option<Value>, String> = (|| {
        Ok(Some(match js(o, "op") {
            "fortune" => {
                let st = run.st(&k).ok_or("no state")?;
                let mut fo = st.clone();
                fo["morale"] = o["morale"].clone();
                fo["day"] = o["day"].clone();
                let hist: Vec<Value> = arr(o, "hist").to_vec();
                lt::fortune(&fo, ch::ch_state(st), &hist)?
            }
            "roll_finds" => {
                let st = run.st(&k).ok_or("no state")?;
                ch::ch_state(st);
                let mut rng = PyRandom::from_i64(ji(o, "seed"));
                let day = o.get("day").filter(|v| !v.is_null()).map(int_of);
                let left = o.get("left").and_then(|x| x.as_bool()).unwrap_or(false);
                Value::Array(lt::roll_finds_left(&mut rng, st, &o["r"], &o["match"], o.get("score").unwrap_or(&Value::Null), day, left)?)
            }
            "update_moods" => {
                let st = run.st(&k).ok_or("no state")?;
                lt::update_moods_map(st)?
            }
            "legacy_loot" => Value::Array(lt::legacy_loot(arr(o, "finds"), &o["have"])),
            "items_text" => json!(lt::items_text_res(&o["finds"], js(o, "lang"))?),
            "public_l" => lt::public_res(&o["finds"], js(o, "lang"))?,
            "render_find" => json!(lt::render_find(&o["f"], js(o, "lang"), o["hook"].as_bool().unwrap_or(false))?),
            "finds_story" => json!(lt::finds_story(arr(o, "finds"), js(o, "lang"), ji(o, "limit") as usize)?),
            "keepsakes" => Value::Array(lt::keepsakes(arr(o, "finds"))?),
            "supply_value" => lt::supply_value(&o["finds"])?,
            "item_phrase" => {
                let a = lt::item_phrase(js(o, "key"), ji(o, "n"), ji(o, "c"), js(o, "lang"))?;
                let b = lt::noun_phrase(js(o, "key"), js(o, "lang"))?;
                json!([a, b])
            }
            "loc_phrase" => json!(lt::loc_phrase(&o["f"], js(o, "lang"), o["wp"].as_bool().unwrap_or(false))?),
            "icon" => {
                let a = lt::icon_for(js(o, "key"))?;
                let b = lt::icon_wanted(js(o, "key"))?;
                json!([a, b, if js(o, "key") == "pine" { lt::missing_icons() } else { Value::Null }])
            }
            "ljoin" => {
                let items: Vec<String> = arr(o, "items").iter().map(|x| x.as_str().unwrap_or("").to_string()).collect();
                json!(lt::join_list(&items, js(o, "lang")))
            }
            "fit" => {
                let st = run.st(&k).ok_or("no state")?;
                let ppl: Vec<Value> = st.get("people").and_then(|x| x.as_array()).cloned().unwrap_or_default();
                let p = if ppl.is_empty() { json!({"i": 0, "r": "cook"}) } else { ppl[(ji(o, "n") as usize) % ppl.len()].clone() };
                json!(sg::fit(ch::ch_state(st), js(o, "kind"), &p))
            }
            "sg_render" => match sg::render_msg(js(o, "lid"), o.get("params").unwrap_or(&Value::Null), js(o, "lang")) {
                Some(s) => json!(s),
                None => Value::Null,
            },
            "they_fix" => json!(sg::they_fix(js(o, "text"))),
            "map_art" => {
                let name = str_or_empty(o.get("name"));
                json!([sg::map_art(&name), std::ptr::eq(sg::map_data(&name), data::t("clan_saga_data", "GENERIC"))])
            }
            "facts" => Value::Object(sg::facts(o.get("m").unwrap_or(&Value::Null))),
            "small" => {
                let n = ji(o, "n");
                json!([sg::nw(n), sg::times_p(n), sg::count_p(n, "x", "xs"), sg::ago(ji(o, "d"), ji(o, "d") + n)])
            }
            "pools" => {
                let mut names: Vec<&String> = sg::POOLS.keys().collect();
                names.sort();
                let mut m = Map::new();
                for nm in names {
                    let rows: Vec<Value> = sg::POOLS[nm]
                        .iter()
                        .map(|l| {
                            let mut pk: Vec<String> = l.pk.iter().map(|c| c.to_string()).collect();
                            pk.sort();
                            let mut vk = l.vk.clone();
                            vk.sort();
                            json!([l.scope, l.text, pk, vk])
                        })
                        .collect();
                    m.insert(nm.clone(), Value::Array(rows));
                }
                Value::Object(m)
            }
            "expand" => {
                let v = o["v"].as_object().cloned().unwrap_or_default();
                let over: Option<pls::Vars> = o.get("over").filter(|x| x.is_object()).map(|x| x.as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), pystr(v))).collect()).unwrap_or_default());
                match pls::expand(&v, &o["people"], js(o, "fk"), over.as_ref()) {
                    Ok(m) => pairs_sorted(m),
                    Err(_) => json!("EXC"),
                }
            }
            "plfn" => {
                let n = ji(o, "n");
                let case = js(o, "case");
                let en = js(o, "en");
                let w = js(o, "w");
                match js(o, "fn") {
                    "times" => json!(pls::times(n)?),
                    "count" => json!(pls::count(n, "a", "b", "c", py::pymod(n, 2) == 0)?),
                    "ex_phrase" => json!(pls::ex_phrase(n, case)?),
                    "hs_phrase" => json!(pls::hs_phrase(n, case)?),
                    "unit_phrase" => json!(pls::unit_phrase(n, case)?),
                    "ago" => json!(pls::ago(en)?),
                    "gone" => json!(pls::gone(en)?),
                    "losses" => json!(pls::losses(en)?),
                    "num" => json!(pls::num_of(en)?),
                    "past_pl" => match pls::past_pl(if py::pymod(n, 2) != 0 { en } else { "grew up in the QZ" }, js(o, "g")) {
                        Some(s) => json!(s),
                        None => Value::Null,
                    },
                    "from" => json!(pls::from_(w)),
                    _ => json!([pls::prep("w", w), pls::prep("na", w)]),
                }
            }
            _ => return Err("\u{0}notmine".into()),
        }))
    })();
    match r {
        Err(e) if e == "\u{0}notmine" => None,
        other => Some(other),
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
    if !ch::reg_ok() || arcs::arcs_ok().is_err() {
        eprintln!("clan_chronicle / clan_arcs: tables differ");
        return 3;
    }
    if let Err(e) = pls::reg_ok() {
        eprintln!("clan_saga_pl: {}", e);
        return 3;
    }
    if !kd::regexes_ok() {
        eprintln!("clan_kids: regexes");
        return 3;
    }
    ext::doubles(true);
    ext::real_saga(true);
    let dbp = std::env::var("T2R_CL_DB").unwrap_or_else(|_| "/tmp/clan_saga_rs.db".into());
    for p in [dbp.clone(), format!("{}-journal", dbp)] {
        let _ = std::fs::remove_file(p);
    }
    let c = match Connection::open(&dbp) {
        Ok(c) => c,
        Err(x) => {
            eprintln!("clan_saga: cannot open {}: {}", dbp, x);
            return 2;
        }
    };
    if c.execute("CREATE TABLE IF NOT EXISTS clan_story (token_id TEXT PRIMARY KEY, state TEXT, updated REAL)", []).is_err() {
        return 2;
    }
    let mut run = Run { c, sts: Vec::new() };
    for (i, o) in script(input).iter().enumerate() {
        let opn = js(o, "op").to_string();
        let res = match mine(&mut run, o) {
            Some(r) => r,
            None => chron_op(&mut run, o),
        };
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
                eprintln!("clan_saga: call {} ({}): {}", i, opn, e);
                m.insert("r".into(), Value::Null);
                errf = true;
            }
        }
        if errf {
            m.insert("err".into(), json!(true));
        }
        let showst = matches!(opn.as_str(), "day" | "fortune" | "roll_finds" | "update_moods");
        if (showst || errf) && opn != "save" && opn != "public" {
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
    ext::real_saga(false);
    ext::doubles(false);
    0
}
