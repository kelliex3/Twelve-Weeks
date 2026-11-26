//! py: clan.py - 2013 Factions clan (the 12 weeks): server side rules + storage. State dicts are `serde_json::Value` objects (insertion
//! order kept: the Python dicts are dumped with `json.dumps`), DB functions take the sqlite connection.
use crate::clan::clan_supply;
use crate::clan::data;
use crate::{clock, py, rnd};
use parking_lot::Mutex;
use rusqlite::types::ValueRef;
use rusqlite::{Connection, OptionalExtension};
use serde_json::{json, Map, Value};
use std::collections::{BTreeMap, HashMap};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::PyRandom;

// ------------------------------------------------------------------ small Python-like helpers (shared by the clan modules)

/// The match's mission counts `{label id: n}`.
pub type Counts = HashMap<i64, i64>;

/// Python `int(v)` of a JSON value (ints as they are, floats truncated, numeric strings parsed, anything else 0).
pub fn int_of(v: &Value) -> i64 {
    match v {
        Value::Number(n) => n.as_i64().or_else(|| n.as_u64().map(|u| u as i64)).unwrap_or_else(|| n.as_f64().map(|f| f.trunc() as i64).unwrap_or(0)),
        Value::Bool(b) => *b as i64,
        Value::String(s) => s.trim().parse().unwrap_or(0),
        _ => 0,
    }
}

/// Python truthiness of a JSON value.
pub fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().map(|f| f != 0.0).unwrap_or(true),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

/// Python `str(v)` of a JSON value (str raw, the rest as `repr`).
pub fn pystr(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        o => py::pyrepr(o),
    }
}

/// `d.get(k, default)` as int.
pub fn gi(v: &Value, k: &str, d: i64) -> i64 {
    v.get(k).map(int_of).unwrap_or(d)
}

fn gm(m: &Map<String, Value>, k: &str, d: i64) -> i64 {
    m.get(k).map(int_of).unwrap_or(d)
}

pub fn vref_to_json(v: ValueRef) -> Value {
    match v {
        ValueRef::Null => Value::Null,
        ValueRef::Integer(i) => Value::from(i),
        ValueRef::Real(f) => serde_json::Number::from_f64(f).map(Value::Number).unwrap_or(Value::Null),
        ValueRef::Text(t) => Value::String(String::from_utf8_lossy(t).into_owned()),
        ValueRef::Blob(_) => Value::Null,
    }
}

pub fn to_sql(v: &Value) -> rusqlite::types::Value {
    use rusqlite::types::Value as S;
    match v {
        Value::Null => S::Null,
        Value::Bool(b) => S::Integer(*b as i64),
        Value::Number(n) => match n.as_i64() {
            Some(i) => S::Integer(i),
            None => S::Real(n.as_f64().unwrap_or(0.0)),
        },
        Value::String(s) => S::Text(s.clone()),
        o => S::Text(py::dumps_compact(o)),
    }
}

/// `{"1": n}` JSON object -> `{1: n}` counts.
pub fn counts_from_json(v: &Value) -> Counts {
    let mut out = Counts::new();
    if let Some(o) = v.as_object() {
        for (k, x) in o {
            if let Ok(i) = k.trim().parse::<i64>() {
                out.insert(i, int_of(x));
            }
        }
    }
    out
}

/// `print(...)` of the module (stdout of the daemon). A test can capture the lines.
static CAPTURE: Mutex<Option<Vec<String>>> = Mutex::new(None);

pub fn capture_prints(on: bool) {
    *CAPTURE.lock() = if on { Some(Vec::new()) } else { None };
}

pub fn take_prints() -> Vec<String> {
    CAPTURE.lock().as_mut().map(std::mem::take).unwrap_or_default()
}

pub fn print_line(s: &str) {
    let mut g = CAPTURE.lock();
    match g.as_mut() {
        Some(v) => v.push(s.to_string()),
        None => println!("{}", s),
    }
}

/// `random.Random()` of clan.py (`clan.rng`): seeded from `T2R_TEST_SEED + 1` when set (the Python tests do `clan.rng = random.Random(seed + 1)`), else entropy.
pub static RNG: LazyLock<Mutex<PyRandom>> = LazyLock::new(|| Mutex::new(crate::rnd::unseeded()));

fn rng() -> parking_lot::MutexGuard<'static, PyRandom> {
    RNG.lock()
}

// ------------------------------------------------------------------ constants (generated table = truth; `consts_ok` compares)
pub const WEEKS: i64 = 12;
pub const DAYS: i64 = WEEKS * 7;
pub const WIN_DAY: i64 = 83;
pub const START_SURVIVORS: i64 = 5;
pub const MAX_CLAN: i64 = 512;
pub const PARTS_PER_SUPPLY: i64 = 50;
pub const NONE: i64 = 0;
pub const WLF: i64 = 1;
pub const SERAPHITES: i64 = 2;
pub const ALIVE: i64 = 1;
pub const DEAD: i64 = 2;
pub const WON: i64 = 3;
pub const HUNGRY_PER_DEFICIT: f64 = 0.75;
pub const SICK_PER_DEFICIT: f64 = 0.3;
pub const SPIRAL_SICK_SHARE: f64 = 0.9;
pub const DEATH_PER_SICK: f64 = 0.2;
pub const DEATH_CLAMP_OVER: i64 = 12;
pub const DEATH_CLAMP_TO: i64 = 10;
pub const HEAL_HUNGRY_PER_SURPLUS: f64 = 0.35;
pub const HEAL_SICK_PER_SURPLUS: f64 = 0.3;
pub const GROW_PER_SURPLUS: f64 = 0.1;
pub const DAY_MULT_LIMIT: f64 = 1.6;
pub const IDLE_MULT: f64 = 2.0;
pub const EVENT_LEN: i64 = 3;
pub const ATTACK_PARTS_BONUS: f64 = 0.10;
pub const EV_NONE: i64 = 0;
pub const EV_GROWTH: i64 = 1;
pub const EV_ATTACK: i64 = 2;
pub const AN_ENEMY: i64 = 0;
pub const AN_HORDE: i64 = 1;
pub const GN_SPOIL: i64 = 5;
pub const MISSION_DAYS: i64 = EVENT_LEN;
pub const MISSION_REPEAT_MAX: i64 = 5;
pub const AUTO_EVENT_MISSION: i64 = 1;
pub const BOOSTER_EVERY: i64 = 5;
pub const BOOSTER_MAX: i64 = 99;
pub const RANK_MAX_WEEKS: i64 = 999;
pub const RANK_MAX_DAYS: i64 = RANK_MAX_WEEKS * 7 + 6;
pub const RANK_SYMBOLS: i64 = 10;
pub const COLS: [&str; 14] = [
    "faction", "status", "day", "healthy", "hungry", "sick", "last_mid", "last_result", "weeks_total", "runs_won", "runs_lost", "days_total", "boost_hi", "meta",
];
pub const TEXT_COLS: [&str; 3] = ["last_mid", "last_result", "meta"];
pub const SNAP_TABLES: [&str; 5] = ["clans", "boosters", "clan_story", "clan_feed", "clan_alife"];

fn t(name: &str) -> &'static Value {
    data::t("clan", name)
}

fn ti(name: &str) -> i64 {
    t(name).as_i64().unwrap_or(i64::MIN)
}

fn tf(name: &str) -> f64 {
    t(name).as_f64().unwrap_or(f64::NAN)
}

fn tints(name: &str) -> Vec<i64> {
    t(name).as_array().map(|a| a.iter().filter_map(|x| x.as_i64()).collect()).unwrap_or_default()
}

fn tfloats(v: &Value) -> Vec<f64> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_f64()).collect()).unwrap_or_default()
}

/// The hand-typed constants above equal the generated table (a failing name is returned).
pub fn consts_ok() -> Result<(), String> {
    let ints: &[(&str, i64)] = &[
        ("WEEKS", WEEKS), ("DAYS", DAYS), ("WIN_DAY", WIN_DAY), ("START_SURVIVORS", START_SURVIVORS), ("MAX_CLAN", MAX_CLAN), ("PARTS_PER_SUPPLY", PARTS_PER_SUPPLY),
        ("NONE", NONE), ("WLF", WLF), ("SERAPHITES", SERAPHITES), ("ALIVE", ALIVE), ("DEAD", DEAD), ("WON", WON), ("DEATH_CLAMP_OVER", DEATH_CLAMP_OVER),
        ("DEATH_CLAMP_TO", DEATH_CLAMP_TO), ("EVENT_LEN", EVENT_LEN), ("EV_NONE", EV_NONE), ("EV_GROWTH", EV_GROWTH), ("EV_ATTACK", EV_ATTACK), ("AN_ENEMY", AN_ENEMY),
        ("AN_HORDE", AN_HORDE), ("GN_SPOIL", GN_SPOIL), ("MISSION_DAYS", MISSION_DAYS), ("MISSION_REPEAT_MAX", MISSION_REPEAT_MAX), ("AUTO_EVENT_MISSION", AUTO_EVENT_MISSION),
        ("BOOSTER_EVERY", BOOSTER_EVERY), ("BOOSTER_MAX", BOOSTER_MAX), ("RANK_MAX_WEEKS", RANK_MAX_WEEKS), ("RANK_MAX_DAYS", RANK_MAX_DAYS), ("RANK_SYMBOLS", RANK_SYMBOLS),
    ];
    for &(k, v) in ints {
        if ti(k) != v {
            return Err(k.to_string());
        }
    }
    let fl: [(&str, f64); 9] = [
        ("HUNGRY_PER_DEFICIT", HUNGRY_PER_DEFICIT), ("SICK_PER_DEFICIT", SICK_PER_DEFICIT), ("SPIRAL_SICK_SHARE", SPIRAL_SICK_SHARE), ("DEATH_PER_SICK", DEATH_PER_SICK),
        ("HEAL_HUNGRY_PER_SURPLUS", HEAL_HUNGRY_PER_SURPLUS), ("HEAL_SICK_PER_SURPLUS", HEAL_SICK_PER_SURPLUS), ("GROW_PER_SURPLUS", GROW_PER_SURPLUS),
        ("DAY_MULT_LIMIT", DAY_MULT_LIMIT), ("IDLE_MULT", IDLE_MULT),
    ];
    for (k, v) in fl {
        if tf(k) != v {
            return Err(k.to_string());
        }
    }
    if tf("ATTACK_PARTS_BONUS") != ATTACK_PARTS_BONUS {
        return Err("ATTACK_PARTS_BONUS".into());
    }
    if data::strs(t("COLS")) != COLS || data::strs(t("TEXT_COLS")) != TEXT_COLS || data::strs(t("SNAP_TABLES")) != SNAP_TABLES {
        return Err("COLS / TEXT_COLS / SNAP_TABLES".into());
    }
    if !clan_supply::consts_ok() {
        return Err("clan_supply".into());
    }
    Ok(())
}

// ------------------------------------------------------------------ tables derived from the generated data
pub struct Mission {
    pub id: i64,
    pub name: String,
    pub label: String,
    pub goals: Vec<i64>,
    pub live: bool,
}

/// `MISSIONS` in Python insertion order.
pub static MISSIONS: LazyLock<Vec<Mission>> = LazyLock::new(|| {
    data::dict_items(t("MISSIONS"))
        .into_iter()
        .filter_map(|(k, v)| {
            let a = v.as_array()?;
            Some(Mission {
                id: k.as_i64()?,
                name: a.first()?.as_str()?.to_string(),
                label: a.get(1)?.as_str()?.to_string(),
                goals: a.get(2)?.as_array()?.iter().filter_map(|x| x.as_i64()).collect(),
                live: a.get(3).and_then(|x| x.as_bool()).unwrap_or(false),
            })
        })
        .collect()
});

pub fn mission(id: i64) -> Option<&'static Mission> {
    MISSIONS.iter().find(|m| m.id == id)
}

/// `EVENT_DAYS[day]` = (is a growth event, the tier percentages).
pub struct EventDay {
    pub growth: bool,
    pub pcts: Vec<f64>,
}

pub static EVENT_DAYS: LazyLock<HashMap<i64, EventDay>> = LazyLock::new(|| {
    data::dict_items(t("EVENT_DAYS"))
        .into_iter()
        .filter_map(|(k, v)| {
            let a = v.as_array()?;
            Some((k.as_i64()?, EventDay { growth: a.first()?.as_str()? == "growth", pcts: tfloats(a.get(1)?) }))
        })
        .collect()
});

pub static GROWTH: LazyLock<Vec<f64>> = LazyLock::new(|| tfloats(t("GROWTH")));
pub static BOOSTER_ROWS: LazyLock<Vec<i64>> = LazyLock::new(|| tints("BOOSTER_ROWS"));
pub static LIVE_BOOSTERS: LazyLock<Vec<i64>> = LazyLock::new(|| tints("LIVE_BOOSTERS"));
pub static OFFER_POOL: LazyLock<Vec<i64>> = LazyLock::new(|| tints("OFFER_POOL"));
pub static EXEC_POOL: LazyLock<Vec<i64>> = LazyLock::new(|| tints("EXEC_POOL"));
pub static EXEC_POOL_PICK: LazyLock<Vec<i64>> = LazyLock::new(|| tints("EXEC_POOL_PICK"));
pub static HORDE_DAYS: LazyLock<Vec<i64>> = LazyLock::new(|| tints("HORDE_DAYS"));
pub static SPOIL_DAYS: LazyLock<Vec<i64>> = LazyLock::new(|| tints("SPOIL_DAYS"));
/// `BOOSTERS` (id -> name) in Python insertion order.
pub static BOOSTERS: LazyLock<Vec<(i64, String)>> =
    LazyLock::new(|| data::dict_items(t("BOOSTERS")).into_iter().filter_map(|(k, v)| Some((k.as_i64()?, v.as_str()?.to_string()))).collect());
static SCHEMA: LazyLock<String> = LazyLock::new(|| t("SCHEMA").as_str().unwrap_or("").to_string());
static BOOSTER_SCHEMA: LazyLock<String> = LazyLock::new(|| t("BOOSTER_SCHEMA").as_str().unwrap_or("").to_string());
static SNAP_SCHEMA: LazyLock<String> = LazyLock::new(|| t("SNAP_SCHEMA").as_str().unwrap_or("").to_string());

pub fn faction_name(f: i64) -> &'static str {
    for (k, v) in data::dict_items(t("FACTION_NAMES")) {
        if k.as_i64() == Some(f) {
            return v.as_str().unwrap_or("");
        }
    }
    ""
}

// ------------------------------------------------------------------ the switch for the two new event stories
static NE: Mutex<(Option<std::time::SystemTime>, bool)> = Mutex::new((None, true));

fn new_events_file() -> String {
    data::here("@HERE@/clan_events.txt")
}

// py: clan.py:84 new_events
/// The horde / spoiled supplies stories are told (default ON; the switch file is re-read when it changes; the env wins).
pub fn new_events() -> bool {
    if let Ok(e) = std::env::var("T2R_CLAN_NEW_EVENTS") {
        if !py::strip(&e).is_empty() {
            let l = py::strip(&e).to_lowercase();
            return matches!(l.as_str(), "1" | "on" | "true" | "yes");
        }
    }
    let file = new_events_file();
    let m = match std::fs::metadata(&file).and_then(|md| md.modified()) {
        Ok(m) => m,
        Err(_) => return true,
    };
    let mut g = NE.lock();
    if g.0 != Some(m) {
        let w = match std::fs::read(&file) {
            Ok(b) => {
                let mut s = String::new();
                for ch in b.utf8_chunks() {
                    s.push_str(ch.valid());
                }
                s.split(py::is_py_space).find(|x| !x.is_empty()).map(|x| py::strip(x).to_lowercase()).unwrap_or_default()
            }
            Err(_) => String::new(),
        };
        *g = (Some(m), !matches!(w.as_str(), "0" | "off" | "false" | "no"));
    }
    g.1
}

// py: clan.py:103 attack_story
/// AN_HORDE / AN_ENEMY: the story of the attack that settled on day index `day` or the day before.
pub fn attack_story(day: i64) -> i64 {
    for d in [day, day - 1] {
        let s = event_window(d);
        if s >= 0 && !EVENT_DAYS.get(&s).map(|e| e.growth).unwrap_or(true) {
            return if new_events() && HORDE_DAYS.contains(&s) { AN_HORDE } else { AN_ENEMY };
        }
    }
    AN_ENEMY
}

// py: clan.py:112 event_name
/// The event's name in capitals (the update server view, logs).
pub fn event_name(kind: i64, name_id: i64) -> String {
    if kind == EV_ATTACK {
        let a = data::strs(t("ATTACK_NAMES"));
        let i = if 0 <= name_id && (name_id as usize) < a.len() { name_id as usize } else { 0 };
        return a.get(i).map(|s| s.to_string()).unwrap_or_default();
    }
    if kind == EV_GROWTH {
        let g = data::strs(t("GROWTH_NAMES_ALL"));
        if g.is_empty() {
            return String::new();
        }
        return g[py::pymod(name_id, g.len() as i64) as usize].to_string();
    }
    String::new()
}

// py: clan.py:120 f32
/// Round to float32 like the engine's lambdas do.
pub fn f32(x: f64) -> f64 {
    (x as f32) as f64
}

// py: clan.py:125 event_today
/// (kind, name id) of the event STARTING on day index `day`; (0, 0) = none.
pub fn event_today(day: i64) -> (i64, i64) {
    let Some(e) = EVENT_DAYS.get(&day) else { return (EV_NONE, 0) };
    let new = new_events();
    if e.growth {
        let n = data::strs(t("GROWTH_NAMES")).len() as i64;
        return (EV_GROWTH, if new && SPOIL_DAYS.contains(&day) { GN_SPOIL } else { py::floordiv(day, 5) % n });
    }
    (EV_ATTACK, if new && HORDE_DAYS.contains(&day) { AN_HORDE } else { AN_ENEMY })
}

// py: clan.py:137 event_window
/// Start day of the event whose three matches contain day index `day`, or -1.
pub fn event_window(day: i64) -> i64 {
    let stop = (day - EVENT_LEN).max(-1);
    let mut s = day;
    while s > stop {
        if EVENT_DAYS.contains_key(&s) {
            return s;
        }
        s -= 1;
    }
    -1
}

// py: clan.py:145 rnd
/// Half up (survivors are whole people).
pub fn rnd(x: f64) -> i64 {
    (x + 0.5).floor() as i64
}

// py: clan.py:150 roll
/// The engine's fractional loop: floor(x) always, one more with probability frac(x) (x <= 0: nothing).
pub fn roll(x: f64) -> i64 {
    if x <= 0.0 {
        return 0;
    }
    let mut n = x as i64;
    if rng().random() < x - n as f64 {
        n += 1;
    }
    n
}

// py: clan.py:160 token_id
/// 12-byte install id = token bytes 20..31 (hex chars 40..63); whole token if not 64 hex.
pub fn token_id(token: &str) -> String {
    let t = token.to_lowercase();
    let n = t.chars().count();
    if n == 64 {
        t.chars().skip(40).take(24).collect()
    } else {
        t
    }
}

// py: clan.py:166 needed
pub fn needed(n: i64) -> i64 {
    if n > 0 {
        5 + 2 * n - py::floordiv(6 * n, 5)
    } else {
        0
    }
}

// py: clan.py:172 supplies_of
/// Whole supplies of a match: supply bottles + trunc(parts * 0.02f).
pub fn supplies_of(parts: i64, bottles: i64) -> i64 {
    bottles.max(0) + f32(parts.max(0) as f64 * f32(0.02)).trunc() as i64
}

// py: clan.py:177 week_day
/// day = days done (0..84) -> (week, day of week) of the day being played, 1-based.
pub fn week_day(day: i64) -> (i64, i64) {
    let d = day.min(DAYS - 1);
    (py::floordiv(d, 7) + 1, py::pymod(d, 7) + 1)
}

// py: clan.py:183 new_clan
pub fn new_clan(faction: i64) -> Value {
    json!({"faction": faction, "status": ALIVE, "day": 0, "healthy": START_SURVIVORS, "hungry": 0, "sick": 0, "boost_hi": START_SURVIVORS / BOOSTER_EVERY})
}

// ------------------------------------------------------------------ the members
#[derive(Clone, Copy, PartialEq)]
enum Cat {
    H,
    U,
    S,
    Su,
}

// py: clan.py:189 class Crowd
/// The clan as four counts: h = neither, u = hungry only, s = sick only, su = both.
#[derive(Clone, Debug)]
pub struct Crowd {
    pub h: i64,
    pub u: i64,
    pub s: i64,
    pub su: i64,
}

impl Crowd {
    pub fn new(h: i64, hu: i64, s: i64, sh: i64) -> Crowd {
        let sh = sh.min(s).max(0);
        Crowd { h, u: hu, s: s - sh, su: sh }
    }
    pub fn size(&self) -> i64 {
        self.h + self.u + self.s + self.su
    }
    pub fn hungry_shown(&self) -> i64 {
        self.u
    }
    pub fn sick_shown(&self) -> i64 {
        self.s + self.su
    }

    fn pick(&self, weights: &[(Cat, i64)]) -> Option<Cat> {
        let tot: i64 = weights.iter().map(|w| w.1).sum();
        if tot <= 0 {
            return None;
        }
        let mut k = rng().randbelow(tot as u64) as i64;
        for &(name, c) in weights {
            if k < c {
                return Some(name);
            }
            k -= c;
        }
        None
    }

    fn dec(&mut self, c: Cat) {
        match c {
            Cat::H => self.h -= 1,
            Cat::U => self.u -= 1,
            Cat::S => self.s -= 1,
            Cat::Su => self.su -= 1,
        }
    }

    // py: clan.py:215 make_hungry
    pub fn make_hungry(&mut self) -> bool {
        let w = self.pick(&[(Cat::H, self.h), (Cat::S, self.s)]);
        match w {
            Some(Cat::H) => {
                self.h -= 1;
                self.u += 1;
            }
            Some(Cat::S) => {
                self.s -= 1;
                self.su += 1;
            }
            _ => {}
        }
        w.is_some()
    }

    // py: clan.py:221 make_sick
    /// 1 = a member fell sick, -1 = a sick one was picked (wasted), 0 = nobody.
    pub fn make_sick(&mut self) -> i64 {
        match self.pick(&[(Cat::H, self.h), (Cat::U, self.u), (Cat::S, self.s), (Cat::Su, self.su)]) {
            Some(Cat::H) => {
                self.h -= 1;
                self.s += 1;
                1
            }
            Some(Cat::U) => {
                self.u -= 1;
                self.su += 1;
                1
            }
            Some(_) => -1,
            None => 0,
        }
    }

    // py: clan.py:227 heal_hungry
    pub fn heal_hungry(&mut self) -> bool {
        let w = self.pick(&[(Cat::U, self.u), (Cat::Su, self.su)]);
        match w {
            Some(Cat::U) => {
                self.u -= 1;
                self.h += 1;
            }
            Some(Cat::Su) => {
                self.su -= 1;
                self.s += 1;
            }
            _ => {}
        }
        w.is_some()
    }

    // py: clan.py:233 heal_sick
    pub fn heal_sick(&mut self) -> bool {
        let w = self.pick(&[(Cat::S, self.s), (Cat::Su, self.su)]);
        match w {
            Some(Cat::S) => {
                self.s -= 1;
                self.h += 1;
            }
            Some(Cat::Su) => {
                self.su -= 1;
                self.u += 1;
            }
            _ => {}
        }
        w.is_some()
    }

    // py: clan.py:239 kill
    /// A random sick member, else a hungry one, else any.
    pub fn kill(&mut self) -> bool {
        let w = if self.s + self.su != 0 {
            self.pick(&[(Cat::S, self.s), (Cat::Su, self.su)])
        } else if self.u != 0 {
            Some(Cat::U)
        } else if self.h != 0 {
            Some(Cat::H)
        } else {
            None
        };
        if let Some(c) = w {
            self.dec(c);
        }
        w.is_some()
    }

    // py: clan.py:250 add
    /// New members are healthy (512 slots at most).
    pub fn add(&mut self, n: i64) -> i64 {
        let n = n.min(MAX_CLAN - self.size());
        self.h += n.max(0);
        n.max(0)
    }
}

// py: clan.py:256 resolve_day
/// One match = one day (ProcessClanDay). Returns (new clan dict, result dict). Pure except for the rng.
pub fn resolve_day(clan: &Value, parts: i64, left_early: bool, downs: i64, counts: Option<&Counts>, mult: f64, forgive: bool, bottles: i64, protect: bool) -> (Value, Value) {
    let mut c = clan.clone();
    let mut counts: Counts = counts.cloned().unwrap_or_default();
    if downs != 0 && !counts.contains_key(&1) {
        counts.insert(1, downs);
    }
    let mut meta = meta_map(&c);
    let sh = meta.get("sh").map(int_of).unwrap_or(0);
    let mut cr = Crowd::new(gi(&c, "healthy", 0), gi(&c, "hungry", 0), gi(&c, "sick", 0), sh);
    let n = cr.size();
    let sick0 = cr.sick_shown();
    let need = needed(n);
    let parts = parts.max(0);
    let idle = !left_early && parts == 0 && bottles <= 0;
    let sup = if left_early { 0 } else { supplies_of(parts, bottles) };
    let mult_all = mult * (if idle { IDLE_MULT } else { 1.0 });
    let eff = f32(mult * sup as f64);
    let q = (eff - need as f64).trunc() as i64;
    let mut deficit = (-q).max(0);
    let surplus = q.max(0);
    if (forgive || protect) && deficit > 0 {
        deficit = 0;
    }
    let mut counted = mult_all <= DAY_MULT_LIMIT;
    let ev = event_window(gi(&c, "day", 0));
    if protect && counted && ev >= 0 && EVENT_DAYS.get(&ev).map(|e| !e.growth).unwrap_or(false) {
        counted = false; // an attack is only settled by matches that were fair
    }
    let mut r = Map::new();
    r.insert("day".into(), Value::from(gi(&c, "day", 0)));
    r.insert("parts".into(), Value::from(parts));
    r.insert("supplies".into(), Value::from(eff.trunc() as i64));
    r.insert("needed".into(), Value::from(need));
    r.insert("left".into(), Value::from(left_early as i64));
    for k in ["died", "new_hungry", "new_sick", "healed", "gained"] {
        r.insert(k.into(), Value::from(0));
    }
    r.insert("counted".into(), Value::from(counted as i64));
    if protect {
        r.insert("prot".into(), Value::from(1));
    }
    if n <= 0 {
        return (c, Value::Object(r));
    }
    let bump = |r: &mut Map<String, Value>, k: &str| {
        let v = r.get(k).map(int_of).unwrap_or(0) + 1;
        r.insert(k.into(), Value::from(v));
    };
    if deficit > 0 {
        // healthy -> hungry: int(0.75 d), no roll
        for _ in 0..(f32(f32(HUNGRY_PER_DEFICIT) * deficit as f64).trunc() as i64).max(0) {
            if cr.make_hungry() {
                bump(&mut r, "new_hungry");
            }
        }
        // hungry -> sick: |0.3 d| rolled; any alive member can be picked, a sick one is wasted and costs a death later
        let spiral = n > 4 && f32(sick0 as f64 / n as f64) >= f32(SPIRAL_SICK_SHARE);
        let mut wasted = 0i64;
        let nsick = if spiral { 0 } else { roll(f32(f32(SICK_PER_DEFICIT) * deficit as f64)) };
        for _ in 0..nsick {
            let k = cr.make_sick();
            if k > 0 {
                bump(&mut r, "new_sick");
            } else if k < 0 {
                wasted += 1;
            }
        }
        // deaths: ceiling(0.2 * sick at the day's start + wasted), more than 12 -> 10
        let mut d = f32(f32(f32(DEATH_PER_SICK) * sick0 as f64) + wasted as f64).ceil() as i64;
        if d > DEATH_CLAMP_OVER {
            d = DEATH_CLAMP_TO;
        }
        for _ in 0..d.max(0) {
            if cr.kill() {
                bump(&mut r, "died");
            }
        }
    } else if surplus > 0 {
        for _ in 0..roll(f32(f32(HEAL_HUNGRY_PER_SURPLUS) * surplus as f64)) {
            if cr.heal_hungry() {
                bump(&mut r, "healed");
            }
        }
        let hs = f32(f32(HEAL_SICK_PER_SURPLUS) * surplus as f64);
        for _ in 0..roll(if hs > 1.0 { hs } else { 1.0 }) {
            if cr.heal_sick() {
                bump(&mut r, "healed");
            }
        }
        let g = cr.add(roll(f32(f32(GROW_PER_SURPLUS) * surplus as f64)));
        r.insert("gained".into(), Value::from(g));
    }
    for k in ["evk", "evn", "evl", "evg", "evt"] {
        r.insert(k.into(), Value::from(0));
    }
    let old_day = gi(&c, "day", 0);
    let mut day = old_day;
    if counted {
        day += 1;
        event_step(old_day, &mut cr, &mut meta, &mut r, &counts);
    } else {
        progress_only(&mut meta, &mut r, &counts);
    }
    {
        let co = c.as_object_mut().expect("clan object");
        co.insert("healthy".into(), Value::from(cr.h));
        co.insert("hungry".into(), Value::from(cr.u));
        co.insert("sick".into(), Value::from(cr.s + cr.su));
        co.insert("day".into(), Value::from(day));
    }
    if cr.su != 0 {
        meta.insert("sh".into(), Value::from(cr.su));
    } else {
        meta.shift_remove("sh");
    }
    let co = c.as_object_mut().expect("clan object");
    co.insert("meta".into(), Value::String(py::dumps_compact(&Value::Object(meta))));
    if cr.size() <= 0 {
        co.insert("status".into(), Value::from(DEAD));
    } else if day >= WIN_DAY {
        co.insert("status".into(), Value::from(WON));
    }
    let st = co.get("status").cloned().unwrap_or(Value::Null);
    r.insert("status".into(), st);
    (c, Value::Object(r))
}

fn r_set(r: &mut Map<String, Value>, items: Vec<(&str, Value)>) {
    for (k, v) in items {
        r.insert(k.to_string(), v);
    }
}

fn mission_obj(meta: &Map<String, Value>) -> Option<Map<String, Value>> {
    meta.get("m").filter(|v| truthy(v)).and_then(|v| v.as_object()).cloned()
}

// py: clan.py:330 _progress_only
/// A match that does not count as a day still counts toward the running mission.
fn progress_only(meta: &mut Map<String, Value>, r: &mut Map<String, Value>, counts: &Counts) {
    if let Some(mut m) = mission_obj(meta) {
        let id = gm(&m, "id", 0);
        let prog = gm(&m, "prog", 0) + counts.get(&id).copied().unwrap_or(0).max(0);
        m.insert("prog".into(), Value::from(prog));
        r_set(
            r,
            vec![
                ("mi", Value::from(id)),
                ("mp", Value::from(prog)),
                ("ml", Value::from(gm(&m, "left", 0))),
                ("mt", Value::from(0)),
                ("ms", Value::from(0)),
                ("mb", Value::from(0)),
                ("mboosters", json!([])),
                ("mz", Value::from(gm(&m, "sz", 0))),
            ],
        );
        meta.insert("m".into(), Value::Object(m));
    }
}

fn goals_reached(id: i64, rep: i64, prog: i64) -> i64 {
    mission_goals(id, rep).iter().filter(|&&g| prog >= g).count() as i64
}

fn rep_of(meta: &Map<String, Value>, id: i64) -> i64 {
    meta.get("rep").and_then(|r| r.get(id.to_string())).map(int_of).unwrap_or(0)
}

// py: clan.py:338 _event_step
/// ProcessEvent: the running mission gets this match's progress; the event whose third match this was is settled by the mission's tier.
fn event_step(day: i64, cr: &mut Crowd, meta: &mut Map<String, Value>, r: &mut Map<String, Value>, counts: &Counts) {
    let mut m = mission_obj(meta);
    let s = event_window(day);
    if m.is_none() && s >= 0 {
        // An event is running but no mission was chosen: the event's own training is set for this match.
        let mm = json!({"id": auto_event_mission(&Value::Object(meta.clone())), "left": s + EVENT_LEN - day, "prog": 0, "ev": s, "sz": cr.size(), "auto": 1});
        meta.insert("m".into(), mm.clone());
        meta.shift_remove("pool");
        m = mm.as_object().cloned();
    }
    if let Some(mm) = m.as_mut() {
        let id = gm(mm, "id", 0);
        let prog = gm(mm, "prog", 0) + counts.get(&id).copied().unwrap_or(0).max(0);
        mm.insert("prog".into(), Value::from(prog));
        let left = (gm(mm, "left", EVENT_LEN) - 1).max(0);
        mm.insert("left".into(), Value::from(left));
        r_set(
            r,
            vec![
                ("mi", Value::from(id)),
                ("mp", Value::from(prog)),
                ("mt", Value::from(0)),
                ("ms", Value::from(0)),
                ("mb", Value::from(0)),
                ("mboosters", json!([])),
                ("mz", Value::from(gm(mm, "sz", 0))),
            ],
        );
        meta.insert("m".into(), Value::Object(mm.clone()));
    }
    if s >= 0 && day == s + EVENT_LEN - 1 {
        // the event's last match: settle it
        let ev = &EVENT_DAYS[&s];
        let mut tier = 0i64;
        let mut rep = 0i64;
        if let Some(mm) = &m {
            let id = gm(mm, "id", 0);
            rep = rep_of(meta, id);
            tier = goals_reached(id, rep, gm(mm, "prog", 0));
        }
        let size = cr.size();
        let pct = ev.pcts.get(tier as usize).copied().unwrap_or(0.0).abs();
        let n = f32(size as f64 * f32(pct)).ceil() as i64;
        r_set(r, vec![("evk", Value::from(if ev.growth { EV_GROWTH } else { EV_ATTACK })), ("evn", Value::from(event_today(s).1)), ("evt", Value::from(tier))]);
        if ev.growth {
            r.insert("evg".into(), Value::from(cr.add(n)));
        } else {
            let mut lost = 0i64;
            for _ in 0..n.max(0) {
                if cr.kill() {
                    lost += 1;
                }
            }
            r.insert("evl".into(), Value::from(lost));
            let died = r.get("died").map(int_of).unwrap_or(0) + lost;
            r.insert("died".into(), Value::from(died));
        }
        if let Some(mm) = &m {
            let id = gm(mm, "id", 0);
            let rows = &*BOOSTER_ROWS;
            let nb = if tier != 0 { rows[(rep + tier - 1).max(0).min(rows.len() as i64 - 1) as usize] } else { 0 };
            let evg = r.get("evg").cloned().unwrap_or(Value::from(0));
            let lb = &*LIVE_BOOSTERS;
            let boosters: Vec<Value> = (0..nb).map(|_| Value::from(lb[rng().choice_index(lb.len()).unwrap_or(0)])).collect();
            r_set(
                r,
                vec![("mt", Value::from(tier)), ("ms", evg), ("mb", Value::from(nb)), ("mboosters", Value::Array(boosters)), ("mdone", Value::from(1)), ("ml", Value::from(0))],
            );
            if tier != 0 {
                let rp = meta.entry("rep").or_insert_with(|| Value::Object(Map::new()));
                if let Some(ro) = rp.as_object_mut() {
                    let cur = ro.get(&id.to_string()).map(int_of).unwrap_or(0);
                    ro.insert(id.to_string(), Value::from((cur + 1).min(9)));
                }
            }
        }
        meta.shift_remove("m");
        return;
    }
    if let Some(mm) = &m {
        let id = gm(mm, "id", 0);
        if gm(mm, "ev", -1) < 0 && gm(mm, "left", 0) <= 0 {
            // a practice mission ends: nothing is paid
            let mt = goals_reached(id, rep_of(meta, id), gm(mm, "prog", 0));
            r_set(r, vec![("mt", Value::from(mt)), ("mdone", Value::from(1)), ("ml", Value::from(0))]);
            meta.shift_remove("m");
        } else {
            r.insert("ml".into(), Value::from(gm(mm, "left", 0)));
        }
    }
    // an event that starts on the next day finds no practice mission in its way (the picker comes back, as in the 2013 flow)
    if let Some(mm) = mission_obj(meta) {
        if gm(&mm, "ev", -1) < 0 && EVENT_DAYS.contains_key(&(day + 1)) {
            meta.shift_remove("m");
        }
    }
}

// py: clan.py:450 mission_goals
/// The three tier goals of mission `mid` after `repeats` completed runs (the goals do not scale with the clan).
pub fn mission_goals(mid: i64, repeats: i64) -> [i64; 3] {
    let Some(m) = mission(mid) else { return [0; 3] };
    let g = &m.goals;
    let r = repeats.max(0);
    let mut out = [0i64; 3];
    for (t, o) in out.iter_mut().enumerate() {
        *o = g[((r + t as i64) as usize).min(g.len() - 1)];
    }
    out
}

// py: clan.py:460 auto_event_mission
/// The mission set for an event when the player chose none: Down Enemies, or the first other one that is not greyed out and not a weapon-execution one.
pub fn auto_event_mission(meta: &Value) -> i64 {
    let rep = |i: i64| meta.get("rep").and_then(|r| r.get(i.to_string())).map(int_of).unwrap_or(0);
    if rep(AUTO_EVENT_MISSION) < MISSION_REPEAT_MAX {
        return AUTO_EVENT_MISSION;
    }
    for m in MISSIONS.iter() {
        if m.live && !EXEC_POOL.contains(&m.id) && rep(m.id) < MISSION_REPEAT_MAX {
            return m.id;
        }
    }
    AUTO_EVENT_MISSION
}

// py: clan.py:472 attack_goals
/// (display compat) the goals shown on an attack day before a mission is picked: Down Enemies, first run.
pub fn attack_goals() -> [i64; 3] {
    mission_goals(1, 0)
}

// py: clan.py:477 attack_tier
pub fn attack_tier(downs: i64) -> i64 {
    attack_goals().iter().filter(|&&g| downs >= g).count() as i64
}

// py: clan.py:481 tier_reward
/// (survivors, boosters) a finished event tier (1..3) pays a growth event of a clan of `size`.
pub fn tier_reward(tier: i64, size: i64) -> (i64, i64) {
    if !(1..=3).contains(&tier) {
        return (0, 0);
    }
    (f32(size as f64 * f32(GROWTH[tier as usize])).ceil() as i64, BOOSTER_ROWS[(tier - 1) as usize])
}

// ------------------------------------------------------------------ storage
// py: clan.py:502 ensure
/// Create / migrate the clans table on an open sqlite connection (players table untouched).
pub fn ensure(c: &Connection) -> rusqlite::Result<()> {
    c.execute(&SCHEMA, [])?;
    c.execute(&BOOSTER_SCHEMA, [])?;
    let have: std::collections::HashSet<String> = {
        let mut st = c.prepare("PRAGMA table_info(clans)")?;
        let rows = st.query_map([], |r| r.get::<_, String>(1))?;
        rows.collect::<Result<_, _>>()?
    };
    let backfill = !have.contains("days_total");
    for (col, decl) in [
        ("last_mid", "TEXT DEFAULT ''"),
        ("last_result", "TEXT DEFAULT ''"),
        ("weeks_total", "INT DEFAULT 0"),
        ("runs_won", "INT DEFAULT 0"),
        ("runs_lost", "INT DEFAULT 0"),
        ("boost_hi", "INT DEFAULT 1"),
        ("meta", "TEXT DEFAULT ''"),
    ] {
        if !have.contains(col) {
            c.execute(&format!("ALTER TABLE clans ADD COLUMN {} {}", col, decl), [])?;
        }
    }
    if backfill {
        // saves from before the 2013 rank: lifetime days = whole weeks booked + 1 week per won campaign + the days of the living clan's current week
        c.execute("ALTER TABLE clans ADD COLUMN days_total INT DEFAULT 0", [])?;
        c.execute(
            "UPDATE clans SET days_total = MIN(?, (COALESCE(weeks_total,0) + COALESCE(runs_won,0)) * 7 + CASE WHEN status=? THEN day % 7 ELSE 0 END)",
            rusqlite::params![RANK_MAX_DAYS, ALIVE],
        )?;
    }
    Ok(())
}

fn blank_clan() -> Value {
    json!({"faction": NONE, "status": 0, "day": 0, "healthy": 0, "hungry": 0, "sick": 0,
           "last_mid": "", "last_result": "", "weeks_total": 0, "runs_won": 0, "runs_lost": 0, "days_total": 0, "boost_hi": 1, "meta": ""})
}

// py: clan.py:518 load
pub fn load(c: &Connection, tid: &str) -> rusqlite::Result<Value> {
    ensure(c)?;
    let sql = format!("SELECT {} FROM clans WHERE token_id=?", COLS.join(","));
    let row = c
        .query_row(&sql, [tid], |r| {
            let mut m = Map::new();
            for (i, k) in COLS.iter().enumerate() {
                m.insert((*k).to_string(), vref_to_json(r.get_ref(i)?));
            }
            Ok(m)
        })
        .optional()?;
    Ok(match row {
        Some(m) => Value::Object(m),
        None => blank_clan(),
    })
}

// py: clan.py:527 save
pub fn save(c: &Connection, tid: &str, clan: &Value) -> rusqlite::Result<()> {
    ensure(c)?;
    let now = clock::now();
    let mut vals: Vec<rusqlite::types::Value> = vec![rusqlite::types::Value::Text(tid.to_string())];
    for k in COLS {
        vals.push(match clan.get(k) {
            Some(v) => to_sql(v),
            None if TEXT_COLS.contains(&k) => rusqlite::types::Value::Text(String::new()),
            None => rusqlite::types::Value::Integer(0),
        });
    }
    vals.push(rusqlite::types::Value::Real(now));
    vals.push(rusqlite::types::Value::Real(now));
    let sql = format!(
        "INSERT INTO clans(token_id,{},created,updated) VALUES(?{},?,?) ON CONFLICT(token_id) DO UPDATE SET {},updated=excluded.updated",
        COLS.join(","),
        ",?".repeat(COLS.len()),
        COLS.iter().map(|k| format!("{}=excluded.{}", k, k)).collect::<Vec<_>>().join(",")
    );
    c.execute(&sql, rusqlite::params_from_iter(vals))?;
    Ok(())
}

// ------------------------------------------------------------------ 2013 rank
// py: clan.py:542 rank_days
/// Lifetime days survived across every clan: + the days this match added; a clan that wins books the 84th too.
pub fn rank_days(total: i64, day_before: i64, day_after: i64, status_after: i64) -> i64 {
    let mut add = (day_after - day_before).max(0);
    if status_after == WON && day_after >= WIN_DAY {
        add += DAYS - WIN_DAY;
    }
    RANK_MAX_DAYS.min(total.max(0) + add)
}

// py: clan.py:551 rank_of
/// (symbol index 0..9, weeks 0..999) of a clan row.
pub fn rank_of(clan: &Value) -> (i64, i64) {
    let won = clan.get("runs_won").map(int_of).unwrap_or(0);
    let dt = clan.get("days_total").map(int_of).unwrap_or(0);
    (won.max(0).min(RANK_SYMBOLS - 1), RANK_MAX_WEEKS.min(py::floordiv(dt.max(0), 7)))
}

// py: clan.py:556 choose_side
/// CHOOSE YOUR SIDE: only when there is no living clan (first launch, clan dead or won). Returns (clan, changed).
pub fn choose_side(c: &Connection, tid: &str, faction: i64) -> rusqlite::Result<(Value, bool)> {
    let clan = load(c, tid)?;
    if faction != WLF && faction != SERAPHITES {
        return Ok((clan, false));
    }
    if gi(&clan, "status", 0) == ALIVE {
        return Ok((clan, false)); // the choice lasts until the clan dies or wins
    }
    let mut n = new_clan(faction);
    {
        let o = n.as_object_mut().expect("clan object");
        o.insert("last_mid".into(), Value::String(String::new()));
        o.insert("last_result".into(), Value::String(String::new()));
        for k in ["weeks_total", "runs_won", "runs_lost", "days_total"] {
            o.insert(k.into(), clan.get(k).cloned().unwrap_or(Value::Null));
        }
    }
    save(c, tid, &n)?;
    Ok((n, true))
}

// py: clan.py:580 threshold_booster
/// The booster the clan earns when it first reaches 5 * k survivors.
pub fn threshold_booster(k: i64) -> i64 {
    let bid = py::pymod(k - 1, 7) + 1;
    if LIVE_BOOSTERS.contains(&bid) {
        bid
    } else {
        let lb = &*LIVE_BOOSTERS;
        lb[rng().choice_index(lb.len()).unwrap_or(0)]
    }
}

// py: clan.py:588 apply_match
/// End of a match for one player. Once per match id. counts = {award label id: n} of the match. Returns (clan, result or None).
pub fn apply_match(c: &Connection, tid: &str, mid: &str, parts: i64, left_early: bool, counts: Option<&Counts>, mult: f64, forgive: bool, bottles: i64, protect: bool) -> rusqlite::Result<(Value, Option<Value>)> {
    let clan = load(c, tid)?;
    if gi(&clan, "status", 0) != ALIVE || mid.is_empty() || clan.get("last_mid").and_then(|v| v.as_str()) == Some(mid) {
        return Ok((clan, None));
    }
    let downs = counts.and_then(|m| m.get(&1)).copied().unwrap_or(0);
    let (mut new, mut r) = resolve_day(&clan, parts, left_early, downs, counts, mult, forgive, bottles, protect);
    let day0 = gi(&clan, "day", 0);
    let size0 = gi(&clan, "healthy", 0) + gi(&clan, "hungry", 0) + gi(&clan, "sick", 0);
    {
        let ro = r.as_object_mut().expect("result object");
        ro.insert("mid".into(), Value::String(mid.to_string()));
    }
    {
        let no = new.as_object_mut().expect("clan object");
        if py::floordiv(gm(no, "day", 0), 7) > py::floordiv(day0, 7) {
            no.insert("weeks_total".into(), Value::from(gi(&clan, "weeks_total", 0) + 1)); // weeks completed in total
        }
        let st = gm(no, "status", 0);
        no.insert("days_total".into(), Value::from(rank_days(gi(&clan, "days_total", 0), day0, gm(no, "day", 0), st)));
        if st == WON {
            no.insert("runs_won".into(), Value::from(gi(&clan, "runs_won", 0) + 1));
        } else if st == DEAD {
            no.insert("runs_lost".into(), Value::from(gi(&clan, "runs_lost", 0) + 1));
        }
        no.insert("last_mid".into(), Value::String(mid.to_string()));
    }
    let mut meta = match meta_of(&new) {
        Value::Object(m) => m,
        _ => Map::new(),
    };
    // supplies per category: the same engine numbers, split; the stock persists in meta["sup"]
    let book_res: Option<(Value, Value)> = {
        let sup = meta.get("sup").filter(|v| truthy(v));
        match sup {
            Some(v) if !v.is_object() => None,
            _ => {
                let stock = sup.and_then(|v| v.get("stock"));
                match stock {
                    Some(s) if truthy(s) && !s.is_object() => None,
                    _ => {
                        let empty = Counts::new();
                        let ro = r.as_object().expect("result object");
                        Some(clan_supply::book(
                            stock,
                            ro.get("supplies").map(int_of).unwrap_or(0),
                            ro.get("needed").map(int_of).unwrap_or(0),
                            size0,
                            ro.get("parts").map(int_of).unwrap_or(0),
                            counts.unwrap_or(&empty),
                            ro.get("left").map(truthy).unwrap_or(false),
                            forgive || protect,
                        ))
                    }
                }
            }
        }
    };
    match book_res {
        Some((stock, rec)) => {
            r.as_object_mut().expect("result object").insert("sup".into(), rec);
            meta.insert("sup".into(), json!({ "stock": stock }));
        }
        None => {
            r.as_object_mut().expect("result object").shift_remove("sup");
        }
    }
    let new_status = new.get("status").map(int_of).unwrap_or(0);
    if new_status != ALIVE {
        meta.shift_remove("m"); // clan dead / won: the mission is void
    }
    let size = gi(&new, "healthy", 0) + gi(&new, "hungry", 0) + gi(&new, "sick", 0);
    let mx = gm(&meta, "max", 0).max(size0).max(size);
    meta.insert("max".into(), Value::from(mx));
    new.as_object_mut().expect("clan object").insert("meta".into(), Value::String(py::dumps_compact(&Value::Object(meta))));
    // a booster for every BOOSTER_EVERY survivors the clan has grown to (never again for a size it already had)
    let mut got: Vec<Value> = r.get("mboosters").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let hi = gi(&clan, "boost_hi", 0);
    for k in (hi + 1)..(py::floordiv(size, BOOSTER_EVERY) + 1) {
        got.push(Value::from(threshold_booster(k)));
    }
    new.as_object_mut().expect("clan object").insert("boost_hi".into(), Value::from(hi.max(py::floordiv(size, BOOSTER_EVERY))));
    r.as_object_mut().expect("result object").insert("boosters".into(), Value::Array(got.clone()));
    new.as_object_mut().expect("clan object").insert("last_result".into(), Value::String(py::dumps_compact(&r)));
    save(c, tid, &new)?;
    for bid in got {
        booster_grant(c, tid, int_of(&bid), 1)?;
    }
    Ok((new, Some(r)))
}

// ------------------------------------------------------------------ re-booking one match
fn has_table(c: &Connection, t: &str) -> rusqlite::Result<bool> {
    Ok(c.query_row("SELECT 1 FROM sqlite_master WHERE type='table' AND name=?", [t], |_| Ok(())).optional()?.is_some())
}

// py: clan.py:642 snap_get
/// The snapshot taken before `mid` was first booked for `tid` (dict) or None.
pub fn snap_get(c: &Connection, tid: &str, mid: &str) -> rusqlite::Result<Option<Value>> {
    c.execute(&SNAP_SCHEMA, [])?;
    let row: Option<(Value, Value)> = c
        .query_row("SELECT mid, data FROM clan_snap WHERE token_id=?", [tid], |r| Ok((vref_to_json(r.get_ref(0)?), vref_to_json(r.get_ref(1)?))))
        .optional()?;
    let Some((m, d)) = row else { return Ok(None) };
    if m.as_str() != Some(mid) {
        return Ok(None);
    }
    Ok(d.as_str().and_then(|s| serde_json::from_str::<Value>(s).ok()))
}

// py: clan.py:654 snap_put
pub fn snap_put(c: &Connection, tid: &str, mid: &str, data: &Value) -> rusqlite::Result<()> {
    c.execute(&SNAP_SCHEMA, [])?;
    c.execute(
        "INSERT OR REPLACE INTO clan_snap(token_id, mid, data, updated) VALUES(?,?,?,?)",
        rusqlite::params![tid, mid, py::dumps_compact(data), clock::now()],
    )?;
    Ok(())
}

// py: clan.py:659 snap_take
/// Before `mid` is booked for `tid`: keep the rows the booking will change (no-op when this mid already has its snapshot).
pub fn snap_take(c: &Connection, tid: &str, mid: &str, token: &str) -> rusqlite::Result<()> {
    if snap_get(c, tid, mid)?.is_some() {
        return Ok(());
    }
    ensure(c)?;
    let mut tabs = Map::new();
    for t in SNAP_TABLES {
        if has_table(c, t)? {
            let mut st = c.prepare(&format!("SELECT * FROM {} WHERE token_id=?", t))?;
            let cols: Vec<Value> = st.column_names().iter().map(|s| Value::String((*s).to_string())).collect();
            let n = cols.len();
            let rows: Vec<Value> = st
                .query_map([tid], |r| {
                    let mut row = Vec::with_capacity(n);
                    for i in 0..n {
                        row.push(vref_to_json(r.get_ref(i)?));
                    }
                    Ok(Value::Array(row))
                })?
                .collect::<Result<_, _>>()?;
            tabs.insert(t.to_string(), json!({"cols": cols, "rows": rows}));
        }
    }
    let pl = if has_table(c, "players")? {
        c.query_row("SELECT parts_total, parts_n FROM players WHERE token=?", [token], |r| Ok(Value::Array(vec![vref_to_json(r.get_ref(0)?), vref_to_json(r.get_ref(1)?)])))
            .optional()?
    } else {
        None
    };
    snap_put(c, tid, mid, &json!({"tabs": tabs, "players": pl.unwrap_or(Value::Null), "stint": Value::Null, "rejoined": false}))
}

// py: clan.py:673 snap_update
pub fn snap_update(c: &Connection, tid: &str, mid: &str, kw: &[(&str, Value)]) -> rusqlite::Result<()> {
    if let Some(mut s) = snap_get(c, tid, mid)? {
        if let Some(o) = s.as_object_mut() {
            for (k, v) in kw {
                o.insert((*k).to_string(), v.clone());
            }
        }
        snap_put(c, tid, mid, &s)?;
    }
    Ok(())
}

// py: clan.py:680 snap_restore
/// Put the rows back to what they were before the first booking of the match.
pub fn snap_restore(c: &Connection, tid: &str, token: &str, snap: &Value) -> rusqlite::Result<()> {
    if let Some(tabs) = snap.get("tabs").and_then(|v| v.as_object()) {
        for (t, v) in tabs {
            if !has_table(c, t)? {
                continue;
            }
            c.execute(&format!("DELETE FROM {} WHERE token_id=?", t), [tid])?;
            let cols: Vec<&str> = v.get("cols").and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|x| x.as_str()).collect()).unwrap_or_default();
            for row in v.get("rows").and_then(|x| x.as_array()).map(|a| a.as_slice()).unwrap_or(&[]) {
                let vals: Vec<rusqlite::types::Value> = row.as_array().map(|a| a.iter().map(to_sql).collect()).unwrap_or_default();
                c.execute(
                    &format!("INSERT INTO {}({}) VALUES({})", t, cols.join(","), vec!["?"; vals.len()].join(",")),
                    rusqlite::params_from_iter(vals),
                )?;
            }
        }
    }
    if let Some(p) = snap.get("players").filter(|v| truthy(v)) {
        if has_table(c, "players")? {
            let a = p.as_array().cloned().unwrap_or_default();
            c.execute(
                "UPDATE players SET parts_total=?, parts_n=? WHERE token=?",
                rusqlite::params![to_sql(a.first().unwrap_or(&Value::Null)), to_sql(a.get(1).unwrap_or(&Value::Null)), token],
            )?;
        }
    }
    Ok(())
}

// ------------------------------------------------------------------ missions
/// `meta_of` as the object map.
pub fn meta_map(clan: &Value) -> Map<String, Value> {
    match meta_of(clan) {
        Value::Object(m) => m,
        _ => Map::new(),
    }
}

// py: clan.py:693 meta_of
/// The clan's `meta` JSON as an object (anything else = empty).
pub fn meta_of(clan: &Value) -> Value {
    let s = match clan.get("meta") {
        Some(Value::String(s)) if !s.is_empty() => s.as_str(),
        _ => "{}",
    };
    match serde_json::from_str::<Value>(s) {
        Ok(v) if v.is_object() => v,
        _ => Value::Object(Map::new()),
    }
}

// py: clan.py:701 mission_choose
/// CHOOSE MISSION: only for a living clan with no mission running. Returns (clan, ok).
pub fn mission_choose(c: &Connection, tid: &str, mid: i64) -> rusqlite::Result<(Value, bool)> {
    let mut clan = load(c, tid)?;
    let mut m = match meta_of(&clan) {
        Value::Object(m) => m,
        _ => Map::new(),
    };
    let live = mission(mid).map(|x| x.live);
    if gi(&clan, "status", 0) != ALIVE || live != Some(true) || m.get("m").map(truthy).unwrap_or(false) {
        return Ok((clan, false));
    }
    let pool: Vec<i64> = m.get("pool").and_then(|v| v.as_array()).map(|a| a.iter().map(int_of).collect()).unwrap_or_default();
    if OFFER_POOL.contains(&mid) && !pool.contains(&mid) {
        return Ok((clan, false)); // (a pool mission only from the pool rolled for this choice)
    }
    let rep = |i: i64| rep_of(&m, i);
    if rep(mid) >= MISSION_REPEAT_MAX && MISSIONS.iter().any(|x| x.live && rep(x.id) < MISSION_REPEAT_MAX) {
        return Ok((clan, false)); // (the 2013 list greys out a mission completed five times)
    }
    let day = gi(&clan, "day", 0);
    let s = event_window(day);
    let left = if s >= 0 { s + EVENT_LEN - day } else { MISSION_DAYS };
    let sz = gi(&clan, "healthy", 0) + gi(&clan, "hungry", 0) + gi(&clan, "sick", 0);
    m.insert("m".into(), json!({"id": mid, "left": left, "prog": 0, "ev": s, "sz": sz}));
    m.shift_remove("pool"); // the next choice rolls a new pool
    clan.as_object_mut().expect("clan object").insert("meta".into(), Value::String(py::dumps_compact(&Value::Object(m))));
    save(c, tid, &clan)?;
    Ok((clan, true))
}

// ------------------------------------------------------------------ boosters
// py: clan.py:723 booster_counts
/// {bid: owned} for every booster id (0 owned = 0).
pub fn booster_counts(c: &Connection, tid: &str) -> rusqlite::Result<BTreeMap<i64, i64>> {
    ensure(c)?;
    let mut out: BTreeMap<i64, i64> = BOOSTERS.iter().map(|(b, _)| (*b, 0)).collect();
    let mut st = c.prepare("SELECT bid, n FROM boosters WHERE token_id=?")?;
    let rows = st.query_map([tid], |r| Ok((vref_to_json(r.get_ref(0)?), vref_to_json(r.get_ref(1)?))))?;
    for row in rows {
        let (bid, n) = row?;
        if let Some(b) = bid.as_i64() {
            if let Some(slot) = out.get_mut(&b) {
                *slot = int_of(&n);
            }
        }
    }
    Ok(out)
}

// py: clan.py:733 booster_grant
pub fn booster_grant(c: &Connection, tid: &str, bid: i64, n: i64) -> rusqlite::Result<i64> {
    if !BOOSTERS.iter().any(|(b, _)| *b == bid) || n <= 0 {
        return Ok(0);
    }
    ensure(c)?;
    let cur = booster_counts(c, tid)?.get(&bid).copied().unwrap_or(0);
    let new = BOOSTER_MAX.min(cur + n);
    c.execute(
        "INSERT INTO boosters(token_id,bid,n) VALUES(?,?,?) ON CONFLICT(token_id,bid) DO UPDATE SET n=excluded.n",
        rusqlite::params![tid, bid, new],
    )?;
    Ok(new - cur)
}

// py: clan.py:744 booster_consume
/// Use one up. True when the player had it.
pub fn booster_consume(c: &Connection, tid: &str, bid: i64) -> rusqlite::Result<bool> {
    if !BOOSTERS.iter().any(|(b, _)| *b == bid) || booster_counts(c, tid)?.get(&bid).copied().unwrap_or(0) <= 0 {
        return Ok(false);
    }
    c.execute("UPDATE boosters SET n = n - 1 WHERE token_id=? AND bid=? AND n > 0", rusqlite::params![tid, bid])?;
    Ok(true)
}

// py: clan.py:752 booster_next
/// Survivors still missing for the clan's next booster ("+5 NEXT ONE-USE BOOSTER").
pub fn booster_next(clan: &Value) -> i64 {
    let size = gi(clan, "healthy", 0) + gi(clan, "hungry", 0) + gi(clan, "sick", 0);
    1.max((gi(clan, "boost_hi", 0).max(py::floordiv(size, BOOSTER_EVERY)) + 1) * BOOSTER_EVERY - size)
}

/// Python `random.sample(range(n), k)` on the global `random` module (`crate::rnd` stream).
fn global_sample_indices(key: &str, n: usize, k: usize) -> Vec<usize> {
    let mut setsize = 21usize;
    if k > 5 {
        let e = ((k * 3) as f64).ln() / 4f64.ln();
        setsize += 4usize.pow(e.ceil() as u32);
    }
    let mut out = Vec::with_capacity(k);
    if n <= setsize {
        let mut pool: Vec<usize> = (0..n).collect();
        for i in 0..k {
            let j = rnd::randbelow(key, (n - i) as u64) as usize;
            out.push(pool[j]);
            pool[j] = pool[n - i - 1];
        }
    } else {
        let mut selected = std::collections::HashSet::new();
        for _ in 0..k {
            let mut j = rnd::randbelow(key, n as u64) as usize;
            while selected.contains(&j) {
                j = rnd::randbelow(key, n as u64) as usize;
            }
            selected.insert(j);
            out.push(j);
        }
    }
    out
}

// py: clan.py:758 meta_text
/// Reply to the client ext (op 0xE9): booster counts bc1..bc7, bp = the pick for the next match, ba = the booster in use this match,
/// bn = survivors to the next booster, the mission fields.
pub fn meta_text(c: &Connection, tid: &str, pick: i64, active: i64) -> rusqlite::Result<String> {
    let mut clan = load(c, tid)?;
    let cnt = booster_counts(c, tid)?;
    let mut f: Vec<String> = vec!["v=1".to_string()];
    for (b, n) in &cnt {
        f.push(format!("bc{}={}", b, n));
    }
    f.push(format!("bp={}", pick));
    f.push(format!("ba={}", active));
    f.push(format!("bn={}", booster_next(&clan)));
    let mut meta = match meta_of(&clan) {
        Value::Object(m) => m,
        _ => Map::new(),
    };
    let m: Map<String, Value> = meta.get("m").filter(|v| truthy(v)).and_then(|v| v.as_object()).cloned().unwrap_or_default();
    let size = gi(&clan, "healthy", 0) + gi(&clan, "hungry", 0) + gi(&clan, "sick", 0);
    if m.is_empty() && gi(&clan, "status", 0) == ALIVE {
        let mut pool: Vec<i64> = meta.get("pool").filter(|v| truthy(v)).and_then(|v| v.as_array()).map(|a| a.iter().map(int_of).filter(|i| OFFER_POOL.contains(i)).collect()).unwrap_or_default();
        if pool.is_empty() {
            // roll the pool missions on offer for this choice (kept until one is chosen)
            let kp = EXEC_POOL_PICK[rnd::choice_index("clan:meta_text", EXEC_POOL_PICK.len())] as usize;
            let mut p: Vec<i64> = global_sample_indices("clan:meta_text", OFFER_POOL.len(), kp).into_iter().map(|i| OFFER_POOL[i]).collect();
            p.sort();
            pool = p;
            meta.insert("pool".into(), json!(pool));
            clan.as_object_mut().expect("clan object").insert("meta".into(), Value::String(py::dumps_compact(&Value::Object(meta.clone()))));
            save(c, tid, &clan)?;
            let names: Vec<String> = pool.iter().map(|i| mission(*i).map(|x| x.name.clone()).unwrap_or_default()).collect();
            print_line(&format!("mission: pool rolled for {} {}", py::cut(tid, 8), py::list_str(&names)));
        }
        for (k, i) in pool.iter().take(3).enumerate() {
            f.push(format!("p{}={}", k + 1, i));
        }
    }
    let mid = gm(&m, "id", 0);
    f.push(format!("mi={}", mid));
    f.push(format!("mp={}", gm(&m, "prog", 0)));
    f.push(format!("ml={}", gm(&m, "left", 0)));
    f.push(format!("mz={}", if !m.is_empty() { gm(&m, "sz", 0) } else { size }));
    let mut reps: Vec<(i64, i64)> = meta
        .get("rep")
        .and_then(|v| v.as_object())
        .map(|o| o.iter().map(|(k, v)| (k.trim().parse::<i64>().unwrap_or(0), int_of(v))).collect())
        .unwrap_or_default();
    reps.sort_by_key(|x| x.0);
    for (k, n) in reps {
        if n != 0 {
            f.push(format!("mr{}={}", k, n));
        }
    }
    if !m.is_empty() {
        let g = mission_goals(mid, rep_of(&meta, mid));
        f.push(format!("mg1={}", g[0]));
        f.push(format!("mg2={}", g[1]));
        f.push(format!("mg3={}", g[2]));
        f.push(format!("mev={}", if gm(&m, "ev", -1) >= 0 { 1 } else { 0 }));
    }
    let r = last_result(&clan);
    if r.get("mi").map(truthy).unwrap_or(false) {
        // what the last match did to the mission (post-match page)
        f.push(format!("lmi={}", gi(&r, "mi", 0)));
        f.push(format!("lmp={}", gi(&r, "mp", 0)));
        f.push(format!("lmt={}", gi(&r, "mt", 0)));
        f.push(format!("lms={}", gi(&r, "ms", 0)));
        f.push(format!("lmb={}", r.get("boosters").and_then(|v| v.as_array()).map(|a| a.len()).unwrap_or(0)));
        f.push(format!("lml={}", gi(&r, "ml", 0)));
        f.push(format!("lmd={}", gi(&r, "day", -1)));
        f.push(format!("lmz={}", gi(&r, "mz", 0)));
        f.push(format!("lmx={}", gi(&r, "mlost", 0)));
    }
    Ok(f.join(" "))
}

fn last_result(clan: &Value) -> Value {
    let s = match clan.get("last_result") {
        Some(Value::String(s)) if !s.is_empty() => s.as_str(),
        _ => "{}",
    };
    match serde_json::from_str::<Value>(s) {
        Ok(v) if v.is_object() => v,
        _ => Value::Object(Map::new()),
    }
}

// py: clan.py:794 match_tag
/// Small positive int that changes with every resolved match (0 = none yet).
pub fn match_tag(mid: Option<&str>) -> i64 {
    match mid {
        Some(m) if !m.is_empty() => ((t2r_clanstory::crc::crc32(m.as_bytes()) & 0x3fff_ffff) | 1) as i64,
        _ => 0,
    }
}

// py: clan.py:802 state_text
/// Reply to the client ext (op 0xE1): one line of key=value, ints only.
pub fn state_text(clan: &Value) -> String {
    let n = gi(clan, "healthy", 0) + gi(clan, "hungry", 0) + gi(clan, "sick", 0);
    let r = last_result(clan);
    let rv = |k: &str, d: i64| r.get(k).map(pystr).unwrap_or_else(|| d.to_string());
    let cv = |k: &str| clan.get(k).map(pystr).unwrap_or_default();
    let (rk, wk) = rank_of(clan);
    let days_total = clan.get("days_total").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
    let mut f: Vec<(&str, String)> = vec![
        ("v", "1".into()),
        ("fac", cv("faction")),
        ("st", cv("status")),
        ("day", cv("day")),
        ("h", cv("healthy")),
        ("hu", cv("hungry")),
        ("s", cv("sick")),
        ("need", needed(n).to_string()),
        ("wk", wk.to_string()),
        ("rk", rk.to_string()),
        ("dt", days_total.to_string()),
        ("lday", rv("day", -1)),
        ("lparts", rv("parts", 0)),
        ("lsup", rv("supplies", 0)),
        ("lneed", rv("needed", 0)),
        ("lgain", rv("gained", 0)),
        ("ldied", rv("died", 0)),
        ("lhungry", rv("new_hungry", 0)),
        ("lsick", rv("new_sick", 0)),
        ("lheal", rv("healed", 0)),
        ("lleft", rv("left", 0)),
        ("lmh", match_tag(clan.get("last_mid").and_then(|v| v.as_str())).to_string()),
    ];
    let (ek, en) = if gi(clan, "status", 0) == ALIVE { event_today(gi(clan, "day", 0)) } else { (0, 0) };
    let meta = match meta_of(clan) {
        Value::Object(m) => m,
        _ => Map::new(),
    };
    f.push(("ev", ek.to_string()));
    f.push(("evn", en.to_string()));
    f.push(("levk", rv("evk", 0)));
    f.push(("levn", rv("evn", 0)));
    f.push(("levl", rv("evl", 0)));
    f.push(("levg", rv("evg", 0)));
    f.push(("levt", rv("evt", 0)));
    f.push(("mx", gm(&meta, "max", 0).max(n).to_string()));
    f.push(("rw", clan.get("runs_won").map(pystr).unwrap_or_else(|| "0".into())));
    f.push(("rl", clan.get("runs_lost").map(pystr).unwrap_or_else(|| "0".into())));
    let g = match meta.get("m").filter(|v| truthy(v)) {
        Some(m) => mission_goals(m.get("id").map(int_of).unwrap_or(0), rep_of(&meta, m.get("id").map(int_of).unwrap_or(0))),
        None => attack_goals(),
    };
    f.push(("ag1", g[0].to_string()));
    f.push(("ag2", g[1].to_string()));
    f.push(("ag3", g[2].to_string()));
    f.iter().map(|(k, v)| format!("{}={}", k, v)).collect::<Vec<_>>().join(" ")
}
