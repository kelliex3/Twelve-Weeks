// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K3 (sess / maps / flow / ping) chunk calls.
#![allow(dead_code, unused_variables)]
use super::*;
use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::LazyLock;

// ---- K2 clan
/// `rank_of_token(token)` (daemon.py:2486): (rank badge, weeks) of the roster row
pub fn rank_of_token(token: &str) -> (i64, i64) {
    (0, 0)
}
/// `CLAN_MATCH.get(token)` (daemon.py:1569): the mid of its last started match
pub fn clan_match_get(token: &str) -> Option<String> {
    None
}
/// `with _clanlock: CLAN_PENDING.setdefault(token, (mid, 0, 0, {}))` (daemon.py:1548)
pub fn clan_pending_setdefault(token: &str, mid: &str) {}
/// `CLAN_DUE[token] = t` (daemon.py:1549)
pub fn clan_due_set(token: &str, t: f64) {}

// ---- K1 private lobbies
/// `priv_pick_map(mid, players)` (daemon.py:7632): (map index, vote options)
pub fn priv_pick_map(mid: &str, players: &[String]) -> (i64, Vec<i64>) {
    (map_index(players), Vec::new())
}
