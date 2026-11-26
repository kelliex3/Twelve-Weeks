//! py: clan_saga.py - the long, match-tied entries of the clan chronicle (EN + PL) with a persistent memory per member. Called by clan_chronicle::day_rows; it never changes a
//! clan number or a person's state, it only writes text and keeps its own memory in `st["ch"]` (`cs` of the Python is never passed: callees take it from `st`).
//! A Python exception is `Err(String)`; the chronicle catches it ("clan_saga failed") and goes on without saga rows.
use crate::clan::clan::{int_of, pystr, truthy};
use crate::clan::clan_loot::{self as lt, hv, pyformat, re, vi, vs};
use crate::clan::clan_saga_pl::{self as pls, splitlines, Vars};
use crate::clan::clan_chronicle::{self as ch, Day};
use crate::clan::{clan_kids as kd, clan_story as cs_, data};
use crate::py;
use regex::Regex;
use serde_json::{json, Map, Value};
use std::collections::{BTreeSet, HashMap};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::PyRandom;

pub const WINDOW: i64 = 14;
const NW: [&str; 11] = ["no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"];
const W: &str = r"\p{L}\p{N}_";

fn sd(name: &str) -> &'static Value {
    data::t("clan_saga_data", name)
}

fn camps(fk: &str) -> [&'static str; 5] {
    if fk == "W" {
        ["the east barracks", "the FOB mess", "the old gym", "the pharmacy block", "the stadium tunnel"]
    } else {
        ["the long house", "the kitchen house", "the weaving house", "the shore camp", "the harbor hut"]
    }
}

fn mood_of(scene: &str) -> Option<&'static str> {
    Some(match scene {
        "loss" | "down" | "exec" | "inj" | "lack" => "worried",
        "win" | "haul" | "keepcb" | "keepfind" => "cheerful",
        "rev" | "rel" => "hopeful",
        "quiet" | "use" | "prof" => "calm",
        "dep" | "grief" | "death" => "grieving",
        "prep" => "afraid",
        _ => return None,
    })
}

fn tail_of(scene: &str) -> &'static str {
    match scene {
        "inj" | "grief" | "death" | "loss" | "dep" | "lack" | "exec" => "tail.bad",
        "rel" | "use" | "prof" | "prep" | "quiet" | "down" => "tail.calm",
        "keepcb" | "keepfind" | "haul" | "win" | "rev" => "tail.good",
        _ => "tail.calm",
    }
}

// ------------------------------------------------------------------ the template pools
static KEY_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"\{{([{w}]+)\}}", w = W)).expect("KEY_RE"));
static PERSON_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"^([ABCD])(?:_([{w}]+))?$", w = W)).expect("PERSON_RE"));
static THEY: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\b([Tt]hey) ([a-z]+)\b").expect("THEY"));

/// One template line of a pool: scope ("*", "W", "S"), text, person letters, variable keys, `str(_h(text))`.
pub struct PoolLine {
    pub scope: String,
    pub text: String,
    pub pk: Vec<char>,
    pub vk: Vec<String>,
    pub hs: String,
}

// py: clan_saga.py:54 pools
/// {pool name: [(scope, text, person letters, variable keys)]} parsed from the clan_saga_text* modules (their TEXT strings are concatenated as they are).
pub fn pools() -> HashMap<String, Vec<PoolLine>> {
    let mut out: HashMap<String, Vec<PoolLine>> = HashMap::new();
    let mut cur: Option<String> = None;
    let mut all = String::new();
    for m in pls::EN_MODS {
        all.push_str(data::t(m, "TEXT").as_str().unwrap_or(""));
    }
    for line in splitlines(&all) {
        let mut line = py::strip(line).to_string();
        if line.is_empty() {
            continue;
        }
        if let Some(h) = line.strip_prefix("== ") {
            let name = py::strip(h).to_string();
            out.entry(name.clone()).or_default();
            cur = Some(name);
            continue;
        }
        let mut scope = "*".to_string();
        if line.starts_with("W:") || line.starts_with("S:") {
            scope = line[..1].to_string();
            line = py::strip(&line[2..]).to_string();
        }
        let mut pk: Vec<char> = Vec::new();
        let mut vk: Vec<String> = Vec::new();
        for m in KEY_RE.captures_iter(&line) {
            let k = &m[1];
            if let Some(pm) = PERSON_RE.captures(k) {
                let c = pm[1].chars().next().unwrap_or('A');
                if !pk.contains(&c) {
                    pk.push(c);
                }
            } else if !vk.iter().any(|x| x == k) {
                vk.push(k.to_string());
            }
        }
        let hs = hv(&[line.clone()]).to_string();
        if let Some(c) = &cur {
            out.entry(c.clone()).or_default().push(PoolLine { scope, text: line, pk, vk, hs });
        }
    }
    out
}

pub static POOLS: LazyLock<HashMap<String, Vec<PoolLine>>> = LazyLock::new(pools);

// ------------------------------------------------------------------ small text helpers
// py: clan_saga.py:81 _nw
pub fn nw(n: i64) -> String {
    if (0..=10).contains(&n) {
        NW[n as usize].to_string()
    } else {
        n.to_string()
    }
}

// py: clan_saga.py:85 times_p
pub fn times_p(n: i64) -> String {
    if n == 1 {
        "once".into()
    } else if n == 2 {
        "twice".into()
    } else {
        format!("{} times", nw(n))
    }
}

// py: clan_saga.py:89 count_p
pub fn count_p(n: i64, one: &str, many: &str) -> String {
    format!("{} {}", nw(n), if n == 1 { one } else { many })
}

// py: clan_saga.py:93 ago
pub fn ago(d0: i64, d1: i64) -> String {
    let n = d1 - d0;
    if n == 1 {
        "yesterday".into()
    } else if n <= 0 {
        "today".into()
    } else {
        format!("{} days ago", nw(n))
    }
}

fn en_short(name: &str) -> &str {
    data::t("clan_chronicle_maps", "EN_SHORT").get(name).and_then(|v| v.as_str()).unwrap_or(name)
}

// py: clan_saga.py:98 map_art
/// 'the Hospital' for a map the English calls by a noun, 'Downtown' / 'Hillside' / 'Hillcrest' without an article.
pub fn map_art(name: &str) -> String {
    let en = en_short(name);
    if en.is_empty() {
        return String::new();
    }
    if matches!(en, "Downtown" | "Hillside" | "Hillcrest" | "Houses" | "Suburbs") {
        en.to_string()
    } else {
        format!("the {}", en)
    }
}

// py: clan_saga.py:106 map_data
pub fn map_data(name: &str) -> &'static Value {
    sd("MAPS").get(en_short(name)).unwrap_or_else(|| sd("GENERIC"))
}

fn places_of(md: &'static Value) -> Vec<String> {
    md.get("places").and_then(|p| p.as_array()).map(|a| a.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect()).unwrap_or_default()
}

const NOT_VERB: [&str; 16] = [
    "always", "perhaps", "afterwards", "towards", "besides", "sometimes", "thus", "upstairs", "downstairs", "this", "its", "as", "us", "across", "less", "unless",
];

// py: clan_saga.py:115 they_fix
/// 'they mentions' -> 'they mention' (a gender-neutral survivor is rendered with 'they'; the templates conjugate for he / she).
pub fn they_fix(text: &str) -> String {
    THEY.replace_all(text, |m: &regex::Captures| {
        let g1 = &m[1];
        let w = &m[2];
        match w {
            "is" => return format!("{} are", g1),
            "was" => return format!("{} were", g1),
            "has" => return format!("{} have", g1),
            "does" => return format!("{} do", g1),
            "goes" => return format!("{} go", g1),
            _ => {}
        }
        if NOT_VERB.contains(&w) || w == "is_" || w == "was_" || !w.ends_with('s') || w.ends_with("ss") || w.ends_with("us") {
            return m[0].to_string();
        }
        if w.ends_with("ies") && w.len() > 5 {
            return format!("{} {}y", g1, &w[..w.len() - 3]);
        }
        if ["shes", "ches", "xes", "sses", "zes"].iter().any(|s| w.ends_with(s)) {
            return format!("{} {}", g1, &w[..w.len() - 2]);
        }
        format!("{} {}", g1, &w[..w.len() - 1])
    })
    .into_owned()
}

// py: clan_saga.py:139 fit
/// How well survivor p fits a story arc of `kind` (lower = better), from the saga memory. `cs` = st["ch"].
pub fn fit(cs: &Value, kind: &str, p: &Value) -> i64 {
    let key = pystr(p.get("i").unwrap_or(&Value::Null));
    let empty = json!({});
    let m = cs.get("mem").filter(|v| truthy(v)).and_then(|mm| mm.get(&key)).filter(|v| truthy(v)).unwrap_or(&empty);
    let inj = m.get("inj").map(truthy).unwrap_or(false);
    let grief = m.get("grief").map(truthy).unwrap_or(false);
    let r = vs(p, "r");
    let mut s = 0i64;
    match kind {
        "missing" => {
            s += if inj { 5 } else { 0 };
            s -= if matches!(r, "scout" | "scavenger" | "guard") { 2 } else { 0 };
            s -= if m.get("rel").map(truthy).unwrap_or(false) { 1 } else { 0 };
        }
        "sick" => s -= if inj { 2 } else { 0 },
        "wish" | "betray" => s -= if grief { 3 } else { 0 },
        "blame" => s -= if m.get("bt").and_then(|x| x.as_str()) == Some("rival") { 2 } else { 0 },
        "keep" => s -= if m.get("poss").map(truthy).unwrap_or(false) { 1 } else { 0 },
        "new" => s -= if m.get("bond").map(truthy).unwrap_or(false) { 1 } else { 0 },
        _ => {}
    }
    s
}

// py: clan_saga.py:162 new_mem
pub fn new_mem() -> Value {
    json!({"inj": null, "scars": [], "grief": null, "poss": null, "fear": null, "rel": {}, "last": -99, "bond": null, "bt": null})
}

// py: clan_saga.py:166 facts
/// The player's real match as the saga reads it (every field optional).
pub fn facts(matchv: &Value) -> Map<String, Value> {
    let mut f = Map::new();
    let m = if truthy(matchv) { matchv } else { &Value::Null };
    f.insert("map".into(), json!(m.get("map").filter(|v| truthy(v)).map(pystr).unwrap_or_default()));
    for (k_in, k_out) in [("kills", "k"), ("execs", "ex"), ("revives", "rv"), ("headshots", "hs"), ("downs", "dn"), ("deaths", "dt"), ("streak", "streak")] {
        if let Some(v) = m.get(k_in).filter(|v| !v.is_null()) {
            f.insert(k_out.into(), json!(int_of(v)));
        }
    }
    // `m.get("win") in (True, False)`: 1 / 0 / 1.0 / 0.0 equal True / False
    match m.get("win") {
        Some(Value::Bool(b)) => {
            f.insert("win".into(), json!(*b));
        }
        Some(Value::Number(n)) if n.as_f64() == Some(1.0) || n.as_f64() == Some(0.0) => {
            f.insert("win".into(), json!(n.as_f64() == Some(1.0)));
        }
        _ => {}
    }
    if let Some(w) = m.get("weapon").and_then(|w| w.as_str()) {
        if sd("WEAPONS").get(w).is_some() {
            f.insert("weapon".into(), json!(w));
        }
    }
    f
}

fn fi(f: &Map<String, Value>, k: &str) -> i64 {
    f.get(k).map(int_of).unwrap_or(0)
}
fn fs<'a>(f: &'a Map<String, Value>, k: &str) -> &'a str {
    f.get(k).and_then(|x| x.as_str()).unwrap_or("")
}

fn omap<'x>(v: &'x mut Value, k: &str) -> Result<&'x mut Map<String, Value>, String> {
    v.get_mut(k).and_then(|x| x.as_object_mut()).ok_or_else(|| format!("TypeError: {}", k))
}
fn varr<'x>(v: &'x mut Value, k: &str) -> Result<&'x mut Vec<Value>, String> {
    v.get_mut(k).and_then(|x| x.as_array_mut()).ok_or_else(|| format!("TypeError: {}", k))
}

fn strict() -> bool {
    std::env::var("T2R_SAGA_STRICT").map(|v| !v.is_empty()).unwrap_or(false)
}

fn people1(k: &str, p: &Value) -> Value {
    let mut m = Map::new();
    m.insert(k.to_string(), p.clone());
    Value::Object(m)
}

fn vmap(pairs: Vec<(&str, Value)>) -> Map<String, Value> {
    pairs.into_iter().map(|(k, v)| (k.to_string(), v)).collect()
}

fn has_word(s: &str, name: &str) -> bool {
    if name.is_empty() {
        return false;
    }
    let isw = |c: char| c.is_alphanumeric() || c == '_';
    for (i, _) in s.char_indices() {
        if s[i..].starts_with(name) {
            let before = s[..i].chars().next_back();
            let after = s[i + name.len()..].chars().next();
            if !before.map(isw).unwrap_or(false) && !after.map(isw).unwrap_or(false) {
                return true;
            }
        }
    }
    false
}

/// `len(re.split(r"(?<=[.!?])\s+", text))`.
fn n_sentences(text: &str) -> usize {
    let mut n = 1;
    let mut prev: Option<char> = None;
    let mut it = text.chars().peekable();
    while let Some(c) = it.next() {
        if py::is_py_space(c) && matches!(prev, Some('.') | Some('!') | Some('?')) {
            while let Some(&d) = it.peek() {
                if py::is_py_space(d) {
                    it.next();
                } else {
                    break;
                }
            }
            n += 1;
            prev = Some(' ');
            continue;
        }
        prev = Some(c);
    }
    n
}

// ------------------------------------------------------------------ the saga of one day
struct Saga<'a> {
    d: &'a mut Day,
    st: &'a mut Value,
    r: &'a Value,
    rng: &'a mut PyRandom,
    day: i64,
    fk: &'static str,
    f: Map<String, Value>,
    base: Map<String, Value>,
    tids: Vec<u32>,
    pl_last: Option<String>,
    strict: bool,
}

#[derive(Clone, Copy, PartialEq)]
enum Scene {
    Injury,
    Grief,
    Scar,
    Rel,
    Lack,
    KeepCb,
    KeepFind,
    Dep,
    Use,
    Prep,
    Prof,
}

type Parts = Vec<(String, bool)>;

fn part(p: &str, must: bool) -> (String, bool) {
    (p.to_string(), must)
}

impl<'a> Saga<'a> {
    // py: clan_saga.py:182 Saga.__init__
    fn new(d: &'a mut Day, st: &'a mut Value, r: &'a Value, matchv: &Value, rng: &'a mut PyRandom) -> Result<Saga<'a>, String> {
        if !r.is_object() {
            return Err("AttributeError: r".into());
        }
        let day = d.day;
        let fk = d.fk;
        let f = if truthy(matchv) { facts(matchv) } else { Map::new() };
        {
            let cs = ch::ch_state(st);
            let co = cs.as_object_mut().ok_or("cs")?;
            for (k, dv) in [
                ("mem", json!({})),
                ("gone", json!({})),
                ("you", json!({"inj": null, "streak": 0})),
                ("sg_used", json!({})),
                ("sg_stock", json!([])),
                ("sg_keep", json!({})),
                ("sg_hist", json!([])),
                ("sg_mourned", json!([])),
                ("sg_last", json!("")),
                ("sg_thread", json!(0)),
                ("sg_kf", json!([])),
            ] {
                co.entry(k.to_string()).or_insert(dv);
            }
        }
        let tid = st.get("tid").map(pystr).unwrap_or_default();
        let fac = data::t("clan_story_data", "FACTION").get(fk).ok_or("KeyError: FACTION")?;
        let enemy = fac.get("enemy").cloned().unwrap_or(Value::Null);
        let enemies = format!("the {}", fac.get("enemies").map(pystr).unwrap_or_default());
        let camp = camps(fk)[(hv(&["camp".to_string(), tid]) % 5) as usize];
        let base = vmap(vec![
            ("camp", json!(camp)),
            ("leader", json!(if fk == "W" { "Command" } else { "the Prophet" })),
            ("enemy", enemy),
            ("enemies", json!(enemies)),
            ("enemy_n", json!(if fk == "W" { "Scar" } else { "Wolf" })),
        ]);
        let mut s = Saga { d, st, r, rng, day, fk, f, base, tids: Vec::new(), pl_last: None, strict: strict() };
        s.sync_roster()?;
        s.note_lack()?;
        Ok(s)
    }

    fn cs(&mut self) -> &mut Value {
        ch::ch_state(self.st)
    }

    // ------------------------------------------------------------ memory
    // py: clan_saga.py:198 Saga.M
    fn m(&mut self, p: &Value) -> Result<&mut Map<String, Value>, String> {
        let key = pystr(p.get("i").ok_or("KeyError: i")?);
        let kid = kd::is_kid(p);
        {
            let mem = omap(self.cs(), "mem")?;
            if !mem.contains_key(&key) {
                mem.insert(key.clone(), new_mem());
            }
            let e = mem.get(&key).ok_or("mem")?;
            let poss = e.get("poss").cloned().unwrap_or(Value::Null);
            let need = poss.is_null() || (kid && kd::combat_text(&pystr(&poss), Some("en")));
            if need {
                let h = hv(&["poss".to_string(), pystr(p.get("n").ok_or("KeyError: n")?), pystr(p.get("r").ok_or("KeyError: r")?)]) as u64;
                let ok: Vec<&'static str> = data::strs(sd("POSSESSIONS")).into_iter().filter(|x| !(kid && kd::combat_text(x, Some("en")))).collect();
                if ok.is_empty() {
                    return Err("ZeroDivisionError".into());
                }
                let pick = ok[(h % ok.len() as u64) as usize];
                let em = mem.get_mut(&key).and_then(|x| x.as_object_mut()).ok_or("mem")?;
                em.insert("poss".into(), json!(pick));
                let fear = em.get("fear").cloned().unwrap_or(Value::Null);
                em.insert("fear".into(), if fear.is_null() { json!(h >> 9) } else { fear });
            }
        }
        omap(self.cs(), "mem")?.get_mut(&key).and_then(|x| x.as_object_mut()).ok_or_else(|| "mem".to_string())
    }

    fn mv(&mut self, p: &Value, k: &str) -> Result<Value, String> {
        Ok(self.m(p)?.get(k).cloned().unwrap_or(Value::Null))
    }

    fn mset(&mut self, p: &Value, k: &str, v: Value) -> Result<(), String> {
        self.m(p)?.insert(k.to_string(), v);
        Ok(())
    }

    // py: clan_saga.py:207 fear_of
    fn fear_of(&mut self, p: &Value) -> Result<(String, String), String> {
        let f = self.mv(p, "fear")?;
        let fears = sd("FEARS").as_array().ok_or("FEARS")?;
        let i = py::pymod(int_of(&f), fears.len() as i64) as usize;
        Ok((pystr(&fears[i][0]), pystr(&fears[i][1])))
    }

    // py: clan_saga.py:211 sync_roster
    /// Memory follows the roster: the persona bond is cached, a member who died since the last day goes to cs["gone"] (grief for the close ones).
    fn sync_roster(&mut self) -> Result<(), String> {
        let people: Vec<Value> = self.st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        let alive: BTreeSet<String> = people.iter().map(|p| pystr(p.get("i").unwrap_or(&Value::Null))).collect();
        for p in &people {
            self.m(p)?;
            let ps = ch::persona(p, &people, self.fk, Some(&mut *self.st));
            let past = ch::pasts().get(ps.past).and_then(|r| r.get(2)).cloned().ok_or("IndexError: PASTS")?;
            let m = self.m(p)?;
            m.insert("bond".into(), ps.bond.map(|b| json!(b)).unwrap_or(Value::Null));
            m.insert("bt".into(), ps.bt.map(|b| json!(b)).unwrap_or(Value::Null));
            m.insert("temper".into(), json!(ps.temper));
            m.insert("past".into(), past);
        }
        let dead: Vec<Value> = self.st.get("dead").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        for dd in &dead {
            let k = pystr(dd.get("i").ok_or("KeyError: i")?);
            let mourned = self.cs().get("sg_mourned").and_then(|v| v.as_array()).map(|a| a.iter().any(|x| x.as_str() == Some(&k))).unwrap_or(false);
            if mourned {
                continue;
            }
            {
                let ml = varr(self.cs(), "sg_mourned")?;
                ml.push(json!(k));
                let n = ml.len();
                if n > 40 {
                    ml.drain(..n - 40);
                }
            }
            let popped = omap(self.cs(), "mem")?.shift_remove(&k);
            let m = popped.filter(|v| truthy(v)).unwrap_or_else(new_mem);
            let rec = json!({"n": dd.get("n").cloned().ok_or("KeyError: n")?, "g": dd.get("g").cloned().ok_or("KeyError: g")?, "r": dd.get("r").cloned().ok_or("KeyError: r")?,
                "d": dd.get("d").cloned().ok_or("KeyError: d")?, "m": m});
            omap(self.cs(), "gone")?.insert(k.clone(), rec);
            let di = dd.get("i").cloned().unwrap_or(Value::Null);
            let dday = int_of(dd.get("d").unwrap_or(&Value::Null));
            for p in &people {
                let mp = self.m(p)?;
                let hit = mp.get("bond") == Some(&di)
                    || dd.get("k").map(|x| x == p.get("i").unwrap_or(&Value::Null)).unwrap_or(false)
                    || mp.get("rel").and_then(|r| r.as_object()).map(|r| r.contains_key(&pystr(&di))).unwrap_or(false);
                if hit {
                    mp.insert("grief".into(), json!({"for": di, "d": dday, "until": dday + 8, "echo": -99}));
                }
            }
        }
        {
            let mem = omap(self.cs(), "mem")?;
            let gone: Vec<String> = mem.keys().filter(|k| !alive.contains(*k)).cloned().collect();
            for k in gone {
                mem.shift_remove(&k);
            }
        }
        let day = self.day;
        let gm = omap(self.cs(), "gone")?;
        let old: Vec<String> = gm.iter().filter(|(_, v)| day - v.get("d").map(int_of).unwrap_or(0) > 60).map(|(k, _)| k.clone()).collect();
        for k in old {
            gm.shift_remove(&k);
        }
        Ok(())
    }

    // py: clan_saga.py:239 note_lack
    /// A shortage day (r["sup"]) goes into the memory of the hungry / sick: m["lack"] = {c: the category that ran short, d: day}; a member who is healthy again forgets it.
    fn note_lack(&mut self) -> Result<(), String> {
        let sup = self.r.get("sup").filter(|v| truthy(v)).cloned().unwrap_or_else(|| json!({}));
        let cat = sup.get("cause").filter(|v| truthy(v)).map(pystr).unwrap_or_default();
        let short = sup.get("short").filter(|v| truthy(v)).cloned().unwrap_or_else(|| json!({}));
        let sh = |k: &str| short.get(k).map(truthy).unwrap_or(false);
        let people: Vec<Value> = self.st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        let day = self.day;
        for p in &people {
            let s = vs(p, "s").to_string();
            let m = self.m(p)?;
            if s == "h" {
                m.shift_remove("lack");
            } else if !cat.is_empty() {
                let mut c = if s == "s" && sh("med") { "med".to_string() } else { cat.clone() };
                if c == "med" && s != "s" {
                    c = ["food", "water"].iter().find(|k| sh(k)).map(|k| k.to_string()).unwrap_or_else(|| cat.clone());
                }
                m.insert("lack".into(), json!({"c": c, "d": day}));
            }
        }
        Ok(())
    }

    // py: clan_saga.py:254 scene_lack
    /// A hungry or sick member whose trouble the memory ties to the day's shortage: a small scene that names the missing goods and what the larder holds.
    fn scene_lack(&mut self) -> Result<Option<Value>, String> {
        let sup = self.r.get("sup").filter(|v| truthy(v)).cloned().unwrap_or_else(|| json!({}));
        if !sup.get("cause").map(truthy).unwrap_or(false) {
            return Ok(None);
        }
        let alive = self.d.alive.clone();
        let mut pool: Vec<(Value, i64, u32)> = Vec::new();
        for p in &alive {
            let s = vs(p, "s");
            if (s == "u" || s == "s") && !self.d.spot.contains(&vi(p, "i")) {
                let lack = self.mv(p, "lack")?;
                let ld = if truthy(&lack) { lack.get("d").cloned() } else { None };
                if ld.map(|x| int_of(&x)) == Some(self.day) {
                    pool.push((p.clone(), self.d.used.get(&format!("P{}", vi(p, "i"))).copied().unwrap_or(-999), hv(&["lack".to_string(), self.day.to_string(), vs(p, "n").to_string()])));
                }
            }
        }
        pool.sort_by_key(|x| (x.1, x.2));
        let a = match pool.first() {
            Some(x) => x.0.clone(),
            None => return Ok(None),
        };
        let lack = self.mv(&a, "lack")?;
        let c = pystr(lack.get("c").ok_or("KeyError: c")?);
        let short = sup.get("short").ok_or("KeyError: short")?;
        let short_c = short.get(&c).map(int_of).unwrap_or(0);
        let days_c = sup.get("days").filter(|v| truthy(v)).and_then(|d| d.get(&c)).map(int_of).unwrap_or(0);
        let stock_c = sup.get("stock").filter(|v| truthy(v)).and_then(|d| d.get(&c)).map(int_of).unwrap_or(0);
        let v = vmap(vec![
            ("lack_n", json!(count_p(if short_c != 0 { short_c } else { 1 }, "unit", "units"))),
            ("lack_days", json!(count_p(days_c, "day", "days"))),
            ("stock_n", json!(stock_c)),
        ]);
        let partner = self.partner(&a, &BTreeSet::new(), false)?;
        let mut people = people1("A", &a);
        if let Some(b) = partner {
            people["B"] = b;
        }
        self.entry("lack", "lack.title", &[part(&format!("lack.{}", c), true), part("lack.tail", false)], people, &v, Some("worried"), None)
    }

    // ------------------------------------------------------------ text
    // py: clan_saga.py:271 has_fresh
    /// True when every pool still has a template not used inside the variety window.
    fn has_fresh(&mut self, pools: &[&str]) -> bool {
        let day = self.day;
        let fk = self.fk;
        let used = self.cs().get("sg_used").cloned().unwrap_or_else(|| json!({}));
        pools.iter().all(|pool| POOLS.get(*pool).map(|v| v.as_slice()).unwrap_or(&[]).iter().any(|l| (l.scope == "*" || l.scope == fk) && day - used.get(&l.hs).map(int_of).unwrap_or(-999) >= WINDOW))
    }

    // py: clan_saga.py:275 pl_text
    fn pl_text(pool: &str, text: &str) -> String {
        pls::REG.get(&(pool.to_string(), text.to_string())).cloned().unwrap_or_default()
    }

    fn vars_of(v: &Map<String, Value>) -> Vars {
        v.iter().filter(|(_, x)| !x.is_null()).map(|(k, x)| (k.clone(), pystr(x))).collect()
    }

    // py: clan_saga.py:282 line
    /// One English sentence of the pool (None when nothing fits); the Polish twin of it is left in self.pl_last (None when it could not be made).
    fn line(&mut self, pool: &str, people: &Value, v: &Map<String, Value>, must: bool) -> Result<Option<String>, String> {
        self.pl_last = None;
        let vars_ = Self::vars_of(v);
        let fk = self.fk;
        let day = self.day;
        let mut cands: Vec<&'static PoolLine> = Vec::new();
        let pmap = people.as_object();
        for l in POOLS.get(pool).map(|x| x.as_slice()).unwrap_or(&[]) {
            if (l.scope == "*" || l.scope == fk) && l.pk.iter().all(|c| pmap.map(|m| m.contains_key(&c.to_string())).unwrap_or(false)) && l.vk.iter().all(|k| vars_.contains_key(k)) {
                if !cands.iter().any(|c| c.text == l.text) && kd::kid_safe(&l.text, people, &[Self::pl_text(pool, &l.text).as_str()]) {
                    let ok = if !kd::any_kid(people) {
                        true
                    } else {
                        let r = cs_::render(&l.text, people, &vars_).ok_or("KeyError: render")?;
                        kd::kid_text_ok(&they_fix(&r), people, "en")
                    };
                    if ok {
                        cands.push(l);
                    }
                }
            }
        }
        if cands.is_empty() {
            return Ok(None);
        }
        let used = self.cs().get("sg_used").cloned().unwrap_or_else(|| json!({}));
        let age = |l: &PoolLine| used.get(&l.hs).map(int_of).unwrap_or(-999);
        let mut fresh: Vec<&'static PoolLine> = cands.iter().copied().filter(|l| day - age(l) >= WINDOW).collect();
        if fresh.is_empty() {
            if !must {
                return Ok(None);
            }
            let mut s = cands.clone();
            s.sort_by(|a, b| (age(a), &a.text).cmp(&(age(b), &b.text)));
            fresh = vec![s[0]];
        }
        fresh.sort_by(|a, b| a.text.cmp(&b.text));
        let t = *re(self.rng.choice(&fresh))?;
        if day - age(t) < WINDOW {
            let cs = self.cs();
            let sp = cs.as_object_mut().ok_or("cs")?.entry("sg_stale_pools").or_insert_with(|| json!({}));
            let spm = sp.as_object_mut().ok_or("sg_stale_pools")?;
            let n = spm.get(pool).map(int_of).unwrap_or(0);
            spm.insert(pool.to_string(), json!(n + 1));
            let n2 = cs.get("sg_stale").map(int_of).unwrap_or(0);
            cs["sg_stale"] = json!(n2 + 1);
        }
        omap(self.cs(), "sg_used")?.insert(t.hs.clone(), json!(day));
        self.tids.push(hv(&[t.text.clone()]));
        self.pl_last = self.pl_line(pool, &t.text, people, v)?;
        let r = cs_::render(&t.text, people, &vars_).ok_or("KeyError: render")?;
        Ok(Some(they_fix(&r)))
    }

    // py: clan_saga.py:307 pl_line
    /// The Polish rendering of the English template `en_tpl` of `pool` with the same people and variables; None when there is a hole (outside the strict test mode).
    fn pl_line(&mut self, pool: &str, en_tpl: &str, people: &Value, v: &Map<String, Value>) -> Result<Option<String>, String> {
        let res = (|| -> Result<String, String> {
            let mut over: Vars = Vars::new();
            if let Some(x) = v.get("loot_list_pl") {
                over.insert("loot_list".into(), pystr(x));
            }
            for k_ in ["finds_text", "kfind"] {
                if let Some(x) = v.get(&format!("{}_pl", k_)) {
                    over.insert(k_.into(), pystr(x));
                }
            }
            let vp = pls::expand(v, people, self.fk, if over.is_empty() { None } else { Some(&over) })?;
            let tpl = pls::REG.get(&(pool.to_string(), en_tpl.to_string())).ok_or("KeyError: REG")?;
            pls::render(tpl, people, &vp).ok_or_else(|| "KeyError: render".to_string())
        })();
        match res {
            Ok(s) => Ok(Some(s)),
            Err(e) => {
                if self.strict {
                    Err(e)
                } else {
                    Ok(None)
                }
            }
        }
    }

    // py: clan_saga.py:324 prune
    fn prune(&mut self) -> Result<(), String> {
        let day = self.day;
        let used = omap(self.cs(), "sg_used")?;
        if used.len() > 900 {
            let old: Vec<String> = used.iter().filter(|(_, v)| day - int_of(v) > 3 * WINDOW).map(|(k, _)| k.clone()).collect();
            for k in old {
                used.shift_remove(&k);
            }
        }
        Ok(())
    }

    // py: clan_saga.py:332 entry
    /// title = pool; parts = [(pool, must)]; people = {"A": person, ..}; extra = vars. None when the title or the body has nothing to say.
    fn entry(&mut self, scene: &str, title: &str, parts: &[(String, bool)], people: Value, extra: &Map<String, Value>, mood: Option<&str>, thread: Option<String>) -> Result<Option<Value>, String> {
        let mut people = people;
        let mut v = self.base.clone();
        for (k, x) in extra {
            if !x.is_null() {
                v.insert(k.clone(), x.clone());
            }
        }
        self.tids = Vec::new();
        let t = match self.line(title, &people, &v, true)? {
            Some(t) if !t.is_empty() => t,
            _ => return Ok(None),
        };
        let t_pl = self.pl_last.clone();
        let mut body: Vec<String> = Vec::new();
        let mut body_pl: Vec<Option<String>> = Vec::new();
        for (pool, must) in parts {
            if let Some(s) = self.line(pool, &people, &v, *must)? {
                if !s.is_empty() {
                    body.push(s);
                    body_pl.push(self.pl_last.clone());
                }
            }
        }
        if body.is_empty() {
            return Ok(None);
        }
        let nsent: usize = body.iter().map(|b| n_sentences(py::strip(b))).sum();
        if nsent < 3 && (nsent < 2 || self.rng.random() < 0.5) {
            // the memory scenes are short: a tail makes them a small scene
            if people.get("B").is_none() && people.get("A").is_some() {
                let a = people["A"].clone();
                let b = self.partner(&a, &BTreeSet::new(), kd::is_combat_scene_saga(scene))?;
                if let Some(b) = b {
                    let bi = b.get("i").cloned();
                    let in_people = people.as_object().map(|o| o.values().any(|x| x.get("i") == bi.as_ref())).unwrap_or(false);
                    if !in_people {
                        people["B"] = b;
                    }
                }
            }
            let tl = self.line(tail_of(scene), &people, &v, false)?;
            if let Some(tl) = tl.filter(|x| !x.is_empty()) {
                body.push(tl);
                body_pl.push(self.pl_last.clone());
            }
        }
        let t = t.trim_end_matches(|c| matches!(c, '.' | '!' | '?' | ' '));
        let text = format!("DAY {}: {}. {}", self.day, t.to_uppercase(), body.join(" "));
        let mut text_pl: Option<String> = None;
        if let Some(tp) = t_pl.as_ref().filter(|x| !x.is_empty()) {
            if body_pl.iter().all(|b| b.as_ref().map(|x| !x.is_empty()).unwrap_or(false)) {
                text_pl = Some(format!("DZIEŃ {}: {}. {}", self.day, tp.trim_end_matches(|c| matches!(c, '.' | '!' | '?' | ' ')).to_uppercase(), body_pl.iter().map(|b| b.clone().unwrap_or_default()).collect::<Vec<_>>().join(" ")));
            }
        }
        let pmap = people.as_object().ok_or("people")?;
        let mut pp = Map::new();
        for (k, x) in pmap {
            pp.insert(k.clone(), json!([x.get("n").cloned().ok_or("KeyError: n")?, x.get("g").cloned().ok_or("KeyError: g")?, x.get("r").cloned().unwrap_or_else(|| json!("scavenger"))]));
        }
        let names: Vec<Value> = pmap.values().filter(|x| has_word(&text, &pystr(x.get("n").unwrap_or(&Value::Null)))).map(|x| x["n"].clone()).collect();
        let mut s = Map::new();
        s.insert("f".into(), json!(self.fk));
        s.insert("day".into(), json!(self.day));
        if truthy(self.f.get("map").unwrap_or(&Value::Null)) {
            s.insert("map".into(), self.f["map"].clone());
        }
        let mut params = Map::new();
        params.insert("p".into(), Value::Object(pp));
        params.insert("s".into(), Value::Object(s));
        params.insert("t".into(), json!(text));
        params.insert("i".into(), json!(self.tids));
        if let Some(tp) = text_pl {
            params.insert("tp".into(), json!(tp));
        }
        if let Some(th) = &thread {
            if !th.is_empty() {
                params.insert("a".into(), json!(th));
            }
        }
        let scene_d = self.day;
        for x in pmap.values() {
            if x.get("i").is_some() {
                let xi = vi(x, "i");
                self.d.used.insert(format!("P{}", xi), self.day);
                self.d.spot.insert(xi);
                let m = self.m(x)?;
                m.insert("last".into(), json!(scene_d));
                m.insert("thread".into(), json!({"k": scene, "d": scene_d, "a": thread.as_ref().map(|t| json!(t)).unwrap_or(Value::Null)}));
            }
        }
        let mut row = Map::new();
        row.insert("kind".into(), json!("chron"));
        row.insert("mood".into(), json!(mood.or_else(|| mood_of(scene)).unwrap_or("calm")));
        row.insert("text".into(), json!(text));
        row.insert("names".into(), Value::Array(names));
        row.insert("lid".into(), json!(format!("sg/{}", scene)));
        row.insert("params".into(), Value::Object(params));
        Ok(Some(Value::Object(row)))
    }

    // ------------------------------------------------------------ vars
    // py: clan_saga.py:383 match_vars
    fn match_vars(&mut self) -> Result<Map<String, Value>, String> {
        let f = self.f.clone();
        let md = map_data(fs(&f, "map"));
        let mut pl = places_of(md);
        self.rng.shuffle(&mut pl);
        let air_l = md.get("air").and_then(|a| a.as_array()).ok_or("air")?;
        let air = re(self.rng.choice(air_l))?;
        let air_s = format!("the {} of {}", if pystr(&air[0]) == "s" { "smell" } else { "sound" }, pystr(&air[1]));
        if pl.len() < 3 {
            return Err("IndexError: places".into());
        }
        let mut v = vmap(vec![
            ("map", json!(map_art(fs(&f, "map")))),
            ("place", json!(pl[0])),
            ("place2", json!(pl[1])),
            ("place3", json!(pl[2])),
            ("air", json!(air_s)),
        ]);
        let sm: Vec<&Value> = air_l.iter().filter(|a| pystr(&a[0]) == "s").collect();
        let sm: Vec<&Value> = if sm.is_empty() { air_l.iter().collect() } else { sm };
        let sd_: Vec<&Value> = air_l.iter().filter(|a| pystr(&a[0]) == "h").collect();
        let sd_: Vec<&Value> = if sd_.is_empty() { air_l.iter().collect() } else { sd_ };
        let a = re(self.rng.choice(&sm))?;
        v.insert("smell".into(), json!(format!("the smell of {}", pystr(&a[1]))));
        let a = re(self.rng.choice(&sd_))?;
        v.insert("sound".into(), json!(format!("the sound of {}", pystr(&a[1]))));
        if fi(&f, "k") != 0 {
            v.insert("k".into(), json!(fi(&f, "k")));
        }
        if fi(&f, "ex") != 0 {
            v.insert("ex_p".into(), json!(count_p(fi(&f, "ex"), "execution", "executions")));
            v.insert("ex_t".into(), json!(times_p(fi(&f, "ex"))));
        }
        if fi(&f, "rv") != 0 {
            v.insert("rv_p".into(), json!(count_p(fi(&f, "rv"), "teammate", "teammates")));
        }
        if fi(&f, "hs") != 0 {
            v.insert("hs_p".into(), json!(count_p(fi(&f, "hs"), "headshot", "headshots")));
        }
        if fi(&f, "dn") != 0 {
            v.insert("dn_p".into(), json!(times_p(fi(&f, "dn"))));
        }
        if fi(&f, "dt") != 0 {
            v.insert("dt_p".into(), json!(times_p(fi(&f, "dt"))));
        }
        if let Some(w) = f.get("weapon").and_then(|w| w.as_str()) {
            v.insert("wpn".into(), sd("WEAPONS").get(w).and_then(|x| x.get(0)).cloned().ok_or("KeyError: weapon")?);
        }
        if !fs(&f, "map").is_empty() {
            v.insert("place_gen".into(), json!(pl[0]));
        }
        Ok(v)
    }

    // py: clan_saga.py:413 person
    /// The member who stars in a scene (spotlight-fair). fight=true: a match, raid, haul, patrol or weapon scene - grown people only, never a kid.
    fn person(&mut self, roles: &[&str], exclude: &BTreeSet<i64>, states: &[&str], fight: bool) -> Result<Option<Value>, String> {
        let alive = self.d.alive.clone();
        let mut pool: Vec<Value> = alive
            .into_iter()
            .filter(|p| states.contains(&vs(p, "s")) && !self.d.spot.contains(&vi(p, "i")) && !exclude.contains(&vi(p, "i")) && (!fight || kd::can_fight(p)))
            .collect();
        if pool.is_empty() {
            return Ok(None);
        }
        let pref: Vec<Value> = pool.iter().filter(|p| roles.contains(&vs(p, "r"))).cloned().collect();
        if !pref.is_empty() {
            pool = pref;
        }
        let mut keyed: Vec<(i64, i64, u32, Value)> = Vec::new();
        for p in pool {
            let last = int_of(&self.mv(&p, "last")?);
            keyed.push((self.d.used.get(&format!("P{}", vi(&p, "i"))).copied().unwrap_or(-999), last, hv(&["sg".to_string(), self.day.to_string(), vs(&p, "n").to_string()]), p));
        }
        keyed.sort_by_key(|x| (x.0, x.1, x.2));
        Ok(keyed.into_iter().next().map(|x| x.3))
    }

    fn person_any(&mut self, fight: bool) -> Result<Option<Value>, String> {
        self.person(&[], &BTreeSet::new(), &["h", "u", "s"], fight)
    }

    // py: clan_saga.py:424 partner
    fn partner(&mut self, a: &Value, exclude: &BTreeSet<i64>, fight: bool) -> Result<Option<Value>, String> {
        let b = self.mv(a, "bond")?;
        let q = self.d.alive.iter().find(|p| p.get("i") == Some(&b) && !b.is_null()).cloned();
        if let Some(q) = q {
            if !self.d.spot.contains(&vi(&q, "i")) && !exclude.contains(&vi(&q, "i")) && (!fight || kd::can_fight(&q)) {
                return Ok(Some(q));
            }
        }
        let mut ex = exclude.clone();
        ex.insert(vi(a, "i"));
        self.person(&[], &ex, &["h", "u", "s"], fight)
    }

    // ------------------------------------------------------------ the match story
    // py: clan_saga.py:432 match_story
    fn match_story(&mut self) -> Result<Option<Value>, String> {
        let f = self.f.clone();
        let day = self.day;
        let mut v = self.match_vars()?;
        let hist: Vec<Value> = self.cs().get("sg_hist").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        let prev = hist.last().cloned();
        let win: Option<bool> = f.get("win").and_then(|w| w.as_bool());
        let mut kinds: Vec<(&str, f64)> = Vec::new();
        if win == Some(false) {
            kinds.push(("loss", 5.0));
        }
        if fi(&f, "dn") >= 2 {
            kinds.push(("down", if win != Some(false) { 5.0 } else { 3.0 }));
        }
        if fi(&f, "ex") >= 1 {
            kinds.push(("exec", (3 + 3.min(fi(&f, "ex"))) as f64));
        }
        if fi(&f, "rv") >= 2 {
            kinds.push(("rev", 3.0));
        }
        if win == Some(true) {
            kinds.push(("win", 4.0));
        }
        if kinds.is_empty() {
            kinds.push(("quiet", 3.0));
        }
        let sg_last = pystr(self.cs().get("sg_last").unwrap_or(&Value::Null));
        if !sg_last.is_empty() && kinds.len() > 1 {
            let k2: Vec<(&str, f64)> = kinds.iter().filter(|x| x.0 != sg_last).copied().collect();
            if !k2.is_empty() {
                kinds = k2;
            }
        }
        let ws: Vec<f64> = kinds.iter().map(|x| x.1).collect();
        let scene = kinds[re(self.rng.choices_weighted(&ws, 1))?[0]].0;
        self.cs()["sg_last"] = json!(scene);
        let role: &[&str] = match scene {
            "down" => &["medic"],
            "loss" => &["cook", "elder", "guard"],
            "win" => &["cook", "elder"],
            "exec" => &["medic", "elder", "cook"],
            "rev" => &["scout", "medic"],
            _ => &["cook", "guard", "scavenger"],
        };
        let a = match self.person(role, &BTreeSet::new(), &["h", "u", "s"], true)? {
            Some(a) => a,
            None => return Ok(None),
        };
        let b = self.partner(&a, &BTreeSet::new(), true)?;
        let mut people = people1("A", &a);
        if let Some(b) = b {
            people["B"] = b;
        }
        // the previous days
        let fmap = fs(&f, "map").to_string();
        let hist_maps = hist.iter().filter(|h| day - vi(h, "d") <= 7 && pystr(h.get("map").unwrap_or(&Value::Null)) == fmap).count();
        if let Some(p) = &prev {
            v.insert("prev_map".into(), json!(map_art(vs(p, "map"))));
            let pw = p.get("win");
            v.insert("prev_res".into(), json!(if pw.map(truthy).unwrap_or(false) { "a win" } else if pw == Some(&json!(false)) { "a loss" } else { "a plain day" }));
        }
        if hist_maps >= 1 && !fmap.is_empty() {
            let n = hist_maps as i64 + 1;
            let ord = match n {
                2 => "second",
                3 => "third",
                4 => "fourth",
                5 => "fifth",
                6 => "sixth",
                7 => "seventh",
                _ => "again",
            };
            v.insert("nth".into(), json!(ord));
        }
        let mut lose_n = 0i64;
        for h in hist.iter().rev() {
            if h.get("win") == Some(&json!(false)) {
                lose_n += 1;
            } else {
                break;
            }
        }
        if win == Some(false) {
            lose_n += 1;
        }
        if lose_n >= 2 {
            v.insert("lossw".into(), json!(nw(lose_n)));
        }
        let yi = self.cs().get("you").and_then(|y| y.get("inj")).cloned().unwrap_or(Value::Null);
        if truthy(&yi) && day - vi(&yi, "d") >= 1 && day - vi(&yi, "d") <= 12 {
            v.insert("ysite".into(), yi.get("site").cloned().ok_or("KeyError: site")?);
            v.insert("ymap".into(), json!(map_art(vs(&yi, "map"))));
            v.insert("yd".into(), yi.get("d").cloned().ok_or("KeyError: d")?);
        }
        let sites = sd("SITES").as_array().ok_or("SITES")?;
        let site = re(self.rng.choice(sites))?.clone();
        let cloths = sd("CLOTHS").as_array().ok_or("CLOTHS")?;
        let cloth = re(self.rng.choice(cloths))?.clone();
        v.insert("site".into(), site.clone());
        v.insert("cloth".into(), cloth.clone());
        let mut parts: Parts = vec![part(&format!("{}.event", scene), true)];
        match scene {
            "down" => {
                parts.push(part("down.care", true));
                let pick = v.contains_key("ysite") && self.rng.random() < 0.6;
                parts.push(part(if pick { "down.you" } else { "down.after" }, true));
            }
            "loss" => {
                parts.push(part("loss.camp", true));
                parts.push(part(if lose_n >= 2 { "loss.streak" } else { "loss.b" }, true));
                if lose_n < 2 && self.rng.random() < 0.4 && v.contains_key("ysite") {
                    parts.push(part("down.you", false));
                }
            }
            "win" => {
                parts.push(part(if f.get("hs").map(truthy).unwrap_or(false) { "win.hs" } else { "win.camp" }, true));
                if prev.as_ref().map(|p| p.get("win") == Some(&json!(false))).unwrap_or(false) && v.contains_key("prev_map") {
                    parts.push(part("win.after_loss", true));
                } else {
                    parts.push(part("win.camp", true));
                }
            }
            "exec" => parts.push(part("exec.react", true)),
            "rev" => parts.push(part("rev.react", true)),
            _ => parts.push(part("quiet.camp", true)),
        }
        if win.is_some() && scene != "win" && scene != "loss" && self.rng.random() < 0.5 {
            parts.insert(1, part(if win == Some(true) { "res.win" } else { "res.loss" }, false));
        }
        if v.contains_key("nth") && self.rng.random() < 0.5 {
            parts.push(part("bridge.same_map", false));
        } else if v.contains_key("ysite") && scene != "down" && scene != "loss" && self.rng.random() < 0.3 {
            parts.push(part("bridge.you_scar", false));
        }
        // a raider is hurt on a bad day: the injury goes into the memory and comes back in later entries
        let mut hurt: Option<Value> = None;
        if (win == Some(false) || fi(&f, "dn") >= 2 || fi(&f, "dt") >= 3) && self.rng.random() < 0.7 {
            let alive = self.d.alive.clone();
            let mut cand: Vec<Value> = Vec::new();
            for p in &alive {
                let r = vs(p, "r");
                if vi(p, "i") != vi(&a, "i") && r != "kid" && r != "elder" && !self.d.spot.contains(&vi(p, "i")) && !truthy(&self.mv(p, "inj")?) && vs(p, "s") != "s" {
                    cand.push(p.clone());
                }
            }
            if !cand.is_empty() {
                cand.sort_by_key(|p| vi(p, "i"));
                hurt = Some(re(self.rng.choice(&cand))?.clone());
            }
        }
        if let Some(h) = &hurt {
            people["B"] = h.clone();
            let causes = sd("CAUSES").as_array().ok_or("CAUSES")?;
            let c = re(self.rng.choice(causes))?;
            let en_n = pystr(self.base.get("enemy_n").unwrap_or(&Value::Null));
            let cause = pyformat(c.as_str().unwrap_or(""), &[("enemy_n", &en_n)])?;
            v.insert("cause".into(), json!(cause));
            v.insert("job".into(), json!(sd("JOBS").get(vs(h, "r")).and_then(|x| x.as_str()).unwrap_or("work")));
            parts.insert(2, part("hurt.line", true));
        } else if self.rng.random() < 0.3 && people.get("B").is_some() {
            parts.push(part("mark.line", false));
        }
        if self.rng.random() < 0.28 {
            let tem = self.mv(&a, "temper")?;
            let tem = if tem.is_null() { "calm".to_string() } else { pystr(&tem) };
            parts.push(part(&format!("quip.{}.{}", if win == Some(true) || scene == "win" || scene == "rev" { "good" } else { "bad" }, tem), false));
        }
        let row = match self.entry(scene, &format!("{}.title", scene), &parts, people, &v, None, None)? {
            Some(r) => r,
            None => return Ok(None),
        };
        if let Some(h) = &hurt {
            let heal = day + re(self.rng.randint(3, 6))?;
            let cause = v.get("cause").cloned().unwrap_or_else(|| json!(""));
            self.mset(h, "inj", json!({"site": site, "cloth": cloth, "d": day, "heal": heal, "map": fmap, "cause": cause}))?;
        }
        if scene == "down" || fi(&f, "dn") >= 2 {
            self.cs()["you"]["inj"] = json!({"site": site, "d": day, "map": fmap});
        }
        let hl = varr(self.cs(), "sg_hist")?;
        hl.push(json!({"d": day, "map": fmap, "win": f.get("win").cloned().unwrap_or(Value::Null), "dn": f.get("dn").cloned().unwrap_or(Value::Null)}));
        let n = hl.len();
        if n > 14 {
            hl.drain(..n - 14);
        }
        Ok(Some(row))
    }

    // ------------------------------------------------------------ loot
    // py: clan_saga.py:544 book_loot
    /// Remember where the loot was found: sg_stock (practical goods) and sg_keep (keepsakes).
    fn book_loot(&mut self, loot: &[Value]) -> Result<Vec<(String, Value, String)>, String> {
        let pl = places_of(map_data(fs(&self.f, "map")));
        let mut out = Vec::new();
        let fmap = fs(&self.f, "map").to_string();
        let day = self.day;
        for kn in loot {
            let key = pystr(kn.get(0).ok_or("IndexError")?);
            let n = kn.get(1).cloned().ok_or("IndexError")?;
            let where_ = re(self.rng.choice(&pl))?.clone();
            if ch::is_keep(&key) {
                omap(self.cs(), "sg_keep")?.insert(key.clone(), json!({"d": day, "m": fmap, "p": where_, "by": null}));
            } else {
                varr(self.cs(), "sg_stock")?.push(json!({"k": key, "n": int_of(&n), "d": day, "m": fmap, "p": where_}));
            }
            out.push((key, n, where_));
        }
        let sl = varr(self.cs(), "sg_stock")?;
        let n = sl.len();
        if n > 24 {
            sl.drain(..n - 24);
        }
        Ok(out)
    }

    // py: clan_saga.py:558 haul_story
    fn haul_story(&mut self, lp: &[(String, Value, String)]) -> Result<Option<Value>, String> {
        let mut v = self.match_vars()?;
        if !lp.is_empty() {
            v.insert("place".into(), json!(lp[0].2));
            if lp.len() > 1 {
                v.insert("place2".into(), json!(lp[1].2));
            }
        }
        let mut en: Vec<String> = Vec::new();
        let mut pl: Vec<String> = Vec::new();
        for (k, n, _p) in lp {
            en.push(ch::loot_phrase(k, int_of(n), "en")?);
        }
        for (k, n, _p) in lp {
            pl.push(ch::loot_phrase(k, int_of(n), "pl")?);
        }
        v.insert("loot_list".into(), json!(ch::join_list(&en, "en")));
        v.insert("loot_list_pl".into(), json!(ch::join_list(&pl, "pl")));
        let mut cats: BTreeSet<String> = BTreeSet::new();
        for (k, _n, _p) in lp {
            if let Some(u) = ch::use_row(k) {
                cats.insert(pystr(&u[0]));
            }
        }
        let keeps: Vec<&String> = lp.iter().filter(|(k, _, _)| ch::is_keep(k)).map(|(k, _, _)| k).collect();
        let role: &[&str] = if cats.contains("food") {
            &["cook"]
        } else if cats.contains("med") {
            &["medic"]
        } else if cats.contains("ammo") {
            &["scout", "guard"]
        } else {
            &["mechanic"]
        };
        let a = match self.person(role, &BTreeSet::new(), &["h", "u", "s"], true)? {
            Some(a) => a,
            None => return Ok(None),
        };
        let b = self.partner(&a, &BTreeSet::new(), true)?;
        let mut people = people1("A", &a);
        if let Some(b) = b {
            people["B"] = b;
        }
        let mut parts: Parts = vec![part("haul.found", true)];
        for (cat, pool) in [("food", "haul.food"), ("med", "haul.med"), ("ammo", "haul.ammo"), ("tool", "haul.tool")] {
            if cats.contains(cat) {
                parts.push(part(pool, false));
                break;
            }
        }
        let fl: Vec<Value> = self.d.finds.clone();
        if !fl.is_empty() {
            // the rich findings of the raid: who found what where, in what state (clan_loot)
            v.insert("finds_text".into(), json!(lt::finds_story(&fl, "en", 2)?));
            v.insert("finds_text_pl".into(), json!(lt::finds_story(&fl, "pl", 2)?));
            let fv = Value::Array(fl.clone());
            v.insert("loot_list".into(), json!(lt::items_text_res(&fv, "en")?));
            v.insert("loot_list_pl".into(), json!(lt::items_text_res(&fv, "pl")?));
            let ks = lt::keepsakes(&fl)?;
            let (day, fmap) = (self.day, fs(&self.f, "map").to_string());
            for f_ in ks.iter().take(2) {
                varr(self.cs(), "sg_kf")?.push(json!({"k": f_.get("k").cloned().ok_or("KeyError: k")?, "d": day, "m": fmap, "w": f_.get("w").cloned().unwrap_or_else(|| json!(""))}));
            }
            let kl = varr(self.cs(), "sg_kf")?;
            let n = kl.len();
            if n > 12 {
                kl.drain(..n - 12);
            }
            parts.insert(1, part("haul.finds", false));
        }
        if let Some(k0) = keeps.first() {
            let kr = ch::keep_row(k0).ok_or("KeyError: KEEP")?;
            v.insert("kitem_a".into(), kr[0].clone());
            parts.push(part(&format!("keep.{}", k0), true));
            parts.push(part("haul.keep", true));
        }
        let mood = if self.f.get("win") != Some(&json!(false)) { "cheerful" } else { "worried" };
        self.entry("haul", "haul.title", &parts, people, &v, Some(mood), None)
    }

    // ------------------------------------------------------------ memory scenes
    // py: clan_saga.py:598 scene_injury
    fn scene_injury(&mut self) -> Result<Option<Value>, String> {
        let day = self.day;
        let alive = self.d.alive.clone();
        for p in &alive {
            let inj = self.mv(p, "inj")?;
            if !truthy(&inj) || self.d.spot.contains(&vi(p, "i")) || day - vi(&inj, "d") < 1 {
                continue;
            }
            let healed = day >= vi(&inj, "heal");
            let kmap = map_art(vs(&inj, "map"));
            let v = vmap(vec![
                ("site", inj.get("site").cloned().ok_or("KeyError: site")?),
                ("cloth", inj.get("cloth").cloned().ok_or("KeyError: cloth")?),
                ("kmap", json!(if kmap.is_empty() { "raid".to_string() } else { kmap })),
                ("kd", inj.get("d").cloned().unwrap_or(Value::Null)),
                ("kd_ago", json!(ago(vi(&inj, "d"), day))),
                ("job", json!(sd("JOBS").get(vs(p, "r")).and_then(|x| x.as_str()).unwrap_or("work"))),
            ]);
            let pool = if healed { "inj.healed" } else { "inj.mid" };
            let row = self.entry("inj", "inj.title", &[part(pool, true)], people1("A", p), &v, Some(if healed { "hopeful" } else { "worried" }), Some(format!("sg-inj-{}-{}", vi(p, "i"), vi(&inj, "d"))))?;
            if row.is_some() {
                if healed {
                    let sc = json!({"site": inj.get("site").cloned().unwrap_or(Value::Null), "map": inj.get("map").cloned().unwrap_or(Value::Null), "d": inj.get("d").cloned().unwrap_or(Value::Null)});
                    let m = self.m(p)?;
                    m.get_mut("scars").and_then(|s| s.as_array_mut()).ok_or("scars")?.push(sc);
                    m.insert("inj".into(), Value::Null);
                }
                return Ok(row);
            }
        }
        Ok(None)
    }

    // py: clan_saga.py:618 scene_scar
    fn scene_scar(&mut self) -> Result<Option<Value>, String> {
        let alive = self.d.alive.clone();
        for p in &alive {
            let scars = self.mv(p, "scars")?;
            let last = scars.as_array().and_then(|a| a.last()).cloned();
            if let Some(l) = last {
                if !self.d.spot.contains(&vi(p, "i")) && self.day - vi(&l, "d") >= 6 && self.rng.random() < 0.5 {
                    let kmap = map_art(vs(&l, "map"));
                    let v = vmap(vec![("site", l.get("site").cloned().ok_or("KeyError: site")?), ("kmap", json!(if kmap.is_empty() { "raid".to_string() } else { kmap }))]);
                    return self.entry("inj", "inj.title", &[part("scar.cb", true)], people1("A", p), &v, Some("calm"), None);
                }
            }
        }
        Ok(None)
    }

    // py: clan_saga.py:626 scene_grief
    fn scene_grief(&mut self) -> Result<Option<Value>, String> {
        let day = self.day;
        let alive = self.d.alive.clone();
        for p in &alive {
            let g = self.mv(p, "grief")?;
            if !truthy(&g) || self.d.spot.contains(&vi(p, "i")) {
                continue;
            }
            if day > vi(&g, "until") {
                self.mset(p, "grief", Value::Null)?;
                continue;
            }
            if day - vi(&g, "echo") < 2 {
                continue;
            }
            let gfor = g.get("for").cloned().ok_or("KeyError: for")?;
            let dead = self.cs().get("gone").and_then(|m| m.get(&pystr(&gfor))).cloned();
            let dead = match dead.filter(|v| truthy(v)) {
                Some(d) => d,
                None => {
                    self.mset(p, "grief", Value::Null)?;
                    continue;
                }
            };
            let dp = json!({"n": dead.get("n").cloned().unwrap_or(Value::Null), "g": dead.get("g").cloned().unwrap_or(Value::Null), "r": dead.get("r").cloned().unwrap_or(Value::Null)});
            let dm = dead.get("m").cloned().unwrap_or(Value::Null);
            let n = day - vi(&dead, "d");
            let poss = dm.get("poss").filter(|v| truthy(v)).cloned().unwrap_or_else(|| json!("a small keepsake"));
            let v = vmap(vec![("dgo", json!(if n <= 1 { "gone since yesterday".to_string() } else { format!("gone {} days", nw(n)) })), ("dpos", poss)]);
            let mut people = people1("A", p);
            people["D"] = dp;
            let row = self.entry("grief", "grief.title", &[part("grief.line", true)], people, &v, None, Some(format!("sg-grief-{}", int_of(&gfor))))?;
            if row.is_some() {
                self.m(p)?.get_mut("grief").and_then(|x| x.as_object_mut()).ok_or("grief")?.insert("echo".into(), json!(day));
                return Ok(row);
            }
        }
        Ok(None)
    }

    // py: clan_saga.py:653 scene_rel
    fn scene_rel(&mut self) -> Result<Option<Value>, String> {
        let alive = self.d.alive.clone();
        let mut cand: Vec<(Value, Value, String, i64, i64, u32)> = Vec::new();
        for p in &alive {
            if self.d.spot.contains(&vi(p, "i")) {
                continue;
            }
            let bond = self.mv(p, "bond")?;
            let q = alive.iter().find(|x| !bond.is_null() && x.get("i") == Some(&bond)).cloned();
            let bt = self.mv(p, "bt")?;
            let q = match q {
                Some(q) if !self.d.spot.contains(&vi(&q, "i")) && truthy(&bt) => q,
                _ => continue,
            };
            let rel = self.mv(p, "rel")?;
            let lvl = rel.get(&pystr(q.get("i").unwrap_or(&Value::Null))).map(int_of).unwrap_or(0);
            if lvl >= 4 {
                continue;
            }
            let last = int_of(&self.mv(p, "last")?);
            let hh = hv(&["rel".to_string(), self.day.to_string(), vs(p, "n").to_string()]);
            cand.push((p.clone(), q, pystr(&bt), lvl + 1, last, hh));
        }
        if cand.is_empty() {
            return Ok(None);
        }
        cand.sort_by_key(|x| (x.4, x.5));
        let (p, q, bt, lvl, _, _) = cand[0].clone();
        let pool = format!("rel.{}.{}", bt, lvl);
        if !POOLS.contains_key(&pool) {
            return Ok(None);
        }
        let v = if !fs(&self.f, "map").is_empty() { self.match_vars()? } else { vmap(vec![("map", json!("the yard"))]) };
        let (pi, qi) = (vi(&p, "i"), vi(&q, "i"));
        let mut people = people1("A", &p);
        people["B"] = q.clone();
        let row = self.entry("rel", "rel.title", &[part(&pool, true)], people, &v, None, Some(format!("sg-rel-{}-{}", pi.min(qi), pi.max(qi))))?;
        if row.is_some() {
            let rel = self.m(&p)?.get_mut("rel").and_then(|x| x.as_object_mut()).ok_or("rel")?;
            rel.insert(qi.to_string(), json!(lvl));
            let rel = self.m(&q)?.get_mut("rel").and_then(|x| x.as_object_mut()).ok_or("rel")?;
            rel.insert(pi.to_string(), json!(lvl));
        }
        Ok(row)
    }

    // py: clan_saga.py:680 scene_keep_callback
    fn scene_keep_callback(&mut self) -> Result<Option<Value>, String> {
        let day = self.day;
        let keep: Vec<(String, Value)> = self.cs().get("sg_keep").and_then(|k| k.as_object()).map(|o| o.iter().map(|(k, e)| (k.clone(), e.clone())).collect()).unwrap_or_default();
        let mut olds: Vec<(String, Value)> = keep.into_iter().filter(|(_, e)| vi(e, "d") < day && day - vi(e, "d") <= 30).collect();
        if olds.is_empty() {
            return Ok(None);
        }
        olds.sort_by(|a, b| a.0.cmp(&b.0));
        let (k, e) = re(self.rng.choice(&olds))?.clone();
        let pool = format!("keepcb.{}", k);
        if !POOLS.contains_key(&pool) {
            return Ok(None);
        }
        let role: &[&str] = if matches!(k.as_str(), "chess" | "teddy" | "comic") { &["kid"] } else { &[] };
        let a = match self.person(role, &BTreeSet::new(), &["h", "u", "s"], false)? {
            Some(a) => a,
            None => return Ok(None),
        };
        let b = self.partner(&a, &BTreeSet::new(), false)?;
        let mut people = people1("A", &a);
        if let Some(b) = b {
            people["B"] = b;
        }
        let kmap = map_art(vs(&e, "m"));
        let kr = ch::keep_row(&k).ok_or("KeyError: KEEP")?;
        let item = pystr(&kr[0]);
        let kitem = item.split_once(' ').map(|x| x.1.to_string()).ok_or("IndexError: kitem")?;
        let v = vmap(vec![
            ("kd", e.get("d").cloned().ok_or("KeyError: d")?),
            ("kmap", json!(if kmap.is_empty() { "a raid".to_string() } else { kmap })),
            ("kplace", e.get("p").cloned().ok_or("KeyError: p")?),
            ("kitem", json!(kitem)),
        ]);
        self.entry("keepcb", "keepcb.title", &[part(&pool, true)], people, &v, None, None)
    }

    // py: clan_saga.py:699 scene_keep_find
    /// A trinket or book found on an earlier raid (clan_loot keepsakes) comes back in a small scene.
    fn scene_keep_find(&mut self) -> Result<Option<Value>, String> {
        if !self.has_fresh(&["keepfind.title", "keepfind.line"]) {
            return Ok(None);
        }
        let day = self.day;
        let kf: Vec<Value> = self.cs().get("sg_kf").and_then(|k| k.as_array()).cloned().unwrap_or_default();
        let olds: Vec<Value> = kf.into_iter().filter(|e| day - vi(e, "d") > 0 && day - vi(e, "d") <= 30 && lt::ITEMS.contains_key(vs(e, "k"))).collect();
        if olds.is_empty() {
            return Ok(None);
        }
        let e = re(self.rng.choice(&olds))?.clone();
        let it = lt::item(vs(&e, "k"))?;
        let kid = (it.cat == "trink" || it.cat == "book") && self.rng.random() < 0.4;
        let a = match self.person(if kid { &["kid"] } else { &[] }, &BTreeSet::new(), &["h", "u", "s"], false)? {
            Some(a) => a,
            None => return Ok(None),
        };
        let b = self.partner(&a, &BTreeSet::new(), false)?;
        let mut people = people1("A", &a);
        if let Some(b) = b {
            people["B"] = b;
        }
        let kmap = map_art(vs(&e, "m"));
        let v = vmap(vec![
            ("kd", e.get("d").cloned().ok_or("KeyError: d")?),
            ("kmap", json!(if kmap.is_empty() { "a raid".to_string() } else { kmap })),
            ("kfind", json!(it.en1)),
            ("kfind_pl", json!(it.pln)),
        ]);
        self.entry("keepfind", "keepfind.title", &[part("keepfind.line", true)], people, &v, None, None)
    }

    // py: clan_saga.py:718 scene_depressed
    /// A member who is depressed (clan_loot.update_moods) - or one who has just come back from it - gets a quiet scene.
    fn scene_depressed(&mut self) -> Result<Option<Value>, String> {
        let dep = self.cs().get("dep").filter(|v| truthy(v)).cloned().unwrap_or_else(|| json!({}));
        if !self.has_fresh(&["dep.title"]) {
            return Ok(None);
        }
        let day = self.day;
        let alive = self.d.alive.clone();
        let mut cand: Vec<(Value, &'static str)> = Vec::new();
        for p in &alive {
            let e = match dep.get(&pystr(p.get("i").unwrap_or(&Value::Null))).filter(|v| truthy(v)) {
                Some(e) => e.clone(),
                None => continue,
            };
            if self.d.spot.contains(&vi(p, "i")) {
                continue;
            }
            let depd = self.mv(p, "depd")?;
            let depd = if depd.is_null() { -99 } else { int_of(&depd) };
            if e.get("n").map(int_of).ok_or("KeyError: n")? >= 4 && day - depd >= 4 {
                cand.push((p.clone(), "dep.line"));
            } else if e.get("up").map(|u| int_of(u) == day && !u.is_null()).unwrap_or(false) && depd != day {
                cand.push((p.clone(), "dep.up"));
            }
        }
        if cand.is_empty() {
            return Ok(None);
        }
        let mut cand2: Vec<(Value, &'static str)> = Vec::new();
        for c in cand {
            if self.has_fresh(&[c.1]) {
                cand2.push(c);
            }
        }
        if cand2.is_empty() {
            return Ok(None);
        }
        cand2.sort_by_key(|c| (c.1 != "dep.up", self.d.used.get(&format!("P{}", vi(&c.0, "i"))).copied().unwrap_or(-999), vi(&c.0, "i")));
        let (a, pool) = cand2[0].clone();
        self.mset(&a, "depd", json!(day))?;
        let b = self.partner(&a, &BTreeSet::new(), false)?;
        let mut people = people1("A", &a);
        if let Some(b) = b {
            people["B"] = b;
        }
        self.entry("dep", "dep.title", &[part(pool, true)], people, &Map::new(), Some(if pool == "dep.line" { "grieving" } else { "hopeful" }), None)
    }

    // py: clan_saga.py:747 scene_use
    fn scene_use(&mut self) -> Result<Option<Value>, String> {
        let day = self.day;
        let stock: Vec<Value> = self.cs().get("sg_stock").and_then(|k| k.as_array()).cloned().unwrap_or_default();
        let cands: Vec<(usize, Value)> = stock.into_iter().enumerate().filter(|(_, e)| vi(e, "n") > 0 && vi(e, "d") < day).collect();
        if cands.is_empty() {
            return Ok(None);
        }
        let (idx, e) = re(self.rng.choice(&cands))?.clone();
        let k = vs(&e, "k").to_string();
        let ur = ch::use_row(&k).ok_or_else(|| format!("KeyError: {}", k))?;
        let cat = pystr(&ur[0]);
        let role: &[&str] = match cat.as_str() {
            "food" => &["cook"],
            "med" => &["medic"],
            "ammo" => &["scout", "guard"],
            "tool" => &["mechanic"],
            _ => return Err(format!("KeyError: {}", cat)),
        };
        let a = match self.person(role, &BTreeSet::new(), &["h", "u", "s"], cat == "ammo" || cat == "tool")? {
            Some(a) => a,
            None => return Ok(None),
        };
        let mut use_ = if k == "r38" { re(self.rng.randint(3, 7))? } else { 1 };
        use_ = use_.min(vi(&e, "n"));
        let left = vi(&e, "n") - use_;
        let kmap = map_art(vs(&e, "m"));
        let mut v = vmap(vec![
            ("item_pl", ur[2].clone()),
            ("item_short", ur[2].clone()),
            ("kd", e.get("d").cloned().ok_or("KeyError: d")?),
            ("kmap", json!(if kmap.is_empty() { "a raid".to_string() } else { kmap })),
            ("kplace", e.get("p").cloned().ok_or("KeyError: p")?),
            ("kd_ago", json!(ago(vi(&e, "d"), day))),
            ("n", json!(use_)),
            ("n_left", json!(if left != 0 { format!("{} left", left) } else { "none left".to_string() })),
        ]);
        if k == "r38" {
            v.insert("item_pl".into(), json!(".38 rounds"));
        }
        let row = self.entry("use", "use.title", &[part(&format!("use.{}", cat), true)], people1("A", &a), &v, None, None)?;
        if row.is_some() {
            self.cs()["sg_stock"][idx]["n"] = json!(left);
            if cat == "food" || cat == "med" {
                let inv = self.cs().get_mut("inv").and_then(|i| i.as_object_mut()).ok_or("inv")?;
                if let Some(cur) = inv.get(&k).map(int_of) {
                    inv.insert(k.clone(), json!(0.max(cur - 1)));
                }
            }
        }
        Ok(row)
    }

    // py: clan_saga.py:772 scene_prep
    fn scene_prep(&mut self) -> Result<Option<Value>, String> {
        let a = match self.person(&["scout", "guard"], &BTreeSet::new(), &["h", "u", "s"], true)? {
            Some(a) => a,
            None => return Ok(None),
        };
        let mut v = if !fs(&self.f, "map").is_empty() { self.match_vars()? } else { Map::new() };
        if v.is_empty() {
            let pl = places_of(sd("GENERIC"));
            v = vmap(vec![("map", json!("the next map")), ("place", json!(pl[0])), ("place2", json!(pl[1]))]);
        }
        let hist: Vec<Value> = self.cs().get("sg_hist").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        let prev = if hist.len() >= 2 { Some(hist[hist.len() - 2].clone()) } else { None };
        if let Some(p) = &prev {
            v.insert("prev_map".into(), json!(map_art(vs(p, "map"))));
            let pw = p.get("win");
            v.insert("prev_res".into(), json!(if pw.map(truthy).unwrap_or(false) { "a win" } else if pw == Some(&json!(false)) { "a loss" } else { "a plain day" }));
        }
        let away_ids: Vec<String> = self.cs().get("away").and_then(|a| a.as_object()).map(|o| o.keys().cloned().collect()).unwrap_or_default();
        let mut away: Vec<Value> = Vec::new();
        for i in &away_ids {
            let n: i64 = py::strip(i).parse().map_err(|_| "ValueError: away id".to_string())?;
            if let Some(p) = self.d.byid(n) {
                if kd::can_fight(p) {
                    away.push(p.clone());
                }
            }
        }
        let mut people = people1("A", &a);
        let mut parts: Parts = vec![part("prep.event", true)];
        if let Some(a0) = away.first() {
            people["B"] = a0.clone();
            v.insert("AW".into(), a0.get("n").cloned().ok_or("KeyError: n")?);
            parts.push(part("prep.away", true));
        }
        let stock: Vec<Value> = self.cs().get("sg_stock").and_then(|k| k.as_array()).map(|a| a.iter().filter(|e| vi(e, "n") > 0).cloned().collect()).unwrap_or_default();
        let inv: Vec<(String, i64)> = self.cs().get("inv").and_then(|i| i.as_object()).map(|o| o.iter().map(|(k, n)| (k.clone(), int_of(n))).collect()).unwrap_or_default();
        if !stock.is_empty() {
            let e = re(self.rng.choice(&stock))?.clone();
            let ur = ch::use_row(vs(&e, "k")).ok_or_else(|| format!("KeyError: {}", vs(&e, "k")))?;
            v.insert("item_pl".into(), ur[2].clone());
            v.insert("kplace".into(), e.get("p").cloned().ok_or("KeyError: p")?);
            v.insert("n".into(), json!(1.max(py::floordiv(vi(&e, "n"), 2))));
            parts.push(part("prep.stock", false));
        }
        let mut ammo = 0i64;
        let mut food = 0i64;
        for (k, n) in &inv {
            let ur = ch::use_row(k).ok_or_else(|| format!("KeyError: {}", k))?;
            if pystr(&ur[0]) == "ammo" {
                ammo += n;
            }
        }
        for (k, n) in &inv {
            let ur = ch::use_row(k).ok_or_else(|| format!("KeyError: {}", k))?;
            if pystr(&ur[0]) == "food" {
                food += n;
            }
        }
        v.insert("ammo_n".into(), json!(count_p(ammo, "round", "rounds")));
        v.insert("food_n".into(), json!(count_p(food, "can", "cans")));
        if prev.is_some() && v.contains_key("prev_map") {
            parts.push(part("prep.recent", false));
        }
        self.entry("prep", "prep.title", &parts, people, &v, None, None)
    }

    // py: clan_saga.py:809 scene_profile
    fn scene_profile(&mut self) -> Result<Option<Value>, String> {
        let a = match self.person_any(false)? {
            Some(a) => a,
            None => return Ok(None),
        };
        let poss = pystr(&self.mv(&a, "poss")?);
        let past = self.mv(&a, "past")?;
        let temper = self.mv(&a, "temper")?;
        let (fear, fwhat) = self.fear_of(&a)?;
        let first = poss.split(' ').next().unwrap_or("");
        let poss_short = if first == "a" || first == "an" { poss.split_once(' ').map(|x| x.1.to_string()).ok_or("IndexError")? } else { poss.clone() };
        let past_s = if truthy(&past) { pystr(&past) } else { "survivor".to_string() };
        let v = vmap(vec![
            ("poss", json!(poss)),
            ("poss_short", json!(poss_short)),
            ("fear", json!(fear)),
            ("fear_what", json!(fwhat)),
            ("past", json!(past_s.to_lowercase())),
        ]);
        let pools = ["prof.poss", "prof.fear", "prof.past"];
        let first_pool = *re(self.rng.choice(&pools))?;
        let mut parts: Parts = vec![part(first_pool, true)];
        if self.rng.random() < 0.7 {
            let tem = if temper.is_null() { "calm".to_string() } else { pystr(&temper) };
            parts.push(part(&format!("prof.{}", tem), false));
        }
        self.entry("prof", "prof.title", &parts, people1("A", &a), &v, None, None)
    }

    // py: clan_saga.py:823 scene_death
    fn scene_death(&mut self, died_now: &[Value]) -> Result<Option<Value>, String> {
        let dd = &died_now[0];
        let k = pystr(dd.get("i").ok_or("KeyError: i")?);
        let dead = self.cs().get("gone").and_then(|g| g.get(&k)).cloned();
        let dead = match dead.filter(|v| truthy(v)) {
            Some(d) => d,
            None => return Ok(None),
        };
        let mem = dead.get("m").cloned().ok_or("KeyError: m")?;
        let dp = json!({"n": dead.get("n").cloned().ok_or("KeyError: n")?, "g": dead.get("g").cloned().ok_or("KeyError: g")?, "r": dead.get("r").cloned().ok_or("KeyError: r")?});
        let bond = mem.get("bond").cloned().unwrap_or(Value::Null);
        let q = self.d.alive.iter().find(|p| !bond.is_null() && p.get("i") == Some(&bond)).cloned();
        let a = match q {
            Some(q) => q,
            None => match self.person(&["elder", "medic", "cook"], &BTreeSet::new(), &["h", "u", "s"], false)? {
                Some(a) => a,
                None => return Ok(None),
            },
        };
        let mut ex = BTreeSet::new();
        ex.insert(vi(&a, "i"));
        let b = self.person(&[], &ex, &["h", "u", "s"], false)?;
        let mut people = people1("A", &a);
        people["D"] = dp;
        if let Some(b) = b {
            people["B"] = b;
        }
        let inj = mem.get("inj").filter(|v| truthy(v)).cloned();
        let kplace = if !fs(&self.f, "map").is_empty() {
            let pl = places_of(map_data(fs(&self.f, "map")));
            re(self.rng.choice(&pl))?.clone()
        } else {
            "the east wall".to_string()
        };
        let poss = mem.get("poss").filter(|v| truthy(v)).cloned();
        let mut v = vmap(vec![("kplace", json!(kplace)), ("dposx", poss.clone().unwrap_or_else(|| json!("boots")))]);
        if let Some(i) = &inj {
            v.insert("dsite".into(), i.get("site").cloned().ok_or("KeyError: site")?);
            v.insert("dcloth".into(), i.get("cloth").cloned().ok_or("KeyError: cloth")?);
        }
        if let Some(p) = poss {
            v.insert("dpos".into(), p);
        }
        let parts = [part("death.line", true), part("death.line", true)];
        self.entry("death", "death.title", &parts, people, &v, Some("grieving"), Some(format!("sg-grief-{}", int_of(dd.get("i").unwrap_or(&Value::Null)))))
    }

    // ------------------------------------------------------------ the day
    // py: clan_saga.py:850 tie_arcs
    /// One sentence that ties a story arc's step to today's match (the arc text is stored by node; the tie rides along in params["tie"], English only).
    fn tie_arcs(&mut self, arc_rows: &mut [Value]) -> Result<(), String> {
        if fs(&self.f, "map").is_empty() || arc_rows.is_empty() {
            return Ok(());
        }
        for row in arc_rows.iter_mut() {
            if self.rng.random() > 0.65 {
                continue;
            }
            let p = row.get("params").and_then(|x| x.get("p")).cloned().ok_or("KeyError: p")?;
            let po = p.as_object().ok_or("p")?;
            let who = po.get("A").filter(|w| truthy(w)).cloned().or_else(|| po.values().next().cloned());
            let who = match who.filter(|w| truthy(w)) {
                Some(w) => w,
                None => continue,
            };
            let people = json!({"A": {"n": who.get(0).cloned().ok_or("IndexError")?, "g": who.get(1).cloned().ok_or("IndexError")?, "r": who.get(2).cloned().unwrap_or_else(|| json!("scavenger"))}});
            let mut v = self.base.clone();
            for (k, x) in self.match_vars()? {
                v.insert(k, x);
            }
            self.tids = Vec::new();
            let t = self.line("arc.tie", &people, &v, false)?;
            if let Some(t) = t.filter(|x| !x.is_empty()) {
                row["params"]["tie"] = json!(t);
                let txt = pystr(row.get("text").ok_or("KeyError: text")?);
                row["text"] = json!(format!("{} {}", txt, t));
                if let Some(pl) = self.pl_last.clone().filter(|x| !x.is_empty()) {
                    row["params"]["tie_pl"] = json!(pl);
                }
            }
        }
        Ok(())
    }

    // py: clan_saga.py:871 run
    fn run(&mut self, loot: &[Value], died_now: &[Value], arc_rows: &mut [Value]) -> Result<Vec<Value>, String> {
        let mut rows: Vec<Value> = Vec::new();
        self.tie_arcs(arc_rows)?;
        let away: Vec<i64> = self.cs().get("away").and_then(|a| a.as_object()).map(|o| o.keys().map(|k| py::strip(k).parse::<i64>().unwrap_or(0)).collect()).unwrap_or_default();
        self.d.spot.extend(away);
        if !fs(&self.f, "map").is_empty() {
            let lp = self.book_loot(loot)?;
            if let Some(row) = self.match_story()? {
                rows.push(row);
            }
            if !loot.is_empty() && !lp.is_empty() {
                if let Some(row) = self.haul_story(&lp)? {
                    rows.push(row);
                }
            }
        }
        let mut extra: Option<Value> = None;
        if !died_now.is_empty() {
            extra = self.scene_death(died_now)?;
        }
        if extra.is_none() {
            let mut pool: Vec<(Scene, f64)> = vec![
                (Scene::Injury, 5.0),
                (Scene::Grief, 5.0),
                (Scene::Scar, 1.0),
                (Scene::Rel, 3.0),
                (Scene::Lack, 4.0),
                (Scene::KeepCb, 3.0),
                (Scene::KeepFind, 3.0),
                (Scene::Dep, 9.0),
                (Scene::Use, 3.0),
                (Scene::Prep, 3.0),
                (Scene::Prof, 2.0),
            ];
            let mut order: Vec<Scene> = Vec::new();
            while !pool.is_empty() {
                let ws: Vec<f64> = pool.iter().map(|x| x.1).collect();
                let ix = re(self.rng.choices_weighted(&ws, 1))?[0];
                let sc = pool[ix].0;
                order.push(sc);
                pool.retain(|x| x.0 != sc);
            }
            for sc in order {
                extra = match sc {
                    Scene::Injury => self.scene_injury()?,
                    Scene::Grief => self.scene_grief()?,
                    Scene::Scar => self.scene_scar()?,
                    Scene::Rel => self.scene_rel()?,
                    Scene::Lack => self.scene_lack()?,
                    Scene::KeepCb => self.scene_keep_callback()?,
                    Scene::KeepFind => self.scene_keep_find()?,
                    Scene::Dep => self.scene_depressed()?,
                    Scene::Use => self.scene_use()?,
                    Scene::Prep => self.scene_prep()?,
                    Scene::Prof => self.scene_profile()?,
                };
                if extra.is_some() {
                    break;
                }
            }
        }
        if let Some(e) = extra {
            rows.push(e);
        }
        self.prune()?;
        Ok(rows)
    }
}

// py: clan_saga.py:911 enabled
/// Switch: env T2R_CLAN_SAGA=0/1, else clan_saga.txt containing 0 / off; default ON.
pub fn enabled() -> bool {
    let v = match std::env::var("T2R_CLAN_SAGA") {
        Ok(v) => v,
        Err(_) => match std::fs::read(data::here(data::t("clan_saga", "SWITCH_FILE").as_str().unwrap_or("@HERE@/clan_saga.txt"))) {
            Ok(b) => py::strip(&String::from_utf8_lossy(&b)).to_string(),
            Err(_) => return true,
        },
    };
    !matches!(v.to_lowercase().as_str(), "0" | "off" | "false" | "no")
}

// py: clan_saga.py:922 run
/// The saga rows of one clan day (at most 3); [] when the roster is too small. `arc_rows` are the arcs' rows of the day: the saga appends a tie sentence to some of them (in place).
pub fn run(d: &mut Day, st: &mut Value, newc: &Value, r: &Value, matchv: &Value, rng: &mut PyRandom, loot: &[Value], died_now: &[Value], arc_rows: &mut [Value]) -> Result<Vec<Value>, String> {
    let _ = newc;
    if d.alive.len() < 3 || !enabled() {
        return Ok(vec![]);
    }
    let mut s = Saga::new(d, st, r, matchv, rng)?;
    s.run(loot, died_now, arc_rows)
}

// py: clan_saga.py:929 render_msg
/// A stored saga row: the finished text in the language of the clan screen - English params["t"], Polish params["tp"] (None for a row written before the Polish table existed).
pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
    if lid.is_empty() || !lid.starts_with("sg/") || !params.is_object() {
        return None;
    }
    let pick = |k: &str| params.get(k).filter(|v| !v.is_null()).map(pystr);
    if lang == "pl" || lang.to_lowercase().starts_with("pl") {
        pick("tp")
    } else {
        pick("t")
    }
}
