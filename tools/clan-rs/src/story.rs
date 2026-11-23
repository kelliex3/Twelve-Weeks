use crate::alife;
use crate::data::{crowd_re, data, group_re, pl_group_re, Data, TplInfo};
use crate::pyrand::PyRandom;
use crate::rules::{week_day, SERAPHITES, WLF};
use crate::text::{
    expand_en, expand_pl, line_id, lex_expand_pl, py_int, py_str, render_en, render_lex, render_pl, split_sentences, widget_text, People, Person,
};
use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{Map, Value};
use std::collections::HashSet;

pub const SLOT_S: f64 = 60.0;
pub const LIVE_N: usize = 6;
pub const FRESH_S: f64 = 900.0;
const STORED_WINDOW: usize = 24;

#[derive(Clone, Debug)]
pub struct Member {
    pub i: i64,
    pub n: String,
    pub g: String,
    pub r: String,
    pub s: String,
    pub il: String,
    pub is_: i64,
    pub gl: bool,
    pub jd: i64,
    pub k: i64,
}

impl Member {
    pub fn person(&self) -> Person {
        Person { n: self.n.clone(), g: self.g.clone(), r: self.r.clone() }
    }

    pub fn is_kid(&self) -> bool {
        self.r == "kid"
    }
}

#[derive(Clone, Debug)]
pub struct Dead {
    pub n: String,
    pub g: String,
    pub r: String,
    pub c: Option<String>,
}

#[derive(Clone, Debug)]
pub struct State {
    pub fac: i64,
    pub run: i64,
    pub day: i64,
    pub morale: f64,
    pub ds: f64,
    pub ss: f64,
    pub people: Vec<Member>,
    pub dead: Vec<Dead>,
    pub seed: String,
    pub n: i64,
    pub sn: Option<i64>,
    pub pid: i64,
}

pub fn norm_lang(x: &str) -> &'static str {
    let s = x.trim().to_lowercase().replace('_', "-");
    if matches!(s.as_str(), "1" | "pl" | "pol" | "polish") || s.starts_with("pl-") {
        "pl"
    } else {
        "en"
    }
}

pub fn slot_of(now: f64) -> i64 {
    (now / SLOT_S).floor() as i64
}

fn fkey(fac: i64) -> &'static str {
    if fac == SERAPHITES {
        "S"
    } else {
        "W"
    }
}

fn s_of(v: Option<&Value>) -> Option<String> {
    v.and_then(Value::as_str).map(str::to_string)
}

fn i_of(v: Option<&Value>) -> Option<i64> {
    v.and_then(py_int)
}

fn member(v: &Value) -> Option<Member> {
    Some(Member {
        i: i_of(v.get("i"))?,
        n: s_of(v.get("n"))?,
        g: s_of(v.get("g"))?,
        r: s_of(v.get("r")).unwrap_or_default(),
        s: s_of(v.get("s"))?,
        il: s_of(v.get("il")).unwrap_or_default(),
        is_: i_of(v.get("is")).unwrap_or(0),
        gl: crate::text::truthy(v.get("gl")),
        jd: i_of(v.get("jd")).unwrap_or(0),
        k: i_of(v.get("k")).unwrap_or(0),
    })
}

fn dead(v: &Value) -> Option<Dead> {
    Some(Dead { n: s_of(v.get("n"))?, g: s_of(v.get("g")).unwrap_or_else(|| "m".into()), r: s_of(v.get("r")).unwrap_or_default(), c: v.get("c").map(py_str) })
}

fn num(v: Option<&Value>) -> Option<f64> {
    v.and_then(Value::as_f64)
}

pub fn parse_state(v: &Value) -> Option<State> {
    if v.get("v").and_then(Value::as_i64) != Some(1) {
        return None;
    }
    let people = v.get("people")?.as_array()?.iter().map(member).collect::<Option<Vec<_>>>()?;
    let dead = match v.get("dead") {
        Some(Value::Array(a)) => a.iter().map(dead).collect::<Option<Vec<_>>>()?,
        _ => Vec::new(),
    };
    Some(State {
        fac: i_of(v.get("fac"))?,
        run: i_of(v.get("run"))?,
        day: i_of(v.get("day"))?,
        morale: num(v.get("morale"))?,
        ds: num(v.get("ds")).unwrap_or(0.0),
        ss: num(v.get("ss")).unwrap_or(0.0),
        people,
        dead,
        seed: s_of(v.get("seed")).unwrap_or_default(),
        n: i_of(v.get("n")).unwrap_or(0),
        sn: i_of(v.get("sn")),
        pid: i_of(v.get("pid")).unwrap_or(0),
    })
}

pub struct RawState {
    pub value: Value,
    pub parsed: Option<State>,
}

#[derive(serde::Deserialize)]
struct Partial {
    v: Option<Value>,
    fac: Option<Value>,
    run: Option<Value>,
    day: Option<Value>,
    morale: Option<Value>,
    ds: Option<Value>,
    ss: Option<Value>,
    seed: Option<Value>,
    n: Option<Value>,
    sn: Option<Value>,
    pid: Option<Value>,
    people: Option<Value>,
    dead: Option<Value>,
}

pub fn load_state(c: &Connection, tid: &str) -> Option<RawState> {
    let row: Option<Option<String>> = c.query_row("SELECT state FROM clan_story WHERE token_id=?", params![tid], |r| r.get(0)).optional().ok()?;
    let p: Partial = serde_json::from_str(&row??).ok()?;
    if p.v != Some(Value::from(1)) {
        return None;
    }
    let mut m = Map::new();
    for (k, x) in [
        ("v", p.v),
        ("fac", p.fac),
        ("run", p.run),
        ("day", p.day),
        ("morale", p.morale),
        ("ds", p.ds),
        ("ss", p.ss),
        ("seed", p.seed),
        ("n", p.n),
        ("sn", p.sn),
        ("pid", p.pid),
        ("people", p.people),
        ("dead", p.dead),
    ] {
        if let Some(x) = x {
            m.insert(k.into(), x);
        }
    }
    let v = Value::Object(m);
    let parsed = parse_state(&v);
    Some(RawState { value: v, parsed })
}

struct ClanRow {
    faction: i64,
    day: i64,
    healthy: i64,
    hungry: i64,
    sick: i64,
}

fn sql_val(v: rusqlite::types::Value) -> Value {
    match v {
        rusqlite::types::Value::Null => Value::Null,
        rusqlite::types::Value::Integer(i) => Value::from(i),
        rusqlite::types::Value::Real(f) => serde_json::Number::from_f64(f).map(Value::Number).unwrap_or(Value::Null),
        rusqlite::types::Value::Text(s) => Value::String(s),
        rusqlite::types::Value::Blob(b) => Value::String(String::from_utf8_lossy(&b).into_owned()),
    }
}

fn clan_row(c: &Connection, tid: &str) -> Option<Option<ClanRow>> {
    let row = c
        .query_row("SELECT faction, day, healthy, hungry, sick FROM clans WHERE token_id=?", params![tid], |r| {
            Ok((r.get::<_, rusqlite::types::Value>(0)?, r.get::<_, rusqlite::types::Value>(1)?, r.get::<_, rusqlite::types::Value>(2)?, r.get::<_, rusqlite::types::Value>(3)?, r.get::<_, rusqlite::types::Value>(4)?))
        })
        .optional()
        .ok()?;
    let (f, d, h, u, s) = match row {
        None => return Some(None),
        Some(x) => x,
    };
    let f = sql_val(f);
    let faction = if f.is_null() { 0 } else { py_int(&f)? };
    Some(Some(ClanRow { faction, day: py_int(&sql_val(d)).unwrap_or(0), healthy: py_int(&sql_val(h))?, hungry: py_int(&sql_val(u))?, sick: py_int(&sql_val(s))? }))
}

fn rng_for(st: &State, salt: &str) -> PyRandom {
    PyRandom::new(&format!("{}:{}:{}:{}", st.seed, st.run, st.sn.unwrap_or(st.n), salt))
}

fn new_person(d: &Data, rng: &mut PyRandom, st: &mut State, state: &str, join_day: i64) -> Option<Member> {
    let fk = fkey(st.fac);
    let mut pool: Vec<(String, String, f64)> = Vec::new();
    for n in &d.story.first {
        let g = if d.story.female.contains(n) { "f" } else { "m" };
        pool.push((n.clone(), g.into(), if fk == "W" { 1.0 } else { 0.3 }));
    }
    let (own, other) = if fk == "W" { (&d.story.extra_w, &d.story.extra_s) } else { (&d.story.extra_s, &d.story.extra_w) };
    for (n, g) in own {
        pool.push((n.clone(), g.clone(), if fk == "W" { 1.0 } else { 1.6 }));
    }
    for (n, g) in other {
        pool.push((n.clone(), g.clone(), 0.1));
    }
    let weights: Vec<f64> = pool.iter().map(|x| x.2).collect();
    let taken: HashSet<&str> = st.people.iter().map(|p| p.n.as_str()).chain(st.dead.iter().map(|x| x.n.as_str())).collect();
    let mut pick: Option<(String, String)> = None;
    let mut free = false;
    for _ in 0..40 {
        let i = rng.choices_weighted(&weights)?;
        let (nm, g, _) = &pool[i];
        pick = Some((nm.clone(), g.clone()));
        if !taken.contains(nm.as_str()) {
            free = true;
            break;
        }
    }
    let (mut nm, mut g) = pick?;
    if !free {
        nm = format!("{} {}", nm, rng.choice(&d.story.last)?);
    }
    if d.story.neutral.contains(nm.split_whitespace().next().unwrap_or("")) && rng.random() < 0.3 {
        g = "n".into();
    }
    let rw: Vec<f64> = d.story.roles.iter().map(|r| r.3).collect();
    let role = d.story.roles.get(rng.choices_weighted(&rw)?)?.0.clone();
    st.pid += 1;
    let _last = rng.choice(&d.story.last)?;
    Some(Member { i: st.pid, n: nm, g, r: role, s: state.into(), il: String::new(), is_: 0, gl: false, jd: join_day, k: 0 })
}

fn pick_illness(d: &Data, rng: &mut PyRandom, st: &State, p: &Member) -> Option<String> {
    let after_fight = st.day - (-99) <= 3;
    let keys: Vec<&(String, String, String, String, f64, f64)> =
        d.story.illness.iter().filter(|k| !(p.is_kid() && (k.0 == "infected_wound" || k.0 == "spores"))).collect();
    let w: Vec<f64> = keys.iter().map(|k| if after_fight { k.5 } else { k.4 }).collect();
    Some(keys.get(rng.choices_weighted(&w)?)?.0.clone())
}

fn populate(d: &Data, rng: &mut PyRandom, st: &mut State, h: i64, u: i64, s: i64, day: i64) -> Option<()> {
    for (state, n) in [("h", h), ("u", u), ("s", s)] {
        for _ in 0..n.max(0) {
            let mut p = new_person(d, rng, st, state, day)?;
            if state == "s" {
                p.il = pick_illness(d, rng, st, &p)?;
            }
            if rng.random() < 0.35 {
                let free: Vec<usize> = (0..st.people.len()).filter(|&q| st.people[q].k == 0 && st.people[q].i != p.i).collect();
                if let Some(&q) = rng.choice(&free) {
                    p.k = st.people[q].i;
                    st.people[q].k = p.i;
                }
            }
            st.people.push(p);
        }
    }
    Some(())
}

fn ensure_roster(c: &Connection, tid: &str, old: Option<&RawState>) -> Option<State> {
    let d = data()?;
    let cl = clan_row(c, tid)??;
    if cl.faction != WLF && cl.faction != SERAPHITES {
        return None;
    }
    let old_st = match old {
        Some(r) => {
            let st = r.parsed.clone();
            if r.value.get("fac").and_then(py_int_v) == Some(cl.faction) && r.value.get("people").map_or(false, |p| crate::text::truthy(Some(p))) {
                return st;
            }
            Some(r.value.clone())
        }
        None => None,
    };
    let (h, u, s) = (cl.healthy, cl.hungry, cl.sick);
    if h + u + s <= 0 {
        return None;
    }
    let n = match &old_st {
        Some(v) => i_of(v.get("n"))?,
        None => 0,
    };
    let run = match &old_st {
        Some(v) => i_of(v.get("run"))? + 1,
        None => 1,
    };
    let mut st = State {
        fac: cl.faction,
        run,
        day: cl.day,
        morale: 55.0,
        ds: 0.0,
        ss: 0.0,
        people: Vec::new(),
        dead: Vec::new(),
        seed: format!("{}:{}", tid, run),
        n,
        sn: None,
        pid: 0,
    };
    let mut rng = rng_for(&st, "lazy");
    let day = st.day;
    populate(d, &mut rng, &mut st, h, u, s, day)?;
    Some(st)
}

fn py_int_v(v: &Value) -> Option<i64> {
    py_int(v)
}

fn fp(st: &State) -> String {
    let mut raw = st
        .people
        .iter()
        .map(|p| format!("{}{}{}{}{}", p.i, p.s, p.il, p.is_, if p.gl { 1 } else { 0 }))
        .collect::<Vec<_>>()
        .join(",");
    let tail = &st.dead[st.dead.len().saturating_sub(8)..];
    raw += "/";
    raw += &tail.iter().map(|x| format!("{}{}", x.n, x.c.clone().unwrap_or_default())).collect::<Vec<_>>().join(",");
    raw += &format!("/{}", st.morale.trunc() as i64);
    format!("{:08x}", crc32fast::hash(raw.as_bytes()))
}

fn tpl<'a>(d: &'a Data, t: &str) -> Option<&'a TplInfo> {
    d.story.tpls.get(t)
}

fn healthy_others<'a>(st: &'a State, p: &Member) -> Vec<&'a Member> {
    st.people.iter().filter(|q| q.i != p.i && q.s == "h").collect()
}

fn companion_ok(d: &Data, st: &State, p: &Member, t: &str) -> Option<bool> {
    let info = tpl(d, t)?;
    if info.l.contains('D') && st.dead.last().map_or(false, |x| x.r == "kid") && info.c {
        return Some(false);
    }
    let need = info.l.chars().filter(|c| *c == 'B' || *c == 'C').count();
    if need == 0 {
        return Some(true);
    }
    let mut others = healthy_others(st, p);
    if info.c {
        others.retain(|q| !q.is_kid());
    }
    Some(others.len() >= need)
}

fn rich_for(d: &Data, st: &State, p: &Member, fk: &str) -> Option<Vec<(String, String)>> {
    let others = healthy_others(st, p).len();
    let two = st.people.len() == 2;
    let mut out = Vec::new();
    for (scope, en) in &d.story.rich {
        if scope != "*" && scope != fk {
            continue;
        }
        let info = tpl(d, en)?;
        if (info.l.contains('B') && others == 0) || (info.l.contains('C') && others < 2) || (info.l.contains('D') && st.dead.is_empty()) {
            continue;
        }
        if fk == "S" && info.s {
            continue;
        }
        if two && info.w {
            continue;
        }
        out.push((en.clone(), "t:g".to_string()));
    }
    Some(out)
}

fn filtered_pool(d: &Data, pool: &[String], fk: &str, two: bool) -> Option<Vec<String>> {
    let mut out = Vec::new();
    for t in pool {
        let info = tpl(d, t)?;
        if fk == "S" && info.s {
            continue;
        }
        if two && info.w {
            continue;
        }
        out.push(t.clone());
    }
    Some(out)
}

fn bare_healthy(d: &Data, fk: &str, two: bool) -> Option<Vec<String>> {
    let mut seen = HashSet::new();
    let uniq: Vec<String> = d.story.healthy.iter().filter(|t| t.as_str() != "{A} is healthy.").filter(|t| seen.insert(t.as_str())).cloned().collect();
    filtered_pool(d, &uniq, fk, two)
}

fn class_items(d: &Data, st: &State, p: &Member, fk: &str) -> Option<Vec<(String, String, String)>> {
    let two = st.people.len() == 2;
    let c: Vec<(String, String)> = if p.s == "h" {
        let r = rich_for(d, st, p, fk)?;
        if r.is_empty() {
            bare_healthy(d, fk, two)?.into_iter().map(|t| (t, "t:h".to_string())).collect()
        } else {
            r
        }
    } else {
        let (pool, pid) = if p.s == "s" { (&d.story.sick, "t:s") } else { (&d.story.hungry, "t:u") };
        filtered_pool(d, pool, fk, two)?.into_iter().map(|t| (t, pid.to_string())).collect()
    };
    Some(c.into_iter().map(|(t, pid)| (line_id(&t), t, pid)).collect())
}

fn perm(m: usize, seed: &str, e: usize) -> Vec<usize> {
    let mut r: Vec<usize> = (0..m).collect();
    PyRandom::new(&format!("{}:{}", seed, e)).shuffle(&mut r);
    r
}

fn cycle_pick<T: Clone>(items: &[T], key: impl Fn(&T) -> &str, seed: &str, k: usize) -> Option<T> {
    let m = items.len();
    if m == 0 {
        return None;
    }
    if m == 1 {
        return Some(items[0].clone());
    }
    let (e, j) = (k / m, k % m);
    let mut cur = perm(m, seed, e);
    let w = ((m - 1) / 3).min(24);
    if e == 0 || w == 0 {
        return Some(items[cur[j]].clone());
    }
    let prev = perm(m, seed, e - 1);
    let tail: HashSet<&str> = prev[m - w..].iter().map(|&x| key(&items[x])).collect();
    let mut t = w;
    for jj in 0..w {
        if tail.contains(key(&items[cur[jj]])) {
            while t < m - w && tail.contains(key(&items[cur[t]])) {
                t += 1;
            }
            if t >= m - w {
                break;
            }
            cur.swap(jj, t);
            t += 1;
        }
    }
    Some(items[cur[j]].clone())
}

#[derive(Clone)]
struct Rec {
    tpl: String,
    pool: String,
    people: People,
    sym: Map<String, Value>,
}

fn sym_of(d: &Data, rng: &mut PyRandom, st: &State, fk: &str) -> Option<Map<String, Value>> {
    let f = d.story.faction.get(fk)?;
    let mut m = Map::new();
    m.insert("f".into(), Value::from(fk));
    m.insert("day".into(), Value::from(st.day));
    m.insert("week".into(), Value::from(week_day(st.day).0));
    m.insert("place".into(), Value::from(rng.choice(&f.places)?.clone()));
    m.insert("inf".into(), Value::from(rng.choice(&d.story.infected)?.clone()));
    Some(m)
}

type SoloItem = (String, String, String, People);

fn solo_candidates(d: &Data, rng: &mut PyRandom, st: &State, p: &Member, fk: &str) -> Option<Vec<SoloItem>> {
    let mut out: Vec<SoloItem> = Vec::new();
    let mut add = |pairs: &[(String, String)], extra: Option<(char, Person)>, tag: &str| -> Option<()> {
        for (sc, t) in pairs {
            if sc != "*" && sc != fk {
                continue;
            }
            let info = tpl(d, t)?;
            if p.is_kid() && info.k.contains('A') && info.c {
                continue;
            }
            let mut ppl = People::default();
            if let Some((c, x)) = &extra {
                ppl.set(*c, x.clone());
            }
            ppl.set('A', p.person());
            let key = line_id(t) + if t.contains("{D}") { tag } else { "" };
            out.push((key, t.clone(), d.story.tpl_pool.get(t)?.clone(), ppl));
        }
        Some(())
    };
    let solo = |k: &str| d.story.solo.get(k);
    let ticker_key = match p.s.as_str() {
        "s" => "t_s",
        "u" => "t_u",
        _ => "t_h",
    };
    add(solo(ticker_key)?, None, "")?;
    add(solo("calm")?, None, "")?;
    if p.s == "u" {
        add(solo("hunger")?, None, "")?;
        add(solo("starving")?, None, "")?;
    } else if p.s == "s" {
        add(solo("sick")?, None, "")?;
        if p.is_ >= 1 {
            add(solo("worse")?, None, "")?;
            add(solo("medicine")?, None, "")?;
        }
    } else {
        add(solo("hope")?, None, "")?;
        if st.morale >= 60.0 {
            add(solo("plenty")?, None, "")?;
        }
        if st.morale <= 30.0 {
            add(solo("despair")?, None, "")?;
        }
    }
    let tail = &st.dead[st.dead.len().saturating_sub(8)..];
    if !tail.is_empty() {
        let dd = rng.choice(tail)?;
        let dp = Person { n: dd.n.clone(), g: dd.g.clone(), r: dd.r.clone() };
        let cause = dd.c.clone()?;
        let pool: Vec<(String, String)> = if cause == "mercy" || cause == "bite_kin" {
            solo("grief_mercy")?.clone()
        } else {
            let mut v = solo("grief")?.clone();
            let grp = d.story.cause_group.get(&cause).cloned().unwrap_or_default();
            if let Some(x) = solo(&format!("grief:{}", grp)) {
                v.extend(x.iter().cloned());
            }
            v
        };
        add(&pool, Some(('D', dp.clone())), &dd.n)?;
        if p.s == "h" {
            add(solo("hope_after")?, Some(('D', dp.clone())), &dd.n)?;
        }
        if p.gl {
            add(solo("guilt")?, Some(('D', dp)), &dd.n)?;
        }
    }
    Some(out)
}

fn kid_plain(d: &Data, s: &str) -> Option<(String, String, String)> {
    let (t, pid) = d.story.kid_plain.get(s)?;
    Some((line_id(t), t.clone(), pid.clone()))
}

fn gen_line(d: &Data, st: &State, tid: &str, k: usize) -> Option<Rec> {
    let fk = fkey(st.fac);
    let base = format!("{}:{}:{}:{}", tid, st.run, fp(st), st.day);
    let n = st.people.len();
    let (rnd, idx) = (k / n, k % n);
    let mut order: Vec<&Member> = st.people.iter().collect();
    PyRandom::new(&format!("order:{}:{}", base, rnd)).shuffle(&mut order);
    let p = order[idx];
    let mut rng = PyRandom::new(&format!("line:{}:{}", base, k));
    let mut sym = sym_of(d, &mut rng, st, fk)?;
    if n == 1 {
        let seed = format!("solo:{}", base);
        let cands = solo_candidates(d, &mut PyRandom::new(&seed), st, p, fk)?;
        let (_key, t, pool, people) = cycle_pick(&cands, |x| x.0.as_str(), &seed, k)?;
        sym.insert("ill".into(), Value::from(if p.il.is_empty() { "none".to_string() } else { p.il.clone() }));
        return Some(Rec { tpl: t, pool, people, sym });
    }
    let mut one = People::default();
    one.set('A', p.person());
    if p.s == "s" && rng.random() < d.story.rare_chance && !p.is_kid() {
        return Some(Rec { tpl: d.story.rare.clone(), pool: "t:r".into(), people: one, sym });
    }
    let mut items = class_items(d, st, p, fk)?;
    let mut cls = p.s.clone();
    if st.people.iter().any(Member::is_kid) || st.dead.last().map_or(false, |x| x.r == "kid") {
        let mut kept = Vec::new();
        for x in items {
            if companion_ok(d, st, p, &x.1)? {
                kept.push(x);
            }
        }
        items = if kept.is_empty() { vec![kid_plain(d, &p.s)?] } else { kept };
        cls.push('c');
    }
    if p.is_kid() {
        let mut kept = Vec::new();
        for x in items {
            if !tpl(d, &x.1)?.c {
                kept.push(x);
            }
        }
        items = if kept.is_empty() { vec![kid_plain(d, &p.s)?] } else { kept };
        cls.push('k');
    }
    let (_key, t, pool) = cycle_pick(&items, |x| x.0.as_str(), &format!("cls{}:{}", cls, base), k)?;
    let info = tpl(d, &t)?;
    let mut people = one;
    if info.l.contains('B') || info.l.contains('C') {
        let all = healthy_others(st, p);
        let mut others: Vec<&Member> = all.clone();
        if info.c {
            let f: Vec<&Member> = all.iter().copied().filter(|q| !q.is_kid()).collect();
            if !f.is_empty() {
                others = f;
            }
        }
        rng.shuffle(&mut others);
        people.set('B', others.first()?.person());
        if info.l.contains('C') {
            people.set('C', others.get(1)?.person());
        }
    }
    if info.l.contains('D') {
        if let Some(dd) = st.dead.last() {
            people.set('D', Person { n: dd.n.clone(), g: dd.g.clone(), r: dd.r.clone() });
        }
    }
    Some(Rec { tpl: t, pool, people, sym })
}

fn reg_people(d: &Data, people: &People) -> People {
    let mut out = People::default();
    for (c, p) in &people.0 {
        let g = if d.story.pron.contains_key(&p.g) { p.g.clone() } else { "n".into() };
        out.set(*c, Person { n: p.n.clone(), g, r: String::new() });
    }
    out
}

fn render_rec(d: &Data, rec: &Rec, pl: bool) -> Option<String> {
    let text = render_en(&rec.tpl, &rec.people, &expand_en(&rec.sym)?)?;
    if pl {
        let lid = format!("{}/{}", rec.pool, line_id(&rec.tpl));
        if let Some((_, _, pl_tpl)) = d.pl.reg.get(&lid) {
            if let Some(t) = expand_pl(&rec.sym).and_then(|v| render_pl(pl_tpl, &reg_people(d, &rec.people), &v)) {
                if !t.is_empty() {
                    return Some(t);
                }
            }
        }
    }
    Some(text)
}

fn live_lines(d: &Data, st: &State, tid: &str, n: usize, pl: bool, slot: i64) -> Option<Vec<(String, String)>> {
    if st.people.is_empty() {
        return Some(Vec::new());
    }
    let base = (slot.max(0) as usize).checked_mul(LIVE_N)?;
    let recs = (0..LIVE_N).map(|i| gen_line(d, st, tid, base + i)).collect::<Option<Vec<_>>>()?;
    let mut out = Vec::new();
    for (i, r) in recs.iter().take(n).enumerate() {
        out.push((format!("l{}{:02}", slot.rem_euclid(100000), i), widget_text(&render_rec(d, r, pl)?)));
    }
    Some(out)
}

#[derive(Clone, Debug)]
struct Row {
    id: i64,
    kind: String,
    text: String,
    names: Vec<Value>,
    ts: f64,
}

fn people_of(d: &Data, params: &Map<String, Value>, with_role: bool) -> Option<People> {
    let mut out = People::default();
    if let Some(p) = params.get("p") {
        for (k, v) in p.as_object()? {
            let c = k.chars().next()?;
            let a = v.as_array()?;
            let g = match a.get(1)? {
                Value::String(s) if d.story.pron.contains_key(s) => s.clone(),
                _ => "n".into(),
            };
            let r = if with_role { a.get(2).map(py_str).unwrap_or_else(|| "scavenger".into()) } else { String::new() };
            if k.chars().count() == 1 {
                out.set(c, Person { n: py_str(a.first()?), g, r });
            }
        }
    }
    Some(out)
}

fn sym_param(params: &Map<String, Value>) -> Option<Map<String, Value>> {
    match params.get("s") {
        None => Some(Map::new()),
        Some(Value::Object(m)) => Some(m.clone()),
        Some(_) => None,
    }
}

fn chron_vars(d: &Data, sym: &Map<String, Value>, pl: bool) -> Option<crate::text::Vars> {
    let ch = &d.chron;
    let fk = sym.get("f")?.as_str()?.to_string();
    let f = d.story.faction.get(&fk)?;
    let mut v = crate::text::Vars::new();
    let zero = Value::from(0);
    for k in ["sup", "need", "kills", "deaths", "downs"] {
        v.insert(k.into(), py_str(sym.get(k).unwrap_or(&zero)));
    }
    let need = sym.get("need").unwrap_or(&zero);
    let sup = sym.get("sup").unwrap_or(&zero);
    let diff = match (need.as_i64(), sup.as_i64()) {
        (Some(a), Some(b)) => Value::from((a - b).max(0)),
        _ => {
            let x = need.as_f64()? - sup.as_f64()?;
            if x > 0.0 {
                serde_json::Number::from_f64(x).map(Value::Number)?
            } else {
                Value::from(0)
            }
        }
    };
    v.insert("n".into(), py_str(&diff));
    v.insert(
        "leader".into(),
        match (pl, fk == "W") {
            (false, true) => "Command",
            (false, false) => "the Prophet",
            (true, true) => "Dowództwo",
            (true, false) => "Prorok",
        }
        .into(),
    );
    let qty = |key: &str, n: &Value| -> Option<String> {
        let e = ch.use_.get(key)?;
        let n = py_int(n)?.max(1);
        Some(if pl {
            if n == 1 {
                e.4.clone()
            } else {
                let few = (2..=4).contains(&n.rem_euclid(10)) && !(12..=14).contains(&n.rem_euclid(100));
                format!("{} {}", n, if few { &e.5 } else { &e.6 })
            }
        } else if n == 1 {
            e.1.clone()
        } else {
            format!("{} {}", n, e.2)
        })
    };
    let mut items = Vec::new();
    if crate::text::truthy(sym.get("loot")) {
        for x in sym.get("loot")?.as_array()? {
            let a = x.as_array()?;
            let k = a.first()?.as_str()?;
            items.push(match ch.keep.get(k) {
                Some(kp) => if pl { kp.1.clone() } else { kp.0.clone() },
                None => qty(k, a.get(1)?)?,
            });
        }
    }
    let joined = if items.len() <= 1 {
        items.concat()
    } else {
        format!("{}{}{}", items[..items.len() - 1].join(", "), if pl { " i " } else { " and " }, items[items.len() - 1])
    };
    v.insert("loot_list".into(), joined);
    if crate::text::truthy(sym.get("finds")) {
        return None;
    }
    if let Some(it) = sym.get("it").and_then(Value::as_str) {
        if ch.use_.contains_key(it) {
            v.insert("item".into(), qty(it, sym.get("itn").unwrap_or(&Value::from(1)))?);
            let mut thing = qty(it, &Value::from(1))?;
            if !pl {
                if let Some(rest) = thing.strip_prefix("a ") {
                    thing = format!("the {}", rest);
                }
            }
            v.insert("thing".into(), thing);
        }
    }
    let when = py_int(sym.get("when").unwrap_or(&zero))?.rem_euclid(3) as usize;
    let w = ch.whens.get(when)?;
    v.insert("when".into(), if pl { w.1.clone() } else { w.0.clone() });
    let m = sym.get("map").filter(|x| crate::text::truthy(Some(x))).map(py_str).unwrap_or_default();
    let m = ch.map_short.get(&m).cloned().unwrap_or(m);
    v.insert("map".into(), if pl { ch.map_pl.get(&m).cloned().unwrap_or(m) } else { m });
    if pl {
        let e = d.pl.enemy.get(&fk)?;
        v.insert("enemy".into(), e.first()?.clone());
        v.insert("enemy_gen".into(), e.get(1)?.clone());
        v.insert("enemy_ins".into(), e.get(2)?.clone());
        v.insert("enemies".into(), e.get(1)?.clone());
    } else {
        v.insert("enemy".into(), f.enemy.clone());
        v.insert("enemies".into(), format!("the {}", f.enemies));
    }
    Some(v)
}

fn render_stored_pl(d: &Data, lid: &str, params: &Value) -> Option<String> {
    let params = params.as_object()?;
    if let Some(ids) = lid.strip_prefix("al/") {
        let ids: Vec<&str> = ids.split('+').collect();
        if ids.iter().any(|i| !d.alife.reg.contains_key(*i)) {
            return None;
        }
        let people = people_of(d, params, true)?;
        let sym = sym_param(params)?;
        let fk = match sym.get("f") {
            None => "W".to_string(),
            Some(x) => x.as_str()?.to_string(),
        };
        let vp = lex_expand_pl(&sym)?;
        let parts = ids.iter().map(|i| render_lex(&d.alife.reg.get(*i)?.pl, &people, &vp, true, &fk)).collect::<Option<Vec<_>>>()?;
        return Some(parts.join(" "));
    }
    if lid.starts_with("cn/") {
        if lid != "cn/recap" {
            return None;
        }
        let day = py_int(params.get("d").unwrap_or(&Value::from(0)))?;
        let lst = |k: &str, fmt: &dyn Fn(&str, i64) -> String| -> Option<String> {
            let mut parts = Vec::new();
            if let Some(a) = params.get(k) {
                for x in a.as_array()? {
                    let x = x.as_array()?;
                    parts.push(fmt(&py_str(x.first()?), py_int(x.get(2)?)?));
                }
            }
            Some(if parts.len() <= 1 { parts.concat() } else { format!("{} i {}", parts[..parts.len() - 1].join(", "), parts[parts.len() - 1]) })
        };
        let dead = lst("dead", &|n, d| format!("{} (dzień {})", n, d))?;
        let sick = lst("sick", &|n, d| format!("{} (od dnia {})", n, d))?;
        let mut s = Vec::new();
        if !dead.is_empty() {
            s.push(format!("Ostatni, których pochowaliśmy: {}.", dead));
        }
        if !sick.is_empty() {
            s.push(format!("Wciąż leżą: {}.", sick));
        }
        if dead.is_empty() && sick.is_empty() && crate::text::truthy(params.get("alive")) {
            s.push(format!("Wciąż jest nas {}.", py_int(params.get("alive")?)?));
        }
        s.push(format!("Zapiski idą dalej od dnia {}.", day));
        return Some(format!("DZIEŃ {}: GDZIE STOIMY. {}", day, s.join(" ")));
    }
    if lid.starts_with("arc/") {
        return None;
    }
    if lid.starts_with("pk/") || lid.starts_with("sg/") {
        return params.get("tp").and_then(Value::as_str).map(str::to_string);
    }
    if let Some(ids) = lid.strip_prefix("ch/") {
        let ids: Vec<&str> = ids.split('+').collect();
        if ids.iter().any(|i| !d.chron.reg.contains_key(*i)) {
            return None;
        }
        let people = people_of(d, params, true)?;
        let sym = sym_param(params)?;
        let fk = sym.get("f")?.as_str()?.to_string();
        let vars = chron_vars(d, &sym, true)?;
        let parts = ids.iter().map(|i| render_lex(&d.chron.reg.get(*i)?.1, &people, &vars, true, &fk)).collect::<Option<Vec<_>>>()?;
        return Some(parts.join(" "));
    }
    let (_, _, pl_tpl) = d.pl.reg.get(lid)?;
    let people = people_of(d, params, false)?;
    let sym = sym_param(params)?;
    render_pl(pl_tpl, &people, &expand_pl(&sym)?)
}

fn style_arc(lid: &str) -> bool {
    if let Some(r) = lid.strip_prefix("ch/") {
        return r.split('+').next().unwrap_or("").split('~').next().unwrap_or("").starts_with("arc_");
    }
    lid.starts_with("arc/")
}

fn days_done(c: &Connection, tid: &str) -> Option<i64> {
    let v: Option<rusqlite::types::Value> = c.query_row("SELECT day FROM clans WHERE token_id=?", params![tid], |r| r.get(0)).optional().ok()?;
    match v {
        None | Some(rusqlite::types::Value::Null) => None,
        Some(x) => py_int(&sql_val(x)),
    }
}

fn read_rows(c: &Connection, tid: &str, pl: bool) -> Vec<Row> {
    let d = match data() {
        Some(d) => d,
        None => return Vec::new(),
    };
    type Raw = (i64, rusqlite::types::Value, Option<String>, Option<String>, Option<String>, f64, Option<String>, Option<String>);
    let full = "SELECT id,day,kind,text,names,ts,lid,params FROM clan_feed WHERE token_id=? AND id>0 ORDER BY id DESC LIMIT ?";
    let short = "SELECT id,day,kind,text,names,ts,NULL,NULL FROM clan_feed WHERE token_id=? AND id>0 ORDER BY id DESC LIMIT ?";
    let run = |sql: &str| -> Option<Vec<Raw>> {
        let mut st = c.prepare(sql).ok()?;
        let rows = st
            .query_map(params![tid, STORED_WINDOW as i64], |r| {
                Ok((
                    r.get::<_, i64>(0)?,
                    r.get::<_, rusqlite::types::Value>(1)?,
                    r.get::<_, Option<String>>(2)?,
                    r.get::<_, Option<String>>(3)?,
                    r.get::<_, Option<String>>(4)?,
                    r.get::<_, Option<f64>>(5)?.unwrap_or(0.0),
                    r.get::<_, Option<String>>(6)?,
                    r.get::<_, Option<String>>(7)?,
                ))
            })
            .ok()?;
        rows.collect::<Result<Vec<_>, _>>().ok()
    };
    let raw = match run(full).or_else(|| run(short)) {
        Some(r) => r,
        None => return Vec::new(),
    };
    let done = days_done(c, tid);
    let mut out = Vec::new();
    for (id, day, kind, text, names, ts, lid, params) in raw {
        let mut text = text.unwrap_or_default();
        if pl {
            if let (Some(l), Some(p)) = (&lid, &params) {
                if !l.is_empty() && !p.is_empty() {
                    if let Ok(pv) = serde_json::from_str::<Value>(p) {
                        if let Some(t) = render_stored_pl(d, l, &pv) {
                            if !t.is_empty() {
                                text = t;
                            }
                        }
                    }
                }
            }
        }
        if let Some(done) = done {
            let dv = sql_val(day);
            let dd = if crate::text::truthy(Some(&dv)) { py_int(&dv).unwrap_or(0) } else { 0 };
            if dd > done {
                continue;
            }
        }
        if lid.as_deref().map_or(false, style_arc) {
            continue;
        }
        let names = names.and_then(|n| serde_json::from_str::<Value>(if n.is_empty() { "[]" } else { &n }).ok()).and_then(|v| v.as_array().cloned()).unwrap_or_default();
        out.push(Row { id, kind: kind.unwrap_or_default(), text, names, ts });
    }
    out.truncate(12);
    out
}

fn plural(t: &str) -> bool {
    group_re().map_or(false, |r| r.is_match(t)) || pl_group_re().map_or(false, |r| r.is_match(t))
}

fn voice_of(lid: Option<&str>) -> &'static str {
    let pool = lid.unwrap_or("").split('/').next().unwrap_or("");
    if pool.starts_with("solo:") || pool.starts_with("t:solo_") {
        "solo"
    } else if pool.starts_with("duo:") {
        "duo"
    } else {
        "group"
    }
}

fn widget_keep(d: &Data, c: &Connection, tid: &str, st: &Value, rows: Vec<Row>, pl: bool) -> Vec<Row> {
    let alive: HashSet<String> = st.get("people").and_then(Value::as_array).map(|a| a.iter().filter_map(|p| p.get("n").map(py_str)).collect()).unwrap_or_default();
    let dead_list: Vec<&Value> = st.get("dead").and_then(Value::as_array).map(|a| a.iter().collect()).unwrap_or_default();
    let r0: i64 = c
        .query_row("SELECT MAX(id) FROM clan_feed WHERE token_id=? AND kind='founding'", params![tid], |r| r.get::<_, Option<i64>>(0))
        .ok()
        .flatten()
        .unwrap_or(0);
    let min_id = rows.iter().map(|m| m.id).min().unwrap_or(0);
    let mut lids: std::collections::HashMap<i64, Option<String>> = std::collections::HashMap::new();
    if let Ok(mut q) = c.prepare("SELECT id,lid FROM clan_feed WHERE token_id=? AND id>=?") {
        if let Ok(it) = q.query_map(params![tid, min_id], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, Option<String>>(1)?))) {
            for (i, l) in it.flatten() {
                lids.insert(i, l);
            }
        }
    }
    let n_alive = alive.len();
    let feeling = &d.story.feeling;
    let crowd = crowd_re();
    let mut out = Vec::new();
    for mut m in rows {
        let k = m.kind.clone();
        if m.id < r0 {
            continue;
        }
        let voice = voice_of(lids.get(&m.id).and_then(|x| x.as_deref()));
        let text = m.text.clone();
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
        if n_alive == 1 && feeling.contains(&k) {
            continue;
        }
        if n_alive == 1 && plural(&text) {
            let parts: Vec<&str> = split_sentences(&text).into_iter().filter(|x| !x.is_empty()).collect();
            if parts.is_empty() || plural(parts[0]) {
                if !(k.starts_with("death") && !m.names.is_empty()) {
                    continue;
                }
                let n0 = &m.names[0];
                let g = dead_list.iter().find(|x| x.get("n") == Some(n0)).map(|x| x.get("g").map(py_str).unwrap_or_else(|| "None".into())).unwrap_or_else(|| "m".into());
                let col = if !pl { 0 } else if g == "f" { 2 } else { 1 };
                let row = d.story.solo_death.get(&k).or_else(|| d.story.solo_death.get("death"));
                if let Some(row) = row {
                    let f = match col {
                        0 => &row.0,
                        1 => &row.1,
                        _ => &row.2,
                    };
                    m.text = f.replace("{A}", &py_str(n0));
                }
                out.push(m);
                continue;
            }
            m.text = parts.into_iter().filter(|x| !plural(x)).collect::<Vec<_>>().join(" ");
        }
        if n_alive == 2 && crowd.map_or(false, |r| r.is_match(&text)) {
            continue;
        }
        if !feeling.contains(&k) {
            out.push(m);
            continue;
        }
        let keep = if m.names.is_empty() {
            true
        } else if k == "ticker" {
            m.names.iter().all(|x| alive.contains(&py_str(x)))
        } else {
            alive.contains(&py_str(&m.names[0]))
        };
        if keep {
            out.push(m);
        }
    }
    out
}

fn pick_slot(d: &Data, c: &Connection, tid: &str, st: Option<&RawState>, slot: i64, n: usize, pl: bool, now: f64) -> Vec<(String, String)> {
    let mut rows = read_rows(c, tid, pl);
    if let Some(st) = st {
        rows = widget_keep(d, c, tid, &st.value, rows, pl);
    }
    if rows.is_empty() {
        return Vec::new();
    }
    let fresh: Vec<Row> = rows.iter().filter(|m| now - m.ts < FRESH_S).take(n).cloned().collect();
    let fresh_ids: HashSet<i64> = fresh.iter().map(|m| m.id).collect();
    let rest: Vec<&Row> = rows.iter().filter(|m| !fresh_ids.contains(&m.id)).collect();
    let mut take = fresh;
    if !rest.is_empty() && take.len() < n {
        let start = (slot * n as i64).rem_euclid(rest.len() as i64) as usize;
        for k in 0..(n - take.len()).min(rest.len()) {
            take.push(rest[(start + k) % rest.len()].clone());
        }
    }
    take.sort_by_key(|m| m.id);
    take.into_iter().map(|m| (m.id.to_string(), widget_text(&m.text))).collect()
}

pub fn menu_lines(c: &Connection, tid: &str, n: usize, lang: &str, now: f64) -> Vec<(String, String)> {
    let d = match data() {
        Some(d) => d,
        None => return Vec::new(),
    };
    let pl = norm_lang(lang) == "pl";
    let slot = slot_of(now);
    let raw = load_state(c, tid);
    let mut out = pick_slot(d, c, tid, raw.as_ref(), slot, n.min(2), pl, now);
    if out.len() < n {
        let st = ensure_roster(c, tid, raw.as_ref());
        if let Some(st) = &st {
            let want = d.alife.ticker_share.min(n - out.len());
            if let Some(lines) = alife::ticker_lines(d, c, tid, st, slot, want, pl) {
                out.extend(lines);
            }
        }
        if let Some(st) = &st {
            if let Some(lines) = live_lines(d, st, tid, n - out.len(), pl, slot) {
                out.extend(lines);
            }
        }
    }
    out.truncate(n);
    out
}
