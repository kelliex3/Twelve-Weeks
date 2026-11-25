//! `t2r-daemon selftest k1`: the Rust side of verify/py/k1_harness.py (differential test of the K1 base chunk: names, caches, leaderboard, Elo,
//! record_result, bans, update_target). The matching Python side imports daemon.py and runs the same script.
use super::*;
use crate::c_util::{emit, script};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};
use crate::names;
use std::sync::atomic::Ordering;

fn strs(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}

fn dir() -> std::path::PathBuf {
    std::path::Path::new(&db::CFG.path).parent().map(|p| p.to_path_buf()).unwrap_or_default()
}

fn set_mtime(p: &std::path::Path, secs: f64) {
    if let Ok(f) = std::fs::OpenOptions::new().write(true).open(p) {
        let _ = f.set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs_f64(secs));
    }
}

fn cell(v: rusqlite::types::ValueRef) -> Value {
    use rusqlite::types::ValueRef;
    match v {
        ValueRef::Null => Value::Null,
        ValueRef::Integer(i) => json!(i),
        ValueRef::Real(f) => json!(f),
        ValueRef::Text(t) => Value::String(String::from_utf8_lossy(t).into_owned()),
        ValueRef::Blob(b) => Value::String(b.iter().map(|x| format!("{:02x}", x)).collect()),
    }
}

fn table(c: &rusqlite::Connection, q: &str) -> Value {
    let Ok(mut st) = c.prepare(q) else { return json!([]) };
    let n = st.column_count();
    let rows = st.query_map([], |r| Ok((0..n).map(|i| cell(r.get_ref_unwrap(i))).collect::<Vec<_>>()));
    match rows {
        Ok(it) => Value::Array(it.filter_map(|x| x.ok()).map(Value::Array).collect()),
        Err(_) => json!([]),
    }
}

fn state() -> Value {
    let r = db::with_write(|c| {
        Ok(json!({
            "players": table(c, "SELECT token,name,loadout,kills,deaths,wins,matches,draws,steamid,last_seen FROM players ORDER BY token"),
            "ratings": table(c, "SELECT key,elo,n,updated FROM ratings ORDER BY key"),
            "names": table(c, "SELECT token_id,name,tag,updated FROM names ORDER BY token_id"),
        }))
    });
    let mut v = r.unwrap_or(json!({}));
    v["ver"] = json!(name_ver());
    v
}

fn seed(s: &Value) {
    let _ = db::with_write(|c| {
        c.execute_batch("CREATE TABLE IF NOT EXISTS match_stats(mid TEXT, token TEXT, parts INT, kills INT, downs INT, deaths INT, revives INT)")?;
        names::init(c)?;
        let _ = elo_ensure(c);
        c.execute_batch("DELETE FROM players; DELETE FROM match_stats; DELETE FROM ratings; DELETE FROM names;")?;
        let arr = |k: &str| s.get(k).and_then(|v| v.as_array()).cloned().unwrap_or_default();
        for r in arr("players") {
            c.execute(
                "INSERT INTO players(token,name,steamid,kills,deaths,wins,matches,draws,last_seen) VALUES (?,?,?,?,?,?,?,?,?)",
                rusqlite::params![r[0].as_str(), r[1].as_str(), r[2].as_i64(), r[3].as_i64(), r[4].as_i64(), r[5].as_i64(), r[6].as_i64(), r[7].as_i64(), r[8].as_f64()],
            )?;
        }
        for r in arr("stats") {
            c.execute(
                "INSERT INTO match_stats(mid,token,parts,kills,downs,deaths,revives) VALUES (?,?,?,?,?,?,?)",
                rusqlite::params![r[0].as_str(), r[1].as_str(), r[2].as_i64(), r[3].as_i64(), r[4].as_i64(), r[5].as_i64(), r[6].as_i64()],
            )?;
        }
        for r in arr("names") {
            c.execute("INSERT OR REPLACE INTO names(token_id,name,tag,updated) VALUES (?,?,?,?)", rusqlite::params![r[0].as_str(), r[1].as_str(), r[2].as_str(), r[3].as_f64()])?;
        }
        Ok(())
    });
    reset_caches();
    *BANS.lock() = Bans::default();
    let _ = std::fs::remove_file(dir().join("bans.txt"));
    let _ = std::fs::remove_dir_all(dir().join("update-data"));
}

fn bans_state() -> Value {
    let b = BANS.lock();
    let sorted = |s: &HashSet<String>| {
        let mut v: Vec<&String> = s.iter().collect();
        v.sort();
        json!(v)
    };
    let mut ip: Vec<String> = b.ip.iter().map(|n| format!("{}/{}", n.addr, n.prefix)).collect();
    ip.sort();
    let mut rs: Vec<(&String, &String)> = b.reason.iter().collect();
    rs.sort();
    let rm: serde_json::Map<String, Value> = rs.into_iter().map(|(k, v)| (k.clone(), json!(v))).collect();
    json!({"mtime": b.mtime, "ids": sorted(&b.ids), "names": sorted(&b.names), "hw": sorted(&b.hw), "tok": sorted(&b.tok), "ip": ip, "pid": sorted(&b.pid),
        "reason": Value::Object(rm)})
}

fn call(f: &str, a: &[Value]) -> Value {
    let s = |i: usize| a.get(i).and_then(|v| v.as_str()).unwrap_or("").to_string();
    let os = |i: usize| a.get(i).and_then(|v| v.as_str());
    match f {
        "leaderboard" => match leaderboard(a[0].as_i64().unwrap_or(10) as usize) {
            Ok(rows) => Value::Array(rows.into_iter().map(|r| json!([r.0, r.1, r.2, r.3, r.4, r.5])).collect()),
            Err(_) => json!("EXC"),
        },
        "elo_update" => {
            let skip: HashSet<String> = strs(&a[2]).into_iter().collect();
            elo_update(&strs(&a[0]), a[1].as_i64().unwrap_or(-1), &skip, os(3));
            Value::Null
        }
        "elo_of" => {
            let toks = strs(&a[0]);
            let rec: Option<HashMap<String, (i64, i64, i64)>> = a[1].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), (v[0].as_i64().unwrap_or(0), v[1].as_i64().unwrap_or(0), v[2].as_i64().unwrap_or(0)))).collect());
            let out = elo_of_tokens(&toks, rec.as_ref());
            Value::Object(out.into_iter().map(|(k, v)| (k, json!(v))).collect())
        }
        "record_result" => {
            let skip: HashSet<String> = strs(&a[2]).into_iter().collect();
            let _ = record_result(&strs(&a[0]), a[1].as_i64().unwrap_or(0), &skip, None);
            Value::Null
        }
        "get_name" => json!(get_name(&s(0))),
        "token_steamid" => json!(token_steamid(&s(0))),
        "custom_name" => {
            let (n, t) = custom_name(&s(0));
            json!([n, t])
        }
        "name_key" => json!(name_key(&s(0))),
        "set_name" => {
            let _ = set_name(&s(0), &s(1));
            Value::Null
        }
        "set_steam" => {
            let _ = set_steam(&s(0), a[1].as_u64().unwrap_or(0), &s(2));
            Value::Null
        }
        "name_dirty" => {
            name_dirty(os(0), os(1));
            Value::Null
        }
        "names_save" => {
            let _ = db::with_write(|c| {
                names::save(c, &s(0), &s(1), &s(2), None)?;
                Ok(())
            });
            Value::Null
        }
        "save_loadout" => {
            let b: Vec<u8> = (0..s(1).len() / 2).map(|i| u8::from_str_radix(&s(1)[i * 2..i * 2 + 2], 16).unwrap_or(0)).collect();
            let _ = save_loadout(&s(0), &b);
            Value::Null
        }
        "get_loadout" => match get_loadout(&s(0)) {
            Ok(Some(b)) => json!(b.iter().map(|x| format!("{:02x}", x)).collect::<String>()),
            _ => Value::Null,
        },
        "add_matches" => {
            let _ = add_matches(&strs(&a[0]));
            Value::Null
        }
        "token_name" => token_name(&s(0)).map(Value::String).unwrap_or(Value::Null),
        "tag" => json!(tag(&s(0))),
        "safe_name" => json!(safe_name(&s(0))),
        "name_taken" => json!(db::with_write(|c| Ok(name_taken(c, &s(0), &s(1))?)).unwrap_or(false)),
        "bans" => {
            let p = dir().join("bans.txt");
            match os(0) {
                None => {
                    let _ = std::fs::remove_file(&p);
                }
                Some(t) => {
                    let _ = std::fs::write(&p, t.as_bytes());
                    set_mtime(&p, a[1].as_f64().unwrap_or(0.0));
                }
            }
            _bans_load();
            bans_state()
        }
        "update_target" => {
            let ud = dir().join("update-data");
            let _ = std::fs::remove_dir_all(&ud);
            if let Some(m) = a[0].as_object() {
                let _ = std::fs::create_dir_all(ud.join("manifests").join("stable"));
                let _ = std::fs::create_dir_all(ud.join("blobs"));
                for (rel, txt) in m {
                    let p = ud.join(rel);
                    let _ = std::fs::create_dir_all(p.parent().unwrap());
                    let _ = std::fs::write(&p, txt.as_str().unwrap_or("").as_bytes());
                }
            }
            reset_target();
            json!(update_target())
        }
        "elo_shown" => json!(elo_shown(a[0].as_f64().unwrap_or(0.0), a[1].as_i64().unwrap_or(0))),
        _ => Value::Null,
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    std::env::set_var("T2R_ELO_NO_PERF", "0");
    let mut n = 0usize;
    for c in script(input) {
        n += 1;
        if let Some(t) = c.get("t").and_then(|v| v.as_f64()) {
            crate::clock::freeze(t);
            TEST_MONO.store(t.to_bits(), Ordering::Relaxed);
        }
        if c["op"].as_str() == Some("seed") {
            seed(&c["s"]);
            emit(out, n, Value::Null, Value::Null);
            continue;
        }
        let a = c["a"].as_array().cloned().unwrap_or_default();
        let r = call(c["f"].as_str().unwrap_or(""), &a);
        emit(out, n, r, state());
    }
    0
}
