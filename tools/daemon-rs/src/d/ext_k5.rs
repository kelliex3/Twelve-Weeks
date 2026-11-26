// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K5 (batch) chunk calls, with the signatures it expects
// (K2 clan glue, K6 loop, KP private hooks). Their bodies are deterministic stand-ins that the K5 selftest (k5_selftest.rs) drives; verify/py/k5_harness.py
// patches the same behaviour into daemon.py.
#![allow(dead_code, unused_variables)]
use super::*;
use crate::py;
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::LazyLock;

/// What the selftest scripts into the stand-ins (tokens / addresses the private hooks answer for).
#[derive(Default)]
pub struct StubWorld {
    pub banned: HashSet<String>,
    pub banned_ips: HashSet<String>,
    pub install: HashSet<String>,
    pub loss_hold: HashSet<String>,
    pub ac_ip: HashSet<String>,
}
pub static STUBS: LazyLock<Mutex<StubWorld>> = LazyLock::new(|| Mutex::new(StubWorld::default()));

/// one log line per call of a stand-in whose Python original has side effects outside this chunk
pub static STUB_LOG: AtomicBool = AtomicBool::new(false);
pub fn stub_note(name: &str, args: String) {
    if !STUB_LOG.load(Ordering::Relaxed) {
        return;
    }
    dlog!("STUB", name, args);
}

fn raw_list(v: &[String]) -> String {
    py::list_str(v)
}

// ---- K2 clan glue (sigs/k2_clan.rs)
pub static CLAN_WIN: LazyLock<Mutex<IndexMap<(String, String), Option<bool>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub static CLAN_MAP: LazyLock<Mutex<IndexMap<String, String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub static END_BY_REPORTS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static RESULT_BOOKED: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static CLAN_LAST_MATCH: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub fn clan_match_started(players: &[String], mid: &str) {
    stub_note("clan_match_started", format!("{} {}", raw_list(players), mid));
}
fn tagged(op_tag: u8, op: u8, pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() % 5 == 0 {
        return None;
    }
    let mut v = vec![op_tag, op];
    v.extend_from_slice(crate::util::sl(pl, 0, 6));
    Some(v)
}
pub fn names_op(op: u8, pl: &[u8]) -> db::DbResult<Option<Vec<u8>>> {
    Ok(tagged(b'N', op, pl))
}
pub fn clan_op(op: u8, pl: &[u8]) -> db::DbResult<Option<Vec<u8>>> {
    Ok(tagged(b'C', op, pl))
}
pub fn meta_op(op: u8, pl: &[u8]) -> db::DbResult<Option<Vec<u8>>> {
    Ok(tagged(b'M', op, pl))
}
pub fn rank_table_op(pl: &[u8]) -> db::DbResult<Option<Vec<u8>>> {
    Ok(tagged(b'R', 0xBF, pl))
}
pub fn feed_op(pl: &[u8]) -> Option<Vec<u8>> {
    tagged(b'F', 0xB4, pl)
}
pub fn clan13_op(pl: &[u8]) -> Option<Vec<u8>> {
    tagged(b'B', 0xB6, pl)
}
pub fn ach_report_op(pl: &[u8]) -> Option<Vec<u8>> {
    tagged(b'A', 0xA9, pl)
}
pub fn ach_live_op(pl: &[u8]) -> Option<Vec<u8>> {
    tagged(b'A', 0xAB, pl)
}
pub fn ach_read_op(pl: &[u8]) -> Option<Vec<u8>> {
    tagged(b'A', 0xAA, pl)
}
pub fn leaderboard_table(top: usize) -> db::DbResult<Vec<u8>> {
    Ok(vec![b'L', b'B', top as u8])
}
/// `clan_ingame.stats_roster(c, mid, mapname, rows, winner)`: rows = (token, name, team, left)
pub fn clan_ingame_stats_roster(c: &rusqlite::Connection, mid: &str, mapname: &str, rows: &[(String, String, i64, bool)], winner: i64) -> rusqlite::Result<()> {
    let r: Vec<String> = rows.iter().map(|(t, n, tm, l)| format!("({}, {}, {}, {})", crate::pyfmt::str_repr(t), crate::pyfmt::str_repr(n), tm, py::b(*l))).collect();
    stub_note("stats_roster", format!("{} {} [{}] {}", mid, crate::pyfmt::str_repr(mapname), r.join(", "), winner));
    Ok(())
}

// ---- threading.Timer(delay, f).start()  (K2's py_timer: sigs/k2_clan.rs)
pub static TEST_TIMERS_ON: AtomicBool = AtomicBool::new(false);
static TIMERS: LazyLock<Mutex<Vec<(f64, Box<dyn FnOnce() + Send>)>>> = LazyLock::new(|| Mutex::new(Vec::new()));
pub fn py_timer(delay: f64, f: Box<dyn FnOnce() + Send>) {
    if TEST_TIMERS_ON.load(Ordering::Relaxed) {
        TIMERS.lock().push((delay, f));
        return;
    }
    let _ = std::thread::Builder::new().name("timer".into()).stack_size(256 * 1024).spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs_f64(delay.max(0.0)));
        f();
    });
}
/// (selftest) run every queued timer callback now, in start order
pub fn test_fire_timers() {
    let v: Vec<(f64, Box<dyn FnOnce() + Send>)> = std::mem::take(&mut *TIMERS.lock());
    for (_, f) in v {
        f();
    }
}

// ---- K6 loop
pub static _SILENT: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// `loss_hold_note(token, now=None)` (daemon.py:10818): the note a new search is refused with while the player's loss hold runs
pub fn loss_hold_note(token: &str, now: Option<f64>) -> Option<Vec<u8>> {
    if STUBS.lock().loss_hold.contains(token) {
        Some(b"LH".to_vec())
    } else {
        None
    }
}

// ---- KP private hooks (the real ones live in private/*.rs; src/privstub.rs has the neutral public versions)
/// `is_banned(token, name=None, ip=None)` (daemon.py:1262)
pub fn is_banned(token: &str, name: Option<&str>, ip: Option<&str>) -> bool {
    let w = STUBS.lock();
    w.banned.contains(token) || ip.is_some_and(|i| w.banned_ips.contains(i))
}
/// `ip_banned(ip)` (daemon.py:1246): the bans.txt entry (as text) that covers the address, else None
pub fn ip_banned(ip: &str) -> Option<String> {
    if STUBS.lock().banned_ips.contains(ip) {
        Some("10.0.0.0/8".to_string())
    } else {
        None
    }
}
/// `ident_note(token, steamid=0, hwids=(), ip=None)` (daemon.py:1165)
pub fn ident_note(token: &str, steamid: u64, ip: Option<&str>) {
    stub_note("ident_note", format!("{} {} {}", token, steamid, ip.unwrap_or("None")));
}
/// `ident_pid(token, steamid=0)` (daemon.py:1237)
pub fn ident_pid(token: &str) -> Option<String> {
    if STUBS.lock().banned.contains(token) {
        Some(format!("PID-{}", py::cut(token, 4)))
    } else {
        None
    }
}
/// `install_refusal(token, ip, wait_s=0.0)` (daemon.py:3025); boot = the boot-handshake call (wait_s = attest.WAIT_S)
pub fn install_refusal(token: &str, ip: Option<&str>, boot: bool) -> Option<(String, Vec<u8>)> {
    if STUBS.lock().install.contains(token) {
        Some((format!("stub install{}", if boot { " boot" } else { "" }), b"IR".to_vec()))
    } else {
        None
    }
}
/// `hwid_op(pl, ip)` (daemon.py:1115): the reply
pub fn hwid_op(pl: &[u8], ip: Option<&str>) -> Vec<u8> {
    let mut v = b"HW".to_vec();
    v.extend_from_slice(crate::util::sl(pl, 0, 2));
    v.extend_from_slice(ip.unwrap_or("").as_bytes());
    v
}
/// `attest_op(pl, addr)` (daemon.py:3059): the reply
pub fn attest_op(pl: &[u8], addr: &Addr) -> Vec<u8> {
    let mut v = b"AT".to_vec();
    v.extend_from_slice(crate::util::sl(pl, 0, 2));
    v.extend_from_slice(ip_of(addr).as_bytes());
    v
}
/// the four anti-cheat ops (daemon.py:2128-2170 ac_events_op / ac_demo_op / ac_report_op / ac_recent_op, dispatched by op): the reply, None = nothing
pub fn ac_op(op: u8, pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() % 2 == 0 {
        None
    } else {
        Some(vec![b'A', b'C', op, pl[0]])
    }
}
/// `AC.note_ip(token, ip)` of the login (daemon.py:8890): Some((kind, weight, evidence)) = a shared token; None when the anti-cheat is off / nothing found
pub fn ac_note_login_ip(token: &str, ip: &str) -> Option<(i64, f64, String)> {
    if STUBS.lock().ac_ip.contains(token) {
        Some((7, 0.5, "stub evidence".to_string()))
    } else {
        None
    }
}
/// `ac_server_event(token, mid, kind, w, evidence)` (daemon.py:2111)
pub fn ac_server_event(token: &str, mid: &str, kind: i64, w: f64, evidence: &str) {
    stub_note("ac_server_event", format!("{} {} {} {} {}", token, mid, kind, py::f(w), evidence));
}
/// (selftest) forget every queued timer
pub fn test_fire_timers_clear() {
    TIMERS.lock().clear();
}
