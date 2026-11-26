//! K3 maps: the map list, respawns, pools, rotation / random pick, the map vote, Part I map preference, the SP-map / pack ops.
//! daemon.py:3519-4000 (MAPS ... vote_op, sp_support_op, map_packs_op).
use super::*;
use crate::{map_packs, py, rnd};
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::atomic::Ordering;
use std::sync::LazyLock;

/// `frozenset(players)` as a dictionary key.
pub type Key = BTreeSet<String>;
pub fn key_of(pl: &[String]) -> Key {
    pl.iter().cloned().collect()
}

/// a host-migration placeholder token (hostmig.placeholder): `"f0" * 30` prefix
pub fn is_placeholder(t: &str) -> bool {
    t.starts_with(&"f0".repeat(30))
}

/// `int(os.environ.get(name, default))`
pub fn env_int(name: &str, default: i64) -> i64 {
    match std::env::var(name) {
        Ok(v) => v.trim().parse().unwrap_or(default),
        Err(_) => default,
    }
}

/// `os.environ.get(name)` is truthy (set and not empty)
pub fn env_set(name: &str) -> bool {
    std::env::var(name).is_ok_and(|v| !v.is_empty())
}

fn csv(name: &str) -> Vec<String> {
    std::env::var(name).unwrap_or_default().split(',').map(|x| py::strip(x).to_string()).filter(|x| !x.is_empty()).collect()
}

// py: daemon.py:3519 MAPS (display name, image key, level name under to1-levels)
const BASE_MAPS: [(&str, &str, &str); 8] = [
    ("Forest", "forest", "to1-forest"),
    ("Gas Station", "gas-station", "to1-gas-station"),
    ("School", "school", "to1-school"),
    ("Shipyard", "shipyard", "to1-shipyard"),
    ("Resort", "resort", "to1-resort"),
    ("Seraphite Camp", "seraphite-camp", "to1-seraphite-camp"),
    ("Houses", "houses", "to1-jesse-houses"),
    ("Hospital", "hospital", "to1-custom-map-1"),
];

// py: daemon.py:3534 SP_MAPS (display name, image key, level name, key in sp_maps.json)
pub const SP_MAPS: [(&str, &str, &str, &str); 17] = [
    ("Hardware Store", "hardware", "to1-hardware", "hardware"),
    ("Motel", "motel", "to1-motel", "motel"),
    ("Hillcrest", "hillcrest-stores", "to1-hillcrest-stores", "hillcrest-stores"),
    ("TV Station", "tv-station", "to1-tv-station", "tv-station"),
    ("Train Station", "train-station", "to1-train-station", "train-station"),
    ("Downtown", "downtown", "to1-downtown", "downtown"),
    ("Lodge", "lodge", "to1-lodge", "lodge"),
    ("Jackson Suburbs", "jackson-suburbs", "to1-jackson-suburbs", "jackson-suburbs"),
    ("Suburbs", "outpost", "to1-outpost", "outpost"),
    ("Bookstore (Part I)", "p1-bookstore", "to1-p1-bookstore", "p1-bookstore"),
    ("Hillside", "wilds-houses", "to1-wilds-houses", "wilds-houses"),
    ("Capitol Hill Shops", "capitol-shops", "to1-capitol-shops", "capitol-shops"),
    ("Convention Center", "into-the-city", "to1-into-the-city", "into-the-city"),
    ("Overpass", "overpass", "to1-overpass", "overpass"),
    ("Chase", "chase", "to1-chase", "chase"),
    ("Supermarket", "jackson-market", "to1-jackson-market", "jackson-market"),
    ("Plaza (Part I)", "p1-plaza", "to1-p1-plaza", "p1-plaza"),
];

fn sp_maps_file() -> Option<serde_json::Value> {
    let b = std::fs::read(crate::gen_modes::tools_dir().join("maps").join("sp_maps.json")).ok()?;
    serde_json::from_slice::<serde_json::Value>(&b).ok()
}

// py: daemon.py:3563 _sp_retired: keys that are NOT part of an "all" build = sp_maps.json "build": false
pub fn sp_retired() -> HashSet<String> {
    let fallback = || ["hardware", "motel", "hillcrest-stores", "tv-station", "lodge"].iter().map(|s| s.to_string()).collect::<HashSet<String>>();
    let Some(spec) = sp_maps_file().and_then(|v| v.get("maps").cloned()).and_then(|m| if let serde_json::Value::Array(a) = m { Some(a) } else { None }) else {
        return fallback();
    };
    let mut out = HashSet::new();
    for m in &spec {
        let Some(o) = m.as_object() else { return fallback() };
        let build = o.get("build").is_some_and(truthy);
        let Some(k) = o.get("key") else { return fallback() };
        if !build {
            out.insert(py_str_of(k));
        }
    }
    out
}

/// Python truthiness of a JSON value
fn truthy(v: &serde_json::Value) -> bool {
    use serde_json::Value as V;
    match v {
        V::Null => false,
        V::Bool(b) => *b,
        V::Number(n) => n.as_f64().is_some_and(|x| x != 0.0),
        V::String(s) => !s.is_empty(),
        V::Array(a) => !a.is_empty(),
        V::Object(o) => !o.is_empty(),
    }
}

fn py_str_of(v: &serde_json::Value) -> String {
    match v {
        serde_json::Value::String(s) => s.clone(),
        o => py::pyrepr(o),
    }
}

// py: daemon.py:3578 BASE_MAX_TEAM
fn base_max_team() -> HashMap<String, i64> {
    [
        ("to1-gas-station", 2),
        ("to1-school", 2),
        ("to1-seraphite-camp", 4),
        ("to1-custom-map-1", 4),
        ("to1-resort", 4),
        ("to1-jesse-houses", 4),
        ("to1-shipyard", 4),
        ("to1-forest", 4),
    ]
    .iter()
    .map(|(k, v)| (k.to_string(), *v))
    .collect()
}

const SP_MAX_TEAM_FALLBACK: [&str; 11] = [
    "to1-train-station",
    "to1-downtown",
    "to1-jackson-suburbs",
    "to1-outpost",
    "to1-wilds-houses",
    "to1-capitol-shops",
    "to1-into-the-city",
    "to1-overpass",
    "to1-chase",
    "to1-jackson-market",
    "to1-p1-plaza",
];

/// `int(x or 4)` of a JSON value (None = int() raised)
fn int_or4(v: Option<&serde_json::Value>) -> Option<i64> {
    use serde_json::Value as V;
    let Some(v) = v else { return Some(4) };
    if !truthy(v) {
        return Some(4);
    }
    match v {
        V::Bool(_) => Some(1),
        V::Number(n) => n.as_i64().or_else(|| n.as_f64().map(|f| f.trunc() as i64)),
        V::String(s) => s.trim().parse::<i64>().ok(),
        _ => None,
    }
}

// py: daemon.py:3593 max_team_table: {level name: max team size}
pub fn max_team_table() -> HashMap<String, i64> {
    let mut t = base_max_team();
    let spec: Option<Vec<(String, i64)>> = (|| {
        let a = sp_maps_file()?.get("maps")?.as_array()?.clone();
        let mut out = Vec::new();
        for m in &a {
            let o = m.as_object()?;
            let lvl = py_str_of(o.get("level")?);
            out.push((lvl, int_or4(o.get("max_team"))?));
        }
        Some(out)
    })();
    match spec {
        Some(v) => t.extend(v),
        None => t.extend(SP_MAX_TEAM_FALLBACK.iter().map(|k| (k.to_string(), 4))),
    }
    t
}

// py: daemon.py:3608 team_size: players per team of a match with these tokens (even split, rounded up); nobody known = 4
pub fn team_size(players: &[String]) -> i64 {
    if players.is_empty() {
        4
    } else {
        (players.len() as i64 + 1) / 2
    }
}

// py: daemon.py:3615 ALWAYS_MAPS
const ALWAYS_MAPS: [&str; 1] = ["train-station"];

// py: daemon.py:3618 sp_maps_on: level names of the SP-section maps enabled by T2R_SP_MAPS
pub fn sp_maps_on() -> HashSet<String> {
    let env = py::strip(&std::env::var("T2R_SP_MAPS").unwrap_or_default()).to_string();
    let always: HashSet<String> = SP_MAPS.iter().filter(|m| ALWAYS_MAPS.contains(&m.3)).map(|m| m.2.to_string()).collect();
    if env.is_empty() || env == "0" || env == "none" {
        return always;
    }
    let mut keys: HashSet<String> = env.split(',').map(|k| py::strip(k).to_string()).collect();
    keys.extend(ALWAYS_MAPS.iter().map(|s| s.to_string()));
    let retired = sp_retired();
    SP_MAPS
        .iter()
        .filter(|m| ((env == "all" || env == "1") && !retired.contains(m.3)) || keys.contains(m.3))
        .map(|m| m.2.to_string())
        .collect()
}

// py: daemon.py:3636 RESPAWNS
fn base_respawns() -> HashMap<String, i64> {
    let mut r: HashMap<String, i64> = [
        ("to1-forest", 16),
        ("to1-gas-station", 11),
        ("to1-school", 11),
        ("to1-shipyard", 12),
        ("to1-resort", 12),
        ("to1-seraphite-camp", 10),
        ("to1-jesse-houses", 14),
        ("to1-hardware", 13),
        ("to1-motel", 13),
        ("to1-hillcrest-stores", 13),
        ("to1-tv-station", 13),
        ("to1-train-station", 15),
        ("to1-downtown", 20),
        ("to1-lodge", 13),
        ("to1-jackson-suburbs", 12),
        ("to1-outpost", 12),
        ("to1-p1-bookstore", 14),
        ("to1-wilds-houses", 12),
        ("to1-capitol-shops", 13),
        ("to1-into-the-city", 12),
        ("to1-overpass", 17),
        ("to1-chase", 13),
        ("to1-jackson-market", 12),
        ("to1-p1-plaza", 14),
    ]
    .iter()
    .map(|(k, v)| (k.to_string(), *v))
    .collect();
    r.insert("to1-custom-map-1".to_string(), if std::env::var("T2R_HOSPITAL_OLD").as_deref() == Ok("1") { 13 } else { 16 });
    r
}

// py: daemon.py:3655 _sp_respawn_counts
fn sp_respawn_counts(r: &mut HashMap<String, i64>) {
    let d = crate::gen_modes::tools_dir().join("maps").join("data");
    for (_n, _i, lvl, key) in SP_MAPS.iter() {
        let gp = d.join(format!("{}-respawns-gen.json", key));
        let sp = d.join(format!("{}-spawns.json", key));
        if !(gp.exists() && sp.exists()) {
            continue;
        }
        let len_of = |p: &std::path::Path| -> Option<usize> {
            let v: serde_json::Value = serde_json::from_slice(&std::fs::read(p).ok()?).ok()?;
            Some(v.get("respawns")?.as_array()?.len())
        };
        let (Some(a), Some(b)) = (len_of(&sp), len_of(&gp)) else { continue };
        r.insert(lvl.to_string(), std::cmp::min((a + b) as i64, 255));
    }
}

/// daemon.py:3684 PACKS: the DLC map packs (map_packs.py)
pub static PACKS: LazyLock<map_packs::Packs> = LazyLock::new(|| map_packs::Packs::new(if map_packs::enabled() { map_packs::load_registry(None) } else { Vec::new() }));
/// daemon.py:3685 SPSUP: which SP-section maps each client's DLL has (0xC8)
pub static SPSUP: LazyLock<map_packs::SpSupport> = LazyLock::new(|| map_packs::SpSupport::new(&SP_MAPS.iter().map(|m| m.2.to_string()).collect::<Vec<_>>()));

pub struct MapList {
    pub maps: Vec<(String, String, String)>,
    pub respawns: HashMap<String, i64>,
}

/// MAPS + RESPAWNS as built at import (daemon.py:3519-3702)
pub static MAPLIST: LazyLock<MapList> = LazyLock::new(|| {
    let mut maps: Vec<(String, String, String)> = BASE_MAPS.iter().map(|m| (m.0.to_string(), m.1.to_string(), m.2.to_string())).collect();
    if !sp_maps_on().is_empty() {
        // listed (all of them, fixed indices) only on a server that enables any: live lists stay as before
        maps.extend(SP_MAPS.iter().map(|m| (m.0.to_string(), m.1.to_string(), m.2.to_string())));
    }
    let mut respawns = base_respawns();
    sp_respawn_counts(&mut respawns);
    if !PACKS.maps().is_empty() {
        for m in SP_MAPS.iter() {
            if maps.len() >= map_packs::PACK_BASE {
                break;
            }
            let t = (m.0.to_string(), m.1.to_string(), m.2.to_string());
            if !maps.contains(&t) {
                maps.push(t);
            }
        }
        if maps.len() != map_packs::PACK_BASE {
            dlog!("MAPS ERR", "map list must reach PACK_BASE before the pack maps");
        }
        for (name, img, lvl, resp) in PACKS.maps() {
            maps.push((name, img, lvl.clone()));
            respawns.insert(lvl, resp);
        }
    }
    MapList { maps, respawns }
});

/// `MAPS`: [(name, image, level)]
pub fn maps_table() -> Vec<(String, String, String)> {
    MAPLIST.maps.clone()
}
/// `RESPAWNS.get(level, 0)` (the wire byte)
pub fn respawns_of(level: &str) -> u8 {
    MAPLIST.respawns.get(level).copied().unwrap_or(0).clamp(0, 255) as u8
}
/// `MAPS[idx][2]` (the level name), "" out of range
pub fn map_level(idx: i64) -> String {
    if WORLD_ON.load(Ordering::Relaxed) {
        return format!("map{}", idx);
    }
    usize::try_from(idx).ok().and_then(|i| MAPLIST.maps.get(i)).map(|m| m.2.clone()).unwrap_or_default()
}
/// the map name the logs print for a map index (`MAPS[idx][2]`)
pub fn map_name(idx: i64) -> String {
    map_level(idx)
}
/// `MAPS[idx][0]` (display name)
pub fn map_display(idx: i64) -> String {
    usize::try_from(idx).ok().and_then(|i| MAPLIST.maps.get(i)).map(|m| m.0.clone()).unwrap_or_default()
}

/// Python import-time side effects of the map block: the SPMAPS gating log line (call once at start)
pub fn init_maps() {
    let _ = &*MAPLIST;
    let mut on: Vec<String> = sp_maps_on().into_iter().collect();
    on.sort();
    dlog!(
        "SPMAPS",
        "gating",
        if map_packs::sp_gating() { "ON (a map is offered only when every player reported it, 0xC8)" } else { "OFF (T2R_SP_GATE=0: enabled SP maps are offered without any report)" },
        "| enabled:",
        if on.is_empty() { "none".to_string() } else { py::list_str(&on) }
    );
}

// py: daemon.py:3704 BROKEN_MAPS
pub static BROKEN_MAPS: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:3706 MATCH_MAP: frozenset(match tokens) -> map index (same for every player)
pub static MATCH_MAP: LazyLock<Mutex<IndexMap<Key, i64>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
type PoolLogKey = (i64, Vec<String>, usize);
pub static POOL_LOGGED: LazyLock<Mutex<HashSet<PoolLogKey>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// `len(_POOL_LOGGED)`
pub fn pool_logged_len() -> usize {
    POOL_LOGGED.lock().len()
}

/// `MATCH_MAP.get(key)`
pub fn match_map_get(key: &Key) -> Option<i64> {
    MATCH_MAP.lock().get(key).copied()
}
// py: daemon.py:3712 _interrogation_players
pub fn interrogation_players(players: &[String]) -> bool {
    if INT.mode_env == "int" || INT.mode_env == "interrogation" {
        return true;
    }
    let ps: HashSet<&String> = players.iter().collect();
    if ps.is_empty() {
        return false;
    }
    // (map_pool runs inside and outside MM.lock: the Python reads MM.matches without taking it)
    let hit = MM.lock.peek(|g| g.matches.iter().find(|(_, pl)| pl.iter().any(|x| ps.contains(x))).map(|(mid, _)| mid.clone()));
    match hit {
        Some(mid) => MODEQ.match_mode(&mid).as_deref() == Some("interrogation"),
        None => false,
    }
}

// py: daemon.py:3728 map_pool: levels in play, as indices into MAPS. `support` = whose packs / SP maps must be checked (a joiner: only itself)
pub fn map_pool(players: &[String], support: Option<&[String]>) -> Vec<i64> {
    if WORLD_ON.load(Ordering::Relaxed) {
        return (0..40).collect();
    }
    let maps = &MAPLIST.maps;
    // a host-migration placeholder token has no SP-map / pack report: placeholders are not players
    let players: Vec<String> = players.iter().filter(|p| !is_placeholder(p)).cloned().collect();
    let sup: Vec<String> = match support {
        None => players.clone(),
        Some(s) => s.iter().filter(|p| !is_placeholder(p)).cloned().collect(),
    };
    let want = csv("T2R_MAP_POOL");
    let mut sp_off: HashSet<String> = SP_MAPS.iter().map(|m| m.2.to_string()).collect::<HashSet<_>>().difference(&sp_maps_on()).cloned().collect();
    sp_off.extend(csv("T2R_MAP_EXCLUDE"));
    let n = team_size(&players);
    let big: HashSet<String> =
        if crate::env_str("T2R_MAX_TEAM", "1") != "0" { max_team_table().into_iter().filter(|(_, mt)| *mt < n).map(|(l, _)| l).collect() } else { HashSet::new() };
    let broken = BROKEN_MAPS.lock().clone();
    let mut idx: Vec<i64> = maps
        .iter()
        .enumerate()
        .filter(|(_, m)| {
            (if !want.is_empty() { want.contains(&m.2) } else { !broken.contains(&m.2) && !sp_off.contains(&m.2) })
                && !big.contains(&m.2)
                && PACKS.allowed(&m.2, &sup)
                && SPSUP.allowed(&m.2, &sup)
        })
        .map(|(i, _)| i as i64)
        .collect();
    if idx.is_empty() {
        // never an empty pool: the maps that fit this team size, else every non-SP / non-pack map
        idx = maps
            .iter()
            .enumerate()
            .filter(|(_, m)| !sp_off.contains(&m.2) && !big.contains(&m.2) && !PACKS.is_pack_map(&m.2) && !SPSUP.is_sp(&m.2))
            .map(|(i, _)| i as i64)
            .collect();
    }
    if idx.is_empty() {
        idx = maps.iter().enumerate().filter(|(_, m)| !sp_off.contains(&m.2) && !PACKS.is_pack_map(&m.2) && !SPSUP.is_sp(&m.2)).map(|(i, _)| i as i64).collect();
    }
    let mut dropped: Vec<String> = maps.iter().filter(|m| big.contains(&m.2) && !sp_off.contains(&m.2)).map(|m| m.2.clone()).collect();
    dropped.sort();
    // 11-02: an Interrogation match only gets maps that have lockbox spots; none left = unfiltered (start() then falls back to Supply Raid)
    if interrogation_players(&players) {
        let lb = &INT.lockboxes;
        let keep: Vec<i64> = if lb.is_empty() { idx.clone() } else { idx.iter().copied().filter(|i| lb.contains_key(i)).collect() };
        if !keep.is_empty() && keep.len() != idx.len() {
            let mut gone: Vec<String> = idx.iter().filter(|i| !lb.contains_key(i)).map(|i| maps[*i as usize].2.clone()).collect();
            gone.sort();
            dlog!("POOL", "Interrogation match: maps without lockboxes dropped:", py::list_str(&gone));
            idx = keep;
        }
    }
    let k: PoolLogKey = (n, dropped.clone(), idx.len());
    if POOL_LOGGED.lock().insert(k) {
        dlog!("POOL", format!("team size {}: {} maps, dropped by max_team: {}", n, idx.len(), if dropped.is_empty() { "none".to_string() } else { dropped.join(", ") }));
    }
    idx
}

/// daemon.py:3766 _rotation: rotation starts at a random map each server run
pub static ROTATION: LazyLock<Mutex<i64>> = LazyLock::new(|| Mutex::new(rnd::randbelow(MAPLIST.maps.len() as u64) as i64));

// py: daemon.py:3768 map_index: T2R_MAP = "rotate" (default), "random" or one level name
pub fn map_index(players: &[String]) -> i64 {
    let key = key_of(players);
    if let Some(v) = MATCH_MAP.lock().get(&key) {
        return *v;
    }
    let want = crate::env_str("T2R_MAP", "rotate");
    let pool = map_pool(players, None);
    let maps = &MAPLIST.maps;
    let idx: i64;
    if want == "rotate" {
        let mut r = ROTATION.lock();
        idx = if pool.is_empty() { 0 } else { pool[py::pymod(*r, pool.len() as i64) as usize] };
        *r += 1;
    } else if want == "random" {
        let mut r = ROTATION.lock();
        let ch: Vec<i64> = pool.iter().copied().filter(|i| *i != *r).collect();
        let choices = if ch.is_empty() { pool.clone() } else { ch };
        idx = if choices.is_empty() { 0 } else { choices[rnd::randbelow(choices.len() as u64) as usize] };
        *r = idx;
    } else {
        idx = maps
            .iter()
            .position(|m| m.2 == want && PACKS.allowed(&m.2, players) && SPSUP.allowed(&m.2, players))
            .map(|i| i as i64)
            .unwrap_or_else(|| pool.first().copied().unwrap_or(0));
    }
    if !key.is_empty() {
        MATCH_MAP.lock().insert(key, idx);
        dlog!("MAP", map_level(idx), py::list_str(&players.iter().map(|p| tag(p)).collect::<Vec<_>>()));
    }
    idx
}

// ---------------------------------------------------------------- MATCH FLOW (2013 find match): the map vote
pub const VOTE_OPTIONS: usize = 3;
pub static VOTE_SECS: LazyLock<i64> = LazyLock::new(|| env_int("T2R_VOTE_SECS", 15));
pub static BOOSTER_SECS: LazyLock<Option<i64>> = LazyLock::new(|| if env_set("T2R_BOOSTER_SECS") { Some(env_int("T2R_BOOSTER_SECS", 0)) } else { None });
pub static LOADING_SECS: LazyLock<i64> = LazyLock::new(|| env_int("T2R_LOADING_SECS", 0));

/// one `VOTES[key]` dict
#[derive(Clone, Debug)]
pub struct Vote {
    pub opts: Vec<i64>,
    pub votes: IndexMap<String, i64>,
    pub winner: i64,
}

/// daemon.py:3822 VOTES + _vote_lock (the one mutex is the `_vote_lock`; MATCH_MAP is taken inside it, never the other way round)
pub static VOTES: LazyLock<Mutex<IndexMap<Key, Vote>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));

// py: daemon.py:3825 map_vote_on
pub fn map_vote_on() -> bool {
    crate::env_str("T2R_MAP_VOTE", "") == "1"
}

// py: daemon.py:3828 match_flow_on
pub fn match_flow_on() -> bool {
    map_vote_on() || crate::env_str("T2R_MATCH_FLOW", "") == "1"
}

// py: daemon.py:3832 flow_stage_secs: (loadout, booster, loading) seconds
pub fn flow_stage_secs(secs: Option<i64>) -> (i64, i64, i64) {
    let secs = secs.unwrap_or_else(|| env_int("T2R_COUNTDOWN", 10));
    let lo = std::cmp::max(1, py::floordiv(secs, 2));
    let bo = BOOSTER_SECS.unwrap_or_else(|| std::cmp::max(1, secs - lo));
    (lo, std::cmp::max(1, bo), std::cmp::max(0, *LOADING_SECS))
}

// py: daemon.py:3843 flow_extra_secs
pub fn flow_extra_secs() -> i64 {
    if !match_flow_on() {
        return 0;
    }
    let (_, bo, ld) = flow_stage_secs(None);
    bo + ld + 1
}

// py: daemon.py:3853 VOTE_WEIGHT
fn vote_weight(level: &str) -> i64 {
    match level {
        "to1-train-station" | "to1-wilds-houses" | "to1-capitol-shops" | "to1-into-the-city" | "to1-overpass" | "to1-chase" | "to1-jackson-market" | "to1-p1-plaza" => 2,
        _ => 1,
    }
}

/// daemon.py:3856 VOTE_RECENT: map idx of the last votes' winners (newest last)
pub static VOTE_RECENT: LazyLock<Mutex<Vec<i64>>> = LazyLock::new(|| Mutex::new(Vec::new()));
/// daemon.py:3857 VOTE_SHOWN: map idx -> how many of the last votes offered it (decays: each vote halves the rest)
pub static VOTE_SHOWN: LazyLock<Mutex<IndexMap<i64, f64>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));

// py: daemon.py:3860 vote_options: n distinct maps of the pool
pub fn vote_options(n: usize, players: &[String]) -> Vec<i64> {
    let pool = map_pool(players, None);
    if crate::env_str("T2R_VOTE_ALL", "") == "1" {
        // test server: the whole pool, in pool order (the ext pages its 3 tiles over the list, MF_GRID = 20 cells)
        return pool.into_iter().take(20).collect();
    }
    let level = |i: i64| map_level(i);
    let recent: Vec<i64> = if pool.len() > n {
        let k = std::cmp::max(1, std::cmp::min(3, pool.len() as i64 - n as i64)) as usize;
        let r = VOTE_RECENT.lock();
        r[r.len().saturating_sub(k)..].to_vec()
    } else {
        Vec::new()
    };
    let mut cand: Vec<i64> = {
        let rs: HashSet<i64> = recent.iter().copied().collect();
        let ps: HashSet<i64> = pool.iter().copied().collect();
        if pool.len() - rs.intersection(&ps).count() >= n {
            pool.iter().copied().filter(|i| !recent.contains(i)).collect()
        } else {
            pool.clone()
        }
    };
    let mut out: Vec<i64> = Vec::new();
    // user 11-06: one slot always offers this map while it is in the pool (T2R_VOTE_ALWAYS=<map name>[,<map>...])
    for nm in std::env::var("T2R_VOTE_ALWAYS").unwrap_or_default().split(',').filter(|x| !x.is_empty()) {
        let hit: Vec<i64> = pool.iter().copied().filter(|i| level(*i) == nm && !out.contains(i)).collect();
        if !hit.is_empty() && out.len() < n {
            out.push(hit[0]);
            if let Some(p) = cand.iter().position(|c| *c == hit[0]) {
                cand.remove(p);
            }
        }
    }
    let shown = VOTE_SHOWN.lock().clone();
    // user 10-13: one slot goes to a favoured (ported) map when the pool has one (not the last winner)
    let fav: Vec<i64> = cand.iter().copied().filter(|i| vote_weight(&level(*i)) > 1 && shown.get(i).copied().unwrap_or(0.0) < 1.5).collect();
    if !fav.is_empty() && n > 0 {
        let pick = fav[rnd::randbelow(fav.len() as u64) as usize];
        out.push(pick);
        if let Some(p) = cand.iter().position(|c| *c == pick) {
            cand.remove(p);
        }
    }
    while !cand.is_empty() && out.len() < n {
        // tickets: favoured maps x2, every map fewer the more it was offered lately (12 base tickets: shown 0 -> 12, 1 -> 6, 2 -> 4)
        let mut tickets: Vec<i64> = Vec::new();
        for i in &cand {
            let k = ((12 * vote_weight(&level(*i))) as f64 / (1.0 + shown.get(i).copied().unwrap_or(0.0))) as i64;
            let k = std::cmp::max(1, k);
            for _ in 0..k {
                tickets.push(*i);
            }
        }
        let pick = tickets[rnd::randbelow(tickets.len() as u64) as usize];
        out.push(pick);
        if let Some(p) = cand.iter().position(|c| *c == pick) {
            cand.remove(p);
        }
    }
    // PART I MAPS = ALWAYS (every player of the match): at least one option is a Part I map of the pool
    if !out.is_empty() && p1_match(players) {
        let p1: Vec<i64> = pool.iter().copied().filter(|i| level(*i).contains(map_packs::P1_MARK)).collect();
        if !p1.is_empty() && !out.iter().any(|i| p1.contains(i)) {
            let l = out.len();
            out[l - 1] = p1[rnd::randbelow(p1.len() as u64) as usize];
        }
    }
    let mut s = VOTE_SHOWN.lock();
    for v in s.values_mut() {
        *v *= 0.5;
    }
    for i in &out {
        let cur = s.get(i).copied().unwrap_or(0.0);
        s.insert(*i, cur + 1.0);
    }
    out
}

// py: daemon.py:3917 vote_start: open the vote of a formed match; the placeholder map (first option) goes into the match record
pub fn vote_start(players: &[String]) -> Vec<i64> {
    let key = key_of(players);
    let opts = vote_options(VOTE_OPTIONS, players);
    {
        let mut v = VOTES.lock();
        v.insert(key.clone(), Vote { opts: opts.clone(), votes: IndexMap::new(), winner: -1 });
        MATCH_MAP.lock().insert(key, opts.first().copied().unwrap_or(0));
    }
    dlog!("VOTE open", py::list_str(&opts.iter().map(|i| map_level(*i)).collect::<Vec<_>>()), py::list_str(&players.iter().map(|p| tag(p)).collect::<Vec<_>>()));
    opts
}

// py: daemon.py:3927 vote_cast: cast or change a vote (option = 0..n-1). True when counted
pub fn vote_cast(token: &str, option: i64) -> bool {
    let pl = MM.players_of(token).unwrap_or_default();
    let lvl;
    {
        let mut g = VOTES.lock();
        let Some(v) = g.get_mut(&key_of(&pl)) else { return false };
        if v.winner >= 0 || !(0 <= option && (option as usize) < v.opts.len()) {
            return false;
        }
        v.votes.insert(token.to_string(), option);
        lvl = map_level(v.opts[option as usize]);
    }
    dlog!("VOTE", tag(token), "->", lvl);
    true
}

// py: daemon.py:3938 vote_tally
pub fn vote_tally(v: &Vote) -> Vec<i64> {
    let mut c = vec![0i64; v.opts.len()];
    for o in v.votes.values() {
        if 0 <= *o && (*o as usize) < c.len() {
            c[*o as usize] += 1;
        }
    }
    c
}

// py: daemon.py:3945 vote_close: pick the winner (most votes, a tie or no votes at random) and make it the match's map
pub fn vote_close(players: &[String]) -> Option<i64> {
    let key = key_of(players);
    let mut g = VOTES.lock();
    let v = g.get_mut(&key)?;
    if v.winner < 0 {
        let c = vote_tally(v);
        let mx = c.iter().copied().max().unwrap_or(0);
        let best: Vec<usize> = c.iter().enumerate().filter(|(_, n)| **n == mx).map(|(k, _)| k).collect();
        v.winner = v.opts[best[rnd::randbelow(best.len() as u64) as usize]];
        let w = v.winner;
        MATCH_MAP.lock().insert(key, w);
        {
            let mut r = VOTE_RECENT.lock();
            r.push(w);
            let l = r.len();
            if l > 6 {
                r.drain(..l - 6);
            }
        }
        dlog!("VOTE closed", map_level(w), py::list_int(&c), py::list_str(&players.iter().map(|p| tag(p)).collect::<Vec<_>>()));
    }
    Some(v.winner)
}

// py: daemon.py:3962 vote_state_text: the 0xEB reply
pub fn vote_state_text(token: &str) -> String {
    let pl = MM.players_of(token).unwrap_or_default();
    let Some(v) = VOTES.lock().get(&key_of(&pl)).cloned() else { return "st=0".to_string() };
    let c = vote_tally(&v);
    let mut t: Vec<String> = vec![format!("st={}", if v.winner >= 0 { 2 } else { 1 }), format!("n={}", v.opts.len())];
    for (k, m) in v.opts.iter().enumerate() {
        t.push(format!("m{}={} v{}={}", k, m, k, c[k]));
    }
    t.push(format!("my={}", v.votes.get(token).copied().unwrap_or(-1)));
    t.push(format!("w={}", v.winner));
    t.push(format!("pc={}", pl.len()));
    for (k, p) in pl.iter().take(8).enumerate() {
        let gn = get_name(p);
        let raw = if gn.is_empty() { format!("PLAYER {}", k + 1) } else { gn };
        let nm: String = raw.chars().map(|ch| if ch.is_alphanumeric() || ch == '-' || ch == '.' { ch } else { '_' }).take(20).collect();
        t.push(format!("p{}={}", k, nm));
        t.push(format!("pv{}={}", k, if v.votes.contains_key(p) { 1 } else { 0 }));
        t.push(format!("po{}={}", k, v.votes.get(p).copied().unwrap_or(-1))); // the roster row names the map this player voted for
        let rkw = rank_of_token(p); // the rank badge of the roster row
        t.push(format!("pk{}={}", k, rkw.0));
        t.push(format!("pw{}={}", k, rkw.1));
    }
    t.join(" ")
}

// py: daemon.py:3990 vote_op: ops 0xEA / 0xEC -> reply frame (0xEB), or None for a bad payload
pub fn vote_op(op: u8, pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 64 {
        return None;
    }
    let token = latin1(&pl[..64]);
    if !token.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    if op == 0xEC {
        if pl.len() < 65 {
            return None;
        }
        vote_cast(&token, pl[64] as i64);
    } else if op != 0xEA {
        return None;
    }
    Some(msg(0xEB, vote_state_text(&token).as_bytes()))
}

// ---------------------------------------------------------------- PART I MAPS preference (OPTIONS > MULTIPLAYER, 0xC8 flags byte)
/// daemon.py:3699 MATCH_P1: match id -> True when it was formed as an owners match with an ALWAYS player
pub static MATCH_P1: LazyLock<Mutex<IndexMap<String, bool>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));

// py: daemon.py:3703 p1_pref_on
pub fn p1_pref_on() -> bool {
    if WORLD_ON.load(Ordering::Relaxed) {
        return false;
    }
    if crate::env_str("T2R_P1_PREF", "1") == "0" || !map_packs::sp_gating() {
        return false;
    }
    let mut on = sp_maps_on();
    on.extend(csv("T2R_MAP_POOL"));
    SP_MAPS.iter().any(|m| m.2.contains(map_packs::P1_MARK) && on.contains(m.2))
}

// py: daemon.py:3710 p1_always: is this searcher (its party leader when it has one) counted as PART I MAPS = ALWAYS?
pub fn p1_always(token: &str, leader: Option<&str>) -> bool {
    let t = leader.filter(|l| !l.is_empty()).unwrap_or(token);
    p1_pref_on() && SPSUP.p1_always(t)
}

// py: daemon.py:3718 p1_owner
pub fn p1_owner(token: &str, leader: Option<&str>) -> bool {
    p1_pref_on() && (SPSUP.p1_owner(token) || p1_always(token, leader))
}

// py: daemon.py:3726 p1_fits: may `token` join the running match `mid`?
pub fn p1_fits(token: &str, mid: &str, leader: Option<&str>) -> bool {
    if !p1_pref_on() {
        return true;
    }
    let Some(m) = MATCH_P1.lock().get(mid).copied() else { return true };
    if m {
        p1_owner(token, leader)
    } else {
        !p1_always(token, leader)
    }
}

// py: daemon.py:3735 p1_match: true when one of these players is ALWAYS
pub fn p1_match(players: &[String]) -> bool {
    let ps: Vec<&String> = players.iter().filter(|p| !p.starts_with(&"f0".repeat(30))).collect();
    !ps.is_empty() && p1_pref_on() && ps.iter().any(|p| SPSUP.p1_always(p))
}

// py: daemon.py:3969 sp_support_op: op 0xC8 [64B token][u32 LE mask] -> reply 0xC9
pub fn sp_support_op(pl: &[u8]) -> Option<Vec<u8>> {
    let (token, mask) = map_packs::parse_sp_report(pl)?;
    if !map_packs::sp_gating() {
        return Some(msg(map_packs::SP_OP_REPLY, b"on=0"));
    }
    let flags = map_packs::parse_sp_flags(pl);
    SPSUP.report(&token, mask, flags as u64);
    let names: Vec<String> = SP_MAPS.iter().enumerate().filter(|(k, _)| (mask >> k) & 1 == 1).map(|(_, m)| m.2.to_string()).collect();
    dlog!("SPMAPS", tag(&token), format!("{:04x}", mask), py::list_str(&names), format!("| PART I MAPS: {}", if SPSUP.p1_always(&token) { "ALWAYS" } else { "DEFAULT" }));
    Some(msg(map_packs::SP_OP_REPLY, SPSUP.reply_text(&token).as_bytes()))
}

// py: daemon.py:3982 map_packs_op: op 0xC6 [64B token][u8 n][n x u32 hash] -> reply 0xC7, on=0 with the flag off
pub fn map_packs_op(pl: &[u8]) -> Option<Vec<u8>> {
    let (token, hashes) = map_packs::parse_report(pl)?;
    if !map_packs::enabled() {
        return Some(msg(map_packs::OP_REPLY, b"on=0"));
    }
    let ids = PACKS.report(&token, &hashes);
    let ids: Vec<String> = ids.into_iter().collect();
    dlog!("PACKS", tag(&token), py::list_str(&hashes.iter().map(|h| format!("{:08x}", h)).collect::<Vec<_>>()), "->", py::list_str(&ids));
    let ml: Vec<(String, String, String, i64)> = MAPLIST.maps.iter().map(|m| (m.0.clone(), m.1.clone(), m.2.clone(), 0)).collect();
    Some(msg(map_packs::OP_REPLY, PACKS.reply_text(&token, &ml).as_bytes()))
}
