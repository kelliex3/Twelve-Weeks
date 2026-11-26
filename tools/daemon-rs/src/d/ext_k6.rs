// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K6 (loop / admin) chunk calls, with the signatures the owners
// published (sigs/k2_clan.rs, sigs/k5_batch.rs; KP = private hooks, neutral). The K5 doubles record their calls while REC_ON is set (selftest k6).
#![allow(dead_code, unused_variables)]
use super::*;
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::LazyLock;

// ---- K2 clan (sigs/k2_clan.rs v2)
pub type Pending = (String, i64, i64, IndexMap<i64, i64>);
pub static CLAN_MATCH: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_ENDED: LazyLock<Mutex<IndexMap<String, f64>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub static CLAN_PENDING: LazyLock<Mutex<HashMap<String, Pending>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_DUE: LazyLock<Mutex<IndexMap<String, f64>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub static CLAN_KICKED: LazyLock<Mutex<IndexMap<(String, String), String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub static CLAN_REPORTED_END: LazyLock<Mutex<HashMap<String, IndexMap<String, f64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static END_BY_REPORTS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static RESULT_BOOKED: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static BOOST_PICK: LazyLock<Mutex<HashMap<String, i64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static BOOST_ACTIVE: LazyLock<Mutex<HashMap<String, i64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_GRACE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_GRACE", 20.0));
pub static HOSTMIG_NOCAND_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_HOSTMIG_NOCAND_S", 45.0));
/// `_clan_sched[0]`: clan_report starts the scheduler thread only while this is false
pub static CLAN_SCHED: AtomicBool = AtomicBool::new(false);
/// `_prune_keys(FEED_SEEN, dead)` (K2 keeps FEED_SEEN private)
pub fn feed_seen_prune(dead: &dyn Fn(&str) -> bool) -> usize {
    0
}
pub fn match_end_via_reports(mid: &str, now: Option<f64>) -> bool {
    rec(format!("match_end_via_reports {}", mid));
    false
}
pub fn clan_scheduler() {
    loop {
        std::thread::sleep(std::time::Duration::from_secs(3600));
    }
}

// ---- K5 batch (sigs/k5_batch.rs v1)
/// (selftest k6) record the K5 doubles' calls here instead of ignoring them
pub static REC_ON: AtomicBool = AtomicBool::new(false);
pub static REC: LazyLock<Mutex<Vec<String>>> = LazyLock::new(|| Mutex::new(Vec::new()));
fn rec(s: String) {
    if REC_ON.load(Ordering::Relaxed) {
        REC.lock().push(s);
    }
}
/// scripted answer of the `maybe_game_start` double
pub static MGS_ANSWER: AtomicBool = AtomicBool::new(false);
pub static JIP_WAIT_LOG: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub fn loaded_clear_match(mid: &str, players: &[String]) {
    rec(format!("loaded_clear_match {} {}", mid, players.join(",")));
}
pub fn jip_requeue_if_empty(mid: &str) -> Vec<String> {
    rec(format!("jip_requeue_if_empty {}", mid));
    Vec::new()
}
pub fn maybe_game_start(token: &str) -> db::DbResult<bool> {
    rec(format!("maybe_game_start {}", token));
    Ok(MGS_ANSWER.load(Ordering::Relaxed))
}

// ---- KP private hooks (neutral)
pub fn ac_tick() {}
pub fn ident_backfill() {}

// ---- K2 admin clan reset (daemon.py:10865-10904)
pub fn admin_clan_reset(body: &serde_json::Value) -> serde_json::Value {
    serde_json::json!({"ok": false, "error": "clan_reset.py missing in this build"})
}
pub fn admin_clan_restore(body: &serde_json::Value) -> serde_json::Value {
    serde_json::json!({"ok": false, "error": "clan_reset.py missing in this build"})
}

/// (selftest k6) the K6 functions are real while the scripted K4 world answers the K3 / K1 lookups
pub static K6_ON: AtomicBool = AtomicBool::new(false);
/// the K4 / K1 selftests run with the scripted world only: silence / ping gate / ping wait keep their neutral answers there
pub fn world_only() -> bool {
    WORLD_ON.load(Ordering::Relaxed) && !K6_ON.load(Ordering::Relaxed)
}

/// (selftest k3) the Python K3 harness replaces `_ping_gate` by `pair_ping(a, b)`: the Rust side keeps the same stand-in there
pub static STANDIN: AtomicBool = AtomicBool::new(false);

// The calls into K1 / K5 code that the K6 selftest records instead of running (REC_ON): same names with a `k_` prefix.
pub fn k_leave_running_match(token: &str, why: &str) -> bool {
    if REC_ON.load(Ordering::Relaxed) {
        rec(format!("leave_running_match {} {}", token, why));
        return true;
    }
    leave_running_match(token, why)
}
pub fn k_forget_match_state(players: &[String]) {
    if REC_ON.load(Ordering::Relaxed) {
        rec(format!("forget_match_state {}", players.join(",")));
        return;
    }
    forget_match_state(players)
}
pub fn k_abort_award(mid: &str, others: &[String], host: Option<&str>, why: &str) -> bool {
    if REC_ON.load(Ordering::Relaxed) {
        rec(format!("abort_award {} {} {} {}", mid, others.join(","), host.unwrap_or("None"), why));
        return true;
    }
    abort_award(mid, others, host, why)
}
