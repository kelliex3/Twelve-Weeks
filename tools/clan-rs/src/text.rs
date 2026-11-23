use crate::data::{data, res, Data};
use serde_json::{Map, Value};
use std::collections::HashMap;

pub const WIDGET_MAX: usize = 118;

pub type Vars = HashMap<String, String>;

#[derive(Clone, Debug, PartialEq)]
pub struct Person {
    pub n: String,
    pub g: String,
    pub r: String,
}

#[derive(Clone, Debug, Default)]
pub struct People(pub Vec<(char, Person)>);

impl People {
    pub fn get(&self, c: char) -> Option<&Person> {
        self.0.iter().find(|(k, _)| *k == c).map(|(_, p)| p)
    }

    pub fn set(&mut self, c: char, p: Person) {
        match self.0.iter_mut().find(|(k, _)| *k == c) {
            Some(x) => x.1 = p,
            None => self.0.push((c, p)),
        }
    }

    pub fn letters(&self) -> String {
        let mut v: Vec<char> = self.0.iter().map(|(k, _)| *k).collect();
        v.sort();
        v.into_iter().collect()
    }
}

pub fn py_float(f: f64) -> String {
    if f.is_nan() {
        "nan".into()
    } else if f.is_infinite() {
        if f > 0.0 { "inf" } else { "-inf" }.into()
    } else if f == f.trunc() && f.abs() < 1e16 {
        format!("{:.1}", f)
    } else {
        format!("{}", f)
    }
}

pub fn py_str(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Null => "None".into(),
        Value::Bool(b) => if *b { "True" } else { "False" }.into(),
        Value::Number(n) => match n.as_i64() {
            Some(i) => i.to_string(),
            None => match n.as_u64() {
                Some(u) => u.to_string(),
                None => py_float(n.as_f64().unwrap_or(0.0)),
            },
        },
        other => other.to_string(),
    }
}

pub fn py_int(v: &Value) -> Option<i64> {
    match v {
        Value::Bool(b) => Some(*b as i64),
        Value::Number(n) => n.as_i64().or_else(|| n.as_f64().filter(|f| f.is_finite()).map(|f| f.trunc() as i64)),
        Value::String(s) => {
            let t = s.trim();
            let body = t.strip_prefix(['+', '-']).unwrap_or(t);
            if body.is_empty() || body.starts_with('_') || body.ends_with('_') || body.contains("__") || !body.chars().all(|c| c.is_ascii_digit() || c == '_') {
                return None;
            }
            t.replace('_', "").parse().ok()
        }
        _ => None,
    }
}

pub fn exact_int(v: &Value) -> Option<i64> {
    match v {
        Value::Bool(b) => Some(*b as i64),
        Value::Number(n) => n.as_i64(),
        _ => None,
    }
}

pub fn truthy(v: Option<&Value>) -> bool {
    match v {
        None | Some(Value::Null) => false,
        Some(Value::Bool(b)) => *b,
        Some(Value::Number(n)) => n.as_f64().map_or(false, |f| f != 0.0),
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
    }
}

pub fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

pub fn line_id(text: &str) -> String {
    format!("{:08x}", crc32fast::hash(text.as_bytes()))
}

pub fn clen(s: &str) -> usize {
    s.chars().count()
}

fn scan_keys(s: &str) -> Vec<(usize, usize, &str)> {
    let b = s.as_bytes();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'{' {
            let mut j = i + 1;
            while j < s.len() {
                let c = match s[j..].chars().next() {
                    Some(c) => c,
                    None => break,
                };
                if !is_word(c) {
                    break;
                }
                j += c.len_utf8();
            }
            if j > i + 1 && b.get(j) == Some(&b'}') {
                out.push((i, j + 1, &s[i + 1..j]));
                i = j + 1;
                continue;
            }
        }
        i += 1;
    }
    out
}

fn sub_keys(s: &str, mut f: impl FnMut(usize, &str) -> Option<Option<String>>) -> Option<String> {
    let mut out = String::with_capacity(s.len() + 16);
    let mut last = 0;
    for (a, b, key) in scan_keys(s) {
        out.push_str(&s[last..a]);
        match f(a, key)? {
            Some(v) => out.push_str(&v),
            None => out.push_str(&s[a..b]),
        }
        last = b;
    }
    out.push_str(&s[last..]);
    Some(out)
}

fn cap_first(v: &str) -> String {
    let mut it = v.chars();
    match it.next() {
        Some(c) => c.to_uppercase().collect::<String>() + it.as_str(),
        None => String::new(),
    }
}

fn starts_lower(v: &str) -> bool {
    v.chars().next().map_or(false, |c| c.is_lowercase())
}

fn sentence_start(prefix: &str, marks: &str) -> bool {
    match prefix.trim_end().chars().next_back() {
        None => true,
        Some(c) => marks.contains(c),
    }
}

fn pron<'a>(d: &'a Data, p: &Person, w: &str) -> Option<String> {
    let r = d.story.pron.get(&p.g)?.get(&w.to_lowercase())?;
    Some(if w.chars().next().map_or(false, |c| c.is_uppercase()) { cap_first(r) } else { r.clone() })
}

fn person_key(key: &str) -> Option<(char, Option<&str>)> {
    let mut it = key.chars();
    let c = it.next()?;
    if !matches!(c, 'A'..='D') {
        return None;
    }
    let rest = it.as_str();
    if rest.is_empty() {
        return Some((c, None));
    }
    let w = rest.strip_prefix('_')?;
    if w.is_empty() || !w.chars().all(is_word) {
        return None;
    }
    Some((c, Some(w)))
}

pub fn render_en(tpl: &str, people: &People, vars: &Vars) -> Option<String> {
    let d = data()?;
    sub_keys(tpl, |at, key| {
        if let Some((c, w)) = person_key(key) {
            let p = people.get(c)?;
            return Some(Some(match w {
                None => p.n.clone(),
                Some(w) => pron(d, p, w)?,
            }));
        }
        let val = vars.get(key)?;
        Some(Some(if starts_lower(val) && sentence_start(&tpl[..at], ".!?\"'") { cap_first(val) } else { val.clone() }))
    })
}

fn switches(tpl: &str, people: &People) -> Option<String> {
    static SW: std::sync::OnceLock<Option<regex::Regex>> = std::sync::OnceLock::new();
    let re = SW.get_or_init(|| regex::Regex::new(r"\{([A-D]+)\?([^|{}]*)\|([^|{}]*)\}").ok()).as_ref()?;
    let mut out = String::with_capacity(tpl.len());
    let mut last = 0;
    for c in re.captures_iter(tpl) {
        let m = c.get(0)?;
        out.push_str(&tpl[last..m.start()]);
        let mut fem = true;
        for ch in c.get(1)?.as_str().chars() {
            if people.get(ch)?.g != "f" {
                fem = false;
            }
        }
        out.push_str(c.get(if fem { 3 } else { 2 })?.as_str());
        last = m.end();
    }
    out.push_str(&tpl[last..]);
    Some(out)
}

pub fn render_pl(tpl: &str, people: &People, vars: &Vars) -> Option<String> {
    let d = data()?;
    let tpl2 = switches(tpl, people)?;
    sub_keys(&tpl2, |at, key| {
        let mut it = key.chars();
        if let (Some(c), None) = (it.next(), it.next()) {
            if let Some(p) = people.get(c) {
                return Some(Some(d.pl.special.get(&p.n).cloned().unwrap_or_else(|| p.n.clone())));
            }
        }
        let val = vars.get(key)?;
        Some(Some(if starts_lower(val) && sentence_start(&tpl2[..at], ".!?\"'„”") { cap_first(val) } else { val.clone() }))
    })
}

fn role_word(d: &Data, p: &Person, fk: &str, pl: bool) -> Option<String> {
    let r = if p.r.is_empty() { "scavenger" } else { p.r.as_str() };
    if pl {
        let pair = &d.pl.roles.get(r)?[if fk == "W" { 0 } else { 1 }];
        Some(if p.g == "f" { pair[1].clone() } else { pair[0].clone() })
    } else {
        let row = d.story.roles.iter().find(|x| x.0 == r)?;
        Some(if fk == "W" { row.1.clone() } else { row.2.clone() })
    }
}

pub fn render_lex(tpl: &str, people: &People, vars: &Vars, pl: bool, fk: &str) -> Option<String> {
    let d = data()?;
    let marks = ".!?\"'„”";
    let s = sub_keys(tpl, |at, key| {
        Some(vars.get(key).map(|val| if starts_lower(val) && sentence_start(&tpl[..at], marks) { cap_first(val) } else { val.clone() }))
    })?;
    let s = switches(&s, people)?;
    sub_keys(&s, |_, key| {
        if let Some((c, Some(w))) = person_key(key) {
            let p = people.get(c)?;
            return Some(Some(if w == "role" { role_word(d, p, fk, pl)? } else { pron(d, p, w)? }));
        }
        let mut it = key.chars();
        if let (Some(c), None) = (it.next(), it.next()) {
            if let Some(p) = people.get(c) {
                return Some(Some(p.n.clone()));
            }
        }
        Some(None)
    })
}

fn numword(n: &Value, words: &[String], people_fmt: bool) -> Option<String> {
    let i = match n {
        Value::Number(x) => x.as_i64().map(|i| i as f64).or_else(|| x.as_f64())?,
        Value::Bool(b) => *b as i64 as f64,
        _ => return None,
    };
    if i >= 0.0 && i < words.len() as f64 && i == i.trunc() {
        return words.get(i as usize).cloned();
    }
    if people_fmt {
        Some(format!("{} people", i.trunc() as i64))
    } else {
        Some(py_str(n))
    }
}

fn get_or<'a>(sym: &'a Map<String, Value>, k: &str, dflt: &'a Value) -> &'a Value {
    sym.get(k).unwrap_or(dflt)
}

fn fk_of(sym: &Map<String, Value>) -> Option<String> {
    match sym.get("f") {
        None => Some("W".into()),
        Some(Value::String(s)) => Some(s.clone()),
        Some(_) => None,
    }
}

pub fn expand_en(sym: &Map<String, Value>) -> Option<Vars> {
    let d = data()?;
    let fk = fk_of(sym)?;
    let f = d.story.faction.get(&fk)?;
    let mut v = Vars::new();
    v.insert("day".into(), py_str(get_or(sym, "day", &Value::from(0))));
    v.insert("week".into(), py_str(get_or(sym, "week", &Value::from(1))));
    v.insert("enemy".into(), f.enemy.clone());
    v.insert("enemies".into(), f.enemies.clone());
    v.insert("leader".into(), f.leader.clone());
    v.insert("faction".into(), f.plural.clone());
    if let Some(p) = sym.get("place") {
        v.insert("place".into(), py_str(p));
    }
    if let Some(i) = sym.get("inf") {
        let s = i.as_str()?;
        v.insert("inf".into(), s.to_string());
        v.insert("infs".into(), format!("{}s", s));
    }
    if let Some(ill) = sym.get("ill") {
        match ill.as_str().and_then(|k| d.story.illness.iter().find(|x| x.0 == k)) {
            Some(row) => {
                v.insert("ill".into(), row.1.clone());
                v.insert("ill_a".into(), row.2.clone());
                v.insert("cure".into(), row.3.clone());
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
    if let Some(n) = sym.get("n") {
        v.insert("n".into(), numword(n, &d.story.nums, false)?);
    }
    if let Some(n) = sym.get("lost") {
        v.insert("lost".into(), numword(n, &d.story.numw, true)?);
    }
    if let Some(n) = sym.get("gain") {
        v.insert("gain".into(), numword(n, &d.story.gainw, false)?);
    }
    if let Some(a) = sym.get("ago") {
        v.insert("ago".into(), py_str(a));
    }
    Some(v)
}

fn people_phrase(d: &Data, n: i64, case: &str, new: bool) -> String {
    if n <= 0 {
        return match case {
            "nom" => if new { "nikt nowy" } else { "nikt" },
            _ => if new { "nikogo nowego" } else { "nikogo" },
        }
        .into();
    }
    let one = n == 1;
    let few = (2..=4).contains(&n);
    if case == "gen" {
        let num = if n <= 10 { d.pl.card_gen.get(n as usize).cloned().unwrap_or_default() } else { n.to_string() };
        let noun = if one { if new { "nowej osoby" } else { "osoby" } } else if new { "nowych osób" } else { "osób" };
        return format!("{} {}", num, noun);
    }
    let mut num = if n <= 10 { d.pl.card_nom.get(n as usize).cloned().unwrap_or_default() } else { n.to_string() };
    let noun = if one {
        num = if case == "acc" { "jedną" } else { "jedna" }.into();
        if new {
            if case == "acc" { "nową osobę" } else { "nowa osoba" }
        } else if case == "acc" {
            "osobę"
        } else {
            "osoba"
        }
    } else if few {
        if new { "nowe osoby" } else { "osoby" }
    } else if new {
        "nowych osób"
    } else {
        "osób"
    };
    format!("{} {}", num, noun)
}

fn forms(table: &HashMap<String, Vec<String>>, v: &Value, n: usize) -> Vec<String> {
    match v.as_str().and_then(|k| table.get(k)) {
        Some(row) => row.clone(),
        None => vec![py_str(v); n],
    }
}

pub fn expand_pl(sym: &Map<String, Value>) -> Option<Vars> {
    let d = data()?;
    let fk = fk_of(sym)?;
    let mut v = Vars::new();
    v.insert("day".into(), py_str(get_or(sym, "day", &Value::from(0))));
    v.insert("week".into(), py_str(get_or(sym, "week", &Value::from(1))));
    if let Some(p) = sym.get("place") {
        let f = forms(&d.pl.places, p, 6);
        for (k, x) in ["place", "place_gen", "place_at", "place_to", "place_from", "place_ins"].iter().zip(f) {
            v.insert(k.to_string(), x);
        }
    }
    let en = d.pl.enemy.get(&fk)?;
    v.insert("enemy".into(), en.first()?.clone());
    v.insert("enemies".into(), en.first()?.clone());
    v.insert("enemy_gen".into(), en.get(1)?.clone());
    v.insert("enemy_ins".into(), en.get(2)?.clone());
    if let Some(i) = sym.get("inf") {
        let f = forms(&d.pl.inf, i, 6);
        for (k, x) in ["inf", "inf_gen", "inf_ins", "infs", "infs_gen", "inf_two"].iter().zip(f) {
            v.insert(k.to_string(), x);
        }
    }
    if let Some(a) = sym.get("ago") {
        v.insert("ago".into(), a.as_str().and_then(|k| d.pl.ago.get(k)).cloned().unwrap_or_else(|| py_str(a)));
    }
    if let Some(ill) = sym.get("ill") {
        let k = ill.as_str().filter(|k| d.pl.ill.contains_key(*k)).unwrap_or("none");
        let i = d.pl.ill.get(k)?;
        let c = d.pl.cure.get(k)?;
        for (n, x) in ["ill", "ill_gen", "ill_acc", "ill_ins"].iter().zip(i) {
            v.insert(n.to_string(), x.clone());
        }
        for (n, x) in ["cure", "cure_gen", "cure_acc", "cure_ins"].iter().zip(c) {
            v.insert(n.to_string(), x.clone());
        }
    }
    for (key, new) in [("n", false), ("lost", false), ("gain", true)] {
        if let Some(x) = sym.get(key) {
            let n = py_int(x)?;
            v.insert(key.into(), people_phrase(d, n, "nom", new));
            v.insert(format!("{}_acc", key), people_phrase(d, n, "acc", new));
            v.insert(format!("{}_gen", key), people_phrase(d, n, "gen", new));
        }
    }
    Some(v)
}

fn subset(sym: &Map<String, Value>) -> Map<String, Value> {
    let mut m = Map::new();
    for k in ["f", "day", "week", "inf", "ill"] {
        if let Some(v) = sym.get(k) {
            m.insert(k.into(), v.clone());
        }
    }
    m
}

const NUMS: [&str; 11] = ["no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"];

fn en_days(n: i64) -> String {
    let w = if (0..=10).contains(&n) { NUMS[n as usize].to_string() } else { n.to_string() };
    format!("{} day{}", w, if n == 1 { "" } else { "s" })
}

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
        "after" => {
            let a = match n {
                1 => "jednym dniu".to_string(),
                2 => "dwóch dniach".into(),
                3 => "trzech dniach".into(),
                4 => "czterech dniach".into(),
                5 => "pięciu dniach".into(),
                6 => "sześciu dniach".into(),
                7 => "siedmiu dniach".into(),
                _ => format!("{} dniach", n),
            };
            format!("po {}", a)
        }
        "ago" => {
            if n != 1 {
                format!("{} temu", nom)
            } else {
                "wczoraj".into()
            }
        }
        _ => format!("za {}", nom),
    }
}

fn pl_people(n: i64) -> String {
    let few = (2..=4).contains(&n.rem_euclid(10)) && !(12..=14).contains(&n.rem_euclid(100));
    format!("{} {}", n, if few { "osoby" } else if n != 1 { "osób" } else { "osoba" })
}

fn lex<'a>(table: &'a HashMap<String, Vec<String>>, v: &Value) -> Option<&'a Vec<String>> {
    table.get(v.as_str()?)
}

fn lex_list<'a>(table: &'a [Vec<String>], v: &Value) -> Option<&'a Vec<String>> {
    let k = v.as_str()?;
    table.iter().find(|r| r.first().map(String::as_str) == Some(k))
}

fn season_idx(d: &Data, v: &Value) -> Option<usize> {
    let i = exact_int(v)?;
    let n = d.alife.seasons.len() as i64;
    let j = if i < 0 { i + n } else { i };
    if (0..n).contains(&j) {
        Some(j as usize)
    } else {
        None
    }
}

fn size_ppl_en(v: &Value) -> Option<String> {
    let n = match v {
        Value::Number(x) => x.as_f64()?,
        Value::Bool(b) => *b as i64 as f64,
        _ => return None,
    };
    if n > 0.0 && n <= 10.0 && n == n.trunc() {
        Some(format!("{} people", NUMS[n as usize]))
    } else {
        Some(format!("{} people", n.trunc() as i64))
    }
}

pub fn with_prep(ins: &str) -> Option<String> {
    Some(format!("{}{}", if res()?.ze.is_match(ins) { "ze " } else { "z " }, ins))
}

const COPY_KEYS: [&str; 8] = ["sup", "need", "size", "sick_n", "hungry_n", "due", "jd", "n"];

pub fn lex_expand_en(sym: &Map<String, Value>) -> Option<Vars> {
    let d = data()?;
    let a = &d.alife;
    let fk = fk_of(sym)?;
    let mut v = expand_en(&subset(sym))?;
    if let Some(x) = sym.get("dest") {
        v.insert("dest".into(), a.places.get(&fk)?.get(x.as_str()?)?.first()?.clone());
    }
    if let Some(x) = sym.get("place") {
        v.insert("place".into(), py_str(x));
    }
    for (key, table, idx) in [("goal", &a.goods, 1usize), ("proj", &a.projects, 1), ("grp", &a.groups, 1), ("inj", &a.injuries, 0), ("keep", &a.keeps, 0)] {
        if let Some(x) = sym.get(key) {
            v.insert(key.into(), lex(table, x)?.get(idx)?.clone());
        }
    }
    if let Some(x) = sym.get("job") {
        v.insert("job".into(), lex_list(&a.jobs, x)?.get(2)?.clone());
    }
    if let Some(x) = sym.get("skill") {
        v.insert("skill".into(), lex(&a.skills, x)?.get(1)?.clone());
    }
    if let Some(x) = sym.get("orig") {
        v.insert("orig".into(), lex_list(&a.origins, x)?.get(2)?.clone());
    }
    if let Some(x) = sym.get("prog") {
        v.insert("prog".into(), a.prog.get(&exact_int(x)?.to_string())?.first()?.clone());
    }
    if let Some(x) = sym.get("nd") {
        let n = py_int(x)?;
        v.insert("nd".into(), en_days(n));
        v.insert("nd_after".into(), format!("after {}", en_days(n)));
        v.insert("nd_ago".into(), if n == 1 { "yesterday".into() } else { format!("{} ago", en_days(n)) });
        v.insert("nd_in".into(), format!("in {}", en_days(n)));
    }
    if let Some(x) = sym.get("wx") {
        v.insert("wx".into(), py_str(x));
    }
    if let Some(x) = sym.get("season") {
        v.insert("season".into(), a.seasons.get(season_idx(d, x)?)?.0.clone());
    }
    for k in COPY_KEYS {
        if let Some(x) = sym.get(k) {
            v.insert(k.into(), py_str(x));
        }
    }
    if let Some(x) = sym.get("size") {
        v.insert("size_ppl".into(), size_ppl_en(x)?);
    }
    Some(v)
}

pub fn lex_expand_pl(sym: &Map<String, Value>) -> Option<Vars> {
    let d = data()?;
    let a = &d.alife;
    let fk = fk_of(sym)?;
    let mut v = expand_pl(&subset(sym))?;
    if let Some(x) = sym.get("dest") {
        let row = a.places.get(&fk)?.get(x.as_str()?)?;
        for (suf, form) in ["", "_gen", "_at", "_to", "_from"].iter().zip(row.iter().skip(1)) {
            v.insert(format!("dest{}", suf), form.clone());
        }
    }
    if let Some(x) = sym.get("place") {
        v.insert("place".into(), py_str(x));
    }
    for (key, table) in [("goal", &a.goods), ("proj", &a.projects), ("grp", &a.groups)] {
        if let Some(x) = sym.get(key) {
            let row = lex(table, x)?;
            for (suf, form) in ["", "_gen", "_acc", "_ins"].iter().zip(row.iter().skip(2)) {
                v.insert(format!("{}{}", key, suf), form.clone());
            }
            v.insert(format!("{}_with", key), with_prep(row.get(5)?)?);
            v.insert(format!("{}_nom", key), row.get(2)?.clone());
        }
    }
    if let Some(x) = sym.get("inj") {
        let row = lex(&a.injuries, x)?;
        for (suf, form) in ["", "_gen", "_acc", "_ins"].iter().zip(row.iter().skip(1)) {
            v.insert(format!("inj{}", suf), form.clone());
        }
        v.insert("inj_with".into(), with_prep(row.get(4)?)?);
        v.insert("inj_nom".into(), row.get(1)?.clone());
    }
    if let Some(x) = sym.get("keep") {
        let row = lex(&a.keeps, x)?;
        for (suf, form) in ["", "_gen", "_acc", "_ins"].iter().zip(row.iter().skip(1)) {
            v.insert(format!("keep{}", suf), form.clone());
        }
        v.insert("keep_nom".into(), row.get(1)?.clone());
    }
    if let Some(x) = sym.get("job") {
        v.insert("job".into(), lex_list(&a.jobs, x)?.get(3)?.clone());
    }
    if let Some(x) = sym.get("skill") {
        v.insert("skill".into(), lex(&a.skills, x)?.get(2)?.clone());
    }
    if let Some(x) = sym.get("orig") {
        v.insert("orig".into(), lex_list(&a.origins, x)?.get(3)?.clone());
    }
    if let Some(x) = sym.get("prog") {
        v.insert("prog".into(), a.prog.get(&exact_int(x)?.to_string())?.get(1)?.clone());
    }
    if let Some(x) = sym.get("nd") {
        let n = py_int(x)?;
        for kind in ["nom", "after", "ago", "in"] {
            v.insert(if kind == "nom" { "nd".into() } else { format!("nd_{}", kind) }, pl_days(n, kind));
        }
    }
    if let Some(x) = sym.get("season") {
        v.insert("season".into(), a.seasons.get(season_idx(d, x)?)?.1.clone());
    }
    for k in COPY_KEYS {
        if let Some(x) = sym.get(k) {
            v.insert(k.into(), py_str(x));
        }
    }
    if let Some(x) = sym.get("size") {
        v.insert("size_ppl".into(), pl_people(py_int(x)?));
    }
    Some(v)
}

pub fn short_text(text: &str, limit: usize) -> String {
    if clen(text) <= limit {
        return text.to_string();
    }
    let mut out = String::new();
    if let Some(r) = res() {
        for m in r.sentence.find_iter(text) {
            let p = m.as_str();
            if clen(&out) + clen(p.trim_end()) <= limit {
                out.push_str(p);
            } else {
                break;
            }
        }
    }
    let out = out.trim();
    if !out.is_empty() {
        return out.to_string();
    }
    let cut: String = text.chars().take(limit.saturating_sub(1)).collect();
    let chars: Vec<char> = cut.chars().collect();
    let k = chars.iter().rposition(|c| matches!(c, ',' | ';' | ' ')).map(|x| x as i64).unwrap_or(-1);
    let base: String = if k > 40 { chars[..k as usize].iter().collect() } else { cut.clone() };
    format!("{}…", base.trim_end_matches([' ', ',', ';']))
}

pub fn widget_text(text: &str) -> String {
    let mut t = short_text(&text.replace('\n', " ").replace('|', "/"), WIDGET_MAX);
    while t.len() > 150 {
        let n = clen(&t);
        let head: String = t.chars().take(n.saturating_sub(2)).collect();
        t = format!("{}…", head.trim_end());
    }
    t
}

pub fn split_sentences(text: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut it = text.char_indices().peekable();
    let mut prev: Option<char> = None;
    while let Some((i, c)) = it.next() {
        if c.is_whitespace() && matches!(prev, Some('.') | Some('!') | Some('?')) {
            let mut end = i + c.len_utf8();
            while let Some(&(j, d)) = it.peek() {
                if d.is_whitespace() {
                    end = j + d.len_utf8();
                    it.next();
                } else {
                    break;
                }
            }
            out.push(&text[start..i]);
            start = end;
            prev = None;
            continue;
        }
        prev = Some(c);
    }
    out.push(&text[start..]);
    out
}

pub fn has_word(s: &str, name: &str) -> bool {
    if name.is_empty() {
        return true;
    }
    let mut from = 0;
    while let Some(i) = s[from..].find(name) {
        let a = from + i;
        let b = a + name.len();
        let before = s[..a].chars().next_back().map_or(false, is_word);
        let after = s[b..].chars().next().map_or(false, is_word);
        if !before && !after {
            return true;
        }
        from = a + s[a..].chars().next().map_or(1, |c| c.len_utf8());
    }
    false
}

fn combat_key(t: &str, vars: &std::collections::HashSet<String>) -> bool {
    for (_, _, key) in scan_keys(t) {
        let chars: Vec<(usize, char)> = key.char_indices().collect();
        let mut g = key;
        for (n, (i, c)) in chars.iter().enumerate() {
            if n >= 1 && *c == '_' && n + 1 < chars.len() {
                g = &key[..*i];
                break;
            }
        }
        if vars.contains(g) {
            return true;
        }
    }
    false
}

pub fn combat_text(t: &str, pl: bool) -> bool {
    let (d, r) = match (data(), res()) {
        (Some(d), Some(r)) => (d, r),
        _ => return false,
    };
    if t.is_empty() {
        return false;
    }
    if combat_key(t, &d.alife.combat_vars) {
        return true;
    }
    if pl {
        r.combat_pl.is_match(t)
    } else {
        r.combat_en.is_match(t)
    }
}

pub fn kid_text_ok(text: &str, people: &People, pl: bool) -> bool {
    let ks: Vec<&str> = people.0.iter().filter(|(_, p)| p.r == "kid" && !p.n.is_empty()).map(|(_, p)| p.n.as_str()).collect();
    if ks.is_empty() || !combat_text(text, pl) {
        return true;
    }
    for s in split_sentences(text) {
        if combat_text(s, pl) && ks.iter().any(|n| has_word(s, n)) {
            return false;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn p(n: &str, g: &str) -> Person {
        Person { n: n.into(), g: g.into(), r: String::new() }
    }

    #[test]
    fn renders() {
        let mut ppl = People::default();
        ppl.set('A', p("Lev", "m"));
        ppl.set('B', p("Ana", "f"));
        let mut v = Vars::new();
        v.insert("place".into(), "stadium".into());
        assert_eq!(render_en("{A_He} and {B} went to the {place}. {place} is far.", &ppl, &v).unwrap(), "He and Ana went to the stadium. Stadium is far.");
        assert!(render_en("{C} is here.", &ppl, &v).is_none());
        assert_eq!(render_pl("{A} poszedł{A?|a}, {B} poszł{B?o|a}.", &ppl, &v).unwrap(), "Lev poszedł, Ana poszła.");
        assert_eq!(render_lex("{A_role} {X} {A}", &ppl, &v, false, "W").unwrap(), "scavenger {X} Lev");
    }

    #[test]
    fn widget() {
        let long = "One sentence that is quite long but fine. Another sentence that pushes the total past the limit for the widget, surely.";
        assert_eq!(widget_text(long), "One sentence that is quite long but fine.");
        assert_eq!(split_sentences("A b. C d!  E"), vec!["A b.", "C d!", "E"]);
        assert!(has_word("x Bob.", "Bob") && !has_word("Bobby", "Bob"));
    }
}
