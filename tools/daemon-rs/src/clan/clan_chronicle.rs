//! py: clan_chronicle.py - the clan chronicle: vignettes for the clan feed with personalities, bonds, habits, portraits (text only: it never changes a
//! clan number). State is the story state `st` (serde_json Value, insertion order kept); `cs` of the Python = `st["ch"]` and is never passed separately.
//! A Python exception is `Err(String)` (day_rows / public) or `None` (render_msg).
use crate::clan::clan::{int_of, pystr, truthy};
use crate::clan::ext_clan_chronicle::{self as ext, lex, Vars};
use crate::clan::{clan_arcs, clan_kids as kd, clan_portraits as pt, data};
use indexmap::IndexMap;
use parking_lot::Mutex;
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::PyRandom;

pub const WINDOW_DAYS: i64 = 24;
pub const MAX_ROWS: usize = 3;
pub const MAX_TOUCH: usize = 2;
pub const PORTRAIT_MODELS: i64 = 4;
pub const BOND_TYPES: [&str; 4] = ["kin", "mentor", "rival", "sweet"];
const TEMPERS: [&str; 6] = ["calm", "hot", "anxious", "dry", "tender", "stubborn"];
const WINDOW_HABIT: i64 = 12;
/// Python source order of `USE` (the generated JSON has its keys sorted: insertion order is lost there) and of `CAT_POOL`.
const USE_ORDER: [&str; 11] = ["pine", "beans", "bandage", "abx", "alcohol", "r38", "box9", "filter", "battery", "scissors", "rags"];
const CAT_ORDER: [&str; 4] = ["food", "med", "ammo", "tool"];

fn tc(name: &str) -> &'static Value {
    data::t("clan_chronicle", name)
}

/// The generated constants equal the ones coded here (checked by the selftest).
pub fn consts_ok() -> Result<(), &'static str> {
    let chk = |n: &'static str, v: i64| if tc(n).as_i64() == Some(v) { Ok(()) } else { Err(n) };
    chk("WINDOW_DAYS", WINDOW_DAYS)?;
    chk("MAX_ROWS", MAX_ROWS as i64)?;
    chk("MAX_TOUCH", MAX_TOUCH as i64)?;
    chk("PORTRAIT_MODELS", PORTRAIT_MODELS)?;
    let mut uo: Vec<&str> = USE_ORDER.to_vec();
    uo.sort();
    let mut co: Vec<&str> = CAT_ORDER.to_vec();
    co.sort();
    let _ = (&uo, &co);
    // the generated tables keep Python insertion order: it must equal the order coded here
    if obj_items(tc("USE")).iter().map(|(k, _)| k.as_str()).collect::<Vec<_>>() != USE_ORDER || obj_items(tc("CAT_POOL")).iter().map(|(k, _)| k.as_str()).collect::<Vec<_>>() != CAT_ORDER {
        return Err("USE/CAT_POOL keys");
    }
    if data::strs(tc("TEMPERS")) != TEMPERS || data::strs(tc("BOND_TYPES")) != BOND_TYPES {
        return Err("TEMPERS/BOND_TYPES");
    }
    Ok(())
}

// py: crc32 of "|".join(str(x) ...)
pub fn h(parts: &[String]) -> u32 {
    let refs: Vec<&str> = parts.iter().map(|s| s.as_str()).collect();
    t2r_clanstory::crc::h(&refs)
}

macro_rules! hh {
    ($($x:expr),+ $(,)?) => { h(&[$(($x).to_string()),+]) };
}

fn vi(v: &Value, k: &str) -> i64 {
    v.get(k).map(int_of).unwrap_or(0)
}
fn vs<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("")
}

// ------------------------------------------------------------------ module data
fn row_str(row: &'static Value, i: usize) -> &'static str {
    row.get(i).and_then(|x| x.as_str()).unwrap_or("")
}

/// `USE[key]` (cat, EN one, EN many, PL nom, PL acc, PL few, PL many, morale).
pub fn use_row(key: &str) -> Option<&'static Value> {
    tc("USE").get(key)
}
/// `KEEP[key]` (EN, PL accusative, morale).
pub fn keep_row(key: &str) -> Option<&'static Value> {
    tc("KEEP").get(key)
}
pub fn is_use(key: &str) -> bool {
    use_row(key).is_some()
}
pub fn is_keep(key: &str) -> bool {
    keep_row(key).is_some()
}
/// `PASTS` rows (faction, stage, EN, PL masculine, PL feminine).
pub fn pasts() -> &'static Value {
    tc("PASTS")
}

// py: clan_chronicle.py:114 map_name
pub fn map_name(en: &str, lang: &str) -> String {
    let en = data::t("clan_chronicle_maps", "EN_SHORT").get(en).and_then(|v| v.as_str()).unwrap_or(en);
    if lang == "pl" {
        data::t("clan_chronicle_maps", "PL").get(en).and_then(|v| v.as_str()).unwrap_or(en).to_string()
    } else {
        en.to_string()
    }
}

// py: clan_chronicle.py:140 qty
pub fn qty(key: &str, n: i64, lang: &str) -> Result<String, String> {
    let e = use_row(key).ok_or_else(|| format!("KeyError: {}", key))?;
    let n = n.max(1);
    if lang == "pl" {
        if n == 1 {
            return Ok(row_str(e, 4).to_string());
        }
        let few = (2..=4).contains(&(n % 10)) && !(12..=14).contains(&(n % 100));
        return Ok(format!("{} {}", n, row_str(e, if few { 5 } else { 6 })));
    }
    Ok(if n == 1 { row_str(e, 1).to_string() } else { format!("{} {}", n, row_str(e, 2)) })
}

// py: clan_chronicle.py:152 loot_phrase
pub fn loot_phrase(key: &str, n: i64, lang: &str) -> Result<String, String> {
    if let Some(k) = keep_row(key) {
        return Ok(row_str(k, if lang == "pl" { 1 } else { 0 }).to_string());
    }
    qty(key, n, lang)
}

// py: clan_chronicle.py:158 join_list
pub fn join_list(items: &[String], lang: &str) -> String {
    if items.len() <= 1 {
        return items.concat();
    }
    format!("{}{}{}", items[..items.len() - 1].join(", "), if lang == "pl" { " i " } else { " and " }, items[items.len() - 1])
}

// py: clan_chronicle.py:164 roll_loot
/// Loot of one match: `[[key, n], ...]`. `have` = `cs["keep"]` (keys = keepsakes already owned).
pub fn roll_loot(rng: &mut PyRandom, r: &Value, matchv: &Value, newc: &Value, have: &Value) -> Result<Vec<Value>, String> {
    let sup = vi(r, "supplies");
    let need = r.get("needed").map(int_of).unwrap_or(1).max(1);
    let k = matchv.get("kills").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
    let deaths = matchv.get("deaths").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
    let perf = 1.0 + (k as f64 / 12.0).min(1.0) - 0.4 * (deaths as f64 / 8.0).min(1.0);
    let units = crate::py::round0((1.0 + sup as f64 / 6.0) * perf).max(1);
    let mut cats: Vec<&str> = Vec::new();
    if vi(newc, "sick") != 0 {
        cats.push("med");
    }
    if sup < need || vi(newc, "hungry") != 0 {
        cats.push("food");
    }
    let sm = rng.sample(&["ammo", "tool", "food", "med"], 2).map_err(|x| format!("{:?}", x))?;
    cats.extend(sm);
    let mut out: Vec<Value> = Vec::new();
    let mut seen: HashSet<&str> = HashSet::new();
    for c in cats {
        if seen.contains(c) || out.len() >= 2 {
            continue;
        }
        seen.insert(c);
        let keys: Vec<&str> = USE_ORDER.iter().copied().filter(|k| use_row(k).map(|v| row_str(v, 0) == c).unwrap_or(false)).collect();
        let key = *rng.choice(&keys).map_err(|x| format!("{:?}", x))?;
        let mul = if c == "ammo" && key == "r38" { *rng.choice(&[4i64, 5, 6, 8]).map_err(|x| format!("{:?}", x))? } else { 1 };
        let mut n = units * mul;
        if matches!(key, "box9" | "filter" | "scissors" | "abx") {
            n = (units / 3).max(1);
        }
        out.push(json!([key, n]));
    }
    if rng.random() < 0.4 {
        let mut ks: Vec<&str> = obj_items(tc("KEEP")).iter().map(|(k, _)| k.as_str()).collect();
        ks.sort();
        let free: Vec<&str> = ks.iter().copied().filter(|k| have.get(*k).is_none()).collect();
        let pick: &Vec<&str> = if free.is_empty() { &ks } else { &free };
        let key = *rng.choice(pick).map_err(|x| format!("{:?}", x))?;
        out.push(json!([key, 1]));
    }
    Ok(out)
}

/// (key, value) pairs of a plain JSON object table in Python insertion order.
fn obj_items(v: &'static Value) -> Vec<(&'static String, &'static Value)> {
    v.as_object().map(|o| o.iter().collect()).unwrap_or_default()
}

// ------------------------------------------------------------------ the pieces (REG / BY_POOL)
pub struct Piece {
    pub pool: &'static str,
    pub sc: &'static str,
    pub tone: &'static str,
    pub en: &'static str,
    pub pl: &'static str,
    pub letters: BTreeSet<char>,
    pub vars: BTreeSet<String>,
}

pub static REG: LazyLock<IndexMap<String, Piece>> = LazyLock::new(|| {
    let mut m = IndexMap::new();
    if let Some(pools) = data::t("clan_chronicle_text", "POOLS").as_object() {
        for (pool, rows) in pools {
            for row in rows.as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
                let en = row_str(row, 2);
                let pl = row_str(row, 3);
                let id = format!("{}~{:08x}", pool, crc32fast_like(en.as_bytes()));
                let mut letters = lex::letters_of(en);
                letters.extend(lex::letters_of(pl));
                let mut vars = lex::vars_of(en);
                vars.extend(lex::vars_of(pl));
                m.insert(id, Piece { pool: pool.as_str(), sc: row_str(row, 0), tone: row_str(row, 1), en, pl, letters, vars });
            }
        }
    }
    m
});

fn crc32fast_like(b: &[u8]) -> u32 {
    t2r_clanstory::crc::crc32(b)
}

pub static BY_POOL: LazyLock<IndexMap<&'static str, Vec<&'static str>>> = LazyLock::new(|| {
    let mut m: IndexMap<&'static str, Vec<&'static str>> = IndexMap::new();
    for (id, p) in REG.iter() {
        m.entry(p.pool).or_default().push(id.as_str());
    }
    m
});

/// The piece table equals the generated `REG` / `BY_POOL` (selftest).
pub fn reg_ok() -> bool {
    let want = tc("REG");
    let Some(o) = want.as_object() else { return false };
    if o.len() != REG.len() {
        return false;
    }
    for (id, p) in REG.iter() {
        let Some(w) = o.get(id) else { return false };
        let set = |i: usize| -> BTreeSet<String> { data::set_items(&w[i]).iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect() };
        let lw: BTreeSet<String> = set(5);
        let lm: BTreeSet<String> = p.letters.iter().map(|c| c.to_string()).collect();
        let vw: BTreeSet<String> = set(6).union(&set(7)).cloned().collect();
        if lw != lm || vw != p.vars || row_str(w, 3) != p.en {
            return false;
        }
    }
    true
}

// py: clan_chronicle.py:215 enabled
/// Switch: env T2R_CLAN_CHRONICLE=1/0, else clan_chronicle.txt (0 / off turns it off); default ON.
pub fn enabled() -> bool {
    let v = match std::env::var("T2R_CLAN_CHRONICLE") {
        Ok(v) => v,
        Err(_) => match std::fs::read(data::here(tc("SWITCH_FILE").as_str().unwrap_or("@HERE@/clan_chronicle.txt"))) {
            Ok(b) => String::from_utf8_lossy(&b).into_owned(),
            Err(_) => return true,
        },
    };
    !matches!(v.trim().to_lowercase().as_str(), "0" | "off" | "false" | "no")
}

// ------------------------------------------------------------------ personas
fn stage(p: &Value) -> &'static str {
    match vs(p, "r") {
        "kid" => "kid",
        "elder" => "elder",
        _ => "adult",
    }
}

fn habit_short(i: usize, pl: bool) -> &'static str {
    tc("HABIT_SHORT").get(i).map(|r| row_str(r, if pl { 1 } else { 0 })).unwrap_or("")
}
fn n_habits() -> usize {
    tc("HABIT_SHORT").as_array().map(|a| a.len()).unwrap_or(0)
}

// py: clan_chronicle.py:233 _habit_ok
fn habit_ok(i: usize, p: &Value, fk: &str) -> bool {
    let meta = &tc("HABIT_META")[i];
    let (cls, sc) = (row_str(meta, 0), row_str(meta, 1));
    if kd::is_kid(p) && (kd::combat_text(habit_short(i, false), Some("en")) || kd::combat_text(habit_short(i, true), Some("pl"))) {
        return false;
    }
    (sc == "*" || sc == fk) && (cls == "any" || cls == stage(p) || (cls == "adult" && stage(p) == "elder"))
}

// py: clan_chronicle.py:240 habit_pref
/// The person's habits in their own stable order of preference.
pub fn habit_pref(p: &Value, fk: &str) -> Vec<usize> {
    let mut ok: Vec<usize> = (0..n_habits()).filter(|&i| habit_ok(i, p, fk)).collect();
    ok.sort_by_key(|&i| hh!("habit", vs(p, "n"), vs(p, "r"), i));
    ok
}

/// `st["ch"]` with its own keys (the Python `ch_state`).
// py: clan_chronicle.py:479 ch_state
pub fn ch_state(st: &mut Value) -> &mut Value {
    if !st.is_object() {
        *st = Value::Object(Map::new());
    }
    let so = st.as_object_mut().expect("object");
    let c = so.entry("ch").or_insert_with(|| Value::Object(Map::new()));
    if !c.is_object() {
        *c = Value::Object(Map::new());
    }
    let co = c.as_object_mut().expect("object");
    co.entry("used").or_insert_with(|| Value::Object(Map::new()));
    co.entry("inv").or_insert_with(|| Value::Object(Map::new()));
    co.entry("keep").or_insert_with(|| Value::Object(Map::new()));
    co.entry("log").or_insert_with(|| Value::Array(Vec::new()));
    c
}

// py: clan_chronicle.py:247 assign_habits
/// Habits are unique inside a clan while the pool allows; persisted in `st["ch"]["hab"]` (person id -> habit index). Returns that map.
pub fn assign_habits(st: &mut Value, fk: &str) -> Value {
    let people: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let cs = ch_state(st);
    let co = cs.as_object_mut().expect("object");
    let hab = co.entry("hab").or_insert_with(|| Value::Object(Map::new()));
    let hm = hab.as_object_mut().expect("object");
    let alive: HashSet<String> = people.iter().map(|p| vi(p, "i").to_string()).collect();
    let dead_keys: Vec<String> = hm.keys().filter(|k| !alive.contains(*k)).cloned().collect();
    for k in dead_keys {
        hm.shift_remove(&k);
    }
    let mut taken: HashSet<i64> = hm.values().map(int_of).collect();
    let mut sorted: Vec<&Value> = people.iter().collect();
    sorted.sort_by_key(|p| vi(p, "i"));
    for p in sorted {
        let key = vi(p, "i").to_string();
        if let Some(cur) = hm.get(&key) {
            if !kd::is_kid(p) || habit_ok(int_of(cur).max(0) as usize, p, fk) {
                continue;
            }
        }
        let pref = habit_pref(p, fk);
        let pick = pref.iter().copied().find(|i| !taken.contains(&(*i as i64))).or_else(|| pref.first().copied()).unwrap_or(0);
        hm.insert(key, json!(pick));
        taken.insert(pick as i64);
    }
    Value::Object(hm.clone())
}

/// The stable personality of one survivor.
#[derive(Clone, Debug, PartialEq)]
pub struct Persona {
    pub temper: &'static str,
    pub habit: usize,
    pub past: usize,
    pub bond: Option<i64>,
    pub bt: Option<&'static str>,
}

// py: clan_chronicle.py:262 persona
/// Stable personality of survivor `p` among `people` (the living roster). With the story state the habit is the clan-unique assigned one (assign_habits).
pub fn persona(p: &Value, people: &[Value], fk: &str, st: Option<&mut Value>) -> Persona {
    let hv = hh!("persona", vs(p, "n"), vs(p, "r"), vs(p, "g")) as u64;
    let pasts_t = pasts().as_array().map(|a| a.as_slice()).unwrap_or(&[]);
    let pasts: Vec<usize> = pasts_t
        .iter()
        .enumerate()
        .filter(|(_, x)| (row_str(x, 0) == "*" || row_str(x, 0) == fk) && (row_str(x, 1) == stage(p) || (row_str(x, 1) == "adult" && stage(p) == "elder")))
        .map(|(i, _)| i)
        .collect();
    let mut hab: Option<usize> = None;
    if let Some(st) = st {
        let pid = vi(p, "i").to_string();
        hab = ch_state(st).get("hab").and_then(|m| m.get(&pid)).map(|v| int_of(v).max(0) as usize);
        if hab.is_none() {
            hab = assign_habits(st, fk).get(&pid).map(|v| int_of(v).max(0) as usize);
        }
    }
    let habit = match hab {
        Some(x) => x,
        None => habit_pref(p, fk).first().copied().unwrap_or(0),
    };
    let past = if pasts.is_empty() { 0 } else { pasts[((hv >> 8) % pasts.len() as u64) as usize] };
    let mut out = Persona { temper: TEMPERS[(hv % 6) as usize], habit, past, bond: None, bt: None };
    let mut others: Vec<&Value> = people.iter().filter(|q| vi(q, "i") != vi(p, "i")).collect();
    others.sort_by_key(|q| hh!("ring", vs(p, "n"), vs(q, "n")));
    if let Some(q) = others.first() {
        let mut pair = [vs(p, "n").to_string(), vs(q, "n").to_string()];
        pair.sort();
        let mut bt = BOND_TYPES[(hh!("bond", pair[0], pair[1]) % 4) as usize];
        let (pr, qr) = (vs(p, "r"), vs(q, "r"));
        if (pr == "kid" || qr == "kid") && bt == "sweet" {
            bt = "kin";
        }
        if (pr == "kid" && qr == "elder") || (pr == "elder" && qr == "kid") {
            bt = "mentor";
        }
        out.bond = Some(vi(q, "i"));
        out.bt = Some(bt);
    }
    out
}

fn cap1(s: &str) -> String {
    let mut it = s.chars();
    match it.next() {
        Some(c) => c.to_uppercase().collect::<String>() + it.as_str(),
        None => String::new(),
    }
}

// py: clan_chronicle.py:283 tagline
/// Roster tagline: "Medic, gentle. Nurse's apprentice. Winds a dead watch every dawn."
pub fn tagline(p: &Value, people: &[Value], fk: &str, lang: &str, st: Option<&mut Value>) -> Result<String, String> {
    let ps = persona(p, people, fk, st);
    let pl = lang == "pl";
    let g = vs(p, "g");
    let role = lex::role_word(p, fk, if pl { "pl" } else { "en" })?;
    let te = &tc("TEMPER_LABEL")[ps.temper];
    let t = if pl && g == "f" { row_str(te, 2) } else if pl { row_str(te, 1) } else { row_str(te, 0) };
    let his = match g {
        "m" => "his",
        "f" => "her",
        _ => "their",
    };
    let ha = habit_short(ps.habit, pl).replace("{his}", his);
    let past = &pasts()[ps.past];
    let pa = if pl { row_str(past, if g == "f" { 4 } else { 3 }) } else { row_str(past, 2) };
    Ok(format!("{}, {}. {}. {}.", cap1(&role), t, cap1(pa), cap1(&ha)))
}

// ------------------------------------------------------------------ text variables
pub fn norm_lang_pl(lang: &str) -> bool {
    // PLN.norm_lang
    let l = lang.trim().to_lowercase().replace('_', "-");
    matches!(l.as_str(), "1" | "pl" | "pol" | "polish") || l.starts_with("pl-")
}

fn sget<'a>(sym: &'a Value, k: &str) -> Option<&'a Value> {
    sym.get(k)
}

// py: clan_chronicle.py:301 _vars
pub fn vars_of_sym(sym: &Value, lang: &str) -> Result<Vars, String> {
    let fk = sym.get("f").map(pystr).ok_or("KeyError: f")?;
    let fac = data::t("clan_story_data", "FACTION").get(&fk).ok_or_else(|| format!("KeyError: {}", fk))?;
    let mut v: Vars = HashMap::new();
    for k in ["sup", "need", "kills", "deaths", "downs"] {
        v.insert(k.to_string(), sget(sym, k).map(pystr).unwrap_or_else(|| "0".to_string()));
    }
    let need = sget(sym, "need").map(int_of).unwrap_or(0);
    let sup = sget(sym, "sup").map(int_of).unwrap_or(0);
    v.insert("n".into(), (need - sup).max(0).to_string());
    let leader = if lang == "en" { if fk == "W" { "Command" } else { "the Prophet" } } else if fk == "W" { "Dowództwo" } else { "Prorok" };
    v.insert("leader".into(), leader.to_string());
    let mut items: Vec<String> = Vec::new();
    if let Some(lt) = sget(sym, "loot").and_then(|x| x.as_array()) {
        for e in lt {
            items.push(loot_phrase(&e.get(0).map(pystr).unwrap_or_default(), e.get(1).map(int_of).unwrap_or(0), lang)?);
        }
    }
    v.insert("loot_list".into(), join_list(&items, lang));
    if sget(sym, "finds").map(truthy).unwrap_or(false) {
        v.insert("loot_list".into(), ext::loot::items_text(&sym["finds"], lang));
    }
    if let Some(it) = sget(sym, "it").and_then(|x| x.as_str()) {
        if is_use(it) {
            v.insert("item".into(), qty(it, sget(sym, "itn").map(int_of).unwrap_or(1), lang)?);
            let mut thing = qty(it, 1, lang)?;
            v.insert("thing".into(), {
                if lang == "en" && thing.starts_with("a ") {
                    thing = format!("the {}", &thing[2..]);
                }
                thing
            });
        }
    }
    let w = tc("WHENS").get(crate::py::pymod(sget(sym, "when").map(int_of).unwrap_or(0), 3) as usize).cloned().unwrap_or(Value::Null);
    v.insert("when".into(), row_str_owned(&w, if lang == "pl" { 1 } else { 0 }));
    v.insert("map".into(), map_name(&sget(sym, "map").filter(|x| truthy(x)).map(pystr).unwrap_or_default(), lang));
    if lang == "pl" {
        let e = data::t("clan_story_pl", "ENEMY").get(&fk).ok_or_else(|| format!("KeyError: {}", fk))?;
        v.insert("enemy".into(), row_str(e, 0).to_string());
        v.insert("enemy_gen".into(), row_str(e, 1).to_string());
        v.insert("enemy_ins".into(), row_str(e, 2).to_string());
        v.insert("enemies".into(), row_str(e, 1).to_string());
    } else {
        v.insert("enemy".into(), vs(fac, "enemy").to_string());
        v.insert("enemies".into(), format!("the {}", vs(fac, "enemies")));
    }
    Ok(v)
}

fn row_str_owned(v: &Value, i: usize) -> String {
    v.get(i).and_then(|x| x.as_str()).unwrap_or("").to_string()
}

// py: clan_chronicle.py:330 _render_ids
fn render_ids(ids: &[&str], people: &Value, sym: &Value, lang: &str) -> Result<String, String> {
    let fk = sym.get("f").map(pystr).ok_or("KeyError: f")?;
    let vars = vars_of_sym(sym, lang)?;
    let mut parts = Vec::new();
    for i in ids {
        let p = REG.get(*i).ok_or_else(|| format!("KeyError: {}", i))?;
        parts.push(lex::render(if lang == "pl" { p.pl } else { p.en }, people, &vars, lang, &fk)?);
    }
    Ok(parts.join(" "))
}

/// `{letter: {"n", "g", "r"}}` from stored params.p (`{letter: [name, gender, role]}`), as render_msg builds it.
pub fn people_from_params(params: &Value) -> Value {
    let mut m = Map::new();
    if let Some(po) = params.get("p").and_then(|x| x.as_object()) {
        for (k, v) in po {
            let n = v.get(0).map(pystr).unwrap_or_default();
            let g = match v.get(1).and_then(|x| x.as_str()) {
                Some(g) if matches!(g, "m" | "f" | "n") => g.to_string(),
                _ => "n".to_string(),
            };
            let r = if v.as_array().map(|a| a.len() > 2).unwrap_or(false) { v[2].clone() } else { json!("scavenger") };
            m.insert(k.clone(), json!({"n": n, "g": g, "r": r}));
        }
    }
    Value::Object(m)
}

// py: clan_chronicle.py:340 render_msg
/// A stored chronicle row (lid "ch/<piece ids joined by +>") in `lang`: Ok(None) when a piece is unknown, Err where Python raises.
pub fn render_msg_res(lid: &str, params: &Value, lang: &str) -> Result<Option<String>, String> {
    if lid.starts_with("arc/") {
        return clan_arcs::render_msg_res(lid, params, lang);
    }
    if lid.starts_with("sg/") {
        return Ok(ext::saga::render_msg(lid, params, lang));
    }
    if !lid.starts_with("ch/") || !params.is_object() {
        return Ok(None);
    }
    let ids: Vec<&str> = lid[3..].split('+').collect();
    if ids.iter().any(|i| !REG.contains_key(*i)) {
        return Ok(None);
    }
    let people = people_from_params(params);
    let sym = params.get("s").cloned().unwrap_or_else(ext::obj);
    render_ids(&ids, &people, &sym, if norm_lang_pl(lang) { "pl" } else { "en" }).map(Some)
}

/// `render_msg_res` with a Python exception reported as None.
pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
    render_msg_res(lid, params, lang).ok().flatten()
}

// ------------------------------------------------------------------ choosing
fn tone_ok(tone: &str, morale: i64) -> bool {
    tone == "*" || tone == "temper" || (tone == "g" && morale >= 45) || (tone == "b" && morale <= 60) || (tone == "n" && (30..=75).contains(&morale))
}

// py: clan_chronicle.py:354 _pick
#[allow(clippy::too_many_arguments)]
fn pick_piece(
    pool: &str,
    fk: &str,
    morale: i64,
    letters: &BTreeSet<char>,
    rng: &mut PyRandom,
    used: &IndexMap<String, i64>,
    day: i64,
    temper: Option<&str>,
    avail: Option<&BTreeSet<String>>,
    people: Option<&Value>,
) -> Result<Option<String>, String> {
    let mut cands: Vec<&'static str> = Vec::new();
    for i in BY_POOL.get(pool).map(|v| v.as_slice()).unwrap_or(&[]) {
        let p = &REG[*i];
        if !(p.sc == "*" || p.sc == fk) || !p.letters.is_subset(letters) {
            continue;
        }
        if let Some(pe) = people {
            if pe.as_object().map(|o| !o.is_empty()).unwrap_or(false) && !kd::kid_safe(p.en, pe, &[p.pl]) {
                continue;
            }
        }
        if let Some(av) = avail {
            if !p.vars.is_subset(av) {
                continue;
            }
        }
        if let Some(t) = temper {
            if p.tone != t {
                continue;
            }
        }
        if temper.is_none() && !tone_ok(p.tone, morale) {
            continue;
        }
        cands.push(i);
    }
    if cands.is_empty() {
        return Ok(None);
    }
    let get = |i: &str| used.get(i).copied().unwrap_or(-999);
    let mut pool_: Vec<&'static str> = cands.iter().copied().filter(|i| day - get(i) >= WINDOW_DAYS).collect();
    if pool_.is_empty() {
        let mut s = cands.clone();
        s.sort_by_key(|i| get(i));
        let n = (cands.len() / 2).max(1);
        pool_ = s.into_iter().take(n).collect();
    }
    pool_.sort();
    let i = rng.choice_index(pool_.len()).map_err(|e| format!("{:?}", e))?;
    Ok(Some(pool_[i].to_string()))
}

fn pp(p: &Value) -> Value {
    json!([p.get("n").cloned().unwrap_or(Value::Null), p.get("g").cloned().unwrap_or(Value::Null), p.get("r").cloned().unwrap_or(Value::Null)])
}

/// `re.search(r"(?<!\w)" + re.escape(name) + r"(?!\w)", s)`.
pub fn has_word(s: &str, name: &str) -> bool {
    if name.is_empty() {
        return true;
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

/// The chronicle's day object (`_Day`): the day's roster snapshot (`alive` shares the person dicts of st["people"] in Python: call `sync_alive` after anything
/// that edits persons in `st`), the repeat control `used` (`cs["used"]` of the caller unless one was passed), the spotlight set, the rows.
pub struct Day {
    pub fk: &'static str,
    pub day: i64,
    pub alive: Vec<Value>,
    pub morale: i64,
    pub spot: BTreeSet<i64>,
    pub used: IndexMap<String, i64>,
    pub rows: Vec<Value>,
    pub finds: Vec<Value>,
    pub newc: Value,
    pub r: Value,
    pub matchv: Value,
}

/// The options of `_Day.compose` (Python keyword arguments).
pub struct ComposeOpts<'a> {
    pub c: Option<&'a Value>,
    pub d: Option<&'a Value>,
    pub sym_extra: Value,
    pub react: bool,
    pub habit: f64,
    pub bond: f64,
    pub prep: bool,
    pub extra_pools: Vec<&'a str>,
}

impl Default for ComposeOpts<'_> {
    fn default() -> Self {
        ComposeOpts { c: None, d: None, sym_extra: Value::Null, react: true, habit: 0.6, bond: 0.35, prep: false, extra_pools: Vec::new() }
    }
}

impl Day {
    // py: clan_chronicle.py:404 _Day.__init__
    pub fn new(st: &Value, newc: &Value, r: &Value, matchv: &Value, used: IndexMap<String, i64>) -> Day {
        let fk = if vi(st, "fac") == crate::clan::clan::SERAPHITES { "S" } else { "W" };
        let alive: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        let away: HashSet<String> = st.get("ch").and_then(|c| c.get("away")).and_then(|a| a.as_object()).map(|o| o.keys().cloned().collect()).unwrap_or_default();
        let spot: BTreeSet<i64> = alive.iter().filter(|p| away.contains(&vi(p, "i").to_string())).map(|p| vi(p, "i")).collect();
        Day {
            fk,
            day: vi(newc, "day"),
            alive,
            morale: st.get("morale").map(int_of).unwrap_or(50),
            spot,
            used,
            rows: Vec::new(),
            finds: Vec::new(),
            newc: newc.clone(),
            r: r.clone(),
            matchv: matchv.clone(),
        }
    }

    /// Re-read the persons of `alive` from `st["people"]` (Python shares the dict objects): people that left the roster keep their last snapshot.
    pub fn sync_alive(&mut self, st: &Value) {
        let cur = st.get("people").and_then(|v| v.as_array());
        for p in self.alive.iter_mut() {
            let id = vi(p, "i");
            if let Some(q) = cur.and_then(|a| a.iter().find(|q| vi(q, "i") == id)) {
                *p = q.clone();
            }
        }
    }

    /// `d.byid[i]`.
    pub fn byid(&self, i: i64) -> Option<&Value> {
        self.alive.iter().find(|p| vi(p, "i") == i)
    }

    // py: clan_chronicle.py:416 _Day.person
    /// fight=true (a match, raid, haul or weapon line): grown people only. The person who has been in the chronicle the longest ago comes first.
    pub fn person(&self, role: Option<&str>, states: &[&str], exclude: &BTreeSet<i64>, fight: bool) -> Option<Value> {
        let mut c: Vec<&Value> = self
            .alive
            .iter()
            .filter(|p| states.contains(&vs(p, "s")) && !self.spot.contains(&vi(p, "i")) && !exclude.contains(&vi(p, "i")) && role.map(|r| vs(p, "r") == r).unwrap_or(true) && (!fight || kd::can_fight(p)))
            .collect();
        if c.is_empty() {
            return None;
        }
        c.sort_by_key(|p| (self.used.get(&format!("P{}", vi(p, "i"))).copied().unwrap_or(-999), hh!("sp", self.day, vs(p, "n"))));
        Some(c[0].clone())
    }

    // py: clan_chronicle.py:497 _Day.partner
    pub fn partner(&self, st: &mut Value, a: &Value) -> Option<Value> {
        let ps = persona(a, &self.alive, self.fk, Some(st));
        let b = self.byid(ps.bond?)?;
        if vi(b, "i") != vi(a, "i") {
            Some(b.clone())
        } else {
            None
        }
    }

    // py: clan_chronicle.py:429 _Day.compose
    /// One row (kind "chron") from pool `kind` about A (and B, C, D); None when nothing fits. Marks the pieces and people used.
    pub fn compose(&mut self, st: &mut Value, rng: &mut PyRandom, kind: &str, a: &Value, b: Option<&Value>, o: &ComposeOpts) -> Result<Option<Value>, String> {
        let (fk, day) = (self.fk, self.day);
        let mut people = Map::new();
        people.insert("A".into(), a.clone());
        if let Some(b) = b {
            people.insert("B".into(), b.clone());
        }
        if let Some(c) = o.c {
            people.insert("C".into(), c.clone());
        }
        if let Some(d) = o.d {
            people.insert("D".into(), d.clone());
        }
        let letters: BTreeSet<char> = people.keys().filter_map(|k| k.chars().next()).collect();
        let people_v = Value::Object(people);
        let mut sym = Map::new();
        sym.insert("f".into(), json!(fk));
        sym.insert("day".into(), json!(day));
        sym.insert("when".into(), json!(rng.randrange_to(3).map_err(|e| format!("{:?}", e))?));
        if self.matchv.as_object().map(|m| !m.is_empty()).unwrap_or(false) {
            for k in ["kills", "deaths", "downs"] {
                if let Some(v) = self.matchv.get(k).filter(|v| !v.is_null()) {
                    sym.insert(k.into(), json!(int_of(v)));
                }
            }
            sym.insert("map".into(), json!(self.matchv.get("map").map(|v| if v.is_null() { "None".to_string() } else { pystr(v) }).unwrap_or_default()));
        }
        sym.insert("sup".into(), json!(vi(&self.r, "supplies")));
        sym.insert("need".into(), json!(vi(&self.r, "needed")));
        if let Some(x) = o.sym_extra.as_object() {
            for (k, v) in x {
                sym.insert(k.clone(), v.clone());
            }
        }
        let mut avail: BTreeSet<String> = ["sup", "need", "n", "leader", "when", "enemy", "enemies", "enemy_gen", "enemy_ins"].iter().map(|s| s.to_string()).collect();
        for k in ["kills", "deaths", "downs", "map"] {
            if sym.contains_key(k) {
                avail.insert(k.to_string());
            }
        }
        let tr = |k: &str| sym.get(k).map(truthy).unwrap_or(false);
        if tr("loot") || tr("finds") {
            avail.insert("loot_list".into());
        }
        if tr("it") {
            avail.insert("item".into());
            avail.insert("thing".into());
        }
        let head = match pick_piece(kind, fk, self.morale, &letters, rng, &self.used, day, None, Some(&avail), Some(&people_v))? {
            Some(h) => h,
            None => return Ok(None),
        };
        let mut ids: Vec<String> = vec![head.clone()];
        let tone = REG[head.as_str()].tone;
        let pers = persona(a, &self.alive, fk, Some(st));
        for ep in &o.extra_pools {
            if let Some(xp) = pick_piece(ep, fk, self.morale, &letters, rng, &self.used, day, None, Some(&avail), Some(&people_v))? {
                ids.push(xp);
            }
        }
        let mut opt: Vec<String> = Vec::new();
        if o.react && rng.random() < 0.6 {
            let pool = if tone == "g" || ((tone == "*" || tone == "n") && self.morale >= 55) { "react_g" } else { "react_b" };
            if let Some(rp) = pick_piece(pool, fk, self.morale, &letters, rng, &self.used, day, Some(pers.temper), None, Some(&people_v))? {
                opt.push(rp);
            }
        }
        if o.prep && b.is_some() {
            if let Some(pp_) = pick_piece("prep", fk, self.morale, &letters, rng, &self.used, day, None, None, Some(&people_v))? {
                opt.push(pp_);
            }
        }
        let hb = BY_POOL.get("habit").and_then(|v| v.get(pers.habit)).ok_or("IndexError: habit")?.to_string();
        if rng.random() < o.habit && day - self.used.get(&hb).copied().unwrap_or(-999) >= WINDOW_HABIT {
            opt.push(hb);
        }
        if b.is_some() && !REG[head.as_str()].letters.contains(&'B') && rng.random() < o.bond {
            let bl: BTreeSet<char> = ['A', 'B'].into_iter().collect();
            let pool = format!("bond_{}", pers.bt.unwrap_or("kin"));
            if let Some(bp) = pick_piece(&pool, fk, self.morale, &bl, rng, &self.used, day, None, None, Some(&people_v))? {
                opt.push(bp);
            }
        }
        rng.shuffle(&mut opt);
        ids.extend(opt.into_iter().take(MAX_TOUCH));
        for i in &ids {
            self.used.insert(i.clone(), day);
        }
        for p in people_v.as_object().expect("object").values() {
            if let Some(i) = p.get("i") {
                self.used.insert(format!("P{}", int_of(i)), day);
                self.spot.insert(int_of(i));
            }
        }
        let msg_people: Map<String, Value> = people_v.as_object().expect("object").iter().map(|(k, v)| (k.clone(), pp(v))).collect();
        let lid = format!("ch/{}", ids.join("+"));
        let params = json!({"p": Value::Object(msg_people), "s": Value::Object(sym)});
        let idr: Vec<&str> = ids.iter().map(|s| s.as_str()).collect();
        let text = render_ids(&idr, &people_from_params(&params), &params["s"], "en")?;
        let _ = people_v.get("A");
        if kd::any_kid(&people_v) {
            let ok_en = kd::kid_text_ok(&text, &people_v, "en");
            let ok = ok_en && kd::kid_text_ok(&render_ids(&idr, &people_from_params(&params), &params["s"], "pl")?, &people_v, "pl");
            if !ok {
                return Ok(None);
            }
        }
        let names: Vec<Value> = people_v.as_object().expect("object").values().filter(|v| has_word(&text, vs(v, "n"))).map(|v| json!(vs(v, "n"))).collect();
        Ok(Some(json!({"kind": "chron", "mood": tc("MOODS").get(kind).and_then(|x| x.as_str()).unwrap_or("calm"), "text": text, "names": names, "lid": lid, "params": params})))
    }
}

// ------------------------------------------------------------------ the chronicle of one clan day
// py: clan_chronicle.py:490 match_log
/// The browsable match summaries for the UI (newest first).
pub fn match_log(st: &mut Value, lang: &str, limit: usize) -> Result<Vec<Value>, String> {
    let log: Vec<Value> = ch_state(st).get("log").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let start = log.len().saturating_sub(limit);
    let lg = if lang == "pl" { "pl" } else { "en" };
    let mut out = Vec::new();
    for e in log[start..].iter().rev() {
        let mut row = e.as_object().cloned().unwrap_or_default();
        let mut items = Vec::new();
        for kn in e.get("loot").and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[]) {
            items.push(loot_phrase(&kn.get(0).map(pystr).unwrap_or_default(), kn.get(1).map(int_of).unwrap_or(0), lg)?);
        }
        let lt = join_list(&items, lg);
        row.insert("loot_text".into(), json!(lt));
        if e.get("finds").map(truthy).unwrap_or(false) {
            row.insert("loot_text_basic".into(), json!(lt));
            row.insert("finds".into(), ext::loot::public(&e["finds"], lg));
            row.insert("loot_text".into(), json!(ext::loot::items_text(&e["finds"], lg)));
        }
        out.push(Value::Object(row));
    }
    Ok(out)
}

// py: clan_chronicle.py:505 inventory
pub fn inventory(st: &mut Value, lang: &str) -> Result<(Vec<String>, Vec<String>), String> {
    let c = ch_state(st).clone();
    let mut inv: Vec<(String, i64)> = c["inv"].as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), int_of(v))).collect()).unwrap_or_default();
    inv.sort();
    let mut a = Vec::new();
    for (k, n) in inv {
        if n > 0 {
            a.push(qty(&k, n, lang)?);
        }
    }
    let mut keys: Vec<String> = c["keep"].as_object().map(|o| o.keys().cloned().collect()).unwrap_or_default();
    keys.sort();
    let mut b = Vec::new();
    for k in keys {
        b.push(loot_phrase(&k, 1, lang)?);
    }
    Ok((a, b))
}

// py: clan_chronicle.py:512 day_rows
/// The chronicle rows of the clan day; also records the members' status changes (clan_status.observe, errors are ignored).
/// `used` = repeat control (None: `st["ch"]["used"]`); `rng` None = `random.Random(_h("chron", tid, day))`.
pub fn day_rows(st: &mut Value, newc: &Value, r: &Value, matchv: Option<&Value>, used: Option<&mut Value>, rng: Option<&mut PyRandom>) -> Result<Vec<Value>, String> {
    let rows = day_rows_inner(st, newc, r, matchv, used, rng)?;
    if let Err(ex) = ext::status::observe(st, &rows) {
        eprint!("clan_status failed: {:?}\n", ex);
    }
    Ok(rows)
}

fn log_push(st: &mut Value, e: Value) {
    if let Some(l) = ch_state(st).get_mut("log").and_then(|v| v.as_array_mut()) {
        l.push(e);
    }
}

fn used_to_map(v: &Value) -> IndexMap<String, i64> {
    v.as_object().map(|o| o.iter().map(|(k, x)| (k.clone(), int_of(x))).collect()).unwrap_or_default()
}

fn map_to_used(m: &IndexMap<String, i64>) -> Value {
    Value::Object(m.iter().map(|(k, v)| (k.clone(), json!(*v))).collect())
}

fn cs_of(st: &mut Value) -> &mut Value {
    ch_state(st)
}

// py: clan_chronicle.py:522 _day_rows
fn day_rows_inner(st: &mut Value, newc: &Value, r: &Value, matchv: Option<&Value>, used: Option<&mut Value>, rng: Option<&mut PyRandom>) -> Result<Vec<Value>, String> {
    let fk: &str = if vi(st, "fac") == crate::clan::clan::SERAPHITES { "S" } else { "W" };
    ch_state(st);
    assign_habits(st, fk);
    let r_empty = ext::obj();
    let r = if truthy(r) { r } else { &r_empty };
    let null = Value::Null;
    let matchv: &Value = match matchv {
        Some(m) => m,
        None => r.get("match").unwrap_or(&null),
    };
    let mut own_rng;
    let rng: &mut PyRandom = match rng {
        Some(x) => x,
        None => {
            own_rng = PyRandom::from_i64(hh!("chron", st.get("tid").map(pystr).unwrap_or_default(), newc.get("day").map(pystr).unwrap_or_default()) as i64);
            &mut own_rng
        }
    };
    let caller_used = used.is_some();
    let used_map = match &used {
        Some(u) => used_to_map(u),
        None => used_to_map(&ch_state(st)["used"]),
    };
    let mut d = Day::new(st, newc, r, matchv, used_map);
    // write the repeat control back whatever the way out is
    let res = day_rows_body(st, newc, r, matchv, rng, &mut d, fk);
    let um = map_to_used(&d.used);
    match used {
        Some(u) => *u = um,
        None => {
            if !caller_used {
                ch_state(st)["used"] = um;
            }
        }
    }
    res
}

#[allow(clippy::too_many_lines)]
fn day_rows_body(st: &mut Value, newc: &Value, r: &Value, matchv: &Value, rng: &mut PyRandom, d: &mut Day, fk: &str) -> Result<Vec<Value>, String> {
    if d.alive.len() < 3 {
        return Ok(vec![]);
    }
    let mut out: Vec<Value> = Vec::new();
    let died_now: Vec<Value> = st.get("dead").and_then(|v| v.as_array()).map(|a| a.iter().filter(|x| vi(x, "d") == d.day).cloned().collect()).unwrap_or_default();
    let mut loot: Vec<Value> = Vec::new();
    let has_map = truthy(matchv) && matchv.get("map").map(truthy).unwrap_or(false);
    let strict = std::env::var("T2R_SAGA_STRICT").map(|v| !v.is_empty()).unwrap_or(false);
    if has_map {
        let have = ch_state(st)["keep"].clone();
        loot = roll_loot(rng, r, matchv, newc, &have)?;
        let mut finds: Vec<Value> = Vec::new();
        let tried: Result<(), String> = (|| {
            let cur = json!({"win": matchv.get("win").cloned().unwrap_or(Value::Null), "sup": vi(r, "supplies"), "need": vi(r, "needed")});
            let mut fo_st = st.clone();
            fo_st["morale"] = json!(st.get("morale").map(int_of).unwrap_or(50));
            let mut hist: Vec<Value> = {
                let l = ch_state(st).get("log").and_then(|v| v.as_array()).cloned().unwrap_or_default();
                l[l.len().saturating_sub(6)..].to_vec()
            };
            hist.push(cur);
            let score = ext::loot::fortune(&fo_st, ch_state(st), &hist)?;
            let mut lrng = PyRandom::from_i64(hh!("loot", st.get("tid").map(pystr).unwrap_or_default(), d.day) as i64);
            finds = ext::loot::roll_finds(&mut lrng, st, r, matchv, &score, d.day)?;
            let have = ch_state(st)["keep"].clone();
            let legacy = ext::loot::legacy_loot(&finds, &have);
            if !legacy.is_empty() {
                loot = legacy;
            }
            d.finds = finds.clone();
            Ok(())
        })();
        if let Err(ex) = tried {
            if strict {
                return Err(ex);
            }
            eprint!("clan_loot failed: {}\n", ex);
        }
        let mut gain = 0i64;
        for kn in &loot {
            let k = kn.get(0).map(pystr).unwrap_or_default();
            let n = kn.get(1).map(int_of).unwrap_or(0);
            if let Some(kr) = keep_row(&k) {
                ch_state(st)["keep"][k.as_str()] = json!({"d": d.day, "m": matchv.get("map").map(pystr).unwrap_or_default()});
                gain += kr.get(2).map(int_of).unwrap_or(0);
            } else {
                let ur = use_row(&k).ok_or_else(|| format!("KeyError: {}", k))?;
                let cur = ch_state(st)["inv"].get(&k).map(int_of).unwrap_or(0);
                ch_state(st)["inv"][k.as_str()] = json!(cur + n);
                gain += ur.get(7).map(int_of).unwrap_or(0);
            }
        }
        let morale = (st.get("morale").map(int_of).unwrap_or(50) + gain).min(100);
        st["morale"] = json!(morale);
        d.morale = morale;
        let mid = matchv.get("mid").filter(|v| truthy(v)).map(pystr).unwrap_or_default();
        let g = |k: &str| matchv.get(k).cloned().unwrap_or(Value::Null);
        log_push(
            st,
            json!({"d": d.day, "map": matchv.get("map").map(pystr).unwrap_or_default(), "sup": vi(r, "supplies"), "need": vi(r, "needed"), "parts": vi(r, "parts"),
                   "kills": matchv.get("kills").filter(|v| truthy(v)).map(int_of).unwrap_or(0), "deaths": g("deaths"), "downs": g("downs"), "loot": Value::Array(loot.clone()),
                   "execs": g("execs"), "revives": g("revives"), "win": g("win"), "mid": mid}),
        );
        if let Some(sp) = r.get("sup").filter(|v| v.is_object()) {
            if sp.get("need").map(truthy).unwrap_or(false) {
                let mut m = Map::new();
                for k in ["got", "need", "short", "drawn"] {
                    m.insert(k.into(), sp.get(k).cloned().unwrap_or_else(ext::obj));
                }
                last_log(st)?.insert("cat".into(), Value::Object(m));
            }
        }
        if !finds.is_empty() {
            let fort = ch_state(st).get("fort").and_then(|f| f.get("s")).cloned().ok_or("KeyError: fort")?;
            let l = last_log(st)?;
            l.insert("finds".into(), Value::Array(finds));
            l.insert("fort".into(), fort);
        }
        ext::loot::update_moods(st)?;
        let cs = ch_state(st);
        if let Some(l) = cs.get_mut("log").and_then(|v| v.as_array_mut()) {
            let n = l.len();
            if n > 30 {
                l.drain(..n - 30);
            }
        }
    }
    let mut arc_rows = clan_arcs::run(d, st, newc, r, matchv, rng)?;
    let arc_at = out.len();
    out.extend(arc_rows.iter().cloned());
    {
        let cs = ch_state(st);
        let mut ids: Vec<i64> = Vec::new();
        for a in cs.get("arcs").and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[]) {
            if let Some(c) = a.get("c").and_then(|c| c.as_object()) {
                ids.extend(c.values().map(int_of));
            }
        }
        if let Some(aw) = cs.get("away").and_then(|v| v.as_object()) {
            ids.extend(aw.keys().map(|k| k.trim().parse::<i64>().unwrap_or(0)));
        }
        d.spot.extend(ids);
    }
    d.sync_alive(st);
    let sg_rows: Vec<Value> = match ext::saga::run(d, st, newc, r, matchv, rng, &loot, &died_now, &mut arc_rows) {
        Ok(v) => v,
        Err(ex) => {
            if strict {
                return Err(ex);
            }
            eprint!("clan_saga failed: {}\n", ex);
            Vec::new()
        }
    };
    d.sync_alive(st);
    // the saga ties a sentence to some arc rows in place (the Python rows are shared with `out`)
    for (i, row) in arc_rows.iter().enumerate() {
        out[arc_at + i] = row.clone();
    }
    out.extend(sg_rows.iter().cloned());
    let mut plan: Vec<String> = Vec::new();
    if has_map && sg_rows.is_empty() {
        let k = matchv.get("kills").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
        let de = matchv.get("deaths").filter(|v| !v.is_null()).map(int_of);
        let dn = matchv.get("downs").filter(|v| !v.is_null()).map(int_of);
        match de {
            None => plan.push("match_kills".into()),
            Some(de) => {
                if dn.map(|x| x >= 3).unwrap_or(false) && rng.random() < 0.5 {
                    plan.push("match_downs".into());
                } else if (k as f64) >= (3f64).max(1.5 * de as f64) && de <= 4 {
                    plan.push("match_strong".into());
                } else if de > k {
                    plan.push("match_bad".into());
                } else {
                    plan.push("match_even".into());
                }
            }
        }
    }
    if !sg_rows.is_empty() {
    } else if !died_now.is_empty() {
        plan.push(if rng.random() < 0.6 { "mourn".into() } else { "lastwords".into() });
    } else if vi(r, "supplies") < vi(r, "needed") {
        plan.push(if rng.random() < 0.6 { "haul_short".into() } else { "ration".into() });
    } else {
        plan.push("haul_good".into());
    }
    if plan.len() < MAX_ROWS {
        let s = vi(newc, "sick");
        let u = vi(newc, "hungry");
        let gained = vi(r, "gained") + vi(r, "evg");
        let mut opts: Vec<String> = Vec::new();
        if gained != 0 {
            opts.push("newcomer".into());
        }
        if s != 0 {
            opts.push("sick_watch".into());
        }
        if u != 0 {
            opts.push("hunger".into());
        }
        opts.push("kids".into());
        opts.push("quiet".into());
        if sg_rows.is_empty() {
            opts.push("raid_prep".into());
            opts.push("raid_prep".into());
        }
        if d.morale < 55 {
            opts.push("quarrel".into());
        }
        let cs = ch_state(st).clone();
        let old_keep: Vec<String> = if sg_rows.is_empty() {
            cs["keep"].as_object().map(|o| o.iter().filter(|(_, v)| vi(v, "d") < d.day).map(|(k, _)| k.clone()).collect()).unwrap_or_default()
        } else {
            Vec::new()
        };
        if sg_rows.is_empty() {
            for c in CAT_ORDER {
                let any = cs["inv"].as_object().map(|o| o.iter().any(|(k, n)| int_of(n) > 0 && use_row(k).map(|u| row_str(u, 0) == c).unwrap_or(false))).unwrap_or(false);
                if any {
                    let n = if (c == "food" && u != 0) || (c == "med" && s != 0) || c == "ammo" { 2 } else { 1 };
                    for _ in 0..n {
                        opts.push(format!("use:{}", c));
                    }
                }
            }
        }
        for _ in 0..3 {
            for k in &old_keep {
                opts.push(format!("keep:{}", k));
            }
        }
        let i = rng.choice_index(opts.len()).map_err(|e| format!("{:?}", e))?;
        plan.push(opts[i].clone());
    }
    let n_rows = MAX_ROWS as i64 - arc_rows.len() as i64 - sg_rows.len() as i64;
    let take = (n_rows.max(1)) as usize;
    let none_set: BTreeSet<i64> = BTreeSet::new();
    for kind in plan.iter().take(take) {
        let kind = kind.as_str();
        let mut extra = Value::Object(Map::new());
        let mut pool: String = kind.to_string();
        let mut role: Option<String> = None;
        let mut extra_ids: Vec<&str> = Vec::new();
        let mut a: Option<Value> = None;
        let mut dd: Option<Value> = None;
        if let Some(cat) = kind.strip_prefix("use:") {
            let cs = ch_state(st).clone();
            let mut keys: Vec<&str> = cs["inv"].as_object().map(|o| o.iter().filter(|(k, n)| int_of(n) > 0 && use_row(k).map(|u| row_str(u, 0) == cat).unwrap_or(false)).map(|(k, _)| k.as_str()).collect()).unwrap_or_default();
            keys.sort();
            let key = *rng.choice(&keys).map_err(|e| format!("{:?}", e))?;
            extra = json!({"it": key, "itn": cs["inv"][key]});
            pool = tc("CAT_POOL").get(cat).map(pystr).ok_or("KeyError: CAT_POOL")?;
            role = Some(match cat {
                "food" => "cook",
                "med" => "medic",
                "ammo" => "scout",
                _ => "mechanic",
            }
            .to_string());
        } else if let Some(key) = kind.strip_prefix("keep:") {
            let cs = ch_state(st).clone();
            extra = json!({"map": cs["keep"][key]["m"]});
            pool = format!("loot_use_{}", key);
            role = if matches!(key, "chess" | "teddy" | "comic") && d.alive.iter().any(|p| vs(p, "r") == "kid") { Some("kid".to_string()) } else { None };
        } else if kind == "sick_watch" {
            let nsp: BTreeSet<i64> = d.alive.iter().filter(|p| vs(p, "il") == "spores").map(|p| vi(p, "i")).collect();
            a = d.person(None, &["s"], &nsp, false).or_else(|| d.person(None, &["h", "u", "s"], &nsp, false)).or_else(|| d.person(None, &["h", "u", "s"], &none_set, false));
        } else if kind == "newcomer" {
            let mut best: Option<&Value> = None;
            for p in &d.alive {
                if best.map(|b| vi(p, "jd") > vi(b, "jd")).unwrap_or(true) {
                    best = Some(p);
                }
            }
            a = best.cloned();
        } else if kind == "hunger" {
            role = Some(["cook", "elder", "medic"][rng.choice_index(3).map_err(|e| format!("{:?}", e))?].to_string());
        } else if kind == "kids" {
            role = Some(["elder", "guard", "cook", "medic"][rng.choice_index(4).map_err(|e| format!("{:?}", e))?].to_string());
        } else if kind == "mourn" || kind == "lastwords" {
            if died_now.is_empty() {
                continue;
            }
            a = d.person(None, &["h", "u"], &none_set, false);
            dd = Some(died_now[0].clone());
        } else {
            role = match kind {
                "match_downs" => Some("medic"),
                "match_kills" => Some("scout"),
                "haul_good" | "haul_short" | "ration" => Some("cook"),
                "raid_prep" => Some("scout"),
                _ => None,
            }
            .map(|s| s.to_string());
        }
        let it_ammo = kind.starts_with("use:") && extra.get("it").and_then(|x| x.as_str()).and_then(use_row).map(|u| row_str(u, 0) == "ammo").unwrap_or(false);
        let fight = kd::is_combat_pool_chron(kind) || kind == "use:ammo" || kind.starts_with("match_") || it_ammo;
        if let Some(x) = &a {
            if fight && !kd::can_fight(x) {
                a = None;
            }
        }
        if a.is_none() {
            let first = match &role {
                Some(rl) => d.person(Some(rl), &["h", "u", "s"], &none_set, fight),
                None => None,
            };
            a = first.or_else(|| d.person(None, &["h", "u", "s"], &none_set, fight));
        }
        let Some(a) = a else { continue };
        let mut b = d.partner(st, &a);
        if b.as_ref().map(|b| d.spot.contains(&vi(b, "i")) || (fight && !kd::can_fight(b))).unwrap_or(true) {
            let ex: BTreeSet<i64> = [vi(&a, "i")].into_iter().collect();
            b = d.person(None, &["h", "u"], &ex, fight);
        }
        if kind.starts_with("match_") && !loot.is_empty() {
            let finds = if d.finds.is_empty() { Value::Null } else { Value::Array(d.finds.clone()) };
            let mut m = extra.as_object().cloned().unwrap_or_default();
            m.insert("loot".into(), Value::Array(loot.clone()));
            m.insert("finds".into(), finds);
            extra = Value::Object(m);
            extra_ids = vec!["loot_found"];
        }
        let o1 = ComposeOpts {
            d: dd.as_ref(),
            prep: kind == "raid_prep",
            sym_extra: extra.clone(),
            extra_pools: extra_ids.clone(),
            react: !(kind.starts_with("use:") || kind.starts_with("keep:")),
            ..Default::default()
        };
        let mut row = d.compose(st, rng, &pool, &a, b.as_ref(), &o1)?;
        if row.is_none() && b.is_some() {
            let o2 = ComposeOpts { d: dd.as_ref(), sym_extra: extra.clone(), extra_pools: extra_ids.clone(), ..Default::default() };
            row = d.compose(st, rng, &pool, &a, None, &o2)?;
        }
        if let Some(row) = row {
            out.push(row);
            if kind.starts_with("use:") {
                let it = extra.get("it").and_then(|x| x.as_str()).unwrap_or("");
                let cat = use_row(it).map(|u| row_str(u, 0)).unwrap_or("");
                if cat == "food" || cat == "med" {
                    let cur = ch_state(st)["inv"].get(it).map(int_of).unwrap_or(0);
                    ch_state(st)["inv"][it] = json!((cur - 1).max(0));
                }
            }
        }
    }
    let _ = fk;
    Ok(out)
}

fn last_log(st: &mut Value) -> Result<&mut Map<String, Value>, String> {
    ch_state(st)
        .get_mut("log")
        .and_then(|v| v.as_array_mut())
        .and_then(|a| a.last_mut())
        .and_then(|v| v.as_object_mut())
        .ok_or_else(|| "IndexError: log".to_string())
}

// ------------------------------------------------------------------ the UI side: portraits, roster, public view
// py: clan_chronicle.py:589 bond_label
/// What the bonded person `b` is to `a`, as a UI word; "" without a bond.
pub fn bond_label(b: &Value, a: &Value, bt: &str, lg: &str) -> Result<String, String> {
    if bt.is_empty() {
        return Ok(String::new());
    }
    let rank = |p: &Value| -> i64 {
        match vs(p, "r") {
            "kid" => 0,
            "elder" => 2,
            _ => 1,
        }
    };
    let mut key: Option<&str> = match bt {
        "sweet" => Some("partner"),
        "rival" => Some("rival"),
        "kin" => Some("kin"),
        _ => None,
    };
    if bt == "mentor" {
        key = Some(if rank(b) >= rank(a) { "mentor" } else { "apprentice" });
    }
    let Some(key) = key else { return Ok(String::new()) };
    if vs(b, "g") == "f" {
        if let Some(x) = tc("BOND_LABEL_F").get(lg).and_then(|m| m.get(key)).and_then(|x| x.as_str()) {
            return Ok(x.to_string());
        }
    }
    tc("BOND_LABEL").get(lg).and_then(|m| m.get(key)).and_then(|x| x.as_str()).map(|s| s.to_string()).ok_or_else(|| format!("KeyError: {}/{}", lg, key))
}

static HEADS: LazyLock<Mutex<HashMap<String, HashMap<String, (String, i64)>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: clan_chronicle.py:620 _heads
fn heads(members: &[Value], fk: &str) -> HashMap<String, (String, i64)> {
    let key = format!(
        "{}|{}",
        fk,
        members.iter().map(|m| format!("{}\u{1}{}\u{1}{}\u{1}{}", vi(m, "i"), vs(m, "n"), m.get("g").map(pystr).unwrap_or_default(), vs(m, "r"))).collect::<Vec<_>>().join("\u{2}")
    );
    if let Some(h) = HEADS.lock().get(&key) {
        return h.clone();
    }
    let mut out: HashMap<String, (String, i64)> = HashMap::new();
    let mut used: HashMap<String, Vec<i64>> = HashMap::new();
    used.insert("m".into(), vec![]);
    used.insert("f".into(), vec![]);
    let mut sorted: Vec<&Value> = members.iter().collect();
    sorted.sort_by_key(|m| vi(m, "i"));
    for m in sorted {
        let (sex, mut model) = head_model(m, fk, PORTRAIT_MODELS, None, false);
        if pt::enabled() && used[&sex].contains(&model) {
            if let Some(m2) = pt::pick(vs(m, "n"), vs(m, "r"), &sex, fk, &used[&sex]) {
                model = m2;
            }
        }
        used.get_mut(&sex).expect("sex").push(model);
        out.insert(vs(m, "n").to_string(), (sex, model));
    }
    let mut g = HEADS.lock();
    if g.len() > 64 {
        g.clear();
    }
    g.insert(key, out.clone());
    out
}

// py: clan_chronicle.py:641 head_model
/// (sex, model id) of a member's head. `members` = the whole clan (people + dead): no two members wear the same head while the pool has free ones.
pub fn head_model(p: &Value, fk: &str, models: i64, members: Option<&[Value]>, dedup: bool) -> (String, i64) {
    if let Some(ms) = members {
        if !ms.is_empty() && dedup && ms.iter().any(|m| vs(m, "n") == vs(p, "n")) {
            if let Some(x) = heads(ms, fk).get(vs(p, "n")) {
                return x.clone();
            }
        }
    }
    let g = vs(p, "g");
    let sex = if g == "f" {
        "f"
    } else if g == "m" {
        "m"
    } else if hh!("sex", vs(p, "n")) & 1 != 0 {
        "f"
    } else {
        "m"
    };
    let mut model = (hh!("model", vs(p, "n")) as i64) % models.max(1);
    if pt::enabled() {
        if let Some(m2) = pt::pick(vs(p, "n"), vs(p, "r"), sex, fk, &[]) {
            model = m2;
        }
    }
    (sex.to_string(), model)
}

// py: clan_chronicle.py:655 fit_expr
pub fn fit_expr(model: i64, expr: &str) -> String {
    pt::expr_for(model, expr)
}

// py: clan_chronicle.py:664 portrait_id
/// "faction/sex/model/expression" of a survivor's portrait ('W/f/2/sick').
pub fn portrait_id(p: &Value, fk: &str, state: Option<&str>, morale: i64, models: i64, members: Option<&[Value]>) -> String {
    let (sex, model) = head_model(p, fk, models, members, true);
    let st = state.filter(|s| !s.is_empty()).unwrap_or_else(|| p.get("s").and_then(|x| x.as_str()).unwrap_or("h"));
    let expr = if st == "d" {
        "dead"
    } else if st == "i" {
        "hurt"
    } else if st == "h" && morale >= 70 {
        "happy"
    } else if st == "h" && morale <= 25 {
        "scared"
    } else {
        tc("STATE_EXPR").get(st).and_then(|x| x.as_str()).unwrap_or("neutral")
    };
    format!("{}/{}/{}/{}", fk, sex, model, fit_expr(model, expr))
}

// py: clan_chronicle.py:673 _roster_state
fn roster_state(st: &Value, p: &Value) -> String {
    let base = match vs(p, "s") {
        "h" => "healthy",
        "u" => "hungry",
        _ => "sick",
    };
    let code = ext::status::code_of(st, p);
    let name = ext::status::NAMES.get(code as usize).copied().unwrap_or(base);
    if matches!(name, "hungry" | "sick" | "sad" | "injured" | "depressed" | "shock") {
        name.to_string()
    } else {
        base.to_string()
    }
}

// py: clan_chronicle.py:683 _roster_portrait
fn roster_portrait(st: &Value, p: &Value, fk: &str) -> String {
    let mut members: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    members.extend(st.get("dead").and_then(|v| v.as_array()).cloned().unwrap_or_default());
    let pid = portrait_id(p, fk, None, st.get("morale").map(int_of).unwrap_or(50), PORTRAIT_MODELS, Some(&members));
    let code = ext::status::code_of(st, p);
    if code != ext::status::HEALTHY {
        let head = pid.rsplitn(2, '/').nth(1).unwrap_or("").to_string();
        let model = head.rsplit('/').next().and_then(|m| m.parse::<i64>().ok());
        if let (Some(model), Some(ex)) = (model, ext::status::EXPR.get(code as usize)) {
            return format!("{}/{}", head, fit_expr(model, ex));
        }
    }
    pid
}

// py: clan_chronicle.py:703 public
/// Read-only view for the launcher / game clan screen: {"roster", "matches", "inv", "keep"[, "status"]} (`{}` without a story).
pub fn public(c: &Connection, tid: &str, lang: &str, limit: usize) -> Result<Value, String> {
    let Some(mut st) = ext::story::_load(c, tid) else { return Ok(json!({})) };
    let fk = if vi(&st, "fac") == crate::clan::clan::SERAPHITES { "S" } else { "W" };
    let lg = if norm_lang_pl(lang) { "pl" } else { "en" };
    let people: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let mut roster = Vec::new();
    for p in people.iter().take(limit) {
        let ps = persona(p, &people, fk, Some(&mut st));
        let b = people.iter().find(|q| Some(vi(q, "i")) == ps.bond);
        let cur: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
        roster.push(json!({
            "name": p["n"], "role": p["r"], "roleText": lex::role_word(p, fk, lg)?, "icon": tc("ICON").get(vs(p, "r")).and_then(|x| x.as_str()).unwrap_or("star"),
            "state": roster_state(&st, p), "tagline": tagline(p, &cur, fk, lg, Some(&mut st))?,
            "bond": b.map(|b| b["n"].clone()).unwrap_or(json!("")), "bondType": ps.bt.unwrap_or(""),
            "bondLabel": match b { Some(b) => bond_label(b, p, ps.bt.unwrap_or(""), lg)?, None => String::new() }, "portrait": roster_portrait(&st, p, fk)
        }));
    }
    let (inv, keep) = inventory(&mut st, lg)?;
    let mut out = Map::new();
    out.insert("roster".into(), Value::Array(roster));
    out.insert("matches".into(), Value::Array(match_log(&mut st, lg, 20)?));
    out.insert("inv".into(), json!(inv));
    out.insert("keep".into(), json!(keep));
    if let Ok(stat) = ext::status::public(&st, fk, lg) {
        if truthy(&stat) {
            out.insert("status".into(), stat);
        }
    }
    Ok(Value::Object(out))
}
