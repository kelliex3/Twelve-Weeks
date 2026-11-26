// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K1 (base / ops) chunk calls, with the signatures it expects.
// Bodies are `unimplemented!()` (the K1 selftest swaps the few it needs for scripted tables, see k1_selftest.rs).
#![allow(dead_code, unused_variables)]
use super::*;
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

// ---- K6 loop
/// `live_save(now=None)` (daemon.py:9741)
pub fn live_save(now: Option<f64>) {
    unimplemented!()
}
/// `mid in RESTORED_T` (daemon.py:9895)
pub fn restored_t_has(mid: &str) -> bool {
    false
}
