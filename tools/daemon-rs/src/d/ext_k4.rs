// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K4 (mm / teams / jip) chunk calls, with the signatures it
// expects. Bodies are `unimplemented!()` (the K4 selftest swaps the few it needs for scripted tables, see k4_selftest.rs).
// (the K3 items that used to be stubbed here are implemented in sess.rs / maps.rs / flow.rs / ping.rs; WORLD / WORLD_ON stay: the K4 / K1
// selftests switch those functions to the scripted world while WORLD_ON is set)
#![allow(dead_code, unused_variables)]
use super::*;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

/// The scripted world of the K4 selftest (names, Steam ids, pings, host rules); the K3 functions read it while WORLD_ON is set.
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
pub static WORLD: LazyLock<Mutex<World>> = LazyLock::new(|| Mutex::new(World::default()));
/// (selftest) get_name / token_steamid answer from WORLD while this is set
pub static WORLD_ON: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

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
