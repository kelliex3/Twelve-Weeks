//! py: clan_continuity.py - clan journal continuity (EN + PL): call-backs and thread ids for the diary rows, the one-time recap row of a
//! clan that existed before the chronicle, proper nouns in lower-cased headlines. Never changes a number or a person's state.
use crate::clan::clan::{int_of, pystr, truthy};
use crate::clan::data;
use crate::py;
use regex::Regex;
use rusqlite::{Connection, OptionalExtension};
use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet};

fn tc(name: &str) -> &'static Value {
    data::t("clan_continuity", name)
}

fn strs_of(name: &str) -> Vec<&'static str> {
    data::strs(tc(name))
}

fn is_word(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

fn rstrip(s: &str) -> &str {
    s.trim_end_matches(py::is_py_space)
}

// py: clan_continuity.py:34 _days_ago_en
fn days_ago_en(n: i64) -> String {
    if n == 1 {
        "yesterday".into()
    } else {
        format!("{} days ago", n)
    }
}

// py: clan_continuity.py:38 tail
/// The call-back sentence. role: 'ill' | 'mourn'; a = onset day / death day; b = the row's day. Relative ("for 4 days"), never an absolute clan day.
pub fn tail(kind: &str, role: &str, name: &str, a: i64, b: i64, lang: &str) -> String {
    let n = b - a;
    if n <= 0 {
        return String::new();
    }
    if lang == "pl" {
        if role == "ill" {
            if kind == "recovery" {
                return format!("{}: choroba zaczęła się {}.", name, if n == 1 { "wczoraj".to_string() } else { format!("{} dni wcześniej", n) });
            }
            if kind == "death_illness" {
                return format!("{}: choroba trwała {}.", name, if n == 1 { "dzień".to_string() } else { format!("{} dni", n) });
            }
            return format!("{} choruje od {}.", name, if n == 1 { "wczoraj".to_string() } else { format!("{} dni", n) });
        }
        return format!("{} nie żyje od {}.", name, if n == 1 { "wczoraj".to_string() } else { format!("{} dni", n) });
    }
    if role == "ill" {
        if kind == "recovery" {
            return format!("{} fell ill {}.", name, if n == 1 { "the day before".to_string() } else { format!("{} days earlier", n) });
        }
        if kind == "death_illness" {
            return format!("{} had been ill for {}.", name, if n == 1 { "a day".to_string() } else { format!("{} days", n) });
        }
        return format!("{} has been ill {}.", name, if n == 1 { "since yesterday".to_string() } else { format!("for {} days", n) });
    }
    format!("{} died {}.", name, days_ago_en(n))
}

fn map_pl(name: &str) -> String {
    data::t("clan_chronicle_maps", "PL").get(name).and_then(|v| v.as_str()).unwrap_or(name).to_string()
}

fn match_style(style: Option<&Value>) -> Option<Option<i64>> {
    let s = style?.as_str()?;
    let m = tc("MATCH_STYLES");
    let v = m.get(s)?;
    Some(v.as_i64())
}

// py: clan_continuity.py:66 match_tail
/// The call-back of a match story to the previous one on the same map: ' Last time on the Lakeside: day 20, lost.'
pub fn match_tail(prev: &Value, m: &Value, lang: &str) -> String {
    let won = match_style(prev.get("style")).flatten();
    let d = prev.get("day").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
    let name = m.get("map").map(pystr).unwrap_or_default();
    if lang == "pl" {
        let suffix = match won {
            None => "",
            Some(w) if w != 0 => ", wygrana",
            _ => ", porażka",
        };
        return format!(" Poprzednio ({}): dzień {}{}.", map_pl(&name), d, suffix);
    }
    let suffix = match won {
        None => "",
        Some(w) if w != 0 => ", won",
        _ => ", lost",
    };
    format!(" Last time on {}: day {}{}.", name, d, suffix)
}

// py: clan_continuity.py:78 haul_tail
pub fn haul_tail(prev: &Value, lang: &str) -> String {
    let d = prev.get("day").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
    if lang == "pl" {
        format!(" Poprzedni łup: dzień {}.", d)
    } else {
        format!(" The haul before this one came in on day {}.", d)
    }
}

struct Thread {
    day: i64,
    tid: String,
    last: Value,
    step: i64,
}

fn set(m: &mut Value, k: &str, v: Value) {
    if let Some(o) = m.as_object_mut() {
        o.insert(k.to_string(), v);
    }
}

fn who(m: &Value, k: &str) -> Option<Value> {
    m.get("who").and_then(|w| w.get(k)).filter(|v| truthy(v)).cloned()
}

// py: clan_continuity.py:87 annotate
/// ctx = the feed rows (newest first, read_feed shape, with "who" = {letter: name}); dead = clan_story state["dead"]; sick_since = {name: day}
/// of the members who are ill right now. Adds, in place, to every diary row that continues an earlier one: text tail, "thread", "tstep",
/// "prev". Returns the indices of the rows oldest first (the list Python returns).
pub fn annotate(ctx: &mut [Value], dead: &Value, sick_since: &Value, lang_req: &str) -> Vec<usize> {
    let mut death_day: HashMap<String, i64> = HashMap::new();
    for d in dead.as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
        death_day.insert(d.get("n").map(pystr).unwrap_or_else(|| "None".into()), d.get("d").map(int_of).unwrap_or(0));
    }
    let mut order: Vec<usize> = (0..ctx.len()).collect();
    order.sort_by_key(|&i| ctx[i].get("id").map(int_of).unwrap_or(0)); // oldest first (stable)
    let mut open_ill: HashMap<String, Thread> = HashMap::new();
    let mut open_mourn: HashMap<String, Thread> = HashMap::new();
    let mut first: HashMap<usize, String> = HashMap::new();
    let mut last_map: HashMap<String, usize> = HashMap::new();
    let mut last_haul: Option<usize> = None;
    let ill_kinds = strs_of("ILL_KINDS");
    let mourn_kinds = strs_of("MOURN_KINDS");
    let haul_style = tc("HAUL_STYLE").as_str().unwrap_or("");
    for &ix in &order {
        let (kind, day) = {
            let m = &ctx[ix];
            (m.get("kind").map(pystr).unwrap_or_default(), m.get("day").filter(|v| truthy(v)).map(int_of).unwrap_or(0))
        };
        let sty = ctx[ix].get("style").cloned();
        let lang = match ctx[ix].get("lang") {
            Some(l) if truthy(l) => pystr(l),
            _ => lang_req.to_string(),
        };
        let map_name = ctx[ix].get("map").filter(|v| truthy(v)).map(pystr);
        if match_style(sty.as_ref()).is_some() && map_name.is_some() {
            // a match story: calls back the previous one on the same map
            let mapn = map_name.clone().unwrap_or_default();
            if let Some(&pi) = last_map.get(&mapn) {
                let pv = ctx[pi].clone();
                if pv.get("day").filter(|v| truthy(v)).map(int_of).unwrap_or(0) < day {
                    let mc = ctx[ix].clone();
                    let text = rstrip(&mc.get("text").map(pystr).unwrap_or_default()).to_string() + &match_tail(&pv, &mc, &lang);
                    let thread = format!("raid-{}", mapn);
                    let tstep = pv.get("tstep").map(int_of).unwrap_or(1) + 1;
                    let m = &mut ctx[ix];
                    set(m, "text", Value::String(text));
                    set(m, "thread", Value::String(thread.clone()));
                    set(m, "prev", pv.get("id").cloned().unwrap_or(Value::Null));
                    set(m, "tstep", Value::from(tstep));
                    if pv.get("thread").is_none() {
                        let p = &mut ctx[pi];
                        set(p, "thread", Value::String(thread));
                        set(p, "tstep", Value::from(1));
                        set(p, "prev", Value::from(0));
                    }
                }
            }
            last_map.insert(mapn, ix);
            continue;
        }
        if sty.as_ref().and_then(|s| s.as_str()) == Some(haul_style) {
            if let Some(li) = last_haul {
                let lh = ctx[li].clone();
                let week_m = ctx[ix].get("week").cloned().unwrap_or(Value::Null);
                if lh.get("day").filter(|v| truthy(v)).map(int_of).unwrap_or(0) < day && lh.get("week").cloned().unwrap_or(Value::Null) == week_m {
                    let text = rstrip(&ctx[ix].get("text").map(pystr).unwrap_or_default()).to_string() + &haul_tail(&lh, &lang);
                    let thread = format!("haul-w{}", pystr(&week_m));
                    let tstep = lh.get("tstep").map(int_of).unwrap_or(1) + 1;
                    let m = &mut ctx[ix];
                    set(m, "text", Value::String(text));
                    set(m, "thread", Value::String(thread.clone()));
                    set(m, "prev", lh.get("id").cloned().unwrap_or(Value::Null));
                    set(m, "tstep", Value::from(tstep));
                    if lh.get("thread").is_none() {
                        let p = &mut ctx[li];
                        set(p, "thread", Value::String(thread));
                        set(p, "tstep", Value::from(1));
                        set(p, "prev", Value::from(0));
                    }
                }
            }
            last_haul = Some(ix);
            continue;
        }
        if sty.as_ref().map(truthy).unwrap_or(false) {
            continue; // other chronicle / saga / arc / perk rows carry their own continuity
        }
        let id = ctx[ix].get("id").cloned().unwrap_or(Value::Null);
        let w_a = who(&ctx[ix], "A");
        if kind == "sick" {
            if let Some(a) = &w_a {
                let name = pystr(a);
                let tid = format!("ill-{}-{}", name, day);
                first.insert(ix, tid.clone());
                open_ill.insert(name, Thread { day, tid, last: id, step: 1 });
                continue;
            }
        }
        if ill_kinds.contains(&kind.as_str()) {
            if let Some(a) = &w_a {
                let name = pystr(a);
                if !open_ill.contains_key(&name) {
                    if let Some(ss) = sick_since.get(&name) {
                        if int_of(ss) < day {
                            let sd = int_of(ss);
                            open_ill.insert(name.clone(), Thread { day: sd, tid: format!("ill-{}-{}", name, sd), last: Value::from(0), step: 0 });
                        }
                    }
                }
                let Some(t) = open_ill.get_mut(&name) else { continue };
                let ln = tail(&kind, "ill", &name, t.day, day, &lang);
                let text = ctx[ix].get("text").map(pystr).unwrap_or_default();
                if !ln.is_empty() && !text.contains(&format!("day {}", t.day)) && !text.contains(&format!("dnia {}", t.day)) {
                    set(&mut ctx[ix], "text", Value::String(format!("{} {}", rstrip(&text), ln)));
                }
                let m = &mut ctx[ix];
                set(m, "thread", Value::String(t.tid.clone()));
                set(m, "prev", t.last.clone());
                set(m, "tstep", Value::from(t.step + 1));
                t.last = id;
                t.step += 1;
                if kind == "recovery" || kind == "death_illness" {
                    open_ill.remove(&name);
                }
                continue;
            }
        }
        if kind.starts_with("death_") || kind.starts_with("loss") {
            let names = ctx[ix].get("names").filter(|v| truthy(v)).and_then(|v| v.as_array()).cloned().unwrap_or_default();
            for nm in names {
                let s = pystr(&nm);
                if death_day.get(&s) == Some(&day) {
                    let tid = format!("mourn-{}", pystr(&nm));
                    first.insert(ix, tid.clone());
                    open_mourn.insert(s, Thread { day, tid, last: id.clone(), step: 1 });
                    break;
                }
            }
            continue;
        }
        if mourn_kinds.contains(&kind.as_str()) {
            if let Some(d) = who(&ctx[ix], "D") {
                let name = pystr(&d);
                let dd = match death_day.get(&name) {
                    Some(&x) if x < day => x,
                    _ => continue,
                };
                let t = open_mourn.entry(name.clone()).or_insert_with(|| Thread { day: dd, tid: format!("mourn-{}", name), last: Value::from(0), step: 0 });
                let ln = tail(&kind, "mourn", &name, dd, day, &lang);
                let text = ctx[ix].get("text").map(pystr).unwrap_or_default();
                if !ln.is_empty() && !text.contains(&format!("day {}", dd)) && !text.contains(&format!("dnia {}", dd)) {
                    set(&mut ctx[ix], "text", Value::String(format!("{} {}", rstrip(&text), ln)));
                }
                let m = &mut ctx[ix];
                set(m, "thread", Value::String(t.tid.clone()));
                set(m, "prev", t.last.clone());
                set(m, "tstep", Value::from(t.step + 1));
                t.last = id;
                t.step += 1;
            }
        }
    }
    // the first row of a thread that did continue (onset / the death) is a thread member too
    let mut ids: HashSet<String> = HashSet::new();
    for &ix in &order {
        if let Some(t) = ctx[ix].get("thread").filter(|v| truthy(v)) {
            ids.insert(pystr(t));
        }
    }
    for &ix in &order {
        if let Some(f) = first.get(&ix) {
            if ids.contains(f) {
                let m = &mut ctx[ix];
                set(m, "thread", Value::String(f.clone()));
                set(m, "tstep", Value::from(1));
                set(m, "prev", Value::from(0));
            }
        }
    }
    let all_ids: Vec<Value> = order.iter().map(|&i| ctx[i].get("id").cloned().unwrap_or(Value::Null)).collect();
    for &ix in &order {
        let m = &ctx[ix];
        if m.get("thread").map(truthy).unwrap_or(false) && m.get("prev").map(truthy).unwrap_or(false) {
            let p = m.get("prev").cloned().unwrap_or(Value::Null);
            if !all_ids.contains(&p) {
                set(&mut ctx[ix], "prev", Value::from(0));
            }
        }
    }
    order
}

// py: clan_continuity.py:171 _lst
fn lst(items: &[(String, i64)], lang: &str, fmt: &str) -> String {
    let parts: Vec<String> = items
        .iter()
        .map(|(n, d)| {
            // fmt is "%s (...%d)": the name first, the day where %d stands
            let a = fmt.find("%s").unwrap_or(0);
            let b = fmt.find("%d").unwrap_or(fmt.len());
            format!("{}{}{}{}{}", &fmt[..a], n, &fmt[a + 2..b], d, fmt.get(b + 2..).unwrap_or(""))
        })
        .collect();
    if parts.len() <= 1 {
        return parts.concat();
    }
    format!("{}{}{}", parts[..parts.len() - 1].join(", "), if lang == "pl" { " i " } else { " and " }, parts[parts.len() - 1])
}

// py: clan_continuity.py:178 render_msg
pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
    if lid != "cn/recap" || !params.is_object() {
        return None;
    }
    let day = params.get("d").map(int_of).unwrap_or(0);
    let pairs = |k: &str| -> Vec<(String, i64)> {
        params
            .get(k)
            .and_then(|v| v.as_array())
            .map(|a| a.iter().map(|x| (x.get(0).map(pystr).unwrap_or_default(), x.get(2).map(int_of).unwrap_or(0))).collect())
            .unwrap_or_default()
    };
    let dead = pairs("dead");
    let sick = pairs("sick");
    let alive = params.get("alive").filter(|v| truthy(v)).map(int_of);
    let mut s: Vec<String> = Vec::new();
    if lang == "pl" {
        if !dead.is_empty() {
            s.push(format!("Ostatni, których pochowaliśmy: {}.", lst(&dead, lang, "%s (dzień %d)")));
        }
        if !sick.is_empty() {
            s.push(format!("Wciąż leżą: {}.", lst(&sick, lang, "%s (od dnia %d)")));
        }
        if dead.is_empty() && sick.is_empty() {
            if let Some(a) = alive {
                s.push(format!("Wciąż jest nas {}.", a));
            }
        }
        s.push(format!("Zapiski idą dalej od dnia {}.", day));
        return Some(format!("DZIEŃ {}: GDZIE STOIMY. {}", day, s.join(" ")));
    }
    if !dead.is_empty() {
        s.push(format!("The last we buried: {}.", lst(&dead, lang, "%s (day %d)")));
    }
    if !sick.is_empty() {
        s.push(format!("Still down: {}.", lst(&sick, lang, "%s (since day %d)")));
    }
    if dead.is_empty() && sick.is_empty() {
        if let Some(a) = alive {
            s.push(format!("{} of us are still here.", a));
        }
    }
    s.push(format!("The ledger goes on from day {}.", day));
    Some(format!("DAY {}: WHERE WE STAND. {}", day, s.join(" ")))
}

// py: clan_continuity.py:203 recap_rows
/// [] or ONE chronicle row; marks st["ch"]["cn"] either way (it is a one-time bridge).
pub fn recap_rows(c: &Connection, tid: &str, st: &mut Value, newc: &Value, mood: &str) -> Vec<Value> {
    let Some(so) = st.as_object_mut() else { return Vec::new() };
    let cs = so.entry("ch").or_insert_with(|| Value::Object(Map::new()));
    if cs.get("cn").map(truthy).unwrap_or(false) {
        return Vec::new();
    }
    let day = newc.get("day").map(int_of).unwrap_or(0);
    if let Some(co) = cs.as_object_mut() {
        co.insert("cn".into(), Value::from(day));
    }
    let n: i64 = match c
        .query_row("SELECT COUNT(*) FROM clan_feed WHERE token_id=? AND kind NOT IN ('chron','alife')", [tid], |r| r.get::<_, i64>(0))
        .optional()
    {
        Ok(Some(n)) => n,
        _ => return Vec::new(),
    };
    if n < tc("RECAP_MIN_ROWS").as_i64().unwrap_or(4) {
        return Vec::new();
    }
    let rd = tc("RECAP_DEAD").as_i64().unwrap_or(3) as usize;
    let rs = tc("RECAP_SICK").as_i64().unwrap_or(3) as usize;
    let rdays = tc("RECAP_DAYS").as_i64().unwrap_or(21);
    let dead_all: Vec<Value> = st.get("dead").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let dd = |d: &Value| d.get("d").map(int_of).unwrap_or(0);
    let mut sorted = dead_all;
    sorted.sort_by_key(|d| -dd(d));
    let dead: Vec<Value> = sorted.into_iter().filter(|d| day - dd(d) <= rdays && dd(d) < day).take(rd).collect();
    let people: Vec<Value> = st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default();
    let sick: Vec<&Value> = people
        .iter()
        .filter(|p| {
            p.get("s").and_then(|v| v.as_str()) == Some("s") && {
                let id = p.get("id").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
                0 < id && id < day
            }
        })
        .take(rs)
        .collect();
    let dead_rows: Vec<Value> = dead.iter().rev().map(|d| json!([d.get("n").cloned().unwrap_or(Value::Null), d.get("g").cloned().unwrap_or(json!("n")), dd(d)])).collect();
    let sick_rows: Vec<Value> =
        sick.iter().map(|p| json!([p.get("n").cloned().unwrap_or(Value::Null), p.get("g").cloned().unwrap_or(json!("n")), p.get("id").map(int_of).unwrap_or(0)])).collect();
    let params = json!({"alive": people.len() as i64, "d": day, "dead": dead_rows, "sick": sick_rows});
    let text = render_msg("cn/recap", &params, "en").unwrap_or_default();
    let mut names: Vec<Value> = dead_rows_names(&params, "dead");
    names.extend(dead_rows_names(&params, "sick"));
    vec![json!({"kind": "chron", "mood": mood, "text": text, "names": names, "lid": "cn/recap", "params": params})]
}

fn dead_rows_names(params: &Value, k: &str) -> Vec<Value> {
    params.get(k).and_then(|v| v.as_array()).map(|a| a.iter().map(|x| x.get(0).cloned().unwrap_or(Value::Null)).collect()).unwrap_or_default()
}

// py: clan_continuity.py:226 proper_nouns
/// Member names (extra) + every map display name (EN and PL): a headline that is lower-cased into a sentence must get them back with their capitals.
pub fn proper_nouns(extra: &[String]) -> Vec<String> {
    let mut out: Vec<String> = extra.iter().filter(|x| !x.is_empty()).cloned().collect();
    if let Some(pl) = data::t("clan_chronicle_maps", "PL").as_object() {
        out.extend(pl.keys().cloned());
        out.extend(pl.values().filter_map(|v| v.as_str().map(|s| s.to_string())));
    }
    if let Some(en) = data::t("clan_chronicle_maps", "EN_SHORT").as_object() {
        out.extend(en.values().filter_map(|v| v.as_str().map(|s| s.to_string())));
    }
    let mut seen: HashSet<String> = HashSet::new();
    let mut uniq: Vec<String> = Vec::new();
    for x in out {
        if !x.is_empty() && seen.insert(x.clone()) {
            uniq.push(x);
        }
    }
    uniq.sort_by_key(|x| std::cmp::Reverse(x.chars().count())); // (ties: Python's set order is arbitrary)
    uniq
}

// py: clan_continuity.py:238 restore_case
pub fn restore_case(text: &str, names: &[String]) -> String {
    let mut text = text.to_string();
    for n in names {
        let nl = n.to_lowercase();
        if nl.is_empty() || !text.to_lowercase().contains(&nl) {
            continue;
        }
        let Ok(re) = Regex::new(&format!("(?i){}", regex::escape(&nl))) else { continue };
        let mut out = String::new();
        let mut pos = 0usize;
        let mut start = 0usize;
        while start <= text.len() {
            let Some(mt) = re.find_at(&text, start) else { break };
            let (s, e) = (mt.start(), mt.end());
            let before = text[..s].chars().next_back();
            let after = text[e..].chars().next();
            if !before.map(is_word).unwrap_or(false) && !after.map(is_word).unwrap_or(false) {
                out.push_str(&text[pos..s]);
                out.push_str(n);
                pos = e;
                start = if e > s { e } else { e + text[e..].chars().next().map(|c| c.len_utf8()).unwrap_or(1) };
            } else {
                start = s + text[s..].chars().next().map(|c| c.len_utf8()).unwrap_or(1);
            }
        }
        out.push_str(&text[pos..]);
        text = out;
    }
    text
}
