//! `t2r-daemon selftest k3`: the Rust side of verify/py/k3_harness.py (differential test of the K3 chunk: presence / bridges / sessions, versions /
//! languages / texts, maps / pools / vote, match records / session ids / teams, match start + countdown, ping locations / pair pings / host choice).
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

fn dir() -> std::path::PathBuf {
    std::path::Path::new(&db::CFG.path).parent().map(|p| p.to_path_buf()).unwrap_or_default()
}

fn hx(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}
fn hx_opt(b: Option<Vec<u8>>) -> Value {
    match b {
        Some(b) => json!(hx(&b)),
        None => Value::Null,
    }
}
fn unhx(s: &str) -> Vec<u8> {
    (0..s.len() / 2).filter_map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).ok()).collect()
}

struct CE {
    conn: Conn,
    peer: UnixStream,
    buf: Vec<u8>,
}
static CONNS: std::sync::LazyLock<parking_lot::Mutex<HashMap<String, CE>>> = std::sync::LazyLock::new(|| parking_lot::Mutex::new(HashMap::new()));

fn conn(cid: &str) -> Conn {
    let mut g = CONNS.lock();
    if let Some(c) = g.get(cid) {
        return c.conn.clone();
    }
    // a unix socket pair: what the daemon writes is readable at once (no loopback softirq delay)
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

fn read_file(name: &str) -> Value {
    match std::fs::read(dir().join(name)) {
        Ok(b) => json!(String::from_utf8_lossy(&b)),
        Err(_) => Value::Null,
    }
}

/// `sorted(frozenset((a, b)))`: one element when both are the same token
fn pair_json(k: &PairKey) -> Vec<String> {
    if k.0 == k.1 {
        vec![k.0.clone()]
    } else {
        vec![k.0.clone(), k.1.clone()]
    }
}

fn kj(k: &Key) -> Vec<String> {
    k.iter().cloned().collect()
}

fn state() -> Value {
    let mut s = serde_json::Map::new();
    s.insert("cv".into(), json!(CLIENT_VERSIONS.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut lc: Vec<String> = LINUX_CLIENTS.lock().iter().cloned().collect();
    lc.sort();
    s.insert("lc".into(), json!(lc));
    s.insert("cl".into(), json!(CLIENT_LANGS.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    s.insert("due".into(), json!(save_due()));
    let mut mm: Vec<(Vec<String>, i64)> = MATCH_MAP.lock().iter().map(|(k, v)| (kj(k), *v)).collect();
    mm.sort();
    s.insert("mm".into(), json!(mm.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut votes: Vec<(Vec<String>, Value, Value, i64)> = VOTES
        .lock()
        .iter()
        .map(|(k, v)| (kj(k), json!(v.opts), json!(v.votes.iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>()), v.winner))
        .collect();
    votes.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("votes".into(), json!(votes.iter().map(|(k, o, v, w)| json!([k, o, v, w])).collect::<Vec<_>>()));
    let mut p2p: Vec<(Vec<String>, bool)> = MATCH_P2P.lock().iter().map(|(k, v)| (kj(k), *v)).collect();
    p2p.sort();
    s.insert("p2p".into(), json!(p2p.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut sid: Vec<(Vec<String>, u32)> = SID_KEEP.lock().iter().map(|(k, v)| (kj(k), *v)).collect();
    sid.sort();
    s.insert("sid".into(), json!(sid.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    s.insert("rot".into(), json!(*ROTATION.lock()));
    s.insert("vr".into(), json!(*VOTE_RECENT.lock()));
    let mut vs: Vec<(i64, f64)> = VOTE_SHOWN.lock().iter().map(|(k, v)| (*k, *v)).collect();
    vs.sort_by_key(|x| x.0);
    s.insert("vs".into(), json!(vs.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut tm: Vec<(Vec<String>, f64, Value, Value)> = TIMING
        .lock()
        .iter()
        .map(|(k, t)| (kj(k), t.start, json!(t.marks.iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>()), json!(t.loaded.iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>())))
        .collect();
    tm.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("tm".into(), json!(tm.iter().map(|(k, st, m, l)| json!([k, st, m, l])).collect::<Vec<_>>()));
    s.insert("pl".into(), json!(pool_logged_len()));
    s.insert("pres".into(), PRES.with(|p| json!(p.players.iter().map(|(t, e)| json!([t, e.name, e.last])).collect::<Vec<_>>())));
    s.insert("udp".into(), json!(PRES.sizes().1));
    s.insert("br".into(), BRIDGES.with(|b| json!(b.by_token.iter().map(|(t, x)| json!([t, x.steamid, x.port, x.ip, x.name, cid_of(&x.conn)])).collect::<Vec<_>>())));
    s.insert(
        "bsid".into(),
        BRIDGES.with(|b| {
            let mut v: Vec<(u64, String)> = b.by_sid.iter().map(|(k, v)| (*k, v.clone())).collect();
            v.sort();
            json!(v.iter().map(|(k, t)| json!([k, t])).collect::<Vec<_>>())
        }),
    );
    s.insert("sess".into(), SESSIONS.with(|m| json!(m.iter().map(|(t, c)| json!([t, cid_of(c)])).collect::<Vec<_>>())));
    let mut lost: Vec<(String, (f64, String))> = SESS_LOST.lock().iter().map(|(k, v)| (k.clone(), v.clone())).collect();
    lost.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("lost".into(), json!(lost.iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>()));
    s.insert("ploc".into(), json!(PING_LOC.lock().iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    s.insert("ping".into(), json!(PING.lock().iter().map(|(k, v)| json!([[k.0, k.1], v.iter().collect::<Vec<_>>()])).collect::<Vec<_>>()));
    let mut pat: Vec<((String, String), f64)> = PING_AT.lock().iter().map(|(k, v)| (k.clone(), *v)).collect();
    pat.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("pat".into(), json!(pat.iter().map(|(k, v)| json!([[k.0, k.1], v])).collect::<Vec<_>>()));
    let mut plast: Vec<(PairKey, (Option<i64>, f64))> = PING_LAST.lock().iter().map(|(k, v)| (k.clone(), *v)).collect();
    plast.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("plast".into(), json!(plast.iter().map(|(k, v)| json!([pair_json(k), [v.0, v.1]])).collect::<Vec<_>>()));
    let mut reg: Vec<(String, String)> = REGION.lock().iter().map(|(k, v)| (k.clone(), v.clone())).collect();
    reg.sort();
    s.insert("reg".into(), json!(reg.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut rc: Vec<(String, (String, i64))> = REGION_CAND.lock().iter().map(|(k, v)| (k.clone(), v.clone())).collect();
    rc.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("rc".into(), json!(rc.iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>()));
    let mut lh: Vec<(String, Vec<i64>)> = LOC_HIST.lock().iter().map(|(k, v)| (k.clone(), v.iter().copied().collect())).collect();
    lh.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("lh".into(), json!(lh.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut ls: Vec<(String, i64)> = LOC_SPIKES.lock().iter().map(|(k, v)| (k.clone(), *v)).collect();
    ls.sort();
    s.insert("ls".into(), json!(ls.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut lf: Vec<(String, f64)> = LOC_FWD.lock().iter().map(|(k, v)| (k.clone(), *v)).collect();
    lf.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("lf".into(), json!(lf.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    s.insert("meas".into(), json!(MEASURED_PAIR.lock().iter().map(|(k, v)| json!([[k.0, k.1], [v.0, v.1]])).collect::<Vec<_>>()));
    s.insert("exc".into(), json!(PING_EXCESS.lock().iter().map(|(k, v)| json!([k, v.iter().map(|x| json!([x.0, x.1])).collect::<Vec<_>>()])).collect::<Vec<_>>()));
    let mut corr: Vec<(String, (i64, f64))> = PING_CORR.lock().iter().map(|(k, v)| (k.clone(), *v)).collect();
    corr.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("corr".into(), json!(corr.iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>()));
    s.insert(
        "formed".into(),
        json!(PING_FORMED
            .lock()
            .iter()
            .map(|(k, m)| {
                let mut e: Vec<(PairKey, i64)> = m.iter().map(|(p, v)| (p.clone(), *v)).collect();
                e.sort();
                json!([k, e.iter().map(|(p, v)| json!([pair_json(p), v])).collect::<Vec<_>>()])
            })
            .collect::<Vec<_>>()),
    );
    let mut mesh: Vec<(String, f64)> = MESH_ON.lock().iter().map(|(k, v)| (k.clone(), *v)).collect();
    mesh.sort_by(|a, b| a.0.cmp(&b.0));
    s.insert("mesh".into(), json!(mesh.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    let mut nl: Vec<(String, String)> = NAMELESS_BY_IP.lock().iter().map(|(k, v)| (k.clone(), v.clone())).collect();
    nl.sort();
    s.insert("nl".into(), json!(nl.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()));
    s.insert("peers".into(), json!(PEERS.lock().iter().map(|(k, v)| json!([k, [v.ip().to_string(), v.port()]])).collect::<Vec<_>>()));
    let mut ld: Vec<String> = LOADED.lock().iter().cloned().collect();
    ld.sort();
    s.insert("loaded".into(), json!(ld));
    let mut cd: Vec<String> = CD_GOT.lock().iter().cloned().collect();
    cd.sort();
    s.insert("cdgot".into(), json!(cd));
    s.insert("jhv".into(), json!(jip_hv_len()));
    let (a, b) = PERF.sizes();
    s.insert("perf".into(), json!([a, b]));
    s.insert(
        "files".into(),
        json!({"client_versions.json": read_file("client_versions.json"), "linux_clients.json": read_file("linux_clients.json"), "client_langs.json": read_file("client_langs.json")}),
    );
    s.insert("sent".into(), drain_conns());
    Value::Object(s)
}

fn clear_all() {
    CONNS.lock().clear();
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
    LINUX_CLIENTS.lock().clear();
    LOADED.lock().clear();
    CD_GOT.lock().clear();
    POOL_LOGGED.lock().clear();
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
        c.execute_batch("DELETE FROM players; DELETE FROM names;")?;
        Ok(())
    });
    reset_caches();
    save_tables_if_due(Some(f64::MAX));
}

fn j_opt_i(v: Option<i64>) -> Value {
    match v {
        Some(x) => json!(x),
        None => Value::Null,
    }
}

fn call(f: &str, a: &[Value]) -> Value {
    let s = |i: usize| a.get(i).and_then(|v| v.as_str()).unwrap_or("").to_string();
    let i = |i: usize| a.get(i).and_then(|v| v.as_i64()).unwrap_or(0);
    let fl = |i: usize| a.get(i).and_then(|v| v.as_f64()).unwrap_or(0.0);
    let b = |i: usize| a.get(i).and_then(|v| v.as_bool()).unwrap_or(false);
    let ss = |i: usize| a.get(i).map(strs).unwrap_or_default();
    let opt_s = |i: usize| a.get(i).and_then(|v| v.as_str()).map(|x| x.to_string());
    let opt_f = |i: usize| a.get(i).and_then(|v| v.as_f64());
    let opt_i = |i: usize| a.get(i).and_then(|v| v.as_i64());
    let opt_b = |i: usize| a.get(i).and_then(|v| v.as_bool());
    let addr_of = |i: usize| -> Option<Addr> {
        let v = a.get(i)?.as_array()?;
        format!("{}:{}", v.first()?.as_str()?, v.get(1)?.as_i64()?).parse().ok()
    };
    match f {
        "seq" => {
            let items = a.first().and_then(|v| v.as_array()).cloned().unwrap_or_default();
            Value::Array(items.iter().map(|x| call(x["f"].as_str().unwrap_or(""), &x["a"].as_array().cloned().unwrap_or_default())).collect())
        }
        "version_op" => json!(hx(&version_op(&unhx(&s(0)), addr_of(1).as_ref()))),
        "parse_version_request" => match parse_version_request(&unhx(&s(0))) {
            Some((v, t)) => json!([v, t]),
            None => Value::Null,
        },
        "note_lang" => {
            note_lang(&s(0), i(1) as u8);
            Value::Null
        }
        "lang_of" => json!(lang_of(&s(0))),
        "norm_lang" => {
            let kind = s(0);
            let r = match kind.as_str() {
                "bytes" => norm_lang_in(LangIn::Bytes(&unhx(&s(1)))),
                "none" => norm_lang_in(LangIn::None),
                "int" => norm_lang_in(LangIn::Int(i(1))),
                _ => norm_lang_in(LangIn::Str(&s(1))),
            };
            json!(r)
        }
        "srv_text" | "srv_note" => {
            let kw: Vec<(String, String)> = a
                .get(2)
                .and_then(|v| v.as_object())
                .map(|m| m.iter().map(|(k, v)| (k.clone(), if let Some(x) = v.as_str() { x.to_string() } else { v.to_string() })).collect())
                .unwrap_or_default();
            let kwr: Vec<(&str, String)> = kw.iter().map(|(k, v)| (k.as_str(), v.clone())).collect();
            if f == "srv_text" {
                json!(srv_text(&s(0), &s(1), &kwr))
            } else {
                json!(hx(&srv_note_kw(&s(0), &s(1), &kwr)))
            }
        }
        "version_problem" => json!(version_problem(&s(0))),
        "version_refusal" => json!(version_refusal(&s(0))),
        "save_tables" => {
            save_tables_if_due(opt_f(0));
            Value::Null
        }
        "save_soon" => {
            save_soon();
            Value::Null
        }
        "map_pool" => {
            let sup = a.get(1).filter(|v| !v.is_null()).map(strs);
            json!(map_pool(&ss(0), sup.as_deref()))
        }
        "map_index" => json!(map_index(&ss(0))),
        "vote_options" => json!(vote_options(VOTE_OPTIONS, &ss(0))),
        "vote_start" => json!(vote_start(&ss(0))),
        "vote_cast" => json!(vote_cast(&s(0), i(1))),
        "vote_close" => j_opt_i(vote_close(&ss(0))),
        "vote_state_text" => json!(vote_state_text(&s(0))),
        "vote_op" => hx_opt(vote_op(i(0) as u8, &unhx(&s(1)))),
        "sp_support_op" => hx_opt(sp_support_op(&unhx(&s(0)))),
        "map_packs_op" => hx_opt(map_packs_op(&unhx(&s(0)))),
        "p1" => {
            let leader = a.get(1).and_then(|v| v.as_str());
            json!([p1_pref_on(), p1_always(&s(0), leader), p1_owner(&s(0), leader), p1_fits(&s(0), &s(2), leader), p1_match(&ss(3))])
        }
        "maps_misc" => {
            let mut t: Vec<(String, i64)> = max_team_table().into_iter().collect();
            t.sort();
            let mut on: Vec<String> = sp_maps_on().into_iter().collect();
            on.sort();
            let (lo, bo, ld) = flow_stage_secs(opt_i(1));
            let steps: Vec<Value> = countdown_steps_flow(i(2), i(3), b(4), opt_b(5)).into_iter().map(|(p, n)| json!([p, n])).collect();
            let ml = maps_table();
            json!([
                t.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
                on,
                team_size(&ss(0)),
                [lo, bo, ld],
                flow_extra_secs(),
                steps,
                countdown_seconds_with(opt_i(6), opt_i(7)),
                map_vote_on(),
                match_flow_on(),
                ml.iter().map(|m| m.2.clone()).collect::<Vec<_>>(),
                ml.iter().map(|m| respawns_of(&m.2) as i64).collect::<Vec<_>>()
            ])
        }
        "match_sid" => json!(match_sid(&ss(0))),
        "teams" => {
            let tok = a.get(2).and_then(|v| v.as_str());
            json!([roster_team(i(0)), team_in(&s(1), i(0), tok), roster_team_in(&s(1), i(0), tok), team_of(i(0))])
        }
        "build_player_record" => {
            let roster: Vec<(i64, String)> = a[3].as_array().map(|r| r.iter().map(|x| (x[0].as_i64().unwrap_or(0), x[1].as_str().unwrap_or("").to_string())).collect()).unwrap_or_default();
            let relay = a.get(4).and_then(|v| v.as_array()).map(|v| (v[0].as_str().unwrap_or("").to_string(), v[1].as_i64().unwrap_or(0)));
            json!(hx(&build_player_record(i(0), i(1) as u32, i(2), &roster, relay, None)))
        }
        "build_peer_table" => json!(hx(&build_peer_table(&ss(0)))),
        "match_start_frame" => json!(hx(&match_start_frame(&s(0), &ss(1)))),
        "stage_name" => json!(stage_name(i(0), i(1))),
        "timing_mark" => json!(timing_mark(&ss(0), &s(1))),
        "timing_summary" => {
            timing_summary(&ss(0));
            Value::Null
        }
        "rekey" => {
            let (old, new) = (key_of(&ss(0)), key_of(&ss(1)));
            rekey_match_dicts(&old, &new);
            sid_keep_set(&new, i(2) as u32);
            Value::Null
        }
        "forget" => {
            match_dicts_pop(&key_of(&ss(0)));
            Value::Null
        }
        "start_match" => {
            start_match(&ss(0));
            Value::Null
        }
        "countdown" => {
            countdown_run(&ss(0));
            Value::Null
        }
        "ping_location" => {
            ping_location(&s(0), &s(1));
            Value::Null
        }
        "ping_report" => {
            ping_report(&s(0), a[1].as_u64().unwrap_or(0), i(2));
            Value::Null
        }
        "pair_ping" => j_opt_i(pair_ping_ex(&s(0), &s(1), b(2), opt_f(3))),
        "ping_kick_learn" => {
            ping_kick_learn(&s(0), &s(1), &s(2), i(3), opt_f(4));
            Value::Null
        }
        "mm_region" => json!(mm_region(&s(0))),
        "mesh_peers" => match mesh_peers(&s(0), None) {
            Some(p) => {
                let mut v: Vec<String> = p.into_iter().collect();
                v.sort();
                json!(v)
            }
            None => Value::Null,
        },
        "mesh_activate" => {
            mesh_activate(&s(0));
            Value::Null
        }
        "hm_start" => {
            HM.start(&s(0), &ss(1));
            Value::Null
        }
        "perf_op" => hx_opt(perf_op(&unhx(&s(0)))),
        "perf_penalty" => json!([perf_penalty(&s(0)), ping_match_ms()]),
        "best_host_order" => json!(best_host_order(&ss(0))),
        "host_pool" => {
            let (v, r) = host_pool(&ss(0));
            json!([v, r])
        }
        "host_allowed" => {
            let t = ss(0);
            json!([host_allowed(&t), t.iter().map(|x| no_host(x)).collect::<Vec<_>>()])
        }
        "push_peers" => {
            push_peers(&ss(0));
            Value::Null
        }
        "party_lead" => json!(party_lead_searching(&s(0))),
        "jip_hv" => {
            let (r, t) = jip_host_verdict(&s(0), i(1));
            json!([r, t])
        }
        "hc" => {
            let _ = HOSTCAP.host_report(&s(0), i(1), i(2), i(3), opt_s(4).as_deref());
            Value::Null
        }
        "pres" => {
            let k = s(0);
            match k.as_str() {
                "touch" => PRES.touch(&s(1), &s(2)),
                "known" => PRES.touch_known(&s(1)),
                "drop" => PRES.drop_token(&s(1)),
                "prune" => {
                    let keep: std::collections::HashSet<String> = ss(2).into_iter().collect();
                    return json!(PRES.prune(fl(1), &keep));
                }
                "ping" => {
                    if let Some(ad) = addr_of(1) {
                        PRES.ping(ad)
                    }
                }
                _ => {}
            }
            let nm = if a.get(1).is_some_and(|v| v.is_string()) { json!(PRES.name_of(&s(1))) } else { Value::Null };
            json!([PRES.count(), PRES.online_names(), nm])
        }
        "bridge" => {
            let k = s(0);
            match k.as_str() {
                "register" => {
                    BRIDGES.register(&s(1), conn(&s(2)), a[3].as_u64().unwrap_or(0), i(4), opt_s(5).as_deref(), opt_s(6).as_deref());
                    Value::Null
                }
                "unregister" => {
                    let c = opt_s(2).map(|x| conn(&x));
                    json!(BRIDGES.unregister_if(&s(1), c.as_ref()))
                }
                _ => {
                    let t = s(1);
                    let mut links: Vec<(String, u64)> = BRIDGES.links().into_iter().map(|(k, v)| (k, v.1)).collect();
                    links.sort();
                    json!([
                        BRIDGES.steamid(&t),
                        BRIDGES.port(&t),
                        BRIDGES.conn(&t).map(|c| cid_of(&c)),
                        BRIDGES.tokens_for_ip(&s(2)),
                        BRIDGES.name_for_ip(&s(2)),
                        links.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>()
                    ])
                }
            }
        }
        "session" => {
            let k = s(0);
            match k.as_str() {
                "register" => SESSIONS.register(&s(1), conn(&s(2))),
                "unregister" => {
                    let c = opt_s(2).map(|x| conn(&x));
                    SESSIONS.unregister_if(&s(1), c.as_ref());
                }
                _ => {}
            }
            json!([SESSIONS.conn(&s(1)).map(|c| cid_of(&c))])
        }
        "adopt" => {
            adopt_bridge_name(&s(0), &s(1));
            let (k, h) = no_helper_note(&s(0), &s(1));
            json!([k, h])
        }
        "lost" => json!(note_session_lost(&s(0), &conn(&s(1)))),
        "reattach" => {
            let c = conn(&s(0));
            let mut st = ConnSt { token: opt_s(1), name: s(2), ..ConnSt::new() };
            let r = session_reattach(&c, addr_of(3).unwrap_or_else(|| "0.0.0.0:0".parse().unwrap()), &mut st);
            match r {
                Ok(Some(o)) => json!([true, st.session, st.authed, true, o.sent_name, o.started]),
                _ => json!([false, st.session, st.authed, Value::Null, Value::Null, Value::Null]),
            }
        }
        "peer_seen" => {
            if let Some(ad) = addr_of(1) {
                peer_seen(&s(0), ad);
            }
            Value::Null
        }
        _ => Value::Null,
    }
}

fn set_mtime(p: &std::path::Path, secs: f64) {
    if let Ok(f) = std::fs::OpenOptions::new().write(true).open(p) {
        let _ = f.set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs_f64(secs));
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    std::env::set_var("T2R_NO_COUNTDOWN", "1");
    TEST_NO_SLEEP.store(true, Ordering::Relaxed);
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
                    for (k, v) in c["left"].as_object().cloned().unwrap_or_default() {
                        g.left.insert(k, (v[0].as_f64().unwrap_or(0.0), v[1].as_f64().unwrap_or(0.0)));
                    }
                    g.joining.clear();
                    g.started = strs(&c["started"]).into_iter().collect();
                    g.started_t.clear();
                    for (k, v) in c["started_t"].as_object().cloned().unwrap_or_default() {
                        g.started_t.insert(k, v.as_f64().unwrap_or(0.0));
                    }
                }
                emit(out, n, Value::Null, state());
            }
            "setmap" => {
                {
                    let mut ts = TEAM_SWAP.lock();
                    ts.clear();
                    for (k, v) in c["swap"].as_object().cloned().unwrap_or_default() {
                        ts.insert(k, v.as_object().map(|m| m.iter().map(|(a, b)| (a.clone(), b.as_i64().unwrap_or(0))).collect()).unwrap_or_default());
                    }
                }
                {
                    let mut p1 = MATCH_P1.lock();
                    p1.clear();
                    for (k, v) in c["p1"].as_object().cloned().unwrap_or_default() {
                        p1.insert(k, v.as_bool().unwrap_or(false));
                    }
                }
                *LOADED.lock() = strs(&c["loaded"]).into_iter().collect();
                *CD_GOT.lock() = strs(&c["cdgot"]).into_iter().collect();
                for kv in c["p2p"].as_array().cloned().unwrap_or_default() {
                    MATCH_P2P.lock().insert(key_of(&strs(&kv[0])), kv[1].as_bool().unwrap_or(false));
                }
                for kv in c["sidkeep"].as_array().cloned().unwrap_or_default() {
                    SID_KEEP.lock().insert(key_of(&strs(&kv[0])), kv[1].as_u64().unwrap_or(0) as u32);
                }
                for kv in c["timing"].as_array().cloned().unwrap_or_default() {
                    TIMING.lock().insert(key_of(&strs(&kv[0])), Timing { start: kv[1].as_f64().unwrap_or(0.0), ..Default::default() });
                }
                emit(out, n, Value::Null, state());
            }
            "nohost" => {
                let p = dir().join("no_host.txt");
                match c["text"].as_str() {
                    None => {
                        let _ = std::fs::remove_file(&p);
                    }
                    Some(t) => {
                        let _ = std::fs::write(&p, t.as_bytes());
                        set_mtime(&p, c["mt"].as_f64().unwrap_or(0.0));
                    }
                }
                emit(out, n, Value::Null, Value::Null);
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
