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

// ---- K6 loop
/// `loss_note(token, parse_loss(pl))` (daemon.py:10600 / 10629): the loss record of an op 0xCA payload; never raises
pub fn loss_note_from_pl(token: &str, pl: &[u8]) {}
