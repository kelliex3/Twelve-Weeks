//! Host migration decisions (port of hostmig.py): the 0xE6 / 0xE7 poll, host loss, planned handover, region pool, ping gate,
//! machine-performance penalty and the periodic re-pick. Pure logic, no sockets.
//!
//! Deviations from the Python (none observable on the wire or in the logs):
//! - Python raises where the input is impossible (a token mapped to a match whose player list lacks it, a reply byte > 255);
//!   `poll` returns `None` for those (the daemon logs "HOSTMIG ERR" like it did for the exception), the other methods return quietly.
//! - callbacks (`ping`, `perf`, `pool`, `link_alive`) cannot raise: `pool` / `perf` / `link_alive` return `None` for "raised"
//!   (the Python swallows those exceptions the same way).
//! - `export_match` / `dump_state` emit object keys sorted (the Python dict order is insertion order and nobody reads it).
use indexmap::IndexMap;
use parking_lot::Mutex;
use serde_json::{Map, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;

use crate::{clock, dlog, py};

#[path = "hostmig/st.rs"]
mod st;
pub use st::selftest;

pub const OP_POLL: u8 = 0xE6;
pub const OP_REPLY: u8 = 0xE7;
pub const ST_NORMAL: u8 = 0;
pub const ST_SUSPECT: u8 = 1;
pub const ST_GIVE_UP: u8 = 2;
pub const ST_HANDOVER: u8 = 3;
pub const OP_PERF: u8 = 0xCA;
pub const OP_PERF_REPLY: u8 = 0xCB;

// ------------------------------------------------------------ configuration (module globals of hostmig.py, read once)
pub struct Cfg {
    pub perf_on: bool,
    pub perf_penalty_ms: i64,
    pub perf_n1000: i64,
    pub perf_n250: i64,
    pub perf_severe_x: i64,
    pub perf_fresh_s: i64,
    pub min_silent_s: f64,
    pub replaced_excuse_s: f64,
    pub pins: HashMap<i64, String>,
    pub region_guard_ms: i64,
    pub repick_mode: i64,
    pub repick_interval: i64,
    pub repick_min_age: i64,
    pub repick_min_left: i64,
    pub repick_min_gain_ms: i64,
    pub repick_min_gain_frac: f64,
    pub repick_confirm: i64,
    pub repick_confirm_gap_s: i64,
    pub repick_total_gain_ms: i64,
    pub repick_total_gain_frac: f64,
    pub repick_total_gain_per_ms: i64,
    pub repick_gap_s: i64,
    pub repick_max: i64,
    pub repick_cap_confirm: i64,
    pub repick_cap_max_worse_ms: i64,
    pub repick_cap_limit_ms: i64,
    pub repick_proven_n: i64,
    pub sat_x: f64,
    pub sat_min_ms: i64,
}

pub const REGION_FALLBACK_UNKNOWN_FRAC: f64 = 0.5;
pub const MM_REGIONS: [&str; 3] = ["EU", "NA-east", "NA-west"];

pub fn cfg() -> &'static Cfg {
    static C: LazyLock<Cfg> = LazyLock::new(|| Cfg {
        perf_on: crate::env_on("T2R_HOSTPERF", true),
        perf_penalty_ms: crate::env_parse("T2R_HOSTPERF_PENALTY_MS", 40i64),
        perf_n1000: crate::env_parse("T2R_HOSTPERF_N1000", 2i64),
        perf_n250: crate::env_parse("T2R_HOSTPERF_N250", 6i64),
        perf_severe_x: crate::env_parse("T2R_HOSTPERF_SEVERE_X", 2i64),
        perf_fresh_s: crate::env_parse("T2R_HOSTPERF_FRESH_S", 360i64),
        min_silent_s: crate::env_parse("T2R_HM_MIN_SILENT_S", 5.0f64),
        replaced_excuse_s: crate::env_parse("T2R_HM_REPLACED_EXCUSE_S", 10.0f64),
        pins: parse_pins(&crate::env_str("T2R_HOST_REGION_PIN", "")),
        region_guard_ms: crate::env_parse("T2R_REGION_GUARD_MS", 60i64),
        repick_mode: crate::env_parse("T2R_HOST_REPICK", 1i64),
        repick_interval: crate::env_parse("T2R_HOST_REPICK_INTERVAL_S", 90i64),
        repick_min_age: crate::env_parse("T2R_HOST_REPICK_MIN_AGE_S", 120i64),
        repick_min_left: crate::env_parse("T2R_HOST_REPICK_MIN_LEFT_S", 90i64),
        repick_min_gain_ms: crate::env_parse("T2R_HOST_REPICK_GAIN_MS", 40i64),
        repick_min_gain_frac: crate::env_parse("T2R_HOST_REPICK_GAIN_PCT", 20i64) as f64 / 100.0,
        repick_confirm: crate::env_parse("T2R_HOST_REPICK_CONFIRM", 2i64),
        repick_confirm_gap_s: crate::env_parse("T2R_HOST_REPICK_CONFIRM_GAP_S", 30i64),
        repick_total_gain_ms: crate::env_parse("T2R_HOST_REPICK_TOTAL_GAIN_MS", 100i64),
        repick_total_gain_frac: crate::env_parse("T2R_HOST_REPICK_TOTAL_GAIN_PCT", 20i64) as f64 / 100.0,
        repick_total_gain_per_ms: crate::env_parse("T2R_HOST_REPICK_TOTAL_GAIN_PER_MS", 25i64),
        repick_gap_s: crate::env_parse("T2R_HOST_REPICK_GAP_S", 180i64),
        repick_max: crate::env_parse("T2R_HOST_REPICK_MAX", 2i64),
        repick_cap_confirm: crate::env_parse("T2R_HOST_REPICK_CAP_CONFIRM", 2i64),
        repick_cap_max_worse_ms: crate::env_parse("T2R_HOST_REPICK_CAP_WORSE_MS", 25i64),
        repick_cap_limit_ms: crate::env_parse("T2R_PING_MATCH_MS", 175i64),
        repick_proven_n: crate::env_parse("T2R_HOST_REPICK_PROVEN_N", 6i64),
        sat_x: crate::env_parse("T2R_HOST_SAT_PCT", 300i64) as f64 / 100.0,
        sat_min_ms: crate::env_parse("T2R_HOST_SAT_MIN_MS", 150i64),
    });
    &C
}

// ------------------------------------------------------------ small helpers
/// py: hostmig.py:39 placeholder
pub fn placeholder(slot: usize) -> String {
    format!("{}{:04x}", "f0".repeat(30), slot)
}

/// C `%g` / Python `"%g" % x`.
fn fmt_g(x: f64) -> String {
    if x.is_nan() {
        return "nan".into();
    }
    if x.is_infinite() {
        return if x > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if x == 0.0 {
        return if x.is_sign_negative() { "-0".into() } else { "0".into() };
    }
    let e = format!("{:.5e}", x);
    let (mant, exp) = e.split_once('e').unwrap_or((&e, "0"));
    let exp: i32 = exp.parse().unwrap_or(0);
    let strip = |s: &str| -> String {
        if s.contains('.') {
            s.trim_end_matches('0').trim_end_matches('.').to_string()
        } else {
            s.to_string()
        }
    };
    if !(-4..6).contains(&exp) {
        let m = strip(mant);
        format!("{}e{}{:02}", m, if exp < 0 { '-' } else { '+' }, exp.abs())
    } else {
        let dec = (5 - exp).max(0) as usize;
        strip(&format!("{:.*}", dec, x))
    }
}

fn set_repr(s: &BTreeSet<usize>) -> String {
    let v: Vec<usize> = s.iter().copied().collect();
    py::list_int(&v)
}

fn i64_set_repr(v: &[i64]) -> String {
    py::list_int(v)
}

type NowFn = Box<dyn Fn() -> f64 + Send + Sync>;
type PingFn = Box<dyn Fn(&str, &str) -> Option<i64> + Send + Sync>;
type PerfFn = Box<dyn Fn(&str) -> Option<i64> + Send + Sync>;
type PoolFn = Box<dyn Fn(&[String]) -> Option<(Vec<String>, String)> + Send + Sync>;
type LinkFn = Box<dyn Fn(&str) -> Option<bool> + Send + Sync>;

// ------------------------------------------------------------ machine performance (host pick + migration)
#[derive(Clone, Debug, PartialEq)]
pub struct PerfRep {
    pub n250: i64,
    pub n1000: i64,
    pub worst: i64,
    pub avg_ms: f64,
    pub vram: i64,
    pub t: f64,
}

/// py: hostmig.py:57 parse_perf  ([64B token][u8 ver=1][u8 n250][u8 n1000][u16 worst ms][u16 avg frame ms*10][u8 flags])
pub fn parse_perf(pl: &[u8]) -> Option<(String, PerfRep)> {
    if pl.len() < 72 || pl[64] != 1 {
        return None;
    }
    let token: String = pl[..64].iter().map(|&b| b as char).collect();
    Some((
        token,
        PerfRep {
            n250: pl[65] as i64,
            n1000: pl[66] as i64,
            worst: crate::util::le(pl, 67, 2) as i64,
            avg_ms: crate::util::le(pl, 69, 2) as f64 / 10.0,
            vram: (pl[71] & 1) as i64,
            t: 0.0,
        },
    ))
}

/// py: hostmig.py:67 perf_penalty
pub fn perf_penalty(rep: Option<&PerfRep>) -> i64 {
    let c = cfg();
    let Some(rep) = rep else { return 0 };
    if !c.perf_on {
        return 0;
    }
    let (n1, n2) = (rep.n1000, rep.n250);
    if n1 >= c.perf_n1000 * c.perf_severe_x || n2 >= c.perf_n250 * c.perf_severe_x {
        return c.perf_penalty_ms * 2;
    }
    if n1 >= c.perf_n1000 || n2 >= c.perf_n250 {
        return c.perf_penalty_ms;
    }
    0
}

#[derive(Default)]
struct PerfInner {
    by_token: HashMap<String, PerfRep>,
    by_steam: HashMap<i64, PerfRep>,
}

/// py: hostmig.py:79 PerfScores
pub struct PerfScores {
    now: NowFn,
    inner: Mutex<PerfInner>,
}

impl PerfScores {
    pub fn new() -> Self {
        Self::with_now(Box::new(clock::now))
    }

    pub fn with_now(now: NowFn) -> Self {
        PerfScores { now, inner: Mutex::new(PerfInner::default()) }
    }

    pub fn report(&self, token: &str, rep: &PerfRep, steamid: i64) {
        let mut rep = rep.clone();
        rep.t = (self.now)();
        let mut g = self.inner.lock();
        g.by_token.insert(token.to_string(), rep.clone());
        if steamid != 0 {
            g.by_steam.insert(steamid, rep);
        }
    }

    /// (tests) forget every report
    pub fn clear(&self) {
        let mut g = self.inner.lock();
        g.by_token.clear();
        g.by_steam.clear();
    }

    pub fn prune(&self) -> usize {
        let now = (self.now)();
        let fresh = cfg().perf_fresh_s as f64;
        let mut g = self.inner.lock();
        let mut n = 0;
        let before = g.by_token.len();
        g.by_token.retain(|_, r| !(now - r.t > fresh));
        n += before - g.by_token.len();
        let before = g.by_steam.len();
        g.by_steam.retain(|_, r| !(now - r.t > fresh));
        n += before - g.by_steam.len();
        n
    }

    pub fn get(&self, token: &str, steamid: i64) -> Option<PerfRep> {
        let rep = {
            let g = self.inner.lock();
            match g.by_token.get(token) {
                Some(r) => Some(r.clone()),
                None => {
                    if steamid != 0 {
                        g.by_steam.get(&steamid).cloned()
                    } else {
                        None
                    }
                }
            }
        };
        match rep {
            Some(r) if (self.now)() - r.t <= cfg().perf_fresh_s as f64 => Some(r),
            _ => None,
        }
    }

    pub fn penalty(&self, token: &str, steamid: i64) -> i64 {
        perf_penalty(self.get(token, steamid).as_ref())
    }

    /// (token reports, steam reports): the sizes the daemon's stats dump counts.
    pub fn sizes(&self) -> (usize, usize) {
        let g = self.inner.lock();
        (g.by_token.len(), g.by_steam.len())
    }

    #[allow(dead_code)]
    pub fn dump(&self) -> Value {
        let g = self.inner.lock();
        let rv = |r: &PerfRep| {
            let mut m = Map::new();
            m.insert("avg_ms".into(), Value::from(r.avg_ms));
            m.insert("n1000".into(), Value::from(r.n1000));
            m.insert("n250".into(), Value::from(r.n250));
            m.insert("t".into(), Value::from(r.t));
            m.insert("vram".into(), Value::from(r.vram));
            m.insert("worst".into(), Value::from(r.worst));
            Value::Object(m)
        };
        let mut tok: Vec<(&String, &PerfRep)> = g.by_token.iter().collect();
        tok.sort_by(|a, b| a.0.cmp(b.0));
        let mut st: Vec<(String, &PerfRep)> = g.by_steam.iter().map(|(k, v)| (k.to_string(), v)).collect();
        st.sort_by(|a, b| a.0.cmp(&b.0));
        let mut m = Map::new();
        let mut a = Map::new();
        for (k, v) in tok {
            a.insert(k.clone(), rv(v));
        }
        let mut b = Map::new();
        for (k, v) in st {
            b.insert(k, rv(v));
        }
        m.insert("by_steam".into(), Value::Object(b));
        m.insert("by_token".into(), Value::Object(a));
        Value::Object(m)
    }
}

/// py: hostmig.py:118 host_key  (worst ping + performance penalty, total ping); `pings` must not be empty (Python max() raises).
pub fn host_key(pings: &[i64], penalty: i64) -> (i64, i64) {
    (pings.iter().copied().max().unwrap_or(0) + penalty, pings.iter().sum())
}

// ------------------------------------------------------------ region
const POPS: [(&str, &str); 4] = [
    ("EU", "ams fra lhr par sto sto2 vie waw mad hel lux mil ber prg arn cdg man dub osl cph sof buc lis"),
    ("NA-east", "iad atl ord mia ewr dfw yto tor bos cmh den hou dtw"),
    ("NA-west", "sea lax sjc pdx phx slc van yvr sfo"),
    ("other", "sgp hkg tyo seo bom maa del dxb jnb gru scl lim eze syd mel bog mex bkk kul tpe osa fuk cpt lax2"),
];

fn region_of_pop(pop: &str) -> Option<&'static str> {
    let mut r = None;
    for (reg, pops) in POPS {
        if pops.split_whitespace().any(|p| p == pop) {
            r = Some(reg);
        }
    }
    r
}

/// py: hostmig.py:140 region_of  ('EU' / 'NA-east' / 'NA-west' / 'other', None when no known data centre)
pub fn region_of(loc: Option<&str>) -> Option<&'static str> {
    let loc = loc?;
    if loc.is_empty() {
        return None;
    }
    let low: Vec<char> = loc.to_lowercase().chars().collect();
    let n = low.len();
    let mut best: Option<(i128, &'static str)> = None;
    let mut i = 0;
    while i < n {
        let c = low[i];
        if c.is_ascii_lowercase() {
            let mut j = i + 1;
            while j < n && (low[j].is_ascii_lowercase() || low[j].is_ascii_digit()) {
                j += 1;
            }
            let run = j - (i + 1);
            if (2..=4).contains(&run) && j < n && low[j] == '=' {
                let mut k = j + 1;
                while k < n && low[k].is_ascii_digit() {
                    k += 1;
                }
                if k > j + 1 {
                    let pop: String = low[i..j].iter().collect();
                    let digits: String = low[j + 1..k].iter().collect();
                    let ms: i128 = digits.parse().unwrap_or(i128::MAX);
                    if let Some(r) = region_of_pop(&pop) {
                        if best.is_none_or(|b| ms < b.0) {
                            best = Some((ms, r));
                        }
                    }
                    i = k;
                    continue;
                }
            }
        }
        i += 1;
    }
    best.map(|b| b.1)
}

/// py: hostmig.py:163 region_first. members_fn(t) -> the party's tokens, waited_fn(t) -> seconds.
pub fn region_first(
    tokens: &[String],
    region_fn: &dyn Fn(&str) -> Option<String>,
    members_fn: Option<&dyn Fn(&str) -> Vec<String>>,
    waited_fn: Option<&dyn Fn(&str) -> f64>,
    max_wait: Option<f64>,
) -> (Vec<String>, Option<String>) {
    let mut reg: HashMap<String, Option<String>> = HashMap::new();
    for t in tokens {
        if reg.contains_key(t) {
            continue;
        }
        let mut group: Vec<String> = match members_fn {
            Some(f) => f(t),
            None => vec![t.clone()],
        };
        if !group.contains(t) {
            group.insert(0, t.clone());
        }
        let mut r: Option<String> = None;
        for m in &group {
            let mr = region_fn(m);
            if let Some(x) = &mr {
                if MM_REGIONS.contains(&x.as_str()) {
                    r = mr;
                    break;
                }
            }
        }
        for m in &group {
            reg.insert(m.clone(), r.clone());
        }
    }
    let mut cnt: HashMap<String, i64> = HashMap::new();
    for t in tokens {
        if let Some(Some(r)) = reg.get(t) {
            *cnt.entry(r.clone()).or_insert(0) += 1;
        }
    }
    let Some(top) = cnt.values().copied().max() else { return (tokens.to_vec(), None) };
    let mut lead: Option<String> = None;
    for t in tokens {
        if let Some(Some(r)) = reg.get(t) {
            if cnt.get(r).copied() == Some(top) {
                lead = Some(r.clone());
                break;
            }
        }
    }
    let first = |t: &String| -> i32 {
        if reg.get(t).and_then(|x| x.as_ref()) == lead.as_ref() {
            return 0;
        }
        if let (Some(w), Some(mw)) = (waited_fn, max_wait) {
            if w(t) >= mw {
                return 0;
            }
        }
        1
    };
    let mut out: Vec<String> = tokens.to_vec();
    out.sort_by_key(|t| first(t));
    (out, lead)
}

/// Python `int(s)` for the pin steam ids (optional sign, digits with single underscores between them).
fn py_int(s: &str) -> Option<i64> {
    let t = py::strip(s);
    let (neg, body) = match t.strip_prefix('-') {
        Some(b) => (true, b),
        None => (false, t.strip_prefix('+').unwrap_or(t)),
    };
    if body.is_empty() || body.starts_with('_') || body.ends_with('_') || body.contains("__") {
        return None;
    }
    let digits: String = body.chars().filter(|&c| c != '_').collect();
    if !digits.chars().all(|c| c.is_ascii_digit()) {
        return None;
    }
    let v: i64 = digits.parse().ok()?;
    Some(if neg { -v } else { v })
}

/// py: hostmig.py:200 parse_pins  ('76561198261455254:EU,1234:NA-east' -> {steamid: region})
pub fn parse_pins(text: &str) -> HashMap<i64, String> {
    let mut out = HashMap::new();
    for part in text.split(',') {
        if let Some((a, r)) = part.split_once(':') {
            if let Some(n) = py_int(a) {
                out.insert(n, py::strip(r).to_string());
            }
        }
    }
    out
}

/// py: hostmig.py:222 ping_fit. Returns (kept, dropped, host, info).
pub fn ping_fit(
    players: &[String],
    pair: &dyn Fn(&str, &str) -> Option<i64>,
    limit: i64,
    units_fn: Option<&dyn Fn(&str) -> Vec<String>>,
    host_ok: Option<&dyn Fn(&str) -> bool>,
) -> (Vec<String>, Vec<String>, Option<String>, String) {
    let players: Vec<String> = players.to_vec();
    if limit <= 0 || players.len() < 2 {
        return (players, Vec::new(), None, if limit <= 0 { "off".into() } else { "one player".into() });
    }
    let mut unit: HashMap<String, Vec<String>> = HashMap::new();
    for t in &players {
        if unit.contains_key(t) {
            continue;
        }
        let mut g: Vec<String> = match units_fn {
            Some(f) => f(t).into_iter().filter(|m| players.contains(m)).collect(),
            None => vec![t.clone()].into_iter().filter(|m| players.contains(m)).collect(),
        };
        if !g.contains(t) {
            g.insert(0, t.clone());
        }
        for m in &g {
            unit.insert(m.clone(), g.clone());
        }
    }
    let mut any = false;
    'o: for (i, a) in players.iter().enumerate() {
        for b in &players[i + 1..] {
            if pair(a, b).is_some() {
                any = true;
                break 'o;
            }
        }
    }
    if !any {
        return (players, Vec::new(), None, "no pings known".into());
    }
    let mut far: HashSet<String> = HashSet::new();
    for t in &players {
        let ut = &unit[t];
        let others: Vec<&String> = players.iter().filter(|o| !ut.contains(*o)).collect();
        let known: Vec<i64> = others.iter().filter_map(|o| pair(t, o)).collect();
        if !others.is_empty() && known.len() as i64 >= std::cmp::max(2, (others.len() as i64 + 1) / 2) && known.iter().copied().min().unwrap_or(0) > limit {
            for m in ut {
                far.insert(m.clone());
            }
        }
    }
    let mut best: Option<((i64, i64, usize), String, Vec<String>)> = None;
    for h in &players {
        if let Some(ok) = host_ok {
            if !ok(h) {
                continue;
            }
        }
        let uh = &unit[h];
        let mut bad: HashSet<String> = far.iter().filter(|x| !uh.contains(*x)).cloned().collect();
        for t in &players {
            if t == h {
                continue;
            }
            let ms = pair(h, t);
            if matches!(ms, Some(v) if v > limit) || (ms.is_none() && far.contains(h) && !uh.contains(t)) {
                for m in &unit[t] {
                    bad.insert(m.clone());
                }
            }
        }
        if bad.contains(h) {
            continue;
        }
        let kept: Vec<String> = players.iter().filter(|t| !bad.contains(*t)).cloned().collect();
        let known: Vec<i64> = kept.iter().filter(|t| *t != h).filter_map(|t| pair(h, t)).collect();
        let key = (-(kept.len() as i64), known.iter().copied().max().unwrap_or(0), players.iter().position(|x| x == h).unwrap_or(0));
        if best.as_ref().is_none_or(|b| key < b.0) {
            best = Some((key, h.clone(), kept));
        }
    }
    let Some((_, h, kept)) = best else { return (players, Vec::new(), None, "no host candidate".into()) };
    let dropped: Vec<String> = players.iter().filter(|t| !kept.contains(t)).cloned().collect();
    let info = format!("host {} keeps {} of {}", py::cut(&h, 6), kept.len(), players.len());
    (kept, dropped, Some(h), info)
}

/// py: hostmig.py:287 region_pool. region(token) / steamid(token) / pair(a, b) are lookups; `pins` None = the env pins.
pub fn region_pool(
    players: &[String],
    region: &dyn Fn(&str) -> Option<String>,
    steamid: &dyn Fn(&str) -> i64,
    pair: &dyn Fn(&str, &str) -> Option<i64>,
    pins: Option<&HashMap<i64, String>>,
    everyone: Option<&[String]>,
) -> (Vec<String>, String) {
    let pins = pins.unwrap_or(&cfg().pins);
    let mut everyone: Vec<String> = match everyone {
        Some(e) if !e.is_empty() => e.to_vec(),
        _ => players.to_vec(),
    };
    for t in players {
        if !everyone.contains(t) {
            everyone.push(t.clone());
        }
    }
    let mut regs: HashMap<String, Option<String>> = HashMap::new();
    for t in &everyone {
        regs.insert(t.clone(), region(t).filter(|r| !r.is_empty()));
    }
    let rg = |t: &str| -> Option<&String> { regs.get(t).and_then(|x| x.as_ref()) };
    let info: String = everyone.iter().map(|t| format!("{}={}", py::cut(t, 6), rg(t).map(|s| s.as_str()).unwrap_or("?"))).collect::<Vec<_>>().join(" ");
    let known: Vec<&String> = everyone.iter().filter(|t| rg(t).is_some()).collect();
    let pinned: Option<&String> = everyone.iter().find_map(|t| pins.get(&steamid(t)));
    if let Some(pinned) = pinned.filter(|p| !p.is_empty()) {
        let pool: Vec<String> = players.iter().filter(|t| rg(t) == Some(pinned)).cloned().collect();
        if !pool.is_empty() {
            return (pool.clone(), format!("region pin {} ({} of {}) | {}", pinned, pool.len(), players.len(), info));
        }
        return (players.to_vec(), format!("region pin {} but nobody there, all | {}", pinned, info));
    }
    if known.is_empty() || (everyone.len() - known.len()) as f64 > REGION_FALLBACK_UNKNOWN_FRAC * everyone.len() as f64 {
        return (players.to_vec(), format!("regions unknown for too many, all | {}", info));
    }
    let mut counts: std::collections::BTreeMap<String, i64> = std::collections::BTreeMap::new();
    for t in &known {
        *counts.entry(rg(t).cloned().unwrap_or_default()).or_insert(0) += 1;
    }
    let combined = |r: &str| -> i64 {
        let m: Vec<&&String> = known.iter().filter(|t| rg(t).map(|x| x.as_str()) == Some(r)).collect();
        let mut tot = 0;
        for a in &m {
            for b in &m {
                if a.as_str() < b.as_str() {
                    tot += pair(a, b).unwrap_or(1000);
                }
            }
        }
        tot
    };
    let top = counts.values().copied().max().unwrap_or(0);
    let tied: Vec<&String> = counts.iter().filter(|(_, c)| **c == top).map(|(r, _)| r).collect();
    let comb: Vec<(&String, i64)> = tied.iter().map(|r| (*r, combined(r))).collect();
    let cnt_txt: String = counts.iter().map(|(k, v)| format!("{} {}", k, v)).collect::<Vec<_>>().join(",");
    let distinct: HashSet<i64> = comb.iter().map(|c| c.1).collect();
    if tied.len() > 1 && distinct.len() == 1 {
        return (players.to_vec(), format!("tied regions ({}), ping cannot break the tie: no region rule | {}", cnt_txt, info));
    }
    let win: &String = comb.iter().min_by(|a, b| (a.1, a.0).cmp(&(b.1, b.0))).map(|c| c.0).unwrap_or(tied[0]);
    let pool: Vec<String> = players.iter().filter(|t| rg(t) == Some(win)).cloned().collect();
    if pool.is_empty() {
        return (players.to_vec(), format!("majority {} has no candidate ({}), all | {}", win, cnt_txt, info));
    }
    let guard = cfg().region_guard_ms;
    if guard > 0 && pool.len() < players.len() {
        let worst = |h: &str| -> Option<i64> {
            let p: Vec<Option<i64>> = everyone.iter().filter(|o| o.as_str() != h).map(|o| pair(h, o)).collect();
            if p.is_empty() || p.iter().any(|x| x.is_none()) {
                None
            } else {
                p.iter().filter_map(|x| *x).max()
            }
        };
        let wp: Vec<Option<i64>> = pool.iter().map(|h| worst(h)).collect();
        let wa: Vec<Option<i64>> = players.iter().map(|h| worst(h)).collect();
        if wp.iter().all(|x| x.is_some()) && wa.iter().all(|x| x.is_some()) {
            let mp = wp.iter().filter_map(|x| *x).min().unwrap_or(0);
            let ma = wa.iter().filter_map(|x| *x).min().unwrap_or(0);
            if mp > ma + guard {
                return (
                    players.to_vec(),
                    format!(
                        "majority {} ({}) would cost {} ms of worst-case ping (best {} ms outside it, guard {}): no region rule | {}",
                        win,
                        cnt_txt,
                        mp - ma,
                        ma,
                        guard,
                        info
                    ),
                );
            }
        }
    }
    (pool, format!("majority {} ({}) | {}", win, cnt_txt, info))
}

// ------------------------------------------------------------ HostMigration
#[derive(Clone, Default)]
struct Ho {
    slot: usize,
    t: f64,
    ready: BTreeSet<usize>,
    epoch: i64,
    roster: BTreeSet<usize>,
    need: BTreeSet<usize>,
}

#[derive(Clone, Default)]
struct Match {
    players: Vec<String>,
    host: usize,
    epoch: i64,
    gone: BTreeSet<usize>,
    seen: HashMap<usize, f64>,
    silent: HashMap<usize, f64>,
    silent_since: Option<f64>,
    lost: Option<f64>,
    bridge_gone: BTreeSet<usize>,
    bridge_at: HashMap<usize, f64>,
    given_up: bool,
    t0: f64,
    cap: BTreeSet<usize>,
    replaced: HashMap<usize, f64>,
    replaced_told: HashMap<usize, f64>,
    pending: bool,
    collect_t0: Option<f64>,
    cand0: Option<BTreeSet<usize>>,
    collect_logged: bool,
    alone: bool,
    alive_host_logged: bool,
    min_silent_logged: bool,
    freezes: Vec<(usize, f64)>,
    joined: HashMap<usize, f64>,
    ho: Option<Ho>,
    migrated_at: Option<f64>,
    adopted: Option<f64>,
}

#[derive(Default)]
struct HmInner {
    matches: IndexMap<String, Match>,
    by_token: HashMap<String, String>,
}

pub struct HostInfo {
    pub host: usize,
    pub epoch: i64,
    pub pending: bool,
    pub given_up: bool,
    pub alone: bool,
}

pub struct Snapshot {
    pub t0: f64,
    pub players: Vec<String>,
    pub alive: Vec<usize>,
    pub host: usize,
}

pub struct HmOpts {
    pub enabled: bool,
    pub now: NowFn,
    pub ping: Option<PingFn>,
    pub perf: Option<PerfFn>,
    pub pool: Option<PoolFn>,
    pub link_alive: Option<LinkFn>,
    pub alive_window: f64,
    pub silent_window: f64,
    pub lone_wait: f64,
    pub min_left: usize,
    pub settle: f64,
    pub bridge_grace: f64,
    pub unknown_keep_going: bool,
    pub collect: f64,
    pub min_silent: Option<f64>,
}

impl HmOpts {
    pub fn new(enabled: bool) -> Self {
        HmOpts {
            enabled,
            now: Box::new(clock::now),
            ping: None,
            perf: None,
            pool: None,
            link_alive: None,
            alive_window: 3.0,
            silent_window: 2.5,
            lone_wait: 6.0,
            min_left: 1,
            settle: 3.0,
            bridge_grace: 4.0,
            unknown_keep_going: true,
            collect: 3.0,
            min_silent: None,
        }
    }
}

const START_GRACE: f64 = 12.0;
const HANDOVER_TIMEOUT: f64 = 30.0;
const POLL_STALE_S: f64 = 25.0;
const JIP_LOAD_S: f64 = 120.0;

/// py: hostmig.py:359 HostMigration
pub struct HostMigration {
    pub enabled: bool,
    now: NowFn,
    ping: Option<PingFn>,
    perf: Option<PerfFn>,
    pool: Option<PoolFn>,
    link_alive: Option<LinkFn>,
    alive_window: f64,
    silent_window: f64,
    lone_wait: f64,
    min_left: usize,
    settle: f64,
    bridge_grace: f64,
    unknown_keep_going: bool,
    collect: f64,
    min_silent: f64,
    inner: Mutex<HmInner>,
}

fn alive_mask(m: &Match) -> i64 {
    (0..m.players.len()).filter(|s| !m.gone.contains(s)).map(|s| 1i64 << s).sum()
}

fn f_or(m: &HashMap<usize, f64>, k: usize, d: f64) -> f64 {
    m.get(&k).copied().unwrap_or(d)
}

impl HostMigration {
    pub fn new(o: HmOpts) -> Self {
        HostMigration {
            enabled: o.enabled,
            now: o.now,
            ping: o.ping,
            perf: o.perf,
            pool: o.pool,
            link_alive: o.link_alive,
            alive_window: o.alive_window,
            silent_window: o.silent_window,
            lone_wait: o.lone_wait,
            min_left: o.min_left,
            settle: o.settle,
            bridge_grace: o.bridge_grace,
            unknown_keep_going: o.unknown_keep_going,
            collect: o.collect,
            min_silent: o.min_silent.unwrap_or(cfg().min_silent_s),
            inner: Mutex::new(HmInner::default()),
        }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    // ---------------------------------------------------------- match lifecycle
    /// py: hostmig.py:386 start
    pub fn start(&self, mid: &str, players: &[String]) {
        let mut g = self.inner.lock();
        Self::drop_match(&mut g, mid);
        let m = Match { players: players.to_vec(), t0: self.now(), ..Default::default() };
        g.matches.insert(mid.to_string(), m);
        for t in players {
            g.by_token.insert(t.clone(), mid.to_string());
        }
    }

    /// py: hostmig.py:396 add_player
    pub fn add_player(&self, mid: &str, token: &str, slot: usize) {
        let mut g = self.inner.lock();
        let g = &mut *g;
        let now = self.now();
        let Some(m) = g.matches.get_mut(mid) else { return };
        if let Some(old) = m.players.iter().position(|x| x == token) {
            if old != slot {
                m.players[old] = placeholder(old);
                m.gone.insert(old);
                m.seen.remove(&old);
                m.silent.remove(&old);
            }
        }
        while slot > m.players.len() {
            let n = m.players.len();
            m.gone.insert(n);
            m.players.push(placeholder(n));
        }
        if slot < m.players.len() {
            let prev = m.players[slot].clone();
            g.by_token.remove(&prev);
            m.players[slot] = token.to_string();
        } else {
            m.players.push(token.to_string());
        }
        m.gone.remove(&slot);
        m.seen.remove(&slot);
        m.silent.remove(&slot);
        m.joined.insert(slot, now);
        g.by_token.insert(token.to_string(), mid.to_string());
        dlog!("HOSTMIG", mid, format!("slot {} joined in progress", slot));
    }

    /// py: hostmig.py:426 end
    pub fn end(&self, mid: &str) {
        let mut g = self.inner.lock();
        Self::drop_match(&mut g, mid);
    }

    fn drop_match(g: &mut HmInner, mid: &str) {
        if let Some(m) = g.matches.shift_remove(mid) {
            for t in &m.players {
                if g.by_token.get(t).map(|x| x.as_str()) == Some(mid) {
                    g.by_token.remove(t);
                }
            }
        }
    }

    pub fn has(&self, token: &str) -> bool {
        self.inner.lock().by_token.contains_key(token)
    }

    pub fn given_up(&self, mid: &str) -> bool {
        self.inner.lock().matches.get(mid).is_some_and(|m| m.given_up)
    }

    /// py: hostmig.py:446 pending_stale
    pub fn pending_stale(&self, mid: &str, secs: f64) -> f64 {
        let g = self.inner.lock();
        let Some(m) = g.matches.get(mid) else { return 0.0 };
        let lost = m.lost.unwrap_or(0.0);
        if !m.pending || m.given_up || lost == 0.0 {
            return 0.0;
        }
        let age = self.now() - lost;
        if age > secs {
            age
        } else {
            0.0
        }
    }

    /// py: hostmig.py:455 host_info
    pub fn host_info(&self, mid: &str) -> Option<HostInfo> {
        let g = self.inner.lock();
        let m = g.matches.get(mid)?;
        Some(HostInfo { host: m.host, epoch: m.epoch, pending: m.pending, given_up: m.given_up, alone: m.alone })
    }

    /// py: hostmig.py:463 snapshot
    pub fn snapshot(&self, mid: &str) -> Option<Snapshot> {
        let g = self.inner.lock();
        let m = g.matches.get(mid)?;
        if m.given_up {
            return None;
        }
        Some(Snapshot { t0: m.t0, players: m.players.clone(), alive: (0..m.players.len()).filter(|s| !m.gone.contains(s)).collect(), host: m.host })
    }

    pub fn mids(&self) -> Vec<String> {
        self.inner.lock().matches.keys().cloned().collect()
    }

    /// (matches, by_token): the sizes the daemon's stats dump counts.
    pub fn sizes(&self) -> (usize, usize) {
        let g = self.inner.lock();
        (g.matches.len(), g.by_token.len())
    }

    /// What daemon.py reads under `with HM.lock` (HM.matches.get(mid)): `(lost or pending) and not given_up`.
    pub fn host_lost_pending(&self, mid: &str) -> bool {
        let g = self.inner.lock();
        g.matches.get(mid).is_some_and(|m| (m.lost.unwrap_or(0.0) != 0.0 || m.pending) && !m.given_up)
    }

    fn to_value(m: &Match, with_ho: bool) -> Value {
        let fmap = |h: &HashMap<usize, f64>| -> Value {
            let mut v: Vec<(String, f64)> = h.iter().map(|(k, x)| (k.to_string(), *x)).collect();
            v.sort_by(|a, b| a.0.cmp(&b.0));
            let mut o = Map::new();
            for (k, x) in v {
                o.insert(k, Value::from(x));
            }
            Value::Object(o)
        };
        let set = |s: &BTreeSet<usize>| Value::Array(s.iter().map(|x| Value::from(*x as u64)).collect());
        let opt = |x: Option<f64>| x.map(Value::from).unwrap_or(Value::Null);
        let mut e: Vec<(&str, Value)> = vec![
            ("players", Value::Array(m.players.iter().map(|s| Value::from(s.as_str())).collect())),
            ("host", Value::from(m.host as u64)),
            ("epoch", Value::from(m.epoch)),
            ("gone", set(&m.gone)),
            ("seen", fmap(&m.seen)),
            ("silent", fmap(&m.silent)),
            ("silent_since", opt(m.silent_since)),
            ("lost", opt(m.lost)),
            ("bridge_gone", set(&m.bridge_gone)),
            ("bridge_at", fmap(&m.bridge_at)),
            ("given_up", Value::Bool(m.given_up)),
            ("t0", Value::from(m.t0)),
        ];
        if !m.cap.is_empty() {
            e.push(("cap", set(&m.cap)));
        }
        if !m.replaced.is_empty() {
            e.push(("replaced", fmap(&m.replaced)));
        }
        if !m.replaced_told.is_empty() {
            e.push(("replaced_told", fmap(&m.replaced_told)));
        }
        if m.pending {
            e.push(("pending", Value::Bool(true)));
        }
        if let Some(t) = m.collect_t0 {
            e.push(("collect_t0", Value::from(t)));
        }
        if let Some(c) = &m.cand0 {
            e.push(("cand0", set(c)));
        }
        if m.collect_logged {
            e.push(("collect_logged", Value::from(1)));
        }
        if m.alone {
            e.push(("alone", Value::Bool(true)));
        }
        if m.alive_host_logged {
            e.push(("alive_host_logged", Value::from(1)));
        }
        if m.min_silent_logged {
            e.push(("min_silent_logged", Value::from(1)));
        }
        if !m.freezes.is_empty() {
            e.push(("freezes", Value::Array(m.freezes.iter().map(|(h, t)| Value::Array(vec![Value::from(*h as u64), Value::from(*t)])).collect())));
        }
        if !m.joined.is_empty() {
            e.push(("joined", fmap(&m.joined)));
        }
        if let Some(t) = m.migrated_at {
            e.push(("migrated_at", Value::from(t)));
        }
        if let Some(t) = m.adopted {
            e.push(("adopted", Value::from(t)));
        }
        if with_ho {
            if let Some(h) = &m.ho {
                let mut o = Map::new();
                o.insert("epoch".into(), Value::from(h.epoch));
                o.insert("need".into(), set(&h.need));
                o.insert("ready".into(), set(&h.ready));
                o.insert("roster".into(), set(&h.roster));
                o.insert("slot".into(), Value::from(h.slot as u64));
                o.insert("t".into(), Value::from(h.t));
                e.push(("ho", Value::Object(o)));
            }
        }
        e.sort_by(|a, b| a.0.cmp(b.0));
        let mut o = Map::new();
        for (k, v) in e {
            o.insert(k.to_string(), v);
        }
        Value::Object(o)
    }

    /// py: hostmig.py:476 export_match
    pub fn export_match(&self, mid: &str) -> Option<Value> {
        let g = self.inner.lock();
        let m = g.matches.get(mid)?;
        if m.given_up {
            return None;
        }
        Some(Self::to_value(m, false))
    }

    /// Test hook: every match record incl. a handover in preparation, plus the token map (keys sorted).
    #[allow(dead_code)]
    pub fn dump_state(&self) -> Value {
        let g = self.inner.lock();
        let mut ms = Vec::new();
        for (mid, m) in &g.matches {
            ms.push(Value::Array(vec![Value::from(mid.as_str()), Self::to_value(m, true)]));
        }
        let mut bt: Vec<(&String, &String)> = g.by_token.iter().collect();
        bt.sort();
        let mut o = Map::new();
        for (k, v) in bt {
            o.insert(k.clone(), Value::from(v.as_str()));
        }
        let mut r = Map::new();
        r.insert("by_token".into(), Value::Object(o));
        r.insert("matches".into(), Value::Array(ms));
        Value::Object(r)
    }

    /// py: hostmig.py:494 import_match (rec = the JSON export_match wrote)
    pub fn import_match(&self, mid: &str, rec: &Value) {
        let mut g = self.inner.lock();
        Self::drop_match(&mut g, mid);
        let now = self.now();
        let obj = rec.as_object();
        let get = |k: &str| obj.and_then(|o| o.get(k));
        let fl = |k: &str| get(k).and_then(|v| v.as_f64());
        let usz_set = |k: &str| -> BTreeSet<usize> { get(k).and_then(|v| v.as_array()).map(|a| a.iter().filter_map(|x| x.as_i64()).map(|x| x as usize).collect()).unwrap_or_default() };
        let fmap = |k: &str| -> HashMap<usize, f64> {
            get(k)
                .and_then(|v| v.as_object())
                .map(|o| o.iter().filter_map(|(a, b)| Some((a.parse::<usize>().ok()?, b.as_f64()?))).collect())
                .unwrap_or_default()
        };
        let truthy = |k: &str| match get(k) {
            None | Some(Value::Null) => false,
            Some(Value::Bool(b)) => *b,
            Some(Value::Number(n)) => n.as_f64().is_some_and(|x| x != 0.0),
            Some(Value::String(s)) => !s.is_empty(),
            Some(Value::Array(a)) => !a.is_empty(),
            Some(Value::Object(o)) => !o.is_empty(),
        };
        let mut m = Match {
            players: get("players").and_then(|v| v.as_array()).map(|a| a.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect()).unwrap_or_default(),
            host: get("host").and_then(|v| v.as_i64()).unwrap_or(0) as usize,
            epoch: get("epoch").and_then(|v| v.as_i64()).unwrap_or(0),
            gone: usz_set("gone"),
            lost: fl("lost"),
            t0: fl("t0").unwrap_or(now),
            cap: usz_set("cap"),
            replaced: fmap("replaced"),
            replaced_told: fmap("replaced_told"),
            pending: truthy("pending"),
            collect_t0: fl("collect_t0"),
            cand0: get("cand0").map(|_| usz_set("cand0")),
            collect_logged: truthy("collect_logged"),
            alone: truthy("alone"),
            alive_host_logged: truthy("alive_host_logged"),
            min_silent_logged: truthy("min_silent_logged"),
            freezes: get("freezes")
                .and_then(|v| v.as_array())
                .map(|a| {
                    a.iter()
                        .filter_map(|x| {
                            let p = x.as_array()?;
                            Some((p.first()?.as_i64()? as usize, p.get(1)?.as_f64()?))
                        })
                        .collect()
                })
                .unwrap_or_default(),
            joined: fmap("joined"),
            migrated_at: fl("migrated_at"),
            adopted: Some(now),
            ..Default::default()
        };
        if m.pending {
            m.collect_t0 = Some(now);
            m.lost = Some(now);
            m.cand0 = Some(BTreeSet::new());
            m.collect_logged = false;
        }
        if m.migrated_at.is_some() {
            m.migrated_at = Some(now - self.settle);
        }
        let (host, epoch, pend) = (m.host, m.epoch, m.pending);
        let players = m.players.clone();
        g.matches.insert(mid.to_string(), m);
        for t in &players {
            if !t.starts_with(&"f0".repeat(30)) {
                g.by_token.insert(t.clone(), mid.to_string());
            }
        }
        dlog!("HOSTMIG", mid, "re-adopted after a daemon restart: host slot", host, "epoch", epoch, if pend { "(host loss pending)" } else { "" });
    }

    /// py: hostmig.py:521 host_token
    pub fn host_token(&self, token: &str) -> Option<String> {
        let g = self.inner.lock();
        let m = g.matches.get(g.by_token.get(token)?)?;
        m.players.get(m.host).cloned()
    }

    // ---------------------------------------------------------- events
    /// py: hostmig.py:528 depart
    pub fn depart(&self, token: &str, reason: &str) {
        let mut g = self.inner.lock();
        let Some(mid) = g.by_token.get(token).cloned() else { return };
        let Some(m) = g.matches.get_mut(&mid) else { return };
        let Some(slot) = m.players.iter().position(|x| x == token) else { return };
        if m.gone.contains(&slot) {
            return;
        }
        m.gone.insert(slot);
        dlog!("HOSTMIG", mid, format!("slot {} {}", slot, reason), if slot == m.host { "(host)" } else { "" });
        if slot == m.host {
            self.host_lost(&mid, m, reason);
        }
    }

    /// py: hostmig.py:545 bridge_gone
    pub fn bridge_gone(&self, token: &str) {
        let mut g = self.inner.lock();
        let Some(mid) = g.by_token.get(token).cloned() else { return };
        let Some(m) = g.matches.get_mut(&mid) else { return };
        let Some(slot) = m.players.iter().position(|x| x == token) else { return };
        m.bridge_gone.insert(slot);
        m.bridge_at.insert(slot, self.now());
        dlog!("HOSTMIG", mid, format!("slot {} Steam helper gone", slot));
        self.evaluate(&mid, m);
    }

    /// py: hostmig.py:558 bridge_back
    pub fn bridge_back(&self, token: &str) {
        let mut g = self.inner.lock();
        let Some(mid) = g.by_token.get(token).cloned() else { return };
        let Some(m) = g.matches.get_mut(&mid) else { return };
        let Some(slot) = m.players.iter().position(|x| x == token) else { return };
        if m.bridge_gone.remove(&slot) {
            m.bridge_at.remove(&slot);
            dlog!("HOSTMIG", mid, format!("slot {} Steam helper back", slot));
        }
    }

    /// py: hostmig.py:571 poll. The 0xE7 payload; None where the Python raises (reply byte > 255, token missing from its match).
    pub fn poll(&self, token: &str, flags: u8, epoch: u8) -> Option<Vec<u8>> {
        if !self.enabled {
            return Some(vec![0, 0, 0, 0, ST_GIVE_UP]);
        }
        let mut g = self.inner.lock();
        let mid = g.by_token.get(token).cloned();
        let Some(m) = mid.as_ref().and_then(|x| g.matches.get_mut(x)) else {
            if self.unknown_keep_going {
                return Some(vec![1, 0, 0, 0xFF, ST_NORMAL]);
            }
            return Some(vec![1, 0, 0, 0, ST_GIVE_UP]);
        };
        let mid = mid.unwrap_or_default();
        let slot = m.players.iter().position(|x| x == token)?;
        let now = self.now();
        m.seen.insert(slot, now);
        if flags & 2 != 0 {
            m.cap.insert(slot);
        }
        if m.gone.contains(&slot) {
            if m.replaced.contains_key(&slot) {
                m.replaced_told.insert(slot, now);
            }
            return reply(m, ST_GIVE_UP);
        }
        if m.pending {
            self.host_lost(&mid, m, "successor asked");
        }
        let settling = now - m.migrated_at.unwrap_or(-1e9) < self.settle;
        if flags & 1 != 0 && slot != m.host && epoch as i64 == m.epoch && !settling {
            if !m.silent.contains_key(&slot) {
                dlog!("HOSTMIG", mid, format!("slot {} reports the host (slot {}) silent", slot, m.host));
            }
            m.silent.insert(slot, now);
            if m.silent_since.is_none() {
                m.silent_since = Some(now);
            }
        } else if flags & 1 == 0 && m.silent.contains_key(&slot) {
            m.silent.remove(&slot);
            if m.silent.is_empty() {
                m.silent_since = None;
                m.min_silent_logged = false;
            }
        }
        self.evaluate(&mid, m);
        if m.ho.is_some() {
            if flags & 4 != 0 && epoch as i64 == m.epoch {
                if let Some(ho) = m.ho.as_mut() {
                    ho.ready.insert(slot);
                }
            }
            self.handover_step(&mid, m);
        }
        let state = if m.given_up {
            ST_GIVE_UP
        } else if !m.silent.is_empty() {
            ST_SUSPECT
        } else if m.ho.is_some() {
            ST_HANDOVER
        } else {
            ST_NORMAL
        };
        reply(m, state)
    }

    // ---------------------------------------------------------- decision
    /// py: hostmig.py:629 _host_dead
    fn host_dead(&self, m: &Match, h: usize) -> bool {
        if m.bridge_gone.contains(&h) {
            return true;
        }
        if let Some(f) = &self.link_alive {
            return match m.players.get(h) {
                Some(t) => match f(t) {
                    Some(alive) => !alive,
                    None => false,
                },
                None => false,
            };
        }
        false
    }

    /// py: hostmig.py:639 replaced_age
    pub fn replaced_age(&self, token: &str) -> Option<f64> {
        let g = self.inner.lock();
        let m = g.matches.get(g.by_token.get(token)?)?;
        let slot = m.players.iter().position(|x| x == token)?;
        let t = *m.replaced.get(&slot)?;
        let now = self.now();
        match m.replaced_told.get(&slot) {
            Some(told) => Some((now - t).min(now - told)),
            None => Some(now - t),
        }
    }

    /// py: hostmig.py:655 _evaluate
    fn evaluate(&self, mid: &str, m: &mut Match) {
        if m.given_up || m.pending {
            return;
        }
        let now = self.now();
        let h = m.host;
        let stale: Vec<usize> = m.silent.iter().filter(|(_, t)| now - **t > self.silent_window).map(|(s, _)| *s).collect();
        for s in stale {
            m.silent.remove(&s);
        }
        if m.silent.is_empty() {
            m.silent_since = None;
            m.min_silent_logged = false;
            return;
        }
        let others: Vec<usize> = (0..m.players.len()).filter(|s| *s != h && !m.gone.contains(s)).collect();
        let mut all_silent = !others.is_empty() && others.iter().all(|s| m.silent.contains_key(s));
        let silent_since = m.silent_since.unwrap_or(now);
        if all_silent && now - f_or(&m.seen, h, -1e9) <= self.alive_window && (others.len() <= 1 || now - m.t0 < START_GRACE) {
            if !m.alive_host_logged {
                m.alive_host_logged = true;
                dlog!("HOSTMIG", mid, format!("host slot {} still polls - silence reports from {} player(s) not enough to replace it", h, others.len()));
            }
            all_silent = false;
        }
        if all_silent && self.min_silent > 0.0 && now - silent_since < self.min_silent && !self.host_dead(m, h) {
            if !m.min_silent_logged {
                m.min_silent_logged = true;
                m.freezes.push((h, now));
                if m.freezes.len() > 20 {
                    let cut = m.freezes.len() - 20;
                    m.freezes.drain(..cut);
                }
                dlog!(
                    "HOSTMIG",
                    mid,
                    format!(
                        "every player reports host slot {} silent, but its link to the server is up - waiting until the silence lasts {} s (a hitch is no host loss)",
                        h,
                        py::fixed(self.min_silent, 0)
                    )
                );
            }
            all_silent = false;
        }
        let lone = now - silent_since >= self.lone_wait && now - f_or(&m.seen, h, -1e9) > self.alive_window;
        let hgone = m.bridge_gone.contains(&h) && now - f_or(&m.bridge_at, h, now) >= self.bridge_grace;
        if hgone || all_silent || lone {
            let why = if hgone {
                "host's Steam helper gone".to_string()
            } else if all_silent {
                "all players report silence".to_string()
            } else {
                format!("silence reports for {} s", py::fixed(self.lone_wait, 0))
            };
            m.gone.insert(h);
            m.replaced.insert(h, now);
            self.host_lost(mid, m, &why);
        }
    }

    // ---------------------------------------------------------- planned handover (HostRepick)
    /// py: hostmig.py:710 freezes
    pub fn freezes(&self, mid: &str, host_slot: usize, window_s: f64) -> usize {
        let g = self.inner.lock();
        let Some(m) = g.matches.get(mid) else { return 0 };
        let now = self.now();
        m.freezes.iter().filter(|(h, t)| *h == host_slot && now - *t <= window_s).count()
    }

    /// py: hostmig.py:722 roster
    pub fn roster(&self, mid: &str) -> Option<BTreeSet<usize>> {
        let g = self.inner.lock();
        let m = g.matches.get(mid)?;
        Some((0..m.players.len()).filter(|s| !m.gone.contains(s)).collect())
    }

    /// py: hostmig.py:732 _stale  -> (stale, loading)
    fn stale_of(m: &Match, now: f64) -> (BTreeSet<usize>, BTreeSet<usize>) {
        let (mut stale, mut loading) = (BTreeSet::new(), BTreeSet::new());
        let dflt = m.adopted.unwrap_or(m.t0);
        for s in 0..m.players.len() {
            if m.gone.contains(&s) || now - f_or(&m.seen, s, dflt) <= POLL_STALE_S {
                continue;
            }
            let j = m.joined.get(&s);
            if !m.seen.contains_key(&s) && j.is_some_and(|j| now - *j < JIP_LOAD_S) {
                loading.insert(s);
            } else {
                stale.insert(s);
            }
        }
        (stale, loading)
    }

    /// py: hostmig.py:748 stale_slots
    pub fn stale_slots(&self, mid: &str) -> (BTreeSet<usize>, BTreeSet<usize>) {
        let g = self.inner.lock();
        match g.matches.get(mid) {
            Some(m) => Self::stale_of(m, self.now()),
            None => (BTreeSet::new(), BTreeSet::new()),
        }
    }

    /// py: hostmig.py:754 _handover_ok
    fn handover_ok_of(&self, m: Option<&Match>, slot: Option<i64>) -> (bool, String) {
        let Some(m) = m else { return (false, "no migration record".into()) };
        if !self.enabled {
            return (false, "no migration record".into());
        }
        if m.given_up || m.pending || m.ho.is_some() || !m.silent.is_empty() || m.bridge_gone.iter().any(|s| !m.gone.contains(s)) {
            return (false, "migration busy / host link trouble".into());
        }
        let now = self.now();
        if now - m.migrated_at.unwrap_or(-1e9) < 20.0 {
            return (false, "previous migration just happened".into());
        }
        let (stale, loading) = Self::stale_of(m, now);
        if let Some(l) = loading.iter().next() {
            return (false, format!("slot {} still loading (joined in progress)", l));
        }
        if stale.contains(&m.host) {
            return (false, format!("host slot {} not polling", m.host));
        }
        if let Some(s) = slot {
            if s >= 0 && stale.contains(&(s as usize)) {
                return (false, format!("target slot {} not polling", s));
            }
        }
        let live: Vec<usize> = (0..m.players.len()).filter(|s| !m.gone.contains(s) && !stale.contains(s)).collect();
        if live.len() < 2 {
            return match stale.iter().next() {
                Some(s) => (false, format!("slot {} not polling", s)),
                None => (false, "nobody to hand over to".into()),
            };
        }
        for s in live {
            if !m.cap.contains(&s) {
                return (false, format!("slot {} ext has no handover support", s));
            }
        }
        (true, String::new())
    }

    /// py: hostmig.py:783 handover_ok
    pub fn handover_ok(&self, mid: &str) -> (bool, String) {
        let g = self.inner.lock();
        self.handover_ok_of(g.matches.get(mid), None)
    }

    /// py: hostmig.py:788 handover
    pub fn handover(&self, mid: &str, slot: i64) -> bool {
        let mut g = self.inner.lock();
        let (ok, _) = self.handover_ok_of(g.matches.get(mid), Some(slot));
        let Some(m) = g.matches.get_mut(mid) else { return false };
        if !ok || slot < 0 || m.gone.contains(&(slot as usize)) || slot as usize == m.host || slot as usize >= m.players.len() {
            return false;
        }
        let slot = slot as usize;
        let now = self.now();
        let stale = Self::stale_of(m, now).0;
        let roster: BTreeSet<usize> = (0..m.players.len()).filter(|s| !m.gone.contains(s)).collect();
        let need: BTreeSet<usize> = roster.difference(&stale).copied().collect();
        m.ho = Some(Ho { slot, t: now, ready: BTreeSet::new(), epoch: m.epoch, roster, need });
        let left = if stale.is_empty() { String::new() } else { format!("- not polling, left out: {}", set_repr(&stale)) };
        dlog!("HOSTMIG", mid, format!("planned handover host slot {} -> slot {}: preparing (epoch {})", m.host, slot, m.epoch), left);
        true
    }

    pub fn handover_pending(&self, mid: &str) -> bool {
        self.inner.lock().matches.get(mid).is_some_and(|m| m.ho.is_some())
    }

    /// py: hostmig.py:812 _handover_step
    fn handover_step(&self, mid: &str, m: &mut Match) {
        let Some(ho) = m.ho.clone() else { return };
        let now = self.now();
        let alive: BTreeSet<usize> = (0..m.players.len()).filter(|s| !m.gone.contains(s)).collect();
        let why: Option<String> = if m.given_up || m.pending || m.epoch != ho.epoch {
            Some("a real migration took over".into())
        } else if alive.difference(&ho.roster).next().is_some() || ho.need.difference(&alive).next().is_some() {
            Some("roster changed".into())
        } else if !m.silent.is_empty() {
            Some("host link trouble".into())
        } else if now - ho.t > HANDOVER_TIMEOUT {
            Some(format!("not everyone prepared in {} s (ready {} of {})", py::fixed(HANDOVER_TIMEOUT, 0), set_repr(&ho.ready), set_repr(&alive)))
        } else {
            None
        };
        if let Some(why) = why {
            m.ho = None;
            dlog!("HOSTMIG", mid, "planned handover aborted:", why);
            return;
        }
        if alive.intersection(&ho.need).all(|s| ho.ready.contains(s)) {
            let old = m.host;
            m.host = ho.slot;
            m.epoch += 1;
            m.silent.clear();
            m.silent_since = None;
            m.migrated_at = Some(now);
            m.ho = None;
            dlog!(
                "HOSTMIG",
                mid,
                format!("planned handover: host slot {} -> new host slot {}", old, m.host),
                format!("epoch {} alive mask {:04b} (old host stays as a client)", m.epoch, alive_mask(m))
            );
        }
    }

    /// py: hostmig.py:841 _host_lost
    fn host_lost(&self, mid: &str, m: &mut Match, why: &str) {
        m.ho = None;
        let now = self.now();
        let old = m.host;
        let left: Vec<usize> = (0..m.players.len()).filter(|s| !m.gone.contains(s)).collect();
        if left.len() < self.min_left {
            m.given_up = true;
            dlog!("HOSTMIG", mid, format!("host slot {} lost ({}); nobody left to host - match ends", old, why));
            return;
        }
        if !m.pending {
            m.pending = true;
            m.lost = Some(now);
            m.collect_t0 = Some(now);
            m.cand0 = Some(left.iter().filter(|s| now - f_or(&m.seen, **s, -1e9) <= self.alive_window).copied().collect());
            m.collect_logged = false;
        }
        let t0 = m.collect_t0.unwrap_or(now);
        let mut cands: Vec<usize> = left
            .iter()
            .filter(|s| m.cand0.as_ref().is_some_and(|c| c.contains(s)) || f_or(&m.seen, **s, -1e9) >= t0)
            .copied()
            .collect();
        let asked = left.iter().all(|s| f_or(&m.seen, *s, -1e9) >= t0);
        if cands.is_empty() {
            if !m.collect_logged {
                m.collect_logged = true;
                dlog!("HOSTMIG", mid, format!("host slot {} lost ({}); waiting for a player to ask", old, why));
            }
            return;
        }
        if now - t0 < self.collect && !asked {
            if !m.collect_logged {
                m.collect_logged = true;
                dlog!(
                    "HOSTMIG",
                    mid,
                    format!("host slot {} lost ({}); collecting host candidates for {} s ({} of {} so far)", old, why, py::fixed(self.collect, 0), cands.len(), left.len())
                );
            }
            return;
        }
        let fresh: Vec<usize> = cands.iter().filter(|s| now - f_or(&m.seen, **s, -1e9) <= self.alive_window + self.collect).copied().collect();
        if !fresh.is_empty() {
            cands = fresh;
        } else if why == "pending" {
            return;
        }
        let new = self.pick(m, &cands);
        m.host = new;
        m.epoch += 1;
        m.silent.clear();
        m.silent_since = None;
        m.min_silent_logged = false;
        m.lost = None;
        m.migrated_at = Some(now);
        if left.len() == 1 {
            m.alone = true;
        }
        m.pending = false;
        m.collect_t0 = None;
        m.collect_logged = false;
        m.cand0 = None;
        dlog!(
            "HOSTMIG",
            mid,
            format!("host slot {} lost ({}) -> new host slot {}", old, why, new),
            format!("epoch {} alive mask {:04b}", m.epoch, alive_mask(m))
        );
    }

    /// py: hostmig.py:895 _pick
    fn pick(&self, m: &Match, cands_in: &[usize]) -> usize {
        let mut cands: Vec<usize> = cands_in.to_vec();
        let tok = |s: usize| m.players.get(s).map(|x| x.as_str()).unwrap_or("");
        if let Some(pool) = &self.pool {
            if cands.len() > 1 {
                let toks: Vec<String> = cands.iter().map(|c| tok(*c).to_string()).collect();
                if let Some((keep, info)) = pool(&toks) {
                    let sub: Vec<usize> = cands.iter().filter(|c| keep.iter().any(|k| k == tok(**c))).copied().collect();
                    if !sub.is_empty() {
                        cands = sub;
                    }
                    dlog!("HOSTMIG", "region rule:", info);
                }
            }
        }
        let pen = |s: usize| -> i64 {
            match &self.perf {
                Some(f) => f(tok(s)).unwrap_or(0),
                None => 0,
            }
        };
        let mut blind: HashSet<usize> = HashSet::new();
        if let Some(ping) = &self.ping {
            if cands.len() > 2 {
                blind = cands.iter().filter(|c| cands.iter().filter(|o| *o != *c).all(|o| ping(tok(**c), tok(*o)).is_none())).copied().collect();
                if blind.len() + 1 >= cands.len() {
                    blind.clear();
                }
            }
        }
        let key = |s: usize| -> (i64, i64, usize) {
            if let Some(ping) = &self.ping {
                if cands.len() > 1 && !blind.contains(&s) {
                    let p: Vec<Option<i64>> = cands.iter().filter(|o| **o != s && !blind.contains(*o)).map(|o| ping(tok(s), tok(*o))).collect();
                    if !p.is_empty() && p.iter().all(|x| x.is_some()) {
                        let pv: Vec<i64> = p.iter().filter_map(|x| *x).collect();
                        let k = host_key(&pv, pen(s));
                        return (k.0, k.1, s);
                    }
                }
            }
            (1_000_000_000 + pen(s), 1_000_000_000, s)
        };
        let mut best = cands[0];
        let mut bk = key(best);
        for c in &cands[1..] {
            let k = key(*c);
            if k < bk {
                bk = k;
                best = *c;
            }
        }
        best
    }

    /// py: hostmig.py:926 tick
    pub fn tick(&self) {
        let mut g = self.inner.lock();
        let mids: Vec<String> = g.matches.keys().cloned().collect();
        for mid in mids {
            if let Some(m) = g.matches.get_mut(&mid) {
                if m.pending {
                    self.host_lost(&mid, m, "pending");
                } else {
                    self.evaluate(&mid, m);
                }
            }
        }
    }
}

fn reply(m: &Match, state: u8) -> Option<Vec<u8>> {
    let mask = alive_mask(m);
    if m.host > 255 || m.epoch > 255 || m.epoch < 0 || mask > 255 {
        return None;
    }
    Some(vec![1, m.host as u8, m.epoch as u8, mask as u8, state])
}

// ------------------------------------------------------------ planned host re-pick
/// py: hostmig.py:1046 _guard_code
fn guard_code(hold: &str) -> &'static str {
    for (k, c) in [("young", "young"), ("pending", "busy"), ("handover", "busy"), ("loading", "loading"), ("clock", "clock"), ("round ends", "endgame"), ("within", "gap"), ("limit", "limit")] {
        if hold.contains(k) {
            return c;
        }
    }
    "busy"
}

pub type Key = (i64, i64);

/// The `info` argument of check(): rtt per pair, perf penalty and loss per slot, age of the oldest pair sample.
#[derive(Default, Clone)]
pub struct CheckInfo {
    pub rtt: Vec<((i64, i64), Option<i64>)>,
    pub perf: HashMap<i64, i64>,
    pub loss: HashMap<i64, f64>,
    pub age: Option<f64>,
}

/// The capacity override dict of check().
#[derive(Default, Clone)]
pub struct CapOverride {
    pub host_bad: Option<String>,
    pub rank: HashMap<i64, i64>,
    pub max_rank: Option<i64>,
    pub text: HashMap<i64, String>,
    pub saturated: bool,
}

/// What check() decided: ("hold", text) / ("migrate", slot).
#[derive(Debug, Clone, PartialEq)]
pub enum Repick {
    Hold(String),
    Migrate(i64),
}

#[derive(Clone, Debug, PartialEq)]
pub enum Tgt {
    Cap(i64),
    Plain(i64),
}

#[derive(Clone)]
struct Rec {
    next: f64,
    streak: (Option<Tgt>, i64),
    done: i64,
    last: Option<f64>,
    roster: Option<BTreeSet<i64>>,
}

#[derive(Default)]
struct RepickInner {
    recs: IndexMap<String, Rec>,
    last_text: HashMap<String, String>,
    base: HashMap<String, HashMap<(String, String), i64>>,
    d: Dec,
}

#[derive(Default, Clone)]
pub struct Dec {
    kind: &'static str,
    why: String,
    tgt: Option<i64>,
    n: i64,
}

fn dec(kind: &'static str, why: &str, tgt: Option<i64>, n: i64) -> Dec {
    Dec { kind, why: why.to_string(), tgt, n }
}

fn pair_key(a: &str, c: &str) -> (String, String) {
    if a <= c {
        (a.to_string(), c.to_string())
    } else {
        (c.to_string(), a.to_string())
    }
}

/// py: hostmig.py:1056 format_check (one line, no names / IPs)
#[allow(clippy::too_many_arguments)]
pub fn format_check(mid: &str, age: f64, left: Option<f64>, host: i64, scores: &IndexMap<i64, Option<Key>>, info: Option<&CheckInfo>, act: &str, d: &Dec) -> String {
    let empty = CheckInfo::default();
    let info = info.unwrap_or(&empty);
    let mut slots: Vec<i64> = scores.keys().copied().collect();
    slots.sort_by_key(|s| s.to_string());
    let mut cands: Vec<String> = Vec::new();
    for s in slots {
        let k = scores[&s];
        let mut c = match k {
            None => format!("{}:?", s),
            Some(k) => format!("{}:{}/{}", s, k.0, k.1),
        };
        if k.is_some() {
            if let Some(l) = info.loss.get(&s) {
                c.push_str(&format!("/l{}", fmt_g(*l)));
            }
            if let Some(p) = info.perf.get(&s) {
                if *p != 0 {
                    c.push_str(&format!("/p{}", p));
                }
            }
        }
        cands.push(c);
    }
    let mut rt: Vec<&((i64, i64), Option<i64>)> = info.rtt.iter().collect();
    rt.sort_by(|a, b| (a.0 .0.to_string(), a.0 .1.to_string()).cmp(&(b.0 .0.to_string(), b.0 .1.to_string())));
    let pairs: String = rt.iter().filter_map(|((a, b), v)| v.map(|v| format!("{}-{}:{}", a, b, v))).collect::<Vec<_>>().join(" ");
    let why = if d.why.is_empty() { "?" } else { d.why.as_str() };
    let decs = if d.kind == "would" {
        let tgt = d.tgt.unwrap_or(0);
        let gain = match (scores.get(&host).copied().flatten(), scores.get(&tgt).copied().flatten()) {
            (Some(c), Some(n)) => c.0 - n.0,
            _ => 0,
        };
        format!("dec={} tgt={} gain={} n={}", if act == "migrate" { "migrate" } else { "would" }, tgt, gain, d.n)
    } else if d.kind == "guard" && d.tgt.is_some() && why == "total" {
        format!("dec=stay tgt={}", d.tgt.unwrap_or(0))
    } else {
        "dec=stay".to_string()
    };
    format!(
        "m={} age={} left={} host={} cand={} rtt {}{} {} why={}",
        mid,
        age as i64,
        match left {
            None => "?".to_string(),
            Some(l) => (l as i64).to_string(),
        },
        host,
        if cands.is_empty() { "-".to_string() } else { cands.join(" ") },
        if pairs.is_empty() { "-" } else { &pairs },
        match info.age {
            None => String::new(),
            Some(a) => format!(" rttage={}", a as i64),
        },
        decs,
        why
    )
}

/// py: hostmig.py:1089 HostRepick
pub struct HostRepick {
    now: NowFn,
    pub mode: i64,
    inner: Mutex<RepickInner>,
}

impl HostRepick {
    pub fn new(now: NowFn, mode: Option<i64>) -> Self {
        HostRepick { now, mode: mode.unwrap_or(cfg().repick_mode), inner: Mutex::new(RepickInner::default()) }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    pub fn forget(&self, mid: &str) {
        let mut g = self.inner.lock();
        g.recs.shift_remove(mid);
        g.last_text.remove(mid);
        g.base.remove(mid);
    }

    /// The mids of every record the re-pick keeps (recs, last_text, base): the daemon's cleanup of ended matches.
    pub fn known_mids(&self) -> Vec<String> {
        let g = self.inner.lock();
        let mut v: Vec<String> = Vec::new();
        for m in g.recs.keys().chain(g.last_text.keys()).chain(g.base.keys()) {
            if !v.contains(m) {
                v.push(m.clone());
            }
        }
        v
    }

    /// (recs, last_text, base): the sizes the daemon's stats dump counts.
    pub fn sizes(&self) -> (usize, usize, usize) {
        let g = self.inner.lock();
        (g.recs.len(), g.last_text.len(), g.base.len())
    }

    /// py: hostmig.py:1103 observe  (rtt: {(token a, token b): ms or None})
    pub fn observe(&self, mid: &str, rtt: &[((String, String), Option<i64>)]) {
        let mut g = self.inner.lock();
        let b = g.base.entry(mid.to_string()).or_default();
        for ((a, c), v) in rtt {
            if let Some(v) = v {
                if *v != 0 {
                    let k = pair_key(a, c);
                    if b.get(&k).is_none_or(|x| *v < *x) {
                        b.insert(k, *v);
                    }
                }
            }
        }
    }

    /// py: hostmig.py:1111 saturated
    pub fn saturated(&self, mid: &str, host: &str, others: &[String], ping: &dyn Fn(&str, &str) -> Option<i64>) -> (bool, String) {
        let g = self.inner.lock();
        let empty = HashMap::new();
        let b = g.base.get(mid).unwrap_or(&empty);
        let c = cfg();
        let mut hits: Vec<String> = Vec::new();
        for o in others {
            let v = ping(host, o);
            let b0 = b.get(&pair_key(host, o)).copied();
            let (Some(v), Some(b0)) = (v, b0) else { continue };
            if b0 == 0 {
                continue;
            }
            if (v as f64) < b0 as f64 * c.sat_x || v - b0 < c.sat_min_ms {
                return (false, String::new());
            }
            hits.push(format!("{}->{}", b0, v));
        }
        if hits.is_empty() {
            return (false, String::new());
        }
        if others.len() >= 2 {
            let mut calm = false;
            for (i, a) in others.iter().enumerate() {
                for cc in &others[i + 1..] {
                    let v = ping(a, cc);
                    let b0 = b.get(&pair_key(a, cc)).copied();
                    if let (Some(v), Some(b0)) = (v, b0) {
                        if b0 != 0 && ((v as f64) < b0 as f64 * c.sat_x || v - b0 < c.sat_min_ms) {
                            calm = true;
                        }
                    }
                }
            }
            if !calm {
                return (false, String::new());
            }
        }
        (true, format!("pings to the host {} ms", hits.join(", ")))
    }

    fn say(&self, g: &mut RepickInner, mid: &str, text: &str) {
        if g.last_text.get(mid).map(|x| x.as_str()) != Some(text) {
            g.last_text.insert(mid.to_string(), text.to_string());
            dlog!("HOST-REPICK", mid, text);
        }
    }

    /// py: hostmig.py:1139 due
    pub fn due(&self, mid: &str, started_at: f64) -> bool {
        let mut g = self.inner.lock();
        let c = cfg();
        let r = g.recs.entry(mid.to_string()).or_insert_with(|| Rec {
            next: started_at + std::cmp::max(c.repick_min_age, c.repick_interval) as f64,
            streak: (None, 0),
            done: 0,
            last: None,
            roster: None,
        });
        self.now() >= r.next
    }

    /// py: hostmig.py:1143 check. One decision + ONE REPICK-CHECK log line per call.
    #[allow(clippy::too_many_arguments)]
    pub fn check(
        &self,
        mid: &str,
        started_at: f64,
        roster: &BTreeSet<i64>,
        host: i64,
        scores: &IndexMap<i64, Option<Key>>,
        time_left: Option<f64>,
        busy: Option<&str>,
        labels: Option<&HashMap<i64, String>>,
        info: Option<&CheckInfo>,
        cap: Option<&CapOverride>,
    ) -> Repick {
        let mut g = self.inner.lock();
        g.d = dec("?", "", None, 0);
        let r = self.decide(&mut g, mid, started_at, roster, host, scores, time_left, busy, labels, cap);
        let act = if matches!(r, Repick::Migrate(_)) { "migrate" } else { "hold" };
        let d = g.d.clone();
        dlog!("REPICK-CHECK", format_check(mid, self.now() - started_at, time_left, host, scores, info, act, &d));
        r
    }

    /// py: hostmig.py:1155 _decide
    #[allow(clippy::too_many_arguments)]
    fn decide(
        &self,
        g: &mut RepickInner,
        mid: &str,
        started_at: f64,
        roster: &BTreeSet<i64>,
        host: i64,
        scores: &IndexMap<i64, Option<Key>>,
        time_left: Option<f64>,
        busy: Option<&str>,
        labels: Option<&HashMap<i64, String>>,
        cap: Option<&CapOverride>,
    ) -> Repick {
        let c = cfg();
        let now = self.now();
        let lab_of = |p: i64| -> String { labels.and_then(|l| l.get(&p)).cloned().unwrap_or_else(|| p.to_string()) };
        {
            let r = g.recs.entry(mid.to_string()).or_insert_with(|| Rec { next: 0.0, streak: (None, 0), done: 0, last: None, roster: None });
            r.next = now + c.repick_interval as f64;
            if r.roster.is_none() {
                r.roster = Some(roster.clone());
            }
        }
        let (done, last, rroster) = {
            let r = &g.recs[mid];
            (r.done, r.last, r.roster.clone())
        };
        let mut hold: Option<String> = None;
        if now - started_at < c.repick_min_age as f64 {
            hold = Some("match too young".into());
        } else if let Some(b) = busy.filter(|b| !b.is_empty()) {
            hold = Some(b.to_string());
        } else if time_left.is_none() {
            hold = Some("round clock unknown or ended".into());
        } else if time_left.is_some_and(|t| t < c.repick_min_left as f64) {
            hold = Some(format!("round ends in {} s", time_left.unwrap_or(0.0) as i64));
        } else if done >= c.repick_max || (done >= 1 && (Some(roster) == rroster.as_ref() || now - last.unwrap_or(f64::NEG_INFINITY) < c.repick_gap_s as f64)) {
            hold = Some(if done >= c.repick_max || Some(roster) == rroster.as_ref() {
                format!("migration limit reached ({})", done)
            } else {
                format!("within {} s of the last migration", c.repick_gap_s)
            });
        }
        if let Some(hold) = hold {
            if let Some(r) = g.recs.get_mut(mid) {
                r.streak = (None, 0);
            }
            g.d = dec("guard", guard_code(&hold), None, 0);
            self.say(g, mid, &format!("hold: {}", hold));
            return Repick::Hold(hold);
        }
        let cur: Option<Key> = scores.get(&host).copied().flatten();
        let cands: IndexMap<i64, Key> = scores.iter().filter_map(|(p, k)| k.map(|k| (*p, k))).collect();
        if let Some(cap) = cap {
            if let Some(hb) = cap.host_bad.as_deref().filter(|s| !s.is_empty()) {
                let max_rank = cap.max_rank.unwrap_or(1);
                let rank_of = |p: i64| cap.rank.get(&p).copied().unwrap_or(1);
                let alts: Vec<i64> = cands
                    .keys()
                    .copied()
                    .filter(|p| {
                        *p != host
                            && rank_of(*p) <= max_rank
                            && (cur.is_none()
                                || cur.is_some_and(|cu| cands[p].0 <= cu.0 + c.repick_cap_max_worse_ms && cands[p].0 <= std::cmp::max(cu.0, c.repick_cap_limit_ms)))
                    })
                    .collect();
                if !alts.is_empty() {
                    let mut best = alts[0];
                    for p in &alts[1..] {
                        if (rank_of(*p), cands[p]) < (rank_of(best), cands[&best]) {
                            best = *p;
                        }
                    }
                    let n = {
                        let r = g.recs.get_mut(mid).expect("rec");
                        let (tgt, n) = r.streak.clone();
                        let n = if tgt == Some(Tgt::Cap(best)) { n + 1 } else { 1 };
                        r.streak = (Some(Tgt::Cap(best)), n);
                        n
                    };
                    let lab = lab_of(best);
                    let need = if cap.saturated { 1 } else { c.repick_cap_confirm };
                    let txt = cap.text.get(&best).cloned().unwrap_or_else(|| format!("rank {}", rank_of(best)));
                    let text = format!(
                        "host {}: migrate to {} ({}), worst {} -> {} ms, check {} of {}",
                        hb,
                        lab,
                        txt,
                        match cur {
                            None => "?".to_string(),
                            Some(cu) => cu.0.to_string(),
                        },
                        cands[&best].0,
                        n,
                        need
                    );
                    g.d = dec("would", if n < need { "cap" } else { "capgo" }, Some(best), n);
                    self.say(g, mid, &text);
                    if n < need {
                        if let Some(r) = g.recs.get_mut(mid) {
                            r.next = r.next.min(now + c.repick_confirm_gap_s as f64);
                        }
                        return Repick::Hold(text);
                    }
                    return Repick::Migrate(best);
                }
                self.say(g, mid, &format!("host {}, but no candidate with enough upload and a known ping", hb));
            }
        }
        let Some(cur) = cur.filter(|_| !cands.is_empty()) else {
            if let Some(r) = g.recs.get_mut(mid) {
                r.streak = (None, 0);
            }
            g.d = dec("guard", "unknown", None, 0);
            self.say(g, mid, "hold: pings unknown");
            return Repick::Hold("pings unknown".into());
        };
        let pick_min = |m: &IndexMap<i64, Key>| -> i64 {
            let mut best = *m.keys().next().unwrap_or(&host);
            let mut bk = (m[&best], if best == host { 0 } else { 1 });
            for (p, k) in m.iter().skip(1) {
                let kk = (*k, if *p == host { 0 } else { 1 });
                if kk < bk {
                    bk = kk;
                    best = *p;
                }
            }
            best
        };
        let mut best = pick_min(&cands);
        if let Some(cp) = cap {
            if !cp.rank.is_empty() && c.repick_proven_n != 0 && roster.len() as i64 >= c.repick_proven_n {
                // only the current host and targets proven at least as well as it stay in the race: a host with lag records may hand over to an
                // unproven (rank 1) target; a clean proven host still needs a proven one.
                let hr = cp.rank.get(&host).copied().unwrap_or(1).clamp(0, 1);
                let ok: IndexMap<i64, Key> = cands.iter().filter(|(p, _)| **p == host || cp.rank.get(p).copied().unwrap_or(1) <= hr).map(|(p, k)| (*p, *k)).collect();
                if best != host && !ok.contains_key(&best) {
                    let best2 = if !ok.is_empty() { pick_min(&ok) } else { host };
                    let t = cp.text.get(&best).cloned().unwrap_or_else(|| "unproven".to_string());
                    self.say(g, mid, &format!("hold: best ping target {} is not proven for {} players ({})", lab_of(best), roster.len(), t));
                    best = best2;
                }
            }
        }
        if best != host && cands[&best].1 > cur.1 {
            if let Some(r) = g.recs.get_mut(mid) {
                r.streak = (None, 0);
            }
            g.d = dec("guard", "total", Some(best), 0);
            self.say(g, mid, &format!("hold: host is fine (worst {} ms, best other has a higher total {} > {} ms)", cur.0, cands[&best].1, cur.1));
            return Repick::Hold("total worse".into());
        }
        let cb = cands[&best];
        let worst_gain = best != host && cb.0 as f64 <= cur.0 as f64 - c.repick_min_gain_ms as f64 && (cb.0 as f64) <= cur.0 as f64 * (1.0 - c.repick_min_gain_frac);
        let total_gain = best != host
            && cb.0 <= cur.0
            && cur.1 - cb.1 >= std::cmp::max(c.repick_total_gain_ms, c.repick_total_gain_per_ms * (roster.len() as i64 - 1))
            && (cb.1 as f64) <= cur.1 as f64 * (1.0 - c.repick_total_gain_frac);
        if !(worst_gain || total_gain) {
            if let Some(r) = g.recs.get_mut(mid) {
                r.streak = (None, 0);
            }
            g.d = dec("stay", if best == host { "best" } else { "small" }, Some(best), 0);
            let extra = if best == host { String::new() } else { format!(", best other {} ms - not worth it", cb.0) };
            self.say(g, mid, &format!("hold: host is fine (worst {} ms{})", cur.0, extra));
            return Repick::Hold("not worth it".into());
        }
        if roster.len() < 3 {
            if let Some(r) = g.recs.get_mut(mid) {
                r.streak = (None, 0);
            }
            g.d = dec("stay", "small", Some(best), 0);
            return Repick::Hold("2 players".into());
        }
        let n = {
            let r = g.recs.get_mut(mid).expect("rec");
            let (tgt, n) = r.streak.clone();
            let n = if tgt == Some(Tgt::Plain(best)) { n + 1 } else { 1 };
            r.streak = (Some(Tgt::Plain(best)), n);
            n
        };
        let text = format!(
            "would migrate to {}: worst {} -> {} ms (total {} -> {}{}), check {} of {}",
            lab_of(best),
            cur.0,
            cb.0,
            cur.1,
            cb.1,
            if worst_gain { "" } else { ", total rule" },
            n,
            c.repick_confirm
        );
        g.d = dec("would", if n < c.repick_confirm { "confirm" } else { "go" }, Some(best), n);
        self.say(g, mid, &text);
        if n < c.repick_confirm {
            if let Some(r) = g.recs.get_mut(mid) {
                r.next = r.next.min(now + c.repick_confirm_gap_s as f64);
            }
            return Repick::Hold(text);
        }
        Repick::Migrate(best)
    }

    /// py: hostmig.py:1262 migrated
    pub fn migrated(&self, mid: &str, roster: &BTreeSet<i64>) {
        let now = self.now();
        let mut g = self.inner.lock();
        if let Some(r) = g.recs.get_mut(mid) {
            r.done += 1;
            r.last = Some(now);
            r.roster = Some(roster.clone());
            r.streak = (None, 0);
        }
    }

    /// Test hook: recs / last_text / base as JSON (keys sorted).
    #[allow(dead_code)]
    pub fn dump_state(&self) -> Value {
        let g = self.inner.lock();
        let mut recs = Map::new();
        let mut ks: Vec<&String> = g.recs.keys().collect();
        ks.sort();
        for k in ks {
            let r = &g.recs[k];
            let mut o = Map::new();
            o.insert("done".into(), Value::from(r.done));
            o.insert("last".into(), r.last.map(Value::from).unwrap_or(Value::Null));
            o.insert("next".into(), Value::from(r.next));
            o.insert("roster".into(), r.roster.as_ref().map(|s| Value::Array(s.iter().map(|x| Value::from(*x)).collect())).unwrap_or(Value::Null));
            let tg = match &r.streak.0 {
                None => Value::Null,
                Some(Tgt::Cap(b)) => Value::Array(vec![Value::from("cap"), Value::from(*b)]),
                Some(Tgt::Plain(b)) => Value::from(*b),
            };
            o.insert("streak".into(), Value::Array(vec![tg, Value::from(r.streak.1)]));
            recs.insert(k.clone(), Value::Object(o));
        }
        let mut lt = Map::new();
        let mut ks: Vec<&String> = g.last_text.keys().collect();
        ks.sort();
        for k in ks {
            lt.insert(k.clone(), Value::from(g.last_text[k].as_str()));
        }
        let mut base = Map::new();
        let mut ks: Vec<&String> = g.base.keys().collect();
        ks.sort();
        for k in ks {
            let mut v: Vec<(&(String, String), &i64)> = g.base[k].iter().collect();
            v.sort();
            base.insert(
                k.clone(),
                Value::Array(v.iter().map(|(p, x)| Value::Array(vec![Value::Array(vec![Value::from(p.0.as_str()), Value::from(p.1.as_str())]), Value::from(**x)])).collect()),
            );
        }
        let mut o = Map::new();
        o.insert("base".into(), Value::Object(base));
        o.insert("last_text".into(), Value::Object(lt));
        o.insert("recs".into(), Value::Object(recs));
        Value::Object(o)
    }
}

#[allow(dead_code)]
fn _unused(v: &[i64]) -> String {
    i64_set_repr(v)
}
