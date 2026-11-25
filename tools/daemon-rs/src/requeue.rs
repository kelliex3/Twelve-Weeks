//! Queue stays after a match: tickets for the players of a finished match, the persistent post-match lobby.
//! Pure bookkeeping (the caller passes the queue, the presence silence and the in-match test). Thread-safe (own lock; never calls out
//! while holding it). Log prefix 'requeue:'.
use crate::c_util::{jb, jf, js, jstrs, script, sys_now, Now};
use indexmap::IndexMap;
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum State {
    Post,
    Search,
}

#[derive(Clone, Debug)]
pub struct Ticket {
    pub mid: String,
    pub t_end: f64,
    pub state: State,
}

/// One group of a finished match (py: dict of `groups()`).
#[derive(Clone, Debug)]
pub struct Group {
    pub post: Vec<String>,
    pub srch: Vec<String>,
    pub end: f64,
    pub go: bool,
    pub left: f64,
    pub next: Option<String>,
}

/// py: the dict `lobby()` returns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RqLobby {
    pub wait: i64,
    pub back: i64,
    pub post: i64,
    pub left: i64,
}

struct Inner {
    t: IndexMap<String, Ticket>,
    first: HashMap<String, f64>,
    next: HashMap<String, String>,
}

// py: requeue.py:36 class Requeue
pub struct Requeue {
    pub wait_max: f64,
    pub ticket_s: f64,
    pub offline_s: f64,
    pub release_s: f64,
    pub min_gap: f64,
    now: Now,
    inner: Mutex<Inner>,
}

impl Requeue {
    /// `Requeue(wait_max, ticket_s, offline_s, now=time.time, release_s=.., min_gap=..)`
    pub fn new(wait_max: f64, ticket_s: f64, offline_s: f64, release_s: f64, min_gap: f64) -> Requeue {
        Requeue::with_clock(wait_max, ticket_s, offline_s, release_s, min_gap, sys_now())
    }

    pub fn with_clock(wait_max: f64, ticket_s: f64, offline_s: f64, release_s: f64, min_gap: f64, now: Now) -> Requeue {
        Requeue {
            wait_max,
            ticket_s,
            offline_s,
            release_s,
            min_gap,
            now,
            inner: Mutex::new(Inner { t: IndexMap::new(), first: HashMap::new(), next: HashMap::new() }),
        }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    // py: requeue.py:50 open
    pub fn open(&self, mid: &str, tokens: &[String], now: Option<f64>) -> Vec<String> {
        let now = now.unwrap_or_else(|| self.now());
        let mut out: Vec<String> = Vec::new();
        let mut g = self.inner.lock();
        for t in tokens {
            if !t.is_empty() && !out.contains(t) {
                g.t.insert(t.clone(), Ticket { mid: mid.to_string(), t_end: now, state: State::Post });
                out.push(t.clone());
            }
        }
        out
    }

    // py: requeue.py:61 searching
    pub fn searching(&self, token: &str) -> Option<String> {
        let now = self.now();
        let mut g = self.inner.lock();
        let e = g.t.get_mut(token)?;
        e.state = State::Search;
        let mid = e.mid.clone();
        g.first.entry(mid.clone()).or_insert(now);
        Some(mid)
    }

    // py: requeue.py:71 drop
    pub fn drop(&self, token: &str) -> bool {
        self.inner.lock().t.shift_remove(token).is_some()
    }

    // py: requeue.py:76 drop_search
    pub fn drop_search(&self, token: &str) -> bool {
        let mut g = self.inner.lock();
        match g.t.get(token) {
            Some(e) if e.state == State::Search => {
                g.t.shift_remove(token);
                true
            }
            _ => false,
        }
    }

    // py: requeue.py:86 has
    pub fn has(&self, token: &str, now: Option<f64>) -> bool {
        let now = now.unwrap_or_else(|| self.now());
        self.inner.lock().t.get(token).map(|e| now - e.t_end < self.ticket_s).unwrap_or(false)
    }

    // py: requeue.py:92 mid_of
    pub fn mid_of(&self, token: &str) -> Option<String> {
        self.inner.lock().t.get(token).map(|e| e.mid.clone())
    }

    // py: requeue.py:97 tokens
    pub fn tokens(&self) -> Vec<String> {
        self.inner.lock().t.keys().cloned().collect()
    }

    // py: requeue.py:101 _prune (lock held)
    fn prune(&self, g: &mut Inner, sil: &HashMap<String, f64>, inm: &HashMap<String, bool>, now: f64) -> Vec<(String, String)> {
        let mut gone = Vec::new();
        let items: Vec<(String, Ticket)> = g.t.iter().map(|(k, v)| (k.clone(), v.clone())).collect();
        for (t, e) in items {
            let why: Option<String> = if now - e.t_end >= self.ticket_s {
                Some(format!("ticket expired ({} s after the match end)", crate::py::fixed(now - e.t_end, 0)))
            } else if inm.get(&t).copied().unwrap_or(false) {
                Some("in a match again".to_string())
            } else if e.state == State::Post && sil.get(&t).copied().unwrap_or(0.0) > self.offline_s {
                Some(format!("offline (silent {} s)", crate::py::fixed(sil[&t], 0)))
            } else {
                None
            };
            if let Some(why) = why {
                g.t.shift_remove(&t);
                gone.push((t, why));
            }
        }
        let live: HashSet<&String> = g.t.values().map(|e| &e.mid).collect();
        let dead: Vec<String> = g.first.keys().filter(|m| !live.contains(m)).cloned().collect();
        let dead2: Vec<String> = g.next.keys().filter(|m| !live.contains(m)).cloned().collect();
        for m in dead {
            g.first.remove(&m);
        }
        for m in dead2 {
            g.next.remove(&m);
        }
        gone
    }

    // py: requeue.py:121 _left (lock held)
    fn left_s(&self, g: &Inner, mid: &str, t_end: f64, now: f64) -> f64 {
        let mut left = self.wait_max - (now - t_end);
        if self.release_s > 0.0 {
            if let Some(f) = g.first.get(mid) {
                left = left.min(self.release_s - (now - f));
            }
        }
        left.max(self.min_gap - (now - t_end)).max(0.0)
    }

    // py: requeue.py:129 groups
    pub fn groups(
        &self,
        queued: &[String],
        silence: &dyn Fn(&str) -> f64,
        in_match: &dyn Fn(&str) -> bool,
        now: Option<f64>,
    ) -> (IndexMap<String, Group>, Vec<(String, String)>) {
        let now = now.unwrap_or_else(|| self.now());
        let qs: HashSet<&String> = queued.iter().collect();
        let toks = self.tokens();
        let sil: HashMap<String, f64> = toks.iter().map(|t| (t.clone(), silence(t))).collect();
        let inm: HashMap<String, bool> = toks.iter().map(|t| (t.clone(), in_match(t))).collect();
        let mut out: IndexMap<String, Group> = IndexMap::new();
        let mut g = self.inner.lock();
        let gone = self.prune(&mut g, &sil, &inm, now);
        for (t, e) in g.t.iter() {
            let nx = g.next.get(&e.mid).cloned();
            let gr = out.entry(e.mid.clone()).or_insert_with(|| Group { post: vec![], srch: vec![], end: e.t_end, go: false, left: 0.0, next: nx });
            gr.end = gr.end.min(e.t_end);
            if e.state == State::Post {
                gr.post.push(t.clone());
            } else if qs.contains(t) {
                gr.srch.push(t.clone());
            }
        }
        let mids: Vec<String> = out.keys().cloned().collect();
        for mid in mids {
            let end = out[&mid].end;
            let left = self.left_s(&g, &mid, end, now);
            if let Some(gr) = out.get_mut(&mid) {
                gr.left = left;
                let gap = now - gr.end < self.min_gap;
                gr.go = !gap && (gr.post.is_empty() || gr.left <= 0.0 || gr.next.is_some());
            }
        }
        (out, gone)
    }

    // py: requeue.py:155 held
    pub fn held(
        &self,
        queued: &[String],
        silence: &dyn Fn(&str) -> f64,
        in_match: &dyn Fn(&str) -> bool,
        now: Option<f64>,
    ) -> (HashSet<String>, IndexMap<String, Vec<String>>, Vec<(String, String)>) {
        let now = now.unwrap_or_else(|| self.now());
        let (out, gone) = self.groups(queued, silence, in_match, Some(now));
        let mut held = HashSet::new();
        let mut waiting = IndexMap::new();
        for (mid, g) in &out {
            if !g.srch.is_empty() && !g.go {
                held.extend(g.srch.iter().cloned());
                waiting.insert(mid.clone(), g.post.clone());
            }
        }
        (held, waiting, gone)
    }

    // py: requeue.py:170 ready_groups
    pub fn ready_groups(
        &self,
        queued: &[String],
        silence: &dyn Fn(&str) -> f64,
        in_match: &dyn Fn(&str) -> bool,
        now: Option<f64>,
    ) -> Vec<(String, Vec<String>, Vec<String>)> {
        let now = now.unwrap_or_else(|| self.now());
        let (out, _) = self.groups(queued, silence, in_match, Some(now));
        let mut res: Vec<(f64, String, Vec<String>, Vec<String>)> = out
            .iter()
            .filter(|(_, g)| g.go && !g.srch.is_empty() && g.next.is_none())
            .map(|(mid, g)| (g.end, mid.clone(), g.srch.clone(), g.post.clone()))
            .collect();
        // sorted() on the tuples (end, mid, srch, post): lexicographic, stable
        res.sort_by(|a, b| {
            a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal).then_with(|| a.1.cmp(&b.1)).then_with(|| a.2.cmp(&b.2)).then_with(|| a.3.cmp(&b.3))
        });
        res.into_iter().map(|(_, mid, s, p)| (mid, s, p)).collect()
    }

    // py: requeue.py:179 formed
    pub fn formed(&self, new_mid: &str, tokens: &[String]) -> HashSet<String> {
        let mut g = self.inner.lock();
        let gs: HashSet<String> = tokens.iter().filter_map(|t| g.t.get(t).map(|e| e.mid.clone())).collect();
        for m in &gs {
            g.next.insert(m.clone(), new_mid.to_string());
        }
        gs
    }

    // py: requeue.py:187 kept
    pub fn kept(&self, new_mid: &str, now: Option<f64>) -> Vec<String> {
        let now = now.unwrap_or_else(|| self.now());
        let g = self.inner.lock();
        let gs: HashSet<&String> = g.next.iter().filter(|(_, n)| n.as_str() == new_mid).map(|(m, _)| m).collect();
        g.t.iter().filter(|(_, e)| gs.contains(&e.mid) && now - e.t_end < self.ticket_s).map(|(t, _)| t.clone()).collect()
    }

    // py: requeue.py:195 next_of
    pub fn next_of(&self, token: &str) -> Option<String> {
        let g = self.inner.lock();
        g.t.get(token).and_then(|e| g.next.get(&e.mid).cloned())
    }

    // py: requeue.py:200 status
    pub fn status(
        &self,
        token: &str,
        queued: &[String],
        silence: &dyn Fn(&str) -> f64,
        in_match: &dyn Fn(&str) -> bool,
        now: Option<f64>,
    ) -> (bool, i64) {
        match self.lobby(token, queued, silence, in_match, now) {
            None => (false, 0),
            Some(lb) => (true, lb.wait),
        }
    }

    // py: requeue.py:205 members
    pub fn members(
        &self,
        token: &str,
        queued: &[String],
        silence: &dyn Fn(&str) -> f64,
        in_match: &dyn Fn(&str) -> bool,
        now: Option<f64>,
    ) -> Option<(Vec<String>, Vec<String>)> {
        let now = now.unwrap_or_else(|| self.now());
        let (out, _) = self.groups(queued, silence, in_match, Some(now));
        let mid = {
            let g = self.inner.lock();
            let e = g.t.get(token)?;
            if now - e.t_end >= self.ticket_s {
                return None;
            }
            e.mid.clone()
        };
        let g = out.get(&mid)?;
        if g.next.as_deref().map(|s| !s.is_empty()).unwrap_or(false) {
            return None;
        }
        Some((g.srch.clone(), g.post.clone()))
    }

    // py: requeue.py:219 lobby
    pub fn lobby(
        &self,
        token: &str,
        queued: &[String],
        silence: &dyn Fn(&str) -> f64,
        in_match: &dyn Fn(&str) -> bool,
        now: Option<f64>,
    ) -> Option<RqLobby> {
        let now = now.unwrap_or_else(|| self.now());
        let (out, _) = self.groups(queued, silence, in_match, Some(now));
        let mid = {
            let g = self.inner.lock();
            let e = g.t.get(token)?;
            if now - e.t_end >= self.ticket_s {
                return None;
            }
            e.mid.clone()
        };
        let Some(g) = out.get(&mid) else { return Some(RqLobby { wait: 0, back: 0, post: 0, left: 0 }) };
        let held = g.srch.iter().any(|t| t == token) && !g.go;
        Some(RqLobby {
            wait: if held { g.post.len() as i64 } else { 0 },
            back: g.srch.len() as i64,
            post: g.post.len() as i64,
            left: if held { (g.left + 0.999).trunc() as i64 } else { 0 },
        })
    }
}

// ------------------------------------------------------------------ differential test (verify/py/requeue_harness.py)

fn state(r: &Requeue) -> Value {
    let g = r.inner.lock();
    let t: Vec<Value> = g.t.iter().map(|(k, e)| json!([k, e.mid, e.t_end, if e.state == State::Post { "post" } else { "search" }])).collect();
    let first: std::collections::BTreeMap<&String, f64> = g.first.iter().map(|(k, v)| (k, *v)).collect();
    let next: std::collections::BTreeMap<&String, &String> = g.next.iter().collect();
    json!({"t": t, "first": first, "next": next})
}

fn group_json(g: &Group) -> Value {
    json!({"post": g.post, "srch": g.srch, "end": g.end, "go": g.go, "left": g.left, "next": g.next})
}

fn sorted(s: &HashSet<String>) -> Vec<String> {
    let mut v: Vec<String> = s.iter().cloned().collect();
    v.sort();
    v
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let clk = crate::c_util::Frozen::new();
    let mut r: Option<Requeue> = None;
    for (i, c) in script(input).iter().enumerate() {
        clk.set(jf(c, "t"));
        let op = js(c, "op");
        if op == "init" {
            r = Some(Requeue::with_clock(jf(c, "wait_max"), jf(c, "ticket_s"), jf(c, "offline_s"), jf(c, "release_s"), jf(c, "min_gap"), clk.now()));
            crate::c_util::emit(out, i, Value::Null, state(r.as_ref().unwrap()));
            continue;
        }
        let Some(rq) = r.as_ref() else { continue };
        let queued = jstrs(c, "queued");
        let sil: HashMap<String, f64> =
            c.get("sil").and_then(|x| x.as_object()).map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_f64().unwrap_or(0.0))).collect()).unwrap_or_default();
        let inm: HashSet<String> = jstrs(c, "inm").into_iter().collect();
        let silence = |t: &str| sil.get(t).copied().unwrap_or(0.0);
        let in_match = |t: &str| inm.contains(t);
        let tok = js(c, "token");
        let res: Value = match op {
            "open" => json!(rq.open(js(c, "mid"), &jstrs(c, "tokens"), Some(jf(c, "t")))),
            "searching" => json!(rq.searching(tok)),
            "drop" => json!(rq.drop(tok)),
            "drop_search" => json!(rq.drop_search(tok)),
            "has" => json!(rq.has(tok, if jb(c, "now") { Some(jf(c, "t")) } else { None })),
            "mid_of" => json!(rq.mid_of(tok)),
            "tokens" => json!(rq.tokens()),
            "groups" => {
                let (g, gone) = rq.groups(&queued, &silence, &in_match, Some(jf(c, "t")));
                let m: serde_json::Map<String, Value> = g.iter().map(|(k, v)| (k.clone(), group_json(v))).collect();
                json!({"g": Value::Object(m), "order": g.keys().collect::<Vec<_>>(), "gone": gone})
            }
            "held" => {
                let (h, w, gone) = rq.held(&queued, &silence, &in_match, Some(jf(c, "t")));
                let wm: serde_json::Map<String, Value> = w.iter().map(|(k, v)| (k.clone(), json!(v))).collect();
                json!({"held": sorted(&h), "waiting": Value::Object(wm), "worder": w.keys().collect::<Vec<_>>(), "gone": gone})
            }
            "ready" => json!(rq.ready_groups(&queued, &silence, &in_match, Some(jf(c, "t")))),
            "formed" => json!(sorted(&rq.formed(js(c, "mid"), &jstrs(c, "tokens")))),
            "kept" => json!(rq.kept(js(c, "mid"), Some(jf(c, "t")))),
            "next_of" => json!(rq.next_of(tok)),
            "status" => {
                let (a, b) = rq.status(tok, &queued, &silence, &in_match, Some(jf(c, "t")));
                json!([a, b])
            }
            "members" => json!(rq.members(tok, &queued, &silence, &in_match, Some(jf(c, "t")))),
            "lobby" => json!(rq
                .lobby(tok, &queued, &silence, &in_match, Some(jf(c, "t")))
                .map(|l| json!({"wait": l.wait, "back": l.back, "post": l.post, "left": l.left}))),
            _ => Value::Null,
        };
        crate::c_util::emit(out, i, res, state(rq));
    }
    0
}
