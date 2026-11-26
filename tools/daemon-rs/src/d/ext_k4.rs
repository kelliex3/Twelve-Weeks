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

// ---- K6 loop: silence / re-adopt / AFK / admin / ping gate
pub fn silence(token: &str, now: f64) -> f64 {
    SILENCE.lock().get(token).copied().unwrap_or(0.0) // (K2 selftest table; 0 otherwise)
}
pub fn readopt_claim(token: &str) {
    // (K2 selftest: no-op until K6 lands)
}
pub fn afk_report(token: &str, idle_s: i64, flags: u8) {
    unimplemented!()
}
pub fn admin_kick(pid: &str, move_: bool) -> serde_json::Value {
    unimplemented!()
}
/// `jip_ping_block(token, mid, host, active, joining, now)`: the ping in ms when the joiner is over the limit to the host, else None
pub fn jip_ping_block(token: &str, mid: &str, host: &str, active: &[String], joining: &HashSet<String>, now: f64) -> Option<i64> {
    None
}
pub fn _ping_gate(a: &str, b: &str, now: f64) -> Option<i64> {
    pair_ping(a, b)
}
pub fn ping_left_out_note(dropped: &[String], now: f64) {}
pub fn ping_wait_judge(
    kept: &[String],
    host: Option<&str>,
    pair: &dyn Fn(&str, &str) -> Option<i64>,
    now: f64,
    members_fn: &dyn Fn(&str) -> Vec<String>,
    host_ok: &dyn Fn(&str) -> bool,
) -> (bool, Vec<String>) {
    (false, Vec::new())
}

