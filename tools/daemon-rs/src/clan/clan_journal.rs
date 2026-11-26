//! py: clan_journal.py - the clan journal: ONE entry per clan day, written from what that day really did to the clan (the difference of the roster before /
//! after the day and the day's result). Records are language-neutral (template ids + people + numbers) and rendered at read time. Text = the generated
//! data (clan_journal_text / _pl); the logic is ported 1:1, including the order of every random draw. State / records = `serde_json::Value`.
use crate::clan::clan::{self as cl, gi, int_of, pystr, truthy};
use crate::clan::clan_kids as kd;
use crate::clan::clan_story as cs;
use crate::clan::clan_story_pl as pln;
use crate::clan::data;
use crate::py;
use regex::Regex;
use rusqlite::{params, Connection};
use serde_json::{json, Map, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::{PyRandom, RandError};

pub type R<T> = Result<T, String>;

pub const REC_V: i64 = 1;
pub const MAX_PARAS: usize = 8;
/// sentences of THE CAMP paragraph (more changes than that are listed by name in one closing sentence)
pub const CAMP_MAX: i64 = 5;
/// a camp-life note is added only to an entry with fewer sentences than this
pub const NOTE_BELOW: usize = 5;
/// supplies this far over the need = "plenty" (own choice of the mark, not a 2013 number)
pub const PLENTY_OVER: i64 = 6;
const NUM: [&str; 15] = ["sup", "need", "short", "over", "k", "x", "r", "hs", "dw", "lost", "gain", "n", "size", "week", "more"];

fn rr<T>(r: Result<T, RandError>) -> R<T> {
    r.map_err(|e| format!("random: {:?}", e))
}

fn en_t(name: &str) -> &'static Value {
    data::t("clan_journal_text", name)
}

fn pl_t(name: &str) -> &'static Value {
    data::t("clan_journal_text_pl", name)
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

fn people_of(st: &Value) -> &[Value] {
    va(st, "people")
}

// ------------------------------------------------------------------ templates and rendering
static KEY: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{([\p{L}\p{N}_]+)\}").expect("KEY"));
static SW: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{([A-D]+)\?[^|{}]*\|[^|{}]*\}").expect("SW"));

// py: clan_journal.py:46 _keys
/// The base placeholders a template needs: {A_he} -> A, {E_gen} -> E, {ill_acc} -> ill, {A?x|y} -> A.
fn keys_of(t: &str) -> BTreeSet<String> {
    let mut k: BTreeSet<String> = KEY.captures_iter(t).filter_map(|m| m.get(1)).map(|g| g.as_str().split('_').next().unwrap_or("").to_string()).collect();
    for m in SW.captures_iter(t) {
        if let Some(g) = m.get(1) {
            for ch in g.as_str().chars() {
                k.insert(ch.to_string());
            }
        }
    }
    k
}

fn en_lines(pool: &str) -> &'static [Value] {
    en_t("POOLS").get(pool).and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

fn line_en(pool: &str, i: usize) -> Option<(&'static str, &'static str)> {
    let r = en_lines(pool).get(i)?;
    Some((r.get(0)?.as_str()?, r.get(1)?.as_str()?))
}

fn line_pl(pool: &str, i: usize) -> Option<&'static str> {
    pl_t("POOLS").get(pool)?.get(i)?.as_str()
}

static NEED: LazyLock<parking_lot::Mutex<HashMap<(String, usize), BTreeSet<String>>>> = LazyLock::new(|| parking_lot::Mutex::new(HashMap::new()));

// py: clan_journal.py:54 _need
fn need(pool: &str, i: usize) -> R<BTreeSet<String>> {
    if let Some(n) = NEED.lock().get(&(pool.to_string(), i)) {
        return Ok(n.clone());
    }
    let (_, en) = line_en(pool, i).ok_or("KeyError: pool")?;
    let pl = line_pl(pool, i).ok_or("KeyError: pl pool")?;
    let mut k = keys_of(en);
    k.extend(keys_of(pl));
    NEED.lock().insert((pool.to_string(), i), k.clone());
    Ok(k)
}

// py: clan_journal.py:58 _repeatable
fn repeatable(pool: &str) -> bool {
    pool.starts_with("t_") || pool.starts_with("f_")
}

fn jn_used<'a>(st: &'a mut Value, pool: &str) -> &'a mut Vec<Value> {
    if st.get("jn").is_none() {
        st["jn"] = json!({});
    }
    if st["jn"].get("u").is_none() {
        st["jn"]["u"] = json!({});
    }
    if st["jn"]["u"].get(pool).is_none() {
        st["jn"]["u"][pool] = json!([]);
    }
    st["jn"]["u"][pool].as_array_mut().expect("used list")
}

// py: clan_journal.py:62 _pick
/// An unused line of the pool for the faction whose placeholders are all there -> index, or None (the pool ran dry / nothing fits). `avoid` None = the
/// default `()`; Some(..) (even empty) = a spent pool gives its least recently used line again instead of None (the status cards).
fn pick(st: &mut Value, rng: &mut PyRandom, pool: &str, fk: &str, have: &BTreeSet<String>, avoid: Option<&HashSet<i64>>, skip: Option<&HashSet<i64>>) -> R<Option<usize>> {
    let lines = en_lines(pool);
    let mut ok: Vec<usize> = Vec::new();
    for (i, r) in lines.iter().enumerate() {
        let sc = r.get(0).and_then(|s| s.as_str()).unwrap_or("");
        if (sc == "*" || sc == fk) && need(pool, i)?.is_subset(have) && !skip.map_or(false, |s| s.contains(&(i as i64))) {
            ok.push(i);
        }
    }
    if ok.is_empty() {
        return Ok(None);
    }
    if repeatable(pool) {
        return Ok(Some(*rr(rng.choice(&ok))?));
    }
    let used: Vec<i64> = jn_used(st, pool).iter().map(int_of).collect();
    let av = |i: i64| avoid.map_or(false, |a| a.contains(&i));
    let fresh: Vec<usize> = ok.iter().copied().filter(|i| !used.contains(&(*i as i64)) && !av(*i as i64)).collect();
    let i: usize;
    if !fresh.is_empty() {
        i = *rr(rng.choice(&fresh))?;
    } else if avoid.is_some() {
        let mut old: Vec<usize> = used.iter().filter(|u| **u >= 0 && ok.contains(&(**u as usize)) && !av(**u)).map(|u| *u as usize).collect();
        if old.is_empty() {
            old = ok.iter().copied().filter(|i| !av(*i as i64)).collect();
        }
        if old.is_empty() {
            old = ok.clone();
        }
        i = old[0];
        let u = jn_used(st, pool);
        if let Some(pos) = u.iter().position(|x| int_of(x) == i as i64) {
            u.remove(pos);
        }
    } else {
        return Ok(None);
    }
    jn_used(st, pool).push(Value::from(i as i64));
    Ok(Some(i))
}

fn join_names(names: &[Value], and_: &str) -> String {
    let n: Vec<String> = names.iter().map(pystr).collect();
    if n.len() == 1 {
        n[0].clone()
    } else {
        format!("{}{}{}", n[..n.len() - 1].join(", "), and_, n[n.len() - 1])
    }
}

// py: clan_journal.py:87 _vars
fn vars_of(sym: &Value, lang: &str) -> R<cs::Vars> {
    let fk = match sym.get("f").and_then(|f| f.as_str()) {
        Some(f @ ("W" | "S")) => f,
        _ => "W",
    };
    let mut v = cs::Vars::new();
    for k in NUM {
        if let Some(x) = sym.get(k) {
            v.insert(k.to_string(), pystr(x));
        }
    }
    let mp = sym.get("map").filter(|m| truthy(m));
    let ill = sym.get("ill").filter(|m| truthy(m));
    let lack = sym.get("lack").filter(|m| truthy(m));
    let names = sym.get("names").filter(|m| truthy(m));
    let maps = data::t("clan_chronicle_maps", "EN_SHORT");
    if lang == "pl" {
        let e = data::t("clan_story_pl", "ENEMY").get(fk).ok_or("KeyError: ENEMY")?;
        v.insert("E".into(), pystr(&e[0]));
        v.insert("E_gen".into(), pystr(&e[1]));
        v.insert("E_ins".into(), pystr(&e[2]));
        if let Some(m) = mp {
            let m = pystr(m);
            let short = maps.get(&m).map(pystr).unwrap_or(m.clone());
            v.insert("map".into(), data::t("clan_chronicle_maps", "PL").get(&short).map(pystr).unwrap_or(short));
        }
        if let Some(i) = ill {
            let ills = data::t("clan_story_pl", "ILL");
            let a = i.as_str().and_then(|k| ills.get(k)).filter(|x| truthy(x)).or_else(|| ills.get("none")).ok_or("KeyError: ILL")?;
            for (n, idx) in [("ill", 0usize), ("ill_gen", 1), ("ill_acc", 2), ("ill_ins", 3)] {
                v.insert(n.into(), pystr(&a[idx]));
            }
        }
        if let Some(l) = lack {
            let t = pl_t("LACK");
            v.insert("lack".into(), l.as_str().and_then(|k| t.get(k)).or_else(|| t.get("food")).map(pystr).unwrap_or_default());
        }
        if let Some(n) = names {
            v.insert("names".into(), join_names(n.as_array().map(|a| a.as_slice()).unwrap_or(&[]), pl_t("AND").as_str().unwrap_or(" i ")));
        }
    } else {
        let fac = data::t("clan_story_data", "FACTION").get(fk).ok_or("KeyError: FACTION")?;
        v.insert("E".into(), pystr(&fac["enemies"]));
        if let Some(m) = mp {
            let m = pystr(m);
            let m = maps.get(&m).map(pystr).unwrap_or(m);
            let no_art = data::strs(en_t("NO_ARTICLE")).contains(&m.as_str());
            v.insert("map".into(), if no_art || m.to_lowercase().starts_with("the ") { m } else { format!("the {}", m) });
        }
        if let Some(i) = ill {
            let ills = data::t("clan_story_data", "ILLNESS");
            match i.as_str().and_then(|k| ills.get(k)) {
                Some(r) => {
                    v.insert("ill".into(), pystr(&r[0]));
                    v.insert("ill_a".into(), pystr(&r[1]));
                }
                None => {
                    v.insert("ill".into(), "sickness".into());
                    v.insert("ill_a".into(), "a sickness".into());
                }
            }
        }
        if let Some(l) = lack {
            let t = en_t("LACK");
            v.insert("lack".into(), l.as_str().and_then(|k| t.get(k)).or_else(|| t.get("food")).map(pystr).unwrap_or_default());
        }
        if let Some(n) = names {
            v.insert("names".into(), join_names(n.as_array().map(|a| a.as_slice()).unwrap_or(&[]), en_t("AND").as_str().unwrap_or(" and ")));
        }
    }
    Ok(v)
}

// py: clan_journal.py:132 render
/// One stored sentence [lid "jn/<pool>.<index>", params {"p": {letter: [name, gender]}, "s": symbols}] in a language; "" when the line set no longer has it.
pub fn render(sent: &Value, lang: &str) -> String {
    render_opt(sent, lang).unwrap_or_default()
}

fn render_opt(sent: &Value, lang: &str) -> Option<String> {
    let a = sent.as_array()?;
    let (lid, params) = (a.get(0)?.as_str()?, a.get(1)?);
    let (pool, i) = lid.get(3..)?.rsplit_once('.')?;
    let i: usize = i.trim().parse().ok()?;
    let mut people = Map::new();
    if let Some(p) = params.get("p").filter(|p| truthy(p)) {
        for (k, v) in p.as_object()? {
            let g = match v.get(1)?.as_str() {
                Some(s @ ("m" | "f" | "n")) => s,
                _ => "n",
            };
            people.insert(k.clone(), json!({"n": pystr(v.get(0)?), "g": g}));
        }
    }
    let people = Value::Object(people);
    let empty = json!({});
    let sym = params.get("s").filter(|s| truthy(s)).unwrap_or(&empty);
    let lang = if pln::norm_lang(lang) == "pl" { "pl" } else { "en" };
    let t = if lang == "pl" {
        pln::render(line_pl(pool, i)?, &people, &vars_of(sym, "pl").ok()?)?
    } else {
        cs::render(line_en(pool, i)?.1, &people, &vars_of(sym, "en").ok()?)?
    };
    Some(pln::cap_first(&t))
}

/// One sentence [lid, params] writer (the Python `_Writer`).
struct Writer<'a> {
    st: &'a mut Value,
    rng: &'a mut PyRandom,
    fk: &'static str,
    base: Map<String, Value>,
}

type Ppl<'p> = &'p [(&'p str, Option<&'p Value>)];

impl<'a> Writer<'a> {
    // py: clan_journal.py:154 s
    /// A sentence of the pool (else of the ledger pool `fb`) -> [lid, params] or None.
    #[allow(clippy::too_many_arguments)]
    fn sx(&mut self, pool: &str, people: Ppl, fb: Option<&str>, avoid: Option<&HashSet<i64>>, skip: Option<&HashSet<i64>>, kw: &[(&str, Value)]) -> R<Option<Value>> {
        let people: Vec<(&str, &Value)> = people.iter().filter_map(|(k, p)| p.filter(|p| truthy(p)).map(|p| (*k, p))).collect();
        let mut sym = self.base.clone();
        for (k, v) in kw {
            if !v.is_null() {
                sym.insert(k.to_string(), v.clone());
            }
        }
        let mut have: BTreeSet<String> = people.iter().map(|(k, _)| k.to_string()).collect();
        have.extend(sym.keys().cloned());
        have.insert("E".into());
        for (n, pl) in [Some(pool), fb].iter().enumerate() {
            let Some(pl) = pl else { continue };
            if pl.is_empty() {
                continue;
            }
            let i = pick(self.st, self.rng, pl, self.fk, &have, avoid, if n == 0 { skip } else { None })?;
            if let Some(i) = i {
                let nd = need(pl, i)?;
                let mut pm = Map::new();
                for (k, p) in &people {
                    if nd.contains(*k) {
                        pm.insert(k.to_string(), json!([p["n"].clone(), p.get("g").cloned().unwrap_or_else(|| Value::from("n"))]));
                    }
                }
                let mut sm = Map::new();
                for (k, v) in &sym {
                    if nd.contains(k) {
                        sm.insert(k.clone(), v.clone());
                    }
                }
                sm.insert("f".into(), Value::from(self.fk));
                return Ok(Some(json!([format!("jn/{}.{}", pl, i), {"p": Value::Object(pm), "s": Value::Object(sm)}])));
            }
        }
        Ok(None)
    }

    fn s(&mut self, pool: &str, kw: &[(&str, Value)]) -> R<Option<Value>> {
        self.sx(pool, &[], None, None, None, kw)
    }

    fn sp(&mut self, pool: &str, people: Ppl, kw: &[(&str, Value)]) -> R<Option<Value>> {
        self.sx(pool, people, None, None, None, kw)
    }

    fn sfb(&mut self, pool: &str, fb: &str, kw: &[(&str, Value)]) -> R<Option<Value>> {
        self.sx(pool, &[], Some(fb), None, None, kw)
    }
}

fn set_of(name: &str) -> HashSet<i64> {
    data::set_items(data::t("clan_journal", name)).iter().map(int_of).collect()
}

// A raid line must not contradict the day's supplies (see the Python module).
static HAUL_WIN: LazyLock<HashSet<i64>> = LazyLock::new(|| set_of("_HAUL_WIN"));
static EMPTY_LOSS: LazyLock<HashSet<i64>> = LazyLock::new(|| set_of("_EMPTY_LOSS"));

// ------------------------------------------------------------------ the day
// py: clan_journal.py:178 snapshot
/// The roster before the day (clan_story.on_day takes it before story_day): {"p": {id: [name, gender, role, state, illness]}, "n": size}.
pub fn snapshot(st: &Value) -> R<Value> {
    let mut p = Map::new();
    for m in people_of(st) {
        p.insert(
            int_of(&m["i"]).to_string(),
            json!([m["n"].clone(), m.get("g").cloned().unwrap_or_else(|| Value::from("n")), m.get("r").cloned().unwrap_or_else(|| Value::from("")),
                   m.get("s").cloned().unwrap_or_else(|| Value::from("h")), m.get("il").cloned().unwrap_or_else(|| Value::from(""))]),
        );
    }
    Ok(json!({"p": p, "n": people_of(st).len()}))
}

fn names_of(ps: &[&Value], limit: usize) -> Value {
    Value::Array(ps.iter().take(limit).map(|p| p["n"].clone()).collect())
}

fn tone_rank(t: &str) -> i64 {
    match t {
        "bad" => 3,
        "warn" => 2,
        "good" => 1,
        _ => 0,
    }
}

fn death_pool(c: &str) -> &'static str {
    match c {
        "attack" => "death_attack",
        "illness" => "death_illness",
        "mercy" => "death_mercy",
        "starved" => "death_starved",
        "lost" => "death_lost",
        "walkout" => "death_walkout",
        "suicide" => "death_suicide",
        "bite" | "bite_kin" => "death_bite",
        "turned" => "death_turned",
        _ => "death_illness",
    }
}

fn c_or_illness(d: &Value) -> String {
    match d.get("c") {
        Some(v) if truthy(v) => pystr(v),
        _ => "illness".into(),
    }
}

fn il_or_fever(p: &Value) -> Value {
    match p.get("il") {
        Some(v) if truthy(v) => v.clone(),
        _ => Value::from("fever"),
    }
}

fn some_i(v: Option<&Value>) -> i64 {
    v.map(int_of).unwrap_or(0)
}

// py: clan_journal.py:192 compose
/// The record of the day just played (see the module doc). Pure but for the used-line lists in st["jn"]. -> (record, {member id: sentence}).
pub fn compose(st: &mut Value, before: &Value, newc: &Value, r: &Value, rng: &mut PyRandom) -> R<(Value, HashMap<i64, Value>)> {
    let fk = cs::_fkey(vi(st, "fac"));
    let day = r.get("day").map(int_of).unwrap_or_else(|| some_i(newc.get("day")) - 1 + 0) + 1;
    let day = if r.get("day").is_none() { (some_i(newc.get("day").or(Some(&Value::from(1)))) - 1) + 1 } else { day };
    let week = py::floordiv(day - 1, 7) + 1;
    let (hh, uu, ss) = (vi(newc, "healthy"), vi(newc, "hungry"), vi(newc, "sick"));
    let size = hh + uu + ss;
    let status = newc.get("status").map(int_of).unwrap_or(cl::ALIVE);
    let empty = json!({});
    let m = r.get("match").filter(|m| truthy(m)).unwrap_or(&empty);
    let left = r.get("left").map_or(false, truthy);
    let (sup, need) = (vi(r, "supplies"), vi(r, "needed"));
    let mp = match m.get("map") {
        Some(v) if truthy(v) => pystr(v),
        _ => String::new(),
    };
    let win: i64 = match m.get("win") {
        Some(Value::Bool(true)) => 1,
        Some(Value::Bool(false)) => 0,
        _ => -1,
    };
    let mg = |q: &str| -> i64 { m.get(q).filter(|v| truthy(v)).map(int_of).unwrap_or(0) };
    let (k, x, rv, hs, dw) = (mg("kills"), mg("execs"), mg("revives"), mg("headshots"), mg("downs"));
    let was = before.get("p").filter(|p| truthy(p)).cloned().unwrap_or_else(|| json!({}));
    let st_ro = st.clone();
    let now: Vec<&Value> = people_of(&st_ro).iter().collect();
    let now_by: HashMap<i64, &Value> = now.iter().map(|p| (vi(p, "i"), *p)).collect();
    let in_was = |i: i64| was.get(i.to_string()).is_some();
    let was_state = |i: i64| -> String { was.get(i.to_string()).and_then(|a| a.get(3)).and_then(|s| s.as_str()).unwrap_or("").to_string() };
    let away: HashSet<i64> = st_ro.get("ch").filter(|c| truthy(c)).and_then(|c| c.get("away")).and_then(|a| a.as_object()).map(|o| o.keys().filter_map(|k| k.parse().ok()).collect()).unwrap_or_default();
    let deaths: Vec<&Value> = va(&st_ro, "dead").iter().filter(|d| in_was(vi(d, "i")) && !now_by.contains_key(&vi(d, "i"))).collect();
    let joined: Vec<&Value> = now.iter().copied().filter(|p| !in_was(vi(p, "i"))).collect();
    let old: Vec<&Value> = now.iter().copied().filter(|p| in_was(vi(p, "i"))).collect();
    let new_sick: Vec<&Value> = old.iter().copied().filter(|p| vs(p, "s") == "s" && was_state(vi(p, "i")) != "s").collect();
    let recovered: Vec<&Value> = old.iter().copied().filter(|p| vs(p, "s") != "s" && was_state(vi(p, "i")) == "s").collect();
    let new_hungry: Vec<&Value> = old.iter().copied().filter(|p| vs(p, "s") == "u" && was_state(vi(p, "i")) == "h").collect();
    let fed: Vec<&Value> = old.iter().copied().filter(|p| vs(p, "s") == "h" && was_state(vi(p, "i")) == "u").collect();
    let ev_names = data::strs(en_t("EVENT_NAMES"));
    let evk = vi(r, "evk");
    let evt = vi(r, "evt");
    let evn = py::pymod(vi(r, "evn"), ev_names.len() as i64);
    let (evl, evg) = (vi(r, "evl"), vi(r, "evg"));
    let horde = evk == cl::EV_ATTACK && vi(r, "evn") == cl::AN_HORDE; // 11-17: the attack was a horde of infected
    let mut per: HashMap<i64, Value> = HashMap::new();
    let mut paras: Vec<Value> = Vec::new();
    let mut tone = "info";
    let worse = |t: &'static str, tone: &mut &'static str| {
        if tone_rank(t) > tone_rank(tone) {
            *tone = t;
        }
    };
    let mut w = Writer { st, rng, fk, base: { let mut b = Map::new(); b.insert("size".into(), Value::from(size)); b.insert("week".into(), Value::from(week)); b } };

    // ---- THE RAID
    let mut raid: Vec<Option<Value>> = Vec::new();
    let mpv = if mp.is_empty() { Value::Null } else { Value::from(mp.as_str()) };
    if left {
        raid.push(w.s("raid_left", &[("map", mpv.clone())])?);
        worse("warn", &mut tone);
    } else if vi(r, "parts") <= 0 && sup <= 0 {
        raid.push(w.s("raid_idle", &[("map", mpv.clone())])?);
        worse("warn", &mut tone);
    } else if !mp.is_empty() || truthy(m) {
        let pool = if win == 1 { "raid_win" } else if win == 0 { "raid_loss" } else { "raid_draw" };
        let skip: Option<&HashSet<i64>> = if pool == "raid_win" && sup < need {
            Some(&HAUL_WIN)
        } else if pool == "raid_loss" && sup >= need {
            Some(&EMPTY_LOSS)
        } else {
            None
        };
        // (a spent pool: no sentence - the facts line of the entry already says where and how it went; never a repeat)
        raid.push(w.sx(pool, &[], None, None, skip, &[("map", mpv.clone())])?);
        let deeds: [(f64, &str, &str, i64); 5] = [(x as f64 / 2.0, "deed_x", "x", x), (rv as f64 / 2.0, "deed_r", "r", rv), (hs as f64 / 4.0, "deed_hs", "hs", hs), (dw as f64 / 3.0, "deed_dw", "dw", dw), (k as f64 / 10.0, "deed_k", "k", k)];
        let mut best = &deeds[0];
        for d in &deeds[1..] {
            if d.0 > best.0 {
                best = d;
            }
        }
        if best.0 >= 1.0 {
            raid.push(w.s(best.1, &[(best.2, Value::from(best.3))])?);
        }
        worse(if win == 1 { "good" } else if win == 0 { "warn" } else { "info" }, &mut tone);
    }
    let raid: Vec<Value> = raid.into_iter().flatten().collect();
    let raid_at = paras.len();
    if !raid.is_empty() {
        paras.push(json!(["raid", "", raid]));
    }

    // ---- SUPPLIES
    if need > 0 {
        let lack = r.get("sup").filter(|s| truthy(s)).and_then(|s| s.get("cause")).filter(|c| truthy(c)).map(pystr).filter(|c| !c.is_empty());
        let (short, over) = ((need - sup).max(0), (sup - need).max(0));
        let pool = if sup <= 0 {
            "sup_none"
        } else if sup >= need + PLENTY_OVER {
            "sup_plenty"
        } else if sup >= need {
            "sup_ok"
        } else if short * 3 < need {
            "sup_short"
        } else {
            "sup_bad"
        };
        let lackv = match &lack {
            Some(l) if data::t("clan_journal_text", "LACK").get(l.as_str()).is_some() => Value::from(l.as_str()),
            _ => Value::Null,
        };
        let q = w.s(pool, &[("sup", Value::from(sup)), ("need", Value::from(need)), ("short", Value::from(short)), ("over", Value::from(over)), ("lack", lackv)])?;
        if let Some(q) = q {
            paras.push(json!(["sup", "", [q]]));
        }
        worse(if matches!(pool, "sup_bad" | "sup_none") { "bad" } else if pool == "sup_short" { "warn" } else { "info" }, &mut tone);
    }

    // ---- THE CAMP: the members whose state changed
    let mut camp: Vec<Value> = Vec::new();
    let attack_now = evk == cl::EV_ATTACK;

    // person(pool, fb, p, extra=None, **kw2)
    macro_rules! person {
        ($pool:expr, $fb:expr, $p:expr, $extra:expr, $kw:expr) => {{
            let p: &Value = $p;
            let mut ppl: Vec<(&str, Option<&Value>)> = vec![("A", Some(p))];
            if let Some((l, e)) = $extra {
                ppl.push((l, Some(e)));
            }
            let q = w.sx($pool, &ppl, Some($fb), None, None, $kw)?;
            if let Some(q) = &q {
                camp.push(q.clone());
                per.insert(vi(p, "i"), q.clone());
            }
            q
        }};
    }

    if !deaths.is_empty() {
        worse("bad", &mut tone);
        if deaths.len() > 3 {
            let q = w.sx(if attack_now { "f_dead_names" } else { "death_many" }, &[], Some("f_dead_names"), None, None, &[("n", Value::from(deaths.len() as i64)), ("names", names_of(&deaths, 3))])?;
            if let Some(q) = q {
                camp.push(q);
            }
        } else {
            for (i, d) in deaths.iter().enumerate() {
                let c = c_or_illness(d);
                let mut pool = death_pool(&c);
                if pool == "death_attack" && horde {
                    pool = "death_horde";
                }
                let ill = if pool == "death_illness" { il_or_fever(d) } else { Value::Null };
                person!(pool, "f_death", *d, None::<(&str, &Value)>, &[("ill", ill)]);
                let kin = now_by.get(&d.get("k").filter(|k| truthy(k)).map(int_of).unwrap_or(0)).copied();
                if let Some(kin) = kin {
                    if i == 0 && !away.contains(&vi(kin, "i")) {
                        let q = w.sp("kin", &[("A", Some(*d)), ("B", Some(kin))], &[])?;
                        if let Some(q) = q {
                            camp.push(q);
                        }
                    }
                }
            }
        }
    }
    let gain_ev = evk == cl::EV_GROWTH && evg > 0;
    let groups: [(&str, &Vec<&Value>); 5] = [("sick", &new_sick), ("join", &joined), ("recover", &recovered), ("hungry", &new_hungry), ("fed", &fed)];
    for (kind, ps) in groups {
        let ps: Vec<&Value> = ps.iter().copied().filter(|p| kind == "join" || !away.contains(&vi(p, "i"))).collect();
        if ps.is_empty() {
            continue;
        }
        let room = CAMP_MAX - camp.len() as i64;
        match kind {
            "sick" | "hungry" => worse("warn", &mut tone),
            "join" | "recover" => worse("good", &mut tone),
            _ => {}
        }
        if kind == "fed" && (room < 2 || ps.len() > 2) {
            continue; // the least of the news: only when the day has room for it
        }
        let many = match kind {
            "sick" => Some("sick_many"),
            "join" => Some("join_many"),
            "hungry" => Some("hungry_many"),
            _ => None,
        };
        let fbm = match kind {
            "sick" => Some("f_more_sick"),
            "join" => Some("f_join_names"),
            "recover" => Some("f_more_recover"),
            "hungry" => Some("f_more_hungry"),
            _ => None,
        };
        if kind == "join" && gain_ev {
            // the event paragraph tells that they came: here only who they are
            if let Some(q) = w.s("f_join_names", &[("names", names_of(&ps, 4))])? {
                camp.push(q);
            }
            continue;
        }
        if ps.len() >= 3 || room < ps.len() as i64 {
            let q = if let Some(many) = many {
                w.sfb(many, fbm.unwrap_or(""), &[("n", Value::from(ps.len() as i64)), ("names", names_of(&ps, 4))])?
            } else if let Some(fbm) = fbm {
                w.s(fbm, &[("names", names_of(&ps, 4))])?
            } else {
                None
            };
            if let Some(q) = q {
                camp.push(q);
            }
            continue;
        }
        for p in ps {
            match kind {
                "sick" => {
                    let pool = if vs(p, "il") == "spores" { "sick_spores" } else { "sick" };
                    person!(pool, "f_sick", p, None::<(&str, &Value)>, &[("ill", il_or_fever(p))]);
                }
                "join" => {
                    person!("join", "f_join", p, None::<(&str, &Value)>, &[]);
                }
                "recover" => {
                    person!("recover", "f_recover", p, None::<(&str, &Value)>, &[]);
                }
                "hungry" => {
                    person!("hungry", "f_hungry", p, None::<(&str, &Value)>, &[]);
                }
                _ => {
                    person!("fed", "f_fed", p, None::<(&str, &Value)>, &[]);
                }
            }
        }
    }
    if !camp.is_empty() {
        paras.push(json!(["camp", "", camp]));
    }

    // ---- the EVENT: settled today, or starting with the next match
    let mut ev: Vec<Option<Value>> = Vec::new();
    let mut cap = String::new();
    if evk == cl::EV_ATTACK {
        cap = if horde { "hatk".into() } else { "atk".into() };
        if evl != 0 {
            ev.push(w.sfb(&format!("{}{}", cap, evt.min(3).max(0)), &format!("f_{}", cap), &[("lost", Value::from(evl))])?);
        } else {
            ev.push(w.sx(&format!("{}_none", cap), &[], None, Some(&HashSet::new()), None, &[])?);
        }
        worse(if evl != 0 { "bad" } else { "warn" }, &mut tone);
    } else if evk == cl::EV_GROWTH {
        cap = format!("g{}", evn);
        if evg > 0 {
            ev.push(w.sfb(&format!("gwin{}", evn), "f_gwin", &[("gain", Value::from(evg))])?);
        } else {
            ev.push(w.sfb(&format!("gfail{}", evn), "f_gfail", &[("gain", Value::from(evg))])?);
        }
        worse(if evg > 0 { "good" } else { "info" }, &mut tone);
    }
    let (nk, nn) = if status == cl::ALIVE { cl::event_today(some_i(newc.get("day"))) } else { (cl::EV_NONE, 0) };
    let mut start: Option<(String, Option<Value>)> = None;
    if nk == cl::EV_ATTACK {
        start = Some(if nn == cl::AN_HORDE { ("hatk".into(), w.s("hwarn", &[])?) } else { ("atk".into(), w.s("warn", &[])?) });
        worse("warn", &mut tone);
    } else if nk == cl::EV_GROWTH {
        let g = py::pymod(nn, ev_names.len() as i64);
        start = Some((format!("g{}", g), w.s(&format!("gstart{}", g), &[])?));
    }
    let ev: Vec<Value> = ev.into_iter().flatten().collect();
    if !ev.is_empty() {
        paras.push(json!(["event", cap, ev]));
    }
    if let Some((c, Some(q))) = &start {
        paras.push(json!(["event", c, [q]]));
    }

    // ---- NOTES: the turn of a week, a size the clan reached, the end; else a line of camp life on a thin day
    let mut note: Vec<Option<Value>> = Vec::new();
    let mut endcap = "note";
    if status == cl::DEAD {
        note.push(w.sx("dead", &[], None, Some(&HashSet::new()), None, &[])?);
        endcap = "end";
        worse("bad", &mut tone);
    } else if status == cl::WON {
        note.push(w.sx("won", &[], None, Some(&HashSet::new()), None, &[])?);
        endcap = "end";
        worse("good", &mut tone);
    } else {
        if day > 1 && py::pymod(day - 1, 7) == 0 && en_t("POOLS").get(format!("week{}", week)).is_some() {
            note.push(w.s(&format!("week{}", week), &[])?);
        }
        if w.st.get("jn").is_none() {
            w.st["jn"] = json!({});
        }
        if w.st["jn"].get("sz").is_none() {
            w.st["jn"]["sz"] = json!([]);
        }
        for mark in [10i64, 20, 30, 50] {
            let before_n = before.get("n").map(int_of).unwrap_or(0);
            let have = w.st["jn"]["sz"].as_array().map_or(false, |a| a.iter().any(|x| int_of(x) == mark));
            if size >= mark && mark > before_n && !have {
                if let Some(a) = w.st["jn"]["sz"].as_array_mut() {
                    a.push(Value::from(mark));
                }
                note.push(w.s(&format!("size{}", mark), &[])?);
            }
        }
        let count: usize = paras.iter().map(|p| p[2].as_array().map_or(0, |a| a.len())).sum::<usize>() + note.iter().filter(|q| q.is_some()).count();
        if count < NOTE_BELOW {
            let pool = if size != 0 && (uu + ss) * 3 >= size { "note_grim" } else if uu + ss != 0 { "note_tense" } else { "note_good" };
            let mut able: Vec<&Value> = now.iter().copied().filter(|p| kd::can_fight(p) && vs(p, "s") != "s" && !away.contains(&vi(p, "i")) && !per.contains_key(&vi(p, "i"))).collect();
            w.rng.shuffle(&mut able);
            let a0 = able.first().copied();
            let a1 = able.get(1).copied();
            note.push(w.sp(pool, &[("A", a0), ("B", a1)], &[])?);
        }
    }
    let note: Vec<Value> = note.into_iter().flatten().collect();
    if !note.is_empty() {
        paras.push(json!([endcap, "", note]));
    }

    if paras.is_empty() && (!mp.is_empty() || left) {
        // every story line of the day's pools is spent: the plain ledger sentence, so no entry is ever empty
        let q = if left { w.s("f_raid_left", &[])? } else { w.s(if win == 1 { "f_raid_win" } else if win == 0 { "f_raid_loss" } else { "f_raid_draw" }, &[("map", mpv.clone())])? };
        if let Some(q) = q {
            paras.insert(raid_at, json!(["raid", "", [q]]));
        }
    }

    // ---- the headline: the day's strongest fact
    let ndead = deaths.len();
    let title: Option<Value> = if status == cl::DEAD {
        w.s("t_dead", &[])?
    } else if status == cl::WON {
        w.s("t_won", &[])?
    } else if evk == cl::EV_ATTACK {
        if evl != 0 {
            w.s(if horde { "t_hatk" } else { "t_atk" }, &[("lost", Value::from(evl))])?
        } else {
            w.s(if horde { "t_hatk_none" } else { "t_atk_none" }, &[])?
        }
    } else if ndead > 2 {
        w.s("t_death_n", &[("n", Value::from(ndead as i64))])?
    } else if ndead == 2 {
        w.sp("t_death2", &[("A", Some(deaths[0])), ("B", Some(deaths[1]))], &[])?
    } else if ndead > 0 {
        w.sp("t_death", &[("A", Some(deaths[0]))], &[])?
    } else if evk == cl::EV_GROWTH {
        if evg > 0 { w.s("t_gwin", &[("gain", Value::from(evg))])? } else { w.s("t_gfail", &[])? }
    } else if nk == cl::EV_ATTACK {
        w.s(if nn == cl::AN_HORDE { "t_hwarn" } else { "t_warn" }, &[])?
    } else if nk == cl::EV_GROWTH {
        w.s(&format!("t_gstart{}", py::pymod(nn, ev_names.len() as i64)), &[])?
    } else if joined.len() > 1 {
        w.s("t_join_n", &[("n", Value::from(joined.len() as i64))])?
    } else if !joined.is_empty() {
        w.sp("t_join", &[("A", Some(joined[0]))], &[])?
    } else if new_sick.len() > 1 {
        w.s("t_sick_n", &[("n", Value::from(new_sick.len() as i64))])?
    } else if !new_sick.is_empty() {
        w.sp("t_sick", &[("A", Some(new_sick[0]))], &[])?
    } else if need != 0 && sup <= 0 {
        w.s("t_none", &[])?
    } else if need != 0 && (need - sup) * 3 >= need {
        w.s("t_bad", &[("short", Value::from(need - sup))])?
    } else if new_hungry.len() > 1 {
        w.s("t_hungry", &[("n", Value::from(new_hungry.len() as i64))])?
    } else if !recovered.is_empty() {
        w.sp("t_recover", &[("A", Some(recovered[0]))], &[])?
    } else if left {
        w.s("t_left", &[])?
    } else if !mp.is_empty() && win == 1 {
        w.s("t_win", &[("map", mpv.clone())])?
    } else if !mp.is_empty() && win == 0 {
        w.s("t_loss", &[("map", mpv.clone())])?
    } else if need != 0 && sup >= need + PLENTY_OVER {
        w.s("t_plenty", &[])?
    } else if !mp.is_empty() {
        w.s("t_raid", &[("map", mpv.clone())])?
    } else {
        w.s("t_quiet", &[])?
    };
    let bn = before.get("n").map(int_of).unwrap_or(size);
    paras.truncate(MAX_PARAS);
    let rec = json!({"v": REC_V, "d": day, "tone": tone, "h": hh, "u": uu, "s": ss, "dn": size - bn, "sup": sup, "need": need, "map": mp, "win": win,
                     "k": k, "x": x, "r": rv, "hs": hs, "dw": dw, "left": left as i64, "ti": title.unwrap_or(Value::Null), "pa": paras});
    Ok((rec, per))
}

// py: clan_journal.py:455 _merge
/// A second match on the same clan day (a day that did not count: late join, idle): its paragraphs follow the first match's; the numbers are the latest.
fn merge(old: &Value, rec: &Value) -> Value {
    let mut out = rec.clone();
    let mut pa: Vec<Value> = va(old, "pa").to_vec();
    pa.extend(va(rec, "pa").iter().cloned());
    pa.truncate(MAX_PARAS);
    out["pa"] = Value::Array(pa);
    out["dn"] = Value::from(vi(old, "dn") + vi(rec, "dn"));
    if tone_rank(vs(old, "tone")) > tone_rank(vs(rec, "tone")) {
        out["tone"] = old.get("tone").cloned().unwrap_or(Value::Null);
        out["ti"] = old.get("ti").cloned().unwrap_or(Value::Null);
    }
    out
}

// ------------------------------------------------------------------ database
// py: clan_journal.py:466 ensure
pub fn ensure(c: &Connection) -> rusqlite::Result<()> {
    c.execute("CREATE TABLE IF NOT EXISTS clan_journal(token_id TEXT, day INTEGER, id INTEGER, data TEXT, PRIMARY KEY(token_id, day))", [])?;
    Ok(())
}

// py: clan_journal.py:470 purge
pub fn purge(c: &Connection, tid: &str) -> Result<(), String> {
    let _ = c.execute("DELETE FROM clan_journal WHERE token_id=?", params![tid]);
    Ok(())
}

// py: clan_journal.py:477 _get
fn get(c: &Connection, tid: &str, day: i64) -> Option<Value> {
    let row: Option<String> = c.query_row("SELECT data FROM clan_journal WHERE token_id=? AND day=?", params![tid, day], |r| r.get::<_, Option<String>>(0)).ok()?;
    serde_json::from_str(&row?).ok()
}

// py: clan_journal.py:485 _put
fn put(c: &Connection, tid: &str, day: i64, jid: i64, rec: &Value) -> rusqlite::Result<()> {
    c.execute("INSERT OR REPLACE INTO clan_journal(token_id, day, id, data) VALUES(?,?,?,?)", params![tid, day, jid, py::dumps_compact_utf8(rec)])?;
    Ok(())
}

// py: clan_journal.py:489 _rng
fn rng_of(st: &Value, salt: &str) -> PyRandom {
    let g = |k: &str| st.get(k).map(pystr).unwrap_or_else(|| "None".into());
    PyRandom::from_str(&format!("jn:{}:{}:{}:{}", g("seed"), g("run"), g("n"), salt))
}

// py: clan_journal.py:493 on_new_run
/// The founding entry (day 0) of a new clan; the old run's entries go.
pub fn on_new_run(c: &Connection, tid: &str, st: &mut Value) -> Result<Value, String> {
    ensure(c).map_err(|e| e.to_string())?;
    purge(c, tid)?;
    let fk = cs::_fkey(vi(st, "fac"));
    if let Some(o) = st.as_object_mut() {
        o.shift_remove("jn");
    }
    let n = people_of(st).len();
    let mut rng = rng_of(st, "new");
    let n_st = st.get("n").cloned().unwrap_or_else(|| Value::from(0));
    let mut base = Map::new();
    base.insert("size".into(), Value::from(n as i64));
    base.insert("week".into(), Value::from(1));
    let mut w = Writer { st, rng: &mut rng, fk, base };
    let q = w.s("founding", &[])?;
    let ti = w.s("t_founding", &[])?;
    let rec = json!({"v": REC_V, "d": 0, "tone": "info", "h": n, "u": 0, "s": 0, "dn": 0, "sup": 0, "need": 0, "map": "", "win": -1, "k": 0, "x": 0, "r": 0, "hs": 0, "dw": 0, "left": 0,
                     "ti": ti.unwrap_or(Value::Null), "pa": match q { Some(q) => json!([["start", "", [q]]]), None => json!([]) }});
    put(c, tid, 0, int_of(&n_st), &rec).map_err(|e| e.to_string())?;
    Ok(rec)
}

// py: clan_journal.py:508 on_day
/// After clan_story wrote the day's feed rows: compose the day's entry, store it, give the status cards their lines. Returns the record.
pub fn on_day(c: &Connection, tid: &str, st: &mut Value, before: &Value, newc: &Value, r: &Value) -> Result<Value, String> {
    ensure(c).map_err(|e| e.to_string())?;
    let have: i64 = c.query_row("SELECT COUNT(*) FROM clan_journal WHERE token_id=?", params![tid], |r| r.get(0)).map_err(|e| e.to_string())?;
    if have == 0 && some_i(newc.get("day")) > 1 {
        if let Err(ex) = backfill(c, tid, st) {
            // the backfill is a courtesy
            eprintln!("clan_journal backfill failed: {}", ex);
        }
    }
    let day_s = newc.get("day").map(pystr).unwrap_or_else(|| "None".into());
    let mut rng = rng_of(st, &format!("day{}", day_s));
    let (mut rec, per) = compose(st, before, newc, r, &mut rng)?;
    let old = get(c, tid, vi(&rec, "d"));
    let counted = r.get("counted").map(int_of).unwrap_or(1) != 0;
    if let Some(old) = &old {
        if truthy(old) && (!counted || old.get("open").map_or(false, truthy)) {
            rec = merge(old, &rec);
        }
    }
    if !counted {
        rec["open"] = Value::from(1); // the clan day is not over: the next match of it is added to this entry
    }
    put(c, tid, vi(&rec, "d"), st.get("n").map(int_of).unwrap_or(0), &rec).map_err(|e| e.to_string())?;
    if let Err(ex) = status_lines(st, &per, &mut rng) {
        eprintln!("clan_journal status lines failed: {}", ex);
    }
    Ok(rec)
}

// py: clan_journal.py:532 add_perk
/// A supply perk was used in the match of clan day `day` (clan_perks.on_day): ONE sentence, once a day, appended to that entry's raid paragraph.
pub fn add_perk(c: &Connection, tid: &str, st: &mut Value, day: i64, cat: &str, who: &Value, mapname: &str) -> R<bool> {
    let Some(mut rec) = get(c, tid, day).filter(|r| truthy(r)) else { return Ok(false) };
    if rec.get("pk").map_or(false, truthy) {
        return Ok(false);
    }
    let pool = format!("perk_{}", match cat { "med" => "med", "ammo" => "ammo", "mat" => "mat", "food" => "food", "water" => "water", _ => "" });
    if en_t("POOLS").get(&pool).is_none() || !truthy(who) {
        return Ok(false);
    }
    let fk = cs::_fkey(vi(st, "fac"));
    let mut rng = rng_of(st, &format!("pk{}{}", day, cat));
    let mut w = Writer { st, rng: &mut rng, fk, base: Map::new() };
    let q = w.sp(&pool, &[("A", Some(who))], &[("map", if mapname.is_empty() { Value::Null } else { Value::from(mapname) })])?;
    let Some(q) = q else { return Ok(false) };
    let mut done = false;
    if let Some(pa) = rec.get_mut("pa").and_then(|p| p.as_array_mut()) {
        for p in pa.iter_mut() {
            if p.get(0).and_then(|k| k.as_str()) == Some("raid") {
                if let Some(a) = p.get_mut(2).and_then(|a| a.as_array_mut()) {
                    a.push(q.clone());
                }
                done = true;
                break;
            }
        }
    }
    if !done {
        if rec.get("pa").is_none() {
            rec["pa"] = json!([]);
        }
        if let Some(a) = rec["pa"].as_array_mut() {
            a.insert(0, json!(["raid", "", [q]]));
        }
    }
    rec["pk"] = Value::from(1);
    let row: Option<i64> = c.query_row("SELECT id FROM clan_journal WHERE token_id=? AND day=?", params![tid, day], |r| r.get(0)).ok();
    put(c, tid, day, row.unwrap_or_else(|| st.get("n").map(int_of).unwrap_or(0)), &rec).map_err(|e| e.to_string())?;
    Ok(true)
}

// ------------------------------------------------------------------ the STATUS UPDATES cards (clan_status)
fn st_pool(c: i64) -> (Option<&'static str>, Option<&'static str>) {
    match c {
        1 => (Some("hungry"), Some("f_hungry")),
        2 => (Some("st_sad"), None),
        3 => (Some("sick"), Some("f_sick")),
        4 => (None, Some("f_death")),
        5 => (Some("st_injured"), None),
        6 => (Some("recover"), Some("f_recover")),
        7 => (Some("join"), Some("f_join")),
        8 => (Some("st_depressed"), None),
        9 => (Some("st_shock"), None),
        _ => (None, None),
    }
}

fn split_lid(q: &Value) -> Option<(String, i64)> {
    let lid = q.get(0)?.as_str()?;
    let (pool, i) = lid.get(3..)?.rsplit_once('.')?;
    Some((pool.to_string(), i.trim().parse().ok()?))
}

// py: clan_journal.py:562 status_lines
/// Every status event of today gets its own sentence e["jl"] = [lid, params]: the journal's sentence about that member when the entry has one, else a line of
/// the state's pool. No two pending cards carry the same line.
pub fn status_lines(st: &mut Value, per: &HashMap<i64, Value>, rng: &mut PyRandom) -> R<()> {
    let ev: Vec<Value> = st.get("ch").filter(|c| truthy(c)).and_then(|c| c.get("stat")).filter(|s| truthy(s)).and_then(|s| s.get("ev")).filter(|e| truthy(e)).and_then(|e| e.as_array()).cloned().unwrap_or_default();
    let day = st.get("day").filter(|d| truthy(d)).map(int_of).unwrap_or(0);
    let fk = cs::_fkey(vi(st, "fac"));
    let mut shown: HashMap<String, HashSet<i64>> = HashMap::new();
    for e in &ev {
        if let Some(jl) = e.get("jl").filter(|j| truthy(j)) {
            let (pool, i) = split_lid(jl).ok_or("jl")?;
            shown.entry(pool).or_default().insert(i);
        }
    }
    let sick: HashMap<i64, Value> = people_of(st).iter().filter(|p| vs(p, "s") == "s").map(|p| (vi(p, "i"), il_or_fever(p))).collect();
    let mut w = Writer { st, rng, fk, base: Map::new() };
    for (idx, e) in ev.iter().enumerate() {
        if e.get("d").map(int_of).unwrap_or(-1) != day || e.get("jl").map_or(false, truthy) {
            continue;
        }
        let eid = vi(e, "i");
        let mut q = per.get(&eid).cloned();
        if q.is_none() {
            let c = e.get("c").map(int_of).unwrap_or(0);
            let (mut pool, fb) = st_pool(c);
            let who = json!({"n": e["n"].clone(), "g": e.get("g").cloned().unwrap_or_else(|| Value::from("n"))});
            let mut ill = if c == 3 { sick.get(&eid).cloned() } else { None };
            let mut pool_s: Option<String> = pool.map(|p| p.to_string());
            if c == 4 {
                // a death the entry only counted (a day with many dead): the line of how THIS member died
                let d = va(w.st, "dead").iter().find(|x| vi(x, "i") == eid).cloned().unwrap_or_else(|| json!({}));
                let cs_ = c_or_illness(&d);
                let mut p = death_pool(&cs_);
                if p == "death_attack" && cl::attack_story(d.get("d").map(int_of).unwrap_or(-9)) == cl::AN_HORDE {
                    p = "death_horde";
                }
                ill = if p == "death_illness" { Some(il_or_fever(&d)) } else { None };
                pool_s = Some(p.to_string());
                pool = None;
            }
            let _ = pool;
            q = if let Some(p) = &pool_s {
                let av = shown.get(p).cloned().unwrap_or_default();
                w.sx(p, &[("A", Some(&who))], fb, Some(&av), None, &[("ill", ill.unwrap_or(Value::Null))])?
            } else if let Some(fb) = fb {
                w.sp(fb, &[("A", Some(&who))], &[])?
            } else {
                None
            };
        }
        if let Some(q) = q {
            let (pool, i) = split_lid(&q).ok_or("jl")?;
            shown.entry(pool).or_default().insert(i);
            if let Some(slot) = w.st.get_mut("ch").and_then(|c| c.get_mut("stat")).and_then(|s| s.get_mut("ev")).and_then(|e| e.get_mut(idx)) {
                slot["jl"] = q;
                slot["ln"] = Value::from("");
            }
        }
    }
    Ok(())
}

// ------------------------------------------------------------------ a clan that lived before the journal
// py: clan_journal.py:601 backfill
/// Entries for the past days of a running clan, from what is still stored: the chronicle's match log and the diary rows that name who died / fell sick /
/// recovered / joined. Ledger sentences only (pools f_*): nothing is invented and no story line is spent.
pub fn backfill(c: &Connection, tid: &str, st: &mut Value) -> R<()> {
    let fk = cs::_fkey(vi(st, "fac"));
    let mut rng = PyRandom::from_i64(0);
    // day -> {"m": match row, "p": [(pool, who, ill)], "id": n}
    let mut days: std::collections::BTreeMap<i64, (Option<Value>, Vec<(String, Value, Value)>, Option<i64>)> = std::collections::BTreeMap::new();
    if let Ok(pubv) = crate::clan::ext_clan_alife::chronicle_public(c, tid) {
        if let Some(ms) = pubv.get("matches").filter(|m| truthy(m)).and_then(|m| m.as_array()) {
            for m in ms {
                days.entry(m.get("d").map(int_of).unwrap_or(0)).or_default().0 = Some(m.clone());
            }
        }
    }
    let rows: Vec<(i64, i64, Option<String>, Option<String>)> = match c.prepare("SELECT id, day, kind, params FROM clan_feed WHERE token_id=? ORDER BY id") {
        Ok(mut s) => match s.query_map(params![tid], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?, r.get::<_, Option<String>>(2)?, r.get::<_, Option<String>>(3)?))) {
            Ok(it) => it.flatten().collect(),
            Err(_) => Vec::new(),
        },
        Err(_) => Vec::new(),
    };
    let illness = data::t("clan_story_data", "ILLNESS");
    for (i, day, kind, params_s) in rows {
        let k = kind.unwrap_or_default();
        let pool = if k.starts_with("death_") { "f_death" } else if k == "join" { "f_join" } else if k == "sick" { "f_sick" } else if k == "recovery" { "f_recover" } else { "" };
        let Some(ps) = params_s.filter(|p| !p.is_empty()) else { continue };
        if pool.is_empty() {
            continue;
        }
        let Ok(pj) = serde_json::from_str::<Value>(&ps) else { continue };
        let a = pj.get("p").filter(|p| truthy(p)).and_then(|p| p.get("A")).filter(|a| truthy(a));
        let Some(a) = a else { continue };
        let (Some(n0), Some(g0)) = (a.get(0), a.get(1)) else { continue };
        let ill = pj.get("s").filter(|s| truthy(s)).and_then(|s| s.get("ill"));
        let ill_v = match ill {
            Some(v) if v.as_str().map_or(false, |s| illness.get(s).is_some()) => v.clone(),
            _ => Value::from("fever"),
        };
        let d = days.entry(day).or_default();
        d.1.push((pool.to_string(), json!({"n": n0.clone(), "g": g0.clone()}), ill_v));
        d.2 = Some(d.2.unwrap_or(0).max(i));
    }
    let cur = st.get("day").map(int_of).unwrap_or(0);
    let mut recs: Vec<(i64, i64, Value)> = Vec::new();
    {
        let mut w = Writer { st, rng: &mut rng, fk, base: Map::new() };
        for (day, d) in &days {
            let day = *day;
            if day <= 0 || day >= cur {
                continue; // (today's entry is written by on_day)
            }
            let empty = json!({});
            let m = d.0.as_ref().filter(|m| truthy(m)).unwrap_or(&empty);
            let mut paras: Vec<Value> = Vec::new();
            let mp = match m.get("map") {
                Some(v) if truthy(v) => pystr(v),
                _ => String::new(),
            };
            let win: i64 = match m.get("win") {
                Some(Value::Bool(true)) => 1,
                Some(Value::Bool(false)) => 0,
                _ => -1,
            };
            let mpv = Value::from(mp.as_str());
            if !mp.is_empty() {
                let q = w.s(if win == 1 { "f_raid_win" } else if win == 0 { "f_raid_loss" } else { "f_raid_draw" }, &[("map", mpv.clone())])?;
                paras.push(json!(["raid", "", [q.unwrap_or(Value::Null)]]));
            }
            let (sup, need) = (m.get("sup").filter(|v| truthy(v)).map(int_of).unwrap_or(0), m.get("need").filter(|v| truthy(v)).map(int_of).unwrap_or(0));
            if need != 0 {
                let q = w.s("f_sup", &[("sup", Value::from(sup)), ("need", Value::from(need))])?;
                paras.push(json!(["sup", "", [q.unwrap_or(Value::Null)]]));
            }
            let mut camp: Vec<Option<Value>> = Vec::new();
            for (pool, who, ill) in d.1.iter().take(CAMP_MAX as usize) {
                camp.push(w.sp(pool, &[("A", Some(who))], &[("ill", if pool == "f_sick" { ill.clone() } else { Value::Null })])?);
            }
            if !camp.is_empty() {
                paras.push(json!(["camp", "", camp.into_iter().flatten().collect::<Vec<_>>()]));
            }
            let dead: Vec<&Value> = d.1.iter().filter(|(p, _, _)| p == "f_death").map(|(_, w, _)| w).collect();
            let title = if let Some(d0) = dead.first() {
                w.sp("t_death", &[("A", Some(*d0))], &[])?
            } else if !mp.is_empty() && win == 1 {
                w.s("t_win", &[("map", mpv.clone())])?
            } else if !mp.is_empty() && win == 0 {
                w.s("t_loss", &[("map", mpv.clone())])?
            } else if !mp.is_empty() {
                w.s("t_raid", &[("map", mpv.clone())])?
            } else {
                w.s("t_quiet", &[])?
            };
            let rec = json!({"v": REC_V, "d": day, "tone": if dead.is_empty() { "info" } else { "bad" }, "h": -1, "u": -1, "s": -1, "dn": 0, "sup": sup, "need": need, "map": mp, "win": win,
                             "k": 0, "x": 0, "r": 0, "hs": 0, "dw": 0, "left": 0, "ti": title.unwrap_or(Value::Null), "pa": paras, "old": 1});
            recs.push((day, d.2.unwrap_or(0), rec));
        }
    }
    for (day, id, rec) in recs {
        put(c, tid, day, id, &rec).map_err(|e| e.to_string())?;
    }
    Ok(())
}

// ------------------------------------------------------------------ reading
// py: clan_journal.py:657 entries
/// The clan's journal, oldest day first, rendered. [] without a table / rows. Never a day the clan has not reached (a restored booking).
pub fn entries(c: &Connection, tid: &str, lang: &str, days_done: Option<i64>) -> Vec<Value> {
    let lang = if pln::norm_lang(lang) == "pl" { "pl" } else { "en" };
    let t = |name: &str| if lang == "pl" { pl_t(name) } else { en_t(name) };
    let rows: Vec<(i64, Option<i64>, Option<String>)> = match c.prepare("SELECT day, id, data FROM clan_journal WHERE token_id=? ORDER BY day") {
        Ok(mut s) => match s.query_map(params![tid], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, Option<i64>>(1)?, r.get::<_, Option<String>>(2)?))) {
            Ok(it) => it.flatten().collect(),
            Err(_) => return Vec::new(),
        },
        Err(_) => return Vec::new(),
    };
    let days_done = days_done.or_else(|| cs::_days_done(c, tid));
    let mut out = Vec::new();
    for (day, jid, data_s) in rows {
        if let Some(dd) = days_done {
            if day > dd + 1 {
                continue;
            }
        }
        let Some(rec) = data_s.and_then(|d| serde_json::from_str::<Value>(&d).ok()) else { continue };
        let mut fk = "W".to_string();
        let mut paras: Vec<Value> = Vec::new();
        for pa in va(&rec, "pa") {
            let kind = pa.get(0).map(pystr).unwrap_or_default();
            let cap = pa.get(1).map(pystr).unwrap_or_default();
            let sents: &[Value] = pa.get(2).and_then(|s| s.as_array()).map(|a| a.as_slice()).unwrap_or(&[]);
            let text: String = sents.iter().map(|q| render(q, lang)).filter(|t| !t.is_empty()).collect::<Vec<_>>().join(" ");
            if text.is_empty() {
                continue;
            }
            for q in sents {
                if let Some(f) = q.get(1).and_then(|p| p.get("s")).filter(|s| truthy(s)).and_then(|s| s.get("f")) {
                    fk = pystr(f);
                }
            }
            let caption = if cap == "hatk" {
                pystr(t("HORDE_NAME"))
            } else if cap == "atk" {
                let en = if lang == "pl" {
                    data::t("clan_story_pl", "ENEMY").get(&fk).and_then(|e| e.get(0)).map(pystr).unwrap_or_default()
                } else {
                    data::t("clan_story_data", "FACTION").get(&fk).and_then(|e| e.get("enemies")).map(pystr).unwrap_or_default()
                };
                pystr(t("ATTACK_NAME")).replace("{E}", &en).to_uppercase()
            } else if cap.starts_with('g') && cap.len() > 1 && cap[1..].chars().all(|c| c.is_ascii_digit()) {
                let names = data::strs(t("EVENT_NAMES"));
                let n: i64 = cap[1..].parse().unwrap_or(0);
                names.get(py::pymod(n, names.len() as i64) as usize).map(|s| s.to_string()).unwrap_or_default()
            } else {
                t("CAPTIONS").get(&kind).map(pystr).unwrap_or_default()
            };
            paras.push(json!([caption, text]));
        }
        let (h, u, s) = (rec.get("h").map(int_of).unwrap_or(-1), rec.get("u").map(int_of).unwrap_or(-1), rec.get("s").map(int_of).unwrap_or(-1));
        let ti = match rec.get("ti") {
            Some(v) if truthy(v) => render(v, lang),
            _ => String::new(),
        };
        let map = if lang == "pl" && rec.get("map").map_or(false, truthy) {
            vars_of(&json!({"map": rec["map"].clone()}), "pl").ok().and_then(|v| v.get("map").cloned()).unwrap_or_default()
        } else {
            rec.get("map").map(pystr).unwrap_or_default()
        };
        out.push(json!({"day": day, "week": py::floordiv(day.max(1) - 1, 7) + 1, "dow": if day > 0 { py::pymod(day - 1, 7) + 1 } else { 0 }, "id": jid.unwrap_or(0), "title": ti,
                        "tone": rec.get("tone").cloned().unwrap_or_else(|| Value::from("")), "map": map,
                        "win": rec.get("win").map(int_of).unwrap_or(-1), "sup": vi(&rec, "sup"), "need": vi(&rec, "need"), "h": h, "u": u, "s": s, "size": if h >= 0 { h + u + s } else { -1 },
                        "dn": vi(&rec, "dn"), "k": vi(&rec, "k"), "x": vi(&rec, "x"), "r": vi(&rec, "r"), "hs": vi(&rec, "hs"), "dw": vi(&rec, "dw"), "left": vi(&rec, "left"), "paras": paras}));
    }
    out
}
