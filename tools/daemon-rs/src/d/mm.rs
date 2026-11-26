//! K4 mm: the MatchMaker (daemon.py:4861-5870 + the MM singleton). Python `with self.lock:` = `MM.lock.lock()`; the methods that say "(lock held)"
//! in Python are `impl MMState` methods here. The join-in-progress half (jip_pick / jip_join ...) is in jip.rs.
use super::*;
use crate::c_util::float_floordiv;
use crate::lobbybook::{self, Lobby, LobbyBook};
use crate::py;
use crate::requeue::Requeue;
use crate::{clock, hostmig, map_packs, modeq, rnd, social};
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

pub type Formed = Option<(String, Vec<String>)>;

/// `str([tag(t) for t in tokens])`
pub fn tags(v: &[String]) -> String {
    py::list_str(&v.iter().map(|t| tag(t)).collect::<Vec<_>>())
}

/// `str(x)` of an optional int the way a log line prints it (`None` or the number)
pub fn oi(v: Option<i64>) -> String {
    match v {
        Some(x) => x.to_string(),
        None => "None".to_string(),
    }
}

// py: daemon.py:4861 class MatchMaker (the fields; the lock is MatchMaker.lock)
pub struct MMState {
    pub queue: IndexMap<String, f64>,
    pub seen: HashMap<String, f64>,
    pub matches: IndexMap<String, Vec<String>>,
    pub formed: HashMap<String, f64>,
    pub left: HashMap<String, (f64, f64)>,
    pub settle: HashMap<String, f64>,
    pub qtime: HashMap<String, IndexMap<String, f64>>,
    pub launched: HashSet<String>,
    pub departed: HashMap<String, HashSet<String>>,
    pub ended: HashSet<String>,
    pub ended_at: HashMap<String, f64>,
    pub timed_out: HashSet<String>,
    pub started: HashSet<String>,
    pub started_t: HashMap<String, f64>,
    pub last_arrival: f64,
    pub hold_since: f64,
    pub rq: Requeue,
    pub rq_go: Vec<(String, Vec<String>, Vec<String>)>,
    pub rq_logged: f64,
    pub book: LobbyBook,
    pub departed_vol: HashSet<String>,
    pub departed_t: HashMap<String, f64>,
    /// match id -> (seconds left, duration, ended, time of the report)
    pub clock: HashMap<String, (f64, f64, bool, f64)>,
    pub polled: HashMap<String, HashMap<String, f64>>,
    /// token -> (match id, time it was put into the running match)
    pub joining: IndexMap<String, (String, f64)>,
    /// match id -> (team 0 lives, team 1 lives, time)
    pub lives: HashMap<String, (i64, i64, f64)>,
    pub late_bonus: HashMap<String, (String, i64)>,
    /// token -> (match id, team wanted, until)
    pub move_team: IndexMap<String, (String, i64, f64)>,
    pub late_mult: HashMap<String, (String, f64)>,
    /// token -> (match id, progress 0..1, behind 0/1): how far the match was when a backfill joined it (CLAN_FAIR, boosters)
    pub late_prog: HashMap<String, (String, f64, i64)>,
    /// (lobby, missing mates) -> last log time (R-5ZCJ wait line)
    pub party_split_log: HashMap<(String, Vec<String>), f64>,
    // the getattr(self, '_x_logged', 0.0) throttles
    pub qhold_logged: f64,
    pub pwait_logged: f64,
    pub pwait_out_logged: f64,
    pub pgate_logged: f64,
    pub book_short_logged: f64,
    pub pneed_noted: f64,
    pub phold_logged: f64,
    pub wait_logged: f64,
    pub party_logged: f64,
    pub lobby_fill_logged: f64,
}

pub struct MatchMaker {
    pub lock: MmMutex,
}

pub static MM: LazyLock<MatchMaker> = LazyLock::new(|| MatchMaker { lock: MmMutex::new(MMState::new()) });

/// `MatchMaker._in_match` on the fields (usable while another field is borrowed)
pub fn in_match_in(matches: &IndexMap<String, Vec<String>>, departed: &HashMap<String, HashSet<String>>, ended: &HashSet<String>, token: &str) -> bool {
    matches.iter().any(|(mid, pl)| pl.iter().any(|x| x == token) && !departed.get(mid).is_some_and(|d| d.contains(token)) && !ended.contains(mid))
}

/// one throttled log line: true when `last` is at least `every` s old (and sets it)
fn throttle(last: &mut f64, now: f64, every: f64) -> bool {
    if now - *last >= every {
        *last = now;
        true
    } else {
        false
    }
}

/// `PARTIES.leader(t)` as the borrowed form the helpers take
fn leader_of(t: &str) -> Option<String> {
    PARTIES.leader(t)
}

/// the view of a lobby `_form_pick` needs while the book is not borrowed
#[derive(Clone)]
pub struct LobbyView {
    pub lid: String,
    pub last_join: f64,
    pub members: Vec<String>,
}

// py: daemon.py:4994 _book_single_unit
pub fn book_single_unit(members: &[String]) -> bool {
    if members.len() <= 1 {
        return true;
    }
    let p = PARTIES.pid(&members[0]);
    p.as_ref().is_some_and(|p| !p.is_empty()) && members.iter().all(|t| PARTIES.pid(t) == p)
}

// py: daemon.py:5003 _book_unit
pub fn book_unit(token: &str) -> Vec<String> {
    if PARTIES.pid(token).is_some() && PARTIES.members(token).len() > 1 {
        return PARTIES.members(token);
    }
    vec![token.to_string()]
}

// py: daemon.py:5027 _book_fits
pub fn book_fits(members: &[String], newcomers: &[String], rq: &Requeue, queue: &IndexMap<String, f64>) -> bool {
    let mut allp: Vec<String> = members.to_vec();
    for t in newcomers {
        if !members.contains(t) {
            allp.push(t.clone());
        }
    }
    if !modeq::forced() {
        let ms: HashSet<i64> = allp.iter().map(|t| _search_mode(t)).filter(|m| *m != modeq::ANY).collect();
        if ms.len() > 1 {
            return false;
        }
    }
    if p1_pref_on() {
        let ld = |t: &str| PARTIES.leader(t);
        if allp.iter().any(|t| p1_always(t, ld(t).as_deref())) && !allp.iter().all(|t| p1_always(t, ld(t).as_deref()) || p1_owner(t, ld(t).as_deref())) {
            return false;
        }
    }
    if !members.is_empty() && !newcomers.is_empty() {
        // 11-26 REGION PREFERENCE: a hard rule, never relaxed by the wait below
        let ua: Vec<Vec<String>> = members.iter().map(|m| book_unit(m)).collect();
        let ub: Vec<Vec<String>> = newcomers.iter().filter(|t| !members.contains(t)).map(|t| book_unit(t)).collect();
        if !region_lock_fits(&ua, &ub, true) {
            return false;
        }
    }
    if *MM_REGION_ON && *MM_REGION_LOBBY && !members.is_empty() && !newcomers.is_empty() {
        let fresh: Vec<String> = newcomers.iter().filter(|t| !members.contains(t)).cloned().collect();
        let (ga, gb) = (region_group(members), region_group(&fresh));
        if let (Some(ga), Some(gb)) = (&ga, &gb) {
            if ga != gb {
                let now = clock::now();
                // the post-match lobby of one match stays together
                let srcs: HashSet<String> = members.iter().filter_map(|t| rq.mid_of(t)).collect();
                let same = newcomers.iter().any(|t| rq.mid_of(t).is_some_and(|m| srcs.contains(&m)));
                let waited = allp.iter().map(|t| now - queue.get(t).copied().unwrap_or(now)).fold(f64::NEG_INFINITY, f64::max);
                if !same && waited < *MM_REGION_WAIT_S {
                    return false;
                }
            }
        }
    }
    if *MM_PING_GATE && *PING_KICK_MS > 0 && allp.len() >= 2 {
        let now = clock::now();
        let (_kept, dropped, _h, _i) = hostmig::ping_fit(
            &allp,
            &|a, b| _ping_gate(a, b, now),
            ping_match_ms(),
            Some(&|t: &str| PARTIES.members(t)),
            Some(&|t: &str| !no_host(t)),
        );
        if !dropped.is_empty() {
            return false;
        }
    }
    true
}

// py: daemon.py:5060 _book_near
/// Same region group (EU / NA / other), or one side unknown: the lobby book places by region first (never a block).
pub fn book_near(members: &[String], newcomers: &[String]) -> bool {
    let fresh: Vec<String> = newcomers.iter().filter(|t| !members.contains(t)).cloned().collect();
    match (region_group(members), region_group(&fresh)) {
        (Some(a), Some(b)) => a == b,
        _ => true,
    }
}

impl MMState {
    pub fn new() -> MMState {
        MMState {
            queue: IndexMap::new(),
            seen: HashMap::new(),
            matches: IndexMap::new(),
            formed: HashMap::new(),
            left: HashMap::new(),
            settle: HashMap::new(),
            qtime: HashMap::new(),
            launched: HashSet::new(),
            departed: HashMap::new(),
            ended: HashSet::new(),
            ended_at: HashMap::new(),
            timed_out: HashSet::new(),
            started: HashSet::new(),
            started_t: HashMap::new(),
            last_arrival: 0.0,
            hold_since: 0.0,
            rq: Requeue::new(
                *REQUEUE_WAIT_MAX,
                *REQUEUE_TICKET_S,
                *REQUEUE_OFFLINE_S,
                if *LOBBY_KEEP { *LOBBY_RELEASE_S } else { 0.0 },
                if *LOBBY_KEEP { *LOBBY_MIN_GAP_S } else { 0.0 },
            ),
            rq_go: Vec::new(),
            rq_logged: 0.0,
            book: LobbyBook::new(*MATCH_SIZE as usize),
            departed_vol: HashSet::new(),
            departed_t: HashMap::new(),
            clock: HashMap::new(),
            polled: HashMap::new(),
            joining: IndexMap::new(),
            lives: HashMap::new(),
            late_bonus: HashMap::new(),
            move_team: IndexMap::new(),
            late_mult: HashMap::new(),
            late_prog: HashMap::new(),
            party_split_log: HashMap::new(),
            qhold_logged: 0.0,
            pwait_logged: 0.0,
            pwait_out_logged: 0.0,
            pgate_logged: 0.0,
            book_short_logged: 0.0,
            pneed_noted: 0.0,
            phold_logged: 0.0,
            wait_logged: 0.0,
            party_logged: 0.0,
            lobby_fill_logged: 0.0,
        }
    }

    // py: daemon.py:5470 _in_match
    pub fn in_match_locked(&self, token: &str) -> bool {
        in_match_in(&self.matches, &self.departed, &self.ended, token)
    }

    // py: daemon.py:5606 _mid_locked
    pub fn mid_locked(&self, token: &str) -> Option<String> {
        for skip_ended in [true, false] {
            for mid in self.matches.keys().rev() {
                let pl = &self.matches[mid];
                if pl.iter().any(|x| x == token) && !self.departed.get(mid).is_some_and(|d| d.contains(token)) && !(skip_ended && self.ended.contains(mid)) {
                    return Some(mid.clone());
                }
            }
        }
        None
    }

    // py: daemon.py:5497 _forget
    pub fn forget_locked(&mut self, mid: &str) {
        PRIV.match_over(mid);
        self.matches.shift_remove(mid);
        self.qtime.remove(mid);
        self.formed.remove(mid);
        self.launched.remove(mid);
        self.departed.remove(mid);
        self.ended.remove(mid);
        self.ended_at.remove(mid);
        self.timed_out.remove(mid);
        self.started.remove(mid);
        self.started_t.remove(mid);
        self.clock.remove(mid);
        self.polled.remove(mid);
        self.lives.remove(mid);
        self.joining.retain(|_, (m, _)| m != mid);
    }

    // py: daemon.py:5364 _form_private
    fn form_private(&mut self, lb: &PrivLobby, have: &[String]) -> (String, Vec<String>) {
        let teams = lb.rules.get("teams").and_then(|v| v.as_i64()).unwrap_or(0);
        let players = if teams == 0 {
            team_order(have)
        } else {
            let (t0, t1) = PRIV.teams_for(lb, have, &|pl: &[String]| {
                (pl.iter().step_by(2).cloned().collect(), pl.iter().skip(1).step_by(2).cloned().collect())
            });
            let bo = if have.len() >= 2 { best_host_order(have) } else { have.to_vec() };
            let big = if t0.len() >= t1.len() { &t0 } else { &t1 };
            let host = bo.iter().find(|h| big.contains(h)).cloned();
            order_slots(&t0, &t1, host.as_deref())
        };
        let mid = rnd::token_hex("daemon:_form_private", 4);
        let mut qt = IndexMap::new();
        for t in &players {
            qt.insert(t.clone(), self.queue.get(t).copied().unwrap_or(0.0));
        }
        self.qtime.insert(mid.clone(), qt);
        for t in &players {
            self.queue.shift_remove(t);
            self.rq.drop(t);
        }
        self.matches.insert(mid.clone(), players.clone());
        self.formed.insert(mid.clone(), clock::now());
        self.hold_since = 0.0;
        let mode = lb.rules.get("mode").and_then(|v| v.as_i64()).unwrap_or(0);
        let modes = modeq::modes();
        let mname = if mode >= 0 && (mode as usize) < modeq::nmodes() { modes[mode as usize].clone() } else { modes[0].clone() };
        MODEQ.formed(&mid, Some(&mname));
        PRIV.bound(lb, &mid, &players);
        dlog!("MATCH formed (private)", mid, lb.code, tags(&players), mname);
        (mid, players)
    }

    // py: daemon.py:5391 _rq_filter
    pub fn rq_filter(&mut self, ready: Vec<String>, now: f64, form: bool) -> Vec<String> {
        self.rq_go = Vec::new();
        if !*REQUEUE {
            return ready;
        }
        let qkeys: Vec<String> = self.queue.keys().cloned().collect();
        let (mut held, waiting, gone) = {
            let (matches, departed, ended) = (&self.matches, &self.departed, &self.ended);
            let inm = |t: &str| in_match_in(matches, departed, ended, t);
            let sil = |t: &str| silence(t, now);
            let (held, waiting, gone) = self.rq.held(&qkeys, &sil, &inm, Some(now));
            let go = if form && *LOBBY_KEEP { Some(self.rq.ready_groups(&qkeys, &sil, &inm, Some(now))) } else { None };
            if let Some(g) = go {
                self.rq_go = g;
            }
            (held, waiting, gone)
        };
        for (t, why) in &gone {
            dlog!("requeue:", tag(t), "ticket ended:", why);
        }
        if form && *LOBBY_KEEP {
            let extra = self.lobby_holds(&ready, &held, &waiting, now);
            held.extend(extra);
        }
        if held.is_empty() {
            return ready;
        }
        if now - self.rq_logged >= 20.0 {
            self.rq_logged = now;
            let mut w = serde_json::Map::new();
            for (m, wt) in &waiting {
                w.insert(m.clone(), serde_json::Value::Array(wt.iter().map(|t| serde_json::Value::String(tag(t))).collect()));
            }
            let held_ready: Vec<String> = ready.iter().filter(|t| held.contains(*t)).cloned().collect();
            dlog!("requeue: HOLD", tags(&held_ready), "wait for the post-match screens of", py::pyrepr(&serde_json::Value::Object(w)),
                format!("(at most {} s after the match end)", fmt0(*REQUEUE_WAIT_MAX)));
        }
        ready.into_iter().filter(|t| !held.contains(t)).collect()
    }

    // py: daemon.py:5420 _lobby_holds
    fn lobby_holds(&mut self, ready: &[String], held: &HashSet<String>, waiting: &IndexMap<String, Vec<String>>, now: f64) -> HashSet<String> {
        let mut extra: HashSet<String> = HashSet::new();
        let tick: HashSet<String> = self.rq.tokens().into_iter().collect();
        for t in ready {
            let nm = if tick.contains(t) { self.rq.next_of(t) } else { None };
            if let Some(nm) = nm {
                if !nm.is_empty() && self.matches.contains_key(&nm) && !self.ended.contains(&nm) && !self.started.contains(&nm) {
                    extra.insert(t.clone());
                }
            }
        }
        let others: Vec<String> = ready.iter().filter(|t| !held.contains(*t) && !tick.contains(*t) && !extra.contains(*t)).cloned().collect();
        if !others.is_empty() && !waiting.is_empty() && !book_on() {
            for (gmid, post) in waiting {
                let size = post.len() as i64 + held.iter().filter(|t| self.rq.mid_of(t).as_deref() == Some(gmid.as_str())).count() as i64;
                if size + others.len() as i64 <= *MATCH_SIZE {
                    extra.extend(others.iter().cloned());
                    if throttle(&mut self.lobby_fill_logged, now, 20.0) {
                        dlog!("LOBBY: hold", tags(&others), "to fill the open seats of the lobby of match", gmid,
                            format!("({} members, still on the post-match screens: {})", size, tags(post)));
                    }
                    break;
                }
            }
        }
        extra
    }

    // py: daemon.py:5443 rq_open
    pub fn rq_open(&mut self, mid: &str, now: f64) -> (Vec<String>, Vec<String>) {
        let empty = HashSet::new();
        let dep = self.departed.get(mid).unwrap_or(&empty);
        let ph = "f0".repeat(30);
        let act: Vec<String> =
            self.matches.get(mid).map(|v| v.as_slice()).unwrap_or(&[]).iter().filter(|t| !dep.contains(*t) && is_token(t) && !t.starts_with(&ph)).cloned().collect();
        let cap0: Vec<String> = {
            let rc = RQ_CAP.lock();
            act.iter().filter(|t| now - rc.get(*t).copied().unwrap_or(0.0) <= *REQUEUE_CAP_S).cloned().collect()
        };
        let cap: Vec<String> = cap0.iter().filter(|t| PARTIES.pid(t).is_none() || PARTIES.leader(t).is_some_and(|l| cap0.contains(&l))).cloned().collect();
        let left: Vec<String> = act.iter().filter(|t| !cap.contains(t)).cloned().collect();
        (self.rq.open(mid, &cap, Some(now)), left)
    }

    // py: daemon.py:5456 _log_party_hold
    fn log_party_hold(&mut self, held: &IndexMap<String, Vec<String>>, now: f64) {
        if !held.is_empty() && throttle(&mut self.party_logged, now, 20.0) {
            let mut m = serde_json::Map::new();
            for (pid, miss) in held {
                m.insert(pid.clone(), serde_json::Value::Array(miss.iter().map(|t| serde_json::Value::String(tag(t))).collect()));
            }
            dlog!("PARTY-WAIT", py::pyrepr(&serde_json::Value::Object(m)), "(every member must be searching)");
        }
    }

    // py: daemon.py:5462 fast_want
    pub fn fast_want(&self, ready: &[String], online: &HashSet<String>, quiet: f64, now: f64) -> (i64, &'static str) {
        let step = if *FAST_STEP > 0.0 { *FAST_STEP } else { *FILL_WAIT };
        let mut want = if step > 0.0 { std::cmp::max(*MIN_PLAYERS, *MATCH_SIZE - float_floordiv(quiet, step) as i64) } else { *MIN_PLAYERS };
        let mut why = "step";
        let mut inm: HashSet<&String> = HashSet::new();
        for (mid, pl) in &self.matches {
            if self.ended.contains(mid) {
                continue;
            }
            for t in pl {
                if !self.departed.get(mid).is_some_and(|d| d.contains(t)) {
                    inm.insert(t);
                }
            }
        }
        let cand = online.iter().filter(|t| !self.queue.contains_key(*t) && !inm.contains(*t)).count();
        if !ready.is_empty() && *FAST_MAX_WAIT > 0.0 && now - self.queue.get(&ready[0]).copied().unwrap_or(now) >= *FAST_MAX_WAIT {
            want = *MIN_PLAYERS;
            why = "max-wait";
        } else if cand == 0 && quiet >= *FAST_ALONE {
            want = *MIN_PLAYERS;
            why = "nobody-else-online";
        }
        (want, why)
    }

    // py: daemon.py:5020 _book_cap (the lobby `lb` seen by the book callbacks)
    pub fn book_cap_of(book: &LobbyBook, rq: &Requeue, queue: &IndexMap<String, f64>, matches: &IndexMap<String, Vec<String>>,
        departed: &HashMap<String, HashSet<String>>, ended: &HashSet<String>, lb: &Lobby, incoming: &[String]) -> i64 {
        let inc: HashSet<&String> = incoming.iter().collect();
        let mut res: HashSet<String> = HashSet::new();
        if let Some(src) = &lb.src {
            if *REQUEUE && *LOBBY_KEEP {
                for t in rq.tokens() {
                    if !inc.contains(&t) && !book.of.contains_key(&t) && !queue.contains_key(&t) && rq.mid_of(&t).as_deref() == Some(src.as_str())
                        && rq.has(&t, None) && !in_match_in(matches, departed, ended, &t)
                    {
                        res.insert(t);
                    }
                }
            }
        }
        for t in &lb.members {
            for m in book_unit(t) {
                if !inc.contains(&m) && !book.of.contains_key(&m) && &m != t {
                    res.insert(m);
                }
            }
        }
        // 11-25 R-5ZCJ: the incoming side's party mates that are in no lobby yet need their seats too (a merge took a party of 4 in as 3
        // into a lobby of 5, which then started with 8 and without the party leader)
        for t in incoming {
            for m in book_unit(t) {
                if !inc.contains(&m) && !lb.members.contains(&m) && !book.of.contains_key(&m) {
                    res.insert(m);
                }
            }
        }
        *MATCH_SIZE - res.len() as i64
    }

    // py: daemon.py:5038 _book_jip_hold
    fn book_jip_hold(&mut self, token: &str, unit: &[String], now: f64) -> bool {
        if !*JIP {
            return false;
        }
        if *QUEUE_HOLD_MAX > 0.0 {
            let m = unit.iter().filter(|u| self.queue.contains_key(*u) || u.as_str() == token).map(|u| self.queue.get(u).copied().unwrap_or(now)).fold(f64::INFINITY, f64::min);
            if now - m >= *QUEUE_HOLD_MAX {
                return false;
            }
        }
        let need = unit.len() as i64;
        if self.jip_pick(token, &mut Vec::new(), Some(*JIP_PREFER_LEFT), need, None, None).is_some() {
            return true;
        }
        for (mid, pl) in &self.matches {
            if self.ended.contains(mid) || pl.is_empty() {
                continue;
            }
            let young = !self.started.contains(mid) || (self.started_t.contains_key(mid) && now - self.started_t[mid] < *JIP_MIN_AGE + 5.0);
            if !young || now - self.formed.get(mid).copied().unwrap_or(now) >= 180.0 {
                continue;
            }
            let ld = PARTIES.leader(token);
            if !MODEQ.fits(token, mid, ld.as_deref()) || !p1_fits(token, mid, ld.as_deref()) {
                continue;
            }
            let dep = self.departed.get(mid);
            let act: Vec<String> = pl.iter().filter(|t| !dep.is_some_and(|d| d.contains(*t))).cloned().collect();
            if act.len() as i64 + need > *MATCH_SIZE {
                continue;
            }
            let joining: HashSet<String> = self.joining.keys().cloned().collect();
            if *MM_PING_GATE && jip_ping_block(token, mid, &pl[0], &act, &joining, now).is_some() {
                continue;
            }
            return true;
        }
        false
    }

    // py: daemon.py:5077 _book_form
    fn book_form(&mut self, ready: &[String], held: &IndexMap<String, Vec<String>>, now: f64, online: &HashSet<String>) -> Formed {
        {
            let (matches, started, ended) = (&self.matches, &self.started, &self.ended);
            let queued: Vec<String> = self.queue.keys().cloned().collect();
            self.book.sync_matches(&|m| matches.contains_key(m), &|m| started.contains(m) || ended.contains(m), &queued, Some(now));
        }
        self.book.expire(Some(now), lobbybook::GRACE_S);
        let qk: Vec<String> = self.queue.keys().cloned().collect();
        let pubq = PRIV.split_ready(&qk).0;
        let mut tick: HashSet<String> = if *REQUEUE && *LOBBY_KEEP { self.rq.tokens().into_iter().collect() } else { HashSet::new() };
        let full = pubq.len() as i64 >= *MATCH_SIZE;
        let mut sorted_pub = pubq.clone();
        sorted_pub.sort_by(|a, b| {
            let (x, y) = (self.queue.get(a).copied().unwrap_or(now), self.queue.get(b).copied().unwrap_or(now));
            x.partial_cmp(&y).unwrap_or(std::cmp::Ordering::Equal)
        });
        for t in &sorted_pub {
            if self.book.lobby_of(t).is_some() {
                self.book.back(t);
                continue;
            }
            let mut src: Option<String> = None;
            let unit = book_unit(t);
            if tick.contains(t) && unit.len() > 1 && unit.iter().any(|u| self.rq.mid_of(u) != self.rq.mid_of(t)) {
                tick.remove(t); // 11-25 R-5ZCJ: a party formed since that match: a new search
            }
            if tick.contains(t) {
                let nm = self.rq.next_of(t);
                if let Some(nm) = nm {
                    if !nm.is_empty() && self.matches.contains_key(&nm) && !self.ended.contains(&nm) {
                        continue;
                    }
                }
                src = self.rq.mid_of(t);
            }
            let mut solo = unit.len() == 1 || unit.iter().all(|u| self.queue.contains_key(u));
            solo = solo && !full && self.book_jip_hold(t, &unit, now);
            let at = self.queue.get(t).copied().unwrap_or(now);
            let (lid, why) = {
                let MMState { book, rq, queue, matches, departed, ended, .. } = &mut *self;
                let (rq, queue, matches, departed, ended) = (&*rq, &*queue, &*matches, &*departed, &*ended);
                let mut near = |m: &[String], n: &[String]| book_near(m, n);
                book.place(
                    t,
                    &unit,
                    &mut |m: &[String], n: &[String]| book_fits(m, n, rq, queue),
                    &mut |bk: &LobbyBook, lb: &Lobby, inc: &[String]| MMState::book_cap_of(bk, rq, queue, matches, departed, ended, lb, inc),
                    Some(now),
                    src.as_deref(),
                    solo,
                    Some(at),
                    if *MM_REGION_ON && *MM_REGION_LOBBY { Some(&mut near) } else { None },
                )
            };
            dlog!("LOBBY-BOOK:", tag(t), why, lid, tags(&self.book.members(&lid)), if solo { "(held for a join in progress)" } else { "" });
        }
        let opens: Vec<(String, Vec<String>, bool)> = self.book.open_lobbies().into_iter().map(|l| (l.lid.clone(), l.members.clone(), l.hold)).collect();
        for (lid, members, hold) in opens {
            if !hold {
                continue;
            }
            let keep = !full && book_single_unit(&members) && {
                let unit = book_unit(&members[0]);
                self.book_jip_hold(&members[0], &unit, now)
            };
            if !keep {
                self.book.set_hold(&lid, false);
            }
        }
        {
            let MMState { book, rq, queue, matches, departed, ended, .. } = &mut *self;
            let (rq, queue, matches, departed, ended) = (&*rq, &*queue, &*matches, &*departed, &*ended);
            let merged = book.merge_pass(
                &mut |m: &[String], n: &[String]| book_fits(m, n, rq, queue),
                &mut |bk: &LobbyBook, lb: &Lobby, inc: &[String]| MMState::book_cap_of(bk, rq, queue, matches, departed, ended, lb, inc),
                Some(now),
            );
            for (a, b) in merged {
                dlog!("LOBBY-BOOK: lobby", b, "merged into", a, tags(&book.members(&a)));
            }
        }
        let rset: HashSet<&String> = ready.iter().collect();
        let pset: HashSet<&String> = pubq.iter().collect();
        let order: Vec<String> = self.book.launch_order().into_iter().map(|l| l.lid.clone()).collect();
        for olid in order {
            let Some(lb) = self.book.get(&olid).cloned() else { continue }; // (the Python loop reads the live lobby: an earlier pass step may have emptied it)
            let mem = lb.members.clone();
            if mem.is_empty() || !lb.away.is_empty() || lb.hold || now < lb.wait_until || mem.iter().any(|t| !rset.contains(t)) {
                continue;
            }
            // 11-25 R-5ZCJ: a party starts together. A searching mate of a member that is not in this lobby: no start; a mate sitting in
            // another lobby leaves it, so that the next pass places it with its party (place(): the same lobby, else the party moves)
            let mut miss: Vec<String> = Vec::new();
            for t in &mem {
                for m in book_unit(t) {
                    if !mem.contains(&m) && pset.contains(&m) && !miss.contains(&m) {
                        miss.push(m);
                    }
                }
            }
            miss.sort();
            if !miss.is_empty() {
                for m in &miss {
                    if self.book.lobby_of(m).is_some() {
                        self.book.remove(m, &format!("its party is in lobby {}", lb.lid), None, 0.0);
                    }
                }
                if self.party_split_log.len() > 500 {
                    self.party_split_log.clear();
                }
                let k = (lb.lid.clone(), miss.clone());
                if self.party_split_log.get(&k).copied().unwrap_or(0.0) < now - 30.0 {
                    self.party_split_log.insert(k, now);
                    dlog!("LOBBY-BOOK: lobby", lb.lid, "waits for party mate(s)", tags(&miss), "(a party starts together)");
                }
                continue;
            }
            let mut sub = mem.clone();
            sub.sort_by(|a, b| {
                let (x, y) = (self.queue.get(a).copied().unwrap_or(now), self.queue.get(b).copied().unwrap_or(now));
                x.partial_cmp(&y).unwrap_or(std::cmp::Ordering::Equal)
            });
            let view = LobbyView { lid: lb.lid.clone(), last_join: lb.last_join, members: mem };
            if let Some((mid, players)) = self.form_pick(&sub, held, now, online, Some(&view)) {
                self.book.launched(&lb.lid, &mid);
                return Some((mid, players));
            }
        }
        None
    }

    /// `_live(mid, pl)` of the second-match hold (the closure inside `_form_pick`)
    fn second_match_live(&self, mid: &str, pl: &[String], ready: &[String], grp: &[String], now: f64) -> bool {
        let dep = self.departed.get(mid);
        let in_dep = |t: &String| dep.is_some_and(|d| d.contains(t));
        if self.ended.contains(mid) || pl.iter().all(in_dep) {
            return false;
        }
        let ck = self.clock.get(mid);
        let joining: HashSet<String> = self.joining.keys().cloned().collect();
        if !self.started.contains(mid) {
            if *MM_PING_GATE && !pl.is_empty() && ready.iter().all(|t| jip_ping_block(t, mid, &pl[0], pl, &joining, now).is_some()) {
                return false;
            }
            return now - self.formed.get(mid).copied().unwrap_or(now) < 180.0;
        }
        if ck.is_none() && now - self.formed.get(mid).copied().unwrap_or(0.0) < 300.0 {
            // (the map-fit check below still applies)
        } else if !(ck.is_some_and(|c| now - c.3 <= *JIP_CLOCK_TTL && !c.2)) {
            return false;
        }
        if self.started.contains(mid)
            && !ready.iter().any(|t| {
                let ld = PARTIES.leader(t);
                MODEQ.fits(t, mid, ld.as_deref())
                    && p1_fits(t, mid, ld.as_deref())
                    && self.move_team.get(t).map(|m| m.0.as_str()).unwrap_or(mid) == mid
            })
        {
            return false;
        }
        let cm = match_map_get(&key_of(pl));
        let act: Vec<String> = pl.iter().filter(|t| !in_dep(t)).cloned().collect();
        if self.started.contains(mid) {
            let hmi = if HM.enabled { HM.host_info(mid) } else { None };
            let hs = hmi.map(|h| h.host).unwrap_or(0);
            if hs < pl.len() && ready.iter().all(|t| jip_ping_block(t, mid, &pl[hs], &act, &joining, now).is_some()) {
                return false;
            }
        }
        if *RELAY_MATCH_MAX > 0 && self.started.contains(mid) && match_p2p_get(&key_of(pl)) == Some(false) && act.len() as i64 >= *RELAY_MATCH_MAX
            && ready.iter().all(|t| has_helper(t))
        {
            return false;
        }
        match cm {
            None => true,
            Some(cm) => {
                let mut v = act.clone();
                v.extend(grp.iter().cloned());
                map_pool(&v, None).contains(&cm)
            }
        }
    }

    // py: daemon.py:5130 _form_pick
    fn form_pick(&mut self, ready_in: &[String], held: &IndexMap<String, Vec<String>>, now: f64, online: &HashSet<String>, lb: Option<&LobbyView>) -> Formed {
        let mut ready: Vec<String> = ready_in.to_vec();
        let (r, grp_mode) = MODEQ.group(&ready, &leader_of);
        ready = r;
        let mut grp_p1 = false;
        if p1_pref_on() && !ready.is_empty() {
            let (r, g) = map_packs::group_p1(&ready, &|t| p1_always(t, PARTIES.leader(t).as_deref()), &|t| p1_owner(t, PARTIES.leader(t).as_deref()));
            ready = r;
            grp_p1 = g;
        }
        ready = region_lock_filter(&ready); // 11-26 REGION PREFERENCE: one match = players that fit each other's lock
        if ready.is_empty() {
            self.log_party_hold(held, now);
            return None;
        }
        let (mut lob, mut lob_keep, mut lob_mid): (Vec<String>, i64, Option<String>) = (Vec::new(), 0, None);
        if *LOBBY_KEEP {
            let rset: HashSet<&String> = ready.iter().collect();
            for (gm, srch, post) in &self.rq_go {
                let have: Vec<String> = ready.iter().filter(|t| srch.contains(*t)).cloned().collect();
                if have.len() as i64 >= *MIN_PLAYERS && srch.iter().all(|t| rset.contains(t)) {
                    lob = have;
                    lob_keep = post.len() as i64;
                    lob_mid = Some(gm.clone());
                    break;
                }
            }
        }
        let quiet = now - lb.map(|l| l.last_join).unwrap_or(self.last_arrival);
        let mut want = if *FILL_WAIT > 0.0 { std::cmp::max(*MIN_PLAYERS, *MATCH_SIZE - float_floordiv(quiet, *FILL_WAIT) as i64) } else { *MIN_PLAYERS };
        if fill_rule() == "fast" {
            want = self.fast_want(&ready, online, quiet, now).0;
        } else if *MAX_WAIT > 0.0 && !ready.is_empty() && now - self.queue.get(&ready[0]).copied().unwrap_or(now) >= *MAX_WAIT {
            want = *MIN_PLAYERS;
        }
        let want0 = want;
        if !lob.is_empty() {
            want = *MIN_PLAYERS;
        }
        let single = lb.map(|l| book_single_unit(&l.members)).unwrap_or(true);
        if *JIP && !self.matches.is_empty() && (ready.len() as i64) < *MATCH_SIZE && lob.is_empty() && single {
            let rs: HashSet<String> = ready.iter().cloned().collect();
            let mut keep = Vec::new();
            for t in &ready {
                let need = 1 + self.jip_mates(t).iter().filter(|m| rs.contains(*m)).count() as i64;
                if self.jip_pick(t, &mut Vec::new(), Some(*JIP_PREFER_LEFT), need, None, None).is_none() {
                    keep.push(t.clone());
                }
            }
            ready = keep;
            if ready.is_empty() {
                return None;
            }
        }
        let held_long = !ready.is_empty()
            && ready.len() as i64 >= *MIN_PLAYERS
            && *QUEUE_HOLD_MAX > 0.0
            && now - ready.iter().map(|t| self.queue.get(t).copied().unwrap_or(now)).fold(f64::INFINITY, f64::min) >= *QUEUE_HOLD_MAX;
        if *SECOND_MATCH_MIN > 0 && (ready.len() as i64) < *SECOND_MATCH_MIN && lob.is_empty() && single {
            let mut grp: Vec<String> = ready[..1].to_vec();
            for t in &ready {
                let mut g = vec![t.clone()];
                g.extend(self.jip_mates(t).into_iter().filter(|m| ready.contains(m)));
                if g.len() > grp.len() {
                    grp = g;
                }
            }
            let mut live: Vec<String> = self.matches.iter().filter(|(mid, pl)| self.second_match_live(mid, pl, &ready, &grp, now)).map(|(m, _)| m.clone()).collect();
            if held_long {
                live.retain(|mid| {
                    !self.started.contains(mid)
                        && self.matches[mid].iter().filter(|t| !self.departed.get(mid).is_some_and(|d| d.contains(*t))).count() + grp.len() <= *MATCH_SIZE as usize
                });
            }
            if !live.is_empty() {
                if throttle(&mut self.qhold_logged, now, 20.0) {
                    dlog!("QUEUE-HOLD", tags(&ready), "wait for match(es)", py::list_str(&live),
                        format!("(a second match needs {} searchers, have {})", *SECOND_MATCH_MIN, ready.len()));
                }
                return None;
            }
        }
        let settling: Vec<String> = if lb.is_none() {
            self.queue.keys().filter(|t| self.settle.get(*t).copied().unwrap_or(0.0) > now && !ready.contains(*t)).cloned().collect()
        } else {
            Vec::new()
        };
        if !settling.is_empty() && (ready.len() as i64) < *MATCH_SIZE && ready.len() as i64 >= want {
            if self.hold_since == 0.0 {
                self.hold_since = now;
            }
            if now - self.hold_since <= *QUEUE_SETTLE + 2.0 {
                return None;
            }
        } else {
            self.hold_since = 0.0;
        }
        if p1_pref_on() && ready.len() as i64 > *MATCH_SIZE {
            let mut pri = ready.clone();
            pri.sort_by_key(|t| if p1_owner(t, PARTIES.leader(t).as_deref()) { 0 } else { 1 });
            if pri != ready {
                dlog!("P1 priority: Part I owners first", ready.iter().filter(|t| p1_owner(t, PARTIES.leader(t).as_deref())).count(), "of", ready.len(), "searchers");
                ready = pri;
            }
        }
        if *MM_REGION_ON && ready.len() as i64 > *MATCH_SIZE {
            let (pri, lead) = hostmig::region_first(
                &ready,
                &|t| mm_region(t),
                Some(&|t: &str| PARTIES.members(t)),
                Some(&|t: &str| now - self.queue.get(t).copied().unwrap_or(now)),
                Some(*MM_REGION_WAIT_S),
            );
            if let Some(lead) = lead {
                if pri != ready {
                    let regs: Vec<String> = ready.iter().map(|t| format!("{}={}", tag(t), mm_region(t).unwrap_or_else(|| "None".into()))).collect();
                    dlog!(format!("REGION priority: {} searchers first", lead), ready.iter().filter(|t| mm_region(t).as_deref() == Some(lead.as_str())).count(), "of",
                        ready.len(), "searchers", format!("({})", regs.join(", ")));
                    ready = pri;
                }
            }
        }
        if !lob.is_empty() {
            let mut r = lob.clone();
            r.extend(ready.iter().filter(|t| !lob.contains(*t)).cloned());
            ready = r;
        }
        if *MM_PING_GATE && *PING_KICK_MS > 0 && ready.len() >= 2 {
            let lim = ping_match_ms();
            let pg = |a: &str, b: &str| _ping_gate(a, b, now);
            let members = |t: &str| PARTIES.members(t);
            let hostok = |t: &str| !no_host(t);
            let (mut kept, mut dropped, mut gh, mut ginfo) = hostmig::ping_fit(&ready, &pg, lim, Some(&members), Some(&hostok));
            ping_left_out_note(&dropped, now);
            let (hold, unk) = ping_wait_judge(&kept, gh.as_deref(), &pg, now, &members, &hostok);
            if hold && kept.len() >= std::cmp::max(2, want as usize) {
                if throttle(&mut self.pwait_logged, now, 20.0) {
                    dlog!("PING-WAIT", "pair pings unknown among", tags(&kept), format!("- holding the formation up to {} s for them", *PING_WAIT_S as i64));
                }
                return None;
            }
            if !unk.is_empty() {
                if throttle(&mut self.pwait_out_logged, now, 20.0) {
                    let k2: Vec<String> = kept.iter().filter(|t| !unk.contains(*t)).cloned().collect();
                    dlog!("PING-GATE", format!("ping to the host still unknown after {} s - left in the queue:", *PING_WAIT_S as i64), tags(&unk), "| kept", tags(&k2));
                }
                let rest: Vec<String> = kept.iter().filter(|t| !unk.contains(*t)).cloned().collect();
                let (k, d2, g, gi) = hostmig::ping_fit(&rest, &pg, lim, Some(&members), Some(&hostok));
                kept = k;
                gh = g;
                ginfo = gi;
                let mut d = dropped.clone();
                d.extend(d2);
                dropped = d;
            }
            if !dropped.is_empty() && throttle(&mut self.pgate_logged, now, 20.0) {
                let s: Vec<String> = dropped.iter().map(|t| format!("{} {} ms", tag(t), oi(gh.as_deref().and_then(|h| _ping_gate(h, t, now))))).collect();
                dlog!("PING-GATE", ginfo, format!("- not matched with them (over {} ms to that host):", lim), s.join(", "), "| kept", tags(&kept));
            }
            if !dropped.is_empty() || !unk.is_empty() {
                if lb.is_some() {
                    let out: Vec<String> = ready.iter().filter(|t| !kept.contains(*t)).cloned().collect();
                    for t in out {
                        self.book.remove(&t, &format!("ping gate at the start (over {} ms to every host, or still unknown)", lim), Some(now), lobbybook::BACKOUT_BLOCK_S);
                    }
                }
                ready = kept;
            }
        }
        if !lob.is_empty() {
            lob.retain(|t| ready.contains(t));
            if (lob.len() as i64) < *MIN_PLAYERS {
                lob = Vec::new();
                lob_keep = 0;
                want = want0;
            } else {
                let mut r = lob.clone();
                r.extend(ready.iter().filter(|t| !lob.contains(*t)).cloned());
                ready = r;
            }
        }
        let size = if !lob.is_empty() { std::cmp::max(*MIN_PLAYERS, *MATCH_SIZE - lob_keep) } else { *MATCH_SIZE };
        let mut players = if ready.len() as i64 >= want { social::pick_units(&ready, &PARTIES, size) } else { Vec::new() };
        if let Some(l) = lb {
            if !players.is_empty() && players.iter().collect::<HashSet<_>>() != ready.iter().collect::<HashSet<_>>() {
                if throttle(&mut self.book_short_logged, now, 20.0) {
                    dlog!("LOBBY-BOOK: lobby", l.lid, "waits: not every member fits one match yet (parties need 2p - 1 players)", tags(&ready));
                }
                return None;
            }
        }
        if !lob.is_empty() && !players.is_empty() && !lob.iter().all(|t| players.contains(t)) {
            dlog!("LOBBY:", lob_mid.clone().unwrap_or_default(), "could not keep every member together (parties / size) - formed as picked:", tags(&players));
        }
        let short = if ready.len() >= 2 { social::party_shortfall(&ready, &PARTIES, *MATCH_SIZE, &players) } else { IndexMap::new() };
        if !short.is_empty() && now - self.pneed_noted >= 30.0 {
            self.pneed_noted = now;
            for (_pid, (u, miss)) in &short {
                for m in PARTIES.members(&u[0]) {
                    SOCIAL.note(&m, "pneed", &miss.to_string());
                }
            }
        }
        if !short.is_empty() && !players.is_empty() && *PARTY_HOLD_S > 0.0 && lob.is_empty() {
            let mn = short.values().flat_map(|(u, _)| u.iter()).map(|t| self.queue.get(t).copied().unwrap_or(now)).fold(f64::INFINITY, f64::min);
            let age = (now - mn).max(0.0);
            if age < *PARTY_HOLD_S {
                if throttle(&mut self.phold_logged, now, 20.0) {
                    let parts: Vec<String> = short.iter().map(|(pid, (u, m))| format!("{}: ({}, {})", py::repr_str(pid), tags(u), py::repr_str(&format!("needs {} more", m)))).collect();
                    dlog!("PARTY-HOLD", tags(&players), "wait for party", format!("{{{}}}", parts.join(", ")),
                        format!("(solos held {} of {} s)", fmt0(age), fmt0(*PARTY_HOLD_S)));
                }
                return None;
            }
        }
        if !players.is_empty() && players.len() as i64 >= want {
            if players.len() > 2 && players.len() % 2 == 1 && std::env::var("T2R_EVEN_ONLY").is_ok_and(|v| !v.is_empty()) {
                players.pop();
            }
            let players = team_order(&players);
            let mid = rnd::token_hex("daemon:_form_pick", 4);
            let mut qt = IndexMap::new();
            for t in &players {
                qt.insert(t.clone(), self.queue.get(t).copied().unwrap_or(0.0));
            }
            self.qtime.insert(mid.clone(), qt);
            for t in &players {
                self.queue.shift_remove(t);
            }
            let gs: HashSet<String> = if *REQUEUE { self.rq.formed(&mid, &players) } else { HashSet::new() };
            for t in &players {
                self.rq.drop(t);
            }
            self.matches.insert(mid.clone(), players.clone());
            self.formed.insert(mid.clone(), clock::now());
            self.hold_since = 0.0;
            if !lob.is_empty() || !gs.is_empty() {
                let mut gsv: Vec<String> = gs.iter().cloned().collect();
                gsv.sort();
                let back: Vec<String> = players.iter().filter(|t| !lob.contains(*t)).cloned().collect();
                dlog!("LOBBY: next match", mid, "of the lobby of match(es)", py::list_str(&gsv), "members", tags(&lob), "| backfill", tags(&back),
                    "| seats kept for", lob_keep, "still on the post-match screens");
            }
            MODEQ.formed(&mid, grp_mode.as_deref());
            if p1_pref_on() {
                let mut m = MATCH_P1.lock();
                m.insert(mid.clone(), grp_p1);
                while m.len() > 256 {
                    m.shift_remove_index(0);
                }
            }
            dlog!("MATCH formed", mid, tags(&players), grp_mode.clone().unwrap_or_default(), if grp_p1 { "(PART I MAPS: ALWAYS)" } else { "" });
            return Some((mid, players));
        }
        if throttle(&mut self.wait_logged, now, 20.0) {
            let fr = fill_rule();
            let stp = if fr == "fast" && *FAST_STEP > 0.0 { *FAST_STEP } else { *FILL_WAIT };
            let nxt = if stp > 0.0 && want > *MIN_PLAYERS { stp - py::fmod(quiet, stp) } else { 0.0 };
            let nq: Vec<String> = self.queue.keys().filter(|t| !ready.contains(*t)).cloned().collect();
            let nxt_s = if nxt != 0.0 { format!("next step-down in {} s", fmt0(nxt)) } else { "at the minimum".to_string() };
            let mut tail: Vec<String> = Vec::new();
            if !short.is_empty() {
                tail.push("| party waits:".to_string());
                for (u, m) in short.values() {
                    tail.push(format!("party of {} {} needs {} players, {} searching ({} more)", u.len(), tags(u), social::party_need(u.len() as i64), ready.len(), m));
                }
            }
            let head = format!("{}", tail.join(" "));
            if tail.is_empty() {
                dlog!("QUEUE-WAIT", "rule", fr, "ready", tags(&ready), "want", want, format!("(min {}, size {})", *MIN_PLAYERS, *MATCH_SIZE),
                    format!("quiet {} s", fmt0(quiet)), nxt_s, "| queued not ready:", tags(&nq));
            } else {
                dlog!("QUEUE-WAIT", "rule", fr, "ready", tags(&ready), "want", want, format!("(min {}, size {})", *MIN_PLAYERS, *MATCH_SIZE),
                    format!("quiet {} s", fmt0(quiet)), nxt_s, "| queued not ready:", tags(&nq), head);
            }
        }
        self.log_party_hold(held, now);
        None
    }
}

/// `"%.0f" % x`
pub fn fmt0(x: f64) -> String {
    py::fixed(x, 0)
}

impl MatchMaker {
    /// seconds a client may stay silent on the matchmaking connection (daemon.py:4868 QUEUE_TTL)
    pub const QUEUE_TTL: f64 = 20.0;

    // py: daemon.py:4912 gc
    pub fn gc(&self, keep: &HashSet<String>, now: Option<f64>) -> i64 {
        let now = now.unwrap_or_else(clock::now);
        let mut n = 0;
        let mut g = self.lock.lock();
        let g = &mut *g;
        let mut live: HashSet<String> = g.queue.keys().cloned().collect();
        for pl in g.matches.values() {
            live.extend(pl.iter().cloned());
        }
        live.extend(keep.iter().cloned());
        macro_rules! prune {
            ($d:expr) => {{
                let dead: Vec<String> = $d.keys().filter(|t| !live.contains(*t)).cloned().collect();
                for t in dead {
                    $d.remove(&t);
                    n += 1;
                }
            }};
        }
        prune!(g.seen);
        prune!(g.settle);
        prune!(g.departed_t);
        prune!(g.late_bonus);
        prune!(g.late_mult);
        prune!(g.late_prog);
        let dead: Vec<String> = g.joining.keys().filter(|t| !live.contains(*t)).cloned().collect();
        for t in dead {
            g.joining.shift_remove(&t);
            n += 1;
        }
        let dead: Vec<String> = g.left.iter().filter(|(t, v)| !live.contains(*t) || now - v.0 > 600.0).map(|(t, _)| t.clone()).collect();
        for t in dead {
            g.left.remove(&t);
            n += 1;
        }
        let dead: Vec<String> = g.departed_vol.iter().filter(|t| !live.contains(*t)).cloned().collect();
        for t in dead {
            g.departed_vol.remove(&t);
            n += 1;
        }
        n
    }

    // py: daemon.py:4936 enqueue
    pub fn enqueue(&self, token: &str) -> Formed {
        PRIV.on_enqueue(token);
        {
            let mut g = self.lock.lock();
            if g.in_match_locked(token) {
                return None;
            }
            let now = clock::now();
            g.seen.insert(token.to_string(), now);
            if !g.queue.contains_key(token) {
                g.settle.insert(token.to_string(), now + *QUEUE_SETTLE);
                let (left, mut since) = g.left.get(token).copied().unwrap_or((0.0, now));
                if now - left > 15.0 {
                    g.last_arrival = now;
                    since = now;
                }
                g.queue.insert(token.to_string(), since);
                evt!("queue", tag(token), format!("queued: {} waiting", g.queue.len()));
            }
        }
        let rq_mid = if *REQUEUE { self.lock.lock().rq.searching(token) } else { None };
        if let Some(m) = rq_mid.filter(|m| !m.is_empty()) {
            requeue_keep_mode(token, &m);
            dlog!("requeue:", tag(token), "searching again after match", m);
        }
        self.try_form()
    }

    // py: daemon.py:4962 try_form
    pub fn try_form(&self) -> Formed {
        let now = clock::now();
        let online: HashSet<String> = PRES_TOKENS().into_iter().collect();
        let mut gd = self.lock.lock();
        let g = &mut *gd;
        let gone: Vec<String> = g.queue.keys().filter(|t| now - g.seen.get(*t).copied().unwrap_or(0.0) >= Self::QUEUE_TTL || !online.contains(*t)).cloned().collect();
        for t in &gone {
            dlog!("QUEUE drop (no matchmaking poll / offline)", tag(t));
            let why = if online.contains(t) { format!("no poll for {} s", fmt0(now - g.seen.get(t).copied().unwrap_or(0.0))) } else { "offline".to_string() };
            evt!("queue-drop", tag(t), why);
            g.queue.shift_remove(t);
            if book_on() {
                g.book.away(t, Some(now));
            }
        }
        let mut ready: Vec<String> = g
            .queue
            .keys()
            .filter(|t| g.settle.get(*t).copied().unwrap_or(0.0) <= now && now - g.seen.get(*t).copied().unwrap_or(0.0) <= *QUEUE_FRESH)
            .cloned()
            .collect();
        ready.sort_by(|a, b| g.queue[a].partial_cmp(&g.queue[b]).unwrap_or(std::cmp::Ordering::Equal));
        let ready = g.rq_filter(ready, now, true);
        let (ready, priv_ready) = PRIV.split_ready(&ready);
        for (lb, have) in PRIV.launches(&priv_ready) {
            return Some(g.form_private(&lb, &have));
        }
        let (ready, held) = social::held_parties(&ready, &PARTIES);
        if book_on() {
            return g.book_form(&ready, &held, now, &online);
        }
        g.form_pick(&ready, &held, now, &online, None)
    }

    // py: daemon.py:5478 mesh_view
    pub fn mesh_view(&self) -> (HashSet<String>, HashMap<String, String>) {
        let g = self.lock.lock();
        let mut q: HashSet<String> = g.queue.keys().cloned().collect();
        q.extend(g.joining.keys().cloned());
        let mut live = HashMap::new();
        for (mid, pl) in &g.matches {
            if g.ended.contains(mid) {
                continue;
            }
            for t in pl {
                if !g.departed.get(mid).is_some_and(|d| d.contains(t)) {
                    live.insert(t.clone(), mid.clone());
                }
            }
        }
        (q, live)
    }

    // py: daemon.py:5484 end
    pub fn end(&self, mid: &str) {
        let mut tk: Option<(Vec<String>, Vec<String>)> = None;
        {
            let mut g = self.lock.lock();
            if g.matches.contains_key(mid) {
                let first = !g.ended.contains(mid);
                g.ended.insert(mid.to_string());
                PRIV.match_over(mid);
                g.ended_at.entry(mid.to_string()).or_insert_with(clock::now);
                if first && *REQUEUE {
                    tk = Some(g.rq_open(mid, clock::now()));
                }
            }
        }
        if let Some((a, b)) = tk {
            dlog!("requeue: match", mid, "over - tickets (search again after the post-match screens, the lobby waits for them):", tags(&a),
                "| no ticket (old ext / party member without its leader):", tags(&b));
        }
    }

    // py: daemon.py:5510 snapshot: [(mid, players, formed, launched, ended, ended_at, departed)]
    pub fn snapshot(&self) -> Vec<(String, Vec<String>, f64, bool, bool, f64, HashSet<String>)> {
        let g = self.lock.lock();
        g.matches
            .iter()
            .map(|(mid, pl)| {
                (
                    mid.clone(),
                    pl.clone(),
                    g.formed.get(mid).copied().unwrap_or(0.0),
                    g.launched.contains(mid),
                    g.ended.contains(mid),
                    g.ended_at.get(mid).copied().unwrap_or(0.0),
                    g.departed.get(mid).cloned().unwrap_or_default(),
                )
            })
            .collect()
    }

    // py: daemon.py:5516 abort
    pub fn abort(&self, mid: &str, only: Option<&HashSet<String>>) -> Option<Vec<String>> {
        let mut g = self.lock.lock();
        let mut players = g.matches.get(mid)?.clone();
        let qt = g.qtime.get(mid).cloned().unwrap_or_default();
        let now = clock::now();
        g.forget_locked(mid);
        PRIV.aborted(mid);
        if let Some(o) = only {
            players.retain(|t| o.contains(t));
        }
        for t in &players {
            g.queue.insert(t.clone(), qt.get(t).copied().unwrap_or(now));
            g.seen.insert(t.clone(), now - *QUEUE_FRESH - 0.001);
            g.settle.insert(t.clone(), now + *QUEUE_SETTLE);
        }
        Some(players)
    }

    // py: daemon.py:5534 in_match
    pub fn in_match(&self, token: &str) -> bool {
        self.lock.lock().in_match_locked(token)
    }

    // py: daemon.py:5537 players_of
    pub fn players_of(&self, token: &str) -> Option<Vec<String>> {
        let g = self.lock.lock();
        g.mid_locked(token).map(|m| g.matches[&m].clone())
    }

    // py: daemon.py:5542 drop
    pub fn drop_mid(&self, mid: &str) {
        let mut g = self.lock.lock();
        if g.matches.contains_key(mid) {
            g.forget_locked(mid);
        }
    }

    // py: daemon.py:5547 loading
    pub fn loading(&self, token: &str, grace: f64) -> bool {
        let g = self.lock.lock();
        if let Some(mid) = g.mid_locked(token) {
            let pl = &g.matches[&mid];
            let loaded = LOADED.lock();
            return clock::now() - g.formed.get(&mid).copied().unwrap_or(0.0) < grace && !pl.iter().all(|p| loaded.contains(p));
        }
        false
    }

    // py: daemon.py:5559 countdown_over
    pub fn countdown_over(&self, token: &str) -> bool {
        let g = self.lock.lock();
        if let Some(mid) = g.mid_locked(token) {
            return clock::now() - g.formed.get(&mid).copied().unwrap_or(0.0) > countdown_seconds() + 2.0;
        }
        false
    }

    // py: daemon.py:5567 match_age
    pub fn match_age(&self, token: &str) -> f64 {
        let g = self.lock.lock();
        if let Some(mid) = g.mid_locked(token) {
            let now = clock::now();
            return now - g.formed.get(&mid).copied().unwrap_or(now);
        }
        0.0
    }

    // py: daemon.py:5574 queued
    pub fn queued(&self) -> Vec<String> {
        let g = self.lock.lock();
        let mut v: Vec<String> = g.queue.keys().cloned().collect();
        v.sort_by(|a, b| g.queue[a].partial_cmp(&g.queue[b]).unwrap_or(std::cmp::Ordering::Equal));
        v.truncate(*MATCH_SIZE as usize);
        v
    }

    // py: daemon.py:5578 players_of_mid
    pub fn players_of_mid(&self, mid: &str) -> Option<Vec<String>> {
        let g = self.lock.lock();
        g.matches.get(mid).filter(|p| !p.is_empty()).cloned()
    }

    // py: daemon.py:5583 active_of
    pub fn active_of(&self, token: &str) -> (Option<String>, Vec<String>) {
        let g = self.lock.lock();
        for (mid, pl) in &g.matches {
            let dep = g.departed.get(mid);
            if pl.iter().any(|x| x == token) && !dep.is_some_and(|d| d.contains(token)) && !g.ended.contains(mid) {
                return (Some(mid.clone()), pl.iter().filter(|p| !dep.is_some_and(|d| d.contains(*p))).cloned().collect());
            }
        }
        (None, Vec::new())
    }

    // py: daemon.py:5617 mid_of
    pub fn mid_of(&self, token: &str) -> Option<String> {
        self.lock.lock().mid_locked(token)
    }

    // py: daemon.py:5621 all_loaded
    pub fn all_loaded(&self, token: &str) -> bool {
        let pl = {
            let g = self.lock.lock();
            g.mid_locked(token).map(|m| g.matches[&m].clone())
        };
        match pl {
            Some(pl) if !pl.is_empty() => {
                let loaded = LOADED.lock();
                pl.iter().all(|p| loaded.contains(p))
            }
            _ => false,
        }
    }

    // py: daemon.py:5627 mark_launched
    pub fn mark_launched(&self, players: &[String]) {
        let mut g = self.lock.lock();
        let hit: Vec<String> = g.matches.iter().filter(|(_, pl)| pl.as_slice() == players).map(|(m, _)| m.clone()).collect();
        for m in hit {
            g.launched.insert(m);
        }
    }

    // py: daemon.py:5632 cancel
    pub fn cancel(&self, token: &str) -> Option<Vec<String>> {
        let mut g = self.lock.lock();
        let mid = g.mid_locked(token)?;
        if g.launched.contains(&mid) {
            return None;
        }
        let players = g.matches.shift_remove(&mid)?;
        let qt = g.qtime.remove(&mid).unwrap_or_default();
        g.formed.remove(&mid);
        let now = clock::now();
        let others: Vec<String> = players.iter().filter(|t| t.as_str() != token).cloned().collect();
        for t in &others {
            g.queue.insert(t.clone(), qt.get(t).copied().unwrap_or(now));
            g.seen.insert(t.clone(), now - *QUEUE_FRESH - 0.001);
            g.settle.insert(t.clone(), now + *QUEUE_SETTLE);
        }
        g.left.insert(token.to_string(), (now, qt.get(token).copied().unwrap_or(now)));
        g.last_arrival = now;
        if book_on() {
            g.book.cancelled(&mid, token, Some(now));
        }
        Some(others)
    }

    // py: daemon.py:5657 poll
    pub fn poll(&self, token: &str) {
        self.lock.lock().seen.insert(token.to_string(), clock::now());
    }

    // py: daemon.py:5660 dequeue
    pub fn dequeue(&self, token: &str) -> bool {
        let mut g = self.lock.lock();
        let since = g.queue.shift_remove(token);
        if let Some(s) = since {
            g.left.insert(token.to_string(), (clock::now(), s));
            g.settle.remove(token);
            if book_on() {
                g.book.away(token, None);
            }
        }
        if g.rq.drop_search(token) {
            dlog!("requeue:", tag(token), "ticket dropped (left the search)");
        }
        since.is_some()
    }

    // py: daemon.py:5673 leave
    pub fn leave(&self, token: &str) {
        let mut g = self.lock.lock();
        g.rq.drop(token);
        if book_on() {
            g.book.remove(token, "left", None, 0.0);
        }
        let since = g.queue.shift_remove(token);
        if let Some(s) = since {
            g.left.insert(token.to_string(), (clock::now(), s));
        }
        let mids: Vec<String> = g.matches.iter().filter(|(_, pl)| pl.iter().any(|x| x == token)).map(|(m, _)| m.clone()).collect();
        for m in mids {
            g.forget_locked(&m);
        }
    }

    // py: daemon.py:5686 depart
    pub fn depart(&self, token: &str, voluntary: bool) -> Option<String> {
        let mut g = self.lock.lock();
        let mid = g
            .matches
            .iter()
            .find(|(m, pl)| pl.iter().any(|x| x == token) && !g.departed.get(*m).is_some_and(|d| d.contains(token)))
            .map(|(m, _)| m.clone())?;
        g.departed.entry(mid.clone()).or_default().insert(token.to_string());
        g.departed_t.insert(token.to_string(), clock::now());
        if voluntary {
            g.departed_vol.insert(token.to_string());
        } else {
            g.departed_vol.remove(token);
        }
        g.joining.shift_remove(token);
        let gone = g.departed.get(&mid).cloned().unwrap_or_default();
        if g.matches[&mid].iter().all(|t| gone.contains(t)) {
            g.forget_locked(&mid);
        }
        Some(mid)
    }

    // py: daemon.py:5848 jip_lives
    pub fn jip_lives(&self, token: &str, myteam: i64, ally: i64, enemy: i64) {
        if !(myteam == 0 || myteam == 1) || ally > 250 || enemy > 250 {
            return;
        }
        let mut g = self.lock.lock();
        let Some(mid) = g.mid_locked(token) else { return };
        let mut l = [0i64; 2];
        l[myteam as usize] = ally;
        l[(1 - myteam) as usize] = enemy;
        g.lives.insert(mid, (l[0], l[1], clock::now()));
    }

    // py: daemon.py:5862 late_mult_take
    pub fn late_mult_take(&self, token: &str, mid: &str) -> f64 {
        let mut g = self.lock.lock();
        if let Some(b) = g.late_mult.get(token).cloned() {
            if b.0 == mid {
                g.late_mult.remove(token);
                return b.1;
            }
        }
        1.0
    }

    // py: daemon.py:5871 late_bonus_take
    pub fn late_bonus_take(&self, token: &str, mid: &str) -> i64 {
        let mut g = self.lock.lock();
        if let Some(b) = g.late_bonus.get(token).cloned() {
            if b.0 == mid {
                g.late_bonus.remove(token);
                return b.1;
            }
        }
        0
    }
}
