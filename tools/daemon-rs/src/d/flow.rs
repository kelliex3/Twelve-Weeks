//! K3 flow: the match record / roster bytes, session ids, teams, match start and the countdown threads.
//! daemon.py:3996-4296 (countdown_steps ... late_countdown, MATCH_P2P).
use super::*;
use crate::{clock, py};
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::Ordering;
use std::sync::LazyLock;
use std::time::Duration;

fn tag_list(players: &[String]) -> String {
    py::list_str(&players.iter().map(|p| tag(p)).collect::<Vec<_>>())
}

// py: daemon.py:3999 countdown_steps: the 0x0c steps (phase, n) of one match start; `flow` None = on whenever the vote is, or T2R_MATCH_FLOW=1
pub fn countdown_steps_flow(pre: i64, secs: i64, vote: bool, flow: Option<bool>) -> Vec<(i64, i64)> {
    let flow = flow.unwrap_or(vote || match_flow_on());
    let mut steps: Vec<(i64, i64)> = vec![(1, 0)];
    steps.extend((1..=pre).rev().map(|n| (2, n)));
    if vote {
        steps.extend((1..=*VOTE_SECS).rev().map(|n| (2, 160 + n)));
    }
    if flow {
        let (lo, bo, ld) = flow_stage_secs(Some(secs));
        steps.extend((1..=lo).rev().map(|n| (2, 100 + n)));
        steps.extend((1..=bo).rev().map(|n| (2, 130 + n)));
        steps.extend((1..=ld).rev().map(|n| (2, 200 + n)));
    } else {
        steps.extend((2..=secs).rev().map(|n| (2, 100 + n)));
    }
    steps.push((2, 1));
    steps.push((2, 0));
    steps
}
pub fn countdown_steps(pre: i64, secs: i64, with_vote: bool) -> Vec<(i64, i64)> {
    countdown_steps_flow(pre, secs, with_vote, None)
}

// py: daemon.py:4018 countdown_seconds: length of one whole match-start countdown in seconds (the 2 s pause before it included)
pub fn countdown_seconds_with(pre: Option<i64>, secs: Option<i64>) -> f64 {
    let pre = pre.unwrap_or_else(|| env_int("T2R_PRE_COUNTDOWN", 5));
    let secs = secs.unwrap_or_else(|| env_int("T2R_COUNTDOWN", 10));
    2.0 + countdown_steps(pre, secs, map_vote_on()).len() as f64
}
pub fn countdown_seconds() -> f64 {
    if WORLD_ON.load(Ordering::Relaxed) {
        return 10.0;
    }
    countdown_seconds_with(None, None)
}

/// daemon.py:4026 SID_KEEP: frozenset(match tokens) -> session id kept from before a join-in-progress grew / changed the player list
pub static SID_KEEP: LazyLock<Mutex<IndexMap<Key, u32>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// `SID_KEEP[key] = sid`
pub fn sid_keep_set(key: &Key, sid: u32) {
    SID_KEEP.lock().insert(key.clone(), sid);
}

// py: daemon.py:4028 match_sid: shared non-zero u32 match session id (a hash of the player list; a join-in-progress pins the id the running players hold)
pub fn match_sid(players: &[String]) -> u32 {
    if let Some(k) = SID_KEEP.lock().get(&key_of(players)).copied() {
        if k != 0 {
            return k;
        }
    }
    let s: String = players.concat();
    let c = t2r_clanstory::crc::crc32(s.as_bytes());
    if c == 0 {
        1
    } else {
        c
    }
}

// py: daemon.py:4038 roster_team: TEST ONLY: T2R_TEST_ONE_TEAM puts everyone on team 0
pub fn roster_team(slot: i64) -> i64 {
    if env_set("T2R_TEST_ONE_TEAM") {
        0
    } else {
        team_of(slot)
    }
}

// py: daemon.py:4042 team_of: alternate teams: slot 0,2 -> team 0; 1,3 -> team 1
pub fn team_of(slot: i64) -> i64 {
    py::pymod(slot, 2)
}

/// daemon.py:4051 TEAM_SWAP: mid -> {token: team}; the admin's in-match team swaps
pub static TEAM_SWAP: LazyLock<Mutex<IndexMap<String, HashMap<String, i64>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub const TS_CAP_S: f64 = 30.0;
/// daemon.py:4053 TS_CAP: token -> time its t2r-ext last said it applies the 0xE5 team-swap block
pub static TS_CAP: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:4055 team_in: team of `slot` in match `mid`: the admin's in-match swap for the token in that slot, else the slot parity.
// With `tok` None the token is read from MM.matches (as the Python does, without taking the lock when this thread holds it).
pub fn team_in(mid: &str, slot: i64, tok: Option<&str>) -> i64 {
    let sw = TEAM_SWAP.lock().get(mid).cloned();
    if let Some(sw) = sw.filter(|s| !s.is_empty()) {
        let t: Option<String> = match tok {
            Some(t) => Some(t.to_string()),
            None => MM.lock.peek(|g| g.matches.get(mid).and_then(|pl| usize::try_from(slot).ok().and_then(|i| pl.get(i).cloned()))),
        };
        if let Some(v) = t.and_then(|t| sw.get(&t).copied()) {
            return v;
        }
    }
    team_of(slot)
}

// py: daemon.py:4066 roster_team_in: roster_team with the in-match swap (T2R_TEST_ONE_TEAM still wins)
pub fn roster_team_in(mid: &str, slot: i64, tok: Option<&str>) -> i64 {
    if env_set("T2R_TEST_ONE_TEAM") {
        0
    } else {
        team_in(mid, slot, tok)
    }
}

/// `str.encode("latin1", "ignore")`
fn latin1_ignore(s: &str) -> Vec<u8> {
    s.chars().filter(|c| (*c as u32) < 256).map(|c| c as u32 as u8).collect()
}

// py: daemon.py:4069 build_player_record: the 0x04 match record (server -> client)
pub fn build_player_record(slot: i64, sid: u32, map_idx: i64, roster: &[(i64, String)], relay: Option<(String, i64)>, team_fn: Option<&dyn Fn(i64) -> i64>) -> Vec<u8> {
    let n = std::cmp::max(1, roster.len());
    let mut p = vec![0u8; 0x1b + n * 0x32 + 8];
    let (ip, port): (Vec<u8>, i64) = match relay {
        Some((ip, port)) => (ip.into_bytes(), port), // Steam P2P: the local helper
        None => (crate::env_str("T2R_PUB_IP", "209.25.141.16").into_bytes(), env_int("T2R_PUB_UDP_PORT", 15389)), // gameplay relay
    };
    let l = ip.len().min(p.len());
    p[..l].copy_from_slice(&ip[..l]);
    p[0x10..0x12].copy_from_slice(&(port as u16).to_le_bytes());
    p[0x12..0x16].copy_from_slice(&sid.to_le_bytes());
    p[0x16] = map_idx as u8;
    p[0x17] = slot as u8;
    p[0x18] = n as u8;
    let rteam = |s: i64| -> i64 {
        match team_fn {
            Some(f) => f(s),
            None => roster_team(s),
        }
    };
    p[0x1a] = rteam(slot) as u8;
    if env_set("T2R_TEST_SAME_SPAWN") || env_set("T2R_TEST_ONE_TEAM") {
        p[0x1a] = 0; // TEST ONLY: team-0 for everyone
    }
    for (i, (rslot, name)) in roster.iter().enumerate() {
        let e = 0x1b + i * 0x32;
        p[e] = *rslot as u8;
        let nm = latin1_ignore(name);
        let nm = &nm[..nm.len().min(0x2e)];
        p[e + 1..e + 1 + nm.len()].copy_from_slice(nm);
        p[e + 0x31] = rteam(*rslot) as u8;
    }
    p
}

// py: daemon.py:4106 build_peer_table: [u8 count][count x 0x30 record]
pub fn build_peer_table(players: &[String]) -> Vec<u8> {
    let n = players.len().min(10);
    let mut body = vec![n as u8];
    for tok in &players[..n] {
        let g = get_name(tok);
        let nm = latin1_ignore(if g.is_empty() { "PLAYER" } else { &g });
        let nm = &nm[..nm.len().min(31)];
        let mut rec = [0u8; 0x30];
        rec[..nm.len()].copy_from_slice(nm);
        body.extend_from_slice(&rec);
    }
    body.push(0); // +1 so len > count*0x30
    body
}

/// daemon.py:4288 MATCH_P2P: frozenset(players) -> True when gameplay rides Steam P2P
pub static MATCH_P2P: LazyLock<Mutex<IndexMap<Key, bool>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// `MATCH_P2P.get(key)`: None = unknown
pub fn match_p2p_get(key: &Key) -> Option<bool> {
    MATCH_P2P.lock().get(key).copied()
}

// py: daemon.py:4120 match_start_frame: match-start bytes: 0x04 match record (slot 0 = host) + optional 0x11 notification
pub fn match_start_frame(tok: &str, players: &[String]) -> Vec<u8> {
    let owned: Vec<String>;
    let players: &[String] = if players.is_empty() {
        owned = vec![tok.to_string()];
        &owned
    } else {
        players
    };
    let slot = players.iter().position(|t| t == tok).unwrap_or(0) as i64;
    // a host-migration placeholder holds a slot nobody plays - it is not a roster entry
    let roster: Vec<(i64, String)> = players
        .iter()
        .enumerate()
        .filter(|(_, t)| !is_placeholder(t))
        .map(|(i, t)| {
            let g = get_name(t);
            (i as i64, if g.is_empty() { format!("PLAYER {}", i + 1) } else { g })
        })
        .collect();
    let relay = if match_p2p_get(&key_of(players)).unwrap_or(false) { Some(("127.0.0.1".to_string(), BRIDGES.port(tok))) } else { None };
    let mid = if players.iter().any(|t| t == tok) { MM.mid_of(tok) } else { None };
    let swapped = mid.as_ref().is_some_and(|m| TEAM_SWAP.lock().get(m).is_some_and(|s| !s.is_empty()));
    let rec = if swapped {
        let mid = mid.clone().unwrap_or_default();
        let tf = |sl: i64| roster_team_in(&mid, sl, usize::try_from(sl).ok().and_then(|i| players.get(i)).map(|s| s.as_str()));
        build_player_record(slot, match_sid(players), map_index(players), &roster, relay, Some(&tf))
    } else {
        build_player_record(slot, match_sid(players), map_index(players), &roster, relay, None)
    };
    let mut frame = msg(0x04, &rec);
    // 0x11 = notification text (shown via FUN_18000bc00), NOT a level loader.
    let note = crate::env_str("T2R_NOTE", "");
    if !note.is_empty() {
        let b = note.as_bytes();
        frame.extend(msg(0x11, &b[..b.len().min(0xf0)]));
    }
    frame
}

// ---------------------------------------------------------------- load timing (user 10-07)
/// one `TIMING[key]` dict
#[derive(Clone, Debug, Default)]
pub struct Timing {
    pub start: f64,
    pub marks: IndexMap<String, f64>,
    pub loaded: IndexMap<String, f64>,
}
/// daemon.py:4138 TIMING: per match (frozenset of players) the stage marks, one summary line at GAME-START
pub static TIMING: LazyLock<Mutex<IndexMap<Key, Timing>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));

// py: daemon.py:4140 stage_name
pub fn stage_name(phase: i64, n: i64) -> &'static str {
    if phase == 1 {
        return "match found";
    }
    if n == 0 {
        return "level start";
    }
    if n == 1 {
        return "horn";
    }
    if n >= 200 {
        return "loading";
    }
    if n >= 160 {
        return "vote";
    }
    if n >= 130 {
        return "booster";
    }
    if n >= 100 {
        return "loadout";
    }
    "pre-countdown"
}

// py: daemon.py:4150 timing_mark: false = no timing record for this match
pub fn timing_mark(players: &[String], what: &str) -> bool {
    let mut g = TIMING.lock();
    let Some(t) = g.get_mut(&key_of(players)) else { return false };
    let now = clock::now();
    if !t.marks.contains_key(what) {
        t.marks.insert(what.to_string(), now);
        dlog!("load timing:", what, format!("at +{} s", py::fixed(now - t.start, 1)));
    }
    true
}

/// `TIMING.get(key)["loaded"][tag(t)] = now` when not yet there (the handler of op 0x09)
pub fn timing_loaded(players: &[String], tagged: &str) {
    let mut g = TIMING.lock();
    if let Some(tm) = g.get_mut(&key_of(players)) {
        if !tm.loaded.contains_key(tagged) {
            tm.loaded.insert(tagged.to_string(), clock::now());
        }
    }
}

// py: daemon.py:4159 timing_summary
pub fn timing_summary(players: &[String]) {
    let Some(t) = TIMING.lock().shift_remove(&key_of(players)) else { return };
    let gs = clock::now();
    let ls = t.marks.get("level start").copied().unwrap_or(gs);
    let first = t.loaded.values().copied().fold(None::<f64>, |a, v| Some(a.map_or(v, |x| x.min(v)))).unwrap_or(gs);
    let mut ms: Vec<(&String, &f64)> = t.marks.iter().collect();
    ms.sort_by(|a, b| a.1.partial_cmp(b.1).unwrap_or(std::cmp::Ordering::Equal));
    let stages = ms.iter().map(|(k, v)| format!("{} +{}", k, py::fixed(**v - t.start, 1))).collect::<Vec<_>>().join(", ");
    let loaded = t.loaded.iter().map(|(k, v)| format!("{} +{}", k, py::fixed(*v - ls, 1))).collect::<Vec<_>>().join(", ");
    dlog!(format!(
        "load timing: flow {} s, level load {} s, wait for others {} s (total {} s) | stages: {} | loaded after level start: {}",
        py::fixed(ls - t.start, 1),
        py::fixed(first - ls, 1),
        py::fixed(gs - first, 1),
        py::fixed(gs - t.start, 1),
        stages,
        loaded
    ));
}

/// a raw pair as `frozenset((a, b))` (unordered)
pub type PairKey = (String, String);
pub fn pair_key(a: &str, b: &str) -> PairKey {
    if a <= b {
        (a.to_string(), b.to_string())
    } else {
        (b.to_string(), a.to_string())
    }
}
/// daemon.py:4442 PING_FORMED: mid -> {pair: the raw estimate at formation}
pub static PING_FORMED: LazyLock<Mutex<IndexMap<Option<String>, HashMap<PairKey, i64>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));

// py: daemon.py:4170 start_match: push the match-start to every player's held session conn (best-effort)
pub fn start_match(players: &[String]) {
    if players.is_empty() {
        return;
    }
    let key = key_of(players);
    TIMING.lock().insert(key.clone(), Timing { start: clock::now(), marks: IndexMap::new(), loaded: IndexMap::new() });
    let mut pings = serde_json::Map::new();
    for (i, a) in players.iter().enumerate() {
        for b in &players[i + 1..] {
            if let Some(ms) = pair_ping(a, b) {
                pings.insert(format!("{}<>{}", tag(a), tag(b)), serde_json::json!(ms));
            }
        }
    }
    {
        let mid0 = MM.mid_of(&players[0]);
        let mut formed: HashMap<PairKey, i64> = HashMap::new();
        for (i, a) in players.iter().enumerate() {
            for b in &players[i + 1..] {
                if let Some(v) = pair_raw(a, b) {
                    formed.insert(pair_key(a, b), v);
                }
            }
        }
        let mut pf = PING_FORMED.lock();
        pf.insert(mid0, formed);
        while pf.len() > 256 {
            pf.shift_remove_index(0);
        }
    }
    let mid0 = MM.mid_of(&players[0]);
    let p2p = match_p2p_get(&key).unwrap_or(false);
    let mut cv = serde_json::Map::new();
    for t in players {
        cv.insert(tag(t), serde_json::Value::String(client_version(t).unwrap_or_else(|| "?".to_string())));
    }
    let pings_s = if pings.is_empty() { "unknown".to_string() } else { py::pyrepr(&serde_json::Value::Object(pings)) };
    evt!(
        "match-formed",
        mid0.clone().unwrap_or_else(|| "None".to_string()),
        "players",
        tag_list(players),
        "host",
        tag(&players[0]),
        "net",
        if p2p { "steam-p2p" } else { "server-relay" },
        "ping-ms",
        pings_s,
        "client-versions",
        py::pyrepr(&serde_json::Value::Object(cv))
    );
    dlog!("load timing: START-MATCH sent", tag_list(players));
    MATCH_MAP.lock().shift_remove(&key); // pick this match's map afresh
    VOTES.lock().shift_remove(&key);
    let pmid = MM.mid_of(&players[0]);
    match pmid.filter(|m| PRIV.is_private_mid(m)) {
        Some(pmid) => {
            // private match (11-17): the lobby's map, a random pool map, or a vote over the lobby's pool
            let (idx, opts) = priv_pick_map(&pmid, players);
            {
                let mut v = VOTES.lock();
                if !opts.is_empty() {
                    v.insert(key.clone(), Vote { opts: opts.clone(), votes: IndexMap::new(), winner: -1 });
                }
                MATCH_MAP.lock().insert(key.clone(), idx);
            }
            dlog!(
                "MAP (private)",
                map_level(idx),
                if opts.is_empty() { "chosen / random".to_string() } else { format!("vote over {} maps", opts.len()) },
                tag_list(players)
            );
        }
        None => {
            if map_vote_on() {
                vote_start(players); // placeholder map now, the winner at the vote's end
            }
        }
    }
    map_index(players);
    for tok in players {
        LOADED.lock().remove(tok); // fresh match: clear any old loaded flag
        let Some(conn) = SESSIONS.conn(tok) else {
            dlog!("START-MATCH", tag(tok), "no session conn (will push on keepalive)");
            continue;
        };
        match conn.sendall(&match_start_frame(tok, players)) {
            Ok(()) => dlog!("START-MATCH", tag(tok), "sent on session conn"),
            Err(e) => dlog!("START-MATCH ERR", tag(tok), crate::pyfmt::os_error_repr(&e)),
        }
    }
    if !env_set("T2R_NO_COUNTDOWN") {
        let pl = players.to_vec();
        let _ = std::thread::Builder::new().name("countdown".into()).stack_size(256 * 1024).spawn(move || countdown(&pl));
    }
}

/// (selftest) `time.sleep` is a no-op in the harness
pub static TEST_NO_SLEEP: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
fn nap(secs: f64) {
    if !TEST_NO_SLEEP.load(Ordering::Relaxed) {
        std::thread::sleep(Duration::from_secs_f64(secs));
    }
}

// py: daemon.py:4221 countdown: thread entry
pub fn countdown(players: &[String]) {
    countdown_run(players);
}

// py: daemon.py:4228 _countdown_run: drive the client's match page-state (0x189bc0) like the original server (op 0x0c [phase][seconds])
pub fn countdown_run(players: &[String]) {
    let pre = env_int("T2R_PRE_COUNTDOWN", 5);
    let secs = env_int("T2R_COUNTDOWN", 10); // CHOOSE LOADOUT window
    nap(2.0); // let 0x04 land (start + keepalive re-push)
    let key = key_of(players);
    let steps = countdown_steps(pre, secs, VOTES.lock().contains_key(&key));
    {
        let mut cd = CD_GOT.lock();
        for t in players {
            cd.remove(t);
        }
    }
    let mut in_vote = false;
    for (phase, n) in steps {
        if MM.players_of(&players[0]).as_deref() != Some(players) {
            dlog!("COUNTDOWN aborted (match cancelled)", tag_list(players));
            VOTES.lock().shift_remove(&key);
            return;
        }
        let mut resend = false;
        if phase == 2 && (160..200).contains(&n) {
            in_vote = true;
        } else if in_vote {
            in_vote = false;
            vote_close(players); // the vote window is over
            resend = true; // the match record again, now with the winning map
        }
        timing_mark(players, stage_name(phase, n));
        if phase == 2 && n == 0 {
            MM.mark_launched(players);
        }
        for tok in players {
            if let Some(c) = SESSIONS.conn(tok) {
                // the re-sent 0x04 (phase reset) and this step's 0x0c (phase 2 again) go in ONE segment
                let mut head = if resend { match_start_frame(tok, players) } else { Vec::new() };
                let drop_ok = head.is_empty();
                head.extend(msg(0x0c, &[phase as u8, n as u8]));
                if push(&c, &head, drop_ok).is_ok() && phase == 2 && n == 0 {
                    CD_GOT.lock().insert(tok.clone());
                }
            }
        }
        if resend {
            dlog!("COUNTDOWN", "match record re-sent with the voted map", map_level(map_index(players)), tag_list(players));
        }
        dlog!("COUNTDOWN", format!("phase={} n={}", phase, n), tag_list(players));
        nap(1.0);
    }
}

/// daemon.py:4284 CD_GOT: tokens that received the final countdown step (level start) for their match
pub static CD_GOT: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));

// py: daemon.py:4286 late_countdown: a player whose start went out late (no session conn when the match formed) missed the shared countdown
pub fn late_countdown(tok: &str, conn: &Conn) {
    if CD_GOT.lock().contains(tok) || MM.lock.peek(|g| g.joining.contains_key(tok)) {
        return; // (a join-in-progress player has its own countdown: jip_countdown)
    }
    let (tok, conn) = (tok.to_string(), conn.clone());
    spawn_t("late-countdown", move || {
        nap(1.0);
        for (phase, n) in [(1u8, 0u8), (2, 3), (2, 2), (2, 1), (2, 0)] {
            if LOADED.lock().contains(&tok) || !MM.in_match(&tok) {
                return;
            }
            if push(&conn, &msg(0x0c, &[phase, n]), true).is_err() {
                return;
            }
            nap(1.0);
        }
        CD_GOT.lock().insert(tok.clone());
        dlog!("COUNTDOWN late", tag(&tok));
    });
}

// ---------------------------------------------------------------- per-match dictionaries
/// `for d in (MATCH_MAP, MATCH_P2P, VOTES, TIMING): if old in d and old != new: d[new] = d.pop(old)` (jip_join, MM.lock held)
pub fn rekey_match_dicts(old: &Key, new: &Key) {
    if old == new {
        return;
    }
    fn mv<V>(m: &Mutex<IndexMap<Key, V>>, old: &Key, new: &Key) {
        let mut g = m.lock();
        if let Some(v) = g.shift_remove(old) {
            g.insert(new.clone(), v);
        }
    }
    mv(&MATCH_MAP, old, new);
    mv(&MATCH_P2P, old, new);
    mv(&VOTES, old, new);
    mv(&TIMING, old, new);
}

/// `VOTES.pop(key) (under _vote_lock); MATCH_MAP.pop(key); TIMING.pop(key); MATCH_P2P.pop(key); SID_KEEP.pop(key)` (forget_match_state)
pub fn match_dicts_pop(key: &Key) {
    VOTES.lock().shift_remove(key);
    MATCH_MAP.lock().shift_remove(key);
    TIMING.lock().shift_remove(key);
    MATCH_P2P.lock().shift_remove(key);
    SID_KEEP.lock().shift_remove(key);
}
