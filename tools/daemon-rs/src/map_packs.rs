//! Map packs (DLC maps) and the single-player-section map support reports (0xC6 / 0xC8). The pack registry is off live: `Packs`
//! answers exactly as it does with no registry file (`load_registry` is ported but not exercised by the differential test).
use crate::c_util::{jf, ji, js, jstrs, latin1, script, sys_now, Now};
use crate::py;
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::io::{BufRead, Write};

pub const OP_REPORT: u8 = 0xC6;
pub const OP_REPLY: u8 = 0xC7;
/// first map-list index of a pack map (= t2r-ext MF_NMAPS)
pub const PACK_BASE: usize = 25;
pub const MAX_PACKS: usize = 11;
pub const MAX_PACK_MAPS: usize = 32;
/// a report older than this no longer counts
pub const REPORT_TTL: f64 = 6.0 * 3600.0;

pub const SP_OP_REPORT: u8 = 0xC8;
pub const SP_OP_REPLY: u8 = 0xC9;
pub const SP_BASE: usize = 8;
pub const P1_FLAG_ALWAYS: u8 = 1;
pub const P1_MARK: &str = "-p1-";
pub const SP_MAX: u32 = 32;

// py: map_packs.py:41 enabled
pub fn enabled() -> bool {
    crate::env_str("T2R_MAP_PACKS", "") == "1"
}

/// One pack of the registry.
#[derive(Clone, Debug)]
pub struct Pack {
    pub id: String,
    pub ver: i64,
    pub hash: u32,
    /// (display name, image key, level, respawns)
    pub maps: Vec<(String, String, String, i64)>,
}

fn pystr(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        o => py::dumps_compact(o),
    }
}

// py: map_packs.py:45 load_registry
pub fn load_registry(path: Option<&std::path::Path>) -> Vec<Pack> {
    let p: std::path::PathBuf = match path {
        Some(p) => p.to_path_buf(),
        None => match std::env::var("T2R_MAP_PACKS_FILE") {
            Ok(v) if !v.is_empty() => v.into(),
            _ => crate::gen_modes::tools_dir().join("map_packs.json"),
        },
    };
    let Ok(b) = std::fs::read(&p) else { return Vec::new() };
    let Ok(data) = serde_json::from_slice::<Value>(&b) else { return Vec::new() };
    let mut out: Vec<Pack> = Vec::new();
    let mut seen_ids: HashSet<String> = HashSet::new();
    let mut seen_lvls: HashSet<String> = HashSet::new();
    let Some(packs) = data.as_object().and_then(|o| o.get("packs")).and_then(|x| x.as_array()) else { return out };
    for p in packs {
        let Some(po) = p.as_object() else { continue };
        let (Some(id), Some(h)) = (po.get("id"), po.get("hash")) else { continue };
        let pid = pystr(id);
        let Ok(hash) = u64::from_str_radix(pystr(h).trim().trim_start_matches("0x"), 16) else { continue };
        let hash = (hash & 0xffff_ffff) as u32;
        let mut maps = Vec::new();
        let mut bad = false;
        for m in po.get("maps").and_then(|x| x.as_array()).cloned().unwrap_or_default() {
            let a = m.as_array();
            match a {
                Some(a) if a.len() == 4 => match a[3].as_i64() {
                    Some(resp) => maps.push((pystr(&a[0]), pystr(&a[1]), pystr(&a[2]), resp)),
                    None => bad = true,
                },
                _ => bad = true,
            }
        }
        if bad {
            continue;
        }
        if pid.is_empty() || seen_ids.contains(&pid) || maps.is_empty() || maps.iter().any(|m| seen_lvls.contains(&m.2)) {
            continue;
        }
        seen_ids.insert(pid.clone());
        for m in &maps {
            seen_lvls.insert(m.2.clone());
        }
        out.push(Pack { id: pid, ver: po.get("ver").and_then(|x| x.as_i64()).unwrap_or(1), hash, maps });
    }
    out
}

// py: map_packs.py:107 sp_gating
pub fn sp_gating() -> bool {
    crate::env_str("T2R_SP_GATE", "1") != "0"
}

// py: map_packs.py:112 parse_sp_report: 0xC8 payload -> (token, mask)
pub fn parse_sp_report(pl: &[u8]) -> Option<(String, u64)> {
    if pl.len() < 68 {
        return None;
    }
    let token = latin1(&pl[..64]);
    if !token.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let m = u32::from_le_bytes([pl[64], pl[65], pl[66], pl[67]]) as u64;
    Some((token, m & ((1u64 << SP_MAX) - 1)))
}

// py: map_packs.py:121 parse_sp_flags
pub fn parse_sp_flags(pl: &[u8]) -> u8 {
    if pl.len() > 68 {
        pl[68]
    } else {
        0
    }
}

// py: map_packs.py:126 group_p1
pub fn group_p1(ready: &[String], is_always: &dyn Fn(&str) -> bool, is_owner: &dyn Fn(&str) -> bool) -> (Vec<String>, bool) {
    let al: Vec<String> = ready.iter().filter(|t| is_always(t)).cloned().collect();
    if al.is_empty() {
        return (ready.to_vec(), false);
    }
    let own: Vec<String> = ready.iter().filter(|t| al.contains(t) || is_owner(t)).cloned().collect();
    let rest: Vec<String> = ready.iter().filter(|t| !al.contains(t)).cloned().collect();
    let first_al = ready.first().map(|t| al.contains(t)).unwrap_or(false);
    let order: Vec<(Vec<String>, bool)> = if first_al { vec![(own, true), (rest, false)] } else { vec![(rest, false), (own, true)] };
    for (grp, kind) in &order {
        if grp.len() >= 2 {
            let any = grp.iter().any(|t| al.contains(t));
            return (grp.clone(), *kind && any);
        }
    }
    order.into_iter().next().unwrap_or((Vec::new(), false))
}

struct SpInner {
    have: HashMap<String, (u64, f64)>,
    flags: HashMap<String, u8>,
}

// py: map_packs.py:145 class SpSupport
pub struct SpSupport {
    levels: HashMap<String, usize>,
    p1_bits: u64,
    clock: Now,
    inner: Mutex<SpInner>,
}

impl SpSupport {
    /// `SpSupport([m[2] for m in SP_MAPS])`
    pub fn new(sp_levels: &[String]) -> SpSupport {
        SpSupport::with_clock(sp_levels, sys_now())
    }

    pub fn with_clock(sp_levels: &[String], clock: Now) -> SpSupport {
        let mut levels: HashMap<String, usize> = HashMap::new();
        for (k, l) in sp_levels.iter().enumerate() {
            levels.insert(l.clone(), k);
        }
        // only bits below SP_MAX can ever meet a report mask
        let p1_bits = levels.iter().filter(|(l, k)| l.contains(P1_MARK) && **k < 64).fold(0u64, |a, (_, k)| a | (1u64 << *k));
        SpSupport { levels, p1_bits, clock, inner: Mutex::new(SpInner { have: HashMap::new(), flags: HashMap::new() }) }
    }

    fn clock(&self) -> f64 {
        (self.clock)()
    }

    // py: map_packs.py:152 is_sp
    pub fn is_sp(&self, level: &str) -> bool {
        self.levels.contains_key(level)
    }

    // py: map_packs.py:155 report
    pub fn report(&self, token: &str, mask: u64, flags: u64) -> u64 {
        let t = self.clock();
        let mut g = self.inner.lock();
        g.have.insert(token.to_string(), (mask & ((1u64 << SP_MAX) - 1), t));
        g.flags.insert(token.to_string(), (flags & 0xff) as u8);
        mask
    }

    // py: map_packs.py:161 p1_always
    pub fn p1_always(&self, token: &str) -> bool {
        let fl = self.inner.lock().flags.get(token).copied().unwrap_or(0);
        (fl & P1_FLAG_ALWAYS) != 0 && (self.mask_of(token) & self.p1_bits) != 0
    }

    // py: map_packs.py:167 p1_owner
    pub fn p1_owner(&self, token: &str) -> bool {
        (self.mask_of(token) & self.p1_bits) != 0
    }

    // py: map_packs.py:171 mask_of
    pub fn mask_of(&self, token: &str) -> u64 {
        let (mask, at) = self.inner.lock().have.get(token).copied().unwrap_or((0, 0.0));
        if self.clock() - at < REPORT_TTL {
            mask
        } else {
            0
        }
    }

    // py: map_packs.py:176 allowed
    pub fn allowed(&self, level: &str, players: &[String]) -> bool {
        let Some(&k) = self.levels.get(level) else { return true };
        if !sp_gating() {
            return true;
        }
        !players.is_empty() && players.iter().all(|t| k < 64 && (self.mask_of(t) >> k) & 1 == 1)
    }

    // py: map_packs.py:182 reply_text
    pub fn reply_text(&self, token: &str) -> String {
        format!("on=1 sp={:x} p1={}", self.mask_of(token), if self.p1_always(token) { 1 } else { 0 })
    }
}

struct PackInner {
    have: HashMap<String, (BTreeSet<String>, f64)>,
}

// py: map_packs.py:186 class Packs
pub struct Packs {
    pub registry: Vec<Pack>,
    level_pack: HashMap<String, String>,
    clock: Now,
    inner: Mutex<PackInner>,
}

impl Packs {
    /// `Packs(map_packs.load_registry() if map_packs.enabled() else [])`
    pub fn new(registry: Vec<Pack>) -> Packs {
        Packs::with_clock(registry, sys_now())
    }

    pub fn with_clock(registry: Vec<Pack>, clock: Now) -> Packs {
        let mut level_pack = HashMap::new();
        for p in &registry {
            for m in &p.maps {
                level_pack.insert(m.2.clone(), p.id.clone());
            }
        }
        Packs { registry, level_pack, clock, inner: Mutex::new(PackInner { have: HashMap::new() }) }
    }

    fn clock(&self) -> f64 {
        (self.clock)()
    }

    // py: map_packs.py:195 maps
    pub fn maps(&self) -> Vec<(String, String, String, i64)> {
        self.registry.iter().flat_map(|p| p.maps.iter().cloned()).take(MAX_PACK_MAPS).collect()
    }

    // py: map_packs.py:199 is_pack_map
    pub fn is_pack_map(&self, level: &str) -> bool {
        self.level_pack.contains_key(level)
    }

    // py: map_packs.py:202 report
    pub fn report(&self, token: &str, hashes: &[u32]) -> BTreeSet<String> {
        let ids: BTreeSet<String> = hashes.iter().filter_map(|h| self.registry.iter().rev().find(|p| p.hash == *h)).map(|p| p.id.clone()).collect();
        let t = self.clock();
        self.inner.lock().have.insert(token.to_string(), (ids.clone(), t));
        ids
    }

    // py: map_packs.py:208 ids_of
    pub fn ids_of(&self, token: &str) -> BTreeSet<String> {
        let (ids, at) = self.inner.lock().have.get(token).cloned().unwrap_or((BTreeSet::new(), 0.0));
        if self.clock() - at < REPORT_TTL {
            ids
        } else {
            BTreeSet::new()
        }
    }

    // py: map_packs.py:213 allowed
    pub fn allowed(&self, level: &str, players: &[String]) -> bool {
        let Some(pid) = self.level_pack.get(level) else { return true };
        !players.is_empty() && players.iter().all(|t| self.ids_of(t).contains(pid))
    }

    // py: map_packs.py:219 reply_text: maps_list = the daemon's MAPS (name, image, level, respawns)
    pub fn reply_text(&self, token: &str, maps_list: &[(String, String, String, i64)]) -> String {
        let acc: Vec<String> = self.ids_of(token).into_iter().collect();
        let mut t = vec!["on=1".to_string(), format!("acc={}", if acc.is_empty() { "-".to_string() } else { acc.join(",") })];
        for (i, m) in maps_list.iter().enumerate() {
            if self.level_pack.contains_key(&m.2) {
                t.push(format!("pm{}={}", i, m.2));
            }
        }
        t.join(" ")
    }
}

// py: map_packs.py:227 parse_report: 0xC6 payload -> (token, [hashes])
pub fn parse_report(pl: &[u8]) -> Option<(String, Vec<u32>)> {
    if pl.len() < 65 {
        return None;
    }
    let token = latin1(&pl[..64]);
    if !token.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let n = (pl[64] as usize).min(MAX_PACKS);
    if pl.len() < 65 + 4 * n {
        return None;
    }
    Some((token, (0..n).map(|i| u32::from_le_bytes([pl[65 + 4 * i], pl[66 + 4 * i], pl[67 + 4 * i], pl[68 + 4 * i]])).collect()))
}

// ------------------------------------------------------------------ differential test (verify/py/map_packs_harness.py)

fn bytes_of(c: &Value, k: &str) -> Vec<u8> {
    c.get(k).and_then(|a| a.as_array()).map(|a| a.iter().map(|v| v.as_u64().unwrap_or(0) as u8).collect()).unwrap_or_default()
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let clk = crate::c_util::Frozen::new();
    let mut sp: Option<SpSupport> = None;
    let packs = Packs::with_clock(Vec::new(), clk.now());
    for (i, c) in script(input).iter().enumerate() {
        clk.set(jf(c, "t"));
        let op = js(c, "op");
        if op == "init" {
            sp = Some(SpSupport::with_clock(&jstrs(c, "levels"), clk.now()));
        }
        let Some(s) = sp.as_ref() else { continue };
        let tok = js(c, "token");
        let players = jstrs(c, "players");
        let r: Value = match op {
            "report" => json!(s.report(tok, ji(c, "mask") as u64, ji(c, "flags") as u64)),
            "p1_always" => json!(s.p1_always(tok)),
            "p1_owner" => json!(s.p1_owner(tok)),
            "mask_of" => json!(s.mask_of(tok)),
            "allowed" => json!(s.allowed(js(c, "level"), &players)),
            "is_sp" => json!(s.is_sp(js(c, "level"))),
            "reply" => json!(s.reply_text(tok)),
            "parse_sp" => json!(parse_sp_report(&bytes_of(c, "pl")).map(|(t, m)| json!([t, m]))),
            "parse_flags" => json!(parse_sp_flags(&bytes_of(c, "pl"))),
            "group_p1" => {
                let al: HashSet<String> = jstrs(c, "always").into_iter().collect();
                let ow: HashSet<String> = jstrs(c, "owner").into_iter().collect();
                let (g, k) = group_p1(&jstrs(c, "ready"), &|t| al.contains(t), &|t| ow.contains(t));
                json!([g, k])
            }
            "packs" => {
                let ml: Vec<(String, String, String, i64)> = jstrs(c, "levels2").into_iter().map(|l| (String::new(), String::new(), l, 0)).collect();
                let hashes: Vec<u32> = c.get("hashes").and_then(|a| a.as_array()).map(|a| a.iter().map(|v| v.as_u64().unwrap_or(0) as u32).collect()).unwrap_or_default();
                json!({"report": packs.report(tok, &hashes).into_iter().collect::<Vec<_>>(), "ids": packs.ids_of(tok).into_iter().collect::<Vec<_>>(),
                       "allowed": packs.allowed(js(c, "level"), &players), "is_pack_map": packs.is_pack_map(js(c, "level")),
                       "maps": packs.maps().len(), "text": packs.reply_text(tok, &ml)})
            }
            "parse_report" => json!(parse_report(&bytes_of(c, "pl")).map(|(t, h)| json!([t, h]))),
            "env" => json!({"enabled": enabled(), "gating": sp_gating()}),
            _ => Value::Null,
        };
        let mut have = serde_json::Map::new();
        {
            let g = s.inner.lock();
            let mut keys: Vec<&String> = g.have.keys().collect();
            keys.sort();
            for k in keys {
                have.insert(k.clone(), json!([g.have[k].0, g.have[k].1, g.flags.get(k).copied().unwrap_or(0)]));
            }
        }
        crate::c_util::emit(out, i, r, json!({"have": Value::Object(have)}));
    }
    0
}
