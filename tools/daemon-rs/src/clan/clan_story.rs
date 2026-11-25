//! py: clan_story.py - the clan as people, and the 2013-style clan feed: roster, morale, the day's diary lines (story_day), storage (clan_story,
//! clan_feed), the read side (read_feed, read_people, journal rows) and the menu ticker (menu_lines: op 0xB4). State, people, messages and rows are
//! `serde_json::Value` objects (insertion order kept: they are dumped with `json.dumps`). The line tables are the generated data
//! (data/clan_tables.json); the logic is ported 1:1, including the order of every random draw.
use crate::clan::clan::{self as cl, int_of, pystr, truthy};
use crate::clan::clan_kids as kd;
use crate::clan::clan_story_pl as pln;
use crate::clan::data;
use crate::clan::ext_clan_story as ext;
use crate::{clock, py};
use regex::Regex;
use rusqlite::{params, Connection};
use serde_json::{Map, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::PyRandom;

pub type Vars = HashMap<String, String>;
pub type Pool = Vec<(&'static str, &'static str)>;

pub const FEED_MAX: i64 = 60;
pub const DAY_MAX_MSGS: usize = 4;
pub const WIDGET_MAX: usize = 118;
pub const STATE_V: i64 = 1;
pub const SLOT_S: i64 = 60;
pub const LIVE_N: usize = 6;
pub const FRESH_S: f64 = 900.0;
pub const WINDOW: usize = 24;
pub const STORY_MIN_CHARS: usize = 150;
pub const DAY_STORIES: usize = 2;
pub const LINE_MAX: usize = 96;

// ------------------------------------------------------------------ tables (generated data)
fn tb(module: &str, name: &str) -> &'static Value {
    data::t(module, name)
}

/// [[scope, template, ...], ...] -> [(scope, template)]
fn pool_of(v: &'static Value) -> Pool {
    v.as_array()
        .map(|a| {
            a.iter()
                .filter_map(|r| {
                    let r = r.as_array()?;
                    Some((r.first()?.as_str()?, r.get(1)?.as_str()?))
                })
                .collect()
        })
        .unwrap_or_default()
}

fn pools_of(v: &'static Value) -> HashMap<&'static str, Pool> {
    v.as_object().map(|o| o.iter().map(|(k, x)| (k.as_str(), pool_of(x))).collect()).unwrap_or_default()
}

/// LINES = LG.LINES + LD.LINES + SPR.LINES (merged in clan_story.py)
static LINES: LazyLock<HashMap<&'static str, Pool>> = LazyLock::new(|| pools_of(tb("clan_story", "LINES")));
static LINES_SICK: LazyLock<HashMap<&'static str, Pool>> = LazyLock::new(|| pools_of(tb("clan_story", "LINES_SICK")));
static SPR_LINES: LazyLock<HashMap<&'static str, Pool>> = LazyLock::new(|| pools_of(tb("clan_story_spores", "LINES")));
static SOLO: LazyLock<HashMap<&'static str, Pool>> = LazyLock::new(|| pools_of(tb("clan_story_solo", "SOLO")));
static DUO_EN: LazyLock<HashMap<&'static str, Pool>> = LazyLock::new(|| pools_of(tb("clan_story_solo", "DUO_EN")));
static TPL_POOL: LazyLock<HashMap<&'static str, &'static str>> = LazyLock::new(|| {
    tb("clan_story_solo", "TPL_POOL").as_object().map(|o| o.iter().filter_map(|(k, v)| Some((k.as_str(), v.as_str()?))).collect()).unwrap_or_default()
});
static KIND_MOOD: LazyLock<HashMap<&'static str, &'static str>> = LazyLock::new(|| {
    tb("clan_story", "KIND_MOOD").as_object().map(|o| o.iter().filter_map(|(k, v)| Some((k.as_str(), v.as_str()?))).collect()).unwrap_or_default()
});
static FEELING_KINDS: LazyLock<HashSet<&'static str>> = LazyLock::new(|| data::set_items(tb("clan_story", "FEELING_KINDS")).iter().filter_map(|v| v.as_str()).collect());
static MINOR_STYLES: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story", "_MINOR_STYLES")));
static STORY_ORDER: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story", "_STORY_ORDER")));

// the dicts of clan_story_data keep their Python insertion order; the generated JSON sorts the keys (gen/export_clan.py: sort_keys), and the
// order is part of the random draws (`list(D.ROLES)`, `list(D.ILLNESS)`): fixed here, checked against the table at start (order_ok)
const ROLE_ORDER: [&str; 10] = ["scavenger", "medic", "cook", "guard", "mechanic", "scout", "gardener", "kid", "elder", "radio"];
const ILLNESS_ORDER: [&str; 8] = ["bronchitis", "infected_wound", "fever", "dysentery", "pneumonia", "broken_leg", "spores", "flu"];

/// The key order constants cover exactly the keys of the generated tables.
pub fn order_ok() -> bool {
    let same = |v: &Value, o: &[&str]| {
        let ks: BTreeSet<&str> = v.as_object().map(|m| m.keys().map(|k| k.as_str()).collect()).unwrap_or_default();
        ks == o.iter().copied().collect::<BTreeSet<_>>()
    };
    same(tb("clan_story_data", "ROLES"), &ROLE_ORDER) && same(tb("clan_story_data", "ILLNESS"), &ILLNESS_ORDER)
}

struct Illness {
    key: &'static str,
    name: &'static str,
    an: &'static str,
    cure: &'static str,
    w_hunger: i64,
    w_fight: i64,
}

static ILLNESS: LazyLock<Vec<Illness>> = LazyLock::new(|| {
    let t = tb("clan_story_data", "ILLNESS");
    ILLNESS_ORDER
        .iter()
        .filter_map(|k| {
            let r = t.get(*k)?.as_array()?;
            Some(Illness {
                key: k,
                name: r.first()?.as_str()?,
                an: r.get(1)?.as_str()?,
                cure: r.get(2)?.as_str()?,
                w_hunger: r.get(3)?.as_i64()?,
                w_fight: r.get(4)?.as_i64()?,
            })
        })
        .collect()
});

fn illness(key: &str) -> Option<&'static Illness> {
    ILLNESS.iter().find(|i| i.key == key)
}

struct Role {
    key: &'static str,
    wlf: &'static str,
    sera: &'static str,
    weight: i64,
}

static ROLES: LazyLock<Vec<Role>> = LazyLock::new(|| {
    let t = tb("clan_story_data", "ROLES");
    ROLE_ORDER
        .iter()
        .filter_map(|k| {
            let r = t.get(*k)?.as_array()?;
            Some(Role { key: k, wlf: r.first()?.as_str()?, sera: r.get(1)?.as_str()?, weight: r.get(2)?.as_i64()? })
        })
        .collect()
});

struct Faction {
    plural: &'static str,
    enemy: &'static str,
    enemies: &'static str,
    leader: &'static str,
    places: Vec<&'static str>,
}

fn faction_data(fk: &str) -> Option<Faction> {
    let f = tb("clan_story_data", "FACTION").get(fk)?;
    Some(Faction {
        plural: f.get("plural")?.as_str()?,
        enemy: f.get("enemy")?.as_str()?,
        enemies: f.get("enemies")?.as_str()?,
        leader: f.get("leader")?.as_str()?,
        places: data::strs(f.get("places")?),
    })
}

static FACTION_W: LazyLock<Option<Faction>> = LazyLock::new(|| faction_data("W"));
static FACTION_S: LazyLock<Option<Faction>> = LazyLock::new(|| faction_data("S"));

fn faction(fk: &str) -> Option<&'static Faction> {
    if fk == "S" {
        FACTION_S.as_ref()
    } else if fk == "W" {
        FACTION_W.as_ref()
    } else {
        None
    }
}

static INFECTED: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story_data", "INFECTED")));
static FIRST_2013: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story_data", "FIRST_2013")));
static LAST_2013: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story_data", "LAST_2013")));
static FEMALE_2013: LazyLock<HashSet<&'static str>> = LazyLock::new(|| data::set_items(tb("clan_story_data", "FEMALE_2013")).iter().filter_map(|v| v.as_str()).collect());
static NEUTRAL_2013: LazyLock<HashSet<&'static str>> = LazyLock::new(|| data::set_items(tb("clan_story_data", "NEUTRAL_2013")).iter().filter_map(|v| v.as_str()).collect());

fn name_gender_list(name: &str) -> Vec<(&'static str, &'static str)> {
    tb("clan_story_data", name)
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|r| {
                    let r = r.as_array()?;
                    Some((r.first()?.as_str()?, r.get(1)?.as_str()?))
                })
                .collect()
        })
        .unwrap_or_default()
}

static EXTRA_WLF: LazyLock<Vec<(&'static str, &'static str)>> = LazyLock::new(|| name_gender_list("EXTRA_WLF"));
static EXTRA_SERA: LazyLock<Vec<(&'static str, &'static str)>> = LazyLock::new(|| name_gender_list("EXTRA_SERA"));

static TICKER_HUNGRY: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story_data", "TICKER_HUNGRY")));
static TICKER_SICK: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story_data", "TICKER_SICK")));
static TICKER_HEALTHY: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story_data", "TICKER_HEALTHY")));
static TICKER_NOT_SERA: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story_data", "TICKER_NOT_SERA")));
static TICKER_RARE_SICK: LazyLock<&'static str> = LazyLock::new(|| tb("clan_story_data", "TICKER_RARE_SICK").as_str().unwrap_or(""));
fn ticker_rare_chance() -> f64 {
    tb("clan_story_data", "TICKER_RARE_CHANCE").as_f64().unwrap_or(0.015)
}

/// clan_story_good_rich.ROWS: (scope, English template, Polish template)
static RG_ROWS: LazyLock<Vec<(&'static str, &'static str, &'static str)>> = LazyLock::new(|| {
    tb("clan_story_good_rich", "ROWS")
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|r| {
                    let r = r.as_array()?;
                    Some((r.first()?.as_str()?, r.get(1)?.as_str()?, r.get(2)?.as_str()?))
                })
                .collect()
        })
        .unwrap_or_default()
});

// py: clan_story.py:1424 _RICH_ROWS
static RICH_ROWS: LazyLock<Vec<(&'static str, &'static str, BTreeSet<char>)>> =
    LazyLock::new(|| RG_ROWS.iter().map(|(sc, en, _pl)| (*sc, *en, pln::tpl_letters(en))).collect());

fn regex_of(module: &str, name: &str) -> Regex {
    let v = tb(module, name);
    let (pat, flags) = data::regex_src(v).unwrap_or(("$^", 0));
    let pre = if flags & 2 != 0 { "(?i)" } else { "" };
    Regex::new(&format!("{}{}", pre, pat)).unwrap_or_else(|_| Regex::new("$^").expect("never"))
}

// py: clan_story_solo.py:22 CROWD_RE / GROUP_RE
static CROWD_RE: LazyLock<Regex> = LazyLock::new(|| regex_of("clan_story_solo", "CROWD_RE"));
static GROUP_RE: LazyLock<Regex> = LazyLock::new(|| regex_of("clan_story_solo", "GROUP_RE"));
// py: clan_story.py:1281 _PL_GROUP_RE  (?<!\w)(...)(?!\w): every alternative starts and ends with a word character, so \b does the same
static PL_GROUP_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?i)\b(?:my|nas|nam|nami|nasz\w*|nikt\w*|wszyscy|wszystkich|\w+(?:śmy|emy|imy|amy|ymy))\b").expect("PL_GROUP_RE"));
static KEY_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{(\w+)\}").expect("KEY_RE"));
static HEAD_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^(?:DAY|DZIEŃ) \d+: [^.]*\.\s*").expect("HEAD_RE"));
static HEAD_NUM_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^(?:DAY|DZIEŃ) \d+: ").expect("HEAD_NUM_RE"));
static SHORT_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"[^.!?]*[.!?]+(?:\s+|$)|[^.!?]+$").expect("SHORT_RE"));
static LINE_HEAD_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"^\s*(?:DAY|DZIEŃ)\s+\d+:\s*([^a-ząćęłńóśźż]+?)[.!?]\s+(\S)").expect("LINE_HEAD_RE"));

// ------------------------------------------------------------------ Value helpers (Python dict / int / str access)
fn vget<'a>(v: &'a Value, k: &str) -> &'a Value {
    static NULL: Value = Value::Null;
    v.get(k).unwrap_or(&NULL)
}

/// `str` field or "".
fn vs<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("")
}

/// int field (Python int()) or 0.
fn vi(v: &Value, k: &str) -> i64 {
    v.get(k).map(int_of).unwrap_or(0)
}

fn jstr(s: &str) -> Value {
    Value::String(s.to_string())
}

fn people_of(st: &Value) -> &[Value] {
    st.get("people").and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

fn people_mut(st: &mut Value) -> &mut Vec<Value> {
    if !st.get("people").map_or(false, |v| v.is_array()) {
        st["people"] = Value::Array(Vec::new());
    }
    match st.get_mut("people") {
        Some(Value::Array(a)) => a,
        _ => unreachable!(),
    }
}

fn dead_of(st: &Value) -> &[Value] {
    st.get("dead").and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

fn dead_mut(st: &mut Value) -> &mut Vec<Value> {
    if !st.get("dead").map_or(false, |v| v.is_array()) {
        st["dead"] = Value::Array(Vec::new());
    }
    match st.get_mut("dead") {
        Some(Value::Array(a)) => a,
        _ => unreachable!(),
    }
}

/// `(st.get("ch") or {}).get(key) or default-ish` : the chronicle's sub-dict `st["ch"][key]` (Null when missing / falsy).
fn ch_get<'a>(st: &'a Value, key: &str) -> &'a Value {
    static NULL: Value = Value::Null;
    match st.get("ch") {
        Some(ch) if truthy(ch) => match ch.get(key) {
            Some(x) if truthy(x) => x,
            _ => &NULL,
        },
        _ => &NULL,
    }
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// `re.search(r"(?<!\w)" + re.escape(name) + r"(?!\w)", s)`
fn has_word(s: &str, name: &str) -> bool {
    let mut idx: Vec<usize> = s.char_indices().map(|(i, _)| i).collect();
    idx.push(s.len());
    for i in idx {
        if s[i..].starts_with(name) {
            let before = s[..i].chars().next_back().map(is_word).unwrap_or(false);
            let after = s[i + name.len()..].chars().next().map(is_word).unwrap_or(false);
            if !before && !after {
                return true;
            }
        }
    }
    false
}

/// `re.split(r"(?<=[.!?])\s+", text)`
fn split_after_punct(text: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut prev: Option<char> = None;
    let mut it = text.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        if py::is_py_space(c) && matches!(prev, Some('.' | '!' | '?')) {
            let mut end = i + c.len_utf8();
            while let Some(&(j, d)) = it.peek() {
                if py::is_py_space(d) {
                    end = j + d.len_utf8();
                    it.next();
                } else {
                    break;
                }
            }
            out.push(&text[start..i]);
            start = end;
            prev = Some(' ');
            continue;
        }
        prev = Some(c);
    }
    out.push(&text[start..]);
    out
}

fn clen(s: &str) -> usize {
    s.chars().count()
}

/// Python `s[:n]`
fn head_chars(s: &str, n: usize) -> &str {
    py::cut(s, n)
}

fn cap_first(s: &str) -> String {
    pln::cap_first(s)
}

fn starts_lower(s: &str) -> bool {
    s.chars().next().map_or(false, |c| c.is_lowercase())
}

// ------------------------------------------------------------------ text
// py: clan_story.py:84 _numword
fn numword(n: i64, words: &[&str], people_fmt: bool) -> String {
    if n >= 0 && (n as usize) < words.len() {
        return words[n as usize].to_string();
    }
    if people_fmt {
        format!("{} people", n)
    } else {
        n.to_string()
    }
}

static NUMS: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story", "NUMS")));
static NUMW: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story", "NUMW")));
static GAINW: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("clan_story", "GAINW")));

// py: clan_story.py:70 PRON
/// PRON[g][key]; key in he him his hes himself is has was title.
pub fn pron(g: &str, key: &str) -> Option<&'static str> {
    tb("clan_story", "PRON").get(g)?.get(key)?.as_str()
}

const PRON_KEYS: [&str; 10] = ["he", "He", "him", "his", "hes", "himself", "is", "has", "was", "title"];

// py: clan_story.py:88 line_keys
pub fn line_keys(tpl: &str) -> BTreeSet<String> {
    KEY_RE.captures_iter(tpl).filter_map(|m| m.get(1).map(|g| g.as_str().to_string())).collect()
}

/// PERSON_RE `^([ABCD])(?:_(\w+))?$` on a KEY_RE key: (letter, suffix).
fn person_key(key: &str) -> Option<(&str, Option<&str>)> {
    let mut it = key.chars();
    let c = it.next()?;
    if !matches!(c, 'A'..='D') {
        return None;
    }
    let rest = it.as_str();
    if rest.is_empty() {
        return Some((&key[..1], None));
    }
    let w = rest.strip_prefix('_')?;
    if w.is_empty() {
        return None;
    }
    Some((&key[..1], Some(w)))
}

// py: clan_story.py:92 render
/// Fill a template. people = {"A": person, ...}; {A} = name, {A_he} / {A_He} ... = pronouns and verbs by gender; vars_ = the plain keys.
/// None = Python raised (KeyError).
pub fn render(tpl: &str, people: &Value, vars_: &Vars) -> Option<String> {
    let mut out = String::with_capacity(tpl.len() + 16);
    let mut last = 0;
    for m in KEY_RE.captures_iter(tpl) {
        let all = m.get(0)?;
        out.push_str(&tpl[last..all.start()]);
        let key = m.get(1)?.as_str();
        if let Some((letter, suffix)) = person_key(key) {
            let p = people.get(letter)?;
            match suffix {
                None => out.push_str(&pystr(p.get("n")?)),
                Some(w) => {
                    let g = p.get("g")?.as_str()?;
                    let r = pron(g, &w.to_lowercase())?;
                    if w.chars().next().map_or(false, |c| c.is_uppercase()) {
                        out.push_str(&cap_first(r));
                    } else {
                        out.push_str(r);
                    }
                }
            }
        } else {
            let val = vars_.get(key)?;
            let before = tpl[..all.start()].trim_end_matches(py::is_py_space);
            let at_start = before.is_empty() || matches!(before.chars().next_back(), Some('.' | '!' | '?' | '"' | '\''));
            if starts_lower(val) && at_start {
                out.push_str(&cap_first(val));
            } else {
                out.push_str(val);
            }
        }
        last = all.end();
    }
    out.push_str(&tpl[last..]);
    Some(out)
}

// py: clan_story.py:113 short_text
/// The in-game widget shows one line: whole sentences up to `limit` chars (the first sentence cut at a clause when it is longer).
pub fn short_text(text: &str, limit: usize) -> String {
    if clen(text) <= limit {
        return text.to_string();
    }
    let mut out = String::new();
    for m in SHORT_RE.find_iter(text) {
        let p = m.as_str();
        if clen(&out) + clen(p.trim_end_matches(py::is_py_space)) <= limit {
            out.push_str(p);
        } else {
            break;
        }
    }
    let out = out.trim_matches(py::is_py_space);
    if !out.is_empty() {
        return out.to_string();
    }
    let cut: Vec<char> = text.chars().take(limit.saturating_sub(1)).collect();
    let rf = |c: char| cut.iter().rposition(|x| *x == c).map(|i| i as i64).unwrap_or(-1);
    let k = rf(',').max(rf(';')).max(rf(' '));
    let base: String = if k > 40 { cut[..k as usize].iter().collect() } else { cut.iter().collect() };
    format!("{}…", base.trim_end_matches([' ', ',', ';']))
}

// py: clan_story.py:132 line_id
pub fn line_id(text: &str) -> String {
    format!("{:08x}", t2r_clanstory::crc::crc32(text.as_bytes()))
}

// ------------------------------------------------------------------ people
// py: clan_story.py:137 _fkey
pub fn _fkey(faction: i64) -> &'static str {
    if faction == cl::SERAPHITES {
        "S"
    } else {
        "W"
    }
}

fn fkey_of(st: &Value) -> &'static str {
    _fkey(vi(st, "fac"))
}

// py: clan_story.py:141 _name_pool (name, gender, weight)
fn name_pool(fk: &str) -> (Vec<(&'static str, &'static str)>, Vec<f64>) {
    let mut pool: Vec<(&'static str, &'static str)> = Vec::new();
    let mut w: Vec<f64> = Vec::new();
    for n in FIRST_2013.iter() {
        let g = if FEMALE_2013.contains(n) { "f" } else { "m" };
        pool.push((n, g));
        w.push(if fk == "W" { 1.0 } else { 0.3 });
    }
    for (n, g) in if fk == "W" { EXTRA_WLF.iter() } else { EXTRA_SERA.iter() } {
        pool.push((n, g));
        w.push(if fk == "W" { 1.0 } else { 1.6 });
    }
    for (n, g) in if fk == "W" { EXTRA_SERA.iter() } else { EXTRA_WLF.iter() } {
        pool.push((n, g));
        w.push(0.1);
    }
    (pool, w)
}

static POOL_W: LazyLock<(Vec<(&'static str, &'static str)>, Vec<f64>)> = LazyLock::new(|| name_pool("W"));
static POOL_S: LazyLock<(Vec<(&'static str, &'static str)>, Vec<f64>)> = LazyLock::new(|| name_pool("S"));

fn rchoice_idx(rng: &mut PyRandom, n: usize) -> usize {
    rng.choice_index(n).unwrap_or(0)
}

fn rchoice<'a, T>(rng: &mut PyRandom, v: &'a [T]) -> Option<&'a T> {
    if v.is_empty() {
        return None;
    }
    let i = rchoice_idx(rng, v.len());
    v.get(i)
}

// py: clan_story.py:156 new_person
pub fn new_person(rng: &mut PyRandom, st: &mut Value, role: Option<&str>, state: &str, join_day: i64) -> Value {
    let fk = fkey_of(st);
    let (pool, weights) = if fk == "W" { &*POOL_W } else { &*POOL_S };
    let mut taken: HashSet<String> = people_of(st).iter().map(|p| vs(p, "n").to_string()).collect();
    taken.extend(dead_of(st).iter().map(|d| vs(d, "n").to_string()));
    let mut nm = String::new();
    let mut g = "m";
    let mut found = false;
    for _ in 0..40 {
        let i = rng.choices_weighted(weights, 1).ok().and_then(|v| v.first().copied()).unwrap_or(0);
        let (n0, g0) = pool[i];
        nm = n0.to_string();
        g = g0;
        if !taken.contains(&nm) {
            found = true;
            break;
        }
    }
    if !found {
        let l = *rchoice(rng, &LAST_2013).unwrap_or(&"");
        nm = format!("{} {}", nm, l);
    }
    let mut g = g.to_string();
    let first = nm.split_whitespace().next().unwrap_or("");
    if NEUTRAL_2013.contains(first) && rng.random() < 0.3 {
        g = "n".to_string();
    }
    let role_s: String = match role {
        Some(r) => r.to_string(),
        None => {
            let w: Vec<f64> = ROLES.iter().map(|r| r.weight as f64).collect();
            let i = rng.choices_weighted(&w, 1).ok().and_then(|v| v.first().copied()).unwrap_or(0);
            ROLES.get(i).map(|r| r.key).unwrap_or("scavenger").to_string()
        }
    };
    let pid = vi(st, "pid") + 1;
    st["pid"] = Value::from(pid);
    let l = *rchoice(rng, &LAST_2013).unwrap_or(&"");
    let mut p = Map::new();
    p.insert("i".into(), Value::from(pid));
    p.insert("n".into(), Value::String(nm));
    p.insert("l".into(), jstr(l));
    p.insert("g".into(), Value::String(g));
    p.insert("r".into(), Value::String(role_s));
    p.insert("s".into(), jstr(state));
    p.insert("il".into(), jstr(""));
    p.insert("is".into(), Value::from(0));
    p.insert("id".into(), Value::from(0));
    p.insert("k".into(), Value::from(0));
    p.insert("jd".into(), Value::from(join_day));
    p.insert("gr".into(), Value::from(0));
    p.insert("gl".into(), Value::from(0));
    Value::Object(p)
}

// py: clan_story.py:176 _link_kin
/// `p` is not in the roster yet; the kin partner is a roster member (changed in place).
fn _link_kin(rng: &mut PyRandom, st: &mut Value, p: &mut Value) {
    if rng.random() < 0.35 {
        let pid = vi(p, "i");
        let free: Vec<i64> = people_of(st).iter().filter(|q| !truthy(vget(q, "k")) && vi(q, "i") != pid).map(|q| vi(q, "i")).collect();
        if !free.is_empty() {
            let qi = free[rchoice_idx(rng, free.len())];
            p["k"] = Value::from(qi);
            if let Some(q) = person_mut(st, qi) {
                q["k"] = Value::from(pid);
            }
        }
    }
}

fn person_mut(st: &mut Value, id: i64) -> Option<&mut Value> {
    people_mut(st).iter_mut().find(|p| vi(p, "i") == id)
}

// py: clan_story.py:188 _by_id
fn _by_id(st: &Value, i: i64) -> Option<Value> {
    people_of(st).iter().find(|p| vi(p, "i") == i).cloned()
}

// py: clan_story.py:195 mood_of
pub fn mood_of(st: &Value, p: &Value) -> &'static str {
    match vs(p, "s") {
        "s" => return "ill",
        "u" => return "hungry",
        _ => {}
    }
    if vi(p, "gr") > vi(st, "day") {
        return "grieving";
    }
    let m = vi(st, "morale");
    if m >= 70 {
        "cheerful"
    } else if m >= 50 {
        "steady"
    } else if m >= 30 {
        "tired"
    } else {
        "hollow"
    }
}

// py: clan_story.py:206 morale_label
pub fn morale_label(m: i64) -> &'static str {
    if m >= 70 {
        "hopeful"
    } else if m >= 50 {
        "steady"
    } else if m >= 30 {
        "strained"
    } else {
        "grim"
    }
}

// ------------------------------------------------------------------ state
// py: clan_story.py:211 new_state
pub fn new_state(tid: &str, faction: i64, run: i64) -> Value {
    let mut m = Map::new();
    m.insert("v".into(), Value::from(STATE_V));
    m.insert("fac".into(), Value::from(faction));
    m.insert("run".into(), Value::from(run));
    m.insert("seed".into(), Value::String(format!("{}:{}", tid, run)));
    m.insert("day".into(), Value::from(0));
    m.insert("morale".into(), Value::from(55));
    m.insert("ds".into(), Value::from(0));
    m.insert("ss".into(), Value::from(0));
    m.insert("pid".into(), Value::from(0));
    m.insert("n".into(), Value::from(0));
    m.insert("people".into(), Value::Array(Vec::new()));
    m.insert("dead".into(), Value::Array(Vec::new()));
    m.insert("used".into(), Value::Object(Map::new()));
    m.insert("atk".into(), Value::from(-99));
    m.insert("warned".into(), Value::from(-1));
    m.insert("suic".into(), Value::from(-99));
    m.insert("boot".into(), Value::from(0));
    Value::Object(m)
}

// py: clan_story.py:216 _populate
fn _populate(rng: &mut PyRandom, st: &mut Value, h: i64, u: i64, s: i64, day: i64) {
    for (state, n) in [("h", h), ("u", u), ("s", s)] {
        for _ in 0..n.max(0) {
            let mut p = new_person(rng, st, None, state, day);
            if state == "s" {
                let il = _pick_illness(rng, st, true, Some(&p));
                p["il"] = jstr(il);
                p["id"] = Value::from(day);
            }
            _link_kin(rng, st, &mut p);
            people_mut(st).push(p);
        }
    }
}

// py: clan_story.py:227 _pick_illness
fn _pick_illness(rng: &mut PyRandom, st: &Value, _hunger: bool, p: Option<&Value>) -> &'static str {
    let after_fight = vi(st, "day") - vi(st, "atk") <= 3;
    let mut keys: Vec<&'static Illness> = ILLNESS.iter().collect();
    if let Some(p) = p {
        if kd::is_kid(p) {
            keys.retain(|k| k.key != "infected_wound" && k.key != "spores");
        }
    }
    let mut w: Vec<f64> = keys.iter().map(|k| (if after_fight { k.w_fight } else { k.w_hunger }) as f64).collect();
    if let Some(i) = keys.iter().position(|k| k.key == "spores") {
        if has_mask(st) {
            w[i] = ((w[i] as i64 / 4).max(1)) as f64;
        }
    }
    let i = rng.choices_weighted(&w, 1).ok().and_then(|v| v.first().copied()).unwrap_or(0);
    keys.get(i).map(|k| k.key).unwrap_or("flu")
}

// py: clan_story.py:238 has_mask
/// The clan found a gas mask in one of its last raids (stored in the chronicle log).
pub fn has_mask(st: &Value) -> bool {
    let log: Vec<&Value> = match st.get("ch") {
        Some(ch) if truthy(ch) => match ch.get("log") {
            Some(Value::Array(a)) => a.iter().collect(),
            _ => Vec::new(),
        },
        _ => Vec::new(),
    };
    let from = log.len().saturating_sub(12);
    for e in &log[from..] {
        if let Some(Value::Array(fs)) = e.get("finds") {
            if fs.iter().any(|f| f.get("k").and_then(|k| k.as_str()) == Some("gasmask")) {
                return true;
            }
        }
    }
    false
}

// ------------------------------------------------------------------ line picking
static PL_BY_EN: LazyLock<HashMap<&'static str, &'static str>> = LazyLock::new(|| {
    let mut m = HashMap::new();
    for (_pool, _sc, e, p) in pln::ROWS.iter() {
        m.entry(*e).or_insert(*p);
    }
    m
});

// py: clan_story.py:251 _pl_twin
/// The Polish twin of an English template of the registry ('' when it has none), for the kid check.
fn _pl_twin(t: &str) -> &'static str {
    PL_BY_EN.get(t).copied().unwrap_or("")
}

fn kid_plain(state: &str) -> (&'static str, &'static str) {
    let v = tb("clan_story", "_KID_PLAIN").get(state);
    let a = v.and_then(|x| x.get(0)).and_then(|x| x.as_str()).unwrap_or("{A} is healthy.");
    let b = v.and_then(|x| x.get(1)).and_then(|x| x.as_str()).unwrap_or("t:h");
    (a, b)
}

fn combat_en_pl(t: &str) -> bool {
    kd::combat_text(t, Some("en")) || kd::combat_text(_pl_twin(t), Some("pl"))
}

// py: clan_story.py:259 _companion_ok
/// True when template t (English) can be filled with companions for subject p: a B / C needs that many healthy others, and a combat line needs them grown.
fn _companion_ok(st: &Value, p: &Value, t: &str) -> bool {
    let letters = pln::tpl_letters(t);
    if letters.contains(&'D') {
        if let Some(last) = dead_of(st).last() {
            if vs(last, "r") == "kid" && combat_en_pl(t) {
                return false;
            }
        }
    }
    let need = letters.iter().filter(|c| matches!(c, 'B' | 'C')).count();
    if need == 0 {
        return true;
    }
    let pid = vi(p, "i");
    let mut others: Vec<&Value> = people_of(st).iter().filter(|q| vi(q, "i") != pid && vs(q, "s") == "h").collect();
    if combat_en_pl(t) {
        others.retain(|q| kd::can_fight(q));
    }
    others.len() >= need
}

/// The symbolic variables the templates of a pool may use (`avail` of _pick).
fn avail_has(people: &Value, vars_: &Vars, key: &str) -> bool {
    if vars_.contains_key(key) {
        return true;
    }
    let Some(o) = people.as_object() else { return false };
    if o.contains_key(key) {
        return true;
    }
    match key.split_once('_') {
        Some((l, s)) => o.contains_key(l) && PRON_KEYS.contains(&s),
        None => false,
    }
}

// py: clan_story.py:273 pick_line
/// One unused line of `kind` for faction key fk whose placeholders can all be filled; the used list of the kind resets when it is exhausted.
pub fn pick_line(rng: &mut PyRandom, st: &mut Value, kind: &str, fk: &str, people: &Value, vars_: &Vars, pool: Option<&[(&'static str, &'static str)]>) -> Option<String> {
    _pick(rng, st, kind, fk, people, vars_, pool).map(|r| r.0)
}

// py: clan_story.py:280 _pick
/// pick_line's work: (rendered text, the English template chosen) or None.
fn _pick(
    rng: &mut PyRandom,
    st: &mut Value,
    kind: &str,
    fk: &str,
    people: &Value,
    vars_: &Vars,
    pool: Option<&[(&'static str, &'static str)]>,
) -> Option<(String, &'static str)> {
    let empty: Pool = Vec::new();
    let lines: &[(&'static str, &'static str)] = match pool {
        Some(p) => p,
        None => LINES.get(kind).unwrap_or(&empty),
    };
    let mut cand: Vec<&'static str> = Vec::new();
    for (scope, t) in lines {
        if *scope != "*" && *scope != fk {
            continue;
        }
        if line_keys(t).iter().all(|k| avail_has(people, vars_, k)) && kd::kid_safe(t, people, &[_pl_twin(t)]) {
            cand.push(t);
        }
    }
    if !cand.is_empty() && kd::any_kid(people) {
        cand.retain(|t| render(t, people, vars_).map_or(false, |s| kd::kid_text_ok(&s, people, "en")));
    }
    if cand.is_empty() {
        return None;
    }
    let mut used = used_list(st, kind);
    let mut fresh: Vec<&'static str> = cand.iter().copied().filter(|t| !used.contains(&line_id(t))).collect();
    if fresh.is_empty() {
        let keep: Vec<String> = used.last().cloned().into_iter().collect();
        used = keep;
        fresh = cand.iter().copied().filter(|t| !used.contains(&line_id(t))).collect();
        if fresh.is_empty() {
            fresh = cand.clone();
        }
    }
    let t = fresh[rchoice_idx(rng, fresh.len())];
    used.push(line_id(t));
    trim_front(&mut used, 400);
    set_used_list(st, kind, &used);
    Some((render(t, people, vars_)?, t))
}

/// `st["used"].setdefault(key, [])` read as the list of ids.
fn used_list(st: &mut Value, key: &str) -> Vec<String> {
    if !st.get("used").map_or(false, |u| u.is_object()) {
        st["used"] = Value::Object(Map::new());
    }
    let u = st["used"].as_object_mut().expect("used object");
    if !u.contains_key(key) {
        u.insert(key.to_string(), Value::Array(Vec::new()));
    }
    u.get(key).and_then(|v| v.as_array()).map(|a| a.iter().map(pystr).collect()).unwrap_or_default()
}

fn set_used_list(st: &mut Value, key: &str, used: &[String]) {
    st["used"][key] = Value::Array(used.iter().map(|s| jstr(s)).collect());
}

/// `del used[:-n]`
fn trim_front(v: &mut Vec<String>, n: usize) {
    if v.len() > n {
        v.drain(..v.len() - n);
    }
}

// ------------------------------------------------------------------ the narrator's voice
// py: clan_story_solo.py:480 solo_pool
/// The one-survivor pool of a message kind, or None when the kind has no solo voice (it is not written).
pub fn solo_pool(kind: &str, cause: Option<&str>) -> Option<Pool> {
    if kind == "grief" {
        let mut p = SOLO.get("grief").cloned().unwrap_or_default();
        let cg = tb("clan_story_solo", "CAUSE_GROUP").get(cause.unwrap_or("")).and_then(|v| v.as_str()).unwrap_or("");
        if let Some(x) = SOLO.get(format!("grief:{}", cg).as_str()) {
            p.extend(x.iter().copied());
        }
        return Some(p);
    }
    let kind = if kind == "death_bite_kin" { "death_bite" } else { kind };
    SOLO.get(kind).cloned()
}

// py: clan_story_solo.py:489 solo_ticker
pub fn solo_ticker(state: &str) -> &'static Pool {
    let key = match state {
        "s" => "t_s",
        "u" => "t_u",
        _ => "t_h",
    };
    SOLO.get(key).unwrap_or_else(|| SOLO.get("t_h").expect("t_h"))
}

// py: clan_story_solo.py:493 voice_of_lid
/// "solo" / "duo" / "group" from a stored template id ("<pool>/<crc>"); no id (an old row) = group.
pub fn voice_of_lid(lid: Option<&str>) -> &'static str {
    let pool = lid.unwrap_or("").split('/').next().unwrap_or("");
    if pool.starts_with("solo:") || pool.starts_with("t:solo_") {
        "solo"
    } else if pool.starts_with("duo:") {
        "duo"
    } else {
        "group"
    }
}

#[derive(Default)]
struct Opts<'a> {
    mood: Option<&'a str>,
    pool: Option<&'a [(&'static str, &'static str)]>,
    ill_of: Option<&'a str>,
    pname: Option<&'a str>,
    cause: Option<&'a str>,
    ill: Option<String>,
    n: Option<i64>,
    lost: Option<i64>,
    gain: Option<i64>,
    ago: Option<&'a str>,
}

// py: clan_story.py:308 Writer
/// Collects one clan day's messages.
struct Writer {
    day: i64,
    week: i64,
    fk: &'static str,
    out: Vec<Value>,
}

impl Writer {
    fn new(st: &Value, day: i64, week: i64) -> Writer {
        Writer { day, week, fk: fkey_of(st), out: Vec::new() }
    }

    // py: clan_story.py:316 Writer.base_vars
    fn base_vars(&self, rng: &mut PyRandom) -> Vars {
        let f = faction(self.fk).expect("faction");
        let mut v = Vars::new();
        v.insert("day".into(), self.day.to_string());
        v.insert("week".into(), self.week.to_string());
        let place = *rchoice(rng, &f.places).unwrap_or(&"");
        v.insert("place".into(), place.to_string());
        v.insert("enemy".into(), f.enemy.to_string());
        v.insert("enemies".into(), f.enemies.to_string());
        v.insert("leader".into(), f.leader.to_string());
        v.insert("faction".into(), f.plural.to_string());
        let inf = *rchoice(rng, &INFECTED).unwrap_or(&"");
        v.insert("inf".into(), inf.to_string());
        v.insert("infs".into(), format!("{}s", inf));
        v
    }

    // py: clan_story.py:324 Writer.add
    fn add(&mut self, rng: &mut PyRandom, st: &mut Value, kind: &str, people: &Value, o: &Opts) -> bool {
        let size = people_of(st).len();
        let mut used_key = kind.to_string();
        let mut duo_pool: Option<&Pool> = None;
        let mut people_v: Value = people.clone();
        let mut pool_v: Option<Pool> = o.pool.map(|p| p.to_vec());
        let mut ill_of = o.ill_of;
        if size == 1 && kind != "clan_dead" {
            let sp = match solo_pool(kind, o.cause) {
                Some(sp) if !sp.is_empty() => sp,
                _ => return false,
            };
            let surv = people_of(st)[0].clone();
            let mut np = Map::new();
            np.insert("A".into(), surv);
            if kind.starts_with("death_") {
                np.insert("D".into(), people.get("A").cloned().unwrap_or(Value::Null));
            } else if let Some(d) = people.get("D") {
                np.insert("D".into(), d.clone());
            }
            people_v = Value::Object(np);
            pool_v = Some(sp);
            used_key = format!("solo:{}", kind);
            ill_of = if ill_of.is_some() { Some("A") } else { None };
        } else if size == 2 && kind != "clan_dead" {
            let base: Pool = match &pool_v {
                Some(p) => p.clone(),
                None => LINES.get(kind).cloned().unwrap_or_default(),
            };
            let filt: Pool = base.iter().copied().filter(|(_, t)| !CROWD_RE.is_match(t)).collect();
            pool_v = Some(if filt.is_empty() { base } else { filt });
            let duo = DUO_EN.get(kind).filter(|d| !d.is_empty());
            if let (Some(duo), Some(a)) = (duo, people.get("A")) {
                if a.get("i").map_or(false, |i| !i.is_null()) {
                    let ai = vi(a, "i");
                    let others: Vec<Value> = people_of(st).iter().filter(|q| vi(q, "i") != ai).cloned().collect();
                    if let Some(first) = others.first() {
                        people_v["B"] = first.clone();
                        if rng.random() < 0.6 {
                            duo_pool = Some(duo);
                        }
                    }
                }
            }
        }
        let mut v = self.base_vars(rng);
        let mut sym = Map::new();
        sym.insert("f".into(), jstr(self.fk));
        sym.insert("day".into(), Value::from(self.day));
        sym.insert("week".into(), Value::from(self.week));
        sym.insert("place".into(), jstr(v.get("place").map(|s| s.as_str()).unwrap_or("")));
        sym.insert("inf".into(), jstr(v.get("inf").map(|s| s.as_str()).unwrap_or("")));
        let p = ill_of.and_then(|k| people_v.get(k));
        let mut ill = o.ill.clone();
        if ill.is_none() {
            if let Some(p) = p {
                if truthy(p) && truthy(vget(p, "il")) {
                    ill = Some(pystr(vget(p, "il")));
                }
            }
        }
        if let Some(il) = &ill {
            sym.insert("ill".into(), jstr(il));
        }
        for (k, x) in [("n", o.n), ("lost", o.lost), ("gain", o.gain)] {
            if let Some(x) = x {
                sym.insert(k.into(), Value::from(x));
            }
        }
        if let Some(a) = o.ago {
            sym.insert("ago".into(), jstr(a));
        }
        let sym = Value::Object(sym);
        if let Some(e) = expand_en(&sym) {
            v.extend(e);
        }
        let mut got = None;
        if let Some(dp) = duo_pool {
            got = _pick(rng, st, &format!("duo:{}", kind), self.fk, &people_v, &v, Some(dp.as_slice()));
        }
        if got.is_none() {
            got = _pick(rng, st, &used_key, self.fk, &people_v, &v, pool_v.as_deref());
        }
        let Some((text, tpl)) = got else { return false };
        let mut names = Vec::new();
        for k in ["A", "B", "C", "D"] {
            if let Some(p) = people_v.get(k) {
                if has_word(&text, vs(p, "n")) {
                    names.push(jstr(vs(p, "n")));
                }
            }
        }
        let pool_name = TPL_POOL.get(tpl).copied().or(o.pname).unwrap_or(kind);
        let mut m = Map::new();
        m.insert("kind".into(), jstr(kind));
        let mood = o.mood.filter(|m| !m.is_empty()).unwrap_or_else(|| KIND_MOOD.get(kind).copied().unwrap_or("calm"));
        m.insert("mood".into(), jstr(mood));
        m.insert("text".into(), Value::String(text));
        m.insert("names".into(), Value::Array(names));
        m.insert("lid".into(), Value::String(pln::lid(pool_name, tpl)));
        m.insert("params".into(), _params(&people_v, &sym));
        self.out.push(Value::Object(m));
        true
    }
}

// py: clan_story.py:381 expand_en
/// Symbolic variables -> the English words the templates use (the Polish twin is clan_story_pl.expand).
pub fn expand_en(sym: &Value) -> Option<Vars> {
    let fk = match sym.get("f") {
        None => "W",
        Some(Value::String(s)) => s.as_str(),
        Some(_) => return None,
    };
    let f = faction(fk)?;
    let mut v = Vars::new();
    v.insert("day".into(), sym.get("day").map(pystr).unwrap_or_else(|| "0".into()));
    v.insert("week".into(), sym.get("week").map(pystr).unwrap_or_else(|| "1".into()));
    v.insert("enemy".into(), f.enemy.to_string());
    v.insert("enemies".into(), f.enemies.to_string());
    v.insert("leader".into(), f.leader.to_string());
    v.insert("faction".into(), f.plural.to_string());
    if let Some(p) = sym.get("place") {
        v.insert("place".into(), pystr(p));
    }
    if let Some(i) = sym.get("inf") {
        let s = pystr(i);
        v.insert("infs".into(), format!("{}s", s));
        v.insert("inf".into(), s);
    }
    if let Some(ill) = sym.get("ill") {
        match ill.as_str().and_then(illness) {
            Some(i) => {
                v.insert("ill".into(), i.name.to_string());
                v.insert("ill_a".into(), i.an.to_string());
                v.insert("cure".into(), i.cure.to_string());
            }
            None => {
                if matches!(ill, Value::Array(_) | Value::Object(_)) {
                    return None;
                }
                v.insert("ill".into(), "the sickness".into());
                v.insert("ill_a".into(), "the sickness".into());
                v.insert("cure".into(), "medicine".into());
            }
        }
    }
    for (key, words, pf) in [("n", &*NUMS, false), ("lost", &*NUMW, true), ("gain", &*GAINW, false)] {
        if let Some(x) = sym.get(key) {
            if !matches!(x, Value::Number(_) | Value::Bool(_)) {
                return None;
            }
            v.insert(key.into(), numword(int_of(x), words, pf));
        }
    }
    if let Some(a) = sym.get("ago") {
        v.insert("ago".into(), pystr(a));
    }
    Some(v)
}

// py: clan_story.py:406 _params
pub fn _params(people: &Value, sym: &Value) -> Value {
    let mut p = Map::new();
    for k in ["A", "B", "C", "D"] {
        if let Some(x) = people.get(k) {
            p.insert(k.into(), Value::Array(vec![vget(x, "n").clone(), vget(x, "g").clone()]));
        }
    }
    let mut m = Map::new();
    m.insert("p".into(), Value::Object(p));
    m.insert("s".into(), sym.clone());
    Value::Object(m)
}

// py: clan_story.py:410 render_msg
/// A stored message (lid + params) in a language ("en" | "pl"); None when the template is unknown (an older / newer line set).
pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
    if lid.starts_with("al/") {
        return ext::alife::render_msg(lid, params, lang);
    }
    if lid.starts_with("cn/") {
        return crate::clan::clan_continuity::render_msg(lid, params, lang);
    }
    if lid.starts_with("arc/") {
        return ext::arcs::render_msg(lid, params, lang);
    }
    if lid.starts_with("pk/") {
        return ext::perks::render_msg(lid, params, lang);
    }
    if lid.starts_with("sg/") {
        return ext::saga::render_msg(lid, params, lang);
    }
    if lid.starts_with("ch/") {
        return ext::chronicle::render_msg(lid, params, lang);
    }
    let (_scope, en_tpl, pl_tpl) = pln::reg(lid)?;
    let pobj = params.as_object()?;
    let mut people = Map::new();
    if let Some(p) = pobj.get("p") {
        for (k, v) in p.as_object()? {
            let a = v.as_array()?;
            let name = pystr(a.first()?);
            let g = a.get(1)?;
            let gs = match g.as_str() {
                Some(s) if pron(s, "he").is_some() => s,
                _ => "n",
            };
            let mut pm = Map::new();
            pm.insert("n".into(), Value::String(name));
            pm.insert("g".into(), jstr(gs));
            people.insert(k.clone(), Value::Object(pm));
        }
    }
    let people = Value::Object(people);
    let empty = Value::Object(Map::new());
    let sym = pobj.get("s").unwrap_or(&empty);
    if !sym.is_object() {
        return None;
    }
    if pln::norm_lang(lang) == "pl" {
        return pln::render(pl_tpl, &people, &pln::expand(sym)?);
    }
    render(en_tpl, &people, &expand_en(sym)?)
}

// ------------------------------------------------------------------ the day
// py: clan_story.py:442 _kill_pick
/// clan.Crowd.kill: a sick member, else a hungry one, else anybody. When the clan is broken, the grieving and the guilty go first.
/// fight=True (a death of the attack): never a kid, unless the clan has nobody else.
fn _kill_pick(rng: &mut PyRandom, st: &Value, fight: bool) -> Option<Value> {
    let mut people: Vec<&Value> = people_of(st).iter().collect();
    if fight {
        let f: Vec<&Value> = people.iter().copied().filter(|p| kd::can_fight(p)).collect();
        if !f.is_empty() {
            people = f;
        }
    }
    let doom = ch_get(st, "doom");
    if let Some(d) = doom.as_object() {
        if let Some(p) = people.iter().find(|p| d.contains_key(&vi(p, "i").to_string()) && kd::can_fight(p)) {
            return Some((*p).clone());
        }
    }
    for state in ["s", "u", "h"] {
        let group: Vec<&Value> = people.iter().copied().filter(|p| vs(p, "s") == state).collect();
        if !group.is_empty() {
            if vi(st, "morale") <= 25 {
                let weak: Vec<&Value> = group.iter().copied().filter(|p| truthy(vget(p, "gl")) || vi(p, "gr") > vi(st, "day")).collect();
                if !weak.is_empty() && rng.random() < 0.6 {
                    return Some(weak[rchoice_idx(rng, weak.len())].clone());
                }
            }
            return Some(group[rchoice_idx(rng, group.len())].clone());
        }
    }
    None
}

/// Mark `m["gr"] = max(m["gr"], day + 6)` etc. on the live roster member with that id (a removed member: nothing changes).
fn set_grief(st: &mut Value, id: i64, day: i64) {
    if let Some(m) = person_mut(st, id) {
        let g = vi(m, "gr").max(day + 6);
        m["gr"] = Value::from(g);
    }
}

fn set_guilt(st: &mut Value, id: i64, day: i64) {
    if let Some(m) = person_mut(st, id) {
        m["gl"] = Value::from(day + 12);
    }
}

// py: clan_story.py:463 finish_member
/// A member leaves the roster for good outside the clan's own death rules (a story arc's end): the dead record, the grief of the kin and of the one
/// who had to do it, the guilt of the helper.
pub fn finish_member(st: &mut Value, p: &Value, day: i64, cause: &str, helper: Option<&Value>) {
    _remove(st, p, day, cause);
    let kid = if truthy(vget(p, "k")) { _by_id(st, vi(p, "k")) } else { None };
    let mut seen: Vec<i64> = Vec::new();
    for x in [kid.as_ref(), helper].into_iter().flatten() {
        if !truthy(x) {
            continue;
        }
        let id = vi(x, "i");
        if seen.contains(&id) {
            continue;
        }
        seen.push(id);
        set_grief(st, id, day);
    }
    if matches!(cause, "mercy" | "bite_kin" | "turned") {
        if let Some(h) = helper.filter(|h| truthy(h)) {
            set_guilt(st, vi(h, "i"), day);
        }
    }
}

// py: clan_story.py:474 spare_sick
/// A quiet newcomer with a flu takes the place in the roster of a spore carrier who left.
pub fn spare_sick(rng: &mut PyRandom, st: &mut Value, day: i64) -> Value {
    let mut q = new_person(rng, st, None, "s", 0);
    q["il"] = jstr("flu");
    q["id"] = Value::from(day);
    people_mut(st).push(q.clone());
    q
}

// py: clan_story.py:483 _remove
fn _remove(st: &mut Value, p: &Value, day: i64, cause: &str) {
    let id = vi(p, "i");
    let pos = people_of(st).iter().position(|x| vi(x, "i") == id);
    let live = match pos {
        Some(i) => people_mut(st).remove(i),
        None => p.clone(),
    };
    let kid = if truthy(vget(&live, "k")) { _by_id(st, vi(&live, "k")) } else { None };
    if let Some(k) = &kid {
        if let Some(m) = person_mut(st, vi(k, "i")) {
            m["k"] = Value::from(0);
        }
    }
    let mut d = Map::new();
    d.insert("i".into(), vget(&live, "i").clone());
    d.insert("n".into(), vget(&live, "n").clone());
    d.insert("g".into(), vget(&live, "g").clone());
    d.insert("r".into(), vget(&live, "r").clone());
    d.insert("d".into(), Value::from(day));
    d.insert("c".into(), jstr(cause));
    d.insert("il".into(), vget(&live, "il").clone());
    d.insert("k".into(), match &kid {
        Some(k) => vget(k, "i").clone(),
        None => Value::from(0),
    });
    let dead = dead_mut(st);
    dead.push(Value::Object(d));
    if dead.len() > 30 {
        dead.drain(..dead.len() - 30);
    }
}

// py: clan_story.py:492 _mourner
fn _mourner(rng: &mut PyRandom, st: &Value, dead_p: &Value) -> Option<Value> {
    if truthy(vget(dead_p, "k")) {
        if let Some(k) = _by_id(st, vi(dead_p, "k")) {
            if truthy(&k) {
                return Some(k);
            }
        }
    }
    let did = vi(dead_p, "i");
    let others: Vec<&Value> = people_of(st).iter().filter(|q| vi(q, "i") != did).collect();
    if others.is_empty() {
        None
    } else {
        Some(others[rchoice_idx(rng, others.len())].clone())
    }
}

// py: clan_story.py:500 _apply_morale
fn _apply_morale(st: &mut Value, r: &Value, size_after: i64, sick_after: i64) -> i64 {
    let sup = vi(r, "supplies");
    let need = vi(r, "needed");
    let d = sup - need;
    let mut m = vi(st, "morale");
    if d >= 0 {
        let ss = vi(st, "ss") + 1;
        st["ss"] = Value::from(ss);
        st["ds"] = Value::from(0);
        m += 2 + 6.min(py::floordiv(d, 3)) + if ss >= 3 { 3 } else { 0 };
    } else {
        let ds = vi(st, "ds") + 1;
        st["ds"] = Value::from(ds);
        st["ss"] = Value::from(0);
        m += -3 + (-8).max(py::floordiv(d, 2)) - if ds >= 2 { 2 } else { 0 };
    }
    m -= 15.min(5 * vi(r, "died"));
    m += 9.min(3 * vi(r, "gained") + 3 * vi(r, "evg"));
    m += 6.min(2 * vi(r, "healed"));
    if size_after != 0 {
        m -= py::round0(10.0 * sick_after as f64 / size_after as f64);
    }
    let evk = r.get("evk").map(int_of);
    if r.get("evk").map_or(false, |x| !x.is_null()) && evk == Some(cl::EV_ATTACK) {
        m += match vi(r, "evt") {
            0 => -8,
            1 => -4,
            2 => 1,
            3 => 6,
            _ => 0,
        };
    } else if evk == Some(cl::EV_GROWTH) && vi(r, "evt") >= 1 {
        m += 4;
    }
    st["morale"] = Value::from(0.max(100.min(m)));
    d
}

fn count_state(st: &Value, state: &str) -> usize {
    people_of(st).iter().filter(|p| vs(p, "s") == state).count()
}

fn msg_kind_of_cause(cause: &str) -> &'static str {
    match cause {
        "attack" => "death_attack",
        "illness" => "death_illness",
        "mercy" => "death_mercy",
        "starved" => "death_starved",
        "lost" => "death_lost",
        "walkout" => "death_walkout",
        "suicide" => "death_suicide",
        "bite" => "death_bite",
        "bite_kin" => "death_bite_kin",
        "turned" => "death_turned",
        _ => "death_illness",
    }
}

fn obj1(k: &str, p: &Value) -> Value {
    let mut m = Map::new();
    m.insert(k.into(), p.clone());
    Value::Object(m)
}

/// `{k: v for k, v in zip("AB", picks)}`
fn zip_people(letters: &str, picks: &[Value]) -> Value {
    let mut m = Map::new();
    for (l, p) in letters.chars().zip(picks) {
        m.insert(l.to_string(), p.clone());
    }
    Value::Object(m)
}

fn dead_person(dd: &Value) -> Value {
    let mut m = Map::new();
    m.insert("n".into(), vget(dd, "n").clone());
    m.insert("g".into(), vget(dd, "g").clone());
    m.insert("r".into(), dd.get("r").cloned().unwrap_or_else(|| jstr("")));
    Value::Object(m)
}

// py: clan_story.py:525 story_day
/// Advance the roster to the clan state `newc` (counts after the day) using the day's result `r`, and write the day's messages.
/// Returns the list of message dicts (kind, mood, text, names, lid, params).
pub fn story_day(st: &mut Value, rng: &mut PyRandom, newc: &Value, r: &Value) -> Vec<Value> {
    let day = vi(newc, "day");
    let week = cl::week_day(day).0;
    let fk = fkey_of(st);
    let (hh, uu, ss) = (vi(newc, "healthy"), vi(newc, "hungry"), vi(newc, "sick"));
    let size = hh + uu + ss;
    let mut w = Writer::new(st, day, week);
    let died = vi(r, "died");
    let evl = vi(r, "evl");
    let evk = vi(r, "evk");
    let evt = vi(r, "evt");
    let gained = vi(r, "gained") + vi(r, "evg");
    st["day"] = Value::from(day);
    let prev_sick: HashSet<i64> = people_of(st).iter().filter(|p| vs(p, "s") == "s").map(|p| vi(p, "i")).collect();
    let deficit_day = vi(r, "supplies") < vi(r, "needed");
    let surplus_day = !deficit_day;
    if evk == cl::EV_ATTACK {
        st["atk"] = Value::from(day);
    }
    let atk_lag = day - vi(st, "atk");

    _apply_morale(st, r, size, ss);
    // --- deaths (clan.resolve_day: the deficit's deaths first, then the attack's)
    let mut deaths: Vec<(Value, String, Option<Value>)> = Vec::new();
    for k in 0..died {
        let fight = k >= died - evl;
        let Some(p) = _kill_pick(rng, st, fight) else { break };
        let mut cause: Option<String> = if fight && kd::can_fight(&p) { Some("attack".into()) } else { None };
        let mut helper: Option<Value> = None;
        if cause.is_none() {
            let recent = dead_of(st).iter().filter(|d| day - vi(d, "d") <= 6).count() as i64 + deaths.len() as i64;
            let gate = vi(st, "morale") <= 22 && (vi(st, "ds") >= 3 || recent >= 2) && day - vi(st, "suic") >= 5;
            let mourner = if people_of(st).len() > 1 { _mourner(rng, st, &p) } else { None };
            let hint = ch_get(st, "doom").get(vi(&p, "i").to_string()).filter(|h| truthy(h)).map(pystr);
            if let Some(h) = hint {
                cause = Some(h);
                helper = mourner;
                if let Some(d) = st.get_mut("ch").and_then(|c| c.get_mut("doom")).and_then(|d| d.as_object_mut()) {
                    d.shift_remove(&vi(&p, "i").to_string());
                }
            } else if vs(&p, "s") == "s" && vs(&p, "il") == "spores" {
                cause = Some("turned".into());
                helper = mourner;
            } else if vs(&p, "s") == "s" {
                let ill = if vs(&p, "il").is_empty() { "fever" } else { vs(&p, "il") };
                if (1..=3).contains(&atk_lag) && rng.random() < 0.5 {
                    let kin = if truthy(vget(&p, "k")) { _by_id(st, vi(&p, "k")) } else { None };
                    if kin.is_some() && rng.random() < 0.7 {
                        cause = Some("bite_kin".into());
                        helper = kin;
                    } else {
                        cause = Some("bite".into());
                        helper = mourner;
                    }
                } else if gate && rng.random() < 0.3 {
                    cause = Some("suicide".into());
                    helper = mourner;
                    st["suic"] = Value::from(day);
                } else if (vi(&p, "is") >= 2 && rng.random() < 0.4) || (ill == "spores" && rng.random() < 0.3) {
                    cause = Some("mercy".into());
                    helper = mourner;
                } else if vi(st, "morale") <= 30 && rng.random() < 0.18 {
                    cause = Some("walkout".into());
                    helper = mourner;
                } else {
                    cause = Some("illness".into());
                    helper = mourner;
                }
            } else if vs(&p, "s") == "u" {
                if gate && rng.random() < 0.25 {
                    cause = Some("suicide".into());
                    helper = mourner;
                    st["suic"] = Value::from(day);
                } else if vi(st, "morale") <= 30 && rng.random() < 0.2 {
                    cause = Some("walkout".into());
                    helper = mourner;
                } else {
                    cause = Some("starved".into());
                    helper = mourner;
                }
            } else if gate && rng.random() < 0.2 {
                cause = Some("suicide".into());
                helper = mourner;
                st["suic"] = Value::from(day);
            } else {
                cause = Some("lost".into());
                helper = mourner;
            }
        }
        let mut cause = cause.unwrap_or_default();
        if matches!(cause.as_str(), "mercy" | "bite" | "bite_kin" | "turned") && helper.as_ref().map_or(false, kd::is_kid) {
            let pid = vi(&p, "i");
            let alt: Vec<&Value> = people_of(st).iter().filter(|q| vi(q, "i") != pid && kd::can_fight(q)).collect();
            helper = if alt.is_empty() { None } else { Some(alt[rchoice_idx(rng, alt.len())].clone()) };
            if helper.is_none() {
                cause = "illness".into();
            }
        }
        if kd::is_kid(&p) && !matches!(cause.as_str(), "illness" | "starved" | "mercy") {
            cause = if vs(&p, "s") == "u" { "starved".into() } else { "illness".into() };
            if !helper.as_ref().map_or(false, truthy) {
                helper = if people_of(st).len() > 1 { _mourner(rng, st, &p) } else { None };
            }
        }
        deaths.push((p.clone(), cause.clone(), helper));
        _remove(st, &p, day, &cause);
    }
    for (p, cause, helper) in &deaths {
        // grief / guilt flags
        let kid = if truthy(vget(p, "k")) { _by_id(st, vi(p, "k")) } else { None };
        let mut seen: Vec<i64> = Vec::new();
        for x in [kid.as_ref(), helper.as_ref()].into_iter().flatten() {
            if !truthy(x) {
                continue;
            }
            let id = vi(x, "i");
            if seen.contains(&id) {
                continue;
            }
            seen.push(id);
            set_grief(st, id, day);
        }
        if matches!(cause.as_str(), "mercy" | "bite_kin" | "turned") {
            if let Some(h) = helper.as_ref().filter(|h| truthy(h)) {
                set_guilt(st, vi(h, "i"), day);
            }
        }
    }
    for p in people_mut(st).iter_mut() {
        let gl = vi(p, "gl");
        if gl != 0 && gl < day {
            p["gl"] = Value::from(0);
        }
    }

    // --- joins
    let mut joined: Vec<Value> = Vec::new();
    for _ in 0..gained.max(0) {
        let mut p = new_person(rng, st, None, "h", day);
        _link_kin(rng, st, &mut p);
        people_mut(st).push(p.clone());
        joined.push(p);
    }

    // --- hunger / sickness / healing, reconciled to the real counts
    let mut new_sick: Vec<Value> = Vec::new();
    let mut recovered: Vec<Value> = Vec::new();
    if deficit_day {
        for p in people_mut(st).iter_mut() {
            if vs(p, "s") == "s" && prev_sick.contains(&vi(p, "i")) {
                p["is"] = Value::from(3.min(vi(p, "is") + 1));
            }
        }
    }
    let total = |st: &Value| count_state(st, "h") + count_state(st, "u") + count_state(st, "s");
    let surplus_roster = (total(st) as i64 - size).max(0);
    for _ in 0..surplus_roster {
        // roster longer than the clan (a bootstrap mismatch): drop silently
        let n = people_of(st).len();
        if n == 0 {
            break;
        }
        let i = rchoice_idx(rng, n);
        people_mut(st).remove(i);
    }
    while (total(st) as i64) < size {
        // roster shorter: silent filler
        let p = new_person(rng, st, None, "h", day);
        people_mut(st).push(p);
    }
    while count_state(st, "s") as i64 > ss {
        let sick_all: Vec<Value> = people_of(st).iter().filter(|p| vs(p, "s") == "s").cloned().collect();
        let cures: Vec<&Value> = sick_all.iter().filter(|p| vs(p, "il") != "spores").collect();
        if cures.is_empty() {
            // only spore carriers are left over: nobody recovers from the spores - the clan's last day with one of them
            let v = sick_all[rchoice_idx(rng, sick_all.len())].clone();
            let mut helper = if people_of(st).len() > 1 { _mourner(rng, st, &v) } else { None };
            if !(helper.is_some() && !kd::is_kid(helper.as_ref().unwrap_or(&Value::Null))) {
                let vid = vi(&v, "i");
                helper = people_of(st).iter().find(|q| vi(q, "i") != vid && kd::can_fight(q)).cloned();
            }
            finish_member(st, &v, day, "turned", helper.as_ref());
            let q = new_person(rng, st, None, "h", day);
            people_mut(st).push(q);
            continue;
        }
        let v = (*cures[rchoice_idx(rng, cures.len())]).clone();
        let vid = vi(&v, "i");
        if let Some(m) = person_mut(st, vid) {
            m["s"] = jstr("h");
            m["il"] = jstr("");
            m["is"] = Value::from(0);
            recovered.push(m.clone());
        }
    }
    while (count_state(st, "s") as i64) < ss {
        let pool: Vec<Value> = people_of(st).iter().filter(|p| matches!(vs(p, "s"), "h" | "u")).cloned().collect();
        if pool.is_empty() {
            break;
        }
        let v = pool[rchoice_idx(rng, pool.len())].clone();
        let il = _pick_illness(rng, st, deficit_day, Some(&v));
        if let Some(m) = person_mut(st, vi(&v, "i")) {
            m["s"] = jstr("s");
            m["il"] = jstr(il);
            m["is"] = Value::from(0);
            m["id"] = Value::from(day);
            new_sick.push(m.clone());
        }
    }
    while count_state(st, "u") as i64 > uu {
        let pool: Vec<i64> = people_of(st).iter().filter(|p| vs(p, "s") == "u").map(|p| vi(p, "i")).collect();
        let v = pool[rchoice_idx(rng, pool.len())];
        if let Some(m) = person_mut(st, v) {
            m["s"] = jstr("h");
        }
    }
    while (count_state(st, "u") as i64) < uu {
        let pool: Vec<i64> = people_of(st).iter().filter(|p| vs(p, "s") == "h").map(|p| vi(p, "i")).collect();
        if pool.is_empty() {
            break;
        }
        let v = pool[rchoice_idx(rng, pool.len())];
        if let Some(m) = person_mut(st, v) {
            m["s"] = jstr("u");
        }
    }
    let mut busy: HashSet<i64> = HashSet::new();
    if let Some(arcs) = ch_get(st, "arcs").as_array() {
        for a in arcs {
            if let Some(c) = a.get("c").and_then(|c| c.as_object()) {
                for i in c.values() {
                    busy.insert(int_of(i));
                }
            }
        }
    }
    let spore_old: Vec<Value> = people_of(st)
        .iter()
        .filter(|p| {
            vs(p, "s") == "s" && vs(p, "il") == "spores" && {
                let idv = vget(p, "id");
                let idn = if truthy(idv) { int_of(idv) } else { day };
                day - idn >= 9
            } && !busy.contains(&vi(p, "i"))
        })
        .cloned()
        .collect();
    for v in &spore_old {
        // the spores do not wait for ever: nobody is sick with them for more than a week and a half
        let mut helper = if people_of(st).len() > 1 { _mourner(rng, st, v) } else { None };
        if !(helper.is_some() && !kd::is_kid(helper.as_ref().unwrap_or(&Value::Null))) {
            let vid = vi(v, "i");
            helper = people_of(st).iter().find(|q| vi(q, "i") != vid && kd::can_fight(q)).cloned();
        }
        finish_member(st, v, day, "turned", helper.as_ref());
        spare_sick(rng, st, day);
    }
    let sick_now: Vec<Value> = people_of(st).iter().filter(|p| vs(p, "s") == "s").cloned().collect();

    let morale = vi(st, "morale");

    // --- messages, in the order of importance
    let status = vi(newc, "status");
    let people_now: Vec<Value> = people_of(st).to_vec();
    let two = |rng: &mut PyRandom, fight: bool| -> Vec<Value> {
        let mut pool: Vec<&Value> = people_now.iter().collect();
        if fight {
            let f: Vec<&Value> = pool.iter().copied().filter(|p| kd::can_fight(p)).collect();
            if !f.is_empty() {
                pool = f;
            }
        }
        let k = pool.len().min(2);
        rng.sample_indices(pool.len(), k).unwrap_or_default().into_iter().map(|i| pool[i].clone()).collect()
    };
    let in_list = |p: &Value, list: &[Value]| list.iter().any(|x| vi(x, "i") == vi(p, "i"));
    if !deaths.is_empty() {
        let mut ordered: Vec<&(Value, String, Option<Value>)> = deaths.iter().take(2).collect();
        let attack_deaths: Vec<&(Value, String, Option<Value>)> = deaths.iter().filter(|d| d.1 == "attack").collect();
        let other: Vec<&(Value, String, Option<Value>)> = deaths.iter().filter(|d| d.1 != "attack").collect();
        let horde = evk == cl::EV_ATTACK && vi(r, "evn") == cl::AN_HORDE;
        if attack_deaths.len() >= 2 {
            let mut m = Map::new();
            m.insert("A".into(), attack_deaths[0].0.clone());
            m.insert("B".into(), attack_deaths[1].0.clone());
            let ppl = Value::Object(m);
            let n = attack_deaths.len() as i64;
            let o = Opts { n: Some(n), lost: Some(n), ..Default::default() };
            let done = horde && w.add(rng, st, "death_horde_many", &ppl, &o);
            if !done {
                w.add(rng, st, "death_attack_many", &ppl, &o);
            }
            ordered = other.iter().take(1).copied().collect();
        } else if deaths.len() >= 4 && attack_deaths.is_empty() {
            let mut m = Map::new();
            m.insert("A".into(), deaths[0].0.clone());
            m.insert("B".into(), deaths[1].0.clone());
            let ppl = Value::Object(m);
            let n = deaths.len() as i64;
            w.add(rng, st, "loss_many", &ppl, &Opts { n: Some(n), lost: Some(n), ..Default::default() });
            ordered = Vec::new();
        }
        for (p, cause, helper) in ordered.iter().map(|d| (&d.0, &d.1, &d.2)) {
            let mut kind = msg_kind_of_cause(cause);
            if kind == "death_attack" && horde {
                kind = "death_horde";
            }
            let mut ppl = obj1("A", p);
            let mut helper = helper.clone();
            let combat_cause = matches!(cause.as_str(), "attack" | "bite" | "bite_kin" | "turned");
            if combat_cause && helper.as_ref().map_or(false, kd::is_kid) {
                helper = None;
            }
            if let Some(h) = helper.as_ref().filter(|h| truthy(h)) {
                ppl["B"] = h.clone();
            } else if !people_now.is_empty() {
                let f: Vec<&Value> = if combat_cause { people_now.iter().filter(|q| kd::can_fight(q)).collect() } else { Vec::new() };
                let pick: Vec<&Value> = if f.is_empty() { people_now.iter().collect() } else { f };
                ppl["B"] = pick[rchoice_idx(rng, pick.len())].clone();
            }
            let mut o = Opts::default();
            if matches!(cause.as_str(), "bite" | "bite_kin") {
                o.ago = Some(if atk_lag == 1 {
                    "yesterday"
                } else if atk_lag == 2 {
                    "the other night"
                } else {
                    "a few days back"
                });
            }
            o.ill = Some(if truthy(vget(p, "il")) { pystr(vget(p, "il")) } else { "none".into() });
            if kind == "death_bite_kin" && ppl.get("B").is_none() {
                kind = "death_bite";
            }
            if !w.add(rng, st, kind, &ppl, &o) {
                if kind == "death_horde" {
                    // (a lone survivor has no horde voice: the attack's own death line)
                    w.add(rng, st, "death_attack", &ppl, &o);
                } else if matches!(kind, "death_bite_kin" | "death_mercy" | "death_walkout" | "death_suicide" | "death_turned") {
                    w.add(rng, st, "death_illness", &ppl, &o);
                }
            }
        }
    }
    // event outcome
    if evk == cl::EV_ATTACK {
        let ppl = zip_people("AB", &two(rng, true));
        let lost = evl;
        let hz = if vi(r, "evn") == cl::AN_HORDE { "_h" } else { "" };
        let o = Opts { lost: Some(lost), ..Default::default() };
        if evt >= 3 {
            w.add(rng, st, &format!("event_hold{}", hz), &ppl, &o);
        } else if evt == 2 {
            w.add(rng, st, &format!("event_heavy{}", hz), &ppl, &o);
        } else if status != cl::DEAD {
            w.add(rng, st, &format!("event_massacre{}", hz), &ppl, &o);
        }
    } else if evk == cl::EV_GROWTH {
        let evg = vi(r, "evg");
        if evg > 0 && !joined.is_empty() {
            let mut ppl = obj1("A", &joined[0]);
            let others: Vec<&Value> = people_now.iter().filter(|p| !in_list(p, &joined)).collect();
            if !others.is_empty() {
                ppl["B"] = others[rchoice_idx(rng, others.len())].clone();
            }
            w.add(rng, st, "event_growth", &ppl, &Opts { gain: Some(evg), ..Default::default() });
            joined = if joined.len() as i64 > evg { joined[evg as usize..].to_vec() } else { Vec::new() };
        } else {
            let ppl = zip_people("A", &two(rng, false));
            w.add(rng, st, "event_nojoin", &ppl, &Opts { mood: Some("grim"), ..Default::default() });
        }
    }
    if status == cl::DEAD {
        let last = deaths.last().map(|d| d.0.clone());
        let ppl = match last {
            Some(l) if truthy(&l) => obj1("A", &l),
            _ => {
                let mut m = Map::new();
                m.insert("n".into(), jstr("Someone"));
                m.insert("g".into(), jstr("n"));
                obj1("A", &Value::Object(m))
            }
        };
        w.add(rng, st, "clan_dead", &ppl, &Opts { mood: Some("grim"), ..Default::default() });
    }
    if status == cl::WON {
        let ppl = zip_people("AB", &two(rng, false));
        w.add(rng, st, "clan_won", &ppl, &Opts::default());
    }
    // next event warning
    if status == cl::ALIVE && cl::event_today(day).0 == cl::EV_ATTACK && vi(st, "warned") != day {
        st["warned"] = Value::from(day);
        let kind = if cl::event_today(day).1 == cl::AN_HORDE { "event_warn_h" } else { "event_warn" };
        let ppl = zip_people("A", &two(rng, true));
        w.add(rng, st, kind, &ppl, &Opts::default());
    }
    if !joined.is_empty() && status == cl::ALIVE && w.out.len() < DAY_MAX_MSGS {
        let mut ppl = obj1("A", &joined[0]);
        let others: Vec<&Value> = people_now.iter().filter(|p| !in_list(p, &joined)).collect();
        if !others.is_empty() {
            ppl["B"] = others[rchoice_idx(rng, others.len())].clone();
        }
        w.add(rng, st, "join", &ppl, &Opts::default());
    }
    if status == cl::ALIVE {
        // the darkest consequences first, gated by the state; then the day's condition
        let recent_dead: Vec<Value> = dead_of(st).iter().filter(|d| day - vi(d, "d") <= 8).cloned().collect();
        for p in new_sick.iter().take(2) {
            if w.out.len() >= DAY_MAX_MSGS {
                break;
            }
            let mut ppl = obj1("A", p);
            let pid = vi(p, "i");
            let medics: Vec<&Value> = people_now.iter().filter(|q| vs(q, "r") == "medic" && vi(q, "i") != pid && vs(q, "s") != "s").collect();
            let others: Vec<&Value> = if medics.is_empty() { people_now.iter().filter(|q| vi(q, "i") != pid).collect() } else { medics };
            if !others.is_empty() {
                ppl["B"] = others[rchoice_idx(rng, others.len())].clone();
            }
            let il = vs(p, "il").to_string();
            let pool = LINES_SICK.get(il.as_str());
            let pname = format!("onset:{}", il);
            let o = Opts { pool: pool.map(|p| p.as_slice()), pname: Some(&pname), ill: Some(il.clone()), ..Default::default() };
            w.add(rng, st, "sick", &ppl, &o);
        }
        let worse: Vec<&Value> = sick_now.iter().filter(|p| prev_sick.contains(&vi(p, "i")) && vi(p, "is") >= 1 && deficit_day).collect();
        if !worse.is_empty() && w.out.len() < DAY_MAX_MSGS && rng.random() < 0.7 {
            let mut p = worse[0];
            for q in &worse {
                if vi(q, "is") > vi(p, "is") {
                    p = q;
                }
            }
            let pid = vi(p, "i");
            let others: Vec<&Value> = people_now.iter().filter(|q| vi(q, "i") != pid).collect();
            let mut ppl = obj1("A", p);
            if !others.is_empty() {
                ppl["B"] = others[rchoice_idx(rng, others.len())].clone();
            }
            if vs(p, "il") == "spores" {
                let o = Opts { pool: SPR_LINES.get("spore_worse").map(|p| p.as_slice()), pname: Some("spore_worse"), ill_of: Some("A"), ..Default::default() };
                w.add(rng, st, "worse", &ppl, &o);
            } else {
                w.add(rng, st, "worse", &ppl, &Opts { ill_of: Some("A"), ..Default::default() });
            }
        } else if !sick_now.is_empty() && deficit_day && morale <= 55 && w.out.len() < DAY_MAX_MSGS && rng.random() < 0.6 {
            let p = &sick_now[rchoice_idx(rng, sick_now.len())];
            let pid = vi(p, "i");
            let others: Vec<&Value> = people_now.iter().filter(|q| vi(q, "i") != pid).collect();
            let mut ppl = obj1("A", p);
            if !others.is_empty() {
                ppl["B"] = others[rchoice_idx(rng, others.len())].clone();
            }
            if vs(p, "il") == "spores" {
                let o = Opts { pool: SPR_LINES.get("spore_hold").map(|p| p.as_slice()), pname: Some("spore_hold"), ill_of: Some("A"), ..Default::default() };
                w.add(rng, st, "medicine", &ppl, &o);
            } else {
                w.add(rng, st, "medicine", &ppl, &Opts { ill_of: Some("A"), ..Default::default() });
            }
        }
        if !recovered.is_empty() && w.out.len() < DAY_MAX_MSGS {
            let p = &recovered[0];
            let pid = vi(p, "i");
            let others: Vec<&Value> = people_now.iter().filter(|q| vi(q, "i") != pid).collect();
            let mut ppl = obj1("A", p);
            if !others.is_empty() {
                ppl["B"] = others[rchoice_idx(rng, others.len())].clone();
            }
            w.add(rng, st, "recovery", &ppl, &Opts { ill: Some("none".into()), ..Default::default() });
        }
        // grief and guilt of the days just lived through
        let mourners: Vec<&Value> = people_now.iter().filter(|p| vi(p, "gr") > day && vs(p, "s") != "s").collect();
        if !recent_dead.is_empty() && !mourners.is_empty() && w.out.len() < DAY_MAX_MSGS && (!deaths.is_empty() || rng.random() < 0.3) {
            let mut pick: Option<Value> = None;
            for dd in recent_dead.iter().rev() {
                pick = if truthy(vget(dd, "k")) { _by_id(st, vi(dd, "k")) } else { None };
                if pick.is_some() {
                    break;
                }
            }
            let p = match pick {
                Some(p) => p,
                None => mourners[rchoice_idx(rng, mourners.len())].clone(),
            };
            let pi = vi(&p, "i");
            let dd = recent_dead.iter().rev().find(|d| vi(d, "k") == pi).unwrap_or_else(|| recent_dead.last().expect("recent_dead")).clone();
            let mut ppl = obj1("A", &p);
            ppl["D"] = dead_person(&dd);
            let c = vs(&dd, "c").to_string();
            let mut kind = if matches!(c.as_str(), "mercy" | "bite_kin") && rng.random() < 0.8 { "grief_mercy" } else { "grief" };
            if c == "turned" && people_now.len() >= 3 && rng.random() < 0.85 {
                // the spores took {D}: its own grief pool
                kind = "grief_turned";
            }
            w.add(rng, st, kind, &ppl, &Opts { cause: Some(&c), ..Default::default() });
        }
        let guilty: Vec<&Value> = people_now.iter().filter(|p| truthy(vget(p, "gl")) && !recent_dead.is_empty()).collect();
        if !guilty.is_empty() && w.out.len() < DAY_MAX_MSGS && rng.random() < 0.5 && deaths.is_empty() {
            let dd = recent_dead.last().expect("recent_dead").clone();
            let gid = vi(guilty[0], "i");
            let others: Vec<&Value> = people_now.iter().filter(|q| vi(q, "i") != gid).collect();
            let mut ppl = obj1("A", guilty[0]);
            ppl["D"] = dead_person(&dd);
            if !others.is_empty() {
                ppl["B"] = others[rchoice_idx(rng, others.len())].clone();
            }
            w.add(rng, st, "guilt", &ppl, &Opts::default());
        }
        if w.out.len() < DAY_MAX_MSGS {
            let hungry_now: Vec<&Value> = people_now.iter().filter(|p| vs(p, "s") == "u").collect();
            let mut kind: Option<&str> = None;
            let third = (size / 3).max(2);
            if deficit_day && uu + ss >= third && rng.random() < 0.8 {
                kind = Some(if uu >= third { "starving" } else { "hunger" });
            } else if deficit_day && rng.random() < 0.7 {
                kind = Some("hunger");
            } else if morale <= 25 && rng.random() < 0.8 {
                kind = Some("despair");
            } else if surplus_day && !recent_dead.is_empty() && morale >= 45 && rng.random() < 0.8 {
                kind = Some("hope_after");
            } else if surplus_day && vi(r, "supplies") - vi(r, "needed") >= 6 && rng.random() < 0.6 {
                kind = Some("plenty");
            } else if surplus_day && morale >= 60 && rng.random() < 0.75 {
                kind = Some("hope");
            } else if rng.random() < 0.5 {
                kind = Some("calm");
            }
            if let Some(kind) = kind {
                let mut ppl = Map::new();
                let base: Vec<&Value> = if matches!(kind, "hunger" | "starving") && !hungry_now.is_empty() { hungry_now } else { people_now.iter().collect() };
                if !base.is_empty() {
                    let a = base[rchoice_idx(rng, base.len())];
                    ppl.insert("A".into(), a.clone());
                    let aid = vi(a, "i");
                    let rest: Vec<&Value> = people_now.iter().filter(|q| vi(q, "i") != aid).collect();
                    if !rest.is_empty() {
                        ppl.insert("B".into(), rest[rchoice_idx(rng, rest.len())].clone());
                    }
                }
                if kind == "hope_after" {
                    let dd = recent_dead.last().expect("recent_dead");
                    ppl.insert("D".into(), dead_person(dd));
                }
                if !ppl.is_empty() {
                    w.add(rng, st, kind, &Value::Object(ppl), &Opts::default());
                }
            }
        }
        // the 2013 ticker: one small line about one person's real state (one survivor: the diary voice, two: no crowd lines)
        if !people_now.is_empty() && w.out.len() < DAY_MAX_MSGS - 1 && rng.random() < 0.75 {
            let p = &people_now[rchoice_idx(rng, people_now.len())];
            let solo = people_now.len() == 1;
            let ukey = format!("{}{}", if solo { "ticker_solo_" } else { "ticker_" }, vs(p, "s"));
            let mut used = used_list(st, &ukey);
            if solo {
                let pool_t: Vec<&'static str> = solo_ticker(vs(p, "s")).iter().map(|(_sc, t)| *t).collect();
                let mut fresh: Vec<&'static str> = pool_t.iter().copied().filter(|t| !used.contains(&line_id(t))).collect();
                if fresh.is_empty() {
                    fresh = pool_t.clone();
                }
                let t = fresh[rchoice_idx(rng, fresh.len())];
                used.push(line_id(t));
                trim_front(&mut used, 200);
                set_used_list(st, &ukey, &used);
                let ppl = obj1("A", p);
                let mut sym = Map::new();
                sym.insert("f".into(), jstr(fk));
                sym.insert("day".into(), Value::from(day));
                sym.insert("week".into(), Value::from(week));
                let sym = Value::Object(sym);
                let mut m = Map::new();
                m.insert("kind".into(), jstr("ticker"));
                m.insert("mood".into(), jstr("neutral"));
                m.insert("text".into(), Value::String(render(t, &ppl, &Vars::new()).unwrap_or_default()));
                m.insert("names".into(), Value::Array(Vec::new()));
                m.insert("lid".into(), Value::String(pln::lid(TPL_POOL.get(t).copied().unwrap_or(""), t)));
                m.insert("params".into(), _params(&ppl, &sym));
                w.out.push(Value::Object(m));
            } else {
                // one line about one member's real state (rare joke for sick ones); a healthy member gets a rich line (clan_story_good_rich.py)
                let avoid: HashSet<String> = used.iter().cloned().collect();
                let (t, tpool, ppl) = ticker_for(rng, st, p, &avoid);
                used.push(line_id(t));
                trim_front(&mut used, 200);
                set_used_list(st, &ukey, &used);
                let mut sym = _sym(rng, st);
                sym["day"] = Value::from(day);
                sym["week"] = Value::from(week);
                let mut names = Vec::new();
                for k in ["A", "B", "C"] {
                    if let Some(x) = ppl.get(k) {
                        names.push(vget(x, "n").clone());
                    }
                }
                let mut m = Map::new();
                m.insert("kind".into(), jstr("ticker"));
                m.insert("mood".into(), jstr("neutral"));
                m.insert("text".into(), Value::String(expand_en(&sym).and_then(|v| render(t, &ppl, &v)).unwrap_or_default()));
                m.insert("names".into(), Value::Array(names));
                m.insert("lid".into(), Value::String(pln::lid(tpool, t)));
                m.insert("params".into(), _params(&ppl, &sym));
                w.out.push(Value::Object(m));
            }
        }
    }
    w.out
}

// ------------------------------------------------------------------ database
// py: clan_story.py:871 ensure
pub fn ensure(c: &Connection) -> rusqlite::Result<()> {
    for s in data::strs(tb("clan_story", "SCHEMA")) {
        c.execute(s, [])?;
    }
    let have: HashSet<String> = {
        let mut st = c.prepare("PRAGMA table_info(clan_feed)")?;
        let rows = st.query_map([], |r| r.get::<_, String>(1))?;
        rows.collect::<Result<_, _>>()?
    };
    for col in tb("clan_story", "NEW_COLUMNS").as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
        let name = col.get(0).and_then(|x| x.as_str()).unwrap_or("");
        let typ = col.get(1).and_then(|x| x.as_str()).unwrap_or("");
        if !have.contains(name) {
            c.execute(&format!("ALTER TABLE clan_feed ADD COLUMN {} {}", name, typ), [])?;
        }
    }
    Ok(())
}

fn v_eq_one(v: &Value) -> bool {
    match v {
        Value::Number(n) => n.as_f64() == Some(1.0),
        Value::Bool(b) => *b,
        _ => false,
    }
}

// py: clan_story.py:880 _load
pub fn _load(c: &Connection, tid: &str) -> Option<Value> {
    let row: Option<Option<String>> = c.query_row("SELECT state FROM clan_story WHERE token_id=?", [tid], |r| r.get::<_, Option<String>>(0)).ok();
    let text = row??;
    let st: Value = serde_json::from_str(&text).ok()?;
    if st.is_object() && st.get("v").map_or(false, v_eq_one) {
        Some(st)
    } else {
        None
    }
}

// py: clan_story.py:894 _save
pub fn _save(c: &Connection, tid: &str, st: &Value) -> rusqlite::Result<()> {
    c.execute(
        "INSERT INTO clan_story(token_id,state,updated) VALUES(?,?,?) ON CONFLICT(token_id) DO UPDATE SET state=excluded.state,updated=excluded.updated",
        params![tid, py::dumps_compact(st), clock::now()],
    )?;
    Ok(())
}

// py: clan_story.py:899 _store
pub fn _store(c: &Connection, tid: &str, st: &mut Value, msgs: &mut [Value], faction: i64, day: i64, week: i64) -> rusqlite::Result<()> {
    let now = clock::now();
    for m in msgs.iter_mut() {
        if st.get("sn").is_none() {
            st["sn"] = vget(st, "n").clone();
        }
        let n = vi(st, "n") + 1;
        st["n"] = Value::from(n);
        let kind = vs(m, "kind").to_string();
        if kind != "alife" && kind != "chron" {
            let sn = vi(st, "sn") + 1;
            st["sn"] = Value::from(sn);
        }
        let ts = py::round_n(now, 3);
        m["id"] = Value::from(n);
        m["day"] = Value::from(day);
        m["week"] = Value::from(week);
        m["faction"] = Value::from(faction);
        m["ts"] = Value::from(ts);
        let lid: Option<String> = m.get("lid").filter(|x| !x.is_null()).map(pystr);
        let params_v = m.get("params").filter(|x| truthy(x)).map(py::dumps_compact_utf8);
        c.execute(
            "INSERT OR REPLACE INTO clan_feed(token_id,id,ts,day,week,kind,mood,faction,text,names,lid,params) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
            params![tid, n, ts, day, week, kind, vs(m, "mood"), faction, vs(m, "text"), py::dumps(vget(m, "names")), lid, params_v],
        )?;
    }
    c.execute("DELETE FROM clan_feed WHERE token_id=? AND id<=?", params![tid, vi(st, "n") - feed_max()])?;
    Ok(())
}

// py: clan_story.py:914 _alife
fn alife_on() -> bool {
    ext::alife::enabled()
}

// py: clan_story.py:923 _chronicle
fn chronicle_on() -> bool {
    ext::chronicle::enabled()
}

// py: clan_story.py:931 feed_max
pub fn feed_max() -> i64 {
    if alife_on() || chronicle_on() {
        100
    } else {
        FEED_MAX
    }
}

// py: clan_story.py:935 _rng
pub fn _rng(st: &Value, salt: &str) -> PyRandom {
    let sn = st.get("sn").map(int_of).unwrap_or_else(|| vi(st, "n"));
    PyRandom::from_str(&format!("{}:{}:{}:{}", pystr(vget(st, "seed")), pystr(vget(st, "run")), sn, salt))
}

// py: clan_story.py:941 _purge_feed
fn _purge_feed(c: &Connection, tid: &str) {
    let _ = c.execute("DELETE FROM clan_feed WHERE token_id=?", [tid]);
    let _ = ext::journal::purge(c, tid);
}

// py: clan_story.py:956 on_new_run
/// First day of a clan (CHOOSE YOUR SIDE): five survivors and the founding line.
pub fn on_new_run(c: &Connection, tid: &str, faction: i64) -> rusqlite::Result<Vec<Value>> {
    ensure(c)?;
    let old = _load(c, tid);
    let mut st = new_state(tid, faction, old.as_ref().map_or(1, |o| vi(o, "run") + 1));
    st["n"] = Value::from(old.as_ref().map_or(0, |o| vi(o, "n")));
    _purge_feed(c, tid);
    let mut rng = _rng(&st, "new");
    _populate(&mut rng, &mut st, cl::START_SURVIVORS, 0, 0, 0);
    let mut w = Writer::new(&st, 0, 1);
    let mut ppl = Map::new();
    for (k, p) in ["A", "B", "C"].iter().zip(people_of(&st)) {
        ppl.insert((*k).into(), p.clone());
    }
    w.add(&mut rng, &mut st, "founding", &Value::Object(ppl), &Opts::default());
    let mut out = w.out;
    _store(c, tid, &mut st, &mut out, faction, 0, 1)?;
    if let Err(ex) = ext::journal::on_new_run(c, tid, &st) {
        eprintln!("clan_journal (new run) failed: {}", ex);
    }
    _save(c, tid, &st)?;
    let _ = ext::alife::reset(c, tid);
    Ok(out)
}

// py: clan_story.py:983 on_day
/// After clan.apply_match: update the roster and write the day's messages. `newc` = the clan dict after the day, `r` = its result.
pub fn on_day(c: &Connection, tid: &str, newc: &Value, r: &Value) -> rusqlite::Result<Vec<Value>> {
    if !truthy(r) || !truthy(newc) || ![cl::WLF, cl::SERAPHITES].contains(&vi(newc, "faction")) {
        return Ok(Vec::new());
    }
    ensure(c)?;
    let loaded = _load(c, tid);
    let faction = vi(newc, "faction");
    let mut st: Value;
    match loaded {
        Some(s) if vi(&s, "fac") == faction => {
            st = s;
            if vi(newc, "day") < vi(&st, "day") {
                // a reset clan that never went through on_new_run
                let n = vi(&st, "n");
                _purge_feed(c, tid);
                let run = vi(&st, "run") + 1;
                st = new_state(tid, faction, run);
                st["n"] = Value::from(n);
                let mut rng = _rng(&st, "reset");
                _populate(&mut rng, &mut st, vi(newc, "healthy"), vi(newc, "hungry"), vi(newc, "sick"), 0);
            }
        }
        other => {
            if other.is_some() {
                _purge_feed(c, tid);
            }
            st = new_state(tid, faction, other.as_ref().map_or(1, |o| vi(o, "run") + 1));
            let mut rng = _rng(&st, "boot");
            // a clan that existed before the feed: rebuild the roster from the counts as they were before this day
            let (h, u, s) = (vi(newc, "healthy"), vi(newc, "hungry"), vi(newc, "sick"));
            let day = r.get("day").map(int_of).unwrap_or_else(|| vi(newc, "day"));
            st["day"] = Value::from(day);
            st["boot"] = Value::from(1);
            let h_before = (h + vi(r, "died") - vi(r, "gained") - vi(r, "evg")).max(0);
            _populate(&mut rng, &mut st, h_before, u, s, day);
        }
    }
    let mut rng = _rng(&st, &format!("day{}", vi(newc, "day")));
    let jsnap = ext::journal::snapshot(&st).ok();
    let mut msgs = story_day(&mut st, &mut rng, newc, r);
    let away: HashSet<String> = {
        let a = ch_get(&st, "away");
        match a.as_object() {
            Some(o) if !o.is_empty() => people_of(&st).iter().filter(|p| o.contains_key(&vi(p, "i").to_string())).map(|p| vs(p, "n").to_string()).collect(),
            _ => HashSet::new(),
        }
    };
    if !away.is_empty() {
        // somebody in a story arc is away from camp: no diary line names them (deaths excepted)
        msgs.retain(|m| {
            let names: Vec<&str> = m.get("names").and_then(|n| n.as_array()).map(|a| a.iter().filter_map(|x| x.as_str()).collect()).unwrap_or_default();
            let kind = vs(m, "kind");
            !names.iter().any(|n| away.contains(*n)) || kind.starts_with("death_") || kind.starts_with("loss")
        });
    }
    if alife_on() {
        // the A-Life rows of the day come after the diary rows (it never changes a number or a state)
        match ext::alife::on_day(c, tid, &mut st, newc, r, &msgs) {
            Ok(extra) => msgs.extend(extra),
            Err(ex) => eprintln!("clan_alife failed: {}", ex),
        }
    }
    if chronicle_on() {
        // the chronicle (multi-sentence vignettes, match results, loot)
        match ext::chronicle::day_rows(&mut st, newc, r) {
            Ok(extra) => {
                msgs.extend(extra);
                msgs.extend(crate::clan::clan_continuity::recap_rows(c, tid, &mut st, newc, "worried"));
            }
            Err(ex) => eprintln!("clan_chronicle failed: {}", ex),
        }
    }
    let day = vi(newc, "day");
    _store(c, tid, &mut st, &mut msgs, faction, day, cl::week_day(day).0)?;
    if let Some(js) = &jsnap {
        if let Err(ex) = ext::journal::on_day(c, tid, &mut st, js, newc, r) {
            eprintln!("clan_journal (day) failed: {}", ex);
        }
    }
    _save(c, tid, &st)?;
    Ok(msgs)
}

// ------------------------------------------------------------------ reading (also used read-only by the update server)
fn row_value(c: &Connection, sql: &str, tid: &str, since: i64, limit: i64, cols: usize) -> rusqlite::Result<Vec<Vec<Value>>> {
    let mut st = c.prepare(sql)?;
    let rows = st.query_map(params![tid, since, limit], |r| {
        let mut v = Vec::with_capacity(cols);
        for i in 0..cols {
            v.push(cl::vref_to_json(r.get_ref(i)?));
        }
        Ok(v)
    })?;
    rows.collect()
}

/// `{k: v[0] for k, v in json.loads(params).get("p", {}).items() if k in "AD"}`, None when Python would have raised.
fn who_of(params: &str) -> Option<Value> {
    let pj: Value = serde_json::from_str(params).ok()?;
    let empty = Value::Object(Map::new());
    let p = pj.as_object()?.get("p").unwrap_or(&empty).as_object()?;
    let mut out = Map::new();
    for (k, v) in p {
        if k.is_empty() || k == "A" || k == "D" || k == "AD" {
            out.insert(k.clone(), v.as_array()?.first()?.clone());
        }
    }
    Some(Value::Object(out))
}

// py: clan_story.py:1047 read_feed
/// Newest first. Works on any sqlite connection (a missing table = [], a table without the lid / params columns = English only).
/// lang "pl": every message that was stored with its template id is rendered in Polish; older ones stay English.
pub fn read_feed(c: &Connection, tid: &str, limit: i64, since: i64, lang: &str, callbacks: bool, who: bool, fold: bool) -> Vec<Value> {
    if callbacks {
        let mut full = read_feed(c, tid, feed_max(), 0, lang, false, true, false);
        let st = _load(c, tid).unwrap_or_else(|| Value::Object(Map::new()));
        let mut sick = Map::new();
        for p in people_of(&st) {
            if vs(p, "s") == "s" && truthy(vget(p, "id")) {
                sick.insert(vs(p, "n").to_string(), vget(p, "id").clone());
            }
        }
        let dead = st.get("dead").cloned().unwrap_or_else(|| Value::Array(Vec::new()));
        crate::clan::clan_continuity::annotate(&mut full, &dead, &Value::Object(sick), pln::norm_lang(lang));
        for m in full.iter_mut() {
            if let Some(o) = m.as_object_mut() {
                o.shift_remove("who");
            }
        }
        if fold {
            full = story_rows(&full);
        }
        let lim = 1.max(limit.min(feed_max())) as usize;
        return full.into_iter().filter(|m| vi(m, "id") > since).take(lim).collect();
    }
    let limit = 1.max(limit.min(feed_max()));
    let lang = pln::norm_lang(lang);
    let rows = match row_value(
        c,
        "SELECT id,day,week,kind,mood,faction,text,names,ts,lid,params FROM clan_feed WHERE token_id=? AND id>? ORDER BY id DESC LIMIT ?",
        tid,
        since,
        limit,
        11,
    ) {
        Ok(r) => r,
        Err(_) => match row_value(c, "SELECT id,day,week,kind,mood,faction,text,names,ts,NULL,NULL FROM clan_feed WHERE token_id=? AND id>? ORDER BY id DESC LIMIT ?", tid, since, limit, 11) {
            Ok(r) => r,
            Err(_) => return Vec::new(),
        },
    };
    let mut out: Vec<Value> = Vec::new();
    for row in rows {
        let mut it = row.into_iter();
        let (i, day, week, kind, mood, fac, mut text, names, ts, lid, params) = (
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
            it.next().unwrap_or(Value::Null),
        );
        let names_s = if truthy(&names) { pystr(&names) } else { "[]".to_string() };
        let nm: Value = serde_json::from_str(&names_s).unwrap_or_else(|_| Value::Array(Vec::new()));
        let mut used = "en";
        let lid_s = lid.as_str().unwrap_or("").to_string();
        let params_s = params.as_str().unwrap_or("").to_string();
        if lang == "pl" && !lid_s.is_empty() && !params_s.is_empty() {
            if let Ok(pj) = serde_json::from_str::<Value>(&params_s) {
                if let Some(t) = render_msg(&lid_s, &pj, "pl") {
                    if !t.is_empty() {
                        text = Value::String(t);
                        used = "pl";
                    }
                }
            }
        }
        let mut m = Map::new();
        m.insert("id".into(), i);
        m.insert("day".into(), day);
        m.insert("week".into(), week);
        m.insert("kind".into(), kind);
        m.insert("mood".into(), mood);
        m.insert("faction".into(), fac);
        m.insert("text".into(), text);
        m.insert("names".into(), nm);
        m.insert("ts".into(), ts);
        m.insert("lang".into(), jstr(used));
        let noise = ["ch/", "sg/", "arc/", "pk/", "al/", "cn/"].iter().any(|p| lid_s.starts_with(p));
        if who && !params_s.is_empty() && !noise {
            // diary rows: who is who (letters A / D), for the call-backs
            if let Some(w) = who_of(&params_s) {
                m.insert("who".into(), w);
            }
        }
        if lid_s.starts_with("ch/") {
            // chronicle rows: the first piece's pool = the entry's style (band colour / icon) in the clan screens
            let body = &lid_s[3..];
            let style = body.split('+').next().unwrap_or("").split('~').next().unwrap_or("");
            m.insert("style".into(), jstr(style));
        } else if lid_s.starts_with("cn/") {
            m.insert("style".into(), jstr("saga_recap"));
        } else if lid_s.starts_with("pk/") {
            m.insert("style".into(), jstr("perk"));
        } else if lid_s.starts_with("sg/") {
            // a saga row: style saga_<scene>; rows of one person's thread share "arc" = thread id
            m.insert("style".into(), jstr(&format!("saga_{}", &lid_s[3..])));
            let src = if params_s.is_empty() { "{}" } else { params_s.as_str() };
            if let Ok(pj) = serde_json::from_str::<Value>(src) {
                if let Some(a) = pj.get("a").filter(|a| truthy(a)) {
                    m.insert("arc".into(), a.clone());
                }
                let s = pj.get("s").filter(|s| truthy(s));
                if let Some(mp) = s.and_then(|s| s.get("map")).filter(|x| truthy(x)) {
                    m.insert("map".into(), mp.clone());
                }
            }
        } else if lid_s.starts_with("arc/") {
            // a story arc step: style arc_<kind>, arc id + step for the thread view of the clan screens
            m.insert("style".into(), jstr(&format!("arc_{}", lid_s[4..].split('.').next().unwrap_or(""))));
            let src = if params_s.is_empty() { "{}" } else { params_s.as_str() };
            if let Ok(pj) = serde_json::from_str::<Value>(src) {
                m.insert("arc".into(), pj.get("a").cloned().unwrap_or(Value::Null));
                m.insert("step".into(), pj.get("n").cloned().unwrap_or(Value::Null));
            }
        }
        out.push(Value::Object(m));
    }
    if let Some(done) = _days_done(c, tid) {
        // never serve a day the clan has not reached (rows of an older run / a reset clan, see _purge_feed)
        out.retain(|m| {
            let d = vget(m, "day");
            (if truthy(d) { int_of(d) } else { 0 }) <= done
        });
    }
    out
}

// py: clan_story.py:1126 _days_done
/// The clan's day counter (days done) from the clans table, None when there is no clan row (tests, a missing table).
pub fn _days_done(c: &Connection, tid: &str) -> Option<i64> {
    let r: Option<Value> = c.query_row("SELECT day FROM clans WHERE token_id=?", [tid], |r| Ok(cl::vref_to_json(r.get_ref(0)?))).ok();
    match r {
        Some(v) if !v.is_null() => Some(int_of(&v)),
        _ => None,
    }
}

/// `re.split(r"(?<=[.!?])\s+(?=[A-Z\"'(])", t)`
fn split_sent(t: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut prev: Option<char> = None;
    let mut it = t.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        if py::is_py_space(c) && matches!(prev, Some('.' | '!' | '?')) {
            let mut end = i + c.len_utf8();
            while let Some(&(j, d)) = it.peek() {
                if py::is_py_space(d) {
                    end = j + d.len_utf8();
                    it.next();
                } else {
                    break;
                }
            }
            if t[end..].chars().next().map_or(false, |n| n.is_ascii_uppercase() || matches!(n, '"' | '\'' | '(')) {
                out.push(&t[start..i]);
                start = end;
                prev = Some(' ');
                continue;
            }
            prev = Some(' ');
            continue;
        }
        prev = Some(c);
    }
    out.push(&t[start..]);
    out
}

/// `re.sub(r"(?<!\w)" + re.escape(old) + r"(?!\w)", new, text)`
fn replace_word(text: &str, old: &str, new: &str) -> String {
    if old.is_empty() {
        return text.to_string();
    }
    let mut out = String::with_capacity(text.len());
    let mut i = 0;
    while i < text.len() {
        if text[i..].starts_with(old) {
            let before = text[..i].chars().next_back().map(is_word).unwrap_or(false);
            let after = text[i + old.len()..].chars().next().map(is_word).unwrap_or(false);
            if !before && !after {
                out.push_str(new);
                i += old.len();
                continue;
            }
        }
        let ch = text[i..].chars().next().unwrap_or(' ');
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

/// `re.findall(r"(?<=[a-z,;] )[A-Z][a-z]+", s)`
fn find_caps_after(s: &str) -> Vec<String> {
    let c: Vec<char> = s.chars().collect();
    let mut out = Vec::new();
    let mut i = 0;
    while i < c.len() {
        if c[i].is_ascii_uppercase() && i >= 2 && c[i - 1] == ' ' && (c[i - 2].is_ascii_lowercase() || c[i - 2] == ',' || c[i - 2] == ';') && c.get(i + 1).map_or(false, |x| x.is_ascii_lowercase()) {
            let mut j = i + 1;
            while j < c.len() && c[j].is_ascii_lowercase() {
                j += 1;
            }
            out.push(c[i..j].iter().collect());
            i = j;
        } else {
            i += 1;
        }
    }
    out
}

/// `re.findall(r"[A-Z][a-z]+(?= [A-Z][a-z]+)", s)`
fn find_caps_before(s: &str) -> Vec<String> {
    let c: Vec<char> = s.chars().collect();
    let mut out = Vec::new();
    let mut i = 0;
    while i < c.len() {
        if c[i].is_ascii_uppercase() && c.get(i + 1).map_or(false, |x| x.is_ascii_lowercase()) {
            let mut j = i + 1;
            while j < c.len() && c[j].is_ascii_lowercase() {
                j += 1;
            }
            if c.get(j) == Some(&' ') && c.get(j + 1).map_or(false, |x| x.is_ascii_uppercase()) && c.get(j + 2).map_or(false, |x| x.is_ascii_lowercase()) {
                out.push(c[i..j].iter().collect());
            }
            i = j;
        } else {
            i += 1;
        }
    }
    out
}

// py: clan_story.py:1141 _sentences
/// The plain sentences of a journal text. The "DAY n: HEADLINE." prefix of an arc / saga row becomes the first sentence in sentence case.
pub fn _sentences(t: &str, names: &[String]) -> Vec<String> {
    let mut t = t.trim_matches(py::is_py_space).to_string();
    if let Some(m) = HEAD_RE.find(&t) {
        let head_all = HEAD_NUM_RE.replace(m.as_str(), "").into_owned();
        let head = head_all.trim_matches(py::is_py_space).trim_end_matches('.').to_string();
        let body = t[m.end()..].to_string();
        let mut low = head.to_lowercase();
        let mut caps: BTreeSet<String> = names.iter().cloned().collect();
        caps.extend(find_caps_after(&body));
        caps.extend(find_caps_before(&body));
        let mut caps: Vec<String> = caps.into_iter().collect();
        caps.sort_by(|a, b| clen(b).cmp(&clen(a)));
        for w in &caps {
            low = replace_word(&low, &w.to_lowercase(), w);
        }
        let mut first = cap_first(&low);
        first.push_str(". ");
        first.push_str(&body);
        t = first.trim_matches(py::is_py_space).to_string();
    }
    split_sent(&t).into_iter().map(|x| x.trim_matches(py::is_py_space).to_string()).filter(|x| !x.is_empty()).collect()
}

fn names_of(m: &Value) -> Vec<String> {
    m.get("names").and_then(|n| n.as_array()).map(|a| a.iter().map(pystr).collect()).unwrap_or_default()
}

fn str_or_empty(m: &Value, k: &str) -> String {
    match m.get(k) {
        Some(x) if truthy(x) => pystr(x),
        _ => String::new(),
    }
}

// py: clan_story.py:1166 _story_rank
/// None = a one-liner (never a card); else 0 founding / a death / an arrival (always a card), 1 arcs and saga pieces.
pub fn _story_rank(m: &Value) -> Option<i64> {
    let k = str_or_empty(m, "kind");
    let st = str_or_empty(m, "style");
    let txt = str_or_empty(m, "text");
    if k == "founding" {
        return Some(0);
    }
    let n = _sentences(&txt, &names_of(m)).len();
    let dlj = k.starts_with("death_") || k.starts_with("loss") || k.starts_with("join");
    if n < 2 || (clen(&txt) < STORY_MIN_CHARS && !dlj) {
        return None;
    }
    if dlj || matches!(st.as_str(), "saga_death" | "saga_loss" | "saga_recap" | "arc_new" | "arc_birth" | "arc_spore") {
        return Some(0);
    }
    if MINOR_STYLES.contains(&st.as_str()) {
        return None;
    }
    if st.starts_with("arc_") || st.starts_with("saga_") {
        return Some(1);
    }
    None
}

// py: clan_story.py:1183 story_rows
/// Every row gets m["card"]: True = a story, False = a one-liner for the timeline between the stories. Returns the same rows (copies) in the same order.
pub fn story_rows(rows: &[Value]) -> Vec<Value> {
    let mut days: Vec<((i64, i64), Vec<&Value>)> = Vec::new();
    for m in rows {
        let key = (if truthy(vget(m, "week")) { int_of(vget(m, "week")) } else { 0 }, if truthy(vget(m, "day")) { int_of(vget(m, "day")) } else { 0 });
        match days.iter_mut().find(|(k, _)| *k == key) {
            Some((_, g)) => g.push(m),
            None => days.push((key, vec![m])),
        }
    }
    let mut demote: Vec<Value> = Vec::new();
    for (_k, grp) in &days {
        let mut ranked: Vec<&&Value> = grp.iter().filter(|m| !matches!(_story_rank(m), None | Some(0))).collect();
        let order_of = |m: &Value| -> usize {
            let sty = str_or_empty(m, "style");
            STORY_ORDER.iter().position(|o| sty.starts_with(o)).unwrap_or(99)
        };
        ranked.sort_by_key(|m| (order_of(m), -vi(m, "id")));
        for m in ranked.iter().skip(DAY_STORIES) {
            demote.push(vget(m, "id").clone());
        }
    }
    let mut out = Vec::new();
    for m in rows {
        let card = _story_rank(m).is_some() && !demote.contains(vget(m, "id"));
        let mut mm = m.clone();
        if card {
            mm["card"] = Value::Bool(true);
        } else {
            mm["card"] = Value::Bool(false);
            mm["line"] = Value::String(line_text(m));
        }
        out.push(mm);
    }
    out
}

// py: clan_story.py:1203 line_text
/// The timeline line of a row: its first sentence(s) in one row of at most LINE_MAX chars.
pub fn line_text(m: &Value) -> String {
    let mut raw = str_or_empty(m, "text");
    if let Some(c) = LINE_HEAD_RE.captures(&raw) {
        if let Some(g) = c.get(2) {
            raw = raw[g.start()..].to_string();
        }
    }
    let mut ss = _sentences(&raw, &names_of(m));
    if ss.is_empty() {
        ss = vec![raw.clone()];
    }
    let mut t = ss[0].clone();
    for x in &ss[1..] {
        // whole sentences while they fit
        if clen(&t) + 1 + clen(x) > LINE_MAX {
            break;
        }
        t.push(' ');
        t.push_str(x);
    }
    let t: String = {
        let mut o = String::new();
        let mut in_run = false;
        for ch in t.chars() {
            if matches!(ch, '\r' | '\n' | '\t') {
                if !in_run {
                    o.push(' ');
                }
                in_run = true;
            } else {
                o.push(ch);
                in_run = false;
            }
        }
        o.trim_matches(py::is_py_space).to_string()
    };
    if clen(&t) <= LINE_MAX {
        return t;
    }
    let cut = head_chars(&t, LINE_MAX);
    let base = match cut.rfind(' ') {
        Some(i) => &cut[..i],
        None => cut,
    };
    format!("{}...", base.trim_end_matches([',', ';', ':']))
}

// py: clan_story.py:1221 journal
/// The journal model of the clan screens: [{"card": row, "lines": [rows], "more": n}] newest story first. A one-liner belongs to the story it FOLLOWS.
pub fn journal(rows: &[Value], max_lines: usize) -> Vec<Value> {
    let mut cards: Vec<&Value> = rows.iter().filter(|m| truthy(vget(m, "card"))).collect();
    cards.sort_by_key(|m| vi(m, "id"));
    if cards.is_empty() {
        return Vec::new();
    }
    let ids: Vec<i64> = cards.iter().map(|m| vi(m, "id")).collect();
    let mut lines: Vec<Vec<Value>> = vec![Vec::new(); cards.len()];
    let mut more: Vec<i64> = vec![0; cards.len()];
    let mut singles: Vec<&Value> = rows.iter().filter(|m| !truthy(vget(m, "card"))).collect();
    singles.sort_by_key(|m| vi(m, "id"));
    for m in singles {
        let mid = vi(m, "id");
        let pos = ids.partition_point(|x| *x <= mid) as i64;
        let i = (pos - 1).max(0) as usize;
        if lines[i].len() < max_lines {
            lines[i].push(m.clone());
        } else {
            more[i] += 1;
        }
    }
    let mut out = Vec::new();
    for i in (0..cards.len()).rev() {
        let mut e = Map::new();
        e.insert("card".into(), cards[i].clone());
        e.insert("lines".into(), Value::Array(lines[i].clone()));
        e.insert("more".into(), Value::from(more[i]));
        out.push(Value::Object(e));
    }
    out
}

// py: clan_story.py:1241 latest
/// For the in-game toast widget: the newest `n` lines with id > since, OLDEST first, text cut to one line.
pub fn latest(c: &Connection, tid: &str, n: i64, since: i64, lang: &str) -> Vec<Value> {
    let rows = read_feed(c, tid, n, since, lang, false, false, false);
    rows.iter()
        .rev()
        .map(|m| {
            let mut o = Map::new();
            for k in ["id", "day", "week", "kind", "mood", "faction"] {
                o.insert(k.into(), vget(m, k).clone());
            }
            o.insert("text".into(), Value::String(short_text(vs(m, "text"), WIDGET_MAX)));
            Value::Object(o)
        })
        .collect()
}

// py: clan_story.py:1249 read_people
/// Roster summary for the CAMP view (alive members): name, role, gender, state, mood, illness. Plus morale. Null when there is no story.
pub fn read_people(c: &Connection, tid: &str, limit: usize, lang: &str) -> Value {
    let Some(st) = _load(c, tid) else { return Value::Object(Map::new()) };
    let fk = fkey_of(&st);
    let pl = pln::norm_lang(lang) == "pl";
    let mut people = Vec::new();
    for p in people_of(&st).iter().take(limit) {
        let il = illness(vs(p, "il"));
        let role = ROLES.iter().find(|r| r.key == vs(p, "r"));
        let mut e = Map::new();
        e.insert("name".into(), vget(p, "n").clone());
        e.insert("gender".into(), vget(p, "g").clone());
        e.insert("role".into(), jstr(role.map_or("", |r| if fk == "W" { r.wlf } else { r.sera })));
        e.insert("state".into(), jstr(match vs(p, "s") {
            "h" => "healthy",
            "u" => "hungry",
            _ => "sick",
        }));
        e.insert("mood".into(), jstr(mood_of(&st, p)));
        e.insert("illness".into(), jstr(il.map_or("", |i| i.name)));
        if pl {
            if let Some(Value::Object(l)) = pln::people_labels(fk, vs(p, "g"), vs(p, "r"), vs(p, "s"), mood_of(&st, p), vs(p, "il")) {
                for (k, v) in l {
                    e.insert(k, v);
                }
            }
        }
        people.push(Value::Object(e));
    }
    let morale_v = vi(&st, "morale");
    let mut morale = Map::new();
    morale.insert("value".into(), vget(&st, "morale").clone());
    let label = morale_label(morale_v);
    morale.insert("label".into(), jstr(label));
    if pl {
        morale.insert("text".into(), Value::String(pln::morale_text(label)));
    }
    let dead = dead_of(&st);
    let from = dead.len().saturating_sub(8);
    let dead_v: Vec<Value> = dead[from..]
        .iter()
        .map(|d| {
            let mut m = Map::new();
            m.insert("name".into(), vget(d, "n").clone());
            m.insert("day".into(), vget(d, "d").clone());
            Value::Object(m)
        })
        .collect();
    let mut out = Map::new();
    out.insert("morale".into(), Value::Object(morale));
    out.insert("people".into(), Value::Array(people));
    out.insert("alive".into(), Value::from(people_of(&st).len() as i64));
    out.insert("dead".into(), Value::Array(dead_v));
    Value::Object(out)
}

// py: clan_story.py:1271 widget_text
/// One in-game line: <= WIDGET_MAX chars, <= 150 bytes, no '|' and no newline (the wire format of op 0xB4).
pub fn widget_text(text: &str) -> String {
    let mut t = short_text(&text.replace('\n', " ").replace('|', "/"), WIDGET_MAX);
    while t.len() > 150 {
        let n = clen(&t);
        let head: String = t.chars().take(n.saturating_sub(2)).collect();
        t = format!("{}…", head.trim_end_matches(py::is_py_space));
    }
    t
}

// py: clan_story.py:1290 _plural
fn _plural(t: &str) -> bool {
    GROUP_RE.is_match(t) || PL_GROUP_RE.is_match(t)
}

static SOLO_DEATH: LazyLock<&'static Value> = LazyLock::new(|| tb("clan_story", "_SOLO_DEATH"));

// py: clan_story.py:1294 _widget_keep
/// The rows of the stored feed the widget may show for the clan state st (alive subjects only, voice that fits the clan's size, this run only).
fn _widget_keep(c: &Connection, tid: &str, st: &Value, rows: Vec<Value>, lang: &str) -> Vec<Value> {
    let alive: HashSet<String> = people_of(st).iter().map(|p| vs(p, "n").to_string()).collect();
    let r0: i64 = c
        .query_row("SELECT MAX(id) FROM clan_feed WHERE token_id=? AND kind='founding'", [tid], |r| r.get::<_, Option<i64>>(0))
        .ok()
        .flatten()
        .unwrap_or(0);
    let mut lids: HashMap<i64, Option<String>> = HashMap::new();
    let minid = rows.iter().map(|m| vi(m, "id")).min().unwrap_or(0);
    if let Ok(mut q) = c.prepare("SELECT id,lid FROM clan_feed WHERE token_id=? AND id>=?") {
        if let Ok(it) = q.query_map(params![tid, minid], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, Option<String>>(1)?))) {
            for x in it.flatten() {
                lids.insert(x.0, x.1);
            }
        }
    }
    let n_alive = alive.len();
    let pl_lang = pln::norm_lang(lang) == "pl";
    let mut out = Vec::new();
    for mut m in rows {
        let k = str_or_empty(&m, "kind");
        if vi(&m, "id") < r0 {
            continue;
        }
        let voice = voice_of_lid(lids.get(&vi(&m, "id")).and_then(|x| x.as_deref()));
        let text = str_or_empty(&m, "text");
        // the narrator's voice must fit the clan's size NOW
        if voice == "solo" {
            if n_alive == 1 {
                out.push(m);
            }
            continue;
        }
        if voice == "duo" {
            if n_alive == 2 {
                out.push(m);
            }
            continue;
        }
        if n_alive == 1 && FEELING_KINDS.contains(k.as_str()) {
            continue;
        }
        if n_alive == 1 && _plural(&text) {
            let parts: Vec<&str> = split_after_punct(&text).into_iter().filter(|x| !x.is_empty()).collect();
            if parts.is_empty() || _plural(parts[0]) {
                let nm = names_of(&m);
                if !(k.starts_with("death") && !nm.is_empty()) {
                    continue;
                }
                let g = dead_of(st).iter().find(|d| vs(d, "n") == nm[0]).map(|d| vget(d, "g").clone()).unwrap_or_else(|| jstr("m"));
                let col = if !pl_lang {
                    0
                } else if g.as_str() == Some("f") {
                    2
                } else {
                    1
                };
                let tbl = *SOLO_DEATH;
                let row = tbl.get(k.as_str()).or_else(|| tbl.get("death"));
                let tpl = row.and_then(|r| r.get(col)).and_then(|x| x.as_str()).unwrap_or("");
                m["text"] = Value::String(tpl.replace("{A}", &nm[0]));
                out.push(m);
                continue;
            }
            let kept: Vec<&str> = parts.iter().copied().filter(|x| !_plural(x)).collect();
            m["text"] = Value::String(kept.join(" "));
        }
        if n_alive == 2 && CROWD_RE.is_match(&text) {
            continue;
        }
        if !FEELING_KINDS.contains(k.as_str()) {
            // a happening: deaths, getting sick / worse / better, joins, events, founding
            out.push(m);
            continue;
        }
        let nm = names_of(&m);
        let keep = if k == "ticker" { nm.is_empty() || nm.iter().all(|x| alive.contains(x)) } else { nm.is_empty() || alive.contains(&nm[0]) };
        if keep {
            out.push(m);
        }
    }
    out
}

// py: clan_story.py:1348 pick_for_widget
/// Legacy per-reader picker: up to n of the newest `window` messages, newest unseen first. Returns [(id, text)] oldest first.
pub fn pick_for_widget(c: &Connection, tid: &str, seen: &mut HashSet<i64>, n: usize, window: i64, lang: &str) -> Vec<(i64, String)> {
    let mut rows = read_feed(c, tid, window, 0, lang, false, false, false);
    if let Some(st) = _load(c, tid) {
        rows = _widget_keep(c, tid, &st, rows, lang);
    }
    if rows.is_empty() {
        return Vec::new();
    }
    let mut fresh: Vec<&Value> = rows.iter().filter(|m| !seen.contains(&vi(m, "id"))).collect();
    if fresh.is_empty() {
        seen.clear();
        fresh = rows.iter().collect();
    }
    let mut take: Vec<&Value> = fresh.into_iter().take(n).collect();
    take.sort_by_key(|m| vi(m, "id"));
    for m in &take {
        seen.insert(vi(m, "id"));
    }
    if seen.len() > 200 {
        seen.clear();
    }
    take.iter().map(|m| (vi(m, "id"), widget_text(vs(m, "text")))).collect()
}

// ------------------------------------------------------------------ the menu ticker
// py: clan_story.py:1389 slot_of
pub fn slot_of(now: Option<f64>) -> i64 {
    let t = now.unwrap_or_else(clock::now);
    (t / SLOT_S as f64).floor() as i64
}

// py: clan_story.py:1393 ensure_roster
/// The clan's story state, created from the clan's current numbers when missing or of another faction (additive: an existing roster is never touched,
/// no feed message is written). None for a player without a WLF / Seraphite clan (or an empty one). save=False (the read-only view): built in memory.
pub fn ensure_roster(c: &Connection, tid: &str, save: bool) -> Option<Value> {
    let cln = cl::load(c, tid).ok()?;
    let faction = vi(&cln, "faction");
    if faction != cl::WLF && faction != cl::SERAPHITES {
        return None;
    }
    let st0 = _load(c, tid);
    if let Some(s) = &st0 {
        if vi(s, "fac") == faction && !people_of(s).is_empty() {
            return st0;
        }
    }
    let (h, u, s) = (vi(&cln, "healthy"), vi(&cln, "hungry"), vi(&cln, "sick"));
    if h + u + s <= 0 {
        return None;
    }
    if save {
        ensure(c).ok()?;
    }
    let n = st0.as_ref().map_or(0, |o| vi(o, "n"));
    let mut st = new_state(tid, faction, st0.as_ref().map_or(1, |o| vi(o, "run") + 1));
    st["n"] = Value::from(n);
    st["day"] = Value::from(cln.get("day").map(int_of).unwrap_or(0));
    st["boot"] = Value::from(1);
    let mut rng = _rng(&st, "lazy");
    let day = vi(&st, "day");
    _populate(&mut rng, &mut st, h, u, s, day);
    if save {
        _save(c, tid, &st).ok()?;
    }
    Some(st)
}

// py: clan_story.py:1427 rich_for
/// [(English template, pool)] of the good-state lines person p can be the subject of right now.
pub fn rich_for(st: &Value, p: &Value, fk: &str) -> Vec<(&'static str, &'static str)> {
    let pid = vi(p, "i");
    let others = people_of(st).iter().filter(|q| vi(q, "i") != pid && vs(q, "s") == "h").count();
    let two = people_of(st).len() == 2;
    let mut out = Vec::new();
    for (scope, en, letters) in RICH_ROWS.iter() {
        if *scope != "*" && *scope != fk {
            continue;
        }
        if (letters.contains(&'B') && others == 0) || (letters.contains(&'C') && others < 2) || (letters.contains(&'D') && dead_of(st).is_empty()) {
            continue;
        }
        if fk == "S" && TICKER_NOT_SERA.iter().any(|k| en.contains(k)) {
            continue;
        }
        if two && CROWD_RE.is_match(en) {
            continue;
        }
        out.push((*en, "t:g"));
    }
    out
}

// py: clan_story.py:1446 _bare_2013_healthy
/// The 2013 healthy lines that say something (not the 13 bare "X is healthy.") - the fallback when no rich row fits.
fn _bare_2013_healthy(fk: &str, two: bool) -> Vec<&'static str> {
    let mut pool: Vec<&'static str> = Vec::new();
    for t in TICKER_HEALTHY.iter() {
        if *t != "{A} is healthy." && !pool.contains(t) {
            pool.push(t);
        }
    }
    if fk == "S" {
        pool.retain(|t| !TICKER_NOT_SERA.iter().any(|k| t.contains(k)));
    }
    if two {
        pool.retain(|t| !CROWD_RE.is_match(t));
    }
    pool
}

/// The sick / hungry pool of the 2013 ticker (faction- and size-filtered) with its pool id.
fn state_pool(fk: &str, state: &str, two: bool) -> (Vec<&'static str>, &'static str) {
    let (src, pid): (&Vec<&'static str>, &'static str) = if state == "s" { (&TICKER_SICK, "t:s") } else { (&TICKER_HUNGRY, "t:u") };
    let mut pool_t: Vec<&'static str> = src.clone();
    if fk == "S" {
        pool_t.retain(|t| !TICKER_NOT_SERA.iter().any(|k| t.contains(k)));
    }
    if two {
        pool_t.retain(|t| !CROWD_RE.is_match(t));
    }
    (pool_t, pid)
}

/// B / C / D of a ticker template: the healthy companions (shuffled with `rng`) and the latest dead.
fn fill_companions(rng: &mut PyRandom, st: &Value, p: &Value, t: &str, people: &mut Value) -> bool {
    let letters = pln::tpl_letters(t);
    if letters.contains(&'B') || letters.contains(&'C') {
        let pid = vi(p, "i");
        let mut others: Vec<Value> = people_of(st).iter().filter(|q| vi(q, "i") != pid && vs(q, "s") == "h").cloned().collect();
        if combat_en_pl(t) {
            // a fight line names grown people only
            let f: Vec<Value> = others.iter().filter(|q| kd::can_fight(q)).cloned().collect();
            if !f.is_empty() {
                others = f;
            }
        }
        rng.shuffle(&mut others);
        match others.first() {
            Some(b) => people["B"] = b.clone(),
            None => return false,
        }
        if letters.contains(&'C') {
            match others.get(1) {
                Some(cc) => people["C"] = cc.clone(),
                None => return false,
            }
        }
    }
    if letters.contains(&'D') && !dead_of(st).is_empty() {
        let dd = dead_of(st).last().expect("dead");
        people["D"] = dead_person(dd);
    }
    true
}

// py: clan_story.py:1456 ticker_for
/// One ticker line about p's real state: (English template, pool id, people dict). `avoid` = template ids (line_id) not to repeat now.
pub fn ticker_for(rng: &mut PyRandom, st: &Value, p: &Value, avoid: &HashSet<String>) -> (&'static str, &'static str, Value) {
    let fk = fkey_of(st);
    let two = people_of(st).len() == 2;
    if vs(p, "s") == "s" && rng.random() < ticker_rare_chance() && !kd::is_kid(p) {
        return (*TICKER_RARE_SICK, "t:r", obj1("A", p));
    }
    let mut cands: Vec<(&'static str, &'static str)>;
    if vs(p, "s") == "h" {
        cands = rich_for(st, p, fk);
        if cands.is_empty() {
            cands = _bare_2013_healthy(fk, two).into_iter().map(|t| (t, "t:h")).collect();
        }
    } else {
        let (pool_t, pid) = state_pool(fk, vs(p, "s"), two);
        cands = pool_t.into_iter().map(|t| (t, pid)).collect();
    }
    let plain = kid_plain(vs(p, "s"));
    let ok: Vec<(&'static str, &'static str)> = cands.iter().copied().filter(|x| _companion_ok(st, p, x.0)).collect();
    cands = if ok.is_empty() { vec![plain] } else { ok };
    if kd::is_kid(p) {
        // a kid's ticker line is never combat, raid, patrol or weapon lore
        let ok: Vec<(&'static str, &'static str)> = cands.iter().copied().filter(|x| !combat_en_pl(x.0)).collect();
        cands = if ok.is_empty() { vec![plain] } else { ok };
    }
    let fresh: Vec<(&'static str, &'static str)> = cands.iter().copied().filter(|x| !avoid.contains(&line_id(x.0))).collect();
    let pickfrom = if fresh.is_empty() { &cands } else { &fresh };
    let (t, tpool) = pickfrom[rchoice_idx(rng, pickfrom.len())];
    let mut people = obj1("A", p);
    // (Python raises IndexError when a template needs a companion that is missing: not reachable after _companion_ok)
    fill_companions(rng, st, p, t, &mut people);
    (t, tpool, people)
}

// py: clan_story.py:1497 _sym
pub fn _sym(rng: &mut PyRandom, st: &Value) -> Value {
    let fk = fkey_of(st);
    let f = faction(fk).expect("faction");
    let mut m = Map::new();
    m.insert("f".into(), jstr(fk));
    m.insert("day".into(), vget(st, "day").clone());
    m.insert("week".into(), Value::from(cl::week_day(vi(st, "day")).0));
    m.insert("place".into(), jstr(rchoice(rng, &f.places).copied().unwrap_or("")));
    m.insert("inf".into(), jstr(rchoice(rng, &INFECTED).copied().unwrap_or("")));
    Value::Object(m)
}

/// One candidate line of the lone survivor: (pool name, template, people, line key)
struct SoloCand {
    pool: &'static str,
    tpl: &'static str,
    people: Value,
    key: String,
}

// py: clan_story.py:1502 _solo_candidates
/// Every diary line the ONE survivor p could say right now. Ticker lines of the state, the quiet / lonely / faction-flavour lines, the mood of the day,
/// and grief for the dead (one dead member at a time, by cause).
fn _solo_candidates(rng: &mut PyRandom, st: &Value, p: &Value, fk: &str) -> Option<Vec<SoloCand>> {
    let morale = vi(st, "morale");
    let mut out: Vec<SoloCand> = Vec::new();
    let mut add = |pairs: &[(&'static str, &'static str)], extra: Option<&Value>, tag: &str| -> Option<()> {
        for (sc, t) in pairs {
            if (*sc == "*" || *sc == fk) && kd::kid_safe(t, &obj1("A", p), &[_pl_twin(t)]) {
                let mut people = Map::new();
                if let Some(Value::Object(e)) = extra {
                    for (k, v) in e {
                        people.insert(k.clone(), v.clone());
                    }
                }
                people.insert("A".into(), p.clone());
                let key = format!("{}{}", line_id(t), if t.contains("{D}") { tag } else { "" });
                out.push(SoloCand { pool: TPL_POOL.get(t).copied()?, tpl: t, people: Value::Object(people), key });
            }
        }
        Some(())
    };
    add(solo_ticker(vs(p, "s")), None, "")?;
    add(SOLO.get("calm")?, None, "")?;
    match vs(p, "s") {
        "u" => {
            add(SOLO.get("hunger")?, None, "")?;
            add(SOLO.get("starving")?, None, "")?;
        }
        "s" => {
            add(SOLO.get("sick")?, None, "")?;
            if vi(p, "is") >= 1 {
                add(SOLO.get("worse")?, None, "")?;
                add(SOLO.get("medicine")?, None, "")?;
            }
        }
        _ => {
            add(SOLO.get("hope")?, None, "")?;
            if morale >= 60 {
                add(SOLO.get("plenty")?, None, "")?;
            }
            if morale <= 30 {
                add(SOLO.get("despair")?, None, "")?;
            }
        }
    }
    let dead = dead_of(st);
    let from = dead.len().saturating_sub(8);
    let dead = &dead[from..];
    if !dead.is_empty() {
        let dd = &dead[rchoice_idx(rng, dead.len())];
        let d = obj1("D", &dead_person(dd));
        let cause = vs(dd, "c");
        let kind = if matches!(cause, "mercy" | "bite_kin") { "grief_mercy" } else { "grief" };
        add(&solo_pool(kind, Some(cause))?, Some(&d), vs(dd, "n"))?;
        if vs(p, "s") == "h" {
            add(SOLO.get("hope_after")?, Some(&d), vs(dd, "n"))?;
        }
        if truthy(vget(p, "gl")) {
            add(SOLO.get("guilt")?, Some(&d), vs(dd, "n"))?;
        }
    }
    Some(out)
}

// py: clan_story.py:1537 _fp
/// The part of the clan state a live line depends on, as a short string.
fn _fp(st: &Value) -> String {
    let mut raw: Vec<String> = Vec::new();
    for p in people_of(st) {
        raw.push(format!("{}{}{}{}{}", vi(p, "i"), vs(p, "s"), vs(p, "il"), vi(p, "is"), if truthy(vget(p, "gl")) { 1 } else { 0 }));
    }
    let mut s = raw.join(",");
    s.push('/');
    let dead = dead_of(st);
    let from = dead.len().saturating_sub(8);
    let ds: Vec<String> = dead[from..].iter().map(|d| format!("{}{}", pystr(vget(d, "n")), d.get("c").map(pystr).unwrap_or_default())).collect();
    s.push_str(&ds.join(","));
    s.push_str(&format!("/{}", vi(st, "morale")));
    format!("{:08x}", t2r_clanstory::crc::crc32(s.as_bytes()))
}

/// An item of a ticker class: (key, template, pool, people when the item carries its own)
#[derive(Clone)]
struct Item {
    key: String,
    tpl: &'static str,
    pool: &'static str,
    people: Option<Value>,
}

// py: clan_story.py:1548 _perm
/// Epoch e of the class's shuffled template cycle: a pure function of (items, seed, e).
fn _perm(items: &[Item], seed: &str, e: usize) -> Vec<Item> {
    let mut r: Vec<Item> = items.to_vec();
    PyRandom::from_str(&format!("{}:{}", seed, e)).shuffle(&mut r);
    r
}

// py: clan_story.py:1561 _cycle_pick
/// The template for line number k of a class: walk the class's pool in shuffled epochs of len(items) (no template twice inside an epoch) and, at an epoch
/// border, swap the head templates that were in the previous epoch's tail with templates from the middle of the epoch.
fn _cycle_pick(items: &[Item], seed: &str, k: usize) -> Item {
    let m = items.len();
    if m == 1 {
        return items[0].clone();
    }
    let (e, j) = (k / m, k % m);
    let mut cur = _perm(items, seed, e);
    let w = WINDOW.min((m - 1) / 3);
    if e == 0 || w == 0 {
        return cur[j].clone();
    }
    let prev = _perm(items, seed, e - 1);
    let tail: HashSet<String> = prev[prev.len() - w..].iter().map(|x| x.key.clone()).collect();
    let mut t = w;
    for jj in 0..w {
        if tail.contains(&cur[jj].key) {
            while t < m - w && tail.contains(&cur[t].key) {
                t += 1;
            }
            if t >= m - w {
                break;
            }
            cur.swap(jj, t);
            t += 1;
        }
    }
    cur[j].clone()
}

// py: clan_story.py:1587 _class_items
/// The template pool (key, template, pool id) of the class person p belongs to right now; the same for every member of that class.
fn _class_items(st: &Value, p: &Value, fk: &str) -> Vec<Item> {
    let two = people_of(st).len() == 2;
    let c: Vec<(&'static str, &'static str)> = if vs(p, "s") == "h" {
        let r = rich_for(st, p, fk);
        if r.is_empty() {
            _bare_2013_healthy(fk, two).into_iter().map(|t| (t, "t:h")).collect()
        } else {
            r
        }
    } else {
        let (pool_t, pid) = state_pool(fk, vs(p, "s"), two);
        pool_t.into_iter().map(|t| (t, pid)).collect()
    };
    c.into_iter().map(|(t, pid)| Item { key: line_id(t), tpl: t, pool: pid, people: None }).collect()
}

/// A generated ticker line, language-neutral.
struct Rec {
    tpl: &'static str,
    pool: &'static str,
    people: Value,
    sym: Value,
}

fn plain_item(state: &str) -> Item {
    let (t, pid) = kid_plain(state);
    Item { key: line_id(t), tpl: t, pool: pid, people: None }
}

// py: clan_story.py:1603 _gen_line
/// Line number k (= slot * LIVE_N + index) of the clan's ticker, language-neutral. Subjects go round-robin over a shuffled roster per round; the template of
/// the line is the next one of the subject's class cycle (_cycle_pick). None = Python raised.
fn _gen_line(st: &Value, tid: &str, k: usize) -> Option<Rec> {
    let fk = fkey_of(st);
    let base = format!("{}:{}:{}:{}", tid, pystr(vget(st, "run")), _fp(st), vi(st, "day"));
    let n = people_of(st).len();
    if n == 0 {
        return None;
    }
    let (rnd, idx) = (k / n, k % n);
    let mut order: Vec<Value> = people_of(st).to_vec();
    PyRandom::from_str(&format!("order:{}:{}", base, rnd)).shuffle(&mut order);
    let p = order[idx].clone();
    let mut rng = PyRandom::from_str(&format!("line:{}:{}", base, k));
    let mut sym = _sym(&mut rng, st);
    if n == 1 {
        let cands = _solo_candidates(&mut PyRandom::from_str(&format!("solo:{}", base)), st, &p, fk)?;
        let items: Vec<Item> = cands.into_iter().map(|c| Item { key: c.key, tpl: c.tpl, pool: c.pool, people: Some(c.people) }).collect();
        if items.is_empty() {
            return None;
        }
        let it = _cycle_pick(&items, &format!("solo:{}", base), k);
        sym["ill"] = if truthy(vget(&p, "il")) { vget(&p, "il").clone() } else { jstr("none") };
        return Some(Rec { tpl: it.tpl, pool: it.pool, people: it.people.unwrap_or(Value::Null), sym });
    }
    if vs(&p, "s") == "s" && rng.random() < ticker_rare_chance() && !kd::is_kid(&p) {
        return Some(Rec { tpl: *TICKER_RARE_SICK, pool: "t:r", people: obj1("A", &p), sym });
    }
    let mut items = _class_items(st, &p, fk);
    let mut cls = vs(&p, "s").to_string();
    let last_kid = dead_of(st).last().map_or(false, |d| vs(d, "r") == "kid");
    if people_of(st).iter().any(kd::is_kid) || last_kid {
        let ok: Vec<Item> = items.iter().filter(|x| _companion_ok(st, &p, x.tpl)).cloned().collect();
        items = if ok.is_empty() { vec![plain_item(vs(&p, "s"))] } else { ok };
        cls.push('c');
    }
    if kd::is_kid(&p) {
        // a kid's lines are never combat, raid, patrol or weapon lore (a class of their own)
        let ok: Vec<Item> = items.iter().filter(|x| !combat_en_pl(x.tpl)).cloned().collect();
        items = if ok.is_empty() { vec![plain_item(vs(&p, "s"))] } else { ok };
        cls.push('k');
    }
    if items.is_empty() {
        return None;
    }
    let it = _cycle_pick(&items, &format!("cls{}:{}", cls, base), k);
    let mut people = obj1("A", &p);
    if !fill_companions(&mut rng, st, &p, it.tpl, &mut people) {
        return None;
    }
    Some(Rec { tpl: it.tpl, pool: it.pool, people, sym })
}

// py: clan_story.py:1648 _slot_recs
/// The LIVE_N ticker lines of one time slot, language-neutral. A pure function of (clan key, state, slot): identical on every path / process.
fn _slot_recs(st: &Value, tid: &str, slot: i64) -> Option<Vec<Rec>> {
    (0..LIVE_N).map(|i| _gen_line(st, tid, (slot as usize).wrapping_mul(LIVE_N) + i)).collect()
}

// py: clan_story.py:1653 _render_rec
fn _render_rec(rec: &Rec, lang: &str) -> Option<String> {
    let mut text = render(rec.tpl, &rec.people, &expand_en(&rec.sym)?)?;
    if pln::norm_lang(lang) == "pl" {
        if let Some(t) = render_msg(&pln::lid(rec.pool, rec.tpl), &_params(&rec.people, &rec.sym), "pl") {
            if !t.is_empty() {
                text = t;
            }
        }
    }
    Some(text)
}

// py: clan_story.py:1664 live_lines
/// n generated ticker lines [(id, text)] about the clan's survivors for the time slot of `now` (roster created lazily when save): the SAME lines for the
/// same (clan, slot, language) whoever asks. Ids "l<slot><index>" are stable within the slot. Err = Python raised; [] without a clan.
pub fn live_lines(c: &Connection, tid: &str, n: usize, lang: &str, now: Option<f64>, save: bool) -> Result<Vec<(Value, String)>, ()> {
    let st = match ensure_roster(c, tid, save) {
        Some(s) if !people_of(&s).is_empty() => s,
        _ => return Ok(Vec::new()),
    };
    let slot = slot_of(now);
    let recs = _slot_recs(&st, tid, slot).ok_or(())?;
    let mut out = Vec::new();
    for (i, r) in recs.iter().take(n).enumerate() {
        let text = _render_rec(r, lang).ok_or(())?;
        out.push((Value::String(format!("l{}{:02}", slot.rem_euclid(100000), i)), widget_text(&text)));
    }
    Ok(out)
}

// py: clan_story.py:1676 _stored_rows
/// The stored day events the widget may show (newest first): the filter of pick_for_widget.
fn _stored_rows(c: &Connection, tid: &str, lang: &str) -> Vec<Value> {
    let rows: Vec<Value> = read_feed(c, tid, 24, 0, lang, false, false, false)
        .into_iter()
        .filter(|m| !str_or_empty(m, "style").starts_with("arc_"))
        .take(12)
        .collect();
    match _load(c, tid) {
        Some(st) => _widget_keep(c, tid, &st, rows, lang),
        None => rows,
    }
}

// py: clan_story.py:1685 pick_slot
/// Deterministic stored-event picks for a time slot [(id, text)] oldest first: events younger than FRESH_S first (newest first), the rest by a
/// rotation (slot * n) over the kept events.
pub fn pick_slot(c: &Connection, tid: &str, slot: i64, n: usize, lang: &str, now: Option<f64>) -> Vec<(Value, String)> {
    let rows = _stored_rows(c, tid, lang);
    if rows.is_empty() {
        return Vec::new();
    }
    let now = now.unwrap_or_else(clock::now);
    let ts_of = |m: &Value| -> f64 {
        let t = vget(m, "ts");
        if truthy(t) {
            t.as_f64().unwrap_or(0.0)
        } else {
            0.0
        }
    };
    let fresh: Vec<&Value> = rows.iter().filter(|m| now - ts_of(m) < FRESH_S).take(n).collect();
    let fresh_ids: Vec<i64> = fresh.iter().map(|m| vi(m, "id")).collect();
    let rest: Vec<&Value> = rows.iter().filter(|m| !fresh_ids.contains(&vi(m, "id"))).collect();
    let mut take: Vec<&Value> = fresh;
    if !rest.is_empty() && take.len() < n {
        let start = py::pymod(slot * n as i64, rest.len() as i64) as usize;
        for k in 0..(n - take.len()).min(rest.len()) {
            take.push(rest[(start + k) % rest.len()]);
        }
    }
    take.sort_by_key(|m| vi(m, "id"));
    take.iter().map(|m| (vget(m, "id").clone(), widget_text(vs(m, "text")))).collect()
}

// py: clan_story.py:1703 menu_lines
/// THE menu / ticker reply for a clan at a time: stored real events first (<= 3, pick_slot), then live ticker lines up to n in total. [(id, text)].
pub fn menu_lines(c: &Connection, tid: &str, n: usize, lang: &str, now: Option<f64>, save: bool) -> Vec<(Value, String)> {
    let al = alife_on();
    let mut out = pick_slot(c, tid, slot_of(now), n.min(if al { 2 } else { 3 }), lang, now);
    if out.len() < n {
        if al {
            // the simulated life: single-sentence facts about the people, expeditions, injuries, projects ...
            if let Some(st) = ensure_roster(c, tid, save) {
                if let Ok(extra) = ext::alife::ticker_lines(c, tid, &st, slot_of(now), ext::alife::TICKER_SHARE.min(n - out.len()), lang, now, save) {
                    out.extend(extra);
                }
            }
        }
        if let Ok(extra) = live_lines(c, tid, n.saturating_sub(out.len()), lang, now, save) {
            out.extend(extra);
        }
    }
    out.truncate(n);
    out
}

// py: clan_story.py:1723 live_for_widget
/// Op 0xB4 reply lines (`seen` is no longer used: the reply is a pure function of clan, time slot and language, see menu_lines).
pub fn live_for_widget(c: &Connection, tid: &str, n: usize, lang: &str, now: Option<f64>) -> Vec<(Value, String)> {
    menu_lines(c, tid, n, lang, now, true)
}

// py: clan_story_solo.py:22 CROWD_RE / GROUP_RE
/// Words of a crowd: never in a clan of two or one.
pub fn crowd_re() -> &'static Regex {
    &CROWD_RE
}

/// The plural voice: never in a clan of one.
pub fn group_re() -> &'static Regex {
    &GROUP_RE
}
