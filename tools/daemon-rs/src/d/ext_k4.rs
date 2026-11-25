// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K4 (mm / teams / jip) chunk calls, with the signatures it
// expects. Bodies are `unimplemented!()` (the K4 selftest swaps the few it needs for scripted tables, see k4_selftest.rs).
#![allow(dead_code, unused_variables)]
use super::*;
use crate::clock;
use parking_lot::Mutex;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;

/// `frozenset(players)` as a dictionary key.
pub type Key = BTreeSet<String>;
pub fn key_of(pl: &[String]) -> Key {
    pl.iter().cloned().collect()
}

// ---- K1 base: config (daemon.py:149-310), all read once from the environment (defaults as in daemon.py)
macro_rules! cfg_f {
    ($($n:ident = $env:expr, $d:expr;)*) => { $( pub static $n: LazyLock<f64> = LazyLock::new(|| crate::env_parse($env, $d)); )* };
}
macro_rules! cfg_on {
    ($($n:ident = $env:expr;)*) => { $( pub static $n: LazyLock<bool> = LazyLock::new(|| crate::env_on($env, true)); )* };
}
cfg_f!(KEEPALIVE_TTL = "T2R_UNUSED_KEEPALIVE_TTL", 15.0; FILL_WAIT = "T2R_FILL_WAIT", 15.0; JIP_MIN_AGE = "T2R_JIP_MIN_AGE", 15.0;
    JIP_MIN_AGE_REMNANT = "T2R_JIP_MIN_AGE_REMNANT", 120.0; JIP_MIN_LEFT = "T2R_JIP_MIN_LEFT", 90.0; JIP_CLOCK_TTL = "T2R_JIP_CLOCK_TTL", 12.0;
    JIP_REUSE_S = "T2R_JIP_REUSE_S", 10.0; JIP_PREFER_LEFT = "T2R_JIP_PREFER_LEFT", 180.0; JIP_ACK_STALE = "T2R_JIP_ACK_STALE", 12.0;
    REQUEUE_WAIT_MAX = "T2R_REQUEUE_WAIT_MAX", 90.0; REQUEUE_TICKET_S = "T2R_REQUEUE_TICKET_S", 300.0; REQUEUE_OFFLINE_S = "T2R_REQUEUE_OFFLINE_S", 30.0;
    REQUEUE_CAP_S = "T2R_UNUSED_REQUEUE_CAP_S", 60.0; LOBBY_RELEASE_S = "T2R_LOBBY_RELEASE_S", 30.0; LOBBY_MIN_GAP_S = "T2R_LOBBY_MIN_GAP_S", 10.0;
    AFK_KICK_S = "T2R_AFK_KICK_S", 90.0; MM_REGION_WAIT_S = "T2R_MM_REGION_WAIT_S", 90.0; QUEUE_HOLD_MAX = "T2R_QUEUE_HOLD_MAX", 75.0;
    PARTY_HOLD_S = "T2R_PARTY_HOLD_S", 120.0; MAX_WAIT = "T2R_MAX_WAIT", 150.0; FAST_STEP = "T2R_FAST_STEP", 8.0; FAST_MAX_WAIT = "T2R_FAST_MAX_WAIT", 40.0;
    FAST_ALONE = "T2R_FAST_ALONE", 15.0; JIP_ACK_MAX = "T2R_JIP_ACK_MAX", 25.0; QUEUE_SETTLE = "T2R_QUEUE_SETTLE", 3.0; QUEUE_FRESH = "T2R_QUEUE_FRESH", 5.0;
    PING_WAIT_S = "T2R_PING_WAIT_S", 10.0;);
cfg_on!(JIP_HOST_CAP = "T2R_JIP_HOST_CAP"; WAVE_RESPAWN = "T2R_WAVE_RESPAWN"; REQUEUE = "T2R_REQUEUE"; LOBBY_KEEP = "T2R_LOBBY_KEEP";
    LOBBY_BOOK = "T2R_LOBBY_BOOK"; MM_REGION_ON = "T2R_MM_REGION"; MM_PING_GATE = "T2R_MM_PING_GATE";);
pub static JIP: LazyLock<bool> = LazyLock::new(|| crate::env_is1("T2R_JIP", true));
pub static ENGINE_SLOTS: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_ENGINE_SLOTS", 8i64));
pub static MATCH_SIZE: LazyLock<i64> = LazyLock::new(|| std::cmp::min(crate::env_parse("T2R_MATCH_SIZE", 8i64), *ENGINE_SLOTS));
pub static MIN_PLAYERS: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_MIN_PLAYERS", std::cmp::min(2, *MATCH_SIZE)));
pub static LIVES_START: LazyLock<i64> = LazyLock::new(|| if crate::env_str("T2R_TEST_LIVES2", "") == "1" { 40 } else { 20 });
pub static SECOND_MATCH_MIN: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_SECOND_MATCH_MIN", 6i64));
pub static RELAY_MATCH_MAX: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_RELAY_MAX", 4i64));
pub static PING_KICK_MS: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_PING_KICK_MS", 190i64));
pub fn book_on() -> bool {
    *LOBBY_BOOK && *LOBBY_KEEP
}
pub fn fill_rule() -> String {
    crate::env_str("T2R_FILL_RULE", "fast").trim().to_lowercase()
}
/// daemon.py:226 RQ_CAP: token -> time of its last 0xE3 report with flag 0x20
pub static RQ_CAP: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:236 PING_KICKED: (token, mid) removed for ping
pub static PING_KICKED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// The scripted world of the K4 selftest (names, Steam ids, pings, host rules); the stubs below read it.
#[derive(Default)]
pub struct World {
    pub names: HashMap<String, String>,
    pub sids: HashMap<String, u64>,
    pub pings: HashMap<(String, String), i64>,
    pub noh: HashSet<String>,
    pub bho: Vec<String>,
    pub perf: HashMap<String, i64>,
    pub lim: i64,
    pub online: Vec<String>,
    pub regions: HashMap<String, String>,
}
/// (selftest) the match map of running matches: frozenset(players) -> map index
pub static TEST_MM: LazyLock<Mutex<HashMap<Key, i64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static WORLD: LazyLock<Mutex<World>> = LazyLock::new(|| Mutex::new(World::default()));
/// `get_name(token)`: "" = none (Python None)
pub fn get_name(token: &str) -> String {
    WORLD.lock().names.get(token).cloned().unwrap_or_default()
}
pub fn token_steamid(token: &str) -> u64 {
    WORLD.lock().sids.get(token).copied().unwrap_or(0)
}
pub fn silence(token: &str, now: f64) -> f64 {
    0.0
}
pub fn requeue_keep_mode(token: &str, mid: &str) {

}
pub fn readopt_claim(token: &str) {
    unimplemented!()
}
pub fn afk_report(token: &str, idle_s: i64, flags: u8) {
    unimplemented!()
}
pub fn shutdown_tick() {
    unimplemented!()
}
pub fn admin_kick(pid: &str, move_: bool) -> serde_json::Value {
    unimplemented!()
}
pub fn match_start_frame(token: &str, players: &[String]) -> Vec<u8> {
    unimplemented!()
}
pub fn countdown_steps(pre: i64, secs: i64, with_vote: bool) -> Vec<(i64, i64)> {
    unimplemented!()
}
pub fn push_peers(players: &[String]) {
    unimplemented!()
}
pub fn start_match(players: &[String]) {
    unimplemented!()
}
pub fn _search_mode(token: &str) -> i64 {
    MODEQ.get(token, PARTIES.leader(token).as_deref())
}
pub fn has_helper(token: &str) -> bool {
    false
}
/// daemon.py:7559 MODEQ
pub static MODEQ: LazyLock<crate::modeq::ModeQ> = LazyLock::new(crate::modeq::ModeQ::new);
/// daemon.py:7562 PRIV (private_lobby.Lobbies); T2R_PRIVATE=0 stub: nothing is ever private
pub struct PrivLobby {
    pub rules: serde_json::Value,
    pub code: String,
    pub members: Vec<String>,
}
pub struct PrivStub;
impl PrivStub {
    pub fn on_enqueue(&self, token: &str) {}
    pub fn split_ready(&self, ready: &[String]) -> (Vec<String>, Vec<String>) {
        (ready.to_vec(), Vec::new())
    }
    pub fn launches(&self, priv_ready: &[String]) -> Vec<(PrivLobby, Vec<String>)> {
        Vec::new()
    }
    pub fn teams_for(&self, lb: &PrivLobby, have: &[String], dflt: &dyn Fn(&[String]) -> (Vec<String>, Vec<String>)) -> (Vec<String>, Vec<String>) {
        dflt(have)
    }
    pub fn bound(&self, lb: &PrivLobby, mid: &str, players: &[String]) {}
    pub fn match_over(&self, mid: &str) {}
    pub fn aborted(&self, mid: &str) {}
    pub fn jip_ok(&self, token: &str, mid: &str) -> bool {
        true
    }
    pub fn gc(&self, keep: &dyn Fn(&str) -> bool) {}
    pub fn rules_of_mid(&self, mid: &str) -> Option<serde_json::Value> {
        None
    }
    /// `PRIV.rule(mid, key)`: the rule value, None = not a private match
    pub fn rule(&self, mid: &str, key: &str) -> Option<i64> {
        None
    }
}
pub static PRIV: PrivStub = PrivStub;

// ---- K1 / K2: clan bookkeeping that clan_rejoin_after_booking touches
pub static CLAN_DONE: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_CREDITED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static CLAN_EXCUSED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static CLAN_QUITTERS: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static CLAN_EXCUSE_WHY: LazyLock<Mutex<HashMap<(String, String), String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_LEFT_AT: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_REJOINED: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_MOVED: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_REBOOK: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:1545 CLAN_TEAM: mid -> {token: team}
pub static CLAN_TEAM: LazyLock<Mutex<HashMap<String, HashMap<String, i64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub fn clan_key(c: &rusqlite::Connection, token: &str) -> String {
    unimplemented!()
}
/// `clan.snap_update(c, key, mid, rejoined=True)`
pub fn clan_snap_rejoined(c: &rusqlite::Connection, key: &str, mid: &str) -> Result<(), String> {
    unimplemented!()
}

// ---- K3 sess / maps: presence, bridges, sessions, per-match tables
pub struct PresStub;
impl PresStub {
    /// `[(token, p.get("last", 0))]` of every `PRES.players` entry, one lock pass
    pub fn players_last(&self) -> Vec<(String, f64)> {
        WORLD.lock().online.iter().map(|t| (t.clone(), clock::now())).collect()
    }
    pub fn touch_known(&self, token: &str) {
        unimplemented!()
    }
}
pub static PRES: PresStub = PresStub;
pub static LOADED: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static CD_GOT: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static MATCH_P1: LazyLock<Mutex<indexmap::IndexMap<String, bool>>> = LazyLock::new(|| Mutex::new(indexmap::IndexMap::new()));
pub static TEAM_SWAP: LazyLock<Mutex<indexmap::IndexMap<String, HashMap<String, i64>>>> = LazyLock::new(|| Mutex::new(indexmap::IndexMap::new()));
pub const TS_CAP_S: f64 = 30.0;
pub static TS_CAP: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static PING_AT: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub fn team_of(slot: i64) -> i64 {
    slot.rem_euclid(2)
}
/// `team_in(mid, slot, tok)`: never takes MM.lock when `tok` is Some (callers hold it)
pub fn team_in(mid: &str, slot: i64, tok: Option<&str>) -> i64 {
    if let Some(t) = tok {
        if let Some(v) = TEAM_SWAP.lock().get(mid).and_then(|m| m.get(t)) {
            return *v;
        }
    }
    team_of(slot)
}
pub fn roster_team_in(mid: &str, slot: i64, tok: Option<&str>) -> i64 {
    if std::env::var("T2R_TEST_ONE_TEAM").is_ok_and(|v| !v.is_empty()) {
        0
    } else {
        team_in(mid, slot, tok)
    }
}
pub fn match_sid(players: &[String]) -> u32 {
    1
}
/// `SID_KEEP[key] = sid`
pub fn sid_keep_set(key: &Key, sid: u32) {

}
/// `for d in (MATCH_MAP, MATCH_P2P, VOTES, TIMING): if old in d and old != new: d[new] = d.pop(old)`
pub fn rekey_match_dicts(old: &Key, new: &Key) {
    let mut t = TEST_MM.lock();
    if old != new {
        if let Some(v) = t.remove(old) {
            t.insert(new.clone(), v);
        }
    }
}
pub fn match_map_get(key: &Key) -> Option<i64> {
    TEST_MM.lock().get(key).copied()
}
/// `MATCH_P2P.get(key)`: None = unknown
pub fn match_p2p_get(key: &Key) -> Option<bool> {
    None
}
pub fn map_name(idx: i64) -> String {
    format!("map{}", idx)
}
pub fn map_pool(players: &[String], support: Option<&[String]>) -> Vec<i64> {
    (0..40).collect()
}
pub fn p1_pref_on() -> bool {
    false
}
pub fn p1_always(token: &str, leader: Option<&str>) -> bool {
    false
}
pub fn p1_owner(token: &str, leader: Option<&str>) -> bool {
    false
}
pub fn p1_fits(token: &str, mid: &str, leader: Option<&str>) -> bool {
    true
}
pub fn countdown_seconds() -> f64 {
    10.0
}
pub fn ping_match_ms() -> i64 {
    WORLD.lock().lim
}
pub fn pair_ping(a: &str, b: &str) -> Option<i64> {
    WORLD.lock().pings.get(&(a.to_string(), b.to_string())).copied()
}
pub fn perf_penalty(token: &str) -> i64 {
    WORLD.lock().perf.get(token).copied().unwrap_or(0)
}
pub fn no_host(token: &str) -> bool {
    WORLD.lock().noh.contains(token)
}
pub fn host_allowed(tokens: &[String]) -> Vec<String> {
    let ok: Vec<String> = tokens.iter().filter(|t| !no_host(t)).cloned().collect();
    if ok.is_empty() {
        tokens.to_vec()
    } else {
        ok
    }
}
pub fn host_pool(tokens: &[String]) -> (Vec<String>, String) {
    unimplemented!()
}
/// (selftest world: the scripted order first, the rest as given)
pub fn best_host_order(tokens: &[String]) -> Vec<String> {
    let w = WORLD.lock();
    let mut v: Vec<String> = w.bho.iter().filter(|t| tokens.contains(*t)).cloned().collect();
    v.extend(tokens.iter().filter(|t| !w.bho.contains(*t)).cloned());
    v
}
pub fn mm_region(token: &str) -> Option<String> {
    WORLD.lock().regions.get(token).cloned()
}
pub fn jip_host_verdict(host: &str, n: i64) -> (i64, String) {
    unimplemented!()
}
/// `jip_ping_block(token, mid, host, active, joining, now)`: the ping in ms when the joiner is over the limit to the host, else None
pub fn jip_ping_block(token: &str, mid: &str, host: &str, active: &[String], joining: &HashSet<String>, now: f64) -> Option<i64> {
    None
}
pub fn _ping_gate(a: &str, b: &str, now: f64) -> Option<i64> {
    pair_ping(a, b)
}
pub fn ping_left_out_note(dropped: &[String], now: f64) {

}
pub fn ping_wait_judge(kept: &[String], host: Option<&str>, pair: &dyn Fn(&str, &str) -> Option<i64>, now: f64,
    members_fn: &dyn Fn(&str) -> Vec<String>, host_ok: &dyn Fn(&str) -> bool) -> (bool, Vec<String>) {
    (false, Vec::new())
}
pub static HOSTCAP: LazyLock<crate::hostcap::HostCap> = LazyLock::new(|| unimplemented!());
// (struct Bridges / Sessions are declared in ext_core.rs; K3 owns them)
impl Bridges {
    pub fn steamid(&self, token: &str) -> u64 {
        unimplemented!()
    }
    pub fn port(&self, token: &str) -> i64 {
        unimplemented!()
    }
    pub fn conn(&self, token: &str) -> Option<Conn> {
        unimplemented!()
    }
}
impl Sessions {
    pub fn conn(&self, token: &str) -> Option<Conn> {
        unimplemented!()
    }
}
