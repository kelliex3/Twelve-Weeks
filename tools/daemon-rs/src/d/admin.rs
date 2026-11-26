//! K6 admin: ping gate / ping_check, packet loss limit, kick / AFK removal, admin control listener (HTTP/1.0 on loopback), `serve` start order.
//! daemon.py:10416-10864 (ping and loss), 10905-11068 (admin server, main); admin_secret / reset_session / kick at 10260-10415.
use super::*;
use crate::{clock, py};
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet, VecDeque};
use std::io::{BufRead, Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::LazyLock;

fn f1(x: f64) -> String {
    py::fixed(x, 1)
}
fn f0(x: f64) -> String {
    py::fixed(x, 0)
}

// ---------------------------------------------------------------- ping gate
// py: daemon.py:10416 PING_GATE_AGE_S (the gates fall back to the last measured ping up to this old)
pub static PING_GATE_AGE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PING_GATE_AGE_S", 3600.0));

/// `pair_ping(a, b, raw=True)` (the scripted world answers it too while the selftests run)
fn pair_ping_raw(a: &str, b: &str) -> Option<i64> {
    if WORLD_ON.load(std::sync::atomic::Ordering::Relaxed) {
        return pair_ping(a, b);
    }
    pair_ping_ex(a, b, true, None)
}

// py: daemon.py:10411 _ping_fresh
pub fn ping_fresh(a: &str, b: &str, now: f64) -> Option<i64> {
    let at = {
        let g = PING_AT.lock();
        g.get(&(a.to_string(), b.to_string())).copied().unwrap_or(0.0).max(g.get(&(b.to_string(), a.to_string())).copied().unwrap_or(0.0))
    };
    if now - at <= PING_FRESH_S {
        pair_ping_raw(a, b)
    } else {
        None
    }
}

// py: daemon.py:10418 ping_wait_judge (the optional arguments as Options; returns (hold, leave_out))
pub fn ping_wait_judge_ex(
    players: &[String],
    host: Option<&str>,
    pair: &dyn Fn(&str, &str) -> Option<i64>,
    now: f64,
    units_fn: Option<&dyn Fn(&str) -> Vec<String>>,
    helper: Option<&dyn Fn(&str) -> bool>,
    wait_s: Option<f64>,
    host_ok: Option<&dyn Fn(&str) -> bool>,
) -> (bool, HashSet<String>) {
    let wait_s = wait_s.unwrap_or(*PING_WAIT_S);
    let mut inv: Vec<String> = Vec::new();
    if wait_s > 0.0 && players.len() >= 2 {
        let hp: Vec<&String> = players.iter().filter(|t| helper.map(|h| h(t)).unwrap_or_else(|| has_helper(t))).collect();
        for (i, a) in hp.iter().enumerate() {
            for b in &hp[i + 1..] {
                if pair(a, b).is_none() {
                    for x in [a, b] {
                        if !inv.contains(*x) {
                            inv.push((*x).clone());
                        }
                    }
                }
            }
        }
    }
    {
        let mut ws = PING_WAIT_SINCE.lock();
        for t in players {
            if !inv.contains(t) {
                ws.remove(t);
            }
        }
        if inv.is_empty() {
            return (false, HashSet::new());
        }
        for t in &inv {
            ws.entry(t.clone()).or_insert(now);
        }
        if ws.len() > 500 {
            ws.retain(|_, v| !(now - *v > 3600.0));
        }
        // (11-24) the hold ends once the longest-waiting unknown player has waited wait_s
        let longest = inv.iter().map(|t| now - ws[t]).fold(f64::NEG_INFINITY, f64::max);
        if longest < wait_s {
            return (true, HashSet::new());
        }
    }
    let left_out = |h: Option<&str>| -> HashSet<String> {
        let mut o: HashSet<String> = inv.iter().filter(|t| Some(t.as_str()) != h && (h.is_none() || pair(h.unwrap_or(""), t).is_none())).cloned().collect();
        for t in o.clone() {
            if let Some(uf) = units_fn {
                for m in uf(&t) {
                    if players.contains(&m) && Some(m.as_str()) != h {
                        o.insert(m);
                    }
                }
            }
        }
        o
    };
    let mut best: Option<HashSet<String>> = None;
    let cands: Vec<Option<&str>> = match host {
        Some(h) => std::iter::once(Some(h)).chain(players.iter().map(|p| Some(p.as_str()))).collect(),
        None => players.iter().map(|p| Some(p.as_str())).collect(),
    };
    for h in cands {
        let Some(hh) = h else { continue };
        if host_ok.is_some_and(|f| !f(hh)) {
            continue;
        }
        let o = left_out(h);
        if best.as_ref().is_none_or(|b| o.len() < b.len()) {
            best = Some(o);
        }
    }
    (false, best.unwrap_or_else(|| left_out(host)))
}

/// the form the K4 callers use (units_fn / host_ok always given; the left-out set sorted)
pub fn ping_wait_judge(
    kept: &[String],
    host: Option<&str>,
    pair: &dyn Fn(&str, &str) -> Option<i64>,
    now: f64,
    members_fn: &dyn Fn(&str) -> Vec<String>,
    host_ok: &dyn Fn(&str) -> bool,
) -> (bool, Vec<String>) {
    if world_only() {
        return (false, Vec::new()); // (selftest k4: the scripted world has no ping wait)
    }
    let (hold, out) = ping_wait_judge_ex(kept, host, pair, now, Some(members_fn), None, None, Some(host_ok));
    let mut v: Vec<String> = out.into_iter().collect();
    v.sort();
    (hold, v)
}

/// py: daemon.py:10464 PING_OUT: token -> [first, last] pass the ping gate left it out for being over the limit to the lobby's host
pub static PING_OUT: LazyLock<Mutex<HashMap<String, (f64, f64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:10466 ping_left_out_note (returns the tokens noted)
pub fn ping_left_out_note_with(dropped: &[String], now: f64, note: Option<&dyn Fn(&str, i64)>) -> Vec<String> {
    let mut cur: Vec<String> = Vec::new();
    for t in dropped {
        if !cur.contains(t) {
            cur.push(t.clone());
        }
    }
    {
        let mut po = PING_OUT.lock();
        po.retain(|t, _| cur.contains(t));
    }
    let mut sent = Vec::new();
    for t in &cur {
        let rec = {
            let mut po = PING_OUT.lock();
            let mut rec = po.get(t).copied();
            if rec.is_none_or(|r| now - r.1 > 30.0) {
                rec = Some((now, now));
            }
            let mut r = rec.unwrap_or((now, now));
            r.1 = now;
            po.insert(t.clone(), r);
            r
        };
        let at = PING_NOTE_AT.lock().get(t).copied().unwrap_or(-1e9);
        if now - rec.0 >= PING_NOTE_AFTER_S && now - at >= PING_NOTE_EVERY_S {
            PING_NOTE_AT.lock().insert(t.clone(), now);
            match note {
                Some(f) => f(t, *PING_KICK_MS),
                None => SOCIAL.note(t, "pingfar", &PING_KICK_MS.to_string()),
            }
            sent.push(t.clone());
            dlog!("PING-GATE", tag(t), format!("left out for {} s: search note sent (no players within {} ms)", (now - rec.0) as i64, *PING_KICK_MS));
        }
    }
    let mut pn = PING_NOTE_AT.lock();
    if pn.len() > 500 {
        pn.retain(|_, v| !(now - *v > 3600.0));
    }
    sent
}

pub fn ping_left_out_note(dropped: &[String], now: f64) {
    if world_only() {
        return;
    }
    ping_left_out_note_with(dropped, now, None);
}

// py: daemon.py:10494 _ping_gate (the ping the gates judge)
pub fn _ping_gate(a: &str, b: &str, now: f64) -> Option<i64> {
    if world_only() || STANDIN.load(std::sync::atomic::Ordering::Relaxed) {
        return pair_ping(a, b); // (selftest k4 / k3: the scripted world)
    }
    let mut ms = ping_fresh(a, b, now);
    if ms.is_none() {
        let at = {
            let g = PING_AT.lock();
            g.get(&(a.to_string(), b.to_string())).copied().unwrap_or(0.0).max(g.get(&(b.to_string(), a.to_string())).copied().unwrap_or(0.0))
        };
        ms = if at != 0.0 && now - at <= *PING_GATE_AGE_S { pair_ping_raw(a, b) } else { None };
    }
    if ms.is_none() {
        // not in PING (evicted / never kept): the last reported pair ping (R-MST5)
        if let Some((Some(p), t)) = ping_last_get(a, b) {
            if now - t <= *PING_GATE_AGE_S {
                ms = Some(p);
            }
        }
    }
    if MEASURED_PAIR.lock().is_empty() && PING_CORR.lock().is_empty() {
        return ms;
    }
    if let Some(m) = ms {
        // the per-player correction on the estimate
        ms = Some(m + corr_of(a, now).max(corr_of(b, now)));
    }
    if *PING_MEMORY_S > 0.0 {
        // what the last removal of this pair measured
        if let Some(m) = measured_of(a, b, now) {
            if ms.is_none_or(|x| m > x) {
                ms = Some(m);
            }
        }
    }
    ms
}

// py: daemon.py:10516 jip_ping_block (called with MM.lock held by the callers: reads only)
pub fn jip_ping_block(token: &str, mid: &str, host: &str, active: &[String], joining: &HashSet<String>, now: f64) -> Option<i64> {
    if world_only() {
        return None; // (selftest k4)
    }
    if *PING_KICK_MS <= 0 || PRIV.rule(mid, "ping") == Some(0) || token == host {
        return None;
    }
    let ms = _ping_gate(token, host, now)?;
    if ms <= (if *MM_PING_GATE { ping_match_ms() } else { *PING_KICK_MS }) {
        return None;
    }
    if *MM_PING_GATE {
        // 11-22 match 72784a06: a joiner over the limit never joins
        return Some(ms);
    }
    let mut known = vec![ms];
    for p in active {
        if p != host && p != token && !joining.contains(p) {
            if let Some(v) = ping_fresh(p, host, now) {
                known.push(v);
            }
        }
    }
    let high = known.iter().filter(|v| **v > *PING_KICK_MS).count();
    if known.len() >= 2 && high * 2 > known.len() {
        return None;
    }
    Some(ms)
}

// py: daemon.py:10537 ping_check (every status tick: the ping limit; returns the tokens removed)
pub fn ping_check(now: Option<f64>) -> Vec<String> {
    let now = now.unwrap_or_else(clock::now);
    let mut out: Vec<String> = Vec::new();
    if *PING_KICK_MS <= 0 {
        return out;
    }
    let mut live: HashSet<(String, String)> = HashSet::new();
    for (mid, players, formed, launched, ended, _, dep) in MM.snapshot() {
        if !launched || ended || now - formed < PING_GRACE_S {
            continue;
        }
        let act: Vec<String> = players.iter().filter(|p| !dep.contains(*p)).cloned().collect();
        if act.len() < 2 || PRIV.rule(&mid, "ping") == Some(0) {
            continue; // (11-17: a private match with PING LIMIT off)
        }
        let host: Option<String> = if HM.enabled && HM.has(&act[0]) { HM.host_token(&act[0]) } else { players.first().cloned() };
        let Some(host) = host.filter(|h| act.contains(h)) else { continue };
        // a player still on the loading screen has not received its GAME-START: its ping is not judged
        let (m_started, joining): (bool, HashSet<String>) = {
            let g = MM.lock.lock();
            (g.started.contains(&mid), g.joining.keys().cloned().collect())
        };
        let waiting: HashSet<String> = if !m_started { act.iter().cloned().collect() } else { act.iter().filter(|p| joining.contains(*p)).cloned().collect() };
        {
            let mut ph = PING_HIGH.lock();
            for p in &waiting {
                ph.remove(&(p.clone(), mid.clone()));
            }
        }
        let pings: Vec<(String, Option<i64>)> = act.iter().filter(|p| **p != host && !waiting.contains(*p)).map(|p| (p.clone(), ping_fresh(p, &host, now))).collect();
        let pget = |p: &str| -> Option<i64> { pings.iter().find(|(q, _)| q == p).and_then(|(_, v)| *v) };
        let high: Vec<&String> = pings.iter().filter(|(_, ms)| ms.is_some_and(|v| v > *PING_KICK_MS)).map(|(p, _)| p).collect();
        let known = pings.iter().filter(|(_, ms)| ms.is_some()).count();
        if known >= 2 && high.len() * 2 > known {
            // most of them are high to the host: its link is the problem, nobody is removed
            let mut ph = PING_HIGH.lock();
            for p in &act {
                ph.remove(&(p.clone(), mid.clone()));
            }
            continue;
        }
        for p in &act {
            if *p == host || waiting.contains(p) {
                continue;
            }
            let key = (p.clone(), mid.clone());
            live.insert(key.clone());
            if pget(p).is_none() && *MM_PING_GATE {
                // 11-22 match 06930992: unknown keeps the timer as it is; the removal still needs a fresh reading over the limit
                continue;
            }
            if !high.contains(&p) {
                if PING_HIGH.lock().remove(&key).is_some() {
                    dlog!("ping:", tag(p), format!("back under {} ms to the host in match {} ({} ms)", *PING_KICK_MS, mid, oi(pget(p))));
                }
                continue;
            }
            let t0 = *PING_HIGH.lock().entry(key.clone()).or_insert(now);
            let ms_now = pget(p).unwrap_or(0);
            if t0 == now {
                dlog!(
                    "ping:",
                    tag(p),
                    format!("{} ms to the host {} in match {} - over the {} ms limit, removed after {} s of it", ms_now, tag(&host), mid, *PING_KICK_MS, *PING_KICK_S as i64)
                );
            }
            if now - t0 < *PING_KICK_S || PING_KICKED.lock().contains(&key) {
                continue;
            }
            {
                let mut pk = PING_KICKED.lock();
                pk.insert(key.clone());
                if pk.len() > 2000 {
                    pk.clear();
                }
            }
            let (ms, secs) = (ms_now, (now - t0) as i64);
            ping_kick_learn(&mid, p, &host, ms, Some(now));
            dlog!("ping:", tag(p), format!("{} ms to the host for {} s in match {} - removed from the match (not counted as leaving early)", ms, secs, mid));
            let did = kick_token(
                p,
                &format!("removed: high ping ({} ms to the host for {} s)", ms, secs),
                &srv_text("ping_kicked", &lang_of(p), &[("ms", ms.to_string()), ("s", secs.to_string()), ("lim", PING_KICK_MS.to_string())]),
                "ping-kick",
                &format!("{} ms to the host for {} s (limit {} ms)", ms, secs, *PING_KICK_MS),
                true,
                true,
            );
            dlog!("ping:", tag(p), "removal done:", if did.is_empty() { "nothing".to_string() } else { did.join(", ") });
            out.push(p.clone());
        }
    }
    PING_HIGH.lock().retain(|k, _| live.contains(k)); // finished matches / departed players
    out
}

// ---------------------------------------------------------------- packet loss
/// py: parse_loss result: the reporter's flags + per-peer (slot, name hash, received, lost)
#[derive(Clone, Debug, PartialEq)]
pub struct LossRep {
    pub paced: bool,
    pub hbw: bool,
    pub peers: Vec<(u8, u32, i64, i64)>,
}

// py: daemon.py:10605 parse_loss (trailer 3 of an op 0xCA payload)
pub fn parse_loss(pl: &[u8]) -> Option<LossRep> {
    if pl.len() < 72 + 7 || pl[71] & 4 == 0 || pl[72] != 1 {
        return None;
    }
    let off = 79 + if pl[71] & 2 != 0 { 7 } else { 0 };
    if pl.len() < off + 3 || pl[off] != 1 {
        return None;
    }
    let (fl, n) = (pl[off + 1], pl[off + 2] as usize);
    if n > 32 || pl.len() < off + 3 + n * 9 {
        return None;
    }
    let mut out = Vec::new();
    for i in 0..n {
        let e = &pl[off + 3 + i * 9..off + 3 + (i + 1) * 9];
        out.push((e[0], u32::from_le_bytes([e[1], e[2], e[3], e[4]]), u16::from_le_bytes([e[5], e[6]]) as i64, u16::from_le_bytes([e[7], e[8]]) as i64));
    }
    Some(LossRep { paced: fl & 1 != 0, hbw: fl & 2 != 0, peers: out })
}

// py: daemon.py:10620 _roster_key (FNV-1a 32 of a name as the ext reads it back: the roster's latin1 bytes, the first 24, up to a NUL)
pub fn roster_key(nm: &str) -> u32 {
    let b: Vec<u8> = nm.chars().filter(|c| (*c as u32) < 256).map(|c| c as u32 as u8).collect();
    let b = &b[..b.len().min(24)];
    let b = b.split(|c| *c == 0).next().unwrap_or(&[]);
    let mut h: u32 = 2166136261;
    for c in b {
        h = (h ^ *c as u32).wrapping_mul(16777619);
    }
    h
}

// py: daemon.py:10629 loss_note_from_pl (the stub name of the other chunks): loss_note(token, parse_loss(pl)); never raises
pub fn loss_note_from_pl(token: &str, pl: &[u8]) {
    loss_note(token, parse_loss(pl).as_ref(), None);
}

// py: daemon.py:10629 loss_note (book one reporter's per-peer loss in its live match)
pub fn loss_note(token: &str, rep: Option<&LossRep>, now: Option<f64>) {
    let Some(rep) = rep else { return };
    if *LOSS_KICK_PCT <= 0.0 {
        return;
    }
    let now = now.unwrap_or_else(clock::now);
    let (mid, act) = MM.active_of(token);
    let Some(mid) = mid else { return };
    LOSS_FLAGS.lock().insert((mid.clone(), token.to_string()), (rep.paced, rep.hbw));
    let mut by: HashMap<u32, Vec<String>> = HashMap::new();
    for p in &act {
        if p != token {
            by.entry(roster_key(&get_name(p))).or_default().push(p.clone());
        }
    }
    let mut loss = LOSS.lock();
    for (_slot, h, rx, lost) in &rep.peers {
        let Some(subj) = by.get(h) else { continue };
        if subj.len() != 1 || rx + lost <= 0 {
            continue;
        }
        let d = loss.entry((mid.clone(), token.to_string(), subj[0].clone())).or_default();
        d.push_back((now, *rx, *lost));
        if d.len() > 4 {
            d.pop_front();
        }
    }
    if loss.len() > 4000 {
        // bounded: the keys with the oldest newest entry out (finished matches are swept by loss_check; K1 keeps LOSS unordered)
        let mut ks: Vec<((String, String, String), f64)> = loss.iter().map(|(k, d)| (k.clone(), d.back().map(|x| x.0).unwrap_or(0.0))).collect();
        ks.sort_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal));
        for (k, _) in ks.into_iter().take(1000) {
            loss.remove(&k);
        }
    }
}

// py: daemon.py:10651 loss_of ({observer: loss %} of one player in one match over the last LOSS_WINDOW_S)
pub fn loss_of(mid: &str, subj: &str, now: f64) -> Vec<(String, f64)> {
    let g = LOSS.lock();
    let mut out: Vec<(String, f64)> = Vec::new();
    for ((m, obs, s), d) in g.iter() {
        if m != mid || s != subj {
            continue;
        }
        let rx: i64 = d.iter().filter(|(t, _, _)| now - t <= LOSS_WINDOW_S).map(|(_, r, _)| *r).sum();
        let lo: i64 = d.iter().filter(|(t, _, _)| now - t <= LOSS_WINDOW_S).map(|(_, _, l)| *l).sum();
        if rx + lo >= LOSS_MIN_PKTS {
            out.push((obs.clone(), 100.0 * lo as f64 / (rx + lo) as f64));
        }
    }
    out
}

fn median(v: &[f64]) -> f64 {
    let mut v = v.to_vec();
    v.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    if v.len() % 2 == 1 {
        v[v.len() / 2]
    } else {
        (v[v.len() / 2 - 1] + v[v.len() / 2]) / 2.0
    }
}

// py: daemon.py:10663 loss_check (every status tick: the packet loss limit; returns the tokens removed)
pub fn loss_check(now: Option<f64>) -> Vec<String> {
    let now = now.unwrap_or_else(clock::now);
    let mut out: Vec<String> = Vec::new();
    if *LOSS_KICK_PCT <= 0.0 {
        return out;
    }
    let mut live: HashSet<(String, String)> = HashSet::new();
    let mut mids: HashSet<String> = HashSet::new();
    for (mid, players, formed, launched, ended, _, dep) in MM.snapshot() {
        if !launched || ended {
            continue;
        }
        mids.insert(mid.clone());
        if now - formed < PING_GRACE_S {
            continue;
        }
        let act: Vec<String> = players.iter().filter(|p| !dep.contains(*p)).cloned().collect();
        if (act.len() as i64) < LOSS_MIN_OBS + 1 || PRIV.rule(&mid, "ping") == Some(0) {
            continue; // a private match with PING LIMIT off has no loss limit either
        }
        let (m_started, joining): (bool, HashSet<String>) = {
            let g = MM.lock.lock();
            (g.started.contains(&mid), g.joining.keys().cloned().collect())
        };
        if !m_started {
            continue;
        }
        let flags = |p: &String| LOSS_FLAGS.lock().get(&(mid.clone(), p.clone())).copied().unwrap_or((false, false));
        if act.iter().any(|p| flags(p).1) {
            continue; // a host upload budget drops relayed msg 7 on purpose: seq gaps are not loss
        }
        let mut med: Vec<(String, (f64, usize))> = Vec::new();
        for p in &act {
            if joining.contains(p) || flags(p).0 {
                continue; // its own sends to far peers are paced: gaps by design
            }
            let obs: Vec<f64> = loss_of(&mid, p, now).into_iter().filter(|(o, _)| act.contains(o) && o != p).map(|(_, v)| v).collect();
            if obs.len() as i64 >= LOSS_MIN_OBS {
                med.push((p.clone(), (median(&obs), obs.len())));
            }
        }
        let mget = |p: &str| med.iter().find(|(q, _)| q == p).map(|(_, v)| *v);
        let high: Vec<&String> = med.iter().filter(|(_, (v, _))| *v >= *LOSS_KICK_PCT).map(|(p, _)| p).collect();
        let near = med.iter().filter(|(_, (v, _))| *v >= *LOSS_KICK_PCT / 2.0).count();
        match loss_host_handover(&mid, &act, &med, now) {
            Ok(_) => {}
            Err(e) => dlog!("loss: host handover ERR", e), // never breaks the loss check
        }
        if med.len() >= 2 && near * 2 >= med.len() {
            // half or more are lossy: the host / relay link is the problem, nobody is removed
            let mut lh = LOSS_HIGH.lock();
            for p in &act {
                lh.remove(&(p.clone(), mid.clone()));
            }
            continue;
        }
        for p in &act {
            let key = (p.clone(), mid.clone());
            live.insert(key.clone());
            let Some((pct, nobs)) = mget(p) else { continue }; // unknown keeps the timer as it is (as the ping limit)
            let rest: Vec<f64> = med.iter().filter(|(q, _)| q != p).map(|(_, (v, _))| *v).collect();
            let base = if rest.is_empty() { 0.0 } else { median(&rest) };
            if high.contains(&p) && base >= *LOSS_BASE_PCT {
                // the others lose packets too: the match's route, not this player alone
                if LOSS_HIGH.lock().remove(&key).is_some() {
                    dlog!("loss:", tag(p), format!("{}% but the rest of match {} loses {}% too (limit {}%): not judged", f1(pct), mid, f1(base), f0(*LOSS_BASE_PCT)));
                }
                continue;
            }
            if !high.contains(&p) {
                if LOSS_HIGH.lock().remove(&key).is_some() {
                    dlog!("loss:", tag(p), format!("back under {}% in match {} ({}%, {} observers)", f0(*LOSS_KICK_PCT), mid, f1(pct), nobs));
                }
                continue;
            }
            let t0 = *LOSS_HIGH.lock().entry(key.clone()).or_insert(now);
            if t0 == now {
                dlog!(
                    "loss:",
                    tag(p),
                    format!(
                        "median packet loss {}% seen by {} players in match {} - at or over the {}% limit, removed after {} s of it{}",
                        f1(pct),
                        nobs,
                        mid,
                        f0(*LOSS_KICK_PCT),
                        *LOSS_KICK_S as i64,
                        if *LOSS_GATE >= 2 { "" } else { " (LOG ONLY)" }
                    )
                );
            }
            if now - t0 < *LOSS_KICK_S || LOSS_KICKED.lock().contains(&key) {
                continue;
            }
            {
                let mut lk = LOSS_KICKED.lock();
                lk.insert(key.clone());
                if lk.len() > 2000 {
                    lk.clear();
                }
            }
            let (secs, ipct) = ((now - t0) as i64, py::round0(pct));
            if *LOSS_GATE < 2 {
                dlog!("loss:", tag(p), format!("{}% loss for {} s in match {} - would be removed (T2R_LOSS_GATE={}: log only)", f1(pct), secs, mid, *LOSS_GATE));
                continue;
            }
            let sid = BRIDGES.steamid(p);
            let sk = if sid != 0 { sid.to_string() } else { p.clone() };
            let st: Vec<f64> = {
                let mut ls = LOSS_STRIKES.lock();
                let mut st: Vec<f64> = ls.get(&sk).map(|v| v.iter().filter(|t| now - **t < *LOSS_REPEAT_S).copied().collect()).unwrap_or_default();
                st.push(now);
                ls.insert(sk.clone(), st.clone());
                if ls.len() > 5000 {
                    ls.clear();
                }
                st
            };
            let hold: f64 = if st.len() >= 2 && *LOSS_HOLD_S > 0.0 { *LOSS_HOLD_S } else { 0.0 };
            if hold != 0.0 {
                let mut lh = LOSS_HOLD.lock();
                lh.insert(sk.clone(), (now, ipct as f64));
                if lh.len() > 5000 {
                    lh.clear();
                }
            }
            dlog!(
                "loss:",
                tag(p),
                format!(
                    "{}% packet loss ({} observers, rest of the match {}%) for {} s in match {} - removed from the match (not counted as leaving early), {}",
                    f1(pct),
                    nobs,
                    f1(base),
                    secs,
                    mid,
                    if hold != 0.0 {
                        format!("searches held {} s (removal {} in {} h)", hold as i64, st.len(), f0(*LOSS_REPEAT_S / 3600.0))
                    } else {
                        "may search again at once (first removal)".to_string()
                    }
                )
            );
            let lim = (*LOSS_KICK_PCT as i64).to_string();
            let ktxt = if hold != 0.0 {
                srv_text(
                    "loss_kicked",
                    &lang_of(p),
                    &[("pct", ipct.to_string()), ("s", secs.to_string()), ("lim", lim.clone()), ("m", std::cmp::max(1, (hold / 60.0).floor() as i64).to_string())],
                )
            } else {
                srv_text("loss_kicked_free", &lang_of(p), &[("pct", ipct.to_string()), ("s", secs.to_string()), ("lim", lim.clone())])
            };
            let did = kick_token(
                p,
                &format!("removed: packet loss ({}% for {} s)", ipct, secs),
                &ktxt,
                "loss-kick",
                &format!("{}% packet loss for {} s (limit {}%)", ipct, secs, lim),
                true,
                true,
            );
            dlog!("loss:", tag(p), "removal done:", if did.is_empty() { "nothing".to_string() } else { did.join(", ") });
            out.push(p.clone());
        }
    }
    LOSS_HIGH.lock().retain(|k, _| live.contains(k));
    LOSS_HOST_HIGH.lock().retain(|k, _| mids.contains(k));
    LOSS.lock().retain(|k, _| mids.contains(&k.0)); // finished matches
    LOSS_FLAGS.lock().retain(|k, _| mids.contains(&k.0));
    out
}

// py: daemon.py:10743 loss_host_handover (the host's own packet loss; returns the new host slot or None)
pub fn loss_host_handover(mid: &str, act: &[String], med: &[(String, (f64, usize))], now: f64) -> Result<Option<usize>, String> {
    if *LOSS_HOST_S <= 0.0 || !HM.enabled || REPICK.mode < 2 {
        return Ok(None);
    }
    let Some(snap) = HM.snapshot(mid) else { return Ok(None) };
    let (players, alive, hslot) = (snap.players, snap.alive, snap.host);
    let host: Option<String> = players.get(hslot).cloned();
    let mget = |p: &str| med.iter().find(|(q, _)| q == p).map(|(_, v)| *v);
    let hv = host.as_deref().and_then(mget);
    let (Some(host), Some(hv)) = (host.clone(), hv) else {
        LOSS_HOST_HIGH.lock().remove(mid);
        return Ok(None);
    };
    if hv.0 < *LOSS_KICK_PCT || med.iter().any(|(q, (v, _))| *q != host && *v > hv.0) {
        LOSS_HOST_HIGH.lock().remove(mid);
        return Ok(None);
    }
    let rec = {
        let mut lh = LOSS_HOST_HIGH.lock();
        match lh.get(mid) {
            Some(r) if r.0 == host => r.clone(),
            _ => {
                let r = (host.clone(), now, false);
                lh.insert(mid.to_string(), r.clone());
                drop(lh);
                dlog!(
                    "loss:",
                    tag(&host),
                    format!(
                        "is the host of match {} and loses {}% of its packets ({} observers) - host handed over after {} s of it",
                        mid,
                        f1(hv.0),
                        hv.1,
                        *LOSS_HOST_S as i64
                    )
                );
                r
            }
        }
    };
    if now - rec.1 < *LOSS_HOST_S || HM.handover_pending(mid) {
        return Ok(None);
    }
    let (ok, _why) = HM.handover_ok(mid);
    if !ok {
        return Ok(None);
    }
    let (stale, _loading) = HM.stale_slots(mid);
    let others: Vec<usize> = alive.iter().copied().filter(|s| *s != hslot).collect();
    let n = alive.len() as i64;
    let lim = ping_match_ms();
    let mut cands: Vec<((i64, i64, i64), usize)> = Vec::new();
    for s in &others {
        let t = &players[*s];
        if stale.contains(s) || no_host(t) || !act.contains(t) {
            continue;
        }
        let Some(mv) = mget(t) else { continue };
        if mv.0 >= *LOSS_KICK_PCT / 2.0 {
            continue; // unknown or lossy itself: not a target
        }
        let pings: Vec<Option<i64>> = others.iter().filter(|o| *o != s).map(|o| pair_ping(t, &players[*o])).collect();
        if pings.is_empty() || pings.iter().any(|p| p.is_none()) || pings.iter().flatten().copied().max().unwrap_or(0) > lim {
            continue;
        }
        let pv: Vec<i64> = pings.into_iter().flatten().collect();
        let rank = if crate::hostcap::mode() >= 1 { HOSTCAP.verdict(t, n)?.0 } else { 1 };
        let hk = crate::hostmig::host_key(&pv, perf_penalty(t));
        cands.push(((if rank >= 2 { 1 } else { 0 }, hk.0, hk.1), *s));
    }
    if cands.is_empty() {
        if !rec.2 {
            LOSS_HOST_HIGH.lock().insert(mid.to_string(), (host.clone(), rec.1, true));
            dlog!(
                "loss:",
                tag(&host),
                format!("host of match {}: no player without heavy loss within {} ms of the others - the host stays (removal after {} s)", mid, lim, *LOSS_KICK_S as i64)
            );
        }
        return Ok(None);
    }
    cands.sort();
    let tgt = cands[0].1;
    if HM.handover(mid, tgt as i64) {
        dlog!(
            "loss:",
            tag(&host),
            format!(
                "host of match {} loses {}% for {} s - host handed over to {} (worst {} ms)",
                mid,
                f1(hv.0),
                (now - rec.1) as i64,
                tag(&players[tgt]),
                cands[0].0 .1
            )
        );
        evt!("host-loss", mid, "handover to", tag(&players[tgt]));
        let roster: std::collections::BTreeSet<i64> = alive.iter().map(|s| *s as i64).collect();
        REPICK.migrated(mid, &roster);
        LOSS_HOST_HIGH.lock().remove(mid);
        return Ok(Some(tgt));
    }
    Ok(None)
}

// py: daemon.py:10826 loss_hold_note (the note a new search is refused with while the player's loss hold runs, else None)
pub fn loss_hold_note(token: &str, now: Option<f64>) -> Option<Vec<u8>> {
    if standin() == 5 {
        // (selftest k5: `b"LH" if token in STUB["loss_hold"] else None`)
        return if STUBS.lock().loss_hold.contains(token) { Some(b"LH".to_vec()) } else { None };
    }
    if *LOSS_GATE < 2 || *LOSS_HOLD_S <= 0.0 || LOSS_HOLD.lock().is_empty() {
        return None;
    }
    let now = now.unwrap_or_else(clock::now);
    let sid = BRIDGES.steamid(token);
    let k = if sid != 0 { sid.to_string() } else { token.to_string() };
    let h = LOSS_HOLD.lock().get(&k).copied()?;
    let left = *LOSS_HOLD_S - (now - h.0);
    if left <= 0.0 {
        LOSS_HOLD.lock().remove(&k);
        return None;
    }
    Some(srv_note_kw("loss_hold", token, &[("pct", (h.1 as i64).to_string()), ("m", std::cmp::max(1, ((left + 59.0) / 60.0).floor() as i64).to_string())]))
}

// ---------------------------------------------------------------- kick / AFK
// py: daemon.py:10260 admin_secret
pub fn admin_secret_path() -> std::path::PathBuf {
    sess::db_dir().join("admin-secret.txt")
}

pub fn admin_secret() -> std::io::Result<String> {
    let p = admin_secret_path();
    if let Ok(s) = std::fs::read(&p) {
        let v = py::strip(&String::from_utf8_lossy(&s)).to_string();
        if v.chars().count() >= 16 {
            return Ok(v);
        }
    }
    let v = crate::rnd::token_hex("daemon:admin_secret", 16);
    std::fs::write(&p, format!("{}\n", v))?;
    Ok(v)
}

fn sha_hex(t: &str) -> String {
    use sha2::{Digest, Sha256};
    Sha256::digest(t.as_bytes()).iter().map(|b| format!("{:02x}", b)).collect()
}

// py: daemon.py:10273 admin_reset_session (test aid: RST-close a player's held session socket)
pub fn admin_reset_session(pid: &Value) -> Value {
    let cand: Vec<String> = match pid.as_str() {
        Some(p) if p.chars().count() >= 4 => {
            let p = p.to_lowercase();
            SESSIONS.with(|s| s.keys().filter(|t| sha_hex(t).starts_with(&p)).cloned().collect())
        }
        _ => Vec::new(),
    };
    if cand.len() != 1 {
        return json!({"ok": false, "error": if cand.is_empty() { "no such session" } else { "ambiguous pid" }});
    }
    let Some(c) = SESSIONS.conn(&cand[0]) else { return json!({"ok": false, "error": "no session"}) };
    c.reset();
    evt!("admin-reset-session", tag(&cand[0]), "session socket reset (test aid)");
    json!({"ok": true, "player": tag(&cand[0])})
}

// py: daemon.py:10293 kick_reason_clean (admin-typed kick reason: control chars dropped, whitespace collapsed, trimmed, max 120 chars)
pub fn kick_reason_clean(reason: &str) -> String {
    let kept: String = reason.chars().filter(|c| *c >= ' ' && *c != '\x7f').collect();
    let mut s = String::new();
    let mut in_ws = false;
    for c in kept.chars() {
        if py::is_py_space(c) {
            if !in_ws {
                s.push(' ');
            }
            in_ws = true;
        } else {
            s.push(c);
            in_ws = false;
        }
    }
    let s = py::strip(&s);
    let cut: String = s.chars().take(120).collect();
    py::strip(&cut).to_string()
}

// py: daemon.py:10299 kick_text
pub fn kick_text(reason: &str, lang: &str) -> String {
    let reason = kick_reason_clean(reason).replace('"', "'");
    if !reason.is_empty() {
        srv_text("kicked_why", lang, &[("r", reason)])
    } else {
        srv_text("kicked", lang, &[])
    }
}

// py: daemon.py:10303 kick_token (the kick itself: admin kick / team move / AFK removal / ping / loss removal)
pub fn kick_token(tok: &str, why: &str, ktxt: &str, kind: &str, reason: &str, keep_out: bool, no_penalty: bool) -> Vec<String> {
    let mut did: Vec<String> = Vec::new();
    if no_penalty {
        // 11-14: its clan day for this match never books a deficit (CLAN_KICKED)
        if let Some(km) = MM.mid_of(tok) {
            let mut ck = CLAN_KICKED.lock();
            ck.insert((tok.to_string(), km), why.to_string());
            if ck.len() > 2000 {
                let first: Vec<(String, String)> = ck.keys().take(500).cloned().collect();
                for k in first {
                    ck.shift_remove(&k);
                }
            }
        }
    }
    if MM.dequeue(tok) {
        did.push("queue".into());
    }
    if MM.lock.lock().rq.drop(tok) {
        did.push("requeue ticket".into());
    }
    for _ in 0..4 {
        // a token can sit in more than one record
        let Some(mid) = MM.mid_of(tok) else { break };
        if let Some(others) = MM.cancel(tok) {
            // before the level start: called off, the others search again
            for o in &others {
                LOADED.lock().remove(o);
                if let Some(c) = SESSIONS.conn(o) {
                    let _ = push(&c, &searching_frame(Some(o)), true);
                }
            }
            did.push(format!("match {} called off", mid));
        } else if MM.lock.peek(|s| s.ended.contains(&mid)) {
            // finished match still held: just step out (no award logic on a result already in)
            MM.depart(tok, true);
            did.push(format!("left finished match {}", mid));
        } else {
            k_leave_running_match(tok, why);
            did.push(format!("left match {}", mid));
        }
    }
    if MM.in_match(tok) {
        // still listed (ended-but-held records): drop its membership
        MM.leave(tok);
        did.push("membership dropped".into());
    }
    {
        let mut g = MM.lock.lock();
        if !keep_out {
            g.departed_vol.remove(tok);
        }
        g.joining.shift_remove(tok);
        g.late_bonus.remove(tok);
        g.late_mult.remove(tok);
        g.seen.remove(tok);
        g.settle.remove(tok);
    }
    LOADED.lock().remove(tok);
    CD_GOT.lock().remove(tok);
    _SILENT.lock().remove(tok);
    SOCIAL.note(tok, "kicked", ktxt); // notice poll (op 0xBA, kind p): shown in a match too, for any client
    if let Some(c) = SESSIONS.conn(tok) {
        let t = ktxt.as_bytes();
        let t = &t[..t.len().min(0xf0)];
        if c.sendall(&[msg(0x11, t), msg(0x0d, &[1])].concat()).is_ok() {
            did.push("sent back to the menu".into());
        }
        SESSIONS.unregister(tok, &c);
    }
    PRES.drop_token(tok);
    evt!(kind, tag(tok), "-", if did.is_empty() { "nothing to clear".to_string() } else { did.join(", ") }, py::clean_str(reason, 120));
    did
}

// py: daemon.py:10372 afk_report (op 0xE3 trailer: seconds without input; removes the player at AFK_KICK_S)
pub fn afk_report(token: &str, idle_s: i64, flags: u8) {
    afk_report_ok(token, idle_s, flags);
}

/// `afk_report` with its True / False result
pub fn afk_report_ok(token: &str, idle_s: i64, flags: u8) -> bool {
    if *AFK_KICK_S <= 0.0 || idle_s == 0xFFFF || (idle_s as f64) < *AFK_KICK_S {
        return false;
    }
    let (mid, live) = {
        let g = MM.lock.lock();
        let mid = g.mid_locked(token);
        let live = mid.as_ref().is_some_and(|m| g.started.contains(m) && !g.ended.contains(m) && !g.departed.get(m).is_some_and(|d| d.contains(token)) && !g.joining.contains_key(token));
        (mid, live)
    };
    let Some(mid) = mid else { return false };
    if !live || AFK_KICKED.lock().contains(&(token.to_string(), mid.clone())) {
        return false;
    }
    if PRIV.rule(&mid, "afk") == Some(0) {
        return false; // private match with AFK REMOVAL off (11-17)
    }
    {
        let mut ak = AFK_KICKED.lock();
        ak.insert((token.to_string(), mid.clone()));
        if ak.len() > 2000 {
            ak.clear();
        }
    }
    dlog!("afk:", tag(token), format!("no input for {} s in match {} (ext flags {}) - removed from the match (counts as leaving early)", idle_s, mid, flags));
    let did = kick_token(
        token,
        &format!("left: afk (no input for {} s)", idle_s),
        &srv_text("afk_kicked", &lang_of(token), &[("s", idle_s.to_string())]),
        "afk-kick",
        &format!("no input for {} s", idle_s),
        true,
        true,
    );
    dlog!("afk:", tag(token), "removal done:", if did.is_empty() { "nothing".to_string() } else { did.join(", ") });
    true
}

// py: daemon.py:10838 admin_kick (the stub signature of the other chunks: no reason)
pub fn admin_kick(pid: &str, move_: bool) -> Value {
    admin_kick_reason(&Value::String(pid.to_string()), &Value::String(String::new()), move_)
}

// py: daemon.py:10838 admin_kick(pid, reason="", move=False) (clear a player's stuck server-side state so it can matchmake again)
pub fn admin_kick_reason(pid: &Value, reason: &Value, move_: bool) -> Value {
    let Some(p) = pid.as_str() else { return json!({"ok": false, "error": "bad pid"}) };
    let n = p.chars().count();
    if !(4..=64).contains(&n) || p.chars().any(|c| !"0123456789abcdef".contains(c.to_ascii_lowercase())) || !p.is_ascii() {
        return json!({"ok": false, "error": "bad pid"});
    }
    let pid = p.to_lowercase();
    let mut cand: HashSet<String> = PRES.with(|p| p.players.keys().cloned().collect());
    {
        let g = MM.lock.lock();
        cand.extend(g.queue.keys().cloned());
        for pl in g.matches.values() {
            cand.extend(pl.iter().cloned());
        }
    }
    cand.extend(SESSIONS.with(|s| s.keys().cloned().collect::<Vec<_>>()));
    cand.extend(LOADED.lock().iter().cloned());
    let toks: Vec<String> = cand.into_iter().filter(|t| !t.is_empty() && sha_hex(t).starts_with(&pid)).collect();
    if toks.len() != 1 {
        return json!({"ok": false, "error": if toks.is_empty() { "no such player" } else { "ambiguous pid" }});
    }
    let tok = &toks[0];
    let name = tag(tok);
    let rs = reason.as_str().unwrap_or("");
    let ktxt = if move_ { srv_text("moved", &lang_of(tok), &[]) } else { kick_text(rs, &lang_of(tok)) };
    let did = kick_token(tok, if move_ { "team move" } else { "admin kick" }, &ktxt, "admin-kick", &kick_reason_clean(rs), false, !move_);
    json!({"ok": true, "name": name, "did": did})
}

// ---------------------------------------------------------------- admin control (tools/admin_site; loopback only)
// py: daemon.py:10900 ADMIN_PORT
pub static ADMIN_PORT: LazyLock<i64> = LazyLock::new(|| {
    let d = if (7001..=7900).contains(&*PORT) { 7011 + *PORT - 7001 } else { 7011 };
    crate::env_parse("T2R_ADMIN_PORT", d)
});

// py: daemon.py:98 notices_public (the notice book's public list with endsAt moved from the steady clock to the real wall clock)
pub fn notices_public() -> Vec<Value> {
    let d = clock::wall_time() - clock::now();
    let mut out = NOTICES.public();
    for x in out.iter_mut() {
        if let Some(o) = x.as_object_mut() {
            if let Some(v) = o.get("endsAt").filter(|v| v.is_number()).and_then(|v| v.as_f64()) {
                o.insert("endsAt".into(), serde_json::Number::from_f64(v + d).map(Value::Number).unwrap_or(Value::Null));
            }
        }
    }
    out
}

/// `str(x)` of a JSON value
fn pystr(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        o => py::pyrepr(o),
    }
}

pub fn http_date(t: f64) -> String {
    // email.utils.formatdate(t, usegmt=True)
    let secs = t.floor() as i64;
    let days = secs.div_euclid(86400);
    let rem = secs.rem_euclid(86400);
    let (h, mi, s) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    let wd = ["Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"][days.rem_euclid(7) as usize];
    // civil from days (Howard Hinnant)
    let z = days + 719468;
    let era = z.div_euclid(146097);
    let doe = z.rem_euclid(146097);
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    let mon = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"][(m - 1) as usize];
    format!("{}, {:02} {} {:04} {:02}:{:02}:{:02} GMT", wd, d, mon, y, h, mi, s)
}

/// the Server header of Python's BaseHTTPRequestHandler (`BaseHTTP/0.6 Python/x.y.z`)
pub static ADMIN_SERVER_HDR: LazyLock<String> = LazyLock::new(|| crate::env_str("T2R_ADMIN_SERVER_HDR", "BaseHTTP/0.6 Python/3.12.3"));

fn reason_of(code: u16) -> (&'static str, &'static str) {
    match code {
        200 => ("OK", "Request fulfilled, document follows"),
        400 => ("Bad Request", "Bad request syntax or unsupported method"),
        403 => ("Forbidden", "Request forbidden -- authorization will not help"),
        404 => ("Not Found", "Nothing matches the given URI"),
        414 => ("Request-URI Too Long", "URI is too long"),
        431 => ("Request Header Fields Too Large", "The server is unwilling to process the request because either an individual header field, or all the header fields collectively, are too large"),
        501 => ("Not Implemented", "Server does not support this operation"),
        505 => ("HTTP Version Not Supported", "Cannot fulfill request"),
        _ => ("???", "???"),
    }
}

fn html_escape(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;")
}

/// BaseHTTPRequestHandler.send_response + headers of this handler
fn head_block(code: u16, message: Option<&str>, date: &str) -> String {
    let msg = message.unwrap_or_else(|| reason_of(code).0);
    format!("HTTP/1.0 {} {}\r\nServer: {}\r\nDate: {}\r\n", code, msg, *ADMIN_SERVER_HDR, date)
}

fn send_error_resp(code: u16, message: Option<&str>, explain: Option<&str>, head: bool, date: &str) -> Vec<u8> {
    let (sm, lm) = reason_of(code);
    let message = message.unwrap_or(sm);
    let explain = explain.unwrap_or(lm);
    let body = format!(
        "<!DOCTYPE HTML>\n<html lang=\"en\">\n    <head>\n        <meta charset=\"utf-8\">\n        <title>Error response</title>\n    </head>\n    <body>\n        <h1>Error response</h1>\n        <p>Error code: {}</p>\n        <p>Message: {}.</p>\n        <p>Error code explanation: {} - {}.</p>\n    </body>\n</html>\n",
        code,
        html_escape(message),
        code,
        html_escape(explain)
    );
    // (an empty `date` = the request was HTTP/0.9: Python writes no status line / headers, only the body)
    let mut out = if date.is_empty() { String::new() } else { head_block(code, Some(message), date) };
    if !date.is_empty() {
        out.push_str("Connection: close\r\n");
        out.push_str(&format!("Content-Type: text/html;charset=utf-8\r\nContent-Length: {}\r\n\r\n", body.len()));
    }
    let mut b = out.into_bytes();
    if !head {
        b.extend_from_slice(body.as_bytes());
    }
    b
}

fn json_resp(code: u16, obj: &Value, date: &str) -> Vec<u8> {
    let body = py::dumps(obj).into_bytes();
    if date.is_empty() {
        return body;
    }
    let mut out = head_block(code, None, date);
    out.push_str(&format!("Content-Type: application/json\r\nContent-Length: {}\r\nCache-Control: no-store\r\n\r\n", body.len()));
    let mut b = out.into_bytes();
    b.extend_from_slice(&body);
    b
}

fn latin1_str(b: &[u8]) -> String {
    b.iter().map(|c| *c as char).collect()
}

/// py: python's `repr(str)` of a latin-1 decoded request word
fn rep(s: &str) -> String {
    crate::pyfmt::str_repr(s)
}

/// the headers as `email` parsed them: (name, value) in order
type Headers = Vec<(String, String)>;

fn hget<'a>(h: &'a Headers, name: &str) -> Option<&'a str> {
    let n = name.to_lowercase();
    h.iter().find(|(k, _)| k.to_lowercase() == n).map(|(_, v)| v.as_str())
}

fn read_line_limited(r: &mut dyn BufRead, limit: usize) -> std::io::Result<Vec<u8>> {
    let mut buf = Vec::new();
    let mut t = (&mut *r).take(limit as u64);
    t.read_until(b'\n', &mut buf)?;
    Ok(buf)
}

/// http.client.parse_headers + email.parser: Err(message, explain) = send_error(431)
fn parse_headers(r: &mut dyn BufRead) -> Result<Headers, (&'static str, String)> {
    let mut lines: Vec<Vec<u8>> = Vec::new();
    loop {
        let line = read_line_limited(r, 65537).unwrap_or_default();
        if line.len() > 65536 {
            return Err(("Line too long", "got more than 65536 bytes when reading header line".to_string()));
        }
        lines.push(line.clone());
        if lines.len() > 100 {
            return Err(("Too many headers", "got more than 100 headers".to_string()));
        }
        if line == b"\r\n" || line == b"\n" || line.is_empty() {
            break;
        }
    }
    let text = latin1_str(&lines.concat());
    let mut out: Headers = Vec::new();
    let mut cur: Option<(String, String)> = None;
    for raw in text.split_inclusive('\n') {
        if raw == "\r\n" || raw == "\n" {
            break;
        }
        let first = raw.chars().next().unwrap_or('\n');
        if first == ' ' || first == '\t' {
            if let Some(c) = cur.as_mut() {
                c.1.push_str(raw);
            }
            continue;
        }
        // headerRE: [\041-\071\073-\176]*: (or "From ")
        let name_len = raw.chars().take_while(|c| ((*c as u32) >= 0x21 && (*c as u32) <= 0x39) || ((*c as u32) >= 0x3b && (*c as u32) <= 0x7e)).count();
        if raw.chars().nth(name_len) != Some(':') {
            break;
        }
        if let Some(c) = cur.take() {
            out.push(c);
        }
        let name: String = raw.chars().take(name_len).collect();
        let value: String = raw.chars().skip(name_len + 1).collect();
        cur = Some((name, value.trim_start_matches([' ', '\t']).to_string()));
    }
    if let Some(c) = cur.take() {
        out.push(c);
    }
    for h in out.iter_mut() {
        h.1 = h.1.trim_end_matches(['\r', '\n']).to_string();
    }
    Ok(out)
}

/// py: `self.path` -> the response (Err = the handler raised: logged, and for GET no response is written)
fn admin_get(path: &str, headers: &Headers, secret: &str, date: &str) -> Result<Vec<u8>, String> {
    let authed = |h: &Headers| -> Result<bool, String> {
        let v = hget(h, "X-Admin-Secret").unwrap_or("");
        if !v.is_ascii() {
            return Err("TypeError('comparing strings with non-ASCII characters is not supported')".into());
        }
        Ok(v == secret)
    };
    if path == "/status" {
        let (q, m) = (MM.lock.peek(|s| s.queue.len()), MM.lock.peek(|s| s.matches.len()));
        return Ok(json_resp(
            200,
            &json!({"ok": true, "online": PRES.count(), "queue": q, "matches": m, "server_version": server_version(), "uptime_s": py::round0(clock::now() - started_at())}),
            date,
        ));
    }
    if !authed(headers)? {
        return Ok(json_resp(403, &json!({"ok": false, "error": "forbidden"}), date));
    }
    if path == "/notices" {
        return Ok(json_resp(200, &json!({"ok": true, "notices": notices_public(), "target_version": update_target(), "now": clock::wall_time()}), date));
    }
    if path == "/sizes" {
        return Ok(json_resp(200, &json!({"ok": true, "sizes": state_sizes(), "uptime_s": py::round0(clock::now() - started_at())}), date));
    }
    if path == "/teams" {
        return Ok(json_resp(200, &json!({"ok": true, "rules": team_rules_text(), "preview": teams_preview(), "live": teams_live()}), date));
    }
    if path.starts_with("/clan/backups") {
        let sid: i64 = if let Some((_, after)) = path.split_once("steamid=") {
            if after.is_empty() {
                0
            } else {
                py_int_str(after).ok_or_else(|| val_err_int(after))?
            }
        } else {
            0
        };
        let dir = sess::db_dir().join("clan-resets").to_string_lossy().into_owned();
        let backups = crate::clan::clan_reset::list_backups(&dir, if sid != 0 { Some(sid) } else { None });
        return Ok(json_resp(200, &json!({"ok": true, "backups": backups}), date));
    }
    if path == "/live" {
        return Ok(json_resp(200, &json!({"ok": true, "ts": clock::wall_time(), "server_version": server_version(), "live": live_snapshot()}), date));
    }
    Ok(json_resp(404, &json!({"ok": false}), date))
}

/// `repr(ValueError("invalid literal for int() with base 10: ..."))`
fn val_err_int(s: &str) -> String {
    format!("ValueError({})", rep(&format!("invalid literal for int() with base 10: {}", rep(s))))
}

/// Python `int(str)` (whitespace around, sign, digits and underscores between digits)
fn py_int_str(s: &str) -> Option<i64> {
    let t = py::strip(s);
    let (neg, d) = match t.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, t.strip_prefix('+').unwrap_or(t)),
    };
    if d.is_empty() || d.starts_with('_') || d.ends_with('_') || d.contains("__") || !d.chars().all(|c| c.is_ascii_digit() || c == '_') {
        return None;
    }
    let v: i64 = d.replace('_', "").parse().ok()?;
    Some(if neg { -v } else { v })
}

fn bfloat(v: &Value) -> Result<f64, String> {
    pyfloat(v).ok_or_else(|| format!("TypeError('float() argument must be a string or a real number, not {}')", py::pyrepr(v)))
}

fn bint(v: &Value) -> Result<i64, String> {
    match v {
        Value::String(s) => py_int_str(s).ok_or_else(|| val_err_int(s)),
        o => pyint(o).ok_or_else(|| format!("TypeError('int() argument must be a string, a bytes-like object or a real number, not {}')", py::pyrepr(o))),
    }
}

/// POST handlers: the body (`body.get` on a non-object raises)
fn admin_post_path(path: &str, body: &Value) -> Result<Option<Value>, String> {
    let get = |k: &str| -> Result<Option<&Value>, String> {
        body.as_object().map(|o| o.get(k)).ok_or_else(|| format!("AttributeError('{} object has no attribute \\'get\\'')", "x"))
    };
    let dflt = |k: &str, d: Value| -> Result<Value, String> { Ok(get(k)?.cloned().unwrap_or(d)) };
    match path {
        "/notice" => {
            let sec = get("seconds")?.cloned().unwrap_or(Value::Null);
            let minutes = {
                let m = dflt("minutes", json!(30))?;
                if truthy(&m) {
                    bfloat(&m)?
                } else {
                    30.0
                }
            };
            let text = pystr(&dflt("text", json!(""))?);
            let secs = if sec.is_null() { None } else { Some(bfloat(&sec)?) };
            let tpl = {
                let v = dflt("text_pl", json!(""))?;
                if truthy(&v) {
                    pystr(&v)
                } else {
                    String::new()
                }
            };
            let nid = notice_send(&text, minutes, secs, &tpl);
            Ok(Some(match nid {
                Some(n) if n != 0 => json!({"ok": true, "id": n}),
                _ => json!({"ok": false, "error": "empty text"}),
            }))
        }
        "/notice/clear" => {
            let id = bint(&dflt("id", json!(-1))?)?;
            Ok(Some(json!({"ok": NOTICES.clear(id)})))
        }
        "/shutdown" => {
            let minutes = bfloat(&dflt("minutes", json!(5))?)?;
            let reason = pystr(&dflt("reason", json!(""))?);
            let restart = truthy(&dflt("restart", json!(true))?);
            let rpl = {
                let v = dflt("reason_pl", json!(""))?;
                if truthy(&v) {
                    pystr(&v)
                } else {
                    String::new()
                }
            };
            let s = shutdown_schedule(minutes, &reason, restart, &rpl);
            Ok(Some(json!({"ok": true, "shutdown": {"id": s.id, "at": s.at, "reason": s.reason, "reason_pl": s.reason_pl, "restart": s.restart}})))
        }
        "/boot-all" => Ok(Some(json!({"ok": true, "closed": boot_all_sessions()}))),
        "/shutdown/cancel" => Ok(Some(json!({"ok": shutdown_cancel()}))),
        "/teams/move" => {
            let v = get("pid")?.cloned().unwrap_or(Value::Null);
            let pid = if truthy(&v) { pystr(&v) } else { String::new() };
            Ok(Some(admin_move_team(&pid)))
        }
        "/teams" => {
            let r = dflt("rules", json!(""))?;
            team_rules_set(&pystr(&r));
            Ok(Some(json!({"ok": true, "rules": team_rules_text(), "preview": teams_preview()})))
        }
        "/kick" => {
            let pid = get("pid")?.cloned().unwrap_or(Value::Null);
            let reason = get("reason")?.cloned().filter(truthy).unwrap_or(Value::String(String::new()));
            Ok(Some(admin_kick_reason(&pid, &reason, false)))
        }
        "/handover" => {
            // A/B test aid (10-31, hm-peer0 fix): start a planned host handover of match `mid` (default: the only live one) to `slot` now
            let mids = HM.mids();
            let given = get("mid")?.cloned().filter(truthy);
            let mid: Option<String> = match given {
                Some(v) => Some(v.as_str().ok_or("TypeError('unhashable')")?.to_string()),
                None => {
                    if mids.len() == 1 {
                        Some(mids[0].clone())
                    } else {
                        None
                    }
                }
            };
            let Some(mid) = mid.filter(|_| REPICK.mode >= 2) else {
                return Ok(Some(json!({"ok": false, "error": "needs T2R_HOST_REPICK=2 and one live match (or mid)", "mids": mids})));
            };
            let slot_v = dflt("slot", json!(1))?;
            let ok = HM.handover(&mid, bint(&slot_v)?);
            let why = HM.handover_ok(&mid).1;
            dlog!("ADMIN handover", mid, "to slot", pystr(&slot_v), "->", py::b(ok), "|", why);
            Ok(Some(json!({"ok": ok, "mid": mid, "why": HM.handover_ok(&mid).1})))
        }
        "/reset-session" => Ok(Some(admin_reset_session(&get("pid")?.cloned().unwrap_or(Value::Null)))),
        "/clan/reset" => {
            get("name")?;
            Ok(Some(admin_clan_reset(body)))
        }
        "/clan/restore" => {
            get("file")?;
            Ok(Some(admin_clan_restore(body)))
        }
        _ => Ok(None),
    }
}

/// One admin request: `rfile` = the connection's read side; the full response bytes (empty = nothing is written back).
pub fn admin_handle(rfile: &mut dyn BufRead, secret: &str, date_full: &str) -> Vec<u8> {
    let mut date = "";
    let raw = match read_line_limited(rfile, 65537) {
        Ok(r) => r,
        Err(_) => return Vec::new(), // timeout: discard this connection
    };
    if raw.len() > 65536 {
        return send_error_resp(414, None, None, false, date_full);
    }
    if raw.is_empty() {
        return Vec::new();
    }
    let requestline = latin1_str(&raw);
    let requestline = requestline.trim_end_matches(['\r', '\n']).to_string();
    let words: Vec<&str> = requestline.split(py::is_py_space).filter(|w| !w.is_empty()).collect();
    if words.is_empty() {
        return Vec::new();
    }
    if words.len() >= 3 {
        let version = words[words.len() - 1];
        let bad = (|| -> bool {
            let Some(base) = version.strip_prefix("HTTP/") else { return true };
            let vn: Vec<&str> = base.split('.').collect();
            vn.len() != 2 || vn.iter().any(|c| c.is_empty() || !c.chars().all(|d| d.is_ascii_digit())) || vn.iter().any(|c| c.len() > 10)
        })();
        if bad {
            return send_error_resp(400, Some(&format!("Bad request version ({})", rep(version))), None, false, date);
        }
        let base = version.strip_prefix("HTTP/").unwrap_or("");
        let major: u64 = base.split('.').next().and_then(|m| m.parse().ok()).unwrap_or(0);
        if major >= 2 {
            return send_error_resp(505, Some(&format!("Invalid HTTP version ({})", base)), None, false, date);
        }
        date = date_full; // request_version = version (HTTP/0.9 stays for a request line of two words)
    }
    if !(2..=3).contains(&words.len()) {
        return send_error_resp(400, Some(&format!("Bad request syntax ({})", rep(&requestline))), None, false, date);
    }
    let (command, mut path) = (words[0].to_string(), words[1].to_string());
    if words.len() == 2 && command != "GET" {
        return send_error_resp(400, Some(&format!("Bad HTTP/0.9 request type ({})", rep(&command))), None, false, date);
    }
    if path.starts_with("//") {
        path = format!("/{}", path.trim_start_matches('/'));
    }
    let headers = match parse_headers(rfile) {
        Ok(h) => h,
        Err((m, e)) => return send_error_resp(431, Some(m), Some(&e), command == "HEAD", date),
    };
    match command.as_str() {
        "GET" => match admin_get(&path, &headers, secret, date) {
            Ok(r) => r,
            Err(e) => {
                dlog!("ADMIN-ERR", e);
                Vec::new()
            }
        },
        "POST" => {
            let res = (|| -> Result<Vec<u8>, String> {
                let v = hget(&headers, "X-Admin-Secret").unwrap_or("");
                if !v.is_ascii() {
                    return Err("TypeError('comparing strings with non-ASCII characters is not supported')".into());
                }
                if v != secret {
                    return Ok(json_resp(403, &json!({"ok": false, "error": "forbidden"}), date));
                }
                let cl = hget(&headers, "Content-Length").filter(|s| !s.is_empty());
                let n = match cl {
                    None => 0,
                    Some(s) => py_int_str(s).ok_or_else(|| val_err_int(s))?,
                };
                let n = n.min(8192).max(0) as usize;
                let mut buf = vec![0u8; n];
                let mut got = 0;
                while got < n {
                    match rfile.read(&mut buf[got..]) {
                        Ok(0) => break,
                        Ok(k) => got += k,
                        Err(e) => return Err(crate::pyfmt::os_error_repr(&e)),
                    }
                }
                buf.truncate(got);
                let txt: &[u8] = if buf.is_empty() { b"{}" } else { &buf };
                let txt = txt.strip_prefix(&[0xEF, 0xBB, 0xBF][..]).unwrap_or(txt);
                let body: Value = serde_json::from_slice(txt).map_err(|e| py_json_err(txt, &e))?;
                match admin_post_path(&path, &body)? {
                    Some(v) => Ok(json_resp(200, &v, date)),
                    None => Ok(json_resp(404, &json!({"ok": false}), date)),
                }
            })();
            match res {
                Ok(r) => r,
                Err(e) => {
                    dlog!("ADMIN-ERR", e);
                    json_resp(400, &json!({"ok": false, "error": "bad request"}), date)
                }
            }
        }
        _ => send_error_resp(501, Some(&format!("Unsupported method ({})", rep(&command))), None, command == "HEAD", date),
    }
}

/// the text of Python's `repr(json.JSONDecodeError)` for the common case (a body that does not start with a JSON value); serde's own text otherwise
fn py_json_err(txt: &[u8], e: &serde_json::Error) -> String {
    let t = String::from_utf8_lossy(txt);
    let ws = t.chars().take_while(|c| py::is_py_space(*c)).count();
    let m = e.to_string();
    if (m.starts_with("expected value") || m.starts_with("expected ident")) && e.line() == 1 && e.column() <= ws + 6 {
        return format!("JSONDecodeError('Expecting value: line 1 column {} (char {})')", ws + 1, ws);
    }
    format!("JSONDecodeError({})", m)
}

fn admin_conn(mut s: TcpStream, secret: &str) {
    let to: f64 = crate::env_parse("T2R_ADMIN_TIMEOUT_S", 10.0);
    let _ = s.set_read_timeout(Some(std::time::Duration::from_secs_f64(to)));
    let _ = s.set_write_timeout(Some(std::time::Duration::from_secs_f64(to)));
    let rd = match s.try_clone() {
        Ok(r) => r,
        Err(_) => return,
    };
    let mut rd = std::io::BufReader::new(rd);
    let resp = admin_handle(&mut rd, secret, &http_date(clock::wall_time()));
    if !resp.is_empty() {
        let _ = s.write_all(&resp);
        let _ = s.flush();
    }
    let _ = s.shutdown(std::net::Shutdown::Both);
}

fn bind_admin(host: &str, port: u16) -> std::io::Result<TcpListener> {
    use socket2::{Domain, Protocol, Socket, Type};
    let addr: std::net::SocketAddr = format!("{}:{}", host, port).parse().map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidInput, format!("{}", e)))?;
    let retry_s: f64 = CONN_CFG.bind_retry_s;
    let t0 = std::time::Instant::now();
    let mut nxt = 1.0f64;
    loop {
        let s = Socket::new(Domain::IPV4, Type::STREAM, Some(Protocol::TCP))?;
        s.set_reuse_address(true)?; // POSIX: only lets a listener rebind over TIME_WAIT, never steals a live port
        match s.bind(&addr.into()) {
            Ok(()) => {
                let el = t0.elapsed().as_secs_f64();
                if el > 0.3 {
                    println!("BIND ADMIN {}:{} ok after {:.1}s of retries", host, port, el);
                }
                s.listen(5)?;
                return Ok(s.into());
            }
            Err(e) => {
                let el = t0.elapsed().as_secs_f64();
                if el >= retry_s {
                    return Err(e);
                }
                if el >= nxt {
                    println!("BIND ADMIN {}:{} busy ({}); retrying for up to {:.0}s more", host, port, e, retry_s - el);
                    nxt = el + 5.0;
                }
                std::thread::sleep(std::time::Duration::from_millis(250));
            }
        }
    }
}

// py: daemon.py:10985 admin_server
pub fn admin_server() -> std::io::Result<()> {
    let secret = admin_secret()?;
    let l = bind_admin("127.0.0.1", *ADMIN_PORT as u16)?;
    dlog!("ADMIN listening on", format!("127.0.0.1:{} (secret file {})", *ADMIN_PORT, admin_secret_path().display()));
    loop {
        let (s, _) = match l.accept() {
            Ok(x) => x,
            Err(_) => {
                std::thread::sleep(std::time::Duration::from_millis(50));
                continue;
            }
        };
        let sec = secret.clone();
        let _ = std::thread::Builder::new().name("admin-conn".into()).stack_size(1 << 20).spawn(move || admin_conn(s, &sec));
    }
}

// py: daemon.py:11039 admin_start
pub fn admin_start() {
    if *ADMIN_PORT <= 0 {
        return;
    }
    let _ = std::thread::Builder::new().name("admin".into()).stack_size(1 << 20).spawn(|| {
        if let Err(e) = admin_server() {
            dlog!("ADMIN server not started:", crate::pyfmt::os_error_repr(&e));
        }
    });
}

// py: daemon.py:11052 main (the `serve` subcommand): the start order of `python3 daemon.py`
pub fn serve() -> i32 {
    let _ = started_at();
    init_k3();
    dlog!("DB check:", db::db_guard());
    live_load(None);
    let sv = server_version();
    dlog!("daemon version", if sv.is_empty() { "dev (no version check)".to_string() } else { sv }, "| require client version:", py::b(*REQUIRE_VERSION));
    if crate::env_str("T2R_UDP", "1") != "0" {
        dlog!("UDP relay is not part of this process: run t2r-daemon relay");
    } else {
        dlog!("UDP off (T2R_UDP=0): the relay runs in t2r-daemon");
    }
    let _ = std::thread::Builder::new().name("status".into()).stack_size(2 << 20).spawn(supervised(status_loop, "status"));
    let _ = std::thread::Builder::new().name("matchmaker".into()).stack_size(2 << 20).spawn(supervised(matchmaker_loop, "matchmaker"));
    CLAN_SCHED.store(true, std::sync::atomic::Ordering::SeqCst);
    let _ = std::thread::Builder::new().name("clan-scheduler".into()).stack_size(2 << 20).spawn(supervised(clan_scheduler, "clan-scheduler"));
    admin_start();
    install_exit_hooks();
    let _ = std::thread::Builder::new().name("ident-backfill".into()).stack_size(1 << 20).spawn(ident_backfill); // 11-23: identity ids for accounts recorded before ident.py
    dlog!("daemon start");
    tcp_server();
    0
}

#[allow(dead_code)]
fn _unused(_: VecDeque<u8>) {}
