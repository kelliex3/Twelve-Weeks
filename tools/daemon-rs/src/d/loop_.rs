//! K6 loop: reaper, live-matches.json (export / save / load / re-adoption), net report, live snapshot / t2r-status.json, gc, status loop.
//! daemon.py:9500-10188 (+ the constants at 9500-9510 that sit between the K5 and K6 line ranges).
use super::*;
use crate::{clock, py};
use indexmap::{IndexMap, IndexSet};
use parking_lot::Mutex;
use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::hash::Hash;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::LazyLock;

// py: daemon.py:9500-9510
pub static OFFLINE_GRACE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_OFFLINE_GRACE", 60.0));
pub static PLAYER_DEPART: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PLAYER_DEPART", 120.0));
pub static LOAD_SESSION_GRACE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LOAD_SESSION_GRACE", 15.0));
pub static LOAD_GRACE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LOAD_GRACE", 45.0));
pub static PRELAUNCH_SILENT: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PRELAUNCH_SILENT", 25.0));
pub static MATCH_MAX_AGE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_MATCH_MAX_AGE", (4 * 3600) as f64));
pub static ENDED_HOLD: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_ENDED_HOLD", 30.0));
pub static LOAD_TIMEOUT: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LOAD_TIMEOUT", 90.0));
pub static NOLOAD_MARGIN: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_NOLOAD_MARGIN", 30.0));
/// py: daemon.py:9510 _SILENT: token -> time it was first seen silent inside a match (for the EVT log)
pub static _SILENT: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

fn lock_loaded() -> parking_lot::MutexGuard<'static, HashSet<String>> {
    LOADED.lock()
}

fn conn_send(c: &Conn, data: &[u8]) {
    let _ = c.sendall(data);
}

// py: daemon.py:9512 silence
pub fn silence(token: &str, now: f64) -> f64 {
    match standin() {
        2 => return SILENCE.lock().get(token).copied().unwrap_or(0.0), // (selftest k2: `lambda t, now=None: SILENT.get(t, 0.0)`)
        5 => return 0.0,                                                // (selftest k5: `lambda t, now=None: 0.0`)
        _ => {}
    }
    if world_only() {
        return 0.0; // (selftest k4 / k1: the scripted world has no presence)
    }
    let last = PRES.with(|p| p.players.get(token).map(|e| e.last).unwrap_or(0.0));
    now - last
}

// py: daemon.py:9517 reap_matches (one reaper pass; returns the (mid, action) list the tests read)
pub fn reap_matches(now: Option<f64>) -> db::DbResult<Vec<(String, String)>> {
    let now = now.unwrap_or_else(clock::now);
    let mut done: Vec<(String, String)> = Vec::new();
    for (mid, players, formed, launched, ended, ended_at, dep) in MM.snapshot() {
        let age = now - formed;
        let active: Vec<String> = players.iter().filter(|p| !dep.contains(*p)).cloned().collect();
        let mut why: Option<String> = None;
        if ended && ended_at != 0.0 && now - ended_at > *ENDED_HOLD {
            why = Some(format!("result in {} s ago, free timer lost", py::fixed(now - ended_at, 0)));
        } else if age > *MATCH_MAX_AGE {
            why = Some(format!("older than {} h", py::fixed(*MATCH_MAX_AGE / 3600.0, 0)));
        } else if active.is_empty() {
            why = Some("nobody left".to_string());
        } else if !launched && !ended && age > 10.0 && active.iter().any(|p| silence(p, now) > *PRELAUNCH_SILENT) {
            let gone: Vec<String> = active.iter().filter(|p| silence(p, now) > *PRELAUNCH_SILENT).cloned().collect();
            let aborted = MM.abort(&mid, None);
            if let Some(aborted) = aborted {
                for p in &aborted {
                    lock_loaded().remove(p);
                    if gone.contains(p) {
                        MM.dequeue(p); // crashed: not a searcher any more
                        continue;
                    }
                    if let Some(c) = SESSIONS.conn(p) {
                        conn_send(&c, &[msg(0x11, &srv_note("dropped", p)), msg(0x03, &[])].concat());
                    }
                }
                k_forget_match_state(&aborted);
                evt!(
                    "reaped",
                    mid,
                    format!("countdown: {} silent > {} s, match called off, the others back in the queue", tags(&gone), py::fixed(*PRELAUNCH_SILENT, 0)),
                    tags(&aborted)
                );
                done.push((mid.clone(), "abort-silent".into()));
            }
            continue;
        } else if !launched && !ended && age > countdown_seconds() + 60.0 {
            if let Some(aborted) = MM.abort(&mid, None) {
                for p in &aborted {
                    lock_loaded().remove(p);
                    if let Some(c) = SESSIONS.conn(p) {
                        conn_send(&c, &msg(0x03, &[]));
                    }
                }
                k_forget_match_state(&aborted);
                evt!("reaped", mid, format!("countdown stalled for {} s: players back in the queue", py::fixed(age, 0)), tags(&aborted));
                done.push((mid.clone(), "abort".into()));
            }
            continue;
        } else if !ended && age > *OFFLINE_GRACE && active.iter().all(|p| silence(p, now) > *OFFLINE_GRACE) {
            let parts: Vec<String> = active.iter().map(|p| format!("{} {}", tag(p), py::fixed(silence(p, now), 0))).collect();
            why = Some(format!("every player offline (silent {} s)", parts.join(", ")));
        }
        if let Some(why) = why {
            MM.drop_mid(&mid);
            HM.end(&mid);
            k_loaded_clear_match(&mid, &players);
            for p in &players {
                CD_GOT.lock().remove(p);
                _SILENT.lock().remove(p);
            }
            k_forget_match_state(&players);
            evt!("reaped", mid, why, tags(&players));
            done.push((mid.clone(), "drop".into()));
            continue;
        }
        if ended {
            continue;
        }
        if launched && CLAN_REPORTED_END.lock().get(&mid).is_some_and(|d| !d.is_empty()) && k_match_end_via_reports(&mid, Some(now)) {
            // no host result, every player saw the end
            done.push((mid.clone(), "end-reports".into()));
            continue;
        }
        if launched && HM.enabled && !active.is_empty() {
            let st = HM.pending_stale(&mid, *HOSTMIG_NOCAND_S);
            if st != 0.0 {
                dlog!("HOSTMIG", mid, format!("host lost {} s ago and nobody asked to take over - ghost match, ending it", py::fixed(st, 0)));
                k_abort_award(&mid, &active, None, &format!("host lost, no successor for {} s", py::fixed(st, 0)));
                done.push((mid.clone(), "hostmig-ghost".into()));
                continue;
            }
        }
        for p in &active {
            // per-player silence inside the match
            let lost = SESS_LOST.lock().get(p).cloned();
            if lost.as_ref().is_some_and(|l| l.1 != mid || ended) {
                SESS_LOST.lock().remove(p);
            } else if lost.as_ref().is_some_and(|l| {
                launched
                    && SESSIONS.conn(p).is_none()
                    && now - l.0 > (if !lock_loaded().contains(p) && !MM.lock.peek(|s| s.started.contains(&mid)) { *LOAD_SESSION_GRACE_S } else { *SESSION_GRACE_S })
            }) {
                let lost = lost.unwrap_or_default();
                // (10-26 PT92: a player whose session socket is gone while the level is still loading has crashed / quit)
                SESS_LOST.lock().remove(p);
                evt!(
                    "timeout",
                    mid,
                    tag(p),
                    format!("session socket lost {} s ago and never came back: removed from the match, the others play on", py::fixed(now - lost.0, 0))
                );
                lock_loaded().remove(p);
                _SILENT.lock().remove(p);
                k_leave_running_match(p, "session lost");
                if !MM.lock.peek(|s| s.started.contains(&mid)) {
                    // it was the one the others waited for: start them now
                    let rest: Vec<String> = active.iter().filter(|q| *q != p && (lock_loaded().contains(*q) || *p == players[0])).cloned().collect();
                    if let Some(r0) = rest.first() {
                        k_maybe_game_start(r0)?;
                    }
                }
                done.push((mid.clone(), format!("session-lost:{}", p)));
                continue;
            }
            let sil = silence(p, now);
            if sil > *OFFLINE_GRACE {
                let first = {
                    let mut s = _SILENT.lock();
                    if !s.contains_key(p) {
                        s.insert(p.clone(), now);
                        true
                    } else {
                        false
                    }
                };
                if first {
                    evt!("silent", mid, tag(p), format!("no presence for {} s (rtt/loss trouble or gone)", py::fixed(sil, 0)));
                }
                if launched && sil > *PLAYER_DEPART {
                    evt!("timeout", mid, tag(p), format!("silent {} s: removed from the match, the others play on", py::fixed(sil, 0)));
                    lock_loaded().remove(p);
                    _SILENT.lock().remove(p);
                    k_leave_running_match(p, "timed out");
                    done.push((mid.clone(), format!("timeout:{}", p)));
                }
            } else if let Some(t) = _SILENT.lock().remove(p) {
                evt!("back", mid, tag(p), format!("present again after {} s", py::fixed(now - t, 0)));
            }
        }
        if *JIP {
            // join in progress: a loaded joiner waits for the others' roster refresh (jip_acked), a stuck one is removed
            if !k_jip_requeue_if_empty(&mid).is_empty() {
                done.push((mid.clone(), "jip-empty".into()));
                continue;
            }
            for j in MM.jip_joiners(&mid) {
                if lock_loaded().contains(&j) {
                    if !k_maybe_game_start(&j)? && MM.jip_age(&j) > countdown_seconds() + *LOAD_TIMEOUT + *JIP_ACK_MAX + 30.0 {
                        // loaded, but its GAME-START still did not go out long after the roster ack limit: send it back to the menu
                        dlog!("jip: joiner", tag(&j), format!("loaded but never started after {} s - sent back to the menu", py::fixed(MM.jip_age(&j), 0)));
                        evt!("jip-timeout", mid, tag(&j), format!("loaded but not started after {} s", py::fixed(MM.jip_age(&j), 0)));
                        lock_loaded().remove(&j);
                        let c = SESSIONS.conn(&j);
                        k_leave_running_match(&j, "jip never started");
                        done.push((mid.clone(), format!("jip-timeout:{}", j)));
                        if let Some(c) = c {
                            conn_send(&c, &msg(0x0d, &[]));
                        }
                    }
                } else if MM.jip_age(&j) > countdown_seconds() + *LOAD_TIMEOUT {
                    evt!("jip-timeout", mid, tag(&j), format!("never loaded after {} s", py::fixed(MM.jip_age(&j), 0)));
                    lock_loaded().remove(&j);
                    k_leave_running_match(&j, "jip never loaded");
                    done.push((mid.clone(), format!("jip-timeout:{}", j)));
                }
            }
        }
        let (mid2, still) = if let Some(a0) = active.first() { MM.active_of(a0) } else { (None, Vec::new()) };
        let late_start = launched && mid2.is_some() && !MM.lock.peek(|s| s.started.contains(&mid));
        let (missing, loaded_now): (Vec<String>, Vec<String>) = if late_start {
            let l = lock_loaded();
            (still.iter().filter(|p| !l.contains(*p)).cloned().collect(), still.iter().filter(|p| l.contains(*p)).cloned().collect())
        } else {
            (Vec::new(), Vec::new())
        };
        // S1: the others have been loaded for LOAD_GRACE s and the missing ones are a minority / a single player
        let soft = late_start && !missing.is_empty() && loaded_now.len() >= 2 && (missing.len() == 1 || loaded_now.len() > missing.len()) && {
            let lt = LOADED_T.lock();
            let mx = loaded_now.iter().map(|p| lt.get(p).copied().unwrap_or(0.0)).fold(f64::NEG_INFINITY, f64::max);
            now - mx > *LOAD_GRACE
        };
        if late_start
            && !still.is_empty()
            && loaded_now.is_empty()
            && age > countdown_seconds() + *LOAD_TIMEOUT + *LOAD_GRACE + *NOLOAD_MARGIN
            && !{
                let l = lock_loaded();
                players.iter().any(|p| l.contains(p))
            }
        {
            // nobody loaded at all: call it off, everyone back to the menu with a reason
            MM.drop_mid(&mid);
            HM.end(&mid);
            k_loaded_clear_match(&mid, &players);
            for p in &players {
                CD_GOT.lock().remove(p);
                _SILENT.lock().remove(p);
                if let Some(c) = SESSIONS.conn(p) {
                    conn_send(&c, &[msg(0x11, &srv_note("load_failed", p)), msg(0x0d, &[])].concat());
                }
            }
            k_forget_match_state(&players);
            dlog!("MATCH-ABORT nobody loaded", mid, format!("age {} s", py::fixed(age, 0)), tags(&players));
            evt!("reaped", mid, format!("MATCH-ABORT nobody loaded after {} s, everyone sent back to the menu", py::fixed(age, 0)), tags(&players));
            done.push((mid.clone(), "abort-noload".into()));
            continue;
        }
        if late_start && (soft || age > countdown_seconds() + *LOAD_TIMEOUT) {
            let timed_out = MM.lock.peek(|s| s.timed_out.contains(&mid));
            if !missing.is_empty() && still.len() - missing.len() == 1 && !timed_out {
                // R-75SX 10-27: the host never loaded, the one loaded player got a GAME-START alone: call it off
                MM.drop_mid(&mid);
                HM.end(&mid);
                k_loaded_clear_match(&mid, &players);
                let names: Vec<String> = missing
                    .iter()
                    .map(|p| {
                        let n = get_name(p);
                        if n.is_empty() {
                            "A player".to_string()
                        } else {
                            n
                        }
                    })
                    .collect();
                let names = names.join(", ");
                for p in &players {
                    CD_GOT.lock().remove(p);
                    _SILENT.lock().remove(p);
                    if let Some(c) = SESSIONS.conn(p) {
                        let why: Vec<u8> = if missing.contains(p) {
                            srv_note("load_long_off", p)
                        } else {
                            py::cut(&srv_text("load_named", &lang_of(p), &[("n", names.clone())]), 120).as_bytes().to_vec()
                        };
                        conn_send(&c, &[msg(0x11, &why), msg(0x0d, &[])].concat());
                    }
                }
                k_forget_match_state(&players);
                dlog!(
                    "MATCH-ABORT one player left after the load timeout",
                    mid,
                    format!("age {} s", py::fixed(age, 0)),
                    "missing",
                    tags(&missing),
                    "loaded",
                    tags(&loaded_now)
                );
                evt!("reaped", mid, format!("MATCH-ABORT {} never loaded, only one player left: called off, everyone back to the menu", tags(&missing)), tags(&players));
                done.push((mid.clone(), "abort-solo".into()));
                continue;
            }
            if !missing.is_empty() && still.len() - missing.len() >= 1 {
                let first = {
                    let mut g = MM.lock.lock();
                    let f = !g.timed_out.contains(&mid);
                    g.timed_out.insert(mid.clone());
                    f
                };
                if first {
                    evt!("load-timeout", mid, format!("not loaded after {} s:", py::fixed(age, 0)), tags(&missing));
                    for p in &missing {
                        let c = SESSIONS.conn(p);
                        k_leave_running_match(p, "never loaded");
                        if let Some(c) = c {
                            // back to the menu with a short reason; it can rejoin through JIP
                            conn_send(&c, &[msg(0x11, &srv_note("load_long", p)), msg(0x0d, &[])].concat());
                        }
                    }
                    let loaded: Vec<String> = {
                        let l = lock_loaded();
                        still.iter().filter(|p| l.contains(*p)).cloned().collect()
                    };
                    if let Some(l0) = loaded.first() {
                        k_maybe_game_start(l0)?;
                    }
                    done.push((mid.clone(), "load-timeout".into()));
                }
            } else if mid2.is_some() && missing.is_empty() {
                if let Some(s0) = still.first() {
                    k_maybe_game_start(s0)?;
                }
            }
        }
    }
    // a host-migration record whose match is gone (a leave raced the GAME-START bookkeeping, fuzz 10-19 seed 86)
    if HM.enabled {
        let live_m: HashSet<String> = MM.lock.peek(|s| s.matches.keys().cloned().collect());
        for hmid in HM.mids().into_iter().filter(|m| !live_m.contains(m)) {
            HM.end(&hmid);
            evt!("reaped", hmid, "host-migration record without a match");
        }
    }
    let online_now: HashSet<String> = PRES_TOKENS().into_iter().collect();
    let stale: Vec<String> = _SILENT.lock().keys().filter(|p| !online_now.contains(*p)).cloned().collect();
    for p in stale {
        if !MM.in_match(&p) {
            _SILENT.lock().remove(&p);
        }
    }
    Ok(done)
}

// ---------------------------------------------------------------- live matches survive a daemon restart
// py: daemon.py:9718 LIVE_ON ...
pub static LIVE_ON: LazyLock<bool> = LazyLock::new(|| crate::env_on("T2R_LIVE_STATE", true));
pub static LIVE_MAX_AGE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LIVE_MAX_AGE", 1200.0));
pub static LIVE_CLAIM_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LIVE_CLAIM_S", 90.0));
pub const LIVE_VERSION: i64 = 1;
/// py: READOPT entry `{"rec": record, "until": time}`
#[derive(Clone)]
pub struct Readopt {
    pub rec: Value,
    pub until: f64,
}
/// py: daemon.py:9723 READOPT (mid -> entry; loaded from the file, waiting for a client of the match)
pub static READOPT: LazyLock<Mutex<IndexMap<String, Readopt>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// py: daemon.py:9878 RESTORED_T: mid -> time the match was brought back after a daemon restart
pub static RESTORED_T: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static RESTORE_EXCUSE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_RESTORE_EXCUSE_S", 600.0));

/// py: `mid in RESTORED_T` (daemon.py:9895)
pub fn restored_t_has(mid: &str) -> bool {
    RESTORED_T.lock().contains_key(mid)
}

// py: daemon.py:9726 live_path
pub fn live_path() -> std::path::PathBuf {
    sess::db_dir().join("live-matches.json")
}

fn jstrs(v: &[String]) -> Value {
    Value::Array(v.iter().map(|s| Value::String(s.clone())).collect())
}

fn jf(x: f64) -> Value {
    serde_json::Number::from_f64(x).map(Value::Number).unwrap_or(Value::Null)
}

// py: daemon.py:9729 live_export (the JSON-able live-state dict: every started, not ended match + the clan bookkeeping that belongs to it)
pub fn live_export(now: Option<f64>) -> Value {
    let now = now.unwrap_or_else(clock::now);
    let mut recs: Vec<Map<String, Value>> = Vec::new();
    {
        let g = MM.lock.lock();
        for (mid, pl) in g.matches.iter() {
            if g.ended.contains(mid) || !g.started.contains(mid) {
                continue;
            }
            let dep: BTreeSet<String> = g.departed.get(mid).map(|d| d.iter().cloned().collect()).unwrap_or_default();
            let mut jn = Map::new();
            for (t, (m, t0)) in g.joining.iter() {
                if m == mid && !dep.contains(t) {
                    jn.insert(t.clone(), jf((now - t0).max(0.0)));
                }
            }
            let loaded = lock_loaded();
            let mut jl: Vec<String> = jn.keys().filter(|t| loaded.contains(*t)).cloned().collect();
            jl.sort();
            drop(loaded);
            let mut r = Map::new();
            r.insert("mid".into(), json!(mid));
            r.insert("players".into(), jstrs(pl));
            r.insert("formed".into(), jf(g.formed.get(mid).copied().unwrap_or(now)));
            r.insert("started_t".into(), jf(g.started_t.get(mid).copied().unwrap_or(now)));
            r.insert("launched".into(), json!(g.launched.contains(mid)));
            r.insert("departed".into(), Value::Array(dep.iter().map(|s| json!(s)).collect()));
            r.insert("joining".into(), Value::Object(jn));
            r.insert("joining_loaded".into(), jstrs(&jl));
            r.insert("dep_vol".into(), jstrs(&pl.iter().filter(|t| g.departed_vol.contains(*t)).cloned().collect::<Vec<_>>()));
            let mut dt = Map::new();
            for t in pl {
                if let Some(v) = g.departed_t.get(t) {
                    dt.insert(t.clone(), jf(*v));
                }
            }
            r.insert("dep_t".into(), Value::Object(dt));
            r.insert("clock".into(), g.clock.get(mid).map(|c| json!([jf(c.0), jf(c.1), c.2, jf(c.3)])).unwrap_or(Value::Null));
            r.insert("lives".into(), g.lives.get(mid).map(|l| json!([l.0, l.1, jf(l.2)])).unwrap_or(Value::Null));
            let mut lb = Map::new();
            for t in pl {
                if let Some(v) = g.late_bonus.get(t) {
                    if &v.0 == mid {
                        lb.insert(t.clone(), json!([v.0, v.1]));
                    }
                }
            }
            r.insert("late_bonus".into(), Value::Object(lb));
            let mut lm = Map::new();
            for t in pl {
                if let Some(v) = g.late_mult.get(t) {
                    if &v.0 == mid {
                        lm.insert(t.clone(), json!([v.0, jf(v.1)]));
                    }
                }
            }
            r.insert("late_mult".into(), Value::Object(lm));
            let mut lp = Map::new();
            for t in pl {
                if let Some(v) = g.late_prog.get(t) {
                    if &v.0 == mid {
                        lp.insert(t.clone(), json!([v.0, jf(v.1), v.2]));
                    }
                }
            }
            r.insert("late_prog".into(), Value::Object(lp));
            let mut pr = Map::new();
            if let Some(m) = MATCH_PRES.lock().get(mid) {
                for (t, v) in m {
                    if pl.contains(t) {
                        pr.insert(t.clone(), json!([jf(v[0]), jf(v[1])]));
                    }
                }
            }
            r.insert("pres".into(), Value::Object(pr));
            // (11-22 R-9ESR) the in-match team swaps; TEAM_SWAP's inner map is unordered here: written sorted by token
            let mut ts = Map::new();
            if let Some(m) = TEAM_SWAP.lock().get(mid) {
                let mut ks: Vec<(&String, &i64)> = m.iter().collect();
                ks.sort();
                for (k, v) in ks {
                    ts.insert(k.clone(), json!(v));
                }
            }
            r.insert("team_swap".into(), Value::Object(ts));
            r.insert("saved".into(), jf(now));
            recs.push(r);
        }
    }
    for r in recs.iter_mut() {
        let mid = r["mid"].as_str().unwrap_or("").to_string();
        let players: Vec<String> = r["players"].as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default();
        let key: Key = players.iter().cloned().collect();
        r.insert("map".into(), MATCH_MAP.lock().get(&key).map(|v| json!(v)).unwrap_or(Value::Null));
        r.insert("p2p".into(), MATCH_P2P.lock().get(&key).map(|v| json!(v)).unwrap_or(Value::Null));
        r.insert("sid".into(), SID_KEEP.lock().get(&key).map(|v| json!(v)).unwrap_or(Value::Null));
        r.insert("hm".into(), if HM.enabled { HM.export_match(&mid).unwrap_or(Value::Null) } else { Value::Null });
        r.insert("gl".into(), GL.export_held(&mid));
        r.insert("int".into(), INT.export(&mid).unwrap_or(Value::Null));
        let cm = CLAN_MATCH.lock();
        let mut o = Map::new();
        for t in &players {
            if cm.get(t) == Some(&mid) {
                o.insert(t.clone(), json!(mid));
            }
        }
        r.insert("clan_match".into(), Value::Object(o));
    }
    let mut all: Vec<Value> = recs.into_iter().map(Value::Object).collect();
    for (_, e) in READOPT.lock().iter() {
        // not claimed yet: keep it (a second restart inside the window does not lose it)
        all.push(e.rec.clone());
    }
    let mids: HashSet<String> = all.iter().filter_map(|r| r.get("mid").and_then(|m| m.as_str()).map(|s| s.to_string())).collect();
    let pend: Map<String, Value> = {
        let cp = CLAN_PENDING.lock();
        let mut ks: Vec<&String> = cp.keys().collect();
        ks.sort(); // (K2 keeps CLAN_PENDING unordered)
        ks.into_iter()
            .map(|t| {
                let p = &cp[t];
                let counts: Map<String, Value> = p.3.iter().map(|(k, v)| (k.to_string(), json!(v))).collect();
                (t.clone(), json!([p.0, p.1, p.2, Value::Object(counts)]))
            })
            .collect()
    };
    let ended: Map<String, Value> = CLAN_ENDED.lock().iter().filter(|(_, t)| now - **t < 3600.0).map(|(m, t)| (m.clone(), jf(*t))).collect();
    let pairs = |s: Vec<(String, String)>| -> Value {
        let mut v: Vec<(String, String)> = s.into_iter().filter(|(_, m)| mids.contains(m)).collect();
        v.sort();
        Value::Array(v.into_iter().map(|(t, m)| json!([t, m])).collect())
    };
    let quitters = pairs(CLAN_QUITTERS.lock().iter().cloned().collect());
    let credited = pairs(CLAN_CREDITED.lock().iter().cloned().collect());
    let excused = pairs(CLAN_EXCUSED.lock().iter().cloned().collect());
    json!({"v": LIVE_VERSION, "saved_at": jf(now), "matches": Value::Array(all), "clan_ended": Value::Object(ended), "clan_pending": Value::Object(pend),
           "clan_quitters": quitters, "clan_credited": credited, "clan_excused": excused, "parties": PARTIES.export()})
}

static LIVE_ERR: AtomicBool = AtomicBool::new(false);
static LIVE_SAVE_LOCK: Mutex<()> = Mutex::new(());

// py: daemon.py:9786 live_save (atomic write of live_export(); never raises)
pub fn live_save(now: Option<f64>) {
    live_save_ok(now);
}

/// `live_save` with its True / False result
pub fn live_save_ok(now: Option<f64>) -> bool {
    if standin() == 5 {
        stub_note("live_save", String::new()); // (selftest k5 stand-in)
        return true;
    }
    if !*LIVE_ON {
        return false;
    }
    let _g = LIVE_SAVE_LOCK.lock();
    let data = live_export(now);
    let path = live_path();
    let empty = data["matches"].as_array().is_none_or(|a| a.is_empty())
        && data["clan_pending"].as_object().is_none_or(|a| a.is_empty())
        && data["parties"]["of"].as_array().is_none_or(|a| a.is_empty());
    if empty && !path.exists() {
        return false;
    }
    let mut tmp = path.clone().into_os_string();
    tmp.push(".tmp");
    let tmp = std::path::PathBuf::from(tmp);
    let r = std::fs::write(&tmp, py::dumps(&data).as_bytes()).and_then(|_| std::fs::rename(&tmp, &path));
    match r {
        Ok(()) => true,
        Err(e) => {
            if !LIVE_ERR.swap(true, Ordering::Relaxed) {
                dlog!("LIVE-STATE not saved:", crate::pyfmt::os_error_repr(&e));
            }
            false
        }
    }
}

/// Python `float(x)` of a JSON value (None = TypeError)
pub fn pyfloat(v: &Value) -> Option<f64> {
    match v {
        Value::Number(n) => n.as_f64(),
        Value::Bool(b) => Some(if *b { 1.0 } else { 0.0 }),
        Value::String(s) => py::strip(s).parse::<f64>().ok(),
        _ => None,
    }
}

/// Python `int(x)` of a JSON value
pub fn pyint(v: &Value) -> Option<i64> {
    match v {
        Value::Number(n) => n.as_i64().or_else(|| n.as_f64().map(|f| f.trunc() as i64)),
        Value::Bool(b) => Some(*b as i64),
        Value::String(s) => py::strip(s).parse::<i64>().ok(),
        _ => None,
    }
}

/// Python truthiness of a JSON value
pub fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().is_some_and(|f| f != 0.0),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

fn vstrs(v: Option<&Value>) -> Vec<String> {
    v.and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}

// py: daemon.py:9812 live_load
pub fn live_load(now: Option<f64>) -> Vec<String> {
    if !*LIVE_ON {
        return Vec::new();
    }
    let now = now.unwrap_or_else(clock::now);
    let data: Value = match std::fs::read(live_path()).ok().and_then(|b| serde_json::from_slice(&b).ok()) {
        Some(d) => d,
        None => return Vec::new(),
    };
    if data.get("v").and_then(|v| v.as_i64()) != Some(LIVE_VERSION) {
        return Vec::new();
    }
    let saved_at = data.get("saved_at").map(|v| if v.is_null() { None } else { pyfloat(v) }).unwrap_or(Some(0.0));
    match saved_at {
        Some(s) if !(now - s > *LIVE_MAX_AGE) => {}
        _ => return Vec::new(),
    }
    let mut mids: Vec<String> = Vec::new();
    for r in data.get("matches").and_then(|m| m.as_array()).cloned().unwrap_or_default() {
        let res = (|| -> Result<(), String> {
            let saved = match r.get("saved") {
                None => 0.0,
                Some(v) => pyfloat(v).ok_or("TypeError('float() argument must be a string or a real number')")?,
            };
            if now - saved > *LIVE_MAX_AGE || !r.get("players").is_some_and(truthy) || !r.get("mid").is_some_and(truthy) {
                return Ok(());
            }
            let mid = r["mid"].as_str().ok_or("TypeError('unhashable')")?.to_string();
            READOPT.lock().insert(mid.clone(), Readopt { rec: r.clone(), until: now + *LIVE_CLAIM_S });
            mids.push(mid);
            if let Some(cm) = r.get("clan_match").and_then(|c| c.as_object()) {
                for (t, m) in cm {
                    if let Some(m) = m.as_str() {
                        CLAN_MATCH.lock().insert(t.clone(), m.to_string());
                    }
                }
            }
            Ok(())
        })();
        if let Err(e) = res {
            dlog!("LIVE-STATE record skipped:", e);
        }
    }
    if let Some(ce) = data.get("clan_ended").and_then(|c| c.as_object()) {
        for (m, t) in ce {
            if let Some(t) = pyfloat(t) {
                CLAN_ENDED.lock().entry(m.clone()).or_insert(t);
            }
        }
    }
    if let Some(cp) = data.get("clan_pending").and_then(|c| c.as_object()) {
        for (t, p) in cp {
            let res = (|| -> Result<(), String> {
                let a = p.as_array().ok_or("TypeError")?;
                if a.len() < 4 {
                    return Err("IndexError('list index out of range')".into());
                }
                let mid = a[0].as_str().ok_or("TypeError")?.to_string();
                let parts = pyint(&a[1]).ok_or("TypeError")?;
                let flags = pyint(&a[2]).ok_or("TypeError")?;
                let mut counts: IndexMap<i64, i64> = IndexMap::new();
                if let Some(o) = a[3].as_object() {
                    for (k, v) in o {
                        counts.insert(k.parse::<i64>().map_err(|_| "ValueError(counts key)".to_string())?, pyint(v).ok_or("TypeError")?);
                    }
                }
                CLAN_PENDING.lock().entry(t.clone()).or_insert((mid, parts, flags, counts));
                CLAN_DUE.lock().entry(t.clone()).or_insert(now + *CLAN_GRACE);
                Ok(())
            })();
            if let Err(e) = res {
                dlog!("LIVE-STATE clan report skipped:", e);
            }
        }
    }
    let pairs = |k: &str| -> Vec<(String, String)> {
        data.get(k)
            .and_then(|x| x.as_array())
            .map(|a| a.iter().filter_map(|e| Some((e.get(0)?.as_str()?.to_string(), e.get(1)?.as_str()?.to_string()))).collect())
            .unwrap_or_default()
    };
    for (t, m) in pairs("clan_quitters") {
        CLAN_QUITTERS.lock().insert((t, m));
    }
    for (t, m) in pairs("clan_credited") {
        CLAN_CREDITED.lock().insert((t, m));
    }
    for (t, m) in pairs("clan_excused") {
        CLAN_EXCUSED.lock().insert((t, m));
    }
    {
        let n = PARTIES.restore(data.get("parties").unwrap_or(&Value::Null), Some(now));
        if n != 0 {
            let pids: HashSet<Option<String>> = PARTIES.grace_tokens().iter().map(|t| PARTIES.pid(t)).collect();
            dlog!(
                "LIVE-STATE: parties restored -",
                n,
                "members in",
                pids.len(),
                format!("party(ies), kept {} s while they reconnect", py::fixed(crate::social::RESTORE_GRACE, 0))
            );
        }
    }
    if !mids.is_empty() {
        evt!("readopt-load", "matches", py::list_str(&mids), format!("waiting {} s for a player of each to report", py::fixed(*LIVE_CLAIM_S, 0)));
        dlog!(format!("LIVE-STATE: {} running match(es) from before the restart wait to be re-adopted:", mids.len()), py::list_str(&mids));
    }
    mids
}

// py: daemon.py:9856 readopt_claim (the stub signature of the other chunks: the mid is not needed there)
pub fn readopt_claim(token: &str) {
    readopt_claim_at(token, None);
}

/// py: daemon.py:9856 readopt_claim(token, now=None): the mid of the match brought back, or None
pub fn readopt_claim_at(token: &str, now: Option<f64>) -> Option<String> {
    match standin() {
        2 => return None, // (selftest k2: `lambda token, now=None: None`)
        5 => {
            stub_note("readopt_claim", token.to_string()); // (selftest k5 stand-in)
            return None;
        }
        _ => {}
    }
    if token.is_empty() {
        return None;
    }
    let (mid, e) = {
        let mut g = READOPT.lock();
        if g.is_empty() {
            return None;
        }
        let mid = g
            .iter()
            .find(|(_, e)| vstrs(e.rec.get("players")).iter().any(|p| p == token) && !vstrs(e.rec.get("departed")).iter().any(|p| p == token))
            .map(|(m, _)| m.clone())?;
        let e = g.shift_remove(&mid)?;
        (mid, e)
    };
    match readopt_apply(&mid, &e.rec, token, now.unwrap_or_else(clock::now)) {
        Ok(()) => Some(mid),
        Err(ex) => {
            dlog!("LIVE-STATE re-adopt of", mid, "failed:", ex);
            None
        }
    }
}

fn vf(r: &Value, k: &str, d: f64) -> Result<f64, String> {
    match r.get(k) {
        None => Ok(d),
        Some(v) => pyfloat(v).ok_or_else(|| format!("TypeError('float() argument must be a string or a real number, not {}')", py::pyrepr(v))),
    }
}

// py: daemon.py:9871 _readopt_apply
pub fn readopt_apply(mid: &str, r: &Value, claimant: &str, now: f64) -> Result<(), String> {
    readopt_apply_impl(mid, r, claimant, now)
}

fn readopt_apply_impl(mid: &str, r: &Value, claimant: &str, now: f64) -> Result<(), String> {
    let pl: Vec<String> = vstrs(r.get("players"));
    let busy: BTreeSet<String> = {
        let g = MM.lock.lock();
        if g.matches.contains_key(mid) {
            return Ok(());
        }
        pl.iter()
            .filter(|p| {
                g.queue.contains_key(*p)
                    || g.matches.iter().any(|(m, o)| o.contains(*p) && !g.departed.get(m).is_some_and(|d| d.contains(*p)) && !g.ended.contains(m))
            })
            .cloned()
            .collect()
    };
    let mut dep: BTreeSet<String> = vstrs(r.get("departed")).into_iter().collect();
    dep.extend(busy.iter().cloned());
    let mut joining: IndexMap<String, f64> = IndexMap::new();
    if let Some(o) = r.get("joining").and_then(|j| j.as_object()) {
        for (t, a) in o {
            if pl.contains(t) && !dep.contains(t) {
                joining.insert(t.clone(), pyfloat(a).ok_or("TypeError('float()')")?);
            }
        }
    }
    let joining_loaded: BTreeSet<String> = vstrs(r.get("joining_loaded")).into_iter().filter(|t| joining.contains_key(t)).collect();
    {
        let mut g = MM.lock.lock();
        g.matches.insert(mid.to_string(), pl.clone());
        g.qtime.insert(mid.to_string(), IndexMap::new());
        g.formed.insert(mid.to_string(), vf(r, "formed", now)?);
        if r.get("launched").map(truthy).unwrap_or(true) {
            g.launched.insert(mid.to_string());
        }
        g.started.insert(mid.to_string());
        g.started_t.insert(mid.to_string(), vf(r, "started_t", now)?);
        if !dep.is_empty() {
            g.departed.insert(mid.to_string(), dep.iter().cloned().collect());
        }
        for t in vstrs(r.get("dep_vol")) {
            if dep.contains(&t) {
                g.departed_vol.insert(t);
            }
        }
        if let Some(o) = r.get("dep_t").and_then(|d| d.as_object()) {
            for (t, v) in o {
                g.departed_t.insert(t.clone(), pyfloat(v).ok_or("TypeError('float()')")?);
            }
        }
        if let Some(c) = r.get("clock").filter(|c| truthy(c)).and_then(|c| c.as_array()) {
            if c.len() >= 4 {
                g.clock.insert(
                    mid.to_string(),
                    (pyfloat(&c[0]).unwrap_or(0.0), pyfloat(&c[1]).unwrap_or(0.0), truthy(&c[2]), pyfloat(&c[3]).unwrap_or(0.0)),
                );
            }
        }
        if let Some(l) = r.get("lives").filter(|c| truthy(c)).and_then(|c| c.as_array()) {
            if l.len() >= 3 {
                g.lives.insert(mid.to_string(), (pyint(&l[0]).unwrap_or(0), pyint(&l[1]).unwrap_or(0), pyfloat(&l[2]).unwrap_or(0.0)));
            }
        }
        for (t, a) in &joining {
            g.joining.insert(t.clone(), (mid.to_string(), now - a)); // a join in progress that the restart interrupted
        }
        if let Some(o) = r.get("late_bonus").and_then(|d| d.as_object()) {
            for (t, v) in o {
                if let Some(a) = v.as_array() {
                    if a.len() >= 2 {
                        g.late_bonus.insert(t.clone(), (a[0].as_str().unwrap_or("").to_string(), pyint(&a[1]).unwrap_or(0)));
                    }
                }
            }
        }
        if let Some(o) = r.get("late_mult").and_then(|d| d.as_object()) {
            for (t, v) in o {
                if let Some(a) = v.as_array() {
                    if a.len() >= 2 {
                        g.late_mult.insert(t.clone(), (a[0].as_str().unwrap_or("").to_string(), pyfloat(&a[1]).unwrap_or(0.0)));
                    }
                }
            }
        }
        if let Some(o) = r.get("late_prog").and_then(|d| d.as_object()) {
            for (t, v) in o {
                if let Some(a) = v.as_array() {
                    if a.len() >= 3 {
                        let p = pyfloat(&a[1]).ok_or("TypeError('float()')")?;
                        let b = pyint(&a[2]).ok_or("TypeError('int()')")?;
                        g.late_prog.insert(t.clone(), (a[0].as_str().unwrap_or("").to_string(), p, b));
                    }
                }
            }
        }
        if let Some(o) = r.get("pres").and_then(|d| d.as_object()).filter(|o| !o.is_empty()) {
            let mut m: IndexMap<String, [f64; 2]> = IndexMap::new();
            for (t, v) in o {
                let a = v.as_array().ok_or("TypeError('list')")?;
                let x = a.first().and_then(pyfloat).ok_or("TypeError('float()')")?;
                let y = a.get(1).and_then(pyfloat).ok_or("TypeError('float()')")?;
                m.insert(t.clone(), [x, y]);
            }
            MATCH_PRES.lock().insert(mid.to_string(), m);
        }
        let mut sw: HashMap<String, i64> = HashMap::new();
        if let Some(o) = r.get("team_swap").and_then(|d| d.as_object()) {
            for (t, v) in o {
                let iv = pyint(v).ok_or("TypeError('int()')")?;
                if pl.contains(t) && (iv == 0 || iv == 1) {
                    sw.insert(t.clone(), iv);
                }
            }
        }
        if !sw.is_empty() {
            TEAM_SWAP.lock().insert(mid.to_string(), sw); // the in-match team swaps survive the restart
        }
    }
    let key: Key = pl.iter().cloned().collect();
    if let Some(m) = r.get("map").and_then(|m| pyint(m)).filter(|_| !r["map"].is_null()) {
        MATCH_MAP.lock().insert(key.clone(), m);
    }
    if let Some(p) = r.get("p2p").filter(|p| !p.is_null()) {
        MATCH_P2P.lock().insert(key.clone(), truthy(p));
    }
    if let Some(s) = r.get("sid").filter(|s| truthy(s)).and_then(|s| s.as_u64()) {
        SID_KEEP.lock().insert(key.clone(), s as u32);
    }
    if HM.enabled && r.get("hm").is_some_and(truthy) {
        HM.import_match(mid, &r["hm"]);
        for p in &busy {
            HM.depart(p, "left (back in the queue after the restart)");
        }
    }
    if r.get("gl").is_some_and(truthy) {
        GL.import_held(mid, &r["gl"]);
    }
    if r.get("int").is_some_and(truthy) {
        INT.import(mid, &r["int"]);
    }
    for t in &pl {
        if !dep.contains(t) {
            let n = get_name(t);
            PRES.touch(t, if n.is_empty() { "PLAYER" } else { n.as_str() }); // every player gets the reaper's silence grace from now
            if !joining.contains_key(t) || joining_loaded.contains(t) {
                lock_loaded().insert(t.clone());
                LOADED_T.lock().insert(t.clone(), now);
            }
            if SESSIONS.conn(t).is_none() {
                // 10-26 (PT92 live): the clients' session sockets were closed by the restart: mark every restored player as lost
                SESS_LOST.lock().insert(t.clone(), (now, mid.to_string()));
            }
        }
    }
    RESTORED_T.lock().insert(mid.to_string(), now);
    let hm = r.get("hm").filter(|h| truthy(h));
    let hmv = |k: &str| hm.and_then(|h| h.get(k)).map(py::pyrepr).unwrap_or_else(|| "0".to_string());
    let alive: Vec<String> = pl.iter().filter(|p| !dep.contains(*p)).cloned().collect();
    evt!("readopt", mid, "claimed by", tag(claimant), "after the daemon restart | players", tags(&alive), "| host slot", hmv("host"), "epoch", hmv("epoch"));
    dlog!("LIVE-STATE: match", mid, "re-adopted (claimed by", format!("{})", tag(claimant)), "players", tags(&pl));
    if !joining.is_empty() {
        let jt: Vec<String> = joining.keys().cloned().collect();
        let jl: Vec<String> = joining_loaded.iter().cloned().collect();
        dlog!("LIVE-STATE: match", mid, "join in progress resumed for", tags(&jt), format!("(loaded: {})", tags(&jl)));
        for t in &jl {
            if let Err(ex) = k_maybe_game_start(t) {
                dlog!("LIVE-STATE joiner start failed", tag(t), ex); // GAME-START once the others' rosters list it (jip_acked) - else the reaper retries
            }
        }
    }
    Ok(())
}

// py: daemon.py:9898 readopt_expire (drop the records nobody claimed in time)
pub fn readopt_expire(now: Option<f64>) -> Vec<String> {
    if READOPT.lock().is_empty() {
        return Vec::new();
    }
    let now = now.unwrap_or_else(clock::now);
    let out: Vec<String> = {
        let mut g = READOPT.lock();
        let dead: Vec<String> = g.iter().filter(|(_, e)| now >= e.until).map(|(m, _)| m.clone()).collect();
        for m in &dead {
            g.shift_remove(m);
        }
        dead
    };
    for mid in &out {
        evt!("readopt-drop", mid, format!("no player of the match reported within {} s", py::fixed(*LIVE_CLAIM_S, 0)));
        dlog!("LIVE-STATE: match", mid, "dropped - nobody came back");
    }
    out
}

// py: daemon.py:9912 supervised (run fn for ever: a panic is logged and the loop restarts)
pub fn supervised(f: fn(), name: &'static str) -> impl FnOnce() + Send + 'static {
    move || {
        crate::heap::set_kind_name(name);
        let mut n = 0u32;
        loop {
            match std::panic::catch_unwind(f) {
                Ok(()) => return,
                Err(_) => {
                    n += 1;
                    dlog!("THREAD-DIED", name, "panic", format!("restart #{}", n));
                    std::thread::sleep(std::time::Duration::from_secs_f64((0.2 * n as f64).min(5.0)));
                }
            }
        }
    }
}

static LAST_NET: Mutex<f64> = Mutex::new(0.0);

/// (selftest) `_last_net[0] = 0`, `_gc_last[0] = 0`
pub fn reset_clocks() {
    *LAST_NET.lock() = 0.0;
    *GC_LAST.lock() = 0.0;
}

fn dict_repr(items: &[(String, String)]) -> String {
    // Python dict repr; a repeated key keeps its first position and takes the last value
    let mut v: Vec<(String, String)> = Vec::new();
    for (k, x) in items {
        if let Some(e) = v.iter_mut().find(|e| &e.0 == k) {
            e.1 = x.clone();
        } else {
            v.push((k.clone(), x.clone()));
        }
    }
    format!("{{{}}}", v.iter().map(|(k, x)| format!("{}: {}", crate::pyfmt::str_repr(k), x)).collect::<Vec<_>>().join(", "))
}

// py: daemon.py:9928 net_report (every `every` s per running match: one EVT line with the pings and silences)
pub fn net_report(now: Option<f64>, every: f64) {
    let now = now.unwrap_or_else(clock::now);
    {
        let mut l = LAST_NET.lock();
        if now - *l < every {
            return;
        }
        *l = now;
    }
    for (mid, players, formed, launched, ended, _, dep) in MM.snapshot() {
        if !launched || ended {
            continue;
        }
        let act: Vec<String> = players.iter().filter(|p| !dep.contains(*p)).cloned().collect();
        let mut pings: Vec<(String, String)> = Vec::new();
        for (i, a) in act.iter().enumerate() {
            for b in &act[i + 1..] {
                if let Some(p) = pair_ping(a, b) {
                    pings.push((format!("{}<>{}", tag(a), tag(b)), p.to_string()));
                }
            }
        }
        let silent: Vec<(String, String)> = act.iter().filter(|p| silence(p, now) > 10.0).map(|p| (tag(p), (silence(p, now) as i64).to_string())).collect();
        evt!(
            "net",
            mid,
            format!("age {} s", py::fixed(now - formed, 0)),
            "players",
            tags(&act),
            "ping-ms",
            if pings.is_empty() { "unknown".to_string() } else { dict_repr(&pings) },
            "silent-s",
            if silent.is_empty() { "none".to_string() } else { dict_repr(&silent) }
        );
    }
}

// ---------------------------------------------------------------- status
// py: daemon.py:9996 STATUS_FILE / STARTED_AT
pub static STATUS_FILE: LazyLock<String> = LazyLock::new(|| match std::env::var("T2R_STATUS_FILE") {
    Ok(v) if v == "0" || v == "off" => String::new(),
    Ok(v) if !v.is_empty() => v,
    _ => sess::db_dir().join("t2r-status.json").to_string_lossy().into_owned(),
});
static STARTED: Mutex<Option<f64>> = Mutex::new(None);
/// py: daemon.py:9997 STARTED_AT (the process start; first use = start)
pub fn started_at() -> f64 {
    *STARTED.lock().get_or_insert_with(clock::now)
}
pub fn set_started_at(t: f64) {
    *STARTED.lock() = Some(t);
}
static STATUS_ERR: AtomicBool = AtomicBool::new(false);

fn party_size(t: &str) -> i64 {
    if PARTIES.pid(t).is_some() {
        PARTIES.members(t).len() as i64
    } else {
        0
    }
}

pub const ROSTER_MAX: usize = 200;

fn pyname(v: Option<&Value>) -> String {
    match v {
        Some(Value::String(s)) if !s.is_empty() => s.clone(),
        _ => "PLAYER".to_string(),
    }
}

// py: daemon.py:10006 roster_public (launcher 'who is online' panel: name + state only)
pub fn roster_public(snap: &Value) -> Value {
    let mut out: Vec<Value> = Vec::new();
    let mut maps: HashMap<String, Value> = HashMap::new();
    if let Some(ms) = snap.get("matches").and_then(|m| m.as_array()) {
        for m in ms {
            if let Some(id) = m.get("id").and_then(|i| i.as_str()) {
                maps.insert(id.to_string(), m.get("map").and_then(|x| x.get("name")).cloned().unwrap_or(Value::Null));
            }
        }
    }
    if let Some(ps) = snap.get("players").and_then(|m| m.as_array()) {
        for p in ps {
            let party = p.get("party").and_then(|x| pyint(x)).unwrap_or(0) > 1;
            let st = p.get("state").and_then(|s| s.as_str()).unwrap_or("");
            let state = if st == "match" {
                "match"
            } else if st == "queue" {
                "searching"
            } else if party {
                "party"
            } else {
                "menu"
            };
            let name: String = pyname(p.get("name")).chars().take(24).collect();
            let map = if state == "match" {
                p.get("match").and_then(|m| m.as_str()).and_then(|m| maps.get(m)).filter(|v| truthy(v)).cloned().unwrap_or(Value::Null)
            } else {
                Value::Null
            };
            out.push(json!({"name": name, "state": state, "map": map, "party": party}));
        }
    }
    out.truncate(ROSTER_MAX);
    Value::Array(out)
}

fn t2r_mode() -> String {
    std::env::var("T2R_MODE").unwrap_or_default()
}

// py: daemon.py:10026 _snap_match_mode
fn snap_match_mode(mid: &str) -> String {
    if let Some(m) = MODEQ.match_mode(mid).filter(|m| !m.is_empty()) {
        return m;
    }
    let e = py::strip(&t2r_mode()).to_lowercase();
    if !e.is_empty() {
        return e;
    }
    crate::modeq::modes().first().cloned().unwrap_or_default()
}

// py: daemon.py:10036 _snap_search_mode
fn snap_search_mode(tok: &str) -> String {
    let m = _search_mode(tok);
    let modes = crate::modeq::modes();
    if m == crate::modeq::ANY {
        "any".to_string()
    } else if m >= 0 && (m as usize) < modes.len() {
        modes[m as usize].clone()
    } else {
        modes.first().cloned().unwrap_or_default()
    }
}

fn sha_pid(t: &str) -> String {
    use sha2::{Digest, Sha256};
    let h = Sha256::digest(t.as_bytes());
    let mut s = String::with_capacity(64);
    for b in h.iter() {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

// py: daemon.py:10042 live_snapshot (who is online / queued / in which match, for the admin site; never raises: {} on a problem)
pub fn live_snapshot() -> Value {
    let now = clock::now();
    let pid = |t: &str| -> String { sha_pid(t)[..12].to_string() };
    let pres: Vec<(String, f64)> = PRES.with(|p| p.players.iter().filter(|(_, e)| now - e.last < *KEEPALIVE_TTL).map(|(t, e)| (t.clone(), e.last)).collect());
    let (queue, matches, formed, launched, ended, started, departed, clock_m, lives) = {
        let g = MM.lock.lock();
        (
            g.queue.clone(),
            g.matches.clone(),
            g.formed.clone(),
            g.launched.clone(),
            g.ended.clone(),
            g.started.clone(),
            g.departed.clone(),
            g.clock.clone(),
            g.lives.clone(),
        )
    };
    let _ = &launched;
    let mut where_: HashMap<String, String> = HashMap::new();
    for (mid, pl) in &matches {
        for t in pl {
            if !departed.get(mid).is_some_and(|d| d.contains(t)) && !ended.contains(mid) {
                where_.insert(t.clone(), mid.clone());
            }
        }
    }
    let nm = |t: &str| -> String {
        let n = get_name(t);
        if !n.is_empty() {
            return n;
        }
        token_name(t).filter(|n| !n.is_empty()).unwrap_or_else(|| "PLAYER".to_string())
    };
    let maps = maps_table();
    let mut out_m: Vec<Value> = Vec::new();
    for (mid, pl) in &matches {
        let key: Key = pl.iter().cloned().collect();
        let mi = MATCH_MAP.lock().get(&key).copied();
        let lvl = mi.and_then(|i| if i >= 0 { maps.get(i as usize) } else { None });
        let mut hostt: Option<String> = pl.first().cloned();
        if HM.enabled {
            if let Some(hi) = HM.host_info(mid) {
                if hi.host < pl.len() {
                    hostt = Some(pl[hi.host].clone());
                }
            }
        }
        let ck = clock_m.get(mid);
        let lv = lives.get(mid);
        let mut plist: Vec<Value> = Vec::new();
        for (i, t) in pl.iter().enumerate() {
            let ping = match &hostt {
                Some(h) if h != t => pair_ping(h, t),
                _ => None,
            };
            plist.push(json!({"name": nm(t), "pid": pid(t), "team": team_in(mid, i as i64, Some(t)), "left": departed.get(mid).is_some_and(|d| d.contains(t)),
                              "host": hostt.as_deref() == Some(t.as_str()), "ping_ms": ping}));
        }
        let state = if ended.contains(mid) {
            "ended"
        } else if started.contains(mid) {
            "running"
        } else {
            "loading"
        };
        out_m.push(json!({
            "id": mid, "state": state, "age_s": py::round0(now - formed.get(mid).copied().unwrap_or(now)),
            "map": lvl.map(|l| json!({"level": l.2, "name": l.0})).unwrap_or(Value::Null),
            "mode": t2r_mode(), "game_mode": snap_match_mode(mid),
            "net": if MATCH_P2P.lock().get(&key).copied().unwrap_or(false) { "steam-p2p" } else { "server-relay" },
            "players": plist,
            "clock": ck.map(|c| json!({"left": jf(c.0), "duration": jf(c.1), "ended": c.2, "age_s": py::round0(now - c.3)})).unwrap_or(Value::Null),
            "lives": lv.map(|l| json!({"team0": l.0, "team1": l.1, "age_s": py::round0(now - l.2)})).unwrap_or(Value::Null)
        }));
    }
    let mut out_p: Vec<Value> = Vec::new();
    for (t, last) in &pres {
        let mid = where_.get(t);
        let queued = queue.contains_key(t);
        let ver = CLIENT_VERSIONS.lock().get(t).cloned().unwrap_or_default();
        let linux = LINUX_CLIENTS.lock().contains(t);
        out_p.push(json!({
            "name": nm(t), "pid": pid(t), "steamid": BRIDGES.steamid(t),
            "state": if mid.is_some() { "match" } else if queued { "queue" } else { "menu" },
            "match": mid, "idle_s": jf(py::round_n(now - last, 1)),
            "queued_s": if queued { json!(py::round0(now - queue[t])) } else { Value::Null }, "queued": queued,
            "version": ver, "platform": if linux { "linux" } else { "windows" },
            "steam_helper": BRIDGES.conn(t).is_some(), "party": party_size(t),
            "search_mode": if queued { json!(snap_search_mode(t)) } else { Value::Null }
        }));
    }
    let q: Vec<Value> = queue.iter().map(|(t, q)| json!({"name": nm(t), "waited_s": py::round0(now - q)})).collect();
    json!({"started": jf(started_at()), "mode": t2r_mode(), "match_size": *MATCH_SIZE, "min_players": *MIN_PLAYERS, "players": out_p, "queue": q, "matches": out_m})
}

// ---------------------------------------------------------------- gc
pub static STATE_TTL_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_STATE_TTL_S", 3600.0));
pub static STATE_GC_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_STATE_GC_S", 300.0));
static GC_LAST: Mutex<f64> = Mutex::new(0.0);

/// the registries `_prune_keys` walks: dict / set of keys
pub trait Reg {
    type K: Clone;
    fn keys_vec(&self) -> Vec<Self::K>;
    fn del(&mut self, k: &Self::K);
}
impl<K: Clone + Hash + Eq, V> Reg for HashMap<K, V> {
    type K = K;
    fn keys_vec(&self) -> Vec<K> {
        self.keys().cloned().collect()
    }
    fn del(&mut self, k: &K) {
        self.remove(k);
    }
}
impl<K: Clone + Hash + Eq, V> Reg for IndexMap<K, V> {
    type K = K;
    fn keys_vec(&self) -> Vec<K> {
        self.keys().cloned().collect()
    }
    fn del(&mut self, k: &K) {
        self.shift_remove(k);
    }
}
impl<K: Clone + Hash + Eq> Reg for HashSet<K> {
    type K = K;
    fn keys_vec(&self) -> Vec<K> {
        self.iter().cloned().collect()
    }
    fn del(&mut self, k: &K) {
        self.remove(k);
    }
}
impl<K: Clone + Hash + Eq> Reg for IndexSet<K> {
    type K = K;
    fn keys_vec(&self) -> Vec<K> {
        self.iter().cloned().collect()
    }
    fn del(&mut self, k: &K) {
        self.shift_remove(k);
    }
}
impl<K: Clone + Ord> Reg for BTreeSet<K> {
    type K = K;
    fn keys_vec(&self) -> Vec<K> {
        self.iter().cloned().collect()
    }
    fn del(&mut self, k: &K) {
        self.remove(k);
    }
}

// py: daemon.py:10152 _prune_keys (a snapshot of the keys is judged without the registry's lock; the dead ones are deleted afterwards)
pub fn prune_keys<R: Reg>(d: &Mutex<R>, dead: impl Fn(&R::K) -> bool) -> usize {
    let keys = d.lock().keys_vec();
    let dead_keys: Vec<R::K> = keys.into_iter().filter(|k| dead(k)).collect();
    let mut g = d.lock();
    for k in &dead_keys {
        g.del(k);
    }
    dead_keys.len()
}

// py: daemon.py:10161 gc_state
pub fn gc_state(now: Option<f64>, ttl: Option<f64>) -> BTreeMap<String, i64> {
    let now = now.unwrap_or_else(clock::now);
    let ttl = ttl.unwrap_or(*STATE_TTL_S);
    let mut out: BTreeMap<String, i64> = BTreeMap::new();
    let (live_mids, mut busy): (HashSet<String>, HashSet<String>) = {
        let g = MM.lock.lock();
        let live: HashSet<String> = g.matches.keys().cloned().collect();
        let mut busy: HashSet<String> = g.queue.keys().cloned().collect();
        for pl in g.matches.values() {
            busy.extend(pl.iter().cloned());
        }
        (live, busy)
    };
    busy.extend(BRIDGES.with(|b| b.by_token.keys().cloned().collect::<Vec<_>>()));
    busy.extend(SESSIONS.with(|s| s.keys().cloned().collect::<Vec<_>>()));
    out.insert("PRES".into(), PRES.prune(ttl, &busy) as i64);
    let keep: HashSet<String> = {
        let mut k = busy.clone();
        PRES.with(|p| k.extend(p.players.iter().filter(|(_, e)| now - e.last <= ttl).map(|(t, _)| t.clone())));
        k
    };
    let dead_tok = |t: &String| !keep.contains(t);
    macro_rules! reg {
        ($name:expr, $d:expr) => {{
            let n = prune_keys(&$d, dead_tok);
            if n != 0 {
                out.insert($name.into(), n as i64);
            }
        }};
    }
    {
        let n = prune_keys(&FEED_SEEN, |t: &String| !keep.contains(t));
        if n != 0 {
            out.insert("FEED_SEEN".into(), n as i64);
        }
    }
    reg!("BOOST_PICK", BOOST_PICK);
    reg!("BOOST_ACTIVE", BOOST_ACTIVE);
    reg!("CLAN_MATCH", CLAN_MATCH);
    reg!("PING_LOC", PING_LOC);
    reg!("LOC_FWD", LOC_FWD);
    reg!("LOC_HIST", LOC_HIST);
    reg!("LOC_SPIKES", LOC_SPIKES);
    reg!("REGION", REGION);
    reg!("REGION_CAND", REGION_CAND);
    reg!("JIP_LATE", JIP_LATE);
    reg!("MESH_ON", MESH_ON);
    reg!("JIP_STARTED", JIP_STARTED);
    reg!("JIP_REPUSH", JIP_REPUSH);
    reg!("_JIP_WAIT_LOG", JIP_WAIT_LOG);
    reg!("LOADED_T", LOADED_T);
    reg!("_SILENT", _SILENT);
    reg!("LOADED", LOADED);
    reg!("CD_GOT", CD_GOT);
    {
        let n = prune_keys(&NAMELESS_BY_IP, |ip: &String| !keep.contains(&NAMELESS_BY_IP.lock().get(ip).cloned().unwrap_or_default()) || NAMELESS_BY_IP.lock().get(ip).is_none());
        if n != 0 {
            out.insert("NAMELESS_BY_IP".into(), n as i64);
        }
    }
    {
        let n = prune_keys(&PING, |k: &(String, String)| !keep.contains(&k.0) || !keep.contains(&k.1));
        if n != 0 {
            out.insert("PING".into(), n as i64);
        }
    }
    prune_keys(&PING_AT, |k: &(String, String)| !PING.lock().contains_key(k));
    {
        let n = prune_keys(&PING_LAST, |k| now - PING_LAST.lock().get(k).map(|v| v.1).unwrap_or(now) > ping_gate_age_s());
        if n != 0 {
            out.insert("PING_LAST".into(), n as i64);
        }
    }
    let mut n = 0usize;
    n += prune_keys(&VOTES, |k: &Key| !k.iter().any(|t| busy.contains(t)));
    n += prune_keys(&MATCH_MAP, |k: &Key| !k.iter().any(|t| busy.contains(t)));
    n += prune_keys(&TIMING, |k: &Key| !k.iter().any(|t| busy.contains(t)));
    n += prune_keys(&SID_KEEP, |k: &Key| !k.iter().any(|t| busy.contains(t)));
    n += prune_keys(&MATCH_P2P, |k: &Key| !k.iter().any(|t| busy.contains(t))); // keyed by the match's token set: gone when none of its players is active
    if n != 0 {
        out.insert("match-sets".into(), n as i64);
    }
    // finished-match records
    let gone_mid = |m: &String| !live_mids.contains(m) && !CLAN_ENDED.lock().contains_key(m);
    let in_pending = |t: &String| CLAN_PENDING.lock().contains_key(t);
    let mut n = prune_keys(&CLAN_ENDED, |m: &String| now - CLAN_ENDED.lock().get(m).copied().unwrap_or(now) > ttl.max(120.0));
    n += prune_keys(&CLAN_REPORTED_END, gone_mid) + prune_keys(&END_BY_REPORTS, gone_mid) + prune_keys(&RESULT_BOOKED, gone_mid);
    n += prune_keys(&CLAN_QUITTERS, |k: &(String, String)| gone_mid(&k.1) && !in_pending(&k.0));
    n += prune_keys(&CLAN_CREDITED, |k: &(String, String)| gone_mid(&k.1) && !in_pending(&k.0));
    n += prune_keys(&CLAN_EXCUSED, |k: &(String, String)| gone_mid(&k.1) && !in_pending(&k.0));
    n += prune_keys(&CLAN_EXCUSE_WHY, |k: &(String, String)| gone_mid(&k.1) && !in_pending(&k.0));
    n += prune_keys(&CLAN_LEFT_AT, |k: &(String, String)| gone_mid(&k.1) && !in_pending(&k.0));
    n += prune_keys(&CLAN_REJOINED, |k: &(String, String)| gone_mid(&k.1) && !in_pending(&k.0));
    n += prune_keys(&CLAN_LAST_DEPART, |m: &String| gone_mid(m) && now - CLAN_LAST_DEPART.lock().get(m).map(|v| v.0).unwrap_or(0.0) > ttl.max(120.0));
    n += prune_keys(&ABORT_AWARDED, gone_mid);
    n += prune_keys(&CLAN_DONE, |t: &String| !keep.contains(t) && !in_pending(t)); // lead 10-19: re-send dedupe only matters while the reporter is around
    n += prune_keys(&JIP_LAST_WHY, |k: &String| !keep.contains(k) && !live_mids.contains(k));
    if n != 0 {
        out.insert("clan-match".into(), n as i64);
    }
    if pool_logged_len() > 500 {
        POOL_LOGGED.lock().clear();
    }
    let mut keep2 = keep.clone();
    keep2.extend(SOCIAL.launcher_tokens());
    for (nm, n) in [
        ("MM", MM.gc(&keep, Some(now))),
        ("PARTIES", PARTIES.gc(&keep2, Some(now))),
        ("PERF", PERF.prune() as i64),
        ("NEW_TOKENS", NEW_TOKENS.prune(Some(now)) as i64),
    ] {
        if n != 0 {
            out.insert(nm.into(), n);
        }
    }
    let n = name_caches_gc();
    if n != 0 {
        out.insert("name-cache".into(), n as i64);
    }
    out.retain(|_, v| *v != 0);
    out
}

// py: daemon.py:10416 PING_GATE_AGE_S
pub fn ping_gate_age_s() -> f64 {
    *PING_GATE_AGE_S
}

// py: daemon.py:10224 gc_tick (status_loop hook: gc_state at most every STATE_GC_S seconds; logs one line when something was dropped)
pub fn gc_tick(now: Option<f64>) -> Option<BTreeMap<String, i64>> {
    let now = now.unwrap_or_else(clock::now);
    {
        let mut l = GC_LAST.lock();
        if now - *l < *STATE_GC_S {
            return None;
        }
        *l = now;
    }
    let r = gc_state(Some(now), None);
    if !r.is_empty() {
        dlog!("GC", "dropped", r.iter().map(|(k, v)| format!("{}={}", k, v)).collect::<Vec<_>>().join(" "));
    }
    Some(r)
}

pub const STATS_LINES: usize = 24;

// py: daemon.py:10253 state_sizes (entry counts of the long-lived registries + thread / connection counts; admin GET /sizes)
pub fn state_sizes() -> Value {
    let mut out = Map::new();
    let mut put = |name: &str, n: usize| {
        out.insert(name.to_string(), json!(n));
    };
    put("CLAN_MATCH", CLAN_MATCH.lock().len());
    put("CLAN_ENDED", CLAN_ENDED.lock().len());
    put("CLAN_PENDING", CLAN_PENDING.lock().len());
    put("CLAN_QUITTERS", CLAN_QUITTERS.lock().len());
    put("CLAN_CREDITED", CLAN_CREDITED.lock().len());
    put("CLAN_LAST_DEPART", CLAN_LAST_DEPART.lock().len());
    put("CLAN_EXCUSED", CLAN_EXCUSED.lock().len());
    put("CLAN_EXCUSE_WHY", CLAN_EXCUSE_WHY.lock().len());
    put("CLAN_REJOINED", CLAN_REJOINED.lock().len());
    put("CLAN_REBOOK", CLAN_REBOOK.lock().len());
    put("CLAN_LEFT_AT", CLAN_LEFT_AT.lock().len());
    put("CLAN_DUE", CLAN_DUE.lock().len());
    put("BOOST_PICK", BOOST_PICK.lock().len());
    put("BOOST_ACTIVE", BOOST_ACTIVE.lock().len());
    put("PEERS", PEERS.lock().len());
    put("LOADED", LOADED.lock().len());
    put("LOADED_T", LOADED_T.lock().len());
    put("NAMELESS_BY_IP", NAMELESS_BY_IP.lock().len());
    put("CLIENT_VERSIONS", CLIENT_VERSIONS.lock().len());
    put("LINUX_CLIENTS", LINUX_CLIENTS.lock().len());
    put("BROKEN_MAPS", BROKEN_MAPS.lock().len());
    put("MATCH_MAP", MATCH_MAP.lock().len());
    put("VOTES", VOTES.lock().len());
    put("_POOL_LOGGED", pool_logged_len());
    put("SID_KEEP", SID_KEEP.lock().len());
    put("TIMING", TIMING.lock().len());
    put("PING_LOC", PING_LOC.lock().len());
    put("PING", PING.lock().len());
    put("LOC_HIST", LOC_HIST.lock().len());
    put("LOC_SPIKES", LOC_SPIKES.lock().len());
    put("REGION", REGION.lock().len());
    put("REGION_CAND", REGION_CAND.lock().len());
    put("JIP_LATE", JIP_LATE.lock().len());
    put("JIP_STARTED", JIP_STARTED.lock().len());
    put("JIP_REPUSH", JIP_REPUSH.lock().len());
    put("_JIP_LAST_WHY", JIP_LAST_WHY.lock().len());
    put("_JIP_WAIT_LOG", JIP_WAIT_LOG.lock().len());
    put("ABORT_AWARDED", ABORT_AWARDED.lock().len());
    put("CD_GOT", CD_GOT.lock().len());
    put("_SILENT", _SILENT.lock().len());
    put("MATCH_P2P", MATCH_P2P.lock().len());
    put("READOPT", READOPT.lock().len());
    let (pp, pu) = PRES.sizes();
    put("Presence.players", pp);
    put("Presence.udp_seen", pu);
    put("Bridges.by_token", BRIDGES.len());
    put("Sessions.by_token", SESSIONS.with(|s| s.len()));
    {
        let g = MM.lock.lock();
        put("MatchMaker.queue", g.queue.len());
        put("MatchMaker.seen", g.seen.len());
        put("MatchMaker.matches", g.matches.len());
        put("MatchMaker.formed", g.formed.len());
        put("MatchMaker.left", g.left.len());
        put("MatchMaker.settle", g.settle.len());
        put("MatchMaker.qtime", g.qtime.len());
        put("MatchMaker.launched", g.launched.len());
        put("MatchMaker.departed", g.departed.len());
        put("MatchMaker.ended", g.ended.len());
        put("MatchMaker.ended_at", g.ended_at.len());
        put("MatchMaker.timed_out", g.timed_out.len());
        put("MatchMaker.started", g.started.len());
        put("MatchMaker.started_t", g.started_t.len());
        put("MatchMaker.departed_vol", g.departed_vol.len());
        put("MatchMaker.departed_t", g.departed_t.len());
        put("MatchMaker.clock", g.clock.len());
        put("MatchMaker.polled", g.polled.len());
        put("MatchMaker.joining", g.joining.len());
        put("MatchMaker.lives", g.lives.len());
        put("MatchMaker.late_bonus", g.late_bonus.len());
        put("MatchMaker.late_mult", g.late_mult.len());
        put("MatchMaker.late_prog", g.late_prog.len());
    }
    let (hm, hb) = HM.sizes();
    put("HostMigration.matches", hm);
    put("HostMigration.by_token", hb);
    // the owners' tables (type name . attribute, as the Python names them)
    put("AddrLimiter.hits", NEW_TOKENS.hits_len());
    put("FEED_SEEN", FEED_SEEN.lock().len());
    let (gh, gw) = GL.sizes();
    put("GunLimits.held", gh);
    put("GunLimits.want", gw);
    let (rr, rl, rb) = REPICK.sizes();
    put("HostRepick.recs", rr);
    put("HostRepick.last_text", rl);
    put("HostRepick.base", rb);
    let (im, ib) = INT.sizes();
    put("Interrogation.matches", im);
    put("Interrogation.by_token", ib);
    put("Notices.texts", NOTICES.texts_len());
    put("Parties.of", PARTIES.of_len());
    put("Parties.inv", PARTIES.inv_len());
    let (pt, ps) = PERF.sizes();
    put("PerfScores.by_token", pt);
    put("PerfScores.by_steam", ps);
    for n in ["RELAY", "RELAY_STATS", "RELAY_TS", "RELAY_TYPES", "UDP_STATS"] {
        put(n, 0); // (the UDP relay is a separate service: its tables are empty here)
    }
    put("_tcp_per_ip", tcp_per_ip_len());
    out.insert("threads".into(), json!(thread_count()));
    out.insert("tcp_live".into(), json!(tcp_live()));
    out.insert("_caps".into(), json!({"Presence.udp_seen": Presence::UDP_SEEN_MAX, "PEERS": PEERS_MAX, "UdpRelay.KA": 512, "UdpRelay.GP": 512, "UdpRelay.buckets": 4096, "UdpRelay.seen": 2000,
                                  "RELAY": 4096, "RELAY_TS": 4096, "AddrLimiter.hits": 4096, "_POOL_LOGGED": 500, "log_queue": 50000, "PING": *PING_MAX}));
    out.insert("log_queue".into(), json!(0));
    Value::Object(out)
}

fn thread_count() -> usize {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("Threads:")).and_then(|l| l.split_whitespace().nth(1)).and_then(|n| n.parse().ok()))
        .unwrap_or(1)
}

// py: daemon.py:10311 _take
fn take_sorted<K: Clone>(c: &mut Vec<(K, i64)>) -> Vec<(K, i64)> {
    let mut items = std::mem::take(c);
    items.sort_by(|a, b| b.1.cmp(&a.1));
    items
}

// py: daemon.py:10318 status_loop
pub fn status_loop() {
    let mut last_bak = clock::now();
    let mut n: u64 = 0;
    // 11-25: db_guard checked the DB at start; the first backup waits DB_BACKUP_S
    loop {
        std::thread::sleep(std::time::Duration::from_secs(10));
        n += 1;
        crate::d::admin::ban_sweep_tick(); // (bans.txt changed: kick the online players it covers)
        if n % 6 == 0 {
            let tr = crate::heap::trim(); // (glibc: freed memory back to the kernel, once a minute)
            dlog!("MEM", format!("{}{}", crate::heap::line(), tr)); // (Rust only, once a minute: live heap vs RSS)
            dlog!("MEM-KINDS", crate::heap::kinds_line()); // (Rust only: net bytes per op / thread kind since start)
        }
        if n % 60 == 0 {
            dlog!("SIZES", py::dumps_compact(&state_sizes())); // (Rust only, every 10 min: the registry sizes of admin GET /sizes)
        }
        if clock::now() - last_bak > db::CFG.backup_s {
            last_bak = clock::now();
            if std::path::Path::new(&db::CFG.path).exists() && db::db_backup() {
                dlog!("DB backup ok");
            }
        }
        status_tick();
    }
}

/// one pass of the status loop body (an exception / panic in it is logged and the loop goes on)
pub fn status_tick() {
    let r = std::panic::catch_unwind(|| -> Result<(), String> {
        dlog!(
            "STATUS",
            format!(
                "online={} queue={} matches={} polls={} tcp={} roreads={}/{} abandoned={}",
                PRES.count(),
                MM.lock.peek(|s| s.queue.len()),
                MM.lock.peek(|s| s.matches.len()),
                NOISY_COUNT.load(Ordering::Relaxed),
                tcp_live(),
                db::RO_READS[0].load(Ordering::Relaxed),
                db::RO_READS[1].load(Ordering::Relaxed),
                db::ABANDONED.load(Ordering::Relaxed)
            )
        );
        NOISY_COUNT.store(0, Ordering::Relaxed);
        reap_matches(None).map_err(|e| e.to_string())?;
        ac_tick();
        gc_tick(None);
        save_tables_if_due(None);
        readopt_expire(None);
        live_save(None);
        net_report(None, 60.0);
        // 11-14 ping limit (never breaks the status tick)
        if let Err(_p) = std::panic::catch_unwind(|| ping_check(None)) {
            dlog!("PING-CHECK ERR", "panic");
        }
        // 11-23 packet loss limit (never breaks the status tick)
        if let Err(_p) = std::panic::catch_unwind(|| loss_check(None)) {
            dlog!("LOSS-CHECK ERR", "panic");
        }
        db::db_maintenance_tick(clock::now());
        if HM.enabled {
            HM.tick();
            repick_tick();
        }
        write_status_file();
        Ok(())
    });
    match r {
        Ok(Ok(())) => {}
        Ok(Err(e)) => dlog!("STATUS-ERR", e),
        Err(_) => dlog!("STATUS-ERR", "panic"),
    }
}

// py: daemon.py:10380 the status file part of status_loop (its own try: written last)
pub fn status_json() -> Value {
    let live = live_snapshot();
    let roster = roster_public(&live);
    json!({"ts": jf(clock::wall_time()), "server_version": server_version(), "players_online": PRES.count(), "queue": MM.lock.peek(|s| s.queue.len()),
           "matches": MM.lock.peek(|s| s.matches.len()), "live": live, "roster": roster, "notices": notices_public(), "target_version": update_target(), "map_pool": public_map_pool()})
}

fn write_status_file() {
    let sf = STATUS_FILE.as_str();
    if sf.is_empty() {
        return;
    }
    let tmp = format!("{}.tmp", sf);
    let r = std::fs::write(&tmp, py::dumps(&status_json()).as_bytes()).and_then(|_| std::fs::rename(&tmp, sf));
    if let Err(e) = r {
        if !STATUS_ERR.swap(true, Ordering::Relaxed) {
            dlog!("STATUS-FILE-ERR", crate::pyfmt::os_error_repr(&e));
        }
    }
}
