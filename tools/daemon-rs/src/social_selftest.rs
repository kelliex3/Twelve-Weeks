//! Differential test of social.rs against social.py: `t2r-daemon selftest social < script`.
//! verify/py/social_harness.py is the Python side; both print the same bytes for the same script.
//!
//! Script lines (JSON): {"m":"social","op":<name>,"t":<frozen time>, ...}. The "world" op replaces what the host reports (game tokens, matchmaker
//! states, Steam ids, shown names, alive tokens). Every op prints "R <json>", then one "L <text>" line per log line it produced.
//! The folder is $T2R_VERIFY_DIR/social (database t.db, mutes.txt).
use parking_lot::Mutex;
use rusqlite::Connection;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

use crate::py::dumps_compact;
use crate::social::*;

#[derive(Default)]
struct World {
    game: Vec<String>,
    mm: HashMap<String, String>,
    sid: HashMap<String, u64>,
    names: HashMap<String, String>,
    alive: Vec<String>,
    logs: Vec<String>,
}

struct Host {
    clock: Arc<AtomicU64>,
    db: PathBuf,
    w: Mutex<World>,
}

impl SocialHost for Host {
    fn now(&self) -> f64 {
        f64::from_bits(self.clock.load(Ordering::Relaxed))
    }
    fn log(&self, parts: &[String]) {
        self.w.lock().logs.push(parts.join(" "));
    }
    fn with_db(&self, f: &mut dyn FnMut(&Connection) -> rusqlite::Result<()>) -> rusqlite::Result<()> {
        let c = Connection::open(&self.db)?;
        f(&c)
    }
    fn steamid(&self, token: &str) -> u64 {
        self.w.lock().sid.get(token).copied().unwrap_or(0)
    }
    fn name(&self, token: &str) -> String {
        self.w.lock().names.get(token).cloned().unwrap_or_default()
    }
    fn game_tokens(&self) -> Vec<String> {
        self.w.lock().game.clone()
    }
    fn alive_tokens(&self) -> HashSet<String> {
        self.w.lock().alive.iter().cloned().collect()
    }
    fn mm_state(&self, token: &str) -> Option<&'static str> {
        match self.w.lock().mm.get(token).map(|s| s.as_str()) {
            Some("match") => Some("match"),
            Some("search") => Some("search"),
            _ => None,
        }
    }
}

fn strs(v: &Value) -> Vec<String> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}

fn opt(o: Option<&str>) -> Value {
    match o {
        Some(s) => json!(s),
        None => Value::Null,
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let dir = crate::selftest::tmp_dir("social");
    let dbp = dir.join("t.db");
    let mutes = dir.join("mutes.txt");
    let clock = Arc::new(AtomicU64::new(0f64.to_bits()));
    let host = Arc::new(Host { clock: clock.clone(), db: dbp.clone(), w: Mutex::new(World::default()) });
    let hc = host.clone();
    let lookup: Lookup = Box::new(move |name: &[u8]| {
        let n = crate::roles::utf8_ignore(name);
        let n = crate::py::strip(&n).to_lowercase();
        if n.is_empty() {
            return None;
        }
        let w = hc.w.lock();
        for t in &w.game {
            if w.names.get(t).map(|x| x.to_lowercase()).unwrap_or_default() == n {
                return Some(t.clone());
            }
        }
        None
    });
    let c2 = clock.clone();
    let max_size: i64 = std::env::var("T2R_SOCIAL_MAX").ok().and_then(|v| v.parse().ok()).unwrap_or(4);
    let parties = Arc::new(Parties::with_clock(Some(max_size), Some(lookup), Box::new(move || f64::from_bits(c2.load(Ordering::Relaxed)))));
    let mut first = true;
    let mut social: Option<Social> = None;
    let lobby_map: Arc<Mutex<HashMap<String, String>>> = Arc::new(Mutex::new(HashMap::new()));
    let match_map: Arc<Mutex<HashMap<String, String>>> = Arc::new(Mutex::new(HashMap::new()));
    let mut line = String::new();
    loop {
        line.clear();
        match input.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let Ok(v) = serde_json::from_str::<Value>(line.trim()) else { continue };
        if v["m"].as_str() != Some("social") {
            continue;
        }
        let t = v.get("t").and_then(|x| x.as_f64()).unwrap_or(0.0);
        clock.store(t.to_bits(), Ordering::Relaxed);
        if first {
            first = false;
            let mut s = Social::new(host.clone(), parties.clone());
            let lm = lobby_map.clone();
            s.lobby_of = Some(Box::new(move |tok: &str| lm.lock().get(tok).cloned()));
            let mm = match_map.clone();
            s.match_of = Some(Box::new(move |tok: &str| mm.lock().get(tok).cloned()));
            s.mute_path = Some(mutes.clone());
            social = Some(s);
        }
        let s = social.as_ref().unwrap();
        let st = |k: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or("").to_string();
        let ints = |k: &str| v.get(k).and_then(|x| x.as_i64()).unwrap_or(0);
        let flt = |k: &str| v.get(k).and_then(|x| x.as_f64());
        let queued_set: HashSet<String> = strs(&v["queued"]).into_iter().collect();
        let queued = |m: &str| queued_set.contains(m);
        let op = v["op"].as_str().unwrap_or("").to_string();
        let res: Value = match op.as_str() {
            "world" => {
                let mut w = host.w.lock();
                if v.get("game").is_some() {
                    w.game = strs(&v["game"]);
                }
                if v.get("alive").is_some() {
                    w.alive = strs(&v["alive"]);
                }
                if let Some(o) = v.get("mm").and_then(|x| x.as_object()) {
                    w.mm = o.iter().map(|(k, x)| (k.clone(), x.as_str().unwrap_or("").to_string())).collect();
                }
                if let Some(o) = v.get("sid").and_then(|x| x.as_object()) {
                    w.sid = o.iter().map(|(k, x)| (k.clone(), x.as_u64().unwrap_or(0))).collect();
                }
                if let Some(o) = v.get("names").and_then(|x| x.as_object()) {
                    w.names = o.iter().map(|(k, x)| (k.clone(), x.as_str().unwrap_or("").to_string())).collect();
                }
                if let Some(o) = v.get("lobby").and_then(|x| x.as_object()) {
                    *lobby_map.lock() = o.iter().map(|(k, x)| (k.clone(), x.as_str().unwrap_or("").to_string())).collect();
                }
                if let Some(o) = v.get("match").and_then(|x| x.as_object()) {
                    *match_map.lock() = o.iter().map(|(k, x)| (k.clone(), x.as_str().unwrap_or("").to_string())).collect();
                }
                Value::Null
            }
            "sql" => {
                // a statement run on a side connection (players rows, names rows, dropping a table): {"sql":..., "args":[...]}
                let args: Vec<rusqlite::types::Value> = v["args"]
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .map(|x| match x {
                                Value::Null => rusqlite::types::Value::Null,
                                Value::Number(n) if n.is_i64() => rusqlite::types::Value::Integer(n.as_i64().unwrap_or(0)),
                                Value::Number(n) => rusqlite::types::Value::Real(n.as_f64().unwrap_or(0.0)),
                                Value::String(s) => rusqlite::types::Value::Text(s.clone()),
                                _ => rusqlite::types::Value::Null,
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                let sql = st("sql");
                let is_dml = ["INSERT", "UPDATE", "DELETE"].iter().any(|p| sql.trim_start().to_uppercase().starts_with(p));
                match Connection::open(&dbp).and_then(|c| c.execute(&sql, rusqlite::params_from_iter(args.iter()))) {
                    Ok(n) => {
                        if is_dml {
                            json!(n)
                        } else {
                            Value::Null
                        }
                    }
                    Err(e) => json!(format!("ERR {}", e)),
                }
            }
            "mutes" => {
                let _ = std::fs::write(&mutes, st("text"));
                if let (Some(mt), Ok(f)) = (v["mt"].as_u64(), std::fs::OpenOptions::new().write(true).open(&mutes)) {
                    let _ = f.set_modified(std::time::UNIX_EPOCH + std::time::Duration::from_secs(mt));
                }
                Value::Null
            }
            "nomutes" => {
                let _ = std::fs::remove_file(&mutes);
                Value::Null
            }
            "handle" => match s.handle(&st("token"), ints("lang"), ints("sub"), &st("arg")) {
                Some(r) => json!(String::from_utf8_lossy(&reply_json(&r)).into_owned()),
                None => json!("EXC"),
            },
            "tick" => json!(s.tick(flt("now"))),
            "party_tick" => json!(s.party_tick(flt("now"))),
            "flush" => {
                s.clog_flush(None, true);
                Value::Null
            }
            "acct" => json!(s.acct(&st("token"))),
            "status_of" => json!(s.status_of(&st("key"), None)),
            "token_for" => json!(s.token_for(&st("key"))),
            "mute_left" => json!(s.mute_left(&st("me"))),
            "touch" => {
                s.touch(&st("token"));
                Value::Null
            }
            "launcher_tokens" => {
                let mut l: Vec<String> = s.launcher_tokens().into_iter().collect();
                l.sort();
                json!(l)
            }
            "note" => {
                s.note(&st("token"), &st("kind"), &st("subj"));
                Value::Null
            }
            "note_tag" => json!(s.note_tag(&st("token"))),
            "note_acct" => {
                s.note_acct(&st("key"), &st("kind"), &st("subj"));
                Value::Null
            }
            "notes_for" => json!(s.notes_for(&st("token"), ints("lang"), v["ingame"].as_bool().unwrap_or(false)).into_iter().map(|(i, k, p, t)| json!([i, k, p, t])).collect::<Vec<_>>()),
            "drain" => {
                s.drain();
                Value::Null
            }
            "on_queue" => json!(s.on_queue(&st("token"), &queued)),
            "search_gate" => json!(s.search_gate(&st("token"), &queued)),
            "party_rows" => json!(s.party_rows(&st("token")).into_iter().map(|(a, b, c)| json!([a, b, c])).collect::<Vec<_>>()),
            "ingame_text" => json!(ingame_text(&st("s"), v.get("n").and_then(|x| x.as_u64()).unwrap_or(90) as usize)),
            "acct_hash" => json!(acct_hash(&st("s"))),
            "token_name_fallback" => json!(token_name_fallback(&st("token"))),
            // --- Parties
            "p_pid" => json!(parties.pid(&st("token"))),
            "p_members" => json!(parties.members(&st("token"))),
            "p_leader" => json!(parties.leader(&st("token"))),
            "p_is_leader" => json!(parties.is_leader(&st("token"))),
            "p_size" => json!(parties.size(&st("token"))),
            "p_invites_for" => json!(parties.invites_for(&st("token")).into_iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>()),
            "p_invite_token" => json!(opt(parties.invite_token(&st("a"), &st("b")))),
            "p_accept_token" => match parties.accept_token(&st("a"), &st("b")) {
                Ok(p) => json!([p, null]),
                Err(e) => json!([null, e]),
            },
            "p_decline_token" => json!(parties.decline_token(&st("a"), &st("b"))),
            "p_leave" => json!(parties.leave(&st("token"))),
            "p_kick" => json!(opt(parties.kick(&st("a"), &st("b")))),
            "p_promote" => json!(opt(parties.promote(&st("a"), &st("b")))),
            "p_clear_ready" => {
                parties.clear_ready(&strs(&v["tokens"]));
                Value::Null
            }
            "p_set_ready" => json!(opt(parties.set_ready(&st("token"), v["on"].as_bool().unwrap_or(false)))),
            "p_gc" => {
                let keep: HashSet<String> = strs(&v["keep"]).into_iter().collect();
                json!(parties.gc(&keep, flt("now")))
            }
            "p_export" => parties.export(),
            "p_restore" => json!(parties.restore(&v["data"], flt("now"))),
            "p_drain" => json!(parties.drain().into_iter().map(|(a, b, c)| json!([a, b, c])).collect::<Vec<_>>()),
            "p_invite" => json!(parties.invite(&st("token"), st("name").as_bytes())),
            "p_accept" => json!(parties.accept(&st("token"), st("name").as_bytes())),
            "p_text" => {
                let h = host.clone();
                json!(parties.text(&st("token"), &move |t: &str| h.name(t)))
            }
            "p_misc" => json!({
                "of_len": parties.of_len(), "inv_len": parties.inv_len(), "leaders": parties.leaders_sorted().into_iter().map(|(a, b)| json!([a, b])).collect::<Vec<_>>(),
                "ready": strs(&v["probe"]).iter().map(|t| parties.is_ready(t)).collect::<Vec<_>>(),
            }),
            // --- matchmaking helpers
            "held" => {
                let (ok, held) = held_parties(&strs(&v["ready"]), &parties);
                json!([ok, held.into_iter().map(|(k, m)| json!([k, m])).collect::<Vec<_>>()])
            }
            "units" => json!(units_of(&strs(&v["ready"]), &parties)),
            "need" => json!(party_need(ints("p"))),
            "shortfall" => {
                let r = party_shortfall(&strs(&v["ready"]), &parties, ints("size"), &strs(&v["chosen"]));
                json!(r.into_iter().map(|(k, (m, n))| json!([k, m, n])).collect::<Vec<_>>())
            }
            "pick" => json!(pick_units(&strs(&v["ready"]), &parties, ints("size"))),
            _ => json!("?"),
        };
        let _ = writeln!(out, "R {}", dumps_compact(&res));
        let logs = std::mem::take(&mut host.w.lock().logs);
        for l in logs {
            let _ = writeln!(out, "L {}", l);
        }
    }
    // the final state of the database
    if let Ok(c) = Connection::open(&dbp) {
        for (name, q) in [
            ("social_friends", "SELECT a,b,state,since FROM social_friends ORDER BY a,b"),
            ("social_seen", "SELECT acct,name,last FROM social_seen ORDER BY acct"),
            ("social_msgs", "SELECT id,a,b,ts,body,rd FROM social_msgs ORDER BY id"),
            ("chat_log", "SELECT id,ts,kind,room,acct,name,body,muted FROM chat_log ORDER BY id"),
        ] {
            if let Ok(mut stmt) = c.prepare(q) {
                let n = stmt.column_count();
                let rows = stmt.query_map([], |r| {
                    let mut a = Vec::new();
                    for i in 0..n {
                        let x: rusqlite::types::Value = r.get(i)?;
                        a.push(match x {
                            rusqlite::types::Value::Null => Value::Null,
                            rusqlite::types::Value::Integer(i) => json!(i),
                            rusqlite::types::Value::Real(f) => json!(f),
                            rusqlite::types::Value::Text(s) => json!(s),
                            rusqlite::types::Value::Blob(_) => json!("blob"),
                        });
                    }
                    Ok(Value::Array(a))
                });
                if let Ok(rows) = rows {
                    for r in rows.flatten() {
                        let _ = writeln!(out, "ROW {} {}", name, dumps_compact(&r));
                    }
                }
            }
        }
    }
    0
}
