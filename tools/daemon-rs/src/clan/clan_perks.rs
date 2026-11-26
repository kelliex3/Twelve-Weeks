//! clan_perks.py: clan supply perks. A category's SURPLUS in the clan stock grants a small passive perk for the clan's player in every match,
//! credited to the member who brought most of that category. Nothing here changes a clan number or a person's state (except the stock the uses draw).
use crate::clan::clan::{self, int_of, pystr, truthy, Counts};
use crate::clan::clan_supply as sup;
use crate::clan::{clan_chronicle as ch, clan_kids as kd, clan_story as cs, data, ext_clan_perks as extp};
use regex::Regex;
use serde_json::{json, Map, Value};
use std::sync::LazyLock;

// cat -> wire id (the ext's CP_* enum), perk key, per-match cap, roles that bring it
const PERKS: [(&str, i64, &str, i64, &[&str]); 5] = [
    ("med", 1, "medkit", 2, &["medic", "elder"]),
    ("ammo", 2, "clip", 3, &["scout", "guard"]),
    ("mat", 3, "parts", 2, &["mechanic", "scavenger"]),
    ("food", 4, "stamina", 1, &["cook", "gardener", "scavenger"]),
    ("water", 5, "bleed", 2, &["scout", "gardener", "scavenger"]),
];
pub const RESERVE_DAYS: i64 = 1;
pub const USE_LABELS: [i64; 2] = [31, 32];
pub const MAX_USES: i64 = 15;

fn roles_of(cat: &str) -> &'static [&'static str] {
    PERKS.iter().find(|p| p.0 == cat).map(|p| p.4).unwrap_or(&[])
}
fn cap_of(cat: &str) -> i64 {
    PERKS.iter().find(|p| p.0 == cat).map(|p| p.3).unwrap_or(0)
}
fn cat_ix(cat: &str) -> usize {
    sup::CATS.iter().position(|c| *c == cat).unwrap_or(0)
}

/// The member a perk row names: {n, r, g (0 / 1), look, i[, pid]}.
#[derive(Clone, Debug, PartialEq)]
pub struct Who {
    pub n: String,
    pub r: String,
    pub g: i64,
    pub look: i64,
    pub i: i64,
    pub pid: Option<String>,
}

impl Who {
    /// The Python dict (key order n, r, g, look, i[, pid]).
    pub fn to_json(&self) -> Value {
        let mut m = Map::new();
        m.insert("n".into(), json!(self.n));
        m.insert("r".into(), json!(self.r));
        m.insert("g".into(), json!(self.g));
        m.insert("look".into(), json!(self.look));
        m.insert("i".into(), json!(self.i));
        if let Some(p) = &self.pid {
            m.insert("pid".into(), json!(p));
        }
        Value::Object(m)
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct Perk {
    pub cat: &'static str,
    pub wire: i64,
    pub key: &'static str,
    pub cap: i64,
    pub who: Option<Who>,
}

impl Perk {
    /// The Python dict {cat, wire, key, cap, who}.
    pub fn to_json(&self) -> Value {
        json!({"cat": self.cat, "wire": self.wire, "key": self.key, "cap": self.cap, "who": self.who.as_ref().map(|w| w.to_json())})
    }
}

// py: clan_perks.py:44 enabled
pub fn enabled() -> bool {
    let v = std::env::var("T2R_CLAN_PERKS").unwrap_or_else(|_| "1".to_string());
    !matches!(v.to_lowercase().as_str(), "0" | "off" | "false" | "no")
}

// py: clan_perks.py:48 _h
fn h(parts: &[String]) -> u32 {
    ch::h(parts)
}

fn gi(v: &Value, k: &str) -> i64 {
    v.get(k).map(|x| if truthy(x) { int_of(x) } else { 0 }).unwrap_or(0)
}

// py: clan_perks.py:52 need_of
/// Per-day need of every category, indexed like clan_supply::CATS (food, water, med, ammo, mat).
pub fn need_of(clan_row: &Value) -> [i64; 5] {
    let n = gi(clan_row, "healthy") + gi(clan_row, "hungry") + gi(clan_row, "sick");
    let s = sup::need_split(clan::needed(n), Some(n));
    let u = sup::use_of(n);
    [s[0], s[1], s[2], u[0], u[1]]
}

// py: clan_perks.py:59 stock_of
pub fn stock_of(clan_row: &Value) -> [i64; 5] {
    let meta = clan::meta_of(clan_row);
    let st = meta.get("sup").filter(|x| truthy(x)).and_then(|s| s.get("stock")).filter(|x| truthy(x));
    let mut out = [0i64; 5];
    for (i, k) in sup::CATS.iter().enumerate() {
        out[i] = st.and_then(|s| s.get(*k)).map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0).max(0);
    }
    out
}

// py: clan_perks.py:64 surplus
/// Units above RESERVE_DAYS of that category's need (0 or more), indexed like CATS.
pub fn surplus(clan_row: &Value) -> [i64; 5] {
    let nd = need_of(clan_row);
    let st = stock_of(clan_row);
    let mut out = [0i64; 5];
    for i in 0..5 {
        out[i] = (st[i] - RESERVE_DAYS * nd[i]).max(0);
    }
    out
}

fn people_of(story: &Value) -> Vec<Value> {
    story.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default()
}

fn p_s(p: &Value) -> String {
    p.get("s").map(pystr).unwrap_or_else(|| "h".to_string())
}

fn p_r(p: &Value) -> String {
    p.get("r").map(pystr).unwrap_or_default()
}

// py: clan_perks.py:70 credit_of
/// The member credited with `cat` (a story person dict) or None: the most credited living one, else the role's first living member.
pub fn credit_of(story: Option<&Value>, cat: &str) -> Option<Value> {
    let story = story.filter(|s| truthy(s))?;
    let people = people_of(story);
    let alive: Vec<&Value> = people.iter().filter(|p| kd::can_fight(p)).collect();
    let cr = story.get("ch").filter(|x| truthy(x)).and_then(|c| c.get("pk")).filter(|x| truthy(x)).and_then(|c| c.get("cr")).filter(|x| truthy(x)).and_then(|c| c.get(cat)).filter(|x| truthy(x));
    if let Some(Value::Object(cr)) = cr {
        let mut best: Option<(i64, i64)> = None; // (n, -i): the largest wins
        for (i, n) in cr {
            let ii: i64 = i.trim().parse().unwrap_or(0);
            if alive.iter().any(|p| gi(p, "i") == ii) && int_of(n) > 0 {
                let key = (int_of(n), -ii);
                if best.map(|b| key > b).unwrap_or(true) {
                    best = Some(key);
                }
            }
        }
        if let Some((_, ni)) = best {
            return alive.iter().rev().find(|p| gi(p, "i") == -ni).map(|p| (*p).clone());
        }
    }
    for role in roles_of(cat) {
        for p in &people {
            if p_r(p) == *role && p_s(p) != "d" && kd::can_fight(p) {
                return Some(p.clone());
            }
        }
    }
    let grown: Vec<&Value> = people.iter().filter(|p| p_s(p) != "d" && kd::can_fight(p)).collect();
    if let Some(p) = grown.iter().find(|p| p_r(p) == "scavenger") {
        return Some((*p).clone());
    }
    if let Some(p) = grown.iter().find(|p| !owns_other(p, cat)) {
        return Some((*p).clone());
    }
    grown.first().map(|p| (*p).clone())
}

// py: clan_perks.py:90 _owns_other
/// True when the member's role is the trade of another perk category only (a medic bringing ammo reads wrong).
fn owns_other(p: &Value, cat: &str) -> bool {
    let r = p_r(p);
    PERKS.iter().any(|(c, _, _, _, roles)| *c != cat && roles.contains(&r.as_str())) && !roles_of(cat).contains(&r.as_str())
}

// py: clan_perks.py:95 active
/// The active perks of a living clan in PERKS order; empty when off / no clan / no surplus.
pub fn active(clan_row: &Value, story: Option<&Value>) -> Vec<Perk> {
    if !enabled() || !truthy(clan_row) || clan_row.get("status").map(int_of) != Some(clan::ALIVE) {
        return vec![];
    }
    let sp = surplus(clan_row);
    let mut out = Vec::new();
    for (cat, wire, key, cap, _roles) in PERKS.iter() {
        let s = sp[cat_ix(cat)];
        if s < 1 {
            continue;
        }
        let who = credit_of(story, cat);
        let mut w = None;
        if let (Some(who), Some(story)) = (&who, story) {
            let name = who.get("n").map(pystr).unwrap_or_default();
            let sex = match who.get("g").and_then(|g| g.as_str()) {
                Some("f") => 1,
                Some("m") => 0,
                _ => (h(&["sex".to_string(), name.clone()]) & 1) as i64,
            };
            // the card's portrait: the member's clan-journal head, happy (fitted to the expressions it has)
            let fac = match story.get("fac") {
                Some(v) if !v.is_null() => int_of(v),
                _ => clan_row.get("fac").map(int_of).unwrap_or(0),
            };
            let fk = if fac == clan::SERAPHITES { "S" } else { "W" };
            let mut members = people_of(story);
            members.extend(story.get("dead").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default());
            let pid = ch::portrait_id(who, fk, Some("h"), 100, ch::PORTRAIT_MODELS, Some(&members));
            let parts: Vec<&str> = pid.split('/').collect();
            let pid = parts.iter().skip(2).take(2).copied().collect::<Vec<_>>().join("_");
            w = Some(Who { n: name.clone(), r: p_r(who), g: sex, look: (h(&["model".to_string(), name]) % 4) as i64, i: gi(who, "i"), pid: Some(pid) });
        }
        out.push(Perk { cat, wire: *wire, key, cap: (*cap).min(s), who: w });
    }
    out
}

// py: clan_perks.py:119 loss_streak
/// Consecutive lost matches at the end of the clan's recent results (a plain/unknown day is skipped). 0 when unknown.
pub fn loss_streak(story: Option<&Value>) -> i64 {
    let cs = story.and_then(|s| s.get("ch")).filter(|x| truthy(x));
    let hist = cs.and_then(|c| c.get("sg_hist")).filter(|x| truthy(x)).or_else(|| cs.and_then(|c| c.get("log")).filter(|x| truthy(x)));
    let mut n = 0;
    if let Some(Value::Array(a)) = hist {
        for hh in a.iter().rev() {
            match hh.get("win") {
                Some(Value::Bool(false)) => n += 1,
                Some(Value::Bool(true)) => break,
                _ => {}
            }
        }
    }
    n.min(9)
}

/// "".join(ch if (ch.isalnum() and ord(ch) < 128) or ch in "-'" else "_" ...)[:20]
fn wire_name(n: &str) -> String {
    n.chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' || c == '\'' { c } else { '_' }).take(20).collect()
}

// py: clan_perks.py:129 wire_text
/// The tail of the clan state reply: integers first (an old ext stops at the first non-integer value), the names last.
pub fn wire_text(perks: &[Perk], streak: i64) -> String {
    if perks.is_empty() {
        return String::new();
    }
    let roles: Vec<&str> = match data::t("clan_story_data", "ROLES") {
        Value::Object(m) => m.keys().map(|k| k.as_str()).collect(),
        _ => vec![],
    };
    let mut f: Vec<String> = vec![format!("pk={}", perks.len())];
    if streak > 0 {
        f.push(format!("pks={}", streak.min(9)));
    }
    for (i, p) in perks.iter().enumerate() {
        let (g, look, r) = match &p.who {
            Some(w) => (w.g, w.look, roles.iter().position(|x| *x == w.r).unwrap_or(0) as i64),
            None => (0, 0, 0),
        };
        f.push(format!("pkc{}={}", i, p.wire));
        f.push(format!("pkn{}={}", i, p.cap));
        f.push(format!("pkg{}={}", i, g));
        f.push(format!("pkl{}={}", i, look));
        f.push(format!("pkr{}={}", i, r));
    }
    for (i, p) in perks.iter().enumerate() {
        if let Some(w) = &p.who {
            f.push(format!("pkm{}={}", i, wire_name(&w.n)));
        }
    }
    for (i, p) in perks.iter().enumerate() {
        if let Some(pid) = p.who.as_ref().and_then(|w| w.pid.as_ref()).filter(|x| !x.is_empty()) {
            let s: String = pid.chars().filter(|c| c.is_alphanumeric() || *c == '_' || *c == '-').take(20).collect();
            f.push(format!("pkp{}={}", i, s));
        }
    }
    f.join(" ")
}

// py: clan_perks.py:151 uses_of
/// {cat: n} from the op 0xE2 count labels 31 / 32 (nibbles), clamped; in the order med, ammo, mat, food, water.
pub fn uses_of(counts: Option<&Counts>) -> Vec<(&'static str, i64)> {
    let get = |l: i64| -> i64 { counts.and_then(|c| c.get(&l)).copied().unwrap_or(0) };
    let (a, b) = (get(31), get(32));
    let u = [("med", a & 15), ("ammo", (a >> 4) & 15), ("mat", (a >> 8) & 15), ("food", b & 15), ("water", (b >> 4) & 15)];
    u.iter().filter(|(_, v)| *v > 0).map(|(k, v)| (*k, (*v).min(cap_of(k)))).collect()
}

// py: clan_perks.py:158 _pick_bringer
fn pick_bringer(story: &Value, cat: &str, day: i64) -> Option<Value> {
    let people: Vec<Value> = people_of(story).into_iter().filter(|p| p_s(p) != "d" && kd::can_fight(p)).collect();
    let mut pool: Vec<&Value> = roles_of(cat).iter().flat_map(|r| people.iter().filter(move |p| p_r(p) == *r)).collect();
    if pool.is_empty() {
        pool = people.iter().filter(|p| p_r(p) == "scavenger").collect();
    }
    if pool.is_empty() {
        pool = people.iter().filter(|p| !owns_other(p, cat)).collect();
    }
    if pool.is_empty() {
        pool = people.iter().collect();
    }
    if pool.is_empty() {
        return None;
    }
    let n = story.get("n").map(pystr).unwrap_or_else(|| "0".to_string());
    let ix = h(&["bring".to_string(), n, day.to_string(), cat.to_string()]) as usize % pool.len();
    Some(pool[ix].clone())
}

// py: clan_perks.py:166 credit_day
/// Book what the day brought in each category to a member (by role); mutates story["ch"]["pk"].
pub fn credit_day(story: &mut Value, got: Option<&Value>, day: i64) {
    if !truthy(story) || !story.get("people").map(truthy).unwrap_or(false) {
        return;
    }
    {
        let m = story.as_object_mut().expect("story object");
        let e = m.entry("ch").or_insert_with(|| json!({}));
        if !e.is_object() {
            return;
        }
        let e = e.as_object_mut().expect("ch").entry("pk").or_insert_with(|| json!({}));
        if !e.is_object() {
            return;
        }
        e.as_object_mut().expect("pk").entry("cr").or_insert_with(|| json!({}));
    }
    for cat in sup::CATS {
        let n = got.and_then(|g| g.get(cat)).map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0);
        let p = if n > 0 { pick_bringer(story, cat, day) } else { None };
        if let Some(p) = p {
            let cr = &mut story["ch"]["pk"]["cr"];
            if !cr.get(cat).map(|x| x.is_object()).unwrap_or(false) {
                cr[cat] = json!({});
            }
            let k = p.get("i").map(pystr).unwrap_or_default();
            let cur = cr[cat].get(&k).map(int_of).unwrap_or(0);
            cr[cat][&k] = json!(cur + n);
        }
    }
}

// py: clan_perks.py:196 _fem
/// Polish: the past-tense verb right after the member's name ('{n} dal') takes the feminine ending.
fn fem(t: &str) -> Result<String, String> {
    let (a, b) = t.split_once("{n} ").ok_or("ValueError: not enough values to unpack")?;
    let (w, rest) = b.split_once(' ').ok_or("ValueError: not enough values to unpack")?;
    let w2 = if let Some(stem) = w.strip_suffix('\u{142}') { format!("{}\u{142}a", stem) } else { w.to_string() };
    Ok(format!("{}{{n}} {} {}", a, w2, rest))
}

static RE_A: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"(: | at | before )\{map\}").expect("RE_A"));
static RE_B: LazyLock<Regex> = LazyLock::new(|| Regex::new(r" \{map\}").expect("RE_B"));

// py: clan_perks.py:203 line
/// The saga line of one used perk. No map name: the place phrase is left out. `who` = {"n", "g"} (g "f" / 1 = female). Err = where Python raises.
pub fn line(cat: &str, who: &Value, mapname: &str, lang: &str, day: i64) -> Result<String, String> {
    let rows = data::t("clan_perks", "LINES").get(cat).ok_or_else(|| format!("KeyError: {}", cat))?;
    let en = &rows[0];
    let pl = &rows[1];
    let name = who.get("n").map(pystr).ok_or("KeyError: n")?;
    let pool = if lang == "pl" { pl } else { en };
    let ix = (h(&["pkline".to_string(), name.clone(), day.to_string(), cat.to_string()]) % 2) as usize;
    let mut t = pool.get(ix).map(pystr).ok_or("IndexError")?;
    let female = match who.get("g") {
        Some(Value::String(s)) => s == "f",
        Some(v) => matches!(v, Value::Number(_) | Value::Bool(_)) && int_of(v) == 1,
        None => false,
    };
    if lang == "pl" && female {
        t = fem(&t)?;
    }
    if !mapname.is_empty() {
        t = t.replace("{map}", mapname);
    } else {
        t = RE_A.replace_all(&t, "").into_owned();
        t = RE_B.replace_all(&t, "").into_owned();
    }
    Ok(t.replace("{n}", &name))
}

// py: clan_perks.py:221 render_msg
/// A stored perk row (lid "pk/use"): params {"t": EN text, "tp": PL text}.
pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
    if lid.is_empty() || !lid.starts_with("pk/") || !params.is_object() {
        return None;
    }
    let k = if lang.to_lowercase().starts_with("pl") { "tp" } else { "t" };
    params.get(k).and_then(|v| v.as_str()).map(|s| s.to_string())
}

// py: clan_perks.py:228 on_day
/// After clan.apply_match + clan_story.on_day: credit the day's goods to members, draw the perk uses from the stock, write the journal line.
/// Returns the list of (cat, n) drawn. `Err` = where Python raises (the daemon swallows it).
pub fn on_day(c: &rusqlite::Connection, tid: &str, newc: &Value, r: &Value, counts: Option<&Counts>, matchv: Option<&Value>) -> Result<Vec<(&'static str, i64)>, String> {
    if !enabled() || !truthy(r) || !truthy(newc) {
        return Ok(vec![]);
    }
    let sup_r = r.get("sup").filter(|x| truthy(x)).cloned().unwrap_or_else(|| json!({}));
    let mut story = cs::_load(c, tid);
    let day = match r.get("day") {
        Some(v) => int_of(v),
        None => gi(newc, "day"),
    };
    if let Some(st) = story.as_mut() {
        credit_day(st, sup_r.get("got"), day);
    }
    let used = uses_of(counts);
    let mut drawn: Vec<(&'static str, i64)> = Vec::new();
    if !used.is_empty() {
        let mut cl = clan::load(c, tid).map_err(|e| e.to_string())?;
        let mut meta = clan::meta_of(&cl);
        let sup_m = meta.get("sup").filter(|x| truthy(x)).cloned().unwrap_or_else(|| json!({}));
        let mut stock: Map<String, Value> = sup_m.get("stock").filter(|x| truthy(x)).and_then(|x| x.as_object()).cloned().unwrap_or_default();
        for (cat, n) in &used {
            let have = stock.get(*cat).map(|v| if truthy(v) { int_of(v) } else { 0 }).unwrap_or(0).max(0);
            let d = (*n).min(have);
            stock.insert((*cat).to_string(), json!(have - d));
            drawn.push((cat, *n));
        }
        let mut sm: Map<String, Value> = sup_m.as_object().cloned().unwrap_or_default();
        sm.insert("stock".into(), Value::Object(stock));
        meta["sup"] = Value::Object(sm);
        cl["meta"] = json!(crate::py::dumps_compact(&meta));
        clan::save(c, tid, &cl).map_err(|e| e.to_string())?;
        if let Some(st) = story.as_mut() {
            let mp = matchv.and_then(|m| m.get("map")).filter(|x| truthy(x)).map(pystr).unwrap_or_default();
            // a used perk is ONE sentence a day in that day's journal entry (clan_journal.add_perk), from pools whose lines are used once per clan run
            for (cat, _n) in &drawn {
                let who = match credit_of(Some(st), cat) {
                    Some(w) => w,
                    None => continue,
                };
                match extp::journal::add_perk(c, tid, st, day + 1, cat, &who, &mp) {
                    Ok(true) => break,
                    Ok(false) => {}
                    Err(_) => break, // the journal never breaks the perks
                }
            }
        }
    }
    if let Some(st) = &story {
        cs::_save(c, tid, st).map_err(|e| e.to_string())?;
    }
    Ok(drawn)
}
