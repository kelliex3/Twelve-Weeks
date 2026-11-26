// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K3 (sess / maps / flow / ping) chunk calls.
#![allow(dead_code, unused_variables)]
use super::*;
use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::LazyLock;

// ---- K1 private lobbies
/// `priv_pick_map(mid, players)` (daemon.py:7632): (map index, vote options)
pub fn priv_pick_map(mid: &str, players: &[String]) -> (i64, Vec<i64>) {
    (map_index(players), Vec::new())
}
