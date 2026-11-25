//! py: clan_story_pl.py - the Polish side of the clan feed: language plumbing, the Polish forms of the symbolic variables, the Polish template
//! renderer, the registry lid -> (scope, English template, Polish template). The tables are the generated data (data/clan_tables.json).
use crate::clan::clan::{int_of, pystr};
use crate::clan::data;
use regex::Regex;
use serde_json::Value;
use std::collections::{BTreeSet, HashMap};
use std::sync::LazyLock;

pub type Vars = HashMap<String, String>;

fn tb(name: &str) -> &'static Value {
    data::t("clan_story_pl", name)
}

// py: clan_story_pl.py:31 norm_lang
/// A language request given as a string ("" = English, "1" / "pl" / "pol" / "polish" / "pl-PL" = Polish) -> "en" | "pl".
pub fn norm_lang(x: &str) -> &'static str {
    let s = x.trim().to_lowercase().replace('_', "-");
    if matches!(s.as_str(), "1" | "pl" | "pol" | "polish") || s.starts_with("pl-") {
        "pl"
    } else {
        "en"
    }
}

/// `norm_lang(bytes)`: the first byte is the language number (empty = English).
pub fn norm_lang_b(x: &[u8]) -> &'static str {
    if x.first().copied().unwrap_or(0) == 1 {
        "pl"
    } else {
        "en"
    }
}

/// `norm_lang(int)`.
pub fn norm_lang_i(x: i64) -> &'static str {
    if x == 1 {
        "pl"
    } else {
        "en"
    }
}

// py: clan_story_pl.py:41 line_crc
pub fn line_crc(text: &str) -> String {
    format!("{:08x}", t2r_clanstory::crc::crc32(text.as_bytes()))
}

// py: clan_story_pl.py:47 PLACES (nom, gen, at, to, from, ins)
fn forms6(table: &'static Value, key: &Value) -> Vec<String> {
    if let Some(row) = key.as_str().and_then(|k| table.get(k)).and_then(|r| r.as_array()) {
        return row.iter().map(pystr).collect();
    }
    vec![pystr(key); 6]
}

static CARD_NOM: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("_CARD_NOM")));
static CARD_GEN: LazyLock<Vec<&'static str>> = LazyLock::new(|| data::strs(tb("_CARD_GEN")));

// py: clan_story_pl.py:104 people_phrase
/// A count of people in Polish: case nom / acc / gen. 0 = nobody. new=True: "nowe osoby" (the newcomers of a clan event).
pub fn people_phrase(n: i64, case: &str, new: bool) -> String {
    if n <= 0 {
        return match case {
            "nom" => {
                if new {
                    "nikt nowy"
                } else {
                    "nikt"
                }
            }
            _ => {
                if new {
                    "nikogo nowego"
                } else {
                    "nikogo"
                }
            }
        }
        .to_string();
    }
    let one = n == 1;
    let few = (2..=4).contains(&n);
    if case == "gen" {
        let num = if n <= 10 { CARD_GEN.get(n as usize).map(|s| s.to_string()).unwrap_or_default() } else { n.to_string() };
        let noun = if one {
            if new {
                "nowej osoby"
            } else {
                "osoby"
            }
        } else if new {
            "nowych osób"
        } else {
            "osób"
        };
        return format!("{} {}", num, noun);
    }
    let mut num = if n <= 10 { CARD_NOM.get(n as usize).map(|s| s.to_string()).unwrap_or_default() } else { n.to_string() };
    let noun = if one {
        let acc = case == "acc";
        num = if acc { "jedną" } else { "jedna" }.to_string();
        if new {
            if acc {
                "nową osobę"
            } else {
                "nowa osoba"
            }
        } else if acc {
            "osobę"
        } else {
            "osoba"
        }
    } else if few {
        if new {
            "nowe osoby"
        } else {
            "osoby"
        }
    } else if new {
        "nowych osób"
    } else {
        "osób"
    };
    format!("{} {}", num, noun)
}

// py: clan_story_pl.py:126 expand
/// Symbolic variables of a message -> every Polish form the templates use. None = Python raised (KeyError / ValueError on a damaged row).
pub fn expand(sym: &Value) -> Option<Vars> {
    let get = |k: &str| sym.get(k);
    let fk = match get("f") {
        None => "W".to_string(),
        Some(Value::String(s)) => s.clone(),
        Some(_) => return None,
    };
    let mut v = Vars::new();
    v.insert("day".into(), get("day").map(pystr).unwrap_or_else(|| "0".into()));
    v.insert("week".into(), get("week").map(pystr).unwrap_or_else(|| "1".into()));
    if let Some(p) = get("place") {
        let f = forms6(tb("PLACES"), p);
        for (k, x) in ["place", "place_gen", "place_at", "place_to", "place_from", "place_ins"].iter().zip(f) {
            v.insert((*k).into(), x);
        }
    }
    let en = tb("ENEMY").get(&fk)?.as_array()?;
    let e0 = en.first().map(pystr)?;
    v.insert("enemy".into(), e0.clone());
    v.insert("enemies".into(), e0);
    v.insert("enemy_gen".into(), en.get(1).map(pystr)?);
    v.insert("enemy_ins".into(), en.get(2).map(pystr)?);
    if let Some(i) = get("inf") {
        let f = forms6(tb("INF"), i);
        for (k, x) in ["inf", "inf_gen", "inf_ins", "infs", "infs_gen", "inf_two"].iter().zip(f) {
            v.insert((*k).into(), x);
        }
    }
    if let Some(a) = get("ago") {
        let s = a.as_str().and_then(|k| tb("AGO").get(k)).map(pystr).unwrap_or_else(|| pystr(a));
        v.insert("ago".into(), s);
    }
    if let Some(ill) = get("ill") {
        let k = match ill.as_str() {
            Some(k) if tb("ILL").get(k).is_some() => k,
            _ => "none",
        };
        let i = tb("ILL").get(k)?.as_array()?;
        let c = tb("CURE").get(k)?.as_array()?;
        for (n, x) in ["ill", "ill_gen", "ill_acc", "ill_ins"].iter().zip(i) {
            v.insert((*n).into(), pystr(x));
        }
        for (n, x) in ["cure", "cure_gen", "cure_acc", "cure_ins"].iter().zip(c) {
            v.insert((*n).into(), pystr(x));
        }
    }
    for (key, new) in [("n", false), ("lost", false), ("gain", true)] {
        if let Some(x) = get(key) {
            if !matches!(x, Value::Number(_) | Value::Bool(_)) {
                return None;
            }
            let n = int_of(x);
            v.insert(key.into(), people_phrase(n, "nom", new));
            v.insert(format!("{}_acc", key), people_phrase(n, "acc", new));
            v.insert(format!("{}_gen", key), people_phrase(n, "gen", new));
        }
    }
    Some(v)
}

static KEY_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{(\w+)\}").expect("KEY_RE"));
static SW_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{([A-D]+)\?([^|{}]*)\|([^|{}]*)\}").expect("SW_RE"));
static LETTER_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{([A-D])(?:_\w+)?\}").expect("LETTER_RE"));

// py: clan_story_pl.py:156 PL_SPECIAL / _name
fn name_of(p: &Value) -> String {
    let n = p.get("n").map(pystr).unwrap_or_default();
    match data::t("clan_story_pl", "PL_SPECIAL").get(&n) {
        Some(s) => pystr(s),
        None => n,
    }
}

/// `s[:1].islower()`
fn starts_lower(s: &str) -> bool {
    s.chars().next().map_or(false, |c| c.is_lowercase())
}

/// `s[:1].upper() + s[1:]`
pub fn cap_first(s: &str) -> String {
    let mut it = s.chars();
    match it.next() {
        Some(c) => c.to_uppercase().collect::<String>() + it.as_str(),
        None => String::new(),
    }
}

// py: clan_story_pl.py:163 render
/// Fill a Polish template. people = {"A": {"n": name, "g": "m|f|n"}}, vars_ = expand()'s dict. None = Python KeyError.
pub fn render(tpl: &str, people: &Value, vars_: &Vars) -> Option<String> {
    let mut tpl2 = String::with_capacity(tpl.len());
    let mut last = 0;
    for m in SW_RE.captures_iter(tpl) {
        let all = m.get(0)?;
        tpl2.push_str(&tpl[last..all.start()]);
        let mut fem = true;
        for ch in m.get(1)?.as_str().chars() {
            let p = people.get(ch.to_string())?;
            if p.get("g").and_then(|g| g.as_str()) != Some("f") {
                fem = false;
            }
        }
        tpl2.push_str(m.get(if fem { 3 } else { 2 })?.as_str());
        last = all.end();
    }
    tpl2.push_str(&tpl[last..]);
    let mut out = String::with_capacity(tpl2.len() + 16);
    let mut last = 0;
    for m in KEY_RE.captures_iter(&tpl2) {
        let all = m.get(0)?;
        out.push_str(&tpl2[last..all.start()]);
        let key = m.get(1)?.as_str();
        if let Some(p) = people.get(key) {
            out.push_str(&name_of(p));
        } else {
            let val = vars_.get(key)?;
            let before = tpl2[..all.start()].trim_end_matches(crate::py::is_py_space);
            let at_start = before.is_empty() || matches!(before.chars().next_back(), Some('.' | '!' | '?' | '"' | '\'' | '„' | '”'));
            if starts_lower(val) && at_start {
                out.push_str(&cap_first(val));
            } else {
                out.push_str(val);
            }
        }
        last = all.end();
    }
    out.push_str(&tpl2[last..]);
    Some(out)
}

// py: clan_story_pl.py:183 tpl_letters
/// Person letters a template (English or Polish) uses.
pub fn tpl_letters(tpl: &str) -> BTreeSet<char> {
    let mut out = BTreeSet::new();
    for m in LETTER_RE.captures_iter(tpl) {
        if let Some(c) = m.get(1).and_then(|g| g.as_str().chars().next()) {
            out.insert(c);
        }
    }
    for m in SW_RE.captures_iter(tpl) {
        if let Some(g) = m.get(1) {
            out.extend(g.as_str().chars());
        }
    }
    out
}

// py: clan_story_pl.py:191 tpl_vars
/// Variable families a template uses (place_at -> place, ill_gen -> ill, inf_two -> inf, lost_acc -> lost ...).
pub fn tpl_vars(tpl: &str) -> BTreeSet<String> {
    static PERSON: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^[A-D](_\w+)?$").expect("PERSON"));
    static SUF: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"_(gen|acc|ins|at|to|from|two|a)$").expect("SUF"));
    let mut out = BTreeSet::new();
    for m in KEY_RE.captures_iter(tpl) {
        let k = m.get(1).map(|g| g.as_str()).unwrap_or("");
        if PERSON.is_match(k) {
            continue;
        }
        let k = SUF.replace(k, "").into_owned();
        let k = match k.as_str() {
            "infs" => "inf".to_string(),
            "enemies" => "enemy".to_string(),
            _ => k,
        };
        out.insert(k);
    }
    out
}

// py: clan_story_pl.py:258 people_labels
/// Roster words for the CAMP view (lang=pl): roleText / stateText / moodText / illnessText.
pub fn people_labels(fk: &str, g: &str, role: &str, state: &str, mood: &str, ill: &str) -> Option<Value> {
    let i = if g == "f" { 1 } else { 0 };
    let st = match state {
        "h" => "healthy",
        "u" => "hungry",
        "s" => "sick",
        _ => return None,
    };
    let role_w = tb("ROLES").get(role)?.get(if fk == "W" { 0 } else { 1 })?.get(i)?.clone();
    let state_w = tb("STATE").get(st)?.get(i)?.clone();
    let mood_w = tb("MOOD").get(mood)?.get(i)?.clone();
    let ill_w = match tb("ILL").get(ill) {
        Some(r) => r.get(0)?.clone(),
        None => Value::String(String::new()),
    };
    let mut m = serde_json::Map::new();
    m.insert("roleText".into(), role_w);
    m.insert("stateText".into(), state_w);
    m.insert("moodText".into(), mood_w);
    m.insert("illnessText".into(), ill_w);
    Some(Value::Object(m))
}

/// MORALE[label]
pub fn morale_text(label: &str) -> String {
    tb("MORALE").get(label).map(pystr).unwrap_or_default()
}

// py: clan_story_pl.py:301 lid
pub fn lid(pool: &str, en_tpl: &str) -> String {
    format!("{}/{}", pool, line_crc(en_tpl))
}

// py: clan_story_pl.py:305 REG
/// REG[lid] = (scope, English template, Polish template).
pub fn reg(lid: &str) -> Option<(&'static str, &'static str, &'static str)> {
    let e = tb("REG").get(lid)?.as_array()?;
    Some((e.first()?.as_str()?, e.get(1)?.as_str()?, e.get(2)?.as_str()?))
}

// py: clan_story_pl.py:305 ROWS
/// (pool, scope, English, Polish) of every template, in registry build order.
pub static ROWS: LazyLock<Vec<(&'static str, &'static str, &'static str, &'static str)>> = LazyLock::new(|| {
    tb("ROWS")
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|r| {
                    let r = r.as_array()?;
                    Some((r.first()?.as_str()?, r.get(1)?.as_str()?, r.get(2)?.as_str()?, r.get(3)?.as_str()?))
                })
                .collect()
        })
        .unwrap_or_default()
});
