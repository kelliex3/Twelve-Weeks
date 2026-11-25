//! Per-mode matchmaking queues (op 0xBC / 0xBD): every token has a preference and a match only forms from players of ONE mode.
//! Daemon env T2R_MODE (anything but empty) FORCES the mode of every match: the preferences are then ignored by the queue.
use crate::c_util::{jf, ji, js, jstrs, latin1, script, sys_now, Now};
use crate::gen_modes;
use indexmap::IndexMap;
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};
use std::sync::OnceLock;

pub const OP_REQ: u8 = 0xBC;
pub const OP_REPLY: u8 = 0xBD;
/// wire value of QUICK MATCH (any mode)
pub const ANY: i64 = 255;
/// a picked mode is forgotten after an hour without a new pick
pub const PREF_TTL: f64 = 3600.0;

static MODES: OnceLock<Vec<String>> = OnceLock::new();

/// py: modeq.py:25 MODES (tools/modes.json keys, else the two built-in modes).
pub fn modes() -> &'static [String] {
    MODES.get_or_init(|| match gen_modes::load() {
        Ok(v) => v.into_iter().map(|e| e.key).collect(),
        Err(_) => vec!["supply-raid".to_string(), "interrogation".to_string()],
    })
}

/// py: modeq.py:29 NMODES
pub fn nmodes() -> usize {
    modes().len()
}

/// py: modeq.py:34 forced
pub fn forced() -> bool {
    !crate::env_str("T2R_MODE", "").trim().is_empty()
}

struct Inner {
    pref: HashMap<String, (i64, f64)>,
    mmode: IndexMap<String, String>,
    rr: i64,
}

// py: modeq.py:39 class ModeQ
pub struct ModeQ {
    now: Now,
    inner: Mutex<Inner>,
}

impl ModeQ {
    pub fn new() -> ModeQ {
        ModeQ::with_clock(sys_now())
    }

    pub fn with_clock(now: Now) -> ModeQ {
        ModeQ { now, inner: Mutex::new(Inner { pref: HashMap::new(), mmode: IndexMap::new(), rr: 0 }) }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    /// `MODEQ.pref.get(token)` (the daemon reads it under MODEQ.lock).
    pub fn pref_of(&self, token: &str) -> Option<(i64, f64)> {
        self.inner.lock().pref.get(token).copied()
    }

    /// `MODEQ.now()`
    pub fn clock(&self) -> f64 {
        self.now()
    }

    // py: modeq.py:46 set
    pub fn set(&self, token: &str, mode: i64) -> bool {
        if !((0..nmodes() as i64).contains(&mode) || mode == ANY) {
            return false;
        }
        let t = self.now();
        self.inner.lock().pref.insert(token.to_string(), (mode, t));
        true
    }

    fn get_locked(&self, g: &Inner, token: &str, leader: Option<&str>) -> i64 {
        let key = match leader {
            Some(l) if !l.is_empty() => l,
            _ => token,
        };
        let e = g.pref.get(key).or_else(|| g.pref.get(token));
        if let Some(e) = e {
            if self.now() - e.1 <= PREF_TTL {
                return e.0;
            }
        }
        0
    }

    // py: modeq.py:53 get
    pub fn get(&self, token: &str, leader: Option<&str>) -> i64 {
        let g = self.inner.lock();
        self.get_locked(&g, token, leader)
    }

    // py: modeq.py:60 group
    pub fn group(&self, ready: &[String], leader_of: &dyn Fn(&str) -> Option<String>) -> (Vec<String>, Option<String>) {
        if forced() || ready.is_empty() {
            return (ready.to_vec(), None);
        }
        let n = nmodes();
        let ms = modes();
        let mut by: Vec<Vec<String>> = vec![Vec::new(); n];
        let mut anys: Vec<String> = Vec::new();
        for t in ready {
            let l = leader_of(t);
            let m = self.get(t, l.as_deref());
            if m == ANY {
                anys.push(t.clone());
            } else if let Some(v) = by.get_mut(m as usize) {
                v.push(t.clone());
            }
        }
        let idx = |t: &String| ready.iter().position(|x| x == t).unwrap_or(0);
        if anys.is_empty() {
            let mut order: Vec<usize> = (0..n).filter(|m| !by[*m].is_empty()).collect();
            order.sort_by_key(|m| idx(&by[*m][0]));
            let Some(&first) = order.first() else { return (Vec::new(), None) };
            let pick = order.iter().copied().find(|m| by[*m].len() >= 2).unwrap_or(first);
            return (by[pick].clone(), Some(ms[pick].clone()));
        }
        // QUICK MATCH searchers fit every lobby
        let key = |m: usize| -> (i64, usize) { (-(by[m].len() as i64), if by[m].is_empty() { ready.len() } else { idx(&by[m][0]) }) };
        let live: Vec<usize> = (0..n).filter(|m| by[*m].len() + anys.len() >= 2).collect();
        let pick: usize;
        if !live.is_empty() && live.iter().all(|m| by[*m].is_empty()) {
            let mut g = self.inner.lock();
            pick = (g.rr.rem_euclid(n as i64)) as usize;
            g.rr += 1;
        } else {
            let cands: Vec<usize> = if live.is_empty() { (0..n).collect() } else { live };
            let mut best: Option<usize> = None;
            for m in cands {
                if best.map(|b| key(m) < key(b)).unwrap_or(true) {
                    best = Some(m);
                }
            }
            pick = best.unwrap_or(0);
        }
        let mut grp: HashSet<&String> = by[pick].iter().collect();
        grp.extend(anys.iter());
        (ready.iter().filter(|t| grp.contains(t)).cloned().collect(), Some(ms[pick].clone()))
    }

    // py: modeq.py:90 fits
    pub fn fits(&self, token: &str, mid: &str, leader: Option<&str>) -> bool {
        if forced() {
            return true;
        }
        let g = self.inner.lock();
        let mm = g.mmode.get(mid).cloned();
        let gm = self.get_locked(&g, token, leader);
        match mm {
            None => true,
            Some(mm) => gm == ANY || modes().get(gm as usize).map(|m| *m == mm).unwrap_or(false),
        }
    }

    // py: modeq.py:99 counts
    pub fn counts(&self, queue_tokens: &[String], leader_of: &dyn Fn(&str) -> Option<String>) -> Vec<i64> {
        let mut c = vec![0i64; nmodes()];
        for t in queue_tokens {
            let l = leader_of(t);
            let m = self.get(t, l.as_deref());
            if m == ANY {
                for n in c.iter_mut() {
                    *n += 1;
                }
            } else if let Some(n) = c.get_mut(m as usize) {
                *n += 1;
            }
        }
        c
    }

    // py: modeq.py:108 total
    pub fn total(&self, queue_tokens: &[String]) -> usize {
        queue_tokens.len()
    }

    // py: modeq.py:112 compat
    pub fn compat(a: i64, b: i64) -> bool {
        a == b || a == ANY || b == ANY
    }

    // py: modeq.py:116 formed
    pub fn formed(&self, mid: &str, mode: Option<&str>) {
        if let Some(mode) = mode.filter(|m| !m.is_empty()) {
            let mut g = self.inner.lock();
            g.mmode.insert(mid.to_string(), mode.to_string());
            while g.mmode.len() > 64 {
                g.mmode.shift_remove_index(0);
            }
        }
    }

    // py: modeq.py:123 match_mode
    pub fn match_mode(&self, mid: &str) -> Option<String> {
        if forced() {
            return None;
        }
        self.inner.lock().mmode.get(mid).cloned()
    }

    // py: modeq.py:129 parse
    pub fn parse(pl: &[u8]) -> Option<(String, u8, u8)> {
        if pl.len() < 65 {
            return None;
        }
        let tok = latin1(&pl[..64]);
        Some((tok, pl[64], if pl.len() > 65 { pl[65] } else { 0 }))
    }

    // py: modeq.py:136 reply
    pub fn reply(q: &[i64], mode: i64, is_forced: bool, total: Option<i64>) -> String {
        let mode = if mode == ANY { nmodes() as i64 } else { mode };
        let qs: Vec<String> = q.iter().enumerate().map(|(i, n)| format!("q{}={}", i, n)).collect();
        format!("{} mode={} forced={} any=1 qa={}", qs.join(" "), mode, if is_forced { 1 } else { 0 }, total.unwrap_or_else(|| q.iter().sum()))
    }
}

impl Default for ModeQ {
    fn default() -> Self {
        ModeQ::new()
    }
}

// ------------------------------------------------------------------ differential test (verify/py/modeq_harness.py)

fn state(m: &ModeQ) -> Value {
    let g = m.inner.lock();
    let pref: std::collections::BTreeMap<&String, Value> = g.pref.iter().map(|(k, v)| (k, json!([v.0, v.1]))).collect();
    let mm: Vec<Value> = g.mmode.iter().map(|(k, v)| json!([k, v])).collect();
    json!({"pref": pref, "mmode": mm, "rr": g.rr})
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let clk = crate::c_util::Frozen::new();
    let m = ModeQ::with_clock(clk.now());
    for (i, c) in script(input).iter().enumerate() {
        clk.set(jf(c, "t"));
        // leaders: {token: leader}
        let leaders: HashMap<String, String> =
            c.get("leaders").and_then(|x| x.as_object()).map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string())).collect()).unwrap_or_default();
        let leader_of = |t: &str| leaders.get(t).cloned();
        let tok = js(c, "token");
        let ready = jstrs(c, "ready");
        let leader = c.get("leader").and_then(|x| x.as_str());
        let r: Value = match js(c, "op") {
            "set" => json!(m.set(tok, ji(c, "mode"))),
            "get" => json!(m.get(tok, leader)),
            "group" => {
                let (g, name) = m.group(&ready, &leader_of);
                json!([g, name])
            }
            "fits" => json!(m.fits(tok, js(c, "mid"), leader)),
            "counts" => json!(m.counts(&ready, &leader_of)),
            "formed" => {
                m.formed(js(c, "mid"), c.get("mode").and_then(|x| x.as_str()));
                Value::Null
            }
            "match_mode" => json!(m.match_mode(js(c, "mid"))),
            "parse" => {
                let pl: Vec<u8> = c.get("pl").and_then(|x| x.as_array()).map(|a| a.iter().map(|v| v.as_u64().unwrap_or(0) as u8).collect()).unwrap_or_default();
                json!(ModeQ::parse(&pl).map(|(a, b, c)| json!([a, b, c])))
            }
            "reply" => {
                let q: Vec<i64> = c.get("q").and_then(|x| x.as_array()).map(|a| a.iter().map(|v| v.as_i64().unwrap_or(0)).collect()).unwrap_or_default();
                let total = c.get("total").and_then(|x| x.as_i64());
                json!(ModeQ::reply(&q, ji(c, "mode"), c.get("forced").and_then(|x| x.as_bool()).unwrap_or(false), total))
            }
            "compat" => json!(ModeQ::compat(ji(c, "a"), ji(c, "b"))),
            "info" => json!({"modes": modes(), "n": nmodes(), "forced": forced()}),
            "header" => json!(gen_modes::header_from(&gen_modes::tools_dir()).ok()),
            _ => Value::Null,
        };
        crate::c_util::emit(out, i, r, state(&m));
    }
    0
}
