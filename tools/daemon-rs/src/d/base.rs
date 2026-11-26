//! K1 base: daemon.py:149-585 (config, search / lobby frames), 738-784 (name caches), 885-1037 (names, tokens, Steam ids, bans),
//! 1270-1543 (names, leaderboard, Elo, record_result). Every function keeps its Python name; module state keeps its name as a `pub static`.
use super::*;
use crate::{clock, modeq, names, py, rnd};
use indexmap::IndexMap;
use parking_lot::Mutex;
use rusqlite::Connection;
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::LazyLock;

// ---------------------------------------------------------------- config (daemon.py:149-310), read once from the environment
macro_rules! cfg_f {
    ($($n:ident = $env:expr, $d:expr;)*) => { $( pub static $n: LazyLock<f64> = LazyLock::new(|| crate::env_parse($env, $d)); )* };
}
macro_rules! cfg_on {
    ($($n:ident = $env:expr;)*) => { $( pub static $n: LazyLock<bool> = LazyLock::new(|| crate::env_on($env, true)); )* };
}
// py: daemon.py:149 HOST / PORT (DB_PATH = db::CFG.path, LOG_PATH = log::Config)
pub static HOST: LazyLock<String> = LazyLock::new(|| crate::env_str("T2R_HOST", "0.0.0.0"));
pub static PORT: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_PORT", 7001i64));
cfg_f!(KEEPALIVE_TTL = "T2R_UNUSED_KEEPALIVE_TTL", 15.0; FILL_WAIT = "T2R_FILL_WAIT", 15.0; JIP_MIN_AGE = "T2R_JIP_MIN_AGE", 15.0;
    JIP_MIN_AGE_REMNANT = "T2R_JIP_MIN_AGE_REMNANT", 120.0; JIP_MIN_LEFT = "T2R_JIP_MIN_LEFT", 90.0; JIP_CLOCK_TTL = "T2R_JIP_CLOCK_TTL", 12.0;
    JIP_REUSE_S = "T2R_JIP_REUSE_S", 10.0; JIP_PREFER_LEFT = "T2R_JIP_PREFER_LEFT", 180.0; JIP_ACK_STALE = "T2R_JIP_ACK_STALE", 12.0;
    REQUEUE_WAIT_MAX = "T2R_REQUEUE_WAIT_MAX", 90.0; REQUEUE_TICKET_S = "T2R_REQUEUE_TICKET_S", 300.0; REQUEUE_OFFLINE_S = "T2R_REQUEUE_OFFLINE_S", 30.0;
    REQUEUE_CAP_S = "T2R_UNUSED_REQUEUE_CAP_S", 60.0; LOBBY_RELEASE_S = "T2R_LOBBY_RELEASE_S", 30.0; LOBBY_MIN_GAP_S = "T2R_LOBBY_MIN_GAP_S", 10.0;
    AFK_KICK_S = "T2R_AFK_KICK_S", 90.0; MM_REGION_WAIT_S = "T2R_MM_REGION_WAIT_S", 90.0; QUEUE_HOLD_MAX = "T2R_QUEUE_HOLD_MAX", 75.0;
    PARTY_HOLD_S = "T2R_PARTY_HOLD_S", 120.0; MAX_WAIT = "T2R_MAX_WAIT", 150.0; FAST_STEP = "T2R_FAST_STEP", 8.0; FAST_MAX_WAIT = "T2R_FAST_MAX_WAIT", 40.0;
    FAST_ALONE = "T2R_FAST_ALONE", 15.0; JIP_ACK_MAX = "T2R_JIP_ACK_MAX", 25.0; QUEUE_SETTLE = "T2R_QUEUE_SETTLE", 3.0; QUEUE_FRESH = "T2R_QUEUE_FRESH", 5.0;
    PING_KICK_S = "T2R_PING_KICK_S", 60.0; LOSS_KICK_PCT = "T2R_LOSS_KICK_PCT", 30.0; LOSS_KICK_S = "T2R_LOSS_KICK_S", 120.0;
    LOSS_BASE_PCT = "T2R_LOSS_BASE_PCT", 8.0; LOSS_HOLD_S = "T2R_LOSS_HOLD_S", 300.0; LOSS_REPEAT_S = "T2R_LOSS_REPEAT_S", 7200.0;
    SF_CACHE_S = "T2R_SEARCH_CACHE_S", 1.0;);
// (the `T2R_UNUSED_*` names above: daemon.py:150 KEEPALIVE_TTL = 15.0 and :201 REQUEUE_CAP_S = 60.0 are plain constants there; the env name no
// deployment sets keeps them at the Python values)
cfg_on!(JIP_HOST_CAP = "T2R_JIP_HOST_CAP"; WAVE_RESPAWN = "T2R_WAVE_RESPAWN"; REQUEUE = "T2R_REQUEUE"; LOBBY_KEEP = "T2R_LOBBY_KEEP";
    LOBBY_BOOK = "T2R_LOBBY_BOOK"; MM_REGION_ON = "T2R_MM_REGION"; MM_PING_GATE = "T2R_MM_PING_GATE";);
pub static JIP: LazyLock<bool> = LazyLock::new(|| crate::env_is1("T2R_JIP", true));
pub static ENGINE_SLOTS: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_ENGINE_SLOTS", 8i64));
pub static MATCH_SIZE: LazyLock<i64> = LazyLock::new(|| std::cmp::min(crate::env_parse("T2R_MATCH_SIZE", 8i64), *ENGINE_SLOTS));
pub static MIN_PLAYERS: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_MIN_PLAYERS", std::cmp::min(2, *MATCH_SIZE)));
pub static LIVES_START: LazyLock<i64> = LazyLock::new(|| if crate::env_str("T2R_TEST_LIVES2", "") == "1" { 40 } else { 20 });
pub static SECOND_MATCH_MIN: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_SECOND_MATCH_MIN", 6i64));
pub static RELAY_MATCH_MAX: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_RELAY_MAX", 4i64));
pub static PING_KICK_MS: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_PING_KICK_MS", 190i64));
pub static LOSS_GATE: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_LOSS_GATE", 2i64));
pub static ANNOUNCE_CLOSE: LazyLock<bool> = LazyLock::new(|| crate::env_is1("T2R_ANNOUNCE_CLOSE", false));
pub const PING_GRACE_S: f64 = 45.0;
pub const PING_FRESH_S: f64 = 90.0;
pub const LOSS_MIN_OBS: i64 = 2;
pub const LOSS_MIN_PKTS: i64 = 300;
pub const LOSS_WINDOW_S: f64 = 75.0;
/// daemon.py:203 RQ_CAP: token -> time of its last 0xE3 report with flag 0x20
pub static RQ_CAP: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:223 AFK_KICKED: (token, mid) removed for inactivity (once per match)
pub static AFK_KICKED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:233 PING_HIGH: (token, mid) -> time its ping to the host was first seen above the limit
pub static PING_HIGH: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:236 PING_KICKED: (token, mid) removed for ping (once per match)
pub static PING_KICKED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:255 LOSS: (mid, observer, subject) -> deque of (time, received, lost)
pub static LOSS: LazyLock<Mutex<HashMap<(String, String, String), VecDeque<(f64, i64, i64)>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:256 LOSS_HIGH: (token, mid) -> time its median loss was first seen at or above the limit
pub static LOSS_HIGH: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:257 LOSS_KICKED: (token, mid) removed for loss (once per match)
pub static LOSS_KICKED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:258 LOSS_HOLD: steam id (or token when unknown) -> (time removed, loss %)
pub static LOSS_HOLD: LazyLock<Mutex<HashMap<String, (f64, f64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:259 LOSS_STRIKES: steam id (or token) -> [times removed for loss]
pub static LOSS_STRIKES: LazyLock<Mutex<HashMap<String, Vec<f64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:260 LOSS_FLAGS: (mid, token) -> (its msg-7 sends are paced, its host upload budget is on)
pub static LOSS_FLAGS: LazyLock<Mutex<HashMap<(String, String), (bool, bool)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

/// daemon.py:248 LOSS_HOST_S (T2R_LOSS_HOST_S=0 = off)
pub static LOSS_HOST_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LOSS_HOST_S", 30.0));
/// daemon.py:253 LOSS_HOST_HIGH: mid -> (host token, time its loss was first seen at or over the limit, 'no target' logged)
pub static LOSS_HOST_HIGH: LazyLock<Mutex<HashMap<String, (String, f64, bool)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:272 MM_REGION_LOBBY (T2R_MM_REGION_LOBBY=0 = off)
pub static MM_REGION_LOBBY: LazyLock<bool> = LazyLock::new(|| crate::env_on("T2R_MM_REGION_LOBBY", true));

// py: daemon.py:273 MM_REGION_GROUP
pub fn mm_region_group(r: &str) -> Option<&'static str> {
    match r {
        "EU" => Some("EU"),
        "NA-east" | "NA-west" => Some("NA"),
        "other" => Some("other"),
        _ => None,
    }
}

// py: daemon.py:275 region_group: the region group most of `tokens` are in (a tie: the first token's that has one); None = none known
pub fn region_group(tokens: &[String]) -> Option<String> {
    let mut cnt: Vec<(&'static str, i64)> = Vec::new();
    let mut first: Option<&'static str> = None;
    for t in tokens {
        if let Some(g) = mm_region(t).as_deref().and_then(mm_region_group) {
            match cnt.iter_mut().find(|c| c.0 == g) {
                Some(c) => c.1 += 1,
                None => cnt.push((g, 1)),
            }
            first = first.or(Some(g));
        }
    }
    let top = cnt.iter().map(|c| c.1).max()?;
    let f = first?;
    if cnt.iter().find(|c| c.0 == f).map(|c| c.1) == Some(top) {
        return Some(f.to_string());
    }
    cnt.iter().find(|c| c.1 == top).map(|c| c.0.to_string())
}

// py: daemon.py:187 has_helper
pub fn has_helper(tok: &str) -> bool {
    BRIDGES.steamid(tok) != 0 && BRIDGES.port(tok) != 0 && BRIDGES.conn(tok).is_some()
}

// py: daemon.py:226 book_on
pub fn book_on() -> bool {
    *LOBBY_BOOK && *LOBBY_KEEP
}

static FILL_CACHE: LazyLock<Mutex<(f64, Option<String>)>> = LazyLock::new(|| Mutex::new((0.0, None)));
// py: daemon.py:292 fill_rule: 'classic' | 'fast'; the file fill_rule.txt next to daemon.py (= the tools dir here) wins over T2R_FILL_RULE
pub fn fill_rule() -> String {
    let now = clock::now();
    let mut c = FILL_CACHE.lock();
    if now - c.0 >= 5.0 {
        c.0 = now;
        let p = crate::gen_modes::tools_dir().join("fill_rule.txt");
        c.1 = match std::fs::read(&p) {
            Ok(b) => {
                let s = super::utf8_replace_py(&b).trim_matches(py::is_py_space).to_lowercase();
                if s.is_empty() {
                    None
                } else {
                    Some(s)
                }
            }
            Err(_) => None,
        };
    }
    match &c.1 {
        Some(v) => v.clone(),
        None => crate::env_str("T2R_FILL_RULE", "fast").trim_matches(py::is_py_space).to_lowercase(),
    }
}

// ---------------------------------------------------------------- framing / search frames (daemon.py:388-585)
/// `bytes.decode("utf-8", "replace")`
pub fn utf8_replace_py(b: &[u8]) -> String {
    py::utf8_replace(b)
}

// py: daemon.py:585 lp
pub fn lp(s: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 1);
    v.push(s.len() as u8);
    v.extend_from_slice(s);
    v
}

/// one searching-page row: (token, shown name bytes, team label bytes)
pub type Ent = (String, Vec<u8>, Vec<u8>);
type SfKey = (Option<i64>, Vec<String>, i64, usize, Vec<(String, String)>);
struct SfCache {
    t: f64,
    key: Option<SfKey>,
    ents: Vec<Ent>,
}
// py: daemon.py:391 _SF_CACHE
static SF_CACHE: LazyLock<Mutex<SfCache>> = LazyLock::new(|| Mutex::new(SfCache { t: 0.0, key: None, ents: Vec::new() }));
static LOBBY_ENT_CACHE: LazyLock<Mutex<IndexMap<String, (f64, (String, Vec<String>, i64, usize, Vec<(String, String)>), Vec<Ent>)>>> =
    LazyLock::new(|| Mutex::new(IndexMap::new()));

/// time.monotonic() of the caches (the selftest pins it with TEST_MONO, bits of an f64; 0 = the real clock)
pub static TEST_MONO: AtomicU64 = AtomicU64::new(0);
fn mono() -> f64 {
    let t = TEST_MONO.load(Ordering::Relaxed);
    if t != 0 {
        f64::from_bits(t)
    } else {
        clock::mono()
    }
}

/// the shared body of `_search_entries` / `_lobby_entries`: names + team labels of `q`
fn build_entries(q: &[String]) -> Vec<Ent> {
    let party_of: HashMap<String, String> = q.iter().filter_map(|t| PARTIES.pid(t).map(|p| (t.clone(), p))).collect();
    let skill = if crate::env_str("T2R_TEAM_BALANCE", "1") != "0" { Some(player_skill(q)) } else { None };
    let top = top_players(q);
    let rg = rule_groups(q);
    let (t0, t1) = split_teams(q, &party_of, skill.as_ref(), Some(&top), Some(&rg));
    let mut lab: HashMap<String, String> = HashMap::new();
    for t in &t0 {
        lab.insert(t.clone(), " [T1]".to_string());
    }
    for t in &t1 {
        lab.insert(t.clone(), " [T2]".to_string());
    }
    for t in q {
        // a party shows on the searching page: " [T1] (party)", its leader " (party lead)" (user 10-25)
        if PARTIES.pid(t).is_some() && PARTIES.members(t).len() > 1 {
            let add = if PARTIES.is_leader(t) { " (party lead)" } else { " (party)" };
            let e = lab.entry(t.clone()).or_default();
            e.push_str(add);
        }
    }
    q.iter()
        .map(|t| {
            let nm = get_name(t);
            let nm = if nm.is_empty() { "PLAYER".to_string() } else { nm };
            (t.clone(), nm.into_bytes(), lab.get(t).cloned().unwrap_or_default().into_bytes())
        })
        .collect()
}

// py: daemon.py:392 _search_mode is in ext (K4 stub) -> defined here
pub fn _search_mode(tok: &str) -> i64 {
    MODEQ.get(tok, PARTIES.leader(tok).as_deref())
}

// py: daemon.py:400 _search_entries
pub fn _search_entries(mode: Option<i64>) -> Vec<Ent> {
    let mut q = MM.queued();
    if let Some(m) = mode {
        if !modeq::forced() {
            // per-mode queues (user 11-02): a searcher only sees the players of its own mode
            q.retain(|t| modeq::ModeQ::compat(_search_mode(t), m));
        }
    }
    let key: SfKey = (mode, q.clone(), name_ver(), PARTIES.of_len(), PARTIES.leaders_sorted());
    let now = mono();
    {
        let c = SF_CACHE.lock();
        if c.key.as_ref() == Some(&key) && now - c.t < *SF_CACHE_S {
            return c.ents.clone();
        }
    }
    let ents = build_entries(&q);
    let mut c = SF_CACHE.lock();
    c.t = now;
    c.key = Some(key);
    c.ents = ents.clone();
    ents
}

// py: daemon.py:420 _LOBBY_ENT_CACHE / _lobby_entries
pub fn _lobby_entries(lid: &str) -> Vec<Ent> {
    let q: Vec<String> = {
        let g = MM.lock.lock();
        g.book.members(lid)
    };
    let key = (lid.to_string(), q.clone(), name_ver(), PARTIES.of_len(), PARTIES.leaders_sorted());
    let now = mono();
    {
        let c = LOBBY_ENT_CACHE.lock();
        if let Some(e) = c.get(lid) {
            if e.1 == key && now - e.0 < *SF_CACHE_S {
                return e.2.clone();
            }
        }
    }
    let ents = build_entries(&q);
    let mut c = LOBBY_ENT_CACHE.lock();
    if c.len() > 256 {
        c.clear();
    }
    c.insert(lid.to_string(), (now, (key.0.clone(), key.1.clone(), key.2, key.3, key.4.clone()), ents.clone()));
    ents
}

// py: daemon.py:441 party_header
pub fn party_header(tok: &str) -> Option<Vec<u8>> {
    if tok.is_empty() || PARTIES.pid(tok).is_none() || PARTIES.members(tok).len() < 2 {
        return None;
    }
    Some(format!("PARTY {}/{}", PARTIES.members(tok).len(), PARTIES.max).into_bytes())
}

// py: daemon.py:447 party_lobby_names
pub fn party_lobby_names(tok: &str) -> Vec<Vec<u8>> {
    if party_header(tok).is_none() {
        return Vec::new();
    }
    let mut out = Vec::new(); // (no 'PARTY n/4' header row, user 10-28)
    for t in PARTIES.members(tok) {
        if t != tok {
            let nm = get_name(&t);
            let nm = if nm.is_empty() { "PLAYER".to_string() } else { nm };
            let mut b = nm.into_bytes();
            b.truncate(24);
            b.extend_from_slice(if PARTIES.is_leader(&t) { b" (party lead)" } else { b" (party)" });
            out.push(b);
        }
    }
    out.truncate(20);
    out
}

// py: daemon.py:459 lobby_list_frame
pub fn lobby_list_frame(names: &[Vec<u8>]) -> Vec<u8> {
    let mut b = vec![names.len() as u8];
    for n in names {
        b.push(n.len() as u8);
        b.extend_from_slice(n);
    }
    b.push(0);
    msg(0x17, &b)
}

// py: daemon.py:462 lobby_marks
pub fn lobby_marks(tok: &str) -> Option<(Vec<String>, Vec<String>)> {
    if !(*REQUEUE && *LOBBY_KEEP && !tok.is_empty()) {
        return None;
    }
    if book_on() {
        // the rows of a post-match lobby: READY = back from that match, IN SUMMARY = still on its screens (seat kept), NEW = everybody else
        let g = MM.lock.lock();
        let lb = g.book.lobby_of(tok).and_then(|l| g.book.get(l))?;
        let src = lb.src.clone()?;
        let mem = lb.members.clone();
        let back: Vec<String> = mem.iter().filter(|t| g.rq.mid_of(t).as_deref() == Some(src.as_str())).cloned().collect();
        let post: Vec<String> = g
            .rq
            .tokens()
            .into_iter()
            .filter(|t| {
                !g.book.of.contains_key(t)
                    && !g.queue.contains_key(t)
                    && g.rq.mid_of(t).as_deref() == Some(src.as_str())
                    && g.rq.has(t, None)
                    && !g.in_match_locked(t)
            })
            .collect();
        return Some((back, post));
    }
    let now = clock::now();
    let g = MM.lock.lock();
    let qd: Vec<String> = g.queue.keys().cloned().collect();
    let sil = |t: &str| silence(t, now);
    let inm = |t: &str| in_match_in(&g.matches, &g.departed, &g.ended, t);
    g.rq.members(tok, &qd, &sil, &inm, Some(now))
}

fn replace_bytes(h: &[u8], from: &[u8], to: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(h.len());
    let mut i = 0;
    while i < h.len() {
        if h[i..].starts_with(from) {
            out.extend_from_slice(to);
            i += from.len();
        } else {
            out.push(h[i]);
            i += 1;
        }
    }
    out
}

fn contains_bytes(h: &[u8], n: &[u8]) -> bool {
    h.windows(n.len()).any(|w| w == n)
}

// py: daemon.py:487 searching_frame
pub fn searching_frame(self_tok: Option<&str>) -> Vec<u8> {
    let self_tok = self_tok.filter(|t| !t.is_empty());
    let mut lid: Option<String> = None;
    if book_on() {
        if let Some(st) = self_tok {
            let g = MM.lock.lock();
            lid = g.book.lobby_of(st).map(|s| s.to_string());
        }
    }
    let mut ents = match lid.as_deref().filter(|l| !l.is_empty()) {
        Some(l) => _lobby_entries(l),
        None => _search_entries(self_tok.map(_search_mode)),
    };
    // user 11-02: the " [T1]" / " [T2]" text after a searcher's name is gone for the player - t2r-ext strips it from the row and draws a blue / red tab instead.
    // The marker is RELATIVE to the viewer here: [T1] = our team, [T2] = the enemy.
    let mine: Option<Vec<u8>> = self_tok.and_then(|st| {
        ents.iter().find(|(t, _b, lab)| t == st && (contains_bytes(lab, b"[T1]") || contains_bytes(lab, b"[T2]"))).map(|(_, _, lab)| lab.clone())
    });
    if let Some(mine) = mine {
        let m1 = contains_bytes(&mine, b"[T1]");
        ents = ents
            .into_iter()
            .map(|(t, b, lab)| {
                let l = replace_bytes(&lab, b"[T1]", b"[X]");
                let l = replace_bytes(&l, b"[T2]", b"[Y]");
                let l = replace_bytes(&l, b"[X]", if m1 { b"[T1]" } else { b"[T2]" });
                let l = replace_bytes(&l, b"[Y]", if m1 { b"[T2]" } else { b"[T1]" });
                (t, b, l)
            })
            .collect();
    }
    let own: Option<Vec<u8>> = self_tok.map(|st| {
        let mut b = get_name(st).into_bytes();
        b.truncate(31);
        b
    });
    let lm = lobby_marks(self_tok.unwrap_or(""));
    let lm_back: HashSet<String> = lm.as_ref().map(|l| l.0.iter().cloned().collect()).unwrap_or_default();
    let mut names: Vec<Vec<u8>> = Vec::new();
    let mut seen_tok: HashSet<String> = HashSet::new();
    let mut seen_nm: HashSet<Vec<u8>> = HashSet::new();
    for (t, base, lab) in &ents {
        let b31 = &base[..base.len().min(31)];
        if Some(t.as_str()) == self_tok || own.as_ref().is_some_and(|o| !o.is_empty() && b31 == o.as_slice()) || seen_tok.contains(t) {
            continue;
        }
        let mut n: Vec<u8> = base[..base.len().min(24)].to_vec();
        n.extend_from_slice(lab);
        if lm.is_some() {
            // (NEW: a searcher from the queue, the backfill)
            n.extend_from_slice(if lm_back.contains(t) { b" (lobby)" } else { b" (new)" });
        }
        if seen_nm.contains(&n) {
            continue;
        }
        seen_tok.insert(t.clone());
        seen_nm.insert(n.clone());
        names.push(n);
    }
    if let Some(lm) = &lm {
        for t in &lm.1 {
            if Some(t.as_str()) != self_tok && !seen_tok.contains(t) {
                let nm = get_name(t);
                let nm = if nm.is_empty() { "PLAYER".to_string() } else { nm };
                let mut n = nm.into_bytes();
                n.truncate(24);
                n.extend_from_slice(b" (summary)");
                if !seen_nm.contains(&n) {
                    seen_tok.insert(t.clone());
                    seen_nm.insert(n.clone());
                    names.push(n);
                }
            }
        }
    }
    if let Some(st) = self_tok {
        if PARTIES.pid(st).is_some() {
            // party mates that are not searching yet are listed too (waiting for FIND MATCH)
            let qs: HashSet<&String> = ents.iter().map(|(t, _, _)| t).collect();
            for t in PARTIES.members(st) {
                if t != st && !qs.contains(&t) {
                    let nm = get_name(&t);
                    let nm = if nm.is_empty() { "PLAYER".to_string() } else { nm };
                    let mut n = nm.into_bytes();
                    n.truncate(24);
                    n.extend_from_slice(if PARTIES.is_leader(&t) { b" (party lead - not searching)" } else { b" (party - not searching)" });
                    if !seen_nm.contains(&n) {
                        seen_nm.insert(n.clone());
                        names.push(n);
                    }
                }
            }
        }
    }
    // user 11-03: YOUR OWN TEAM is always on top, the enemy below: a stable sort on the relative marker
    names.sort_by_key(|n| if contains_bytes(n, b" [T1]") { 0 } else if contains_bytes(n, b" [T2]") { 1 } else { 2 });
    names.truncate(20);
    let mut body = vec![names.len() as u8];
    for n in &names {
        body.push(n.len() as u8);
        body.extend_from_slice(n);
    }
    let mut out = msg(0x03, &[]);
    if !names.is_empty() {
        body.push(0);
        out.extend_from_slice(&msg(0x17, &body));
    }
    out
}

// ---------------------------------------------------------------- name caches (daemon.py:738-784)
pub static NAME_CACHE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_NAME_CACHE_S", 3.0));
struct CEnt<V> {
    exp: f64,
    gen: (i64, i64),
    val: V,
    key: Option<String>,
}
#[derive(Default)]
struct NameState {
    name_cache: HashMap<String, CEnt<String>>,
    steam_cache: HashMap<String, CEnt<u64>>,
    name_gen: i64,
    name_ver: i64,
    tok_gen: HashMap<String, i64>,
}
static NS: LazyLock<Mutex<NameState>> = LazyLock::new(|| Mutex::new(NameState::default()));

impl NameState {
    // py: daemon.py:746 _gen_of
    fn gen_of(&self, token: &str) -> (i64, i64) {
        (self.name_gen, self.tok_gen.get(token).copied().unwrap_or(0))
    }
}

/// (selftest) empty every cache and counter of this file
pub fn reset_caches() {
    *NS.lock() = NameState::default();
    *ELO_POOL.lock() = (0.0, 0.4);
    SF_CACHE.lock().key = None;
    LOBBY_ENT_CACHE.lock().clear();
}

/// `_name_ver[0]`: bumped by every name_dirty() (keys of caches built from many names)
pub fn name_ver() -> i64 {
    NS.lock().name_ver
}

// py: daemon.py:748 name_dirty
pub fn name_dirty(token: Option<&str>, key: Option<&str>) {
    let token = token.filter(|t| !t.is_empty());
    let key = key.filter(|t| !t.is_empty());
    let mut s = NS.lock();
    s.name_ver += 1;
    if token.is_none() && key.is_none() {
        s.name_gen += 1;
        return;
    }
    let mut toks: Vec<String> = Vec::new();
    if let Some(t) = token {
        toks.push(t.to_string());
    }
    if let Some(k) = key {
        let hits: Vec<String> = s.name_cache.iter().filter(|(t, e)| e.key.as_deref() == Some(k) || names::token_id(t) == k).map(|(t, _)| t.clone()).collect();
        toks.extend(hits);
    }
    if s.tok_gen.len() > 8192 {
        // bounded: start over with a global drop
        s.tok_gen.clear();
        s.name_gen += 1;
        return;
    }
    let mut seen: HashSet<String> = HashSet::new();
    for t in toks {
        if seen.insert(t.clone()) {
            *s.tok_gen.entry(t).or_insert(0) += 1;
        }
    }
}

// py: daemon.py:772 _cache_get (name cache)
fn name_cache_get(token: &str) -> Option<String> {
    let s = NS.lock();
    let e = s.name_cache.get(token)?;
    if e.gen == s.gen_of(token) && mono() < e.exp {
        Some(e.val.clone())
    } else {
        None
    }
}

fn steam_cache_get(token: &str) -> Option<u64> {
    let s = NS.lock();
    let e = s.steam_cache.get(token)?;
    if e.gen == s.gen_of(token) && mono() < e.exp {
        Some(e.val)
    } else {
        None
    }
}

// ---------------------------------------------------------------- names, tokens, Steam ids (daemon.py:885-955)
// py: daemon.py:885 save_loadout
pub fn save_loadout(token: &str, blob: &[u8]) -> db::DbResult<()> {
    db::with_write(|c| {
        c.execute(
            "INSERT INTO players(token,loadout,last_seen) VALUES(?,?,?) ON CONFLICT(token) DO UPDATE SET loadout=excluded.loadout,last_seen=excluded.last_seen",
            rusqlite::params![token, blob, clock::now()],
        )?;
        Ok(())
    })
}

// py: daemon.py:891 set_name
pub fn set_name(token: &str, name: &str) -> db::DbResult<()> {
    name_dirty(Some(token), None);
    db::with_write(|c| {
        c.execute(
            "INSERT INTO players(token,name,last_seen) VALUES(?,?,?) ON CONFLICT(token) DO UPDATE SET name=excluded.name,last_seen=excluded.last_seen",
            rusqlite::params![token, name, clock::now()],
        )?;
        Ok(())
    })?;
    name_dirty(Some(token), None); // (again after the commit: a reader between the first call and the write cached the old value)
    Ok(())
}

// py: daemon.py:900 set_steam
pub fn set_steam(token: &str, steamid: u64, name: &str) -> db::DbResult<()> {
    name_dirty(Some(token), None);
    db::with_write(|c| {
        c.execute(
            "INSERT INTO players(token,name,steamid,last_seen) VALUES(?,?,?,?) ON CONFLICT(token) DO UPDATE SET name=excluded.name,steamid=excluded.steamid,last_seen=excluded.last_seen",
            rusqlite::params![token, name, steamid as i64, clock::now()],
        )?;
        Ok(())
    })?;
    name_dirty(Some(token), None);
    Ok(())
}

pub const NAME_TAG: &[u8] = b"T2N:";

// py: daemon.py:918 make_token
pub fn make_token(name: &str) -> String {
    let mut raw: Vec<u8> = NAME_TAG.to_vec();
    let nb = name.as_bytes();
    raw.extend_from_slice(&nb[..nb.len().min(15)]);
    raw.push(0);
    let pad = 32usize.saturating_sub(raw.len());
    raw.extend_from_slice(&rnd::token_bytes("daemon:make_token", pad));
    raw.iter().map(|b| format!("{:02x}", b)).collect()
}

/// `bytes.fromhex(s)`: ASCII whitespace between byte pairs is skipped; anything else that is not a hex pair is an error
fn fromhex(s: &str) -> Option<Vec<u8>> {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len() / 2);
    let mut i = 0;
    let hx = |c: u8| (c as char).to_digit(16);
    while i < b.len() {
        if matches!(b[i], b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r') {
            i += 1;
            continue;
        }
        if i + 1 >= b.len() {
            return None;
        }
        let (h, l) = (hx(b[i])?, hx(b[i + 1])?);
        out.push((h * 16 + l) as u8);
        i += 2;
    }
    Some(out)
}

// py: daemon.py:927 token_name
pub fn token_name(token: &str) -> Option<String> {
    let raw = fromhex(token)?;
    if raw.starts_with(NAME_TAG) {
        let rest = &raw[NAME_TAG.len()..];
        let nm = rest.split(|&c| c == 0).next().unwrap_or(&[]);
        let s = teams::utf8_ignore(nm);
        let s = py::cut(&s, 15).to_string();
        if s.is_empty() {
            return None;
        }
        return Some(s);
    }
    None
}

// py: daemon.py:938 tag
pub fn tag(token: &str) -> String {
    token_name(token).unwrap_or_else(|| py::cut(token, 8).to_string())
}

// py: daemon.py:942 token_steamid
pub fn token_steamid(token: &str) -> u64 {
    if WORLD_ON.load(Ordering::Relaxed) {
        return WORLD.lock().sids.get(token).copied().unwrap_or(0);
    }
    if token.is_empty() {
        return 0;
    }
    if let Some(v) = steam_cache_get(token) {
        return v;
    }
    let gen = NS.lock().gen_of(token);
    let r: Option<i64> = db::db_read(|c| {
        use rusqlite::OptionalExtension;
        c.query_row("SELECT steamid FROM players WHERE token=?", [token], |r| r.get::<_, Option<i64>>(0)).optional().map(|o| o.flatten())
    })
    .unwrap_or(None);
    let v = match r {
        Some(x) if x != 0 => x as u64,
        _ => 0,
    };
    let mut s = NS.lock();
    if s.steam_cache.len() > 4096 {
        s.steam_cache.clear();
    }
    s.steam_cache.insert(token.to_string(), CEnt { exp: mono() + *NAME_CACHE_S, gen, val: v, key: None });
    v
}

// ---------------------------------------------------------------- bans (daemon.py:957-1037, the public part)
pub const BAN_MSG: &[u8] = b"You are banned from the server.";
pub static BAN_PATH: LazyLock<String> = LazyLock::new(|| {
    let db = std::path::Path::new(&db::CFG.path);
    let abs = if db.is_absolute() { db.to_path_buf() } else { std::env::current_dir().unwrap_or_default().join(db) };
    abs.parent().unwrap_or(std::path::Path::new("")).join("bans.txt").to_string_lossy().into_owned()
});

/// an `ipaddress.ip_network(.., strict=False)`: address masked to `prefix` bits
#[derive(Clone, Debug, PartialEq)]
pub struct IpNet {
    pub addr: std::net::IpAddr,
    pub prefix: u8,
}

impl IpNet {
    // py: ipaddress.ip_network(s, strict=False)
    pub fn parse(s: &str) -> Option<IpNet> {
        use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
        let (a, p) = match s.split_once('/') {
            Some((a, p)) => (a, Some(p)),
            None => (s, None),
        };
        let ip: IpAddr = a.parse().ok()?;
        let maxp: u32 = if ip.is_ipv4() { 32 } else { 128 };
        let prefix: u32 = match p {
            None => maxp,
            Some(p) => {
                if !p.is_empty() && p.bytes().all(|c| c.is_ascii_digit()) {
                    let n: u32 = p.parse().ok()?;
                    if n > maxp {
                        return None;
                    }
                    n
                } else if ip.is_ipv4() {
                    // a netmask (255.255.255.0) or a hostmask (0.0.0.255)
                    let m: Ipv4Addr = p.parse().ok()?;
                    let v = u32::from(m);
                    if v.leading_ones() + v.trailing_zeros() == 32 || (v == 0) {
                        v.leading_ones()
                    } else if v.leading_zeros() + v.trailing_ones() == 32 {
                        32 - v.trailing_ones()
                    } else {
                        return None;
                    }
                } else {
                    return None;
                }
            }
        };
        let masked = match ip {
            IpAddr::V4(v) => {
                let m = if prefix == 0 { 0 } else { u32::MAX << (32 - prefix) };
                IpAddr::V4(Ipv4Addr::from(u32::from(v) & m))
            }
            IpAddr::V6(v) => {
                let m = if prefix == 0 { 0 } else { u128::MAX << (128 - prefix) };
                IpAddr::V6(Ipv6Addr::from(u128::from(v) & m))
            }
        };
        Some(IpNet { addr: masked, prefix: prefix as u8 })
    }
    /// `ip_address(x) in net` (false across families)
    pub fn contains(&self, ip: &std::net::IpAddr) -> bool {
        use std::net::IpAddr;
        match (self.addr, ip) {
            (IpAddr::V4(n), IpAddr::V4(a)) => {
                let m = if self.prefix == 0 { 0 } else { u32::MAX << (32 - self.prefix as u32) };
                u32::from(n) == u32::from(*a) & m
            }
            (IpAddr::V6(n), IpAddr::V6(a)) => {
                let m = if self.prefix == 0 { 0 } else { u128::MAX << (128 - self.prefix as u32) };
                u128::from(n) == u128::from(*a) & m
            }
            _ => false,
        }
    }
}

/// daemon.py:1030 `_bans`
#[derive(Default)]
pub struct Bans {
    pub mtime: Option<f64>,
    pub ids: HashSet<String>,
    pub names: HashSet<String>,
    pub hw: HashSet<String>,
    pub tok: HashSet<String>,
    pub ip: Vec<IpNet>,
    pub pid: HashSet<String>,
    pub reason: HashMap<String, String>,
}
pub static BANS: LazyLock<Mutex<Bans>> = LazyLock::new(|| Mutex::new(Bans::default()));

fn py_isdigit(e: &str) -> bool {
    !e.is_empty() && e.chars().all(|c| c.is_ascii_digit())
}

// py: daemon.py:966 ban_note
pub fn ban_note(token: &str) -> Vec<u8> {
    // 11-08: a per-ban reason ("# ... reason: <text>" on the bans.txt line) is shown after the banned note
    if token.is_empty() {
        return BAN_MSG.to_vec();
    }
    let base = srv_note("banned", token);
    let mut why: Option<String> = None;
    let sid = {
        let b = BRIDGES.steamid(token);
        if b != 0 {
            b
        } else {
            token_steamid(token)
        }
    };
    if sid != 0 {
        why = BANS.lock().reason.get(&sid.to_string()).cloned();
    }
    if why.as_deref().is_none_or(|w| w.is_empty()) {
        let row = db::with_write(|c| {
            use rusqlite::OptionalExtension;
            Ok(c.query_row("SELECT name, steamid FROM players WHERE token=?", [token], |r| Ok((r.get::<_, Option<String>>(0)?, r.get::<_, Option<i64>>(1)?))).optional()?)
        });
        match row {
            Ok(Some((name, sid))) => {
                let b = BANS.lock();
                let k1 = match sid {
                    Some(x) => x.to_string(),
                    None => "None".to_string(),
                };
                why = b.reason.get(&k1).cloned().filter(|w| !w.is_empty()).or_else(|| b.reason.get(&name.unwrap_or_default().to_lowercase()).cloned());
            }
            Ok(None) => {}
            Err(ex) => dlog!("BANS reason lookup failed", ex),
        }
    }
    match why {
        Some(w) if !w.is_empty() => {
            let mut v = base;
            v.push(b' ');
            v.extend_from_slice(w.as_bytes());
            v.truncate(0xf0);
            v
        }
        _ => base,
    }
}

// py: daemon.py:991 _bans_load
pub fn _bans_load() {
    let path = std::path::Path::new(&*BAN_PATH);
    let m: f64 = match std::fs::metadata(path).and_then(|m| m.modified()) {
        Ok(t) => match t.duration_since(std::time::UNIX_EPOCH) {
            Ok(d) => d.as_secs_f64(),
            Err(e) => -e.duration().as_secs_f64(),
        },
        Err(_) => {
            let mut b = BANS.lock();
            b.mtime = None;
            b.ids = HashSet::new();
            b.names = HashSet::new();
            b.hw = HashSet::new();
            b.tok = HashSet::new();
            b.ip = Vec::new();
            b.pid = HashSet::new();
            return;
        }
    };
    if BANS.lock().mtime == Some(m) {
        return;
    }
    let mut ids: HashSet<String> = HashSet::new();
    let mut nms: HashSet<String> = HashSet::new();
    let mut hw: HashSet<String> = HashSet::new();
    let mut tk: HashSet<String> = HashSet::new();
    let mut ipn: Vec<IpNet> = Vec::new();
    let mut pids: HashSet<String> = HashSet::new();
    let mut reason: HashMap<String, String> = HashMap::new();
    let raw = match std::fs::read(path) {
        Ok(b) => b,
        Err(_) => return, // (Python would raise: the bans stay as they were)
    };
    let text = teams::utf8_ignore(&raw).replace("\r\n", "\n").replace('\r', "\n");
    for line in text.split('\n') {
        let e0 = line.split('#').next().unwrap_or("");
        let e = py::strip(e0);
        if e.is_empty() {
            continue;
        }
        let cm: &str = match line.split_once('#') {
            Some((_, c)) => c,
            None => "",
        };
        let cml = cm.to_lowercase();
        if let Some(i) = cml.find("reason:") {
            let ci = cml[..i].chars().count();
            let tail: String = cm.chars().skip(ci + 7).collect();
            let k = if py_isdigit(e) { e.to_string() } else { e.to_lowercase() };
            reason.insert(k, py::strip(&tail).to_string());
        }
        let el = e.to_lowercase();
        if el.starts_with("token:") {
            // anti-cheat review (tools/reports/review.py): the 64-hex name token of a banned install
            let t = py::strip(&e.chars().skip(6).collect::<String>()).to_lowercase();
            if t.chars().count() == 64 && t.chars().all(|c| "0123456789abcdef".contains(c)) {
                tk.insert(t);
            }
            continue;
        }
        if el.starts_with("ip:") {
            let a: String = e.chars().skip(3).collect();
            match IpNet::parse(py::strip(&a)) {
                Some(n) => ipn.push(n),
                None => dlog!("BANS bad ip entry", e),
            }
            continue;
        }
        if el.starts_with("pid:") {
            let p: String = e.chars().skip(4).collect();
            pids.insert(py::strip(&p).to_uppercase());
            continue;
        }
        if el.starts_with("hwid:") {
            let h = py::strip(&e.chars().skip(5).collect::<String>()).to_lowercase();
            if h.chars().count() >= HWID_HEX && h.chars().all(|c| "0123456789abcdef".contains(c)) {
                hw.insert(h.chars().take(HWID_HEX).collect());
            }
            continue;
        }
        if py_isdigit(e) && e.chars().count() >= 15 {
            ids.insert(e.to_string());
        } else {
            nms.insert(if py_isdigit(e) { e.to_string() } else { e.to_lowercase() });
        }
    }
    if !nms.is_empty() {
        let r = db::with_write(|c| {
            let mut found: Vec<String> = Vec::new();
            for n in &nms {
                let mut st = c.prepare("SELECT steamid FROM players WHERE lower(name)=? AND steamid")?;
                let rows = st.query_map([n], |r| r.get::<_, rusqlite::types::Value>(0))?;
                for v in rows {
                    found.push(value_str(&v?));
                }
            }
            Ok(found)
        });
        match r {
            Ok(f) => ids.extend(f),
            Err(ex) => dlog!("BANS name lookup failed", ex),
        }
    }
    if !nms.is_empty() && !reason.is_empty() {
        let r = db::with_write(|c| {
            let mut found: Vec<(String, String)> = Vec::new();
            for n in &nms {
                if let Some(rs) = reason.get(n) {
                    let mut st = c.prepare("SELECT steamid FROM players WHERE lower(name)=? AND steamid")?;
                    let rows = st.query_map([n], |r| r.get::<_, rusqlite::types::Value>(0))?;
                    for v in rows {
                        found.push((value_str(&v?), rs.clone()));
                    }
                }
            }
            Ok(found)
        });
        match r {
            Ok(f) => {
                for (k, v) in f {
                    reason.entry(k).or_insert(v); // setdefault
                }
            }
            Err(ex) => dlog!("BANS reason name lookup failed", ex),
        }
    }
    let (ni, nn, nh, nt, np, nd) = (ids.len(), nms.len(), hw.len(), tk.len(), ipn.len(), pids.len());
    {
        let mut b = BANS.lock();
        b.mtime = Some(m);
        b.ids = ids;
        b.names = nms;
        b.hw = hw;
        b.tok = tk;
        b.ip = ipn;
        b.pid = pids;
        b.reason = reason;
    }
    dlog!("BANS loaded", ni, "steam id(s),", nn, "name(s),", nh, "hwid(s),", nt, "token(s),", np, "ip / range(s),", nd, "identity id(s) from", &*BAN_PATH);
}

/// `str(v)` of a sqlite value (steamid column)
fn value_str(v: &rusqlite::types::Value) -> String {
    use rusqlite::types::Value;
    match v {
        Value::Null => "None".to_string(),
        Value::Integer(i) => i.to_string(),
        Value::Real(f) => py::f(*f),
        Value::Text(s) => s.clone(),
        Value::Blob(b) => format!("b'{}'", String::from_utf8_lossy(b)),
    }
}

// ---------------------------------------------------------------- names (daemon.py:1270-1380)
// py: daemon.py:1270 name_key
pub fn name_key(token: &str) -> String {
    let sid = token_steamid(token);
    if sid != 0 {
        format!("s:{}", sid)
    } else {
        names::token_id(token)
    }
}

// py: daemon.py:1277 custom_name
pub fn custom_name(token: &str) -> (String, String) {
    if token.is_empty() || token.chars().count() != 64 {
        return (String::new(), String::new());
    }
    let key = name_key(token);
    let r = db::db_read(|c| {
        let (mut cn, mut ct) = names::load(c, &key)?;
        if cn.is_empty() && key.starts_with("s:") {
            // a name saved before the Steam keying: the token id's
            let (a, b) = names::load(c, &names::token_id(token))?;
            cn = a;
            ct = b;
        }
        Ok((cn, ct))
    });
    r.unwrap_or_default()
}

// py: daemon.py:1290 safe_name
pub fn safe_name(nm: &str) -> String {
    if nm.is_empty() || (!nm.contains('<') && !nm.contains('>')) {
        return nm.to_string();
    }
    let s = nm.replace(['<', '>'], "");
    let s = py::strip(&s);
    if s.is_empty() {
        "PLAYER".to_string()
    } else {
        s.to_string()
    }
}

// py: daemon.py:1298 get_name ("" = none)
pub fn get_name(token: &str) -> String {
    if WORLD_ON.load(Ordering::Relaxed) {
        return WORLD.lock().names.get(token).cloned().unwrap_or_default();
    }
    if token.is_empty() {
        return String::new();
    }
    if let Some(v) = name_cache_get(token) {
        return v;
    }
    let gen = NS.lock().gen_of(token); // (before the read: see _cache_put)
    let v = safe_name(&_get_name(token));
    let key = if token.chars().count() == 64 { Some(name_key(token)) } else { None };
    let mut s = NS.lock();
    if s.name_cache.len() > 4096 {
        s.name_cache.clear();
    }
    s.name_cache.insert(token.to_string(), CEnt { exp: mono() + *NAME_CACHE_S, gen, val: v.clone(), key });
    v
}

// py: daemon.py:1309 _get_name
pub fn _get_name(token: &str) -> String {
    let (cn, ctag) = custom_name(token);
    if !cn.is_empty() {
        return names::display(&cn, &ctag);
    }
    if let Some(tn) = token_name(token) {
        return tn;
    }
    let r: Option<String> = db::db_read(|c| {
        use rusqlite::OptionalExtension;
        c.query_row("SELECT name FROM players WHERE token=?", [token], |r| r.get::<_, Option<String>>(0)).optional().map(|o| o.flatten())
    })
    .unwrap_or(None);
    r.unwrap_or_default()
}

// py: daemon.py:1318 get_loadout
pub fn get_loadout(token: &str) -> db::DbResult<Option<Vec<u8>>> {
    db::with_write(|c| {
        use rusqlite::OptionalExtension;
        let r = c.query_row("SELECT loadout FROM players WHERE token=?", [token], |r| r.get::<_, Option<Vec<u8>>>(0)).optional()?;
        Ok(r.flatten().filter(|b| !b.is_empty()))
    })
}

/// one leaderboard row: (token, name, kills, deaths, wins, matches - draws)
pub type LbRow = (String, String, i64, i64, i64, i64);

// py: daemon.py:1323 leaderboard
pub fn leaderboard(top: usize) -> db::DbResult<Vec<LbRow>> {
    // one row per player: the Steam account when known (every install / token of one player adds up), else the token id
    let rows: Vec<(String, Option<String>, i64, i64, i64, i64)> = db::with_write(|c| {
        let mut st = c.prepare(
            "SELECT MAX(last_seen),token,name,SUM(kills) k,SUM(deaths),SUM(wins) w,SUM(matches) - SUM(draws) FROM players \
             WHERE length(token) = 64 AND (matches > 0 OR token LIKE '54324e3a%') \
             GROUP BY CASE WHEN steamid > 0 THEN 's' || steamid ELSE substr(token,41,24) END",
        )?;
        let it = st.query_map([], |r| {
            Ok((
                r.get::<_, Option<String>>(1)?.unwrap_or_default(),
                r.get::<_, Option<String>>(2)?,
                r.get::<_, Option<i64>>(3)?.unwrap_or(0),
                r.get::<_, Option<i64>>(4)?.unwrap_or(0),
                r.get::<_, Option<i64>>(5)?.unwrap_or(0),
                r.get::<_, Option<i64>>(6)?.unwrap_or(0),
            ))
        })?;
        let mut v = Vec::new();
        for x in it {
            v.push(x?);
        }
        Ok(v)
    })?;
    let mut best: IndexMap<String, LbRow> = IndexMap::new();
    for r in rows {
        let mut nm = get_name(&r.0);
        if nm.is_empty() {
            nm = r.1.clone().filter(|s| !s.is_empty()).unwrap_or_else(|| "PLAYER".to_string());
        }
        let k = nm.to_lowercase();
        match best.get_mut(&k) {
            Some(b) => {
                *b = (b.0.clone(), nm, b.2 + r.2, b.3 + r.3, b.4 + r.4, b.5 + r.5);
            }
            None => {
                best.insert(k, (r.0, nm, r.2, r.3, r.4, r.5));
            }
        }
    }
    let mut out: Vec<LbRow> = best.into_values().collect();
    out.sort_by_key(|x| (-x.4, -x.2));
    out.truncate(top);
    Ok(out)
}

// py: daemon.py:1347 name_taken
pub fn name_taken(c: &Connection, key: &str, name: &str) -> rusqlite::Result<bool> {
    let n = py::strip(name).to_lowercase();
    if n.is_empty() {
        return Ok(false);
    }
    names::init(c)?;
    let sid: i64 = if let Some(r) = key.strip_prefix("s:") { r.trim().parse().map_err(|_| rusqlite::Error::InvalidQuery)? } else { -1 };
    let mut own: HashSet<String> = HashSet::new();
    own.insert(key.to_string());
    {
        let mut st = c.prepare("SELECT token FROM players WHERE steamid=?")?;
        let rows = st.query_map([sid], |r| r.get::<_, String>(0))?;
        for t in rows {
            own.insert(names::token_id(&t?));
        }
    }
    {
        let mut st = c.prepare("SELECT token_id FROM names WHERE lower(name)=?")?;
        let rows = st.query_map([&n], |r| r.get::<_, Option<String>>(0))?;
        for t in rows {
            let t = t?.unwrap_or_default();
            if !own.contains(&t) {
                return Ok(true);
            }
        }
    }
    let mut st = c.prepare("SELECT 1 FROM players WHERE lower(name)=? AND steamid>0 AND steamid<>?")?;
    let mut rows = st.query(rusqlite::params![n, sid])?;
    Ok(rows.next()?.is_some())
}

// py: daemon.py:1360 add_matches
pub fn add_matches(tokens: &[String]) -> db::DbResult<()> {
    db::with_write(|c| {
        for t in tokens {
            c.execute(
                "INSERT INTO players(token,name,matches,last_seen) VALUES(?,?,1,?) ON CONFLICT(token) DO UPDATE SET matches=matches+1,last_seen=excluded.last_seen",
                rusqlite::params![t, token_name(t), clock::now()],
            )?;
        }
        Ok(())
    })
}

// ---------------------------------------------------------------- Elo (daemon.py:1368-1500)
pub const ELO_BASE: f64 = 1000.0;
pub const ELO_K_NEW: f64 = 40.0;
pub const ELO_K: f64 = 24.0;
pub const ELO_NEW_N: i64 = 20;
pub const ELO_PRIOR: f64 = 40.0;
pub const ELO_UNSURE: f64 = 200.0;
pub const ELO_PERF_LO: f64 = 0.5;
pub const ELO_PERF_HI: f64 = 1.5;
pub const ELO_PERF_WAIT_S: f64 = 45.0;
/// daemon.py:1387 `_elo_pool`: (time, p)
static ELO_POOL: LazyLock<Mutex<(f64, f64)>> = LazyLock::new(|| Mutex::new((0.0, 0.4)));

type ER<T> = Result<T, String>;
fn es(e: rusqlite::Error) -> String {
    e.to_string()
}

// py: daemon.py:1391 elo_pool_rate
pub fn elo_pool_rate(c: &Connection) -> ER<f64> {
    let mut p = ELO_POOL.lock();
    if clock::now() - p.0 > 600.0 {
        let (w, m): (Option<i64>, Option<i64>) = c.query_row("SELECT SUM(wins), SUM(matches) FROM players", [], |r| Ok((r.get(0)?, r.get(1)?))).map_err(es)?;
        let m = m.unwrap_or(0);
        if m != 0 {
            p.1 = f64::min(0.9, f64::max(0.1, w.unwrap_or(0) as f64 / m as f64));
        }
        p.0 = clock::now();
    }
    Ok(p.1)
}

// py: daemon.py:1401 elo_shown
pub fn elo_shown(e: f64, matches: i64) -> f64 {
    e - ELO_UNSURE / ((matches.max(0)) as f64 + 1.0).sqrt()
}

// py: daemon.py:1405 elo_key -> (key, steam id)
pub fn elo_key(c: &Connection, token: &str) -> ER<(String, i64)> {
    use rusqlite::OptionalExtension;
    let r = c.prepare_cached("SELECT steamid FROM players WHERE token=?").and_then(|mut s| s.query_row([token], |r| r.get::<_, Option<i64>>(0))).optional().map_err(es)?;
    let sid = r.flatten().unwrap_or(0);
    let key = if sid != 0 {
        format!("s{}", sid)
    } else {
        let tail: String = token.chars().skip(40).take(24).collect();
        format!("t{}", tail.to_lowercase())
    };
    Ok((key, sid))
}

/// glibc's `log10` (sysdeps/ieee754/dbl-64/e_log10.c) so the rating matches Python's `math.log10` to the last bit (musl's log10 differs by 1 ulp now and then)
pub fn glibc_log10(x: f64) -> f64 {
    const IVLN10: f64 = 4.34294481903251816668e-01;
    const LOG10_2HI: f64 = 3.01029995663611771306e-01;
    const LOG10_2LO: f64 = 3.69423907715893078616e-13;
    let mut x = x;
    let bits = x.to_bits();
    let mut hx = (bits >> 32) as i32;
    let lx = bits as u32;
    let mut k: i32 = 0;
    if hx < 0x0010_0000 {
        if ((hx & 0x7fff_ffff) as u32 | lx) == 0 {
            return f64::NEG_INFINITY;
        }
        if hx < 0 {
            return f64::NAN;
        }
        k -= 54;
        x *= 18014398509481984.0; // 2^54
        hx = (x.to_bits() >> 32) as i32;
    }
    if hx >= 0x7ff0_0000 {
        return x + x;
    }
    k += (hx >> 20) - 1023;
    let i = ((k as u32) & 0x8000_0000) >> 31;
    hx = (hx & 0x000f_ffff) | ((0x3ff - i as i32) << 20);
    let y = (k + i as i32) as f64;
    x = f64::from_bits(((hx as u32 as u64) << 32) | (x.to_bits() & 0xffff_ffff));
    let z = y * LOG10_2LO + IVLN10 * x.ln();
    z + y * LOG10_2HI
}

// py: daemon.py:1411 elo_seed
pub fn elo_seed(c: &Connection, _key: &str, sid: i64, token: &str, record: Option<(i64, i64, i64)>) -> ER<f64> {
    let (w, l, d);
    if let Some((rw, rl, rd)) = record {
        w = rw;
        l = rl;
        d = rd;
    } else {
        let row: Option<(Option<i64>, Option<i64>, Option<i64>)> = if sid != 0 {
            c.query_row("SELECT SUM(wins), SUM(matches), SUM(draws) FROM players WHERE steamid=?", [sid], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?))).ok()
        } else {
            let tail: String = token.chars().skip(40).take(24).collect();
            c.query_row("SELECT SUM(wins), SUM(matches), SUM(draws) FROM players WHERE substr(token,41,24)=?", [tail], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?))).ok()
        };
        let (a, m, dd) = row.unwrap_or((None, None, None));
        let (a, m, dd) = (a.unwrap_or(0), m.unwrap_or(0), dd.unwrap_or(0));
        w = a;
        d = dd;
        l = (m - a - dd).max(0);
    }
    let p0 = elo_pool_rate(c)?;
    let m = w + l + d;
    let wr = (w as f64 + 0.5 * d as f64 + ELO_PRIOR * p0) / (m as f64 + ELO_PRIOR);
    let arg = (wr / (1.0 - wr)) / (p0 / (1.0 - p0));
    if !(arg > 0.0) || !arg.is_finite() {
        return Err("math domain error".to_string());
    }
    Ok(ELO_BASE + 400.0 * glibc_log10(arg))
}

// py: daemon.py:1428 elo_ensure
pub fn elo_ensure(c: &Connection) -> ER<()> {
    // (prepared once per connection: elo_of_tokens calls this for every leaderboard row while holding the DB lock)
    c.prepare_cached("CREATE TABLE IF NOT EXISTS ratings(key TEXT PRIMARY KEY, elo REAL, n INT DEFAULT 0, updated REAL)").and_then(|mut s| s.execute([])).map(|_| ()).map_err(es)
}

// py: daemon.py:1432 elo_get -> (key, rating, rated matches)
pub fn elo_get(c: &Connection, token: &str, record: Option<(i64, i64, i64)>) -> ER<(String, f64, i64)> {
    use rusqlite::OptionalExtension;
    elo_ensure(c)?;
    let (key, sid) = elo_key(c, token)?;
    let r = c
        .prepare_cached("SELECT elo, n FROM ratings WHERE key=?")
        .and_then(|mut s| s.query_row([&key], |r| Ok((r.get::<_, f64>(0)?, r.get::<_, Option<i64>>(1)?))))
        .optional()
        .map_err(es)?;
    if let Some((e, n)) = r {
        return Ok((key, e, n.unwrap_or(0)));
    }
    let e = elo_seed(c, &key, sid, token, record)?;
    c.execute("INSERT OR IGNORE INTO ratings(key, elo, n, updated) VALUES(?,?,0,?)", rusqlite::params![key, e, clock::now()]).map_err(es)?;
    Ok((key, e, 0))
}

// py: daemon.py:1456 elo_perf
pub fn elo_perf(c: &Connection, mid: &str, tokens: &[String]) -> IndexMap<String, f64> {
    if mid.is_empty() || tokens.is_empty() {
        return IndexMap::new();
    }
    let rows: Vec<(String, Option<f64>, Option<f64>, Option<f64>, Option<f64>, Option<f64>)> = match (|| -> rusqlite::Result<_> {
        let mut st = c.prepare("SELECT token, parts, kills, downs, deaths, revives FROM match_stats WHERE mid=?")?;
        let it = st.query_map([mid], |r| Ok((r.get::<_, String>(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?, r.get(5)?)))?;
        let mut v = Vec::new();
        for x in it {
            v.push(x?);
        }
        Ok(v)
    })() {
        Ok(v) => v,
        Err(_) => return IndexMap::new(), // no table yet
    };
    let want: HashSet<&String> = tokens.iter().collect();
    let mut by_parts: IndexMap<String, f64> = IndexMap::new();
    let mut by_counts: IndexMap<String, f64> = IndexMap::new();
    for (tok, parts, k, dn, de, rv) in rows {
        if !want.contains(&tok) {
            continue;
        }
        if let Some(p) = parts {
            if p >= 0.0 {
                by_parts.insert(tok.clone(), p);
            }
        }
        if let Some(k) = k {
            if k >= 0.0 {
                let v = 2.0 * k + f64::max(dn.unwrap_or(0.0), 0.0) + f64::max(rv.unwrap_or(0.0), 0.0) - f64::max(de.unwrap_or(0.0), 0.0);
                by_counts.insert(tok, f64::max(0.0, v));
            }
        }
    }
    let sc = if by_parts.len() >= 2 { by_parts } else { by_counts }; // one measure for the whole match
    if sc.len() < 2 {
        return IndexMap::new();
    }
    let vals: Vec<f64> = sc.values().copied().collect();
    let mean = teams::py_sum(&vals) / sc.len() as f64;
    if mean <= 0.0 {
        return IndexMap::new();
    }
    sc.into_iter().map(|(t, v)| (t, v / mean)).collect()
}

// py: daemon.py:1479 elo_update
pub fn elo_update(players: &[String], winner: i64, skip: &HashSet<String>, mid: Option<&str>) {
    if !matches!(winner, 0 | 1 | 2) {
        return;
    }
    let r = db::with_write(|c| {
        use rusqlite::OptionalExtension;
        let mut side: [IndexMap<String, (f64, i64, String)>; 2] = [IndexMap::new(), IndexMap::new()];
        for (i, t) in players.iter().enumerate() {
            if t.is_empty() || t.chars().count() != 64 || skip.contains(t) {
                continue;
            }
            let has = c.query_row("SELECT 1 FROM players WHERE token=?", [t], |_| Ok(())).optional()?.is_some();
            if !has {
                continue;
            }
            let (key, e, n) = elo_get(c, t, None).map_err(|m| db::DbErr::Sql(rusqlite::Error::InvalidParameterName(m)))?;
            let k = team_of(i as i64) as usize;
            side[k].entry(key).or_insert((e, n, t.clone())); // one account twice in a match (two installs) counts once
        }
        if side[0].is_empty() || side[1].is_empty() {
            return Ok(());
        }
        let toks: Vec<String> = (0..2).flat_map(|k| side[k].values().map(|v| v.2.clone()).collect::<Vec<_>>()).collect();
        let perf = if crate::env_str("T2R_ELO_NO_PERF", "") == "1" { IndexMap::new() } else { elo_perf(c, mid.unwrap_or(""), &toks) };
        let mut mean = [0.0f64; 2];
        for k in 0..2 {
            let vals: Vec<f64> = side[k].values().map(|v| v.0).collect();
            mean[k] = teams::py_sum(&vals) / side[k].len() as f64;
        }
        let mut notes: Vec<String> = Vec::new();
        for k in 0..2usize {
            let exp = 1.0 / (1.0 + 10.0f64.powf((mean[1 - k] - mean[k]) / 400.0));
            let res = if winner == 2 {
                0.5
            } else if winner == k as i64 {
                1.0
            } else {
                0.0
            };
            for (key, (e, n, t)) in side[k].iter() {
                let kf = if *n < ELO_NEW_N { ELO_K_NEW } else { ELO_K };
                let mut d = kf * (res - exp);
                if let Some(p) = perf.get(t) {
                    let mut w = if d > 0.0 { 0.5 + 0.5 * p } else { 1.5 - 0.5 * p };
                    w = f64::min(ELO_PERF_HI, f64::max(ELO_PERF_LO, w));
                    d *= w;
                    notes.push(format!("{} p{} x{} {:+.1}", tag(t), py::fixed(*p, 2), py::fixed(w, 2), d));
                }
                c.execute("UPDATE ratings SET elo=?, n=n+1, updated=? WHERE key=?", rusqlite::params![e + d, clock::now(), key])?;
            }
        }
        let perf_s = if notes.is_empty() { "none (no stats: team result only)".to_string() } else { notes.join("; ") };
        let mid_s = mid.map(|m| m.to_string()).unwrap_or_else(|| "None".to_string());
        dlog!(
            "ELO match",
            mid_s,
            "winner",
            winner,
            format!("team means {} / {}", py::fixed(mean[0], 0), py::fixed(mean[1], 0)),
            "rated",
            side[0].len() + side[1].len(),
            "perf",
            perf_s
        );
        Ok(())
    });
    if let Err(ex) = r {
        dlog!("ELO update failed", ex);
    }
}

// py: daemon.py:1521 elo_of_tokens
pub fn elo_of_tokens(tokens: &[String], records: Option<&HashMap<String, (i64, i64, i64)>>) -> IndexMap<String, i64> {
    let mut out: IndexMap<String, i64> = IndexMap::new();
    let r = db::with_write(|c| {
        for t in tokens {
            let rec = records.and_then(|r| r.get(t)).copied();
            let e = elo_get(c, t, rec).map_err(|m| db::DbErr::Sql(rusqlite::Error::InvalidParameterName(m)))?.1;
            let m = match rec {
                Some((a, b, d)) => a + b + d,
                None => c
                    .prepare_cached("SELECT matches FROM players WHERE token=?")
                    .and_then(|mut s| s.query_row([t], |r| r.get::<_, Option<i64>>(0)))
                    .ok()
                    .flatten()
                    .unwrap_or(0),
            };
            out.insert(t.clone(), py::round0(elo_shown(e, m)));
        }
        Ok(())
    });
    if let Err(ex) = r {
        dlog!("ELO read failed", ex);
    }
    out
}

// py: daemon.py:1534 record_result
pub fn record_result(players: &[String], winner: i64, skip: &HashSet<String>, mid: Option<&str>) -> db::DbResult<()> {
    if let Some(m) = mid.filter(|m| !m.is_empty()) {
        if PRIV.is_private_mid(m) {
            dlog!("RESULT private match", m, "winner", winner, "- no wins / skill rating booked");
            return Ok(());
        }
    }
    if crate::env_str("T2R_ELO_NO_PERF", "") == "1" || mid.is_none_or(|m| m.is_empty()) {
        elo_update(players, winner, skip, mid);
    } else {
        // the players' own stats reports come after the host's result (elo_perf)
        let (p, s, m) = (players.to_vec(), skip.clone(), mid.map(|m| m.to_string()));
        py_timer(
            ELO_PERF_WAIT_S,
            Box::new(move || {
            elo_update(&p, winner, &s, m.as_deref());
            }),
        );
    }
    db::with_write(|c| {
        for (i, t) in players.iter().enumerate() {
            if skip.contains(t) {
                continue;
            }
            if winner == 2 {
                c.execute("UPDATE players SET draws = draws + 1 WHERE token = ?", [t])?;
            } else if team_in(mid.unwrap_or(""), i as i64, Some(t)) == winner {
                c.execute("UPDATE players SET wins = wins + 1 WHERE token = ?", [t])?;
            }
        }
        Ok(())
    })
}

/// py: daemon.py:10155 gc_state: `_prune_keys(cache, lambda t: cache[t][0] < mono)` for `_name_cache` and `_steam_cache` (K6 gc)
pub fn name_caches_gc() -> usize {
    let mono = mono();
    let mut s = NS.lock();
    let before = s.name_cache.len() + s.steam_cache.len();
    s.name_cache.retain(|_, e| !(e.exp < mono));
    s.steam_cache.retain(|_, e| !(e.exp < mono));
    before - s.name_cache.len() - s.steam_cache.len()
}
