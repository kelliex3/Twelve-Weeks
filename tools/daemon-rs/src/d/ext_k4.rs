// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K4 (mm / teams / jip) chunk calls, with the signatures it
// expects. Bodies are `unimplemented!()` (the K4 selftest swaps the few it needs for scripted tables, see k4_selftest.rs).
#![allow(dead_code, unused_variables)]
use super::*;
use parking_lot::Mutex;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;

/// `frozenset(players)` as a dictionary key.
pub type Key = BTreeSet<String>;
pub fn key_of(pl: &[String]) -> Key {
    pl.iter().cloned().collect()
}

// ---- K1 base: config (daemon.py:149-310), all read once from the environment
macro_rules! cfg_stub {
    ($($n:ident : $t:ty),* $(,)?) => { $( pub static $n: LazyLock<$t> = LazyLock::new(|| unimplemented!()); )* };
}
cfg_stub!(KEEPALIVE_TTL: f64, ENGINE_SLOTS: i64, MATCH_SIZE: i64, MIN_PLAYERS: i64, FILL_WAIT: f64, JIP: bool, JIP_HOST_CAP: bool,
    WAVE_RESPAWN: bool, JIP_MIN_AGE: f64, JIP_MIN_AGE_REMNANT: f64, JIP_MIN_LEFT: f64, LIVES_START: i64, JIP_CLOCK_TTL: f64, JIP_REUSE_S: f64,
    JIP_PREFER_LEFT: f64, JIP_ACK_STALE: f64, SECOND_MATCH_MIN: i64, RELAY_MATCH_MAX: i64, REQUEUE: bool, REQUEUE_WAIT_MAX: f64,
    REQUEUE_TICKET_S: f64, REQUEUE_OFFLINE_S: f64, REQUEUE_CAP_S: f64, LOBBY_KEEP: bool, LOBBY_RELEASE_S: f64, LOBBY_MIN_GAP_S: f64,
    LOBBY_BOOK: bool, AFK_KICK_S: f64, PING_KICK_MS: i64, MM_REGION_ON: bool, MM_REGION_WAIT_S: f64, MM_PING_GATE: bool, QUEUE_HOLD_MAX: f64,
    PARTY_HOLD_S: f64, MAX_WAIT: f64, FAST_STEP: f64, FAST_MAX_WAIT: f64, FAST_ALONE: f64, JIP_ACK_MAX: f64, QUEUE_SETTLE: f64,
    QUEUE_FRESH: f64, PING_WAIT_S: f64);
pub fn book_on() -> bool {
    unimplemented!()
}
pub fn fill_rule() -> String {
    unimplemented!()
}
/// daemon.py:226 RQ_CAP: token -> time of its last 0xE3 report with flag 0x20
pub static RQ_CAP: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:236 PING_KICKED: (token, mid) removed for ping
pub static PING_KICKED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// `get_name(token)`: "" = none (Python None)
pub fn get_name(token: &str) -> String {
    unimplemented!()
}
pub fn token_steamid(token: &str) -> u64 {
    unimplemented!()
}
pub fn silence(token: &str, now: f64) -> f64 {
    unimplemented!()
}
pub fn requeue_keep_mode(token: &str, mid: &str) {
    unimplemented!()
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
    unimplemented!()
}
pub fn has_helper(token: &str) -> bool {
    unimplemented!()
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
        unimplemented!()
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
    unimplemented!()
}
pub fn roster_team_in(mid: &str, slot: i64, tok: Option<&str>) -> i64 {
    unimplemented!()
}
pub fn match_sid(players: &[String]) -> u32 {
    unimplemented!()
}
/// `SID_KEEP[key] = sid`
pub fn sid_keep_set(key: &Key, sid: u32) {
    unimplemented!()
}
/// `for d in (MATCH_MAP, MATCH_P2P, VOTES, TIMING): if old in d and old != new: d[new] = d.pop(old)`
pub fn rekey_match_dicts(old: &Key, new: &Key) {
    unimplemented!()
}
pub fn match_map_get(key: &Key) -> Option<i64> {
    unimplemented!()
}
/// `MATCH_P2P.get(key)`: None = unknown
pub fn match_p2p_get(key: &Key) -> Option<bool> {
    unimplemented!()
}
pub fn map_name(idx: i64) -> String {
    unimplemented!()
}
pub fn map_pool(players: &[String], support: Option<&[String]>) -> Vec<i64> {
    unimplemented!()
}
pub fn p1_pref_on() -> bool {
    unimplemented!()
}
pub fn p1_always(token: &str, leader: Option<&str>) -> bool {
    unimplemented!()
}
pub fn p1_owner(token: &str, leader: Option<&str>) -> bool {
    unimplemented!()
}
pub fn p1_fits(token: &str, mid: &str, leader: Option<&str>) -> bool {
    unimplemented!()
}
pub fn countdown_seconds() -> f64 {
    unimplemented!()
}
pub fn ping_match_ms() -> i64 {
    unimplemented!()
}
pub fn pair_ping(a: &str, b: &str) -> Option<i64> {
    unimplemented!()
}
pub fn perf_penalty(token: &str) -> i64 {
    unimplemented!()
}
pub fn no_host(token: &str) -> bool {
    unimplemented!()
}
pub fn host_allowed(tokens: &[String]) -> Vec<String> {
    unimplemented!()
}
pub fn host_pool(tokens: &[String]) -> (Vec<String>, String) {
    unimplemented!()
}
pub fn best_host_order(tokens: &[String]) -> Vec<String> {
    unimplemented!()
}
pub fn mm_region(token: &str) -> Option<String> {
    unimplemented!()
}
pub fn jip_host_verdict(host: &str, n: i64) -> (i64, String) {
    unimplemented!()
}
/// `jip_ping_block(token, mid, host, active, joining, now)`: the ping in ms when the joiner is over the limit to the host, else None
pub fn jip_ping_block(token: &str, mid: &str, host: &str, active: &[String], joining: &HashSet<String>, now: f64) -> Option<i64> {
    unimplemented!()
}
pub fn _ping_gate(a: &str, b: &str, now: f64) -> Option<i64> {
    unimplemented!()
}
pub fn ping_left_out_note(dropped: &[String], now: f64) {
    unimplemented!()
}
pub fn ping_wait_judge(kept: &[String], host: Option<&str>, pair: &dyn Fn(&str, &str) -> Option<i64>, now: f64,
    members_fn: &dyn Fn(&str) -> Vec<String>, host_ok: &dyn Fn(&str) -> bool) -> (bool, Vec<String>) {
    unimplemented!()
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
