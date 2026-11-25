//! Stable search lobbies. Pure bookkeeping: the caller (the matchmaker) passes the rules as callbacks and holds its own lock
//! around every call.
use crate::c_util::{head, jb, jf, js, jstrs, script, sys_now, Now};
use crate::{dlog, py, rnd};
use indexmap::IndexMap;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::io::{BufRead, Write};

// py: lobbybook.py:24 constants
pub const GRACE_S: f64 = 10.0;
pub const BACKOUT_BLOCK_S: f64 = 60.0;
pub const REJOIN_S: f64 = 45.0;
pub const RELAUNCH_S: f64 = 8.0;

/// fits(members, newcomers) -> may the newcomers share a lobby with the members.
pub type Fits<'a> = &'a mut dyn FnMut(&[String], &[String]) -> bool;
/// cap(book, lobby, newcomers) -> seats of that lobby for them. The book is passed (read-only) because the Python callback reads `book.of`.
pub type Cap<'a> = &'a mut dyn FnMut(&LobbyBook, &Lobby, &[String]) -> i64;

// py: lobbybook.py:31 class Lobby
#[derive(Clone, Debug)]
pub struct Lobby {
    pub lid: String,
    pub members: Vec<String>,
    pub joined: HashMap<String, f64>,
    pub away: IndexMap<String, f64>,
    pub created: f64,
    pub last_join: f64,
    pub mid: Option<String>,
    pub src: Option<String>,
    pub hold: bool,
    pub wait_until: f64,
}

impl Lobby {
    fn new(lid: &str, now: f64, src: Option<&str>) -> Lobby {
        Lobby {
            lid: lid.to_string(),
            members: Vec::new(),
            joined: HashMap::new(),
            away: IndexMap::new(),
            created: now,
            last_join: 0.0,
            mid: None,
            src: src.map(|s| s.to_string()),
            hold: false,
            wait_until: 0.0,
        }
    }
}

// py: lobbybook.py:46 class LobbyBook
pub struct LobbyBook {
    pub size: usize,
    now: Now,
    pub lobbies: IndexMap<String, Lobby>,
    pub of: HashMap<String, String>,
    pub left: HashMap<String, (String, f64, f64)>,
    pub mid_lid: IndexMap<String, String>,
}

fn tag8(token: &str) -> &str {
    head(token, 8)
}

impl LobbyBook {
    pub fn new(size: usize) -> LobbyBook {
        LobbyBook::with_clock(size, sys_now())
    }

    pub fn with_clock(size: usize, now: Now) -> LobbyBook {
        LobbyBook { size, now, lobbies: IndexMap::new(), of: HashMap::new(), left: HashMap::new(), mid_lid: IndexMap::new() }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    // ------------------------------------------------------------------ queries
    // py: lobbybook.py:58 lobby_of
    pub fn lobby_of(&self, token: &str) -> Option<&str> {
        self.of.get(token).map(|s| s.as_str())
    }

    // py: lobbybook.py:61 get
    pub fn get(&self, lid: &str) -> Option<&Lobby> {
        self.lobbies.get(lid)
    }

    pub fn get_mut(&mut self, lid: &str) -> Option<&mut Lobby> {
        self.lobbies.get_mut(lid)
    }

    // py: lobbybook.py:64 members
    pub fn members(&self, lid: &str) -> Vec<String> {
        self.lobbies.get(lid).map(|l| l.members.clone()).unwrap_or_default()
    }

    // py: lobbybook.py:68 mates
    pub fn mates(&self, token: &str) -> Vec<String> {
        match self.of.get(token) {
            Some(l) => self.members(l),
            None => Vec::new(),
        }
    }

    // py: lobbybook.py:72 open_lobbies
    pub fn open_lobbies(&self) -> Vec<&Lobby> {
        self.lobbies.values().filter(|l| l.mid.is_none()).collect()
    }

    // py: lobbybook.py:75 launch_order
    pub fn launch_order(&self) -> Vec<&Lobby> {
        let mut v = self.open_lobbies();
        v.sort_by(|a, b| {
            (-(a.members.len() as i64))
                .cmp(&-(b.members.len() as i64))
                .then(a.created.partial_cmp(&b.created).unwrap_or(std::cmp::Ordering::Equal))
        });
        v
    }

    // py: lobbybook.py:79 key_for_mid
    pub fn key_for_mid(&self, mid: &str) -> String {
        match self.mid_lid.get(mid) {
            Some(l) if !l.is_empty() => l.clone(),
            _ => format!("m{}", mid),
        }
    }

    /// `lb.hold = x` (the daemon clears it for lobbies that no longer wait for a join in progress).
    pub fn set_hold(&mut self, lid: &str, hold: bool) {
        if let Some(l) = self.lobbies.get_mut(lid) {
            l.hold = hold;
        }
    }

    // ------------------------------------------------------------------ changes
    // py: lobbybook.py:84 _new
    fn new_lobby(&mut self, now: f64, src: Option<&str>, lid: Option<&str>) -> String {
        let lid = match lid {
            Some(l) if !self.lobbies.contains_key(l) => l.to_string(),
            _ => format!("L{}", rnd::token_hex(4)),
        };
        let lb = Lobby::new(&lid, now, src);
        self.lobbies.insert(lid.clone(), lb);
        lid
    }

    // py: lobbybook.py:90 _add
    fn add(&mut self, lid: &str, token: &str, now: f64, at: Option<f64>) {
        let old = self.of.get(token).cloned();
        if old.as_deref() == Some(lid) {
            if let Some(lb) = self.lobbies.get_mut(lid) {
                lb.away.shift_remove(token);
            }
            return;
        }
        if old.is_some() {
            self.take_out(token);
        }
        if let Some(lb) = self.lobbies.get_mut(lid) {
            lb.members.push(token.to_string());
            lb.joined.insert(token.to_string(), now);
            let v = match at {
                None => now,
                Some(a) => a.min(now),
            };
            lb.last_join = lb.last_join.max(v);
        }
        self.of.insert(token.to_string(), lid.to_string());
        self.left.remove(token);
    }

    // py: lobbybook.py:101 _take_out
    fn take_out(&mut self, token: &str) -> Option<String> {
        let lid = self.of.remove(token)?;
        let lb = self.lobbies.get_mut(&lid)?;
        if let Some(p) = lb.members.iter().position(|m| m == token) {
            lb.members.remove(p);
        }
        lb.joined.remove(token);
        lb.away.shift_remove(token);
        if lb.members.is_empty() && lb.mid.is_none() {
            self.lobbies.shift_remove(&lid);
        }
        Some(lid)
    }

    // py: lobbybook.py:113 src_lobby
    pub fn src_lobby(&self, mid: &str) -> Option<String> {
        self.lobbies.values().find(|lb| lb.src.as_deref() == Some(mid) && lb.mid.is_none()).map(|lb| lb.lid.clone())
    }

    // py: lobbybook.py:123 place
    /// `near(members, newcomers)` (optional): same region group (or unknown) - region first, never a block: a returner whose post-match
    /// lobby is mostly of another region takes an open lobby of its own region when one fits, else its post-match lobby as before;
    /// among the other lobbies the same-region ones come first.
    #[allow(clippy::too_many_arguments)]
    pub fn place(
        &mut self,
        token: &str,
        unit: &[String],
        fits: Fits,
        cap: Cap,
        now: Option<f64>,
        src: Option<&str>,
        solo: bool,
        at: Option<f64>,
        mut near: Option<Fits>,
    ) -> (String, String) {
        let now = now.unwrap_or_else(|| self.now());
        if let Some(lid) = self.of.get(token).cloned() {
            if let Some(lb) = self.lobbies.get_mut(&lid) {
                lb.away.shift_remove(token);
                return (lid, "back".into());
            }
        }
        let mut unit_v: Vec<String> = vec![token.to_string()];
        unit_v.extend(unit.iter().filter(|m| m.as_str() != token).cloned());
        let tok_v = vec![token.to_string()];
        // a party mate is in a lobby already: the same lobby (room permitting), else the whole party moves to a new one
        for m in unit_v[1..].to_vec() {
            let Some(ml) = self.of.get(&m).cloned() else { continue };
            let room = match self.lobbies.get(&ml) {
                None => continue,
                Some(lb) if lb.mid.is_some() => continue,
                Some(lb) => lb.members.len() as i64 + 1 <= cap(&*self, lb, &tok_v),
            };
            if room {
                self.add(&ml, token, now, at);
                return (ml, "party".into());
            }
            let nb = self.new_lobby(now, None, None);
            let movers: Vec<String> = unit_v.iter().filter(|x| self.of.get(*x).map(|s| s.as_str()) == Some(ml.as_str())).cloned().collect();
            for x in movers {
                self.add(&nb, &x, now, None);
            }
            self.add(&nb, token, now, at);
            if let Some(lb) = self.lobbies.get(&ml) {
                if lb.members.is_empty() && lb.mid.is_none() {
                    self.lobbies.shift_remove(&ml);
                }
            }
            dlog!(format!("LOBBY-BOOK: party of {} moved to a new lobby {} (no room in {})", tag8(token), nb, ml));
            return (nb, "party-moved".into());
        }
        let need = unit_v.len() as i64;
        let lf = self.left.get(token).cloned();
        let blocked: Option<String> = match &lf {
            Some(e) if now < e.2 => Some(e.0.clone()),
            _ => None,
        };

        let mut ok = |this: &LobbyBook, lid: Option<&str>| -> bool {
            let Some(lb) = lid.and_then(|l| this.lobbies.get(l)) else { return false };
            lb.mid.is_none()
                && Some(lb.lid.as_str()) != blocked.as_deref()
                && !lb.hold
                && !lb.members.is_empty()
                && lb.members.len() as i64 + need <= cap(this, lb, &unit_v)
                && fits(&lb.members.clone(), &unit_v)
        };
        let mut near_ok = |this: &LobbyBook, lid: &str| -> bool {
            match near.as_mut() {
                None => true,
                Some(nf) => this.lobbies.get(lid).is_some_and(|lb| nf(&lb.members.clone(), &unit_v)),
            }
        };
        // best_open: the fullest, oldest open lobby (same region first); `skip_src` = not the post-match lobby of that match
        let best_open = |this: &LobbyBook, skip_src: Option<&str>, same_only: bool,
                             ok: &mut dyn FnMut(&LobbyBook, Option<&str>) -> bool,
                             near_ok: &mut dyn FnMut(&LobbyBook, &str) -> bool| -> Option<String> {
            let mut best: Option<((i64, i64, f64), String)> = None;
            let ids: Vec<String> = this.lobbies.keys().cloned().collect();
            for id in ids {
                let Some(lb) = this.lobbies.get(&id) else { continue };
                if (skip_src.is_some() && lb.src.as_deref() == skip_src) || !ok(this, Some(&id)) {
                    continue;
                }
                let nr = near_ok(this, &id);
                if same_only && !nr {
                    continue;
                }
                let lb = &this.lobbies[&id];
                let k = (if nr { 0 } else { 1 }, -(lb.members.len() as i64), lb.created);
                let better = match &best {
                    None => true,
                    Some((bk, _)) => k.0 < bk.0 || (k.0 == bk.0 && (k.1 < bk.1 || (k.1 == bk.1 && k.2 < bk.2))),
                };
                if better {
                    best = Some((k, id));
                }
            }
            best.map(|(_, id)| id)
        };

        if let Some(src) = src {
            let lb = self.src_lobby(src);
            if ok(&*self, lb.as_deref()) {
                let lid = lb.unwrap_or_default();
                if !solo && !self.lobbies[&lid].members.is_empty() && !near_ok(&*self, &lid) {
                    if let Some(alt) = best_open(&*self, Some(src), true, &mut ok, &mut near_ok) {
                        self.add(&alt, token, now, at);
                        return (alt, format!("own region (post-match lobby {} is mostly another region)", lid));
                    }
                }
                self.add(&lid, token, now, at);
                return (lid, "post-match lobby".into());
            }
            if lb.is_none() {
                let key = self.key_for_mid(src);
                let nb = self.new_lobby(now, Some(src), Some(&key));
                self.add(&nb, token, now, at);
                return (nb, "post-match lobby (first back)".into());
            }
        }
        if let Some(e) = &lf {
            if Some(e.0.as_str()) != blocked.as_deref() && now - e.1 <= REJOIN_S && ok(&*self, Some(&e.0)) {
                let lid = e.0.clone();
                self.add(&lid, token, now, at);
                return (lid, "rejoin".into());
            }
        }
        if !solo {
            // (best_open: every open lobby is a choice here)
            if let Some(id) = best_open(&*self, None, false, &mut ok, &mut near_ok) {
                self.add(&id, token, now, at);
                return (id, "joined".into());
            }
        }
        let nb = self.new_lobby(now, None, None);
        self.add(&nb, token, now, at);
        if let Some(l) = self.lobbies.get_mut(&nb) {
            l.hold = solo;
        }
        (nb, "new".into())
    }

    // py: lobbybook.py:190 away
    pub fn away(&mut self, token: &str, now: Option<f64>) {
        let n = now.unwrap_or_else(|| self.now());
        if let Some(lid) = self.of.get(token).cloned() {
            if let Some(lb) = self.lobbies.get_mut(&lid) {
                if !lb.away.contains_key(token) {
                    lb.away.insert(token.to_string(), n);
                }
            }
        }
    }

    // py: lobbybook.py:196 back
    pub fn back(&mut self, token: &str) {
        if let Some(lid) = self.of.get(token).cloned() {
            if let Some(lb) = self.lobbies.get_mut(&lid) {
                lb.away.shift_remove(token);
            }
        }
    }

    // py: lobbybook.py:201 remove
    pub fn remove(&mut self, token: &str, why: &str, now: Option<f64>, block_s: f64) -> Option<String> {
        let now = now.unwrap_or_else(|| self.now());
        let lid = self.take_out(token);
        if let Some(lid) = &lid {
            self.left.insert(token.to_string(), (lid.clone(), now, now + block_s));
            if !why.is_empty() {
                dlog!(format!("LOBBY-BOOK: {} left lobby {}: {}", tag8(token), lid, why));
            }
        }
        lid
    }

    // py: lobbybook.py:210 expire
    pub fn expire(&mut self, now: Option<f64>, grace: f64) -> Vec<(String, String)> {
        let now = now.unwrap_or_else(|| self.now());
        let mut out: Vec<(String, String)> = Vec::new();
        for lb in self.lobbies.values() {
            for (t, at) in &lb.away {
                if now - *at >= grace {
                    out.push((t.clone(), lb.lid.clone()));
                }
            }
        }
        for (t, _) in &out {
            self.remove(t, &format!("left the search (no search again within {} s)", py::fixed(grace, 0)), Some(now), 0.0);
        }
        let lim = REJOIN_S.max(BACKOUT_BLOCK_S) * 4.0;
        self.left.retain(|_, e| now - e.1 <= lim);
        out
    }

    // py: lobbybook.py:223 merge_pass
    pub fn merge_pass(&mut self, fits: Fits, cap: Cap, now: Option<f64>) -> Vec<(String, String)> {
        let now = now.unwrap_or_else(|| self.now());
        let mut done = Vec::new();
        let mut changed = true;
        while changed {
            changed = false;
            let ls: Vec<String> =
                self.launch_order().into_iter().filter(|lb| !lb.hold && lb.away.is_empty() && !lb.members.is_empty()).map(|lb| lb.lid.clone()).collect();
            let mut i = ls.len();
            while i > 1 {
                i -= 1;
                for j in 0..i {
                    let (mut a, mut b) = (ls[j].clone(), ls[i].clone());
                    let (Some(la), Some(lb_)) = (self.lobbies.get(&a), self.lobbies.get(&b)) else { continue };
                    if la.src.is_some() && lb_.src.is_some() {
                        continue;
                    }
                    if lb_.src.is_some() {
                        std::mem::swap(&mut a, &mut b);
                    }
                    let (Some(la), Some(lb_)) = (self.lobbies.get(&a), self.lobbies.get(&b)) else { continue };
                    let bm = lb_.members.clone();
                    if (la.members.len() + bm.len()) as i64 > cap(&*self, la, &bm) {
                        continue;
                    }
                    if bm.iter().any(|t| match self.left.get(t) {
                        Some(e) => e.0 == a && now < e.2,
                        None => false,
                    }) {
                        continue;
                    }
                    if !fits(&la.members.clone(), &bm) {
                        continue;
                    }
                    let blj = lb_.last_join;
                    let lj = la.last_join.max(blj);
                    for t in &bm {
                        self.add(&a, t, now, Some(blj));
                    }
                    if let Some(l) = self.lobbies.get_mut(&a) {
                        l.last_join = lj;
                    }
                    self.lobbies.shift_remove(&b);
                    done.push((a, b));
                    changed = true;
                    break;
                }
                if changed {
                    break;
                }
            }
        }
        done
    }

    // py: lobbybook.py:254 launched
    pub fn launched(&mut self, lid: &str, mid: &str) {
        if let Some(lb) = self.lobbies.get_mut(lid) {
            lb.mid = Some(mid.to_string());
            lb.hold = false;
            self.mid_lid.insert(mid.to_string(), lid.to_string());
            while self.mid_lid.len() > 512 {
                self.mid_lid.shift_remove_index(0);
            }
        }
    }

    // py: lobbybook.py:262 lobby_of_mid
    pub fn lobby_of_mid(&self, mid: &str) -> Option<&Lobby> {
        self.lobbies.values().find(|lb| lb.mid.as_deref() == Some(mid))
    }

    // py: lobbybook.py:268 cancelled
    pub fn cancelled(&mut self, mid: &str, leaver: &str, now: Option<f64>) -> Option<String> {
        let now = now.unwrap_or_else(|| self.now());
        let lid = self.lobby_of_mid(mid)?.lid.clone();
        if let Some(lb) = self.lobbies.get_mut(&lid) {
            lb.mid = None;
            lb.wait_until = now + RELAUNCH_S;
        }
        self.mid_lid.shift_remove(mid);
        let has = self.lobbies.get(&lid).map(|l| l.members.iter().any(|m| m == leaver)).unwrap_or(false);
        if has {
            self.remove(leaver, &format!("backed out of the countdown of {}", mid), Some(now), BACKOUT_BLOCK_S);
        }
        if let Some(lb) = self.lobbies.get(&lid) {
            if lb.members.is_empty() {
                self.lobbies.shift_remove(&lid);
            }
        }
        Some(lid)
    }

    // py: lobbybook.py:283 sync_matches
    pub fn sync_matches(
        &mut self,
        alive: &dyn Fn(&str) -> bool,
        started: &dyn Fn(&str) -> bool,
        queued: &[String],
        now: Option<f64>,
    ) {
        let now = now.unwrap_or_else(|| self.now());
        let qs: HashSet<&String> = queued.iter().collect();
        let ids: Vec<String> = self.lobbies.keys().cloned().collect();
        for id in ids {
            let Some(lb) = self.lobbies.get(&id) else { continue };
            let Some(mid) = lb.mid.clone() else { continue };
            if started(&mid) {
                let members = lb.members.clone();
                for t in members {
                    if self.of.get(&t).map(|s| s.as_str()) == Some(id.as_str()) {
                        self.of.remove(&t);
                    }
                }
                self.lobbies.shift_remove(&id);
            } else if !alive(&mid) {
                dlog!(format!("LOBBY-BOOK: match {} of lobby {} is gone before its start: the lobby is open again", mid, id));
                if let Some(lb) = self.lobbies.get_mut(&id) {
                    lb.mid = None;
                    lb.wait_until = now + RELAUNCH_S;
                    let mem: Vec<String> = lb.members.iter().filter(|t| !qs.contains(t)).cloned().collect();
                    for t in mem {
                        if !lb.away.contains_key(&t) {
                            lb.away.insert(t, now);
                        }
                    }
                }
            }
        }
    }
}

// ------------------------------------------------------------------ differential test (verify/py/lobbybook_harness.py)

fn state(b: &LobbyBook) -> Value {
    let mut lobbies = serde_json::Map::new();
    for (lid, lb) in &b.lobbies {
        let joined: std::collections::BTreeMap<&String, f64> = lb.joined.iter().map(|(k, v)| (k, *v)).collect();
        lobbies.insert(
            lid.clone(),
            json!({"members": lb.members, "joined": joined, "away": lb.away.iter().map(|(k, v)| json!([k, v])).collect::<Vec<_>>(),
                   "created": lb.created, "last_join": lb.last_join, "mid": lb.mid, "src": lb.src, "hold": lb.hold, "wait_until": lb.wait_until}),
        );
    }
    let of: std::collections::BTreeMap<&String, &String> = b.of.iter().collect();
    let left: std::collections::BTreeMap<&String, Value> = b.left.iter().map(|(k, e)| (k, json!([e.0, e.1, e.2]))).collect();
    let order: Vec<&String> = b.lobbies.keys().collect();
    let ml: Vec<Value> = b.mid_lid.iter().map(|(k, v)| json!([k, v])).collect();
    json!({"lobbies": Value::Object(lobbies), "order": order, "of": of, "left": left, "mid_lid": ml})
}

fn pairs(v: &[(String, String)]) -> Value {
    Value::Array(v.iter().map(|(a, b)| json!([a, b])).collect())
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let clk = crate::c_util::Frozen::new();
    let mut b = LobbyBook::with_clock(8, clk.now());
    for (i, c) in script(input).iter().enumerate() {
        clk.set(jf(c, "t"));
        let r: Value = match js(c, "op") {
            "place" => {
                let unit = jstrs(c, "unit");
                let src = c.get("src").and_then(|x| x.as_str()).map(|s| s.to_string());
                let at = c.get("at").and_then(|x| x.as_f64());
                let cap_n = c.get("cap").and_then(|x| x.as_i64()).unwrap_or(8);
                let nofit: HashSet<String> = jstrs(c, "nofit").into_iter().collect();
                let capx: HashMap<String, i64> = c
                    .get("capx")
                    .and_then(|x| x.as_object())
                    .map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_i64().unwrap_or(0))).collect())
                    .unwrap_or_default();
                let mut fits = |m: &[String], n: &[String]| !m.iter().chain(n.iter()).any(|t| nofit.contains(t));
                let mut cap = |_b: &LobbyBook, lb: &Lobby, _n: &[String]| cap_n - lb.members.iter().map(|m| capx.get(m).copied().unwrap_or(0)).sum::<i64>();
let reg: Option<HashMap<String, String>> = c
                    .get("reg")
                    .and_then(|x| x.as_object())
                    .map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string())).collect());
                let g = |reg: &HashMap<String, String>, ts: &[String]| ts.iter().find_map(|t| reg.get(t).cloned());
                let mut near_f = |m: &[String], n: &[String]| {
                    let Some(reg) = &reg else { return true };
                    let fresh: Vec<String> = n.iter().filter(|t| !m.contains(t)).cloned().collect();
                    !matches!((g(reg, m), g(reg, &fresh)), (Some(a), Some(b)) if a != b)
                };
                let near: Option<Fits> = if reg.is_some() { Some(&mut near_f) } else { None };
                let (lid, why) = b.place(js(c, "token"), &unit, &mut fits, &mut cap, Some(jf(c, "t")), src.as_deref(), jb(c, "solo"), at, near);
                json!([lid, why])
            }
            "away" => {
                b.away(js(c, "token"), Some(jf(c, "t")));
                Value::Null
            }
            "back" => {
                b.back(js(c, "token"));
                Value::Null
            }
            "remove" => json!(b.remove(js(c, "token"), js(c, "why"), Some(jf(c, "t")), jf(c, "block"))),
            "expire" => pairs(&b.expire(Some(jf(c, "t")), jf(c, "grace"))),
            "merge" => {
                let cap_n = c.get("cap").and_then(|x| x.as_i64()).unwrap_or(8);
                let nofit: HashSet<String> = jstrs(c, "nofit").into_iter().collect();
                let mut fits = |m: &[String], n: &[String]| !m.iter().chain(n.iter()).any(|t| nofit.contains(t));
                let mut cap = |_b: &LobbyBook, _lb: &Lobby, _n: &[String]| cap_n;
                pairs(&b.merge_pass(&mut fits, &mut cap, Some(jf(c, "t"))))
            }
            "launched" => {
                let lid = b.of.get(js(c, "tok")).cloned().unwrap_or_default();
                b.launched(&lid, js(c, "mid"));
                Value::Null
            }
            "cancelled" => json!(b.cancelled(js(c, "mid"), js(c, "leaver"), Some(jf(c, "t")))),
            "sync" => {
                let alive: HashSet<String> = jstrs(c, "alive").into_iter().collect();
                let started: HashSet<String> = jstrs(c, "started").into_iter().collect();
                b.sync_matches(&|m| alive.contains(m), &|m| started.contains(m), &jstrs(c, "queued"), Some(jf(c, "t")));
                Value::Null
            }
            "set_hold" => {
                let lid = b.of.get(js(c, "tok")).cloned().unwrap_or_default();
                b.set_hold(&lid, jb(c, "hold"));
                Value::Null
            }
            "q" => {
                let t = js(c, "token");
                let mid = js(c, "mid");
                let lid = b.of.get(js(c, "tok")).cloned().unwrap_or_default();
                let lid = lid.as_str();
                json!({"lobby_of": b.lobby_of(t), "members": b.members(lid), "mates": b.mates(t), "key": b.key_for_mid(mid),
                       "of_mid": b.lobby_of_mid(mid).map(|l| l.lid.clone()), "src": b.src_lobby(mid),
                       "open": b.open_lobbies().iter().map(|l| l.lid.clone()).collect::<Vec<_>>(),
                       "order": b.launch_order().iter().map(|l| l.lid.clone()).collect::<Vec<_>>(),
                       "get": b.get(lid).map(|l| l.members.clone())})
            }
            _ => Value::Null,
        };
        crate::c_util::emit(out, i, r, state(&b));
    }
    0
}
