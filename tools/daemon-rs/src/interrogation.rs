//! Interrogation (TLOU 2013 Factions game mode), daemon side: the server is the authority for the intel / lockbox counters, the clients
//! only report their own events (op 0xDC / 0xDD, t2r-ext's own short TCP connection). Pure logic, no sockets.
use crate::c_util::{float_floordiv, jf, ji, js, jstrs, latin1, script, sys_now, Now};
use crate::{dlog, py};
use indexmap::IndexMap;
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::io::{BufRead, Write};
use t2r_clanstory::pyrand::PyRandom;

pub const OP_REQ: u8 = 0xDC;
pub const OP_REPLY: u8 = 0xDD;
pub const SUB_POLL: i64 = 0;
pub const SUB_INTERROGATE: i64 = 1;
pub const SUB_STAGE: i64 = 2;
pub const SUB_DIED: i64 = 3;
pub const MODES: [&str; 2] = ["supply-raid", "interrogation"];
pub const STAGES: i64 = 3;

/// py: interrogation.py:44 load_lockboxes: map index -> (candidates team 0, candidates team 1). Empty on any read / JSON error.
pub fn load_lockboxes(path: &std::path::Path) -> HashMap<i64, (i64, i64)> {
    let mut out = HashMap::new();
    let Ok(b) = std::fs::read(path) else { return out };
    let Ok(v) = serde_json::from_slice::<Value>(&b) else { return out };
    let Some(arr) = v.as_array() else { return out };
    for e in arr {
        let (Some(m), Some(teams)) = (e.get("map").and_then(|x| x.as_i64()), e.get("teams").and_then(|x| x.as_array())) else { return HashMap::new() };
        let n = |i: usize| teams.get(i).map(|t| t.as_array().map(|a| a.len()).unwrap_or(0).max(1) as i64).unwrap_or(1);
        out.insert(m, (n(0), n(1)));
    }
    out
}

/// py: the default `lockboxes=None` -> load_lockboxes() next to the Python modules.
pub fn default_lockboxes() -> HashMap<i64, (i64, i64)> {
    load_lockboxes(&crate::gen_modes::tools_dir().join("interrogation_lockboxes.json"))
}

/// One match record (py: the dict `start()` builds; it is also exported as JSON for the daemon restart).
#[derive(Clone, Debug)]
pub struct Rec {
    pub players: Vec<String>,
    pub team: IndexMap<String, i64>,
    pub map: i64,
    pub t0: f64,
    pub intel: [i64; 2],
    pub kills: [i64; 2],
    pub stage: [i64; 2],
    pub stage_at: [f64; 2],
    pub boxes: [i64; 2],
    pub win: i64,
    pub ended: Option<f64>,
    pub seq: IndexMap<String, i64>,
    /// token -> (percent, time): created by the first poll that carries a percentage
    pub hold: Option<IndexMap<String, (i64, f64)>>,
    pub full_at: Option<[Option<f64>; 2]>,
    /// the order in which "hold" / "full_at" were added to the dict (visible in the JSON export)
    extras: Vec<&'static str>,
}

fn ix(team: i64) -> usize {
    team.rem_euclid(2) as usize
}

fn arr2i(v: Option<&Value>) -> Option<[i64; 2]> {
    let a = v?.as_array()?;
    Some([a.first()?.as_i64()?, a.get(1)?.as_i64()?])
}

fn arr2f(v: Option<&Value>) -> Option<[f64; 2]> {
    let a = v?.as_array()?;
    Some([a.first()?.as_f64()?, a.get(1)?.as_f64()?])
}

impl Rec {
    /// `json.loads(json.dumps(record, default=str))`
    pub fn to_json(&self) -> Value {
        let mut m = serde_json::Map::new();
        m.insert("players".into(), json!(self.players));
        let team: serde_json::Map<String, Value> = self.team.iter().map(|(k, v)| (k.clone(), json!(v))).collect();
        m.insert("team".into(), Value::Object(team));
        m.insert("map".into(), json!(self.map));
        m.insert("t0".into(), json!(self.t0));
        m.insert("intel".into(), json!(self.intel));
        m.insert("kills".into(), json!(self.kills));
        m.insert("stage".into(), json!(self.stage));
        m.insert("stage_at".into(), json!(self.stage_at));
        m.insert("box".into(), json!(self.boxes));
        m.insert("win".into(), json!(self.win));
        m.insert("ended".into(), json!(self.ended));
        let seq: serde_json::Map<String, Value> = self.seq.iter().map(|(k, v)| (k.clone(), json!(v))).collect();
        m.insert("seq".into(), Value::Object(seq));
        for x in &self.extras {
            if *x == "hold" {
                if let Some(h) = &self.hold {
                    let o: serde_json::Map<String, Value> = h.iter().map(|(k, v)| (k.clone(), json!([v.0, v.1]))).collect();
                    m.insert("hold".into(), Value::Object(o));
                }
            } else if let Some(f) = &self.full_at {
                m.insert("full_at".into(), json!(f));
            }
        }
        Value::Object(m)
    }

    /// The record restored by the daemon after a restart (None when it is not a record this module could have written).
    pub fn from_json(v: &Value) -> Option<Rec> {
        let o = v.as_object()?;
        let players: Vec<String> = o.get("players")?.as_array()?.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect();
        let team: IndexMap<String, i64> = o.get("team")?.as_object()?.iter().map(|(k, x)| (k.clone(), x.as_i64().unwrap_or(0))).collect();
        let seq: IndexMap<String, i64> = o.get("seq").and_then(|s| s.as_object()).map(|s| s.iter().map(|(k, x)| (k.clone(), x.as_i64().unwrap_or(0))).collect()).unwrap_or_default();
        let mut r = Rec {
            players,
            team,
            map: o.get("map")?.as_i64()?,
            t0: o.get("t0")?.as_f64()?,
            intel: arr2i(o.get("intel"))?,
            kills: arr2i(o.get("kills"))?,
            stage: arr2i(o.get("stage"))?,
            stage_at: arr2f(o.get("stage_at"))?,
            boxes: arr2i(o.get("box"))?,
            win: o.get("win")?.as_i64()?,
            ended: o.get("ended").and_then(|x| x.as_f64()),
            seq,
            hold: None,
            full_at: None,
            extras: Vec::new(),
        };
        for (k, x) in o {
            if k == "hold" {
                let h: IndexMap<String, (i64, f64)> = x
                    .as_object()?
                    .iter()
                    .map(|(t, e)| {
                        let a = e.as_array();
                        (t.clone(), (a.and_then(|a| a.first()).and_then(|v| v.as_i64()).unwrap_or(0), a.and_then(|a| a.get(1)).and_then(|v| v.as_f64()).unwrap_or(0.0)))
                    })
                    .collect();
                r.hold = Some(h);
                r.extras.push("hold");
            } else if k == "full_at" {
                let a = x.as_array()?;
                r.full_at = Some([a.first().and_then(|v| v.as_f64()), a.get(1).and_then(|v| v.as_f64())]);
                r.extras.push("full_at");
            }
        }
        Some(r)
    }
}

struct Inner {
    matches: IndexMap<String, Rec>,
    by_token: HashMap<String, String>,
    rot: i64,
}

/// Constructor arguments (py: `Interrogation.__init__` keywords; None = the env var / default).
#[derive(Clone, Debug)]
pub struct Params {
    pub mode: Option<String>,
    pub need: Option<i64>,
    pub kills_per_info: Option<i64>,
    pub stage_secs: f64,
    pub boost_after: f64,
    pub boost_every: f64,
    pub boost_step: f64,
    pub boost_max: i64,
    pub locate_secs: f64,
    pub keep: f64,
}

impl Default for Params {
    fn default() -> Params {
        Params {
            mode: None,
            need: None,
            kills_per_info: None,
            stage_secs: 18.0,
            boost_after: 720.0,
            boost_every: 90.0,
            boost_step: 0.12,
            boost_max: 4,
            locate_secs: 20.0,
            keep: 30.0,
        }
    }
}

// py: interrogation.py:52 class Interrogation
pub struct Interrogation {
    pub mode_env: String,
    pub need: i64,
    pub kpi: i64,
    now: Now,
    rng: Mutex<PyRandom>,
    pub lockboxes: HashMap<i64, (i64, i64)>,
    pub stage_secs: f64,
    pub boost_after: f64,
    pub boost_every: f64,
    pub boost_step: f64,
    pub boost_max: i64,
    pub locate_secs: f64,
    pub keep: f64,
    inner: Mutex<Inner>,
}

impl Interrogation {
    /// (len(matches), len(by_token)): the admin /sizes counts
    pub fn sizes(&self) -> (usize, usize) {
        let g = self.inner.lock();
        (g.matches.len(), g.by_token.len())
    }

    /// `Interrogation(log=log, now=time.time)`: env T2R_MODE, T2R_INT_NEED, T2R_INT_KILLS_PER_INFO, T2R_INT_LOCATE_SECS; the lockbox table is
    /// read from `interrogation_lockboxes.json` in the tools directory (see gen_modes::tools_dir).
    pub fn new() -> Interrogation {
        Interrogation::with(Params::default(), sys_now(), crate::rnd::unseeded(), None)
    }

    /// The general constructor. `lockboxes` None = `load_lockboxes()`.
    pub fn with(p: Params, now: Now, rng: PyRandom, lockboxes: Option<HashMap<i64, (i64, i64)>>) -> Interrogation {
        let mode_env = match &p.mode {
            Some(m) if !m.is_empty() => m.clone(),
            _ => crate::env_str("T2R_MODE", "supply-raid"),
        };
        let mode_env = py::strip(&mode_env).to_lowercase();
        Interrogation {
            mode_env,
            need: p.need.unwrap_or_else(|| crate::env_parse("T2R_INT_NEED", 5i64)),
            kpi: p.kills_per_info.unwrap_or_else(|| crate::env_parse("T2R_INT_KILLS_PER_INFO", 4i64)),
            now,
            rng: Mutex::new(rng),
            lockboxes: lockboxes.unwrap_or_else(default_lockboxes),
            stage_secs: p.stage_secs,
            boost_after: p.boost_after,
            boost_every: p.boost_every,
            boost_step: p.boost_step,
            boost_max: p.boost_max,
            locate_secs: crate::env_parse("T2R_INT_LOCATE_SECS", p.locate_secs),
            keep: p.keep,
            inner: Mutex::new(Inner { matches: IndexMap::new(), by_token: HashMap::new(), rot: 0 }),
        }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    // ------------------------------------------------------------ mode select
    // py: interrogation.py:84 pick_mode
    pub fn pick_mode(&self) -> &'static str {
        let m = self.mode_env.as_str();
        if m == "int" || m == "interrogation" {
            return "interrogation";
        }
        if m == "rotate" {
            let mut g = self.inner.lock();
            g.rot += 1;
            return MODES[(g.rot % 2) as usize];
        }
        "supply-raid"
    }

    // ------------------------------------------------------------ match lifecycle
    // py: interrogation.py:95 start
    pub fn start(&self, mid: &str, players: &[String], map_idx: i64, team_of: &dyn Fn(usize) -> i64, mode: Option<&str>) -> String {
        let mode: String = match mode {
            Some(m) if !m.is_empty() => m.to_string(),
            _ => self.pick_mode().to_string(),
        };
        let rec;
        {
            let mut g = self.inner.lock();
            Self::drop_locked(&mut g, mid);
            if mode != "interrogation" {
                return mode;
            }
            if !self.lockboxes.is_empty() && !self.lockboxes.contains_key(&map_idx) {
                dlog!("INTERROGATION: map", map_idx, "has no lockboxes - Supply Raid match");
                return "supply-raid".to_string();
            }
            let nb = self.lockboxes.get(&map_idx).copied().unwrap_or((1, 1));
            let mut rng = self.rng.lock();
            let b0 = rng.randrange_to(nb.0).unwrap_or(0);
            let b1 = rng.randrange_to(nb.1).unwrap_or(0);
            drop(rng);
            rec = Rec {
                players: players.to_vec(),
                team: players.iter().enumerate().map(|(i, t)| (t.clone(), team_of(i))).collect(),
                map: map_idx,
                t0: self.now(),
                intel: [0, 0],
                kills: [0, 0],
                stage: [0, 0],
                stage_at: [0.0, 0.0],
                boxes: [b0, b1],
                win: -1,
                ended: None,
                seq: IndexMap::new(),
                hold: None,
                full_at: None,
                extras: Vec::new(),
            };
            g.matches.insert(mid.to_string(), rec.clone());
            for t in players {
                g.by_token.insert(t.clone(), mid.to_string());
            }
        }
        dlog!("INTERROGATION start", mid, "map", map_idx, "lockboxes", py::list_int(&rec.boxes), "need", self.need, "kpi", self.kpi);
        mode
    }

    // py: interrogation.py:119 roster
    pub fn roster(&self, mid: &str, players: &[String], team_of: &dyn Fn(usize) -> i64) {
        {
            let mut g = self.inner.lock();
            let Some(r) = g.matches.get_mut(mid) else { return };
            r.players = players.to_vec();
            for (i, t) in players.iter().enumerate() {
                r.team.insert(t.clone(), team_of(i));
            }
            for t in players {
                g.by_token.insert(t.clone(), mid.to_string());
            }
        }
        dlog!("INTERROGATION roster", mid, "now", players.len(), "slots");
    }

    // py: interrogation.py:132 end
    pub fn end(&self, mid: &str, winner: Option<i64>) {
        let mut g = self.inner.lock();
        let now = self.now();
        if let Some(r) = g.matches.get_mut(mid) {
            if r.ended.is_none() {
                r.ended = Some(now);
                if r.win < 0 && (winner == Some(0) || winner == Some(1)) {
                    r.win = winner.unwrap_or(-1);
                }
                dlog!("INTERROGATION end", mid, "winner", r.win, "intel", py::list_int(&r.intel), "stages", py::list_int(&r.stage));
            }
        }
    }

    // py: interrogation.py:142 drop
    pub fn drop(&self, mid: &str) {
        let mut g = self.inner.lock();
        Self::drop_locked(&mut g, mid);
    }

    // py: interrogation.py:146 _drop
    fn drop_locked(g: &mut Inner, mid: &str) {
        if let Some(r) = g.matches.shift_remove(mid) {
            for t in &r.players {
                if g.by_token.get(t).map(|s| s.as_str()) == Some(mid) {
                    g.by_token.remove(t);
                }
            }
        }
    }

    // py: interrogation.py:152 _rec (lock held): the record of the token's match, None when there is none / it ended too long ago
    fn rec_mid(&self, g: &mut Inner, token: &str) -> Option<String> {
        let mid = g.by_token.get(token)?.clone();
        let r = g.matches.get(&mid)?;
        if let Some(e) = r.ended {
            if self.now() - e > self.keep {
                Self::drop_locked(g, &mid);
                return None;
            }
        }
        Some(mid)
    }

    // ------------------------------------------------------------ rules
    // py: interrogation.py:162 progress
    pub fn progress(&self, r: &Rec, team: i64) -> i64 {
        let t = ix(team);
        r.intel[t] + if self.kpi > 0 { py::floordiv(r.kills[t], self.kpi) } else { 0 }
    }

    // py: interrogation.py:165 intel_full
    fn intel_full(&self, r: &mut Rec, team: i64) -> bool {
        if self.progress(r, team) < self.need {
            return false;
        }
        if r.full_at.is_none() {
            r.full_at = Some([None, None]);
            r.extras.push("full_at");
        }
        let now = self.now();
        if let Some(f) = r.full_at.as_mut() {
            if f[ix(team)].is_none() {
                f[ix(team)] = Some(now);
            }
        }
        true
    }

    // py: interrogation.py:172 revealed
    fn revealed(&self, r: &mut Rec, team: i64) -> bool {
        if !self.intel_full(r, team) {
            return false;
        }
        let at = r.full_at.and_then(|f| f[ix(team)]).unwrap_or(0.0);
        self.now() - at >= self.locate_secs
    }

    // py: interrogation.py:176 speed_level
    fn speed_level(&self, r: &Rec) -> i64 {
        let el = self.now() - r.t0;
        if el < self.boost_after || self.boost_every <= 0.0 {
            return 0;
        }
        self.boost_max.min(float_floordiv(el - self.boost_after, self.boost_every) as i64 + 1)
    }

    // py: interrogation.py:182 hold_secs
    fn hold_secs(&self, r: &Rec) -> f64 {
        py::round_n(self.stage_secs / (1.0 + self.boost_step * self.speed_level(r) as f64), 2)
    }

    // py: interrogation.py:185 is_new
    fn is_new(r: &mut Rec, token: &str, seq: i64) -> bool {
        if seq == 0 {
            return true;
        }
        if let Some(last) = r.seq.get(token) {
            let d = (seq - last) & 0xFFFF;
            if !(0 < d && d < 0x8000) {
                return false;
            }
        }
        r.seq.insert(token.to_string(), seq);
        true
    }

    // py: interrogation.py:194 event: one request from a client. Returns the reply text.
    pub fn event(&self, token: &str, sub: i64, seq: i64, args: &[u8]) -> String {
        let mut g = self.inner.lock();
        let mid = self.rec_mid(&mut g, token);
        let Some(mid) = mid else { return self.text(None, token) };
        let now = self.now();
        if let Some(r) = g.matches.get_mut(&mid) {
            if sub == SUB_POLL && !args.is_empty() && r.ended.is_none() && r.win < 0 {
                if r.hold.is_none() {
                    r.hold = Some(IndexMap::new());
                    r.extras.push("hold");
                }
                if let Some(h) = r.hold.as_mut() {
                    h.insert(token.to_string(), ((args[0] as i64).min(100), now));
                }
            }
            if sub != SUB_POLL && r.ended.is_none() && r.win < 0 && Self::is_new(r, token, seq) {
                self.apply(r, token, sub, args);
            }
            return self.text(Some(r), token);
        }
        self.text(None, token)
    }

    // py: interrogation.py:203 _apply
    fn apply(&self, r: &mut Rec, token: &str, sub: i64, args: &[u8]) {
        let team = r.team.get(token).copied().unwrap_or(0);
        let tx = ix(team);
        if sub == SUB_INTERROGATE && !args.is_empty() {
            let victim = r.players.get(args[0] as usize).cloned();
            let bad = match &victim {
                None => true,
                Some(v) => r.team.get(v).copied() == Some(team),
            };
            if bad {
                dlog!(format!("INTERROGATION refused (no enemy in slot {})", args[0]));
                return;
            }
            let before = self.intel_full(r, team);
            r.intel[tx] += 1;
            dlog!("INTERROGATION team", team, "intel", r.intel[tx], "progress", self.progress(r, team), "/", self.need);
            if !before && self.intel_full(r, team) {
                dlog!("INTERROGATION team", team, "has all intel - lockbox located in", py::f(self.locate_secs), "s");
            }
        } else if sub == SUB_DIED {
            let enemy = 1 - team;
            let before = self.intel_full(r, enemy);
            r.kills[ix(enemy)] += 1;
            if !before && self.intel_full(r, enemy) {
                dlog!("INTERROGATION team", enemy, "has all intel (kills) - lockbox located in", py::f(self.locate_secs), "s");
            }
        } else if sub == SUB_STAGE && !args.is_empty() {
            let stage = args[0] as i64;
            if !self.revealed(r, team) {
                dlog!(format!("INTERROGATION stage refused (team {} has no intel)", team));
                return;
            }
            if stage != r.stage[tx] + 1 || stage > STAGES {
                return;
            }
            r.stage[tx] = stage;
            r.stage_at[tx] = self.now();
            if let Some(h) = r.hold.as_mut() {
                let gone: Vec<String> = h.keys().filter(|t| r.team.get(*t).copied() == Some(team)).cloned().collect();
                for t in gone {
                    h.shift_remove(&t);
                }
            }
            dlog!("INTERROGATION team", team, "lockbox stage", stage, "/", STAGES);
            if stage == STAGES {
                r.win = team;
                dlog!("INTERROGATION team", team, "stole the lockbox - wins");
            }
        }
    }

    // py: interrogation.py:241 live_hold
    fn live_hold(&self, r: &Rec, team: i64) -> i64 {
        let now = self.now();
        let mut best = 0;
        if let Some(h) = &r.hold {
            for (t, (pct, at)) in h {
                if r.team.get(t).copied() == Some(team) && now - at < 3.0 && r.stage[ix(team)] < STAGES {
                    best = best.max(*pct);
                }
            }
        }
        best
    }

    // py: interrogation.py:250 _text
    fn text(&self, r: Option<&mut Rec>, token: &str) -> String {
        let Some(r) = r else { return "on=0 mode=supply-raid".to_string() };
        let team = r.team.get(token).copied().unwrap_or(0);
        let p = [self.progress(r, 0).min(99), self.progress(r, 1).min(99)];
        let rev0 = self.revealed(r, 0) as i64;
        let rev1 = self.revealed(r, 1) as i64;
        let f: Vec<(&str, String)> = vec![
            ("on", "1".into()),
            ("mode", "interrogation".into()),
            ("map", r.map.to_string()),
            ("team", team.to_string()),
            ("need", self.need.to_string()),
            ("kpi", self.kpi.to_string()),
            ("i0", r.intel[0].to_string()),
            ("i1", r.intel[1].to_string()),
            ("k0", r.kills[0].to_string()),
            ("k1", r.kills[1].to_string()),
            ("p0", p[0].to_string()),
            ("p1", p[1].to_string()),
            ("rev0", rev0.to_string()),
            ("rev1", rev1.to_string()),
            ("box0", r.boxes[0].to_string()),
            ("box1", r.boxes[1].to_string()),
            ("st0", r.stage[0].to_string()),
            ("st1", r.stage[1].to_string()),
            ("hold", py::f(self.hold_secs(r))),
            ("sp", self.speed_level(r).to_string()),
            ("h0", self.live_hold(r, 0).to_string()),
            ("h1", self.live_hold(r, 1).to_string()),
            ("win", r.win.to_string()),
            ("ended", (r.ended.is_some() as i64).to_string()),
            ("seq", r.seq.get(token).copied().unwrap_or(0).to_string()),
        ];
        f.iter().map(|(k, v)| format!("{}={}", k, v)).collect::<Vec<_>>().join(" ")
    }

    // py: interrogation.py:265 parse_request: 0xDC payload -> (token, sub, seq, args)
    pub fn parse_request(&self, pl: &[u8]) -> Option<(String, i64, i64, Vec<u8>)> {
        if pl.len() < 65 {
            return None;
        }
        let token = latin1(&pl[..64]);
        if !token.chars().all(|c| c.is_ascii_hexdigit()) {
            return None;
        }
        let sub = pl[64] as i64;
        let seq = if pl.len() >= 67 { pl[65] as i64 | ((pl[66] as i64) << 8) } else { 0 };
        Some((token, sub, seq, pl.get(67..).map(|s| s.to_vec()).unwrap_or_default()))
    }

    // ---- what the daemon reads from `INT.matches` / `INT.by_token` / `INT.lock` directly
    /// `mid in INT.matches`
    pub fn has_match(&self, mid: &str) -> bool {
        self.inner.lock().matches.contains_key(mid)
    }

    /// `INT.by_token.get(token)`
    pub fn by_token_get(&self, token: &str) -> Option<String> {
        self.inner.lock().by_token.get(token).cloned()
    }

    /// `json.loads(json.dumps(INT.matches.get(mid), default=str))` for the daemon's restart record (None = no record).
    pub fn export(&self, mid: &str) -> Option<Value> {
        self.inner.lock().matches.get(mid).map(|r| r.to_json())
    }

    /// `INT.matches[mid] = rec; INT.by_token[t] = mid for t in rec["players"]` (restart restore). False when the record is unusable.
    pub fn import(&self, mid: &str, rec: &Value) -> bool {
        let Some(r) = Rec::from_json(rec) else { return false };
        let mut g = self.inner.lock();
        for t in &r.players {
            g.by_token.insert(t.clone(), mid.to_string());
        }
        g.matches.insert(mid.to_string(), r);
        true
    }

    /// daemon.py int_rebind (after `cur = MM.mid_of(token)` was found): INT.by_token follows the token's current match. `tag` = the
    /// daemon's log label of the token.
    pub fn rebind(&self, token: &str, cur: &str, tag: &str) {
        let had;
        let has_cur;
        {
            let mut g = self.inner.lock();
            had = g.by_token.get(token).cloned();
            has_cur = g.matches.contains_key(cur);
            match &had {
                None => {
                    if has_cur {
                        g.by_token.insert(token.to_string(), cur.to_string());
                    }
                    return;
                }
                Some(h) if h == cur => return,
                _ => {}
            }
            if has_cur {
                g.by_token.insert(token.to_string(), cur.to_string());
            } else {
                g.by_token.remove(token);
            }
        }
        let to = if has_cur { cur.to_string() } else { format!("none (its match {} has no record)", cur) };
        dlog!("INTERROGATION rebind", tag, "from match", had.unwrap_or_default(), "to", to);
    }
}

impl Default for Interrogation {
    fn default() -> Self {
        Interrogation::new()
    }
}

// ------------------------------------------------------------------ differential test (verify/py/interrogation_harness.py)

fn state(it: &Interrogation) -> Value {
    let g = it.inner.lock();
    let mut m = serde_json::Map::new();
    for (mid, r) in &g.matches {
        m.insert(mid.clone(), r.to_json());
    }
    let bt: std::collections::BTreeMap<&String, &String> = g.by_token.iter().collect();
    json!({"matches": Value::Object(m), "order": g.matches.keys().collect::<Vec<_>>(), "by_token": bt, "rot": g.rot})
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let clk = crate::c_util::Frozen::new();
    let seed: i64 = std::env::var("T2R_TEST_SEED").ok().and_then(|v| v.trim().parse().ok()).unwrap_or(0);
    let mut it: Option<Interrogation> = None;
    for (i, c) in script(input).iter().enumerate() {
        clk.set(jf(c, "t"));
        let op = js(c, "op");
        if op == "init" {
            let lb: HashMap<i64, (i64, i64)> = c
                .get("lockboxes")
                .and_then(|x| x.as_object())
                .map(|o| {
                    o.iter()
                        .map(|(k, v)| {
                            let a = v.as_array();
                            (k.parse::<i64>().unwrap_or(0), (a.and_then(|a| a.first()).and_then(|x| x.as_i64()).unwrap_or(1), a.and_then(|a| a.get(1)).and_then(|x| x.as_i64()).unwrap_or(1)))
                        })
                        .collect()
                })
                .unwrap_or_default();
            let p = Params {
                mode: c.get("mode").and_then(|x| x.as_str()).map(|s| s.to_string()),
                need: c.get("need").and_then(|x| x.as_i64()),
                kills_per_info: c.get("kpi").and_then(|x| x.as_i64()),
                stage_secs: jf(c, "stage_secs"),
                boost_after: jf(c, "boost_after"),
                boost_every: jf(c, "boost_every"),
                boost_step: jf(c, "boost_step"),
                boost_max: ji(c, "boost_max"),
                locate_secs: jf(c, "locate_secs"),
                keep: jf(c, "keep"),
            };
            it = Some(Interrogation::with(p, clk.now(), PyRandom::from_i64(seed), Some(lb)));
            crate::c_util::emit(out, i, Value::Null, state(it.as_ref().unwrap()));
            continue;
        }
        let Some(x) = it.as_ref() else { continue };
        let teams: HashMap<String, i64> =
            c.get("teams").and_then(|t| t.as_object()).map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_i64().unwrap_or(0))).collect()).unwrap_or_default();
        let players = jstrs(c, "players");
        // team_of(slot): the script gives the team of every slot
        let slots: Vec<i64> = c.get("slots").and_then(|s| s.as_array()).map(|a| a.iter().map(|v| v.as_i64().unwrap_or(0)).collect()).unwrap_or_default();
        let team_of = |slot: usize| -> i64 { slots.get(slot).copied().unwrap_or((slot % 2) as i64) };
        let _ = teams;
        let r: Value = match op {
            "pick_mode" => json!(x.pick_mode()),
            "start" => json!(x.start(js(c, "mid"), &players, ji(c, "map"), &team_of, c.get("mode").and_then(|v| v.as_str()))),
            "roster" => {
                x.roster(js(c, "mid"), &players, &team_of);
                Value::Null
            }
            "end" => {
                x.end(js(c, "mid"), c.get("winner").and_then(|v| v.as_i64()));
                Value::Null
            }
            "drop" => {
                x.drop(js(c, "mid"));
                Value::Null
            }
            "event" => {
                let args: Vec<u8> = c.get("args").and_then(|a| a.as_array()).map(|a| a.iter().map(|v| v.as_u64().unwrap_or(0) as u8).collect()).unwrap_or_default();
                json!(x.event(js(c, "token"), ji(c, "sub"), ji(c, "seq"), &args))
            }
            "parse" => {
                let pl: Vec<u8> = c.get("pl").and_then(|a| a.as_array()).map(|a| a.iter().map(|v| v.as_u64().unwrap_or(0) as u8).collect()).unwrap_or_default();
                json!(x.parse_request(&pl).map(|(t, s, q, a)| json!([t, s, q, a])))
            }
            "rebind" => {
                x.rebind(js(c, "token"), js(c, "cur"), js(c, "tag"));
                Value::Null
            }
            "roundtrip" => {
                let e = x.export(js(c, "mid"));
                if let Some(v) = &e {
                    x.import(js(c, "to"), v);
                }
                json!(e.map(|v| py::dumps(&v)))
            }
            "has" => json!([x.has_match(js(c, "mid")), x.by_token_get(js(c, "token"))]),
            _ => Value::Null,
        };
        crate::c_util::emit(out, i, r, state(x));
    }
    0
}
