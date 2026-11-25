//! py: clan_journal_entries.py - in-game journal entries: regroups the rows of clan_story.story_rows() into entries of 3..6 consecutive events with ONE author
//! (two when the strongest event names exactly two members), authors rotating. Pure functions (no db).
use crate::clan::clan::{int_of, pystr, truthy};
use crate::clan::clan_story as cs;
use crate::py;
use regex::Regex;
use serde_json::{json, Map, Value};
use std::collections::HashMap;
use std::sync::LazyLock;

pub const MIN_EVENTS: usize = 3;
pub const MAX_EVENTS: usize = 6;
pub const BODY_CHARS: i64 = 1450;

static ARC: LazyLock<Regex> = LazyLock::new(|| Regex::new(r"^(?:DAY|DZIEŃ) (\d+): ([^.]*)\.\s*([\s\S]*)$").expect("ARC"));

fn text_of(m: &Value) -> String {
    match m.get("text") {
        Some(v) if truthy(v) => pystr(v),
        _ => String::new(),
    }
}

fn names_of(m: &Value) -> Vec<String> {
    m.get("names").and_then(|n| n.as_array()).map(|a| a.iter().map(pystr).collect()).unwrap_or_default()
}

fn isw(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// first start of `(?<!\w)name(?!\w)` in text (byte offset)
fn find_word(s: &str, name: &str) -> Option<usize> {
    for (i, _) in s.char_indices() {
        if s[i..].starts_with(name) {
            let before = s[..i].chars().next_back();
            let after = s[i + name.len()..].chars().next();
            if !before.map(isw).unwrap_or(false) && !after.map(isw).unwrap_or(false) {
                return Some(i);
            }
        }
    }
    if name.is_empty() && !s.is_empty() {
        return Some(0);
    }
    None
}

// py: clan_journal_entries.py:30 _named
/// Roster names that occur in text, in order of first occurrence.
fn named(text: &str, names: &[String]) -> Vec<String> {
    let mut hit: Vec<(usize, String)> = names.iter().filter_map(|n| find_word(text, n).map(|p| (p, n.clone()))).collect();
    hit.sort();
    hit.into_iter().map(|(_, n)| n).collect()
}

// py: clan_journal_entries.py:40 chunk
/// events oldest first (list of rows). Returns lists of 3..6 consecutive events; a day with fewer than 3 events carries into the next one.
pub fn chunk(events: &[Value]) -> Vec<Vec<Value>> {
    let mut out: Vec<Vec<Value>> = Vec::new();
    let mut pend: Vec<Value> = Vec::new();
    let mut days: Vec<((i64, i64), Vec<Value>)> = Vec::new();
    let g = |m: &Value, k: &str| m.get(k).filter(|v| truthy(v)).map(int_of).unwrap_or(0);
    for m in events {
        let k = (g(m, "week"), g(m, "day"));
        match days.last_mut() {
            Some(l) if l.0 == k => l.1.push(m.clone()),
            _ => days.push((k, vec![m.clone()])),
        }
    }
    let nd = days.len();
    for (i, (_, grp)) in days.into_iter().enumerate() {
        pend.extend(grp);
        if pend.len() < MIN_EVENTS && i + 1 < nd {
            continue;
        }
        let n = pend.len();
        let k = ((n + MAX_EVENTS - 1) / MAX_EVENTS).max(1);
        let (base, extra) = (n / k, n % k);
        let mut pos = 0;
        let mut piece: Vec<Vec<Value>> = Vec::new();
        for j in 0..k {
            let sz = base + if j < extra { 1 } else { 0 };
            piece.push(pend[pos..pos + sz].to_vec());
            pos += sz;
        }
        if k == 1 && n < MIN_EVENTS && !out.is_empty() && out.last().map_or(0, |l| l.len()) + n <= MAX_EVENTS {
            if let Some(l) = out.last_mut() {
                l.extend(pend.iter().cloned());
            }
        } else {
            out.extend(piece);
        }
        pend = Vec::new();
    }
    out
}

// py: clan_journal_entries.py:73 _primary
fn primary(ev: &[Value]) -> usize {
    let mut best = 0usize;
    let mut bk: Option<(i64, i64)> = None;
    for (i, m) in ev.iter().enumerate() {
        let k = (cs::_story_rank(m).unwrap_or(3), -(text_of(m).chars().count() as i64));
        if bk.map_or(true, |b| k < b) {
            bk = Some(k);
            best = i;
        }
    }
    best
}

// py: clan_journal_entries.py:140 _cut
fn cut(t: &str, n: usize) -> String {
    let t: String = t.split(py::is_py_space).filter(|s| !s.is_empty()).collect::<Vec<_>>().join(" ");
    if t.chars().count() <= n {
        return t;
    }
    let c: String = t.chars().take(n).collect();
    let k = [". ", "! ", "? "].iter().filter_map(|p| c.rfind(p)).max();
    match k {
        Some(k) if c[..k].chars().count() > 30 => c[..k + 1].to_string(),
        _ => {
            let head = match c.rfind(' ') {
                Some(p) => &c[..p],
                None => &c[..],
            };
            format!("{}...", head.trim_end_matches(|ch| matches!(ch, ',' | ';' | ':')))
        }
    }
}

struct Entry {
    events: Vec<Value>,
    prim: usize,
    authors: Vec<String>,
    score: Vec<(String, i64)>,
}

// py: clan_journal_entries.py:82 compose_entries
/// rows = clan_story.story_rows() output (read_feed shape, newest first); roster = [{"name", "portrait"}]. -> entries NEWEST first, every row in exactly one entry:
/// [{"prim", "events", "authors", "score", "head", "body", "id", "new"}].
pub fn compose_entries(rows: &[Value], roster: &[Value], seen_id: i64) -> Vec<Value> {
    let mut names: Vec<String> = Vec::new();
    for p in roster {
        let n = pystr(&p["name"]);
        if !names.contains(&n) {
            names.push(n);
        }
    }
    names.sort_by(|a, b| b.chars().count().cmp(&a.chars().count()));
    let mut events: Vec<Value> = rows.to_vec();
    events.sort_by_key(|m| int_of(&m["id"]));
    let mut entries: Vec<Entry> = Vec::new();
    let mut last_wrote: HashMap<String, i64> = HashMap::new();
    for (idx, ev) in chunk(&events).into_iter().enumerate() {
        let prim = primary(&ev);
        let mut score: Vec<(String, i64)> = Vec::new();
        for (mi, m) in ev.iter().enumerate() {
            for n in named(&text_of(m), &names) {
                let add = if mi == prim { 3 } else { 1 };
                match score.iter_mut().find(|(k, _)| *k == n) {
                    Some(s) => s.1 += add,
                    None => score.push((n, add)),
                }
            }
        }
        let lw = |n: &str| last_wrote.get(n).copied().unwrap_or(-99);
        let mut cand: Vec<&(String, i64)> = score.iter().collect();
        cand.sort_by(|a, b| (lw(&a.0), -a.1, &a.0).cmp(&(lw(&b.0), -b.1, &b.0)));
        let mut authors: Vec<String> = Vec::new();
        if let Some(c0) = cand.first() {
            authors = vec![c0.0.clone()];
            let ptxt = text_of(&ev[prim]);
            let first = ptxt.split('.').next().unwrap_or("");
            let pair = named(first, &names);
            if pair.len() == 2 && !pair.iter().any(|p| !score.iter().any(|(k, _)| k == p)) {
                authors = pair;
            }
        }
        for a in &authors {
            last_wrote.insert(a.clone(), idx as i64);
        }
        entries.push(Entry { events: ev, prim, authors, score });
    }
    let mut out: Vec<Value> = Vec::new();
    for e in entries {
        let ev = &e.events;
        let prim = &ev[e.prim];
        let ptext = text_of(prim);
        let m0 = ARC.captures(&ptext);
        let mut head = m0.as_ref().map(|m| m[2].to_string()).unwrap_or_default();
        let mut ptxt = match &m0 {
            Some(m) => m[3].to_string(),
            None => ptext.clone(),
        };
        if head.is_empty() {
            // a plain camp-log piece: its first short sentence is the title
            let ss = cs::_sentences(&ptxt, &names_of(prim));
            if ss.len() > 1 && ss[0].chars().count() <= 50 {
                head = ss[0].trim_end_matches(|ch| matches!(ch, '.' | '!' | '?')).to_string();
                ptxt = ss[1..].join(" ");
            }
        }
        let weights: Vec<i64> = ev.iter().map(|m| if m.get("card").map_or(false, truthy) { 2 } else { 1 }).collect();
        let tot: i64 = weights.iter().sum();
        let mut parts: Vec<String> = Vec::new();
        for (i, (m, w)) in ev.iter().zip(weights.iter()).enumerate() {
            let share = 90.max((BODY_CHARS as f64 * *w as f64 / tot as f64) as i64);
            let mut t = text_of(m);
            if i == e.prim {
                t = ptxt.clone();
            } else if ARC.is_match(&t) {
                t = cs::_sentences(&t, &names_of(m)).join(" ");
            }
            parts.push(cut(&t, share as usize));
        }
        let body = parts.into_iter().filter(|p| !p.is_empty()).collect::<Vec<_>>().join(" ");
        let id = ev.iter().map(|m| int_of(&m["id"])).max().unwrap_or(0);
        let mut sc = Map::new();
        for (k, v) in &e.score {
            sc.insert(k.clone(), Value::from(*v));
        }
        out.push(json!({"prim": prim.clone(), "events": ev.clone(), "authors": e.authors, "score": Value::Object(sc), "head": head, "body": body, "id": id, "new": if id > seen_id { 1 } else { 0 }}));
    }
    out.reverse();
    out
}
