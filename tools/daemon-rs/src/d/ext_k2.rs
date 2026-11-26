// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K2 (clan glue) chunk calls, with the signatures they will have.
// The CL6 modules (clan_perks, clan_status, clan_ingame, achievements) are not merged yet: they are DETERMINISTIC DOUBLES here (the same doubles as
// verify/py/k2_harness.py injects as Python modules): they log their arguments and write one note row, so the differential test sees exactly what the glue
// passed and the transaction it ran in. At integration: `use crate::clan::{achievements, clan_ingame, clan_perks, clan_status};` replaces the four
// `pub mod` blocks and the harness runs with T2R_K2_DOUBLES=0 (real modules on both sides).
#![allow(dead_code, unused_variables)]
use super::*;
use crate::clock;
use indexmap::IndexMap;
use parking_lot::Mutex;
use rusqlite::Connection;
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::AtomicBool;
use std::sync::LazyLock;

fn note(c: &Connection, what: &str) {
    let _ = c.execute("CREATE TABLE IF NOT EXISTS dbl_notes(seq INTEGER PRIMARY KEY AUTOINCREMENT, note TEXT)", []);
    let _ = c.execute("INSERT INTO dbl_notes(note) VALUES(?)", [what]);
}

fn counts_str<'a>(it: impl Iterator<Item = (&'a i64, &'a i64)>) -> String {
    format!("{{{}}}", it.map(|(k, v)| format!("{}: {}", k, v)).collect::<Vec<_>>().join(", "))
}

fn sorted_counts(m: &HashMap<i64, i64>) -> String {
    let mut v: Vec<(&i64, &i64)> = m.iter().collect();
    v.sort();
    counts_str(v.into_iter())
}

fn ob(v: Option<bool>) -> &'static str {
    match v {
        Some(true) => "True",
        Some(false) => "False",
        None => "None",
    }
}

// CL6 (clan_perks, clan_status, clan_ingame, achievements): the real modules (crate::clan), see clan.rs.


// ---------------------------------------------------------------- test hooks (selftest k2 only; empty in production)
/// token -> seconds of silence (`silence(token, now)` of K6 reads the presence table)
pub static SILENCE: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// mids the PRIV double reports as private matches (K1's PrivStub.is_private_mid reads it)
pub static PRIV_MIDS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
