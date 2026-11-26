//! `t2r-daemon selftest k6`: the Rust side of verify/py/k6_harness.py (differential test of the K6 chunk: reaper, live-matches.json, snapshot,
//! gc, ping gate / ping_check, packet loss, kick / AFK, admin HTTP handler).
use super::*;
use crate::c_util::{emit, script};
use crate::{clock, py};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};
use std::sync::atomic::Ordering;

const SECRET: &str = "k6secret0123456789abcdef";

fn strs(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}
fn f(v: &Value) -> f64 {
    v.as_f64().unwrap_or(0.0)
}
fn key_of_v(v: &Value) -> Key {
    strs(v).into_iter().collect()
}
fn jv(v: &[String]) -> Value {
    Value::Array(v.iter().map(|s| Value::String(s.clone())).collect())
}
fn sorted(mut v: Vec<String>) -> Vec<String> {
    v.sort();
    v
}
fn obj_f64(m: &HashMap<String, f64>) -> Value {
    Value::Object(m.iter().map(|(k, v)| (k.clone(), json!(v))).collect())
}

fn reset() {
    *MM.lock.lock() = MMState::new();
    PRES.with(|p| {
        p.players.clear();
        p.udp_seen.clear();
    });
    LOADED_T.lock().clear();
    SESS_LOST.lock().clear();
    _SILENT.lock().clear();
    TEAM_SWAP.lock().clear();
    MATCH_MAP.lock().clear();
    MATCH_P2P.lock().clear();
    SID_KEEP.lock().clear();
    READOPT.lock().clear();
    RESTORED_T.lock().clear();
    PING.lock().clear();
    PING_AT.lock().clear();
    PING_LAST.lock().clear();
    PING_HIGH.lock().clear();
    PING_OUT.lock().clear();
    PING_NOTE_AT.lock().clear();
    PING_WAIT_SINCE.lock().clear();
    LOSS.lock().clear();
    LOSS_HIGH.lock().clear();
    LOSS_HOLD.lock().clear();
    LOSS_STRIKES.lock().clear();
    LOSS_FLAGS.lock().clear();
    LOSS_HOST_HIGH.lock().clear();
    CLAN_MATCH.lock().clear();
    CLAN_ENDED.lock().clear();
    CLAN_PENDING.lock().clear();
    CLAN_DUE.lock().clear();
    CLAN_KICKED.lock().clear();
    CLAN_REPORTED_END.lock().clear();
    MEASURED_PAIR.lock().clear();
    PING_CORR.lock().clear();
    PING_EXCESS.lock().clear();
    PING_LOC.lock().clear();
    LOC_FWD.lock().clear();
    LOC_HIST.lock().clear();
    LOC_SPIKES.lock().clear();
    REGION.lock().clear();
    REGION_CAND.lock().clear();
    JIP_LATE.lock().clear();
    MESH_ON.lock().clear();
    JIP_STARTED.lock().clear();
    JIP_REPUSH.lock().clear();
    JIP_LAST_WHY.lock().clear();
    JIP_WAIT_LOG.lock().clear();
    BOOST_PICK.lock().clear();
    BOOST_ACTIVE.lock().clear();
    CLAN_DONE.lock().clear();
    CLAN_EXCUSE_WHY.lock().clear();
    CLAN_LEFT_AT.lock().clear();
    CLAN_REJOINED.lock().clear();
    CLAN_LAST_DEPART.lock().clear();
    NAMELESS_BY_IP.lock().clear();
    VOTES.lock().clear();
    TIMING.lock().clear();
    PING_FORMED.lock().clear();
    LOADED.lock().clear();
    CD_GOT.lock().clear();
    AFK_KICKED.lock().clear();
    PING_KICKED.lock().clear();
    LOSS_KICKED.lock().clear();
    CLAN_QUITTERS.lock().clear();
    CLAN_CREDITED.lock().clear();
    CLAN_EXCUSED.lock().clear();
    END_BY_REPORTS.lock().clear();
    RESULT_BOOKED.lock().clear();
    ABORT_AWARDED.lock().clear();
    POOL_LOGGED.lock().clear();
    for mid in HM.mids() {
        HM.end(&mid);
    }
    let of: Vec<String> = PARTIES.export()["of"].as_array().map(|a| a.iter().filter_map(|e| e[0].as_str().map(|s| s.to_string())).collect()).unwrap_or_default();
    for t in of {
        PARTIES.leave(&t);
    }
    PARTIES.gc(&HashSet::new(), Some(1e18));
    reset_clocks();
    REC.lock().clear();
    MGS_ANSWER.store(false, Ordering::Relaxed);
    let _ = std::fs::remove_file(live_path());
}

fn world(w: &Value) {
    reset();
    {
        let mut wd = WORLD.lock();
        wd.names = w["names"].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string())).collect()).unwrap_or_default();
        wd.sids = w["sids"].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_u64().unwrap_or(0))).collect()).unwrap_or_default();
        wd.pings = w["pings"]
            .as_object()
            .map(|m| {
                m.iter()
                    .filter_map(|(k, v)| {
                        let (a, b) = k.split_once('|')?;
                        Some(((a.to_string(), b.to_string()), v.as_i64()?))
                    })
                    .collect()
            })
            .unwrap_or_default();
        wd.noh = strs(&w["noh"]).into_iter().collect();
        wd.perf = w["perf"].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_i64().unwrap_or(0))).collect()).unwrap_or_default();
        wd.lim = w["lim"].as_i64().unwrap_or(175);
        wd.online = strs(&w["online"]);
        wd.bho = Vec::new();
        wd.regions = HashMap::new();
    }
    if let Some(p) = w["pres"].as_object() {
        PRES.with(|pr| {
            for (t, v) in p {
                pr.players.insert(t.clone(), PresEntry { name: v[0].as_str().unwrap_or("").to_string(), last: f(&v[1]) });
            }
        });
    }
    let m = &w["mm"];
    {
        let mut g = MM.lock.lock();
        for q in m["queue"].as_array().cloned().unwrap_or_default() {
            g.queue.insert(q[0].as_str().unwrap_or("").to_string(), f(&q[1]));
        }
        for e in m["matches"].as_array().cloned().unwrap_or_default() {
            let mid = e[0].as_str().unwrap_or("").to_string();
            g.matches.insert(mid.clone(), strs(&e[1]));
            g.qtime.insert(mid, indexmap::IndexMap::new());
        }
        for (k, v) in m["formed"].as_object().cloned().unwrap_or_default() {
            g.formed.insert(k, f(&v));
        }
        g.launched.extend(strs(&m["launched"]));
        g.started.extend(strs(&m["started"]));
        g.ended.extend(strs(&m["ended"]));
        for (k, v) in m["ended_at"].as_object().cloned().unwrap_or_default() {
            g.ended_at.insert(k, f(&v));
        }
        for (k, v) in m["started_t"].as_object().cloned().unwrap_or_default() {
            g.started_t.insert(k, f(&v));
        }
        for (k, v) in m["departed"].as_object().cloned().unwrap_or_default() {
            g.departed.insert(k, strs(&v).into_iter().collect());
        }
        g.departed_vol.extend(strs(&m["dep_vol"]));
        for (k, v) in m["dep_t"].as_object().cloned().unwrap_or_default() {
            g.departed_t.insert(k, f(&v));
        }
        for (k, v) in m["clock"].as_object().cloned().unwrap_or_default() {
            g.clock.insert(k, (f(&v[0]), f(&v[1]), v[2].as_bool().unwrap_or(false), f(&v[3])));
        }
        for (k, v) in m["lives"].as_object().cloned().unwrap_or_default() {
            g.lives.insert(k, (v[0].as_i64().unwrap_or(0), v[1].as_i64().unwrap_or(0), f(&v[2])));
        }
        for j in m["joining"].as_array().cloned().unwrap_or_default() {
            g.joining.insert(j[0].as_str().unwrap_or("").to_string(), (j[1].as_str().unwrap_or("").to_string(), f(&j[2])));
        }
        for (k, v) in m["late_bonus"].as_object().cloned().unwrap_or_default() {
            g.late_bonus.insert(k, (v[0].as_str().unwrap_or("").to_string(), v[1].as_i64().unwrap_or(0)));
        }
        for (k, v) in m["late_mult"].as_object().cloned().unwrap_or_default() {
            g.late_mult.insert(k, (v[0].as_str().unwrap_or("").to_string(), f(&v[1])));
        }
        g.timed_out.extend(strs(&m["timed_out"]));
    }
    LOADED.lock().extend(strs(&w["loaded"]));
    for (k, v) in w["loaded_t"].as_object().cloned().unwrap_or_default() {
        LOADED_T.lock().insert(k, f(&v));
    }
    for (k, v) in w["sess_lost"].as_object().cloned().unwrap_or_default() {
        SESS_LOST.lock().insert(k, (f(&v[0]), v[1].as_str().unwrap_or("").to_string()));
    }
    for (mid, sw) in w["team_swap"].as_object().cloned().unwrap_or_default() {
        TEAM_SWAP.lock().insert(mid, sw.as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_i64().unwrap_or(0))).collect()).unwrap_or_default());
    }
    for e in w["match_map"].as_array().cloned().unwrap_or_default() {
        MATCH_MAP.lock().insert(key_of_v(&e[0]), e[1].as_i64().unwrap_or(0));
    }
    for e in w["p2p"].as_array().cloned().unwrap_or_default() {
        MATCH_P2P.lock().insert(key_of_v(&e[0]), e[1].as_bool().unwrap_or(false));
    }
    for e in w["sid_keep"].as_array().cloned().unwrap_or_default() {
        SID_KEEP.lock().insert(key_of_v(&e[0]), e[1].as_u64().unwrap_or(0) as u32);
    }
    for (k, v) in w["ping_at"].as_object().cloned().unwrap_or_default() {
        if let Some((a, b)) = k.split_once('|') {
            PING_AT.lock().insert((a.to_string(), b.to_string()), f(&v));
        }
    }
    for e in w["ping_last"].as_array().cloned().unwrap_or_default() {
        PING_LAST.lock().insert(pair_key(e[0].as_str().unwrap_or(""), e[1].as_str().unwrap_or("")), (e[2].as_i64(), f(&e[3])));
    }
    for e in w["hm"].as_array().cloned().unwrap_or_default() {
        HM.start(e[0].as_str().unwrap_or(""), &strs(&e[1]));
    }
    let c = &w["clan"];
    for (k, v) in c["match"].as_object().cloned().unwrap_or_default() {
        CLAN_MATCH.lock().insert(k, v.as_str().unwrap_or("").to_string());
    }
    for (k, v) in c["ended"].as_object().cloned().unwrap_or_default() {
        CLAN_ENDED.lock().insert(k, f(&v));
    }
    for (t, p) in c["pending"].as_object().cloned().unwrap_or_default() {
        let counts = p[3].as_object().map(|o| o.iter().map(|(k, v)| (k.parse::<i64>().unwrap_or(0), v.as_i64().unwrap_or(0))).collect()).unwrap_or_default();
        CLAN_PENDING.lock().insert(t, (p[0].as_str().unwrap_or("").to_string(), p[1].as_i64().unwrap_or(0), p[2].as_i64().unwrap_or(0), counts));
    }
    for e in c["quitters"].as_array().cloned().unwrap_or_default() {
        CLAN_QUITTERS.lock().insert((e[0].as_str().unwrap_or("").to_string(), e[1].as_str().unwrap_or("").to_string()));
    }
    for e in c["credited"].as_array().cloned().unwrap_or_default() {
        CLAN_CREDITED.lock().insert((e[0].as_str().unwrap_or("").to_string(), e[1].as_str().unwrap_or("").to_string()));
    }
    for e in c["excused"].as_array().cloned().unwrap_or_default() {
        CLAN_EXCUSED.lock().insert((e[0].as_str().unwrap_or("").to_string(), e[1].as_str().unwrap_or("").to_string()));
    }
    for g in w["parties"].as_array().cloned().unwrap_or_default() {
        let g = strs(&g);
        for m in g.iter().skip(1) {
            PARTIES.invite_token(&g[0], m);
            let _ = PARTIES.accept_token(m, &g[0]);
        }
    }
    for (k, v) in w["loss_flags"].as_object().cloned().unwrap_or_default() {
        if let Some((mid, tk)) = k.split_once('|') {
            LOSS_FLAGS.lock().insert((mid.to_string(), tk.to_string()), (v[0].as_bool().unwrap_or(false), v[1].as_bool().unwrap_or(false)));
        }
    }
    for (k, v) in w["ping_high"].as_object().cloned().unwrap_or_default() {
        if let Some((a, b)) = k.split_once('|') {
            PING_HIGH.lock().insert((a.to_string(), b.to_string()), f(&v));
        }
    }
    regs(&w["regs"]);
    for (k, v) in w["strikes"].as_object().cloned().unwrap_or_default() {
        LOSS_STRIKES.lock().insert(k, v.as_array().map(|a| a.iter().map(f).collect()).unwrap_or_default());
    }
}

fn regs(g: &Value) {
    let t = clock::now();
    for k in strs(&g["tok"]) {
        PING_LOC.lock().insert(k.clone(), "ams=20".to_string());
        LOC_FWD.lock().insert(k.clone(), t - 5.0);
        LOC_HIST.lock().insert(k.clone(), [5i64, 6].into_iter().collect());
        LOC_SPIKES.lock().insert(k.clone(), 1);
        REGION.lock().insert(k.clone(), "eu".to_string());
        REGION_CAND.lock().insert(k.clone(), ("eu".to_string(), 1));
        JIP_LATE.lock().insert(k.clone(), ("deadbeef".to_string(), t - 9.0));
        MESH_ON.lock().insert(k.clone(), t - 3.0);
        JIP_STARTED.lock().insert(k.clone(), ("deadbeef".to_string(), t - 9.0));
        JIP_REPUSH.lock().insert(k.clone(), (t - 1.0, 2));
        JIP_WAIT_LOG.lock().insert(k.clone(), t - 4.0);
        BOOST_PICK.lock().insert(k.clone(), 1);
        BOOST_ACTIVE.lock().insert(k.clone(), 2);
        CD_GOT.lock().insert(k.clone());
        _SILENT.lock().insert(k.clone(), t - 50.0);
        CLAN_DONE.lock().insert(k.clone(), "mid".to_string());
        JIP_LAST_WHY.lock().insert(k.clone(), "why".to_string());
    }
    for (i, k) in strs(&g["ip"]).into_iter().enumerate() {
        NAMELESS_BY_IP.lock().insert(format!("10.0.0.{}", i), k);
    }
    for p in g["pairs"].as_array().cloned().unwrap_or_default() {
        let (a, b) = (p[0].as_str().unwrap_or("").to_string(), p[1].as_str().unwrap_or("").to_string());
        PING.lock().insert((a.clone(), b.clone()), [30i64, 40].into_iter().collect());
        PING_AT.lock().insert((a, b), t - 7.0);
    }
    for m in strs(&g["mid"]) {
        CLAN_REPORTED_END.lock().insert(m.clone(), [("x".to_string(), t)].into_iter().collect());
        END_BY_REPORTS.lock().insert(m.clone());
        RESULT_BOOKED.lock().insert(m.clone());
        ABORT_AWARDED.lock().insert(m.clone());
        CLAN_LAST_DEPART.lock().insert(m.clone(), (if m.len() % 2 == 1 { t - 500.0 } else { t - 5000.0 }, "tok".to_string()));
        JIP_LAST_WHY.lock().insert(m, "why".to_string());
    }
    for e in g["tm"].as_array().cloned().unwrap_or_default() {
        let k = (e[0].as_str().unwrap_or("").to_string(), e[1].as_str().unwrap_or("").to_string());
        CLAN_EXCUSE_WHY.lock().insert(k.clone(), "x".to_string());
        CLAN_LEFT_AT.lock().insert(k.clone(), t - 3.0);
        CLAN_REJOINED.lock().insert(k, t - 3.0);
    }
}

fn keys_sorted<I: IntoIterator<Item = String>>(i: I) -> Value {
    let mut v: Vec<String> = i.into_iter().collect();
    v.sort();
    json!(v)
}

fn regs_dump() -> Value {
    let pk = |k: &(String, String)| format!("{}|{}", k.0, k.1);
    let pl: Vec<String> = PING_LAST.lock().keys().map(|k| format!("{}|{}", k.0, k.1)).collect();
    json!({
        "PING_LOC": keys_sorted(PING_LOC.lock().keys().cloned()), "LOC_FWD": keys_sorted(LOC_FWD.lock().keys().cloned()), "LOC_HIST": keys_sorted(LOC_HIST.lock().keys().cloned()),
        "LOC_SPIKES": keys_sorted(LOC_SPIKES.lock().keys().cloned()), "REGION": keys_sorted(REGION.lock().keys().cloned()), "REGION_CAND": keys_sorted(REGION_CAND.lock().keys().cloned()),
        "JIP_LATE": keys_sorted(JIP_LATE.lock().keys().cloned()), "MESH_ON": keys_sorted(MESH_ON.lock().keys().cloned()), "JIP_STARTED": keys_sorted(JIP_STARTED.lock().keys().cloned()),
        "JIP_REPUSH": keys_sorted(JIP_REPUSH.lock().keys().cloned()), "JIP_WAIT_LOG": keys_sorted(JIP_WAIT_LOG.lock().keys().cloned()), "BOOST_PICK": keys_sorted(BOOST_PICK.lock().keys().cloned()),
        "BOOST_ACTIVE": keys_sorted(BOOST_ACTIVE.lock().keys().cloned()), "CD_GOT": keys_sorted(CD_GOT.lock().iter().cloned()), "CLAN_DONE": keys_sorted(CLAN_DONE.lock().keys().cloned()),
        "JIP_LAST_WHY": keys_sorted(JIP_LAST_WHY.lock().keys().cloned()), "FEED_SEEN": json!([]), "NAMELESS": keys_sorted(NAMELESS_BY_IP.lock().keys().cloned()),
        "PING": keys_sorted(PING.lock().keys().map(pk)), "PING_AT": keys_sorted(PING_AT.lock().keys().map(pk)), "PING_LAST": keys_sorted(pl),
        "REPORTED_END": keys_sorted(CLAN_REPORTED_END.lock().keys().cloned()), "END_BY_REPORTS": keys_sorted(END_BY_REPORTS.lock().iter().cloned()),
        "RESULT_BOOKED": keys_sorted(RESULT_BOOKED.lock().iter().cloned()), "ABORT_AWARDED": keys_sorted(ABORT_AWARDED.lock().iter().cloned()),
        "LAST_DEPART": keys_sorted(CLAN_LAST_DEPART.lock().keys().cloned()), "EXCUSE_WHY": keys_sorted(CLAN_EXCUSE_WHY.lock().keys().map(pk)),
        "LEFT_AT": keys_sorted(CLAN_LEFT_AT.lock().keys().map(pk)), "REJOINED": keys_sorted(CLAN_REJOINED.lock().keys().map(pk)),
    })
}

fn pairs_sorted(s: Vec<(String, String)>) -> Value {
    let mut v = s;
    v.sort();
    Value::Array(v.into_iter().map(|(a, b)| json!([a, b])).collect())
}

fn sorted_key_list(m: Vec<(Key, Value)>) -> Value {
    let mut v: Vec<(Vec<String>, Value)> = m.into_iter().map(|(k, x)| (k.into_iter().collect(), x)).collect();
    v.sort_by(|a, b| py::dumps_compact(&json!([a.0, a.1])).cmp(&py::dumps_compact(&json!([b.0, b.1]))));
    Value::Array(v.into_iter().map(|(k, x)| json!([k, x])).collect())
}

fn sdump() -> Value {
    let mm = {
        let g = MM.lock.lock();
        let hs = |s: &std::collections::HashSet<String>| json!(sorted(s.iter().cloned().collect()));
        json!({
            "queue": g.queue.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
            "matches": g.matches.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
            "formed": obj_f64(&g.formed), "launched": hs(&g.launched), "started": hs(&g.started), "ended": hs(&g.ended), "ended_at": obj_f64(&g.ended_at),
            "started_t": obj_f64(&g.started_t),
            "departed": Value::Object(g.departed.iter().map(|(k, v)| (k.clone(), hs(v))).collect()),
            "dep_vol": hs(&g.departed_vol), "dep_t": obj_f64(&g.departed_t),
            "clock": Value::Object(g.clock.iter().map(|(k, v)| (k.clone(), json!([v.0, v.1, v.2, v.3]))).collect()),
            "lives": Value::Object(g.lives.iter().map(|(k, v)| (k.clone(), json!([v.0, v.1, v.2]))).collect()),
            "joining": g.joining.iter().map(|(k, v)| json!([k, v.0, v.1])).collect::<Vec<_>>(),
            "late_bonus": Value::Object(g.late_bonus.iter().map(|(k, v)| (k.clone(), json!([v.0, v.1]))).collect()),
            "late_mult": Value::Object(g.late_mult.iter().map(|(k, v)| (k.clone(), json!([v.0, v.1]))).collect()),
            "timed_out": hs(&g.timed_out),
            "qtime": Value::Object(g.qtime.iter().map(|(k, v)| (k.clone(), Value::Object(v.iter().map(|(a, b)| (a.clone(), json!(b))).collect()))).collect()),
            "seen": obj_f64(&g.seen), "settle": obj_f64(&g.settle),
        })
    };
    let mp: Vec<(Key, Value)> = MATCH_MAP.lock().iter().map(|(k, v)| (k.clone(), json!(v))).collect();
    let p2p: Vec<(Key, Value)> = MATCH_P2P.lock().iter().map(|(k, v)| (k.clone(), json!(v))).collect();
    let sk: Vec<(Key, Value)> = SID_KEEP.lock().iter().map(|(k, v)| (k.clone(), json!(v))).collect();
    let mut loss: Vec<Value> = LOSS.lock().iter().map(|(k, v)| json!([k.0, k.1, k.2, v.iter().map(|x| json!([x.0, x.1, x.2])).collect::<Vec<_>>()])).collect();
    loss.sort_by_key(|v| py::dumps_compact(v));
    let mut ph: Vec<Value> = PING_HIGH.lock().iter().map(|(k, v)| json!([k.0, k.1, v])).collect();
    ph.sort_by_key(|v| py::dumps_compact(v));
    let mut lh: Vec<Value> = LOSS_HIGH.lock().iter().map(|(k, v)| json!([k.0, k.1, v])).collect();
    lh.sort_by_key(|v| py::dumps_compact(v));
    let mut lf: Vec<Value> = LOSS_FLAGS.lock().iter().map(|(k, v)| json!([k.0, k.1, [v.0, v.1]])).collect();
    lf.sort_by_key(|v| py::dumps_compact(v));
    let mut ck: Vec<Value> = CLAN_KICKED.lock().iter().map(|(k, v)| json!([k.0, k.1, v])).collect();
    ck.sort_by_key(|v| py::dumps_compact(v));
    let mut parties: Vec<Value> = PARTIES.export()["of"].as_array().cloned().unwrap_or_default();
    parties.sort_by_key(|v| py::dumps_compact(v));
    let mut pres: Vec<String> = PRES.with(|p| p.players.keys().cloned().collect());
    pres.sort();
    json!({
        "mm": mm,
        "loaded": sorted(LOADED.lock().iter().cloned().collect()),
        "loaded_t": obj_f64(&LOADED_T.lock()),
        "sess_lost": Value::Object(SESS_LOST.lock().iter().map(|(k, v)| (k.clone(), json!([v.0, v.1]))).collect()),
        "silent": obj_f64(&_SILENT.lock()),
        "restored": obj_f64(&RESTORED_T.lock()),
        "readopt": sorted(READOPT.lock().keys().cloned().collect()),
        "team_swap": Value::Object(TEAM_SWAP.lock().iter().map(|(k, v)| (k.clone(), Value::Object(v.iter().map(|(a, b)| (a.clone(), json!(b))).collect()))).collect()),
        "match_map": sorted_key_list(mp), "p2p": sorted_key_list(p2p), "sid_keep": sorted_key_list(sk),
        "hm": sorted(HM.mids()),
        "ping_high": ph,
        "ping_kicked": pairs_sorted(PING_KICKED.lock().iter().cloned().collect()),
        "loss": loss, "loss_high": lh,
        "loss_kicked": pairs_sorted(LOSS_KICKED.lock().iter().cloned().collect()),
        "loss_strikes": Value::Object(LOSS_STRIKES.lock().iter().map(|(k, v)| (k.clone(), json!(v))).collect()),
        "loss_hold": Value::Object(LOSS_HOLD.lock().iter().map(|(k, v)| (k.clone(), json!([v.0, v.1 as i64]))).collect()),
        "loss_flags": lf,
        "loss_host_high": Value::Object(LOSS_HOST_HIGH.lock().iter().map(|(k, v)| (k.clone(), json!([v.0, v.1, v.2]))).collect()),
        "afk": pairs_sorted(AFK_KICKED.lock().iter().cloned().collect()),
        "clan_kicked": ck,
        "clan_match": Value::Object(CLAN_MATCH.lock().iter().map(|(k, v)| (k.clone(), json!(v))).collect()),
        "clan_ended": Value::Object(CLAN_ENDED.lock().iter().map(|(k, v)| (k.clone(), json!(v))).collect()),
        "clan_pending": Value::Object(CLAN_PENDING.lock().iter().map(|(k, v)| (k.clone(), json!([v.0, v.1, v.2, Value::Object(v.3.iter().map(|(a, b)| (a.to_string(), json!(b))).collect())]))).collect()),
        "clan_due": Value::Object(CLAN_DUE.lock().iter().map(|(k, v)| (k.clone(), json!(v))).collect()),
        "quitters": pairs_sorted(CLAN_QUITTERS.lock().iter().cloned().collect()),
        "credited": pairs_sorted(CLAN_CREDITED.lock().iter().cloned().collect()),
        "excused": pairs_sorted(CLAN_EXCUSED.lock().iter().cloned().collect()),
        "ping_out": Value::Object(PING_OUT.lock().iter().map(|(k, v)| (k.clone(), json!([v.0, v.1]))).collect()),
        "ping_note_at": obj_f64(&PING_NOTE_AT.lock()),
        "ping_wait": obj_f64(&PING_WAIT_SINCE.lock()),
        "regs": regs_dump(),
        "pres": pres, "parties": parties,
        "rec": std::mem::take(&mut *REC.lock()),
    })
}

fn norm_text(t: Option<String>) -> Value {
    let Some(t) = t else { return Value::Null };
    let mut o: Value = serde_json::from_str(&t).unwrap_or(Value::Null);
    if let Some(ms) = o.get_mut("matches").and_then(|m| m.as_array_mut()) {
        for m in ms {
            if let Some(Value::Object(h)) = m.get_mut("hm") {
                let mut es: Vec<(String, Value)> = std::mem::take(h).into_iter().collect();
                es.sort_by(|a, b| a.0.cmp(&b.0));
                *h = es.into_iter().collect();
            }
        }
    }
    for k in ["clan_quitters", "clan_credited", "clan_excused"] {
        if let Some(a) = o.get_mut(k).and_then(|a| a.as_array_mut()) {
            a.sort_by_key(|v| py::dumps_compact(v));
        }
    }
    json!({"head": t.chars().take(90).collect::<String>(), "text": py::dumps(&o)})
}

fn canon_live(mut x: Value) -> Value {
    for k in ["clan_quitters", "clan_credited", "clan_excused"] {
        if let Some(a) = x.get_mut(k).and_then(|a| a.as_array_mut()) {
            a.sort_by_key(|v| py::dumps_compact(v));
        }
    }
    x
}

fn unix_date(t: f64) -> String {
    http_date(t)
}

/// `emit` with the error texts of the admin handler cut off (a Python exception text is not worth matching: ADMIN-ERR lines compare as the bare tag)
fn emit6(out: &mut dyn Write, n: usize, r: Value, s: Value) {
    let logs: Vec<Value> = crate::log::take_captured().into_iter().map(|l| Value::String(if l.starts_with("ADMIN-ERR") { "ADMIN-ERR".to_string() } else { l })).collect();
    let mut m = serde_json::Map::new();
    m.insert("i".into(), Value::from(n as i64));
    m.insert("l".into(), Value::Array(logs));
    m.insert("r".into(), r);
    m.insert("s".into(), s);
    let _ = writeln!(out, "{}", py::dumps_compact(&crate::c_util::canon(Value::Object(m))));
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    K6_ON.store(true, Ordering::Relaxed);
    WORLD_ON.store(true, Ordering::Relaxed);
    REC_ON.store(true, Ordering::Relaxed);
    set_server_version("sv-test");
    set_started_at(1000.0);
    let dir = sess::db_dir();
    let _ = std::fs::create_dir_all(&dir);
    let _ = std::fs::write(dir.join("admin-secret.txt"), format!("{}\n", SECRET));
    let mut n = 0usize;
    for c in script(input) {
        n += 1;
        if let Some(t) = c.get("t").and_then(|t| t.as_f64()) {
            clock::freeze(t);
        }
        let t = clock::now();
        let op = c["op"].as_str().unwrap_or("").to_string();
        let now_opt = c.get("now").and_then(|x| x.as_f64());
        let r: Value = match op.as_str() {
            "world" => {
                world(&c["w"]);
                Value::Null
            }
            "dump" => Value::Null,
            "mgs" => {
                MGS_ANSWER.store(c["v"].as_bool().unwrap_or(false), Ordering::Relaxed);
                Value::Null
            }
            "live_export" => canon_live(serde_json::from_str(&py::dumps(&live_export(now_opt))).unwrap_or(Value::Null)),
            "live_save" => {
                let ok = live_save_ok(now_opt);
                let p = live_path();
                json!({"ok": ok, "text": norm_text(std::fs::read_to_string(&p).ok())})
            }
            "live_load" => jv(&live_load(now_opt)),
            "file_copy" => {
                let _ = std::fs::copy(c["from"].as_str().unwrap_or(""), live_path());
                Value::Null
            }
            "file_save_as" => {
                let _ = std::fs::copy(live_path(), c["to"].as_str().unwrap_or(""));
                Value::Null
            }
            "reset_runtime" => {
                let p = live_path();
                let data = std::fs::read(&p).ok();
                let w = json!({"names": WORLD.lock().names, "sids": WORLD.lock().sids, "pings": Value::Null, "online": WORLD.lock().online});
                let (names, sids, online) = {
                    let wd = WORLD.lock();
                    (wd.names.clone(), wd.sids.clone(), wd.online.clone())
                };
                let pings = WORLD.lock().pings.clone();
                let _ = w;
                reset();
                {
                    let mut wd = WORLD.lock();
                    wd.names = names;
                    wd.sids = sids;
                    wd.online = online;
                    wd.pings = pings;
                }
                if let Some(d) = data {
                    let _ = std::fs::write(&p, d);
                }
                Value::Null
            }
            "claim" => json!(readopt_claim_at(c["tok"].as_str().unwrap_or(""), now_opt)),
            "claim_all" => Value::Array(strs(&c["ts"]).iter().map(|t| json!(readopt_claim_at(t, None))).collect()),
            "roundtrip" => {
                let ok = live_save_ok(None);
                let p = live_path();
                let text = std::fs::read_to_string(&p).ok();
                let (names, sids, online, pings) = {
                    let wd = WORLD.lock();
                    (wd.names.clone(), wd.sids.clone(), wd.online.clone(), wd.pings.clone())
                };
                reset();
                {
                    let mut wd = WORLD.lock();
                    wd.names = names;
                    wd.sids = sids;
                    wd.online = online;
                    wd.pings = pings;
                }
                if let Some(d) = &text {
                    let _ = std::fs::write(&p, d);
                }
                let mids = live_load(None);
                let claims: Vec<Value> = strs(&c["ts"]).iter().map(|t| json!(readopt_claim_at(t, None))).collect();
                json!({"ok": ok, "text": norm_text(text), "mids": mids, "claims": claims})
            }
            "expire" => jv(&readopt_expire(now_opt)),
            "reap" => match reap_matches(now_opt) {
                Ok(v) => Value::Array(v.into_iter().map(|(a, b)| json!([a, b])).collect()),
                Err(e) => json!(format!("ERR {}", e)),
            },
            "net_report" => {
                net_report(now_opt, c.get("every").and_then(|x| x.as_f64()).unwrap_or(60.0));
                Value::Null
            }
            "snapshot" => serde_json::from_str(&py::dumps(&live_snapshot())).unwrap_or(Value::Null),
            "status" => {
                let live = live_snapshot();
                let roster = roster_public(&live);
                let v = json!({"ts": 0, "server_version": server_version(), "players_online": PRES.count(), "queue": MM.lock.peek(|s| s.queue.len()),
                               "matches": MM.lock.peek(|s| s.matches.len()), "live": live, "roster": roster, "notices": notices_public(), "target_version": "-"});
                serde_json::from_str(&py::dumps(&v)).unwrap_or(Value::Null)
            }
            "roster" => roster_public(&c["snap"]),
            "gc" => json!(gc_state(now_opt, c.get("ttl").and_then(|x| x.as_f64()))),
            "gc_tick" => match gc_tick(now_opt) {
                Some(m) => json!(m),
                None => Value::Null,
            },
            "ping_gate" => json!(_ping_gate(c["a"].as_str().unwrap_or(""), c["b"].as_str().unwrap_or(""), t)),
            "ping_fresh" => json!(ping_fresh(c["a"].as_str().unwrap_or(""), c["b"].as_str().unwrap_or(""), t)),
            "ping_wait" => {
                let players = strs(&c["players"]);
                let helpers = strs(&c["helpers"]);
                let units = c["units"].as_object().cloned();
                let nohost = c["nohost"].as_array().map(|_| strs(&c["nohost"]));
                let uf = |t: &str| -> Vec<String> { units.as_ref().and_then(|u| u.get(t)).map(strs).unwrap_or_default() };
                let hf = |t: &str| helpers.iter().any(|h| h == t);
                let hok = |t: &str| nohost.as_ref().map(|n| !n.iter().any(|h| h == t)).unwrap_or(true);
                let pairf = |a: &str, b: &str| pair_ping(a, b);
                let (hold, o) = ping_wait_judge_ex(
                    &players,
                    c["host"].as_str(),
                    &pairf,
                    t,
                    if units.is_some() { Some(&uf) } else { None },
                    Some(&hf),
                    c["wait_s"].as_f64(),
                    if nohost.is_some() { Some(&hok) } else { None },
                );
                json!([hold, sorted(o.into_iter().collect())])
            }
            "left_out" => {
                let sent: parking_lot::Mutex<Vec<Value>> = parking_lot::Mutex::new(Vec::new());
                let note = |t: &str, n: i64| sent.lock().push(json!([t, n]));
                let r = ping_left_out_note_with(&strs(&c["dropped"]), t, Some(&note));
                json!([sorted(r), sent.into_inner()])
            }
            "jip_block" => {
                let joining: HashSet<String> = strs(&c["joining"]).into_iter().collect();
                json!(jip_ping_block(c["tok"].as_str().unwrap_or(""), c["mid"].as_str().unwrap_or(""), c["host"].as_str().unwrap_or(""), &strs(&c["active"]), &joining, t))
            }
            "ping_check" => jv(&ping_check(now_opt)),
            "pres_touch" => {
                let ago = c["ago"].as_f64().unwrap_or(0.0);
                let names = WORLD.lock().names.clone();
                for k in strs(&c["toks"]) {
                    let nm = names.get(&k).cloned().filter(|n| !n.is_empty()).unwrap_or_else(|| "p".to_string());
                    PRES.with(|p| {
                        p.players.insert(k.clone(), PresEntry { name: nm, last: t - ago });
                    });
                }
                Value::Null
            }
            "ping_touch" => {
                for k in strs(&c["pairs"]) {
                    if let Some((a, b)) = k.split_once('|') {
                        PING_AT.lock().insert((a.to_string(), b.to_string()), t);
                    }
                }
                Value::Null
            }
            "loss_note" => {
                let pl = hex(c["pl"].as_str().unwrap_or(""));
                loss_note(c["tok"].as_str().unwrap_or(""), parse_loss(&pl).as_ref(), now_opt);
                Value::Null
            }
            "parse_loss" => match parse_loss(&hex(c["pl"].as_str().unwrap_or(""))) {
                None => Value::Null,
                Some(x) => json!({"paced": x.paced, "hbw": x.hbw, "peers": x.peers.iter().map(|p| json!([p.0, p.1, p.2, p.3])).collect::<Vec<_>>()}),
            },
            "roster_key" => json!(roster_key(c["name"].as_str().unwrap_or(""))),
            "loss_check" => jv(&loss_check(now_opt)),
            "loss_hold_note" => match loss_hold_note(c["tok"].as_str().unwrap_or(""), now_opt) {
                Some(b) => json!(b.iter().map(|x| format!("{:02x}", x)).collect::<String>()),
                None => Value::Null,
            },
            "kick" => jv(&kick_token(
                c["tok"].as_str().unwrap_or(""),
                c["why"].as_str().unwrap_or("admin kick"),
                c["ktxt"].as_str().unwrap_or("kicked"),
                c["kind"].as_str().unwrap_or("admin-kick"),
                c["reason"].as_str().unwrap_or(""),
                c["keep_out"].as_bool().unwrap_or(false),
                c["no_penalty"].as_bool().unwrap_or(false),
            )),
            "afk" => json!(afk_report_ok(c["tok"].as_str().unwrap_or(""), c["idle"].as_i64().unwrap_or(0), c["flags"].as_u64().unwrap_or(0) as u8)),
            "admin_kick" => admin_kick_reason(&c["pid"], &c.get("reason").cloned().unwrap_or(json!("")), c["move"].as_bool().unwrap_or(false)),
            "reason_clean" => json!(kick_reason_clean(c["s"].as_str().unwrap_or(""))),
            "reset_session" => admin_reset_session(&c["pid"]),
            "secret_new" => {
                let p = admin_secret_path();
                let _ = std::fs::remove_file(&p);
                if c["short"].as_bool().unwrap_or(false) {
                    let _ = std::fs::write(&p, "short\n");
                }
                let r = admin_secret().unwrap_or_default();
                let _ = std::fs::write(&p, format!("{}\n", SECRET));
                json!(r)
            }
            "http" => {
                let raw = hex(c["raw"].as_str().unwrap_or(""));
                let mut rd = std::io::BufReader::new(&raw[..]);
                let resp = admin_handle(&mut rd, SECRET, &unix_date(t));
                json!(resp.iter().map(|x| format!("{:02x}", x)).collect::<String>())
            }
            other => {
                eprintln!("unknown op {}", other);
                return 2;
            }
        };
        let s = sdump();
        emit6(out, n, r, s);
    }
    0
}

fn hex(s: &str) -> Vec<u8> {
    (0..s.len() / 2).filter_map(|i| u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).ok()).collect()
}
