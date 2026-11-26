//! `t2r-daemon selftest k4`: the Rust side of verify/py/k4_harness.py (differential test of the pure parts of the K4 chunk).
use super::*;
use crate::c_util::{emit, script};
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};

static ALL: parking_lot::Mutex<Vec<String>> = parking_lot::Mutex::new(Vec::new());

fn strs(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}

fn skill_map(v: &Value) -> Option<HashMap<String, f64>> {
    v.as_object().map(|m| m.iter().map(|(k, x)| (k.clone(), x.as_f64().unwrap_or(0.0))).collect())
}

fn rg_of(v: &Value) -> Option<RuleGroups> {
    v.as_array().map(|a| a.iter().map(|x| (x[0].as_str().unwrap_or("").to_string(), strs(&x[1]))).collect())
}

fn party_map(v: &Value) -> HashMap<String, String> {
    v.as_object().map(|m| m.iter().map(|(k, x)| (k.clone(), x.as_str().unwrap_or("").to_string())).collect()).unwrap_or_default()
}

fn jv(v: &[String]) -> Value {
    Value::Array(v.iter().map(|s| Value::String(s.clone())).collect())
}

fn set_mtime(p: &std::path::Path, secs: u64) {
    if let Ok(f) = std::fs::OpenOptions::new().write(true).open(p) {
        let _ = f.set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(secs));
    }
}

static MTIME: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1000);

fn world(w: &Value) {
    let tools = crate::gen_modes::tools_dir();
    let _ = std::fs::create_dir_all(&tools);
    for (fn_, key) in [("team_rules.txt", "rules"), ("top_players.txt", "top")] {
        let p = tools.join(fn_);
        match w.get(key).and_then(|v| v.as_str()) {
            None => {
                let _ = std::fs::remove_file(&p);
            }
            Some(t) => {
                let _ = std::fs::write(&p, t.as_bytes());
                let m = MTIME.fetch_add(10, std::sync::atomic::Ordering::Relaxed) + 10;
                set_mtime(&p, m);
            }
        }
    }
    *TOP_CACHE.lock() = (None, HashSet::new());
    *RULES_CACHE.lock() = (None, Vec::new());
    SKILL_CACHE.lock().clear();
    {
        let mut wd = WORLD.lock();
        wd.names = w["names"].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string())).collect()).unwrap_or_default();
        wd.sids = w["sids"].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_u64().unwrap_or(0))).collect()).unwrap_or_default();
        wd.pings = w["pings"]
            .as_object()
            .map(|m| {
                m.iter()
                    .filter_map(|(k, v)| {
                        let (a, b) = k.split_once('|')?;
                        Some(((a.to_string(), b.to_string()), v.as_i64()?))
                    })
                    .collect()
            })
            .unwrap_or_default();
        wd.noh = strs(&w["noh"]).into_iter().collect();
        wd.bho = strs(&w["bho"]);
        wd.perf = w["perf"].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_i64().unwrap_or(0))).collect()).unwrap_or_default();
        wd.lim = w["lim"].as_i64().unwrap_or(175);
    }
    if let Ok(c) = rusqlite::Connection::open(&db::CFG.path) {
        let _ = c.execute_batch(
            "CREATE TABLE IF NOT EXISTS match_stats(token TEXT, kills INT, deaths INT, ts REAL);
             CREATE TABLE IF NOT EXISTS players(token TEXT PRIMARY KEY, name TEXT, loadout BLOB, kills INT DEFAULT 0, deaths INT DEFAULT 0, wins INT DEFAULT 0, matches INT DEFAULT 0, steamid INTEGER DEFAULT 0, last_seen REAL);
             DELETE FROM match_stats; DELETE FROM players;",
        );
        for p in w["players"].as_array().cloned().unwrap_or_default() {
            let _ = c.execute("INSERT INTO players(token, steamid) VALUES (?,?)", rusqlite::params![p[0].as_str().unwrap_or(""), p[1].as_i64().unwrap_or(0)]);
        }
        for s in w["stats"].as_array().cloned().unwrap_or_default() {
            let _ = c.execute(
                "INSERT INTO match_stats(token, kills, deaths, ts) VALUES (?,?,?,?)",
                rusqlite::params![s[0].as_str().unwrap_or(""), s[1].as_i64(), s[2].as_i64(), s[3].as_f64()],
            );
        }
    }
    {
        let mut wd = WORLD.lock();
        wd.online = strs(&w["online"]);
        wd.regions = w["regions"].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string())).collect()).unwrap_or_default();
    }
    WORLD_ON.store(true, std::sync::atomic::Ordering::Relaxed);
    *ALL.lock() = strs(&w["all"]);
    *MM.lock.lock() = MMState::new();
    MATCH_MAP.lock().clear();
    JIP_BACK.lock().clear();
    TEAM_SWAP.lock().clear();
    PING_KICKED.lock().clear();
    for t in strs(&w["all"]) {
        PARTIES.leave(&t);
    }
    for g in w["parties"].as_array().cloned().unwrap_or_default() {
        let g = strs(&g);
        for m in &g[1..] {
            PARTIES.invite_token(&g[0], m);
            let _ = PARTIES.accept_token(m, &g[0]);
        }
    }
}

fn sorted_map<V: Clone>(m: &HashMap<String, V>, f: &dyn Fn(&V) -> Value) -> Value {
    let mut ks: Vec<&String> = m.keys().collect();
    ks.sort();
    Value::Object(ks.into_iter().map(|k| (k.clone(), f(&m[k]))).collect())
}

fn sorted_set(m: &HashSet<String>) -> Value {
    let mut v: Vec<&String> = m.iter().collect();
    v.sort();
    json!(v)
}

fn mmstate() -> Value {
    let g = MM.lock.lock();
    let jb = JIP_BACK.lock();
    let mm = MATCH_MAP.lock();
    let mut mmv: Vec<(Vec<String>, i64)> = mm.iter().map(|(k, v)| (k.iter().cloned().collect(), *v)).collect();
    mmv.sort();
    let lobbies: HashMap<String, crate::lobbybook::Lobby> = g.book.lobbies.iter().map(|(k, l)| (k.clone(), l.clone())).collect();
    json!({
        "queue": g.queue.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
        "seen": sorted_map(&g.seen, &|v| json!(v)),
        "matches": g.matches.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
        "formed": sorted_map(&g.formed, &|v| json!(v)),
        "left": sorted_map(&g.left, &|v| json!([v.0, v.1])),
        "settle": sorted_map(&g.settle, &|v| json!(v)),
        "qtime": sorted_map(&g.qtime, &|v| Value::Array(v.iter().map(|(a, b)| json!([a, b])).collect())),
        "launched": sorted_set(&g.launched),
        "departed": sorted_map(&g.departed, &|v| sorted_set(v)),
        "ended": sorted_set(&g.ended),
        "ended_at": sorted_map(&g.ended_at, &|v| json!(v)),
        "started": sorted_set(&g.started),
        "started_t": sorted_map(&g.started_t, &|v| json!(v)),
        "clock": sorted_map(&g.clock, &|v| json!([v.0, v.1, v.2, v.3])),
        "polled": sorted_map(&g.polled, &|v| sorted_map(v, &|x| json!(x))),
        "joining": g.joining.iter().map(|(k, v)| json!([k, [v.0, v.1]])).collect::<Vec<_>>(),
        "lives": sorted_map(&g.lives, &|v| json!([v.0, v.1, v.2])),
        "late_bonus": sorted_map(&g.late_bonus, &|v| json!([v.0, v.1])),
        "late_mult": sorted_map(&g.late_mult, &|v| json!([v.0, v.1])),
        "move_team": g.move_team.iter().map(|(k, v)| json!([k, [v.0, v.1, v.2]])).collect::<Vec<_>>(),
        "departed_t": sorted_map(&g.departed_t, &|v| json!(v)),
        "departed_vol": sorted_set(&g.departed_vol),
        "last_arrival": g.last_arrival,
        "hold": g.hold_since,
        "book": sorted_map(&lobbies, &|l| json!([l.members, l.hold, l.wait_until])),
        "bookof": sorted_map(&g.book.of, &|v| json!(v)),
        "jip_back": sorted_map(&jb, &|v| json!([v.0, v.1])),
        "mm_map": mmv.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
    })
}

fn formed_json(f: Formed) -> Value {
    match f {
        Some((m, p)) => json!([m, p]),
        None => json!([null, null]),
    }
}

fn mm_op(c: &Value) -> Value {
    let k = c["m"].as_str().unwrap_or("");
    let tok = c["tok"].as_str().unwrap_or("").to_string();
    let mids: Vec<String> = MM.lock.lock().matches.keys().cloned().collect();
    let mid = match c.get("i").and_then(|v| v.as_u64()) {
        Some(i) if !mids.is_empty() => mids[i as usize % mids.len()].clone(),
        _ => "none".to_string(),
    };
    match k {
        "enqueue" => formed_json(MM.enqueue(&tok)),
        "poll" => {
            MM.poll(&tok);
            Value::Null
        }
        "pollall" => {
            let q: Vec<String> = MM.lock.lock().queue.keys().cloned().collect();
            for t in q {
                MM.poll(&t);
            }
            Value::Null
        }
        "try_form" => formed_json(MM.try_form()),
        "cancel" => json!(MM.cancel(&tok)),
        "dequeue" => json!(MM.dequeue(&tok)),
        "leave" => {
            MM.leave(&tok);
            Value::Null
        }
        "depart" => json!(MM.depart(&tok, c["vol"].as_bool().unwrap_or(false))),
        "end" => {
            MM.end(&mid);
            Value::Null
        }
        "abort" => json!(MM.abort(&mid, None)),
        "drop" => {
            MM.drop_mid(&mid);
            Value::Null
        }
        "setmode" => json!(MODEQ.set(&tok, c["mode"].as_i64().unwrap_or(0))),
        "start" => {
            let mut g = MM.lock.lock();
            if let Some(pl) = g.matches.get(&mid).cloned() {
                g.started.insert(mid.clone());
                g.started_t.insert(mid.clone(), crate::clock::now());
                g.launched.insert(mid.clone());
                MATCH_MAP.lock().insert(key_of(&pl), c["map"].as_i64().unwrap_or(0));
            }
            json!(mid)
        }
        "jip_clock" => json!(MM.jip_clock(&tok, c["rem"].as_f64().unwrap_or(0.0), c["dur"].as_f64().unwrap_or(0.0), c["ended"].as_bool().unwrap_or(false))),
        "clockm" => {
            let first = MM.lock.lock().matches.get(&mid).and_then(|p| p.first().cloned());
            match first {
                Some(t) => json!(MM.jip_clock(&t, c["rem"].as_f64().unwrap_or(0.0), c["dur"].as_f64().unwrap_or(0.0), c["ended"].as_bool().unwrap_or(false))),
                None => Value::Null,
            }
        }
        "departm" => {
            let pl = MM.lock.lock().matches.get(&mid).cloned();
            match pl {
                Some(p) if !p.is_empty() => json!(MM.depart(&p[c["j"].as_u64().unwrap_or(0) as usize % p.len()], c["vol"].as_bool().unwrap_or(false))),
                _ => Value::Null,
            }
        }
        "modes" => {
            let all = ALL.lock().clone();
            let a: serde_json::Map<String, Value> = all.iter().map(|t| (t[..2].to_string(), json!(_search_mode(t)))).collect();
            let b: serde_json::Map<String, Value> = all.iter().map(|t| (t[..2].to_string(), json!([MODEQ.pref_of(t).map(|p| json!([p.0, p.1]))]))).collect();
            json!([a, b])
        }
        "freshjoin" => {
            let all = ALL.lock().clone();
            let tk = {
                let g = MM.lock.lock();
                all.iter().find(|t| !g.queue.contains_key(*t) && !g.in_match_locked(t)).cloned()
            };
            match tk {
                None => Value::Null,
                Some(tk) => {
                    let e = formed_json(MM.enqueue(&tk));
                    let q: Vec<String> = MM.lock.lock().queue.keys().cloned().collect();
                    for t in q {
                        MM.poll(&t);
                    }
                    let first = MM.lock.lock().matches.get(&mid).and_then(|p| p.first().cloned());
                    if let Some(f) = first {
                        MM.jip_clock(&f, c["rem"].as_f64().unwrap_or(0.0), c["dur"].as_f64().unwrap_or(0.0), c["ended"].as_bool().unwrap_or(false));
                    }
                    let mut why: Vec<String> = Vec::new();
                    let j = MM.jip_join(&tk, &mut why, c["need"].as_i64().unwrap_or(1), None, None);
                    let j = j.map(|(m, s, p, k)| json!([m, s, p, k.iter().cloned().collect::<Vec<_>>()]));
                    json!([tk, e, j, why])
                }
            }
        }
        "jip_lives" => {
            MM.jip_lives(&tok, c["team"].as_i64().unwrap_or(0), c["ally"].as_i64().unwrap_or(0), c["enemy"].as_i64().unwrap_or(0));
            Value::Null
        }
        "jip_ready" => json!(MM.jip_ready()),
        "jip_join" => {
            let mut why: Vec<String> = Vec::new();
            let r = MM.jip_join(&tok, &mut why, c["need"].as_i64().unwrap_or(1), c.get("only").and_then(|v| v.as_str()), c.get("team").and_then(|v| v.as_i64()));
            let r = r.map(|(m, s, p, k)| json!([m, s, p, k.iter().cloned().collect::<Vec<_>>()]));
            json!([r, why])
        }
        "jip_acked" => json!(MM.jip_acked(&tok)),
        "jip_done" => match MM.jip_done(&tok) {
            Some((m, t)) => json!([m, t]),
            None => Value::Null,
        },
        "late" => json!([MM.late_mult_take(&tok, &mid), MM.late_bonus_take(&tok, &mid)]),
        "query" => {
            let (am, ap) = MM.active_of(&tok);
            let mv = MM.mesh_view();
            let mut q: Vec<String> = mv.0.into_iter().collect();
            q.sort();
            let mut lv: Vec<(String, String)> = mv.1.into_iter().collect();
            lv.sort();
            let snap: Vec<Value> = MM
                .snapshot()
                .into_iter()
                .map(|x| {
                    let mut d: Vec<String> = x.6.into_iter().collect();
                    d.sort();
                    json!([x.0, x.1, x.2, x.3, x.4, x.5, d])
                })
                .collect();
            json!([MM.in_match(&tok), MM.players_of(&tok), MM.mid_of(&tok), [am, ap], MM.queued(), MM.loading(&tok, 5.0), MM.countdown_over(&tok),
                MM.all_loaded(&tok), MM.jip_can_join(&tok, None, 1), MM.jip_age(&tok), q, lv, snap, MM.jip_joiners(&mid)])
        }
        "gc" => json!(MM.gc(&strs(&c["keep"]).into_iter().collect(), None)),
        _ => Value::Null,
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    std::env::set_var("T2R_HOST_CAP", "0");
    let mut n = 0usize;
    for c in script(input) {
        n += 1;
        if let Some(t) = c.get("t").and_then(|v| v.as_f64()) {
            crate::clock::freeze(t);
        }
        let op = c["op"].as_str().unwrap_or("");
        if op == "world" {
            world(&c["w"]);
            emit(out, n, Value::Null, Value::Null);
            continue;
        }
        if op == "mm" {
            let r = mm_op(&c);
            emit(out, n, r, mmstate());
            continue;
        }
        let a = c["a"].as_array().cloned().unwrap_or_default();
        let f = c["f"].as_str().unwrap_or("");
        let r: Value = match f {
            "skill_of_kd" => json!(skill_of_kd(a[0].as_i64().unwrap_or(0), a[1].as_i64().unwrap_or(0))),
            "player_skill" => {
                let m = player_skill(&strs(&a[0]));
                Value::Object(m.into_iter().map(|(k, v)| (k, json!(v))).collect())
            }
            "rule_match" => jv(&rule_match(a[0].as_str().unwrap_or(""), &strs(&a[1]), None)),
            "rule_groups" => Value::Array(rule_groups(&strs(&a[0])).into_iter().map(|(k, g)| json!([k, g])).collect()),
            "rule_team_for" => {
                let teams_of: indexmap::IndexMap<String, i64> =
                    a[1].as_object().map(|m| m.iter().map(|(k, v)| (k.clone(), v.as_i64().unwrap_or(0))).collect()).unwrap_or_default();
                let (t, note) = rule_team_for(a[0].as_str().unwrap_or(""), &teams_of, &rg_of(&a[2]).unwrap_or_default());
                json!([t, note])
            }
            "split_teams" => {
                let top: Option<HashSet<String>> = a[3].as_array().map(|_| strs(&a[3]).into_iter().collect());
                let sk = skill_map(&a[2]);
                let (t0, t1) = split_teams(&strs(&a[0]), &party_map(&a[1]), sk.as_ref(), top.as_ref(), rg_of(&a[4]).as_ref());
                json!([t0, t1])
            }
            "order_slots" => jv(&order_slots(&strs(&a[0]), &strs(&a[1]), a[2].as_str())),
            "host_swap_pick" => {
                let top: HashSet<String> = strs(&a[5]).into_iter().collect();
                let sk = skill_map(&a[4]);
                match host_swap_pick(a[0].as_str().unwrap_or(""), &strs(&a[1]), &strs(&a[2]), &strs(&a[3]), sk.as_ref(), &top, &rg_of(&a[6]).unwrap_or_default(), &party_map(&a[7])) {
                    None => Value::Null,
                    Some((x, y)) => json!([x, y]),
                }
            }
            "host_from_team" => match host_from_team(&strs(&a[0]), &strs(&a[1])) {
                Some(h) => Value::String(h),
                None => Value::Null,
            },
            "team_order" => jv(&team_order(&strs(&a[0]))),
            _ => {
                eprintln!("unknown f {}", f);
                return 2;
            }
        };
        emit(out, n, r, Value::Null);
    }
    0
}
