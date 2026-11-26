//! `t2r-daemon selftest k2`: the Rust side of verify/py/k2_harness.py (differential test of the K2 clan glue: clan_key / clan_report / clan_finalize / match end by
//! reports / match start and end / boosters / the clan ops / leaderboard table / admin clan reset + restore). The Python side imports daemon.py with the real
//! clan engine and the same doubles as src/d/ext_k2.rs; see the docstring of the harness.
use super::*;
use crate::c_util::{emit, jb, jf, ji, js, script};
use indexmap::IndexMap;
use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};
use std::sync::atomic::Ordering;

fn dir() -> std::path::PathBuf {
    std::path::Path::new(&db::CFG.path).parent().map(|p| p.to_path_buf()).unwrap_or_default()
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

fn strs(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}

fn tkey(v: &Value) -> (String, String) {
    let a = strs(v);
    (a.first().cloned().unwrap_or_default(), a.get(1).cloned().unwrap_or_default())
}

fn kk(k: &(String, String)) -> Value {
    json!([k.0, k.1])
}

fn sorted_pairs<V>(m: &HashMap<(String, String), V>, f: &dyn Fn(&V) -> Value) -> Value {
    let mut v: Vec<(&(String, String), &V)> = m.iter().collect();
    v.sort_by(|a, b| a.0.cmp(b.0));
    Value::Array(v.into_iter().map(|(k, x)| json!([kk(k), f(x)])).collect())
}

fn sorted_str<V>(m: &HashMap<String, V>, f: &dyn Fn(&V) -> Value) -> Value {
    let mut v: Vec<(&String, &V)> = m.iter().collect();
    v.sort_by(|a, b| a.0.cmp(b.0));
    Value::Array(v.into_iter().map(|(k, x)| json!([k, f(x)])).collect())
}

fn set_pairs(s: &HashSet<(String, String)>) -> Value {
    let mut v: Vec<&(String, String)> = s.iter().collect();
    v.sort();
    Value::Array(v.into_iter().map(kk).collect())
}

fn sorted_set(s: &HashSet<String>) -> Value {
    let mut v: Vec<&String> = s.iter().collect();
    v.sort();
    json!(v)
}

fn counts_items(m: &IndexMap<i64, i64>) -> Value {
    Value::Array(m.iter().map(|(k, v)| json!([k, v])).collect())
}

fn sorted_counts(m: &IndexMap<i64, i64>) -> Value {
    let mut v: Vec<(&i64, &i64)> = m.iter().collect();
    v.sort();
    Value::Array(v.into_iter().map(|(k, x)| json!([k, x])).collect())
}

fn state() -> Value {
    let pend = {
        let p = CLAN_PENDING.lock();
        let mut v: Vec<(&String, &Pending)> = p.iter().collect();
        v.sort_by(|a, b| a.0.cmp(b.0));
        Value::Array(v.into_iter().map(|(t, p)| json!([t, p.0, p.1, p.2, sorted_counts(&p.3)])).collect())
    };
    let mm = {
        let g = MM.lock.lock();
        let mut m: Vec<(&String, &Vec<String>)> = g.matches.iter().collect();
        m.sort_by(|a, b| a.0.cmp(b.0));
        let mut dep: Vec<(&String, Vec<&String>)> = g.departed.iter().map(|(k, v)| (k, { let mut x: Vec<&String> = v.iter().collect(); x.sort(); x })).collect();
        dep.sort_by(|a, b| a.0.cmp(b.0));
        let mut join: Vec<(&String, &(String, f64))> = g.joining.iter().collect();
        join.sort_by(|a, b| a.0.cmp(b.0));
        let mut lm: Vec<(&String, &(String, f64))> = g.late_mult.iter().collect();
        lm.sort_by(|a, b| a.0.cmp(b.0));
        let mut lb: Vec<(&String, &(String, i64))> = g.late_bonus.iter().collect();
        lb.sort_by(|a, b| a.0.cmp(b.0));
        let mut lp: Vec<(&String, &(String, f64, i64))> = g.late_prog.iter().collect();
        lp.sort_by(|a, b| a.0.cmp(b.0));
        let pres: Vec<Value> = {
            let mp = MATCH_PRES.lock();
            let mut ks: Vec<&String> = mp.keys().collect();
            ks.sort();
            ks.into_iter()
                .map(|k| {
                    let mut ts: Vec<(&String, &[f64; 2])> = mp[k].iter().collect();
                    ts.sort_by(|a, b| a.0.cmp(b.0));
                    json!([k, ts.into_iter().map(|(t, x)| json!([t, [x[0], x[1]]])).collect::<Vec<_>>()])
                })
                .collect()
        };
        let jl: Vec<Value> = {
            let j = JIP_LATE.lock();
            let mut v: Vec<(&String, &(String, f64))> = j.iter().collect();
            v.sort_by(|a, b| a.0.cmp(b.0));
            v.into_iter().map(|(t, x)| json!([t, [x.0, x.1]])).collect()
        };
        json!({
            "lp": lp.into_iter().map(|(k, v)| json!([k, [v.0, v.1, v.2]])).collect::<Vec<_>>(),
            "pres": pres,
            "jl": jl,
            "m": m.into_iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
            "st": sorted_set(&g.started), "en": sorted_set(&g.ended),
            "dep": dep.into_iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
            "join": join.into_iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>(),
            "lm": lm.into_iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>(),
            "lb": lb.into_iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>(),
        })
    };
    let rend = {
        let r = CLAN_REPORTED_END.lock();
        let mut v: Vec<(&String, &IndexMap<String, f64>)> = r.iter().collect();
        v.sort_by(|a, b| a.0.cmp(b.0));
        Value::Array(v.into_iter().map(|(m, x)| json!([m, x.iter().map(|(t, f)| json!([t, f])).collect::<Vec<_>>()])).collect())
    };
    let achdone = {
        let d = ACH_DONE.lock();
        let mut v: Vec<(&(String, String), Value)> = d
            .iter()
            .map(|(k, x)| {
                let mut y: Vec<(&i64, &i64)> = x.iter().collect();
                y.sort();
                (k, Value::Array(y.into_iter().map(|(a, b)| json!([a, b])).collect()))
            })
            .collect();
        v.sort_by(|a, b| a.0.cmp(b.0));
        Value::Array(v.into_iter().map(|(k, x)| json!([kk(k), x])).collect())
    };
    let team: Vec<Value> = CLAN_TEAM
        .lock()
        .iter()
        .map(|(m, v)| {
            let mut y: Vec<(&String, &i64)> = v.iter().collect();
            y.sort();
            json!([m, y.into_iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>()])
        })
        .collect();
    let feedseen = {
        let mut k = clan_ops::feed_seen_keys();
        k.sort();
        json!(k)
    };
    json!({
        "cm": sorted_str(&CLAN_MATCH.lock(), &|v| json!(v)),
        "map": CLAN_MAP.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
        "team": team,
        "ended": CLAN_ENDED.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
        "pend": pend,
        "cred": set_pairs(&CLAN_CREDITED.lock()), "quit": set_pairs(&CLAN_QUITTERS.lock()), "exc": set_pairs(&CLAN_EXCUSED.lock()),
        "excwhy": sorted_pairs(&CLAN_EXCUSE_WHY.lock(), &|v| json!(v)), "done": sorted_str(&CLAN_DONE.lock(), &|v| json!(v)),
        "leftat": sorted_pairs(&CLAN_LEFT_AT.lock(), &|v| json!(v)), "rej": sorted_pairs(&CLAN_REJOINED.lock(), &|v| json!(v)), "moved": sorted_pairs(&CLAN_MOVED.lock(), &|v| json!(v)),
        "rebook": set_pairs(&CLAN_REBOOK.lock()),
        "kicked": CLAN_KICKED.lock().iter().map(|(k, v)| json!([kk(k), v])).collect::<Vec<_>>(),
        "aborted": CLAN_ABORTED.lock().iter().map(|(k, v)| json!([kk(k), v])).collect::<Vec<_>>(),
        "due": CLAN_DUE.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
        "linkex": sorted_str(&CLAN_LINK_EXCUSES.lock(), &|v| json!(v)), "lostex": sorted_str(&CLAN_LOST_EXCUSES.lock(), &|v| json!(v)),
        "win": sorted_pairs(&CLAN_WIN.lock().iter().map(|(k, v)| (k.clone(), *v)).collect::<HashMap<_, _>>(), &|v| json!(v)),
        "bp": sorted_str(&BOOST_PICK.lock(), &|v| json!(v)), "ba": sorted_str(&BOOST_ACTIVE.lock(), &|v| json!(v)),
        "achdone": achdone,
        "achpend": ACH_PEND.lock().iter().map(|(k, v)| json!([kk(k), counts_items(v)])).collect::<Vec<_>>(),
        "achday": ACH_DAY.lock().iter().map(|(k, v)| json!([kk(k), counts_items(&v.e2), v.win, v.left, v.late])).collect::<Vec<_>>(),
        "achlive": ACH_LIVE_AT.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
        "rend": rend, "ebr": sorted_set(&END_BY_REPORTS.lock()), "rb": sorted_set(&RESULT_BOOKED.lock()),
        "lastm": sorted_str(&CLAN_LAST_MATCH.lock(), &|v| json!(v)), "privlast": sorted_str(&PRIV_LAST_MID.lock(), &|v| json!(v)),
        "feedseen": feedseen,
        "perklog": sorted_set(&CLAN_PERK_LOGGED.lock()),
        "etimers": sorted_set(&END_TIMERS.lock()), "ntimers": test_timers_len(),
        "mm": mm,
    })
}

fn dump_db() -> Value {
    let mut out = Map::new();
    let r = db::with_write(|c| {
        for n in ["players", "clans", "boosters", "clan_snap", "clan_story", "clan_feed", "pstats", "pweapon", "names", "dbl_notes", "fake_notes", "clan_journal", "clan_alife", "ach", "ach_count"] {
            out.insert(n.to_string(), table(c, &format!("SELECT * FROM \"{}\" ORDER BY rowid", n)));
        }
        Ok(())
    });
    let _ = r;
    let bd = dir().join("clan-resets");
    let mut files = Map::new();
    if let Ok(rd) = std::fs::read_dir(&bd) {
        let mut names: Vec<String> = rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        names.sort();
        for n in names {
            if let Ok(b) = std::fs::read(bd.join(&n)) {
                files.insert(n, Value::String(sha1_hex(&b)));
            }
        }
    }
    out.insert("resets".into(), Value::Object(files));
    Value::Object(out)
}

/// SHA-1 (only for comparing the backup files with the Python side)
fn sha1_hex(data: &[u8]) -> String {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut msg = data.to_vec();
    let ml = (data.len() as u64) * 8;
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&ml.to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([chunk[4 * i], chunk[4 * i + 1], chunk[4 * i + 2], chunk[4 * i + 3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | ((!b) & d), 0x5A827999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }
    h.iter().map(|x| format!("{:08x}", x)).collect()
}

fn mm_set(m: &Value) {
    let mut g = MM.lock.lock();
    if let Some(o) = m.get("matches").and_then(|x| x.as_object()) {
        for (mid, pl) in o {
            g.matches.insert(mid.clone(), strs(pl));
        }
    }
    for s in strs(&m["started"]) {
        g.started.insert(s);
    }
    for s in strs(&m["ended"]) {
        g.ended.insert(s);
    }
    if let Some(o) = m.get("departed").and_then(|x| x.as_object()) {
        for (mid, ts) in o {
            g.departed.insert(mid.clone(), strs(ts).into_iter().collect());
        }
    }
    if let Some(o) = m.get("joining").and_then(|x| x.as_object()) {
        for (t, v) in o {
            g.joining.insert(t.clone(), (v[0].as_str().unwrap_or("").to_string(), v[1].as_f64().unwrap_or(0.0)));
        }
    }
    if let Some(o) = m.get("late_mult").and_then(|x| x.as_object()) {
        for (t, v) in o {
            g.late_mult.insert(t.clone(), (v[0].as_str().unwrap_or("").to_string(), v[1].as_f64().unwrap_or(1.0)));
        }
    }
    if let Some(o) = m.get("late_bonus").and_then(|x| x.as_object()) {
        for (t, v) in o {
            g.late_bonus.insert(t.clone(), (v[0].as_str().unwrap_or("").to_string(), v[1].as_i64().unwrap_or(0)));
        }
    }
    if let Some(o) = m.get("late_prog").and_then(|x| x.as_object()) {
        for (t, v) in o {
            g.late_prog.insert(t.clone(), (v[0].as_str().unwrap_or("").to_string(), v[1].as_f64().unwrap_or(0.0), v[2].as_i64().unwrap_or(0)));
        }
    }
    if let Some(o) = m.get("pres").and_then(|x| x.as_object()) {
        let mut mp = MATCH_PRES.lock();
        for (mid, v) in o {
            let e: indexmap::IndexMap<String, [f64; 2]> =
                v.as_object().map(|oo| oo.iter().map(|(t, x)| (t.clone(), [x[0].as_f64().unwrap_or(0.0), x[1].as_f64().unwrap_or(0.0)])).collect()).unwrap_or_default();
            mp.insert(mid.clone(), e);
        }
    }
    if let Some(o) = m.get("jlate").and_then(|x| x.as_object()) {
        let mut j = JIP_LATE.lock();
        for (t, v) in o {
            j.insert(t.clone(), (v[0].as_str().unwrap_or("").to_string(), v[1].as_f64().unwrap_or(0.0)));
        }
    }
}

fn world(w: &Value) {
    PRIV_MIDS.lock().clear();
    for m in strs(&w["priv"]) {
        PRIV_MIDS.lock().insert(m);
    }
    {
        let mut s = SILENCE.lock();
        s.clear();
        if let Some(o) = w.get("silent").and_then(|x| x.as_object()) {
            for (k, v) in o {
                s.insert(k.clone(), v.as_f64().unwrap_or(0.0));
            }
        }
    }
    TEST_LEAVE.store(jb(w, "test_leave"), Ordering::Relaxed);
    let roles = w.get("roles").and_then(|x| x.as_object()).cloned().unwrap_or_default();
    for fn_ in ["devs.txt", "web-admins.txt", "og-steamids.txt"] {
        let p = dir().join(fn_);
        match roles.get(fn_) {
            Some(t) => {
                let _ = std::fs::write(&p, t.as_str().unwrap_or("").as_bytes());
            }
            None => {
                let _ = std::fs::remove_file(&p);
            }
        }
    }
    let arr = |k: &str| w.get(k).and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let _ = db::with_write(|c| {
        let tabs: Vec<String> = {
            let mut st = c.prepare("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")?;
            let it = st.query_map([], |r| r.get::<_, String>(0))?;
            it.filter_map(|x| x.ok()).collect()
        };
        for n in tabs {
            c.execute_batch(&format!("DELETE FROM \"{}\"", n))?;
        }
        let _ = c.execute_batch("DELETE FROM sqlite_sequence");
        crate::names::init(c)?;
        let _ = elo_ensure(c);
        for r in arr("players") {
            c.execute(
                "INSERT INTO players(token,name,steamid,kills,deaths,wins,matches,draws,last_seen) VALUES (?,?,?,?,?,?,?,?,?)",
                rusqlite::params![r[0].as_str(), r[1].as_str(), r[2].as_i64(), r[3].as_i64(), r[4].as_i64(), r[5].as_i64(), r[6].as_i64(), r[7].as_i64(), r[8].as_f64()],
            )?;
        }
        for r in arr("names") {
            c.execute("INSERT OR REPLACE INTO names(token_id,name,tag,updated) VALUES (?,?,?,?)", rusqlite::params![r[0].as_str(), r[1].as_str(), r[2].as_str(), r[3].as_f64()])?;
        }
        Ok(())
    });
    k2_reset();
    reset_caches();
    *MM.lock.lock() = MMState::new();
    JIP_LATE.lock().clear();
    MATCH_PRES.lock().clear();
    {
        let mut ts = TEAM_SWAP.lock();
        ts.clear();
        if let Some(o) = w.get("swap").and_then(|x| x.as_object()) {
            for (mid, m) in o {
                ts.insert(mid.clone(), m.as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_i64().unwrap_or(0))).collect()).unwrap_or_default());
            }
        }
    }
    if let Some(o) = w.get("restored").and_then(|x| x.as_object()) {
        for (mid, t) in o {
            RESTORED_T.lock().insert(mid.clone(), t.as_f64().unwrap_or(0.0));
        }
    }
    mm_set(&w["mm"]);
    let _ = std::fs::remove_dir_all(dir().join("clan-resets"));
}

fn setkey(c: &Value) {
    let n = js(c, "name");
    let k = c.get("key").cloned().unwrap_or(Value::Null);
    let v = c.get("val").cloned().unwrap_or(Value::Null);
    let ks = |v: &Value| v.as_str().unwrap_or("").to_string();
    match n {
        "CLAN_QUITTERS" => {
            CLAN_QUITTERS.lock().insert(tkey(&k));
        }
        "CLAN_EXCUSED" => {
            CLAN_EXCUSED.lock().insert(tkey(&k));
        }
        "CLAN_REBOOK" => {
            CLAN_REBOOK.lock().insert(tkey(&k));
        }
        "CLAN_CREDITED" => {
            CLAN_CREDITED.lock().insert(tkey(&k));
        }
        "CLAN_CLOSE_DROPS" => {
            CLAN_CLOSE_DROPS.lock().insert(tkey(&k));
        }
        "CLAN_WIN" => {
            CLAN_WIN.lock().insert(tkey(&k), v.as_bool());
        }
        "CLAN_KICKED" => {
            CLAN_KICKED.lock().insert(tkey(&k), ks(&v));
        }
        "CLAN_ABORTED" => {
            CLAN_ABORTED.lock().insert(tkey(&k), ks(&v));
        }
        "CLAN_EXCUSE_WHY" => {
            CLAN_EXCUSE_WHY.lock().insert(tkey(&k), ks(&v));
        }
        "CLAN_LEFT_AT" => {
            CLAN_LEFT_AT.lock().insert(tkey(&k), v.as_f64().unwrap_or(0.0));
        }
        "CLAN_REJOINED" => {
            CLAN_REJOINED.lock().insert(tkey(&k), v.as_f64().unwrap_or(0.0));
        }
        "CLAN_MOVED" => {
            CLAN_MOVED.lock().insert(tkey(&k), v.as_f64().unwrap_or(0.0));
        }
        "CLAN_DONE" => {
            CLAN_DONE.lock().insert(ks(&k), ks(&v));
        }
        "BOOST_PICK" => {
            BOOST_PICK.lock().insert(ks(&k), v.as_i64().unwrap_or(0));
        }
        "CLAN_ENDED" => {
            CLAN_ENDED.lock().insert(ks(&k), v.as_f64().unwrap_or(0.0));
        }
        "CLAN_MATCH" => {
            CLAN_MATCH.lock().insert(ks(&k), ks(&v));
        }
        "CLAN_MAP" => {
            CLAN_MAP.lock().insert(ks(&k), ks(&v));
        }
        "RESULT_BOOKED" => {
            RESULT_BOOKED.lock().insert(ks(&k));
        }
        "CLAN_LINK_EXCUSES" => {
            CLAN_LINK_EXCUSES.lock().insert(ks(&k), v.as_array().map(|a| a.iter().filter_map(|x| x.as_f64()).collect()).unwrap_or_default());
        }
        "CLAN_PENDING" => {
            let counts: IndexMap<i64, i64> = v[3].as_array().map(|a| a.iter().map(|p| (p[0].as_i64().unwrap_or(0), p[1].as_i64().unwrap_or(0))).collect()).unwrap_or_default();
            CLAN_PENDING.lock().insert(ks(&k), (v[0].as_str().unwrap_or("").to_string(), v[1].as_i64().unwrap_or(0), v[2].as_i64().unwrap_or(0), counts));
        }
        _ => {}
    }
}

fn unhex(s: &str) -> Vec<u8> {
    (0..s.len() / 2).map(|i| u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).unwrap_or(0)).collect()
}

fn hexr(b: Option<Vec<u8>>) -> Value {
    match b {
        Some(b) => Value::String(b.iter().map(|x| format!("{:02x}", x)).collect()),
        None => Value::Null,
    }
}

fn exc() -> Value {
    json!("EXC")
}

fn hexr_db(r: db::DbResult<Option<Vec<u8>>>) -> Value {
    match r {
        Ok(b) => hexr(b),
        Err(_) => exc(),
    }
}

fn one(c: &Value) -> Value {
    let o = js(c, "op");
    match o {
        "world" => {
            world(&c["w"]);
            Value::Null
        }
        "unset" => {
            let k = c.get("key").cloned().unwrap_or(Value::Null);
            let ks = |v: &Value| v.as_str().unwrap_or("").to_string();
            match js(c, "name") {
                "CLAN_CREDITED" => {
                    CLAN_CREDITED.lock().remove(&tkey(&k));
                }
                "CLAN_EXCUSED" => {
                    CLAN_EXCUSED.lock().remove(&tkey(&k));
                }
                "CLAN_QUITTERS" => {
                    CLAN_QUITTERS.lock().remove(&tkey(&k));
                }
                "CLAN_REBOOK" => {
                    CLAN_REBOOK.lock().remove(&tkey(&k));
                }
                "CLAN_LEFT_AT" => {
                    CLAN_LEFT_AT.lock().remove(&tkey(&k));
                }
                "CLAN_REJOINED" => {
                    CLAN_REJOINED.lock().remove(&tkey(&k));
                }
                "CLAN_MOVED" => {
                    CLAN_MOVED.lock().remove(&tkey(&k));
                }
                "CLAN_DONE" => {
                    CLAN_DONE.lock().remove(&ks(&k));
                }
                _ => {}
            }
            Value::Null
        }
        "setsid" => {
            let _ = db::with_write(|cn| {
                cn.execute("UPDATE players SET steamid=? WHERE token=?", rusqlite::params![ji(c, "sid"), js(c, "token")])?;
                Ok(())
            });
            Value::Null
        }
        "restored" => {
            RESTORED_T.lock().insert(js(c, "mid").to_string(), jf(c, "t"));
            Value::Null
        }
        "priv" => {
            for m in strs(&c["mids"]) {
                PRIV_MIDS.lock().insert(m);
            }
            Value::Null
        }
        "mmset" => {
            mm_set(&c["m"]);
            Value::Null
        }
        "set" => {
            setkey(c);
            Value::Null
        }
        "silent" => {
            let mut s = SILENCE.lock();
            s.clear();
            if let Some(m) = c["m"].as_object() {
                for (k, v) in m {
                    s.insert(k.clone(), v.as_f64().unwrap_or(0.0));
                }
            }
            Value::Null
        }
        "started" => {
            clan_match_started(&strs(&c["players"]), js(c, "mid"));
            Value::Null
        }
        "ended" => {
            clan_match_ended(&strs(&c["players"]), js(c, "mid"));
            Value::Null
        }
        "report" => {
            let counts: IndexMap<i64, i64> = c["counts"].as_array().map(|a| a.iter().map(|p| (p[0].as_i64().unwrap_or(0), p[1].as_i64().unwrap_or(0))).collect()).unwrap_or_default();
            match clan_report(js(c, "token"), ji(c, "parts"), ji(c, "flags"), counts) {
                Ok(st) => json!(crate::clan::clan::state_text(&st)),
                Err(_) => exc(),
            }
        }
        "finalize" => match clan_finalize(js(c, "token")) {
            Ok(()) => json!("ok"),
            Err(_) => exc(),
        },
        "tick" => {
            clan_scheduler_tick(crate::clock::now());
            json!("ok")
        }
        "timers" => json!(test_fire_timers(crate::clock::now())),
        "mer" => json!(match_end_via_reports(js(c, "mid"), None)),
        "booster" => {
            let token = js(c, "token").to_string();
            let r = db::with_write(|cn| {
                let k = clan_key_r(cn, &token)?;
                Ok(crate::clan::clan::booster_grant(cn, &k, ji(c, "bid"), ji(c, "n"))?)
            });
            match r {
                Ok(n) => json!(n),
                Err(_) => exc(),
            }
        }
        "frame" => {
            let pl = unhex(js(c, "pl"));
            let code = ji(c, "code") as u8;
            match code {
                0xE0 | 0xE2 | 0xE4 => hexr_db(clan_op(code, &pl)),
                0xE8 | 0xEE | 0xEF => hexr_db(meta_op(code, &pl)),
                0xD0 | 0xD2 => hexr_db(names_op(code, &pl)),
                0xB4 => hexr(feed_op(&pl)),
                0xB6 => hexr(clan13_op(&pl)),
                0xBF => hexr_db(rank_table_op(&pl)),
                0xA9 => hexr(ach_report_op(&pl)),
                0xAB => hexr(ach_live_op(&pl)),
                0xAA => hexr(ach_read_op(&pl)),
                _ => Value::Null,
            }
        }
        "lb" => match leaderboard_table(ji(c, "top") as usize) {
            Ok(b) => hexr(Some(b)),
            Err(_) => exc(),
        },
        "admin_reset" => admin_clan_reset(&c["body"]),
        "admin_restore" => {
            let mut body = c["body"].clone();
            if body.get("file").and_then(|f| f.as_str()) == Some("@last") {
                let bd = dir().join("clan-resets");
                let mut fs: Vec<String> = std::fs::read_dir(&bd).map(|rd| rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|n| n.ends_with(".json")).collect()).unwrap_or_default();
                fs.sort();
                body["file"] = json!(fs.last().cloned().unwrap_or_default());
            }
            admin_clan_restore(&body)
        }
        "dump" => dump_db(),
        _ => Value::Null,
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    // T2R_K2_DOUBLES=0: the real clan modules on both sides (the Python harness drops its doubles then too)
    crate::clan::ext_clan_story::set_test_fake(std::env::var("T2R_K2_DOUBLES").map(|v| v != "0").unwrap_or(true));
    TEST_TIMERS_ON.store(true, Ordering::Relaxed);
    TEST_NO_LOADED_CLEAR.store(true, Ordering::Relaxed);
    crate::clan::clan::capture_prints(true);
    let _ = std::fs::create_dir_all(dir());
    let mut n = 0usize;
    for c in script(input) {
        n += 1;
        if let Some(t) = c.get("t").and_then(|v| v.as_f64()) {
            crate::clock::freeze(t);
            TEST_MONO.store(t.to_bits(), Ordering::Relaxed);
        }
        let r = one(&c);
        for line in crate::clan::clan::take_prints() {
            dlog!("PRINT", line); // print() of the clan modules (missions: pool rolled ...)
        }
        let s = if js(&c, "op") == "dump" { Value::Null } else { state() };
        emit(out, n, r, s);
    }
    let _ = jf(&json!({}), "x");
    0
}
