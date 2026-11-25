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

// ---- K3 sess: ping wait (daemon.py:4459)
pub static PING_WAIT_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PING_WAIT_S", 10.0));
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
/// (selftest) get_name / token_steamid answer from WORLD while this is set
pub static WORLD_ON: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
pub fn silence(token: &str, now: f64) -> f64 {
    0.0
}
pub fn readopt_claim(token: &str) {
    unimplemented!()
}
pub fn afk_report(token: &str, idle_s: i64, flags: u8) {
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
