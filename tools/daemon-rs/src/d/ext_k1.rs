// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K1 (base / ops) chunk calls, with the signatures it expects.
// Bodies are `unimplemented!()` (the K1 selftest swaps the few it needs for scripted tables, see k1_selftest.rs).
#![allow(dead_code, unused_variables)]
use super::*;
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

