//! K4 jip: join in progress (daemon.py:5700-5880 MatchMaker half, 6890-7240 functions), matchmaker_loop, the host re-pick glue and gunlimit_op.
use super::*;
use crate::hostcap;
use crate::hostmig::{self, CapOverride, CheckInfo, HmOpts, HostMigration, HostRepick, Repick};
use crate::py;
use crate::{clock, gunlimit, interrogation, social};
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;

pub type Score = (i64, i64, i64, f64);

fn score_gt(a: &Score, b: &Score) -> bool {
    if a.0 != b.0 {
        return a.0 > b.0;
    }
    if a.1 != b.1 {
        return a.1 > b.1;
    }
    if a.2 != b.2 {
        return a.2 > b.2;
    }
    a.3 > b.3
}

// JIP_FIRST offers running matches to many more searchers each pass (lobby members too): the team-choice notes of jip_pick
// would repeat for every searcher x match every second. One note per (match, searcher) per 60 s; without JIP_FIRST every note as before.
static JIP_NOTE: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
fn jip_note_ok(mid: &str, token: &str, now: f64) -> bool {
    if !*JIP_FIRST {
        return true;
    }
    let mut m = JIP_NOTE.lock();
    if m.len() > 4000 {
        m.retain(|_, t| now - *t < 60.0);
    }
    let k = (mid.to_string(), token.to_string());
    if m.get(&k).is_some_and(|t| now - *t < 60.0) {
        return false;
    }
    m.insert(k, now);
    true
}

fn name_or_player(t: &str) -> String {
    let n = get_name(t);
    if n.is_empty() {
        "PLAYER".to_string()
    } else {
        n
    }
}

impl MMState {
    // py: daemon.py:5733 _jip_mates
    pub fn jip_mates(&self, token: &str) -> Vec<String> {
        if PARTIES.pid(token).is_some_and(|p| !p.is_empty()) {
            PARTIES.members(token).into_iter().filter(|m| m != token).collect()
        } else {
            Vec::new()
        }
    }

    // py: daemon.py:5740 _jip_pick
    pub fn jip_pick(&mut self, token: &str, why_not: &mut Vec<String>, min_left: Option<f64>, need: i64, only_mid: Option<&str>, team: Option<i64>) -> Option<(Score, String, usize)> {
        let now = clock::now();
        let min_left = min_left.unwrap_or(*JIP_MIN_LEFT);
        let mates = self.jip_mates(token);
        let mut best: Option<(Score, String, usize)> = None;
        let mids: Vec<String> = self.matches.keys().cloned().collect();
        let ld = PARTIES.leader(token);
        for mid in mids {
            if only_mid.is_some_and(|o| o != mid) {
                continue;
            }
            if !self.started.contains(&mid) || self.ended.contains(&mid) || !self.launched.contains(&mid) {
                continue;
            }
            let pl: Vec<String> = self.matches[&mid].clone();
            let tpos = pl.iter().position(|x| x == token);
            let in_pl = tpos.is_some();
            if !in_pl && !MODEQ.fits(token, &mid, ld.as_deref()) {
                why_not.push(format!("{}: another game mode", mid));
                continue;
            }
            if !in_pl && !p1_fits(token, &mid, ld.as_deref()) {
                why_not.push(format!("{}: other PART I MAPS preference", mid));
                continue;
            }
            if !in_pl && !region_lock_fits(&[std::iter::once(token.to_string()).chain(mates.iter().filter(|m| m.as_str() != token).cloned()).collect()], &pl.iter().filter(|t| !self.departed.get(&mid).is_some_and(|d| d.contains(*t))).map(|t| vec![t.clone()]).collect::<Vec<_>>(), true) {
                why_not.push(format!("{}: region preference", mid));
                continue;
            }
            if !in_pl && !PRIV.jip_ok(token, &mid) {
                why_not.push(format!("{}: private match / private lobby", mid));
                continue;
            }
            let dep: HashSet<String> = self.departed.get(&mid).cloned().unwrap_or_default();
            let active: Vec<String> = pl.iter().filter(|t| !dep.contains(*t)).cloned().collect();
            let age = now - self.started_t.get(&mid).copied().unwrap_or(0.0);
            if !in_pl && *JIP_MIN_AGE > 0.0 && self.started_t.contains_key(&mid) && age < *JIP_MIN_AGE {
                why_not.push(format!("{}: only {} s past GAME-START (< {} s)", mid, py::fixed(age, 0), py::fixed(*JIP_MIN_AGE, 0)));
                continue;
            }
            let mut mv = self.move_team.get(token).cloned();
            if let Some(m) = &mv {
                if m.2 < now || !self.matches.contains_key(&m.0) || self.ended.contains(&m.0) {
                    self.move_team.shift_remove(token);
                    mv = None;
                }
            }
            if mv.as_ref().is_some_and(|m| m.0 != mid) {
                continue;
            }
            let mut back = in_pl && mv.is_none();
            if back {
                if let Some(tm) = team {
                    if team_in(&mid, tpos.unwrap_or(0) as i64, Some(token)) != tm {
                        back = false;
                    }
                }
            }
            let hmi = if HM.enabled { HM.host_info(&mid) } else { None };
            let hslot = hmi.as_ref().map(|h| h.host).unwrap_or(0);
            if let Some(h) = &hmi {
                if h.pending || h.given_up {
                    why_not.push(format!("{}: host migration in progress", mid));
                    continue;
                }
                if h.alone {
                    why_not.push(format!("{}: host moved to the last player left (the match ended on its side)", mid));
                    continue;
                }
            }
            let rejoin_host = back && hslot != 0 && tpos == Some(0);
            if back && pl.get(hslot).is_some_and(|x| x == token) {
                why_not.push(format!("{}: was its host", mid));
                continue;
            }
            if rejoin_host {
                back = false;
            }
            if INT.has_match(&mid) && crate::env_str("T2R_JIP_INT", "1") != "1" {
                why_not.push(format!("{}: Interrogation match", mid));
                continue;
            }
            if !back && active.len() <= 1 && now - self.formed.get(&mid).copied().unwrap_or(0.0) < countdown_seconds() + *JIP_MIN_AGE_REMNANT {
                why_not.push(format!("{}: only {} player left and the match is young (a load-timeout remnant)", mid, active.len()));
                continue;
            }
            let Some(host) = pl.get(hslot).cloned() else { continue };
            if dep.contains(&host) {
                why_not.push(format!("{}: host gone", mid));
                continue;
            }
            if let Some(ur) = JIP_UNREACH.lock().get(&(token.to_string(), mid.clone())).cloned() {
                if now - ur.0 < *JIP_UNREACH_S && (ur.1.is_none() || ur.1.as_deref() == Some(host.as_str())) {
                    why_not.push(format!("{}: could not reach its host {} s ago (not sent there again for {} s while that host stays)", mid, py::fixed(now - ur.0, 0), py::fixed(*JIP_UNREACH_S, 0)));
                    continue;
                }
            }
            let joining: HashSet<String> = self.joining.keys().cloned().collect();
            let lim = if *MM_PING_GATE { ping_match_ms() } else { *PING_KICK_MS };
            if !back || PING_KICKED.lock().contains(&(token.to_string(), mid.clone())) {
                if let Some(pms) = jip_ping_block(token, &mid, &host, &active, &joining, now) {
                    why_not.push(format!("{}: {} ms to the host, over the {} ms ping limit (it would be removed)", mid, pms, lim));
                    continue;
                }
            }
            if need > 1 && only_mid.is_none() {
                let bad: Vec<(String, i64)> = mates
                    .iter()
                    .filter(|m| self.queue.contains_key(*m) && !pl.contains(*m))
                    .filter_map(|m| jip_ping_block(m, &mid, &host, &active, &joining, now).map(|b| (m.clone(), b)))
                    .collect();
                if let Some((m, b)) = bad.first() {
                    why_not.push(format!("{}: party mate {} {} ms to the host, over the {} ms ping limit (the party stays together)", mid, tag(m), b, lim));
                    continue;
                }
            }
            let ck = self.clock.get(&mid).copied();
            let ck = match ck {
                Some(c) if now - c.3 <= *JIP_CLOCK_TTL => c,
                other => {
                    why_not.push(format!(
                        "{}: no fresh clock report ({}; the players' ext stopped reporting or is an old build)",
                        mid,
                        match other {
                            None => "never reported".to_string(),
                            Some(c) => format!("newest {} s old", py::fixed(now - c.3, 0)),
                        }
                    ));
                    continue;
                }
            };
            let left = ck.0 - (now - ck.3);
            if ck.2 || ck.1 <= 0.0 || left < min_left {
                why_not.push(format!("{}: {} s left / ended={}", mid, py::fixed(left, 0), py::b(ck.2)));
                continue;
            }
            let lv = self.lives.get(&mid).copied();
            if !INT.has_match(&mid) {
                if let Some(l) = lv {
                    if now - l.2 <= 3.0 * *JIP_CLOCK_TTL && l.0.min(l.1) <= 0 {
                        why_not.push(format!("{}: overtime (team lives {} / {})", mid, l.0, l.1));
                        continue;
                    }
                }
            }
            // Join in progress only while the match is less than JIP_MAX_PROG done (clock, lives used, Interrogation lead)
            if !back && *JIP_MAX_PROG > 0.0 {
                let (p, _) = join_progress(Some(ck), lv, 0, now);
                let p = p.max(INT.lead_frac(&mid).unwrap_or(0.0));
                if p >= *JIP_MAX_PROG {
                    why_not.push(format!("{}: {} % done (join in progress only below {} %)", mid, py::fixed(100.0 * p, 0), py::fixed(100.0 * *JIP_MAX_PROG, 0)));
                    continue;
                }
            }
            if !back && *LOBBY_KEEP && *REQUEUE {
                let kept: Vec<String> = self.rq.kept(&mid, None).into_iter().filter(|k| k != token && !pl.contains(k)).collect();
                if !kept.is_empty() && active.len() as i64 + need + kept.len() as i64 > *MATCH_SIZE {
                    why_not.push(format!("{}: {} seat(s) kept for its lobby members still on the post-match screens", mid, kept.len()));
                    continue;
                }
            }
            if active.len() as i64 + if back { 1 } else { need } > *MATCH_SIZE {
                continue;
            }
            if !back && *JIP_HOST_CAP && hostcap::mode() >= 2 {
                let (rk, txt) = jip_host_verdict(&host, active.len() as i64 + need);
                if rk >= 2 {
                    why_not.push(format!("{}: host {} cannot carry {} players ({})", mid, tag(&host), active.len() as i64 + need, txt));
                    continue;
                }
            }
            let key = key_of(&pl);
            if *RELAY_MATCH_MAX > 0 && !back && match_p2p_get(&key) == Some(false) && has_helper(token) && active.len() as i64 + need > *RELAY_MATCH_MAX {
                why_not.push(format!(
                    "{}: server-relay match with {} players (limit {} for players with a Steam helper: they play Steam P2P)",
                    mid,
                    active.len(),
                    *RELAY_MATCH_MAX
                ));
                continue;
            }
            let p2p = match_p2p_get(&key) == Some(true);
            if p2p {
                let sid = BRIDGES.steamid(token);
                let usable = sid != 0 && BRIDGES.port(token) != 0 && BRIDGES.conn(token).is_some() && !active.iter().any(|t| BRIDGES.steamid(t) == sid);
                if !usable {
                    why_not.push(format!("{}: Steam P2P match, joiner has no usable helper", mid));
                    continue;
                }
            }
            let Some(cur_map) = match_map_get(&key) else { continue };
            let mut cnt = [0i64; 2];
            for (i, t) in pl.iter().enumerate() {
                if !dep.contains(t) {
                    cnt[team_in(&mid, i as i64, Some(t)).clamp(0, 1) as usize] += 1;
                }
            }
            let reuse_s = *JIP_REUSE_S;
            let dep_t = |t: &str, s: &MMState| s.departed_t.get(t).copied().unwrap_or(0.0);
            if back && mates.is_empty() {
                let own = team_in(&mid, tpos.unwrap_or(0) as i64, Some(token)).clamp(0, 1);
                let free_other = pl.iter().enumerate().any(|(i, t)| i != 0 && (i as i64) % 2 != own && dep.contains(t) && now - dep_t(t, self) >= reuse_s)
                    || (pl.len()..*ENGINE_SLOTS as usize).any(|i| (i as i64) % 2 != own);
                if cnt[own as usize] > cnt[1 - own as usize] && free_other && now - dep_t(token, self) >= reuse_s {
                    back = false;
                }
            }
            if back && mv.is_none() {
                let teams_of: IndexMap<String, i64> =
                    pl.iter().enumerate().filter(|(_, t)| !dep.contains(*t) && *t != token).map(|(i, t)| (t.clone(), team_in(&mid, i as i64, Some(t)))).collect();
                let mut keys: Vec<String> = teams_of.keys().cloned().collect();
                keys.push(token.to_string());
                let (rt, rnote) = rule_team_for(token, &teams_of, &rule_groups(&keys));
                if let Some(rt) = rt {
                    if rt != team_in(&mid, tpos.unwrap_or(0) as i64, Some(token)) && rnote.starts_with("together") {
                        back = false;
                    }
                }
            }
            if back {
                if now - dep_t(token, self) < reuse_s {
                    why_not.push(format!("{}: left < {} s ago", mid, py::fixed(reuse_s, 0)));
                    continue;
                }
                let mut v = active.clone();
                v.push(token.to_string());
                if !map_pool(&v, Some(&[token.to_string()])).contains(&cur_map) {
                    why_not.push(format!("{}: map {} not in the pool for this joiner", mid, map_name(cur_map)));
                    continue;
                }
                let score: Score = (3, 0, 0, ck.0);
                if best.as_ref().is_none_or(|b| score_gt(&score, &b.0)) {
                    best = Some((score, mid.clone(), tpos.unwrap_or(0)));
                }
                continue;
            }
            if rejoin_host && now - dep_t(token, self) < reuse_s {
                why_not.push(format!("{}: left < {} s ago", mid, py::fixed(reuse_s, 0)));
                continue;
            }
            let free: Vec<usize> = pl.iter().enumerate().filter(|(i, t)| *i != 0 && dep.contains(*t) && now - dep_t(t, self) >= reuse_s).map(|(i, _)| i).collect();
            let anchor: BTreeSet<i64> = pl.iter().enumerate().filter(|(_, t)| mates.contains(*t) && !dep.contains(*t)).map(|(i, t)| team_in(&mid, i as i64, Some(t))).collect();
            let lim2: i64 = if need == 1 { 1 } else { 2 };
            let mut want: Option<i64> = None;
            let mut forced = false;
            if let Some(m) = &mv {
                want = Some(m.1);
                forced = true;
            } else if let Some(t) = team {
                want = Some(t);
                forced = true;
            } else if !anchor.is_empty() {
                let a = anchor.iter().copied().min_by_key(|x| cnt[(*x).clamp(0, 1) as usize]).unwrap_or(0);
                if cnt[a.clamp(0, 1) as usize] + need - cnt[1 - a.clamp(0, 1) as usize] <= lim2 {
                    want = Some(a);
                    forced = true;
                }
            }
            if want.is_none() && mv.is_none() && need == 1 {
                let teams_of: IndexMap<String, i64> = pl.iter().enumerate().filter(|(_, t)| !dep.contains(*t)).map(|(i, t)| (t.clone(), team_in(&mid, i as i64, Some(t)))).collect();
                let mut keys: Vec<String> = teams_of.keys().cloned().collect();
                keys.push(token.to_string());
                let (rt, rnote) = rule_team_for(token, &teams_of, &rule_groups(&keys));
                if let Some(rt) = rt {
                    let rtu = rt.clamp(0, 1) as usize;
                    let has_slot = free.iter().any(|i| (*i as i64) % 2 == rt) || (pl.len() < *ENGINE_SLOTS as usize && (pl.len() as i64) % 2 == rt);
                    if (cnt[rtu] + 1 - cnt[1 - rtu] <= lim2 || rnote.starts_with("together")) && has_slot {
                        want = Some(rt);
                        forced = false;
                        if jip_note_ok(&mid, token, now) { dlog!("JIP", mid, format!("joiner {} -> team {} by admin rule ({})", tag(token), rt, rnote)); }
                    } else {
                        let why = if !has_slot { "no free slot".to_string() } else { format!("it would be {} v {}", cnt[0] + (rt == 0) as i64, cnt[1] + (rt == 1) as i64) };
                        if jip_note_ok(&mid, token, now) { dlog!("JIP", mid, format!("admin rule for {} wants team {} but {} - balance instead", tag(token), rt, why)); }
                    }
                } else if !rnote.is_empty() {
                    if jip_note_ok(&mid, token, now) { dlog!("JIP", mid, format!("admin rules for {} cannot be met ({}) - balance instead", tag(token), rnote)); }
                }
            }
            if want.is_none() && mv.is_none() {
                if let Some(l) = lv {
                    if now - l.2 < 30.0 && l.0 != l.1 {
                        let lose: i64 = if l.0 < l.1 { 0 } else { 1 };
                        let has_slot = free.iter().any(|i| (*i as i64) % 2 == lose)
                            || (pl.len() < *ENGINE_SLOTS as usize && ((pl.len() as i64) % 2 == lose || need > 1));
                        if (l.0 - l.1).abs() >= 3 && cnt[lose as usize] + need - cnt[1 - lose as usize] <= std::cmp::max(1, need) && has_slot {
                            want = Some(lose);
                            forced = need > 1;
                            if jip_note_ok(&mid, token, now) { dlog!("JIP", mid, format!("joiner -> losing team {} (lives {} v {}, players {} v {})", lose, l.0, l.1, cnt[0], cnt[1])); }
                        }
                    }
                }
            }
            if want.is_none() && need == 1 && cnt[0] == cnt[1] && crate::env_str("T2R_TEAM_BALANCE", "1") != "0" {
                let tin: Vec<(i64, String)> = pl.iter().enumerate().filter(|(_, t)| !dep.contains(*t)).map(|(i, t)| (team_in(&mid, i as i64, Some(t)), t.clone())).collect();
                let mut all: Vec<String> = tin.iter().map(|x| x.1.clone()).collect();
                all.push(token.to_string());
                let sk = player_skill(&all);
                let mut tot = [0.0f64; 2];
                for side in 0..2usize {
                    tot[side] = py_sum(&tin.iter().filter(|(k, _)| *k == side as i64).map(|(_, t)| sk[t]).collect::<Vec<_>>());
                }
                if (tot[0] - tot[1]).abs() > 1e-6 {
                    let weak: i64 = if tot[0] < tot[1] { 0 } else { 1 };
                    if free.iter().any(|i| (*i as i64) % 2 == weak) || (pl.len() < *ENGINE_SLOTS as usize && (pl.len() as i64) % 2 == weak) {
                        want = Some(weak);
                        if jip_note_ok(&mid, token, now) {
                            dlog!("JIP", mid, format!("joiner {} ({}) -> weaker team {} (rating {} v {}, players {} v {})", tag(token), py::fixed(sk[token], 2), weak,
                                py::fixed(tot[0], 2), py::fixed(tot[1], 2), cnt[0], cnt[1]));
                        }
                    }
                }
            }
            if want.is_none() {
                want = if cnt[0] < cnt[1] { Some(0) } else if cnt[1] < cnt[0] { Some(1) } else { None };
                if need > 1 {
                    forced = true;
                    if want.is_none() {
                        let mut room = [0i64; 2];
                        for k in 0..2usize {
                            room[k] = free.iter().filter(|i| *i % 2 == k).count() as i64 + (pl.len()..*ENGINE_SLOTS as usize).filter(|i| i % 2 == k).count() as i64;
                        }
                        want = Some(if room[0] >= room[1] { 0 } else { 1 });
                    }
                    let w = want.unwrap_or(0).clamp(0, 1) as usize;
                    if cnt[w] + need - cnt[1 - w] > std::cmp::max(1, need) {
                        why_not.push(format!("{}: a party of {} would unbalance the teams ({} v {})", mid, need, cnt[0], cnt[1]));
                        continue;
                    }
                }
            }
            let reuse: Vec<usize> = free.iter().copied().filter(|i| want.is_none() || Some(*i as i64 % 2) == want).collect();
            let swap_set = TEAM_SWAP.lock().get(&mid).is_some_and(|m| !m.is_empty());
            let new_idx: Vec<usize> = if forced || swap_set {
                (pl.len()..*ENGINE_SLOTS as usize).filter(|i| want.is_none() || Some(*i as i64 % 2) == want).collect()
            } else if pl.len() < *ENGINE_SLOTS as usize && (want.is_none() || Some(pl.len() as i64 % 2) == want) {
                vec![pl.len()]
            } else {
                Vec::new()
            };
            if ((reuse.len() + new_idx.len()) as i64) < need || (reuse.is_empty() && new_idx.is_empty()) {
                why_not.push(format!("{}: no free slot on the smaller team ({} v {})", mid, cnt[0], cnt[1]));
                continue;
            }
            let holes_free: Vec<usize> = reuse.iter().copied().filter(|i| pl[*i] == hostmig::placeholder(*i)).collect();
            let slot = if let Some(h) = holes_free.first() {
                *h
            } else if !new_idx.is_empty() && new_idx[0] == pl.len() {
                new_idx[0]
            } else if !reuse.is_empty() {
                reuse[0]
            } else {
                new_idx[0]
            };
            let mut new_pl = pl.clone();
            if rejoin_host {
                new_pl[0] = hostmig::placeholder(0);
            }
            let mut holes: Vec<String> = Vec::new();
            while new_pl.len() < slot {
                let ph = hostmig::placeholder(new_pl.len());
                holes.push(ph.clone());
                new_pl.push(ph);
            }
            if slot == new_pl.len() {
                new_pl.push(token.to_string());
            } else {
                new_pl[slot] = token.to_string();
            }
            let new_active: Vec<String> = new_pl.iter().filter(|t| (!dep.contains(*t) && !holes.contains(*t)) || t.as_str() == token).cloned().collect();
            if !map_pool(&new_active, Some(&[token.to_string()])).contains(&cur_map) {
                why_not.push(format!("{}: map {} not in the pool for {} players / this joiner", mid, map_name(cur_map), new_active.len()));
                continue;
            }
            let tier: i64 = if rejoin_host { 3 } else if !anchor.is_empty() && forced { 2 } else { 1 };
            let mut same = 0i64;
            if tier == 1 && *MM_REGION_ON {
                let jr = mm_region(token);
                same = if jr.as_deref().is_some_and(|r| hostmig::MM_REGIONS.contains(&r)) && jr == mm_region(&host) { 1 } else { 0 };
            }
            let mut score: Score = (tier, same, if tier == 1 { active.len() as i64 } else { 0 }, ck.0);
            if tier == 1 && *JIP_FIRST {
                // JIP_FIRST: short-handed team first (a 2 v 1 before a 3 v 3), then the lowest ping band to the host (a joiner far from the host
                // lags every fight; same region counts inside the band), then the fuller match (finish a 4 v 4), then the time left
                let tgt = team_in(&mid, slot as i64, None).clamp(0, 1) as usize;
                let gap = (cnt[1 - tgt] - cnt[tgt]).clamp(0, 3);
                let band = _ping_gate(token, &host, now).map(|p| p / *JIP_PING_BAND).unwrap_or(2).min(9);
                score = (tier, gap, (9 - band) * 100 + same * 10 + active.len() as i64, ck.0);
            }
            if best.as_ref().is_none_or(|b| score_gt(&score, &b.0)) {
                best = Some((score, mid.clone(), slot));
            }
        }
        best
    }
}

impl MatchMaker {
    // py: daemon.py:5700 jip_clock
    pub fn jip_clock(&self, token: &str, rem: f64, dur: f64, ended: bool) -> Option<String> {
        let mut g = self.lock.lock();
        let mid = g.mid_locked(token)?;
        let now = clock::now();
        g.clock.insert(mid.clone(), (rem, dur, ended, now));
        g.polled.entry(mid.clone()).or_default().insert(token.to_string(), now);
        Some(mid)
    }

    // py: daemon.py:5713 jip_ready
    pub fn jip_ready(&self) -> Vec<String> {
        let now = clock::now();
        let ready = {
            let mut g = self.lock.lock();
            let mut ready: Vec<String> = g
                .queue
                .keys()
                .filter(|t| g.settle.get(*t).copied().unwrap_or(0.0) <= now && now - g.seen.get(*t).copied().unwrap_or(0.0) <= *QUEUE_FRESH)
                .cloned()
                .collect();
            ready.sort_by(|a, b| g.queue[a].partial_cmp(&g.queue[b]).unwrap_or(std::cmp::Ordering::Equal));
            let mut ready = g.rq_filter(ready, now, false);
            if book_on() {
                // JIP_FIRST: a member of a lobby still below the fill floor is offered a running match too (the lobby would start a
                // small match or keep waiting; a short-handed running match needs it now). A lobby at the floor starts its own match.
                ready.retain(|t| match g.book.lobby_of(t) {
                    None => true,
                    Some(lid) => g
                        .book
                        .get(lid)
                        .map(|l| book_single_unit(&l.members) || (*JIP_FIRST && (l.members.len() as i64) < *FILL_FLOOR))
                        .unwrap_or(true),
                });
            }
            ready
        };
        social::held_parties(&ready, &PARTIES).0
    }

    // py: daemon.py:5724 jip_acked
    pub fn jip_acked(&self, token: &str) -> bool {
        let g = self.lock.lock();
        let Some((mid, t0)) = g.joining.get(token).cloned() else { return true };
        if clock::now() - t0 > *JIP_ACK_MAX {
            return true;
        }
        let empty = Vec::new();
        let pl = g.matches.get(&mid).unwrap_or(&empty);
        let dep = g.departed.get(&mid);
        let seen = g.polled.get(&mid);
        pl.iter().filter(|p| p.as_str() != token && !dep.is_some_and(|d| d.contains(*p)) && !g.joining.contains_key(*p)).all(|p| {
            let s = seen.and_then(|m| m.get(p)).copied().unwrap_or(0.0);
            s >= t0 || (s != 0.0 && t0 - s > *JIP_ACK_STALE)
        })
    }

    // py: daemon.py:5733 jip_done
    pub fn jip_done(&self, token: &str) -> Option<(String, f64)> {
        self.lock.lock().joining.shift_remove(token)
    }

    // py: daemon.py:5737 jip_age
    pub fn jip_age(&self, token: &str) -> f64 {
        match self.lock.lock().joining.get(token) {
            Some(j) => clock::now() - j.1,
            None => 0.0,
        }
    }

    // py: daemon.py:5741 jip_joiners
    pub fn jip_joiners(&self, mid: &str) -> Vec<String> {
        self.lock.lock().joining.iter().filter(|(_, (m, _))| m == mid).map(|(t, _)| t.clone()).collect()
    }

    // py: daemon.py:5877 jip_can_join
    pub fn jip_can_join(&self, token: &str, min_left: Option<f64>, need: i64) -> bool {
        let mut g = self.lock.lock();
        if !g.queue.contains_key(token) || g.in_match_locked(token) {
            return false;
        }
        g.jip_pick(token, &mut Vec::new(), min_left, need, None, None).is_some()
    }

    // py: daemon.py:5885 jip_join: (mid, slot, players, old_key)
    pub fn jip_join(&self, token: &str, why_not: &mut Vec<String>, need: i64, only_mid: Option<&str>, team: Option<i64>) -> Option<(String, i64, Vec<String>, Key)> {
        let now = clock::now();
        let mut gd = self.lock.lock();
        let g = &mut *gd;
        if !g.queue.contains_key(token) || g.in_match_locked(token) {
            return None;
        }
        let (_, mid, slot) = g.jip_pick(token, why_not, None, need, only_mid, team)?;
        let mut pl: Vec<String> = g.matches[&mid].clone();
        let old_key = key_of(&pl);
        let old_sid = match_sid(&pl);
        let mut dep: HashSet<String> = g.departed.remove(&mid).unwrap_or_default();
        let mv = g.move_team.shift_remove(token);
        let tpos = pl.iter().position(|x| x == token);
        let moved = mv.is_some() || tpos.is_some_and(|p| p != slot);
        if tpos.is_some() {
            JIP_BACK.lock().insert(token.to_string(), (mid.clone(), now));
        } else {
            JIP_BACK.lock().remove(token);
        }
        if moved && tpos.is_some_and(|p| p != slot) {
            let oi = tpos.unwrap_or(0);
            if oi != 0 {
                let ph = hostmig::placeholder(oi);
                pl[oi] = ph.clone();
                dep.insert(ph.clone());
                let t0 = g.departed_t.get(token).copied().unwrap_or(now);
                g.departed_t.insert(ph, t0);
                dep.remove(token);
            }
            {
                let mut ts = TEAM_SWAP.lock();
                if let Some(m) = ts.get_mut(&mid) {
                    m.remove(token);
                }
            }
            dlog!(format!("TEAMS move{}: {} re-enters {} in slot {} (team {}), was slot {}", if mv.is_some() { "" } else { " (balance)" }, tag(token), mid, slot, slot % 2, oi));
        }
        if !pl.is_empty() && pl[0] == token && slot != 0 {
            let ph = hostmig::placeholder(0);
            dep.insert(ph.clone());
            let t0 = g.departed_t.get(token).copied().unwrap_or(0.0);
            g.departed_t.insert(ph.clone(), t0);
            pl[0] = ph;
            dep.remove(token);
        }
        while pl.len() < slot {
            let ph = hostmig::placeholder(pl.len());
            pl.push(ph.clone());
            dep.insert(ph.clone());
            g.departed_t.insert(ph, 0.0);
        }
        let tnow = pl.iter().position(|x| x == token);
        let team = if tnow == Some(slot) { team_in(&mid, slot as i64, Some(token)) } else { slot as i64 % 2 };
        let n_ally = pl.iter().enumerate().filter(|(i, t)| !dep.contains(*t) && team_in(&mid, *i as i64, Some(t)) == team && t.as_str() != token).count() as i64;
        let n_enemy = pl.iter().enumerate().filter(|(i, t)| !dep.contains(*t) && team_in(&mid, *i as i64, Some(t)) != team).count() as i64;
        let lv = g.lives.get(&mid).copied();
        let ck = g.clock.get(&mid).copied();
        let mut down: i64 = 0;
        if let Some(c) = ck {
            if !c.2 {
                let elapsed = c.1 - (c.0 - (now - c.3));
                down = (elapsed / 60.0 - 1.0 + 0.5).floor() as i64;
            }
        }
        down += match n_enemy - n_ally {
            1 => 2,
            2 => 4,
            3 => 6,
            _ => 0,
        };
        let bonus = if down > 0 { 100 * down } else { 0 };
        if slot == pl.len() {
            pl.push(token.to_string());
        } else {
            let old = pl[slot].clone();
            pl[slot] = token.to_string();
            dep.remove(&old);
            dep.remove(token);
        }
        let new_key = key_of(&pl);
        g.matches.insert(mid.clone(), pl.clone());
        g.departed.insert(mid.clone(), dep);
        if INT.has_match(&mid) {
            INT.roster(&mid, &pl, &|i| team_in(&mid, i as i64, pl.get(i).map(|s| s.as_str())));
        }
        sid_keep_set(&new_key, old_sid);
        rekey_match_dicts(&old_key, &new_key);
        let since = g.queue.shift_remove(token).unwrap_or(now);
        g.qtime.entry(mid.clone()).or_default().insert(token.to_string(), since);
        if book_on() {
            g.book.remove(token, &format!("joined the running match {}", mid), Some(now), 0.0);
        }
        g.joining.insert(token.to_string(), (mid.clone(), now));
        if let Some(p) = g.polled.get_mut(&mid) {
            p.remove(token);
        }
        g.departed_t.remove(token);
        let lives_ok = lv.is_some_and(|l| now - l.2 <= 3.0 * *JIP_CLOCK_TTL && l.0.min(l.1) < *LIVES_START);
        if old_key != new_key && !moved && lives_ok {
            let l = lv.unwrap_or((0, 0, 0.0));
            g.late_mult.insert(token.to_string(), (mid.clone(), (*LIVES_START as f64 / std::cmp::max(1, l.0.min(l.1)) as f64).min(3.0)));
        } else {
            g.late_mult.remove(token);
        }
        if old_key != new_key && !moved {
            let (prog, behind) = join_progress(ck, lv, team, now);
            g.late_prog.insert(token.to_string(), (mid.clone(), prog, behind));
            dlog!("jip: join progress", tag(token), format!("{} %{}", py::fixed(100.0 * prog, 0), if behind != 0 { ", its team behind on lives" } else { "" }));
        } else {
            g.late_prog.remove(token);
        }
        if bonus != 0 && old_key != new_key && !moved {
            g.late_bonus.insert(token.to_string(), (mid.clone(), bonus));
            dlog!("jip: late-join bonus", tag(token), bonus, format!("parts ({} late-join points, {} v {} players)", down, n_ally, n_enemy));
        } else {
            g.late_bonus.remove(token);
        }
        Some((mid, slot as i64, pl, old_key))
    }
}

// ---------------------------------------------------------------- join in progress glue
pub static JIP_LAST_WHY: LazyLock<Mutex<HashMap<String, String>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// token -> (match id, time): its join in progress put it back into the match it had left (wave_byte bit 2)
pub static JIP_BACK: LazyLock<Mutex<HashMap<String, (String, f64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// token -> (match id, time): joined a running match (join in progress)
pub static JIP_LATE: LazyLock<Mutex<HashMap<String, (String, f64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// token -> (mid, time of its GAME-START) of a joiner, kept a few minutes
pub static JIP_STARTED: LazyLock<Mutex<HashMap<String, (String, f64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// token -> (last re-push time, count)
pub static JIP_REPUSH: LazyLock<Mutex<HashMap<String, (f64, i64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static JIP_REPUSH_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_JIP_REPUSH_S", 15.0));
pub static JIP_REPUSH_MAX: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_JIP_REPUSH_MAX", 8i64));

// py: daemon.py:6902 jip_push_peers
pub fn jip_push_peers(mid: &str, players: &[String]) {
    if match_p2p_get(&key_of(players)) != Some(true) {
        return;
    }
    let dep: HashSet<String> = MM.lock.lock().departed.get(mid).cloned().unwrap_or_default();
    let live: Vec<(usize, &String)> = players.iter().enumerate().filter(|(_, t)| !dep.contains(*t)).collect();
    let sid = match_sid(players);
    let mut table = vec![live.len() as u8];
    for (i, t) in &live {
        table.push(*i as u8);
        table.extend_from_slice(&BRIDGES.steamid(t).to_le_bytes());
    }
    for (i, tok) in &live {
        let mut p = sid.to_le_bytes().to_vec();
        p.push(*i as u8);
        p.extend_from_slice(&table);
        match BRIDGES.conn(tok) {
            Some(c) => {
                if let Err(e) = c.sendall(&msg(0xF2, &p)) {
                    dlog!("jip: PUSH-PEER ERR", tag(tok), e);
                }
            }
            None => dlog!("jip: PUSH-PEER ERR", tag(tok), "'NoneType' object has no attribute 'sendall'"),
        }
    }
}

// py: daemon.py:6926 clan_rejoin_after_booking
pub fn clan_rejoin_after_booking(token: &str, mid: &str) {
    let tm = (token.to_string(), mid.to_string());
    let done = CLAN_DONE.lock().get(token).is_some_and(|m| m == mid);
    let mark = || -> Result<(), String> {
        let mut res: Result<(), String> = Ok(());
        let r = db::with_write(|c| {
            let key = clan_key(c, token);
            res = clan_snap_rejoined(c, &key, mid);
            Ok(())
        });
        if let Err(e) = r {
            return Err(e.to_string());
        }
        res
    };
    if !done {
        let _ = mark();
        return;
    }
    CLAN_DONE.lock().remove(token);
    CLAN_CREDITED.lock().remove(&tm);
    CLAN_EXCUSED.lock().remove(&tm);
    CLAN_QUITTERS.lock().remove(&tm);
    CLAN_EXCUSE_WHY.lock().remove(&tm);
    CLAN_LEFT_AT.lock().remove(&tm);
    CLAN_REJOINED.lock().remove(&tm);
    CLAN_MOVED.lock().remove(&tm);
    CLAN_REBOOK.lock().insert(tm);
    if let Err(e) = mark() {
        dlog!("CLAN rejoin mark failed", e);
    }
    dlog!("CLAN rejoin", tag(token), "mid", mid, "after its day was booked - its next report replaces that booking (one day per match)");
}

// py: daemon.py:6959 jip_start
pub fn jip_start(token: &str, mid: &str, slot: i64, players: &[String]) -> bool {
    if !MM.jip_joiners(mid).iter().any(|t| t == token) {
        dlog!("jip:", tag(token), "left before its START-MATCH for", mid, "went out - not started");
        evt!("jip-aborted", mid, tag(token), "backed out at the moment the match was found");
        return false;
    }
    dlog!("jip:", tag(token), "-> running match", mid, "slot", slot, "team", team_of(slot), "players", tags(players));
    JIP_LATE.lock().insert(token.to_string(), (mid.to_string(), clock::now()));
    clan_rejoin_after_booking(token, mid);
    evt!("jip-join", mid, tag(token), "slot", slot, "team", team_of(slot), "players", tags(players));
    if HM.enabled {
        HM.add_player(mid, token, slot.max(0) as usize);
    }
    jip_push_peers(mid, players);
    LOADED.lock().remove(token);
    CD_GOT.lock().remove(token);
    if let Some(conn) = SESSIONS.conn(token) {
        match conn.sendall(&match_start_frame(token, players)) {
            Ok(()) => dlog!("jip: START-MATCH", tag(token), "sent on session conn"),
            Err(e) => dlog!("jip: START-MATCH ERR", tag(token), e),
        }
    }
    if !MM.jip_joiners(mid).iter().any(|t| t == token) {
        if HM.enabled {
            HM.depart(token, "left (backed out at match found)");
        }
        JIP_LATE.lock().remove(token);
        dlog!("jip:", tag(token), "backed out while START-MATCH went out - slot freed again");
        evt!("jip-aborted", mid, tag(token), "backed out while its START-MATCH was sent");
        return false;
    }
    let (t, m) = (token.to_string(), mid.to_string());
    spawn_t("jip-countdown", move || jip_countdown(&t, &m));
    true
}

// py: daemon.py:6991 jip_countdown
pub fn jip_countdown(token: &str, mid: &str) {
    std::thread::sleep(std::time::Duration::from_secs_f64(2.0));
    let pre: i64 = crate::env_parse("T2R_JIP_PRE_COUNTDOWN", 2i64);
    let secs: i64 = crate::env_parse("T2R_COUNTDOWN", 10i64);
    for (phase, n) in countdown_steps(pre, secs, false) {
        if !MM.jip_joiners(mid).iter().any(|t| t == token) {
            dlog!("jip: countdown aborted (left)", tag(token));
            return;
        }
        if let Some(c) = SESSIONS.conn(token) {
            if push(&c, &msg(0x0c, &[phase as u8, n as u8]), true).is_ok() && phase == 2 && n == 0 {
                CD_GOT.lock().insert(token.to_string());
            }
        }
        std::thread::sleep(std::time::Duration::from_secs_f64(1.0));
    }
    dlog!("jip: countdown done", tag(token));
}

// py: daemon.py:7016 jip_pass
pub fn jip_pass() {
    if !*JIP {
        return;
    }
    let ready = MM.jip_ready();
    let mut done: HashSet<String> = HashSet::new();
    for token in &ready {
        if done.contains(token) {
            continue;
        }
        let mates: Vec<String> = if PARTIES.pid(token).is_some_and(|p| !p.is_empty()) {
            PARTIES.members(token).into_iter().filter(|m| m != token && ready.contains(m) && !done.contains(m)).collect()
        } else {
            Vec::new()
        };
        let mut why: Vec<String> = Vec::new();
        let mut res = None;
        let nm = if *LOBBY_KEEP && *REQUEUE { MM.lock.lock().rq.next_of(token).filter(|n| !n.is_empty()) } else { None };
        let nm_live = nm.as_ref().is_some_and(|n| {
            let g = MM.lock.lock();
            g.matches.contains_key(n) && !g.ended.contains(n)
        });
        if nm_live {
            let nm = nm.clone().unwrap_or_default();
            res = MM.jip_join(token, &mut why, 1 + mates.len() as i64, Some(&nm), None);
            if res.is_some() {
                dlog!("LOBBY:", tag(token), "back from the post-match screens - joins its lobby's match", nm);
            } else if !MM.lock.lock().started.contains(&nm) {
                continue;
            }
        }
        if res.is_none() {
            res = MM.jip_join(token, &mut why, 1 + mates.len() as i64, None, None);
        }
        let Some((mid, slot, players, _)) = res else {
            let w = why.join("; ");
            let wk: String = w.chars().filter(|c| !c.is_ascii_digit()).collect();
            let mut lw = JIP_LAST_WHY.lock();
            if !w.is_empty() && lw.get(token) != Some(&wk) {
                lw.insert(token.clone(), wk);
                drop(lw);
                dlog!("jip:", tag(token), "no running match to join:", w);
            }
            continue;
        };
        JIP_LAST_WHY.lock().remove(token);
        jip_start(token, &mid, slot, &players);
        done.insert(token.clone());
        for m in &mates {
            let mut why2: Vec<String> = Vec::new();
            match MM.jip_join(m, &mut why2, 1, Some(&mid), Some(team_of(slot))) {
                Some(r2) => {
                    JIP_LAST_WHY.lock().remove(m);
                    jip_start(m, &r2.0, r2.1, &r2.2);
                    done.insert(m.clone());
                }
                None => dlog!("jip: party mate", tag(m), "could not follow", tag(token), "into", mid, "yet:", why2.join("; ")),
            }
        }
    }
}

// py: daemon.py:7078 jip_unreachable
pub fn jip_unreachable(token: &str, now: Option<f64>) -> bool {
    let now = now.unwrap_or_else(clock::now);
    let Some(st) = JIP_STARTED.lock().get(token).cloned() else { return false };
    if now - st.1 > 300.0 {
        return false;
    }
    let (last, n) = JIP_REPUSH.lock().get(token).copied().unwrap_or((0.0, 0));
    if n >= *JIP_REPUSH_MAX || now - last < *JIP_REPUSH_S {
        return false;
    }
    let players = match MM.players_of(token) {
        Some(p) if !p.is_empty() && MM.mid_of(token).as_deref() == Some(st.0.as_str()) => p,
        _ => return false,
    };
    JIP_REPUSH.lock().insert(token.to_string(), (now, n + 1));
    dlog!("jip: joiner", tag(token), format!("reports the host silent {} s after its start: re-pushing the peer table ({})", py::fixed(now - st.1, 0), n + 1));
    evt!("jip-repush", st.0, tag(token), n + 1);
    jip_push_peers(&st.0, &players);
    true
}

// py: daemon.py:7096 wave_byte
pub fn wave_byte(late: bool, back: bool, mid: Option<&str>) -> u8 {
    let is1 = |k: &str| crate::env_str(k, "") == "1";
    let mut test: u8 = (if is1("T2R_TEST_FREECAM") { 8 } else { 0 }) | (if is1("T2R_TEST_CLOCK2") { 0x10 } else { 0 }) | (if is1("T2R_TEST_LIVES2") { 0x20 } else { 0 });
    if *AFK_KICK_S <= 0.0 {
        test |= 0x40;
    }
    let mut wave = *WAVE_RESPAWN;
    if let Some(pr) = mid.and_then(|m| PRIV.rules_of_mid(m)) {
        wave = pr.get("resp").and_then(|v| v.as_i64()).unwrap_or(0) == 0;
        if pr.get("afk").and_then(|v| v.as_i64()).unwrap_or(0) == 0 {
            test |= 0x40;
        }
    }
    0x80 | (wave as u8) | (if late { 2 } else { 0 }) | (if late && back { 4 } else { 0 }) | test
}

// py: daemon.py:7112 jip_is_late
pub fn jip_is_late(token: &str) -> bool {
    let Some(j) = JIP_LATE.lock().get(token).cloned() else { return false };
    if clock::now() - j.1 > 7200.0 || MM.mid_of(token).as_deref() != Some(j.0.as_str()) {
        JIP_LATE.lock().remove(token);
        return false;
    }
    true
}

// py: daemon.py:7121 jip_roster_op
pub fn jip_roster_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 64 {
        return None;
    }
    let t = latin1(&pl[0..64]);
    readopt_claim(&t);
    if pl.len() > 64 && pl[64] & 0x20 != 0 {
        RQ_CAP.lock().insert(t.clone(), clock::now());
    }
    if pl.len() >= 80 && pl[76] == 0xAF {
        afk_report(&t, pl[77] as i64 | ((pl[78] as i64) << 8), pl[79]);
    }
    if !*JIP {
        return Some(msg(0xE5, &[0, 0, wave_byte(false, false, None)]));
    }
    let flags = if pl.len() > 64 { pl[64] } else { 0 };
    let (rem, dur) = if pl.len() >= 73 {
        (f32::from_le_bytes([pl[65], pl[66], pl[67], pl[68]]) as f64, f32::from_le_bytes([pl[69], pl[70], pl[71], pl[72]]) as f64)
    } else {
        (0.0, 0.0)
    };
    if flags & 0x10 != 0 {
        TS_CAP.lock().insert(t.clone(), clock::now());
    }
    PRES.touch_known(&t);
    if flags & 1 != 0 || flags & 2 != 0 {
        MM.jip_clock(&t, if flags & 1 != 0 { rem } else { 0.0 }, if flags & 1 != 0 { dur } else { 0.0 }, flags & 2 != 0);
    }
    if pl.len() >= 76 {
        MM.jip_lives(&t, pl[73] as i64, pl[74] as i64, pl[75] as i64);
    }
    let players: Vec<String> = MM.players_of(&t).unwrap_or_default();
    let mid = MM.mid_of(&t);
    let gone: HashSet<String> = match &mid {
        Some(m) => MM.lock.lock().departed.get(m).cloned().unwrap_or_default(),
        None => HashSet::new(),
    };
    let ph = "f0".repeat(30);
    let mids = mid.as_deref().unwrap_or("");
    let mut body: Vec<u8> = Vec::new();
    let mut n: u8 = 0;
    for (i, tok) in players.iter().take(std::cmp::max(8, *ENGINE_SLOTS) as usize).enumerate() {
        if (gone.contains(tok) && *tok != t) || tok.starts_with(&ph) {
            continue;
        }
        n += 1;
        let mut e = [0u8; 0x32];
        e[0] = i as u8;
        let nm0 = get_name(tok);
        let nm0 = if nm0.is_empty() { format!("PLAYER {}", i + 1) } else { nm0 };
        let nm: Vec<u8> = nm0.chars().filter(|c| (*c as u32) < 256).map(|c| c as u32 as u8).take(0x2e).collect();
        e[1..1 + nm.len()].copy_from_slice(&nm);
        e[0x31] = roster_team_in(mids, i as i64, Some(tok)) as u8;
        body.extend_from_slice(&e);
    }
    let late = flags & 4 != 0 && jip_is_late(&t);
    let jb = JIP_BACK.lock().get(&t).cloned();
    let back = late && jb.as_ref().is_some_and(|j| Some(&j.0) == mid.as_ref() && clock::now() - j.1 < 600.0);
    let mut tail: Vec<u8> = Vec::new();
    if flags & 0x10 != 0 && mid.is_some() {
        let sw: HashMap<String, i64> = TEAM_SWAP.lock().get(mids).cloned().unwrap_or_default();
        let items: Vec<(usize, i64)> = players.iter().take(std::cmp::max(8, *ENGINE_SLOTS) as usize).enumerate().filter(|(_, tok)| sw.contains_key(*tok) && !gone.contains(*tok)).map(|(i, tok)| (i, roster_team_in(mids, i as i64, Some(tok)))).collect();
        if !items.is_empty() {
            tail.push(0xA7);
            tail.push(items.len() as u8);
            for (i, tm) in items {
                tail.push(i as u8);
                tail.push(tm as u8);
            }
        }
    }
    let mut out = vec![1u8, n];
    out.extend_from_slice(&body);
    out.push(wave_byte(late, back, mid.as_deref()));
    out.extend_from_slice(&tail);
    Some(msg(0xE5, &out))
}

// py: daemon.py:7168 matchmaker_loop
pub fn matchmaker_loop() {
    loop {
        std::thread::sleep(std::time::Duration::from_secs_f64(1.0));
        shutdown_tick();
        SOCIAL.tick(None);
        PRIV.gc(&|t: &str| MM.in_match(t) || MM.lock.lock().queue.contains_key(t));
        match MM.try_form() {
            Some((_mid, players)) => {
                push_peers(&players);
                start_match(&players);
            }
            None => jip_pass(),
        }
        auto_balance_tick();
    }
}

// ---------------------------------------------------------------- host migration + re-pick
// py: daemon.py:7194 HM
pub static HM: LazyLock<HostMigration> = LazyLock::new(|| {
    let mut o = HmOpts::new(crate::env_is1("T2R_HOST_MIGRATION", true));
    o.ping = Some(Box::new(|a: &str, b: &str| pair_ping(a, b)));
    o.perf = Some(Box::new(|t: &str| Some(perf_penalty(t))));
    o.pool = Some(Box::new(|ts: &[String]| Some(host_pool(ts))));
    o.link_alive = Some(Box::new(|t: &str| Some(SESSIONS.conn(t).is_some())));
    HostMigration::new(o)
});

// py: daemon.py:7200 REPICK
pub static REPICK: LazyLock<HostRepick> = LazyLock::new(|| HostRepick::new(Box::new(clock::now), None));

// py: daemon.py:7202 hostcap_repick_log
pub fn hostcap_repick_log(mid: &str, players: &[String], alive: &[usize], hslot: usize, scores: &IndexMap<i64, Option<(i64, i64)>>, act: &Repick) {
    let n = alive.len() as i64;
    let cur = match HOSTCAP.verdict(&players[hslot], n) {
        Ok(v) => v,
        Err(e) => {
            dlog!("HOST-REPICK cap ERR", e);
            return;
        }
    };
    let mut cand: IndexMap<i64, (i64, String)> = IndexMap::new();
    for (s, sc) in scores {
        if *s != hslot as i64 && sc.is_some() {
            match HOSTCAP.verdict(&players[*s as usize], n) {
                Ok(v) => {
                    cand.insert(*s, v);
                }
                Err(e) => {
                    dlog!("HOST-REPICK cap ERR", e);
                    return;
                }
            }
        }
    }
    let cur_k = scores.get(&(hslot as i64)).copied().flatten();
    let sk = |s: &i64| scores.get(s).copied().flatten().unwrap_or((0, 0));
    let near: Vec<i64> = cand.keys().copied().filter(|s| cur_k.is_none() || sk(s).0 - cur_k.map(|c| c.0).unwrap_or(0) <= hostcap::cfg().window_ms).collect();
    let better: Vec<i64> = near.iter().copied().filter(|s| cand[s].0 < cur.0).collect();
    let verd = match act {
        Repick::Migrate(w) if cand.get(w).is_some_and(|c| c.0 >= 2) => format!("would BLOCK the migration: target {} {}", tag(&players[*w as usize]), cand[w].1),
        Repick::Migrate(w) if cand.get(w).is_some_and(|c| c.0 < cur.0) => format!("agrees: target {} {}", tag(&players[*w as usize]), cand[w].1),
        _ if cur.0 >= 2 && !better.is_empty() => {
            let b = better.iter().copied().min_by_key(|s| (cand[s].0, sk(s))).unwrap_or(better[0]);
            format!("would favour {} ({}) over the lagging host", tag(&players[b as usize]), cand[&b].1)
        }
        _ => "no change".to_string(),
    };
    dlog!("HOST-REPICK", mid, format!("cap: host {} {}; {}", tag(&players[hslot]), cur.1, verd));
}

pub static FREEZE_N: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_HOST_FREEZE_N", 3i64));
pub static FREEZE_WINDOW_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_HOST_FREEZE_WINDOW_S", 300.0));
pub static SAT_TARGET_RANK: LazyLock<i64> = LazyLock::new(|| crate::env_parse("T2R_HOST_SAT_TARGET_RANK", 1i64));

// py: daemon.py:7230 repick_cap
pub fn repick_cap(mid: &str, players: &[String], alive: &[usize], hslot: usize) -> Result<Option<CapOverride>, String> {
    if hostcap::mode() < 1 {
        return Ok(None);
    }
    let n = alive.len() as i64;
    let h = &players[hslot];
    let others: Vec<String> = alive.iter().filter(|o| **o != hslot).map(|o| players[*o].clone()).collect();
    let (sat, why) = REPICK.saturated(mid, h, &others, &|a, b| pair_ping(a, b));
    if n < 3 && !sat {
        return Ok(None);
    }
    let mut vs: IndexMap<usize, (i64, String)> = IndexMap::new();
    for s in alive {
        vs.insert(*s, HOSTCAP.verdict(&players[*s], std::cmp::max(n, 2))?);
    }
    let mut bad: Option<String> = None;
    let mut max_rank: i64 = 0;
    if n >= 3 && vs[&hslot].0 >= 2 && HOSTCAP.lagged_in(Some(mid), h)? {
        bad = Some(format!("{} lagged in this match ({})", tag(h), vs[&hslot].1));
        max_rank = 1;
    } else if sat {
        bad = Some(format!("{} link saturated ({}; {})", tag(h), why, vs[&hslot].1));
        max_rank = *SAT_TARGET_RANK;
    } else if n >= 3 && *FREEZE_N > 0 && HM.freezes(mid, hslot, *FREEZE_WINDOW_S) as i64 >= *FREEZE_N {
        bad = Some(format!("{} froze for every player {} times in {} s ({})", tag(h), HM.freezes(mid, hslot, *FREEZE_WINDOW_S), *FREEZE_WINDOW_S as i64, vs[&hslot].1));
        max_rank = 1;
    }
    let Some(bad) = bad else {
        let pn = hostmig::cfg().repick_proven_n;
        if hostcap::mode() >= 2 && n >= pn && pn > 0 {
            let mut prov: IndexMap<usize, bool> = IndexMap::new();
            for s in alive {
                prov.insert(*s, HOSTCAP.proven_for(&players[*s], n)?);
            }
            // a host with recent lag records (verdict rank 1 'proven but lagged xN') counts as unproven itself, so an unproven target with a
            // much better ping may take over (hostmig _decide: targets proven at least as well as the host)
            if prov.contains_key(&hslot) && vs.get(&hslot).map(|v| v.0).unwrap_or(0) >= 1 {
                prov.insert(hslot, false);
            }
            let mut o = CapOverride { host_bad: None, max_rank: Some(0), ..Default::default() };
            for s in alive {
                o.rank.insert(*s as i64, if prov[s] { 0 } else { 1 });
                o.text.insert(
                    *s as i64,
                    if prov[s] { format!("proven for {} players", n) } else { format!("not proven for {} players (needs ~{} KB/s: {})", n, hostcap::need_kb_relay(n), vs[s].1) },
                );
            }
            return Ok(Some(o));
        }
        return Ok(None);
    };
    if hostcap::mode() < 2 {
        dlog!("HOST-REPICK", mid, "cap:", bad, "- override off (T2R_HOST_CAP=1, log only)");
        return Ok(None);
    }
    let mut o = CapOverride { host_bad: Some(bad), max_rank: Some(max_rank), saturated: sat, ..Default::default() };
    for (s, v) in &vs {
        o.rank.insert(*s as i64, v.0);
        o.text.insert(*s as i64, v.1.clone());
    }
    Ok(Some(o))
}

static REPICK_BLIND: LazyLock<Mutex<HashMap<String, Vec<usize>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:7266 repick_tick
pub fn repick_tick() {
    if !HM.enabled || REPICK.mode <= 0 {
        return;
    }
    let now = clock::now();
    for mid in HM.mids() {
        let Some(snap) = HM.snapshot(&mid) else {
            REPICK.forget(&mid);
            continue;
        };
        let (t0, players, alive, hslot) = (snap.t0, snap.players, snap.alive, snap.host);
        let mut rtt: Vec<((String, String), Option<i64>)> = Vec::new();
        for a in &alive {
            for b in &alive {
                if a < b {
                    rtt.push(((players[*a].clone(), players[*b].clone()), pair_ping(&players[*a], &players[*b])));
                }
            }
        }
        REPICK.observe(&mid, &rtt);
        if alive.len() < 2 || !alive.contains(&hslot) || !REPICK.due(&mid, t0) {
            continue;
        }
        let cap = match repick_cap(&mid, &players, &alive, hslot) {
            Ok(c) => c,
            Err(e) => {
                dlog!("HOST-REPICK cap ERR", e);
                None
            }
        };
        if alive.len() < 3 && cap.is_none() {
            continue;
        }
        let (stale, _loading) = HM.stale_slots(&mid);
        if !stale.is_empty() {
            let s: Vec<String> = stale.iter().map(|s| format!("slot {} {}", s, tag(&players[*s]))).collect();
            dlog!("HOST-REPICK", mid, "not polling host migration:", s.join(", "), "- not a candidate, scored as a peer with its known pings");
        }
        let toks: Vec<String> = alive.iter().map(|s| players[*s].clone()).collect();
        let pool = host_pool(&toks).0;
        let blind: BTreeSet<usize> = if alive.len() >= 3 {
            alive.iter().copied().filter(|o| alive.iter().filter(|q| *q != o).all(|q| pair_ping(&players[*o], &players[*q]).is_none())).collect()
        } else {
            BTreeSet::new()
        };
        let bv: Vec<usize> = blind.iter().copied().collect();
        if !blind.is_empty() && blind.len() + 1 < alive.len() && REPICK_BLIND.lock().get(&mid) != Some(&bv) {
            REPICK_BLIND.lock().insert(mid.clone(), bv.clone());
            let s: Vec<String> = bv.iter().map(|b| format!("slot {} {}", b, tag(&players[*b]))).collect();
            dlog!("HOST-REPICK", mid, "no ping data for", s.join(", "), "- scored without it");
        }
        let skip: BTreeSet<usize> = if blind.len() + 1 < alive.len() { stale.union(&blind).copied().collect() } else { stale.clone() };
        let mut scores: IndexMap<i64, Option<(i64, i64)>> = IndexMap::new();
        for s in &alive {
            let t = &players[*s];
            if (!pool.contains(t) || skip.contains(s)) && *s != hslot {
                continue;
            }
            let pings: Vec<Option<i64>> =
                alive.iter().filter(|o| *o != s).filter_map(|o| { let v = pair_ping(t, &players[*o]); if v.is_none() && skip.contains(o) { None } else { Some(v) } }).collect();
            scores.insert(*s as i64, if pings.is_empty() || pings.iter().any(|p| p.is_none()) { None } else { Some(hostmig::host_key(&pings.iter().flatten().copied().collect::<Vec<_>>(), perf_penalty(t))) });
        }
        let ck = MM.lock.lock().clock.get(&mid).copied();
        let left = ck.filter(|c| !c.2 && now - c.3 < 20.0).map(|c| (c.0 - (now - c.3)).max(0.0));
        let busy: Option<String> = if HM.handover_pending(&mid) {
            Some("a handover is already pending".to_string())
        } else if REPICK.mode >= 2 {
            let (ok, why) = HM.handover_ok(&mid);
            if ok { None } else { Some(why) }
        } else {
            HM.host_info(&mid).filter(|hi| hi.pending || hi.given_up).map(|_| "migration pending".to_string())
        };
        let labels: HashMap<i64, String> = alive.iter().map(|s| (*s as i64, tag(&players[*s]))).collect();
        let mut info = CheckInfo::default();
        for a in &alive {
            for b in &alive {
                if a < b {
                    info.rtt.push(((*a as i64, *b as i64), pair_ping(&players[*a], &players[*b])));
                }
            }
            info.perf.insert(*a as i64, perf_penalty(&players[*a]));
        }
        let mut age = 0.0f64;
        {
            let pa = PING_AT.lock();
            for a in &alive {
                for b in &alive {
                    if a != b {
                        age = age.max(now - pa.get(&(players[*a].clone(), players[*b].clone())).copied().unwrap_or(now));
                    }
                }
            }
        }
        info.age = Some(age);
        let roster: BTreeSet<i64> = alive.iter().map(|s| *s as i64).collect();
        let act = REPICK.check(&mid, t0, &roster, hslot as i64, &scores, left, busy.as_deref(), Some(&labels), Some(&info), cap.as_ref());
        if hostcap::mode() >= 1 {
            hostcap_repick_log(&mid, &players, &alive, hslot, &scores, &act);
        }
        if let Repick::Migrate(what) = act {
            if REPICK.mode >= 2 && HM.handover(&mid, what) {
                evt!("host-repick", mid, "handover to", tag(&players[what as usize]));
                REPICK.migrated(&mid, &roster);
            }
        }
    }
    let live: HashSet<String> = HM.mids().into_iter().collect();
    REPICK_BLIND.lock().retain(|m, _| live.contains(m));
    for mid in REPICK.known_mids() {
        if !live.contains(&mid) {
            REPICK.forget(&mid);
        }
    }
}

// ---------------------------------------------------------------- interrogation, gun limits
// py: daemon.py:7378 INT
pub static INT: LazyLock<interrogation::Interrogation> = LazyLock::new(interrogation::Interrogation::new);
// py: daemon.py:7383 GL
pub static GL: LazyLock<gunlimit::GunLimits> = LazyLock::new(gunlimit::GunLimits::new);
pub static GL_ON: LazyLock<bool> = LazyLock::new(|| crate::env_on("T2R_GUN_LIMITS", true));

// py: daemon.py:7386 gunlimit_op
pub fn gunlimit_op(pl: &[u8]) -> Option<Vec<u8>> {
    let (token, mask) = GL.parse_request(pl)?;
    readopt_claim(&token);
    if !*GL_ON {
        return Some(msg(gunlimit::OP_REPLY, b"on=0"));
    }
    let (mid, active) = MM.active_of(&token);
    let players = mid.as_ref().and_then(|m| MM.players_of_mid(m));
    let (Some(mid), Some(players)) = (mid, players) else {
        GL.release(&token);
        return Some(msg(gunlimit::OP_REPLY, b"on=0"));
    };
    let Some(idx) = players.iter().position(|x| *x == token) else {
        GL.release(&token);
        return Some(msg(gunlimit::OP_REPLY, b"on=0"));
    };
    if PRIV.rule(&mid, "glim") == Some(0) {
        GL.release(&token);
        return Some(msg(gunlimit::OP_REPLY, b"on=0"));
    }
    let live: HashSet<String> = MM.snapshot().into_iter().map(|m| m.0).collect();
    GL.gc(&live);
    let team = roster_team_in(&mid, idx as i64, Some(&token));
    let pre = !MM.lock.lock().started.contains(&mid);
    let act: HashSet<String> = active.into_iter().collect();
    let c = GL.claim(&mid, team, &token, mask, Some(&act), pre, idx as i64);
    Some(msg(gunlimit::OP_REPLY, gunlimit::GunLimits::text(true, team, c.held, c.grant, c.deny, c.full, c.pend).as_bytes()))
}
