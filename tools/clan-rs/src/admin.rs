//! The admin site's clan page (what `clanfeed_view.view(..., key=k)` answers on the daemon's modules): the journal feed with call-backs and story cards,
//! the camp roster summary (morale / people / alive / dead) and the chronicle (roster cards, match log, inventory, status cards). Read-only.
use crate::data::{data, Data};
use crate::story::{days_done, load_state, norm_lang, render_stored_pl, sql_val};
use crate::text::{clen, py_int, py_str, render_en, render_pl, role_word, truthy, widget_text, People, Person, Vars};
use regex::Regex;
use rusqlite::{params, Connection};
use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet};
use std::sync::OnceLock;

static RAW: &str = include_str!("../data/admin.json");
const FEED_MAX: i64 = 100; // the daemon runs with the A-Life and the chronicle on
const DAY_STORIES: usize = 2;
const LINE_MAX: usize = 96;
const STORY_MIN_CHARS: usize = 150;
const MINOR: [&str; 10] = ["saga_keepcb", "saga_use", "saga_lack", "kids", "quiet", "sick_watch", "saga_prep", "saga_keepfind", "saga_rel", "saga_prof"];
const STORY_ORDER: [&str; 8] = ["arc_", "saga_win", "saga_down", "saga_exec", "saga_rev", "saga_inj", "saga_grief", "saga_haul"];

fn adm() -> &'static Value {
    static A: OnceLock<Value> = OnceLock::new();
    A.get_or_init(|| serde_json::from_str(RAW).unwrap_or(Value::Null))
}

fn h(parts: &[String]) -> u32 {
    crc32fast::hash(parts.join("|").as_bytes())
}

fn st_(v: Option<&Value>) -> String {
    v.and_then(Value::as_str).unwrap_or("").to_string()
}

fn int_of(v: Option<&Value>) -> i64 {
    match v {
        Some(x) if truthy(Some(x)) => py_int(x).unwrap_or(0),
        _ => 0,
    }
}

fn w_char(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

// ------------------------------------------------------------------ the feed
type Row = Map<String, Value>;

fn feed_rows(c: &Connection, tid: &str, pl: bool, limit: i64, who: bool) -> Vec<Row> {
    use rusqlite::types::Value as S;
    let d = match data() {
        Some(d) => d,
        None => return Vec::new(),
    };
    type Raw = Vec<S>;
    let run = |sql: &str, n: usize| -> Option<Vec<Raw>> {
        let mut st = c.prepare(sql).ok()?;
        let rows = st.query_map(params![tid, limit], |r| Ok((0..n).map(|i| r.get::<_, S>(i).unwrap_or(S::Null)).collect::<Vec<S>>())).ok()?;
        rows.collect::<Result<Vec<_>, _>>().ok()
    };
    let raw = run("SELECT id,day,week,kind,mood,faction,text,names,ts,lid,params FROM clan_feed WHERE token_id=? AND id>0 ORDER BY id DESC LIMIT ?", 11)
        .or_else(|| run("SELECT id,day,week,kind,mood,faction,text,names,ts,NULL,NULL FROM clan_feed WHERE token_id=? AND id>0 ORDER BY id DESC LIMIT ?", 11))
        .unwrap_or_default();
    let done = days_done(c, tid);
    let txt = |v: &S| -> Option<String> {
        match v {
            S::Text(t) => Some(t.clone()),
            S::Blob(b) => Some(String::from_utf8_lossy(b).into_owned()),
            _ => None,
        }
    };
    let mut out = Vec::new();
    for r in raw {
        let lid = txt(&r[9]);
        let prm = txt(&r[10]);
        let mut text = sql_val(r[6].clone());
        let names: Value = serde_json::from_str(&txt(&r[7]).filter(|s| !s.is_empty()).unwrap_or_else(|| "[]".into())).unwrap_or(json!([]));
        let mut used = "en";
        if pl {
            if let (Some(l), Some(p)) = (&lid, &prm) {
                if !l.is_empty() && !p.is_empty() {
                    if let Ok(pv) = serde_json::from_str::<Value>(p) {
                        if let Some(t) = render_stored_pl(d, l, &pv) {
                            if !t.is_empty() {
                                text = Value::from(t);
                                used = "pl";
                            }
                        }
                    }
                }
            }
        }
        let mut m = Map::new();
        m.insert("id".into(), sql_val(r[0].clone()));
        for (k, i) in [("day", 1), ("week", 2), ("kind", 3), ("mood", 4), ("faction", 5)] {
            m.insert(k.into(), sql_val(r[i].clone()));
        }
        m.insert("text".into(), text);
        m.insert("names".into(), names);
        m.insert("ts".into(), sql_val(r[8].clone()));
        m.insert("lang".into(), json!(used));
        let lids = lid.clone().unwrap_or_default();
        let params_o = || -> Map<String, Value> {
            let p = prm.clone().filter(|s| !s.is_empty()).unwrap_or_else(|| "{}".into());
            match serde_json::from_str::<Value>(&p) {
                Ok(Value::Object(o)) => o,
                _ => Map::new(),
            }
        };
        if who && prm.as_deref().map_or(false, |p| !p.is_empty()) && !["ch/", "sg/", "arc/", "pk/", "al/", "cn/"].iter().any(|p| lids.starts_with(p)) {
            if let Some(Value::Object(pp)) = params_o().get("p") {
                let mut w = Map::new();
                let mut ok = true;
                for (k, v) in pp {
                    if k == "A" || k == "D" {
                        match v.as_array().and_then(|a| a.first()) {
                            Some(x) => {
                                w.insert(k.clone(), x.clone());
                            }
                            None => ok = false,
                        }
                    }
                }
                if ok {
                    m.insert("who".into(), Value::Object(w));
                }
            }
        }
        if let Some(rest) = lids.strip_prefix("ch/") {
            m.insert("style".into(), json!(rest.split('+').next().unwrap_or("").split('~').next().unwrap_or("")));
        } else if lids.starts_with("cn/") {
            m.insert("style".into(), json!("saga_recap"));
        } else if lids.starts_with("pk/") {
            m.insert("style".into(), json!("perk"));
        } else if let Some(rest) = lids.strip_prefix("sg/") {
            m.insert("style".into(), json!(format!("saga_{}", rest)));
            let pj = params_o();
            if let Some(a) = pj.get("a").filter(|v| truthy(Some(v))) {
                m.insert("arc".into(), a.clone());
            }
            if let Some(map) = pj.get("s").and_then(Value::as_object).and_then(|s| s.get("map")).filter(|v| truthy(Some(v))) {
                m.insert("map".into(), map.clone());
            }
        } else if let Some(rest) = lids.strip_prefix("arc/") {
            m.insert("style".into(), json!(format!("arc_{}", rest.split('.').next().unwrap_or(""))));
            let pj = params_o();
            m.insert("arc".into(), pj.get("a").cloned().unwrap_or(Value::Null));
            m.insert("step".into(), pj.get("n").cloned().unwrap_or(Value::Null));
        }
        out.push(m);
    }
    if let Some(done) = done {
        out.retain(|m| int_of(m.get("day")) <= done);
    }
    out
}

fn map_pl(name: &str) -> String {
    data().and_then(|d| d.chron.map_pl.get(name).cloned()).unwrap_or_else(|| name.to_string())
}

fn tail(kind: &str, role: &str, name: &str, a: i64, b: i64, lang: &str) -> String {
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
    format!("{} died {}.", name, if n == 1 { "yesterday".to_string() } else { format!("{} days ago", n) })
}

fn match_won(style: &str) -> Option<Option<bool>> {
    match style {
        "saga_win" => Some(Some(true)),
        "saga_loss" => Some(Some(false)),
        "saga_down" | "saga_prep" => Some(None),
        _ => None,
    }
}

struct Thr {
    day: i64,
    thread: String,
    last: i64,
    step: i64,
}

fn text_of(m: &Row) -> String {
    m.get("text").map(py_str).unwrap_or_default()
}

fn set_text(m: &mut Row, t: String) {
    m.insert("text".into(), Value::from(t));
}

/// clan_continuity.annotate: call-back tails and thread ids on the diary rows (rows newest first; changed in place).
fn annotate(rows: &mut [Row], dead: &[Value], sick_since: &HashMap<String, i64>, lang_req: &str) {
    let mut death_day: HashMap<String, i64> = HashMap::new();
    for dd in dead {
        death_day.insert(py_str(dd.get("n").unwrap_or(&Value::Null)), dd.get("d").and_then(py_int).unwrap_or(0));
    }
    let mut order: Vec<usize> = (0..rows.len()).collect();
    order.sort_by_key(|&i| rows[i].get("id").and_then(py_int).unwrap_or(0));
    let mut open_ill: HashMap<String, Thr> = HashMap::new();
    let mut open_mourn: HashMap<String, Thr> = HashMap::new();
    let mut firsts: HashMap<usize, String> = HashMap::new();
    let mut last_map: HashMap<String, usize> = HashMap::new();
    let mut last_haul: Option<usize> = None;
    for &ix in &order {
        let kind = st_(rows[ix].get("kind"));
        let day = int_of(rows[ix].get("day"));
        let wv: Map<String, Value> = rows[ix].get("who").and_then(Value::as_object).cloned().unwrap_or_default();
        let w = |k: &str| -> Option<String> { wv.get(k).filter(|v| truthy(Some(v))).map(py_str) };
        let sty = st_(rows[ix].get("style"));
        let lang_s = st_(rows[ix].get("lang"));
        let lang = if lang_s.is_empty() { lang_req.to_string() } else { lang_s };
        let mapname = rows[ix].get("map").filter(|v| truthy(Some(v))).map(py_str);
        if match_won(&sty).is_some() && mapname.is_some() {
            let mp = mapname.clone().unwrap_or_default();
            if let Some(&pv) = last_map.get(&mp) {
                if int_of(rows[pv].get("day")) < day {
                    let pstyle = st_(rows[pv].get("style"));
                    let pd = int_of(rows[pv].get("day"));
                    let won = match_won(&pstyle).flatten();
                    let tl = if lang == "pl" {
                        format!(" Poprzednio ({}): dzień {}{}.", map_pl(&mp), pd, match won { None => "", Some(true) => ", wygrana", Some(false) => ", porażka" })
                    } else {
                        format!(" Last time on {}: day {}{}.", mp, pd, match won { None => "", Some(true) => ", won", Some(false) => ", lost" })
                    };
                    let t = text_of(&rows[ix]).trim_end().to_string() + &tl;
                    set_text(&mut rows[ix], t);
                    let thread = format!("raid-{}", mp);
                    let pstep = rows[pv].get("tstep").cloned().unwrap_or(json!(1));
                    let pid = rows[pv].get("id").cloned().unwrap_or(Value::Null);
                    rows[ix].insert("thread".into(), json!(thread));
                    rows[ix].insert("prev".into(), pid);
                    rows[ix].insert("tstep".into(), json!(py_int(&pstep).unwrap_or(1) + 1));
                    if !rows[pv].contains_key("thread") {
                        rows[pv].insert("thread".into(), json!(thread));
                        rows[pv].insert("tstep".into(), json!(1));
                        rows[pv].insert("prev".into(), json!(0));
                    }
                }
            }
            last_map.insert(mp, ix);
            continue;
        }
        if sty == "saga_haul" {
            if let Some(lh) = last_haul {
                if int_of(rows[lh].get("day")) < day && rows[lh].get("week") == rows[ix].get("week") {
                    let ld = int_of(rows[lh].get("day"));
                    let tl = if lang == "pl" { format!(" Poprzedni łup: dzień {}.", ld) } else { format!(" The haul before this one came in on day {}.", ld) };
                    let t = text_of(&rows[ix]).trim_end().to_string() + &tl;
                    set_text(&mut rows[ix], t);
                    let wk = rows[ix].get("week").map(|v| if v.is_null() { "None".to_string() } else { py_str(v) }).unwrap_or_else(|| "None".into());
                    let thread = format!("haul-w{}", wk);
                    let pstep = rows[lh].get("tstep").cloned().unwrap_or(json!(1));
                    let pid = rows[lh].get("id").cloned().unwrap_or(Value::Null);
                    rows[ix].insert("thread".into(), json!(thread));
                    rows[ix].insert("prev".into(), pid);
                    rows[ix].insert("tstep".into(), json!(py_int(&pstep).unwrap_or(1) + 1));
                    if !rows[lh].contains_key("thread") {
                        rows[lh].insert("thread".into(), json!(thread));
                        rows[lh].insert("tstep".into(), json!(1));
                        rows[lh].insert("prev".into(), json!(0));
                    }
                }
            }
            last_haul = Some(ix);
            continue;
        }
        if !sty.is_empty() {
            continue;
        }
        let id_i = rows[ix].get("id").and_then(py_int).unwrap_or(0);
        if kind == "sick" {
            if let Some(a) = w("A") {
                let th = format!("ill-{}-{}", a, day);
                firsts.insert(ix, th.clone());
                open_ill.insert(a, Thr { day, thread: th, last: id_i, step: 1 });
                continue;
            }
        }
        if ["worse", "medicine", "recovery", "death_illness"].contains(&kind.as_str()) {
            if let Some(name) = w("A") {
                if !open_ill.contains_key(&name) {
                    if let Some(&s) = sick_since.get(&name) {
                        if s < day {
                            open_ill.insert(name.clone(), Thr { day: s, thread: format!("ill-{}-{}", name, s), last: 0, step: 0 });
                        }
                    }
                }
                let Some(t) = open_ill.get_mut(&name) else { continue };
                let ln = tail(&kind, "ill", &name, t.day, day, &lang);
                let cur = text_of(&rows[ix]);
                if !ln.is_empty() && !cur.contains(&format!("day {}", t.day)) && !cur.contains(&format!("dnia {}", t.day)) {
                    set_text(&mut rows[ix], cur.trim_end().to_string() + " " + &ln);
                }
                rows[ix].insert("thread".into(), json!(t.thread));
                rows[ix].insert("prev".into(), json!(t.last));
                rows[ix].insert("tstep".into(), json!(t.step + 1));
                t.last = id_i;
                t.step += 1;
                if kind == "recovery" || kind == "death_illness" {
                    open_ill.remove(&name);
                }
                continue;
            }
        }
        if kind.starts_with("death_") || kind.starts_with("loss") {
            let names: Vec<Value> = rows[ix].get("names").and_then(Value::as_array).cloned().unwrap_or_default();
            for nm in names {
                let n = py_str(&nm);
                if death_day.get(&n) == Some(&day) {
                    let th = format!("mourn-{}", n);
                    firsts.insert(ix, th.clone());
                    open_mourn.insert(n, Thr { day, thread: th, last: id_i, step: 1 });
                    break;
                }
            }
            continue;
        }
        if ["grief", "hope_after"].contains(&kind.as_str()) {
            if let Some(name) = w("D") {
                let Some(&dd) = death_day.get(&name) else { continue };
                if dd >= day {
                    continue;
                }
                let t = open_mourn.entry(name.clone()).or_insert_with(|| Thr { day: dd, thread: format!("mourn-{}", name), last: 0, step: 0 });
                let ln = tail(&kind, "mourn", &name, dd, day, &lang);
                let cur = text_of(&rows[ix]);
                if !ln.is_empty() && !cur.contains(&format!("day {}", dd)) && !cur.contains(&format!("dnia {}", dd)) {
                    set_text(&mut rows[ix], cur.trim_end().to_string() + " " + &ln);
                }
                rows[ix].insert("thread".into(), json!(t.thread));
                rows[ix].insert("prev".into(), json!(t.last));
                rows[ix].insert("tstep".into(), json!(t.step + 1));
                t.last = id_i;
                t.step += 1;
            }
        }
    }
    let threads: HashSet<String> = rows.iter().filter_map(|m| m.get("thread").filter(|v| truthy(Some(v))).map(py_str)).collect();
    for &ix in &order {
        if let Some(th) = firsts.get(&ix) {
            if threads.contains(th) {
                rows[ix].insert("thread".into(), json!(th));
                rows[ix].insert("tstep".into(), json!(1));
                rows[ix].insert("prev".into(), json!(0));
            }
        }
    }
    let ids: HashSet<i64> = rows.iter().filter_map(|m| m.get("id").and_then(py_int)).collect();
    for m in rows.iter_mut() {
        if m.get("thread").map_or(false, |v| truthy(Some(v))) && m.get("prev").map_or(false, |v| truthy(Some(v))) {
            let p = m.get("prev").and_then(py_int).unwrap_or(0);
            if !ids.contains(&p) {
                m.insert("prev".into(), json!(0));
            }
        }
    }
}

// ------------------------------------------------------------------ story cards
fn sentences(t: &str, names: &[String]) -> Vec<String> {
    static HEAD: OnceLock<Option<Regex>> = OnceLock::new();
    static HNUM: OnceLock<Option<Regex>> = OnceLock::new();
    let head = HEAD.get_or_init(|| Regex::new(r"^(?:DAY|DZIEŃ) \d+: [^.]*\.\s*").ok()).as_ref();
    let hnum = HNUM.get_or_init(|| Regex::new(r"^(?:DAY|DZIEŃ) \d+: ").ok()).as_ref();
    let mut t = t.trim().to_string();
    if let (Some(hr), Some(nr)) = (head, hnum) {
        if let Some(m) = hr.find(&t) {
            let hd = nr.replace(m.as_str(), "").trim().trim_end_matches('.').to_string();
            let body = t[m.end()..].to_string();
            let mut low = hd.to_lowercase();
            let mut caps: HashSet<String> = names.iter().cloned().collect();
            let bc: Vec<char> = body.chars().collect();
            let cap_word = |i: usize| -> Option<usize> {
                if i < bc.len() && bc[i].is_ascii_uppercase() {
                    let mut j = i + 1;
                    while j < bc.len() && bc[j].is_ascii_lowercase() {
                        j += 1;
                    }
                    if j > i + 1 {
                        return Some(j);
                    }
                }
                None
            };
            let mut i = 0;
            while i < bc.len() {
                match cap_word(i) {
                    Some(j) => {
                        if i >= 2 && bc[i - 1] == ' ' && (bc[i - 2].is_ascii_lowercase() || bc[i - 2] == ',' || bc[i - 2] == ';') {
                            caps.insert(bc[i..j].iter().collect());
                        }
                        i = j;
                    }
                    None => i += 1,
                }
            }
            i = 0;
            while i < bc.len() {
                match cap_word(i) {
                    Some(j) => {
                        if j < bc.len() && bc[j] == ' ' && cap_word(j + 1).is_some() {
                            caps.insert(bc[i..j].iter().collect());
                        }
                        i = j;
                    }
                    None => i += 1,
                }
            }
            let mut cv: Vec<String> = caps.into_iter().collect();
            cv.sort_by(|a, b| b.chars().count().cmp(&a.chars().count()).then(a.cmp(b)));
            for w in cv {
                low = replace_word(&low, &w.to_lowercase(), &w);
            }
            let mut ch = low.chars();
            let first: String = ch.next().map(|c| c.to_uppercase().collect()).unwrap_or_default();
            t = format!("{}{}. {}", first, ch.as_str(), body).trim().to_string();
        }
    }
    // re.split(r"(?<=[.!?])\s+(?=[A-Z\"'(])")
    let cs: Vec<char> = t.chars().collect();
    let mut out = Vec::new();
    let mut start = 0;
    let mut i = 0;
    while i < cs.len() {
        if cs[i].is_whitespace() {
            let mut j = i;
            while j < cs.len() && cs[j].is_whitespace() {
                j += 1;
            }
            if i > 0 && matches!(cs[i - 1], '.' | '!' | '?') && j < cs.len() && (cs[j].is_ascii_uppercase() || matches!(cs[j], '"' | '\'' | '(')) {
                out.push(cs[start..i].iter().collect::<String>());
                start = j;
            }
            i = j;
        } else {
            i += 1;
        }
    }
    out.push(cs[start..].iter().collect::<String>());
    out.into_iter().map(|x| x.trim().to_string()).filter(|x| !x.is_empty()).collect()
}

/// re.sub(r"(?<!\w)needle(?!\w)", repl, text)
fn replace_word(text: &str, needle: &str, repl: &str) -> String {
    if needle.is_empty() {
        return text.to_string();
    }
    let mut out = String::new();
    let mut last = 0;
    let mut from = 0;
    while let Some(p) = text[from..].find(needle) {
        let s = from + p;
        let e = s + needle.len();
        let before_ok = text[..s].chars().next_back().map_or(true, |c| !w_char(c));
        let after_ok = text[e..].chars().next().map_or(true, |c| !w_char(c));
        if before_ok && after_ok {
            out.push_str(&text[last..s]);
            out.push_str(repl);
            last = e;
            from = e;
        } else {
            from = s + text[s..].chars().next().map_or(1, |c| c.len_utf8());
        }
    }
    out.push_str(&text[last..]);
    out
}

fn names_of(m: &Row) -> Vec<String> {
    m.get("names").and_then(Value::as_array).map(|a| a.iter().map(py_str).collect()).unwrap_or_default()
}

fn story_rank(m: &Row) -> Option<i64> {
    let k = m.get("kind").filter(|v| truthy(Some(v))).map(py_str).unwrap_or_default();
    let st = m.get("style").filter(|v| truthy(Some(v))).map(py_str).unwrap_or_default();
    let txt = text_of(m);
    if k == "founding" {
        return Some(0);
    }
    let n = sentences(&txt, &names_of(m)).len();
    let death = k.starts_with("death_") || k.starts_with("loss") || k.starts_with("join");
    if n < 2 || (clen(&txt) < STORY_MIN_CHARS && !death) {
        return None;
    }
    if death || ["saga_death", "saga_loss", "saga_recap", "arc_new", "arc_birth", "arc_spore"].contains(&st.as_str()) {
        return Some(0);
    }
    if MINOR.contains(&st.as_str()) {
        return None;
    }
    if st.starts_with("arc_") || st.starts_with("saga_") {
        return Some(1);
    }
    None
}

fn line_text(m: &Row) -> String {
    static HD: OnceLock<Option<Regex>> = OnceLock::new();
    let hd = HD.get_or_init(|| Regex::new(r"^\s*(?:DAY|DZIEŃ)\s+\d+:\s*[^a-ząćęłńóśźż]+?[.!?]\s+\S").ok()).as_ref();
    let mut raw = text_of(m);
    if let Some(r) = hd {
        if let Some(x) = r.find(&raw) {
            let last = raw[..x.end()].chars().next_back().map_or(0, |c| c.len_utf8());
            raw = raw[x.end() - last..].to_string();
        }
    }
    let mut ss = sentences(&raw, &names_of(m));
    if ss.is_empty() {
        ss = vec![raw.clone()];
    }
    let mut t = ss[0].clone();
    for x in &ss[1..] {
        if clen(&t) + 1 + clen(x) > LINE_MAX {
            break;
        }
        t.push(' ');
        t.push_str(x);
    }
    let t: String = t.split(['\r', '\n', '\t']).filter(|x| !x.is_empty()).collect::<Vec<_>>().join(" ");
    let t = t.trim().to_string();
    if clen(&t) <= LINE_MAX {
        return t;
    }
    let cut: String = t.chars().take(LINE_MAX).collect();
    let head = match cut.rfind(' ') {
        Some(p) => cut[..p].to_string(),
        None => cut,
    };
    format!("{}...", head.trim_end_matches([',', ';', ':']))
}

fn story_rows(rows: Vec<Row>) -> Vec<Row> {
    let mut days: Vec<((i64, i64), Vec<usize>)> = Vec::new();
    for (i, m) in rows.iter().enumerate() {
        let k = (int_of(m.get("week")), int_of(m.get("day")));
        match days.iter_mut().find(|d| d.0 == k) {
            Some(d) => d.1.push(i),
            None => days.push((k, vec![i])),
        }
    }
    let ranks: Vec<Option<i64>> = rows.iter().map(story_rank).collect();
    let mut demote: HashSet<usize> = HashSet::new();
    for (_, grp) in &days {
        let mut ranked: Vec<usize> = grp.iter().copied().filter(|&i| !matches!(ranks[i], None | Some(0))).collect();
        ranked.sort_by_key(|&i| {
            let st = st_(rows[i].get("style"));
            (STORY_ORDER.iter().position(|o| st.starts_with(o)).unwrap_or(99), -rows[i].get("id").and_then(py_int).unwrap_or(0))
        });
        for &i in ranked.iter().skip(DAY_STORIES) {
            demote.insert(i);
        }
    }
    let mut out = Vec::new();
    for (i, mut m) in rows.into_iter().enumerate() {
        let card = ranks[i].is_some() && !demote.contains(&i);
        if card {
            m.insert("card".into(), json!(true));
        } else {
            let l = line_text(&m);
            m.insert("card".into(), json!(false));
            m.insert("line".into(), json!(l));
        }
        out.push(m);
    }
    out
}

fn journal_feed(c: &Connection, tid: &str, pl: bool, limit: i64) -> Vec<Row> {
    let mut full = feed_rows(c, tid, pl, FEED_MAX, true);
    let raw = load_state(c, tid);
    let dead: Vec<Value> = raw.as_ref().and_then(|r| r.value.get("dead").and_then(Value::as_array).cloned()).unwrap_or_default();
    let sick: HashMap<String, i64> = raw
        .as_ref()
        .and_then(|r| r.value.get("people").and_then(Value::as_array).cloned())
        .map(|a| {
            a.iter()
                .filter(|p| p.get("s").and_then(Value::as_str) == Some("s") && truthy(p.get("id")))
                .map(|p| (py_str(p.get("n").unwrap_or(&Value::Null)), int_of(p.get("id"))))
                .collect()
        })
        .unwrap_or_default();
    annotate(&mut full, &dead, &sick, if pl { "pl" } else { "en" });
    for m in full.iter_mut() {
        m.remove("who");
    }
    let full = story_rows(full);
    let mut out: Vec<Row> = full.into_iter().filter(|m| int_of(m.get("id")) > 0).take(limit.clamp(1, FEED_MAX) as usize).collect();
    for m in out.iter_mut() {
        let s = widget_text(&text_of(m));
        m.insert("short".into(), json!(s));
    }
    out
}

// ------------------------------------------------------------------ the camp roster (clan_story.read_people)
pub fn read_people(c: &Connection, tid: &str, limit: usize, lang: &str) -> Value {
    let Some(raw) = load_state(c, tid) else { return Value::Object(Map::new()) };
    let st = &raw.value;
    let Some(d) = data() else { return Value::Object(Map::new()) };
    let pl = norm_lang(lang) == "pl";
    let w = st.get("fac").and_then(py_int) != Some(crate::rules::SERAPHITES);
    let day = st.get("day").and_then(Value::as_f64).unwrap_or(0.0);
    let morale = st.get("morale").cloned().unwrap_or(Value::Null);
    let mf = morale.as_f64().unwrap_or(0.0);
    let people_v: Vec<Value> = st.get("people").and_then(Value::as_array).cloned().unwrap_or_default();
    let pla = &adm()["plabels"];
    let mut people = Vec::new();
    for p in people_v.iter().take(limit) {
        let g = |k: &str| p.get(k).and_then(Value::as_str).unwrap_or("");
        let role = d.story.roles.iter().find(|x| x.0 == g("r")).map(|x| if w { x.1.clone() } else { x.2.clone() }).unwrap_or_default();
        let state = match g("s") {
            "u" => "hungry",
            "s" => "sick",
            _ => "healthy",
        };
        let mood = mood_of(p, day, mf);
        let ill = d.story.illness.iter().find(|x| x.0 == g("il")).map(|x| x.1.clone()).unwrap_or_default();
        let mut e = Map::new();
        e.insert("name".into(), json!(g("n")));
        e.insert("gender".into(), json!(g("g")));
        e.insert("role".into(), json!(role));
        e.insert("state".into(), json!(state));
        e.insert("mood".into(), json!(mood));
        e.insert("illness".into(), json!(ill));
        if pl {
            let i = if g("g") == "f" { 1 } else { 0 };
            let rt = d.pl.roles.get(g("r")).map(|r| r[if w { 0 } else { 1 }][i].clone()).unwrap_or_default();
            e.insert("roleText".into(), json!(rt));
            e.insert("stateText".into(), pla["state"][state][i].clone());
            e.insert("moodText".into(), pla["mood"][mood][i].clone());
            let it = pla["ill"].get(g("il")).and_then(|x| x.get(0)).cloned().unwrap_or(json!(""));
            e.insert("illnessText".into(), it);
        }
        people.push(Value::Object(e));
    }
    let label = morale_label(mf);
    let mut mo = Map::new();
    mo.insert("value".into(), morale);
    mo.insert("label".into(), json!(label));
    if pl {
        mo.insert("text".into(), pla["morale"][label].clone());
    }
    let dead: Vec<Value> = st
        .get("dead")
        .and_then(Value::as_array)
        .map(|a| {
            let from = a.len().saturating_sub(8);
            a[from..].iter().map(|x| json!({"name": x.get("n").cloned().unwrap_or(Value::Null), "day": x.get("d").cloned().unwrap_or(Value::Null)})).collect()
        })
        .unwrap_or_default();
    json!({"morale": Value::Object(mo), "people": people, "alive": people_v.len(), "dead": dead})
}

fn mood_of(p: &Value, day: f64, mf: f64) -> &'static str {
    let s = p.get("s").and_then(Value::as_str).unwrap_or("");
    if s == "s" {
        "ill"
    } else if s == "u" {
        "hungry"
    } else if p.get("gr").and_then(Value::as_f64).unwrap_or(0.0) > day {
        "grieving"
    } else if mf >= 70.0 {
        "cheerful"
    } else if mf >= 50.0 {
        "steady"
    } else if mf >= 30.0 {
        "tired"
    } else {
        "hollow"
    }
}

fn morale_label(m: f64) -> &'static str {
    if m >= 70.0 {
        "hopeful"
    } else if m >= 50.0 {
        "steady"
    } else if m >= 30.0 {
        "strained"
    } else {
        "grim"
    }
}

// ------------------------------------------------------------------ loot phrases (clan_loot)
// ITEMS fields (clan_loot.KEYS): key cat tier rar qmin qmax val kinds icon g en1 enn pln pla plf plm  (indexes 0..15)
fn item(k: &str) -> Option<&'static Vec<Value>> {
    adm()["loot"]["items"].get(k).and_then(Value::as_array)
}

fn istr(it: &[Value], i: usize) -> String {
    it[i].as_str().unwrap_or("").to_string()
}

fn art(w: &str) -> &'static str {
    if w.chars().next().map_or(true, |c| "aeiouAEIOU".contains(c)) {
        "an"
    } else {
        "a"
    }
}

fn item_phrase(key: &str, n: i64, cond: i64, pl: bool) -> Option<String> {
    let it = item(key)?;
    let mut n = n.max(1);
    if istr(it, 11) == "-" {
        n = 1;
    }
    let conds = adm()["loot"]["conds"].as_array()?;
    let cd = if cond >= 0 && (cond as usize) < conds.len() { conds[cond as usize].as_array() } else { None };
    if pl {
        let (noun, ad): (String, String) = if n == 1 {
            let g = istr(it, 9);
            (istr(it, 13), if g != "f" && g != "n" { "m".into() } else { g })
        } else {
            let few = (2..=4).contains(&n.rem_euclid(10)) && !(12..=14).contains(&n.rem_euclid(100));
            if few {
                (istr(it, 14), "few".into())
            } else {
                (istr(it, 15), "many".into())
            }
        };
        let pre = match cd {
            Some(c) => {
                let adj = c[2].as_str().unwrap_or("");
                let mut cs: Vec<char> = adj.chars().collect();
                cs.pop();
                let s: String = cs.into_iter().collect();
                let form = match ad.as_str() {
                    "m" => adj.to_string(),
                    "f" => format!("{}ą", s),
                    "n" | "few" => format!("{}e", s),
                    _ => format!("{}ych", s),
                };
                format!("{}{} ", c[1].as_str().unwrap_or(""), form)
            }
            None => String::new(),
        };
        return Some(format!("{}{}{}", if n > 1 { format!("{} ", n) } else { String::new() }, pre, noun));
    }
    if n == 1 {
        let word = format!("{}{}", cd.map(|c| format!("{} ", c[0].as_str().unwrap_or(""))).unwrap_or_default(), istr(it, 10));
        return Some(format!("{} {}", art(&word), word));
    }
    Some(format!("{} {}{}", n, cd.map(|c| format!("{} ", c[0].as_str().unwrap_or(""))).unwrap_or_default(), istr(it, 11)))
}

fn join_list(items: &[String], pl: bool) -> String {
    if items.len() <= 1 {
        return items.concat();
    }
    format!("{}{}{}", items[..items.len() - 1].join(", "), if pl { " i " } else { " and " }, items[items.len() - 1])
}

fn cap1(s: &str) -> String {
    let mut c = s.chars();
    match c.next() {
        Some(f) => f.to_uppercase().collect::<String>() + c.as_str(),
        None => String::new(),
    }
}

fn loc_phrase(f: &Value, pl: bool, with_place: bool) -> Option<String> {
    let lst = adm()["loot"]["micro"].get(f.get("kd")?.as_str()?)?.as_array()?;
    let mi = py_int(f.get("mi")?)?;
    let e = lst.get(mi.rem_euclid(lst.len() as i64) as usize)?.as_array()?;
    let mut txt = e[if pl { 1 } else { 0 }].as_str()?.to_string();
    if with_place && truthy(f.get("pl")) {
        let p = py_str(f.get("pl")?);
        if !pl {
            txt.push_str(&format!(", near {}", p));
        } else if let Some(v) = adm()["loot"]["places_pl"].get(&p).and_then(Value::as_str) {
            txt.push_str(&format!(", w pobliżu {}", v));
        }
    }
    Some(txt)
}

fn hook_text(f: &Value, pl: bool) -> String {
    let hk = f.get("h").and_then(py_int).unwrap_or(-1);
    let ha = f.get("ha").and_then(py_int).unwrap_or(0);
    if hk == -2 {
        return if pl { format!("Kombinacja z dnia {} otworzyła sejf.", ha) } else { format!("The combination from day {} opened a safe.", ha) };
    }
    if hk < 0 {
        return String::new();
    }
    let l = &adm()["loot"];
    let Some(row) = l["hooks"].get(hk as usize).and_then(Value::as_array) else { return String::new() };
    let names = l["names"].as_array().cloned().unwrap_or_default();
    let notes = l["notes"].as_array().cloned().unwrap_or_default();
    let name = names.get(ha.rem_euclid(names.len().max(1) as i64) as usize).map(py_str).unwrap_or_default();
    let note = notes.get(ha.rem_euclid(notes.len().max(1) as i64) as usize).and_then(|x| x.get(if pl { 1 } else { 0 })).map(py_str).unwrap_or_default();
    let combo = format!("{}-{}-{}", ha.rem_euclid(40) + 1, ha.div_euclid(7).rem_euclid(40) + 1, ha.div_euclid(11).rem_euclid(40) + 1);
    row[if pl { 1 } else { 0 }].as_str().unwrap_or("").replace("{name}", &name).replace("{combo}", &combo).replace("{note}", &note)
}

fn render_find(f: &Value, pl: bool) -> Option<String> {
    let k = f.get("k")?.as_str()?;
    let ph = item_phrase(k, f.get("n").and_then(py_int)?, f.get("c").and_then(py_int).unwrap_or(-1), pl)?;
    let v = f.get("v").and_then(py_int).unwrap_or(0);
    let loc = loc_phrase(f, pl, v.rem_euclid(4) == 0)?;
    let who = f.get("w").filter(|x| truthy(Some(x))).map(py_str).unwrap_or_else(|| if pl { "Ktoś".into() } else { "Someone".into() });
    let l = &adm()["loot"];
    let tpl = l[if pl { "sent_pl" } else { "sent_en" }].get(v.rem_euclid(4) as usize)?.as_str()?;
    let fem = f.get("g").and_then(Value::as_str) == Some("f");
    let mut s = tpl
        .replace("{who}", &who)
        .replace("{item}", &ph)
        .replace("{loc}", &loc)
        .replace("{Loc}", &cap1(&loc))
        .replace("{a}", if fem { "a" } else { "" })
        .replace("{natknal}", if fem { "natknęła" } else { "natknął" });
    let hk = hook_text(f, pl);
    if !hk.is_empty() {
        s.push(' ');
        s.push_str(&hk);
    } else if truthy(f.get("rare")) {
        s.push(' ');
        s.push_str(if pl { "Nikt w obozie nie widział takiego od lat." } else { "Nobody in the camp has seen one in years." });
    }
    Some(s)
}

fn items_text(finds: &[Value], pl: bool) -> Option<String> {
    let mut v = Vec::new();
    for f in finds {
        v.push(item_phrase(f.get("k")?.as_str()?, f.get("n").and_then(py_int)?, f.get("c").and_then(py_int).unwrap_or(-1), pl)?);
    }
    Some(join_list(&v, pl))
}

fn loot_public(finds: &[Value], pl: bool) -> Vec<Value> {
    let l = &adm()["loot"];
    let mut out = Vec::new();
    for f in finds {
        let Some(k) = f.get("k").and_then(Value::as_str) else { continue };
        let Some(it) = item(k) else { continue };
        let icon = istr(it, 8);
        let icons: Vec<&str> = l["icons"].as_array().map(|a| a.iter().filter_map(Value::as_str).collect()).unwrap_or_default();
        let cat = istr(it, 1);
        let shown = if icons.contains(&icon.as_str()) { icon.clone() } else { l["fallback"][&cat].as_str().unwrap_or("").to_string() };
        let tier = it[2].as_i64().unwrap_or(0);
        let rar = it[3].as_i64().unwrap_or(0);
        out.push(json!({"k": k, "n": f.get("n").cloned().unwrap_or(Value::Null), "icon": shown, "icon_want": icon, "cat": cat,
                        "tier": f.get("t").cloned().unwrap_or(json!(tier)), "tier_name": l["tiers"].get(tier as usize).cloned().unwrap_or(Value::Null),
                        "item": item_phrase(k, f.get("n").and_then(py_int).unwrap_or(1), f.get("c").and_then(py_int).unwrap_or(-1), pl),
                        "text": render_find(f, pl), "who": f.get("w").cloned().unwrap_or(json!("")), "where": loc_phrase(f, pl, false),
                        "rare": truthy(f.get("rare")) || rar >= 2, "keep": cat == "trink" || cat == "book"}));
    }
    out
}

fn qty(key: &str, n: i64, pl: bool) -> Option<String> {
    let d = data()?;
    let e = d.chron.use_.get(key)?;
    let n = n.max(1);
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
}

fn loot_phrase(key: &str, n: i64, pl: bool) -> Option<String> {
    let d = data()?;
    if let Some(k) = d.chron.keep.get(key) {
        return Some(if pl { k.1.clone() } else { k.0.clone() });
    }
    qty(key, n, pl)
}

// ------------------------------------------------------------------ portraits (clan_portraits + clan_chronicle head helpers)
struct Head {
    id: i64,
    fac: String,
    sex: String,
    tags: HashSet<String>,
}

struct Pool {
    heads: Vec<Head>,
    rendered: HashSet<i64>,
    expr: HashMap<i64, HashSet<String>>,
}

fn pool() -> &'static Pool {
    static P: OnceLock<Pool> = OnceLock::new();
    P.get_or_init(|| {
        let a = &adm()["portraits"];
        let heads = a["pool"]
            .as_array()
            .map(|rows| {
                rows.iter()
                    .filter_map(|r| {
                        let r = r.as_array()?;
                        Some(Head { id: r[0].as_i64()?, fac: r[2].as_str()?.to_string(), sex: r[3].as_str()?.to_string(), tags: r[5].as_str()?.split('+').map(str::to_string).collect() })
                    })
                    .collect()
            })
            .unwrap_or_default();
        let rendered = a["rendered"].as_array().map(|x| x.iter().filter_map(Value::as_i64).collect()).unwrap_or_default();
        let expr = a["expr"]
            .as_object()
            .map(|o| o.iter().filter_map(|(k, v)| Some((k.parse().ok()?, v.as_array()?.iter().filter_map(|s| s.as_str().map(str::to_string)).collect()))).collect())
            .unwrap_or_default();
        Pool { heads, rendered, expr }
    })
}

fn pref(role: &str) -> Vec<(&'static str, i64)> {
    match role {
        "kid" => vec![("kid", 7), ("teen", 3)],
        "elder" => vec![("elder", 8), ("civilian", 2)],
        "medic" => vec![("medic", 8), ("patient", 1), ("civilian", 2)],
        "cook" => vec![("kitchen", 8), ("civilian", 2)],
        "guard" => vec![("soldier", 8), ("gym", 1), ("raider", 1)],
        "scout" => vec![("soldier", 5), ("scout", 4), ("raider", 1)],
        "scavenger" => vec![("civilian", 5), ("refugee", 2), ("raider", 2), ("soldier", 1)],
        "mechanic" => vec![("gym", 3), ("civilian", 4), ("raider", 2), ("soldier", 1)],
        "gardener" => vec![("civilian", 8), ("elder", 2)],
        "radio" => vec![("civilian", 5), ("soldier", 3), ("scout", 2)],
        _ => vec![("civilian", 5), ("soldier", 3), ("gym", 1), ("refugee", 1)],
    }
}

fn cands(role: &str, sex: &str, fk: &str, sides: &[&str], sera_civ: bool) -> Vec<(i64, i64)> {
    let p = pool();
    let pf = pref(role);
    let elder_ok = ["elder", "gardener", "cook", "radio", "medic"].contains(&role);
    let mut out: Vec<(i64, i64)> = Vec::new();
    for hd in &p.heads {
        if hd.sex != sex || !sides.contains(&hd.fac.as_str()) {
            continue;
        }
        let mut tg = hd.tags.clone();
        if sera_civ && hd.fac == "S" && tg.contains("soldier") {
            tg.insert("civilian".into());
        }
        let mut w = 0;
        for (tag, wt) in &pf {
            if tg.contains(*tag) {
                w = w.max(*wt);
            }
        }
        if w == 0 {
            continue;
        }
        if !elder_ok && tg.contains("elder") {
            continue;
        }
        if role != "kid" && (tg.contains("kid") || tg.contains("teen")) {
            continue;
        }
        if role == "kid" && !tg.contains("kid") && !tg.contains("teen") {
            continue;
        }
        out.push((hd.id, w * if hd.fac == fk { 3 } else { 1 }));
    }
    let shown: Vec<(i64, i64)> = out.iter().copied().filter(|x| p.rendered.contains(&x.0)).collect();
    if shown.is_empty() {
        out
    } else {
        shown
    }
}

fn draw(c: &[(i64, i64)], name: &str, salt: &str) -> i64 {
    let tot: i64 = c.iter().map(|x| x.1).sum();
    let mut x = (h(&[salt.to_string(), name.to_string()]) as i64) % tot;
    for (id, w) in c {
        if x < *w {
            return *id;
        }
        x -= w;
    }
    c[c.len() - 1].0
}

fn pick_in(name: &str, role: &str, sex: &str, fk: &str, used: &HashSet<i64>, legacy: bool) -> Option<i64> {
    let get = |r: &str| if legacy { cands(r, sex, fk, &[fk, "N"], false) } else { cands(r, sex, fk, &[fk], true) };
    let mut c = get(role);
    if c.is_empty() {
        c = get(if role != "kid" { "scavenger" } else { "kid" });
    }
    if c.is_empty() {
        return None;
    }
    let first = draw(&c, name, "head");
    if !used.contains(&first) {
        return Some(first);
    }
    let free: Vec<(i64, i64)> = c.iter().copied().filter(|x| !used.contains(&x.0)).collect();
    if !free.is_empty() {
        return Some(draw(&free, name, "head2"));
    }
    let wide = get(if role != "kid" { "scavenger" } else { "kid" });
    let free: Vec<(i64, i64)> = wide.iter().copied().filter(|x| !used.contains(&x.0)).collect();
    if !free.is_empty() {
        return Some(draw(&free, name, "head3"));
    }
    Some(first)
}

fn faction_of(model: i64) -> Option<String> {
    pool().heads.iter().find(|x| x.id == model).map(|x| x.fac.clone())
}

fn pick(name: &str, role: &str, sex: &str, fk: &str, used: &HashSet<i64>) -> Option<i64> {
    let old = pick_in(name, role, sex, fk, used, true);
    if let Some(o) = old {
        if faction_of(o).as_deref() == Some(fk) {
            return Some(o);
        }
    }
    let new = pick_in(name, role, sex, fk, used, false);
    if new.is_some() {
        new
    } else if fk != "W" && fk != "S" {
        old
    } else {
        None
    }
}

fn expr_for(model: i64, want: &str) -> String {
    let have = pool().expr.get(&model);
    let has = |x: &str| have.map_or(false, |s| s.contains(x));
    if has(want) {
        return want.to_string();
    }
    let alt = match want {
        "hurt" | "scared" | "shock" | "grieving" => "sad",
        "injured" => "sick",
        _ => "neutral",
    };
    if has(alt) {
        alt.to_string()
    } else {
        "neutral".to_string()
    }
}

struct Member {
    i: i64,
    n: String,
    g: String,
    r: String,
}

fn member_of(v: &Value) -> Member {
    Member { i: int_of(v.get("i")), n: st_(v.get("n")), g: st_(v.get("g")), r: st_(v.get("r")) }
}

fn sex_of(g: &str, name: &str) -> &'static str {
    if g == "f" {
        "f"
    } else if g == "m" {
        "m"
    } else if h(&["sex".into(), name.into()]) & 1 == 1 {
        "f"
    } else {
        "m"
    }
}

fn head_plain(m: &Member, fk: &str) -> (&'static str, i64) {
    let sex = sex_of(&m.g, &m.n);
    let mut model = (h(&["model".into(), m.n.clone()]) as i64) % 4;
    if let Some(p) = pick(&m.n, &m.r, sex, fk, &HashSet::new()) {
        model = p;
    }
    (sex, model)
}

fn heads_of(members: &[Member], fk: &str) -> HashMap<String, (&'static str, i64)> {
    let mut out = HashMap::new();
    let mut used_m: HashSet<i64> = HashSet::new();
    let mut used_f: HashSet<i64> = HashSet::new();
    let mut ord: Vec<&Member> = members.iter().collect();
    ord.sort_by_key(|m| m.i);
    for m in ord {
        let (sex, mut model) = head_plain(m, fk);
        let used = if sex == "m" { &mut used_m } else { &mut used_f };
        if used.contains(&model) {
            if let Some(m2) = pick(&m.n, &m.r, sex, fk, used) {
                model = m2;
            }
        }
        used.insert(model);
        out.insert(m.n.clone(), (sex, model));
    }
    out
}

// ------------------------------------------------------------------ personas and status codes
const HEALTHY: i64 = 0;
const HUNGRY: i64 = 1;
const SAD: i64 = 2;
const SICK: i64 = 3;
const INJURED: i64 = 5;
const DEPRESSED: i64 = 8;
const SHOCK: i64 = 9;

fn code_of(st: &Value, p: &Value) -> i64 {
    let s = p.get("s").and_then(Value::as_str).unwrap_or("h");
    let day = int_of(st.get("day"));
    let key = p.get("i").map(py_str).unwrap_or_default();
    let ch = st.get("ch");
    let mem = ch.and_then(|c| c.get("mem")).and_then(|m| m.get(&key)).filter(|v| truthy(Some(v)));
    let dep = ch.and_then(|c| c.get("dep")).and_then(|m| m.get(&key)).filter(|v| truthy(Some(v)));
    let g = |o: Option<&Value>, k: &str| o.and_then(|x| x.get(k)).cloned();
    if int_of(g(dep, "n").as_ref()) >= 4 {
        return DEPRESSED;
    }
    let g0 = g(mem, "grief").filter(|v| truthy(Some(v)));
    if let Some(g0) = &g0 {
        if g0.get("d").and_then(py_int).unwrap_or(-1) == day {
            return SHOCK;
        }
    }
    if s == "s" {
        return SICK;
    }
    if s == "u" {
        return HUNGRY;
    }
    if let Some(g0) = &g0 {
        if g0.get("until").and_then(py_int).unwrap_or(-1) >= day {
            return SAD;
        }
    }
    if truthy(g(dep, "rec").as_ref()) && int_of(g(dep, "n").as_ref()) > 0 {
        return SAD;
    }
    if (truthy(g(mem, "inj").as_ref()) || truthy(p.get("il"))) && p.get("r").and_then(Value::as_str) != Some("kid") {
        return INJURED;
    }
    HEALTHY
}

fn stage(r: &str) -> &'static str {
    match r {
        "kid" => "kid",
        "elder" => "elder",
        _ => "adult",
    }
}

fn habit_ok(i: usize, m: &Member, fk: &str) -> bool {
    let c = &adm()["chron"];
    let meta = &c["habit_meta"][i];
    let (cls, sc) = (meta[0].as_str().unwrap_or(""), meta[1].as_str().unwrap_or(""));
    let hs = &c["habit_short"][i];
    if m.r == "kid" && (crate::text::combat_text(hs[0].as_str().unwrap_or(""), false) || crate::text::combat_text(hs[1].as_str().unwrap_or(""), true)) {
        return false;
    }
    (sc == "*" || sc == fk) && (cls == "any" || cls == stage(&m.r) || (cls == "adult" && stage(&m.r) == "elder"))
}

fn habit_pref(m: &Member, fk: &str) -> Vec<usize> {
    let n = adm()["chron"]["habit_short"].as_array().map_or(0, Vec::len);
    let mut ok: Vec<usize> = (0..n).filter(|&i| habit_ok(i, m, fk)).collect();
    ok.sort_by_key(|&i| h(&["habit".into(), m.n.clone(), m.r.clone(), i.to_string()]));
    ok
}

fn assign_habits(hab: &mut HashMap<String, usize>, people: &[Member], fk: &str) {
    let alive: HashSet<String> = people.iter().map(|p| p.i.to_string()).collect();
    hab.retain(|k, _| alive.contains(k));
    let mut taken: HashSet<usize> = hab.values().copied().collect();
    let mut ord: Vec<&Member> = people.iter().collect();
    ord.sort_by_key(|p| p.i);
    for p in ord {
        let k = p.i.to_string();
        if let Some(&cur) = hab.get(&k) {
            if p.r != "kid" || habit_ok(cur, p, fk) {
                continue;
            }
        }
        let pf = habit_pref(p, fk);
        let free: Vec<usize> = pf.iter().copied().filter(|i| !taken.contains(i)).collect();
        let pick = *free.first().or(pf.first()).unwrap_or(&0);
        hab.insert(k, pick);
        taken.insert(pick);
    }
}

struct Persona {
    temper: usize,
    habit: usize,
    past: usize,
    bond: Option<i64>,
    bt: Option<String>,
}

fn persona(m: &Member, people: &[Member], fk: &str, hab: &mut HashMap<String, usize>) -> Persona {
    let c = &adm()["chron"];
    let hh = h(&["persona".into(), m.n.clone(), m.r.clone(), m.g.clone()]);
    let pasts: Vec<usize> = c["pasts"]
        .as_array()
        .map(|a| {
            a.iter()
                .enumerate()
                .filter(|(_, x)| {
                    let (f, s) = (x[0].as_str().unwrap_or(""), x[1].as_str().unwrap_or(""));
                    (f == "*" || f == fk) && (s == stage(&m.r) || (s == "adult" && stage(&m.r) == "elder"))
                })
                .map(|(i, _)| i)
                .collect()
        })
        .unwrap_or_default();
    let mut cur = hab.get(&m.i.to_string()).copied();
    if cur.is_none() {
        assign_habits(hab, people, fk);
        cur = hab.get(&m.i.to_string()).copied();
    }
    let habit = cur.unwrap_or_else(|| habit_pref(m, fk).first().copied().unwrap_or(0));
    let mut out = Persona { temper: (hh % 6) as usize, habit, past: pasts.get(((hh >> 8) as usize) % pasts.len().max(1)).copied().unwrap_or(0), bond: None, bt: None };
    let mut others: Vec<&Member> = people.iter().filter(|q| q.i != m.i).collect();
    others.sort_by_key(|q| h(&["ring".into(), m.n.clone(), q.n.clone()]));
    if let Some(q) = others.first() {
        let mut pair = [m.n.clone(), q.n.clone()];
        pair.sort();
        let mut parts = vec!["bond".to_string()];
        parts.extend(pair);
        let types = c["bond_types"].as_array().cloned().unwrap_or_default();
        let mut bt = types[(h(&parts) % 4) as usize].as_str().unwrap_or("").to_string();
        if (m.r == "kid" || q.r == "kid") && bt == "sweet" {
            bt = "kin".into();
        }
        if (m.r == "kid" && q.r == "elder") || (m.r == "elder" && q.r == "kid") {
            bt = "mentor".into();
        }
        out.bond = Some(q.i);
        out.bt = Some(bt);
    }
    out
}

fn tagline(m: &Member, ps: &Persona, fk: &str, pl: bool, d: &Data) -> String {
    let c = &adm()["chron"];
    let role = role_word(d, &Person { n: m.n.clone(), g: m.g.clone(), r: m.r.clone() }, fk, pl).unwrap_or_default();
    let tkey = c["tempers"][ps.temper].as_str().unwrap_or("");
    let te = &c["temper_label"][tkey];
    let t = if pl && m.g == "f" { te[2].as_str() } else if pl { te[1].as_str() } else { te[0].as_str() }.unwrap_or("");
    let his = match m.g.as_str() {
        "m" => "his",
        "f" => "her",
        _ => "their",
    };
    let ha = c["habit_short"][ps.habit][if pl { 1 } else { 0 }].as_str().unwrap_or("").replace("{his}", his);
    let past = &c["pasts"][ps.past];
    let pa = if pl { if m.g == "f" { past[4].as_str() } else { past[3].as_str() } } else { past[2].as_str() }.unwrap_or("");
    format!("{}, {}. {}. {}.", cap1(&role), t, cap1(pa), cap1(&ha))
}

fn bond_label(b: &Member, a: &Member, bt: &str, pl: bool) -> String {
    let c = &adm()["chron"];
    let rank = |r: &str| match stage(r) {
        "kid" => 0,
        "adult" => 1,
        _ => 2,
    };
    let key = match bt {
        "sweet" => "partner",
        "rival" => "rival",
        "kin" => "kin",
        "mentor" => {
            if rank(&b.r) >= rank(&a.r) {
                "mentor"
            } else {
                "apprentice"
            }
        }
        _ => return String::new(),
    };
    let lg = if pl { "pl" } else { "en" };
    if b.g == "f" {
        if let Some(x) = c["bond_label_f"].get(lg).and_then(|o| o.get(key)).and_then(Value::as_str) {
            return x.to_string();
        }
    }
    c["bond_label"][lg][key].as_str().unwrap_or("").to_string()
}

fn status_name(code: i64) -> &'static str {
    ["healthy", "hungry", "sad", "sick", "dead", "injured", "recovered", "new", "depressed", "shock"].get(code as usize).copied().unwrap_or("healthy")
}

fn status_expr(code: i64) -> &'static str {
    ["neutral", "hungry", "sad", "sick", "dead", "sick", "neutral", "neutral", "depressed", "shock"].get(code as usize).copied().unwrap_or("neutral")
}

fn portrait_of(st: &Value, p: &Member, pv: &Value, fk: &str, members: &[Member], heads: &HashMap<String, (&'static str, i64)>) -> String {
    let (sex, model) = if members.iter().any(|x| x.n == p.n) { heads[&p.n] } else { head_plain(p, fk) };
    let morale = st.get("morale").and_then(Value::as_f64).unwrap_or(50.0) as i64;
    let s = pv.get("s").and_then(Value::as_str).unwrap_or("h");
    let expr = match s {
        "d" => "dead",
        "i" => "hurt",
        "h" if morale >= 70 => "happy",
        "h" if morale <= 25 => "scared",
        "u" => "hungry",
        "s" => "sick",
        _ => "neutral",
    };
    let pid = format!("{}/{}/{}/{}", fk, sex, model, expr_for(model, expr));
    let code = code_of(st, pv);
    if code != HEALTHY {
        return format!("{}/{}/{}/{}", fk, sex, model, expr_for(model, status_expr(code)));
    }
    pid
}

fn status_cards(st: &Value, fk: &str, pl: bool, members: &[Member], d: &Data) -> Vec<Value> {
    let a = adm();
    let day = int_of(st.get("day"));
    let heads = heads_of(members, fk);
    let mut out = Vec::new();
    let evs = st.get("ch").and_then(|c| c.get("stat")).and_then(|s| s.get("ev")).and_then(Value::as_array).cloned().unwrap_or_default();
    for e in evs.iter().filter(|e| day - int_of(e.get("d")) < 3) {
        let code = int_of(e.get("c"));
        let name = st_(e.get("n"));
        let mem = Member { i: int_of(e.get("i")), n: name.clone(), g: e.get("g").and_then(Value::as_str).unwrap_or("n").to_string(), r: st_(e.get("r")) };
        let (sex, model) = if members.iter().any(|x| x.n == name) { heads[&name] } else { head_plain(&mem, fk) };
        let mut img = Map::new();
        for x in ["neutral", "hungry", "sad", "sick", "dead", "depressed", "shock"] {
            img.insert(x.into(), json!(format!("{}/{}/{}/{}", fk, sex, model, expr_for(model, x))));
        }
        let mut line: Option<String> = None;
        if truthy(e.get("jl")) {
            line = journal_render(e.get("jl").unwrap_or(&Value::Null), pl, d).filter(|t| !t.is_empty());
        }
        if line.is_none() && truthy(e.get("ln")) {
            line = Some(st_(e.get("ln")));
        }
        let line = line.unwrap_or_else(|| {
            let t = &a["status"][if pl { "lines_pl" } else { "lines_en" }][code.to_string()];
            let tpl = match t {
                Value::Array(x) => {
                    let fem = if mem.g == "f" {
                        true
                    } else if mem.g == "m" {
                        false
                    } else {
                        h(&["sex".into(), name.clone()]) & 1 == 1
                    };
                    x.get(if fem { 1 } else { 0 }).and_then(Value::as_str).unwrap_or("%s").to_string()
                }
                Value::String(s) => s.clone(),
                _ => "%s".into(),
            };
            tpl.replacen("%s", &name, 1)
        });
        out.push(json!({"id": format!("{}:{}:{}", py_str(e.get("i").unwrap_or(&Value::Null)), code, py_str(e.get("d").unwrap_or(&Value::Null))),
                        "name": name, "role": st_(e.get("r")), "code": code, "state": status_name(code), "from": status_name(int_of(e.get("f"))),
                        "day": e.get("d").cloned().unwrap_or(Value::Null), "expr": status_expr(code), "img": Value::Object(img), "line": line,
                        "ln": truthy(e.get("ln"))}));
    }
    out
}

fn journal_render(sent: &Value, pl: bool, d: &Data) -> Option<String> {
    let arr = sent.as_array()?;
    let lid = arr.first()?.as_str()?;
    let params = arr.get(1)?.as_object()?;
    let (pool, i) = lid.get(3..)?.rsplit_once('.')?;
    let i: usize = i.parse().ok()?;
    let mut people = People::default();
    if let Some(p) = params.get("p").and_then(Value::as_object) {
        for (k, v) in p {
            let a = v.as_array()?;
            let g = match a.get(1)?.as_str() {
                Some(s) if d.story.pron.contains_key(s) => s.to_string(),
                _ => "n".into(),
            };
            if k.chars().count() == 1 {
                people.set(k.chars().next()?, Person { n: py_str(a.first()?), g, r: String::new() });
            }
        }
    }
    let sym = params.get("s").and_then(Value::as_object).cloned().unwrap_or_default();
    let vars = journal_vars(&sym, pl, d);
    let j = &adm()["journal"];
    let t = if pl {
        render_pl(j["pl"].get(pool)?.get(i)?.as_str()?, &people, &vars)?
    } else {
        render_en(j["en"].get(pool)?.get(i)?.get(1)?.as_str()?, &people, &vars)?
    };
    Some(cap1(&t))
}

fn journal_vars(sym: &Map<String, Value>, pl: bool, d: &Data) -> Vars {
    let j = &adm()["journal"];
    let fk = match sym.get("f").and_then(Value::as_str) {
        Some("S") => "S",
        _ => "W",
    };
    let mut v = Vars::new();
    for k in j["num"].as_array().cloned().unwrap_or_default() {
        if let Some((k, x)) = k.as_str().and_then(|k| sym.get(k).map(|x| (k, x))) {
            v.insert(k.to_string(), py_str(x));
        }
    }
    let mp = sym.get("map").filter(|x| truthy(Some(x))).map(py_str);
    let ill = sym.get("ill").filter(|x| truthy(Some(x))).map(py_str);
    let lack = sym.get("lack").filter(|x| truthy(Some(x))).map(py_str);
    let names = sym.get("names").filter(|x| truthy(Some(x))).and_then(Value::as_array).map(|a| a.iter().map(py_str).collect::<Vec<_>>());
    let join = |n: &[String], and: &str| if n.len() == 1 { n[0].clone() } else { format!("{}{}{}", n[..n.len() - 1].join(", "), and, n[n.len() - 1]) };
    if pl {
        if let Some(e) = d.pl.enemy.get(fk) {
            v.insert("E".into(), e[0].clone());
            v.insert("E_gen".into(), e[1].clone());
            v.insert("E_ins".into(), e[2].clone());
        }
        if let Some(m) = mp {
            let short = d.chron.map_short.get(&m).cloned().unwrap_or(m);
            v.insert("map".into(), d.chron.map_pl.get(&short).cloned().unwrap_or(short));
        }
        if let Some(i) = ill {
            let a = adm()["plabels"]["ill"].get(&i).or_else(|| adm()["plabels"]["ill"].get("none"));
            if let Some(a) = a {
                for (k, n) in [("ill", 0), ("ill_gen", 1), ("ill_acc", 2), ("ill_ins", 3)] {
                    v.insert(k.into(), py_str(&a[n]));
                }
            }
        }
        if let Some(l) = lack {
            let t = j["lack_pl"].get(&l).or_else(|| j["lack_pl"].get("food")).map(py_str).unwrap_or_default();
            v.insert("lack".into(), t);
        }
        if let Some(n) = names {
            v.insert("names".into(), join(&n, " i "));
        }
    } else {
        if let Some(f) = d.story.faction.get(fk) {
            v.insert("E".into(), f.enemies.clone());
        }
        if let Some(m) = mp {
            let short = d.chron.map_short.get(&m).cloned().unwrap_or(m);
            let na = j["no_article"].as_array().map_or(false, |a| a.iter().any(|x| x.as_str() == Some(&short)));
            v.insert("map".into(), if na || short.to_lowercase().starts_with("the ") { short } else { format!("the {}", short) });
        }
        if let Some(i) = ill {
            match d.story.illness.iter().find(|x| x.0 == i) {
                Some(r) => {
                    v.insert("ill".into(), r.1.clone());
                    v.insert("ill_a".into(), r.2.clone());
                }
                None => {
                    v.insert("ill".into(), "sickness".into());
                    v.insert("ill_a".into(), "a sickness".into());
                }
            }
        }
        if let Some(l) = lack {
            let t = j["lack_en"].get(&l).or_else(|| j["lack_en"].get("food")).map(py_str).unwrap_or_default();
            v.insert("lack".into(), t);
        }
        if let Some(n) = names {
            v.insert("names".into(), join(&n, " and "));
        }
    }
    v
}

/// clan_story._load: the whole stored story state (with the chronicle's "ch" block).
fn load_full(c: &Connection, tid: &str) -> Option<Value> {
    let s: Option<String> = c.query_row("SELECT state FROM clan_story WHERE token_id=?", params![tid], |r| r.get(0)).ok()?;
    let v: Value = serde_json::from_str(&s?).ok()?;
    if v.is_object() && v.get("v").and_then(Value::as_i64) == Some(1) {
        Some(v)
    } else {
        None
    }
}

/// clan_chronicle.public: {"roster", "matches", "inv", "keep", ["status"]}; an empty object without a story.
fn chronicle_public(c: &Connection, tid: &str, pl: bool) -> Value {
    let Some(st) = load_full(c, tid) else { return Value::Object(Map::new()) };
    let Some(d) = data() else { return Value::Object(Map::new()) };
    let st = &st;
    let fk = if st.get("fac").and_then(py_int) == Some(crate::rules::SERAPHITES) { "S" } else { "W" };
    let pv: Vec<Value> = st.get("people").and_then(Value::as_array).cloned().unwrap_or_default();
    let people: Vec<Member> = pv.iter().map(member_of).collect();
    let dead: Vec<Value> = st.get("dead").and_then(Value::as_array).cloned().unwrap_or_default();
    let mut members: Vec<Member> = pv.iter().map(member_of).collect();
    members.extend(dead.iter().map(member_of));
    let mut hab: HashMap<String, usize> = st
        .get("ch")
        .and_then(|c| c.get("hab"))
        .and_then(Value::as_object)
        .map(|o| o.iter().filter_map(|(k, v)| Some((k.clone(), v.as_u64()? as usize))).collect())
        .unwrap_or_default();
    let heads = heads_of(&members, fk);
    let ic = &adm()["chron"]["icon"];
    let mut roster = Vec::new();
    for (i, p) in pv.iter().enumerate().take(40) {
        let m = &people[i];
        let ps = persona(m, &people, fk, &mut hab);
        let b = ps.bond.and_then(|bi| people.iter().find(|q| q.i == bi));
        let sname = status_name(code_of(st, p));
        let base = match p.get("s").and_then(Value::as_str) {
            Some("u") => "hungry",
            Some("s") => "sick",
            _ => "healthy",
        };
        let state = if ["hungry", "sick", "sad", "injured", "depressed", "shock"].contains(&sname) { sname } else { base };
        let rw = role_word(d, &Person { n: m.n.clone(), g: m.g.clone(), r: m.r.clone() }, fk, pl).unwrap_or_default();
        roster.push(json!({"name": m.n, "role": m.r, "roleText": rw, "icon": ic.get(&m.r).cloned().unwrap_or(json!("star")), "state": state,
                           "tagline": tagline(m, &ps, fk, pl, d), "bond": b.map(|x| x.n.clone()).unwrap_or_default(), "bondType": ps.bt.clone().unwrap_or_default(),
                           "bondLabel": match (&b, &ps.bt) { (Some(b), Some(bt)) => bond_label(b, m, bt, pl), _ => String::new() },
                           "portrait": portrait_of(st, m, p, fk, &members, &heads)}));
    }
    let ch = st.get("ch").cloned().unwrap_or(Value::Null);
    let log = ch.get("log").and_then(Value::as_array).cloned().unwrap_or_default();
    let mut matches = Vec::new();
    for e in log.iter().rev().take(20) {
        let Some(mut row) = e.as_object().cloned() else { continue };
        let loot: Vec<Value> = e.get("loot").and_then(Value::as_array).cloned().unwrap_or_default();
        let phrases: Vec<String> = loot.iter().filter_map(|x| loot_phrase(x.get(0)?.as_str()?, x.get(1).and_then(py_int)?, pl)).collect();
        let lt = join_list(&phrases, pl);
        row.insert("loot_text".into(), json!(lt));
        if truthy(e.get("finds")) {
            let finds: Vec<Value> = e.get("finds").and_then(Value::as_array).cloned().unwrap_or_default();
            row.insert("loot_text_basic".into(), json!(lt));
            row.insert("finds".into(), Value::Array(loot_public(&finds, pl)));
            row.insert("loot_text".into(), json!(items_text(&finds, pl).unwrap_or_default()));
        }
        matches.push(Value::Object(row));
    }
    let mut inv: Vec<(String, i64)> = ch.get("inv").and_then(Value::as_object).map(|o| o.iter().map(|(k, v)| (k.clone(), py_int(v).unwrap_or(0))).collect()).unwrap_or_default();
    inv.sort();
    let inv_t: Vec<String> = inv.iter().filter(|x| x.1 > 0).filter_map(|(k, n)| qty(k, *n, pl)).collect();
    let mut keep: Vec<String> = ch.get("keep").and_then(Value::as_object).map(|o| o.keys().cloned().collect()).unwrap_or_default();
    keep.sort();
    let keep_t: Vec<String> = keep.iter().filter_map(|k| loot_phrase(k, 1, pl)).collect();
    let mut out = Map::new();
    out.insert("roster".into(), Value::Array(roster));
    out.insert("matches".into(), Value::Array(matches));
    out.insert("inv".into(), json!(inv_t));
    out.insert("keep".into(), json!(keep_t));
    let stat = status_cards(st, fk, pl, &members, d);
    if !stat.is_empty() {
        out.insert("status".into(), Value::Array(stat));
    }
    Value::Object(out)
}

/// What `clanfeed_view.view(db, "", 40, 0, lang, key=key)` answers on the daemon's modules (without the optional A-Life block).
pub fn admin_feed(c: &Connection, key: &str, lang: &str, now: f64) -> Value {
    let base = crate::view::clanfeed(c, "", "40", "0", lang, now, Some(key));
    let Value::Object(b) = base else { return base };
    if !b.get("hasClan").map_or(false, |v| truthy(Some(v))) {
        let mut o = b.clone();
        o.insert("feed".into(), json!([]));
        return Value::Object(o);
    }
    let pl = norm_lang(lang) == "pl";
    let feed = journal_feed(c, key, pl, 40);
    let mut out = Map::new();
    for (k, v) in b {
        let after = k == "daysDone";
        out.insert(k, v);
        if after {
            out.insert("feed".into(), Value::Array(feed.iter().cloned().map(Value::Object).collect()));
        }
    }
    if let Value::Object(camp) = read_people(c, key, 80, lang) {
        for (k, v) in camp {
            out.insert(k, v);
        }
    }
    if let Value::Object(ch) = chronicle_public(c, key, pl) {
        if truthy(ch.get("matches")) || truthy(ch.get("roster")) {
            out.insert("chron".into(), Value::Object(ch));
        }
    }
    Value::Object(out)
}
