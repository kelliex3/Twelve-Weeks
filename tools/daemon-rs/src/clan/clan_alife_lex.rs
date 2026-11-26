//! py: clan_alife_lex.py - vocabulary of the clan A-Life (places, goods, projects, injuries, groups, jobs, skills, weather; English and Polish side by side)
//! and the two template renderers. The tables are the generated data (data/clan_tables.json); only the logic is ported.
use crate::clan::clan::{int_of, pystr};
use crate::clan::clan_story as cs;
use crate::clan::clan_story_pl as pln;
use crate::clan::data;
use regex::Regex;
use serde_json::Value;
use std::collections::BTreeSet;
use std::sync::LazyLock;

pub type Vars = cs::Vars;

fn tb(name: &str) -> &'static Value {
    data::t("clan_alife_lex", name)
}

// py: clan_alife_lex.py:16 PLACES   {fk: {key: (EN, nom, gen, at, to, from)}}
pub fn places() -> &'static Value {
    tb("PLACES")
}
pub fn goods() -> &'static Value {
    tb("GOODS")
}
pub fn projects() -> &'static Value {
    tb("PROJECTS")
}
pub fn injuries() -> &'static Value {
    tb("INJURIES")
}
pub fn groups() -> &'static Value {
    tb("GROUPS")
}
pub fn origins() -> &'static Value {
    tb("ORIGINS")
}
pub fn jobs() -> &'static Value {
    tb("JOBS")
}
pub fn skills() -> &'static Value {
    tb("SKILLS")
}
pub fn keeps() -> &'static Value {
    tb("KEEPS")
}

/// WEATHER (rain fog cold clear smoke wind)
pub fn weather() -> Vec<&'static str> {
    data::strs(tb("WEATHER"))
}

/// SEASONS[i] = (EN, PL)
pub fn season_name(i: i64, pl: bool) -> Option<&'static str> {
    tb("SEASONS").get(usize::try_from(i).ok()?)?.get(if pl { 1 } else { 0 })?.as_str()
}

// py: clan_alife_lex.py:192 season_of
/// 12 weeks: 1-4 late autumn, 5-8 deep winter, 9-12 early spring.
pub fn season_of(week: i64) -> i64 {
    if week <= 4 {
        0
    } else if week <= 8 {
        1
    } else {
        2
    }
}

fn row_str(row: &'static Value, i: usize) -> Option<&'static str> {
    row.get(i)?.as_str()
}

// py: clan_alife_lex.py:201 pool_keys
/// Keys of a (scope-first) lexicon table usable by faction fk, in a stable order.
pub fn pool_keys(table: &'static Value, fk: &str) -> Vec<&'static str> {
    match table.as_object() {
        Some(o) => o.iter().filter(|(_, v)| matches!(row_str(v, 0), Some("*")) || row_str(v, 0) == Some(fk)).map(|(k, _)| k.as_str()).collect(),
        None => Vec::new(),
    }
}

static ZE: LazyLock<Regex> = LazyLock::new(|| {
    let (src, _flags) = data::regex_src(tb("_ZE")).unwrap_or(("(?i)^$", 0));
    Regex::new(src).unwrap_or_else(|_| Regex::new("^$").expect("ZE fallback"))
});

// py: clan_alife_lex.py:80 with_prep
/// 'z' / 'ze' + an instrumental form ('ze złomem', 'z solą').
pub fn with_prep(ins: &str) -> String {
    format!("{}{}", if ZE.is_match(ins) { "ze " } else { "z " }, ins)
}

const NUMS: [&str; 11] = ["no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"];

// py: clan_alife_lex.py:210 _en_days
fn en_days(n: i64) -> String {
    let w = if (0..=10).contains(&n) { NUMS[n as usize].to_string() } else { n.to_string() };
    format!("{} day{}", w, if n == 1 { "" } else { "s" })
}

// py: clan_alife_lex.py:214 _pl_days
/// kind: nom 'dwa dni', after 'po dwóch dniach', ago 'dwa dni temu', in 'za dwa dni'.
fn pl_days(n: i64, kind: &str) -> String {
    let nom = match n {
        1 => "jeden dzień".to_string(),
        2 => "dwa dni".into(),
        3 => "trzy dni".into(),
        4 => "cztery dni".into(),
        5 => "pięć dni".into(),
        6 => "sześć dni".into(),
        7 => "siedem dni".into(),
        _ => format!("{} dni", n),
    };
    match kind {
        "nom" => nom,
        "after" => format!(
            "po {}",
            match n {
                1 => "jednym dniu".to_string(),
                2 => "dwóch dniach".into(),
                3 => "trzech dniach".into(),
                4 => "czterech dniach".into(),
                5 => "pięciu dniach".into(),
                6 => "sześciu dniach".into(),
                7 => "siedmiu dniach".into(),
                _ => format!("{} dniach", n),
            }
        ),
        "ago" => {
            if n != 1 {
                format!("{} temu", nom)
            } else {
                "wczoraj".to_string()
            }
        }
        _ => format!("za {}", nom),
    }
}

// py: clan_alife_lex.py:305 pl_people
/// '5 osób', '3 osoby', '22 osoby'.
pub fn pl_people(n: i64) -> String {
    let few = (2..=4).contains(&crate::py::pymod(n, 10)) && !(12..=14).contains(&crate::py::pymod(n, 100));
    format!("{} {}", n, if few { "osoby" } else if n != 1 { "osób" } else { "osoba" })
}

/// `{k: sym[k] for k in ("f", "day", "week", "inf", "ill") if k in sym}`
fn core_sym(sym: &Value) -> Value {
    let mut m = serde_json::Map::new();
    for k in ["f", "day", "week", "inf", "ill"] {
        if let Some(v) = sym.get(k) {
            m.insert(k.into(), v.clone());
        }
    }
    Value::Object(m)
}

fn sym_fk(sym: &Value) -> &str {
    sym.get("f").and_then(|v| v.as_str()).unwrap_or("W")
}

fn table_row(table: &'static Value, key: &Value) -> Option<&'static Value> {
    table.get(key.as_str()?)
}

// py: clan_alife_lex.py:228 expand_en
/// The English words of the symbolic variables. None = Python raised (KeyError ...).
pub fn expand_en(sym: &Value) -> Option<Vars> {
    let fk = sym_fk(sym);
    let mut v = cs::expand_en(&core_sym(sym))?;
    if let Some(d) = sym.get("dest") {
        v.insert("dest".into(), row_str(table_row(places().get(fk)?, d)?, 0)?.to_string());
    }
    if let Some(p) = sym.get("place") {
        v.insert("place".into(), pystr(p));
    }
    for (key, table, idx) in [("goal", goods(), 1usize), ("proj", projects(), 1), ("grp", groups(), 1), ("inj", injuries(), 0), ("keep", keeps(), 0)] {
        if let Some(k) = sym.get(key) {
            v.insert(key.into(), row_str(table_row(table, k)?, idx)?.to_string());
        }
    }
    if let Some(j) = sym.get("job") {
        v.insert("job".into(), row_str(table_row(jobs(), j)?, 1)?.to_string());
    }
    if let Some(s) = sym.get("skill") {
        v.insert("skill".into(), row_str(table_row(skills(), s)?, 1)?.to_string());
    }
    if let Some(o) = sym.get("orig") {
        v.insert("orig".into(), row_str(table_row(origins(), o)?, 1)?.to_string());
    }
    if let Some(p) = sym.get("prog") {
        let row = data::dict_get_int(tb("PROG"), int_of(p))?;
        v.insert("prog".into(), row_str(row, 0)?.to_string());
    }
    if let Some(nd) = sym.get("nd") {
        let n = int_of(nd);
        v.insert("nd".into(), en_days(n));
        v.insert("nd_after".into(), format!("after {}", en_days(n)));
        v.insert("nd_ago".into(), if n == 1 { "yesterday".to_string() } else { format!("{} ago", en_days(n)) });
        v.insert("nd_in".into(), format!("in {}", en_days(n)));
    }
    if let Some(w) = sym.get("wx") {
        v.insert("wx".into(), pystr(w));
    }
    if let Some(s) = sym.get("season") {
        v.insert("season".into(), season_name(int_of(s), false)?.to_string());
    }
    for k in ["sup", "need", "size", "sick_n", "hungry_n", "due", "jd", "n"] {
        if let Some(x) = sym.get(k) {
            v.insert(k.into(), pystr(x));
        }
    }
    if let Some(s) = sym.get("size") {
        let n = int_of(s);
        v.insert("size_ppl".into(), if 0 < n && n <= 10 { format!("{} people", NUMS[n as usize]) } else { format!("{} people", n) });
    }
    Some(v)
}

// py: clan_alife_lex.py:261 expand_pl
pub fn expand_pl(sym: &Value) -> Option<Vars> {
    let fk = sym_fk(sym);
    let mut v = pln::expand(&core_sym(sym))?;
    if let Some(d) = sym.get("dest") {
        let row = table_row(places().get(fk)?, d)?.as_array()?;
        for (suf, form) in ["", "_gen", "_at", "_to", "_from"].iter().zip(row.iter().skip(1)) {
            v.insert(format!("dest{}", suf), pystr(form));
        }
    }
    if let Some(p) = sym.get("place") {
        v.insert("place".into(), pystr(p));
    }
    for (key, table) in [("goal", goods()), ("proj", projects()), ("grp", groups())] {
        if let Some(k) = sym.get(key) {
            let row = table_row(table, k)?.as_array()?;
            for (suf, form) in ["", "_gen", "_acc", "_ins"].iter().zip(row.iter().skip(2)) {
                v.insert(format!("{}{}", key, suf), pystr(form));
            }
            v.insert(format!("{}_with", key), with_prep(&pystr(row.get(5)?)));
            v.insert(format!("{}_nom", key), pystr(row.get(2)?));
        }
    }
    for (key, table) in [("inj", injuries()), ("keep", keeps())] {
        if let Some(k) = sym.get(key) {
            let row = table_row(table, k)?.as_array()?;
            for (suf, form) in ["", "_gen", "_acc", "_ins"].iter().zip(row.iter().skip(1)) {
                v.insert(format!("{}{}", key, suf), pystr(form));
            }
            if key == "inj" {
                v.insert("inj_with".into(), with_prep(&pystr(row.get(4)?)));
                v.insert("inj_nom".into(), pystr(row.get(1)?));
            } else {
                v.insert("keep_nom".into(), pystr(row.get(1)?));
            }
        }
    }
    if let Some(j) = sym.get("job") {
        v.insert("job".into(), row_str(table_row(jobs(), j)?, 2)?.to_string());
    }
    if let Some(s) = sym.get("skill") {
        v.insert("skill".into(), row_str(table_row(skills(), s)?, 2)?.to_string());
    }
    if let Some(o) = sym.get("orig") {
        v.insert("orig".into(), row_str(table_row(origins(), o)?, 2)?.to_string());
    }
    if let Some(p) = sym.get("prog") {
        let row = data::dict_get_int(tb("PROG"), int_of(p))?;
        v.insert("prog".into(), row_str(row, 1)?.to_string());
    }
    if let Some(nd) = sym.get("nd") {
        let n = int_of(nd);
        v.insert("nd".into(), pl_days(n, "nom"));
        v.insert("nd_after".into(), pl_days(n, "after"));
        v.insert("nd_ago".into(), pl_days(n, "ago"));
        v.insert("nd_in".into(), pl_days(n, "in"));
    }
    if let Some(s) = sym.get("season") {
        v.insert("season".into(), season_name(int_of(s), true)?.to_string());
    }
    for k in ["sup", "need", "size", "sick_n", "hungry_n", "due", "jd", "n"] {
        if let Some(x) = sym.get(k) {
            v.insert(k.into(), pystr(x));
        }
    }
    if let Some(s) = sym.get("size") {
        v.insert("size_ppl".into(), pl_people(int_of(s)));
    }
    Some(v)
}

const W: &str = r"\p{L}\p{N}_";
static KEY: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"\{{([{w}]+)\}}", w = W)).expect("KEY"));
static SW: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"\{([A-D]+)\?([^|{}]*)\|([^|{}]*)\}").expect("SW"));
static LETTER: LazyLock<Regex> = LazyLock::new(|| Regex::new(&format!(r"\{{([A-D])(?:_[{w}]+)?\}}", w = W)).expect("LETTER"));

fn person_key(key: &str) -> Option<(char, &str)> {
    // _PERSON = ^([ABCD])_(\w+)$
    let mut it = key.chars();
    let c = it.next()?;
    if !matches!(c, 'A'..='D') {
        return None;
    }
    let rest = it.as_str().strip_prefix('_')?;
    if rest.is_empty() {
        return None;
    }
    Some((c, rest))
}

// py: clan_alife_lex.py:317 role_word
/// `Err` = the KeyError of an unknown role.
pub fn role_word(person: &Value, fk: &str, lang: &str) -> Result<String, String> {
    let r = match person.get("r") {
        Some(v) if crate::clan::clan::truthy(v) => pystr(v),
        _ => "scavenger".to_string(),
    };
    let i = if fk == "W" { 0 } else { 1 };
    if lang == "pl" {
        let pair = data::t("clan_story_pl", "ROLES").get(&r).and_then(|x| x.get(i)).ok_or_else(|| format!("KeyError: {}", r))?;
        let k = if person.get("g").and_then(|v| v.as_str()) == Some("f") { 1 } else { 0 };
        Ok(pair.get(k).map(pystr).unwrap_or_default())
    } else {
        data::t("clan_story_data", "ROLES").get(&r).and_then(|x| x.get(i)).map(pystr).ok_or_else(|| format!("KeyError: {}", r))
    }
}

/// `CS.PRON[g][word]`.
pub fn pron(g: &str, word: &str) -> Result<String, String> {
    data::t("clan_story", "PRON").get(g).and_then(|x| x.get(word)).map(pystr).ok_or_else(|| format!("KeyError: PRON[{}][{}]", g, word))
}

// py: clan_alife_lex.py:325 render
/// Fill a template: variables first (their values may hold switches), then the {A?m|f} switches (PL), then names / pronouns / roles.
/// None = Python raised (KeyError on a missing person / pronoun).
pub fn render(tpl: &str, people: &Value, vars_: &Vars, lang: &str, fk: &str) -> Result<String, String> {
    let ke = |s: &str| s.to_string();
    let mut s = String::with_capacity(tpl.len() + 16);
    let mut last = 0;
    for m in KEY.captures_iter(tpl) {
        let all = m.get(0).ok_or_else(|| ke("re"))?;
        s.push_str(&tpl[last..all.start()]);
        let k = m.get(1).map(|g| g.as_str()).unwrap_or("");
        match vars_.get(k) {
            Some(val) => {
                let before = tpl[..all.start()].trim_end_matches(crate::py::is_py_space);
                let at_start = before.is_empty() || matches!(before.chars().next_back(), Some('.' | '!' | '?' | '"' | '\'' | '„' | '”'));
                if val.chars().next().map_or(false, |c| c.is_lowercase()) && at_start {
                    s.push_str(&pln::cap_first(val));
                } else {
                    s.push_str(val);
                }
            }
            None => s.push_str(all.as_str()),
        }
        last = all.end();
    }
    s.push_str(&tpl[last..]);

    let mut s2 = String::with_capacity(s.len());
    let mut last = 0;
    for m in SW.captures_iter(&s) {
        let all = m.get(0).ok_or_else(|| ke("re"))?;
        s2.push_str(&s[last..all.start()]);
        let mut fem = true;
        for ch in m.get(1).map(|g| g.as_str()).unwrap_or("").chars() {
            let p = people.get(ch.to_string()).ok_or_else(|| format!("KeyError: {}", ch))?;
            if p.get("g").and_then(|g| g.as_str()) != Some("f") {
                fem = false;
                break;
            }
        }
        s2.push_str(m.get(if fem { 3 } else { 2 }).map(|g| g.as_str()).unwrap_or(""));
        last = all.end();
    }
    s2.push_str(&s[last..]);

    let mut out = String::with_capacity(s2.len() + 16);
    let mut last = 0;
    for m in KEY.captures_iter(&s2) {
        let all = m.get(0).ok_or_else(|| ke("re"))?;
        out.push_str(&s2[last..all.start()]);
        let k = m.get(1).map(|g| g.as_str()).unwrap_or("");
        if let Some((letter, w)) = person_key(k) {
            let p = people.get(letter.to_string()).ok_or_else(|| format!("KeyError: {}", letter))?;
            if w == "role" {
                out.push_str(&role_word(p, fk, lang)?);
            } else {
                let g = p.get("g").map(pystr).unwrap_or_default();
                let r = pron(&g, &w.to_lowercase())?;
                if w.chars().next().map_or(false, |c| c.is_uppercase()) {
                    out.push_str(&pln::cap_first(&r));
                } else {
                    out.push_str(&r);
                }
            }
        } else if let Some(p) = people.get(k) {
            out.push_str(&p.get("n").map(pystr).unwrap_or_default());
        } else {
            out.push_str(all.as_str());
        }
        last = all.end();
    }
    out.push_str(&s2[last..]);
    Ok(out)
}

// py: clan_alife_lex.py:359 letters_of
pub fn letters_of(tpl: &str) -> BTreeSet<char> {
    let mut out = BTreeSet::new();
    for m in LETTER.captures_iter(tpl) {
        if let Some(c) = m.get(1).and_then(|g| g.as_str().chars().next()) {
            out.insert(c);
        }
    }
    for m in SW.captures_iter(tpl) {
        if let Some(g) = m.get(1) {
            out.extend(g.as_str().chars());
        }
    }
    out
}

// py: clan_alife_lex.py:366 vars_of
/// Variable keys (not people) of a template.
pub fn vars_of(tpl: &str) -> BTreeSet<String> {
    KEY.captures_iter(tpl)
        .filter_map(|m| m.get(1).map(|g| g.as_str().to_string()))
        .filter(|k| {
            // not re.fullmatch(r"[A-D](_\w+)?", k)
            let mut it = k.chars();
            match it.next() {
                Some('A'..='D') => {
                    let rest = it.as_str();
                    !(rest.is_empty() || (rest.starts_with('_') && rest.len() > 1))
                }
                _ => true,
            }
        })
        .collect()
}
