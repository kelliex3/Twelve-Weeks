use crate::data::Data;
use crate::pyrand::PyRandom;
use crate::rules::{week_day, SERAPHITES};
use crate::story::{Member, State};
use crate::text::{clen, kid_text_ok, lex_expand_en, lex_expand_pl, py_int, py_str, render_lex, truthy, widget_text, People, Person, WIDGET_MAX};
use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{Map, Value};
use std::collections::{HashMap, HashSet};

struct Fact {
    cls: String,
    key: String,
    pool: String,
    people: People,
    sym: Map<String, Value>,
}

fn fkey(st: &State) -> &'static str {
    if st.fac == SERAPHITES {
        "S"
    } else {
        "W"
    }
}

fn season_of(week: i64) -> i64 {
    if week <= 4 {
        0
    } else if week <= 8 {
        1
    } else {
        2
    }
}

fn by_id<'a>(st: &'a State, i: &Value) -> Option<&'a Member> {
    let i = i.as_i64()?;
    st.people.iter().find(|p| p.i == i)
}

fn pp(p: &Member) -> Person {
    p.person()
}

fn load(c: &Connection, tid: &str) -> Option<Map<String, Value>> {
    let row: Option<Option<String>> = c.query_row("SELECT state FROM clan_alife WHERE token_id=?", params![tid], |r| r.get(0)).optional().ok()?;
    match serde_json::from_str::<Value>(&row??).ok()? {
        Value::Object(m) if m.get("v") == Some(&Value::from(1)) => Some(m),
        _ => None,
    }
}

fn same_num(a: Option<&Value>, b: i64) -> Option<bool> {
    let a = a?;
    Some(match a {
        Value::Number(n) => n.as_f64() == Some(b as f64),
        Value::Bool(x) => (*x as i64) == b,
        _ => false,
    })
}

fn rec_for(d: &Data, al: &mut Map<String, Value>, p: &Member, fk: &str) -> Option<()> {
    let k = p.i.to_string();
    if al.get("P")?.as_object()?.contains_key(&k) {
        return Some(());
    }
    let mut rng = PyRandom::new(&format!("{}:{}:p{}", py_str(al.get("seed")?), py_str(al.get("run")?), p.i));
    let a = &d.alife;
    let mut jobs: Vec<&String> = a.jobs.iter().filter(|r| r.get(1).map_or(false, |s| s == "*" || s == fk)).filter_map(|r| r.first()).collect();
    let mut origs: Vec<&String> = a.origins.iter().filter(|r| r.get(1).map_or(false, |s| s == "*" || s == fk)).filter_map(|r| r.first()).collect();
    if p.is_kid() {
        let o: Vec<&String> = origs.iter().copied().filter(|o| !a.origin_combat.contains(*o)).collect();
        if !o.is_empty() {
            origs = o;
        }
        let j: Vec<&String> = jobs.iter().copied().filter(|j| !a.job_combat.contains(*j)).collect();
        if !j.is_empty() {
            jobs = j;
        }
    }
    let t = rng.sample_indices(a.traits.len(), 2)?;
    let job = (*rng.choice(&jobs)?).clone();
    let orig = (*rng.choice(&origs)?).clone();
    let mut r = Map::new();
    r.insert("t".into(), Value::from(t));
    r.insert("job".into(), Value::from(job));
    r.insert("orig".into(), Value::from(orig));
    for key in ["f", "rv", "pa", "sk"] {
        r.insert(key.into(), Value::from(0));
    }
    al.get_mut("P")?.as_object_mut()?.insert(k, Value::Object(r));
    Some(())
}

fn state_for_read(d: &Data, c: &Connection, tid: &str, st: &State) -> Option<Map<String, Value>> {
    let fk = fkey(st);
    let loaded = load(c, tid);
    let fresh = match &loaded {
        None => true,
        Some(al) => !same_num(al.get("run"), st.run)? || !same_num(al.get("fac"), st.fac)?,
    };
    let mut al = if fresh {
        let mut m = Map::new();
        m.insert("v".into(), Value::from(1));
        m.insert("seed".into(), Value::from(tid));
        m.insert("run".into(), Value::from(st.run));
        m.insert("fac".into(), Value::from(st.fac));
        m.insert("P".into(), Value::Object(Map::new()));
        m.insert("T".into(), Value::Array(Vec::new()));
        m.insert("D".into(), Value::Array(Vec::new()));
        m
    } else {
        loaded?
    };
    for p in &st.people {
        rec_for(d, &mut al, p, fk)?;
    }
    Some(al)
}

fn weather(d: &Data, al: &Map<String, Value>, day: i64) -> Option<String> {
    let w: [f64; 6] = match season_of(week_day(day).0) {
        0 => [4.0, 2.0, 1.0, 2.0, 1.0, 2.0],
        1 => [2.0, 2.0, 4.0, 2.0, 1.0, 1.0],
        _ => [3.0, 2.0, 1.0, 3.0, 1.0, 2.0],
    };
    let mut rng = PyRandom::new(&format!("{}:{}:wx:{}", py_str(al.get("seed")?), py_str(al.get("run")?), day.div_euclid(2)));
    d.alife.weather.get(rng.choices_weighted(&w)?).cloned()
}

fn one(c: char, p: Person) -> People {
    let mut x = People::default();
    x.set(c, p);
    x
}

fn two(a: Person, b: Person) -> People {
    let mut x = People::default();
    x.set('A', a);
    x.set('B', b);
    x
}

fn sym_of(pairs: &[(&str, Value)]) -> Map<String, Value> {
    let mut m = Map::new();
    for (k, v) in pairs {
        m.insert(k.to_string(), v.clone());
    }
    m
}

fn facts(d: &Data, al: &Map<String, Value>, st: &State) -> Option<Vec<Fact>> {
    let a = &d.alife;
    let fk = fkey(st);
    let day = st.day;
    let week = week_day(day).0;
    let mut base = Map::new();
    base.insert("f".into(), Value::from(fk));
    base.insert("day".into(), Value::from(day));
    base.insert("week".into(), Value::from(week));
    base.insert("size".into(), Value::from(st.people.len()));
    base.insert("season".into(), Value::from(season_of(week)));
    let threads = al.get("T")?.as_array()?;
    let mut away: HashSet<i64> = HashSet::new();
    for t in threads {
        if t.get("k")? == "exp" {
            for x in t.get("a")?.as_array()? {
                if let Some(i) = x.as_i64() {
                    away.insert(i);
                }
            }
        }
    }
    let mut out: Vec<(String, String, String, People, Map<String, Value>)> = Vec::new();
    for t in threads {
        let ids = t.get("a")?.as_array()?;
        let ps: Vec<Option<&Member>> = ids.iter().map(|i| by_id(st, i)).collect();
        if ps.is_empty() || ps.iter().any(Option::is_none) {
            continue;
        }
        let ps: Vec<&Member> = ps.into_iter().flatten().collect();
        let k = t.get("k")?.as_str().unwrap_or("");
        let x = t.get("x")?;
        let ti = || -> Option<i64> { py_int(t.get("i")?) };
        let all_h = ps.iter().all(|p| p.s == "h");
        if k == "exp" && py_int(t.get("d")?)? > day && all_h {
            let nd = (py_int(t.get("d")?)? - day).max(1);
            out.push(("f_exp".into(), format!("exp{}", ti()?), "f_exp".into(), two(pp(ps[0]), pp(ps[ps.len() - 1])), sym_of(&[("dest", x.get("dest")?.clone()), ("goal", x.get("goal")?.clone()), ("nd", Value::from(nd))])));
        } else if k == "inj" && ps[0].s == "h" {
            out.push(("f_inj".into(), format!("inj{}", ti()?), "f_inj".into(), one('A', pp(ps[0])), sym_of(&[("inj", x.get("inj")?.clone())])));
        } else if k == "proj" && ps.len() >= 2 {
            let pv = x.get("p")?.as_f64()?;
            let nv = x.get("n")?.as_f64()?;
            if nv == 0.0 {
                return None;
            }
            let prog = ((4.0 * pv / nv).trunc() as i64).min(3);
            out.push(("f_proj".into(), format!("proj{}", ti()?), "f_proj".into(), two(pp(ps[0]), pp(ps[1])), sym_of(&[("proj", x.get("proj")?.clone()), ("prog", Value::from(prog))])));
        } else if k == "trade" && py_int(t.get("d")?)? >= day {
            out.push(("f_trade".into(), format!("trade{}", ti()?), "f_trade".into(), one('A', pp(ps[0])), sym_of(&[("grp", x.get("grp")?.clone()), ("dest", x.get("dest")?.clone()), ("due", t.get("d")?.clone())])));
        } else if k == "qrl" && ps.len() >= 2 {
            out.push(("f_rival".into(), format!("qrl{}", ti()?), "f_rival".into(), two(pp(ps[0]), pp(ps[1])), Map::new()));
        } else if k == "men" && ps.len() >= 2 && all_h {
            let kid_combat = if ps.iter().any(|p| p.is_kid()) {
                let sk = x.get("skill")?.as_str()?;
                if !a.skills.contains_key(sk) {
                    return None;
                }
                a.skill_combat.contains(sk)
            } else {
                false
            };
            if !kid_combat {
                out.push(("f_mentor".into(), format!("men{}", ti()?), "f_mentor".into(), two(pp(ps[0]), pp(ps[1])), sym_of(&[("skill", x.get("skill")?.clone())])));
            }
        }
    }
    let recs = al.get("P")?.as_object()?;
    let mut seen_pairs: HashSet<(String, i64, i64)> = HashSet::new();
    for p in &st.people {
        let r = match recs.get(&p.i.to_string()) {
            Some(r) if truthy(Some(r)) => r,
            _ => continue,
        };
        if away.contains(&p.i) {
            continue;
        }
        if p.s == "h" || p.s == "u" {
            let tl = r.get("t")?.as_array()?;
            for n in 0..2 {
                let idx = py_int(tl.get(n)?)?;
                let len = a.traits.len() as i64;
                let j = if idx < 0 { idx + len } else { idx };
                let trait_id = a.traits.get(usize::try_from(j).ok()?)?;
                out.push(("f_trait".into(), format!("tr{}_{}", p.i, n), format!("f_trait:{}", trait_id), one('A', pp(p)), Map::new()));
            }
            out.push(("f_job".into(), format!("job{}", p.i), "f_job".into(), one('A', pp(p)), sym_of(&[("job", r.get("job")?.clone())])));
            let rpool = format!("f_role:{}", p.r);
            let rpool = if a.by_pool.contains_key(&rpool) { rpool } else { "f_role".into() };
            out.push(("f_role".into(), format!("role{}", p.i), rpool, one('A', pp(p)), Map::new()));
            if p.jd > 0 && p.jd >= st.day - 20 {
                out.push(("f_origin".into(), format!("orig{}", p.i), "f_origin".into(), one('A', pp(p)), sym_of(&[("orig", r.get("orig")?.clone()), ("jd", Value::from(p.jd))])));
            }
            for (key, pool) in [("f", "f_friend"), ("pa", "f_romance")] {
                let q = if truthy(r.get(key)) { r.get(key).and_then(|v| by_id(st, v)) } else { None };
                let q = match q {
                    Some(q) => q,
                    None => continue,
                };
                let pair = if p.i <= q.i { (p.i, q.i) } else { (q.i, p.i) };
                let tag = (pool.to_string(), pair.0, pair.1);
                if (q.s == "h" || q.s == "u") && !seen_pairs.contains(&tag) && (pool != "f_friend" || !truthy(r.get("pa"))) {
                    seen_pairs.insert(tag);
                    out.push((pool.into(), format!("{}{}_{}", pool, pair.0, pair.1), pool.into(), two(pp(p), pp(q)), Map::new()));
                }
            }
        } else if p.s == "s" {
            let mut tend: Vec<&Member> = st.people.iter().filter(|q| q.s == "h" && q.i != p.i && !away.contains(&q.i)).collect();
            if !tend.is_empty() {
                tend.sort_by_key(|q| (q.r != "medic", q.i));
                let ill = if p.il.is_empty() { "fever".to_string() } else { p.il.clone() };
                out.push(("f_vigil".into(), format!("vig{}", p.i), "f_vigil".into(), two(pp(p), pp(tend[0])), sym_of(&[("ill", Value::from(ill))])));
            }
        }
    }
    for x in al.get("D")?.as_array()? {
        let dd = py_int(x.get("d")?)?;
        if day - dd <= 20 && a.mem_causes.contains(&py_str(x.get("c")?)) {
            let k = if truthy(x.get("k")) { x.get("k").and_then(|v| by_id(st, v)) } else { None };
            if let Some(k) = k {
                if k.s == "h" && !away.contains(&k.i) {
                    let mut people = one('A', pp(k));
                    people.set('D', Person { n: x.get("n")?.as_str()?.to_string(), g: py_str(x.get("g")?), r: py_str(x.get("r")?) });
                    out.push(("f_memorial".into(), format!("mem{}", py_int(x.get("i")?)?), "f_memorial".into(), people, sym_of(&[("keep", x.get("keep")?.clone())])));
                }
            }
        }
    }
    let wx = weather(d, al, day)?;
    out.push(("wx".into(), format!("wx{}", day), format!("wx_{}", wx), People::default(), Map::new()));
    out.push(("f_season".into(), "season".into(), "f_season".into(), People::default(), Map::new()));
    out.push(("f_count".into(), "count".into(), "f_count".into(), People::default(), Map::new()));
    if st.ss >= 2.0 {
        out.push(("f_rat_good".into(), "ratg".into(), "f_rat_good".into(), People::default(), Map::new()));
    }
    if st.ds >= 2.0 {
        out.push(("f_rat_bad".into(), "ratb".into(), "f_rat_bad".into(), People::default(), Map::new()));
    }
    Some(
        out.into_iter()
            .map(|(cls, key, pool, people, sy)| {
                let mut sym = base.clone();
                for (k, v) in sy {
                    sym.insert(k, v);
                }
                Fact { cls, key, pool, people, sym }
            })
            .collect(),
    )
}

fn daily_facts(d: &Data, al: &Map<String, Value>, st: &State, tid: &str) -> Option<Vec<Fact>> {
    let fs = facts(d, al, st)?;
    let (mut dyn_, mut per): (Vec<Fact>, Vec<Fact>) = fs.into_iter().partition(|f| !d.alife.persistent.contains(&f.cls));
    per.sort_by(|a, b| a.key.cmp(&b.key));
    let k = (d.alife.daily_facts as i64 - dyn_.len() as i64).min(per.len() as i64).max(0) as usize;
    if !per.is_empty() {
        let idx = PyRandom::new(&format!("sub:{}:{}:{}", tid, st.run, st.day)).sample_indices(per.len(), k)?;
        let mut slots: Vec<Option<Fact>> = per.into_iter().map(Some).collect();
        for i in idx {
            dyn_.push(slots[i].take()?);
        }
    }
    Some(dyn_)
}

fn tone_ok(tone: &str, morale: f64) -> bool {
    tone == "*" || (tone == "g" && morale >= 55.0) || (tone == "n" && (35.0..=70.0).contains(&morale)) || (tone == "b" && morale <= 45.0)
}

fn rows_for<'a>(d: &'a Data, pool: &str, fk: &str, morale: f64, letters: &str, av_en: &HashSet<String>, av_pl: &HashSet<String>, people: Option<&People>) -> Vec<&'a str> {
    let mut out = Vec::new();
    let ids = match d.alife.by_pool.get(pool) {
        Some(v) => v,
        None => return out,
    };
    for id in ids {
        let t = match d.alife.reg.get(id) {
            Some(t) => t,
            None => continue,
        };
        if t.s != "*" && t.s != fk {
            continue;
        }
        if !tone_ok(&t.t, morale) || !t.l.chars().all(|c| letters.contains(c)) {
            continue;
        }
        if !t.ve.iter().all(|v| av_en.contains(v)) || !t.vp.iter().all(|v| av_pl.contains(v)) {
            continue;
        }
        if let Some(p) = people.filter(|p| !p.0.is_empty()) {
            let kid = t.k.chars().any(|c| p.get(c).map_or(false, |x| x.r == "kid"));
            if kid && t.c {
                continue;
            }
        }
        out.push(id.as_str());
    }
    out
}

fn deck(d: &Data, fs: &[Fact], seed: &str, epoch: i64) -> Vec<(usize, i64)> {
    let mut order: Vec<usize> = (0..fs.len()).collect();
    order.sort_by(|a, b| fs[*a].key.cmp(&fs[*b].key));
    let mut items: Vec<(usize, i64)> = Vec::new();
    for i in order {
        let w = d.alife.fact_w.get(&fs[i].cls).copied().unwrap_or(1);
        for m in 0..w {
            items.push((i, m));
        }
    }
    PyRandom::new(&format!("{}:{}", seed, epoch)).shuffle(&mut items);
    for j in 1..items.len() {
        let prev = fs[items[j - 1].0].cls.clone();
        if fs[items[j].0].cls == prev {
            let nxt = items.get(j + 1).map(|x| fs[x.0].cls.clone());
            for q in j + 1..items.len() {
                let cq = &fs[items[q].0].cls;
                if *cq != prev && Some(cq) != nxt.as_ref() {
                    items.swap(j, q);
                    break;
                }
            }
        }
    }
    items
}

fn variant<'a>(rows: &[&'a str], key: &str, n: i64) -> &'a str {
    let mut perm = rows.to_vec();
    PyRandom::new(&format!("v:{}", key)).shuffle(&mut perm);
    perm[n.rem_euclid(perm.len() as i64) as usize]
}

fn render_ids(d: &Data, ids: &[&str], people: &People, sym: &Map<String, Value>, pl: bool, fk: &str) -> Option<String> {
    let vars = if pl { lex_expand_pl(sym)? } else { lex_expand_en(sym)? };
    let mut parts = Vec::new();
    for id in ids {
        let t = d.alife.reg.get(*id)?;
        parts.push(render_lex(if pl { &t.pl } else { &t.en }, people, &vars, pl, fk)?);
    }
    Some(parts.join(" "))
}

fn avail(sym: &Map<String, Value>) -> Option<(HashSet<String>, HashSet<String>)> {
    Some((lex_expand_en(sym)?.into_keys().collect(), lex_expand_pl(sym)?.into_keys().collect()))
}

pub fn ticker_lines(d: &Data, c: &Connection, tid: &str, st: &State, slot: i64, n: usize, pl: bool) -> Option<Vec<(String, String)>> {
    if n == 0 || st.people.len() < d.alife.min_clan {
        return Some(Vec::new());
    }
    let al = state_for_read(d, c, tid, st)?;
    let fk = fkey(st);
    let fs = daily_facts(d, &al, st, tid)?;
    if fs.is_empty() {
        return Some(Vec::new());
    }
    let seed = format!("{}:{}:{}", tid, st.run, st.day);
    let morale = st.morale;
    let l = deck(d, &fs, &seed, 0).len() as i64;
    if l == 0 {
        return Some(Vec::new());
    }
    let mut decks: HashMap<i64, Vec<(usize, i64)>> = HashMap::new();
    let mut out = Vec::new();
    let share = d.alife.ticker_share as i64;
    for i in 0..n as i64 {
        let k = slot * share + i;
        let mut found = None;
        for attempt in 0..8 {
            let e = (k + attempt).div_euclid(l);
            let j = (k + attempt).rem_euclid(l) as usize;
            let dk = decks.entry(e).or_insert_with(|| deck(d, &fs, &seed, e));
            let (fi, m) = dk[j];
            let f = &fs[fi];
            let (av_en, av_pl) = avail(&f.sym)?;
            let letters = f.people.letters();
            let mut rows = rows_for(d, &f.pool, fk, morale, &letters, &av_en, &av_pl, Some(&f.people));
            if rows.is_empty() {
                rows = rows_for(d, &f.pool, fk, 50.0, &letters, &av_en, &av_pl, Some(&f.people));
            }
            if !rows.is_empty() {
                found = Some((e, fi, m, rows));
                break;
            }
        }
        let (e, fi, m, rows) = match found {
            Some(x) => x,
            None => continue,
        };
        let f = &fs[fi];
        let w = d.alife.fact_w.get(&f.cls).copied().unwrap_or(1);
        let first = variant(&rows, &format!("{}:{}", tid, f.key), st.day + e * w + m);
        let mut ids = vec![first];
        let mut trng = PyRandom::new(&format!("tail:{}:{}:{}:{}", tid, st.day, slot, i));
        if d.alife.tail_ok.contains(&f.cls) && trng.random() < 0.5 {
            let wx = weather(d, &al, st.day)?;
            let tpool = if trng.random() < 0.3 { format!("f_tail_{}", wx) } else { "f_tail".to_string() };
            let empty = HashSet::new();
            let tails = rows_for(d, &tpool, fk, morale, "", &empty, &empty, None);
            if let Some(tl) = trng.choice(&tails) {
                let head = render_ids(d, &[first], &f.people, &f.sym, pl, fk)?;
                let tail = render_ids(d, &[tl], &People::default(), &f.sym, pl, fk)?;
                if clen(&head) + 1 + clen(&tail) <= WIDGET_MAX {
                    ids.push(tl);
                }
            }
        }
        let text = render_ids(d, &ids, &f.people, &f.sym, pl, fk)?;
        if f.people.0.iter().any(|(_, p)| p.r == "kid") && !kid_text_ok(&text, &f.people, pl) {
            continue;
        }
        out.push((format!("a{}{:02}", slot.rem_euclid(100000), i), widget_text(&text)));
    }
    Some(out)
}
