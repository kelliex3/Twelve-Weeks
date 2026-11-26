//! K5 batch: daemon.py:8265-9127 (handle_leave, handle_match_end, loaded_clear_match, jip_requeue_if_empty, prestart_host_lost,
//! maybe_game_start, handle_batch = the request dispatcher of tcp_conn).
//!
//! Error handling: the Python lets sqlite errors / RequestAbandoned propagate out of these functions (tcp_conn logs "HANDLER ERR (conn kept)" or
//! closes the connection); here `db::DbResult` carries them and `?` turns them into `HErr`. A socket error of a plain `conn.sendall` propagates the
//! same way (`HErr::Conn`); pushes to OTHER players' connections ignore it (`except OSError: pass`).
use super::*;
use crate::{clock, py, pyfmt, util};
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;
use super::ext_k5::k2::{
    ach_live_op, ach_read_op, ach_report_op, clan13_op, clan_ingame_stats_roster, clan_match_started, clan_op, feed_op, leaderboard_table, meta_op, names_op,
    rank_table_op,
};

/// `str(x)` of an optional string the way a log line prints it (`None` or the text)
fn on(o: &Option<String>) -> String {
    match o {
        Some(s) => s.clone(),
        None => "None".to_string(),
    }
}

/// `hex(op)`
fn hx(op: u8) -> String {
    format!("0x{:x}", op)
}

/// `get_name(t) or fallback`
fn name_or(t: &str, fallback: &str) -> String {
    let n = get_name(t);
    if n.is_empty() {
        fallback.to_string()
    } else {
        n
    }
}

fn loaded_has(t: &str) -> bool {
    LOADED.lock().contains(t)
}

fn le_u64(b: &[u8]) -> u64 {
    let mut v: u64 = 0;
    for (i, x) in b.iter().take(8).enumerate() {
        v |= (*x as u64) << (8 * i);
    }
    v
}

// py: daemon.py:9509 LEAVE_KEEP_S (S2: a 0x02 this soon after the level start = dying conn, the re-queue re-sends the start)
pub static LEAVE_KEEP_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LEAVE_KEEP", 15.0));

// py: daemon.py:8265 handle_leave
/// Op 0x02 (client leave/cancel). Runs BEFORE the other ops of its batch.
pub fn handle_leave(addr: Addr, st: &ConnSt) -> db::DbResult<()> {
    let Some(token) = st.token.as_deref().filter(|t| !t.is_empty()) else { return Ok(()) };
    let others = MM.cancel(token);
    if let Some(others) = others {
        // backed out during the countdown: everyone else returns to SEARCHING
        for o in &others {
            LOADED.lock().remove(o);
            if let Some(c) = SESSIONS.conn(o) {
                let _ = push(&c, &searching_frame(Some(o)), true);
            }
        }
        dlog!("LEAVE during countdown: match cancelled, back to searching", pa(addr), st.name, tags(&others));
        evt!("cancel", tag(token), "backed out before the start; back in the queue:", tags(&others));
        return Ok(());
    }
    let keep = {
        let in_match = MM.in_match(token);
        in_match && !loaded_has(token) && !MM.all_loaded(token) && MM.match_age(token) <= countdown_seconds() + *LEAVE_KEEP_S && !MM.lock.peek(|g| g.joining.contains_key(token))
    };
    if keep {
        // the level start went out but this player never loaded (the start likely hit a dying conn): keep the match, their re-queue re-sends the start
        dlog!("LEAVE kept in match (launched, not loaded; re-queue re-sends the start)", pa(addr), st.name);
        return Ok(());
    }
    if MM.dequeue(token) {
        evt!("dequeue", tag(token), "left the searching page (0x02)");
    }
    let mid_l = MM.mid_of(token);
    let loading_l = {
        let g = MM.lock.lock();
        mid_l.as_ref().is_some_and(|m| !g.started.contains(m) && g.launched.contains(m))
    };
    let jl = JIP_LATE.lock().get(token).cloned();
    if let Some(jl) = jl {
        if Some(&jl.0) == mid_l.as_ref() && !loaded_has(token) && clock::now() - jl.1 < 120.0 {
            dlog!(format!("jip: joiner backed out before loading (0x02 {} s after the start)", py::fixed(clock::now() - jl.1, 0)), tag(token), "match", on(&mid_l));
        }
    }
    let kept = leave_running_match(token, "left (0x02)");
    LOADED.lock().remove(token);
    dlog!("LEAVE", pa(addr), st.name, if kept { "(the others play on)" } else { "" });
    if kept && loading_l {
        // S2: a player backed out while the level loads: the others may all be loaded already - start now, do not wait for the reaper
        let mid_l = mid_l.unwrap_or_default();
        let dep_l: HashSet<String> = MM.lock.lock().departed.get(&mid_l).cloned().unwrap_or_default();
        let pl_l = MM.players_of_mid(&mid_l).unwrap_or_default();
        let rest: Vec<String> = pl_l.iter().filter(|p| !dep_l.contains(*p)).cloned().collect();
        if !rest.is_empty() {
            let all_in = {
                let ld = LOADED.lock();
                rest.iter().all(|p| ld.contains(p))
            };
            if all_in || (!pl_l.is_empty() && pl_l[0] == token) {
                // (the host left: survivors back to the queue now)
                maybe_game_start(&rest[0])?;
            }
        }
    }
    Ok(())
}

// py: daemon.py:8318 handle_match_end
/// Op 0x12 [winning team] from the host. Must run BEFORE a 0x02 of the same batch.
pub fn handle_match_end(addr: Addr, st: &ConnSt, batch: &Batch) -> db::DbResult<()> {
    for (op, pl) in batch {
        let Some(token) = st.token.as_deref().filter(|t| !t.is_empty()) else { continue };
        if *op != 0x12 {
            continue;
        }
        readopt_claim(token);
        let hm_has = HM.enabled && HM.has(token);
        if hm_has && HM.host_token(token).as_deref() != Some(token) {
            // a host we migrated away from (the others could not hear it) ended its own lonely copy of the match: the real host reports the result
            dlog!("MATCH-END ignored (not the current host after a migration)", pa(addr), tag(token));
            continue;
        }
        let players = MM.players_of(token);
        let host_t: Option<String> = if hm_has { HM.host_token(token) } else { players.as_ref().and_then(|p| p.first().cloned()) };
        let players_v: Vec<String> = players.clone().unwrap_or_default();
        if !players_v.is_empty() {
            if let Some(h) = &host_t {
                if h != token {
                    dlog!("MATCH-END ignored (not from the host)", pa(addr), tag(token), "host is", tag(h));
                    continue;
                }
            }
        }
        let winner: u8 = pl.first().copied().unwrap_or(0xFF);
        if !matches!(winner, 0 | 1 | 2) {
            dlog!("MATCH-END ignored (bad winner byte)", pa(addr), tag(token), winner);
            continue;
        }
        dlog!("MATCH-END", pa(addr), "winner team", winner, "players", tags(&players_v));
        evt!("match-end", on(&MM.mid_of(token)), "winner team", winner, "reported by", tag(token), format!("after {} s", py::fixed(MM.match_age(token), 0)), "players", tags(&players_v));
        if players_v.is_empty() {
            continue;
        }
        let winner = winner as i64;
        let left_early: HashSet<String> = {
            let g = MM.lock.lock();
            g.mid_locked(token).and_then(|m| g.departed.get(&m).cloned()).unwrap_or_default()
        };
        let mid_r = MM.mid_of(token);
        if let Some(m) = &mid_r {
            if RESULT_BOOKED.lock().contains(m) {
                dlog!("MATCH-END repeated for", m, "- already booked, ignored");
                continue;
            }
            RESULT_BOOKED.lock().insert(m.clone());
        }
        record_result(&players_v, winner, &left_early, mid_r.as_deref())?;
        if let Some(m) = &mid_r {
            // the clan chronicle: did this player's team win (read by clan_match_facts)
            for (i, t) in players_v.iter().enumerate() {
                if !left_early.contains(t) {
                    let v = if winner == 2 { None } else { Some(team_in(m, i as i64, Some(t)) == winner) };
                    CLAN_WIN.lock().insert((t.clone(), m.clone()), v);
                }
            }
            {
                let mut cw = CLAN_WIN.lock();
                if cw.len() > 600 {
                    cw.shift_remove_index(0);
                }
            }
            // who played on which team and who won: the RAIDS page's MATCH STATS list (clan_ingame section 5)
            let ros: Vec<(String, String, i64, bool)> = players_v
                .iter()
                .enumerate()
                .map(|(i, t)| {
                    let g = get_name(t);
                    let nm = if !g.is_empty() {
                        g
                    } else {
                        match token_name(t) {
                            Some(n) if !n.is_empty() => n,
                            _ => "?".to_string(),
                        }
                    }; // (names outside _dblock: deadlock)
                    (t.clone(), nm, team_in(m, i as i64, Some(t)), left_early.contains(t))
                })
                .collect();
            let mapname = CLAN_MAP.lock().get(m).cloned().unwrap_or_default();
            match db::with_write(|c| {
                clan_ingame_stats_roster(c, m, &mapname, &ros, winner)?;
                Ok(())
            }) {
                Ok(()) => {}
                Err(db::DbErr::Abandoned) => return Err(db::DbErr::Abandoned),
                Err(e) => dlog!("CLAN match roster failed", e),
            }
        }
        if let Some(m) = &mid_r {
            if END_BY_REPORTS.lock().contains(m) {
                // the daemon closed the match from the clients' reports first: the late result still books the win, the closers already ran
                dlog!("MATCH-END after the client-report close of", m, "- winner booked, nothing closed twice");
                continue;
            }
        }
        let mid_now = MM.mid_of(token);
        clan_match_ended(&players_v, mid_now.as_deref().unwrap_or(""));
        loaded_clear_match(mid_now.as_deref().unwrap_or(""), &players_v);
        // free the match for requeue only after the scoreboard: dropping it at once made the clients quit to the menu right away
        let mid_end = MM.mid_of(token);
        if let Some(m) = &mid_end {
            MM.end(m);
            INT.end(m, Some(winner));
            GL.end(m);
        }
        live_save(None); // fuzz seed 243: a restart in the 9 s scoreboard hold re-adopted the ended match as running
        let p0 = players_v[0].clone();
        let pl_c = players_v.clone();
        py_timer(
            9.0,
            Box::new(move || {
                match &mid_end {
                    Some(m) => MM.drop_mid(m),
                    None => MM.leave(&p0),
                }
                if let Some(m) = &mid_end {
                    HM.end(m);
                }
                forget_match_state(&pl_c);
                evt!("freed", on(&mid_end), tags(&pl_c));
            }),
        );
    }
    Ok(())
}

// py: daemon.py:8396 loaded_clear_match
/// Forget the LOADED mark of a finished / dropped match's players - but NOT of a player who already plays in ANOTHER live match.
pub fn loaded_clear_match(mid: &str, players: &[String]) {
    for p in players {
        let cur = MM.mid_of(p);
        if cur.is_none() || cur.as_deref() == Some(mid) {
            LOADED.lock().remove(p);
        } else {
            dlog!("LOADED mark of", tag(p), "kept: it already plays in match", on(&cur), "(match", mid, "ended / dropped)");
        }
    }
}

// py: daemon.py:8409 _JIP_WAIT_LOG
pub static JIP_WAIT_LOG: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:8411 jip_requeue_if_empty
/// J3: every established player of a running match left while a joiner was still loading. Returns the joiners sent back to searching.
pub fn jip_requeue_if_empty(mid: &str) -> Vec<String> {
    let (pl, dep, joining) = {
        let g = MM.lock.lock();
        let pl: Vec<String> = g.matches.get(mid).cloned().unwrap_or_default();
        let dep: HashSet<String> = g.departed.get(mid).cloned().unwrap_or_default();
        let joining: Vec<String> = g.joining.iter().filter(|(_, (m, _))| m == mid).map(|(t, _)| t.clone()).collect();
        (pl, dep, joining)
    };
    if joining.is_empty() {
        return Vec::new();
    }
    let est: Vec<&String> = pl.iter().filter(|p| !dep.contains(*p) && !joining.contains(*p)).collect();
    if !est.is_empty() {
        return Vec::new();
    }
    for j in &joining {
        LOADED.lock().remove(j);
        let c = SESSIONS.conn(j);
        leave_running_match(j, "match empty before its start");
        evt!("jip-empty", mid, tag(j), "everybody else left the match: joiner sent back to searching");
        dlog!("jip: match", mid, "has no established player left - joiner", tag(j), "goes back to searching");
        if let Some(c) = c {
            let _ = c.sendall(&[msg(0x11, &srv_note("jip_ended", j)), searching_frame(Some(j))].concat());
        }
    }
    joining
}

// py: daemon.py:8436 PRESTART_HOSTLOST_REQUEUE
pub static PRESTART_HOSTLOST_REQUEUE: LazyLock<bool> = LazyLock::new(|| crate::env_str("T2R_PRESTART_HOSTLOST_REQUEUE", "1") != "0");

// py: daemon.py:8438 prestart_host_lost
/// The host (slot 0) crashed during the level load: the match is called off and the survivors go back to SEARCHING at their old queue place.
pub fn prestart_host_lost(mid: &str, players: &[String], survivors: &[String]) -> bool {
    if !*PRESTART_HOSTLOST_REQUEUE {
        return false;
    }
    let only: HashSet<String> = survivors.iter().cloned().collect();
    let back = MM.abort(mid, Some(&only)).unwrap_or_default();
    HM.end(mid);
    GL.end(mid);
    loaded_clear_match(mid, players);
    for p in &back {
        CD_GOT.lock().remove(p);
        _SILENT.lock().remove(p);
        SESS_LOST.lock().remove(p);
        if let Some(c) = SESSIONS.conn(p) {
            let _ = c.sendall(&[msg(0x11, &srv_note("host_lost", p)), searching_frame(Some(p))].concat());
        }
    }
    forget_match_state(players);
    let h0 = players.first().cloned().unwrap_or_default();
    dlog!("MATCH-ABORT host lost before the start", mid, "host", tag(&h0), "| back to searching:", tags(&back));
    evt!("reaped", mid, format!("host {} gone before the GAME-START: called off, survivors back in the queue at their old place", tag(&h0)), tags(&back));
    live_save(None);
    true
}

// py: daemon.py:8467 maybe_game_start
/// When ALL players still in the token's match have loaded -> send 0x0a -> GAME STARTS. Returns True when the start went out.
pub fn maybe_game_start(token: &str) -> db::DbResult<bool> {
    let (mid, active) = MM.active_of(token);
    let players = MM.players_of(token);
    let Some(mid) = mid else { return Ok(false) };
    if active.is_empty() {
        return Ok(false);
    }
    let running = MM.lock.lock().started.contains(&mid);
    let joiners: Vec<String> = if *JIP && running { MM.jip_joiners(&mid) } else { Vec::new() };
    if !joiners.is_empty() && !jip_requeue_if_empty(&mid).is_empty() {
        return Ok(false);
    }
    if joiners.is_empty() && !running && !active.is_empty() {
        let (pl_m, launched_m) = {
            let g = MM.lock.lock();
            (g.matches.get(&mid).cloned().unwrap_or_default(), g.launched.contains(&mid))
        };
        if launched_m && !pl_m.is_empty() && !active.contains(&pl_m[0]) && prestart_host_lost(&mid, &pl_m, &active) {
            return Ok(false); // the host (slot 0) left during the load: nobody can start this match
        }
    }
    if joiners.is_empty() {
        let all_in = {
            let ld = LOADED.lock();
            active.iter().all(|p| ld.contains(p))
        };
        if !all_in {
            return Ok(false);
        }
    }
    if !joiners.is_empty() {
        // join in progress: 0x0a goes to the joiner(s) ONLY. It waits until the others' rosters list the joiner (jip_acked, max JIP_ACK_MAX s).
        let go: Vec<String> = joiners.iter().filter(|j| loaded_has(j) && MM.jip_acked(j)).cloned().collect();
        for j in &joiners {
            if loaded_has(j) && !go.contains(j) {
                let last = JIP_WAIT_LOG.lock().get(j).copied().unwrap_or(0.0);
                if clock::now() - last > 5.0 {
                    JIP_WAIT_LOG.lock().insert(j.clone(), clock::now());
                    dlog!("jip: GAME-START of", tag(j), format!("waits for the other players' roster refresh ({} s of max {} s)", py::fixed(MM.jip_age(j), 0), py::fixed(*JIP_ACK_MAX, 0)));
                }
            }
        }
        for j in &go {
            if let Some(c) = SESSIONS.conn(j) {
                let _ = c.sendall(&msg(0x0a, b"\x01"));
            }
            let done = MM.jip_done(j);
            JIP_STARTED.lock().insert(j.clone(), (mid.clone(), clock::now()));
            JIP_REPUSH.lock().remove(j);
            if !PRIV.is_private_mid(&mid) {
                add_matches(std::slice::from_ref(j))?;
            }
            clan_match_started(std::slice::from_ref(j), &mid);
            let after = clock::now() - done.map(|d| d.1).unwrap_or_else(clock::now);
            dlog!("jip: GAME-START 0x0a ->", tag(j), "(joined the running match", mid, format!("after {} s)", py::fixed(after, 0)));
            evt!("jip-start", mid, tag(j));
        }
        if !go.is_empty() {
            live_save(None); // (fuzz 10-19: a daemon restart within 10 s of a joiner's GAME-START forgot the joiner)
        }
        return Ok(!go.is_empty());
    }
    for p in &active {
        if let Some(c) = SESSIONS.conn(p) {
            let _ = c.sendall(&msg(0x0a, b"\x01"));
        }
    }
    dlog!("GAME-START 0x0a ->", tags(&active));
    let first = {
        let mut g = MM.lock.lock();
        let first = !g.started.contains(&mid);
        g.started.insert(mid.clone());
        g.started_t.insert(mid.clone(), clock::now());
        first
    };
    if first {
        let players = players.unwrap_or_default();
        evt!("game-start", mid, "map", map_level(map_index(&players)), "players", tags(&active));
        timing_summary(&players);
        if !PRIV.is_private_mid(&mid) {
            add_matches(&active)?; // (11-17: a private match is not a played match on the leaderboard)
        }
        clan_match_started(&players, &mid);
        HM.start(&mid, &players);
        let gone_already = !MM.lock.lock().matches.contains_key(&mid);
        if gone_already {
            // (fuzz 10-19 seed 86: the last player left between the GAME-START and this line; its leave had already run HM.end)
            HM.end(&mid);
        }
        let left_before: Vec<String> = {
            let g = MM.lock.lock();
            players.iter().filter(|p| g.departed.get(&mid).is_some_and(|d| d.contains(*p))).cloned().collect()
        };
        for p in &left_before {
            // S4: left during the load: its slot is gone from the first frame (a lost host migrates at once)
            if MM.mid_of(p).is_none() {
                HM.depart(p, "left before the start");
            }
        }
        let mode_in = MODEQ.match_mode(&mid);
        let mode = INT.start(&mid, &players, map_index(&players), &|s: usize| team_of(s as i64), mode_in.as_deref());
        dlog!("MODE", mode, tags(&players));
        live_save(None);
    }
    Ok(true)
}

// py: daemon.py:9120 EXT_PRESENCE_OPS
/// Ext short-connection ops whose payload starts with the 64-hex-char token: each one is proof of life of a player that is already present.
/// (0xC4 = the capability + region preference report; 11-26: also with T2R_DEDICATED off, the preference is stored)
pub const EXT_PRESENCE_OPS: [u8; 18] = [0xBC, 0xCC, 0xD6, 0xE0, 0xE2, 0xE3, 0xE4, 0xE6, 0xE8, 0xEE, 0xEF, 0xBA, 0xDE, 0xCA, 0xB4, 0xB6, 0xD0, OP_CAPS];

const OP_HWID: u8 = 0xB2;
const OP_ATTEST_REQ: u8 = 0xA1;
const OP_PRIV_REQ: u8 = 0xF8;

/// Python `rep` truthiness for bytes
fn non_empty(r: Option<Vec<u8>>) -> Option<Vec<u8>> {
    r.filter(|b| !b.is_empty())
}

/// Run one op of a DB-backed handler: `Ok(Some(reply))` to send, `Ok(None)` nothing, `Err` = RequestAbandoned (propagates); any other error is
/// logged like the Python's `except Exception` (`what` = the log tag, e.g. "NAMES ERR").
fn db_rep(what: &str, addr: Addr, extra: Option<String>, r: db::DbResult<Option<Vec<u8>>>) -> Result<Option<Vec<u8>>, HErr> {
    match r {
        Ok(v) => Ok(non_empty(v)),
        Err(db::DbErr::Abandoned) => Err(HErr::Abandoned),
        Err(e) => {
            match extra {
                Some(x) => dlog!(what, pa(addr), x, e),
                None => dlog!(what, pa(addr), e),
            }
            Ok(None)
        }
    }
}

// py: daemon.py:9124 handle_batch
pub fn handle_batch(conn: &Conn, addr: Addr, st: &mut ConnSt, batch: &mut Batch) -> HResult {
    let ops: Vec<u8> = batch.iter().map(|x| x.0).collect();
    let has = |o: u8| ops.contains(&o);
    let mut done_end = false;
    let ip = ip_of(&addr);
    for (op, pl) in batch.iter() {
        if EXT_PRESENCE_OPS.contains(op) && pl.len() >= 64 {
            let tk = latin1(&pl[..64]);
            if is_token(&tk) {
                PRES.touch_known(&tk);
            }
        }
    }
    if has(0x12) {
        handle_match_end(addr, st, batch)?;
        done_end = true;
    }
    if has(0x02) {
        handle_leave(addr, st)?;
    }

    // --- dedicated match servers: capability + region pings (t2r-ext's own short connection) ---
    if has(OP_CAPS) {
        for (op, pl) in batch.iter() {
            if *op == OP_CAPS {
                if let Some(rep) = dedicated_caps_op(pl) {
                    if !rep.is_empty() {
                        conn.sendall(&rep)?;
                    }
                }
            }
        }
        return Ok(());
    }

    // --- custom name / clan tag from t2r-ext (its own short connection) ---
    if ops.iter().any(|o| matches!(o, 0xD0 | 0xD2)) {
        for (op, pl) in batch.iter() {
            if matches!(op, 0xD0 | 0xD2) {
                if _creates_rows(*op, pl, &addr) {
                    continue;
                }
                if let Some(rep) = db_rep("NAMES ERR", addr, Some(hx(*op)), names_op(*op, pl))? {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- anti-cheat: flag events / demo upload (t2r-ext) and player reports / recent players (launcher): own short connections ---
    if ops.iter().any(|o| matches!(o, 0xCC | 0xCE | 0xD4 | 0xD6)) {
        for (op, pl) in batch.iter() {
            if matches!(op, 0xCC | 0xCE | 0xD4 | 0xD6) {
                if let Some(rep) = non_empty(ac_op(*op, pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- clan requests from t2r-ext (its own short connection) ---
    if ops.iter().any(|o| matches!(o, 0xE0 | 0xE2 | 0xE4)) {
        for (op, pl) in batch.iter() {
            if matches!(op, 0xE0 | 0xE2 | 0xE4) {
                if _creates_rows(*op, pl, &addr) {
                    continue;
                }
                if let Some(rep) = db_rep("CLAN ERR", addr, Some(hx(*op)), clan_op(*op, pl))? {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- clan feed lines for the main menu notification (t2r-ext's own short connection): op 0xB4, see feed_op ---
    if has(0xB4) {
        for (op, pl) in batch.iter() {
            if *op == 0xB4 {
                if let Some(rep) = non_empty(feed_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- the rank table for the badge in front of every name: op 0xBF [64B token] -> reply 0xBF ---
    if has(0xBF) {
        for (op, pl) in batch.iter() {
            if *op == 0xBF {
                if let Some(rep) = db_rep("RANKS ERR", addr, None, rank_table_op(pl))? {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- achievements: op 0xA9 match counters (books) / 0xAA read / 0xAB live counters ---
    if has(0xA9) || has(0xAA) || has(0xAB) {
        for (op, pl) in batch.iter() {
            if matches!(op, 0xA9 | 0xAA | 0xAB) {
                if _creates_rows(*op, pl, &addr) {
                    continue;
                }
                let rep = match *op {
                    0xA9 => ach_report_op(pl),
                    0xAB => ach_live_op(pl),
                    _ => ach_read_op(pl),
                };
                if let Some(rep) = non_empty(rep) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- the in-game CLAN page (to1/clan13): op 0xB6, see clan13_op ---
    if has(0xB6) {
        for (op, pl) in batch.iter() {
            if *op == 0xB6 {
                if let Some(rep) = non_empty(clan13_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- boosters (t2r-ext's own short connection) ---
    if ops.iter().any(|o| matches!(o, 0xE8 | 0xEE | 0xEF)) {
        for (op, pl) in batch.iter() {
            if matches!(op, 0xE8 | 0xEE | 0xEF) {
                if _creates_rows(*op, pl, &addr) {
                    continue;
                }
                if let Some(rep) = db_rep("META ERR", addr, Some(hx(*op)), meta_op(*op, pl))? {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- join in progress: clock report + roster refresh from t2r-ext (its own short connection) ---
    if has(0xE3) {
        for (op, pl) in batch.iter() {
            if *op == 0xE3 {
                if let Some(rep) = non_empty(jip_roster_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        if ops.iter().all(|o| *o == 0xE3) {
            return Ok(());
        }
    }
    // --- host migration poll from t2r-ext (its own short connection) ---
    if has(0xE6) {
        for (op, pl) in batch.iter() {
            if *op == 0xE6 && pl.len() >= 64 {
                let t = latin1(&pl[0..64]);
                let flags = if pl.len() > 64 { pl[64] } else { 0 };
                let epoch = if pl.len() > 65 { pl[65] } else { 0 };
                // the ext polls this every 0.5-2 s for the whole match: proof the player is there
                readopt_claim(&t);
                PRES.touch_known(&t);
                if flags & 1 != 0 && *JIP {
                    jip_unreachable(&t, None);
                }
                match HM.poll(&t, flags, epoch) {
                    Some(r) => {
                        if let Err(e) = conn.sendall(&msg(0xE7, &r)) {
                            dlog!("HOSTMIG ERR", pa(addr), e);
                        }
                    }
                    None => dlog!("HOSTMIG ERR", pa(addr), "poll failed"),
                }
            }
        }
        if ops.iter().all(|o| *o == 0xE6) {
            return Ok(()); // (it used to fall through to the 'SESSION ops=[0xe6]' log line every 2 s)
        }
    }
    // --- HWID report from t2r-ext (its own short connection): op 0xB2, see hwid_op ---
    if has(OP_HWID) {
        for (op, pl) in batch.iter() {
            if *op == OP_HWID {
                conn.sendall(&hwid_op(pl, Some(&ip)))?;
            }
        }
        return Ok(());
    }
    // --- anti-cheat scan report (op 0xC2, acscan.py) // PRIVATE HOOK: the public build has no acscan, so the op falls through like Python's `acscan is None` ---
    // --- install check file report from t2r-ext (its own short connection): op 0xA1, see attest_op ---
    if has(OP_ATTEST_REQ) {
        for (op, pl) in batch.iter() {
            if *op == OP_ATTEST_REQ {
                conn.sendall(&attest_op(pl, &addr))?;
            }
        }
        return Ok(());
    }
    // --- version check from the launcher (Play.ps1) / any client: op 0xB0, see version_op ---
    if has(OP_VERSION) {
        for (op, pl) in batch.iter() {
            if *op == OP_VERSION {
                conn.sendall(&version_op(pl, Some(&addr)))?;
            }
        }
        return Ok(());
    }
    // --- server notices (t2r-ext short connection, every ~6 s in a match and in the menus): op 0xBA, see notice_op ---
    if has(crate::notices::OP_REQ) {
        for (op, pl) in batch.iter() {
            if *op == crate::notices::OP_REQ {
                if let Some(rep) = non_empty(notice_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- private matches (t2r-ext short connection): op 0xF8, see priv_op ---
    if has(OP_PRIV_REQ) {
        for (op, pl) in batch.iter() {
            if *op == OP_PRIV_REQ {
                if let Some(rep) = non_empty(priv_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- party ops (t2r-ext short connection) ---
    if has(0xDA) {
        for (op, pl) in batch.iter() {
            if *op == 0xDA {
                if let Some(rep) = non_empty(party_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- friends + parties (the launcher's own short connection): op 0xD8, see social_op ---
    if has(crate::social::OP_REQ) {
        for (op, pl) in batch.iter() {
            if *op == crate::social::OP_REQ {
                if let Some(rep) = non_empty(social_op(pl, Some(&addr))) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- per-team gun limits (t2r-ext short connection): op 0xDE, see gunlimit_op ---
    if has(crate::gunlimit::OP_REQ) {
        for (op, pl) in batch.iter() {
            if *op == crate::gunlimit::OP_REQ {
                if let Some(rep) = non_empty(gunlimit_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- per-mode queues (t2r-ext short connection): op 0xBC, see modeq_op ---
    if has(crate::modeq::OP_REQ) {
        for (op, pl) in batch.iter() {
            if *op == crate::modeq::OP_REQ {
                if let Some(rep) = non_empty(modeq_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- Interrogation mode (t2r-ext short connection) ---
    if has(crate::interrogation::OP_REQ) {
        for (op, pl) in batch.iter() {
            if *op == crate::interrogation::OP_REQ {
                if let Some(rep) = non_empty(interrogation_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- performance report from t2r-ext (its own short connection) ---
    if has(crate::hostmig::OP_PERF) {
        for (op, pl) in batch.iter() {
            if *op == crate::hostmig::OP_PERF {
                if let Some(rep) = non_empty(perf_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- SP-section map support from t2r-ext (its own short connection) ---
    if has(crate::map_packs::SP_OP_REPORT) {
        for (op, pl) in batch.iter() {
            if *op == crate::map_packs::SP_OP_REPORT {
                if let Some(rep) = non_empty(sp_support_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- map packs from t2r-ext (its own short connection) ---
    if has(crate::map_packs::OP_REPORT) {
        for (op, pl) in batch.iter() {
            if *op == crate::map_packs::OP_REPORT {
                if let Some(rep) = non_empty(map_packs_op(pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }
    // --- map vote from t2r-ext (its own short connection) ---
    if ops.iter().any(|o| matches!(o, 0xEA | 0xEC)) {
        for (op, pl) in batch.iter() {
            if matches!(op, 0xEA | 0xEC) {
                if let Some(rep) = non_empty(vote_op(*op, pl)) {
                    conn.sendall(&rep)?;
                }
            }
        }
        return Ok(());
    }

    // --- bridge register (its own connection) ---
    for (op, pl) in batch.iter() {
        if *op == 0xF0 && pl.len() >= 73 {
            let t = latin1(&pl[0..64]);
            let steamid = le_u64(&pl[64..72]);
            let nl = pl[72] as usize;
            let nm = py::clean_bytes(util::sl(pl, 73, 73 + nl), 31);
            let name = if nm.is_empty() { "PLAYER".to_string() } else { nm };
            let port: i64 = if pl.len() >= 75 + nl { le_u64(&pl[73 + nl..75 + nl]) as i64 } else { 0 };
            if !is_token(&t) || !(STEAMID_MIN <= steamid && steamid < STEAMID_MAX) || !(0 < port && port < 65536) {
                dlog!("BRIDGE", pa(addr), "refused: malformed registration");
                return Ok(());
            }
            if !token_known(&t) && !NEW_TOKENS.allow(&ip, None) {
                dlog!("BRIDGE", pa(addr), "refused: too many new identities from this address");
                return Ok(());
            }
            st.token = Some(t.clone());
            st.name = name.clone();
            st.is_bridge = true;
            set_steam(&t, steamid, &name)?;
            if HM.enabled && HM.has(&t) {
                HM.bridge_back(&t);
            }
            BRIDGES.register(&t, conn.clone(), steamid, port, Some(&ip), Some(&name));
            PING_LOC.lock().shift_remove(&t); // resend its location to the others
            let game_tok = NAMELESS_BY_IP.lock().get(&ip).cloned();
            if let Some(g) = game_tok {
                if g != t {
                    adopt_bridge_name(&g, &ip);
                }
            }
            dlog!("BRIDGE", pa(addr), name, "steamid", steamid, "port", port, "token", tag(&t));
            ident_note(&t, steamid, Some(&ip));
            let _ = conn.sendall(&msg(0xF1, &[])); // registration ack: the helper reports "server up" to the menu only after a real reply
            return Ok(());
        }
    }
    if st.is_bridge {
        let btok = st.token.clone().unwrap_or_default();
        for (op, pl) in batch.iter() {
            if *op == 0xF4 && !pl.is_empty() {
                let loc = latin1(pl);
                if loc.chars().count() > LOC_MAX || !loc_ok(&loc) {
                    dlog!("PING-LOC", tag(&btok), format!("refused: malformed location string ({} B)", pl.len()));
                    continue;
                }
                ping_location(&btok, &loc);
            } else if *op == 0xF6 && pl.len() >= 12 {
                let ms = i32::from_le_bytes([pl[8], pl[9], pl[10], pl[11]]) as i64;
                ping_report(&btok, le_u64(&pl[0..8]), ms);
            }
        }
        return Ok(()); // helper keepalive (0xF3)
    }

    // --- persist any profile/login; learn token ---
    for (op, pl) in batch.iter() {
        if *op == 0x0f {
            let (t, loadout) = parse_login(pl);
            if let Some(t) = t.filter(|t| !t.is_empty()) {
                readopt_claim(&t);
                st.token = Some(t.clone());
                if let Some((kind, w, evid)) = ac_note_login_ip(&t, &ip) {
                    // one token from many addresses = a shared / stolen token (scored low, never acts alone)
                    let m = CLAN_LAST_MATCH.lock().get(&t).cloned().filter(|m| !m.is_empty()).unwrap_or_else(|| "00000000".to_string());
                    ac_server_event(&t, Some(&m), kind, w, &evid);
                }
                if let Some(lo) = &loadout {
                    if new_identity_ok(&t, Some(&addr)) {
                        save_loadout(&t, lo)?;
                    }
                }
                adopt_bridge_name(&t, &ip);
                PRES.touch(&t, &name_or(&t, &st.name));
            }
        } else if *op == 0x01 || *op == 0x0e {
            if let Some(nm0) = name_from_session(pl).filter(|n| !n.is_empty()) {
                let nm = py::clean_str(&nm0, 31);
                st.name = nm.clone();
                // (a frame without a login used to create a player row keyed by the NAME: 47 junk rows from the open internet in the live DB)
                if !nm.is_empty() && nm != "PLAYER" {
                    if let Some(tk) = st.token.clone() {
                        if is_token(&tk) && get_name(&tk).is_empty() {
                            // skip echoed default
                            set_name(&tk, &nm)?;
                        }
                    }
                }
            }
        }
    }

    let tok: String = match &st.token {
        Some(t) if !t.is_empty() => t.clone(),
        _ => st.name.clone(),
    };

    // --- match result: host sends op 0x12 [winning team] when the match ends ---
    if !done_end {
        handle_match_end(addr, st, batch)?;
    }

    // --- menu data requests (answered whatever else the batch holds) ---
    if has(0x15) {
        // online-count poll, every 3s on the MP menu
        conn.sendall(&msg(0x16, &(std::cmp::max(1, PRES.count()) as u32).to_le_bytes()))?;
    }
    if has(0x13) {
        // LEADERBOARDS page opened
        conn.sendall(&msg(0x14, &leaderboard_table(10)?))?;
    }

    let ops_s = || py::list_str(&ops.iter().map(|o| hx(*o)).collect::<Vec<_>>());

    // --- decide response by batch content ---
    if has(0x01) || has(0xfc) {
        // MATCHMAKING (host/join / select). ONLY match on a real 64-hex token.
        let realtok: Option<String> = st.token.clone().filter(|t| t.chars().count() == 64);
        if st.closed_sent {
            // FIND MATCH pressed while our boot reply (ending in 0x0d) was on its way: nothing we send now reaches it, so do not queue a ghost
            dlog!("MATCHMAKE", pa(addr), "after 0x0d on this conn (client drops to the title); not queued", match &realtok {
                Some(r) => tag(r),
                None => st.name.clone(),
            });
            return Ok(());
        }
        st.session = true;
        conn.sendall(&searching_frame(realtok.as_deref()))?; // ack -> state 3 (searching) + queue names, NO 0x0d
        let Some(realtok) = realtok else {
            dlog!("MATCHMAKE", pa(addr), format!("ops={} (no token yet, ack only)", ops_s()));
            return Ok(());
        };
        let rt = realtok.as_str();
        if crate::env_str("T2R_STEAM_ONLY", "1") != "0" && !LINUX_CLIENTS.lock().contains(rt) && !(BRIDGES.steamid(rt) != 0 && BRIDGES.port(rt) != 0 && BRIDGES.conn(rt).is_some()) {
            // Steam-only matches (no server relay): a player whose Steam helper isn't registered is not queued; the client (ext) shows why
            let (key, helper) = no_helper_note(rt, &ip);
            let h = helper.filter(|h| !h.is_empty());
            dlog!(
                "MATCHMAKE",
                pa(addr),
                "refused (no Steam helper)",
                tag(rt),
                match &h {
                    Some(h) => format!("- helper {} on this IP: game files changed (substitute token)", pyfmt::str_repr(h)),
                    None => String::new(),
                }
            );
            MM.lock.lock().rq.drop(rt); // (11-12: a refused search ends a requeue ticket - its group does not wait for it)
            conn.sendall(&msg(0x11, &srv_note(key, rt)))?;
            return Ok(());
        }
        if is_banned(rt, Some(&st.name), Some(&ip)) {
            dlog!("MATCHMAKE", pa(addr), "refused (banned)", tag(rt), ident_pid(rt).unwrap_or_default());
            MM.lock.lock().rq.drop(rt);
            conn.sendall(&[msg(0x11, &ban_note(rt)), msg(0x0d, b"\x02")].concat())?; // note, then 0x0d [2]: winmm quits to the title (boat menu)
            return Ok(());
        }
        if let Some((why, note)) = install_refusal(rt, Some(&ip), false) {
            dlog!("MATCHMAKE", pa(addr), "refused (install)", tag(rt), why);
            MM.lock.lock().rq.drop(rt);
            conn.sendall(&[msg(0x11, &note), msg(0x0d, b"\x02")].concat())?;
            return Ok(());
        }
        if let Some(vp) = version_refusal(rt).filter(|v| !v.is_empty()) {
            let cv = CLIENT_VERSIONS.lock().get(rt).cloned();
            dlog!("MATCHMAKE", pa(addr), "refused (version)", tag(rt), on(&cv), "!=", server_version());
            MM.lock.lock().rq.drop(rt);
            let b = vp.as_bytes();
            conn.sendall(&msg(0x11, &b[..std::cmp::min(0xf0, b.len())]))?;
            return Ok(());
        }
        let lh = if MM.in_match(rt) { None } else { loss_hold_note(rt, None) };
        if let Some(lh) = lh.filter(|l| !l.is_empty()) {
            dlog!("MATCHMAKE", pa(addr), "refused (packet loss hold)", tag(rt));
            MM.lock.lock().rq.drop(rt);
            conn.sendall(&msg(0x11, &lh))?;
            return Ok(());
        }
        SESSIONS.register(rt, conn.clone()); // hold conn for match-start push
        PRES.touch(rt, &name_or(rt, &st.name));
        if MM.in_match(rt) && (loaded_has(rt) || MM.match_age(rt) > 120.0) {
            // (or: launched > 2 min ago and never loaded = crashed / killed while loading) searching again although its match already started
            dlog!("MATCHMAKE", pa(addr), "stale match (already started) dropped for", tag(rt));
            LOADED.lock().remove(rt);
            leave_running_match(rt, "searching again");
        }
        if MM.in_match(rt) {
            // re-queued (client auto-cancel) while its match is loading: send the start right here
            if st.started != MM.mid_of(rt) && !loaded_has(rt) {
                conn.sendall(&match_start_frame(rt, &MM.players_of(rt).unwrap_or_default()))?;
                st.started = MM.mid_of(rt);
                dlog!("MATCHMAKE", pa(addr), "already in match; start re-sent", tag(rt));
                if MM.countdown_over(rt) || SESSIONS.conn(rt).is_none() {
                    late_countdown(rt, conn);
                }
            } else {
                dlog!("MATCHMAKE", pa(addr), "already in match; holding");
            }
        } else if PARTIES.pid(rt).is_some_and(|p| !p.is_empty()) && SOCIAL.search_gate(rt, &party_lead_searching) {
            // party rule (user 11-02): a member does not search - his FIND MATCH press is READY UP / UNREADY; not queued; the client pops the search page
            dlog!("MATCHMAKE", pa(addr), "party FIND MATCH not queued (member = ready toggle while the leader is not searching / leader = a mate is not ready)", tag(rt));
        } else {
            let formed = MM.enqueue(rt);
            dlog!("MATCHMAKE", pa(addr), format!("ops={}", ops_s()), "tok", tag(rt), "-> mid", on(&formed.as_ref().map(|f| f.0.clone())));
            mesh_activate(rt); // ping mesh: swap Steam locations with the searchers / in-match players now
            if formed.is_none() && PARTIES.pid(rt).is_some_and(|p| !p.is_empty()) {
                // tells the party mates to press FIND MATCH too
                SOCIAL.on_queue(rt, &|m| {
                    let q = MM.lock.lock().queue.contains_key(m);
                    q || MM.in_match(m)
                });
            }
            if let Some((_mid, players)) = formed {
                push_peers(&players);
                start_match(&players);
            } else if *JIP {
                jip_pass(); // no new match: a running one with room? (jip:)
            }
        }
    } else if has(0x0f) {
        // BOOT handshake
        let tok64 = st.token.clone().filter(|t| t.chars().count() == 64);
        if !st.authed {
            if let Some(t) = &tok64 {
                ident_note(t, 0, Some(&ip)); // 11-23: identity id (Steam id + hardware + IP), ident.py
            }
        }
        if !st.authed {
            if let Some(t) = &tok64 {
                if is_banned(t, Some(&st.name), Some(&ip)) {
                    // FACTIONS II pressed on the boat menu: the client opens the session and logs in. Refuse right here: the note + 0x0d with payload 2
                    let steam = {
                        let b = BRIDGES.steamid(t);
                        if b != 0 {
                            b
                        } else {
                            token_steamid(t)
                        }
                    };
                    let ipb = ip_banned(&ip);
                    dlog!("BOOT", pa(addr), "refused (banned)", tag(t), "steam", steam, ident_pid(t).unwrap_or_default(), match &ipb {
                        Some(n) => format!("ip ban {}", n),
                        None => String::new(),
                    });
                    conn.sendall(&[msg(0x11, &ban_note(t)), msg(0x0d, b"\x02")].concat())?;
                    st.closed_sent = true;
                    return Ok(());
                }
            }
        }
        if !st.authed {
            if let Some(t) = &tok64 {
                // an edited install: refused the same way, before anything is announced / counted online (attest.py)
                if let Some((why, note)) = install_refusal(t, Some(&ip), true) {
                    dlog!("BOOT", pa(addr), "refused (install)", tag(t), why);
                    conn.sendall(&[msg(0x11, &note), msg(0x0d, b"\x02")].concat())?;
                    st.closed_sent = true;
                    return Ok(());
                }
            }
        }
        if !st.authed && has(0x0e) {
            if let Some(out) = session_reattach(conn, addr, st)? {
                st.menu = true;
                st.sent_name = Some(out.sent_name);
                st.started = Some(out.started);
                return Ok(());
            }
        }
        if !st.authed {
            let mut extra: Vec<u8> = Vec::new();
            let shown = name_or(&tok, &st.name);
            if has(0x0e) {
                PRES.touch(&tok, &shown);
                extra = [msg(0x16, &(std::cmp::max(1, PRES.count()) as u32).to_le_bytes()), msg(0x14, &leaderboard_table(10)?)].concat();
            }
            if has(0x0e) && !*ANNOUNCE_CLOSE {
                // MP-menu announce (spec's MM_HELLO): keep this socket OPEN, no 0x0d.
                conn.sendall(&auth_reply_open(&shown, &extra))?; // 0x10+0x08+extra, NO 0x0d
                st.menu = true;
                st.sent_name = Some(shown);
            } else {
                conn.sendall(&auth_reply(&shown, &extra))?; // 0x10+0x08[+extra]+0x0d
                st.closed_sent = true;
            }
            st.authed = true;
        }
        if has(0x0e) {
            // presence announce (NOT queue)
            PRES.touch(&tok, &name_or(&tok, &st.name));
            dlog!("ANNOUNCE", pa(addr), st.name);
        }
    } else if has(0x09) {
        // client "level loaded / ready"
        if let Some(t) = st.token.clone().filter(|t| !t.is_empty()) {
            LOADED.lock().insert(t.clone());
            LOADED_T.lock().insert(t.clone(), clock::now());
            let players = MM.players_of(&t).unwrap_or_default();
            if !players.is_empty() {
                timing_loaded(&players, &tag(&t));
            }
            dlog!("LOADED", pa(addr), tag(&t), "match", if players.is_empty() { "None".to_string() } else { tags(&players) });
            evt!("loaded", on(&MM.mid_of(&t)), tag(&t));
            maybe_game_start(&t)?;
        }
    } else if has(0x05) || has(0x15) {
        // session keepalive / poll
        if let Some(t) = st.token.as_deref().filter(|t| !t.is_empty()) {
            PRES.touch(t, &st.name);
        }
        let tok2: Option<String> = st.token.clone().filter(|t| !t.is_empty());
        if let (true, Some(t2)) = (st.menu, &tok2) {
            let in_m = MM.in_match(t2);
            if !st.session || (!in_m && MM.queued().contains(t2)) {
                // held menu socket: a name changed in-game (0xD2) reaches the client's own-name
                let shown = safe_name(&name_or(t2, &st.name));
                if st.sent_name.as_deref() != Some(shown.as_str()) {
                    let b = shown.as_bytes();
                    let mut f = b[..std::cmp::min(31, b.len())].to_vec();
                    f.resize(32, 0);
                    conn.sendall(&msg(0x10, &f))?;
                    st.sent_name = Some(shown.clone());
                    dlog!("NAME-REFRESH", pa(addr), tag(t2), "->", shown);
                }
            }
        }
        if let (true, Some(t2)) = (st.menu, &tok2) {
            if !MM.in_match(t2) && !MM.queued().contains(t2) {
                // user 10-26: the lobby list shows the party as soon as it exists, before FIND MATCH; T2R_NO_PARTY_LOBBY=1 = off
                if std::env::var("T2R_NO_PARTY_LOBBY").ok().as_deref() != Some("1") {
                    let pn = party_lobby_names(t2);
                    if pn != st.sent_lobby {
                        conn.sendall(&lobby_list_frame(&pn))?;
                        st.sent_lobby = pn.clone();
                        dlog!("LOBBY-PARTY", pa(addr), tag(t2), py::list_str(&pn.iter().map(|x| utf8_ignore(x)).collect::<Vec<_>>()));
                    }
                }
            }
        }
        if st.session && tok2.as_deref().is_some_and(|t| MM.in_match(t)) {
            // matched: push the match-start on THIS (live keepalive) conn, once.
            let t2 = tok2.clone().unwrap_or_default();
            if st.started != MM.mid_of(&t2) {
                conn.sendall(&match_start_frame(&t2, &MM.players_of(&t2).unwrap_or_default()))?;
                st.started = MM.mid_of(&t2);
                dlog!("START-ON-KEEPALIVE", pa(addr), tag(&t2));
                if MM.countdown_over(&t2) || SESSIONS.conn(&t2).is_none() {
                    late_countdown(&t2, conn);
                }
            }
        } else if st.session {
            if let Some(t2) = &tok2 {
                MM.poll(t2); // queue heartbeat
            }
            conn.sendall(&searching_frame(tok2.as_deref()))?; // still searching -> state 3 + queue names
        }
        return Ok(());
    } else if has(0x15) || has(0x07) || has(0x02) {
        // polls - noop (0x02 was handled first)
    } else {
        let hexes: Vec<String> = batch.iter().map(|(_, pl)| py::cut(&pyfmt::hex_sp(pl), 40).to_string()).collect();
        dlog!("SESSION", pa(addr), format!("ops={}", ops_s()), hexes.join(" | "));
    }
    Ok(())
}

/// `threading.Thread(target=f, daemon=True).start()`. (selftest: with TEST_NO_SLEEP set the function runs inline on the caller, so the differential
/// test sees the same order of effects as the Python harness, whose Thread shim also runs the target inline.)
pub fn spawn_t(name: &str, f: impl FnOnce() + Send + 'static) {
    if TEST_NO_SLEEP.load(std::sync::atomic::Ordering::Relaxed) {
        f();
        return;
    }
    let nm = name.to_string();
    let _ = std::thread::Builder::new().name(name.to_string()).stack_size(256 * 1024).spawn(move || {
        crate::heap::set_kind_name(&nm);
        f()
    });
}
