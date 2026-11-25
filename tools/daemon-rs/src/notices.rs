//! Server notices: messages the daemon shows to players in game (match + menus) and in the launcher. Pure logic, no sockets.
//!
//! The Rust version of notices.py. Kinds: u update available, v update required, r planned restart, s shutting down now, t free text.
//! Wire: op 0xBA request (polled by the game), op 0xBB reply "n=<k>\n<id>|<kind>|<param>|<text>\n...".
use parking_lot::Mutex;
use serde_json::{json, Map, Value};
use std::collections::BTreeMap;

use crate::py::{round0, strip};

pub const OP_REQ: u8 = 0xBA;
pub const OP_REPLY: u8 = 0xBB;
pub const MAX_TEXT: usize = 140;
pub const MAX_LINES: usize = 4;

// py: notices.py:27 clean
pub fn clean(s: &str, n: usize) -> String {
    let t: String = s.chars().map(|c| if (c as u32) < 32 || matches!(c as u32, 0x7f | 0x2028 | 0x2029 | 0x85) { ' ' } else { c }).collect();
    let t = t.replace('|', "/");
    crate::py::cut(strip(&t), n).to_string()
}

/// `clean(s)` with the default length.
pub fn clean_d(s: &str) -> String {
    clean(s, MAX_TEXT)
}

#[derive(Clone, Debug)]
struct Text {
    text: String,
    text_pl: String,
    until: f64,
    show: i64,
}

/// The scheduled shutdown (`self.shutdown` dict).
#[derive(Clone, Debug)]
pub struct Shutdown {
    pub id: i64,
    pub at: f64,
    pub reason: String,
    pub reason_pl: String,
    pub restart: bool,
}

#[derive(Clone, Debug)]
struct Final {
    id: i64,
    until: f64,
}

struct Inner {
    seq: i64,
    texts: BTreeMap<i64, Text>,
    shutdown: Option<Shutdown>,
    final_: Option<Final>,
}

/// One active notice line: (id, kind, param, text).
pub type Item = (i64, &'static str, i64, String);

pub struct Notices {
    now: Box<dyn Fn() -> f64 + Send + Sync>,
    inner: Mutex<Inner>,
}

/// Python `min(a, b)` / `max(a, b)` (first argument wins ties and NaN comparisons).
fn pmin(a: f64, b: f64) -> f64 {
    if b < a {
        b
    } else {
        a
    }
}
fn pmax(a: f64, b: f64) -> f64 {
    if b > a {
        b
    } else {
        a
    }
}

impl Notices {
    // py: notices.py:42 Notices.__init__ (now=time.time)
    pub fn new() -> Notices {
        Notices::with_clock(Box::new(crate::clock::now))
    }

    pub fn with_clock(now: Box<dyn Fn() -> f64 + Send + Sync>) -> Notices {
        Notices { now, inner: Mutex::new(Inner { seq: 0, texts: BTreeMap::new(), shutdown: None, final_: None }) }
    }

    fn next_id(g: &mut Inner) -> i64 {
        g.seq += 1;
        g.seq
    }

    /// Number of stored free texts (`len(NOTICES.texts)`).
    pub fn texts_len(&self) -> usize {
        self.inner.lock().texts.len()
    }

    // py: notices.py:55 add_text
    /// `seconds` = time ON SCREEN (kind t, 2..600 s); without it the old minutes window, 8 s on screen. `minutes` defaults to 30.0 in Python.
    pub fn add_text(&self, text: &str, minutes: f64, seconds: Option<f64>, text_pl: Option<&str>) -> Option<i64> {
        let text = clean_d(text);
        let text_pl = clean_d(text_pl.unwrap_or(""));
        if text.is_empty() && text_pl.is_empty() {
            return None;
        }
        let text = if text.is_empty() { text_pl.clone() } else { text };
        let now = (self.now)();
        let mut g = self.inner.lock();
        let i = Self::next_id(&mut g);
        let (show, keep);
        if let Some(sec) = seconds {
            show = pmax(2.0, pmin(sec, 600.0)) as i64;
            keep = pmax(show as f64 + 20.0, 60.0);
        } else {
            show = 8;
            keep = pmax(0.5, pmin(minutes, (24 * 60) as f64)) * 60.0;
        }
        g.texts.insert(i, Text { text, text_pl, until: now + keep, show });
        Some(i)
    }

    // py: notices.py:75 clear
    pub fn clear(&self, nid: i64) -> bool {
        let mut g = self.inner.lock();
        if g.texts.remove(&nid).is_some() {
            return true;
        }
        if g.shutdown.as_ref().map(|s| s.id == nid).unwrap_or(false) {
            g.shutdown = None;
            return true;
        }
        false
    }

    // py: notices.py:83 schedule
    pub fn schedule(&self, minutes: f64, reason: &str, restart: bool, reason_pl: Option<&str>) -> Shutdown {
        let minutes = pmax(0.0, pmin(minutes, (24 * 60) as f64));
        let now = (self.now)();
        let mut g = self.inner.lock();
        let s = Shutdown { id: Self::next_id(&mut g), at: now + minutes * 60.0, reason: clean_d(reason), reason_pl: clean_d(reason_pl.unwrap_or("")), restart };
        g.shutdown = Some(s.clone());
        s
    }

    // py: notices.py:89 cancel
    pub fn cancel(&self) -> bool {
        let mut g = self.inner.lock();
        let had = g.shutdown.is_some();
        g.shutdown = None;
        had
    }

    // py: notices.py:95 go_final
    pub fn go_final(&self, hold: f64) {
        let now = (self.now)();
        let mut g = self.inner.lock();
        let id = Self::next_id(&mut g);
        g.final_ = Some(Final { id, until: now + hold });
    }

    // py: notices.py:99 due
    /// The scheduled shutdown if its time has come (the daemon then exits), else None.
    pub fn due(&self) -> Option<Shutdown> {
        let now = (self.now)();
        let g = self.inner.lock();
        match &g.shutdown {
            Some(s) if now >= s.at => Some(s.clone()),
            _ => None,
        }
    }

    // py: notices.py:104 active
    /// [(id, kind, param, text)] for one player. `lang` "pl": admin free text / restart reason in their Polish form when typed.
    pub fn active(&self, stale_build: bool, refused: bool, lang: &str) -> Vec<Item> {
        let pl = lang == "pl";
        let now = (self.now)();
        let mut g = self.inner.lock();
        let mut out: Vec<Item> = Vec::new();
        let dead: Vec<i64> = g.texts.iter().filter(|(_, t)| t.until <= now).map(|(i, _)| *i).collect();
        for i in dead {
            g.texts.remove(&i);
        }
        if let Some(f) = &g.final_ {
            if now < f.until {
                out.push((f.id, "s", 0, String::new()));
            }
        }
        if let Some(s) = &g.shutdown {
            let txt = if pl && !s.reason_pl.is_empty() { s.reason_pl.clone() } else { s.reason.clone() };
            out.push((s.id, "r", pmax(0.0, round0(s.at - now) as f64) as i64, txt));
        }
        if refused {
            out.push((1, "v", 0, String::new()));
        } else if stale_build {
            out.push((0, "u", 0, String::new()));
        }
        for (i, t) in g.texts.iter() {
            let txt = if pl && !t.text_pl.is_empty() { t.text_pl.clone() } else { t.text.clone() };
            out.push((*i, "t", t.show, txt));
        }
        out.truncate(MAX_LINES);
        out
    }

    // py: notices.py:128 public
    /// Player-independent list for the status file / launcher: [{"id","kind","param","text","endsAt"[,"text_pl"]}].
    pub fn public(&self) -> Vec<Value> {
        let now = (self.now)();
        let items = self.active(false, false, "en");
        let g = self.inner.lock();
        let mut out = Vec::new();
        for (i, k, p, t) in items {
            let ends: Value = match k {
                "r" => json!(now + p as f64),
                "t" => match g.texts.get(&i) {
                    Some(x) => json!(x.until),
                    None => Value::Null,
                },
                _ => Value::Null,
            };
            let mut e = Map::new();
            e.insert("id".into(), json!(i));
            e.insert("kind".into(), json!(k));
            e.insert("param".into(), json!(p));
            e.insert("text".into(), json!(t));
            e.insert("endsAt".into(), ends);
            let pt: String = if k == "t" {
                g.texts.get(&i).map(|x| x.text_pl.clone()).unwrap_or_default()
            } else if k == "r" {
                g.shutdown.as_ref().map(|s| s.reason_pl.clone()).unwrap_or_default()
            } else {
                String::new()
            };
            if !pt.is_empty() {
                e.insert("text_pl".into(), json!(pt));
            }
            out.push(Value::Object(e));
        }
        out
    }

    // py: notices.py:146 reply_text
    pub fn reply_text(items: &[Item]) -> Vec<u8> {
        let lines: Vec<String> = items.iter().map(|(i, k, p, t)| format!("{}|{}|{}|{}", i, k, p, clean_d(t))).collect();
        format!("n={}\n{}", lines.len(), lines.join("\n")).into_bytes()
    }
}

// ---------------------------------------------------------------- differential test
// verify/py/small_harness.py is the Python side: JSON lines {"m":"notices","op":..,"t": frozen time, ..}.
pub fn selftest(input: &mut dyn std::io::BufRead, out: &mut dyn std::io::Write) -> i32 {
    use crate::py::dumps_compact;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::sync::Arc;
    let clock = Arc::new(AtomicU64::new(0f64.to_bits()));
    let c2 = clock.clone();
    let n = Notices::with_clock(Box::new(move || f64::from_bits(c2.load(Ordering::Relaxed))));
    let mut line = String::new();
    loop {
        line.clear();
        match input.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let Ok(v) = serde_json::from_str::<Value>(line.trim()) else { continue };
        if v["m"].as_str() != Some("notices") {
            continue;
        }
        clock.store(v["t"].as_f64().unwrap_or(0.0).to_bits(), Ordering::Relaxed);
        let s = |k: &str| v.get(k).and_then(|x| x.as_str()).map(|x| x.to_string());
        let f = |k: &str| v.get(k).and_then(|x| x.as_f64());
        let items_json = |items: &[Item]| -> Value { Value::Array(items.iter().map(|(i, k, p, t)| json!([i, k, p, t])).collect()) };
        let res: Value = match v["op"].as_str().unwrap_or("") {
            "clean" => json!(clean(&s("s").unwrap_or_default(), v.get("n").and_then(|x| x.as_u64()).unwrap_or(MAX_TEXT as u64) as usize)),
            "add_text" => json!(n.add_text(&s("text").unwrap_or_default(), f("minutes").unwrap_or(30.0), f("seconds"), s("text_pl").as_deref())),
            "clear" => json!(n.clear(v["id"].as_i64().unwrap_or(-1))),
            "schedule" => {
                let r = n.schedule(f("minutes").unwrap_or(0.0), &s("reason").unwrap_or_default(), v.get("restart").and_then(|x| x.as_bool()).unwrap_or(true), s("reason_pl").as_deref());
                json!({"id": r.id, "at": r.at, "reason": r.reason, "reason_pl": r.reason_pl, "restart": r.restart})
            }
            "cancel" => json!(n.cancel()),
            "go_final" => {
                n.go_final(f("hold").unwrap_or(60.0));
                Value::Null
            }
            "due" => match n.due() {
                Some(r) => json!({"id": r.id, "at": r.at, "reason": r.reason, "reason_pl": r.reason_pl, "restart": r.restart}),
                None => Value::Null,
            },
            "active" => items_json(&n.active(
                v.get("stale").and_then(|x| x.as_bool()).unwrap_or(false),
                v.get("refused").and_then(|x| x.as_bool()).unwrap_or(false),
                &s("lang").unwrap_or_else(|| "en".into()),
            )),
            "public" => Value::Array(n.public()),
            "reply_text" => {
                let items: Vec<Item> = v["items"]
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .map(|r| {
                                let k: &'static str = match r[1].as_str().unwrap_or("") {
                                    "u" => "u",
                                    "v" => "v",
                                    "r" => "r",
                                    "s" => "s",
                                    "t" => "t",
                                    _ => "p",
                                };
                                (r[0].as_i64().unwrap_or(0), k, r[2].as_i64().unwrap_or(0), r[3].as_str().unwrap_or("").to_string())
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                json!(crate::py::utf8_replace(&Notices::reply_text(&items)))
            }
            _ => json!("?"),
        };
        let _ = writeln!(out, "{}", dumps_compact(&res));
    }
    0
}
