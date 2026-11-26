//! py: clan_loot.py - the clan loot model: what a raid really brings home (findings with finder, place, condition, hook). EN + PL.
//! It never changes a clan number. State lives in `st["ch"]` (`cs` of the Python is never passed: callees take it from `st`).
//! A Python exception is `Err(String)`. Data: clan_loot_data.py / clan_loot_data2.py via the generated tables.
use crate::clan::clan::{int_of, pystr, truthy, Counts};
use crate::clan::{clan_chronicle as ch, clan_kids as kd, clan_supply, data};
use crate::py;
use indexmap::IndexMap;
use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::PyRandom;

pub const GROUPS: [&str; 5] = ["food", "water", "med", "ammo", "mat"];
pub const DEP_AT: i64 = 4;
pub const STALE_DAYS: i64 = 14;

/// release/launcher/src/clan-icons.js
const LAUNCHER_ICONS: &str = "alcohol ammo arrow binding canister coins explosive food gasmask healthbar journal keepsake key killfeed-skull materials medicine molotov parts person pills pipebomb pistolammo rag rifleammo scissors shiv shotgunammo water wrench";

// ------------------------------------------------------------------ small helpers shared with clan_saga
pub(crate) fn re<T>(r: Result<T, t2r_clanstory::pyrand::RandError>) -> Result<T, String> {
    r.map_err(|e| format!("random: {:?}", e))
}
pub(crate) fn vi(v: &Value, k: &str) -> i64 {
    v.get(k).map(int_of).unwrap_or(0)
}
/// `str(v.get(k, ""))`-like: a JSON string raw, "" when absent.
pub(crate) fn vs<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("")
}
pub(crate) fn hv(parts: &[String]) -> u32 {
    ch::h(parts)
}
/// Python `"...".format(**kw)` for `{name}` fields (and `{{` / `}}`); `Err` = KeyError / ValueError.
pub(crate) fn pyformat(tpl: &str, args: &[(&str, &str)]) -> Result<String, String> {
    let mut out = String::with_capacity(tpl.len() + 32);
    let cs: Vec<char> = tpl.chars().collect();
    let mut i = 0;
    while i < cs.len() {
        let c = cs[i];
        if c == '{' {
            if i + 1 < cs.len() && cs[i + 1] == '{' {
                out.push('{');
                i += 2;
                continue;
            }
            let mut j = i + 1;
            let mut name = String::new();
            while j < cs.len() && cs[j] != '}' {
                name.push(cs[j]);
                j += 1;
            }
            if j >= cs.len() {
                return Err("ValueError: Single '{' encountered in format string".into());
            }
            match args.iter().find(|(k, _)| *k == name) {
                Some((_, v)) => out.push_str(v),
                None => return Err(format!("KeyError: {}", name)),
            }
            i = j + 1;
        } else if c == '}' {
            if i + 1 < cs.len() && cs[i + 1] == '}' {
                out.push('}');
                i += 2;
                continue;
            }
            return Err("ValueError: Single '}' encountered in format string".into());
        } else {
            out.push(c);
            i += 1;
        }
    }
    Ok(out)
}

fn d1(name: &str) -> &'static Value {
    data::t("clan_loot_data", name)
}
fn d2(name: &str) -> &'static Value {
    data::t("clan_loot_data2", name)
}
fn s_of(v: &Value) -> String {
    v.as_str().unwrap_or("").to_string()
}

// ------------------------------------------------------------------ data
pub struct Item {
    pub key: String,
    pub cat: String,
    pub tier: i64,
    pub rar: usize,
    pub qmin: i64,
    pub qmax: i64,
    pub val: i64,
    pub kinds: Vec<String>,
    pub icon: String,
    pub g: String,
    pub en1: String,
    pub enn: String,
    pub pln: String,
    pub pla: String,
    pub plf: String,
    pub plm: String,
}

/// `ITEMS`: the rows of clan_loot_data.ITEMS in file order.
pub static ITEMS: LazyLock<IndexMap<String, Item>> = LazyLock::new(|| {
    let mut m = IndexMap::new();
    let src = d1("ITEMS").as_str().unwrap_or("");
    for ln in py::strip(src).lines() {
        let f: Vec<&str> = ln.split('|').collect();
        if f.len() != 15 {
            continue;
        }
        let q: Vec<&str> = f[4].split('-').collect();
        let it = Item {
            key: f[0].to_string(),
            cat: f[1].to_string(),
            tier: f[2].parse().unwrap_or(0),
            rar: f[3].parse().unwrap_or(0),
            qmin: q.first().and_then(|x| x.parse().ok()).unwrap_or(0),
            qmax: q.get(1).and_then(|x| x.parse().ok()).unwrap_or(0),
            val: f[5].parse().unwrap_or(0),
            kinds: f[6].split(',').map(|x| x.to_string()).collect(),
            icon: f[7].to_string(),
            g: f[8].to_string(),
            en1: f[9].to_string(),
            enn: f[10].to_string(),
            pln: f[11].to_string(),
            pla: f[12].to_string(),
            plf: f[13].to_string(),
            plm: f[14].to_string(),
        };
        m.insert(it.key.clone(), it);
    }
    m
});

pub fn item(key: &str) -> Result<&'static Item, String> {
    ITEMS.get(key).ok_or_else(|| format!("KeyError: {}", key))
}

/// `GROUP[cat]` ("" for trinkets and books).
pub fn group_of(cat: &str) -> String {
    d1("GROUP").get(cat).map(s_of).unwrap_or_default()
}

pub fn tier_name(t: i64) -> String {
    d1("TIERS").get(t.max(0) as usize).map(s_of).unwrap_or_default()
}

pub struct Cond {
    pub en: String,
    pub pre: String,
    pub adj: String,
    pub q: i64,
    /// None = "*" (every category)
    pub cats: Option<Vec<String>>,
}

pub static CONDS: LazyLock<Vec<Cond>> = LazyLock::new(|| {
    d2("CONDS")
        .as_array()
        .map(|a| {
            a.iter()
                .map(|c| Cond {
                    en: s_of(&c[0]),
                    pre: s_of(&c[1]),
                    adj: s_of(&c[2]),
                    q: c[3].as_i64().unwrap_or(0),
                    cats: match &c[4] {
                        Value::Array(l) => Some(l.iter().map(s_of).collect()),
                        _ => None,
                    },
                })
                .collect()
        })
        .unwrap_or_default()
});

static MICRO: LazyLock<HashMap<String, Vec<(String, String)>>> = LazyLock::new(|| {
    d2("MICRO")
        .as_object()
        .map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_array().map(|a| a.iter().map(|p| (s_of(&p[0]), s_of(&p[1]))).collect()).unwrap_or_default())).collect())
        .unwrap_or_default()
});
static HOOKS: LazyLock<Vec<(String, String, String)>> =
    LazyLock::new(|| d2("HOOKS").as_array().map(|a| a.iter().map(|h| (s_of(&h[0]), s_of(&h[1]), s_of(&h[2]))).collect()).unwrap_or_default());
static HOOK_FORMS: LazyLock<Vec<Option<Vec<String>>>> = LazyLock::new(|| {
    d2("HOOK_FORMS")
        .as_array()
        .map(|a| a.iter().map(|x| x.as_array().map(|l| l.iter().map(s_of).collect())).collect())
        .unwrap_or_default()
});
static MAP_KINDS: LazyLock<HashMap<String, Vec<String>>> = LazyLock::new(|| {
    d2("MAP_KINDS").as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_array().map(|a| a.iter().map(s_of).collect()).unwrap_or_default())).collect()).unwrap_or_default()
});
static GENERIC_KINDS: LazyLock<Vec<String>> = LazyLock::new(|| d2("GENERIC_KINDS").as_array().map(|a| a.iter().map(s_of).collect()).unwrap_or_default());
static CAT_FORM: LazyLock<HashMap<String, String>> =
    LazyLock::new(|| d2("CAT_FORM").as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), s_of(v))).collect()).unwrap_or_default());
static FORM_OF: LazyLock<HashMap<String, String>> =
    LazyLock::new(|| d2("FORM_OF").as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), s_of(v))).collect()).unwrap_or_default());
static COND_FORMS: LazyLock<HashMap<String, Vec<String>>> = LazyLock::new(|| {
    d2("COND_FORMS").as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_array().map(|a| a.iter().map(s_of).collect()).unwrap_or_default())).collect()).unwrap_or_default()
});

fn names() -> Vec<&'static str> {
    data::strs(d2("NAMES"))
}
fn note_of(i: usize, en: bool) -> String {
    d2("NOTES").get(i).and_then(|p| p.get(if en { 0 } else { 1 })).map(s_of).unwrap_or_default()
}

pub fn launcher_icon(id: &str) -> bool {
    LAUNCHER_ICONS.split(' ').any(|x| x == id)
}

// ------------------------------------------------------------------ icons
// py: clan_loot.py:52 icon_wanted
pub fn icon_wanted(key: &str) -> Result<String, String> {
    Ok(item(key)?.icon.clone())
}

// py: clan_loot.py:56 icon_for
/// The icon id to show: the item's own icon when the launcher has it, else its category's.
pub fn icon_for(key: &str) -> Result<String, String> {
    let it = item(key)?;
    if launcher_icon(&it.icon) {
        Ok(it.icon.clone())
    } else {
        d1("FALLBACK_ICON").get(&it.cat).map(s_of).ok_or_else(|| format!("KeyError: {}", it.cat))
    }
}

// py: clan_loot.py:62 missing_icons
/// Icon ids the items would like that clan-icons.js lacks: {icon id: [item keys]}.
pub fn missing_icons() -> Value {
    let mut out: IndexMap<String, Vec<Value>> = IndexMap::new();
    for (k, it) in ITEMS.iter() {
        if !launcher_icon(&it.icon) {
            out.entry(it.icon.clone()).or_default().push(json!(k));
        }
    }
    Value::Object(out.into_iter().map(|(k, v)| (k, Value::Array(v))).collect())
}

// ------------------------------------------------------------------ phrases
// py: clan_loot.py:73 _art
fn art(w: &str) -> &'static str {
    match w.chars().next() {
        None => "an", // "" in "aeiou"
        Some(c) => {
            let l: String = c.to_lowercase().collect();
            if "aeiou".contains(l.as_str()) {
                "an"
            } else {
                "a"
            }
        }
    }
}

// py: clan_loot.py:77 _adj_forms
fn adj_forms(adj: &str, which: &str) -> String {
    let mut cs: Vec<char> = adj.chars().collect();
    cs.pop();
    let s: String = cs.into_iter().collect();
    match which {
        "m" => adj.to_string(),
        "f" => format!("{}ą", s),
        "n" | "few" => format!("{}e", s),
        _ => format!("{}ych", s),
    }
}

// py: clan_loot.py:82 _plural_kind
fn plural_kind(n: i64) -> &'static str {
    let few = (2..=4).contains(&py::pymod(n, 10)) && !(12..=14).contains(&py::pymod(n, 100));
    if few {
        "few"
    } else {
        "many"
    }
}

// py: clan_loot.py:87 item_phrase
/// '3 rusted cans of pineapple' / 'zardzewiałą puszkę ananasa' (accusative, the object of 'found').
pub fn item_phrase(key: &str, n: i64, cond: i64, lang: &str) -> Result<String, String> {
    let it = item(key)?;
    let mut n = n.max(1);
    if it.enn == "-" {
        n = 1;
    }
    let c = if cond >= 0 && (cond as usize) < CONDS.len() { Some(&CONDS[cond as usize]) } else { None };
    if lang == "pl" {
        let (noun, ad) = if n == 1 {
            (&it.pla, if it.g != "f" && it.g != "n" { "m".to_string() } else { it.g.clone() })
        } else {
            let k = plural_kind(n);
            (if k == "few" { &it.plf } else { &it.plm }, k.to_string())
        };
        let pre = match c {
            Some(c) => format!("{}{} ", c.pre, adj_forms(&c.adj, &ad)),
            None => String::new(),
        };
        return Ok(format!("{}{}{}", if n > 1 { format!("{} ", n) } else { String::new() }, pre, noun));
    }
    if n == 1 {
        let word = format!("{}{}", c.map(|c| format!("{} ", c.en)).unwrap_or_default(), it.en1);
        return Ok(format!("{} {}", art(&word), word));
    }
    Ok(format!("{} {}{}", n, c.map(|c| format!("{} ", c.en)).unwrap_or_default(), it.enn))
}

// py: clan_loot.py:108 noun_phrase
/// The bare name of the item (singular, nominative; EN with a/an).
pub fn noun_phrase(key: &str, lang: &str) -> Result<String, String> {
    let it = item(key)?;
    Ok(if lang == "pl" { it.pln.clone() } else { format!("{} {}", art(&it.en1), it.en1) })
}

// py: clan_loot.py:114 join_list
pub fn join_list(items: &[String], lang: &str) -> String {
    if items.len() <= 1 {
        return items.concat();
    }
    format!("{}{}{}", items[..items.len() - 1].join(", "), if lang == "pl" { " i " } else { " and " }, items[items.len() - 1])
}

// py: clan_loot.py:120 _cap
fn cap(s: &str) -> String {
    let mut it = s.chars();
    match it.next() {
        Some(c) => c.to_uppercase().collect::<String>() + it.as_str(),
        None => String::new(),
    }
}

// ------------------------------------------------------------------ fortune
fn f_of(v: &Value, d: f64) -> f64 {
    v.as_f64().unwrap_or(d)
}

// py: clan_loot.py:125 fortune
/// Momentum score -1..+1 of the clan; updates cs["fort"] once per day. `hist` None = the raid log `cs["log"]`. `day` / `morale` = st["day"] / st["morale"].
pub fn fortune_impl(day: i64, morale: i64, cs: &mut Value, hist: Option<&[Value]>) -> Result<f64, String> {
    let log_all: Vec<Value> = match hist {
        Some(h) => h.to_vec(),
        None => cs.get("log").and_then(|v| v.as_array()).cloned().unwrap_or_default(),
    };
    let log: &[Value] = &log_all[log_all.len().saturating_sub(7)..];
    if !cs.is_object() {
        return Err("TypeError: cs".into());
    }
    let co = cs.as_object_mut().ok_or("cs")?;
    let fo = co.entry("fort").or_insert_with(|| json!({"s": 0.0, "d": -1, "low": 0, "hi": 0}));
    if log.is_empty() {
        return Ok(fo.get("s").map(|v| f_of(v, 0.0)).unwrap_or(0.0));
    }
    let n = log.len() as f64;
    let mut wsum = 0i64;
    for e in log {
        wsum += match e.get("win") {
            Some(Value::Bool(true)) => 1,
            Some(Value::Bool(false)) => -1,
            _ => 0,
        };
    }
    let w = wsum as f64 / n;
    // Python >= 3.12 sum() of floats = Neumaier compensated summation (the test box and the host run 3.12)
    let (mut ssum, mut comp) = (0.0f64, 0.0f64);
    for e in log {
        let need = vi(e, "need");
        let x = (-1.0f64).max((1.0f64).min((vi(e, "sup") - need) as f64 / (need.max(1) as f64)));
        let t = ssum + x;
        if ssum.abs() >= x.abs() {
            comp += (ssum - t) + x;
        } else {
            comp += (x - t) + ssum;
        }
        ssum = t;
    }
    if comp != 0.0 && comp.is_finite() {
        ssum += comp;
    }
    let s = ssum / n;
    let m = (-1.0f64).max((1.0f64).min((morale - 50) as f64 / 50.0));
    let score = py::round_n(0.45 * w + 0.35 * s + 0.20 * m, 3);
    if fo.get("d").map(int_of) != Some(day) || fo.get("d").is_none() {
        let low = fo.get("low").map(int_of).ok_or("KeyError: low")?;
        let hi = fo.get("hi").map(int_of).ok_or("KeyError: hi")?;
        fo["low"] = json!(if score < -0.3 { low + 1 } else { 0 });
        fo["hi"] = json!(if score > 0.3 { hi + 1 } else { 0 });
        fo["d"] = json!(day);
    }
    fo["s"] = json!(score);
    Ok(score)
}

/// `fortune(st, cs, hist)` with the chronicle's call shape: `fo_st` = dict(st, morale=...) (day + morale are read from it), `cs` = st["ch"]. Returns the score as a JSON float.
pub fn fortune(fo_st: &Value, cs: &mut Value, hist: &[Value]) -> Result<Value, String> {
    let day = fo_st.get("day").map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
    let morale = fo_st.get("morale").map(int_of).unwrap_or(50);
    let s = fortune_impl(day, morale, cs, Some(hist))?;
    Ok(json!(s))
}

// py: clan_loot.py:145 base_tier
pub fn base_tier(score: f64) -> i64 {
    (0i64).max((4i64).min(2 + py::round0(2.0 * score)))
}

// py: clan_loot.py:149 roll_tier
pub fn roll_tier(rng: &mut PyRandom, base: i64) -> i64 {
    let r = rng.random();
    let t = if r < 0.6 {
        base
    } else if r < 0.8 {
        base - 1
    } else {
        base + 1
    };
    (0i64).max((4i64).min(t))
}

// py: clan_loot.py:157 update_moods
/// Depression counters (module doc of clan_status): once per day. Returns {member id: counter}.
pub fn update_moods_map(st: &mut Value) -> Result<Value, String> {
    let day = st.get("day").map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
    let people: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let cs = ch::ch_state(st);
    let co = cs.as_object_mut().ok_or("cs")?;
    let fo = co.entry("fort").or_insert_with(|| json!({"s": 0.0, "d": -1, "low": 0, "hi": 0})).clone();
    let alive: HashSet<String> = people.iter().map(|p| pystr(p.get("i").unwrap_or(&Value::Null))).collect();
    let dep = co.entry("dep").or_insert_with(|| json!({}));
    {
        let dm = dep.as_object_mut().ok_or("dep")?;
        let gone: Vec<String> = dm.keys().filter(|k| !alive.contains(*k)).cloned().collect();
        for k in gone {
            dm.shift_remove(&k);
        }
    }
    let mem = co.get("mem").filter(|v| truthy(v)).cloned().unwrap_or_else(|| json!({}));
    let dep = co.get_mut("dep").ok_or("dep")?;
    let dm = dep.as_object_mut().ok_or("dep")?;
    for p in &people {
        let key = pystr(p.get("i").unwrap_or(&Value::Null));
        let e = dm.entry(key.clone()).or_insert_with(|| json!({"n": 0, "d": -1, "up": -1, "rec": false}));
        if e.get("d").map(int_of) == Some(day) && e.get("d").is_some() {
            continue;
        }
        e["d"] = json!(day);
        let g = mem.get(&key).filter(|v| truthy(v)).and_then(|m| m.get("grief")).cloned().unwrap_or(Value::Null);
        let bad = p.get("s").and_then(|x| x.as_str()) == Some("u") || (truthy(&g) && g.get("until").map(int_of).unwrap_or(-1) >= day);
        let low = fo.get("low").map(int_of).unwrap_or(0);
        let fs = fo.get("s").map(|v| f_of(v, 0.0)).unwrap_or(0.0);
        let en = e.get("n").map(int_of).ok_or("KeyError: n")?;
        if low >= 2 && (bad || en > 0) && fs < -0.3 {
            e["n"] = json!(en + 1);
            e["rec"] = json!(false);
        } else if fs > 0.0 && en > 0 {
            let prev = en;
            let nn = (en - 2).max(0);
            e["n"] = json!(nn);
            e["rec"] = json!(nn > 0);
            if prev >= DEP_AT && nn < DEP_AT {
                e["up"] = json!(day);
            }
        }
    }
    let mut out = Map::new();
    for (k, v) in dm.iter() {
        out.insert(k.clone(), v.get("n").cloned().ok_or("KeyError: n")?);
    }
    Ok(Value::Object(out))
}

/// The chronicle's call shape (`update_moods(st, cs)`).
pub fn update_moods(st: &mut Value) -> Result<(), String> {
    update_moods_map(st).map(|_| ())
}

// ------------------------------------------------------------------ choosing
// py: clan_loot.py:187 kinds_of
pub fn kinds_of(mapname: &str) -> Vec<String> {
    let en = data::t("clan_chronicle_maps", "EN_SHORT").get(mapname).and_then(|v| v.as_str()).unwrap_or(mapname);
    MAP_KINDS.get(en).cloned().unwrap_or_else(|| GENERIC_KINDS.clone())
}

// py: clan_loot.py:192 affinity
pub fn affinity(it: &Item, mk: &[String]) -> f64 {
    if it.kinds.iter().any(|k| k == "*") {
        return 2.0;
    }
    let mut w = 0.2;
    for (i, k) in mk.iter().enumerate() {
        if it.kinds.contains(k) {
            w += 5.0 * if i == 0 { 2.0 } else { 1.0 };
        }
    }
    w
}

// py: clan_loot.py:202 candidates
/// Items of the categories at the nearest tier that has any (tier, tier-1, tier+1, ...), with their weights.
pub fn candidates(cats: &[&str], tier: i64, mk: &[String], rar_ok: usize, avoid: &HashSet<String>) -> (Vec<&'static Item>, Vec<f64>) {
    let mut fallback: Option<(Vec<&'static Item>, Vec<f64>)> = None;
    for dt in [0i64, -1, 1, -2, 2, -3, 3, -4, 4] {
        let t = tier + dt;
        if !(0..=4).contains(&t) {
            continue;
        }
        let pool: Vec<&'static Item> = ITEMS.values().filter(|it| cats.contains(&it.cat.as_str()) && it.tier == t && it.rar <= rar_ok).collect();
        if pool.is_empty() {
            continue;
        }
        let ws: Vec<f64> = pool.iter().map(|it| affinity(it, mk) * [1.0, 0.5, 0.2][it.rar.min(2)] * if avoid.contains(&it.key) { 0.15 } else { 1.0 }).collect();
        if fallback.is_none() {
            fallback = Some((pool.clone(), ws.clone()));
        }
        let mx = ws.iter().cloned().fold(f64::MIN, f64::max);
        if mx >= 1.0 {
            return (pool, ws);
        }
        if dt.abs() >= 2 {
            break;
        }
    }
    fallback.unwrap_or_default()
}

// py: clan_loot.py:228 form_of
/// What the thing is made of / how it is kept.
pub fn form_of(it: &Item) -> String {
    match FORM_OF.get(&it.key) {
        Some(f) if !f.is_empty() => f.clone(),
        _ => CAT_FORM.get(&it.cat).cloned().unwrap_or_default(),
    }
}

// py: clan_loot.py:233 cond_ok
pub fn cond_ok(it: &Item, ci: usize) -> bool {
    let c = &CONDS[ci];
    if data::set_has_str(d2("NO_COND"), &it.key) {
        return false;
    }
    if let Some(ban) = d2("COND_BAN").get(&it.key) {
        if data::set_has_str(ban, &c.en) {
            return false;
        }
    }
    let cat_ok = match &c.cats {
        None => true,
        Some(l) => l.iter().any(|x| *x == it.cat),
    };
    if !cat_ok {
        return false;
    }
    if !COND_FORMS.get(&c.en).map(|l| l.contains(&form_of(it))).unwrap_or(false) {
        return false;
    }
    c.en != "fully stocked" || it.key == "firstaid" || it.key == "trauma"
}

// py: clan_loot.py:244 hook_ok
pub fn hook_ok(it: &Item, hi: usize) -> bool {
    match HOOK_FORMS.get(hi) {
        Some(Some(forms)) => forms.contains(&form_of(it)),
        _ => true,
    }
}

// py: clan_loot.py:249 pick_cond
fn pick_cond(rng: &mut PyRandom, it: &Item, tier: i64) -> Result<i64, String> {
    let cs_: Vec<usize> = (0..CONDS.len()).filter(|&i| cond_ok(it, i)).collect();
    if cs_.is_empty() || rng.random() > 0.72 {
        return Ok(-1);
    }
    let wq: [[f64; 3]; 5] = [[3.0, 1.0, 0.2], [2.0, 1.5, 0.5], [1.0, 1.5, 1.0], [0.4, 1.0, 2.0], [0.1, 0.6, 3.0]];
    let row = wq.get(tier as usize).ok_or_else(|| format!("KeyError: {}", tier))?;
    let ws: Vec<f64> = cs_.iter().map(|&i| row[(CONDS[i].q + 1).clamp(0, 2) as usize]).collect();
    let ix = re(rng.choices_weighted(&ws, 1))?;
    Ok(cs_[ix[0]] as i64)
}

// py: clan_loot.py:257 pick_member
/// The finder, weighted by role fit and recency, never the same member twice in one raid while the roster allows it. `people` = st["people"]. Returns (name, gender).
fn pick_member(rng: &mut PyRandom, people: &[Value], it: &Item, lo: &mut Value, day: i64) -> Result<(String, String), String> {
    let ppl: Vec<&Value> = people.iter().filter(|p| kd::can_fight(p)).collect();
    if ppl.is_empty() {
        return Ok((String::new(), "n".into()));
    }
    let want: &[&str] = match it.cat.as_str() {
        "food" | "seed" | "anim" | "water" => &["cook", "scavenger"],
        "med" => &["medic"],
        "ammo" => &["guard", "scout"],
        "mat" | "tool" => &["mechanic", "scavenger"],
        "part" => &["mechanic", "guard"],
        "cloth" => &["scavenger"],
        "fuel" => &["mechanic", "scavenger"],
        "trink" | "book" => &["kid", "scavenger"],
        _ => &[],
    };
    let taken: HashSet<String> = lo.get("raid").and_then(|v| v.as_array()).map(|a| a.iter().map(pystr).collect()).unwrap_or_default();
    let mut pool: Vec<&Value> = ppl.iter().copied().filter(|p| !taken.contains(&pystr(p.get("n").unwrap_or(&Value::Null)))).collect();
    if pool.is_empty() {
        pool = ppl.clone();
    }
    let last = lo.get("finders").cloned().unwrap_or_else(|| json!({}));
    let mut ws: Vec<f64> = Vec::new();
    for p in &pool {
        let r = p.get("r").and_then(|x| x.as_str()).unwrap_or("");
        let mut fit = if want.contains(&r) { 3.0 } else { 1.0 };
        if r == "kid" && it.cat != "trink" && it.cat != "book" {
            fit = 0.25;
        }
        let nm = pystr(p.get("n").unwrap_or(&Value::Null));
        let lastd = last.get(&nm).map(int_of).unwrap_or(day - 12);
        let gap = 12i64.min(0i64.max(day - lastd));
        ws.push(fit * (0.4 + gap as f64 / 4.0));
    }
    let ix = re(rng.choices_weighted(&ws, 1))?[0];
    let p = pool[ix];
    let name = pystr(p.get("n").unwrap_or(&Value::Null));
    let lm = lo.as_object_mut().ok_or("lo")?;
    lm.entry("raid").or_insert_with(|| json!([])).as_array_mut().ok_or("raid")?.push(json!(name));
    lm.entry("finders").or_insert_with(|| json!({}))[name.as_str()] = json!(day);
    let g = p.get("g").cloned().unwrap_or_else(|| json!("n"));
    Ok((name, pystr(&g)))
}

// py: clan_loot.py:279 place_for
fn place_for(rng: &mut PyRandom, mapname: &str) -> String {
    let en = data::t("clan_chronicle_maps", "EN_SHORT").get(mapname).and_then(|v| v.as_str()).unwrap_or(mapname);
    let md = data::t("clan_saga_data", "MAPS").get(en).unwrap_or_else(|| data::t("clan_saga_data", "GENERIC"));
    match md.get("places").and_then(|p| p.as_array()) {
        Some(pl) if !pl.is_empty() => rng.choice(pl).map(s_of).unwrap_or_default(),
        _ => String::new(),
    }
}

// py: clan_loot.py:290 budget_of
/// Supplies per group of this raid: r["sup"]["got"], else derived from the engine's supplies and the match (clan_supply.intake). Returns (got, short) in GROUPS order.
fn budget_of(r: &Value, matchv: &Value) -> ([i64; 5], [i64; 5]) {
    let sp = r.get("sup");
    if let Some(sp) = sp.filter(|s| s.is_object() && s.get("got").map(|g| g.is_object()).unwrap_or(false)) {
        let mut got = [0i64; 5];
        let mut short = [0i64; 5];
        let sh = sp.get("short").filter(|v| truthy(v));
        for (i, g) in GROUPS.iter().enumerate() {
            got[i] = sp.get("got").and_then(|x| x.get(*g)).map(int_of).unwrap_or(0).max(0);
            short[i] = sh.and_then(|x| x.get(*g)).map(int_of).unwrap_or(0).max(0);
        }
        return (got, short);
    }
    let mi = |k: &str| -> i64 { matchv.get(k).filter(|v| truthy(v)).map(int_of).unwrap_or(0) };
    let mut c: Counts = HashMap::new();
    c.insert(1, mi("kills"));
    c.insert(2, mi("execs"));
    c.insert(3, mi("revives"));
    c.insert(4, mi("headshots"));
    c.insert(21, mi("downs"));
    let rv = |k: &str| -> i64 { r.get(k).filter(|v| truthy(v)).map(int_of).unwrap_or(0) };
    let got = clan_supply::intake(rv("supplies"), rv("parts"), &c, r.get("left").map(truthy).unwrap_or(false));
    (got, [0; 5])
}

// py: clan_loot.py:304 CATS_OF
fn cats_of(g: &str) -> &'static [(&'static str, f64)] {
    match g {
        "food" => &[("food", 8.0), ("seed", 1.0), ("anim", 1.0)],
        "water" => &[("water", 1.0)],
        "med" => &[("med", 1.0)],
        "ammo" => &[("ammo", 1.0)],
        _ => &[("mat", 3.0), ("tool", 3.0), ("part", 1.0), ("fuel", 1.0), ("cloth", 1.0)],
    }
}

fn setdef<'a>(m: &'a mut Map<String, Value>, k: &str, d: Value) -> &'a mut Value {
    m.entry(k.to_string()).or_insert(d)
}

// py: clan_loot.py:311 _recent
fn recent(cs: &mut Value) -> Result<&mut Value, String> {
    let co = cs.as_object_mut().ok_or("cs")?;
    let lo = setdef(co, "loot", json!({}));
    let lm = lo.as_object_mut().ok_or("TypeError: loot")?;
    setdef(lm, "recent", json!([]));
    setdef(lm, "seen", json!({}));
    setdef(lm, "leads", json!([]));
    Ok(lo)
}

type Find = Map<String, Value>;

fn fget_i(f: &Find, k: &str) -> i64 {
    f.get(k).map(int_of).unwrap_or(0)
}

// py: clan_loot.py:319 _make
/// One finding record of item `it` (quantity chosen by the caller in 'n' later).
fn make(rng: &mut PyRandom, it: &Item, tier: i64, mapname: &str, day: i64) -> Result<Find, String> {
    let c = pick_cond(rng, it, tier)?;
    let mut f = Find::new();
    f.insert("k".into(), json!(it.key));
    f.insert("n".into(), json!(1));
    f.insert("c".into(), json!(c));
    f.insert("m".into(), json!(mapname));
    f.insert("t".into(), json!(it.tier));
    f.insert("d".into(), json!(day));
    f.insert("w".into(), json!(""));
    f.insert("g".into(), json!("n"));
    let mk = kinds_of(mapname);
    let own: Vec<&String> = mk.iter().filter(|k| it.kinds.contains(k) || it.kinds.iter().any(|x| x == "*")).collect();
    let kdn: String = if own.is_empty() { re(rng.choice(&mk))?.clone() } else { (*re(rng.choice(&own))?).clone() };
    f.insert("kd".into(), json!(kdn));
    let nm = MICRO.get(&kdn).map(|v| v.len()).ok_or_else(|| format!("KeyError: {}", kdn))?;
    f.insert("mi".into(), json!(re(rng.randrange_to(nm as i64))?));
    let pl = if rng.random() < 0.5 { place_for(rng, mapname) } else { String::new() };
    f.insert("pl".into(), json!(pl));
    f.insert("v".into(), json!(re(rng.randrange_to(4))?));
    f.insert("h".into(), json!(-1));
    f.insert("ha".into(), json!(0));
    if rng.random() < 0.33 {
        let ok: Vec<usize> = (0..HOOKS.len()).filter(|&hi| hook_ok(it, hi)).collect();
        let h = *re(rng.choice(&ok))?;
        f.insert("h".into(), json!(h as i64));
        f.insert("ha".into(), json!(re(rng.randrange_to(1000))?));
    }
    Ok(f)
}

// py: clan_loot.py:341 _sent_key
fn sent_key(f: &Find) -> Result<String, String> {
    render_find_map(f, "en", true)
}

// py: clan_loot.py:345 _unique
/// Change the sentence variant / micro-location until the finding's sentence is not one told in the last 14 days (a few tries).
fn unique(f: &mut Find, seen: &Map<String, Value>) -> Result<(), String> {
    for i in 0..10 {
        if !seen.contains_key(&sent_key(f)?) {
            return Ok(());
        }
        if i % 2 == 0 {
            f.insert("v".into(), json!((fget_i(f, "v") + 1) % 4));
        } else {
            let kdn = f.get("kd").map(pystr).unwrap_or_default();
            let n = MICRO.get(&kdn).map(|v| v.len() as i64).ok_or_else(|| format!("KeyError: {}", kdn))?;
            f.insert("mi".into(), json!((fget_i(f, "mi") + 1) % n));
        }
    }
    Ok(())
}

fn qty_n(it: &Item, rng: &mut PyRandom, lo: i64, hi: i64) -> Result<i64, String> {
    if it.enn != "-" {
        re(rng.randint(lo, hi))
    } else {
        Ok(1)
    }
}

// py: clan_loot.py:353 roll_finds
/// The findings of one raid. Updates cs["loot"] (recent items, sentence memory, safe leads) and returns the list of stored find dicts. `score` Null = None.
pub fn roll_finds(rng: &mut PyRandom, st: &mut Value, r: &Value, matchv: &Value, score: &Value, day: Option<i64>) -> Result<Vec<Value>, String> {
    roll_finds_left(rng, st, r, matchv, score, day, false)
}

pub fn roll_finds_left(rng: &mut PyRandom, st: &mut Value, r: &Value, matchv: &Value, score: &Value, day: Option<i64>, left: bool) -> Result<Vec<Value>, String> {
    let st_day = st.get("day").map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
    let morale = st.get("morale").map(int_of).unwrap_or(50);
    let day = day.unwrap_or(st_day);
    let mapname = matchv.get("map").filter(|v| truthy(v)).map(pystr).unwrap_or_default();
    let rleft = r.get("left").map(truthy).unwrap_or(false);
    if mapname.is_empty() || left || rleft {
        return Ok(vec![]);
    }
    let people: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let cs = ch::ch_state(st);
    recent(cs)?;
    // the loot dict is worked on outside `cs` and written back whatever the way out is (Python mutates it in place)
    let mut lo = cs.get_mut("loot").map(std::mem::take).ok_or("loot")?;
    let mut score_v = score.as_f64();
    let res = (|| -> Result<Vec<Value>, String> {
        let sc = match score_v {
            Some(s) => s,
            None => {
                let s = fortune_impl(st_day, morale, cs, None)?;
                score_v = Some(s);
                s
            }
        };
        roll_inner(rng, &people, &mut lo, r, matchv, sc, day, &mapname)
    })();
    if let Some(slot) = cs.get_mut("loot") {
        *slot = lo;
    }
    res
}

fn roll_inner(rng: &mut PyRandom, people: &[Value], lo: &mut Value, r: &Value, matchv: &Value, score: f64, day: i64, mapname: &str) -> Result<Vec<Value>, String> {
    let base = base_tier(score);
    let (got0, short) = budget_of(r, matchv);
    let sup = r.get("supplies").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
    let total = 2i64.max(6i64.min(2 + py::floordiv(sup, 7) + if matchv.get("win").map(truthy).unwrap_or(false) { 1 } else { 0 }));
    {
        let lm = lo.as_object_mut().ok_or("lo")?;
        let rec: Vec<Value> = lm.get("recent").and_then(|v| v.as_array()).cloned().unwrap_or_default().into_iter().filter(|x| day - int_of(&x[1]) <= STALE_DAYS).collect();
        lm.insert("recent".into(), Value::Array(rec));
        lm.insert("raid".into(), json!([]));
        let fin = setdef(lm, "finders", json!({}));
        let fm = fin.as_object_mut().ok_or("finders")?;
        let old: Vec<String> = fm.iter().filter(|(_, d)| day - int_of(d) > 40).map(|(k, _)| k.clone()).collect();
        for k in old {
            fm.shift_remove(&k);
        }
    }
    let avoid: HashSet<String> = lo["recent"].as_array().map(|a| a.iter().map(|x| pystr(&x[0])).collect()).unwrap_or_default();
    let mk = kinds_of(mapname);
    let mut out: Vec<Find> = Vec::new();
    let mut used: HashSet<String> = HashSet::new();
    let mut got = got0;
    let mut groups: Vec<usize> = (0..5).filter(|&i| got[i] > 0).collect();
    groups.sort_by(|&a, &b| (-short[a], -got[a], GROUPS[a]).cmp(&(-short[b], -got[b], GROUPS[b])));
    if groups.is_empty() {
        groups = vec![0, 4];
        got[0] = 1;
        got[4] = 1;
    }
    let mut left_ = got;
    let uni = |a: &HashSet<String>, b: &HashSet<String>| -> HashSet<String> { a.union(b).cloned().collect() };
    let mut seq = groups.clone();
    seq.extend(groups.iter().copied());
    for g in seq {
        if out.len() as i64 >= total {
            break;
        }
        let nsame = out.iter().filter(|f| group_of(&ITEMS[f.get("k").and_then(|k| k.as_str()).unwrap_or("")].cat) == GROUPS[g]).count();
        if left_[g] <= 0 || nsame >= 2 {
            continue;
        }
        let cats = cats_of(GROUPS[g]);
        let t0 = roll_tier(rng, base);
        let mut okc: Vec<(&str, f64)> = cats.iter().filter(|(c, _)| ITEMS.values().any(|it| (it.tier - t0).abs() <= 1 && it.cat == *c)).copied().collect();
        if okc.is_empty() {
            okc = cats.to_vec();
        }
        let cat = okc[re(rng.choices_weighted(&okc.iter().map(|x| x.1).collect::<Vec<_>>(), 1))?[0]].0;
        let mut chosen: Option<(Find, &'static Item)> = None;
        for try_ in 0..8 {
            let tier = if try_ == 0 { t0 } else { roll_tier(rng, base) };
            let av = uni(&avoid, &used);
            let (mut pool, mut ws) = candidates(&[cat], tier, &mk, 1, &av);
            if pool.is_empty() {
                let c2 = candidates(&[cat], tier, &mk, 2, &av);
                pool = c2.0;
                ws = c2.1;
            }
            let it = pool[re(rng.choices_weighted(&ws, 1))?[0]];
            let mut f = make(rng, it, tier, mapname, day)?;
            let qmax = it.qmin.max(it.qmax.min(((left_[g] as f64) / (it.val.max(1) as f64)).ceil() as i64));
            f.insert("n".into(), json!(qty_n(it, rng, it.qmin, qmax)?));
            let seen = lo["seen"].as_object().ok_or("seen")?;
            let s = sent_key(&f)?;
            if !seen.contains_key(&s) && !used.contains(&it.key) {
                chosen = Some((f, it));
                break;
            }
            f.insert("v".into(), json!((fget_i(&f, "v") + 1) % 4));
            let s = sent_key(&f)?;
            let ok = !seen.contains_key(&s) && !used.contains(&it.key);
            chosen = Some((f, it));
            if ok {
                break;
            }
        }
        let (f, it) = chosen.ok_or("no find")?;
        left_[g] = 0i64.max(left_[g] - fget_i(&f, "n") * it.val.max(1));
        used.insert(it.key.clone());
        out.push(f);
    }
    // morale finds: a trinket or a book
    if out.len() < 6 && rng.random() < 0.55 {
        let cat = if rng.random() < 0.7 { "trink" } else { "book" };
        let tier = roll_tier(rng, base);
        let (pool, ws) = candidates(&[cat], tier, &mk, 1, &uni(&avoid, &used));
        if !pool.is_empty() {
            let it = pool[re(rng.choices_weighted(&ws, 1))?[0]];
            let mut f = make(rng, it, tier, mapname, day)?;
            f.insert("n".into(), json!(qty_n(it, rng, it.qmin, it.qmax.min(2).max(it.qmin))?));
            out.push(f);
            used.insert(it.key.clone());
        }
    }
    while out.len() < 2 {
        let cat = *re(rng.choice(&["trink", "book", "mat"]))?;
        let rt = roll_tier(rng, base.min(1));
        let (pool, ws) = candidates(&[cat], rt, &mk, 1, &uni(&avoid, &used));
        let it = *pool.get(re(rng.choices_weighted(&ws, 1))?[0]).ok_or("IndexError")?;
        let mut f = make(rng, it, it.tier, mapname, day)?;
        f.insert("n".into(), json!(qty_n(it, rng, it.qmin, it.qmax.min(2).max(it.qmin))?));
        out.push(f);
        used.insert(it.key.clone());
    }
    // a rare find now and then (more often on a good streak)
    if rng.random() < 0.05 + (if score > 0.4 { 0.10 } else { 0.0 }) && out.len() < 7 {
        let pool: Vec<&'static Item> = ITEMS.values().filter(|it| it.rar == 2 && !used.contains(&it.key) && (it.tier - base).abs() <= 1).collect();
        if !pool.is_empty() {
            let ws: Vec<f64> = pool.iter().map(|x| affinity(x, &mk)).collect();
            let it = pool[re(rng.choices_weighted(&ws, 1))?[0]];
            let mut f = make(rng, it, it.tier, mapname, day)?;
            f.insert("n".into(), json!(1));
            f.insert("rare".into(), json!(1));
            out.push(f);
        }
    }
    // a safe seen on an earlier day opens now
    let leads: Vec<Value> = lo["leads"].as_array().map(|a| a.iter().filter(|x| day - int_of(&x["d"]) > 0 && day - int_of(&x["d"]) <= 12).cloned().collect()).unwrap_or_default();
    lo["leads"] = Value::Array(if leads.is_empty() { vec![] } else { leads[1..].to_vec() });
    if !leads.is_empty() {
        let (pool, ws) = candidates(&["trink", "med", "ammo", "tool", "food"], 3i64.max(base), &mk, 2, &used);
        let it = *pool.get(re(rng.choices_weighted(&ws, 1))?[0]).ok_or("IndexError")?;
        let mut f = make(rng, it, 3, mapname, day)?;
        f.insert("n".into(), json!(1));
        f.insert("h".into(), json!(-2));
        f.insert("ha".into(), json!(int_of(&leads[0]["d"])));
        f.insert("kd".into(), json!("of"));
        f.insert("mi".into(), json!(2));
        out.push(f);
    }
    // the finders: role fit x recency, nobody twice in a raid; then every sentence is made unique
    for f in out.iter_mut() {
        let it = item(f.get("k").and_then(|k| k.as_str()).unwrap_or(""))?;
        let (w, g) = pick_member(rng, people, it, lo, day)?;
        f.insert("w".into(), json!(w));
        f.insert("g".into(), json!(g));
        let seen = lo["seen"].as_object().ok_or("seen")?.clone();
        unique(f, &seen)?;
    }
    // bookkeeping
    for f in &out {
        let k = f.get("k").cloned().unwrap_or(Value::Null);
        lo["recent"].as_array_mut().ok_or("recent")?.push(json!([k, day]));
        let sk = sent_key(f)?;
        lo["seen"][sk.as_str()] = json!(day);
        let h = fget_i(f, "h");
        if h >= 0 && HOOKS.get(h as usize).map(|x| x.2 == "lead").ok_or_else(|| format!("IndexError: {}", h))? {
            lo["leads"].as_array_mut().ok_or("leads")?.push(json!({"d": day, "m": mapname}));
        }
    }
    {
        let seen = lo["seen"].as_object_mut().ok_or("seen")?;
        let old: Vec<String> = seen.iter().filter(|(_, d)| day - int_of(d) > STALE_DAYS).map(|(k, _)| k.clone()).collect();
        for s in old {
            seen.shift_remove(&s);
        }
    }
    {
        let rec = lo["recent"].as_array_mut().ok_or("recent")?;
        let n = rec.len();
        if n > 60 {
            rec.drain(..n - 60);
        }
        let ld = lo["leads"].as_array_mut().ok_or("leads")?;
        let n = ld.len();
        if n > 3 {
            ld.drain(..n - 3);
        }
    }
    Ok(out.into_iter().map(Value::Object).collect())
}

// ------------------------------------------------------------------ rendering
// py: clan_loot.py:487 _hook_text
fn hook_text(f: &Find, lang: &str) -> Result<String, String> {
    let en = lang == "en";
    let h = f.get("h").map(int_of).unwrap_or(-1);
    let ha = f.get("ha").map(int_of).unwrap_or(0);
    if h == -2 {
        return Ok(if en { format!("The combination from day {} opened a safe.", ha) } else { format!("Kombinacja z dnia {} otworzyła sejf.", ha) });
    }
    if h < 0 {
        return Ok(String::new());
    }
    let hk = HOOKS.get(h as usize).ok_or_else(|| format!("IndexError: {}", h))?;
    let nn = names();
    if nn.is_empty() {
        return Err("ZeroDivisionError".into());
    }
    let name = nn[py::pymod(ha, nn.len() as i64) as usize];
    let combo = format!("{}-{}-{}", py::pymod(ha, 40) + 1, py::pymod(py::floordiv(ha, 7), 40) + 1, py::pymod(py::floordiv(ha, 11), 40) + 1);
    let nl = d2("NOTES").as_array().map(|a| a.len()).unwrap_or(0) as i64;
    if nl == 0 {
        return Err("ZeroDivisionError".into());
    }
    let note = note_of(py::pymod(ha, nl) as usize, en);
    pyformat(if en { &hk.0 } else { &hk.1 }, &[("name", name), ("combo", &combo), ("note", &note)])
}

// py: clan_loot.py:499 loc_phrase
/// 'in the baggage cage, near the platform' / 'w klatce bagażowej, w pobliżu peronu'.
pub fn loc_phrase_map(f: &Find, lang: &str, with_place: bool) -> Result<String, String> {
    let kdn = f.get("kd").map(pystr).ok_or("KeyError: kd")?;
    let rows = MICRO.get(&kdn).ok_or_else(|| format!("KeyError: {}", kdn))?;
    let mi = f.get("mi").map(int_of).ok_or("KeyError: mi")?;
    let (en, pl) = &rows[py::pymod(mi, rows.len() as i64) as usize];
    let mut txt = if lang == "en" { en.clone() } else { pl.clone() };
    let place = f.get("pl").filter(|v| truthy(v)).map(pystr);
    if with_place {
        if let Some(p) = place {
            if lang == "en" {
                txt.push_str(", near ");
                txt.push_str(&p);
            } else if let Some(gen) = data::t("clan_saga_pl_data", "PLACES").get(&p).and_then(|x| x.get(1)).and_then(|x| x.as_str()) {
                txt.push_str(", w pobliżu ");
                txt.push_str(gen);
            }
        }
    }
    Ok(txt)
}

pub fn loc_phrase(f: &Value, lang: &str, with_place: bool) -> Result<String, String> {
    loc_phrase_map(f.as_object().ok_or("find")?, lang, with_place)
}

// py: clan_loot.py:517 render_find
/// The finding as a small scene: the sentence (4 variants) + the story hook.
pub fn render_find_map(f: &Find, lang: &str, hook: bool) -> Result<String, String> {
    let k = f.get("k").map(pystr).ok_or("KeyError: k")?;
    item(&k)?;
    let ph = item_phrase(&k, f.get("n").map(int_of).ok_or("KeyError: n")?, f.get("c").map(int_of).unwrap_or(-1), lang)?;
    let v = f.get("v").map(int_of).unwrap_or(0);
    let loc = loc_phrase_map(f, lang, py::pymod(v, 4) == 0)?;
    let who = f.get("w").filter(|x| truthy(x)).map(pystr).unwrap_or_else(|| if lang == "pl" { "Ktoś".to_string() } else { "Someone".to_string() });
    let fem = f.get("g").and_then(|x| x.as_str()) == Some("f");
    let vi_ = py::pymod(v, 4) as usize;
    let mut s = if lang == "pl" {
        pyformat(
            &d2("SENT_PL").get(vi_).map(s_of).unwrap_or_default(),
            &[("who", &who), ("item", &ph), ("loc", &loc), ("Loc", &cap(&loc)), ("a", if fem { "a" } else { "" }), ("natknal", if fem { "natknęła" } else { "natknął" })],
        )?
    } else {
        pyformat(&d2("SENT_EN").get(vi_).map(s_of).unwrap_or_default(), &[("who", &who), ("item", &ph), ("loc", &loc), ("Loc", &cap(&loc))])?
    };
    if hook {
        let h = hook_text(f, lang)?;
        if !h.is_empty() {
            s.push(' ');
            s.push_str(&h);
        } else if f.get("rare").map(truthy).unwrap_or(false) {
            s.push(' ');
            s.push_str(if lang == "en" { "Nobody in the camp has seen one in years." } else { "Nikt w obozie nie widział takiego od lat." });
        }
    }
    Ok(s)
}

pub fn render_find(f: &Value, lang: &str, hook: bool) -> Result<String, String> {
    render_find_map(f.as_object().ok_or("find")?, lang, hook)
}

fn find_item_phrase(f: &Value, lang: &str) -> Result<String, String> {
    item_phrase(&f.get("k").map(pystr).ok_or("KeyError: k")?, f.get("n").map(int_of).ok_or("KeyError: n")?, f.get("c").map(int_of).unwrap_or(-1), lang)
}

// py: clan_loot.py:540 items_text
/// '3 cans of pineapple, a sealed pack of antibiotics and a deck of cards'.
pub fn items_text_res(finds: &Value, lang: &str) -> Result<String, String> {
    let mut v = Vec::new();
    for f in finds.as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
        v.push(find_item_phrase(f, lang)?);
    }
    Ok(join_list(&v, lang))
}

/// The chronicle's call shape: a Python exception cannot happen on rows this module wrote; on a broken row the text is empty.
pub fn items_text(finds: &Value, lang: &str) -> String {
    items_text_res(finds, lang).unwrap_or_default()
}

// py: clan_loot.py:545 public
/// The launcher / game view of the findings: a list of dicts with the icon id.
pub fn public_res(finds: &Value, lang: &str) -> Result<Value, String> {
    let mut out = Vec::new();
    for f in finds.as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
        let k = f.get("k").map(pystr);
        let it = match k.as_deref().and_then(|k| ITEMS.get(k)) {
            Some(it) => it,
            None => continue,
        };
        let mut m = Map::new();
        m.insert("k".into(), f["k"].clone());
        m.insert("n".into(), f.get("n").cloned().ok_or("KeyError: n")?);
        m.insert("icon".into(), json!(icon_for(&it.key)?));
        m.insert("icon_want".into(), json!(it.icon));
        m.insert("cat".into(), json!(it.cat));
        m.insert("tier".into(), f.get("t").cloned().unwrap_or_else(|| json!(it.tier)));
        m.insert("tier_name".into(), json!(tier_name(it.tier)));
        m.insert("item".into(), json!(find_item_phrase(f, lang)?));
        m.insert("text".into(), json!(render_find(f, lang, true)?));
        m.insert("who".into(), f.get("w").cloned().unwrap_or_else(|| json!("")));
        m.insert("where".into(), json!(loc_phrase(f, lang, false)?));
        m.insert("rare".into(), json!(f.get("rare").map(truthy).unwrap_or(false) || it.rar >= 2));
        m.insert("keep".into(), json!(it.cat == "trink" || it.cat == "book"));
        out.push(Value::Object(m));
    }
    Ok(Value::Array(out))
}

pub fn public(finds: &Value, lang: &str) -> Value {
    public_res(finds, lang).unwrap_or(Value::Null)
}

// py: clan_loot.py:557 supply_value
/// What the findings are worth per supply group: {food, water, med, ammo, mat} (units).
pub fn supply_value(finds: &Value) -> Result<Value, String> {
    let mut out = [0i64; 5];
    for f in finds.as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
        let it = item(&f.get("k").map(pystr).ok_or("KeyError: k")?)?;
        let g = group_of(&it.cat);
        if !g.is_empty() {
            if let Some(i) = GROUPS.iter().position(|x| *x == g) {
                out[i] += f.get("n").map(int_of).ok_or("KeyError: n")? * it.val;
            }
        }
    }
    let mut m = Map::new();
    for (i, g) in GROUPS.iter().enumerate() {
        m.insert((*g).to_string(), json!(out[i]));
    }
    Ok(Value::Object(m))
}

// py: clan_loot.py:568 keepsakes
/// Findings that become keepsakes of the clan (trinkets / books): the saga calls them back later.
pub fn keepsakes(finds: &[Value]) -> Result<Vec<Value>, String> {
    let mut out = Vec::new();
    for f in finds {
        let it = item(&f.get("k").map(pystr).ok_or("KeyError: k")?)?;
        if it.cat == "trink" || it.cat == "book" {
            out.push(f.clone());
        }
    }
    Ok(out)
}

// py: clan_loot.py:573 finds_story
/// The first `limit` findings as sentences for a saga entry.
pub fn finds_story(finds: &[Value], lang: &str, limit: usize) -> Result<String, String> {
    let mut v = Vec::new();
    for (i, f) in finds.iter().take(limit).enumerate() {
        v.push(render_find(f, lang, i == 0)?);
    }
    Ok(v.join(" "))
}

// py: clan_loot.py:580 legacy_loot
/// The findings as the chronicle's old [[key, n]] loot list (practical goods by their key, keepsakes by the old keepsake key).
pub fn legacy_loot(finds: &[Value], have: &Value) -> Vec<Value> {
    let mut out: Vec<Value> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    for f in finds {
        let k0 = f.get("k").map(pystr).unwrap_or_default();
        let old = match k0.as_str() {
            "chess" => Some("chess"),
            "guitar" => Some("guitar"),
            "comic" => Some("comic"),
            "teddy" => Some("teddy"),
            "deck" => Some("cards"),
            "harmonica" => Some("harmonica"),
            "mixedseed" => Some("seeds"),
            _ => None,
        };
        let (k, n) = if let Some(o) = old {
            if have.get(o).is_some() {
                continue;
            }
            (o.to_string(), 1)
        } else if ch::is_use(&k0) {
            (k0.clone(), f.get("n").map(int_of).unwrap_or(0))
        } else {
            continue;
        };
        if seen.insert(k.clone()) {
            out.push(json!([k, n]));
        }
    }
    out
}
