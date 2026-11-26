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
