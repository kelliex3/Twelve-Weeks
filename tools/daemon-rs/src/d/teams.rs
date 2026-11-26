//! K4 teams: daemon.py:5880-6890 (team balance rating, admin rules, split / order / host pick, live team moves, auto balance, parties glue,
//! the social host adapter and the party / social ops).
use super::*;
use crate::py;
use crate::social::{self, Parties, Social, SocialHost};
use crate::{clock, hostcap, hostmig};
use indexmap::IndexMap;
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, LazyLock};

// ---------------------------------------------------------------- rating
pub const SKILL_KD_PRIOR: f64 = 10.0;
pub const SKILL_MATCHES: i64 = 100;
pub const SKILL_A: f64 = 0.36;
pub const SKILL_B: f64 = 0.245;
pub const SKILL_NEW: f64 = SKILL_A + SKILL_B * 1.0;
pub const SKILL_CACHE_S: f64 = 300.0;
pub static SKILL_CACHE: LazyLock<Mutex<HashMap<String, (f64, f64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
static SKILL_INDEX: AtomicBool = AtomicBool::new(false);

fn team_balance_on() -> bool {
    crate::env_on("T2R_TEAM_BALANCE", true)
}

// py: daemon.py:5905 skill_of_kd
pub fn skill_of_kd(kills: i64, deaths: i64) -> f64 {
    let kd = (kills.max(0) as f64 + SKILL_KD_PRIOR) / (deaths.max(0) as f64 + SKILL_KD_PRIOR);
    SKILL_A + SKILL_B * kd
}

// py: daemon.py:5910 player_skill
pub fn player_skill(tokens: &[String]) -> HashMap<String, f64> {
    let now = clock::now();
    let mut out: HashMap<String, f64> = HashMap::new();
    let mut need: Vec<String> = Vec::new();
    {
        let cache = SKILL_CACHE.lock();
        for t in tokens {
            match cache.get(t) {
                Some(e) if now - e.0 < SKILL_CACHE_S => {
                    out.insert(t.clone(), e.1);
                }
                _ => need.push(t.clone()),
            }
        }
    }
    if !need.is_empty() {
        let r = db::with_write(|c| {
            if !SKILL_INDEX.load(Ordering::Relaxed) {
                c.execute_batch("CREATE INDEX IF NOT EXISTS match_stats_token_ts ON match_stats(token, ts)")?;
                SKILL_INDEX.store(true, Ordering::Relaxed);
            }
            for t in &need {
                let sid: i64 = c
                    .query_row("SELECT steamid FROM players WHERE token=?", [t], |r| r.get::<_, Option<i64>>(0))
                    .ok()
                    .flatten()
                    .unwrap_or(0);
                let mut toks: Vec<String> = if sid != 0 {
                    let mut s = c.prepare("SELECT token FROM players WHERE steamid=?")?;
                    let v = s.query_map([sid], |r| r.get::<_, String>(0))?.collect::<Result<Vec<_>, _>>()?;
                    v
                } else {
                    Vec::new()
                };
                if !toks.contains(t) {
                    toks.push(t.clone());
                }
                let sql = format!(
                    "SELECT kills, deaths FROM match_stats WHERE token IN ({}) AND kills >= 0 ORDER BY ts DESC LIMIT ?",
                    vec!["?"; toks.len()].join(",")
                );
                let mut params: Vec<rusqlite::types::Value> = toks.iter().map(|x| rusqlite::types::Value::Text(x.clone())).collect();
                params.push(rusqlite::types::Value::Integer(SKILL_MATCHES));
                let mut s = c.prepare(&sql)?;
                let rows: Vec<(Option<i64>, Option<i64>)> =
                    s.query_map(rusqlite::params_from_iter(params), |r| Ok((r.get::<_, Option<i64>>(0)?, r.get::<_, Option<i64>>(1)?)))?.collect::<Result<Vec<_>, _>>()?;
                let k: i64 = rows.iter().map(|r| r.0.unwrap_or(0)).sum();
                let d: i64 = rows.iter().map(|r| r.1.unwrap_or(0).max(0)).sum();
                let v = skill_of_kd(k, d);
                out.insert(t.clone(), v);
                SKILL_CACHE.lock().insert(t.clone(), (now, v));
            }
            Ok(())
        });
        if let Err(e) = r {
            dlog!("SKILL lookup failed", e);
        }
    }
    {
        let mut cache = SKILL_CACHE.lock();
        if cache.len() > 20000 {
            cache.retain(|_, e| now - e.0 < SKILL_CACHE_S);
        }
    }
    tokens.iter().map(|t| (t.clone(), out.get(t).copied().unwrap_or(SKILL_NEW))).collect()
}

// ---------------------------------------------------------------- top players and admin rules (files next to the daemon)
fn top_file() -> PathBuf {
    crate::gen_modes::tools_dir().join("top_players.txt")
}
fn rules_file() -> PathBuf {
    crate::gen_modes::tools_dir().join("team_rules.txt")
}
fn mtime(p: &PathBuf) -> Option<f64> {
    let m = std::fs::metadata(p).ok()?.modified().ok()?;
    Some(m.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0))
}

pub static TOP_CACHE: LazyLock<Mutex<(Option<f64>, HashSet<String>)>> = LazyLock::new(|| Mutex::new((None, HashSet::new())));

/// Python `str.splitlines()`
pub fn py_splitlines(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut it = s.chars().peekable();
    let mut any = false;
    while let Some(c) = it.next() {
        any = true;
        match c {
            '\r' => {
                if it.peek() == Some(&'\n') {
                    it.next();
                }
                out.push(std::mem::take(&mut cur));
                any = false;
            }
            '\n' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}' | '\u{2029}' => {
                out.push(std::mem::take(&mut cur));
                any = false;
            }
            _ => cur.push(c),
        }
    }
    if any {
        out.push(cur);
    }
    out
}

/// `bytes.decode("utf-8", "ignore")`
pub fn utf8_ignore(b: &[u8]) -> String {
    let mut out = String::new();
    let mut rest = b;
    loop {
        match std::str::from_utf8(rest) {
            Ok(s) => {
                out.push_str(s);
                return out;
            }
            Err(e) => {
                let (good, bad) = rest.split_at(e.valid_up_to());
                out.push_str(std::str::from_utf8(good).unwrap_or(""));
                match e.error_len() {
                    Some(n) => rest = &bad[n..],
                    None => return out,
                }
            }
        }
    }
}

// py: daemon.py:5956 top_players
pub fn top_players(tokens: &[String]) -> HashSet<String> {
    let p = top_file();
    let Some(mt) = mtime(&p) else { return HashSet::new() };
    let keys: HashSet<String> = {
        let mut c = TOP_CACHE.lock();
        if c.0 != Some(mt) {
            let mut keys = HashSet::new();
            if let Ok(b) = std::fs::read(&p) {
                for ln in py_splitlines(&utf8_ignore(&b)) {
                    let ln = py::strip(ln.split('#').next().unwrap_or("")).to_lowercase();
                    if !ln.is_empty() {
                        keys.insert(ln);
                    }
                }
            }
            *c = (Some(mt), keys);
        }
        c.1.clone()
    };
    if keys.is_empty() {
        return HashSet::new();
    }
    let mut out = HashSet::new();
    for t in tokens {
        let sid = token_steamid(t);
        if keys.contains(&py::strip(&get_name(t)).to_lowercase()) || (sid != 0 && keys.contains(&sid.to_string())) {
            out.insert(t.clone());
        }
    }
    out
}

pub static RULES_CACHE: LazyLock<Mutex<(Option<f64>, Vec<(String, String)>)>> = LazyLock::new(|| Mutex::new((None, Vec::new())));

// py: daemon.py:5985 team_rules_text
pub fn team_rules_text() -> String {
    match std::fs::read(rules_file()) {
        Ok(b) => utf8_ignore(&b),
        Err(_) => String::new(),
    }
}

// py: daemon.py:5990 team_rules_set
pub fn team_rules_set(text: &str) -> Vec<(String, String)> {
    let text = py::cut(text, 4000).replace('\r', "");
    let p = rules_file();
    let tmp = PathBuf::from(format!("{}.tmp", p.to_string_lossy()));
    let _ = std::fs::write(&tmp, text.as_bytes());
    let _ = std::fs::rename(&tmp, &p);
    let lines: Vec<String> = py_splitlines(&text)
        .iter()
        .map(|l| py::strip(l).to_string())
        .filter(|l| !l.is_empty() && !l.starts_with('#'))
        .collect();
    dlog!(format!("TEAMS rules set by admin: {}", lines.join(" | ")));
    team_rules()
}

// py: daemon.py:6001 team_rules
pub fn team_rules() -> Vec<(String, String)> {
    let Some(mt) = mtime(&rules_file()) else {
        *RULES_CACHE.lock() = (None, Vec::new());
        return Vec::new();
    };
    let mut c = RULES_CACHE.lock();
    if c.0 != Some(mt) {
        let mut out = Vec::new();
        for ln in py_splitlines(&team_rules_text()) {
            let ln = py::strip(ln.split('#').next().unwrap_or(""));
            if let Some(i) = ln.find(py::is_py_space) {
                let kind = &ln[..i];
                let rest = ln[i..].trim_start_matches(py::is_py_space);
                let lk = kind.to_lowercase();
                if !rest.is_empty() && (lk == "apart" || lk == "together") {
                    out.push((lk, rest.to_string()));
                }
            }
        }
        *c = (Some(mt), out);
    }
    c.1.clone()
}

/// name and Steam id strings per token (the `key` argument of rule_match)
pub type RuleKey = HashMap<String, (String, String)>;

fn rule_key(tokens: &[String]) -> RuleKey {
    tokens
        .iter()
        .map(|t| {
            let sid = token_steamid(t);
            (t.clone(), (py::strip(&get_name(t)).to_lowercase(), if sid != 0 { sid.to_string() } else { String::new() }))
        })
        .collect()
}

fn split_ws_join(s: &str) -> String {
    s.split(py::is_py_space).filter(|x| !x.is_empty()).collect::<Vec<_>>().join(" ")
}

// py: daemon.py:6024 rule_match
pub fn rule_match(text: &str, tokens: &[String], key: Option<&RuleKey>) -> Vec<String> {
    let cleaned: String = text.chars().map(|c| if matches!(c, '"' | '\'' | '`' | ',' | ';') { ' ' } else { c }).collect();
    let mut norm = format!(" {} ", split_ws_join(&cleaned.to_lowercase()));
    let own;
    let key = match key {
        Some(k) => k,
        None => {
            own = rule_key(tokens);
            &own
        }
    };
    let empty = (String::new(), String::new());
    let mut order: Vec<&String> = tokens.iter().collect();
    order.sort_by_key(|t| std::cmp::Reverse(key.get(*t).unwrap_or(&empty).0.chars().count()));
    let mut out: HashSet<&String> = HashSet::new();
    for t in order {
        let k = key.get(t).unwrap_or(&empty);
        for nm in [&k.0, &k.1] {
            let nm = split_ws_join(nm);
            if !nm.is_empty() && norm.contains(&format!(" {} ", nm)) {
                if nm == k.0 {
                    norm = norm.replacen(&format!(" {} ", nm), "  ", 1);
                }
                out.insert(t);
                break;
            }
        }
    }
    tokens.iter().filter(|t| out.contains(t)).cloned().collect()
}

pub type RuleGroups = Vec<(String, Vec<String>)>;

// py: daemon.py:6042 rule_groups
pub fn rule_groups(tokens: &[String]) -> RuleGroups {
    let key = rule_key(tokens);
    let mut out: RuleGroups = Vec::new();
    for (kind, text) in team_rules() {
        let g = rule_match(&text, tokens, Some(&key));
        if g.len() >= 2 {
            out.push((kind, g));
        }
    }
    let tp = top_players(tokens);
    let top: Vec<String> = tokens.iter().filter(|t| tp.contains(*t)).cloned().collect();
    if top.len() >= 2 {
        out.push(("apart".to_string(), top));
    }
    out
}

// py: daemon.py:6055 rule_team_for
pub fn rule_team_for(token: &str, teams_of: &IndexMap<String, i64>, rg: &RuleGroups) -> (Option<i64>, String) {
    let mut tog: Vec<i64> = Vec::new();
    let mut avoid: Vec<i64> = Vec::new();
    for (kind, g) in rg {
        if !g.iter().any(|x| x == token) {
            continue;
        }
        for x in g {
            if x == token || !teams_of.contains_key(x) {
                continue;
            }
            let v = if kind == "together" { &mut tog } else { &mut avoid };
            if !v.contains(&teams_of[x]) {
                v.push(teams_of[x]);
            }
        }
    }
    if tog.len() > 1 {
        return (None, "together rules need both teams".to_string());
    }
    if let Some(&t) = tog.first() {
        if avoid.contains(&t) && avoid.len() == 1 {
            return (Some(t), format!("together beats apart (both rules name team {})", t));
        }
        return (Some(t), "together".to_string());
    }
    if avoid.len() == 1 {
        return (Some(1 - avoid[0]), "apart".to_string());
    }
    if avoid.len() == 2 {
        return (None, "apart rules exclude both teams".to_string());
    }
    (None, String::new())
}

fn wsum(skill: Option<&HashMap<String, f64>>, top: &HashSet<String>, g: &[String]) -> f64 {
    py_sum(&g.iter().map(|x| skill.and_then(|m| m.get(x)).copied().unwrap_or(SKILL_NEW) + if top.contains(x) { 0.5 } else { 0.0 }).collect::<Vec<_>>())
}

/// Python 3.12 `sum()` of floats (Neumaier compensated summation, starting from the int 0)
pub fn py_sum(xs: &[f64]) -> f64 {
    if xs.is_empty() {
        return 0.0;
    }
    let mut f = 0.0 + xs[0];
    let mut c = 0.0f64;
    for &x in &xs[1..] {
        let t = f + x;
        if f.abs() >= x.abs() {
            c += (f - t) + x;
        } else {
            c += (x - t) + f;
        }
        f = t;
    }
    if c != 0.0 && c.is_finite() {
        f += c;
    }
    f
}

// py: daemon.py:6076 split_teams
pub fn split_teams(
    players: &[String],
    party_of: &HashMap<String, String>,
    skill: Option<&HashMap<String, f64>>,
    top: Option<&HashSet<String>>,
    groups_rules: Option<&RuleGroups>,
) -> (Vec<String>, Vec<String>) {
    let n = players.len();
    let cap = n.div_ceil(2);
    let mut groups: Vec<Vec<String>> = Vec::new();
    let mut seen: HashMap<&String, usize> = HashMap::new();
    for t in players {
        match party_of.get(t) {
            None => groups.push(vec![t.clone()]),
            Some(pid) => {
                if let Some(&i) = seen.get(pid) {
                    groups[i].push(t.clone());
                } else {
                    seen.insert(pid, groups.len());
                    groups.push(vec![t.clone()]);
                }
            }
        }
    }
    groups.sort_by_key(|g| std::cmp::Reverse(g.len()));
    let mut teams: (Vec<String>, Vec<String>) = (Vec::new(), Vec::new());
    let skill = skill.filter(|s| !s.is_empty());
    let empty_top = HashSet::new();
    let top = top.unwrap_or(&empty_top);
    if let Some(sk) = skill {
        if n <= 12 {
            if n == 0 {
                return (Vec::new(), Vec::new());
            }
            let rg: &[(String, Vec<String>)] = groups_rules.map(|v| v.as_slice()).unwrap_or(&[]);
            let w: HashMap<&String, f64> = players.iter().map(|t| (t, sk.get(t).copied().unwrap_or(SKILL_NEW) + if top.contains(t) { 0.5 } else { 0.0 })).collect();
            let tot = py_sum(&players.iter().map(|t| w[t]).collect::<Vec<_>>());
            let parties: Vec<&Vec<String>> = groups.iter().filter(|g| g.len() > 1 && g.len() <= cap).collect();
            let mut best: Option<Vec<usize>> = None;
            let mut best_c = 0.0;
            let mut idx: Vec<usize> = (0..cap).collect();
            loop {
                let skip;
                let a: HashSet<usize> = idx.iter().copied().collect();
                skip = n % 2 == 0 && !a.contains(&0);
                if !skip {
                    let in_a = |x: &String| players.iter().position(|p| p == x).is_some_and(|i| a.contains(&i));
                    let mut pen = 0.0f64;
                    for (kind, g) in rg {
                        let k = g.iter().filter(|x| in_a(x)).count();
                        if kind == "apart" {
                            let s1: Vec<&String> = g.iter().filter(|x| in_a(x)).collect();
                            let s2: Vec<&String> = g.iter().filter(|x| !in_a(x)).collect();
                            for side in [s1, s2] {
                                for i in 0..side.len() {
                                    for j in i + 1..side.len() {
                                        pen += 100.0 + 10.0 * (w.get(side[i]).copied().unwrap_or(0.0) + w.get(side[j]).copied().unwrap_or(0.0));
                                    }
                                }
                            }
                        } else if 0 < k && k < g.len() {
                            pen += 10000.0;
                        }
                    }
                    for g in &parties {
                        let k = g.iter().filter(|x| in_a(x)).count();
                        if 0 < k && k < g.len() {
                            pen += 500.0;
                        }
                    }
                    let sa = py_sum(&players.iter().enumerate().filter(|(i, _)| a.contains(i)).map(|(_, t)| w[t]).collect::<Vec<_>>());
                    let cost = pen + (2.0 * sa - tot).abs();
                    if best.is_none() || cost < best_c - 1e-9 {
                        best_c = cost;
                        best = Some(idx.clone());
                    }
                }
                // next combination in lexicographic order
                let mut i = cap;
                let mut done = true;
                while i > 0 {
                    i -= 1;
                    if idx[i] != i + n - cap {
                        done = false;
                        break;
                    }
                }
                if done {
                    break;
                }
                idx[i] += 1;
                for j in i + 1..cap {
                    idx[j] = idx[j - 1] + 1;
                }
            }
            let bset: HashSet<usize> = best.unwrap_or_default().into_iter().collect();
            let t0: Vec<String> = players.iter().enumerate().filter(|(i, _)| bset.contains(i)).map(|(_, x)| x.clone()).collect();
            let t1: Vec<String> = players.iter().enumerate().filter(|(i, _)| !bset.contains(i)).map(|(_, x)| x.clone()).collect();
            return if t0.len() >= t1.len() { (t0, t1) } else { (t1, t0) };
        }
        let nt = |g: &[String]| g.iter().filter(|x| top.contains(*x)).count() as i64;
        groups.sort_by(|a, b| {
            let ka = (-(a.len() as i64), -nt(a), -wsum(Some(sk), top, a));
            let kb = (-(b.len() as i64), -nt(b), -wsum(Some(sk), top, b));
            ka.0.cmp(&kb.0).then(ka.1.cmp(&kb.1)).then(ka.2.partial_cmp(&kb.2).unwrap_or(std::cmp::Ordering::Equal))
        });
        let mut tot = [0.0f64; 2];
        let mut tops = [0i64; 2];
        let mut tv: [Vec<String>; 2] = [Vec::new(), Vec::new()];
        for g in &groups {
            let gs = wsum(Some(sk), top, g);
            let mut order = [0usize, 1usize];
            let kf = |i: usize| (if nt(g) != 0 { tops[i] } else { 0 }, tot[i], tv[i].len());
            order.sort_by(|&x, &y| {
                let (a, b) = (kf(x), kf(y));
                a.0.cmp(&b.0).then(a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal)).then(a.2.cmp(&b.2))
            });
            let mut placed = false;
            for a in order {
                if tv[a].len() + g.len() <= cap {
                    tv[a].extend(g.iter().cloned());
                    tot[a] += gs;
                    tops[a] += nt(g);
                    placed = true;
                    break;
                }
            }
            if !placed {
                for x in g {
                    let a = if tv[0].len() < tv[1].len() || (tv[0].len() == tv[1].len() && tot[0] <= tot[1]) { 0 } else { 1 };
                    tv[a].push(x.clone());
                    tot[a] += sk.get(x).copied().unwrap_or(SKILL_NEW) + if top.contains(x) { 0.5 } else { 0.0 };
                    tops[a] += top.contains(x) as i64;
                }
            }
        }
        let [a, b] = tv;
        return if a.len() >= b.len() { (a, b) } else { (b, a) };
    }
    for g in &groups {
        let a = if teams.0.len() <= teams.1.len() { 0 } else { 1 };
        let tgt = if a == 0 { &mut teams.0 } else { &mut teams.1 };
        if tgt.len() + g.len() <= cap {
            tgt.extend(g.iter().cloned());
            continue;
        }
        for t in g {
            if teams.0.len() <= teams.1.len() {
                teams.0.push(t.clone());
            } else {
                teams.1.push(t.clone());
            }
        }
    }
    if teams.0.len() >= teams.1.len() {
        teams
    } else {
        (teams.1, teams.0)
    }
}

// py: daemon.py:6173 order_slots
pub fn order_slots(team0: &[String], team1: &[String], host: Option<&str>) -> Vec<String> {
    let (mut team0, mut team1) = (team0.to_vec(), team1.to_vec());
    let mut host = host.map(|h| h.to_string());
    let in1 = host.as_ref().is_some_and(|h| team1.contains(h));
    if in1 && team1.len() >= team0.len() {
        std::mem::swap(&mut team0, &mut team1);
    } else if in1 {
        dlog!(format!("TEAMS: host {} is on the smaller team - teams kept, host not slot 0", tag(host.as_deref().unwrap_or(""))));
        host = None;
    }
    if let Some(h) = &host {
        if let Some(p) = team0.iter().position(|x| x == h) {
            team0.remove(p);
            team0.insert(0, h.clone());
        }
    }
    let mut out = Vec::new();
    for i in 0..team0.len().max(team1.len()) {
        if i < team0.len() {
            out.push(team0[i].clone());
        }
        if i < team1.len() {
            out.push(team1[i].clone());
        }
    }
    out
}

pub static HOST_SWAP: LazyLock<bool> = LazyLock::new(|| crate::env_on("T2R_HOST_SWAP", true));
pub static HOST_SWAP_SKILL: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_HOST_SWAP_SKILL", 0.35));

// py: daemon.py:6196 team_order
pub fn team_order(players: &[String]) -> Vec<String> {
    let party_of: HashMap<String, String> = players.iter().filter_map(|t| PARTIES.pid(t).filter(|p| !p.is_empty()).map(|p| (t.clone(), p))).collect();
    let sk: Option<HashMap<String, f64>> = if team_balance_on() { Some(player_skill(players)).filter(|s| !s.is_empty()) } else { None };
    let top: HashSet<String> = if sk.is_some() { top_players(players) } else { HashSet::new() };
    let rg: RuleGroups = if sk.is_some() { rule_groups(players) } else { Vec::new() };
    let (mut t0, mut t1) = split_teams(players, &party_of, sk.as_ref(), Some(&top), Some(&rg));
    if !rg.is_empty() {
        let v: Vec<String> = rg.iter().map(|(k, g)| format!("{} {}", k, g.iter().map(|x| tag(x)).collect::<Vec<_>>().join(","))).collect();
        dlog!(format!("TEAMS rules: {}", v.join("; ")));
    }
    for (k, g) in &rg {
        if k == "together" && g.iter().map(|x| t0.contains(x)).collect::<HashSet<_>>().len() > 1 {
            dlog!(format!("TEAMS: together rule {} could not be kept", g.iter().map(|x| tag(x)).collect::<Vec<_>>().join(",")));
        }
    }
    if let Some(sk) = &sk {
        let s0: f64 = py_sum(&t0.iter().map(|x| sk[x]).collect::<Vec<_>>());
        let s1: f64 = py_sum(&t1.iter().map(|x| sk[x]).collect::<Vec<_>>());
        let f = |team: &[String]| team.iter().map(|x| format!("{} {}{}", tag(x), py::fixed(sk[x], 2), if top.contains(x) { " TOP" } else { "" })).collect::<Vec<_>>().join(",");
        dlog!(format!("TEAMS balanced: {} v {} | {} v {}", py::fixed(s0, 2), py::fixed(s1, 2), f(&t0), f(&t1)));
    }
    let bo: Vec<String> = if players.len() >= 3 { best_host_order(players) } else { Vec::new() };
    let mut host: Option<String> = bo.first().cloned();
    if players.len() >= 3 && t0.len() != t1.len() {
        let t0_big = t0.len() > t1.len();
        let big = if t0_big { t0.clone() } else { t1.clone() };
        let small = if t0_big { t1.clone() } else { t0.clone() };
        if !(!bo.is_empty() && big.contains(&bo[0])) {
            let mut sw: Option<(Vec<String>, Vec<String>)> = None;
            if *HOST_SWAP && *MM_PING_GATE && *PING_KICK_MS > 0 && !bo.is_empty() {
                sw = host_swap_pick(&bo[0], &big, &small, players, sk.as_ref(), &top, &rg, &party_of);
            }
            if let Some((big2, small2)) = sw {
                t0 = big2;
                t1 = small2;
                host = Some(bo[0].clone());
            } else {
                host = host_from_team(&big, players);
            }
        }
    }
    if players.len() >= 3 && !host.as_ref().is_some_and(|h| t0.contains(h) || t1.contains(h)) {
        host = None;
    }
    if players.len() < 3 {
        host = if players.is_empty() { None } else { best_host_order(players).first().cloned() };
    }
    order_slots(&t0, &t1, host.as_deref())
}

fn worst_ping(h: &str, players: &[String]) -> Option<i64> {
    let ps: Vec<Option<i64>> = players.iter().filter(|o| o.as_str() != h).map(|o| pair_ping(h, o)).collect();
    if ps.is_empty() || ps.iter().any(|p| p.is_none()) {
        None
    } else {
        ps.iter().filter_map(|p| *p).max()
    }
}

// py: daemon.py:6264 host_swap_pick
#[allow(clippy::too_many_arguments)]
pub fn host_swap_pick(host: &str, big: &[String], small: &[String], players: &[String], sk: Option<&HashMap<String, f64>>, top: &HashSet<String>,
    rg: &RuleGroups, party_of: &HashMap<String, String>) -> Option<(Vec<String>, Vec<String>)> {
    let lim = ping_match_ms();
    let hw = worst_ping(host, players);
    if !small.iter().any(|x| x == host) || hw.is_none() || hw.is_some_and(|h| h > lim) {
        return None;
    }
    let hw = hw.unwrap_or(0);
    for c in host_allowed(big) {
        if let Some(w) = worst_ping(&c, players) {
            if w <= lim {
                return None;
            }
        }
    }
    let skf = sk.filter(|s| !s.is_empty());
    let w: HashMap<&String, f64> = players.iter().map(|x| (x, sk.and_then(|m| m.get(x)).copied().unwrap_or(0.5) + if top.contains(x) { 0.5 } else { 0.0 })).collect();
    let wv = |x: &String| w.get(x).copied().unwrap_or(0.0);
    let pid_mates = |x: &String, team: &[String]| {
        let pid = party_of.get(x);
        pid.is_some() && team.iter().any(|y| y != x && party_of.get(y) == pid)
    };
    let cost = |b: &[String], _s: &[String]| -> (i64, i64) {
        let (mut tog, mut apart) = (0i64, 0i64);
        for (kind, g) in rg {
            let k = g.iter().filter(|x| b.contains(*x)).count() as i64;
            let l = g.len() as i64;
            if kind == "together" && 0 < k && k < l {
                tog += 1;
            } else if kind == "apart" {
                apart += k * (k - 1) / 2 + (l - k) * (l - k - 1) / 2;
            }
        }
        (tog, apart)
    };
    let base = cost(big, small);
    let host_s = host.to_string();
    let mates = pid_mates(&host_s, small);
    if mates {
        dlog!("HOST-PICK", tag(host), "is on the smaller team with party mates - no swap");
    }
    let mut best: Option<(f64, String, Vec<String>, Vec<String>)> = None;
    if !mates {
        for c in big {
            if c == host || pid_mates(c, big) {
                continue;
            }
            let gap = (wv(c) - wv(&host_s)).abs();
            if skf.is_some() && gap > *HOST_SWAP_SKILL {
                continue;
            }
            let nb: Vec<String> = big.iter().map(|x| if x == c { host.to_string() } else { x.clone() }).collect();
            let ns: Vec<String> = small.iter().map(|x| if x == host { c.clone() } else { x.clone() }).collect();
            let k = cost(&nb, &ns);
            if k.0 > base.0 || k.1 > base.1 {
                continue;
            }
            if best.as_ref().is_none_or(|b| gap < b.0 - 1e-9) {
                best = Some((gap, c.clone(), nb, ns));
            }
        }
    }
    let sumw = |v: &[String]| py_sum(&v.iter().map(|x| wv(x)).collect::<Vec<_>>());
    let bests = || big.iter().map(|x| oi(worst_ping(x, players))).collect::<Vec<_>>().join(",");
    match best {
        None => {
            let mut mv: Option<(f64, String, Vec<String>, Vec<String>)> = None;
            for c in big {
                if c == host || pid_mates(c, big) {
                    continue;
                }
                let nb: Vec<String> = players.iter().filter(|x| small.contains(*x) || *x == c).cloned().collect();
                let ns: Vec<String> = players.iter().filter(|x| big.contains(*x) && *x != c).cloned().collect();
                if nb.len() <= ns.len() {
                    continue;
                }
                let k = cost(&nb, &ns);
                if k.0 > base.0 || k.1 > base.1 {
                    continue;
                }
                let gap = (sumw(&nb) - sumw(&ns)).abs();
                if skf.is_some() && gap > (sumw(big) - sumw(small)).abs() + 2.0 * *HOST_SWAP_SKILL {
                    continue;
                }
                if mv.as_ref().is_none_or(|m| gap < m.0 - 1e-9) {
                    mv = Some((gap, c.clone(), nb, ns));
                }
            }
            if let Some((_gap, c, nb, ns)) = mv {
                dlog!("HOST-PICK", tag(host), format!("(worst {} ms, within {}) is on the smaller team, nobody on the bigger one is within the limit (best {} ms) and no swap fits - moved {} to the host's team; teams now {} v {}",
                    hw, lim, bests(), tag(&c), py::fixed(sumw(&nb), 2), py::fixed(sumw(&ns), 2)));
                return Some((nb, ns));
            }
            dlog!("HOST-PICK", tag(host), format!("(worst {} ms) is on the smaller team and nobody on the bigger one is within {} ms - no valid swap or move (parties / rules / skill), old rule", hw, lim));
            None
        }
        Some((_gap, c, nb, ns)) => {
            dlog!("HOST-PICK", tag(host), format!("(worst {} ms, within {}) is on the smaller team and nobody on the bigger one is within the limit (best {} ms) - swapped with {} (skill {} v {}); teams now {} v {}",
                hw, lim, bests(), tag(&c), py::fixed(wv(&host_s), 2), py::fixed(wv(&c), 2), py::fixed(sumw(&nb), 2), py::fixed(sumw(&ns), 2)));
            Some((nb, ns))
        }
    }
}

// py: daemon.py:6342 host_from_team
pub fn host_from_team(big: &[String], players: &[String]) -> Option<String> {
    let allowed = host_allowed(big);
    let mut keyed: Vec<(String, (i64, i64))> = Vec::new();
    for c in allowed {
        let pings: Vec<Option<i64>> = players.iter().filter(|o| **o != c).map(|o| pair_ping(&c, o)).collect();
        if pings.iter().any(|p| p.is_none()) {
            dlog!("HOST-PICK", "bigger team: ping unknown for", tag(&c), "- queue order");
            return best_host_order(big).first().cloned();
        }
        let p: Vec<i64> = pings.into_iter().flatten().collect();
        keyed.push((c.clone(), hostmig::host_key(&p, perf_penalty(&c))));
    }
    if keyed.is_empty() {
        return None;
    }
    if *MM_PING_GATE && *PING_KICK_MS > 0 {
        let lim = ping_match_ms();
        let fit: Vec<(String, (i64, i64))> = keyed
            .iter()
            .filter(|kc| players.iter().filter(|o| **o != kc.0).map(|o| pair_ping(&kc.0, o).unwrap_or(0)).max().unwrap_or(0) <= lim)
            .cloned()
            .collect();
        if !fit.is_empty() && fit.len() < keyed.len() {
            dlog!("HOST-PICK", format!("bigger team: ping limit {} ms leaves", lim), py::list_str(&fit.iter().map(|kc| tag(&kc.0)).collect::<Vec<_>>()));
            keyed = fit;
        }
    }
    let mut best = keyed.iter().min_by_key(|c| c.1).map(|c| c.0.clone()).unwrap_or_default();
    if hostcap::mode() >= 2 {
        match hostcap::pick(&keyed, players.len() as i64, &|t, n| HOSTCAP.verdict(t, n), None, &|t| tag(t)) {
            Ok((alt, lines)) => {
                dlog!("HOST-PICK", "bigger team cap:", lines.join("; "), "->", tag(alt.as_deref().unwrap_or("")));
                if let Some(a) = alt.filter(|a| !a.is_empty()) {
                    best = a;
                }
            }
            Err(e) => dlog!("HOST-PICK cap ERR", e),
        }
    }
    let s: Vec<String> = keyed.iter().map(|(c, k)| format!("{} worst {} total {}", tag(c), k.0, k.1)).collect();
    dlog!("HOST-PICK", "overall pick is on the smaller team - bigger team", s.join(", "), "->", tag(&best));
    Some(best)
}

// py: daemon.py:6379 _pid_of
pub fn pid_of(t: &str) -> String {
    use sha2::{Digest, Sha256};
    let h = Sha256::digest(t.as_bytes());
    let mut s = String::new();
    for b in h.iter().take(6) {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

// py: daemon.py:6383 teams_live
pub fn teams_live() -> Value {
    let mut out = Vec::new();
    let now = clock::now();
    let g = MM.lock.lock();
    for (mid, pl) in &g.matches {
        if !g.started.contains(mid) || g.ended.contains(mid) {
            continue;
        }
        let dep = g.departed.get(mid);
        let mut teams: [Vec<Value>; 2] = [Vec::new(), Vec::new()];
        for (i, t) in pl.iter().enumerate() {
            if dep.is_some_and(|d| d.contains(t)) || t.starts_with('~') || get_name(t).is_empty() {
                continue;
            }
            let swapped = TEAM_SWAP.lock().get(mid).is_some_and(|m| m.contains_key(t));
            let inmatch = now - TS_CAP.lock().get(t).copied().unwrap_or(0.0) < TS_CAP_S;
            let tm = (team_in(mid, i as i64, Some(t)).clamp(0, 1)) as usize;
            teams[tm].push(json!({"pid": pid_of(t), "name": get_name(t), "slot": i, "swapped": swapped, "inmatch": inmatch}));
        }
        let mv: Vec<Value> = g
            .move_team
            .iter()
            .filter(|(_, v)| &v.0 == mid)
            .map(|(t, v)| {
                let n = get_name(t);
                json!({"pid": pid_of(t), "name": if n.is_empty() { "PLAYER".to_string() } else { n }, "team": v.1})
            })
            .collect();
        let [a, b] = teams;
        out.push(json!({"mid": mid, "teams": [a, b], "moving": mv}));
    }
    Value::Array(out)
}

// py: daemon.py:6400 team_counts_in
pub fn team_counts_in(mid: &str, pl: &[String], dep: &HashSet<String>) -> [i64; 2] {
    let ph = "f0".repeat(30);
    let mut n = [0i64; 2];
    for (i, t) in pl.iter().enumerate() {
        if !dep.contains(t) && !t.starts_with(&ph) {
            n[team_in(mid, i as i64, Some(t)).clamp(0, 1) as usize] += 1;
        }
    }
    n
}

// py: daemon.py:6408 team_swap_now
pub fn team_swap_now(mid: &str, slot: i64, tok: &str, want: i64, why: &str) -> Option<[i64; 2]> {
    let (pl, dep, cur) = {
        let g = MM.lock.lock();
        if !g.matches.contains_key(mid) || g.ended.contains(mid) {
            return None;
        }
        let cur = team_in(mid, slot, Some(tok));
        {
            let mut ts = TEAM_SWAP.lock();
            ts.entry(mid.to_string()).or_default().insert(tok.to_string(), want);
            while ts.len() > 200 {
                ts.shift_remove_index(0);
            }
        }
        (g.matches.get(mid).cloned().unwrap_or_default(), g.departed.get(mid).cloned().unwrap_or_default(), cur)
    };
    {
        let mut ct = CLAN_TEAM.lock();
        if let Some(m) = ct.get_mut(mid) {
            if m.contains_key(tok) {
                m.insert(tok.to_string(), want);
            }
        }
    }
    if INT.has_match(mid) {
        INT.roster(mid, &pl, &|i| team_in(mid, i as i64, pl.get(i).map(|s| s.as_str())));
    }
    let n = team_counts_in(mid, &pl, &dep);
    dlog!(format!("TEAMS swap ({}): {} (match {} slot {}) team {} -> {} IN THE MATCH (its ext applies it; teams now {} v {})", why, tag(tok), mid, slot, cur, want, n[0], n[1]));
    evt!(if why == "admin" { "admin-move-team" } else { "auto-balance" }, tag(tok), mid, format!("team {} -> {} in match (slot {}{})", cur, want, slot, if slot == 0 { ", host" } else { "" }));
    Some(n)
}

pub static AUTO_BALANCE: LazyLock<bool> = LazyLock::new(|| crate::env_on("T2R_AUTO_BALANCE", true));
pub static AUTO_BALANCE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_AUTO_BALANCE_S", 90.0));
pub const AUTO_BALANCE_MIN_LEFT: f64 = 30.0;
/// daemon.py:7110 AUTO_BALANCE_INT_LATE (11-26): no move in an Interrogation match once either team has this much intel
pub static AUTO_BALANCE_INT_LATE: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_AUTO_BALANCE_INT_LATE", 3i64));
pub static AB_NEXT: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:6448 auto_balance_tick
pub fn auto_balance_tick() {
    if !*AUTO_BALANCE {
        return;
    }
    let now = clock::now();
    type Live = (String, Vec<String>, HashSet<String>, Option<(f64, f64, bool, f64)>, bool);
    let live: Vec<Live> = {
        let g = MM.lock.lock();
        g.matches
            .iter()
            .filter(|(mid, _)| g.started.contains(*mid) && !g.ended.contains(*mid))
            .map(|(mid, pl)| {
                (
                    mid.clone(),
                    pl.clone(),
                    g.departed.get(mid).cloned().unwrap_or_default(),
                    g.clock.get(mid).copied(),
                    g.joining.values().any(|(m, _)| m == mid),
                )
            })
            .collect()
    };
    {
        let ids: HashSet<&String> = live.iter().map(|l| &l.0).collect();
        AB_NEXT.lock().retain(|k, _| ids.contains(k));
    }
    let ph = "f0".repeat(30);
    for (mid, pl, dep, ck, joining) in live {
        let nxt = AB_NEXT.lock().get(&mid).copied();
        match nxt {
            None => {
                AB_NEXT.lock().insert(mid.clone(), now + *AUTO_BALANCE_S);
                continue;
            }
            Some(x) if now < x => continue,
            _ => {}
        }
        AB_NEXT.lock().insert(mid.clone(), now + *AUTO_BALANCE_S);
        let mut n = team_counts_in(&mid, &pl, &dep);
        if (n[0] - n[1]).abs() < 2 {
            continue;
        }
        let mut why: Option<String> = None;
        if joining {
            why = Some("a joiner is loading into it".to_string());
        } else if ck.is_some_and(|c| c.2 || (c.1 > 0.0 && c.0 - (now - c.3) < AUTO_BALANCE_MIN_LEFT)) {
            why = Some("the match is ending".to_string());
        } else if INT.lead(&mid).unwrap_or(0) >= *AUTO_BALANCE_INT_LATE && *AUTO_BALANCE_INT_LATE > 0 {
            why = Some(format!("a team has {} of {} intel", INT.lead(&mid).unwrap_or(0), INT.need));
        } else if HM.enabled {
            if let Some(hi) = HM.host_info(&mid) {
                if hi.pending || hi.given_up || hi.alone {
                    why = Some("host migration in progress".to_string());
                }
            }
        }
        if let Some(w) = why {
            dlog!(format!("TEAMS auto-balance: match {} is {} v {} - not now ({})", mid, n[0], n[1], w));
            continue;
        }
        let hslot = if HM.enabled { HM.host_info(&mid).map(|h| h.host).unwrap_or(0) } else { 0 };
        let mut moved_any = false;
        while (n[0] - n[1]).abs() >= 2 {
            let big = if n[0] > n[1] { 0 } else { 1 };
            let sw: HashMap<String, i64> = TEAM_SWAP.lock().get(&mid).cloned().unwrap_or_default();
            let mut cands: Vec<(bool, i64, usize, String)> = Vec::new();
            for (i, t) in pl.iter().enumerate() {
                if dep.contains(t) || t.starts_with(&ph) || team_in(&mid, i as i64, Some(t)) != big {
                    continue;
                }
                if i == hslot || now - TS_CAP.lock().get(t).copied().unwrap_or(0.0) >= TS_CAP_S {
                    continue;
                }
                let pid = PARTIES.pid(t).filter(|p| !p.is_empty());
                if let Some(pid) = pid {
                    if pl.iter().enumerate().any(|(j, u)| u != t && !dep.contains(u) && PARTIES.pid(u).as_deref() == Some(pid.as_str()) && team_in(&mid, j as i64, Some(u)) == big) {
                        continue;
                    }
                }
                cands.push((sw.contains_key(t), -(i as i64), i, t.clone()));
            }
            if cands.is_empty() {
                dlog!(format!("TEAMS auto-balance: match {} is {} v {} - nobody can move (host / older game version / party mates)", mid, n[0], n[1]));
                break;
            }
            let (_, _, i, t) = cands.into_iter().min().unwrap_or((false, 0, 0, String::new()));
            match team_swap_now(&mid, i as i64, &t, 1 - big, "auto-balance") {
                None => break,
                Some(r) => {
                    n = r;
                    moved_any = true;
                }
            }
        }
        if moved_any {
            dlog!(format!("TEAMS auto-balance: match {} now {} v {}", mid, n[0], n[1]));
        }
    }
}

// py: daemon.py:6509 admin_move_team
pub fn admin_move_team(pid: &str) -> Value {
    let pid = pid.to_lowercase();
    if !(4..=64).contains(&pid.chars().count()) || pid.chars().any(|c| !"0123456789abcdef".contains(c)) {
        return json!({"ok": false, "error": "bad pid"});
    }
    let hit: Vec<(String, usize, String)> = {
        let g = MM.lock.lock();
        let mut v = Vec::new();
        for (mid, pl) in &g.matches {
            if !g.started.contains(mid) || g.ended.contains(mid) {
                continue;
            }
            for (i, t) in pl.iter().enumerate() {
                if !g.departed.get(mid).is_some_and(|d| d.contains(t)) && pid_of(t).starts_with(&pid) {
                    v.push((mid.clone(), i, t.clone()));
                }
            }
        }
        v
    };
    if hit.len() != 1 {
        return json!({"ok": false, "error": if hit.is_empty() { "player is not in a running match" } else { "ambiguous pid" }});
    }
    let (mid, slot, tok) = hit[0].clone();
    let now = clock::now();
    let cur = team_in(&mid, slot as i64, Some(&tok));
    let want = 1 - cur;
    let cap_t = TS_CAP.lock().get(&tok).copied().unwrap_or(0.0);
    let nm = {
        let n = get_name(&tok);
        if n.is_empty() { "PLAYER".to_string() } else { n }
    };
    if now - cap_t < TS_CAP_S {
        return match team_swap_now(&mid, slot as i64, &tok, want, "admin") {
            None => json!({"ok": false, "error": "the match just ended"}),
            Some(_) => json!({"ok": true, "name": nm, "to_team": want, "match": mid, "mode": "in-match", "note": "stays in the match on the other team (applied within ~3 s)"}),
        };
    }
    if slot == 0 {
        return json!({"ok": false, "error": format!("{} hosts this match and its game is too old to switch teams in the match (no t2r-ext team swap seen in the last {} s) - a host cannot be kicked to rejoin", nm, TS_CAP_S as i64)});
    }
    MM.lock.lock().move_team.insert(tok.clone(), (mid.clone(), want, clock::now() + 900.0));
    CLAN_MOVED.lock().insert((tok.clone(), mid.clone()), clock::now());
    let r = admin_kick(&pid, true);
    let okv = match r.get("ok") {
        None => true,
        Some(Value::Bool(b)) => *b,
        Some(Value::Null) => false,
        Some(Value::Number(n)) => n.as_f64() != Some(0.0),
        Some(_) => true,
    };
    if !okv && r.get("error").is_some() {
        MM.lock.lock().move_team.shift_remove(&tok);
        CLAN_MOVED.lock().remove(&(tok.clone(), mid.clone()));
        return r;
    }
    dlog!(format!("TEAMS move: admin moves {} (match {} slot {} team {}) to team {} - its client searches again by itself (join in progress)", tag(&tok), mid, slot, slot % 2, want));
    evt!("admin-move-team", tag(&tok), mid, format!("team {} -> {}", slot % 2, want));
    json!({"ok": true, "name": nm, "to_team": want, "match": mid, "mode": "rejoin", "note": "older game version: sent to the menu, it re-joins this match on the other team by itself"})
}

// py: daemon.py:6560 teams_preview
pub fn teams_preview() -> Value {
    let q = MM.queued();
    let nm = |t: &String| {
        let n = get_name(t);
        if n.is_empty() { "PLAYER".to_string() } else { n }
    };
    if q.len() < 2 {
        return json!({"queue": q.iter().map(nm).collect::<Vec<_>>(), "t0": [], "t1": []});
    }
    let sk = player_skill(&q);
    let party_of: HashMap<String, String> = q.iter().filter_map(|t| PARTIES.pid(t).filter(|p| !p.is_empty()).map(|p| (t.clone(), p))).collect();
    let (t0, t1) = split_teams(&q, &party_of, Some(&sk), Some(&top_players(&q)), Some(&rule_groups(&q)));
    let lab = |t: &String| format!("{} ({})", nm(t), py::fixed(sk.get(t).copied().unwrap_or(SKILL_NEW), 2));
    json!({"queue": q.iter().map(nm).collect::<Vec<_>>(), "t0": t0.iter().map(lab).collect::<Vec<_>>(), "t1": t1.iter().map(lab).collect::<Vec<_>>()})
}

// ---------------------------------------------------------------- parties, social host
// py: daemon.py:6575 token_by_name
pub fn token_by_name(name: &[u8]) -> Option<String> {
    let name = py::strip(&utf8_ignore(name)).to_lowercase();
    if name.is_empty() {
        return None;
    }
    PRES_TOKENS().into_iter().find(|t| get_name(t).to_lowercase() == name)
}

// py: daemon.py:6574 PARTIES
pub static PARTIES: LazyLock<Arc<Parties>> = LazyLock::new(|| {
    Arc::new(Parties::new(Some(std::cmp::max(1, *MATCH_SIZE / 2)), Some(Box::new(|name: &[u8]| token_by_name(name)))))
});

static TID_SID_CACHE: LazyLock<Mutex<HashMap<String, (f64, u64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
const TID_SID_TTL: f64 = 30.0;

// py: daemon.py:6580 install_steamid
pub fn install_steamid(token: &str) -> u64 {
    let tid = crate::names::token_id(token);
    if tid.chars().count() != 24 {
        return 0;
    }
    let now = clock::now();
    if let Some(h) = TID_SID_CACHE.lock().get(&tid) {
        if now - h.0 < TID_SID_TTL {
            return h.1;
        }
    }
    let rows = db::db_read(|c| {
        let mut s = c.prepare("SELECT DISTINCT steamid FROM players WHERE substr(lower(token),41,24)=? AND steamid>0 LIMIT 2")?;
        let v = s.query_map([&tid], |r| r.get::<_, i64>(0))?.collect::<Result<Vec<_>, _>>()?;
        Ok(v)
    });
    let Ok(rows) = rows else { return 0 };
    let sid = if rows.len() == 1 { rows[0] as u64 } else { 0 };
    let mut c = TID_SID_CACHE.lock();
    if c.len() > 5000 {
        c.clear();
    }
    c.insert(tid, (now, sid));
    sid
}

/// a connection of its own (`dbc()`), never under the daemon's database lock
fn dbc() -> rusqlite::Result<rusqlite::Connection> {
    let c = rusqlite::Connection::open(&db::CFG.path)?;
    c.busy_timeout(std::time::Duration::from_secs(10))?;
    c.execute_batch("PRAGMA synchronous=FULL")?;
    Ok(c)
}

// py: daemon.py:6602 class _SocialHost
pub struct SocialHostImpl;

impl SocialHost for SocialHostImpl {
    fn now(&self) -> f64 {
        clock::now()
    }
    fn log(&self, parts: &[String]) {
        let v: Vec<&dyn std::fmt::Display> = parts.iter().map(|p| p as &dyn std::fmt::Display).collect();
        crate::log::line(&v);
    }
    fn with_db(&self, f: &mut dyn FnMut(&rusqlite::Connection) -> rusqlite::Result<()>) -> rusqlite::Result<()> {
        let c = dbc()?;
        c.execute_batch("BEGIN")?;
        match f(&c) {
            Ok(()) => c.execute_batch("COMMIT"),
            Err(e) => {
                let _ = c.execute_batch("ROLLBACK");
                Err(e)
            }
        }
    }
    fn steamid(&self, token: &str) -> u64 {
        let a = token_steamid(token);
        if a != 0 {
            return a;
        }
        let b = BRIDGES.steamid(token);
        if b != 0 {
            return b;
        }
        install_steamid(token)
    }
    fn name(&self, token: &str) -> String {
        get_name(token)
    }
    fn game_tokens(&self) -> Vec<String> {
        PRES_TOKENS()
    }
    fn alive_tokens(&self) -> HashSet<String> {
        let mut alive: HashSet<String> = PRES_TOKENS().into_iter().collect();
        // (the Python reads MM's dicts without the lock: this runs inside a database transaction while try_form holds MM.lock and waits for the database)
        if let Some(g) = MM.lock.try_lock_for(std::time::Duration::from_millis(50)) {
            alive.extend(g.queue.keys().cloned());
            for pl in g.matches.values() {
                alive.extend(pl.iter().cloned());
            }
        }
        alive
    }
    fn mm_state(&self, token: &str) -> Option<&'static str> {
        let g = MM.lock.try_lock_for(std::time::Duration::from_millis(50))?;
        for (mid, pl) in &g.matches {
            if pl.iter().any(|x| x == token) && !g.departed.get(mid).is_some_and(|d| d.contains(token)) && !g.ended.contains(mid) {
                return Some("match");
            }
        }
        if g.queue.contains_key(token) && clock::now() - g.seen.get(token).copied().unwrap_or(0.0) < MatchMaker::QUEUE_TTL {
            Some("search")
        } else {
            None
        }
    }
}

// py: daemon.py:6643 _lobby_chat_key
pub fn lobby_chat_key(tok: &str) -> Option<String> {
    if tok.is_empty() {
        return None;
    }
    let g = MM.lock.lock();
    if book_on() {
        if let Some(lid) = g.book.lobby_of(tok) {
            return Some(lid.to_string());
        }
        let m = if *REQUEUE && *LOBBY_KEEP { g.rq.mid_of(tok).filter(|m| !m.is_empty()) } else { None };
        if let Some(m) = m {
            return Some(g.book.key_for_mid(&m));
        }
        return g.mid_locked(tok).and_then(|cm| g.book.mid_lid.get(&cm).cloned());
    }
    if g.queue.contains_key(tok) {
        return Some("queue".to_string());
    }
    if *REQUEUE && *LOBBY_KEEP {
        g.rq.mid_of(tok)
    } else {
        None
    }
}

// py: daemon.py:6674 _chat_match_of
pub fn chat_match_of(tok: &str) -> Option<String> {
    MM.lock.lock().mid_locked(tok)
}

// py: daemon.py:6641 SOCIAL
pub static SOCIAL: LazyLock<Social> = LazyLock::new(|| {
    let mut s = Social::new(Arc::new(SocialHostImpl), PARTIES.clone());
    s.lobby_of = Some(Box::new(|t: &str| lobby_chat_key(t)));
    s.match_of = Some(Box::new(|t: &str| chat_match_of(t)));
    let dbdir = std::path::Path::new(&db::CFG.path).parent().map(|p| p.to_path_buf()).unwrap_or_default();
    s.mute_path = Some(dbdir.join("mutes.txt"));
    s.automod_path = Some(dbdir.join("automod.json")); // automatic slur strikes + mutes (social.py)
    crate::slurs::load_extra(&dbdir.join("slurs.txt")); // extra patterns (slurs.py)
    s.vban_path = Some(dbdir.join("voicebans.txt")); // voice bans (the admin site writes it, like mutes.txt)
    s.match_peers = Some(Box::new(|t: &str| Some(chat_match_peers(t))));
    s
});

// py: daemon.py:7341 _chat_match_peers
/// The tokens of the token's current match, departed ones left out (S_MUTE "vbanned", S_VSPAM).
pub fn chat_match_peers(tok: &str) -> Vec<String> {
    let g = MM.lock.lock();
    let Some(mid) = g.mid_locked(tok) else { return Vec::new() };
    let gone = g.departed.get(&mid);
    g.matches.get(&mid).map(|pl| pl.iter().filter(|t| !gone.is_some_and(|d| d.contains(*t))).cloned().collect()).unwrap_or_default()
}

// py: daemon.py:6682 party_op
pub fn party_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 65 {
        return None;
    }
    let token = latin1(&pl[..64]);
    if !token.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let (sub, arg) = (pl[64], &pl[65..]);
    let mut ok = true;
    match sub {
        1 => ok = PARTIES.invite(&token, arg),
        2 => ok = PARTIES.accept(&token, arg),
        3 => {
            PARTIES.leave(&token);
        }
        _ => {}
    }
    SOCIAL.drain();
    Some(msg(0xDB, format!("ok={} {}", if ok { 1 } else { 0 }, PARTIES.text(&token, &|t| get_name(t))).as_bytes()))
}

// py: daemon.py:6699 social_op
pub fn social_op(pl: &[u8], addr: Option<&Addr>) -> Option<Vec<u8>> {
    if pl.len() < 67 {
        return None;
    }
    let token = latin1(&pl[..64]);
    if !is_token(&token) || pl[64] != 1 {
        return None;
    }
    let (lang, sub) = (pl[65], pl[66] as i64);
    let arg = utf8_ignore(&pl[67..pl.len().min(67 + 420)]);
    if social::is_write_sub(sub) && !new_identity_ok(&token, addr) {
        dlog!("NEWID", addr.map(|a| pa(*a).to_string()).unwrap_or_else(|| "None".into()), "op 0xD8 refused: too many new identities from this address");
        return Some(msg(social::OP_REPLY, &social::reply_json(&json!({"ok": 0, "err": "rate"}))));
    }
    let r = SOCIAL.handle(&token, if lang == 1 { 1 } else { 0 }, sub, &arg)?;
    Some(msg(social::OP_REPLY, &social::reply_json(&r)))
}

// py: daemon.py:6712 PRES_TOKENS
#[allow(non_snake_case)]
pub fn PRES_TOKENS() -> Vec<String> {
    let now = clock::now();
    PRES.players_last().into_iter().filter(|(_, last)| now - last < *KEEPALIVE_TTL).map(|(t, _)| t).collect()
}
