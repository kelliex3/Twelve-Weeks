//! `t2r-daemon selftest k5`: the Rust side of verify/py/k5_harness.py (differential test of the K5 chunk: handle_batch and its helpers).
use super::*;
use crate::c_util::{emit, script};
use crate::names;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{BufRead, Read, Write};
use std::net::TcpStream;
use std::os::unix::io::FromRawFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::Ordering;

fn strs(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}

fn hx(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}
fn unhx(s: &str) -> Vec<u8> {
    (0..s.len() / 2).filter_map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()).collect()
}

fn tok(i: usize) -> String {
    let s = format!("{:02x}{}", i, "c0ffee11deadbeef".repeat(4));
    s[..64].to_string()
}

struct CE {
    conn: Conn,
    peer: UnixStream,
    buf: Vec<u8>,
}
static CONNS: std::sync::LazyLock<parking_lot::Mutex<HashMap<String, CE>>> = std::sync::LazyLock::new(|| parking_lot::Mutex::new(HashMap::new()));
static STS: std::sync::LazyLock<parking_lot::Mutex<HashMap<String, ConnSt>>> = std::sync::LazyLock::new(|| parking_lot::Mutex::new(HashMap::new()));

fn conn(cid: &str) -> Conn {
    let mut g = CONNS.lock();
    if let Some(c) = g.get(cid) {
        return c.conn.clone();
    }
    let mut fds = [0i32; 2];
    let rc = unsafe { libc::socketpair(libc::AF_UNIX, libc::SOCK_STREAM, 0, fds.as_mut_ptr()) };
    assert_eq!(rc, 0, "socketpair");
    let srv = unsafe { TcpStream::from_raw_fd(fds[0]) };
    let peer = unsafe { UnixStream::from_raw_fd(fds[1]) };
    peer.set_nonblocking(true).ok();
    let c = OutConn::new(srv, "127.0.0.1:0".parse().unwrap());
    g.insert(cid.to_string(), CE { conn: c.clone(), peer, buf: Vec::new() });
    c
}

fn cid_of(c: &Conn) -> String {
    CONNS.lock().iter().find(|(_, e)| std::sync::Arc::ptr_eq(&e.conn, c)).map(|(k, _)| k.clone()).unwrap_or_default()
}

fn drain_conns() -> Value {
    let mut sent = serde_json::Map::new();
    let mut g = CONNS.lock();
    let mut ids: Vec<String> = g.keys().cloned().collect();
    ids.sort();
    for id in ids {
        let e = g.get_mut(&id).unwrap();
        let mut tmp = [0u8; 65536];
        loop {
            match e.peer.read(&mut tmp) {
                Ok(0) => break,
                Ok(n) => e.buf.extend_from_slice(&tmp[..n]),
                Err(_) => break,
            }
        }
        if !e.buf.is_empty() {
            sent.insert(id, json!(hx(&e.buf)));
            e.buf.clear();
        }
    }
    Value::Object(sent)
}

fn dir() -> std::path::PathBuf {
    std::path::Path::new(&db::CFG.path).parent().map(|p| p.to_path_buf()).unwrap_or_default()
}

fn sorted_items<V: Clone>(it: impl Iterator<Item = (String, V)>, f: &dyn Fn(&V) -> Value) -> Value {
    let mut v: Vec<(String, V)> = it.collect();
    v.sort_by(|a, b| a.0.cmp(&b.0));
    json!(v.iter().map(|(k, x)| json!([k, f(x)])).collect::<Vec<_>>())
}

fn sorted_set<'a>(it: impl Iterator<Item = &'a String>) -> Value {
    let mut v: Vec<&String> = it.collect();
    v.sort();
    json!(v)
}

fn dbdump() -> Value {
    let Ok(c) = rusqlite::Connection::open(&db::CFG.path) else { return Value::Null };
    let mut out = serde_json::Map::new();
    let tables: Vec<String> = {
        let mut s = c.prepare("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").unwrap();
        let v = s.query_map([], |r| r.get::<_, String>(0)).unwrap().filter_map(|x| x.ok()).collect();
        v
    };
    for t in tables {
        if t.starts_with("sqlite_") {
            continue;
        }
        let mut s = match c.prepare(&format!("SELECT * FROM {}", t)) {
            Ok(s) => s,
            Err(_) => continue,
        };
        let n = s.column_count();
        let mut rows: Vec<Value> = Vec::new();
        let mut q = s.query([]).unwrap();
        while let Ok(Some(r)) = q.next() {
            let mut row = Vec::new();
            for i in 0..n {
                use rusqlite::types::ValueRef;
                row.push(match r.get_ref(i).unwrap() {
                    ValueRef::Null => Value::Null,
                    ValueRef::Integer(x) => json!(x),
                    ValueRef::Real(x) => json!(x),
                    ValueRef::Text(b) => json!(String::from_utf8_lossy(b)),
                    ValueRef::Blob(b) => json!(hx(b)),
                });
            }
            rows.push(Value::Array(row));
        }
        rows.sort_by_key(|v| crate::py::dumps_compact(v));
        if !rows.is_empty() {
            out.insert(t, Value::Array(rows));
        }
    }
    Value::Object(out)
}

fn state() -> Value {
    let mut s = serde_json::Map::new();
    {
        let g = MM.lock.lock();
        s.insert("q".into(), json!(g.queue.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
        s.insert("matches".into(), json!(g.matches.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
        s.insert("started".into(), sorted_set(g.started.iter()));
        s.insert("st_t".into(), sorted_items(g.started_t.iter().map(|(k, v)| (k.clone(), *v)), &|v| json!(v)));
        s.insert("launched".into(), sorted_set(g.launched.iter()));
        s.insert(
            "dep".into(),
            sorted_items(g.departed.iter().map(|(k, v)| (k.clone(), v.clone())), &|v| {
                let mut x: Vec<&String> = v.iter().collect();
                x.sort();
                json!(x)
            }),
        );
        s.insert("ended".into(), sorted_set(g.ended.iter()));
        s.insert("joining".into(), json!(g.joining.iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>()));
        s.insert("left".into(), sorted_items(g.left.iter().map(|(k, v)| (k.clone(), *v)), &|v| json!([v.0, v.1])));
    }
    s.insert("loaded".into(), sorted_set(LOADED.lock().iter()));
    s.insert("loaded_t".into(), sorted_items(LOADED_T.lock().iter().map(|(k, v)| (k.clone(), *v)), &|v| json!(v)));
    s.insert("cdgot".into(), sorted_set(CD_GOT.lock().iter()));
    s.insert("lost".into(), sorted_items(SESS_LOST.lock().iter().map(|(k, v)| (k.clone(), v.clone())), &|v| json!([v.0, v.1])));
    s.insert("jstart".into(), sorted_items(JIP_STARTED.lock().iter().map(|(k, v)| (k.clone(), v.clone())), &|v| json!([v.0, v.1])));
    s.insert("jrepush".into(), sorted_items(JIP_REPUSH.lock().iter().map(|(k, v)| (k.clone(), *v)), &|v| json!([v.0, v.1])));
    s.insert("jlate".into(), sorted_items(JIP_LATE.lock().iter().map(|(k, v)| (k.clone(), v.clone())), &|v| json!([v.0, v.1])));
    s.insert("jwait".into(), sorted_items(JIP_WAIT_LOG.lock().iter().map(|(k, v)| (k.clone(), *v)), &|v| json!(v)));
    s.insert("cwin".into(), json!(CLAN_WIN.lock().iter().map(|(k, v)| json!([k.0, k.1, v])).collect::<Vec<_>>()));
    s.insert("booked".into(), sorted_set(RESULT_BOOKED.lock().iter()));
    s.insert("silent".into(), sorted_items(_SILENT.lock().iter().map(|(k, v)| (k.clone(), *v)), &|v| json!(v)));
    {
        let mut tm: Vec<(Vec<String>, f64, Value, Value)> = TIMING
            .lock()
            .iter()
            .map(|(k, t)| (k.iter().cloned().collect(), t.start, json!(t.marks.iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>()), json!(t.loaded.iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>())))
            .collect();
        tm.sort_by(|a, b| a.0.cmp(&b.0));
        s.insert("tm".into(), json!(tm.iter().map(|(k, st, m, l)| json!([k, st, m, l])).collect::<Vec<_>>()));
    }
    s.insert("pres".into(), PRES.with(|p| json!(p.players.iter().map(|(t, e)| json!([t, e.name, e.last])).collect::<Vec<_>>())));
    s.insert("br".into(), BRIDGES.with(|b| json!(b.by_token.iter().map(|(t, x)| json!([t, x.steamid, x.port, x.ip, x.name, cid_of(&x.conn)])).collect::<Vec<_>>())));
    s.insert("sess".into(), SESSIONS.with(|m| json!(m.iter().map(|(t, c)| json!([t, cid_of(c)])).collect::<Vec<_>>())));
    s.insert("nl".into(), sorted_items(NAMELESS_BY_IP.lock().iter().map(|(k, v)| (k.clone(), v.clone())), &|v| json!(v)));
    s.insert("ploc".into(), json!(PING_LOC.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    s.insert("cv".into(), json!(CLIENT_VERSIONS.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    s.insert(
        "hm".into(),
        json!((0..12)
            .map(|i| {
                let t = tok(i);
                let h = HM.has(&t);
                json!([t, h, if h { json!(HM.host_token(&t)) } else { Value::Null }])
            })
            .collect::<Vec<_>>()),
    );
    s.insert("hmx".into(), json!(["m0", "m1", "m2"].iter().map(|m| HM.export_match(m)).collect::<Vec<_>>()));
    s.insert("notes".into(), json!(SOCIAL.dump_notes().iter().map(|(t, l)| json!([t, l.iter().map(|n| json!([n.0, n.1, n.2, n.3])).collect::<Vec<_>>()])).collect::<Vec<_>>()));
    {
        let g = MM.lock.lock();
        let bk = &g.book;
        s.insert("book".into(), json!(bk.lobbies.iter().map(|(k, lb)| {
            let mut j: Vec<(&String, &f64)> = lb.joined.iter().collect();
            j.sort_by(|a, b| a.0.cmp(b.0));
            json!([k, lb.members, j, lb.away.iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>(), lb.created, lb.last_join, lb.mid, lb.src, lb.hold, lb.wait_until])
        }).collect::<Vec<_>>()));
        let mut of: Vec<(&String, &String)> = bk.of.iter().collect();
        of.sort();
        s.insert("bookof".into(), json!(of));
        let mut lf: Vec<(&String, &(String, f64, f64))> = bk.left.iter().collect();
        lf.sort_by(|a, b| a.0.cmp(b.0));
        s.insert("bookleft".into(), json!(lf.iter().map(|(k, v)| json!([k, [v.0, v.1, v.2]])).collect::<Vec<_>>()));
    }
    s.insert("party".into(), json!((0..12).map(|i| json!([tok(i), PARTIES.pid(&tok(i))])).collect::<Vec<_>>()));
    s.insert("db".into(), dbdump());
    {
        let sts = STS.lock();
        let mut ids: Vec<&String> = sts.keys().collect();
        ids.sort();
        let mut m = serde_json::Map::new();
        for id in ids {
            let st = &sts[id];
            m.insert(
                id.clone(),
                json!([st.token, st.name, st.authed, st.session, st.is_bridge, st.closed_sent, st.menu, st.started, st.sent_name, st.sent_lobby.iter().map(|x| hx(x)).collect::<Vec<_>>()]),
            );
        }
        s.insert("st".into(), Value::Object(m));
    }
    s.insert("sent".into(), drain_conns());
    Value::Object(s)
}

fn clear_all() {
    CONNS.lock().clear();
    STS.lock().clear();
    test_fire_timers_clear();
    *STUBS.lock() = StubWorld::default();
    CLIENT_VERSIONS.lock().clear();
    CLIENT_LANGS.lock().clear();
    MATCH_MAP.lock().clear();
    VOTES.lock().clear();
    MATCH_P2P.lock().clear();
    SID_KEEP.lock().clear();
    TIMING.lock().clear();
    TEAM_SWAP.lock().clear();
    MATCH_P1.lock().clear();
    PING.lock().clear();
    PING_AT.lock().clear();
    PING_LOC.lock().clear();
    LOC_FWD.lock().clear();
    LOC_HIST.lock().clear();
    LOC_SPIKES.lock().clear();
    REGION.lock().clear();
    REGION_CAND.lock().clear();
    PING_LAST.lock().clear();
    MEASURED_PAIR.lock().clear();
    PING_EXCESS.lock().clear();
    PING_CORR.lock().clear();
    PING_FORMED.lock().clear();
    MESH_ON.lock().clear();
    SESS_LOST.lock().clear();
    NAMELESS_BY_IP.lock().clear();
    PEERS.lock().clear();
    VOTE_SHOWN.lock().clear();
    JIP_HV.lock().clear();
    BRIDGES.with(|b| {
        b.by_token.clear();
        b.by_sid.clear();
    });
    SESSIONS.with(|m| m.clear());
    PRES.with(|p| {
        p.players.clear();
        p.udp_seen.clear();
    });
    LOADED_T.lock().clear();
    CLAN_WIN.lock().clear();
    CLAN_MAP.lock().clear();
    JIP_WAIT_LOG.lock().clear();
    JIP_STARTED.lock().clear();
    JIP_REPUSH.lock().clear();
    JIP_LATE.lock().clear();
    VOICE_PUSH.lock().last.clear();
    DED.prefs.lock().clear();
    MATCH_PRES.lock().clear();
    JIP_UNREACH.lock().clear();
    region_lock_reset();
    _SILENT.lock().clear();
    CLAN_LAST_MATCH.lock().clear();
    NEW_TOKENS.clear();
    LINUX_CLIENTS.lock().clear();
    LOADED.lock().clear();
    CD_GOT.lock().clear();
    POOL_LOGGED.lock().clear();
    RESULT_BOOKED.lock().clear();
    END_BY_REPORTS.lock().clear();
    VOTE_RECENT.lock().clear();
    *ROTATION.lock() = 2;
    *MM.lock.lock() = MMState::new();
    for f in ["client_versions.json", "linux_clients.json", "client_langs.json", "no_host.txt"] {
        let _ = std::fs::remove_file(dir().join(f));
    }
    {
        let mut nh = NO_HOST.lock();
        nh.mtime = None;
        nh.ids.clear();
        nh.names.clear();
    }
    PERF.clear();
    let _ = db::with_write(|c| {
        names::init(c)?;
        c.execute_batch("CREATE TABLE IF NOT EXISTS match_stats(token TEXT, kills INT, deaths INT, ts REAL)")?;
        let tables: Vec<String> = {
            let mut s = c.prepare("SELECT name FROM sqlite_master WHERE type='table'")?;
            let v = s.query_map([], |r| r.get::<_, String>(0))?.filter_map(|x| x.ok()).collect();
            v
        };
        for t in tables {
            if !t.starts_with("sqlite_") {
                c.execute_batch(&format!("DELETE FROM {}", t))?;
            }
        }
        Ok(())
    });
    reset_caches();
    for m in ["m0", "m1", "m2"] {
        HM.end(m);
    }
    for i in 0..12 {
        PARTIES.leave(&tok(i));
    }
}

fn run_batch(c: &Value) -> Value {
    let cid = c["cid"].as_str().unwrap_or("").to_string();
    let mut st = {
        let mut g = STS.lock();
        if c["new"].as_bool().unwrap_or(false) || !g.contains_key(&cid) {
            g.insert(cid.clone(), ConnSt::new());
        }
        g.remove(&cid).unwrap()
    };
    let mut batch: Batch = c["frames"].as_array().cloned().unwrap_or_default().iter().map(|f| (f[0].as_u64().unwrap_or(0) as u8, unhx(f[1].as_str().unwrap_or("")))).collect();
    let a = c["addr"].as_array().cloned().unwrap_or_default();
    let addr: Addr = format!("{}:{}", a[0].as_str().unwrap_or("0.0.0.0"), a[1].as_i64().unwrap_or(0)).parse().unwrap_or_else(|_| "0.0.0.0:0".parse().unwrap());
    let cn = conn(&cid);
    let r = handle_batch(&cn, addr, &mut st, &mut batch);
    STS.lock().insert(cid, st);
    match r {
        Ok(()) => Value::Null,
        Err(HErr::Abandoned) => json!("aband"),
        Err(HErr::Conn(_)) => json!("conn"),
        Err(HErr::Other(_)) => json!("other"),
    }
}

fn kind_db(e: &db::DbErr) -> &'static str {
    match e {
        db::DbErr::Abandoned => "exc:aband",
        _ => "exc:other",
    }
}

fn call(f: &str, a: &[Value]) -> Value {
    let s = |i: usize| a.get(i).and_then(|v| v.as_str()).unwrap_or("").to_string();
    let ss = |i: usize| a.get(i).map(strs).unwrap_or_default();
    match f {
        "loaded_clear" => {
            loaded_clear_match(&s(0), &ss(1));
            Value::Null
        }
        "asay" => {
            let (lid, err) = SOCIAL.admin_say(&s(0), &s(1), &s(2), &s(3));
            json!([lid, err])
        }
        "voice" => {
            let sent = voice_room_tick(Some(crate::clock::now()));
            Value::Array(sent.into_iter().map(|(t, pl)| json!([t, pl.iter().map(|b| format!("{:02x}", b)).collect::<String>()])).collect())
        }
        "jip_requeue" => json!(jip_requeue_if_empty(&s(0))),
        "prestart" => json!(prestart_host_lost(&s(0), &ss(1), &ss(2))),
        "mgs" => match maybe_game_start(&s(0)) {
            Ok(b) => json!(b),
            Err(e) => json!(kind_db(&e)),
        },
        "leave" => {
            let ad = a.get(2).and_then(|v| v.as_array()).cloned().unwrap_or_default();
            let addr: Addr = format!("{}:{}", ad[0].as_str().unwrap_or("0.0.0.0"), ad[1].as_i64().unwrap_or(0)).parse().unwrap_or_else(|_| "0.0.0.0:0".parse().unwrap());
            let st = ConnSt { token: a.first().and_then(|v| v.as_str()).map(|x| x.to_string()), name: s(1), ..ConnSt::new() };
            let _ = handle_leave(addr, &st);
            Value::Null
        }
        _ => Value::Null,
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    std::env::set_var("T2R_NO_COUNTDOWN", "1");
    TEST_NO_SLEEP.store(true, Ordering::Relaxed);
    TEST_TIMERS_ON.store(true, Ordering::Relaxed);
    STUB_LOG.store(true, Ordering::Relaxed);
    K5_FAKE_K2.store(true, Ordering::Relaxed);
    set_server_version("sv-test");
    clear_all();
    let mut n = 0usize;
    for c in script(input) {
        n += 1;
        if let Some(t) = c.get("t").and_then(|v| v.as_f64()) {
            crate::clock::freeze(t);
        }
        let op = c["op"].as_str().unwrap_or("");
        match op {
            "reset" => {
                clear_all();
                emit(out, n, Value::Null, state());
            }
            "env" => {
                let k = c["k"].as_str().unwrap_or("");
                match c["v"].as_str() {
                    Some(v) => std::env::set_var(k, v),
                    None => std::env::remove_var(k),
                }
                emit(out, n, Value::Null, Value::Null);
            }
            "seedrot" => {
                let _ = &*ROTATION;
                *ROTATION.lock() = c["v"].as_i64().unwrap_or(0);
                crate::rnd::reseed(c["seed"].as_i64().unwrap_or(0));
                emit(out, n, Value::Null, Value::Null);
            }
            "names" => {
                if let Some(m) = c["n"].as_object() {
                    for (t, nm) in m {
                        let _ = set_name(t, nm.as_str().unwrap_or(""));
                    }
                }
                emit(out, n, Value::Null, Value::Null);
            }
            "world" => {
                {
                    let mut w = STUBS.lock();
                    w.banned = strs(&c["banned"]).into_iter().collect();
                    w.banned_ips = strs(&c["banned_ips"]).into_iter().collect();
                    w.install = strs(&c["install"]).into_iter().collect();
                    w.loss_hold = strs(&c["loss_hold"]).into_iter().collect();
                    w.ac_ip = strs(&c["ac_ip"]).into_iter().collect();
                }
                {
                    let mut cm = CLAN_MAP.lock();
                    cm.clear();
                    for (k, v) in c["clanmap"].as_object().cloned().unwrap_or_default() {
                        cm.insert(k, v.as_str().unwrap_or("").to_string());
                    }
                }
                *RESULT_BOOKED.lock() = strs(&c["booked"]).into_iter().collect();
                *END_BY_REPORTS.lock() = strs(&c["endrep"]).into_iter().collect();
                {
                    let mut lm = CLAN_LAST_MATCH.lock();
                    lm.clear();
                    for (k, v) in c["lastmatch"].as_object().cloned().unwrap_or_default() {
                        lm.insert(k, v.as_str().unwrap_or("").to_string());
                    }
                }
                *LINUX_CLIENTS.lock() = strs(&c["linux"]).into_iter().collect();
                {
                    let mut cv = CLIENT_VERSIONS.lock();
                    cv.clear();
                    for (k, v) in c["cver"].as_object().cloned().unwrap_or_default() {
                        cv.insert(k, v.as_str().unwrap_or("").to_string());
                    }
                }
                emit(out, n, Value::Null, Value::Null);
            }
            "party" => {
                for i in 0..12 {
                    PARTIES.leave(&tok(i));
                }
                for g in c["groups"].as_array().cloned().unwrap_or_default() {
                    let g = strs(&g);
                    for m in &g[1..] {
                        PARTIES.invite_token(&g[0], m);
                        let _ = PARTIES.accept_token(m, &g[0]);
                    }
                }
                emit(out, n, Value::Null, Value::Null);
            }
            "mm" => {
                {
                    let mut g = MM.lock.lock();
                    g.matches.clear();
                    for (k, v) in c["matches"].as_object().cloned().unwrap_or_default() {
                        g.matches.insert(k, strs(&v));
                    }
                    g.launched = strs(&c["launched"]).into_iter().collect();
                    g.ended = strs(&c["ended"]).into_iter().collect();
                    g.departed.clear();
                    for (k, v) in c["departed"].as_object().cloned().unwrap_or_default() {
                        g.departed.insert(k, strs(&v).into_iter().collect());
                    }
                    g.queue.clear();
                    g.seen.clear();
                    let now = crate::clock::now();
                    for t in strs(&c["queue"]) {
                        g.queue.insert(t.clone(), now);
                        g.seen.insert(t, now);
                    }
                    g.left.clear();
                    g.joining.clear();
                    for (k, v) in c["joining"].as_object().cloned().unwrap_or_default() {
                        g.joining.insert(k, (v[0].as_str().unwrap_or("").to_string(), v[1].as_f64().unwrap_or(0.0)));
                    }
                    g.started = strs(&c["started"]).into_iter().collect();
                    g.started_t.clear();
                    for (k, v) in c["started_t"].as_object().cloned().unwrap_or_default() {
                        g.started_t.insert(k, v.as_f64().unwrap_or(0.0));
                    }
                }
                *LOADED.lock() = strs(&c["loaded"]).into_iter().collect();
                {
                    let mut jl = JIP_LATE.lock();
                    jl.clear();
                    for (k, v) in c["jlate"].as_object().cloned().unwrap_or_default() {
                        jl.insert(k, (v[0].as_str().unwrap_or("").to_string(), v[1].as_f64().unwrap_or(0.0)));
                    }
                }
                HM.end("zz");
                for mid in strs(&c["hm"]) {
                    let pl = MM.lock.lock().matches.get(&mid).cloned().unwrap_or_default();
                    HM.start(&mid, &pl);
                }
                {
                    let mut tm = TIMING.lock();
                    tm.clear();
                    for kv in c["timing"].as_array().cloned().unwrap_or_default() {
                        tm.insert(key_of(&strs(&kv[0])), Timing { start: kv[1].as_f64().unwrap_or(0.0), ..Default::default() });
                    }
                }
                emit(out, n, Value::Null, state());
            }
            "fire" => {
                test_fire_timers_all();
                emit(out, n, Value::Null, state());
            }
            "batch" => {
                let r = run_batch(&c);
                emit(out, n, r, state());
            }
            _ => {
                let a = c["a"].as_array().cloned().unwrap_or_default();
                let r = call(c["f"].as_str().unwrap_or(""), &a);
                emit(out, n, r, state());
            }
        }
    }
    0
}
