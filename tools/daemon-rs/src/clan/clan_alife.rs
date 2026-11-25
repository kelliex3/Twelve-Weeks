//! py: clan_alife.py - the clan A-Life: an offline, server-side simulation of each player's clan that narrates and enriches the authoritative clan numbers.
//! It NEVER changes a number or a person's state: it reads clan_story's roster and the day's result and only writes text (feed rows kind "alife", the
//! menu ticker lines, the read-only views). State = `serde_json::Value` (insertion order kept: it is stored with `json.dumps`). The template registry is the
//! generated data (clan_alife.REG / BY_POOL); the logic is ported 1:1, including the order of every random draw.
use crate::clan::clan::{self as cl, gi, int_of, pystr, truthy};
use crate::clan::clan_alife_lex as x;
use crate::clan::clan_kids as kd;
use crate::clan::clan_story as cs;
use crate::clan::clan_story_pl as pln;
use crate::clan::data;
use crate::{clock, py};
use rusqlite::{params, Connection};
use serde_json::{json, Map, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::{PyRandom, RandError};

pub type R<T> = Result<T, String>;

pub const STATE_V: i64 = 1;
pub const WINDOW_DAYS: i64 = 999; // once per run (a run is at most 85 days): a spent pool writes nothing
pub const OPENER_DAYS: i64 = 999;
pub const OPENER_P: f64 = 0.5; // chance of a weather opener on a day the weather turned (at most one a day)
pub const DAY_MIN_ROWS: usize = 2;
pub const DAY_MAX_ROWS: usize = 3;
/// of the menu's 6 ticker lines this many come from the A-Life facts
pub const TICKER_SHARE: usize = 3;
/// below this the clan_story solo / duo voices own the feed
pub const MIN_CLAN: usize = 3;
pub const DAILY_FACTS: usize = 30;

fn tb(name: &str) -> &'static Value {
    data::t("clan_alife", name)
}

/// the camp incidents of re/clan-events-1117.md (text only; tests switch it to compare). Read once, like the Python module constant.
static INCIDENTS: LazyLock<bool> = LazyLock::new(|| std::env::var("T2R_CLAN_INCIDENTS").map(|v| v != "0").unwrap_or(true));

static MEM_CAUSES: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("MEM_CAUSES")));
static TRAIT_IDS: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("TRAIT_IDS")));
static TAIL_OK: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("TAIL_OK")));
static PERSISTENT: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("PERSISTENT")));

fn rr<T>(r: Result<T, RandError>) -> R<T> {
    r.map_err(|e| format!("random: {:?}", e))
}

fn opt<T>(o: Option<T>, what: &str) -> R<T> {
    o.ok_or_else(|| format!("KeyError: {}", what))
}

// ------------------------------------------------------------------ the template registry
/// py: REG[id] = (pool, scope, tone, en, pl, letters, en vars, pl vars)
pub struct Tpl {
    pub pool: &'static str,
    pub sc: &'static str,
    pub tone: &'static str,
    pub en: &'static str,
    pub pl: &'static str,
    pub letters: BTreeSet<char>,
    pub ve: Vec<&'static str>,
    pub vp: Vec<&'static str>,
}

fn set_strs(v: &'static Value) -> Vec<&'static str> {
    data::set_items(v).iter().filter_map(|s| s.as_str()).collect()
}

pub static REG: LazyLock<HashMap<&'static str, Tpl>> = LazyLock::new(|| {
    let mut out = HashMap::new();
    if let Some(o) = tb("REG").as_object() {
        for (id, row) in o {
            let Some(r) = row.as_array() else { continue };
            let s = |i: usize| r.get(i).and_then(|v| v.as_str()).unwrap_or("");
            let letters: BTreeSet<char> = set_strs(&r[5]).iter().filter_map(|l| l.chars().next()).collect();
            out.insert(
                id.as_str(),
                Tpl { pool: s(0), sc: s(1), tone: s(2), en: s(3), pl: s(4), letters, ve: set_strs(&r[6]), vp: set_strs(&r[7]) },
            );
        }
    }
    out
});

pub static BY_POOL: LazyLock<HashMap<&'static str, Vec<&'static str>>> = LazyLock::new(|| {
    let mut out = HashMap::new();
    if let Some(o) = tb("BY_POOL").as_object() {
        for (pool, ids) in o {
            out.insert(pool.as_str(), data::strs(ids));
        }
    }
    out
});

/// `pool in POOLS`
fn has_pool(pool: &str) -> bool {
    tb("POOLS").get(pool).is_some()
}

// ------------------------------------------------------------------ the switch
struct Sw {
    mtime: Option<f64>,
    val: bool,
}
static SW: LazyLock<parking_lot::Mutex<Sw>> = LazyLock::new(|| parking_lot::Mutex::new(Sw { mtime: None, val: false }));

// py: clan_alife.py:90 enabled
/// Default ON (user 11-12). env T2R_CLAN_ALIFE=1/0 wins; else the first word of clan_alife.txt (0 / off / false / no turns it off; anything else, or no file, is on).
pub fn enabled() -> bool {
    if let Ok(e) = std::env::var("T2R_CLAN_ALIFE") {
        let e = py::strip(&e);
        if !e.is_empty() {
            return matches!(e.to_lowercase().as_str(), "1" | "on" | "true" | "yes");
        }
    }
    let path = data::here(tb("SWITCH_FILE").as_str().unwrap_or("@HERE@/clan_alife.txt"));
    let m = match std::fs::metadata(&path).and_then(|m| m.modified()) {
        Ok(t) => t.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0),
        Err(_) => return true,
    };
    let mut sw = SW.lock();
    if sw.mtime != Some(m) {
        let w = match std::fs::read(&path) {
            Ok(b) => String::from_utf8_lossy(&b).split_whitespace().next().map(|s| s.to_lowercase()).unwrap_or_default(),
            Err(_) => String::new(),
        };
        sw.mtime = Some(m);
        sw.val = !matches!(w.as_str(), "0" | "off" | "false" | "no");
    }
    sw.val
}

fn fk_of(st: &Value) -> &'static str {
    if gi(st, "fac", 0) == cl::SERAPHITES {
        "S"
    } else {
        "W"
    }
}

fn morale_of(st: &Value) -> f64 {
    st.get("morale").and_then(|m| m.as_f64()).unwrap_or(0.0)
}

fn tone_ok(tone: &str, morale: f64) -> bool {
    tone == "*" || (tone == "g" && morale >= 55.0) || (tone == "n" && (35.0..=70.0).contains(&morale)) || (tone == "b" && morale <= 45.0)
}

// py: clan_alife.py:118 _pp
fn pp(p: &Value) -> Value {
    json!({"n": p.get("n").cloned().unwrap_or(Value::Null), "g": p.get("g").cloned().unwrap_or(Value::Null), "r": p.get("r").cloned().unwrap_or(Value::Null), "i": p.get("i").cloned().unwrap_or(Value::Null)})
}

fn vi(v: &Value, k: &str) -> i64 {
    gi(v, k, 0)
}

fn vs<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("")
}

fn va<'a>(v: &'a Value, k: &str) -> &'a [Value] {
    v.get(k).and_then(|x| x.as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

fn ints(v: &Value, k: &str) -> Vec<i64> {
    va(v, k).iter().map(int_of).collect()
}

fn jints(a: &[i64]) -> Value {
    Value::Array(a.iter().map(|i| Value::from(*i)).collect())
}

fn people_of(st: &Value) -> &[Value] {
    va(st, "people")
}

fn is_s(v: &Value, k: &str, want: &str) -> bool {
    v.get(k).and_then(|x| x.as_str()) == Some(want)
}

// ------------------------------------------------------------------ storage
// py: clan_alife.py:123 SCHEMA / ensure
pub fn ensure(c: &Connection) -> rusqlite::Result<()> {
    c.execute("CREATE TABLE IF NOT EXISTS clan_alife(token_id TEXT PRIMARY KEY, state TEXT, updated REAL)", [])?;
    Ok(())
}

// py: clan_alife.py:130 _new
fn new_state(tid: &str, st: &Value) -> Value {
    json!({"v": STATE_V, "seed": tid, "run": st.get("run").cloned().unwrap_or(Value::Null), "fac": st.get("fac").cloned().unwrap_or(Value::Null), "day": -1,
           "P": {}, "T": [], "nid": 0, "mem": {}, "D": [], "done": [], "lastdest": [], "cool": {}, "stat": {"lines": 0, "rep": 0, "days": 0}, "ls": {}})
}

// py: clan_alife.py:135 load
pub fn load(c: &Connection, tid: &str) -> Option<Value> {
    let row: Option<String> = c.query_row("SELECT state FROM clan_alife WHERE token_id=?", params![tid], |r| r.get::<_, Option<String>>(0)).ok()?;
    let al: Value = serde_json::from_str(&row?).ok()?;
    if al.is_object() && al.get("v").map(int_of) == Some(STATE_V) && al.get("v").map_or(false, |v| v.is_number()) {
        Some(al)
    } else {
        None
    }
}

// py: clan_alife.py:149 save
pub fn save(c: &Connection, tid: &str, al: &mut Value) -> rusqlite::Result<()> {
    let cut = vi(al, "day") - 120;
    let keep: Map<String, Value> = al.get("mem").and_then(|m| m.as_object()).map(|o| o.iter().filter(|(_, v)| v.get(0).map(int_of).unwrap_or(0) >= cut).map(|(k, v)| (k.clone(), v.clone())).collect()).unwrap_or_default();
    al["mem"] = Value::Object(keep);
    c.execute(
        "INSERT INTO clan_alife(token_id,state,updated) VALUES(?,?,?) ON CONFLICT(token_id) DO UPDATE SET state=excluded.state,updated=excluded.updated",
        params![tid, py::dumps_compact(al), clock::now()],
    )?;
    Ok(())
}

// py: clan_alife.py:156 reset
/// A new clan run (CHOOSE YOUR SIDE): the old simulation goes.
pub fn reset(c: &Connection, tid: &str) -> Result<(), String> {
    let _ = c.execute("DELETE FROM clan_alife WHERE token_id=?", params![tid]);
    Ok(())
}

// ------------------------------------------------------------------ people
// py: clan_alife.py:165 _rec
/// The persistent extras of one survivor (created on first sight, deterministic from the seed).
fn rec_ensure(al: &mut Value, p: &Value, fk: &str) -> R<()> {
    let pid = vi(p, "i");
    let k = pid.to_string();
    if al.get("P").and_then(|m| m.get(&k)).is_some() {
        return Ok(());
    }
    let mut rng = PyRandom::from_str(&format!("{}:{}:p{}", pystr(&al["seed"]), pystr(&al["run"]), pid));
    let mut jobs = x::pool_keys(x::jobs(), fk);
    let mut origs = x::pool_keys(x::origins(), fk);
    if is_s(p, "r", "kid") {
        // a kid's past is no fighter's past: no Scars, no raids, no patrols
        let o: Vec<&str> = origs.iter().copied().filter(|o| !(kd::combat_text(&x::origins()[*o][1].as_str().unwrap_or(""), Some("en")) || kd::combat_text(&x::origins()[*o][2].as_str().unwrap_or(""), Some("pl")))).collect();
        if !o.is_empty() {
            origs = o;
        }
        let j: Vec<&str> = jobs.iter().copied().filter(|j| !(kd::combat_text(&x::jobs()[*j][1].as_str().unwrap_or(""), Some("en")) || kd::combat_text(&x::jobs()[*j][2].as_str().unwrap_or(""), Some("pl")))).collect();
        if !j.is_empty() {
            jobs = j;
        }
    }
    let t = rr(rng.sample_indices(TRAIT_IDS.len(), 2))?;
    let job = *rr(rng.choice(&jobs))?;
    let orig = *rr(rng.choice(&origs))?;
    let r = json!({"t": t.iter().map(|i| Value::from(*i as i64)).collect::<Vec<_>>(), "job": job, "orig": orig, "f": 0, "rv": 0, "pa": 0, "sk": 0, "lm": -99,
                   "vg": -99, "ls": p.get("s").cloned().unwrap_or(Value::Null), "seen": 0});
    al["P"][k.as_str()] = r;
    Ok(())
}

fn rget<'a>(al: &'a Value, pid: i64) -> &'a Value {
    al.get("P").and_then(|m| m.get(&pid.to_string())).unwrap_or(&Value::Null)
}

fn rset(al: &mut Value, pid: i64, key: &str, v: Value) {
    if let Some(r) = al.get_mut("P").and_then(|m| m.get_mut(&pid.to_string())) {
        if let Some(o) = r.as_object_mut() {
            o.insert(key.to_string(), v);
        }
    }
}

/// `TRAIT_IDS[r["t"][n]]`
fn trait_of(r: &Value, n: usize) -> R<&'static str> {
    let i = opt(r.get("t").and_then(|t| t.get(n)).map(int_of), "t")?;
    let len = TRAIT_IDS.len() as i64;
    let j = if i < 0 { i + len } else { i };
    opt(usize::try_from(j).ok().and_then(|j| TRAIT_IDS.get(j).copied()), "trait")
}

// ------------------------------------------------------------------ choosing and composing text
// py: clan_alife.py:187 _avail
fn avail(sym: &Value) -> R<(HashSet<String>, HashSet<String>)> {
    Ok((opt(x::expand_en(sym), "expand_en")?.into_keys().collect(), opt(x::expand_pl(sym), "expand_pl")?.into_keys().collect()))
}

fn subset(need: &[&str], have: &HashSet<String>) -> bool {
    need.iter().all(|k| have.contains(*k))
}

// py: clan_alife.py:191 _choose
/// One template id of `pool` that fits the faction, the clan's mood and the people / variables at hand; None when nothing fits.
#[allow(clippy::too_many_arguments)]
fn choose(al: &mut Value, pool: &str, fk: &str, morale: f64, letters: &BTreeSet<char>, av_en: &HashSet<String>, av_pl: &HashSet<String>, rng: &mut PyRandom, day: i64, window: i64,
          exclude: &HashSet<&'static str>, record: bool, people: Option<&Value>) -> R<Option<&'static str>> {
    let mut cands: Vec<&'static str> = Vec::new();
    for tid in BY_POOL.get(pool).map(|v| v.as_slice()).unwrap_or(&[]) {
        let Some(t) = REG.get(tid) else { continue };
        if !(t.sc == "*" || t.sc == fk) || exclude.contains(tid) || !tone_ok(t.tone, morale) {
            continue;
        }
        if !t.letters.is_subset(letters) || !subset(&t.ve, av_en) || !subset(&t.vp, av_pl) {
            continue;
        }
        if let Some(pe) = people {
            if truthy(pe) && !kd::kid_safe(t.en, pe, &[t.pl]) {
                continue;
            }
        }
        cands.push(*tid);
    }
    if cands.is_empty() {
        return Ok(None);
    }
    let mem = al["mem"].clone();
    let fresh: Vec<&'static str> = cands.iter().copied().filter(|t| match mem.get(*t) { None => true, Some(m) => day - m.get(0).map(int_of).unwrap_or(0) >= window }).collect();
    if fresh.is_empty() {
        // the pool is spent for this run: nothing, never a repeat
        if record {
            let sp = &mut al["stat"];
            if sp.get("spent").is_none() {
                sp["spent"] = json!({});
            }
            let n = sp["spent"].get(pool).map(int_of).unwrap_or(0);
            sp["spent"][pool] = Value::from(n + 1);
        }
        return Ok(None);
    }
    let never: Vec<&'static str> = fresh.iter().copied().filter(|t| mem.get(*t).is_none()).collect();
    let use_never = !never.is_empty() && rng.random() < 0.7;
    let pick = *rr(rng.choice(if use_never { &never } else { &fresh }))?;
    if record {
        note(al, pick, day);
    }
    Ok(Some(pick))
}

// py: clan_alife.py:220 _note
fn note(al: &mut Value, tid: &str, day: i64) {
    let n = al["mem"].get(tid).and_then(|m| m.get(1)).map(int_of).unwrap_or(0);
    let had = al["mem"].get(tid).is_some();
    al["mem"][tid] = json!([day, if had { n + 1 } else { 1 }]);
}

// py: clan_alife.py:225 _render_ids
fn render_ids(ids: &[&str], people: &Value, sym: &Value, lang: &str, fk: &str) -> R<String> {
    let mut parts = Vec::new();
    let mut ve: Option<cs::Vars> = None;
    let mut vp: Option<cs::Vars> = None;
    for tid in ids {
        let t = opt(REG.get(tid), "template id")?;
        if lang == "pl" {
            if vp.is_none() {
                vp = Some(opt(x::expand_pl(sym), "expand_pl")?);
            }
            parts.push(x::render(t.pl, people, vp.as_ref().expect("vp"), "pl", fk)?);
        } else {
            if ve.is_none() {
                ve = Some(opt(x::expand_en(sym), "expand_en")?);
            }
            parts.push(x::render(t.en, people, ve.as_ref().expect("ve"), "en", fk)?);
        }
    }
    Ok(parts.join(" "))
}

// py: clan_alife.py:239 render_msg
/// A stored A-Life row (lid "al/<template ids joined by +>", params {"p": people, "s": sym}) in a language; None when a template is unknown.
pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
    if lid.is_empty() || !lid.starts_with("al/") || !params.is_object() {
        return None;
    }
    let ids: Vec<&str> = lid[3..].split('+').collect();
    if ids.iter().any(|i| !REG.contains_key(i)) {
        return None;
    }
    let mut people = Map::new();
    if let Some(p) = params.get("p") {
        for (k, v) in p.as_object()? {
            let a = v.as_array()?;
            let g = match a.get(1)?.as_str() {
                Some(s) if matches!(s, "m" | "f" | "n") => s,
                _ => "n",
            };
            let r = a.get(2).cloned().unwrap_or_else(|| Value::from("scavenger"));
            people.insert(k.clone(), json!({"n": pystr(a.first()?), "g": g, "r": r}));
        }
    }
    let empty = json!({});
    let sym = params.get("s").unwrap_or(&empty);
    let fk = match sym.get("f") {
        None => "W".to_string(),
        Some(v) => pystr(v),
    };
    render_ids(&ids, &Value::Object(people), sym, if pln::norm_lang(lang) == "pl" { "pl" } else { "en" }, &fk).ok()
}

// py: clan_alife.py:251 _base_sym
fn base_sym(st: &Value, day: i64) -> Map<String, Value> {
    let fk = fk_of(st);
    let week = cl::week_day(day).0;
    let mut m = Map::new();
    m.insert("f".into(), Value::from(fk));
    m.insert("day".into(), Value::from(day));
    m.insert("week".into(), Value::from(week));
    m.insert("size".into(), Value::from(people_of(st).len() as i64));
    m.insert("season".into(), Value::from(x::season_of(week)));
    m
}

// py: clan_alife.py:257 _weather
fn weather(al: &Value, st: &Value, day: i64) -> R<&'static str> {
    let season = x::season_of(cl::week_day(day).0);
    let w: [f64; 6] = match season {
        0 => [4.0, 2.0, 1.0, 2.0, 1.0, 2.0],
        1 => [2.0, 2.0, 4.0, 2.0, 1.0, 1.0],
        _ => [3.0, 2.0, 1.0, 3.0, 1.0, 2.0],
    };
    let _ = st;
    let mut rng = PyRandom::from_str(&format!("{}:{}:wx:{}", pystr(&al["seed"]), pystr(&al["run"]), py::floordiv(day, 2)));
    let i = rr(rng.choices_weighted(&w, 1))?[0];
    let ws = x::weather();
    opt(ws.get(i).copied(), "weather")
}

fn mood_of_pool(pool: &str) -> Option<&'static str> {
    tb("MOODS").get(pool).and_then(|v| v.as_str())
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

fn crc(s: &str) -> i64 {
    t2r_clanstory::crc::crc32(s.as_bytes()) as i64
}

/// One event of the day: {pool, people{A..}, sym{...}, mood?, opener, coda}
struct Ev {
    pool: String,
    people: Value,
    sym: Value,
    mood: Option<String>,
    opener: bool,
}

// py: clan_alife.py:270 _compose
/// One feed row from an event: [weather opener] + core. None when no core template fits (or its pool is spent).
fn compose(al: &mut Value, st: &Value, ev: &Ev, rng: &mut PyRandom, day: i64, wx: &str) -> R<Option<Value>> {
    let fk = fk_of(st);
    let morale = morale_of(st);
    let people = &ev.people;
    let mut sym = base_sym(st, day);
    if let Some(o) = ev.sym.as_object() {
        for (k, v) in o {
            sym.insert(k.clone(), v.clone());
        }
    }
    let sym = Value::Object(sym);
    let letters: BTreeSet<char> = people.as_object().map(|o| o.keys().filter_map(|k| k.chars().next()).collect()).unwrap_or_default();
    let (av_en, av_pl) = avail(&sym)?;
    // the core: never a template whose wording (with these very people) was already published inside the window - tried again with another template
    let mut tried: HashSet<&'static str> = HashSet::new();
    let mut core: Option<&'static str> = None;
    for _attempt in 0..5 {
        core = choose(al, &ev.pool, fk, morale, &letters, &av_en, &av_pl, rng, day, WINDOW_DAYS, &tried, false, Some(people))?;
        let Some(c0) = core else { break };
        let text0 = render_ids(&[c0], people, &sym, "en", fk)?;
        let h0 = crc(&text0);
        if kd::any_kid(people) && !(kd::kid_text_ok(&text0, people, "en") && kd::kid_text_ok(&render_ids(&[c0], people, &sym, "pl", fk)?, people, "pl")) {
            tried.insert(c0); // the filled-in line (an origin, a past, a keepsake) makes a kid a fighter: another template
            core = None;
            continue;
        }
        let seen = va(al, "tx").iter().any(|e| e.get(0).map(int_of) == Some(h0) && day - e.get(1).map(int_of).unwrap_or(0) < WINDOW_DAYS);
        if seen && tried.len() < 4 {
            tried.insert(c0);
            continue;
        }
        break;
    }
    let Some(core) = core else { return Ok(None) };
    note(al, core, day);
    let mut ids: Vec<&str> = vec![core];
    if al.get("wxo").is_none() {
        al["wxo"] = json!([-99, ""]);
    }
    let wo_day = al["wxo"].get(0).map(int_of).unwrap_or(-99);
    let wo_wx = al["wxo"].get(1).and_then(|v| v.as_str()).unwrap_or("").to_string();
    if ev.opener && wo_day != day && wo_wx != wx && rng.random() < OPENER_P {
        let none = BTreeSet::new();
        let nop = HashSet::new();
        let op = choose(al, &format!("wx_{}", wx), fk, morale, &none, &av_en, &av_pl, rng, day, OPENER_DAYS, &nop, true, None)?;
        if let Some(op) = op {
            ids.insert(0, op);
            al["wxo"] = json!([day, wx]);
        }
    }
    let text = render_ids(&ids, people, &sym, "en", fk)?;
    let hcore = crc(&render_ids(&[core], people, &sym, "en", fk)?);
    if al.get("tx").is_none() {
        al["tx"] = json!([]);
    }
    if let Some(tx) = al["tx"].as_array_mut() {
        tx.push(json!([hcore, day]));
        let n = tx.len();
        if n > 160 {
            tx.drain(..n - 160);
        }
    }
    let mut names = Vec::new();
    for k in ["A", "B", "C", "D"] {
        if let Some(p) = people.get(k) {
            if has_word(&text, &pystr(&p["n"])) {
                names.push(p["n"].clone());
            }
        }
    }
    let mood = match ev.mood.as_deref().filter(|m| !m.is_empty()).or_else(|| mood_of_pool(&ev.pool)) {
        Some(m) => m,
        None => {
            if morale >= 60.0 {
                "hopeful"
            } else if morale < 35.0 {
                "grim"
            } else {
                "calm"
            }
        }
    };
    let n = vi(&al["stat"], "lines");
    al["stat"]["lines"] = Value::from(n + 1);
    let mut pm = Map::new();
    if let Some(o) = people.as_object() {
        for (k, v) in o {
            pm.insert(k.clone(), json!([v["n"].clone(), v["g"].clone(), v.get("r").cloned().unwrap_or_else(|| Value::from("scavenger"))]));
        }
    }
    Ok(Some(json!({"kind": "alife", "mood": mood, "text": text, "names": names, "lid": format!("al/{}", ids.join("+")), "params": {"p": Value::Object(pm), "s": sym}})))
}

// ------------------------------------------------------------------ the day
/// Options of `Day::pick` / `Day::free` (Python keyword arguments)
#[derive(Clone, Default)]
struct Pk<'k> {
    role_w: &'k [(&'k str, f64)],
    role: Option<&'k str>,
    exclude: HashSet<i64>,
    h_only: bool,
    fight: bool,
}

fn pk_h_fight<'k>() -> Pk<'k> {
    Pk { h_only: true, fight: true, ..Pk::default() }
}

/// One simulated clan day. All randomness comes from rng (seeded by token, run, day): same inputs -> same day.
struct Day<'a> {
    al: &'a mut Value,
    st: &'a Value,
    rng: PyRandom,
    named: HashSet<String>,
    recovered_named: HashSet<String>,
    fk: &'static str,
    day: i64,
    sup: i64,
    need: i64,
    deficit: bool,
    alive: HashMap<i64, &'a Value>,
    events: Vec<(f64, Ev)>,
    used: HashSet<i64>,
    wx: &'static str,
    r: &'a Value,
    dead_today: Vec<Value>,
    arrivals_ok: bool,
    new_ids: Vec<&'a Value>,
}

impl<'a> Day<'a> {
    // py: clan_alife.py:320 __init__
    fn new(al: &'a mut Value, st: &'a Value, r: &'a Value, rng: PyRandom, story_msgs: &[Value]) -> R<Day<'a>> {
        let mut named = HashSet::new();
        let mut recovered_named = HashSet::new();
        for m in story_msgs {
            for n in va(m, "names") {
                named.insert(pystr(n));
            }
            if vs(m, "kind") == "recovery" {
                for n in va(m, "names") {
                    recovered_named.insert(pystr(n));
                }
            }
        }
        let fk = fk_of(st);
        let day = vi(st, "day");
        let sup = vi(r, "supplies");
        let need = vi(r, "needed");
        let alive: HashMap<i64, &Value> = people_of(st).iter().map(|p| (vi(p, "i"), p)).collect();
        let wx = weather(al, st, day)?;
        Ok(Day { al, st, rng, named, recovered_named, fk, day, sup, need, deficit: sup < need, alive, events: Vec::new(), used: HashSet::new(), wx, r, dead_today: Vec::new(), arrivals_ok: false, new_ids: Vec::new() })
    }

    // --- helpers
    fn rec(&mut self, p: &Value) -> R<()> {
        rec_ensure(self.al, p, self.fk)
    }

    fn rv(&mut self, p: &Value, key: &str) -> R<Value> {
        self.rec(p)?;
        Ok(rget(self.al, vi(p, "i")).get(key).cloned().unwrap_or(Value::Null))
    }

    fn rvi(&mut self, p: &Value, key: &str) -> R<i64> {
        Ok(int_of(&self.rv(p, key)?))
    }

    fn rw(&mut self, p: &Value, key: &str, v: Value) -> R<()> {
        self.rec(p)?;
        rset(self.al, vi(p, "i"), key, v);
        Ok(())
    }

    fn thr(&self, kind: &str) -> Vec<Value> {
        va(self.al, "T").iter().filter(|t| vs(t, "k") == kind).cloned().collect()
    }

    fn nthr(&self, kind: &str) -> usize {
        va(self.al, "T").iter().filter(|t| vs(t, "k") == kind).count()
    }

    fn away(&self) -> HashSet<i64> {
        let mut out = HashSet::new();
        for t in va(self.al, "T") {
            if vs(t, "k") == "exp" && (vi(t, "d") > self.day || vi(t, "s") == self.day) {
                for i in ints(t, "a") {
                    if self.alive.contains_key(&i) {
                        out.insert(i);
                    }
                }
            }
        }
        out
    }

    fn hurt(&self) -> HashSet<i64> {
        va(self.al, "T").iter().filter(|t| vs(t, "k") == "inj").filter_map(|t| t.get("a").and_then(|a| a.get(0)).map(int_of)).collect()
    }

    // py: clan_alife.py:352 free
    /// Survivors who are in camp, not on light duty, not already spotlighted today, in one of the given real states (default healthy or hungry).
    fn free(&self, o: &Pk) -> Vec<&'a Value> {
        let mut away = self.away();
        let hurt = self.hurt();
        // away in a story arc (clan_arcs)
        let ch_away = self.st.get("ch").filter(|c| truthy(c)).and_then(|c| c.get("away")).filter(|a| truthy(a));
        for p in people_of(self.st) {
            if ch_away.map_or(false, |a| a.get(vi(p, "i").to_string()).is_some()) {
                away.insert(vi(p, "i"));
            }
        }
        let st: &'a Value = self.st;
        people_of(st)
            .iter()
            .filter(|p| {
                let s = vs(p, "s");
                let i = vi(p, "i");
                (if o.h_only { s == "h" } else { s == "h" || s == "u" })
                    && !away.contains(&i)
                    && !hurt.contains(&i)
                    && !self.used.contains(&i)
                    && !o.exclude.contains(&i)
                    && o.role.map_or(true, |r| vs(p, "r") == r)
                    && (!o.fight || kd::can_fight(p))
            })
            .collect()
    }

    // py: clan_alife.py:361 pick
    /// k distinct free survivors, spotlight-fair: the longer since someone was last in a line the likelier they are.
    fn pick(&mut self, k: usize, o: &Pk) -> R<Vec<&'a Value>> {
        let mut pool = self.free(o);
        let mut out = Vec::new();
        while !pool.is_empty() && out.len() < k {
            let mut w = Vec::with_capacity(pool.len());
            for p in &pool {
                let lm = self.rvi(p, "lm")?;
                let rw = o.role_w.iter().find(|(r, _)| *r == vs(p, "r")).map(|(_, v)| *v).unwrap_or(1.0);
                w.push((1.0 + ((self.day - lm).min(14)) as f64) * rw);
            }
            let qi = rr(self.rng.choices_weighted(&w, 1))?[0];
            let q = pool.remove(qi);
            out.push(q);
        }
        Ok(out)
    }

    fn pick1(&mut self) -> R<Vec<&'a Value>> {
        self.pick(1, &Pk::default())
    }

    fn person(&self, i: i64) -> Option<&'a Value> {
        self.alive.get(&i).copied()
    }

    // py: clan_alife.py:376 mark
    fn mark(&mut self, ps: &[Option<&Value>]) -> R<()> {
        for p in ps.iter().flatten() {
            if p.get("s").is_some() {
                self.used.insert(vi(p, "i"));
                let d = self.day;
                self.rw(p, "lm", Value::from(d))?;
            }
        }
        Ok(())
    }

    // py: clan_alife.py:382 add
    fn add(&mut self, drama: f64, pool: &str, people: &[Option<&Value>], sym: Option<Value>, mood: Option<&str>, opener: bool) -> R<()> {
        let mut pe = Map::new();
        for (k, p) in ["A", "B", "C", "D"].iter().zip(people.iter()) {
            if let Some(v) = p {
                if v.get("s").is_some() {
                    pe.insert((*k).to_string(), pp(v));
                } else {
                    pe.insert((*k).to_string(), json!({"n": v["n"].clone(), "g": v["g"].clone(), "r": v.get("r").cloned().unwrap_or_else(|| Value::from("scavenger"))}));
                }
            }
        }
        let ev = Ev { pool: pool.to_string(), people: Value::Object(pe), sym: sym.filter(|s| truthy(s)).unwrap_or_else(|| json!({})), mood: mood.map(|m| m.to_string()), opener };
        let d = drama + self.rng.random() * 12.0;
        self.events.push((d, ev));
        self.mark(people)
    }

    fn add2(&mut self, drama: f64, pool: &str, people: &[Option<&Value>], sym: Option<Value>) -> R<()> {
        self.add(drama, pool, people, sym, None, true)
    }

    fn days_gone(&self, t: &Value) -> i64 {
        (self.day - vi(t, "s")).max(1)
    }

    fn nid(&mut self) -> i64 {
        let n = vi(self.al, "nid") + 1;
        self.al["nid"] = Value::from(n);
        n
    }

    fn t_push(&mut self, t: Value) {
        if let Some(a) = self.al["T"].as_array_mut() {
            a.push(t);
        }
    }

    fn t_remove(&mut self, id: i64) {
        if let Some(a) = self.al["T"].as_array_mut() {
            if let Some(pos) = a.iter().position(|t| vi(t, "i") == id) {
                a.remove(pos);
            }
        }
    }

    fn t_mut(&mut self, id: i64) -> Option<&mut Value> {
        self.al["T"].as_array_mut()?.iter_mut().find(|t| vi(t, "i") == id)
    }

    // --- sync with the real roster
    // py: clan_alife.py:397 sync
    fn sync(&mut self) -> R<()> {
        let day = self.day;
        let st: &'a Value = self.st;
        for p in people_of(st) {
            self.rec(p)?;
        }
        // the dead of today
        for d in va(st, "dead") {
            if vi(d, "d") == day && !va(self.al, "D").iter().any(|x| vi(x, "i") == vi(d, "i")) {
                let is_kid = is_s(d, "r", "kid");
                let mut keys: Vec<&str> = x::keeps().as_object().map(|o| o.iter().filter(|(_, v)| !(is_kid && kd::combat_text(v[0].as_str().unwrap_or(""), Some("en")))).map(|(k, _)| k.as_str()).collect()).unwrap_or_default();
                keys.sort();
                let keep = *rr(self.rng.choice(&keys))?;
                let rec = json!({"i": d["i"].clone(), "n": d["n"].clone(), "g": d["g"].clone(), "r": d["r"].clone(), "c": d.get("c").cloned().unwrap_or(Value::Null), "d": day, "keep": keep,
                                 "k": d.get("k").cloned().unwrap_or_else(|| Value::from(0))});
                self.dead_today.push(rec.clone());
                if let Some(a) = self.al["D"].as_array_mut() {
                    a.push(rec);
                }
            }
        }
        if let Some(a) = self.al["D"].as_array_mut() {
            let n = a.len();
            if n > 10 {
                a.drain(..n - 10);
            }
        }
        let ids: Vec<i64> = va(self.al, "T").iter().map(|t| vi(t, "i")).collect();
        for id in ids {
            let Some(t) = va(self.al, "T").iter().find(|t| vi(t, "i") == id).cloned() else { continue };
            let a = ints(&t, "a");
            let gone: Vec<i64> = a.iter().copied().filter(|i| !self.alive.contains_key(i)).collect();
            if gone.is_empty() {
                continue;
            }
            let kind = vs(&t, "k").to_string();
            let kept: Vec<i64> = a.iter().copied().filter(|i| self.alive.contains_key(i)).collect();
            if kind == "exp" {
                let mut lost_add = Vec::new();
                for i in &gone {
                    let d = va(self.al, "D").iter().find(|x| vi(x, "i") == *i);
                    if let Some(d) = d {
                        if matches!(d.get("c").and_then(|c| c.as_str()), Some("lost") | Some("walkout")) {
                            lost_add.push(*i);
                        }
                    }
                }
                if let Some(tm) = self.t_mut(id) {
                    if !lost_add.is_empty() {
                        if tm["x"].get("lost").is_none() {
                            tm["x"]["lost"] = json!([]);
                        }
                        if let Some(l) = tm["x"]["lost"].as_array_mut() {
                            for i in lost_add {
                                l.push(Value::from(i));
                            }
                        }
                    }
                    tm["a"] = jints(&kept);
                }
                if kept.is_empty() {
                    self.t_remove(id);
                }
            } else if kind == "mem" {
            } else {
                if let Some(tm) = self.t_mut(id) {
                    tm["a"] = jints(&kept);
                }
                if kept.is_empty() || matches!(kind.as_str(), "inj" | "men" | "qrl") {
                    self.t_remove(id);
                }
            }
        }
        // a party member the day's diary rows put in camp: the party is home today (the return is narrated like any other)
        for t in self.thr("exp") {
            if vi(&t, "s") < day && day < vi(&t, "d") && ints(&t, "a").iter().any(|i| self.alive.get(i).map_or(false, |p| self.named.contains(&pystr(&p["n"])))) {
                if let Some(tm) = self.t_mut(vi(&t, "i")) {
                    tm["d"] = Value::from(day);
                }
            }
        }
        // an expedition member who fell sick: the party is recalled today
        for t in self.thr("exp") {
            let sick: Vec<i64> = ints(&t, "a").into_iter().filter(|i| self.alive.get(i).map_or(false, |p| vs(p, "s") == "s")).collect();
            // (t is a snapshot from before the loop: re-read d / s of the live thread, an earlier iteration may have changed it)
            let live = va(self.al, "T").iter().find(|u| vi(u, "i") == vi(&t, "i")).cloned().unwrap_or(t.clone());
            if !sick.is_empty() && vi(&live, "d") >= day && vi(&live, "s") < day {
                if let Some(tm) = self.t_mut(vi(&t, "i")) {
                    tm["d"] = Value::from(day);
                    tm["x"]["recall"] = Value::from(sick[0]);
                }
            }
        }
        // injured people who fell ill or were unlucky: the injury thread ends silently
        for t in self.thr("inj") {
            let p = self.alive.get(&ints(&t, "a").first().copied().unwrap_or(-1));
            if p.map_or(true, |p| vs(p, "s") != "h") {
                self.t_remove(vi(&t, "i"));
            }
        }
        Ok(())
    }

    // --- events from the real day
    // py: clan_alife.py:443 real_day
    fn real_day(&mut self) -> R<()> {
        let day = self.day;
        let st: &'a Value = self.st;
        let r: &'a Value = self.r;
        let evk = vi(r, "evk");
        // a real attack: the aftermath, and the threats that were being watched are over
        if evk == cl::EV_ATTACK {
            for t in self.thr("thr") {
                self.t_remove(vi(&t, "i"));
            }
            let a = self.pick(2, &Pk { fight: true, ..Pk::default() })?;
            if a.len() >= 2 {
                self.add2(88.0, "thr_after", &[Some(a[0]), Some(a[1])], None)?;
            }
        }
        // joins
        let arr = int_of(self.al["cool"].get("arr").unwrap_or(&Value::from(-99)));
        if self.arrivals_ok && day >= arr + 2 && (people_of(st).len() < 8 || self.rng.random() < 0.6) {
            let cands: Vec<&'a Value> = self.new_ids.iter().copied().filter(|q| vs(q, "s") == "h").take(1).collect();
            for p in cands {
                self.al["cool"]["arr"] = Value::from(day);
                self.rec(p)?;
                let orig = self.rv(p, "orig")?;
                let job = self.rv(p, "job")?;
                let helper = self.pick(1, &Pk { exclude: HashSet::from([vi(p, "i")]), ..Pk::default() })?;
                if let Some(h) = helper.first() {
                    self.add2(80.0, "arrive", &[Some(p), Some(h)], Some(json!({"orig": orig, "job": job})))?;
                    let nid = self.nid();
                    self.t_push(json!({"i": nid, "k": "arr", "s": day, "d": day + 2, "a": [vi(p, "i")], "x": {}}));
                    self.used.insert(vi(p, "i"));
                }
            }
        }
        // deaths remembered
        let dead_today: Vec<Value> = self.dead_today.iter().take(1).cloned().collect();
        for d in dead_today {
            let mem_cool = int_of(self.al["cool"].get("mem").unwrap_or(&Value::from(-99)));
            let cause_ok = d.get("c").and_then(|c| c.as_str()).map_or(false, |c| MEM_CAUSES.contains(&c));
            if !cause_ok || day < mem_cool + 3 {
                // after a massacre one scene, not twelve
                continue;
            }
            self.al["cool"]["mem"] = Value::from(day);
            let kid = d.get("k").filter(|k| truthy(k)).map(int_of);
            let k = kid.and_then(|i| self.alive.get(&i).copied());
            let mourner: Option<&'a Value> = match k {
                Some(k) if !self.used.contains(&vi(k, "i")) && !self.away().contains(&vi(k, "i")) && vs(k, "s") != "s" => Some(k),
                _ => self.pick1()?.first().copied(),
            };
            let Some(mourner) = mourner else { continue };
            let dp = json!({"n": d["n"].clone(), "g": d["g"].clone(), "r": d["r"].clone()});
            self.add(85.0, "memorial", &[Some(mourner), None, None, Some(&dp)], Some(json!({"keep": d["keep"].clone()})), None, false)?;
            if self.rng.random() < 0.75 {
                let nid = self.nid();
                let dd = day + rr(self.rng.randint(5, 8))?;
                self.t_push(json!({"i": nid, "k": "mem", "s": day, "d": dd, "a": [vi(mourner, "i")], "x": {"dead": d["i"].clone()}}));
            }
        }
        // a recovery: the real state went from sick to well
        for p in people_of(st) {
            self.rec(p)?;
            if self.rv(p, "ls")?.as_str() == Some("sick_placeholder") {
                continue;
            }
            let ls = self.rv(p, "ls")?;
            if ls.as_str() == Some("s") && vs(p, "s") != "s" {
                let tender = self.pick(1, &Pk { role_w: &[("medic", 5.0)], exclude: HashSet::from([vi(p, "i")]), ..Pk::default() })?;
                if let Some(t) = tender.first() {
                    if !self.used.contains(&vi(p, "i")) && !self.recovered_named.contains(&pystr(&p["n"])) {
                        let ill = il_or_fever(p);
                        self.add(72.0, "recovered", &[Some(p), Some(t)], Some(json!({"ill": ill})), None, true)?;
                        self.used.insert(vi(p, "i"));
                    }
                }
            }
        }
        // the sick tended
        let mut sick: Vec<&'a Value> = Vec::new();
        for p in people_of(st) {
            if vs(p, "s") == "s" && day - self.rvi(p, "vg")? >= 2 && !self.used.contains(&vi(p, "i")) {
                sick.push(p);
            }
        }
        if !sick.is_empty() && self.rng.random() < 0.7 {
            let p = *rr(self.rng.choice(&sick))?;
            let tender = self.pick(1, &Pk { role_w: &[("medic", 6.0)], exclude: HashSet::from([vi(p, "i")]), ..Pk::default() })?;
            if let Some(t) = tender.first() {
                self.rw(p, "vg", Value::from(day))?;
                let pool = if vs(p, "il") == "spores" { "vigil_spore" } else { "vigil" };
                self.add(64.0, pool, &[Some(p), Some(t)], Some(json!({"ill": il_or_fever(p)})), None, true)?;
                self.used.insert(vi(p, "i"));
            }
        }
        // rations
        let a = self.pick(2, &Pk::default())?;
        if a.len() == 2 {
            let sym = json!({"sup": self.sup, "need": self.need});
            let ds = vi(st, "ds");
            let ss = vi(st, "ss");
            if self.deficit && self.rng.random() < 0.72 {
                let mut pool = if ds >= 3 && self.rng.random() < 0.5 { "rat_streak_bad" } else { "rat_deficit" };
                if pool == "rat_deficit" && (self.sup as f64) >= 0.85 * (self.need as f64) {
                    // a small shortfall gets a small line (no famine talk at 23 of 24)
                    pool = "rat_mild";
                }
                self.add2(75.0, pool, &[Some(a[0]), Some(a[1])], Some(if pool == "rat_deficit" || pool == "rat_mild" { sym } else { json!({}) }))?;
            } else if !self.deficit && self.need > 0 && self.rng.random() < 0.4 {
                let pool = if ss >= 3 && self.rng.random() < 0.5 { "rat_streak_good" } else { "rat_surplus" };
                self.add2(50.0, pool, &[Some(a[0]), Some(a[1])], Some(if pool == "rat_surplus" { sym } else { json!({}) }))?;
            }
        }
        Ok(())
    }

    // --- the threads
    // py: clan_alife.py:513 advance
    fn advance(&mut self) -> R<()> {
        let day = self.day;
        let ids: Vec<i64> = va(self.al, "T").iter().map(|t| vi(t, "i")).collect();
        for id in ids {
            let Some(t) = va(self.al, "T").iter().find(|t| vi(t, "i") == id).cloned() else { continue };
            match vs(&t, "k") {
                "exp" => self.exp(&t)?,
                "inj" => self.inj(&t)?,
                "proj" => self.proj(&t)?,
                "trade" => self.trade(&t)?,
                "thr" => self.thr_step(&t)?,
                "qrl" => self.qrl(&t)?,
                "men" => self.men(&t)?,
                "mem" => {
                    if day >= vi(&t, "d") {
                        let who = ints(&t, "a").first().copied().unwrap_or(-1);
                        let p = self.person(who);
                        let wait = t["x"].get("wait").map(int_of).unwrap_or(0);
                        if let Some(p) = p {
                            if (self.away().contains(&vi(p, "i")) || self.used.contains(&vi(p, "i")) || vs(p, "s") == "s") && wait < 3 {
                                // the mourner is away / busy / ill: tomorrow
                                if let Some(tm) = self.t_mut(id) {
                                    tm["x"]["wait"] = Value::from(wait + 1);
                                    tm["d"] = Value::from(day + 1);
                                }
                                continue;
                            }
                        }
                        self.t_remove(id);
                        let dead_id = t["x"].get("dead").map(int_of).unwrap_or(-1);
                        let d = va(self.al, "D").iter().find(|x| vi(x, "i") == dead_id).cloned();
                        if let (Some(p), Some(d)) = (p, d) {
                            if !self.used.contains(&vi(p, "i")) && vs(p, "s") != "s" && !self.away().contains(&vi(p, "i")) {
                                let dp = json!({"n": d["n"].clone(), "g": d["g"].clone(), "r": d["r"].clone()});
                                self.add(40.0, "memorial_late", &[Some(p), None, None, Some(&dp)], Some(json!({"keep": d["keep"].clone()})), None, false)?;
                            }
                        }
                    }
                }
                "arr" => {
                    if day >= vi(&t, "d") {
                        self.t_remove(id);
                        let who = ints(&t, "a").first().copied().unwrap_or(-1);
                        let p = self.person(who);
                        let h = self.pick(1, &Pk { exclude: HashSet::from([who]), ..Pk::default() })?;
                        if let (Some(p), Some(h)) = (p, h.first()) {
                            if vs(p, "s") == "h" && !self.used.contains(&vi(p, "i")) && !self.away().contains(&vi(p, "i")) {
                                self.add2(42.0, "arrive_follow", &[Some(p), Some(h)], None)?;
                            }
                        }
                    }
                }
                _ => {}
            }
        }
        Ok(())
    }

    // py: clan_alife.py:550 _exp
    fn exp(&mut self, t: &Value) -> R<()> {
        let day = self.day;
        let id = vi(t, "i");
        let party: Vec<&'a Value> = ints(t, "a").iter().filter_map(|i| self.alive.get(i).copied()).collect();
        let lost_ids = ints(&t["x"], "lost");
        let lost: Vec<Value> = va(self.al, "D").iter().filter(|x| lost_ids.contains(&vi(x, "i"))).cloned().collect();
        if day < vi(t, "d") {
            if day > vi(t, "s") && !party.is_empty() && self.rng.random() < 0.55 {
                let camp = self.pick(1, &Pk { fight: true, ..Pk::default() })?;
                if let Some(c) = camp.first() {
                    let nd = self.days_gone(t);
                    self.add(34.0, "exp_wait", &[Some(c), Some(party[0])], Some(json!({"dest": t["x"]["dest"].clone(), "nd": nd})), None, true)?;
                }
            }
            return Ok(());
        }
        self.t_remove(id);
        if party.is_empty() {
            return Ok(());
        }
        let xx = &t["x"];
        let sym = json!({"dest": xx["dest"].clone(), "goal": xx["goal"].clone(), "nd": (day - vi(t, "s")).max(1)});
        for p in &party {
            self.used.insert(vi(p, "i"));
        }
        if let Some(d) = lost.first() {
            let dp = json!({"n": d["n"].clone(), "g": d["g"].clone(), "r": d["r"].clone()});
            self.add2(90.0, "exp_lost", &[Some(party[0]), None, None, Some(&dp)], Some(sym))?;
            return Ok(());
        }
        if xx.get("recall").map_or(false, |r| truthy(r)) {
            let sick = self.alive.get(&vi(xx, "recall")).copied();
            if let Some(sick) = sick {
                if party.len() >= 2 {
                    let lead = party.iter().copied().find(|p| vi(p, "i") != vi(sick, "i")).unwrap_or(party[0]);
                    self.add(66.0, "exp_recall", &[Some(lead), Some(sick)], Some(sym), Some("worried"), true)?;
                    return Ok(());
                }
            }
        }
        let a = party[0];
        let b = if party.len() > 1 { party[1] } else { party[0] };
        if party.len() < 2 {
            return Ok(());
        }
        let roll = self.rng.random();
        let morale = morale_of(self.st);
        let m = if morale >= 55.0 { 1.0 } else if morale < 35.0 { -1.0 } else { 0.0 };
        let p_good = 0.5 + 0.2 * m + 0.05 * (vi(self.st, "ss").min(3) as f64);
        let mut ppl: Vec<Option<&Value>> = vec![Some(a), Some(b)];
        if party.len() > 2 {
            ppl.push(Some(party[2]));
        }
        if roll < 0.12 {
            let hurt = *rr(self.rng.choice(&party))?;
            let others: Vec<&Value> = party.iter().copied().filter(|p| vi(p, "i") != vi(hurt, "i")).collect();
            let mut inj_keys: Vec<&str> = x::injuries().as_object().map(|o| o.keys().map(|k| k.as_str()).collect()).unwrap_or_default();
            inj_keys.sort();
            let inj = *rr(self.rng.choice(&inj_keys))?;
            let mut s2 = sym.clone();
            s2["inj"] = Value::from(inj);
            self.add2(60.0, "exp_ret_hurt", &[Some(hurt), Some(others[0])], Some(s2))?;
            let nid = self.nid();
            let dd = day + rr(self.rng.randint(2, 4))?;
            self.t_push(json!({"i": nid, "k": "inj", "s": day, "d": dd, "a": [vi(hurt, "i")], "x": {"inj": inj}}));
        } else if roll < 0.22 {
            self.add2(62.0, "exp_ret_contact", &ppl, Some(sym))?;
        } else if roll < 0.30 {
            let inf_list = data::strs(data::t("clan_story_data", "INFECTED"));
            let inf = *rr(self.rng.choice(&inf_list))?;
            let mut s2 = sym.clone();
            s2["inf"] = Value::from(inf);
            self.add2(58.0, "exp_ret_inf", &ppl, Some(s2))?;
        } else if roll < 0.30 + p_good * 0.7 {
            self.add2(70.0, "exp_ret_good", &ppl, Some(sym))?;
        } else {
            self.add2(52.0, "exp_ret_empty", &ppl, Some(sym))?;
        }
        Ok(())
    }

    // py: clan_alife.py:599 _inj
    fn inj(&mut self, t: &Value) -> R<()> {
        let day = self.day;
        let who = ints(t, "a").first().copied().unwrap_or(-1);
        let p = self.person(who);
        let Some(p) = p.filter(|p| vs(p, "s") == "h") else {
            self.t_remove(vi(t, "i"));
            return Ok(());
        };
        let inj = t["x"]["inj"].clone();
        if day >= vi(t, "d") {
            self.t_remove(vi(t, "i"));
            let b = self.pick(1, &Pk { exclude: HashSet::from([vi(p, "i")]), fight: true, ..Pk::default() })?;
            if !self.used.contains(&vi(p, "i")) {
                let mut ppl = vec![Some(p)];
                ppl.extend(b.iter().map(|q| Some(*q)));
                self.add2(45.0, "inj_heal", &ppl, Some(json!({"inj": inj})))?;
            }
        } else if day > vi(t, "s") && self.rng.random() < 0.4 && !self.used.contains(&vi(p, "i")) {
            let b = self.pick(1, &Pk { exclude: HashSet::from([vi(p, "i")]), fight: true, ..Pk::default() })?;
            let mut ppl = vec![Some(p)];
            ppl.extend(b.iter().map(|q| Some(*q)));
            self.add2(30.0, "inj_mid", &ppl, Some(json!({"inj": inj})))?;
        }
        Ok(())
    }

    // py: clan_alife.py:614 _proj
    fn proj(&mut self, t: &Value) -> R<()> {
        let day = self.day;
        let id = vi(t, "i");
        let xx = &t["x"];
        let workers: Vec<&'a Value> = ints(t, "a").iter().filter_map(|i| self.alive.get(i).copied()).collect();
        let away = self.away();
        let hurt = self.hurt();
        let ok: Vec<&'a Value> = workers.iter().copied().filter(|p| vs(p, "s") == "h" && !away.contains(&vi(p, "i")) && !hurt.contains(&vi(p, "i")) && !self.used.contains(&vi(p, "i"))).collect();
        if day == vi(t, "s") {
            return Ok(());
        }
        if self.deficit || ok.len() < 2 {
            if ok.len() >= 2 && self.rng.random() < 0.6 {
                self.add2(35.0, "proj_stall", &[Some(ok[0]), Some(ok[1])], Some(json!({"proj": xx["proj"].clone()})))?;
            } else if ok.len() < 2 {
                let extra = self.pick(2 - ok.len(), &Pk { exclude: ok.iter().map(|p| vi(p, "i")).collect(), h_only: true, fight: true, ..Pk::default() })?;
                let mut all: Vec<i64> = ok.iter().map(|p| vi(p, "i")).collect();
                all.extend(extra.iter().map(|p| vi(p, "i")));
                if !all.is_empty() {
                    if let Some(tm) = self.t_mut(id) {
                        tm["a"] = jints(&all);
                    }
                }
            }
            return Ok(());
        }
        let p_new = vi(xx, "p") + 1;
        let n = vi(xx, "n");
        if let Some(tm) = self.t_mut(id) {
            tm["x"]["p"] = Value::from(p_new);
        }
        if p_new >= n {
            self.t_remove(id);
            let proj = xx["proj"].clone();
            if let Some(a) = self.al["done"].as_array_mut() {
                a.push(proj.clone());
                let l = a.len();
                if l > 8 {
                    a.drain(..l - 8);
                }
            }
            self.add2(60.0, "proj_done", &[Some(ok[0]), Some(ok[1])], Some(json!({"proj": proj})))?;
            return Ok(());
        }
        let prog = ((4 * p_new) as f64 / n as f64).trunc() as i64;
        let prog = prog.min(3);
        let last = xx.get("last");
        if (last.is_none() || last.map(int_of) != Some(prog) || last.map_or(false, |l| l.is_null())) && self.rng.random() < 0.85 {
            if let Some(tm) = self.t_mut(id) {
                tm["x"]["last"] = Value::from(prog);
            }
            self.add2(40.0, "proj_prog", &[Some(ok[0]), Some(ok[1])], Some(json!({"proj": xx["proj"].clone(), "prog": prog})))?;
        }
        Ok(())
    }

    // py: clan_alife.py:640 _trade
    fn trade(&mut self, t: &Value) -> R<()> {
        let day = self.day;
        if day < vi(t, "d") {
            return Ok(());
        }
        self.t_remove(vi(t, "i"));
        let a = self.pick(2, &Pk { fight: true, ..Pk::default() })?;
        if a.len() < 2 {
            return Ok(());
        }
        let xx = &t["x"];
        let sym = json!({"grp": xx["grp"].clone(), "dest": xx["dest"].clone(), "goal": xx["goal"].clone(), "due": t["d"].clone()});
        self.add2(50.0, "tr_meet", &[Some(a[0]), Some(a[1])], Some(sym))?;
        let good = self.rng.random() < (0.35 + if morale_of(self.st) >= 55.0 { 0.25 } else { 0.0 });
        let nid = self.nid();
        let mut x2 = xx.clone();
        x2["good"] = Value::from(good);
        self.t_push(json!({"i": nid, "k": "tradeend", "s": day, "d": day + 1, "a": [vi(a[0], "i")], "x": x2}));
        Ok(())
    }

    // py: clan_alife.py:654 _thr
    fn thr_step(&mut self, t: &Value) -> R<()> {
        let day = self.day;
        let xx = &t["x"];
        if day == vi(t, "s") {
            return Ok(());
        }
        let stage = xx.get("st").map(int_of).unwrap_or(0);
        let a = self.pick(2, &Pk { fight: true, ..Pk::default() })?;
        if day >= vi(t, "d") {
            self.t_remove(vi(t, "i"));
            if a.len() >= 2 {
                self.add2(46.0, "thr_down", &[Some(a[0]), Some(a[1])], Some(json!({"dest": xx["dest"].clone()})))?;
            }
            return Ok(());
        }
        if stage == 0 && a.len() >= 2 && self.rng.random() < 0.55 {
            if let Some(tm) = self.t_mut(vi(t, "i")) {
                tm["x"]["st"] = Value::from(1);
            }
            self.add2(58.0, "thr_up", &[Some(a[0]), Some(a[1])], Some(json!({"dest": xx["dest"].clone()})))?;
        }
        Ok(())
    }

    // py: clan_alife.py:670 _qrl
    fn qrl(&mut self, t: &Value) -> R<()> {
        let day = self.day;
        if day >= vi(t, "d") {
            let a = ints(t, "a");
            let away = self.away();
            if a.iter().take(2).any(|i| away.contains(i)) {
                // one of them is out on an expedition: the making-up waits for the return
                if let Some(tm) = self.t_mut(vi(t, "i")) {
                    tm["d"] = Value::from(day + 1);
                }
                return Ok(());
            }
            self.t_remove(vi(t, "i"));
            let (pa, pb) = (self.person(a[0]), self.person(a[1]));
            if let (Some(pa), Some(pb)) = (pa, pb) {
                if !self.used.contains(&vi(pa, "i")) && !self.used.contains(&vi(pb, "i")) && vs(pa, "s") != "s" && vs(pb, "s") != "s" {
                    self.add2(44.0, "reconcile", &[Some(pa), Some(pb)], None)?;
                    self.rw(pa, "rv", Value::from(0))?;
                    self.rw(pb, "rv", Value::from(0))?;
                }
            }
        }
        Ok(())
    }

    // py: clan_alife.py:683 _men
    fn men(&mut self, t: &Value) -> R<()> {
        let day = self.day;
        let a = ints(t, "a");
        let (pa, pb) = (self.person(a[0]), self.person(a[1]));
        if day >= vi(t, "d") {
            self.t_remove(vi(t, "i"));
            if let Some(b) = pb {
                let sk = self.rvi(b, "sk")?;
                self.rw(b, "sk", Value::from(sk + 1))?;
            }
            if let (Some(pa), Some(pb)) = (pa, pb) {
                let away = self.away();
                if !self.used.contains(&vi(pa, "i")) && !self.used.contains(&vi(pb, "i")) && vs(pa, "s") == "h" && vs(pb, "s") == "h" && !(away.contains(&vi(pa, "i")) || away.contains(&vi(pb, "i"))) {
                    self.add2(38.0, "mentor", &[Some(pa), Some(pb)], Some(json!({"skill": t["x"]["skill"].clone()})))?;
                }
            }
        }
        Ok(())
    }

    // --- new threads and ambient scenes
    // py: clan_alife.py:694 spawn
    fn spawn(&mut self) -> R<()> {
        let day = self.day;
        let st: &'a Value = self.st;
        let fk = self.fk;
        let morale = morale_of(st);
        let size = people_of(st).len() as i64;
        let ready = |al: &Value, k: &str, n: i64| day >= int_of(al["cool"].get(k).unwrap_or(&Value::from(-99))) + n;
        let sick_n = people_of(st).iter().filter(|p| vs(p, "s") == "s").count();
        let hungry_n = people_of(st).iter().filter(|p| vs(p, "s") == "u").count();
        // an expedition
        let n_exp = self.nthr("exp") as i64;
        if day >= 1 && n_exp < 1 + size / 16 && ready(self.al, "exp", 2) && self.rng.random() < (if morale <= 25.0 { 0.2 } else { 0.55 }) {
            let eligible: Vec<&'a Value> = self.free(&Pk { h_only: true, ..Pk::default() }).into_iter().filter(|p| !matches!(vs(p, "r"), "kid" | "elder")).collect();
            if eligible.len() >= 2 && self.free(&Pk::default()).len() >= 3 {
                let rw: [(&str, f64); 8] = [("scout", 3.0), ("scavenger", 3.0), ("guard", 2.0), ("mechanic", 1.0), ("medic", 0.6), ("cook", 0.4), ("gardener", 0.4), ("radio", 0.3)];
                let k = if size >= 10 && eligible.len() >= 4 && self.rng.random() < 0.4 { 3 } else { 2 };
                let mut party: Vec<&'a Value> = Vec::new();
                let mut pool = eligible.clone();
                while !pool.is_empty() && party.len() < k {
                    let mut w = Vec::with_capacity(pool.len());
                    for p in &pool {
                        let lm = self.rvi(p, "lm")?;
                        let m = rw.iter().find(|(r, _)| *r == vs(p, "r")).map(|(_, v)| *v).unwrap_or(1.0);
                        w.push((1.0 + ((day - lm).min(14)) as f64) * m);
                    }
                    let qi = rr(self.rng.choices_weighted(&w, 1))?[0];
                    party.push(pool.remove(qi));
                }
                let places: Vec<&str> = x::places().get(fk).and_then(|o| o.as_object()).map(|o| o.keys().map(|k| k.as_str()).collect()).unwrap_or_default();
                let lastd = ints_str(&self.al["lastdest"]);
                let tail: Vec<&String> = lastd.iter().rev().take(3).collect();
                let dests: Vec<&str> = places.iter().copied().filter(|d| !tail.iter().any(|t| t.as_str() == *d)).collect();
                let dest = *rr(self.rng.choice(if dests.is_empty() { &places } else { &dests }))?;
                let mut ld = lastd.clone();
                ld.push(dest.to_string());
                let n = ld.len();
                if n > 6 {
                    ld.drain(..n - 6);
                }
                self.al["lastdest"] = Value::Array(ld.into_iter().map(Value::from).collect());
                let goals = x::pool_keys(x::goods(), fk);
                let pref: Vec<&str> = if sick_n > 0 { vec!["antibiotics", "bandages"] } else if hungry_n > 0 { vec!["cans", "salt"] } else { vec![] };
                let goal = if !pref.is_empty() && self.rng.random() < 0.6 {
                    let g: Vec<&str> = pref.iter().copied().filter(|g| goals.contains(g)).collect();
                    *rr(self.rng.choice(&g))?
                } else {
                    *rr(self.rng.choice(&goals))?
                };
                let nd = [1i64, 2, 3][rr(self.rng.choices_weighted(&[45.0, 40.0, 15.0], 1))?[0]];
                self.al["cool"]["exp"] = Value::from(day);
                let nid = self.nid();
                let pids: Vec<i64> = party.iter().map(|p| vi(p, "i")).collect();
                self.t_push(json!({"i": nid, "k": "exp", "s": day, "d": day + nd, "a": jints(&pids), "x": {"dest": dest, "goal": goal}}));
                let ppl: Vec<Option<&Value>> = party.iter().map(|p| Some(*p)).collect();
                self.add(55.0, "exp_leave", &ppl, Some(json!({"dest": dest, "goal": goal, "nd": nd})), None, true)?;
            }
        }
        // a project
        if (self.nthr("proj") as i64) < 1 + size / 20 && ready(self.al, "proj", 4) && morale >= 30.0 && self.rng.random() < 0.35 {
            let done = ints_str(&self.al["done"]);
            let running: Vec<String> = self.thr("proj").iter().map(|t| vs(&t["x"], "proj").to_string()).collect();
            let keys: Vec<&str> = x::pool_keys(x::projects(), fk).into_iter().filter(|k| !done.iter().any(|d| d == k) && !running.iter().any(|d| d == k)).collect();
            let w = self.pick(2, &pk_h_fight())?;
            if !keys.is_empty() && w.len() == 2 {
                let proj = *rr(self.rng.choice(&keys))?;
                self.al["cool"]["proj"] = Value::from(day);
                let nid = self.nid();
                let n = rr(self.rng.randint(4, 6))?;
                self.t_push(json!({"i": nid, "k": "proj", "s": day, "d": day + 30, "a": [vi(w[0], "i"), vi(w[1], "i")], "x": {"proj": proj, "p": 0, "n": n}}));
                self.add2(54.0, "proj_start", &[Some(w[0]), Some(w[1])], Some(json!({"proj": proj})))?;
            }
        }
        // a trade
        if self.thr("trade").is_empty() && self.thr("tradeend").is_empty() && day >= 3 && ready(self.al, "trade", 8) && self.rng.random() < 0.16 {
            let a = self.pick(1, &pk_h_fight())?;
            if let Some(a0) = a.first() {
                let grps = x::pool_keys(x::groups(), fk);
                let grp = *rr(self.rng.choice(&grps))?;
                let places: Vec<&str> = x::places().get(fk).and_then(|o| o.as_object()).map(|o| o.keys().map(|k| k.as_str()).collect()).unwrap_or_default();
                let dest = *rr(self.rng.choice(&places))?;
                let goals = x::pool_keys(x::goods(), fk);
                let due = day + rr(self.rng.randint(2, 4))?;
                self.al["cool"]["trade"] = Value::from(day);
                let nid = self.nid();
                let goal = *rr(self.rng.choice(&goals))?;
                self.t_push(json!({"i": nid, "k": "trade", "s": day, "d": due, "a": [vi(a0, "i")], "x": {"grp": grp, "dest": dest, "goal": goal}}));
                self.add2(36.0, "tr_rumor", &[Some(a0)], Some(json!({"grp": grp, "dest": dest, "due": due})))?;
            }
        }
        // the day after a meeting: how it went
        for t in self.thr("tradeend") {
            self.t_remove(vi(&t, "i"));
            let who = ints(&t, "a").first().copied().unwrap_or(-1);
            let a = self.person(who);
            let b = self.pick(1, &Pk { exclude: HashSet::from([who]), fight: true, ..Pk::default() })?;
            if let Some(a) = a {
                if vs(a, "s") != "s" && !self.used.contains(&vi(a, "i")) {
                    let xx = &t["x"];
                    let pool = if xx.get("good").map_or(false, truthy) { "tr_good" } else { "tr_bad" };
                    let mut ppl = vec![Some(a)];
                    ppl.extend(b.iter().map(|q| Some(*q)));
                    self.add2(40.0, pool, &ppl, Some(json!({"grp": xx["grp"].clone(), "dest": xx["dest"].clone(), "goal": xx["goal"].clone()})))?;
                }
            }
        }
        // a threat
        if self.thr("thr").is_empty() && ready(self.al, "thr", 4) && self.rng.random() < 0.22 {
            let a = self.pick(2, &pk_h_fight())?;
            if a.len() == 2 {
                let subs = ["thr_enemy", "thr_inf", "thr_smoke"];
                let sub = subs[rr(self.rng.choices_weighted(&[4.0, 3.0, 1.0], 1))?[0]];
                let places: Vec<&str> = x::places().get(fk).and_then(|o| o.as_object()).map(|o| o.keys().map(|k| k.as_str()).collect()).unwrap_or_default();
                let dest = *rr(self.rng.choice(&places))?;
                self.al["cool"]["thr"] = Value::from(day);
                let nid = self.nid();
                let dd = day + rr(self.rng.randint(2, 3))?;
                self.t_push(json!({"i": nid, "k": "thr", "s": day, "d": dd, "a": [vi(a[0], "i"), vi(a[1], "i")], "x": {"dest": dest, "sub": sub}}));
                let inf_list = data::strs(data::t("clan_story_data", "INFECTED"));
                let inf = *rr(self.rng.choice(&inf_list))?;
                self.add2(56.0, sub, &[Some(a[0]), Some(a[1])], Some(json!({"dest": dest, "inf": inf})))?;
            }
        }
        // relationships
        if self.rng.random() < 0.34 {
            self.social()?;
        }
        // a rumour
        if self.rng.random() < 0.18 {
            let a = self.pick(2, &Pk::default())?;
            if a.len() == 2 {
                self.add2(28.0, "rumor", &[Some(a[0]), Some(a[1])], None)?;
            }
        }
        // a small camp incident (re/clan-events-1117.md): text only. It has its own random stream (seed, run, day).
        let mut irng = PyRandom::from_str(&format!("{}:{}:inc:{}", pystr(&self.al["seed"]), pystr(&self.al["run"]), day));
        if *INCIDENTS && ready(self.al, "inc", 6) && irng.random() < 0.2 {
            let mut kinds = vec!["inc_spoil", "inc_tres", "inc_horde"];
            if sick_n > 0 {
                kinds.push("inc_sick");
            }
            std::mem::swap(&mut self.rng, &mut irng);
            let res = self.incident(&kinds);
            std::mem::swap(&mut self.rng, &mut irng);
            res?;
        }
        Ok(())
    }

    /// the body of the `try:` of the incident block (self.rng is the incident stream)
    fn incident(&mut self, kinds: &[&str]) -> R<()> {
        let a = self.pick(2, &pk_h_fight())?;
        if a.len() == 2 {
            self.al["cool"]["inc"] = Value::from(self.day);
            let pool = *rr(self.rng.choice(kinds))?;
            self.add2(34.0, pool, &[Some(a[0]), Some(a[1])], None)?;
        }
        Ok(())
    }

    // py: clan_alife.py:789 _social
    /// One relationship beat. Bonds are persistent.
    fn social(&mut self) -> R<()> {
        let day = self.day;
        let fk = self.fk;
        let free = self.free(&Pk::default());
        if free.len() < 2 {
            return Ok(());
        }
        let ids: HashMap<i64, &'a Value> = free.iter().map(|p| (vi(p, "i"), *p)).collect();
        let mut bonded: Vec<(&'a Value, &'a Value)> = Vec::new(); // free friends (A's friend is B, both free)
        for p in &free {
            let f = self.rvi(p, "f")?;
            if let Some(q) = ids.get(&f) {
                if vi(p, "i") < vi(q, "i") {
                    bonded.push((*p, *q));
                }
            }
        }
        let mut opts: Vec<(&str, f64)> = vec![("friend", 3.0), ("mentor", 2.0)];
        if !bonded.is_empty() {
            opts.push(("quarrel", 3.0));
            if day - int_of(self.al["cool"].get("rom").unwrap_or(&Value::from(-99))) >= 6 {
                opts.push(("romance", 1.0));
            }
        }
        let w: Vec<f64> = opts.iter().map(|(_, w)| *w).collect();
        let mut kind = opts[rr(self.rng.choices_weighted(&w, 1))?[0]].0;
        if kind == "quarrel" || kind == "romance" {
            let mut cand: Vec<(&'a Value, &'a Value)> = Vec::new();
            for b in &bonded {
                if kind == "quarrel" || (self.rvi(b.0, "pa")? == 0 && self.rvi(b.1, "pa")? == 0) {
                    cand.push(*b);
                }
            }
            if cand.is_empty() {
                kind = "friend";
            } else {
                let (a, b) = *rr(self.rng.choice(&cand))?;
                if kind == "quarrel" {
                    self.rw(a, "rv", Value::from(vi(b, "i")))?;
                    self.rw(b, "rv", Value::from(vi(a, "i")))?;
                    let nid = self.nid();
                    let dd = day + rr(self.rng.randint(2, 4))?;
                    self.t_push(json!({"i": nid, "k": "qrl", "s": day, "d": dd, "a": [vi(a, "i"), vi(b, "i")], "x": {}}));
                    self.add2(42.0, "quarrel", &[Some(a), Some(b)], None)?;
                } else {
                    self.rw(a, "pa", Value::from(vi(b, "i")))?;
                    self.rw(b, "pa", Value::from(vi(a, "i")))?;
                    self.al["cool"]["rom"] = Value::from(day);
                    self.add2(38.0, "romance", &[Some(a), Some(b)], None)?;
                }
                return Ok(());
            }
        }
        let a = self.pick(2, &Pk::default())?;
        if a.len() < 2 {
            return Ok(());
        }
        let (pa, pb) = (a[0], a[1]);
        if kind == "friend" && self.rvi(pa, "f")? == 0 && self.rvi(pb, "f")? == 0 {
            self.rw(pa, "f", Value::from(vi(pb, "i")))?;
            self.rw(pb, "f", Value::from(vi(pa, "i")))?;
            self.add2(40.0, "friend", &[Some(pa), Some(pb)], None)?;
            return Ok(());
        }
        let mut busy: HashSet<i64> = HashSet::new();
        for t in self.thr("men") {
            let l = ints(&t, "a");
            busy.insert(l[1]);
            busy.insert(l[0]);
        }
        if busy.contains(&vi(pa, "i")) || busy.contains(&vi(pb, "i")) {
            return Ok(());
        }
        let (ska, skb) = (self.rvi(pa, "sk")?, self.rvi(pb, "sk")?);
        let (mentor, app) = if vs(pa, "r") == "elder" || vs(pb, "r") == "kid" || ska >= skb { (pa, pb) } else { (pb, pa) };
        let mut skills = x::pool_keys(x::skills(), fk);
        if kd::is_kid(pa) || kd::is_kid(pb) {
            // a kid learns a trade, never a weapon: no rifle, no bow, no clicker lore
            let f: Vec<&str> = skills.iter().copied().filter(|k| !(kd::combat_text(x::skills()[*k][1].as_str().unwrap_or(""), Some("en")) || kd::combat_text(x::skills()[*k][2].as_str().unwrap_or(""), Some("pl")))).collect();
            skills = if f.is_empty() { vec!["knot"] } else { f };
        }
        let skill = *rr(self.rng.choice(&skills))?;
        let nid = self.nid();
        self.t_push(json!({"i": nid, "k": "men", "s": day, "d": day + 3, "a": [vi(mentor, "i"), vi(app, "i")], "x": {"skill": skill}}));
        self.add2(38.0, "mentor", &[Some(mentor), Some(app)], Some(json!({"skill": skill})))?;
        Ok(())
    }

    // py: clan_alife.py:844 ambient
    /// Fill the day: a camp scene by role or a general one.
    fn ambient(&mut self) -> R<()> {
        let a = self.pick(3, &Pk::default())?;
        if a.is_empty() {
            return Ok(());
        }
        let role_pool = format!("amb_{}", vs(a[0], "r"));
        if has_pool(&role_pool) && self.rng.random() < 0.55 {
            let mut ppl = vec![Some(a[0])];
            ppl.extend(a.iter().skip(1).take(1).map(|q| Some(*q)));
            self.add2(20.0, &role_pool, &ppl, None)?;
        } else {
            let ppl: Vec<Option<&Value>> = a.iter().map(|q| Some(*q)).collect();
            self.add2(20.0, "amb_any", &ppl, None)?;
        }
        Ok(())
    }

    // py: clan_alife.py:858 run
    // --- the whole day
    fn run(&mut self, newcomers: Vec<&'a Value>) -> R<Vec<Value>> {
        self.arrivals_ok = !newcomers.is_empty();
        self.new_ids = newcomers;
        self.sync()?;
        self.real_day()?;
        self.advance()?;
        self.spawn()?;
        let want = DAY_MIN_ROWS + if self.rng.random() < 0.4 { 1 } else { 0 };
        let mut guard = 0;
        while self.events.len() < want && guard < 4 {
            self.ambient()?;
            guard += 1;
        }
        self.events.sort_by(|a, b| (-a.0).partial_cmp(&(-b.0)).unwrap_or(std::cmp::Ordering::Equal));
        let events = std::mem::take(&mut self.events);
        let mut out = Vec::new();
        let mut pools: HashSet<String> = HashSet::new();
        for (_d, ev) in events.iter() {
            if out.len() >= want {
                break;
            }
            if pools.contains(&ev.pool) {
                continue;
            }
            if let Some(msg) = compose(self.al, self.st, ev, &mut self.rng, self.day, self.wx)? {
                out.push(msg);
                pools.insert(ev.pool.clone());
            }
        }
        // the real states, for tomorrow's transitions (a recovery)
        let st: &'a Value = self.st;
        for p in people_of(st) {
            self.rw(p, "ls", p.get("s").cloned().unwrap_or(Value::Null))?;
        }
        Ok(out)
    }
}

fn il_or_fever(p: &Value) -> Value {
    match p.get("il") {
        Some(v) if truthy(v) => v.clone(),
        _ => Value::from("fever"),
    }
}

/// the strings of a JSON array
fn ints_str(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().map(pystr).collect()).unwrap_or_default()
}

// py: clan_alife.py:886 on_day
/// Called by clan_story.on_day AFTER the roster was advanced to the clan state `newc` for the day result `r`. Returns the A-Life feed rows of the day
/// (dicts like clan_story's: kind "alife", mood, text, names, lid, params); they are stored by clan_story._store. [] when switched off / clan too small.
pub fn on_day(c: &Connection, tid: &str, st: &Value, newc: &Value, r: &Value, story_msgs: &[Value]) -> Result<Vec<Value>, String> {
    let _ = newc;
    if !enabled() || !truthy(st) || !truthy(r) {
        return Ok(Vec::new());
    }
    ensure(c).map_err(|e| e.to_string())?;
    let mut al = load(c, tid);
    let mut new = false;
    let fresh = match &al {
        None => true,
        Some(a) => a.get("run") != st.get("run") || a.get("fac") != st.get("fac") || vi(st, "day") < vi(a, "day"),
    };
    let mut al = if fresh {
        new = true;
        new_state(tid, st)
    } else {
        al.take().ok_or("state")?
    };
    if vi(&al, "day") == vi(st, "day") {
        return Ok(Vec::new());
    }
    let fk = fk_of(st);
    for p in people_of(st) {
        rec_ensure(&mut al, p, fk)?;
    }
    let rng = PyRandom::from_str(&format!("{}:{}:{}:day", tid, pystr(&st["run"]), pystr(&st["day"])));
    let mut out = Vec::new();
    if people_of(st).len() >= MIN_CLAN {
        // the newcomers of today: unseen before, joined today (never on the very first sight of a clan: those are its founders)
        let newcomers: Vec<&Value> = if new { Vec::new() } else { people_of(st).iter().filter(|p| !truthy(rget(&al, vi(p, "i")).get("seen").unwrap_or(&Value::Null)) && vi(p, "jd") == vi(st, "day")).collect() };
        let mut d = Day::new(&mut al, st, r, rng, story_msgs)?;
        out = d.run(newcomers)?;
    }
    for p in people_of(st) {
        rset(&mut al, vi(p, "i"), "seen", Value::from(1));
    }
    al["day"] = st["day"].clone();
    let days = vi(&al["stat"], "days");
    al["stat"]["days"] = Value::from(days + 1);
    save(c, tid, &mut al).map_err(|e| e.to_string())?;
    Ok(out)
}

// ------------------------------------------------------------------ the menu ticker: facts of the simulated life, one sentence each
fn fact_w(cls: &str) -> i64 {
    tb("FACT_W").get(cls).map(int_of).unwrap_or(1)
}

fn byid(st: &Value, i: i64) -> Option<&Value> {
    people_of(st).iter().find(|p| vi(p, "i") == i)
}

pub struct Fact {
    pub cls: String,
    pub key: String,
    pub pool: String,
    pub people: Value,
    pub sym: Value,
}

// py: clan_alife.py:927 facts
/// The current facts of the clan's simulated life that may be told, validated against the live roster (alive, right state).
pub fn facts(al: &Value, st: &Value) -> R<Vec<Fact>> {
    let mut out: Vec<(String, String, String, Value, Value)> = Vec::new();
    let day = vi(st, "day");
    let base = base_sym(st, day);
    let mut away: HashSet<i64> = HashSet::new();
    for t in va(al, "T") {
        if vs(t, "k") == "exp" {
            away.extend(ints(t, "a"));
        }
    }
    for t in va(al, "T") {
        let ids = ints(t, "a");
        let ps: Vec<Option<&Value>> = ids.iter().map(|i| byid(st, *i)).collect();
        if ps.iter().any(|p| p.is_none()) || ps.is_empty() {
            continue;
        }
        let ps: Vec<&Value> = ps.into_iter().flatten().collect();
        let k = vs(t, "k");
        let xx = &t["x"];
        let ti = vi(t, "i");
        if k == "exp" && vi(t, "d") > day && ps.iter().all(|p| vs(p, "s") == "h") {
            out.push(("f_exp".into(), format!("exp{}", ti), "f_exp".into(), json!({"A": pp(ps[0]), "B": pp(ps[ps.len() - 1])}), json!({"dest": xx["dest"].clone(), "goal": xx["goal"].clone(), "nd": (vi(t, "d") - day).max(1)})));
        } else if k == "inj" && vs(ps[0], "s") == "h" {
            out.push(("f_inj".into(), format!("inj{}", ti), "f_inj".into(), json!({"A": pp(ps[0])}), json!({"inj": xx["inj"].clone()})));
        } else if k == "proj" && ps.len() >= 2 {
            let prog = (((4 * vi(xx, "p")) as f64) / (vi(xx, "n") as f64)).trunc() as i64;
            let prog = prog.min(3);
            out.push(("f_proj".into(), format!("proj{}", ti), "f_proj".into(), json!({"A": pp(ps[0]), "B": pp(ps[1])}), json!({"proj": xx["proj"].clone(), "prog": prog})));
        } else if k == "trade" && vi(t, "d") >= day {
            out.push(("f_trade".into(), format!("trade{}", ti), "f_trade".into(), json!({"A": pp(ps[0])}), json!({"grp": xx["grp"].clone(), "dest": xx["dest"].clone(), "due": t["d"].clone()})));
        } else if k == "qrl" && ps.len() >= 2 {
            out.push(("f_rival".into(), format!("qrl{}", ti), "f_rival".into(), json!({"A": pp(ps[0]), "B": pp(ps[1])}), json!({})));
        } else if k == "men" && ps.len() >= 2 && ps.iter().all(|p| vs(p, "s") == "h") && {
            let sk = opt(x::skills().get(vs(xx, "skill")), "skill")?;
            !(ps.iter().any(|p| kd::is_kid(p)) && kd::combat_text(sk[1].as_str().unwrap_or(""), Some("en")))
        } {
            out.push(("f_mentor".into(), format!("men{}", ti), "f_mentor".into(), json!({"A": pp(ps[0]), "B": pp(ps[1])}), json!({"skill": xx["skill"].clone()})));
        }
    }
    let mut seen_pairs: HashSet<(String, i64, i64)> = HashSet::new();
    for p in people_of(st) {
        let r = al.get("P").and_then(|m| m.get(vi(p, "i").to_string())).filter(|r| truthy(r));
        let Some(r) = r else { continue };
        if away.contains(&vi(p, "i")) {
            continue;
        }
        let pe = json!({"A": pp(p)});
        let s = vs(p, "s");
        if s == "h" || s == "u" {
            for n in 0..2usize {
                out.push(("f_trait".into(), format!("tr{}_{}", vi(p, "i"), n), format!("f_trait:{}", trait_of(r, n)?), pe.clone(), json!({})));
            }
            out.push(("f_job".into(), format!("job{}", vi(p, "i")), "f_job".into(), pe.clone(), json!({"job": r["job"].clone()})));
            let rpool = format!("f_role:{}", vs(p, "r"));
            let rp = if BY_POOL.contains_key(rpool.as_str()) { rpool } else { "f_role".to_string() };
            out.push(("f_role".into(), format!("role{}", vi(p, "i")), rp, pe.clone(), json!({})));
            if vi(p, "jd") > 0 && vi(p, "jd") >= day - 20 {
                out.push(("f_origin".into(), format!("orig{}", vi(p, "i")), "f_origin".into(), pe.clone(), json!({"orig": r["orig"].clone(), "jd": p["jd"].clone()})));
            }
            for (key, pool) in [("f", "f_friend"), ("pa", "f_romance")] {
                let q = if r.get(key).map_or(false, truthy) { byid(st, vi(r, key)) } else { None };
                let pair = q.map(|q| {
                    let (a, b) = (vi(p, "i"), vi(q, "i"));
                    if a <= b { (a, b) } else { (b, a) }
                });
                if let (Some(q), Some(pair)) = (q, pair) {
                    let qs = vs(q, "s");
                    let tag = (pool.to_string(), pair.0, pair.1);
                    if (qs == "h" || qs == "u") && !seen_pairs.contains(&tag) && (pool != "f_friend" || !r.get("pa").map_or(false, truthy)) {
                        seen_pairs.insert(tag);
                        out.push((pool.into(), format!("{}{}_{}", pool, pair.0, pair.1), pool.into(), json!({"A": pp(p), "B": pp(q)}), json!({})));
                    }
                }
            }
        } else if s == "s" {
            let mut tend: Vec<&Value> = people_of(st).iter().filter(|q| vs(q, "s") == "h" && vi(q, "i") != vi(p, "i") && !away.contains(&vi(q, "i"))).collect();
            if !tend.is_empty() {
                tend.sort_by_key(|q| (vs(q, "r") != "medic", vi(q, "i")));
                out.push(("f_vigil".into(), format!("vig{}", vi(p, "i")), "f_vigil".into(), json!({"A": pp(p), "B": pp(tend[0])}), json!({"ill": il_or_fever(p)})));
            }
        }
    }
    for d in va(al, "D") {
        let c = d.get("c").and_then(|c| c.as_str());
        if day - vi(d, "d") <= 20 && c.map_or(false, |c| MEM_CAUSES.contains(&c)) {
            let k = if d.get("k").map_or(false, truthy) { byid(st, vi(d, "k")) } else { None };
            if let Some(k) = k {
                if vs(k, "s") == "h" && !away.contains(&vi(k, "i")) {
                    out.push(("f_memorial".into(), format!("mem{}", vi(d, "i")), "f_memorial".into(), json!({"A": pp(k), "D": {"n": d["n"].clone(), "g": d["g"].clone(), "r": d["r"].clone()}}), json!({"keep": d["keep"].clone()})));
                }
            }
        }
    }
    let wx = weather(al, st, day)?;
    out.push(("wx".into(), format!("wx{}", day), format!("wx_{}", wx), json!({}), json!({})));
    out.push(("f_season".into(), "season".into(), "f_season".into(), json!({}), json!({})));
    out.push(("f_count".into(), "count".into(), "f_count".into(), json!({}), json!({})));
    if vi(st, "ss") >= 2 {
        out.push(("f_rat_good".into(), "ratg".into(), "f_rat_good".into(), json!({}), json!({})));
    }
    if vi(st, "ds") >= 2 {
        out.push(("f_rat_bad".into(), "ratb".into(), "f_rat_bad".into(), json!({}), json!({})));
    }
    Ok(out
        .into_iter()
        .map(|(cls, key, pool, people, sy)| {
            let mut sym = base.clone();
            if let Some(o) = sy.as_object() {
                for (k, v) in o {
                    sym.insert(k.clone(), v.clone());
                }
            }
            Fact { cls, key, pool, people, sym: Value::Object(sym) }
        })
        .collect())
}

// py: clan_alife.py:1003 daily_facts
/// The facts of today's ticker: everything that is happening plus a day-specific sample of the lasting ones.
pub fn daily_facts(al: &Value, st: &Value, tid: &str) -> R<Vec<Fact>> {
    let fs = facts(al, st)?;
    let (mut dyn_, mut per): (Vec<Fact>, Vec<Fact>) = fs.into_iter().partition(|f| !PERSISTENT.contains(&f.cls.as_str()));
    per.sort_by(|a, b| a.key.cmp(&b.key));
    let k = (DAILY_FACTS as i64 - dyn_.len() as i64).min(per.len() as i64).max(0) as usize;
    if !per.is_empty() {
        let mut rng = PyRandom::from_str(&format!("sub:{}:{}:{}", tid, pystr(&st["run"]), pystr(&st["day"])));
        let idx = rr(rng.sample_indices(per.len(), k))?;
        let mut slots: Vec<Option<Fact>> = per.into_iter().map(Some).collect();
        for i in idx {
            dyn_.push(opt(slots[i].take(), "sample")?);
        }
    }
    Ok(dyn_)
}

// py: clan_alife.py:1014 _rows
fn rows_of(pool: &str, fk: &str, morale: f64, letters: &BTreeSet<char>, av_en: &HashSet<String>, av_pl: &HashSet<String>, people: Option<&Value>) -> Vec<&'static str> {
    let mut out = Vec::new();
    for tid in BY_POOL.get(pool).map(|v| v.as_slice()).unwrap_or(&[]) {
        let Some(t) = REG.get(tid) else { continue };
        if (t.sc == "*" || t.sc == fk) && tone_ok(t.tone, morale) && t.letters.is_subset(letters) && subset(&t.ve, av_en) && subset(&t.vp, av_pl) && !people.map_or(false, |p| truthy(p) && !kd::kid_safe(t.en, p, &[t.pl])) {
            out.push(*tid);
        }
    }
    out
}

fn letters_of_people(people: &Value) -> BTreeSet<char> {
    people.as_object().map(|o| o.keys().filter_map(|k| k.chars().next()).collect()).unwrap_or_default()
}

// py: clan_alife.py:1023 _deck
/// The shuffled deck of one epoch: every fact w times (its class weight), no two of one class in a row where avoidable. Items = (fact index, copy).
fn deck(fs: &[Fact], seed: &str, epoch: i64) -> Vec<(usize, i64)> {
    let mut order: Vec<usize> = (0..fs.len()).collect();
    order.sort_by(|a, b| fs[*a].key.cmp(&fs[*b].key));
    let mut items: Vec<(usize, i64)> = Vec::new();
    for i in order {
        for m in 0..fact_w(&fs[i].cls) {
            items.push((i, m));
        }
    }
    PyRandom::from_str(&format!("{}:{}", seed, epoch)).shuffle(&mut items);
    for j in 1..items.len() {
        let prev = fs[items[j - 1].0].cls.as_str();
        if fs[items[j].0].cls == prev {
            let nxt = items.get(j + 1).map(|x| fs[x.0].cls.as_str());
            for q in j + 1..items.len() {
                let cq = fs[items[q].0].cls.as_str();
                if cq != prev && Some(cq) != nxt {
                    items.swap(j, q);
                    break;
                }
            }
        }
    }
    items
}

// py: clan_alife.py:1040 _variant
/// The n-th phrasing of a fact: a seeded permutation of its rows, so the same fact is told in all its phrasings before any repeats.
fn variant(rows: &[&'static str], key: &str, n: i64) -> &'static str {
    let mut perm = rows.to_vec();
    PyRandom::from_str(&format!("v:{}", key)).shuffle(&mut perm);
    perm[py::pymod(n, perm.len() as i64) as usize]
}

// py: clan_alife.py:1047 state_for_read
/// The A-Life state of a clan for the ticker / views: the stored one, else a deterministic fresh one from the live roster (in memory, or stored when persist).
pub fn state_for_read(c: &Connection, tid: &str, st: &Value, persist: bool) -> R<Option<Value>> {
    if !enabled() {
        return Ok(None);
    }
    let fk = fk_of(st);
    let al = load(c, tid);
    let stale = match &al {
        None => true,
        Some(a) => a.get("run") != st.get("run") || a.get("fac") != st.get("fac"),
    };
    if stale {
        let mut al = new_state(tid, st);
        al["day"] = st["day"].clone();
        for p in people_of(st) {
            rec_ensure(&mut al, p, fk)?;
            rset(&mut al, vi(p, "i"), "seen", Value::from(1));
        }
        if persist {
            // the in-memory state is the same
            let _ = ensure(c).and_then(|_| save(c, tid, &mut al));
        }
        Ok(Some(al))
    } else {
        let mut al = al.ok_or("state")?;
        for p in people_of(st) {
            // members who joined while the switch was off
            if al["P"].get(vi(p, "i").to_string()).is_none() {
                rec_ensure(&mut al, p, fk)?;
                rset(&mut al, vi(p, "i"), "seen", Value::from(1));
            }
        }
        Ok(Some(al))
    }
}

// py: clan_alife.py:1072 ticker_lines
/// n ticker lines [(id, text)] of the simulated life for a time slot: a pure function of (clan, state, slot, language). [] when switched off / too small.
/// (save = also create the stored state when it is missing; a read-only caller passes False and gets the same lines.)
pub fn ticker_lines(c: &Connection, tid: &str, st: &Value, slot: i64, n: usize, lang: &str, now: Option<f64>, save: bool) -> Result<Vec<(Value, String)>, String> {
    let _ = now;
    if n == 0 || !truthy(st) || people_of(st).len() < MIN_CLAN {
        return Ok(Vec::new());
    }
    let Some(al) = state_for_read(c, tid, st, save)? else { return Ok(Vec::new()) };
    let fk = fk_of(st);
    let fs = daily_facts(&al, st, tid)?;
    if fs.is_empty() {
        return Ok(Vec::new());
    }
    let seed = format!("{}:{}:{}", tid, pystr(&st["run"]), pystr(&st["day"]));
    let morale = morale_of(st);
    let l = deck(&fs, &seed, 0).len() as i64;
    let mut decks: HashMap<i64, Vec<(usize, i64)>> = HashMap::new();
    let pl = pln::norm_lang(lang) == "pl";
    let lg = if pl { "pl" } else { "en" };
    let mut out = Vec::new();
    let day = vi(st, "day");
    for i in 0..n as i64 {
        let k = slot * TICKER_SHARE as i64 + i;
        let mut found: Option<(i64, usize, i64, Vec<&'static str>)> = None;
        for attempt in 0..8 {
            let e = py::floordiv(k + attempt, l);
            let j = py::pymod(k + attempt, l) as usize;
            let dk = decks.entry(e).or_insert_with(|| deck(&fs, &seed, e));
            let (fi, m) = dk[j];
            let f = &fs[fi];
            let (av_en, av_pl) = avail(&f.sym)?;
            let letters = letters_of_people(&f.people);
            let mut rows = rows_of(&f.pool, fk, morale, &letters, &av_en, &av_pl, Some(&f.people));
            if rows.is_empty() {
                rows = rows_of(&f.pool, fk, 50.0, &letters, &av_en, &av_pl, Some(&f.people));
            }
            if !rows.is_empty() {
                found = Some((e, fi, m, rows));
                break;
            }
        }
        let Some((e, fi, m, rows)) = found else { continue }; // a fact with no phrasing for the mood is skipped (rare)
        let f = &fs[fi];
        let w = fact_w(&f.cls);
        let first = variant(&rows, &format!("{}:{}", tid, f.key), day + e * w + m);
        let mut ids: Vec<&str> = vec![first];
        let mut trng = PyRandom::from_str(&format!("tail:{}:{}:{}:{}", tid, day, slot, i));
        if TAIL_OK.contains(&f.cls.as_str()) && trng.random() < 0.5 {
            let wx = weather(&al, st, day)?;
            let tpool = if trng.random() < 0.3 { format!("f_tail_{}", wx) } else { "f_tail".to_string() };
            let none = BTreeSet::new();
            let noset = HashSet::new();
            let tails = rows_of(&tpool, fk, morale, &none, &noset, &noset, None);
            if !tails.is_empty() {
                let tl = *rr(trng.choice(&tails))?;
                let head = render_ids(&[first], &f.people, &f.sym, lg, fk)?;
                let tail = render_ids(&[tl], &json!({}), &f.sym, lg, fk)?;
                if head.chars().count() + 1 + tail.chars().count() <= cs::WIDGET_MAX {
                    ids.push(tl);
                }
            }
        }
        let text = render_ids(&ids, &f.people, &f.sym, lg, fk)?;
        if kd::any_kid(&f.people) && !kd::kid_text_ok(&text, &f.people, lg) {
            continue; // (a kid's fact that came out as combat is not told)
        }
        out.push((Value::String(format!("a{}{:02}", py::pymod(slot, 100000), i)), cs::widget_text(&text)));
    }
    Ok(out)
}

// ------------------------------------------------------------------ read-only views (admin site, update server)
// py: clan_alife.py:1122 read_state
/// A JSON-able view of the clan's simulation for the admin site and the launcher: people with their extras, the running threads, today's weather, the
/// variety statistics. {} without a state.
pub fn read_state(c: &Connection, tid: &str, lang: &str) -> R<Value> {
    let st = cs::_load(c, tid);
    let al = load(c, tid);
    let (Some(st), Some(al)) = (st.filter(|s| truthy(s)), al.filter(|a| truthy(a))) else { return Ok(json!({})) };
    if al.get("run") != st.get("run") {
        return Ok(json!({}));
    }
    let pl = pln::norm_lang(lang) == "pl";
    let fk = fk_of(&st);
    let mut people = Vec::new();
    let mut away: HashSet<i64> = HashSet::new();
    for t in va(&al, "T") {
        if vs(t, "k") == "exp" {
            away.extend(ints(t, "a"));
        }
    }
    let roles = data::t("clan_story_data", "ROLES");
    let nm = |i: &Value| -> String {
        if !truthy(i) {
            return String::new();
        }
        byid(&st, int_of(i)).map(|q| pystr(&q["n"])).unwrap_or_default()
    };
    for p in people_of(&st) {
        let empty = json!({});
        let r = al.get("P").and_then(|m| m.get(vi(p, "i").to_string())).unwrap_or(&empty);
        let tr: Vec<Value> = if truthy(r) {
            let mut v = Vec::new();
            for n in 0..2usize {
                let id = trait_of(r, n)?;
                v.push(opt(tb("TRAIT_LABEL").get(id).and_then(|l| l.get(if pl { 1 } else { 0 })).cloned(), "TRAIT_LABEL")?);
            }
            v
        } else {
            Vec::new()
        };
        let job = r.get("job").and_then(|j| j.as_str()).and_then(|j| x::jobs().get(j));
        let orig = r.get("orig").and_then(|j| j.as_str()).and_then(|j| x::origins().get(j));
        let state = match vs(p, "s") {
            "h" => "healthy",
            "u" => "hungry",
            "s" => "sick",
            _ => return Err("KeyError: state".into()),
        };
        people.push(json!({"name": p["n"].clone(), "role": opt(roles.get(vs(p, "r")).and_then(|r| r.get(if fk == "W" { 0 } else { 1 })).cloned(), "ROLES")?, "state": state,
                           "traits": tr, "friend": nm(r.get("f").unwrap_or(&Value::Null)), "rival": nm(r.get("rv").unwrap_or(&Value::Null)), "partner": nm(r.get("pa").unwrap_or(&Value::Null)),
                           "skill": r.get("sk").cloned().unwrap_or_else(|| Value::from(0)),
                           "away": away.contains(&vi(p, "i")), "past": job.map(|j| j[1].clone()).unwrap_or_else(|| Value::from("")),
                           "origin": orig.map(|o| o[1].clone()).unwrap_or_else(|| Value::from("")), "since": p["jd"].clone()}));
    }
    let mut threads = Vec::new();
    for f in facts(&al, &st)? {
        if matches!(f.cls.as_str(), "f_exp" | "f_inj" | "f_proj" | "f_trade" | "f_rival" | "f_mentor") {
            let (a, b) = avail(&f.sym)?;
            let rows = rows_of(&f.pool, fk, 50.0, &letters_of_people(&f.people), &a, &b, None);
            if !rows.is_empty() {
                // one phrasing per thread and day (not always the pool's first line)
                let pick = rows[(crc(&format!("{}:{}:{}", pystr(&al["seed"]), f.key, vi(&st, "day"))) as usize) % rows.len()];
                let text = render_ids(&[pick], &f.people, &f.sym, if pl { "pl" } else { "en" }, fk)?;
                threads.push(json!({"kind": &f.cls[2..], "text": text}));
            }
        }
    }
    let mem = al["mem"].as_object().cloned().unwrap_or_default();
    let cores = mem.keys().filter(|t| !t.starts_with("wx_")).count();
    let ticker: Vec<Value> = ticker_lines(c, tid, &st, cs::slot_of(None), TICKER_SHARE, lang, None, false)?.into_iter().map(|(_i, t)| Value::from(t)).collect();
    let memorial: Vec<Value> = va(&al, "D").iter().map(|d| json!({"name": d["n"].clone(), "day": d["d"].clone(), "cause": d["c"].clone()})).collect();
    Ok(json!({"enabled": enabled(), "day": al["day"].clone(), "weather": weather(&al, &st, vi(&st, "day"))?,
              "season": opt(x::season_name(x::season_of(cl::week_day(vi(&st, "day")).0), pl), "season")?,
              "people": people, "threads": threads, "memorial": memorial,
              "stats": {"rows": al["stat"]["lines"].clone(), "days": al["stat"]["days"].clone(), "windowRepeats": al["stat"]["rep"].clone(), "templatesUsed": mem.len(), "coreTemplatesUsed": cores,
                        "templatesTotal": REG.len()},
              "ticker": ticker}))
}

// py: clan_alife.py:1164 summary
/// One line about a clan's simulation for lists (admin site): {} without a state.
pub fn summary(c: &Connection, tid: &str) -> Value {
    let Some(al) = load(c, tid).filter(|a| truthy(a)) else { return json!({}) };
    json!({"day": al["day"].clone(), "threads": va(&al, "T").len(), "people": al["P"].as_object().map_or(0, |o| o.len()), "rows": al["stat"]["lines"].clone(),
           "templatesUsed": al["mem"].as_object().map_or(0, |o| o.len())})
}

// py: clan_alife.py:1172 public
/// What the launcher shows next to the journal (update server /clanfeed.json "alife"): {} unless the switch is on and a simulation exists.
pub fn public(c: &Connection, tid: &str, lang: &str) -> Value {
    if !enabled() {
        return json!({});
    }
    let rs = match read_state(c, tid, lang) {
        Ok(r) => r,
        Err(_) => return json!({}),
    };
    if !truthy(&rs) {
        return json!({});
    }
    let ongoing: Vec<Value> = va(&rs, "threads").iter().map(|t| t["text"].clone()).take(4).collect();
    json!({"day": rs["day"].clone(), "weather": rs["weather"].clone(), "season": rs["season"].clone(), "ongoing": ongoing})
}

// py: clan_alife.py:1186 view
/// Everything the admin site shows about one clan's A-Life, in both languages, read-only.
pub fn view(c: &Connection, tid: &str) -> Value {
    let mut out = json!({"enabled": enabled(), "exists": false});
    let en = read_state(c, tid, "en").unwrap_or_else(|_| json!({})); // a view never breaks the page
    if !truthy(&en) {
        return out;
    }
    let pl = read_state(c, tid, "pl").unwrap_or_else(|_| json!({}));
    let mut traits = Map::new();
    for p in va(&pl, "people") {
        traits.insert(pystr(&p["name"]), p["traits"].clone());
    }
    out["exists"] = Value::from(true);
    out["state"] = en;
    out["pl"] = json!({"threads": va(&pl, "threads").iter().map(|t| t["text"].clone()).collect::<Vec<_>>(), "season": pl.get("season").cloned().unwrap_or(Value::Null), "traits": traits});
    let menu = |lang: &str| -> Vec<Value> { cs::menu_lines(c, tid, 6, lang, None, false).into_iter().map(|(_i, t)| Value::from(t)).collect() };
    out["menu"] = json!({"en": menu("en"), "pl": menu("pl")});
    out
}
