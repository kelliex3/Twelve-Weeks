// TEMPORARY: removed at integration. What the K6 chunk still needs from outside: the KP private hooks (neutral), the `k_` wrappers through which the
// K6 selftest records the K1 calls (leave_running_match / forget_match_state / abort_award) and the K5 / K2 calls (maybe_game_start, loaded_clear_match,
// jip_requeue_if_empty, match_end_via_reports) instead of running them (REC_ON).
#![allow(dead_code, unused_variables)]
use super::*;
use parking_lot::Mutex;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::LazyLock;

/// (selftest k6) record the doubles' calls here instead of running the real functions
pub static REC_ON: AtomicBool = AtomicBool::new(false);
pub static REC: LazyLock<Mutex<Vec<String>>> = LazyLock::new(|| Mutex::new(Vec::new()));
pub fn rec(s: String) {
    if REC_ON.load(Ordering::Relaxed) {
        REC.lock().push(s);
    }
}
/// scripted answer of the `maybe_game_start` double
pub static MGS_ANSWER: AtomicBool = AtomicBool::new(false);

// ---- KP private hooks (neutral)
pub fn ac_tick() {}
pub fn ident_backfill() {}

/// (selftest k6) the K6 functions are real while the scripted K4 world answers the K3 / K1 lookups
pub static K6_ON: AtomicBool = AtomicBool::new(false);
/// the K4 / K1 selftests run with the scripted world only: silence / ping gate / ping wait keep their neutral answers there
pub fn world_only() -> bool {
    WORLD_ON.load(Ordering::Relaxed) && !K6_ON.load(Ordering::Relaxed)
}
/// (selftest k2 / k5) the stand-ins their Python harnesses patch over silence / readopt_claim / live_save / loss_hold_note: 2 = K2, 5 = K5, 0 = real
pub static HARNESS_STANDIN: std::sync::atomic::AtomicU8 = std::sync::atomic::AtomicU8::new(0);
pub fn standin() -> u8 {
    HARNESS_STANDIN.load(Ordering::Relaxed)
}
/// (selftest k3) the Python K3 harness replaces `_ping_gate` by `pair_ping(a, b)`: the Rust side keeps the same stand-in there
pub static STANDIN: AtomicBool = AtomicBool::new(false);

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
pub fn k_loaded_clear_match(mid: &str, players: &[String]) {
    if REC_ON.load(Ordering::Relaxed) {
        rec(format!("loaded_clear_match {} {}", mid, players.join(",")));
        return;
    }
    loaded_clear_match(mid, players)
}
pub fn k_jip_requeue_if_empty(mid: &str) -> Vec<String> {
    if REC_ON.load(Ordering::Relaxed) {
        rec(format!("jip_requeue_if_empty {}", mid));
        return Vec::new();
    }
    jip_requeue_if_empty(mid)
}
pub fn k_maybe_game_start(token: &str) -> db::DbResult<bool> {
    if REC_ON.load(Ordering::Relaxed) {
        rec(format!("maybe_game_start {}", token));
        return Ok(MGS_ANSWER.load(Ordering::Relaxed));
    }
    maybe_game_start(token)
}
pub fn k_match_end_via_reports(mid: &str, now: Option<f64>) -> bool {
    if REC_ON.load(Ordering::Relaxed) {
        rec(format!("match_end_via_reports {}", mid));
        return false;
    }
    match_end_via_reports(mid, now)
}
