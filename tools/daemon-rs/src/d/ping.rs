//! K3 ping: Steam ping locations + the ping mesh, pair pings, the pings the gates judge, performance reports, host capacity / one-way notes,
//! the no-host list, host choice and the P2P push. daemon.py:4296-4885.
use super::*;
use crate::{clock, hostcap, hostmig, py};
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet, VecDeque};
use std::sync::atomic::Ordering;
use std::sync::LazyLock;

fn tag_list(players: &[String]) -> String {
    py::list_str(&players.iter().map(|p| tag(p)).collect::<Vec<_>>())
}

/// daemon.py:4296 PING_LOC: token -> ping location string (the last ACCEPTED one: forwarded to the others, decodes the region)
pub static PING_LOC: LazyLock<Mutex<IndexMap<String, String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:4297 PING_AT: (from token, to token) -> time of the last report
pub static PING_AT: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:4298 LOC_FWD: token -> time its location was last forwarded
pub static LOC_FWD: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:4299 PING: (from token, to token) -> the last 5 ms reports (pair_ping = median)
pub static PING: LazyLock<Mutex<IndexMap<(String, String), VecDeque<i64>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:4305 LOC_HIST: token -> best-ms of the last 8 accepted locations
pub static LOC_HIST: LazyLock<Mutex<HashMap<String, VecDeque<i64>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:4306 LOC_SPIKES: token -> consecutive spiked samples
pub static LOC_SPIKES: LazyLock<Mutex<HashMap<String, i64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:4307 REGION: token -> stable region
pub static REGION: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:4308 REGION_CAND: token -> (region, count) waiting to confirm a change
pub static REGION_CAND: LazyLock<Mutex<HashMap<String, (String, i64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

/// `REGION.get(token) or hostmig.region_of(PING_LOC.get(token))`
pub fn region_of_token(token: &str) -> Option<String> {
    if let Some(r) = REGION.lock().get(token).filter(|r| !r.is_empty()) {
        return Some(r.clone());
    }
    let loc = PING_LOC.lock().get(token).cloned();
    hostmig::region_of(loc.as_deref()).map(|s| s.to_string())
}

// py: daemon.py:4310 mm_region: matchmaking region of a token: the stable region, else the decode of its last ping location; None = unknown
pub fn mm_region(token: &str) -> Option<String> {
    if WORLD_ON.load(Ordering::Relaxed) {
        return WORLD.lock().regions.get(token).cloned();
    }
    region_of_token(token)
}

pub const LOC_MAX: usize = 900; // the helper reads < 1100 B; a real Steam location string is a few hundred
static LOC_OK: LazyLock<regex::Regex> = LazyLock::new(|| regex::Regex::new(r"^[A-Za-z0-9=+,:/_. -]+$").unwrap());
// py: daemon.py:4322 LOC_OK.match(loc) (the `$` of Python also matches before a final newline)
pub fn loc_ok(loc: &str) -> bool {
    LOC_OK.is_match(loc.strip_suffix('\n').unwrap_or(loc)) && !loc.ends_with("\n\n")
}

static LOC_BEST: LazyLock<regex::Regex> = LazyLock::new(|| regex::Regex::new(r"([a-z][a-z0-9]{2,4})=(\d+)").unwrap());
// py: daemon.py:4325 _loc_best
pub fn loc_best(loc: &str) -> Option<i64> {
    let low = loc.to_lowercase();
    LOC_BEST.captures_iter(&low).map(|c| c[2].parse::<i64>().unwrap_or(i64::MAX)).min()
}

pub static PING_MAX: LazyLock<usize> = LazyLock::new(|| crate::env_parse("T2R_PING_MAX", 16384usize));
/// daemon.py:4339 PING_LAST: the last pair ping per unordered pair = (raw ms, time)
pub static PING_LAST: LazyLock<Mutex<IndexMap<PairKey, (Option<i64>, f64)>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// `PING_LAST.get(frozenset((a, b)))`
pub fn ping_last_get(a: &str, b: &str) -> Option<(Option<i64>, f64)> {
    PING_LAST.lock().get(&pair_key(a, b)).copied()
}

// py: daemon.py:4340 _drop_pings
pub fn drop_pings(token: &str) {
    PING.lock().retain(|k, _| !(k.0 == token || k.1 == token));
    PING_AT.lock().retain(|k, _| !(k.0 == token || k.1 == token));
    PING_LAST.lock().retain(|k, _| !(k.0 == token || k.1 == token));
}

// py: daemon.py:4348 _ping_evict: drop n entries of the full PING table: pairs with a player who is not online first, then the least recently reported
static PING_EVICTING: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Drop the `n` pairs Python's sort picks (offline pairs first, then the least recently reported; ties in insertion order).
/// One evictor at a time: reports that find the table full while another thread evicts go in over the cap (by a few) instead of each
/// copying the whole table (11-26: dozens of helper threads at once held ~10 MB copies each and RSS jumped by hundreds of MB).
/// One pass over the table under its lock (PING_AT before PING, as the state GC takes them), no per-key shift_remove.
fn ping_evict(online: &HashSet<String>, n: usize) {
    use std::sync::atomic::Ordering as O;
    if PING_EVICTING.swap(true, O::AcqRel) {
        return;
    }
    {
        let mut at = PING_AT.lock();
        let mut p = PING.lock();
        let mut order: Vec<(bool, f64, usize)> = p.keys().enumerate().map(|(i, k)| (online.contains(&k.0) && online.contains(&k.1), at.get(k).copied().unwrap_or(0.0), i)).collect();
        let n = n.min(order.len());
        if n > 0 {
            let cmp = |a: &(bool, f64, usize), b: &(bool, f64, usize)| a.0.cmp(&b.0).then(a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal)).then(a.2.cmp(&b.2));
            if n < order.len() {
                order.select_nth_unstable_by(n - 1, cmp);
            }
            let mut drop = vec![false; p.len()];
            for x in &order[..n] {
                drop[x.2] = true;
            }
            let mut i = 0usize;
            p.retain(|k, _| {
                let d = drop[i];
                i += 1;
                if d {
                    at.remove(k);
                }
                !d
            });
        }
    }
    PING_EVICTING.store(false, O::Release);
}

pub static PING_MESH: LazyLock<bool> = LazyLock::new(|| crate::env_str("T2R_PING_MESH", "1") != "0");
pub const MESH_ACTIVATE_S: f64 = 60.0;
/// daemon.py:4372 MESH_ON: token -> time of its last full swap as a searcher
pub static MESH_ON: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:4374 mesh_peers: tokens whose helpers need this player's location (and whose locations it needs); None = everyone (T2R_PING_MESH=0)
pub fn mesh_peers(token: &str, view: Option<(HashSet<String>, HashMap<String, String>)>) -> Option<HashSet<String>> {
    if !*PING_MESH {
        return None;
    }
    let (q, live) = view.unwrap_or_else(|| MM.mesh_view());
    let mut out: HashSet<String>;
    if q.contains(token) {
        out = q.clone();
        out.extend(live.keys().cloned());
    } else if let Some(mid) = live.get(token) {
        out = live.iter().filter(|(_, m)| *m == mid).map(|(t, _)| t.clone()).collect();
        out.extend(q.iter().cloned());
    } else {
        return Some(HashSet::new());
    }
    out.remove(token);
    Some(out)
}

// py: daemon.py:4388 _mesh_send: send token's location to its peers' helpers (both=True: and each peer's location to token's helper)
fn mesh_send(links: &IndexMap<String, (Conn, u64)>, token: &str, peers: Option<&HashSet<String>>, loc: &str, both: bool) {
    let Some(me) = links.get(token) else { return };
    let snapshot: Vec<(String, String)> = PING_LOC.lock().iter().map(|(k, v)| (k.clone(), v.clone())).collect();
    for (other, oloc) in snapshot {
        if other == token || peers.is_some_and(|p| !p.contains(&other)) {
            continue;
        }
        let Some(ol) = links.get(&other) else { continue };
        let r = (|| -> Result<(), String> {
            if !loc.chars().all(|c| (c as u32) < 256) || !oloc.chars().all(|c| (c as u32) < 256) {
                return Err("UnicodeEncodeError".to_string());
            }
            let mut a = me.1.to_le_bytes().to_vec();
            a.extend(loc.chars().map(|c| c as u32 as u8));
            push(&ol.0, &msg(0xF5, &a), true).map_err(|e| crate::pyfmt::os_error_repr(&e))?;
            if both {
                let mut b = ol.1.to_le_bytes().to_vec();
                b.extend(oloc.chars().map(|c| c as u32 as u8));
                push(&me.0, &msg(0xF5, &b), true).map_err(|e| crate::pyfmt::os_error_repr(&e))?;
            }
            Ok(())
        })();
        if let Err(e) = r {
            dlog!("PING-LOC ERR", tag(token), e);
        }
    }
}

// py: daemon.py:4403 mesh_activate: a player started searching: swap locations with every searcher and in-match player now
pub fn mesh_activate(token: &str) {
    if !*PING_MESH || token.is_empty() {
        return;
    }
    let now = clock::now();
    if now - MESH_ON.lock().get(token).copied().unwrap_or(0.0) < MESH_ACTIVATE_S {
        return;
    }
    let Some(loc) = PING_LOC.lock().get(token).cloned().filter(|l| !l.is_empty()) else { return };
    MESH_ON.lock().insert(token.to_string(), now);
    mesh_send(&BRIDGES.links(), token, mesh_peers(token, None).as_ref(), &loc, true);
}

// py: daemon.py:4418 ping_location
pub fn ping_location(token: &str, loc: &str) {
    if loc.is_empty() {
        return;
    }
    let same = PING_LOC.lock().get(token).is_some_and(|l| l == loc);
    if same {
        // 10-19: an unchanged location is re-forwarded every 60 s: in-match RTT refresh
        if clock::now() - LOC_FWD.lock().get(token).copied().unwrap_or(0.0) >= 60.0 && BRIDGES.conn(token).is_some() {
            LOC_FWD.lock().insert(token.to_string(), clock::now());
            mesh_send(&BRIDGES.links(), token, mesh_peers(token, None).as_ref(), loc, false);
        }
        return;
    }
    let best = loc_best(loc);
    {
        let mut hs = LOC_HIST.lock();
        let hist = hs.entry(token.to_string()).or_default();
        if let (Some(best), false) = (best, hist.is_empty()) {
            let mut v: Vec<i64> = hist.iter().copied().collect();
            v.sort();
            let med = v[v.len() / 2];
            if best > std::cmp::max(2 * med, med + 80) {
                let n = {
                    let mut sp = LOC_SPIKES.lock();
                    let n = sp.get(token).copied().unwrap_or(0) + 1;
                    sp.insert(token.to_string(), n);
                    n
                };
                if n < 3 {
                    dlog!("PING-LOC", tag(token), format!("spike ignored: best {} ms vs usual {} ms ({} in a row)", best, med, n), "loc", py::cut(loc, 80));
                    return;
                }
                dlog!(
                    "PING-LOC",
                    tag(token),
                    format!("connection changed (3 spikes in a row): best {} ms, usual was {} ms - accepted, pair pings reset", best, med)
                );
                hist.clear();
                drop_pings(token);
            }
        }
    }
    LOC_SPIKES.lock().insert(token.to_string(), 0);
    if let Some(b) = best {
        let mut hs = LOC_HIST.lock();
        let hist = hs.entry(token.to_string()).or_default();
        hist.push_back(b);
        if hist.len() > 8 {
            hist.pop_front();
        }
    }
    PING_LOC.lock().insert(token.to_string(), loc.to_string());
    LOC_FWD.lock().insert(token.to_string(), clock::now());
    let r = hostmig::region_of(Some(loc)).map(|s| s.to_string());
    let old = REGION.lock().get(token).cloned();
    match (&r, &old) {
        (Some(r), None) => {
            REGION.lock().insert(token.to_string(), r.clone());
        }
        (Some(r), Some(old)) if r != old => {
            let (cr, cn) = REGION_CAND.lock().get(token).cloned().unwrap_or((String::new(), 0));
            let cn = if cr == *r { cn + 1 } else { 1 };
            REGION_CAND.lock().insert(token.to_string(), (r.clone(), cn));
            if cn >= 2 {
                REGION.lock().insert(token.to_string(), r.clone());
                REGION_CAND.lock().remove(token);
                drop_pings(token);
                dlog!("PING-LOC", tag(token), format!("region changed {} -> {} (2 samples agree) - pair pings reset", old, r));
            }
        }
        _ => {
            REGION_CAND.lock().remove(token);
        }
    }
    dlog!(
        "PING-LOC",
        tag(token),
        "region",
        REGION.lock().get(token).cloned().unwrap_or_else(|| "None".to_string()),
        format!("(sample {})", r.clone().unwrap_or_else(|| "None".to_string())),
        "loc",
        py::cut(loc, 120)
    ); // 10-15: the region decode was written from the format docs - log real strings
    mesh_send(&BRIDGES.links(), token, mesh_peers(token, None).as_ref(), loc, true);
}

// py: daemon.py:4476 ping_report
pub fn ping_report(token: &str, steamid: u64, ms: i64) {
    let other = BRIDGES.token_of_sid(steamid);
    let Some(other) = other else { return };
    let full = {
        let p = PING.lock();
        p.len() >= *PING_MAX && !p.contains_key(&(token.to_string(), other.clone()))
    };
    if ms >= 0 {
        if full {
            // bounded by a cap: offline pairs out first, then the least recently reported (R-MST5)
            let online: HashSet<String> = BRIDGES.tokens().into_iter().collect();
            ping_evict(&online, std::cmp::max(1, *PING_MAX / 8));
        }
        {
            let mut p = PING.lock();
            let d = p.entry((token.to_string(), other.clone())).or_default();
            d.push_back(ms);
            if d.len() > 5 {
                d.pop_front();
            }
        }
        let now = clock::now();
        PING_AT.lock().insert((token.to_string(), other.clone()), now);
        let raw = pair_raw(token, &other);
        PING_LAST.lock().insert(pair_key(token, &other), (raw, now));
    }
}

// py: daemon.py:4491 _ping_med
fn ping_med(d: Option<&VecDeque<i64>>) -> Option<i64> {
    let d = d?;
    if d.is_empty() {
        return None;
    }
    let mut v: Vec<i64> = d.iter().copied().collect();
    v.sort();
    Some(v[v.len() / 2])
}

pub static PING_MATCH_MS: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_PING_MATCH_MS", 175i64));
pub static PING_WAIT_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PING_WAIT_S", 10.0));
pub static PING_MEMORY_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PING_MEMORY_S", 21600.0));
pub static PING_CORR_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PING_CORR_S", 43200.0));
pub const PING_CORR_EXCESS_MS: i64 = 40;
pub const PING_CORR_KICKS: usize = 2;
pub const PING_NOTE_AFTER_S: f64 = 60.0;
pub const PING_NOTE_EVERY_S: f64 = 120.0;
/// daemon.py:4459 MEASURED_PAIR: (a, b) -> (measured ms, time): both orders, set when the ping removal took a player off a host
pub static MEASURED_PAIR: LazyLock<Mutex<IndexMap<(String, String), (i64, f64)>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:4460 PING_EXCESS: token -> [(measured - formation estimate, time)] of its removals
pub static PING_EXCESS: LazyLock<Mutex<IndexMap<String, Vec<(i64, f64)>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
/// daemon.py:4461 PING_CORR: token -> (ms added to its estimated pair pings, until)
pub static PING_CORR: LazyLock<Mutex<HashMap<String, (i64, f64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:4463 PING_WAIT_SINCE: token -> first time it was ready in a lobby with an unknown pair ping
pub static PING_WAIT_SINCE: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// daemon.py:4464 PING_NOTE_AT: token -> time of the last "no players within the limit" note
pub static PING_NOTE_AT: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:4466 ping_match_ms: the limit the formation gate, the host pick and join in progress judge at
pub fn ping_match_ms() -> i64 {
    if WORLD_ON.load(Ordering::Relaxed) {
        return WORLD.lock().lim;
    }
    if 0 < *PING_MATCH_MS {
        *PING_MATCH_MS
    } else {
        *PING_KICK_MS
    }
}

// py: daemon.py:4470 _pair_raw
pub fn pair_raw(a: &str, b: &str) -> Option<i64> {
    let p = PING.lock();
    let x = ping_med(p.get(&(a.to_string(), b.to_string())));
    let y = ping_med(p.get(&(b.to_string(), a.to_string())));
    [x, y].into_iter().flatten().max()
}

// py: daemon.py:4474 _corr_of
pub fn corr_of(t: &str, now: f64) -> i64 {
    let mut c = PING_CORR.lock();
    match c.get(t).copied() {
        None => 0,
        Some(v) if v.1 <= now => {
            c.remove(t);
            0
        }
        Some(v) => v.0,
    }
}

// py: daemon.py:4483 _measured_of
pub fn measured_of(a: &str, b: &str, now: f64) -> Option<i64> {
    let mut m = MEASURED_PAIR.lock();
    let v = m.get(&(a.to_string(), b.to_string())).copied()?;
    if now - v.1 > *PING_MEMORY_S {
        m.shift_remove(&(a.to_string(), b.to_string()));
        m.shift_remove(&(b.to_string(), a.to_string()));
        return None;
    }
    Some(v.0)
}

// py: daemon.py:4492 pair_ping: the ping the gates judge for a pair (raw=True: the estimate alone)
pub fn pair_ping_ex(a: &str, b: &str, raw: bool, now: Option<f64>) -> Option<i64> {
    let mut est = pair_raw(a, b);
    if raw || (MEASURED_PAIR.lock().is_empty() && PING_CORR.lock().is_empty()) {
        return est;
    }
    let now = now.unwrap_or_else(clock::now);
    if let Some(e) = est.as_mut() {
        *e += std::cmp::max(corr_of(a, now), corr_of(b, now));
    }
    if *PING_MEMORY_S > 0.0 {
        if let Some(m) = measured_of(a, b, now) {
            if est.is_none_or(|e| m > e) {
                est = Some(m);
            }
        }
    }
    est
}
pub fn pair_ping(a: &str, b: &str) -> Option<i64> {
    if WORLD_ON.load(Ordering::Relaxed) {
        return WORLD.lock().pings.get(&(a.to_string(), b.to_string())).copied();
    }
    pair_ping_ex(a, b, false, None)
}

// py: daemon.py:4509 ping_kick_learn: the ping removal took `p` off `host` at a measured `ms`
pub fn ping_kick_learn(mid: &str, p: &str, host: &str, ms: i64, now: Option<f64>) {
    let now = now.unwrap_or_else(clock::now);
    if *PING_MEMORY_S > 0.0 {
        {
            let mut m = MEASURED_PAIR.lock();
            m.insert((p.to_string(), host.to_string()), (ms, now));
            m.insert((host.to_string(), p.to_string()), (ms, now));
            if m.len() > 2000 {
                let mut gone: Vec<(String, String)> = m.iter().filter(|(_, v)| now - v.1 > *PING_MEMORY_S).map(|(k, _)| k.clone()).collect();
                if gone.is_empty() {
                    gone = m.keys().take(500).cloned().collect();
                }
                for k in gone {
                    m.shift_remove(&k);
                }
            }
        }
        dlog!(
            "ping-learn:",
            tag(p),
            "<>",
            tag(host),
            "measured",
            ms,
            format!("ms - the gates judge this pair at least that for {} s", *PING_MEMORY_S as i64)
        );
    }
    if *PING_CORR_S > 0.0 {
        let est = PING_FORMED.lock().get(&Some(mid.to_string())).and_then(|m| m.get(&pair_key(p, host)).copied());
        if let Some(est) = est {
            if ms - est > PING_CORR_EXCESS_MS {
                let ex_now: Vec<(i64, f64)> = {
                    let mut e = PING_EXCESS.lock();
                    let ex = e.entry(p.to_string()).or_default();
                    ex.retain(|x| now - x.1 <= *PING_CORR_S);
                    ex.push((ms - est, now));
                    ex.clone()
                };
                if ex_now.len() >= PING_CORR_KICKS {
                    let mut v: Vec<i64> = ex_now.iter().map(|e| e.0).collect();
                    v.sort();
                    let l = v.len();
                    let med: f64 = if l % 2 == 1 { v[l / 2] as f64 } else { (v[l / 2 - 1] + v[l / 2]) as f64 / 2.0 };
                    PING_CORR.lock().insert(p.to_string(), (med as i64, now + *PING_CORR_S));
                    dlog!(
                        "ping-learn:",
                        tag(p),
                        format!(
                            "estimate under-called {} removals (excess {} ms) - +{} ms on his estimated pings for {} s",
                            ex_now.len(),
                            py::list_int(&v),
                            med as i64,
                            *PING_CORR_S as i64
                        )
                    );
                } else {
                    dlog!(
                        "ping-learn:",
                        tag(p),
                        format!(
                            "measured {} ms, formation estimate {} ms (excess {} ms) - noted ({} of {} for a correction)",
                            ms,
                            est,
                            ms - est,
                            ex_now.len(),
                            PING_CORR_KICKS
                        )
                    );
                }
            }
        }
        let mut e = PING_EXCESS.lock();
        if e.len() > 2000 {
            let gone: Vec<String> = e.iter().filter(|(_, v)| v.is_empty() || now - v[v.len() - 1].1 > *PING_CORR_S).map(|(k, _)| k.clone()).collect();
            for k in gone {
                e.shift_remove(&k);
            }
        }
    }
}

/// daemon.py:4538 PERF: token -> latest performance report (op 0xCA)
pub static PERF: LazyLock<hostmig::PerfScores> = LazyLock::new(hostmig::PerfScores::new);

// py: daemon.py:4540 perf_op: op 0xCA -> reply 0xCB, or None for a bad payload
pub fn perf_op(pl: &[u8]) -> Option<Vec<u8>> {
    let (token, rep) = hostmig::parse_perf(pl)?;
    PERF.report(&token, &rep, BRIDGES.steamid(&token) as i64);
    if hostcap::mode() >= 1 {
        if let Err(e) = hostcap_note(&token, hostcap::parse_cap(pl)) {
            dlog!("HOSTCAP ERR", e); // the capacity history must never break the performance report
        }
    }
    if hostcap::mode() >= 1 {
        if let Err(e) = oneway_note(&token, hostcap::parse_oneway(pl)) {
            dlog!("ONEWAY ERR", e); // observation only, never breaks the report
        }
    }
    loss_note_from_pl(&token, pl); // loss_note(token, parse_loss(pl)): the loss record must never break the performance report
    if rep.n250 != 0 || rep.vram != 0 {
        dlog!(
            "PERF",
            tag(&token),
            "stalls>=250ms",
            rep.n250,
            ">=1s",
            rep.n1000,
            "worst",
            rep.worst,
            "ms avg frame",
            py::f(rep.avg_ms),
            "ms",
            if rep.vram != 0 { "vram-over" } else { "" },
            "-> host penalty",
            hostmig::perf_penalty(Some(&rep)),
            "ms"
        );
    }
    Some(msg(hostmig::OP_PERF_REPLY, b"ok=1"))
}

// py: daemon.py:4569 oneway_note: alarm of an ext that sent for seconds and received nothing (re/oneway-p2p-1019.md)
pub fn oneway_note(token: &str, ow: Option<hostcap::OneWay>) -> Result<(), String> {
    let Some(ow) = ow else { return Ok(()) };
    let (mid, _active) = MM.active_of(token);
    let snap = mid.as_ref().and_then(|m| HM.snapshot(m));
    let (mut role, mut slot, mut players): (&str, String, Vec<String>) = ("no match", "?".to_string(), Vec::new());
    if let Some(s) = snap {
        let idx = s.players.iter().position(|p| p == token);
        slot = idx.map(|i| i.to_string()).unwrap_or_else(|| "?".to_string());
        role = if idx == Some(s.host) { "HOST" } else { "client" };
        players = s.players;
    }
    let (n, hn) = HOSTCAP.oneway_note(token, mid.as_deref(), role == "HOST")?;
    let p2p = if players.is_empty() { None } else { match_p2p_get(&key_of(&players)) };
    dlog!(
        "ONEWAY",
        tag(token),
        role,
        "slot",
        slot,
        "match",
        mid.clone().unwrap_or_else(|| "None".to_string()),
        "net",
        match p2p {
            Some(true) => "steam-p2p",
            Some(false) => "server-relay",
            None => "?",
        },
        format!("- sent {} KB and got nothing for {} s ({} datagrams in {} s of the match);", ow.tx_kb, ow.quiet_s, ow.rx_pkts, ow.in_match_s),
        "alarms so far",
        n,
        "as host",
        hn,
        "others:",
        py::list_str(&players.iter().filter(|p| *p != token).map(|p| tag(p)).collect::<Vec<_>>())
    );
    evt!(
        "oneway",
        mid.unwrap_or_else(|| "None".to_string()),
        tag(token),
        role,
        "quiet-s",
        ow.quiet_s,
        "rx",
        ow.rx_pkts,
        "tx-kb",
        ow.tx_kb,
        "n",
        n
    );
    Ok(())
}

// py: daemon.py:4586 perf_penalty
pub fn perf_penalty(token: &str) -> i64 {
    if WORLD_ON.load(Ordering::Relaxed) {
        return WORLD.lock().perf.get(token).copied().unwrap_or(0);
    }
    PERF.penalty(token, BRIDGES.steamid(token) as i64)
}

/// daemon.py:4590 HOSTCAP: host upload capacity (T2R_HOST_CAP 0 off / 1 record + log only / 2 live in the start pick + re-pick)
pub static HOSTCAP: LazyLock<hostcap::HostCap> = LazyLock::new(|| hostcap::HostCap::new(Box::new(|| db::open_rw(&db::CFG.path).map_err(|e| e.to_string()))));

// py: daemon.py:4592 hostcap_note: optional capacity trailer of a 0xCA report (None = old ext)
pub fn hostcap_note(token: &str, cap: Option<hostcap::CapTrailer>) -> Result<(), String> {
    let Some(cap) = cap else { return Ok(()) };
    let (mid, _active) = MM.active_of(token);
    let Some(mid) = mid else { return Ok(()) };
    let Some(snap) = HM.snapshot(&mid) else { return Ok(()) };
    if snap.host >= snap.players.len() {
        return Ok(());
    }
    let host = snap.players[snap.host].clone();
    if token == host {
        HOSTCAP.host_report(token, snap.alive.len() as i64, cap.avg_kb, cap.peak_kb, Some(&mid))?;
        dlog!(
            "HOSTCAP",
            tag(token),
            "hosting",
            snap.alive.len(),
            "players: tx avg",
            cap.avg_kb,
            "KB/s peak",
            cap.peak_kb,
            "KB/s (need",
            hostcap::need_kb(snap.alive.len() as i64),
            ")"
        );
    } else if HOSTCAP.client_report(&mid, &host, token, cap.loss10)? {
        let last = HOSTCAP.get(&host)?.map(|r| r.last_kb.to_string()).unwrap_or_else(|| "None".to_string());
        dlog!(
            "HOSTCAP",
            tag(&host),
            "LAGGED as host: client",
            tag(token),
            "msg-7 loss",
            py::f(cap.loss10 as f64 / 10.0),
            "% twice in a row, host last tx",
            last,
            "KB/s"
        );
    }
    Ok(())
}

// ---------------------------------------------------------------- no-host list
pub static NO_HOST_PATH: LazyLock<std::path::PathBuf> = LazyLock::new(|| db_dir().join("no_host.txt"));
pub struct NoHost {
    pub mtime: Option<f64>,
    pub ids: HashSet<u64>,
    pub names: HashSet<String>,
}
pub static NO_HOST: LazyLock<Mutex<NoHost>> = LazyLock::new(|| Mutex::new(NoHost { mtime: None, ids: HashSet::new(), names: HashSet::new() }));

fn mtime_of(p: &std::path::Path) -> Option<f64> {
    let m = std::fs::metadata(p).ok()?.modified().ok()?;
    Some(m.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs_f64()).unwrap_or(0.0))
}

// py: daemon.py:4626 no_host: players that are never picked as host while anyone else is in the match
pub fn no_host(token: &str) -> bool {
    if WORLD_ON.load(Ordering::Relaxed) {
        return WORLD.lock().noh.contains(token);
    }
    let mt = mtime_of(&NO_HOST_PATH);
    {
        let mut nh = NO_HOST.lock();
        if mt != nh.mtime {
            let (mut ids, mut names) = (HashSet::new(), HashSet::new());
            if mt.is_some() {
                if let Ok(b) = std::fs::read(&*NO_HOST_PATH) {
                    for ln in String::from_utf8_lossy(&b).lines() {
                        let ln = py::strip(ln.split('#').next().unwrap_or(""));
                        if !ln.is_empty() && ln.chars().all(|c| c.is_ascii_digit()) {
                            if let Ok(v) = ln.parse::<u64>() {
                                ids.insert(v);
                            }
                        } else if !ln.is_empty() {
                            names.insert(ln.to_lowercase());
                        }
                    }
                }
            }
            nh.mtime = mt;
            dlog!("NO-HOST", format!("loaded {} steam id(s), {} name(s) from {}", ids.len(), names.len(), NO_HOST_PATH.display()));
            nh.ids = ids;
            nh.names = names;
        }
    }
    let sid = BRIDGES.steamid(token);
    let nm = get_name(token).to_lowercase();
    let nh = NO_HOST.lock();
    (sid != 0 && nh.ids.contains(&sid)) || (!nm.is_empty() && nh.names.contains(&nm))
}

// py: daemon.py:4648 host_allowed: tokens without the no-host players (all of them when nobody else is left)
pub fn host_allowed(tokens: &[String]) -> Vec<String> {
    let ok: Vec<String> = tokens.iter().filter(|t| !no_host(t)).cloned().collect();
    if ok.is_empty() {
        tokens.to_vec()
    } else {
        ok
    }
}

// py: daemon.py:4653 host_pool: host candidates by region
pub fn host_pool(tokens: &[String]) -> (Vec<String>, String) {
    let allowed = host_allowed(tokens);
    if allowed.len() < tokens.len() {
        dlog!("NO-HOST", "not host candidates:", tag_list(&tokens.iter().filter(|t| !allowed.contains(t)).cloned().collect::<Vec<_>>()));
    }
    let everyone: Vec<String> = tokens.to_vec(); // (no-host players still count for the majority region and the worst-case ping)
    hostmig::region_pool(
        &allowed,
        &|t: &str| region_of_token(t),
        &|t: &str| BRIDGES.steamid(t) as i64,
        &|a: &str, b: &str| pair_ping(a, b),
        None,
        Some(&everyone),
    )
}

fn swap0(out: &mut [String], i: usize) {
    out.swap(0, i);
}

fn first_allowed(pl: &[String]) -> Vec<String> {
    let a = host_allowed(pl);
    let mut out = pl.to_vec();
    if !out.is_empty() && !a.contains(&out[0]) {
        let i = out.iter().position(|x| *x == a[0]).unwrap_or(0);
        swap0(&mut out, i);
        dlog!("NO-HOST", tag(&pl[0]), "may not host -", tag(&out[0]), "hosts");
    }
    out
}

/// online players in no live match (post-match screens, menus): the usual join-in-progress arrivals
fn likely_joiners(pl: &[String]) -> Vec<String> {
    let busy: HashSet<String> = MM.lock.peek(|g| {
        let mut busy = HashSet::new();
        for (mid, mp) in g.matches.iter() {
            if g.ended.contains(mid) {
                continue;
            }
            let dep = g.departed.get(mid);
            busy.extend(mp.iter().filter(|t| !dep.is_some_and(|d| d.contains(*t))).cloned());
        }
        busy
    });
    PRES_TOKENS().into_iter().filter(|t| !pl.contains(t) && !busy.contains(t)).collect()
}

fn verdict_of(t: &str, n: i64) -> Result<(i64, String), String> {
    HOSTCAP.verdict(t, n)
}

/// the pings `host_key` is built from (None = one of them unknown: Python's max() of a None raises)
fn pings_to(c: &str, view: &[String]) -> Result<Vec<i64>, String> {
    view.iter().filter(|o| *o != c).map(|o| pair_ping(c, o).ok_or_else(|| "TypeError".to_string())).collect()
}

// no ping pick: capacity alone (T2R_HOST_CAP=2) - a player that lagged as host never hosts before one that did not
fn cap_first(pl: &[String]) -> Vec<String> {
    let mut out = first_allowed(pl);
    if hostcap::mode() < 2 || out.len() < 2 {
        return out;
    }
    // 11-14 (match 01660bc0): 2 players = one link, the ping cannot choose between them - but a third player often joins in progress seconds later.
    if out.len() == 2 && pair_ping(&out[0], &out[1]).is_some() {
        let r: Result<bool, String> = (|| {
            let a = host_allowed(&out);
            let extra: Vec<String> = likely_joiners(&out).into_iter().filter(|t| out.iter().all(|c| pair_ping(t, c).is_some())).collect();
            if !extra.is_empty() && a.len() == 2 {
                let mut view = out.clone();
                view.extend(extra.iter().cloned());
                let mut keyed: Vec<(String, (i64, i64))> = Vec::new();
                for c in &a {
                    keyed.push((c.clone(), hostmig::host_key(&pings_to(c, &view)?, perf_penalty(c))));
                }
                let (alt, lines) = hostcap::pick(&keyed, std::cmp::max(view.len() as i64, 3), &|t, n| verdict_of(t, n), None, &|t| tag(t))?;
                let alt = alt.ok_or_else(|| "tag(None)".to_string())?;
                dlog!("HOST-PICK", "2 players + likely joiners", tag_list(&extra), "|", lines.join("; "), "->", tag(&alt));
                if alt != out[0] {
                    let i = out.iter().position(|x| *x == alt).unwrap_or(0);
                    swap0(&mut out, i);
                }
                return Ok(true);
            }
            Ok(false)
        })();
        match r {
            Ok(true) => return out,
            Ok(false) => {}
            Err(e) => dlog!("HOST-PICK joiners ERR", e),
        }
    }
    let r: Result<(), String> = (|| {
        let n = std::cmp::max(out.len() as i64, 3); // a 2-player match grows by join in progress: judge for at least 3
        let a = host_allowed(&out);
        let mut vs: HashMap<String, (i64, String)> = HashMap::new();
        for t in &a {
            vs.insert(t.clone(), verdict_of(t, n)?);
        }
        let mut best: Option<&String> = None;
        for t in &a {
            let k = (vs[t].0, out.iter().position(|x| x == t).unwrap_or(0));
            if best.is_none_or(|b| k < (vs[b].0, out.iter().position(|x| x == b).unwrap_or(0))) {
                best = Some(t);
            }
        }
        let best = best.ok_or_else(|| "min() arg is an empty sequence".to_string())?.clone();
        let cur = vs.get(&out[0]).map(|v| v.0).unwrap_or(1);
        if best != out[0] && vs[&best].0 < cur {
            let cur_text = vs.get(&out[0]).map(|v| v.1.clone()).ok_or_else(|| "KeyError".to_string())?;
            dlog!("HOST-PICK", "cap:", tag(&out[0]), cur_text, "->", tag(&best), vs[&best].1.clone(), "hosts (no ping pick)");
            let i = out.iter().position(|x| *x == best).unwrap_or(0);
            swap0(&mut out, i);
        }
        Ok(())
    })();
    if let Err(e) = r {
        dlog!("HOST-PICK cap ERR", e);
    }
    out
}

// some pings unknown: capacity picks (cap_first), but never a host known to be over the match ping limit
fn limit_first(pl: &[String]) -> Vec<String> {
    // 11-25 (match 44039e46): one unknown pair among 8 made the pick skip every ping; the pings are the ones the formation gate judged
    let mut out = cap_first(pl);
    if !(*MM_PING_GATE && *PING_KICK_MS > 0) || out.len() < 3 {
        return out;
    }
    let r: Result<(), String> = (|| {
        let lim = ping_match_ms();
        let now = clock::now();
        let a = host_allowed(&out);
        // 0 = all known and within, 1 = the known ones within, 2 = somebody known over the limit
        let tier = |h: &str| -> i64 {
            let v: Vec<Option<i64>> = out.iter().filter(|o| *o != h).map(|o| _ping_gate(h, o, now)).collect();
            let k: Vec<i64> = v.iter().flatten().copied().collect();
            if k.iter().copied().max().is_some_and(|m| m > lim) {
                return 2;
            }
            if k.len() == v.len() {
                0
            } else {
                1
            }
        };
        let tiers: Vec<(String, i64)> = a.iter().map(|h| (h.clone(), tier(h))).collect();
        let bt = tiers.iter().map(|x| x.1).min().ok_or_else(|| "min() arg is an empty sequence".to_string())?;
        let cur_tier = tiers.iter().find(|x| x.0 == out[0]).map(|x| x.1).unwrap_or(2);
        if cur_tier == 2 && bt < 2 {
            let pool: Vec<String> = tiers.iter().filter(|x| x.1 == bt).map(|x| x.0.clone()).collect();
            let n = std::cmp::max(out.len() as i64, 3);
            let mut vs: HashMap<String, (i64, String)> = HashMap::new();
            for t in &pool {
                vs.insert(t.clone(), if hostcap::mode() >= 2 { verdict_of(t, n)? } else { (1, String::new()) });
            }
            let mut best: Option<&String> = None;
            for t in &pool {
                let k = (vs[t].0, out.iter().position(|x| x == t).unwrap_or(0));
                if best.is_none_or(|b| k < (vs[b].0, out.iter().position(|x| x == b).unwrap_or(0))) {
                    best = Some(t);
                }
            }
            let best = best.ok_or_else(|| "min() arg is an empty sequence".to_string())?.clone();
            dlog!(
                "HOST-PICK",
                format!("ping limit {} ms (some pings unknown):", lim),
                tag(&out[0]),
                "has a player over it ->",
                tag(&best),
                format!("hosts ({})", if bt == 0 { "all its pings known and within" } else { "its known pings within" }),
                vs[&best].1.clone()
            );
            let i = out.iter().position(|x| *x == best).unwrap_or(0);
            swap0(&mut out, i);
        }
        Ok(())
    })();
    if let Err(e) = r {
        dlog!("HOST-PICK limit ERR", e);
    }
    out
}

// py: daemon.py:4668 best_host_order: players with the best host moved to slot 0 (slot 0 = host); unchanged when any candidate's pings are unknown
pub fn best_host_order(players: &[String]) -> Vec<String> {
    if WORLD_ON.load(Ordering::Relaxed) {
        let w = WORLD.lock();
        let mut v: Vec<String> = w.bho.iter().filter(|t| players.contains(*t)).cloned().collect();
        v.extend(players.iter().filter(|t| !w.bho.contains(*t)).cloned());
        return v;
    }
    if env_set("T2R_NO_HOST_PICK") {
        return first_allowed(players);
    }
    if players.len() < 3 {
        return cap_first(players); // 2 players: both directions are the same link
    }
    for h in players {
        if players.iter().filter(|o| *o != h).any(|o| pair_ping(h, o).is_none()) {
            dlog!("HOST-PICK", "ping unknown, keeping queue order");
            return limit_first(players);
        }
    }
    let (mut pool, rinfo) = host_pool(players);
    dlog!("HOST-PICK", "region:", rinfo);
    if *MM_PING_GATE && *PING_KICK_MS > 0 {
        // (MM_PING_GATE) the ping limit beats the region rule and capacity: never a host that gets a player removed when another would not
        let lim = ping_match_ms();
        let fit: Vec<String> = host_allowed(players)
            .into_iter()
            .filter(|h| players.iter().filter(|o| *o != h).filter_map(|o| pair_ping(h, o)).max().unwrap_or(i64::MIN) <= lim)
            .collect();
        if !fit.is_empty() {
            let mut fpool: Vec<String> = pool.iter().filter(|h| fit.contains(h)).cloned().collect();
            if fpool.is_empty() {
                fpool = fit.clone();
            }
            if fpool != pool {
                dlog!("HOST-PICK", format!("ping limit {} ms: candidates", lim), tag_list(&fpool), "instead of", tag_list(&pool));
                pool = fpool;
            }
        }
    }
    let mut best: Option<((i64, i64), String)> = None;
    let mut cands: Vec<(String, i64, i64, Option<hostmig::PerfRep>, i64)> = Vec::new();
    for h in &pool {
        let pings: Vec<i64> = players.iter().filter(|o| *o != h).filter_map(|o| pair_ping(h, o)).collect();
        let pen = perf_penalty(h);
        let key = hostmig::host_key(&pings, pen);
        cands.push((tag(h), pings.iter().copied().max().unwrap_or(0), pings.iter().sum(), PERF.get(h, BRIDGES.steamid(h) as i64), pen));
        if best.as_ref().is_none_or(|b| key < b.0) {
            best = Some((key, h.clone()));
        }
    }
    let Some(best) = best else { return players.to_vec() };
    let mut h = best.1.clone();
    if hostcap::mode() >= 1 {
        let r: Result<(), String> = (|| {
            let mut keyed: Vec<(String, (i64, i64))> = Vec::new();
            for c in &pool {
                keyed.push((c.clone(), hostmig::host_key(&pings_to(c, players)?, perf_penalty(c))));
            }
            let (alt, lines) = hostcap::pick(&keyed, players.len() as i64, &|t, n| verdict_of(t, n), None, &|t| tag(t))?;
            let alt_t = alt.clone().ok_or_else(|| "tag(None)".to_string())?;
            dlog!(
                "HOST-PICK",
                "cap:",
                lines.join("; "),
                "->",
                if alt_t == h { format!("keeps {}", tag(&h)) } else { format!("would pick {} instead of {}", tag(&alt_t), tag(&h)) },
                if hostcap::mode() >= 2 { "(LIVE)" } else { "(log only)" }
            );
            if hostcap::mode() >= 2 && alt_t != h {
                h = alt_t;
            }
            Ok(())
        })();
        if let Err(e) = r {
            dlog!("HOST-PICK cap ERR", e);
        }
    }
    if hostcap::mode() >= 1 {
        // one-way inbound record (re/oneway-p2p-1019.md): LOG ONLY, the pick ignores it
        let r: Result<(), String> = (|| {
            let mut sus: Vec<(String, (i64, i64))> = Vec::new();
            for c in &pool {
                sus.push((tag(c), HOSTCAP.oneway_get(c, 30)?));
            }
            sus.retain(|(_, v)| v.0 != 0);
            if !sus.is_empty() {
                dlog!(
                    "HOST-PICK",
                    "one-way inbound alarms (30 d, alarms/as host):",
                    sus.iter().map(|(nm, v)| format!("{} {}/{}", nm, v.0, v.1)).collect::<Vec<_>>().join(", "),
                    "(log only)",
                    "- picked",
                    tag(&h)
                );
            }
            Ok(())
        })();
        if let Err(e) = r {
            dlog!("HOST-PICK oneway ERR", e);
        }
    }
    let mut out = players.to_vec();
    let i = out.iter().position(|x| *x == h).unwrap_or(0);
    swap0(&mut out, i);
    for (nm, w, tot, rep, pen) in &cands {
        dlog!(
            "HOST-PICK",
            "candidate",
            nm,
            "worst",
            w,
            "ms total",
            tot,
            "ms",
            match rep {
                Some(r) => format!("stalls>=250ms {} >=1s {} worst {} ms", r.n250, r.n1000, r.worst),
                None => "stalls unknown".to_string(),
            },
            "penalty",
            pen,
            "ms ->",
            w + pen
        );
    }
    dlog!("HOST-PICK", tag(&h), "worst", best.0 .0, "ms (incl. penalty) total", best.0 .1, "ms");
    out
}

// py: daemon.py:4803 push_peers: decide Steam P2P vs server relay for this match and tell the helpers
pub fn push_peers(players: &[String]) {
    let key = key_of(players);
    let has = |t: &str| BRIDGES.steamid(t) != 0 && BRIDGES.port(t) != 0 && BRIDGES.conn(t).is_some();
    let mut ok = crate::env_str("T2R_P2P", "1") != "0" && players.iter().all(|t| has(t));
    if ok && players.iter().map(|t| BRIDGES.steamid(t)).collect::<HashSet<u64>>().len() < players.len() {
        ok = false; // same Steam account twice: can't address
    }
    MATCH_P2P.lock().insert(key.clone(), ok);
    dlog!("MATCH-NET", if ok { "steam-p2p" } else { "server-relay" }, tag_list(players));
    let missing: Vec<String> = players.iter().filter(|t| !has(t)).map(|t| tag(t)).collect();
    if !missing.is_empty() {
        dlog!("MATCH-NET", "no Steam helper registered for", py::list_str(&missing));
    }
    if !ok {
        return;
    }
    let sid = match_sid(players);
    let mut table = vec![players.len() as u8];
    for (i, t) in players.iter().enumerate() {
        table.push(i as u8);
        table.extend_from_slice(&BRIDGES.steamid(t).to_le_bytes());
    }
    let mut failed: Vec<String> = Vec::new();
    for (i, tok) in players.iter().enumerate() {
        let mut p = sid.to_le_bytes().to_vec();
        p.push(i as u8);
        p.extend_from_slice(&table);
        let r = match BRIDGES.conn(tok) {
            Some(c) => c.sendall(&msg(0xF2, &p)).map_err(|e| crate::pyfmt::os_error_repr(&e)),
            None => Err("AttributeError(\"'NoneType' object has no attribute 'sendall'\")".to_string()),
        };
        if let Err(e) = r {
            dlog!("PUSH-PEER ERR", tag(tok), e);
            failed.push(tag(tok));
        }
    }
    if !failed.is_empty() {
        // 10-19 pre-mortem: a helper whose link died between the check above and the push never gets its slot table
        MATCH_P2P.lock().insert(key, false);
        dlog!("MATCH-NET", "0xF2 slot table not delivered to", py::list_str(&failed), "- server relay for this match");
    }
}

pub static PARTY_GAP_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_PARTY_GAP", 15.0));

// py: daemon.py:4849 party_lead_searching: a READY mate always goes with its leader's search
pub fn party_lead_searching(m: &str) -> bool {
    let (q, left) = MM.lock.peek(|g| (g.queue.contains_key(m), g.left.get(m).copied()));
    if q || MM.in_match(m) {
        return true;
    }
    left.is_some_and(|l| clock::now() - l.0 < *PARTY_GAP_S)
}

// ---------------------------------------------------------------- matchmaking helpers
/// daemon.py:4865 _JIP_HV: (host token, players) -> (time, (rank, text))
pub static JIP_HV: LazyLock<Mutex<HashMap<(String, i64), (f64, (i64, String))>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub const JIP_HV_S: f64 = 10.0;
/// `len(_JIP_HV)`
pub fn jip_hv_len() -> usize {
    JIP_HV.lock().len()
}
// py: daemon.py:4867 jip_host_verdict: HOSTCAP.verdict(host, n), cached JIP_HV_S seconds; (1, error) when the lookup fails
pub fn jip_host_verdict(host: &str, n: i64) -> (i64, String) {
    let now = clock::now();
    if let Some(e) = JIP_HV.lock().get(&(host.to_string(), n)) {
        if now - e.0 < JIP_HV_S {
            return e.1.clone();
        }
    }
    let v = match HOSTCAP.verdict(host, n) {
        Ok(v) => v,
        Err(ex) => (1, format!("lookup failed {}", py::repr_str(&ex))),
    };
    let mut c = JIP_HV.lock();
    if c.len() > 2000 {
        c.clear();
    }
    c.insert((host.to_string(), n), (now, v.clone()));
    v
}
