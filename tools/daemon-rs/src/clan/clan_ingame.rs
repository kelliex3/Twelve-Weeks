//! clan_ingame.py: the in-game CLAN page's data (daemon op 0xB6 -> reply 0xB7, compact tab-separated lines) and the per-match player stats of the RAIDS page.
//! Pure functions of a sqlite connection (read only) except the `stats_*` writers.
use crate::clan::clan::{self, int_of, pystr, truthy, Counts};
use crate::clan::{clan_chronicle as ch, clan_status as status, clan_story as cs, clan_story_pl as pln, clan_supply, data, ext_clan_perks as extp};
use crate::py;
use regex::Regex;
use rusqlite::{params, Connection};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

pub const MAX_ROSTER: usize = 40;
pub const MAX_JOURNAL: usize = 24;
pub const MAX_MATCHES: usize = 8;
pub const MAX_STATS: usize = 12;
const STATS_KEEP_S: f64 = 60.0 * 86400.0;
pub const LATELY_DAYS: i64 = 7;
pub const ROLL: usize = 7;
const MATCH_EXPR: [&str; 5] = ["neutral", "happy", "dead", "angry", "perk"];
const STATS_SCHEMA: &str = "CREATE TABLE IF NOT EXISTS match_stats(mid TEXT, token TEXT, name TEXT, team INTEGER DEFAULT -1, win INTEGER DEFAULT -1, map TEXT, \
kills INTEGER DEFAULT -1, downs INTEGER DEFAULT -1, deaths INTEGER DEFAULT -1, revives INTEGER DEFAULT -1, execs INTEGER DEFAULT -1, heads INTEGER DEFAULT -1, \
parts INTEGER DEFAULT -1, left INTEGER DEFAULT 0, ts REAL, PRIMARY KEY(mid, token))";

type R<T> = Result<T, String>;

fn ci(t: &str) -> &'static Value {
    data::t("clan_ingame", t)
}

/// `TABLE.get(key)` of a str -> str table of the data.
fn tget(table: &str, key: &str) -> Option<&'static str> {
    ci(table).get(key).and_then(|v| v.as_str())
}

static ARC_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^(?:DAY|DZIEŃ) (\d+): ([^.]*)\.\s*([\s\S]*)$").expect("ARC_RE"));
static T_RE: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"[\t\r\n|]+").expect("T_RE"));

// py: clan_ingame.py:62 _icon
fn icon(k: &str) -> String {
    if data::set_has_str(ci("ICON_IDS"), k) {
        k.to_string()
    } else {
        "keepsake".to_string()
    }
}

/// Python `str(x or "")` of an optional value.
fn s_or(v: Option<&Value>) -> String {
    match v {
        Some(x) if truthy(x) => pystr(x),
        _ => String::new(),
    }
}

/// Python `str(x if x is not None else "")`.
fn s_none(v: Option<&Value>) -> String {
    match v {
        Some(Value::Null) | None => String::new(),
        Some(x) => pystr(x),
    }
}

// py: clan_ingame.py:67 _t
fn t(s: &str, n: usize) -> String {
    let r = T_RE.replace_all(s, " ");
    let r = py::strip(&r);
    if n > 0 {
        py::cut(r, n).to_string()
    } else {
        r.to_string()
    }
}

fn tv(v: Option<&Value>, n: usize) -> String {
    t(&s_none(v), n)
}

fn chars_find_last(cut: &[char], pat: &str) -> i64 {
    let p: Vec<char> = pat.chars().collect();
    if cut.len() < p.len() {
        return -1;
    }
    for i in (0..=cut.len() - p.len()).rev() {
        if cut[i..i + p.len()] == p[..] {
            return i as i64;
        }
    }
    -1
}

// py: clan_ingame.py:72 _first_sentences
fn first_sentences(text: &str, n: usize) -> String {
    let tt = t(text, 0);
    let chars: Vec<char> = tt.chars().collect();
    if chars.len() <= n {
        return tt;
    }
    let cut = &chars[..n];
    let k = chars_find_last(cut, ". ").max(chars_find_last(cut, "! ")).max(chars_find_last(cut, "? "));
    if k > 40 {
        cut[..(k + 1) as usize].iter().collect()
    } else {
        let s: String = cut.iter().collect();
        let head = match s.rfind(' ') {
            Some(i) => &s[..i],
            None => s.as_str(),
        };
        format!("{}...", head)
    }
}

// py: clan_ingame.py:81 _dow
/// A clan day (1..) as the day of its week.
fn dow(d: Option<&Value>) -> i64 {
    let d = match d {
        Some(Value::Number(_)) | Some(Value::Bool(_)) => int_of(d.unwrap_or(&Value::Null)),
        Some(Value::String(s)) => s.trim().parse::<i64>().unwrap_or(0),
        _ => 0,
    };
    if d > 0 {
        py::pymod(d - 1, 7) + 1
    } else {
        0
    }
}

fn dow_i(d: i64) -> i64 {
    if d > 0 {
        py::pymod(d - 1, 7) + 1
    } else {
        0
    }
}

// py: clan_ingame.py:89 _clan_row
fn clan_row(c: &Connection, tid: &str) -> Option<(i64, i64, i64)> {
    c.query_row("SELECT faction, status, day FROM clans WHERE token_id=?", [tid], |r| {
        Ok((r.get::<_, Option<i64>>(0)?.unwrap_or(0), r.get::<_, Option<i64>>(1)?.unwrap_or(0), r.get::<_, Option<i64>>(2)?.unwrap_or(0)))
    })
    .ok()
}

// py: clan_ingame.py:96 _last_result
fn last_result(c: &Connection, tid: &str) -> Value {
    let cl = match clan::load(c, tid) {
        Ok(x) => x,
        Err(_) => return json!({}),
    };
    let s = cl.get("last_result").filter(|v| truthy(v)).map(pystr).unwrap_or_else(|| "{}".to_string());
    match serde_json::from_str::<Value>(&s) {
        Ok(v) if truthy(&v) => v,
        _ => json!({}),
    }
}

fn lstr(lang: &str) -> &'static str {
    if lang == "pl" {
        "pl"
    } else {
        "en"
    }
}

fn obj_get<'a>(v: &'a Value, k: &str) -> Option<&'a Value> {
    v.get(k).filter(|x| truthy(x))
}

fn gint(v: Option<&Value>, d: i64) -> i64 {
    v.map(int_of).unwrap_or(d)
}

// py: clan_ingame.py:104 _header
fn header(row: (i64, i64, i64), camp: &Value, last: &Value, pubv: &Value, lang: &str) -> String {
    let (week, dw) = clan::week_day(row.2);
    let mo = obj_get(camp, "morale").cloned().unwrap_or_else(|| json!({}));
    let sup = obj_get(last, "sup").cloned().unwrap_or_else(|| json!({}));
    let mut days: Vec<i64> = Vec::new();
    if let Some(Value::Object(d)) = obj_get(&sup, "days") {
        for (k, v) in d {
            if clan_supply::CATS.contains(&k.as_str()) && obj_get(&sup, "need").and_then(|n| n.get(k)).map(truthy).unwrap_or(false) {
                days.push(int_of(v));
            }
        }
    }
    let worst = days.iter().copied().min().unwrap_or(-1);
    let fk = if row.0 == clan::SERAPHITES { "S" } else { "W" };
    let label = if lang == "pl" && mo.get("text").map(truthy).unwrap_or(false) { pystr(&mo["text"]) } else { mo.get("label").map(pystr).unwrap_or_default() };
    let cl = obj_get(pubv, "_clan").cloned().unwrap_or_else(|| json!({}));
    let alive = match camp.get("alive") {
        Some(v) => int_of(v),
        None => pubv.get("roster").and_then(|r| r.as_array()).map(|a| a.len() as i64).unwrap_or(0),
    };
    format!(
        "H\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
        week,
        dw,
        fk,
        gint(mo.get("value"), 50),
        t(&label.to_uppercase(), 24),
        alive,
        worst,
        row.2,
        gint(cl.get("won"), 0),
        gint(cl.get("lost"), 0),
        gint(cl.get("max"), 0)
    )
}

fn is_w(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// Python `re.I` single-char fold (sre lower of the character).
fn fold(c: char) -> char {
    let mut it = c.to_lowercase();
    let first = it.next().unwrap_or(c);
    first
}

/// `re.search(r"(?<!\w)NAME(?!\w)", text, re.I)`: the char index of the first hit.
fn find_word(text: &[char], name: &[char], ci_: bool, from: usize) -> Option<usize> {
    if name.len() > text.len() {
        return None;
    }
    for i in from..=text.len() - name.len() {
        if i > 0 && is_w(text[i - 1]) {
            continue;
        }
        let ok = (0..name.len()).all(|j| if ci_ { fold(text[i + j]) == fold(name[j]) } else { text[i + j] == name[j] });
        if !ok {
            continue;
        }
        if i + name.len() < text.len() && is_w(text[i + name.len()]) {
            continue;
        }
        return Some(i);
    }
    None
}

// py: clan_ingame.py:121 _sentence
/// A journal headline (upper case) as a sentence: lower case, the members' names with their capital.
fn sentence(head: &str, names: &[String]) -> String {
    let low = py::strip(head).to_lowercase();
    let mut it = low.chars();
    let mut tt: String = match it.next() {
        Some(c) => c.to_uppercase().collect::<String>() + it.as_str(),
        None => String::new(),
    };
    let mut sorted: Vec<&String> = names.iter().collect();
    sorted.sort_by(|a, b| b.chars().count().cmp(&a.chars().count()));
    for n in sorted {
        let nl: Vec<char> = n.to_lowercase().chars().collect();
        if nl.is_empty() {
            continue;
        }
        let text: Vec<char> = tt.chars().collect();
        let mut out = String::with_capacity(tt.len());
        let mut i = 0;
        let mut changed = false;
        while i < text.len() {
            match find_word(&text, &nl, false, i) {
                Some(p) => {
                    out.extend(&text[i..p]);
                    out.push_str(n);
                    i = p + nl.len();
                    changed = true;
                }
                None => {
                    out.extend(&text[i..]);
                    i = text.len();
                }
            }
        }
        if changed {
            tt = out;
        }
    }
    crate::clan::clan_continuity::restore_case(&tt, &crate::clan::clan_continuity::proper_nouns(&[]))
}

// py: clan_ingame.py:76 _split
fn split_arc(text: &str) -> (String, String) {
    match ARC_RE.captures(text) {
        Some(m) => (m.get(2).map(|x| x.as_str()).unwrap_or("").to_string(), m.get(3).map(|x| x.as_str()).unwrap_or("").to_string()),
        None => (String::new(), text.to_string()),
    }
}

// py: clan_ingame.py:137 _lately
/// LATELY of a member, always self-contained and about THEM.
fn lately(r: &Value, feed: &[Value], lang: &str, cur_day: i64, gender: &str, names: &[String]) -> String {
    let name = r.get("name").map(pystr).unwrap_or_default();
    let nm: Vec<char> = name.chars().collect();
    for m in feed {
        let (head, _b) = split_arc(&s_or(m.get("text")));
        if head.is_empty() {
            continue;
        }
        let hc: Vec<char> = head.chars().collect();
        if find_word(&hc, &nm, true, 0).is_none() {
            continue;
        }
        let d = m.get("day").filter(|x| truthy(x)).map(int_of).unwrap_or(0);
        if cur_day - d > LATELY_DAYS {
            break; // the newest one is old: nothing "lately"
        }
        return format!("{} {}: {}", if lang == "pl" { "DZIE\u{143}" } else { "DAY" }, dow_i(d), t(&sentence(&head, names), 140));
    }
    let code = match r.get("state").and_then(|s| s.as_str()).unwrap_or("") {
        "hungry" => 1,
        "sad" => 2,
        "sick" => 3,
        "injured" => 5,
        "depressed" => 8,
        "shock" => 9,
        _ => 0,
    };
    if code != 0 {
        return t(&status::story_line(&json!({"c": code, "n": name, "g": gender, "ln": ""}), lang), 140);
    }
    String::new()
}

fn word2(lang: &str, st: &str) -> Option<&'static str> {
    ci("STATE_WORD2").get(lang).and_then(|m| m.get(st)).and_then(|v| v.as_str())
}

// py: clan_ingame.py:158 _person_rows
fn person_rows(pubv: &Value, feed: &[Value], lang: &str, camp: &Value, cur_day: i64) -> R<Vec<String>> {
    let mut gender: HashMap<String, String> = HashMap::new();
    if let Some(a) = camp.get("people").and_then(|x| x.as_array()) {
        for p in a {
            gender.insert(p.get("name").map(pystr).unwrap_or_default(), p.get("gender").map(pystr).unwrap_or_else(|| "n".to_string()));
        }
    }
    let roster: Vec<Value> = pubv.get("roster").and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let mut names: Vec<String> = roster.iter().map(|p| p.get("name").map(pystr).unwrap_or_default()).collect();
    if let Some(a) = camp.get("dead").and_then(|x| x.as_array()) {
        names.extend(a.iter().map(|d| d.get("name").map(pystr).unwrap_or_default()));
    }
    let mut lines = Vec::new();
    for r in roster.iter().take(MAX_ROSTER) {
        let state = r.get("state").map(pystr).unwrap_or_else(|| "healthy".to_string());
        let code = tget("STATE_CODE", &state).unwrap_or("h");
        let pid = r.get("portrait").map(pystr).unwrap_or_default();
        let expr = if !pid.is_empty() { pid.rsplit('/').next().unwrap_or("").to_string() } else { "neutral".to_string() };
        let name = r.get("name").map(pystr).ok_or("KeyError: name")?;
        let lat = lately(r, feed, lang, cur_day, gender.get(&name).map(|s| s.as_str()).unwrap_or("n"), &names);
        let bw = s_or(r.get("bondLabel"));
        let role_v = match r.get("roleText") {
            Some(v) => pystr(v),
            None => r.get("role").map(pystr).unwrap_or_default(),
        };
        let word = word2(lang, r.get("state").map(pystr).unwrap_or_default().as_str()).map(|s| s.to_string()).unwrap_or_else(|| ci("STATE_WORD").get(lang).and_then(|m| m.get(code)).map(pystr).unwrap_or_default());
        lines.push(format!(
            "P\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            t(&name, 24),
            t(&role_v.to_uppercase(), 24),
            code,
            word,
            t(&expr, 12),
            t(&pid, 20),
            tv(r.get("bond"), 24),
            t(&bw, 16),
            tv(r.get("tagline"), 150),
            lat,
            tv(r.get("icon"), 12)
        ));
    }
    Ok(lines)
}

// py: clan_ingame.py:177 _body
/// A journal card body for the wire: up to 1500 chars cut at a sentence boundary; blank lines become spaces.
fn body(text: &str) -> String {
    let mut tt = t(text, 0);
    tt = first_sentences(&tt, 1500);
    while tt.len() > 1700 {
        let n = tt.chars().count();
        tt = first_sentences(&tt, 60.max(n.saturating_sub(120)));
    }
    tt
}

// py: clan_ingame.py:188 _first_seen
/// A first visit (no seen id yet): only the newest entry is NEW.
fn first_seen(ents: &[Value], seen_id: i64) -> i64 {
    if seen_id != 0 || ents.is_empty() {
        return seen_id;
    }
    ents[..ents.len() - 1].iter().map(|e| gint(e.get("id"), 0)).max().unwrap_or(0)
}

fn ev(e: &Value, k: &str) -> R<i64> {
    e.get(k).map(int_of).ok_or_else(|| format!("KeyError: {}", k))
}

fn paras_of(e: &Value) -> Vec<(String, String)> {
    e.get("paras")
        .and_then(|p| p.as_array())
        .map(|a| a.iter().map(|x| (x.get(0).map(pystr).unwrap_or_default(), x.get(1).map(pystr).unwrap_or_default())).collect())
        .unwrap_or_default()
}

// py: clan_ingame.py:196 _journal_day_rows
/// Section 2 from the day entries: one J row per clan day, newest first.
fn journal_day_rows(ents: &[Value], lang: &str, seen_id: i64) -> R<Vec<String>> {
    let seen_id = first_seen(ents, seen_id);
    let (wk, dy) = if lang == "pl" { ("TYDZIE\u{143}", "DZIE\u{143}") } else { ("WEEK", "DAY") };
    let mut out = Vec::new();
    let tail = &ents[ents.len().saturating_sub(MAX_JOURNAL)..];
    for e in tail.iter().rev() {
        let cap = if ev(e, "day")? > 0 { format!("{} {} - {} {}", wk, ev(e, "week")?, dy, ev(e, "dow")?) } else { format!("{} 1", wk) };
        let b = paras_of(e).iter().map(|(_, x)| x.clone()).collect::<Vec<_>>().join(" ");
        out.push(format!(
            "J\t{}\t{}\t{}\t{}\t{}\t\t0\t{}\t{}\t\t\t{}\t0",
            ev(e, "id")?,
            t(&cap, 60),
            t(&e.get("title").map(pystr).unwrap_or_default(), 90).to_uppercase(),
            body(&b),
            e.get("tone").map(pystr).unwrap_or_default(),
            ev(e, "dow")?,
            ev(e, "week")?,
            if ev(e, "id")? > seen_id { 1 } else { 0 }
        ));
    }
    Ok(out)
}

fn win_is(v: Option<&Value>, n: i64) -> bool {
    match v {
        Some(Value::Bool(b)) => (*b as i64) == n,
        Some(Value::Number(x)) => x.as_f64() == Some(n as f64),
        _ => false,
    }
}

// py: clan_ingame.py:208 _journal_week_rows
/// Section 6: the index of every entry (D) and the paragraphs (G) of one week.
fn journal_week_rows(ents: &[Value], seen_id: i64, week: i64) -> R<Vec<String>> {
    let seen_id = first_seen(ents, seen_id);
    let mut top = 1;
    if !ents.is_empty() {
        top = ents.iter().map(|e| gint(e.get("week"), 0)).max().unwrap_or(1);
    }
    let week = if week <= 0 || week > top { top } else { week };
    let mut out = vec![format!("K\t{}\t{}", week, top)];
    for e in ents {
        let win = if win_is(e.get("win"), 1) {
            "1"
        } else if win_is(e.get("win"), 0) {
            "0"
        } else {
            "-"
        };
        out.push(format!(
            "D\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            ev(e, "day")?,
            ev(e, "dow")?,
            ev(e, "week")?,
            ev(e, "id")?,
            t(&e.get("title").map(pystr).unwrap_or_default(), 70).to_uppercase(),
            e.get("tone").map(pystr).unwrap_or_default(),
            t(&e.get("map").map(pystr).unwrap_or_default(), 30).to_uppercase(),
            win,
            ev(e, "sup")?,
            ev(e, "need")?,
            ev(e, "h")?,
            ev(e, "u")?,
            ev(e, "s")?,
            ev(e, "dn")?,
            if ev(e, "id")? > seen_id { 1 } else { 0 }
        ));
    }
    for e in ents {
        if ev(e, "week")? != week {
            continue;
        }
        for (cap, text) in paras_of(e) {
            out.push(format!("G\t{}\t{}\t{}", ev(e, "day")?, t(&cap, 40), t(&text, 700)));
        }
    }
    Ok(out)
}

fn tuple_of(m: &Value) -> (i64, i64) {
    let g = |k: &str| m.get(k).filter(|x| truthy(x)).map(int_of).unwrap_or(0);
    (g("week"), g("day"))
}

// py: clan_ingame.py:229 _journal_rows
fn journal_rows(pubv: &Value, feed: &[Value], lang: &str, seen_id: i64) -> R<Vec<String>> {
    let mut seen_id = seen_id;
    if seen_id == 0 && !feed.is_empty() {
        // a first visit: only the newest day's lines are NEW
        let top = feed.iter().map(tuple_of).max().unwrap_or((0, 0));
        let older: Vec<i64> = feed.iter().filter(|m| tuple_of(m) < top).map(|m| m.get("id").filter(|x| truthy(x)).map(int_of).unwrap_or(0)).collect();
        seen_id = older.into_iter().max().unwrap_or(0);
    }
    let roster: Vec<Value> = pubv.get("roster").and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let mut ppl: HashMap<String, &Value> = HashMap::new();
    for p in &roster {
        ppl.insert(p.get("name").map(pystr).unwrap_or_default(), p);
    }
    let mut out = Vec::new();
    let jr: Vec<Value> = if !feed.is_empty() && feed[0].get("card").is_none() { cs::story_rows(feed) } else { feed.to_vec() };
    // an entry = 3..6 events, ONE author (two when the lead event names two), authors rotate; nothing is dropped
    for e in extp::journal_entries::compose_entries(&jr, &roster, seen_id).iter().take(MAX_JOURNAL) {
        let m = e.get("prim").ok_or("KeyError: prim")?;
        let style = s_or(m.get("style"));
        let arc = t(&{
            let a = s_or(m.get("arc"));
            if !a.is_empty() {
                a
            } else {
                s_or(m.get("thread"))
            }
        }, 0);
        let is_arc = !arc.is_empty();
        let step = if is_arc {
            let st = m.get("step").filter(|x| truthy(x)).or_else(|| m.get("tstep").filter(|x| truthy(x)));
            st.map(int_of).unwrap_or(1)
        } else {
            0
        };
        let head = e.get("head").map(pystr).unwrap_or_default();
        let bd = e.get("body").map(pystr).unwrap_or_default();
        let kind = style.replace("arc_", "");
        let mut cap;
        if is_arc {
            let mut tag = tget("ARC_TAG", &kind).map(|s| s.to_string()).unwrap_or_default();
            if tag.is_empty() {
                tag = tget("SAGA_TAG", &style).map(|s| s.to_string()).unwrap_or_default();
            }
            if tag.is_empty() {
                let a0 = arc.split('-').next().unwrap_or("");
                tag = tget("DIARY_TAG", a0).map(|s| s.to_string()).unwrap_or_default();
            }
            cap = format!("STORY - {}DAY {} - STEP {}", if tag.is_empty() { String::new() } else { format!("{} - ", tag) }, dow(m.get("day")), step);
        } else {
            let c = if !style.is_empty() { tget("CAP", &style).unwrap_or("CAMP LOG") } else { "CAMP LOG" };
            cap = format!("{} - DAY {}", c, dow(m.get("day")));
        }
        if lang == "pl" {
            cap = cap
                .split(" - ")
                .map(|p| {
                    let p = py::strip(p);
                    match tget("CAP_PL", p) {
                        Some(x) => x.to_string(),
                        None => p.replace("DAY", "DZIE\u{143}").replace("STEP", "KROK"),
                    }
                })
                .collect::<Vec<_>>()
                .join(" - ");
        }
        let tone = if !style.is_empty() {
            tget("TONE", &style).unwrap_or("").to_string()
        } else {
            tget("MOOD_TONE", &s_or(m.get("mood"))).unwrap_or("").to_string()
        };
        let au: Vec<&Value> = e.get("authors").and_then(|a| a.as_array()).map(|a| a.iter().filter_map(|n| ppl.get(&pystr(n)).copied()).collect()).unwrap_or_default();
        let first = au.first();
        let who = au.iter().map(|p| p.get("name").map(pystr).unwrap_or_default()).collect::<Vec<_>>().join(" & ");
        out.push(format!(
            "J\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t0",
            e.get("id").map(pystr).unwrap_or_else(|| "None".into()),
            t(&cap, 60),
            t(&head, 90).to_uppercase(),
            body(&bd),
            tone,
            arc,
            step,
            dow(m.get("day")),
            pystr(m.get("week").unwrap_or(&Value::Null)),
            t(&first.map(|f| s_none(f.get("portrait"))).unwrap_or_default(), 20),
            t(&who, 24),
            gint(e.get("new"), 0)
        ));
    }
    Ok(out)
}

// py: clan_ingame.py:274 _status_rows
fn status_rows(pubv: &Value, lang: &str) -> Vec<String> {
    let roster: Vec<Value> = pubv.get("roster").and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let mut ppl: HashMap<String, &Value> = HashMap::new();
    for p in &roster {
        ppl.insert(p.get("name").map(pystr).unwrap_or_default(), p);
    }
    let mut out = Vec::new();
    let st: Vec<Value> = pubv.get("status").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let tail = &st[st.len().saturating_sub(4)..];
    for e in tail.iter().rev() {
        let state = e.get("state").map(pystr).unwrap_or_else(|| "healthy".to_string());
        let expr = e.get("expr").map(pystr).unwrap_or_else(|| "neutral".to_string());
        let pid = e.get("img").filter(|x| truthy(x)).and_then(|i| i.get(&expr)).map(pystr).unwrap_or_default();
        let name = e.get("name").map(pystr).unwrap_or_default();
        let role_t = ppl.get(&name).and_then(|p| p.get("roleText")).filter(|x| truthy(x)).map(pystr);
        let role = role_t.unwrap_or_else(|| e.get("role").map(pystr).unwrap_or_default());
        out.push(format!(
            "U\t{}\t{}\t{}\t{}\t{}\t{}",
            t(&name, 24),
            t(&role.to_uppercase(), 24),
            state,
            word2(lang, &state).map(|s| s.to_string()).unwrap_or_else(|| state.to_uppercase()),
            t(&pid, 20),
            tv(e.get("line"), 150)
        ));
    }
    out
}

fn cat_vals(v: Option<&Value>) -> Vec<i64> {
    clan_supply::CATS.iter().map(|k| v.and_then(|x| x.get(*k)).filter(|x| truthy(x)).map(int_of).unwrap_or(0)).collect()
}

// py: clan_ingame.py:284 _raid_rows
fn raid_rows(pubv: &Value, last: &Value, lang: &str, seen_day: i64) -> R<Vec<String>> {
    let matches: Vec<Value> = pubv.get("matches").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let mut seen_day = seen_day;
    if seen_day == 0 && !matches.is_empty() {
        seen_day = matches.iter().map(|m| gint(m.get("d"), 0)).max().unwrap_or(0) - 1; // a first visit: only the newest raid is NEW
    }
    let mut out = Vec::new();
    let sup = obj_get(last, "sup").cloned().unwrap_or_else(|| json!({}));
    let stock = obj_get(&sup, "stock").cloned().unwrap_or_else(|| json!({}));
    let any_stock = stock.as_object().map(|o| o.values().any(|v| int_of(v) != 0)).unwrap_or(false);
    let nodata = matches.is_empty() && !any_stock;
    let need_o = obj_get(&sup, "need").cloned().unwrap_or_else(|| json!({}));
    let days_o = obj_get(&sup, "days").cloned().unwrap_or_else(|| json!({}));
    if obj_get(&sup, "need").is_some() && !nodata {
        for (i, k) in clan_supply::CATS.iter().enumerate() {
            let need = need_o.get(*k).map(int_of).unwrap_or(0);
            let label = ci("CAT_LABEL").get(lang).and_then(|a| a.get(i)).map(pystr).unwrap_or_default();
            out.push(format!("S\t{}\t{}\t{}\t{}", label, gint(stock.get(*k), 0), if need != 0 { gint(days_o.get(*k), 0) } else { -1 }, need));
        }
    }
    let mut loot: Vec<String> = Vec::new();
    for m in matches.iter().take(MAX_MATCHES) {
        let cat = obj_get(m, "cat").cloned().unwrap_or_else(|| json!({}));
        let got = cat_vals(cat.get("got").filter(|x| truthy(x)));
        let need = cat_vals(cat.get("need").filter(|x| truthy(x)));
        let win = m.get("win");
        let new = if gint(m.get("d"), 0) > seen_day { 1 } else { 0 };
        let wtxt = match win {
            None | Some(Value::Null) => "-",
            Some(w) if truthy(w) => "1",
            _ => "0",
        };
        let d = pystr(m.get("d").unwrap_or(&Value::Null));
        out.push(format!(
            "M\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            d,
            tv(m.get("map"), 30),
            wtxt,
            m.get("sup").map(pystr).unwrap_or_else(|| "0".into()),
            m.get("need").map(pystr).unwrap_or_else(|| "0".into()),
            new,
            got.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(" "),
            need.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(" ")
        ));
        let finds: Vec<Value> = m.get("finds").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
        for f in finds.iter().take(6) {
            loot.push(format!(
                "T\t{}\t{}\t{}\t{}\t{}",
                d,
                f.get("n").map(pystr).unwrap_or_else(|| "1".into()),
                t(&f.get("item").map(pystr).unwrap_or_default(), 60),
                icon(&s_or(f.get("icon"))),
                new
            ));
        }
        if finds.is_empty() {
            let lt: Vec<Value> = m.get("loot").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
            for kn in lt.iter().take(6) {
                let k = kn.get(0).map(pystr).unwrap_or_default();
                let n = kn.get(1).cloned().unwrap_or(Value::Null);
                loot.push(format!(
                    "T\t{}\t{}\t{}\t{}\t{}",
                    d,
                    pystr(&n),
                    t(&ch::loot_phrase(&k, int_of(&n), lang)?, 60),
                    icon(if ch::is_keep(&k) { "keepsake" } else { &k }),
                    new
                ));
            }
        }
    }
    out.extend(loot);
    Ok(out)
}

// py: clan_ingame.py:308 stats_roster
/// The players of a finished match (daemon, on the host's 0x12): rows = [(token, name, team, left)], winner = 0 / 1 team or 2 for a draw.
pub fn stats_roster(c: &Connection, mid: &str, mapname: &str, rows: &[(String, String, i64, bool)], winner: i64) -> rusqlite::Result<()> {
    if mid.is_empty() {
        return Ok(());
    }
    c.execute(STATS_SCHEMA, [])?;
    let now = crate::clock::now();
    for (tok, name, team, left) in rows {
        let win: i64 = if winner != 0 && winner != 1 {
            -1
        } else if *team == winner {
            1
        } else {
            0
        };
        c.execute(
            "INSERT INTO match_stats(mid, token, name, team, win, map, left, ts) VALUES(?,?,?,?,?,?,?,?) ON CONFLICT(mid, token) DO UPDATE SET \
             name=excluded.name, team=excluded.team, win=excluded.win, map=excluded.map, left=excluded.left",
            params![mid, tok, t(name, 24), team, win, t(mapname, 30), if *left { 1 } else { 0 }, now],
        )?;
    }
    c.execute("DELETE FROM match_stats WHERE ts < ?", [now - STATS_KEEP_S])?;
    Ok(())
}

// py: clan_ingame.py:322 stats_report
/// One player's own award counts of the match (op 0xE2 labels: 1 kills / 2 executions / 3 revives / 4 headshots / 21 downs / 22 deaths).
pub fn stats_report(c: &Connection, mid: &str, token: &str, name: &str, team: i64, counts: Option<&Counts>, left: bool) -> rusqlite::Result<()> {
    if mid.is_empty() {
        return Ok(());
    }
    c.execute(STATS_SCHEMA, [])?;
    let empty = Counts::new();
    let cnt = counts.unwrap_or(&empty);
    let v = |lab: i64, always: bool| -> i64 {
        if cnt.contains_key(&lab) || (!cnt.is_empty() && always) {
            cnt.get(&lab).copied().unwrap_or(0)
        } else {
            -1
        }
    };
    let now = crate::clock::now();
    c.execute(
        "INSERT INTO match_stats(mid, token, name, team, kills, downs, deaths, revives, execs, heads, left, ts) VALUES(?,?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(mid, token) DO UPDATE SET \
         name=excluded.name, team=CASE WHEN excluded.team >= 0 THEN excluded.team ELSE team END, kills=excluded.kills, downs=excluded.downs, deaths=excluded.deaths, \
         revives=excluded.revives, execs=excluded.execs, heads=excluded.heads, left=excluded.left",
        params![mid, token, t(name, 24), team, v(1, true), v(21, false), v(22, false), v(3, true), v(2, true), v(4, true), if left { 1 } else { 0 }, now],
    )?;
    Ok(())
}

// py: clan_ingame.py:332 stats_parts
/// The parts the player earned in the match (booked by clan_finalize after the late-join bonus). Errors are swallowed.
pub fn stats_parts(c: &Connection, mid: &str, token: &str, parts: i64) {
    let _ = c.execute("UPDATE match_stats SET parts=? WHERE mid=? AND token=?", params![parts, mid, token]);
}

/// A head of the pool: (model id, sex, faction).
pub type Head = (i64, String, String);

static HEAD_POOL: LazyLock<Vec<Head>> = LazyLock::new(|| {
    let pool = data::t("clan_portrait_pool", "POOL").as_array().cloned().unwrap_or_default();
    let ex = data::t("clan_portrait_pool", "EXPR");
    let mut out = Vec::new();
    for r in pool {
        let age = r.get(4).and_then(|x| x.as_str()).unwrap_or("");
        if age != "adult" && age != "elder" {
            continue;
        }
        let id = r.get(0).and_then(|x| x.as_i64()).unwrap_or(0);
        let have: Vec<&str> = data::dict_get_int(ex, id).and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|e| e.as_str()).collect()).unwrap_or_default();
        if MATCH_EXPR.iter().all(|e| have.contains(e)) {
            out.push((id, r.get(3).map(pystr).unwrap_or_default(), r.get(2).map(pystr).unwrap_or_default()));
        }
    }
    out
});

// py: clan_ingame.py:343 stats_head_pool
/// [(model id, sex, faction)] of the grown-up heads that have every MATCH_EXPR render.
pub fn stats_head_pool() -> &'static Vec<Head> {
    &HEAD_POOL
}

// py: clan_ingame.py:354 stats_heads
/// {token: head model id} for the players of one match: a stable face per player token, team 0 wears WLF heads and team 1 Seraphite ones, no two
/// players of the match share a head while the pool has free ones.
pub fn stats_heads(rows: &[(String, i64)]) -> HashMap<String, i64> {
    let pool = stats_head_pool();
    let mut used: HashSet<i64> = HashSet::new();
    let mut out = HashMap::new();
    if pool.is_empty() {
        return out;
    }
    let mut sorted: Vec<&(String, i64)> = rows.iter().collect();
    sorted.sort();
    for (tok, team) in sorted {
        let h = t2r_clanstory::crc::crc32(format!("head|{}", tok).as_bytes());
        let sex = if h & 1 != 0 { "f" } else { "m" };
        let fk = if *team == 1 { "S" } else { "W" };
        let side: Vec<(i64, &str)> = pool.iter().filter(|(_, _, fc)| fc == fk).map(|(m, sx, _)| (*m, sx.as_str())).collect();
        if side.is_empty() {
            continue;
        }
        let mut cand: Vec<i64> = side.iter().filter(|(m, sx)| *sx == sex && !used.contains(m)).map(|(m, _)| *m).collect();
        if cand.is_empty() {
            cand = side.iter().filter(|(m, _)| !used.contains(m)).map(|(m, _)| *m).collect();
        }
        if cand.is_empty() {
            cand = side.iter().filter(|(_, sx)| *sx == sex).map(|(m, _)| *m).collect();
        }
        if cand.is_empty() {
            cand = side.iter().map(|(m, _)| *m).collect();
        }
        let pick = cand[((h >> 12) as usize) % cand.len()];
        out.insert(tok.clone(), pick);
        used.insert(pick);
    }
    out
}

// py: clan_ingame.py:376 player_rank
/// (symbol 0..9, weeks 0..999) of the player behind a daemon token: the clan row of his Steam account ("s:<id>") else the install token's; (0, 0) when unknown.
pub fn player_rank(c: &Connection, token: &str) -> (i64, i64) {
    let sid: i64 = match c.query_row("SELECT steamid FROM players WHERE token=?", [token], |r| r.get::<_, rusqlite::types::Value>(0)) {
        Ok(rusqlite::types::Value::Integer(i)) => i,
        Ok(rusqlite::types::Value::Real(f)) => f as i64,
        Ok(rusqlite::types::Value::Text(s)) => s.trim().parse().unwrap_or(0),
        Ok(_) => 0,
        Err(rusqlite::Error::QueryReturnedNoRows) => 0,
        Err(_) => return (0, 0),
    };
    let mut keys: Vec<String> = Vec::new();
    if sid != 0 {
        keys.push(format!("s:{}", sid));
    }
    keys.push(clan::token_id(token));
    for key in keys {
        match c.query_row("SELECT 1 FROM clans WHERE token_id=?", [&key], |_| Ok(())) {
            Ok(()) => return clan::load(c, &key).map(|cl| clan::rank_of(&cl)).unwrap_or((0, 0)),
            Err(rusqlite::Error::QueryReturnedNoRows) => {}
            Err(_) => return (0, 0),
        }
    }
    (0, 0)
}

type StatRow = (Option<String>, i64, i64, i64, i64, i64, i64, i64, i64, i64, i64, String);

// py: clan_ingame.py:389 _stats_rows
fn stats_rows(c: &Connection, pubv: &Value, day: i64, token: Option<&str>) -> R<Vec<String>> {
    let matches: Vec<Value> = pubv.get("matches").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let m = matches.iter().find(|x| gint(x.get("d"), 0) == day);
    let mid = m.map(|x| s_or(x.get("mid"))).unwrap_or_default();
    let mut rows: Vec<StatRow> = Vec::new();
    if !mid.is_empty() {
        let q = c.prepare(
            "SELECT name, team, win, kills, downs, deaths, revives, execs, heads, parts, left, token FROM match_stats WHERE mid=? ORDER BY CASE WHEN team < 0 THEN 2 ELSE team END, parts DESC, kills DESC, name",
        );
        if let Ok(mut st) = q {
            let it = st.query_map([&mid], |r| {
                Ok((
                    r.get::<_, Option<String>>(0)?,
                    r.get::<_, i64>(1)?,
                    r.get::<_, i64>(2)?,
                    r.get::<_, i64>(3)?,
                    r.get::<_, i64>(4)?,
                    r.get::<_, i64>(5)?,
                    r.get::<_, i64>(6)?,
                    r.get::<_, i64>(7)?,
                    r.get::<_, i64>(8)?,
                    r.get::<_, i64>(9)?,
                    r.get::<_, i64>(10)?,
                    r.get::<_, String>(11)?,
                ))
            });
            match it {
                Ok(it) => {
                    for r in it {
                        rows.push(r.map_err(|e| e.to_string())?);
                    }
                }
                Err(_) => rows.clear(),
            }
        }
    }
    let mine = match token.filter(|x| !x.is_empty()) {
        Some(tk) => rows.iter().find(|r| r.11 == tk).map(|r| r.1).unwrap_or(-1),
        None => -1,
    };
    if mine >= 0 {
        rows.sort_by_key(|r| if r.1 == mine { 0 } else { 1 }); // the requester's own team first (stable)
    }
    let win = m.and_then(|x| x.get("win"));
    let wtxt = match win {
        None | Some(Value::Null) => "-",
        Some(w) if truthy(w) => "1",
        _ => "0",
    };
    let mut out = vec![format!("X\t{}\t{}\t{}\t{}", day, tv(m.and_then(|x| x.get("map")), 30), wtxt, rows.len().min(MAX_STATS))];
    let shown = &rows[..rows.len().min(MAX_STATS)];
    let heads = stats_heads(&shown.iter().map(|r| (r.11.clone(), r.1)).collect::<Vec<_>>());
    for r in shown {
        let (rk, wk) = player_rank(c, &r.11);
        let nm = r.0.clone().filter(|x| !x.is_empty()).unwrap_or_else(|| "?".to_string());
        out.push(format!(
            "Q\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}",
            t(&nm, 24),
            r.1,
            if token.map(|x| !x.is_empty() && r.11 == x).unwrap_or(false) { 1 } else { 0 },
            r.3,
            r.4,
            r.5,
            r.6,
            r.7,
            r.8,
            r.9,
            r.10,
            heads.get(&r.11).copied().unwrap_or(0),
            rk,
            wk
        ));
    }
    Ok(out)
}

// py: clan_ingame.py:415 build
/// The reply text of op 0xB6 for the clan `tid` ("" when it has no clan / no story / an unknown section). `Err` = where Python raises (the daemon logs it and sends "").
pub fn build(c: &Connection, tid: &str, lang: &str, section: i64, seen_id: i64, seen_day: i64, token: Option<&str>) -> R<String> {
    let lang = if pln::norm_lang(lang) == "pl" { "pl" } else { "en" };
    if !(1..=7).contains(&section) {
        return Ok(String::new());
    }
    let row = match clan_row(c, tid) {
        Some(r) if r.1 != 0 => r,
        _ => return Ok(String::new()),
    };
    let camp = cs::read_people(c, tid, 80, lang);
    let mut pubv = ch::public(c, tid, lang, 40)?;
    if !truthy(&pubv) {
        pubv = json!({});
    }
    let last = last_result(c, tid);
    // the clan's own numbers; nothing is added when anything is unreadable
    if let Ok(cl) = clan::load(c, tid) {
        let meta: Option<Value> = match cl.get("meta") {
            Some(Value::String(s)) => {
                let s = if s.is_empty() { "{}" } else { s.as_str() };
                serde_json::from_str::<Value>(s).ok()
            }
            _ => Some(json!({})),
        };
        if let Some(meta) = meta {
            let mx = meta.get("max").filter(|x| truthy(x)).map(int_of).unwrap_or(0);
            let rl = pubv.get("roster").filter(|x| truthy(x)).and_then(|x| x.as_array()).map(|a| a.len() as i64).unwrap_or(0);
            if let Some(o) = pubv.as_object_mut() {
                o.insert("_clan".into(), json!({"won": cl.get("runs_won").cloned().unwrap_or(json!(0)), "lost": cl.get("runs_lost").cloned().unwrap_or(json!(0)), "max": mx.max(rl)}));
            }
        }
    }
    let mut lines = vec![header(row, &camp, &last, &pubv, lang)];
    let mut ents: Vec<Value> = Vec::new();
    if section == 2 || section == 6 {
        ents = extp::journal::entries(c, tid, lang, Some(row.2)); // no journal table yet: the older rows below
    }
    if section == 6 {
        lines.extend(journal_week_rows(&ents, seen_id, seen_day)?);
    } else if section == 2 && !ents.is_empty() {
        lines.extend(journal_day_rows(&ents, lang, seen_id)?);
    } else if section == 1 || section == 2 {
        let feed = cs::read_feed(c, tid, if section == 2 { 100 } else { 40 }, 0, lang, section == 2, false, section == 2);
        if section == 1 {
            lines.extend(person_rows(&pubv, &feed, lang, &camp, row.2)?);
        } else {
            lines.extend(journal_rows(&pubv, &feed, lang, seen_id)?);
        }
    } else if section == 4 {
        lines.extend(status_rows(&pubv, lang));
    } else if section == 5 {
        lines.extend(stats_rows(c, &pubv, seen_day, token)?);
    } else if section == 7 {
        lines.extend(end_rows(c, tid, row.1)?);
    } else {
        lines.extend(raid_rows(&pubv, &last, lang, seen_day)?);
    }
    Ok(lines.join("\n"))
}

// py: clan_ingame.py:469 _end_rows
/// Section 7 (the clan end screens): E kind(W won / L lost) shown more remembered raidsWon raidsPlayed, then R name portraitId per face (<= 7).
fn end_rows(c: &Connection, tid: &str, status_v: i64) -> R<Vec<String>> {
    if status_v != clan::DEAD && status_v != clan::WON {
        return Ok(vec![]);
    }
    let k = if status_v == clan::WON { "W" } else { "L" };
    let Some(mut st) = cs::_load(c, tid) else { return Ok(vec![format!("E\t{}\t0\t0\t0\t0\t0", k)]) };
    let fk = if gint(st.get("fac"), 0) == clan::SERAPHITES { "S" } else { "W" };
    let people: Vec<Value> = st.get("people").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let dead: Vec<Value> = st.get("dead").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let log: Vec<Value> = ch::ch_state(&mut st).get("log").filter(|x| truthy(x)).and_then(|x| x.as_array()).cloned().unwrap_or_default();
    let won = log.iter().filter(|e| matches!(e.get("win"), Some(Value::Bool(true))) || win_is(e.get("win"), 1)).count();
    let mut members = people.clone();
    members.extend(dead.iter().cloned());
    let faces: Vec<(String, String)>;
    let more: usize;
    if status_v == clan::WON {
        faces = people.iter().take(ROLL).map(|p| (p.get("n").map(pystr).unwrap_or_default(), ch::portrait_id(p, fk, Some("h"), 80, ch::PORTRAIT_MODELS, Some(&members)))).collect();
        more = people.len().saturating_sub(ROLL);
    } else {
        faces = dead[dead.len().saturating_sub(ROLL)..].iter().map(|d| (d.get("n").map(pystr).unwrap_or_default(), ch::portrait_id(d, fk, Some("d"), 50, ch::PORTRAIT_MODELS, Some(&members)))).collect();
        more = 0;
    }
    let mut out = vec![["E".to_string(), k.to_string(), faces.len().to_string(), more.to_string(), dead.len().to_string(), won.to_string(), log.len().to_string()].join("\t")];
    for (n, pid) in &faces {
        out.push(["R".to_string(), t(n, 24), t(pid, 20)].join("\t"));
    }
    Ok(out)
}
