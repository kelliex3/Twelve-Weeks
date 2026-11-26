//! `t2r-daemon selftest clan_base`: the Rust side of verify/py/clan_base_harness.py (same script, same output bytes).
use crate::c_util::{jb, jf, ji, js, script};
use crate::clan::clan::{self as cl, counts_from_json, vref_to_json, Counts};
use crate::clan::{clan_continuity as con, clan_kids as kd, clan_portraits as pt, clan_reset as rst, clan_supply as sup, data};
use crate::{clock, py};
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::io::{BufRead, Write};

fn jv<'a>(o: &'a Value, k: &str) -> &'a Value {
    o.get(k).unwrap_or(&Value::Null)
}

fn jints(o: &Value, k: &str) -> Vec<i64> {
    o.get(k).and_then(|x| x.as_array()).map(|a| a.iter().map(cl::int_of).collect()).unwrap_or_default()
}

fn counts_opt(o: &Value, k: &str) -> Option<Counts> {
    match o.get(k) {
        Some(Value::Null) | None => None,
        Some(v) => Some(counts_from_json(v)),
    }
}

fn sup_dict(keys: &[&str], vals: &[i64]) -> Value {
    let mut m = Map::new();
    for (i, k) in keys.iter().enumerate() {
        m.insert((*k).to_string(), Value::from(vals[i]));
    }
    Value::Object(m)
}

fn db_err(e: rusqlite::Error) -> String {
    e.to_string()
}

fn dump(c: &Connection, bdir: &str) -> Result<Value, String> {
    let mut tables = Map::new();
    let names: Vec<(String, Option<String>)> = {
        let mut st = c.prepare("SELECT name, sql FROM sqlite_master WHERE type='table' ORDER BY name").map_err(db_err)?;
        let rows = st.query_map([], |r| Ok((r.get::<_, String>(0)?, r.get::<_, Option<String>>(1)?))).map_err(db_err)?;
        rows.collect::<Result<_, _>>().map_err(db_err)?
    };
    for (name, sql) in names {
        let mut st = c.prepare(&format!("SELECT * FROM \"{}\" ORDER BY rowid", name)).map_err(db_err)?;
        let n = st.column_count();
        let rows: Vec<Value> = st
            .query_map([], |r| {
                let mut row = Vec::new();
                for i in 0..n {
                    row.push(vref_to_json(r.get_ref(i)?));
                }
                Ok(Value::Array(row))
            })
            .map_err(db_err)?
            .collect::<Result<_, _>>()
            .map_err(db_err)?;
        tables.insert(name, json!({"sql": sql, "rows": rows}));
    }
    let mut files = Map::new();
    if let Ok(rd) = std::fs::read_dir(bdir) {
        let mut fs: Vec<String> = rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        fs.sort();
        for f in fs {
            let t = std::fs::read(std::path::Path::new(bdir).join(&f)).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
            files.insert(f, Value::String(t));
        }
    }
    Ok(json!({"tables": tables, "files": files}))
}

fn restore_target(bdir: &str, f: &str) -> String {
    if f == "LAST" {
        let mut fs: Vec<String> = std::fs::read_dir(bdir)
            .map(|rd| rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|x| x.ends_with(".json")).collect())
            .unwrap_or_default();
        fs.sort();
        if fs.is_empty() {
            "none.json".to_string()
        } else {
            fs[fs.len() / 2].clone()
        }
    } else {
        f.to_string()
    }
}

fn strs(o: &Value, k: &str) -> Vec<String> {
    o.get(k).and_then(|x| x.as_array()).map(|a| a.iter().map(cl::pystr).collect()).unwrap_or_default()
}

fn op(c: &Connection, o: &Value, bdir: &str) -> Result<Value, String> {
    let e = db_err;
    let tid = js(o, "tid");
    Ok(match js(o, "op") {
        "sql" => {
            let params: Vec<rusqlite::types::Value> = o.get("a").and_then(|x| x.as_array()).map(|a| a.iter().map(cl::to_sql).collect()).unwrap_or_default();
            c.execute(js(o, "q"), rusqlite::params_from_iter(params)).map_err(e)?;
            Value::Null
        }
        "env" => {
            match o.get("v").and_then(|x| x.as_str()) {
                Some(v) => std::env::set_var(js(o, "k"), v),
                None => std::env::remove_var(js(o, "k")),
            }
            Value::Null
        }
        "evfile" => {
            let p = data::here("@HERE@/clan_events.txt");
            match o.get("c").and_then(|x| x.as_str()) {
                None => {
                    let _ = std::fs::remove_file(&p);
                }
                Some(t) => {
                    std::fs::write(&p, t).map_err(|x| x.to_string())?;
                    let f = std::fs::OpenOptions::new().write(true).open(&p).map_err(|x| x.to_string())?;
                    f.set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(ji(o, "m") as u64)).map_err(|x| x.to_string())?;
                }
            }
            Value::Null
        }
        "ptfile" => {
            let p = data::here("@HERE@/clan_portrait_pool.txt");
            match o.get("c").and_then(|x| x.as_str()) {
                None => {
                    let _ = std::fs::remove_file(&p);
                }
                Some(t) => std::fs::write(&p, t).map_err(|x| x.to_string())?,
            }
            Value::Null
        }
        "ensure" => {
            cl::ensure(c).map_err(e)?;
            Value::Null
        }
        "load" => cl::load(c, tid).map_err(e)?,
        "save" => {
            let mut x = cl::load(c, tid).map_err(e)?;
            if let (Some(xo), Some(p)) = (x.as_object_mut(), o.get("patch").and_then(|p| p.as_object())) {
                for (k, v) in p {
                    xo.insert(k.clone(), v.clone());
                }
            }
            cl::save(c, tid, &x).map_err(e)?;
            cl::load(c, tid).map_err(e)?
        }
        "choose" => {
            let (x, ok) = cl::choose_side(c, tid, ji(o, "f")).map_err(e)?;
            json!([x, ok])
        }
        "apply" => {
            let counts = counts_opt(o, "counts");
            let (x, r) = cl::apply_match(c, tid, js(o, "mid"), ji(o, "parts"), jb(o, "left"), counts.as_ref(), jf(o, "mult"), jb(o, "forgive"), ji(o, "bottles"), jb(o, "protect")).map_err(e)?;
            json!([x, r])
        }
        "resolve" => {
            let x = cl::load(c, tid).map_err(e)?;
            let counts = counts_opt(o, "counts");
            let (n, r) = cl::resolve_day(&x, ji(o, "parts"), jb(o, "left"), ji(o, "downs"), counts.as_ref(), jf(o, "mult"), jb(o, "forgive"), ji(o, "bottles"), jb(o, "protect"));
            json!([n, r])
        }
        "mchoose" => {
            let mut mid = ji(o, "mid");
            if let Some(ix) = o.get("pool_idx").and_then(|x| x.as_i64()) {
                let m = cl::meta_of(&cl::load(c, tid).map_err(e)?);
                let pool = m.get("pool").filter(|v| cl::truthy(v)).and_then(|v| v.as_array()).cloned().unwrap_or_default();
                if (ix as usize) < pool.len() {
                    mid = cl::int_of(&pool[ix as usize]);
                }
            }
            let (x, ok) = cl::mission_choose(c, tid, mid).map_err(e)?;
            json!([x, ok])
        }
        "bcounts" => json!(cl::booster_counts(c, tid).map_err(e)?),
        "bgrant" => json!(cl::booster_grant(c, tid, ji(o, "bid"), ji(o, "n")).map_err(e)?),
        "bconsume" => json!(cl::booster_consume(c, tid, ji(o, "bid")).map_err(e)?),
        "bnext" => json!(cl::booster_next(&cl::load(c, tid).map_err(e)?)),
        "auto" => json!(cl::auto_event_mission(&cl::meta_of(&cl::load(c, tid).map_err(e)?))),
        "meta_text" => json!(cl::meta_text(c, tid, ji(o, "pick"), ji(o, "active")).map_err(e)?),
        "state_text" => json!(cl::state_text(&cl::load(c, tid).map_err(e)?)),
        "rank" => {
            let x = cl::load(c, tid).map_err(e)?;
            let day = cl::gi(&x, "day", 0);
            let (rk, wk) = cl::rank_of(&x);
            let (w, d) = cl::week_day(day);
            let (ek, en) = cl::event_today(day);
            json!([[rk, wk], [w, d], [ek, en]])
        }
        "pure" => {
            let a = jints(o, "a");
            let s = js(o, "s");
            let x = jf(o, "x");
            match js(o, "f") {
                "needed" => json!(cl::needed(a[0])),
                "supplies_of" => json!(cl::supplies_of(a[0] * 37, a[2] - 1)),
                "week_day" => json!(cl::week_day(a[0])),
                "event_today" => json!(cl::event_today(a[0])),
                "event_window" => json!(cl::event_window(a[0])),
                "attack_story" => json!(cl::attack_story(a[0])),
                "event_name" => json!([cl::event_name(a[2], a[1]), cl::event_name(1, a[0]), cl::event_name(2, py::pymod(a[0], 3) - 1)]),
                "token_id" => json!(cl::token_id(s)),
                "mission_goals" => json!([1, 2, 40, 52].iter().map(|m| cl::mission_goals(*m, a[1]).to_vec()).collect::<Vec<_>>()),
                "attack_tier" => json!([cl::attack_tier(a[0]), cl::attack_goals().to_vec()]),
                "tier_reward" => json!(cl::tier_reward(a[2], a[0])),
                "threshold_booster" => json!(cl::threshold_booster(a[1] + a[2])),
                "rank_days" => json!(cl::rank_days(a[3] * 5, a[1], a[0], a[2] + 1)),
                "f32" => json!(cl::f32(x)),
                "rnd" => json!(cl::rnd(x)),
                "roll" => json!(cl::roll(x)),
                "new_events" => json!(cl::new_events()),
                "match_tag" => json!(cl::match_tag(if s.is_empty() { None } else { Some(s) })),
                _ => Value::Null,
            }
        }
        "snap" => {
            let mid = js(o, "mid");
            match js(o, "w") {
                "take" => {
                    cl::snap_take(c, tid, mid, js(o, "token")).map_err(e)?;
                    Value::Null
                }
                "get" => cl::snap_get(c, tid, mid).map_err(e)?.unwrap_or(Value::Null),
                "update" => {
                    let kw: Vec<(String, Value)> = o.get("kw").and_then(|x| x.as_object()).map(|m| m.iter().map(|(k, v)| (k.clone(), v.clone())).collect()).unwrap_or_default();
                    let kw2: Vec<(&str, Value)> = kw.iter().map(|(k, v)| (k.as_str(), v.clone())).collect();
                    cl::snap_update(c, tid, mid, &kw2).map_err(e)?;
                    cl::snap_get(c, tid, mid).map_err(e)?.unwrap_or(Value::Null)
                }
                "restore" => {
                    let s = cl::snap_get(c, tid, mid).map_err(e)?;
                    if let Some(sn) = &s {
                        cl::snap_restore(c, tid, js(o, "token"), sn).map_err(e)?;
                    }
                    json!(s.is_some())
                }
                _ => Value::Null,
            }
        }
        "sup" => {
            let counts = counts_opt(o, "counts").unwrap_or_default();
            match js(o, "w") {
                "split" => json!(sup::split(ji(o, "total"), &jints(o, "weights"))),
                "need_split" => sup_dict(&["food", "water", "med"], &sup::need_split(ji(o, "need"), o.get("n").and_then(|x| x.as_i64()))),
                "use_of" => sup_dict(&["ammo", "mat"], &sup::use_of(ji(o, "need"))),
                "intake" => sup::intake_value(ji(o, "supplies"), ji(o, "parts"), &counts, jb(o, "left")),
                "book" => {
                    let need = ji(o, "need");
                    let (st, rec) = sup::book(o.get("stock"), ji(o, "supplies"), need, py::floordiv(need, 2).max(0), ji(o, "parts"), &counts, jb(o, "left"), jb(o, "forgive"));
                    json!([st, rec])
                }
                _ => Value::Null,
            }
        }
        "kid" => {
            let t = js(o, "x1");
            let t2 = js(o, "x2");
            let people = jv(o, "people");
            let lang = o.get("lang").and_then(|x| x.as_str());
            match js(o, "w") {
                "combat" => json!([kd::combat_text(t, Some("en")), kd::combat_text(t2, Some("pl")), kd::combat_text(t, lang), kd::combat_text("", None)]),
                "kid_safe" => json!([kd::kid_safe(t, people, &[t2]), kd::kid_safe(t, people, &[]), kd::kid_safe(t2, people, &[t, ""])]),
                "kid_text_ok" => json!([kd::kid_text_ok(t, people, "en"), kd::kid_text_ok(t2, people, "pl")]),
                "letters" => json!(kd::letters_in(t).into_iter().map(|c| c.to_string()).collect::<Vec<_>>()),
                "kid_letters" => json!(kd::kid_letters(t, people).into_iter().map(|c| c.to_string()).collect::<Vec<_>>()),
                "is_kid" => {
                    let mut ps: Vec<Value> = people.as_object().map(|m| m.values().cloned().collect()).unwrap_or_default();
                    ps.push(Value::Null);
                    ps.push(json!({}));
                    ps.push(json!({"r": "kid"}));
                    json!(ps.iter().map(|p| json!([kd::is_kid(p), kd::can_fight(p)])).collect::<Vec<_>>())
                }
                "any_kid" => json!(kd::any_kid(people)),
                "kid_names" => json!(kd::kid_names(jv(o, "st")).into_iter().collect::<Vec<_>>()),
                "sets" => {
                    let s = js(o, "s");
                    json!([kd::is_combat_kind(s), kd::is_combat_pool_alife(s), kd::is_combat_pool_chron(s), kd::is_combat_scene_saga(s)])
                }
                _ => Value::Null,
            }
        }
        "cont" => {
            let lang = js(o, "lang");
            match js(o, "w") {
                "tail" => json!(con::tail(js(o, "kind"), js(o, "role"), js(o, "name"), ji(o, "a"), ji(o, "b"), lang)),
                "match_tail" => json!(con::match_tail(jv(o, "prev"), jv(o, "m"), lang)),
                "haul_tail" => json!(con::haul_tail(jv(o, "prev"), lang)),
                "annotate" => {
                    let mut rows: Vec<Value> = jv(o, "rows").as_array().cloned().unwrap_or_default();
                    let order = con::annotate(&mut rows, jv(o, "dead"), jv(o, "sick_since"), lang);
                    let ids: Vec<Value> = order.iter().map(|&i| rows[i].get("id").cloned().unwrap_or(Value::Null)).collect();
                    json!([rows, ids])
                }
                "render" => json!(con::render_msg(js(o, "lid"), jv(o, "params"), lang)),
                "recap" => {
                    let mut st = jv(o, "st").clone();
                    let res = con::recap_rows(c, tid, &mut st, &json!({"day": ji(o, "day")}), js(o, "mood"));
                    json!([res, st])
                }
                "proper" => {
                    let mut v = con::proper_nouns(&strs(o, "extra"));
                    v.sort_by(|a, b| b.chars().count().cmp(&a.chars().count()).then(a.cmp(b)));
                    json!(v)
                }
                "restore_case" => {
                    let mut v = con::proper_nouns(&strs(o, "extra"));
                    v.sort_by(|a, b| b.chars().count().cmp(&a.chars().count()).then(a.cmp(b)));
                    json!(con::restore_case(js(o, "text"), &v))
                }
                _ => Value::Null,
            }
        }
        "port" => match js(o, "w") {
            "enabled" => json!(pt::enabled()),
            "candidates" => json!(pt::candidates(js(o, "role"), js(o, "sex"), js(o, "fk")).into_iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>()),
            "pick" => json!(pt::pick(js(o, "name"), js(o, "role"), js(o, "sex"), js(o, "fk"), &jints(o, "used"))),
            "faction_of" => json!(pt::faction_of(ji(o, "model"))),
            "look_of" => json!(pt::look_of(ji(o, "model"))),
            "expr_for" => json!(pt::expr_for(ji(o, "model"), js(o, "want"))),
            _ => Value::Null,
        },
        "rst" => {
            let tids = strs(o, "tids");
            match js(o, "w") {
                "keys" => json!(rst::keys_for(ji(o, "sid"), &tids)),
                "reset" => rst::reset(c, bdir, js(o, "name"), ji(o, "sid"), &tids, jb(o, "names"), None)?,
                "list" => rst::list_backups(bdir, Some(ji(o, "sid")).filter(|s| *s != 0)),
                "restore" => rst::restore(c, bdir, &restore_target(bdir, js(o, "file")), None)?,
                _ => Value::Null,
            }
        }
        "dump" => dump(c, bdir)?,
        other => return Err(format!("unknown op {}", other)),
    })
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let dbp = std::env::var("T2R_CL_DB").unwrap_or_else(|_| "/tmp/clan_base_rs.db".into());
    let bdir = std::env::var("T2R_CL_DIR").unwrap_or_else(|_| "/tmp/clan_base_rs_bk".into());
    for p in [dbp.clone(), format!("{}-journal", dbp), data::here("@HERE@/clan_events.txt"), data::here("@HERE@/clan_portrait_pool.txt")] {
        let _ = std::fs::remove_file(p);
    }
    std::env::remove_var("T2R_CLAN_NEW_EVENTS");
    std::env::remove_var("T2R_PORTRAIT_POOL");
    if let Err(k) = cl::consts_ok() {
        eprintln!("clan_base: constant {} differs from the generated table", k);
        return 3;
    }
    if !kd::regexes_ok() {
        eprintln!("clan_base: kid word lists do not compile");
        return 3;
    }
    let c = match Connection::open(&dbp) {
        Ok(c) => c,
        Err(x) => {
            eprintln!("clan_base: cannot open {}: {}", dbp, x);
            return 2;
        }
    };
    cl::capture_prints(true);
    for (i, o) in script(input).iter().enumerate() {
        clock::freeze(jf(o, "t"));
        let r = match op(&c, o, &bdir) {
            Ok(r) => r,
            Err(m) => {
                eprintln!("clan_base: call {} failed: {}", i, m);
                return 1;
            }
        };
        let p: Vec<Value> = cl::take_prints().into_iter().flat_map(|s| s.lines().map(|l| Value::String(l.to_string())).collect::<Vec<_>>()).collect();
        let line = json!({"i": i as i64, "r": r, "p": p});
        let _ = writeln!(out, "{}", py::dumps_compact(&line));
    }
    clock::freeze(0.0);
    0
}
