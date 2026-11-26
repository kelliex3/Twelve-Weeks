//! K2 clan glue, part 1: daemon.py:1569-2101 (clan bookkeeping, clan keys, day resolution, achievements glue) and 2203-2420 (clan_report,
//! match end by reports, match start / end, boosters). The clan engine itself is `crate::clan::*`; part 2 (ops, feed, leaderboard, admin) is clan_ops.rs.
//! Every function keeps its Python name; module state keeps its name as a `pub static`.
use super::*;
use crate::clan::clan as cl;
use crate::clock;
use crate::clan::clan_story;
use crate::py;
use indexmap::IndexMap;
use parking_lot::Mutex;
use rusqlite::{Connection, OptionalExtension};
use serde_json::{Map, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::LazyLock;

pub use crate::clan::{achievements, clan_ingame, clan_perks, clan_status};

type DbR<T> = db::DbResult<T>;

// ---------------------------------------------------------------- state (daemon.py:1569-1625)
/// daemon.py:1569 CLAN_MATCH: token -> mid of its last started match
pub static CLAN_MATCH: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1570 CLAN_TEAM: mid -> {token: team} at the match start
pub static CLAN_TEAM: LazyLock<Mutex<IndexMap<String, HashMap<String, i64>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1571 CLAN_MAP: mid -> display name of the map the match was started on
pub static CLAN_MAP: LazyLock<Mutex<IndexMap<String, String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1572 CLAN_ENDED: mid -> time the host reported the end
pub static CLAN_ENDED: LazyLock<Mutex<IndexMap<String, f64>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// `(mid, parts, flags, counts)` of a pending 0xE2 report
pub type Pending = (String, i64, i64, IndexMap<i64, i64>);
/// daemon.py:1573 CLAN_PENDING: token -> (mid, parts, flags, counts)
pub static CLAN_PENDING: LazyLock<Mutex<HashMap<String, Pending>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1574 CLAN_CREDITED: (token, mid) whose clan day was resolved
pub static CLAN_CREDITED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:1575 CLAN_QUITTERS: (token, mid): the host that left a match we then ended for the others
pub static CLAN_QUITTERS: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static CLAN_GRACE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_GRACE", 20.0));
pub static CLAN_EXCUSE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_EXCUSE_S", 90.0));
pub static HOSTMIG_NOCAND_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_HOSTMIG_NOCAND_S", 45.0));
pub static CLAN_START_GRACE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_START_GRACE", 45.0));
pub static CLAN_END_GRACE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_END_GRACE", 30.0));
pub static CLAN_REJOIN_MAX_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_REJOIN_MAX", 900.0));
pub static CLAN_NO_PENALTY: LazyLock<bool> = LazyLock::new(|| !matches!(crate::env_str("T2R_CLAN_NO_PENALTY", "0").as_str(), "" | "0"));
pub const CLAN_DELIBERATE_WHY: [&str; 2] = ["left (0x02)", "searching again"];
/// daemon.py:1600 CLAN_EXCUSE_WHY: (token, mid) -> why the departure was excused
pub static CLAN_EXCUSE_WHY: LazyLock<Mutex<HashMap<(String, String), String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1605 CLAN_KICKED: (token, mid) -> why (log text)
pub static CLAN_KICKED: LazyLock<Mutex<IndexMap<(String, String), String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1611 CLAN_ABORTED: (token, mid) -> why the match was ended for it (log text)
pub static CLAN_ABORTED: LazyLock<Mutex<IndexMap<(String, String), String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1612 CLAN_LEFT_AT: (token, mid) -> time of its departure from the running match
pub static CLAN_LEFT_AT: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1613 CLAN_REJOINED: (token, mid) -> time the first deferral happened
pub static CLAN_REJOINED: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1614 CLAN_LAST_DEPART: mid -> (time, token) of the latest departure from the running match
pub static CLAN_LAST_DEPART: LazyLock<Mutex<HashMap<String, (f64, String)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1615 CLAN_EXCUSED: (token, mid)
pub static CLAN_EXCUSED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:1616 CLAN_LINK_EXCUSES: token -> [times]: 'my link was lost' excuses granted
pub static CLAN_LINK_EXCUSES: LazyLock<Mutex<HashMap<String, Vec<f64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub const CLAN_LINK_EXCUSE_MAX: usize = 2;
pub const CLAN_LINK_EXCUSE_WINDOW: f64 = 86400.0;
/// daemon.py:1621 CLAN_CLOSING: token -> (time, mid) of its 'my window is closing' notice
pub static CLAN_CLOSING: LazyLock<Mutex<IndexMap<String, (f64, Option<String>)>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1622 CLAN_CLOSE_DROPS: (token, mid): its session dropped within CLAN_CLOSING_S of that notice
pub static CLAN_CLOSE_DROPS: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static CLAN_CLOSING_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_CLOSING_S", 20.0));
/// daemon.py:1624 CLAN_LOST_EXCUSES: token -> [times]: crash / lost-connection excuses granted
pub static CLAN_LOST_EXCUSES: LazyLock<Mutex<HashMap<String, Vec<f64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static CLAN_LOST_EXCUSE_MAX: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_LOST_EXCUSE_MAX", 3i64));
pub static CLAN_LOST_EXCUSE_WINDOW: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_CLAN_LOST_EXCUSE_WINDOW", 86400.0));
/// daemon.py:1627 `_clanlock` (guards CLAN_PENDING compound updates, CLAN_REPORTED_END, _end_timers, _clan_sched)
pub static CLANLOCK: Mutex<()> = Mutex::new(());
/// daemon.py:1685 CLAN_WIN: (token, mid) -> True win / False loss / None draw of the token's team (read once by clan_match_facts)
pub static CLAN_WIN: LazyLock<Mutex<IndexMap<(String, String), Option<bool>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:1686 CLAN_WEAPONS: optional extended label 24 -> clan_saga_data.WEAPONS key
pub const CLAN_WEAPONS: [&str; 12] = ["", "bow", "revolver", "shorty", "hunting", "semi", "burst", "pistol", "crossbow", "melee", "bomb", "molotov"];
/// daemon.py:1710 CLAN_MOVED: (token, mid) -> time: the admin moved it to the other team
pub static CLAN_MOVED: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:1711 CLAN_REBOOK: (token, mid): booked once, then the player joined the SAME running match again
pub static CLAN_REBOOK: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:1712 CLAN_DONE: token -> mid of the last match whose report was resolved
pub static CLAN_DONE: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:~2020 CLAN_DUE: token -> time its pending report is finalised when no match end arrived
pub static CLAN_DUE: LazyLock<Mutex<IndexMap<String, f64>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
static CLAN_SCHED: AtomicBool = AtomicBool::new(false);
/// daemon.py:2297 CLAN_LAST_MATCH: token -> last mid it was in
pub static CLAN_LAST_MATCH: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:2298 PRIV_LAST_MID: token -> mid of its last private match
pub static PRIV_LAST_MID: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static MEND_GRACE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_MEND_GRACE", 8.0));
/// daemon.py:2210 CLAN_REPORTED_END: mid -> {token: time} of the 0xE2 reports with flags bit 1
pub static CLAN_REPORTED_END: LazyLock<Mutex<HashMap<String, IndexMap<String, f64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:2211 END_BY_REPORTS: mids closed by match_end_via_reports
pub static END_BY_REPORTS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:2212 RESULT_BOOKED: mids whose winner (0x12) was booked in record_result
pub static RESULT_BOOKED: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
pub static END_TIMERS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:2350 BOOST_PICK: token -> booster id picked for the next match
pub static BOOST_PICK: LazyLock<Mutex<HashMap<String, i64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:2351 BOOST_ACTIVE: token -> booster id in use in the running match
pub static BOOST_ACTIVE: LazyLock<Mutex<HashMap<String, i64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

pub fn clan_tk(token: &str, mid: &str) -> (String, String) {
    (token.to_string(), mid.to_string())
}

// ---------------------------------------------------------------- items K3 calls
/// `CLAN_MATCH.get(token)`: the mid of its last started match
pub fn clan_match_get(token: &str) -> Option<String> {
    CLAN_MATCH.lock().get(token).cloned()
}

/// `with _clanlock: CLAN_PENDING.setdefault(token, (mid, 0, 0, {}))` (daemon.py:3100 clan_closed_book)
pub fn clan_pending_setdefault(token: &str, mid: &str) {
    let _g = CLANLOCK.lock();
    CLAN_PENDING.lock().entry(token.to_string()).or_insert_with(|| (mid.to_string(), 0, 0, IndexMap::new()));
}

/// `CLAN_DUE[token] = t`
pub fn clan_due_set(token: &str, t: f64) {
    CLAN_DUE.lock().insert(token.to_string(), t);
}

// ---------------------------------------------------------------- timers (threading.Timer / Thread of the module; a selftest queues them)
pub static TEST_TIMERS_ON: AtomicBool = AtomicBool::new(false);
static TIMER_SEQ: AtomicU64 = AtomicU64::new(0);
type TimerFn = Box<dyn FnOnce() + Send + 'static>;
static TEST_TIMERS: LazyLock<Mutex<Vec<(f64, u64, TimerFn)>>> = LazyLock::new(|| Mutex::new(Vec::new()));
/// Test hook (K2 selftest): `loaded_clear_match` is a no-op double (the K2 harness patches it to `lambda mid, players: None`).
pub static TEST_NO_LOADED_CLEAR: AtomicBool = AtomicBool::new(false);
/// Test hook: while set, clan_report's `leave_running_match` is the double of the harness (logs and returns true).
pub static TEST_LEAVE: AtomicBool = AtomicBool::new(false);

/// `threading.Timer(delay, f).start()` (daemon thread)
pub fn py_timer(delay: f64, f: TimerFn) {
    if TEST_TIMERS_ON.load(Ordering::Relaxed) {
        TEST_TIMERS.lock().push((clock::now() + delay, TIMER_SEQ.fetch_add(1, Ordering::Relaxed), f));
        return;
    }
    let _ = std::thread::Builder::new().name("clan-timer".into()).stack_size(256 * 1024).spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs_f64(delay.max(0.0)));
        f();
    });
}

/// Test: run every queued timer due at or before `t`, oldest first (the order of a Python `threading.Timer` queue is (due, start order)); the count.
pub fn test_fire_timers(t: f64) -> usize {
    let mut n = 0;
    loop {
        let next = {
            let mut q = TEST_TIMERS.lock();
            let mut best: Option<usize> = None;
            for (i, e) in q.iter().enumerate() {
                if e.0 <= t && best.is_none_or(|b| (e.0, e.1) < (q[b].0, q[b].1)) {
                    best = Some(i);
                }
            }
            best.map(|i| q.remove(i))
        };
        match next {
            Some((_, _, f)) => {
                f();
                n += 1;
            }
            None => return n,
        }
    }
}

// ---------------------------------------------------------------- clan keys (daemon.py:1628-1679)
fn first_value(c: &Connection, sql: &str, p: impl rusqlite::Params) -> rusqlite::Result<Option<Value>> {
    c.query_row(sql, p, |r| Ok(cl::vref_to_json(r.get_ref(0)?))).optional()
}

// py: daemon.py:1628 clan_key
/// Key of a player's clan: the Steam account when known ("s:<steamid>"), else the install token id; the first use of the Steam key adopts
/// the account's furthest-along clan among its old tokens (alive first, then day, then most recently played), boosters included.
pub fn clan_key_r(c: &Connection, token: &str) -> rusqlite::Result<String> {
    let tid = cl::token_id(token);
    let r = first_value(c, "SELECT steamid FROM players WHERE token=?", [token])?;
    let sid = match r {
        Some(v) if cl::truthy(&v) => cl::int_of(&v),
        _ => 0,
    };
    if sid == 0 {
        return Ok(tid);
    }
    let key = format!("s:{}", sid);
    cl::ensure(c)?;
    if c.query_row("SELECT 1 FROM clans WHERE token_id=?", [&key], |_| Ok(())).optional()?.is_some() {
        return Ok(key);
    }
    let mut set: BTreeSet<String> = BTreeSet::new();
    {
        let mut st = c.prepare("SELECT token FROM players WHERE steamid=?")?;
        let rows = st.query_map([sid], |r| r.get::<_, String>(0))?;
        for t in rows {
            set.insert(cl::token_id(&t?));
        }
    }
    set.insert(tid);
    let olds: Vec<String> = set.into_iter().collect();
    let mut best: Option<((bool, i64, f64), String)> = None;
    {
        let q = format!("SELECT token_id, status, day, updated FROM clans WHERE token_id IN ({})", vec!["?"; olds.len()].join(","));
        let mut st = c.prepare(&q)?;
        let mut rows = st.query(rusqlite::params_from_iter(olds.iter()))?;
        while let Some(r) = rows.next()? {
            let tok: String = r.get(0)?;
            let status = cl::vref_to_json(r.get_ref(1)?);
            let day = cl::vref_to_json(r.get_ref(2)?);
            let upd = cl::vref_to_json(r.get_ref(3)?);
            let kk = (cl::int_of(&status) == cl::ALIVE && !status.is_null(), if cl::truthy(&day) { cl::int_of(&day) } else { 0 }, if cl::truthy(&upd) { value_f64(&upd) } else { 0.0 });
            let better = match &best {
                None => true,
                Some((b, _)) => kk.0 > b.0 || (kk.0 == b.0 && (kk.1 > b.1 || (kk.1 == b.1 && kk.2 > b.2))),
            };
            if better {
                best = Some((kk, tok));
            }
        }
    }
    if let Some((k, from)) = best {
        let mut cols: Vec<String> = Vec::new();
        {
            let mut st = c.prepare("PRAGMA table_info(clans)")?;
            let rows = st.query_map([], |r| r.get::<_, String>(1))?;
            for n in rows {
                let n = n?;
                if n != "token_id" {
                    cols.push(n);
                }
            }
        }
        let cl_s = cols.join(",");
        c.execute(&format!("INSERT INTO clans(token_id,{}) SELECT ?,{} FROM clans WHERE token_id=?", cl_s, cl_s), rusqlite::params![key, from])?;
        // (sqlite3.OperationalError: no boosters table yet - ignored)
        let _ = c.execute("INSERT INTO boosters(token_id,bid,n) SELECT ?,bid,n FROM boosters WHERE token_id=? ON CONFLICT(token_id,bid) DO NOTHING", rusqlite::params![key, from]);
        dlog!("CLAN key", key, "adopted clan of install", from, format!("(alive {}, day {}) from {} install(s)", k.0 as i64, k.1, olds.len()));
    }
    Ok(key)
}

fn value_f64(v: &Value) -> f64 {
    match v {
        Value::Number(n) => n.as_f64().unwrap_or(0.0),
        Value::String(s) => s.trim().parse().unwrap_or(0.0),
        Value::Bool(b) => *b as i64 as f64,
        _ => 0.0,
    }
}

/// `clan_key(c, token)` for callers that cannot raise (join in progress): a database error falls back to the install token id
/// (the statements after it on the same connection fail the same way).
pub fn clan_key(c: &Connection, token: &str) -> String {
    match clan_key_r(c, token) {
        Ok(k) => k,
        Err(e) => {
            dlog!("CLAN key failed", e);
            cl::token_id(token)
        }
    }
}

/// `clan.snap_update(c, key, mid, rejoined=True)` (jip: clan_rejoin_after_booking)
pub fn clan_snap_rejoined(c: &Connection, key: &str, mid: &str) -> Result<(), String> {
    cl::snap_update(c, key, mid, &[("rejoined", Value::Bool(true))]).map_err(|e| e.to_string())
}

// py: daemon.py:1666 clan_read_key
/// READ-ONLY clan key for the launcher's reads (the hint is used only when the token itself has no steamid in the players table).
pub fn clan_read_key(c: &Connection, token: &str, sid_hint: i64) -> rusqlite::Result<String> {
    let r = first_value(c, "SELECT steamid FROM players WHERE token=?", [token])?;
    if r.as_ref().is_some_and(cl::truthy) {
        return clan_key_r(c, token);
    }
    if sid_hint > 0 {
        return Ok(format!("s:{}", sid_hint));
    }
    clan_key_r(c, token)
}

// py: daemon.py:1676 clan_state
pub fn clan_state(token: &str, sid_hint: i64) -> DbR<Value> {
    db::with_write(|c| {
        if sid_hint != 0 {
            let k = clan_read_key(c, token, sid_hint)?;
            return Ok(cl::load(c, &k)?);
        }
        let k = clan_key_r(c, token)?;
        Ok(cl::load(c, &k)?)
    })
}

// py: daemon.py:1682 clan_side
pub fn clan_side(token: &str, faction: i64) -> DbR<Value> {
    let (st, ok) = db::with_write(|c| {
        let key = clan_key_r(c, token)?;
        let (st, ok) = cl::choose_side(c, &key, faction)?;
        if ok {
            let k2 = clan_key_r(c, token)?;
            if let Err(e) = clan_story::on_new_run(c, &k2, faction) {
                dlog!("CLAN story (new run) failed", e);
            }
        }
        Ok((st, ok))
    })?;
    let fname = cl::faction_name(faction);
    let fshow = if (0..=2).contains(&faction) { fname.to_string() } else { faction.to_string() }; // FACTION_NAMES.get(faction, faction)
    dlog!("CLAN side", tag(token), fshow, if ok { "ok" } else { "refused (clan alive)" });
    if ok {
        let r = db::with_write(|c| {
            let k = clan_key_r(c, token)?;
            let got = achievements::book_side(c, &k, None);
            ach_log(token, &got);
            Ok(())
        });
        if let Err(e) = r {
            dlog!("ACH side failed", e);
        }
    }
    Ok(st)
}

// py: daemon.py:1688 clan_match_facts
/// What the clan chronicle / saga may tell about the player's match: the map, supplies, parts, award counts, the result.
pub fn clan_match_facts(token: &str, mid: &str, r: &Value, parts: i64, counts: &IndexMap<i64, i64>) -> Value {
    let mut m = Map::new();
    m.insert("map".into(), Value::String(CLAN_MAP.lock().get(mid).cloned().unwrap_or_default()));
    m.insert("supplies".into(), Value::from(cl::gi(r, "supplies", 0)));
    m.insert("parts".into(), Value::from(parts));
    m.insert("kills".into(), Value::from(counts.get(&1).copied().unwrap_or(0)));
    m.insert("mid".into(), Value::String(mid.to_string()));
    if !counts.is_empty() {
        for (lab, key) in [(2, "execs"), (3, "revives"), (4, "headshots"), (5, "marked"), (21, "downs"), (22, "deaths"), (23, "streak")] {
            if counts.contains_key(&lab) || lab <= 5 {
                m.insert(key.into(), Value::from(counts.get(&lab).copied().unwrap_or(0)));
            }
        }
        let w = counts.get(&24).copied().unwrap_or(0);
        if 0 < w && (w as usize) < CLAN_WEAPONS.len() {
            m.insert("weapon".into(), Value::String(CLAN_WEAPONS[w as usize].to_string()));
        }
    }
    if let Some(Some(b)) = CLAN_WIN.lock().shift_remove(&clan_tk(token, mid)) {
        m.insert("win".into(), Value::Bool(b));
    }
    Value::Object(m)
}

// py: daemon.py:1719 book_career_stats
/// Career totals for the website profile: the award counts of each booked match report (booked once per match).
pub fn book_career_stats(token: &str, counts: &IndexMap<i64, i64>) {
    let r = db::with_write(|c| {
        c.execute_batch(
            "CREATE TABLE IF NOT EXISTS pstats(token TEXT PRIMARY KEY, kills INT DEFAULT 0, execs INT DEFAULT 0, revives INT DEFAULT 0,
                headshots INT DEFAULT 0, marked INT DEFAULT 0, downs INT DEFAULT 0, deaths INT DEFAULT 0, streak INT DEFAULT 0,
                reports INT DEFAULT 0, updated REAL)",
        )?;
        c.execute_batch("CREATE TABLE IF NOT EXISTS pweapon(token TEXT, weapon INT, n INT DEFAULT 0, PRIMARY KEY(token, weapon))")?;
        let g = |k: i64| counts.get(&k).copied().unwrap_or(0);
        let v = [g(1), g(2), g(3), g(4), g(5), g(21), g(22)];
        let st = g(23);
        c.execute(
            "INSERT INTO pstats(token,kills,execs,revives,headshots,marked,downs,deaths,streak,reports,updated) VALUES(?,?,?,?,?,?,?,?,?,1,?) \
             ON CONFLICT(token) DO UPDATE SET kills=kills+excluded.kills, execs=execs+excluded.execs, revives=revives+excluded.revives, \
             headshots=headshots+excluded.headshots, marked=marked+excluded.marked, downs=downs+excluded.downs, deaths=deaths+excluded.deaths, \
             streak=MAX(streak, excluded.streak), reports=reports+1, updated=excluded.updated",
            rusqlite::params![token, v[0], v[1], v[2], v[3], v[4], v[5], v[6], st, clock::now()],
        )?;
        let w = g(24);
        if 0 < w && (w as usize) < CLAN_WEAPONS.len() {
            c.execute("INSERT INTO pweapon(token,weapon,n) VALUES(?,?,1) ON CONFLICT(token,weapon) DO UPDATE SET n=n+1", rusqlite::params![token, w])?;
        }
        Ok(())
    });
    if let Err(e) = r {
        dlog!("STATS career record failed", e);
    }
}

// ---------------------------------------------------------------- achievements glue (daemon.py:1753-1938)
/// what a booked clan day passes on to a later 0xA9 (ACH_DAY value)
#[derive(Clone)]
pub struct AchDay {
    pub e2: IndexMap<i64, i64>,
    pub win: Option<bool>,
    pub left: bool,
    pub late: bool,
}
pub static ACH_DONE: LazyLock<Mutex<IndexMap<(String, String), HashMap<i64, i64>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub static ACH_PEND: LazyLock<Mutex<IndexMap<(String, String), IndexMap<i64, i64>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub static ACH_DAY: LazyLock<Mutex<IndexMap<(String, String), AchDay>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// labels the clamped 0xE2 report is the authority for
pub const ACH_E2_ONLY: [i64; 11] = [1, 2, 3, 4, 5, 6, 21, 22, 23, 24, 33];
pub static ACH_LIVE_AT: LazyLock<Mutex<IndexMap<String, f64>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub const ACH_LIVE_GAP: f64 = 2.0;

// py: daemon.py:1767 _ach_trim
pub fn ach_trim<K: std::hash::Hash + Eq, V>(d: &mut IndexMap<K, V>) {
    while d.len() > 3000 {
        d.shift_remove_index(0);
    }
}

// py: daemon.py:1772 ach_log
pub fn ach_log(token: &str, got: &[String]) {
    if !got.is_empty() {
        let t = tag(token);
        let parts: Vec<String> = got.iter().map(|a| format!("{} ({})", a, achievements::by_id(a).map(|x| x.title.as_str()).unwrap_or(""))).collect();
        dlog!("ACH unlocked", t, parts.join(", "));
        evt!("ach", t, py::list_str(got));
    }
}

fn dict_repr(m: &IndexMap<i64, i64>) -> String {
    format!("{{{}}}", m.iter().map(|(k, v)| format!("{}: {}", k, v)).collect::<Vec<_>>().join(", "))
}

// py: daemon.py:1778 ach_merge
pub fn ach_merge(e2: &IndexMap<i64, i64>, a9: Option<&IndexMap<i64, i64>>) -> IndexMap<i64, i64> {
    let mut m: IndexMap<i64, i64> = e2.clone();
    if let Some(a9) = a9 {
        for (k, v) in a9 {
            if ACH_E2_ONLY.contains(k) {
                continue;
            }
            let cur = m.get(k).copied().unwrap_or(0);
            m.insert(*k, cur.max(*v));
        }
    }
    m
}

/// `done = ACH_DONE.setdefault((token, mid), {})`, copied out (the caller writes it back with `ach_done_put`)
fn ach_done_take(token: &str, mid: &str) -> HashMap<i64, i64> {
    ACH_DONE.lock().entry(clan_tk(token, mid)).or_default().clone()
}

fn ach_done_put(token: &str, mid: &str, d: HashMap<i64, i64>) {
    ACH_DONE.lock().insert(clan_tk(token, mid), d);
}

// py: daemon.py:1787 ach_book_day
/// The clan day of (token, mid) was booked: credit the match (0xE2 counts + a 0xA9 that came first) and the clan day.
#[allow(clippy::too_many_arguments)]
pub fn ach_book_day(token: &str, mid: &str, counts: &IndexMap<i64, i64>, win: Option<bool>, left: bool, late: bool, before: Option<&Value>, after: &Value, r: Option<&Value>) {
    let res = (|| -> DbR<Vec<String>> {
        let mut done = ach_done_take(token, mid);
        let mut out: DbR<Vec<String>> = Ok(Vec::new());
        let r2 = db::with_write(|c| {
            let key = clan_key_r(c, token)?;
            let first = !ACH_DAY.lock().contains_key(&clan_tk(token, mid));
            ACH_DAY.lock().insert(clan_tk(token, mid), AchDay { e2: counts.clone(), win, left, late });
            let auth: HashMap<i64, i64> = counts.iter().map(|(k, v)| (*k, *v)).collect();
            achievements::settle(c, &key, &auth, &mut done, &ACH_E2_ONLY);
            let pend = ACH_PEND.lock().shift_remove(&clan_tk(token, mid));
            let mut got = achievements::book_match(c, &key, &ach_merge(counts, pend.as_ref()), &mut done, win, left, late, first, None);
            if let Some(r) = r.filter(|r| cl::truthy(r)) {
                for a in achievements::book_clan(c, &key, before, Some(after), Some(r), None, None) {
                    if !got.contains(&a) {
                        got.push(a);
                    }
                }
            }
            out = Ok(got);
            Ok(())
        });
        ach_done_put(token, mid, done);
        r2?;
        out
    })();
    match res {
        Ok(got) => {
            ach_trim(&mut ACH_DONE.lock());
            ach_trim(&mut ACH_DAY.lock());
            ach_log(token, &got);
        }
        Err(e) => dlog!("ACH day failed", e),
    }
}

// ---- CPython's set of small ints (iteration order of `set(a) | set(b)`): used only for the order of ACH_PEND's keys
struct PySet {
    table: Vec<Option<i64>>,
    fill: usize,
}

impl PySet {
    fn new() -> PySet {
        PySet { table: vec![None; 8], fill: 0 }
    }
    fn mask(&self) -> usize {
        self.table.len() - 1
    }
    fn insert_clean(table: &mut [Option<i64>], key: i64) {
        let mask = table.len() - 1;
        let mut i = (key as usize) & mask;
        let mut perturb = key as usize;
        loop {
            let probes = if i + 9 <= mask { 9 } else { 0 };
            for j in 0..=probes {
                if table[i + j].is_none() {
                    table[i + j] = Some(key);
                    return;
                }
            }
            perturb >>= 5;
            i = (i.wrapping_mul(5).wrapping_add(1).wrapping_add(perturb)) & mask;
        }
    }
    fn resize(&mut self, minused: usize) {
        let mut n = 8;
        while n <= minused {
            n <<= 1;
        }
        let mut t = vec![None; n];
        for k in self.table.iter().flatten() {
            PySet::insert_clean(&mut t, *k);
        }
        self.table = t;
    }
    fn used(&self) -> usize {
        self.table.iter().flatten().count()
    }
    fn add(&mut self, key: i64) {
        let mask = self.mask();
        let mut i = (key as usize) & mask;
        let mut perturb = key as usize;
        loop {
            let probes = if i + 9 <= mask { 9 } else { 0 };
            for j in 0..=probes {
                match self.table[i + j] {
                    None => {
                        self.table[i + j] = Some(key);
                        self.fill += 1;
                        if self.fill * 5 < mask * 3 {
                            return;
                        }
                        let u = self.used();
                        self.resize(if u > 50000 { u * 2 } else { u * 4 });
                        return;
                    }
                    Some(k) if k == key => return,
                    _ => {}
                }
            }
            perturb >>= 5;
            i = (i.wrapping_mul(5).wrapping_add(1).wrapping_add(perturb)) & mask;
        }
    }
    /// `set(dict)`
    fn from_dict(keys: &[i64]) -> PySet {
        let mut s = PySet::new();
        if (s.fill + keys.len()) * 5 >= s.mask() * 3 {
            let u = s.used();
            s.resize((u + keys.len()) * 2);
        }
        for k in keys {
            s.add(*k);
        }
        s
    }
    /// `self.update(other)` (set_merge)
    fn merge(&mut self, other: &PySet) {
        let ou = other.used();
        if ou == 0 {
            return;
        }
        if (self.fill + ou) * 5 >= self.mask() * 3 {
            let u = self.used();
            self.resize((u + ou) * 2);
        }
        if self.fill == 0 {
            if self.mask() == other.mask() {
                self.table = other.table.clone();
                self.fill = ou;
            } else {
                for k in other.table.iter().flatten() {
                    PySet::insert_clean(&mut self.table, *k);
                }
                self.fill = ou;
            }
            return;
        }
        for k in other.table.iter().flatten() {
            self.add(*k);
        }
    }
    fn order(&self) -> Vec<i64> {
        self.table.iter().flatten().copied().collect()
    }
}

/// `list(set(a) | set(b))` in CPython's iteration order
fn set_union_order(a: &[i64], b: &[i64]) -> Vec<i64> {
    let sa = PySet::from_dict(a);
    let sb = PySet::from_dict(b);
    let mut res = PySet::new();
    res.merge(&sa);
    res.merge(&sb);
    res.order()
}

// py: daemon.py:1807 ach_report_op
/// Op 0xA9 [64B token][u32 seq][u8 n][n x (u8 label, u16 count)] -> reply 0xA9 = the player's achievement text.
pub fn ach_report_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 69 {
        return None;
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return None;
    }
    let mut a9: IndexMap<i64, i64> = IndexMap::new();
    for k in 0..(pl[68] as usize).min(80) {
        let o = 69 + 3 * k;
        if pl.len() >= o + 3 {
            a9.insert(pl[o] as i64, u16::from_le_bytes([pl[o + 1], pl[o + 2]]) as i64);
        }
    }
    let cur = CLAN_MATCH.lock().get(&token).cloned();
    let mid: Option<String> = cur.clone().filter(|m| !m.is_empty()).or_else(|| CLAN_LAST_MATCH.lock().get(&token).cloned());
    let last_priv = PRIV_LAST_MID.lock().get(&token).cloned().unwrap_or_default();
    if cur.as_deref().is_none_or(|m| m.is_empty()) && PRIV.is_private_mid(&last_priv) {
        dlog!("ACH report from a private match", tag(&token), "- not booked (private matches have no clan / stats effects)");
        return Some(ach_read(&token, 0, 0xA9));
    }
    let mut got: Vec<String> = Vec::new();
    let res = (|| -> DbR<()> {
        match mid.as_deref().filter(|m| !m.is_empty()) {
            Some(mid) if AC.as_ref().is_none_or(|a| a.rate_ok(&token)) => {
                let day = ACH_DAY.lock().get(&clan_tk(&token, mid)).cloned();
                match day {
                    None => {
                        let old = ACH_PEND.lock().get(&clan_tk(&token, mid)).cloned().unwrap_or_default();
                        let ka: Vec<i64> = a9.keys().copied().collect();
                        let ko: Vec<i64> = old.keys().copied().collect();
                        let mut np: IndexMap<i64, i64> = IndexMap::new();
                        for k in set_union_order(&ka, &ko) {
                            np.insert(k, a9.get(&k).copied().unwrap_or(0).max(old.get(&k).copied().unwrap_or(0)));
                        }
                        {
                            let mut p = ACH_PEND.lock();
                            p.insert(clan_tk(&token, mid), np);
                            ach_trim(&mut p);
                        }
                        dlog!("ACH report", tag(&token), "mid", mid, "held until the day books:", dict_repr(&a9));
                    }
                    Some(day) => {
                        let mut done = ach_done_take(&token, mid);
                        let mut out = Vec::new();
                        let r2 = db::with_write(|c| {
                            let key = clan_key_r(c, &token)?;
                            out = achievements::book_match(c, &key, &ach_merge(&day.e2, Some(&a9)), &mut done, day.win, day.left, day.late, false, None);
                            Ok(())
                        });
                        ach_done_put(&token, mid, done);
                        r2?;
                        got = out;
                        dlog!("ACH report", tag(&token), "mid", mid, "booked:", dict_repr(&a9));
                    }
                }
            }
            None => dlog!("ACH report without a match", tag(&token)),
            Some(_) => dlog!("ACH report refused: report flood", tag(&token)),
        }
        ach_log(&token, &got);
        Ok(())
    })();
    if let Err(e) = res {
        dlog!("ACH report failed", e);
    }
    Some(ach_read(&token, 0, 0xA9))
}

// py: daemon.py:1834 ach_live_op
/// Op 0xAB [64B token][u8 n][n x (u8 label, u16 count)]: the ext's counters of the match it is in, booked at once -> reply 0xAB = the table.
pub fn ach_live_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 65 {
        return None;
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return None;
    }
    let mut cnt: IndexMap<i64, i64> = IndexMap::new();
    for k in 0..(pl[64] as usize).min(80) {
        let o = 65 + 3 * k;
        if pl.len() >= o + 3 {
            cnt.insert(pl[o] as i64, u16::from_le_bytes([pl[o + 1], pl[o + 2]]) as i64);
        }
    }
    let mid = CLAN_MATCH.lock().get(&token).cloned().filter(|m| !m.is_empty());
    let now = clock::now();
    let res = (|| -> DbR<()> {
        if let Some(mid) = &mid {
            let booked = ACH_DAY.lock().contains_key(&clan_tk(&token, mid));
            let last = ACH_LIVE_AT.lock().get(&token).copied().unwrap_or(0.0);
            if !booked && now - last >= ACH_LIVE_GAP {
                {
                    let mut l = ACH_LIVE_AT.lock();
                    l.insert(token.clone(), now);
                    ach_trim(&mut l);
                }
                let mut done = ach_done_take(&token, mid);
                let mut got = Vec::new();
                let r2 = db::with_write(|c| {
                    let key = clan_key_r(c, &token)?;
                    got = achievements::book_live(c, &key, &cnt, &mut done, None);
                    Ok(())
                });
                ach_done_put(&token, mid, done);
                r2?;
                ach_trim(&mut ACH_DONE.lock());
                dlog!("ACH live", tag(&token), "mid", mid, "booked:", dict_repr(&cnt));
                ach_log(&token, &got);
            }
        }
        Ok(())
    })();
    if let Err(e) = res {
        dlog!("ACH live failed", e);
    }
    Some(ach_read(&token, 0, 0xAB))
}

// py: daemon.py:1873 ach_read
pub fn ach_read(token: &str, sid_hint: i64, op: u8) -> Vec<u8> {
    let r = db::with_write(|c| {
        let key = clan_read_key(c, token, sid_hint)?;
        let row = cl::load(c, &key)?;
        let weeks = cl::gi(&row, "days_total", 0) / 7;
        let mut txt = achievements::wire_text(c, &key, Some(weeks));
        txt.push_str("LIVE 1\n"); // (this daemon takes op 0xAB; a line without '|' fields, skipped by every reader)
        Ok(txt)
    });
    let txt = match r {
        Ok(t) => t,
        Err(e) => {
            dlog!("ACH read failed", e);
            String::new()
        }
    };
    msg(op, txt.as_bytes())
}

// py: daemon.py:1886 ach_read_op
/// Op 0xAA [64B token][optional 8B steamid hint, the launcher's] -> reply 0xAA (achievements.wire_text). Read only.
pub fn ach_read_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 64 {
        return None;
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return None;
    }
    let hint = if pl.len() >= 72 { u64::from_le_bytes(pl[64..72].try_into().unwrap_or([0; 8])) as i64 } else { 0 };
    Some(ach_read(&token, hint, 0xAA))
}

// ---------------------------------------------------------------- the day (daemon.py:1896-2022)
fn s2(token: &str, mid: &str, f: &Mutex<HashSet<(String, String)>>) -> bool {
    f.lock().contains(&clan_tk(token, mid))
}

fn drawn_repr(d: &[(&'static str, i64)]) -> String {
    format!("[{}]", d.iter().map(|(c, n)| format!("('{}', {})", c, n)).collect::<Vec<_>>().join(", "))
}

// py: daemon.py:1896 clan_finalize
/// Resolve the token's pending report (if any): match end seen = normal day, else left early.
pub fn clan_finalize(token: &str) -> DbR<()> {
    let p = {
        let _g = CLANLOCK.lock();
        CLAN_PENDING.lock().remove(token)
    };
    let Some(p) = p else { return Ok(()) };
    let (mid, mut parts, mut flags, mut counts) = p.clone();
    let mid = mid.as_str();
    let mvt = CLAN_MOVED.lock().get(&clan_tk(token, mid)).copied();
    if mvt.is_some_and(|t| t != 0.0) && flags & 2 == 0 {
        // user 10-20: a team move is no leave. The report of the first half waits while the player is back in the match (or its move is still open)
        let back_in = MM.in_match(token) && MM.mid_of(token).as_deref() == Some(mid);
        let ended = CLAN_ENDED.lock().contains_key(mid);
        if !ended && (back_in || clock::now() - mvt.unwrap_or(0.0) < 200.0) {
            {
                let _g = CLANLOCK.lock();
                CLAN_PENDING.lock().entry(token.to_string()).or_insert(p);
            }
            CLAN_DUE.lock().insert(token.to_string(), clock::now() + 10.0);
            return Ok(());
        }
        CLAN_EXCUSED.lock().insert(clan_tk(token, mid));
    }
    CLAN_MOVED.lock().remove(&clan_tk(token, mid));
    if flags & 2 == 0 {
        let t0 = CLAN_REJOINED.lock().get(&clan_tk(token, mid)).copied();
        let dep = MM.lock.lock().departed.get(mid).is_some_and(|s| s.contains(token));
        if CLAN_LEFT_AT.lock().contains_key(&clan_tk(token, mid))
            && MM.in_match(token)
            && MM.mid_of(token).as_deref() == Some(mid)
            && !dep
            && t0.is_none_or(|t| clock::now() - t < *CLAN_REJOIN_MAX_S)
        {
            // left, then came back into the same match: it plays on for full credit - the verdict waits for its next leave or the end report
            if t0.is_none() {
                CLAN_REJOINED.lock().insert(clan_tk(token, mid), clock::now());
                dlog!("CLAN rejoin", tag(token), "mid", mid, "is back in the match - report held, full credit (the next leave / the end decides)");
            }
            CLAN_EXCUSED.lock().remove(&clan_tk(token, mid));
            CLAN_EXCUSE_WHY.lock().remove(&clan_tk(token, mid));
            {
                let _g = CLANLOCK.lock();
                CLAN_PENDING.lock().entry(token.to_string()).or_insert(p);
            }
            CLAN_DUE.lock().insert(token.to_string(), clock::now() + 10.0);
            return Ok(());
        }
    }
    CLAN_DONE.lock().insert(token.to_string(), mid.to_string()); // this match id is booked (a re-sent report is acknowledged, not booked again)
    if s2(token, mid, &CLAN_CREDITED) {
        dlog!("CLAN day", tag(token), "mid", mid, "already credited for this match (it left, came back and reported again) - ignored");
        return Ok(());
    }
    let mut left = (!CLAN_ENDED.lock().contains_key(mid) && flags & 2 == 0) || s2(token, mid, &CLAN_QUITTERS);
    if !left && flags & 2 == 0 && s2(token, mid, &CLAN_EXCUSED) {
        let la = CLAN_LEFT_AT.lock().get(&clan_tk(token, mid)).copied().unwrap_or(1e18);
        let en = CLAN_ENDED.lock().get(mid).copied().unwrap_or(0.0);
        if la <= en + 2.0 {
            // 11-23 FKZB: an excused leave (host lost) was itself the last departure, so the abort award ended the match in the same second
            left = true;
        }
    }
    if left && flags & 4 != 0 && !s2(token, mid, &CLAN_QUITTERS) && !s2(token, mid, &CLAN_EXCUSED) {
        // (the flag is the client's own word - pulling the cable sets it too: at most CLAN_LINK_EXCUSE_MAX per day and token)
        let now_ = clock::now();
        let ex: Vec<f64> = CLAN_LINK_EXCUSES.lock().get(token).map(|v| v.iter().copied().filter(|t| now_ - t < CLAN_LINK_EXCUSE_WINDOW).collect()).unwrap_or_default();
        if ex.len() >= CLAN_LINK_EXCUSE_MAX {
            let n = ex.len();
            CLAN_LINK_EXCUSES.lock().insert(token.to_string(), ex);
            dlog!("CLAN excuse", tag(token), "mid", mid, format!("link-lost flag refused - {} such excuses in the last day already", n));
            flags &= !4;
        }
    }
    if left && flags & 4 != 0 && !s2(token, mid, &CLAN_QUITTERS) && !s2(token, mid, &CLAN_EXCUSED) {
        CLAN_LINK_EXCUSES.lock().entry(token.to_string()).or_default().push(clock::now());
        // R-2VAZ 10-24: the player's own link died (report flag 4): a dropped link is no leave, same excuse as a host that was lost
        CLAN_EXCUSED.lock().insert(clan_tk(token, mid));
        dlog!("CLAN excuse", tag(token), "mid", mid, "its own link was lost (report flag 4) - no left-early penalty");
    }
    if left && !s2(token, mid, &CLAN_QUITTERS) {
        let rt = RESTORED_T.lock().get(mid).copied();
        if clock::now() - rt.unwrap_or(-1e9) < *RESTORE_EXCUSE_S {
            // 10-26 (PT92 live): a report within RESTORE_EXCUSE_S of a restored match's re-adoption is ambiguous (restart or deliberate leave): excused
            CLAN_EXCUSED.lock().insert(clan_tk(token, mid));
            dlog!("CLAN excuse", tag(token), "mid", mid, format!("report within {} s of the daemon restart that restored this match - no left-early penalty", py::fixed(clock::now() - rt.unwrap_or(0.0), 0)));
        }
    }
    let mut forgive_excused = false;
    if left && *CLAN_NO_PENALTY {
        // 11-15: a server without clan penalties (test server) - every early leave is excused
        if parts <= 0 {
            dlog!("CLAN day", tag(token), "mid", mid, "left early - no clan penalties on this server: nothing earned, no day, no penalty");
            return Ok(());
        }
        left = false;
        forgive_excused = true;
        dlog!("CLAN day", tag(token), "mid", mid, "left early - no clan penalties on this server: parts count, deficit forgiven");
    }
    let kicked = CLAN_KICKED.lock().get(&clan_tk(token, mid)).cloned();
    if let Some(why) = kicked {
        if parts <= 0 {
            dlog!("CLAN day", tag(token), "mid", mid, format!("kicked ({}) - nothing earned, no day, no penalty", why));
            return Ok(());
        }
        left = false;
        forgive_excused = true;
        dlog!("CLAN day", tag(token), "mid", mid, format!("kicked ({}) - parts count, deficit forgiven", why));
    }
    let aborted = CLAN_ABORTED.lock().get(&clan_tk(token, mid)).cloned();
    if let Some(why) = aborted {
        if !s2(token, mid, &CLAN_QUITTERS) && !CLAN_KICKED.lock().contains_key(&clan_tk(token, mid)) {
            if parts <= 0 {
                dlog!("CLAN day", tag(token), "mid", mid, format!("match ended for it ({}) - nothing earned, no day, no penalty", why));
                return Ok(());
            }
            left = false;
            forgive_excused = true;
            dlog!("CLAN day", tag(token), "mid", mid, format!("match ended for it ({}) - parts count, deficit forgiven", why));
        }
    }
    if left && s2(token, mid, &CLAN_EXCUSED) && !s2(token, mid, &CLAN_QUITTERS) {
        let why = CLAN_EXCUSE_WHY.lock().get(&clan_tk(token, mid)).cloned().unwrap_or_else(|| "left after another player left / crashed".to_string());
        if parts <= 0 {
            dlog!("CLAN day", tag(token), "mid", mid, format!("excused ({}) - nothing earned, no day, no penalty", why));
            return Ok(());
        }
        left = false;
        forgive_excused = true;
        dlog!("CLAN day", tag(token), "mid", mid, format!("excused ({}) - parts count, deficit forgiven", why));
    }
    let (dup, prev, tid): (bool, Option<Value>, Option<String>) = match db::with_write(|c| {
        let t = clan_key_r(c, token)?;
        let dup = cl::load(c, &t)?.get("last_mid").and_then(|v| v.as_str()) == Some(mid);
        let prev = cl::snap_get(c, &t, mid)?;
        Ok((dup, prev, t))
    }) {
        Ok((d, p2, t)) => (d, p2, Some(t)),
        Err(_) => (false, None, None),
    };
    let rebook = dup
        && prev.as_ref().is_some_and(|p| {
            cl::truthy(p) && p.get("stint").is_some_and(cl::truthy) && (CLAN_REBOOK.lock().contains(&clan_tk(token, mid)) || p.get("rejoined").is_some_and(cl::truthy))
        });
    if dup && !rebook {
        dlog!("CLAN day", tag(token), "mid", mid, "already booked - ignored (no second parts record)");
        return Ok(());
    }
    book_career_stats(token, &counts); // website career totals (booked once per match)
    let mut late: i64 = 0;
    let mut mult0: f64 = 1.0;
    if rebook {
        // R-G6L3 11-03: the player left this match, joined it again and played on. The report of the second stint REPLACES the first booking
        let prev = prev.clone().unwrap_or(Value::Null);
        let st1 = prev.get("stint").cloned().unwrap_or(Value::Null);
        let mut merged: IndexMap<i64, i64> = IndexMap::new();
        if let Some(o) = st1.get("counts").and_then(|c| c.as_object()) {
            for (k, v) in o {
                merged.insert(k.trim().parse().unwrap_or(0), cl::int_of(v));
            }
        }
        for (k, v) in counts.iter() {
            if *k == 23 {
                // best kill streak: the better stint
                let cur = merged.get(k).copied().unwrap_or(0);
                merged.insert(*k, cur.max(*v));
            } else if *k == 24 {
                // weapon class with the most downs: the first stint's, else this one's
                let cur = merged.get(k).copied().unwrap_or(0);
                merged.insert(*k, if cur != 0 { cur } else { *v });
            } else {
                let cur = merged.get(k).copied().unwrap_or(0);
                merged.insert(*k, cur + *v);
            }
        }
        parts = (parts + cl::gi(&st1, "parts", 0)).clamp(0, 100000).max(0);
        counts = merged;
        late = st1.get("late").map(cl::int_of).unwrap_or(0);
        mult0 = st1.get("mult").map(value_f64).unwrap_or(1.0);
        forgive_excused = forgive_excused || st1.get("forgive").is_some_and(cl::truthy);
        MM.late_mult_take(token, mid);
        MM.late_bonus_take(token, mid); // (the first booking used them; its bonus / multiplier is reused below)
        let t = tid.clone().unwrap_or_default();
        db::with_write(|c| Ok(cl::snap_restore(c, &t, token, &prev)?))?;
        dlog!("CLAN day", tag(token), "mid", mid, format!("second stint: the first booking is undone, the whole match ({} parts) books once", parts));
    } else if let Some(t) = tid.clone() {
        if let Err(e) = db::with_write(|c| Ok(cl::snap_take(c, &t, mid, token)?)) {
            dlog!("CLAN snapshot failed", e); // a snapshot failure never blocks a day
        }
    }
    let raw_parts = parts;
    let raw_counts = counts.clone();
    if !left {
        if let Err(e) = db::with_write(|c| {
            c.execute("UPDATE players SET parts_total = parts_total + ?, parts_n = parts_n + 1 WHERE token = ?", rusqlite::params![parts, token])?;
            Ok(())
        }) {
            dlog!("SKILL parts record failed", e);
        }
    }
    CLAN_CREDITED.lock().insert(clan_tk(token, mid));
    {
        // the per-match player stats of the clan page's RAIDS tab: this player's own award counts
        let gn = get_name(token);
        let nm = if !gn.is_empty() { gn } else { token_name(token).filter(|n| !n.is_empty()).unwrap_or_else(|| "?".to_string()) };
        let team = CLAN_TEAM.lock().get(mid).and_then(|m| m.get(token)).copied().unwrap_or(-1);
        let cmap: cl::Counts = counts.iter().map(|(k, v)| (*k, *v)).collect();
        if let Err(e) = db::with_write(|c| {
            clan_ingame::stats_report(c, mid, token, &nm, team, Some(&cmap), left)?;
            clan_ingame::stats_parts(c, mid, token, parts);
            Ok(())
        }) {
            dlog!("CLAN match stats failed", e); // only the stats page; the clan day never depends on it
        }
    }
    let mult = if rebook { mult0 } else { MM.late_mult_take(token, mid) };
    if !left {
        late = if rebook { late } else { MM.late_bonus_take(token, mid) };
        if late != 0 {
            parts += late;
            dlog!("CLAN late-join bonus", tag(token), format!("+{} parts", late));
        }
    }
    if mult > 1.0 && !left {
        dlog!("CLAN late-join multiplier", tag(token), format!("x{} (a day above x{} does not count, a deficit is forgiven)", py::fixed(mult, 2), py::fixed(cl::DAY_MULT_LIMIT, 1)));
    }
    let ach_win: Option<Option<bool>> = CLAN_WIN.lock().get(&clan_tk(token, mid)).copied(); // (clan_match_facts pops it below)
    let ach_late = late != 0 || mult > 1.0;
    let cmap: cl::Counts = counts.iter().map(|(k, v)| (*k, *v)).collect();
    let (before, st, r) = db::with_write(|c| {
        let before = Some(cl::load(c, &clan_key_r(c, token)?)?);
        let key = clan_key_r(c, token)?;
        let bottles = if left { 0 } else { counts.get(&33).copied().unwrap_or(0).clamp(0, 200).max(0) };
        let (st, mut r) = cl::apply_match(c, &key, mid, parts, left, Some(&cmap), if !left { mult } else { 1.0 }, (mult > 1.0 && !left) || forgive_excused, bottles, false)?; // label 33 = supply bottles picked up; TEMPORARY: protect (daemon.py CLAN_FAIR) is wired with the dispatcher pass
        CLAN_REBOOK.lock().remove(&clan_tk(token, mid));
        let k2 = clan_key_r(c, token)?;
        // this booking's raw numbers, for a later stint of the same match
        let stint = {
            let mut cn = Map::new();
            for (k, v) in &raw_counts {
                cn.insert(k.to_string(), Value::from(*v));
            }
            let mut m = Map::new();
            m.insert("parts".into(), Value::from(raw_parts));
            m.insert("counts".into(), Value::Object(cn));
            m.insert("late".into(), Value::from(if left { 0 } else { late }));
            m.insert("mult".into(), serde_json::Number::from_f64(mult).map(Value::Number).unwrap_or(Value::Null));
            m.insert("forgive".into(), Value::Bool(forgive_excused || (mult > 1.0 && !left)));
            Value::Object(m)
        };
        if let Err(e) = cl::snap_update(c, &k2, mid, &[("rejoined", Value::Bool(false)), ("stint", stint)]) {
            dlog!("CLAN snapshot meta failed", e);
        }
        if let Some(rv) = r.as_mut().filter(|r| cl::truthy(r)) {
            if !left && CLAN_MAP.lock().get(mid).is_some_and(|m| !m.is_empty()) {
                // what the clan chronicle may tell about the match: the map, the supplies / parts of the day and the Downed Enemy count
                let facts = clan_match_facts(token, mid, rv, parts, &counts);
                if let Some(o) = rv.as_object_mut() {
                    o.insert("match".into(), facts);
                }
            }
        }
        if let Some(rv) = r.as_ref().filter(|r| cl::truthy(r)) {
            if let Err(e) = clan_story::on_day(c, &k2, &st, rv) {
                dlog!("CLAN story (day) failed", e); // the feed never breaks the clan
            }
            match clan_perks::on_day(c, &k2, &st, rv, Some(&cmap), rv.get("match")) {
                Ok(drawn) => {
                    if !drawn.is_empty() {
                        dlog!("clanperk: drawn from the stock", tag(token), "mid", mid, drawn_repr(&drawn));
                    }
                }
                Err(e) => dlog!("CLAN perks (day) failed", e),
            }
        }
        Ok((before, st, r))
    })?;
    dlog!("CLAN day", tag(token), "mid", mid, "parts", parts, if left { "left" } else { "" }, "->", r.as_ref().map(py::pyrepr).unwrap_or_else(|| "None".to_string()), "state", cl::state_text(&st));
    let win = match ach_win {
        Some(Some(b)) => Some(b),
        _ => None,
    };
    ach_book_day(token, mid, &counts, win, left, ach_late, before.as_ref(), &st, r.as_ref());
    Ok(())
}

// py: daemon.py:2024 clan_scheduler
/// One pass of the scheduler thread: finalise every pending report whose time came (in CLAN_DUE order).
pub fn clan_scheduler_tick(now: f64) {
    let due: Vec<String> = CLAN_DUE.lock().iter().filter(|(_, d)| **d <= now).map(|(t, _)| t.clone()).collect();
    for t in due {
        CLAN_DUE.lock().shift_remove(&t);
        if let Err(e) = clan_finalize(&t) {
            dlog!("CLAN scheduler", e);
        }
    }
}

/// `while True: time.sleep(0.25); ...` (the integration starts it; clan_report starts it on the first report that has to wait)
pub fn clan_scheduler() {
    loop {
        std::thread::sleep(std::time::Duration::from_millis(250));
        clan_scheduler_tick(clock::now());
    }
}

// py: daemon.py:2213 clan_report
/// Op 0xE2. Returns the state to reply with (before the day resolves if it has to wait).
pub fn clan_report(token: &str, parts: i64, flags: i64, counts: IndexMap<i64, i64>) -> DbR<Value> {
    readopt_claim(token);
    let mid = CLAN_MATCH.lock().get(token).cloned().filter(|m| !m.is_empty());
    if AC.as_ref().is_some_and(|a| !a.rate_ok(token)) {
        dlog!("AC clan report refused: report flood", tag(token));
        return clan_state(token, 0);
    }
    let last_priv = PRIV_LAST_MID.lock().get(token).cloned().unwrap_or_default();
    if mid.is_none() && PRIV.is_private_mid(&last_priv) {
        dlog!("CLAN report from a private match", tag(token), "parts", parts, "- not booked (private matches have no clan effects)");
        return clan_state(token, 0);
    }
    let Some(mid) = mid else {
        dlog!("CLAN report without a started match", tag(token), "parts", parts);
        if AC.is_some() {
            let lm = CLAN_LAST_MATCH.lock().get(token).cloned();
            ac_server_event(token, lm.as_deref(), S_ORPHAN, 0.5, "clan report with no started match");
        }
        return clan_state(token, 0);
    };
    if CLAN_DONE.lock().get(token).is_some_and(|m| *m == mid) {
        // a re-send: the client never saw our reply to the first report (tunnel drop / restart) - answer only
        dlog!("CLAN report re-sent for an already booked match", tag(token), "mid", mid, "- acknowledged, not booked again");
        return clan_state(token, 0);
    }
    let mut parts = parts.clamp(0, 100000).max(0);
    let mut counts = counts;
    if let Some(ac) = AC.as_ref() {
        // anti-cheat (10-25): clamp parts / counts to what the match length allows; findings are scored once per distinct report (the ext re-sends)
        let fresh = ac.first_time(token, &mid, parts, &counts);
        let (p2, c2, mut issues) = ac.check_report(token, &mid, parts, counts, flags);
        parts = p2;
        counts = c2;
        if fresh {
            issues.extend(ac.note_report(token, &mid, parts, &counts));
            for (kind, w, ev) in issues {
                ac_server_event(token, Some(&mid), kind, w, &ev);
            }
        }
    }
    {
        let _g = CLANLOCK.lock();
        let old = CLAN_PENDING.lock().get(token).cloned();
        let moved = CLAN_MOVED.lock().contains_key(&clan_tk(token, &mid));
        let rej = CLAN_REJOINED.lock().contains_key(&clan_tk(token, &mid));
        if let Some(old) = old {
            if old.0 == mid && (moved || rej) && !(old.1 == parts && old.2 == flags && old.3 == counts) {
                // admin team move / a leave-and-return: the first half's report + this one = one day (an identical re-send is no second half)
                let first = parts;
                parts += old.1;
                for (k, v) in old.3.iter() {
                    if *k == 23 {
                        let cur = counts.get(k).copied().unwrap_or(0);
                        counts.insert(*k, cur.max(*v)); // best kill streak: the better half
                    } else if *k == 24 {
                        let cur = counts.get(k).copied().unwrap_or(0);
                        counts.insert(*k, if cur != 0 { cur } else { *v }); // weapon class with the most downs: this half's, else the first half's
                    } else {
                        let cur = counts.get(k).copied().unwrap_or(0);
                        counts.insert(*k, cur + *v);
                    }
                }
                dlog!("CLAN team move", tag(token), "mid", mid, format!("parts {} (first team) + {} = {}", old.1, first, parts));
            }
        }
        CLAN_PENDING.lock().insert(token.to_string(), (mid.clone(), parts, flags, counts));
    }
    if flags & 2 == 0 && MM.mid_of(token).as_deref() == Some(mid.as_str()) {
        // the client's final report for the match while the daemon still has it in the match (a daemon restart ate its leave op): its game is out of it
        dlog!("CLAN report from a player still listed in match", mid, tag(token), "- its game left it: departing");
        if TEST_LEAVE.load(Ordering::Relaxed) {
            dlog!("DBL leave_running_match", tag(token), "left (0x02) without a leave op, final report");
        } else {
            leave_running_match(token, "left (0x02) without a leave op, final report");
        }
    }
    if flags & 2 != 0 {
        end_report_note(token, &mid);
    }
    if CLAN_ENDED.lock().contains_key(&mid) || flags & 2 != 0 {
        clan_finalize(token)?;
    } else {
        CLAN_DUE.lock().insert(token.to_string(), clock::now() + *CLAN_GRACE); // one scheduler thread (clan_scheduler), not a Timer thread per report
        let start = {
            let _g = CLANLOCK.lock();
            !CLAN_SCHED.swap(true, Ordering::SeqCst)
        };
        if start && !TEST_TIMERS_ON.load(Ordering::Relaxed) {
            let _ = std::thread::Builder::new().name("clan-scheduler".into()).stack_size(256 * 1024).spawn(clan_scheduler);
        }
    }
    clan_state(token, 0)
}

// py: daemon.py:2276 end_report_note
/// A client reported that its own game ended match `mid`: remember it, and give the host's 0x12 MEND_GRACE s before the daemon closes by itself.
pub fn end_report_note(token: &str, mid: &str) {
    {
        let _g = CLANLOCK.lock();
        let first;
        {
            let mut r = CLAN_REPORTED_END.lock();
            first = !r.contains_key(mid);
            r.entry(mid.to_string()).or_default().entry(token.to_string()).or_insert_with(clock::now);
        }
        if !first || END_TIMERS.lock().contains(mid) || CLAN_ENDED.lock().contains_key(mid) {
            return;
        }
        END_TIMERS.lock().insert(mid.to_string());
    }
    let m = mid.to_string();
    py_timer(
        *MEND_GRACE + 0.5,
        Box::new(move || {
            // match_end_via_reports raising = Python's `except Exception: log(...)`; a panic cannot be caught here (panic = abort), none is expected
            match_end_via_reports(&m, None);
            END_TIMERS.lock().remove(&m);
        }),
    );
}

// py: daemon.py:2294 match_end_via_reports
/// Close the live match `mid` as op 0x12 would when every player still in it reported the end (0xE2 flags bit 1) and the host's 0x12 did not come
/// within MEND_GRACE s. Idempotent. Returns True when it closed the match.
pub fn match_end_via_reports(mid: &str, now: Option<f64>) -> bool {
    let now = now.unwrap_or_else(clock::now);
    if mid.is_empty() || END_BY_REPORTS.lock().contains(mid) || RESULT_BOOKED.lock().contains(mid) {
        return false; // (RESULT_BOOKED: a 0x12 is mid-close on another thread - its closers run after the booking, before MM.ended is set)
    }
    let rep: IndexMap<String, f64> = {
        let _g = CLANLOCK.lock();
        CLAN_REPORTED_END.lock().get(mid).cloned().unwrap_or_default()
    };
    if rep.is_empty() {
        return false;
    }
    let (players, dep, joining) = {
        let g = MM.lock.lock();
        if g.ended.contains(mid) || !g.started.contains(mid) || !g.matches.contains_key(mid) {
            return false; // the host's result (or an abort) already closed it / it never went live
        }
        let players = g.matches.get(mid).cloned().unwrap_or_default();
        let dep: HashSet<String> = g.departed.get(mid).cloned().unwrap_or_default();
        let joining = g.joining.iter().any(|(_, (m, _))| m == mid);
        (players, dep, joining)
    };
    if joining {
        return false; // a joiner that has not started yet: the match is still being played
    }
    let active: Vec<String> = players.iter().filter(|p| !dep.contains(*p)).cloned().collect();
    let reported: Vec<String> = active.iter().filter(|p| rep.contains_key(*p)).cloned().collect();
    let min_rep = reported.iter().map(|p| rep[p]).fold(f64::INFINITY, f64::min);
    if reported.is_empty() || now - min_rep < *MEND_GRACE {
        return false;
    }
    let playing = active.iter().any(|p| !rep.contains_key(p) && silence(p, now) <= *OFFLINE_GRACE);
    if playing {
        return false; // somebody has not seen the end yet (or plays on alone after a migration)
    }
    END_BY_REPORTS.lock().insert(mid.to_string());
    clan_match_ended(&players, mid);
    if !TEST_NO_LOADED_CLEAR.load(Ordering::Relaxed) {
        loaded_clear_match(mid, &players);
    }
    MM.end(mid);
    INT.end(mid, Some(0xFF));
    GL.end(mid);
    {
        let m = mid.to_string();
        let pl = players.clone();
        py_timer(
            9.0,
            Box::new(move || {
                MM.drop_mid(&m);
                HM.end(&m);
                forget_match_state(&pl);
                evt!("freed", m, mm::tags(&pl));
            }),
        );
    }
    let silent: Vec<String> = active.iter().filter(|p| !rep.contains_key(*p)).cloned().collect();
    evt!("match-end-reports", mid, "no host result; reports from", mm::tags(&reported), "silent/gone", mm::tags(&silent));
    dlog!("MATCH-END via client reports", mid, "players", mm::tags(&players), "reported", mm::tags(&reported), "- no 0x12 from the host, result unknown: no win / draw credited");
    true
}

// py: daemon.py:2325 clan_match_started
pub fn clan_match_started(players: &[String], mid: &str) {
    {
        // 11-04: a join-in-progress start passes [joiner] alone: the match's own list is the key of the map
        let pl: Vec<String> = MM.players_of_mid(mid).filter(|v| !v.is_empty()).unwrap_or_else(|| players.to_vec());
        let name = map_display(map_index(&pl));
        let mut m = CLAN_MAP.lock();
        m.insert(mid.to_string(), name);
        if m.len() > 400 {
            m.shift_remove_index(0);
        }
    }
    if PRIV.is_private_mid(mid) {
        // private match (11-17): no clan day, no booster used, no clan feed / chronicle; the anti-cheat roster below still runs
        for p in players {
            PRIV_LAST_MID.lock().insert(p.clone(), mid.to_string());
            CLAN_MATCH.lock().remove(p);
        }
        dlog!("CLAN private match", mid, "- no clan effects for", mm::tags(players));
    } else {
        {
            let mut ct = CLAN_TEAM.lock();
            let m: HashMap<String, i64> = players.iter().enumerate().map(|(i, p)| (p.clone(), team_in(mid, i as i64, Some(p)))).collect();
            ct.insert(mid.to_string(), m);
            if ct.len() > 400 {
                ct.shift_remove_index(0);
            }
        }
        for p in players {
            CLAN_MATCH.lock().insert(p.clone(), mid.to_string());
            CLAN_LAST_MATCH.lock().insert(p.clone(), mid.to_string());
            PRIV_LAST_MID.lock().remove(p);
            if let Err(e) = boost_start(p) {
                dlog!("CLAN boost start failed", e);
            }
        }
    }
    if let Some(ac) = AC.as_ref() {
        let slots = MM.players_of_mid(mid).filter(|v| !v.is_empty()).unwrap_or_else(|| players.to_vec());
        let names: HashMap<String, String> = players.iter().map(|p| (p.clone(), get_name(p))).collect();
        let sids: HashMap<String, u64> = players
            .iter()
            .map(|p| {
                let s = BRIDGES.steamid(p);
                (p.clone(), if s != 0 { s } else { token_steamid(p) })
            })
            .collect();
        ac.match_started(mid, &slots, players, &names, &sids);
    }
}

// py: daemon.py:2343 clan_match_ended
pub fn clan_match_ended(players: &[String], mid: &str) {
    if super::ext_k5::K5_FAKE_K2.load(Ordering::Relaxed) {
        // K5 selftest: the stand-in of verify/py/k5_harness.py (it patches the module global, so every caller gets it)
        return super::ext_k5::stub_note("clan_match_ended", format!("{} {}", crate::py::list_str(players), mid));
    }
    {
        let mut ba = BOOST_ACTIVE.lock();
        for p in players {
            ba.remove(p);
        }
    }
    if mid.is_empty() {
        return;
    }
    if let Some(ac) = AC.as_ref() {
        ac.match_ended(mid);
    }
    CLAN_ENDED.lock().insert(mid.to_string(), clock::now());
    for p in players {
        let pend = CLAN_PENDING.lock().get(p).is_some_and(|x| x.0 == mid);
        if pend {
            if let Err(e) = clan_finalize(p) {
                dlog!("CLAN finalize ERR", e);
            }
        }
    }
    let now = clock::now();
    let mut e = CLAN_ENDED.lock();
    let old: Vec<String> = e.iter().filter(|(_, t)| now - **t > 3600.0).map(|(m, _)| m.clone()).collect();
    for m in old {
        e.shift_remove(&m);
    }
}

// py: daemon.py:2358 boost_start
pub fn boost_start(token: &str) -> DbR<()> {
    BOOST_ACTIVE.lock().remove(token);
    let bid = BOOST_PICK.lock().remove(token).unwrap_or(0);
    if bid == 0 {
        return Ok(());
    }
    let ok = db::with_write(|c| {
        let k = clan_key_r(c, token)?;
        Ok(cl::booster_consume(c, &k, bid)?)
    })?;
    if ok {
        BOOST_ACTIVE.lock().insert(token.to_string(), bid);
    }
    let name = cl::BOOSTERS.iter().find(|(i, _)| *i == bid).map(|(_, n)| n.clone()).unwrap_or_else(|| bid.to_string());
    dlog!("BOOSTER", tag(token), name, if ok { "used" } else { "not owned" });
    Ok(())
}

// ---------------------------------------------------------------- selftest support
/// (selftest) the state of every K2 table back to a fresh daemon, and the timer queue empty
pub fn k2_reset() {
    CLAN_MATCH.lock().clear();
    CLAN_TEAM.lock().clear();
    CLAN_MAP.lock().clear();
    CLAN_ENDED.lock().clear();
    CLAN_PENDING.lock().clear();
    CLAN_CREDITED.lock().clear();
    CLAN_QUITTERS.lock().clear();
    CLAN_EXCUSE_WHY.lock().clear();
    CLAN_KICKED.lock().clear();
    CLAN_ABORTED.lock().clear();
    CLAN_LEFT_AT.lock().clear();
    CLAN_REJOINED.lock().clear();
    CLAN_LAST_DEPART.lock().clear();
    CLAN_EXCUSED.lock().clear();
    CLAN_LINK_EXCUSES.lock().clear();
    CLAN_CLOSING.lock().clear();
    CLAN_CLOSE_DROPS.lock().clear();
    CLAN_LOST_EXCUSES.lock().clear();
    CLAN_WIN.lock().clear();
    CLAN_MOVED.lock().clear();
    CLAN_REBOOK.lock().clear();
    CLAN_DONE.lock().clear();
    CLAN_DUE.lock().clear();
    CLAN_LAST_MATCH.lock().clear();
    PRIV_LAST_MID.lock().clear();
    CLAN_REPORTED_END.lock().clear();
    END_BY_REPORTS.lock().clear();
    RESULT_BOOKED.lock().clear();
    BOOST_PICK.lock().clear();
    BOOST_ACTIVE.lock().clear();
    ACH_DONE.lock().clear();
    ACH_PEND.lock().clear();
    ACH_DAY.lock().clear();
    ACH_LIVE_AT.lock().clear();
    RESTORED_T.lock().clear();
    END_TIMERS.lock().clear();
    TEST_TIMERS.lock().clear();
    clan_ops::k2_reset_ops();
}

/// (selftest) run every queued timer now in start order (a harness that fires its whole `threading.Timer` list at once); the count.
pub fn test_fire_timers_all() -> usize {
    let mut q: Vec<(f64, u64, TimerFn)> = std::mem::take(&mut *TEST_TIMERS.lock());
    q.sort_by_key(|e| e.1);
    let n = q.len();
    for (_, _, f) in q {
        f();
    }
    n
}

/// (selftest) forget every queued timer
pub fn test_fire_timers_clear() {
    TEST_TIMERS.lock().clear();
}

/// (selftest) the number of queued timers
pub fn test_timers_len() -> usize {
    TEST_TIMERS.lock().len()
}
