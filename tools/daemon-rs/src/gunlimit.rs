//! Per-team gun limits (op 0xDE / 0xDF): the daemon is the single arbiter, a gun is granted only while fewer than `max` teammates
//! hold it. Pure logic, no sockets.
use crate::c_util::{head, jf, ji, js, jstrs, latin1, script, sys_now, Now};
use crate::dlog;
use indexmap::IndexMap;
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::HashSet;
use std::io::{BufRead, Write};

pub const OP_REQ: u8 = 0xDE;
pub const OP_REPLY: u8 = 0xDF;
pub const CLAIM_TTL: f64 = 45.0;
/// pre-match: a claim is decided this long after its first arrival
pub const SETTLE: f64 = 0.6;
/// pre-match: claims within one bucket tie (lowest slot wins)
pub const SETTLE_BUCKET: f64 = 0.25;

/// One row of LIMITS: (weapon ids that count as the gun, max per team, display name for the message).
#[derive(Clone, Debug)]
pub struct Limit {
    pub ids: Vec<String>,
    pub max: i64,
    pub name: String,
}

/// py: gunlimit.py:42 LIMITS
pub fn default_limits() -> Vec<Limit> {
    vec![Limit { ids: vec!["shotgun-tactical".into(), "shotgun-tactical-supp".into()], max: 1, name: "TACTICAL SHOTGUN".into() }]
}

/// py: gunlimit.py:52 table_ids
pub fn table_ids() -> Vec<Vec<String>> {
    default_limits().into_iter().map(|l| l.ids).collect()
}

/// py: gunlimit.py (return of claim(with_pend=True))
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Claim {
    pub grant: i64,
    pub deny: i64,
    pub full: i64,
    pub held: i64,
    pub pend: i64,
}

type Holders = IndexMap<String, f64>;
type Teams<T> = IndexMap<i64, IndexMap<usize, T>>;

struct Inner {
    held: IndexMap<String, Teams<Holders>>,
    /// mid -> {team -> {bit -> {token: [first claim time, slot, last claim time]}}}
    want: IndexMap<String, Teams<IndexMap<String, (f64, i64, f64)>>>,
}

// py: gunlimit.py:56 class GunLimits
pub struct GunLimits {
    pub limits: Vec<Limit>,
    now: Now,
    ttl: f64,
    inner: Mutex<Inner>,
}

fn tok_gone(token: &str, active: &HashSet<String>) -> bool {
    !active.contains(token)
}

impl GunLimits {
    pub fn new() -> GunLimits {
        GunLimits::with(default_limits(), sys_now(), CLAIM_TTL)
    }

    /// `GunLimits(limits, now, log, ttl)` (the log is the daemon log).
    pub fn with(limits: Vec<Limit>, now: Now, ttl: f64) -> GunLimits {
        GunLimits { limits, now, ttl, inner: Mutex::new(Inner { held: IndexMap::new(), want: IndexMap::new() }) }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    // py: gunlimit.py:68 _prune (lock held)
    fn prune(&self, g: &mut Inner, mid: &str, active: &HashSet<String>) {
        let t = self.now();
        let Some(teams) = g.held.get_mut(mid) else { return };
        for bits in teams.values_mut() {
            for (bit, holders) in bits.iter_mut() {
                let dead: Vec<String> = holders.iter().filter(|(k, at)| tok_gone(k, active) || t - **at > self.ttl).map(|(k, _)| k.clone()).collect();
                for tok in dead {
                    holders.shift_remove(&tok);
                    let name = self.limits.get(*bit).map(|l| l.name.as_str()).unwrap_or("");
                    dlog!("GUNLIMIT", name, "released by", head(&tok, 6), "(left / stale)");
                }
            }
        }
    }

    /// py: gunlimit.py:76 claim. `active` = tokens still in the match (None: no pruning). pre = the match has not started yet.
    #[allow(clippy::too_many_arguments)]
    pub fn claim(&self, mid: &str, team: i64, token: &str, mask: i64, active: Option<&HashSet<String>>, pre: bool, slot: i64) -> Claim {
        let mut g = self.inner.lock();
        if let Some(a) = active {
            self.prune(&mut g, mid, a);
        }
        g.held.entry(mid.to_string()).or_default().entry(team).or_default();
        g.want.entry(mid.to_string()).or_default().entry(team).or_default();
        let t = self.now();
        let (mut grant, mut deny, mut full, mut held, mut pend) = (0i64, 0i64, 0i64, 0i64, 0i64);
        let Inner { held: hmap, want: wmap } = &mut *g;
        let Some(bits) = hmap.get_mut(mid).and_then(|m| m.get_mut(&team)) else { return Claim { grant, deny, full, held, pend } };
        let Some(wants) = wmap.get_mut(mid).and_then(|m| m.get_mut(&team)) else { return Claim { grant, deny, full, held, pend } };
        for (i, lim) in self.limits.iter().enumerate() {
            let (mx, name) = (lim.max, lim.name.as_str());
            let holders = bits.entry(i).or_default();
            let wi = wants.entry(i).or_default();
            let stale: Vec<String> =
                wi.iter().filter(|(k, w)| t - w.2 > self.ttl || active.map(|a| tok_gone(k, a)).unwrap_or(false)).map(|(k, _)| k.clone()).collect();
            for k in stale {
                wi.shift_remove(&k);
            }
            let want = (mask >> i) & 1 == 1;
            if want {
                if let Some(w) = wi.get_mut(token) {
                    w.2 = t;
                    w.1 = slot;
                } else {
                    wi.insert(token.to_string(), (t, slot, t));
                }
            } else {
                wi.shift_remove(token);
            }
            if want && pre && !holders.contains_key(token) {
                let mut ranked: Vec<&String> = wi.keys().filter(|k| !holders.contains_key(*k)).collect();
                ranked.sort_by(|a, b| {
                    let ka = ((wi[*a].0 / SETTLE_BUCKET).trunc() as i64, wi[*a].1);
                    let kb = ((wi[*b].0 / SETTLE_BUCKET).trunc() as i64, wi[*b].1);
                    ka.cmp(&kb).then_with(|| a.cmp(b))
                });
                let rank = ranked.iter().position(|k| k.as_str() == token).unwrap_or(0) as i64;
                if t - wi[token].0 < SETTLE {
                    pend |= 1 << i;
                } else if rank < mx - holders.len() as i64 {
                    holders.insert(token.to_string(), t);
                    grant |= 1 << i;
                    dlog!("GUNLIMIT", name, "team", team, "pre-match granted to", head(token, 6), format!("(earliest claim, slot {})", slot));
                } else {
                    deny |= 1 << i;
                    dlog!("GUNLIMIT", name, "team", team, "pre-match DENIED to", head(token, 6), "(earlier claim holds it)");
                }
            } else if want {
                if holders.contains_key(token) || (holders.len() as i64) < mx {
                    if !holders.contains_key(token) {
                        dlog!("GUNLIMIT", name, "team", team, "granted to", head(token, 6), format!("({}/{})", holders.len() + 1, mx));
                    }
                    holders.insert(token.to_string(), t);
                    grant |= 1 << i;
                } else {
                    deny |= 1 << i;
                    dlog!("GUNLIMIT", name, "team", team, "DENIED to", head(token, 6), format!("({}/{} taken)", holders.len(), mx));
                }
            } else if holders.contains_key(token) {
                holders.shift_remove(token);
                dlog!("GUNLIMIT", name, "team", team, "released by", head(token, 6));
            }
            if holders.contains_key(token) {
                held |= 1 << i;
            }
            if holders.keys().filter(|k| k.as_str() != token).count() as i64 >= mx {
                full |= 1 << i;
            }
        }
        Claim { grant, deny, full, held, pend }
    }

    // py: gunlimit.py:131 release
    pub fn release(&self, token: &str) {
        let mut g = self.inner.lock();
        for teams in g.want.values_mut() {
            for bits in teams.values_mut() {
                for wi in bits.values_mut() {
                    wi.shift_remove(token);
                }
            }
        }
        for teams in g.held.values_mut() {
            for bits in teams.values_mut() {
                for holders in bits.values_mut() {
                    holders.shift_remove(token);
                }
            }
        }
    }

    // py: gunlimit.py:142 end
    pub fn end(&self, mid: &str) {
        let mut g = self.inner.lock();
        g.held.shift_remove(mid);
        g.want.shift_remove(mid);
    }

    // py: gunlimit.py:147 export_held
    pub fn export_held(&self, mid: &str) -> Value {
        let g = self.inner.lock();
        let mut out = serde_json::Map::new();
        if let Some(teams) = g.held.get(mid) {
            for (team, bits) in teams {
                let mut b = serde_json::Map::new();
                for (bit, h) in bits {
                    if !h.is_empty() {
                        let mut v: Vec<&String> = h.keys().collect();
                        v.sort();
                        b.insert(bit.to_string(), json!(v));
                    }
                }
                out.insert(team.to_string(), Value::Object(b));
            }
        }
        Value::Object(out)
    }

    // py: gunlimit.py:154 import_held
    pub fn import_held(&self, mid: &str, data: &Value) {
        let t = self.now();
        let mut g = self.inner.lock();
        let Some(teams) = data.as_object() else { return };
        for (team, bits) in teams {
            let Some(bits) = bits.as_object() else { continue };
            for (bit, toks) in bits {
                let (Ok(team), Ok(bit)) = (team.trim().parse::<i64>(), bit.trim().parse::<usize>()) else { continue };
                let holders = g.held.entry(mid.to_string()).or_default().entry(team).or_default().entry(bit).or_default();
                if let Some(a) = toks.as_array() {
                    for tok in a {
                        if let Some(s) = tok.as_str() {
                            holders.insert(s.to_string(), t);
                        }
                    }
                }
            }
        }
    }

    // py: gunlimit.py:164 gc
    pub fn gc(&self, live_mids: &HashSet<String>) {
        let mut g = self.inner.lock();
        g.held.retain(|m, _| live_mids.contains(m));
        g.want.retain(|m, _| live_mids.contains(m));
    }

    // py: gunlimit.py:171 parse_request
    pub fn parse_request(&self, pl: &[u8]) -> Option<(String, i64)> {
        if pl.len() < 66 {
            return None;
        }
        let token = latin1(&pl[..64]);
        if !token.chars().all(|c| c.is_ascii_hexdigit()) {
            return None;
        }
        let m = pl[64] as i64 | ((pl[65] as i64) << 8);
        Some((token, m & ((1i64 << self.limits.len()) - 1)))
    }

    // py: gunlimit.py:178 text
    pub fn text(on: bool, team: i64, held: i64, grant: i64, deny: i64, full: i64, pend: i64) -> String {
        if !on {
            return "on=0".to_string();
        }
        format!("on=1 team={} mask={} grant={} deny={} full={} pend={}", team, held, grant, deny, full, pend)
    }
}

impl Default for GunLimits {
    fn default() -> Self {
        GunLimits::new()
    }
}

// ------------------------------------------------------------------ differential test (verify/py/gunlimit_harness.py)

fn state(gl: &GunLimits) -> Value {
    let g = gl.inner.lock();
    let mut held = serde_json::Map::new();
    for (mid, teams) in &g.held {
        let mut tm = serde_json::Map::new();
        for (team, bits) in teams {
            let mut bm = serde_json::Map::new();
            for (bit, h) in bits {
                bm.insert(bit.to_string(), Value::Array(h.iter().map(|(k, v)| json!([k, v])).collect()));
            }
            tm.insert(team.to_string(), Value::Object(bm));
        }
        held.insert(mid.clone(), Value::Object(tm));
    }
    let mut want = serde_json::Map::new();
    for (mid, teams) in &g.want {
        let mut tm = serde_json::Map::new();
        for (team, bits) in teams {
            let mut bm = serde_json::Map::new();
            for (bit, w) in bits {
                bm.insert(bit.to_string(), Value::Array(w.iter().map(|(k, v)| json!([k, [v.0, v.1, v.2]])).collect()));
            }
            tm.insert(team.to_string(), Value::Object(bm));
        }
        want.insert(mid.clone(), Value::Object(tm));
    }
    json!({"held": Value::Object(held), "want": Value::Object(want), "horder": g.held.keys().collect::<Vec<_>>(), "worder": g.want.keys().collect::<Vec<_>>()})
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let clk = crate::c_util::Frozen::new();
    // two guns, one of them with max 2, so the ranking and the "full" bit are exercised beyond the shipped table
    let limits = vec![
        Limit { ids: vec!["shotgun-tactical".into()], max: 1, name: "TACTICAL SHOTGUN".into() },
        Limit { ids: vec!["x".into()], max: 2, name: "SECOND GUN".into() },
    ];
    let gl = GunLimits::with(limits, clk.now(), CLAIM_TTL);
    for (i, c) in script(input).iter().enumerate() {
        clk.set(jf(c, "t"));
        let r: Value = match js(c, "op") {
            "claim" => {
                let active: Option<HashSet<String>> = if c.get("active").map(|a| a.is_null()).unwrap_or(true) { None } else { Some(jstrs(c, "active").into_iter().collect()) };
                let cl = gl.claim(js(c, "mid"), ji(c, "team"), js(c, "token"), ji(c, "mask"), active.as_ref(), c.get("pre").and_then(|x| x.as_bool()).unwrap_or(false), ji(c, "slot"));
                json!([cl.grant, cl.deny, cl.full, cl.held, cl.pend, GunLimits::text(true, ji(c, "team"), cl.held, cl.grant, cl.deny, cl.full, cl.pend)])
            }
            "release" => {
                gl.release(js(c, "token"));
                Value::Null
            }
            "end" => {
                gl.end(js(c, "mid"));
                Value::Null
            }
            "gc" => {
                gl.gc(&jstrs(c, "live").into_iter().collect());
                Value::Null
            }
            "export" => gl.export_held(js(c, "mid")),
            "import" => {
                gl.import_held(js(c, "mid"), c.get("data").unwrap_or(&Value::Null));
                Value::Null
            }
            "parse" => {
                let pl: Vec<u8> = c.get("pl").and_then(|x| x.as_array()).map(|a| a.iter().map(|v| v.as_u64().unwrap_or(0) as u8).collect()).unwrap_or_default();
                json!(gl.parse_request(&pl).map(|(t, m)| json!([t, m])))
            }
            "text" => json!([
                GunLimits::text(false, 0, 0, 0, 0, 0, 0),
                GunLimits::text(true, ji(c, "team"), ji(c, "held"), ji(c, "grant"), ji(c, "deny"), ji(c, "full"), ji(c, "pend"))
            ]),
            "table" => json!(table_ids()),
            _ => Value::Null,
        };
        crate::c_util::emit(out, i, r, state(&gl));
    }
    0
}
