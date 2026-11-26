// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K1 (base / ops) chunk calls, with the signatures it expects.
// Bodies are `unimplemented!()` (the K1 selftest swaps the few it needs for scripted tables, see k1_selftest.rs).
#![allow(dead_code, unused_variables)]
use super::*;
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

// ---- KP private: daemon.py:1038 HWID_HEX (the width of a hardware id entry in bans.txt)
pub const HWID_HEX: usize = 24;

// ---- K2 clan: daemon.py:1556-1595 settings and tables that the leave / abort paths read
pub static CLAN_NO_PENALTY: LazyLock<bool> = LazyLock::new(|| !matches!(crate::env_str("T2R_CLAN_NO_PENALTY", "0").as_str(), "" | "0"));
pub static CLAN_EXCUSE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_EXCUSE_S", 90.0));
pub static CLAN_START_GRACE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_START_GRACE", 45.0));
pub static CLAN_END_GRACE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_END_GRACE", 30.0));
pub static CLAN_CLOSING_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_CLOSING_S", 20.0));
pub static CLAN_LOST_EXCUSE_MAX: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_LOST_EXCUSE_MAX", 3i64));
pub static CLAN_LOST_EXCUSE_WINDOW: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_LOST_EXCUSE_WINDOW", 86400.0));
pub const CLAN_DELIBERATE_WHY: [&str; 2] = ["left (0x02)", "searching again"];
/// daemon.py:1590 token -> (time, mid) of its 'my window is closing' notice
pub static CLAN_CLOSING: LazyLock<Mutex<IndexMap<String, (f64, Option<String>)>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1591 (token, mid): its session dropped within CLAN_CLOSING_S of that notice
pub static CLAN_CLOSE_DROPS: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:1579 (token, mid) -> why the match was ended for it (log text)
pub static CLAN_ABORTED: LazyLock<Mutex<IndexMap<(String, String), String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1582 mid -> (time, token) of the latest departure from the running match
pub static CLAN_LAST_DEPART: LazyLock<Mutex<HashMap<String, (f64, String)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1593 token -> [times]: crash / lost-connection excuses granted
pub static CLAN_LOST_EXCUSES: LazyLock<Mutex<HashMap<String, Vec<f64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// `clan_match_ended(players, mid)` (daemon.py:2343)
pub fn clan_match_ended(players: &[String], mid: &str) {
    unimplemented!()
}
