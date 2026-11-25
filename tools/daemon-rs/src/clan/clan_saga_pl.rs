//! py: clan_saga_pl.py - the Polish side of the clan saga: the Polish template registry (one Polish line for every English line of clan_saga_text*.py) and the
//! Polish forms of every variable (`expand`). A Python exception is `Err(String)` (the saga catches it per row: no Polish text).
use crate::clan::clan::pystr;
use crate::clan::{clan_chronicle as ch, clan_story_pl as pln, data};
use regex::Regex;
use serde_json::Value;
use std::collections::HashMap;
use std::sync::LazyLock;

pub type Vars = HashMap<String, String>;

pub const NW: [&str; 11] = ["no", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten"];

fn pd(name: &str) -> &'static Value {
    data::t("clan_saga_pl_data", name)
}

static ZE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(?i)^(s|z|ś|ź|ż|w[bcćdfghjklłmnńprsśtwzźż])").expect("ZE"));
static WE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(?i)^[wf][bcćdfghjklłmnńprsśtwzźż]").expect("WE"));
static AIR_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^the (smell|sound) of ([^\n]*)$").expect("AIR_RE"));

fn s_of(v: &Value) -> String {
    v.as_str().unwrap_or("").to_string()
}

/// `PD.<table>[key]` as a list of strings (KeyError = Err).
fn row(table: &str, key: &str) -> Result<Vec<String>, String> {
    pd(table).get(key).and_then(|v| v.as_array()).map(|a| a.iter().map(s_of).collect()).ok_or_else(|| format!("KeyError: {}[{}]", table, key))
}

/// `PD.NUM_x[n]`.
fn num(table: &str, n: i64) -> Result<String, String> {
    let len = pd(table).as_array().map(|a| a.len() as i64).unwrap_or(0);
    let ix = if n < 0 { n + len } else { n }; // Python negative index
    if ix < 0 {
        return Err(format!("IndexError: {}[{}]", table, n));
    }
    pd(table).get(ix as usize).map(s_of).ok_or_else(|| format!("IndexError: {}[{}]", table, n))
}

/// Python `str.splitlines()`.
pub(crate) fn splitlines(s: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let b: Vec<(usize, char)> = s.char_indices().collect();
    let mut i = 0;
    while i < b.len() {
        let (p, c) = b[i];
        let brk = matches!(c, '\n' | '\r' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}' | '\u{2029}');
        if brk {
            out.push(&s[start..p]);
            let mut next = p + c.len_utf8();
            if c == '\r' && i + 1 < b.len() && b[i + 1].1 == '\n' {
                next += 1;
                i += 1;
            }
            start = next;
        }
        i += 1;
    }
    if start < s.len() {
        out.push(&s[start..]);
    }
    out
}

// py: clan_saga_pl.py:35 _prep
/// 'w' -> 'we' before a word that begins with w / f and a consonant.
pub fn prep(p: &str, loc: &str) -> String {
    if p == "w" && WE.is_match(loc) {
        "we".into()
    } else {
        p.to_string()
    }
}

// py: clan_saga_pl.py:40 _from
pub fn from_(gen: &str) -> String {
    format!("{}{}", if ZE.is_match(gen) { "ze " } else { "z " }, gen)
}

// py: clan_saga_pl.py:44 _num
/// 'five' -> 5, '12' -> 12.
pub fn num_of(word: &str) -> Result<i64, String> {
    let w = crate::py::strip(word).to_lowercase();
    if let Some(i) = NW.iter().position(|x| *x == w) {
        return Ok(i as i64);
    }
    w.parse::<i64>().map_err(|_| format!("ValueError: invalid literal for int() with base 10: {}", crate::py::repr_str(&w)))
}

/// `str(val).split(" ")[0]` (Python split with a separator keeps empty parts).
fn first_word(val: &str) -> &str {
    val.split(' ').next().unwrap_or("")
}

// py: clan_saga_pl.py:52 _few
fn few(n: i64) -> bool {
    (2..=4).contains(&crate::py::pymod(n, 10)) && !(12..=14).contains(&crate::py::pymod(n, 100))
}

fn n_or(table: &str, n: i64) -> Result<String, String> {
    if n <= 10 {
        num(table, n)
    } else {
        Ok(n.to_string())
    }
}

// py: clan_saga_pl.py:56 times
/// 'raz' / 'dwa razy' / 'pięć razy' / '12 razy'.
pub fn times(n: i64) -> Result<String, String> {
    if n == 1 {
        return Ok("raz".into());
    }
    Ok(format!("{} razy", n_or("NUM_M", n)?))
}

// py: clan_saga_pl.py:63 count
/// A counted noun in the nominative / accusative of things.
pub fn count(n: i64, one: &str, few_: &str, many: &str, fem: bool) -> Result<String, String> {
    let nm = n_or(if fem { "NUM_F" } else { "NUM_M" }, n)?;
    Ok(format!("{} {}", nm, if n == 1 { one } else if few(n) { few_ } else { many }))
}

// py: clan_saga_pl.py:69 ex_phrase
pub fn ex_phrase(n: i64, case: &str) -> Result<String, String> {
    if case == "gen" {
        let nm = if n <= 10 { if n == 1 { "jednej".to_string() } else { num("NUM_GEN", n)? } } else { n.to_string() };
        return Ok(format!("{} egzekucji", nm));
    }
    if n == 1 {
        return Ok(if case == "acc" { "jedną egzekucję" } else { "jedna egzekucja" }.into());
    }
    Ok(format!("{} {}", n_or("NUM_F", n)?, if few(n) { "egzekucje" } else { "egzekucji" }))
}

// py: clan_saga_pl.py:79 hs_phrase
pub fn hs_phrase(n: i64, case: &str) -> Result<String, String> {
    if case == "gen" {
        let nm = if n <= 10 { num("NUM_GEN", n)? } else { n.to_string() };
        return Ok(format!("{} {}", nm, if n == 1 { "strzału w głowę" } else { "strzałów w głowę" }));
    }
    count(n, "strzał w głowę", "strzały w głowę", "strzałów w głowę", false)
}

// py: clan_saga_pl.py:86 unit_phrase
pub fn unit_phrase(n: i64, case: &str) -> Result<String, String> {
    if case == "gen" {
        if n == 1 {
            return Ok("jednej jednostki".into());
        }
        let nm = if n <= 10 { num("NUM_GEN", n)? } else { n.to_string() };
        return Ok(format!("{} jednostek", nm));
    }
    if n == 1 {
        return Ok(if case == "acc" { "jedną jednostkę" } else { "jedna jednostka" }.into());
    }
    Ok(format!("{} {}", n_or("NUM_F", n)?, if few(n) { "jednostki" } else { "jednostek" }))
}

// py: clan_saga_pl.py:95 ago
/// 'yesterday' / 'today' / 'three days ago' -> 'wczoraj' / 'dzisiaj' / 'trzy dni temu'.
pub fn ago(en: &str) -> Result<String, String> {
    if en == "yesterday" {
        return Ok("wczoraj".into());
    }
    if en == "today" {
        return Ok("dzisiaj".into());
    }
    let n = num_of(first_word(en))?;
    Ok(format!("{} dni temu", n_or("NUM_M", n)?))
}

// py: clan_saga_pl.py:105 gone
/// 'gone since yesterday' / 'gone three days' -> 'od wczoraj' / 'od trzech dni'.
pub fn gone(en: &str) -> Result<String, String> {
    if en.contains("yesterday") {
        return Ok("od wczoraj".into());
    }
    let w = en.split(' ').nth(1).ok_or("IndexError: split")?;
    let n = num_of(w)?;
    Ok(format!("od {} dni", n_or("NUM_GEN", n)?))
}

// py: clan_saga_pl.py:112 losses
pub fn losses(en: &str) -> Result<String, String> {
    let n = num_of(en)?;
    Ok(format!("{} {}", n_or("NUM_F", n)?, if few(n) { "porażki" } else { "porażek" }))
}

fn put(out: &mut Vars, key: &str, val: String) {
    out.insert(key.to_string(), val);
}

// py: clan_saga_pl.py:121 _acc
fn acc(nom: &str) -> String {
    if let Some(a) = pd("ACC").get(nom).and_then(|v| v.as_str()) {
        return a.to_string();
    }
    if let Some(rest) = nom.strip_prefix("mapa ") {
        return format!("mapę {}", rest);
    }
    nom.to_string()
}

// py: clan_saga_pl.py:129 _group
/// (nom, gen, prep, loc, ins) -> pre pre_gen pre_acc pre_loc pre_at pre_to pre_from pre_ins.
fn group(out: &mut Vars, pre: &str, spec: &[String]) -> Result<(), String> {
    if spec.len() != 5 {
        return Err("ValueError: group".into());
    }
    let (nom, gen, p, loc, ins) = (&spec[0], &spec[1], &spec[2], &spec[3], &spec[4]);
    put(out, pre, nom.clone());
    put(out, &format!("{}_gen", pre), gen.clone());
    put(out, &format!("{}_acc", pre), acc(nom));
    put(out, &format!("{}_loc", pre), loc.clone());
    put(out, &format!("{}_at", pre), format!("{} {}", prep(p, loc), loc));
    put(out, &format!("{}_to", pre), format!("do {}", gen));
    put(out, &format!("{}_from", pre), from_(gen));
    put(out, &format!("{}_ins", pre), ins.clone());
    Ok(())
}

// py: clan_saga_pl.py:141 _map_spec
/// EN 'the Hospital' / 'Downtown' / a fallback place -> the Polish group.
fn map_spec(val: &str) -> Result<Vec<String>, String> {
    let name = val.strip_prefix("the ").unwrap_or(val);
    let name = data::t("clan_chronicle_maps", "EN_SHORT").get(name).and_then(|v| v.as_str()).unwrap_or(name);
    if let Ok(r) = row("MAPS", name) {
        return Ok(r);
    }
    if let Ok(r) = row("PLACES", val) {
        return Ok(r);
    }
    if let Some(pl) = data::t("clan_chronicle_maps", "PL").get(name).and_then(|v| v.as_str()).filter(|s| !s.is_empty()) {
        return Ok(vec![format!("mapa {}", pl), format!("mapy {}", pl), "na".into(), format!("mapie {}", pl), format!("mapą {}", pl)]);
    }
    Err(format!("KeyError: no Polish map name for {}", crate::py::repr_str(val)))
}

// py: clan_saga_pl.py:155 _sg
fn sg(out: &mut Vars, pre: &str, spec: &[String], names: &[&str]) {
    for (nm, val) in names.iter().zip(spec.iter()) {
        put(out, &format!("{}{}", pre, nm), val.clone());
    }
}

fn fear_what_map() -> &'static HashMap<String, String> {
    static M: LazyLock<HashMap<String, String>> = LazyLock::new(|| {
        let mut m = HashMap::new();
        for f in data::t("clan_saga_data", "FEARS").as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
            let (en, what) = (s_of(&f[0]), s_of(&f[1]));
            if let Some(pl) = pd("FEARS").get(&en).and_then(|x| x.get(1)).and_then(|x| x.as_str()) {
                m.insert(what, pl.to_string());
            }
        }
        m
    });
    &M
}

/// `_items()`: {EN many: (PL few, PL many)} of the chronicle's USE table (a later row with the same key wins).
fn items_map() -> &'static HashMap<String, (String, String)> {
    static M: LazyLock<HashMap<String, (String, String)>> = LazyLock::new(|| {
        let mut m = HashMap::new();
        if let Some(o) = data::t("clan_chronicle", "USE").as_object() {
            for (_k, e) in o {
                m.insert(s_of(&e[2]), (s_of(&e[5]), s_of(&e[6])));
            }
        }
        m
    });
    &M
}

// py: clan_saga_pl.py:327 past_pl
/// The Polish past of a member in the right gender, from the chronicle's PASTS table (English text -> the m / f Polish pair).
pub fn past_pl(en_past: &str, g: &str) -> Option<String> {
    let low = en_past.to_lowercase();
    for p in ch::pasts().as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
        if s_of(&p[2]).to_lowercase() == low {
            return Some(s_of(if g == "f" { &p[4] } else { &p[3] }));
        }
    }
    None
}

/// `g` of a person dict of the template people (default "m").
fn gender_of(people: &Value, letter: &str) -> String {
    people.get(letter).filter(|p| crate::clan::clan::truthy(p)).and_then(|p| p.get("g")).map(pystr).unwrap_or_else(|| "m".to_string())
}

fn needs<'a, T>(o: Option<T>, what: &'a str) -> Result<T, String> {
    o.ok_or_else(|| format!("KeyError: {}", what))
}

// py: clan_saga_pl.py:180 expand
/// English variables of a saga entry -> the Polish variables. `Err` = a value the tables do not know (KeyError / ValueError ...). `over` = the rich-loot overrides.
pub fn expand(v: &serde_json::Map<String, Value>, people: &Value, fk: &str, over: Option<&Vars>) -> Result<Vars, String> {
    let mut out: Vars = HashMap::new();
    for (key, valv) in v.iter() {
        if valv.is_null() {
            continue;
        }
        let key = key.as_str();
        let val = pystr(valv);
        let val = val.as_str();
        match key {
            "map" | "prev_map" | "kmap" | "ymap" => group(&mut out, key, &map_spec(val)?)?,
            "place_gen" => continue,
            "place" | "place2" | "place3" | "kplace" => group(&mut out, key, &row("PLACES", val)?)?,
            "camp" => group(&mut out, key, &row("CAMPS", val)?)?,
            "leader" => sg(&mut out, "leader", &row("LEADERS", val)?, &["", "_gen", "_ins"]),
            "enemies" | "enemy" => {
                let name = if val.starts_with("the ") { val.to_string() } else { format!("the {}", val) };
                let e = row("ENEMIES", &name)?;
                sg(&mut out, key, &e[..5.min(e.len())], &["", "_gen", "_ins", "_dat", "_loc"]);
            }
            "enemy_n" => {
                let e = row("ENEMIES", &format!("the {}", if val == "Scar" { "Scars" } else { "Wolves" }))?;
                put(&mut out, "enemy_n", needs(e.get(5).cloned(), "ENEMIES[5]")?);
            }
            "smell" | "sound" | "air" => {
                let m = needs(AIR_RE.captures(val), "air match")?;
                let noun = if &m[1] == "smell" { "zapach" } else { "dźwięk" };
                let g = needs(pd("AIR").get(&m[2]).map(s_of), "AIR")?;
                put(&mut out, key, format!("{} {}", noun, g));
                put(&mut out, &format!("{}_gen", key), format!("{}u {}", noun, g));
                put(&mut out, &format!("{}_ins", key), format!("{}em {}", noun, g));
            }
            "k" => {
                let n = int_of_str(val)?;
                put(&mut out, "k", n.to_string());
                put(&mut out, "k_dn", format!("{} powalon{}", n, if n == 1 { "y" } else { "ych" }));
            }
            "ex_p" => {
                let n = num_of(first_word(val))?;
                for c in ["nom", "acc", "gen"] {
                    put(&mut out, &format!("ex_{}", c), ex_phrase(n, c)?);
                }
            }
            "ex_t" => {
                let alpha = val.chars().next().map(|c| c.is_alphabetic()).ok_or("IndexError: ex_t")?;
                let mut n = if alpha && val != "once" && val != "twice" { num_of(first_word(val))? } else { 0 };
                n = if val == "once" { 1 } else if val == "twice" { 2 } else { n };
                put(&mut out, "ex_t", times(n)?);
            }
            "dn_p" | "dt_p" => {
                let n = if val == "once" { 1 } else if val == "twice" { 2 } else { num_of(first_word(val))? };
                put(&mut out, key, times(n)?);
            }
            "rv_p" => {
                let n = num_of(first_word(val))?;
                for c in ["nom", "acc", "gen"] {
                    put(&mut out, &format!("rv_{}", c), format!("{} z drużyny", pln::people_phrase(n, c, false)));
                }
            }
            "hs_p" => {
                let n = num_of(first_word(val))?;
                for c in ["nom", "acc", "gen"] {
                    put(&mut out, &format!("hs_{}", c), hs_phrase(n, if c == "acc" { "nom" } else { c })?);
                }
            }
            "wpn" => sg(&mut out, "wpn", &row("WEAPONS", val)?, &["", "_acc", "_gen", "_ins"]),
            "site" | "ysite" | "dsite" => {
                let s = row("SITES", val)?;
                if s.len() != 5 {
                    return Err("ValueError: SITES".into());
                }
                let (nom, gen, acc_, loc, ins) = (&s[0], &s[1], &s[2], &s[3], &s[4]);
                put(&mut out, key, nom.clone());
                put(&mut out, &format!("{}_gen", key), gen.clone());
                put(&mut out, &format!("{}_from", key), from_(gen));
                put(&mut out, &format!("{}_acc", key), acc_.clone());
                put(&mut out, &format!("{}_w", key), format!("w {}", acc_));
                put(&mut out, &format!("{}_loc", key), loc.clone());
                put(&mut out, &format!("{}_at", key), format!("na {}", loc));
                put(&mut out, &format!("{}_ins", key), ins.clone());
            }
            "cloth" | "dcloth" => sg(&mut out, key, &row("CLOTHS", val)?, &["", "_gen", "_ins"]),
            "cause" => {
                let en_n = if fk == "W" { "Scar" } else { "Wolf" };
                let causes = data::t("clan_saga_data", "CAUSES").as_array().cloned().unwrap_or_default();
                let mut idx = None;
                for (i, c) in causes.iter().enumerate() {
                    if crate::clan::clan_loot::pyformat(&s_of(c), &[("enemy_n", en_n)])? == val {
                        idx = Some(i);
                        break;
                    }
                }
                let idx = needs(idx, "ValueError: not in list")?;
                let e = row("ENEMIES", if fk == "W" { "the Scars" } else { "the Wolves" })?;
                let e5 = needs(e.get(5).cloned(), "ENEMIES[5]")?;
                let trip = needs(pd("CAUSES").get(idx).and_then(|x| x.as_array()), "CAUSES[idx]")?;
                let mut forms = Vec::new();
                for x in trip.iter().take(3) {
                    forms.push(crate::clan::clan_loot::pyformat(&s_of(x), &[("e", &e5)])?);
                }
                if forms.len() != 3 {
                    return Err("ValueError: CAUSES".into());
                }
                put(&mut out, "cause", forms[0].clone());
                put(&mut out, "cause_acc", forms[0].clone());
                put(&mut out, "cause_gen", forms[1].clone());
                put(&mut out, "cause_ins", forms[2].clone());
            }
            "job" => sg(&mut out, "job", &row("JOBS", val)?, &["", "_neg"]),
            "poss" | "dpos" | "dposx" => sg(&mut out, key, &row("POSS", val)?, &["", "_acc", "_gen", "_ins"]),
            "poss_short" => {
                let mut nom: Option<String> = None;
                if let Some(o) = pd("POSS").as_object() {
                    for (en, spec) in o {
                        let tail = en.split_once(' ').map(|x| x.1).unwrap_or(en.as_str());
                        if tail == val || en == val {
                            nom = spec.get(0).map(s_of);
                            break;
                        }
                    }
                }
                put(&mut out, key, nom.unwrap_or_else(|| val.to_string()));
            }
            "kitem" | "kitem_a" => {
                let k = match val.split_once(' ') {
                    Some((a, rest)) if a == "a" || a == "an" => rest.to_string(),
                    None if val == "a" || val == "an" => return Err("IndexError: kitem".into()),
                    _ => val.to_string(),
                };
                sg(&mut out, key, &row("KEEPS", &k)?, &["", "_acc", "_gen", "_ins"]);
            }
            "fear" => {
                let r = row("FEARS", val)?;
                put(&mut out, "fear", needs(r.first().cloned(), "FEARS[0]")?);
            }
            "fear_what" => put(&mut out, "fear_what", needs(fear_what_map().get(val).cloned(), "fear_what")?),
            "past" => {
                let g = gender_of(people, "A");
                let pp = needs(past_pl(val, &g), &format!("no Polish past for {}", val))?;
                put(&mut out, "past", pp);
            }
            "lack_n" => {
                let n = num_of(first_word(val))?;
                for c in ["nom", "acc", "gen"] {
                    put(&mut out, &format!("lack_n{}", if c == "nom" { String::new() } else { format!("_{}", c) }), unit_phrase(n, c)?);
                }
            }
            "lack_days" => {
                let n = num_of(first_word(val))?;
                put(&mut out, key, count(n, "dzień", "dni", "dni", false)?);
            }
            "dgo" => put(&mut out, "dgo", gone(val)?),
            "kd_ago" | "ago" => put(&mut out, key, ago(val)?),
            "nth" => put(&mut out, "nth", needs(pd("NTH").get(val).map(s_of), "NTH")?),
            "lossw" => put(&mut out, "lossw", losses(val)?),
            "prev_res" => put(&mut out, "prev_res", needs(pd("PREV_RES").get(val).map(s_of), "PREV_RES")?),
            "n_left" => put(&mut out, "n_left", if val.starts_with("none") { "nic nie zostało".to_string() } else { format!("zostało {}", val.split(' ').nth(0).unwrap_or("")) }),
            "ammo_n" => put(&mut out, key, count(num_of(first_word(val))?, "nabój", "naboje", "naboi", false)?),
            "food_n" => put(&mut out, key, count(num_of(first_word(val))?, "puszka", "puszki", "puszek", true)?),
            "item_pl" | "item_short" => {
                let (nom, gen) = needs(items_map().get(val).cloned(), "items")?;
                put(&mut out, key, nom);
                put(&mut out, &format!("{}_gen", key), gen);
            }
            "AW" => {
                let g = gender_of(people, "B");
                let fem = g == "f";
                put(&mut out, "AW", val.to_string());
                put(&mut out, "AW_go", if fem { "ją" } else { "go" }.into());
                put(&mut out, "AW_niego", if fem { "nią" } else { "niego" }.into());
                put(&mut out, "AW_mu", if fem { "jej" } else { "mu" }.into());
                put(&mut out, "AW_jego", if fem { "jej" } else { "jego" }.into());
            }
            _ => put(&mut out, key, val.to_string()),
        }
    }
    if let Some(o) = over {
        for (k, x) in o {
            out.insert(k.clone(), x.clone());
        }
    }
    Ok(out)
}

/// Python `int(str)`.
fn int_of_str(s: &str) -> Result<i64, String> {
    crate::py::strip(s).parse::<i64>().map_err(|_| format!("ValueError: invalid literal for int(): {}", crate::py::repr_str(s)))
}

// ------------------------------------------------------------------ the Polish template registry
// py: clan_saga_pl.py:354 _flat
/// [(pool, scope, line)] of one TEXT module.
fn flat(text: &str) -> Vec<(String, String, String)> {
    let mut out = Vec::new();
    let mut cur: Option<String> = None;
    for line in splitlines(text) {
        let line = crate::py::strip(line);
        if line.is_empty() {
            continue;
        }
        if let Some(h) = line.strip_prefix("== ") {
            cur = Some(crate::py::strip(h).to_string());
            continue;
        }
        let mut scope = "*".to_string();
        let mut l = line.to_string();
        if line.starts_with("W:") || line.starts_with("S:") {
            scope = line[..1].to_string();
            l = crate::py::strip(&line[2..]).to_string();
        }
        out.push((cur.clone().unwrap_or_default(), scope, l));
    }
    out
}

pub const EN_MODS: [&str; 5] = ["clan_saga_text", "clan_saga_text2", "clan_saga_text3", "clan_saga_text4", "clan_saga_text_sup"];
pub const PL_MODS: [&str; 5] = ["clan_saga_pl_text", "clan_saga_pl_text2", "clan_saga_pl_text3", "clan_saga_pl_text4", "clan_saga_pl_text_sup"];

// py: clan_saga_pl.py:370 _build / REG
/// (pool, English template) -> Polish template (the first Polish line of a pair wins).
pub static REG: LazyLock<HashMap<(String, String), String>> = LazyLock::new(|| {
    let mut reg: HashMap<(String, String), String> = HashMap::new();
    for (en_mod, pl_mod) in EN_MODS.iter().zip(PL_MODS.iter()) {
        let en = flat(data::t(en_mod, "TEXT").as_str().unwrap_or(""));
        let pl = flat(data::t(pl_mod, "TEXT").as_str().unwrap_or(""));
        for (e, p) in en.iter().zip(pl.iter()) {
            reg.entry((e.0.clone(), e.2.clone())).or_insert_with(|| p.2.clone());
        }
    }
    reg
});

/// The registry holds one Polish line for every English line (the Python `_build` raises when the files differ in length or in pool / scope).
pub fn reg_ok() -> Result<(), String> {
    for (en_mod, pl_mod) in EN_MODS.iter().zip(PL_MODS.iter()) {
        let en = flat(data::t(en_mod, "TEXT").as_str().unwrap_or(""));
        let pl = flat(data::t(pl_mod, "TEXT").as_str().unwrap_or(""));
        if en.len() != pl.len() {
            return Err(format!("{} has {} lines, {} has {}", en_mod, en.len(), pl_mod, pl.len()));
        }
        for (i, (e, p)) in en.iter().zip(pl.iter()).enumerate() {
            if e.0 != p.0 || e.1 != p.1 {
                return Err(format!("{} line {}: pool/scope mismatch", pl_mod, i + 1));
            }
        }
    }
    Ok(())
}

// py: clan_saga_pl.py:385 render
/// Fill a Polish template (people = {"A": {"n","g"}}; vp = expand()'s dict). None = Python exception.
pub fn render(tpl: &str, people: &Value, vp: &Vars) -> Option<String> {
    let mut pp = serde_json::Map::new();
    for (k, p) in people.as_object()? {
        let g = match p.get("g").and_then(|g| g.as_str()) {
            Some(g @ ("m" | "f")) => g,
            _ => "n",
        };
        pp.insert(k.clone(), serde_json::json!({"n": pystr(p.get("n")?), "g": g}));
    }
    pln::render(tpl, &Value::Object(pp), vp)
}
