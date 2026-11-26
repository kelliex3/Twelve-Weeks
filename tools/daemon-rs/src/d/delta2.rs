//! srv202 delta of daemon.py (git 6291ca1 -> 3181f9a), the parts that are new functions: the region preference (dedicated.py's preference store
//! with T2R_DEDICATED=0 + daemon.py's region_lock_*), the fair clan days (join progress, presence sampling, protected days, boosters kept), the lobby
//! voice room push, the 0xC4 report op, public_map_pool, the join-in-progress "could not reach the host" memory.
//! Python names kept; `// py:` lines give the new file's lines (scratch py202full).
use super::*;
use crate::{clock, py, voiceroom};
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

// ---------------------------------------------------------------- dedicated.py: the part that runs with T2R_DEDICATED=0 (preference store)
// (the dedicated match servers themselves are NOT ported: `enabled()` is false, every `DED.is_dedicated(..)` branch of daemon.py is its off branch)
pub const OP_CAPS: u8 = 0xC4;
pub const OP_CAPS_REPLY: u8 = 0xC5;

/// py: dedicated.py:31 lock_on
pub static REGION_LOCK_ON: LazyLock<bool> = LazyLock::new(|| crate::env_str("T2R_REGION_LOCK", "1") != "0");

/// py: dedicated.py:78 Region (name, ip, udp)
pub struct Region {
    pub name: String,
    pub ip: String,
    pub udp: i64,
}

// py: dedicated.py:49 region_collision
pub fn region_collision(ip: &str, udp: i64) -> Option<String> {
    let pub_ip = crate::env_str("T2R_PUB_IP", "209.25.141.16");
    let pub_udp = crate::env_parse("T2R_PUB_UDP_PORT", 15389i64);
    if ip == pub_ip && udp == pub_udp {
        return Some(format!("equals the daemon's own P2P relay endpoint {}:{}", pub_ip, pub_udp));
    }
    if ip == "127.0.0.1" && crate::env_str("T2R_DED_ALLOW_LOOPBACK", "") != "1" {
        return Some("is 127.0.0.1, the endpoint of a Steam helper".to_string());
    }
    None
}

// py: dedicated.py:61 parse_regions
pub fn parse_regions(spec: &str) -> Vec<Region> {
    let mut out = Vec::new();
    for part in spec.split(',') {
        let part = crate::py::strip(part);
        if part.is_empty() || !part.contains('=') {
            continue;
        }
        let (name, addr) = part.split_once('=').unwrap_or((part, ""));
        let addr = addr.split_once('@').map(|x| x.0).unwrap_or(addr); // optional @host: where the control link connects
        let bits: Vec<&str> = addr.split(':').collect();
        let ip = bits[0].to_string();
        let udp = if bits.len() > 1 { crate::py::strip(bits[1]).parse::<i64>().unwrap_or(7001) } else { 7001 };
        out.push(Region { name: crate::py::strip(name).to_string(), ip, udp });
    }
    out
}

/// py: dedicated.py:154 Dedicated (the preference store, the region list; no matchd links)
pub struct Dedicated {
    pub mode: i64,
    pub regions: Vec<Region>,
    /// token -> region preference byte (1 NA / 2 EU) from its last op 0xC4; kept with T2R_DEDICATED=0 too
    pub prefs: Mutex<HashMap<String, u8>>,
}

pub static DED: LazyLock<Dedicated> = LazyLock::new(|| {
    let mode = crate::env_str("T2R_DEDICATED", "0").trim().parse::<i64>().unwrap_or(0);
    let mut ok = Vec::new();
    for r in parse_regions(&crate::env_str("T2R_REGIONS", "")) {
        match region_collision(&r.ip, r.udp) {
            Some(why) => dlog!("DEDICATED region", r.name, format!("{}:{}", r.ip, r.udp), "REFUSED:", why),
            None => ok.push(r),
        }
    }
    Dedicated { mode, regions: ok, prefs: Mutex::new(HashMap::new()) }
});

impl Dedicated {
    /// py: dedicated.py:186 enabled. NOT ported: the matchd links, so always false (T2R_DEDICATED=0 path only).
    pub fn enabled(&self) -> bool {
        false
    }

    // py: dedicated.py:243 region_list
    pub fn region_list(&self) -> Vec<u8> {
        let mut out = vec![self.regions.len() as u8];
        for r in &self.regions {
            let ip = r.ip.as_bytes();
            out.push(ip.len() as u8);
            out.extend_from_slice(ip);
            out.extend_from_slice(&(r.udp as u16).to_le_bytes());
        }
        out
    }

    // py: dedicated.py:201 caps_report: payload after the token: [flags][n][n x (region, u16 ms)][pref]. Returns the 0xC5 reply payload.
    pub fn caps_report(&self, token: &str, payload: &[u8]) -> Vec<u8> {
        let n = if payload.len() >= 2 { payload[1] as usize } else { 0 };
        let pref = if payload.len() > 2 + 3 * n { payload[2 + 3 * n] } else { 0 };
        {
            let mut p = self.prefs.lock();
            if p.len() > 20000 {
                p.clear();
            }
            if pref == 1 || pref == 2 {
                p.insert(token.to_string(), pref);
            } else {
                p.remove(token); // ANY is not stored: an empty map lets matchmaking skip every lock check
            }
        }
        self.region_list()
    }

    // py: dedicated.py:230 pref_of: the area (EU / NA) the player is locked to, or None (ANY, no report, or T2R_REGION_LOCK=0)
    pub fn pref_of(&self, token: &str) -> Option<&'static str> {
        if !*REGION_LOCK_ON {
            return None;
        }
        match self.prefs.lock().get(token).copied().unwrap_or(0) {
            1 => Some("NA"),
            2 => Some("EU"),
            _ => None,
        }
    }

    /// `DED.prefs` truthiness
    pub fn has_prefs(&self) -> bool {
        !self.prefs.lock().is_empty()
    }
}

// py: daemon.py:7915 dedicated_caps_op: Op 0xC4 [64B token][flags][n][n x (region, u16 ms)][pref] -> reply 0xC5
pub fn dedicated_caps_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 64 {
        return None;
    }
    let token = latin1(&pl[..64]);
    if !token.chars().all(|c| c.is_ascii_hexdigit()) {
        return None;
    }
    let old = DED.prefs.lock().get(&token).copied().unwrap_or(0);
    let rep = DED.caps_report(&token, &pl[64..]);
    let new = DED.prefs.lock().get(&token).copied().unwrap_or(0);
    if new != old {
        let p = match new {
            0 => "ANY".to_string(),
            1 => "NA".to_string(),
            2 => "EU".to_string(),
            n => n.to_string(),
        };
        dlog!("REGIONPREF", tag(&token), "pref", p, format!("(region {})", opt_str(&mm_region(&token))));
    }
    Some(msg(OP_CAPS_REPLY, &rep))
}

// ---------------------------------------------------------------- daemon.py: region preference (OPTIONS > MULTIPLAYER, op 0xC4 trailing byte)
/// (selftest) the 60 s note table empty again
pub fn region_lock_reset() {
    REGION_LOCK_LOG.lock().clear();
}

/// py: daemon.py:304 REGION_LOCK_LOG: (tag, tag) -> time of the last 'skipped because of a lock' line
static REGION_LOCK_LOG: LazyLock<Mutex<HashMap<(String, String), f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:306 region_lock_of: the area (EU / NA) a unit (one player or a party's members) is locked to, or None
pub fn region_lock_of(unit: &[String]) -> Option<&'static str> {
    for t in unit {
        if let Some(a) = DED.pref_of(t) {
            return Some(a);
        }
    }
    None
}

// py: daemon.py:314 region_lock_ok: may two units share a match under the region lock? `why` gets the reason when not
pub fn region_lock_ok(unit_a: &[String], unit_b: &[String], mut why: Option<&mut Vec<String>>) -> bool {
    let (la, lb) = (region_lock_of(unit_a), region_lock_of(unit_b));
    if la.is_none() && lb.is_none() {
        return true;
    }
    if let (Some(x), Some(y)) = (la, lb) {
        if x != y {
            if let Some(w) = why.as_mut() {
                w.push(format!("locked to {} and {}", x, y));
            }
            return false;
        }
    }
    for (lock, other) in [(la, unit_b), (lb, unit_a)] {
        if let Some(lock) = lock {
            for t in other {
                // (11-26 smoke) a player whose measured region is unknown counts in the area of their own pick
                let r = mm_region(t);
                let area = r.as_deref().and_then(mm_region_group).or_else(|| DED.pref_of(t));
                if area != Some(lock) {
                    if let Some(w) = why.as_mut() {
                        w.push(format!("locked to {}, {} is {}", lock, tag(t), r.as_deref().filter(|s| !s.is_empty()).unwrap_or("unknown")));
                    }
                    return false;
                }
            }
        }
    }
    true
}

// py: daemon.py:333 region_lock_note: one log line per pair per 60 s
pub fn region_lock_note(a: &[String], b: &[String], why: &[String]) {
    let now = clock::now();
    let k = (a.first().cloned().unwrap_or_default(), b.first().cloned().unwrap_or_default());
    {
        let mut l = REGION_LOCK_LOG.lock();
        if now - l.get(&k).copied().unwrap_or(0.0) < 60.0 {
            return;
        }
        if l.len() > 500 {
            l.clear();
        }
        l.insert(k, now);
    }
    dlog!("REGIONPREF skip", mm::tags(a), "with", mm::tags(b), "-", why.first().cloned().unwrap_or_else(|| "region lock".to_string()));
}

// py: daemon.py:345 region_lock_fits: every player of A may share a match with every player of B; locks are per unit, so a party is judged whole
pub fn region_lock_fits(units_a: &[Vec<String>], units_b: &[Vec<String>], note: bool) -> bool {
    if !DED.has_prefs() {
        return true;
    }
    let mut why: Vec<String> = Vec::new();
    for a in units_a {
        for b in units_b {
            if !region_lock_ok(a, b, Some(&mut why)) {
                if note {
                    region_lock_note(a, b, &why);
                }
                return false;
            }
        }
    }
    true
}

// py: daemon.py:358 region_lock_filter: the searchers of `ready` (queue order) that can share ONE match under the region lock
pub fn region_lock_filter(ready: &[String]) -> Vec<String> {
    if !DED.has_prefs() || !ready.iter().any(|t| region_lock_of(std::slice::from_ref(t)).is_some()) {
        return ready.to_vec();
    }
    let mut kept: Vec<String> = Vec::new();
    let mut units: Vec<Vec<String>> = Vec::new();
    let mut seen: HashSet<String> = HashSet::new();
    for t in ready {
        if seen.contains(t) {
            continue;
        }
        let base: Vec<String> = if PARTIES.pid(t).is_some_and(|p| !p.is_empty()) { PARTIES.members(t) } else { vec![t.clone()] };
        let mut unit: Vec<String> = base.into_iter().filter(|m| ready.contains(m)).collect();
        if unit.is_empty() {
            unit = vec![t.clone()];
        }
        seen.extend(unit.iter().cloned());
        if region_lock_fits(&units, std::slice::from_ref(&unit), true) {
            kept.extend(unit.iter().cloned());
            units.push(unit);
        }
    }
    ready.iter().filter(|t| kept.contains(t)).cloned().collect()
}

// ---------------------------------------------------------------- daemon.py: fair clan days
pub static CLAN_FAIR: LazyLock<bool> = LazyLock::new(|| !matches!(crate::env_str("T2R_CLAN_FAIR", "1").as_str(), "" | "0"));
/// join progress (max of the clock and the lives used) from which a backfill is protected
pub static LATE_PROTECT: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LATE_PROTECT", 0.5));
/// ... or the joiner's team had at most this share of the enemy team's lives at the join
pub static LATE_BEHIND: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LATE_BEHIND", 0.5));
/// user 11-24: a join past 70 % of the match does not use the booster up
pub static BOOST_KEEP_AT: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_BOOST_KEEP_AT", 0.7));
/// protected when the match was lopsided for at least this share of the player's time
pub static LOP_SHARE: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LOPSIDED_SHARE", 0.5));
pub const LOP_MIN_S: f64 = 30.0;
pub const PRES_DT: f64 = 2.0;
/// match id -> {token: [seconds in the match, seconds of them lopsided]} (presence_tick)
pub static MATCH_PRES: LazyLock<Mutex<IndexMap<String, IndexMap<String, [f64; 2]>>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));

// py: daemon.py:2476 join_progress
/// (progress 0..1, behind 0/1) of a match a backfill joins: progress = the larger of the clock's elapsed share (op 0xE3 clock report) and the
/// lives used (1 - min team lives / LIVES_START); behind = the joiner's team has at most LATE_BEHIND of the enemy team's lives. Unknown = (0.0, 0).
pub fn join_progress(ck: Option<(f64, f64, bool, f64)>, lv: Option<(i64, i64, f64)>, team: i64, now: f64) -> (f64, i64) {
    let (mut prog, mut behind) = (0.0f64, 0i64);
    if let Some(ck) = ck {
        if !ck.2 && ck.1 > 0.0 && now - ck.3 <= 3.0 * *JIP_CLOCK_TTL {
            let elapsed = ck.1 - (ck.0 - (now - ck.3));
            prog = prog.max((elapsed / ck.1).max(0.0).min(1.0));
        }
    }
    if let Some(lv) = lv {
        if now - lv.2 <= 3.0 * *JIP_CLOCK_TTL && (team == 0 || team == 1) {
            let m = std::cmp::min(lv.0, lv.1) as f64;
            prog = prog.max((1.0 - m / *LIVES_START as f64).max(0.0).min(1.0));
            let (own, enemy) = if team == 0 { (lv.0, lv.1) } else { (lv.1, lv.0) };
            behind = (enemy > 0 && own as f64 <= *LATE_BEHIND * enemy as f64) as i64;
        }
    }
    (prog, behind)
}

// py: daemon.py:2490 presence_tick
/// Sample every running match once: per player in it, its seconds in the match and the seconds the match was LOPSIDED from its side
/// (the enemy team empty, or the teams 2+ players apart with one side at most 1 player: 4v1, 3v1, 2v0).
pub fn presence_tick(dt: f64) {
    let mut snap: Vec<(String, Vec<(i64, String)>)> = Vec::new();
    let live: HashSet<String>;
    {
        let g = MM.lock.lock();
        live = g.matches.keys().cloned().collect(); // (for the prune below: MATCH_PRES is never held while MM.lock is taken)
        let mut mids: Vec<&String> = g.started.iter().collect();
        mids.sort();
        for mid in mids {
            let Some(pl) = g.matches.get(mid) else { continue };
            if pl.is_empty() || g.ended.contains(mid) {
                continue;
            }
            let dep = g.departed.get(mid);
            let f0 = "f0".repeat(30);
            let act: Vec<(i64, String)> = pl
                .iter()
                .enumerate()
                .filter(|(_, p)| !dep.is_some_and(|d| d.contains(*p)) && is_token(p) && !p.starts_with(&f0) && !g.joining.contains_key(*p))
                .map(|(i, p)| (team_in(mid, i as i64, Some(p)), p.clone()))
                .collect();
            snap.push((mid.clone(), act));
        }
    }
    let mut mp = MATCH_PRES.lock();
    for (mid, act) in snap {
        let mut n = [0i64; 2];
        for (t, _) in &act {
            if *t == 0 || *t == 1 {
                n[*t as usize] += 1;
            }
        }
        let acc = mp.entry(mid).or_default();
        for (t, p) in &act {
            if *t != 0 && *t != 1 {
                continue;
            }
            let (own, enemy) = (n[*t as usize], n[1 - *t as usize]);
            let lop = enemy == 0 || ((own - enemy).abs() >= 2 && std::cmp::min(own, enemy) <= 1);
            let a = acc.entry(p.clone()).or_insert([0.0, 0.0]);
            a[0] += dt;
            if lop {
                a[1] += dt;
            }
        }
    }
    if mp.len() > 400 {
        let dead: Vec<String> = mp.keys().filter(|m| !live.contains(*m)).take(200).cloned().collect();
        for m in dead {
            mp.shift_remove(&m);
        }
    }
}

// py: daemon.py:2528 clan_fair_reason: why this player's clan day for `mid` is protected (CLAN_FAIR), or '' (a normal day)
pub fn clan_fair_reason(token: &str, mid: &str) -> String {
    if BAN_MIDS.lock().contains_key(mid) && !CLAN_BANNED.lock().contains_key(&(token.to_string(), mid.to_string())) {
        return "a banned player was removed from this match".to_string();
    }
    if !*CLAN_FAIR {
        return String::new();
    }
    let lp = MM.lock.lock().late_prog.get(token).cloned();
    if let Some(lp) = lp {
        if lp.0 == mid {
            if lp.1 >= *LATE_PROTECT {
                return format!("backfill at {} % of the match", py::round0(100.0 * lp.1));
            }
            if lp.2 != 0 {
                return "backfill into the team behind on lives".to_string();
            }
        }
    }
    let pr = MATCH_PRES.lock().get(mid).and_then(|m| m.get(token)).copied();
    if let Some(pr) = pr {
        if pr[0] >= LOP_MIN_S && pr[1] >= *LOP_SHARE * pr[0] {
            return format!("no enemies / one player against a team for {} % of its time", py::round0(100.0 * pr[1] / pr[0]));
        }
    }
    String::new()
}

// py: daemon.py:2541 boost_keep: true when `token` backfilled `mid` past BOOST_KEEP_AT of the match: its booster is not used up there
pub fn boost_keep(token: &str, mid: Option<&str>) -> bool {
    let Some(mid) = mid.filter(|m| !m.is_empty()) else { return false };
    if !*CLAN_FAIR {
        return false;
    }
    let lp = MM.lock.lock().late_prog.get(token).cloned();
    lp.is_some_and(|lp| lp.0 == mid && lp.1 >= *BOOST_KEEP_AT)
}

// py: daemon.py:2546 boost_keep_now: the booster page's notice (0xE9 bk=1)
pub fn boost_keep_now(token: &str) -> bool {
    let Some(j) = JIP_LATE.lock().get(token).cloned() else { return false };
    let live = {
        let g = MM.lock.lock();
        g.matches.contains_key(&j.0) && !g.ended.contains(&j.0)
    };
    live && boost_keep(token, Some(&j.0))
}

// ---------------------------------------------------------------- daemon.py: misc new helpers
// py: daemon.py:4488 lookup_token: a token to find the match of this player list by (MM.mid_of / players_of skip departed tokens): players[0]
pub fn lookup_token(players: &[String]) -> String {
    players[0].clone()
}

/// py: daemon.py:7609 JIP_UNREACH: (token, mid) -> (time, host token) of an excused "could not reach the host" leave after a join in progress
pub static JIP_UNREACH: LazyLock<Mutex<HashMap<(String, String), (f64, Option<String>)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub static JIP_UNREACH_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_JIP_UNREACH_S", 600.0));

// py: daemon.py:7611 _match_host_token
pub fn match_host_token(mid: &str) -> Option<String> {
    let hs = if HM.enabled { HM.host_info(mid).map(|h| h.host).unwrap_or(0) } else { 0 };
    let g = MM.lock.lock();
    g.matches.get(mid).and_then(|pl| pl.get(hs).cloned())
}

/// the body added to clan_leave_excuse (daemon.py:8539): remember the unreachable host, drop the stale entries
pub fn jip_unreach_note(token: &str, mid: &str, now: f64) {
    let h = match_host_token(mid);
    let mut u = JIP_UNREACH.lock();
    u.insert((token.to_string(), mid.to_string()), (now, h));
    u.retain(|_, v| !(now - v.0 > *JIP_UNREACH_S));
}

// py: daemon.py:4072 public_map_pool
/// The pool as the launcher's map ids (MAPS image names) for /status.json map_pool: the launcher MAPS tab lists only these.
pub fn public_map_pool() -> Vec<String> {
    let csv = |k: &str| -> Vec<String> { crate::env_str(k, "").split(',').map(|x| py::strip(x).to_string()).filter(|x| !x.is_empty()).collect() };
    let want = csv("T2R_MAP_POOL");
    let mut off: HashSet<String> = BROKEN_MAPS.lock().clone();
    let spon = sp_maps_on();
    off.extend(SP_MAPS.iter().map(|m| m.2.to_string()).filter(|l| !spon.contains(l)));
    off.extend(csv("T2R_MAP_EXCLUDE"));
    MAPLIST.maps.iter().filter(|m| if !want.is_empty() { want.contains(&m.2) } else { !off.contains(&m.2) }).map(|m| m.1.clone()).collect()
}

// ---------------------------------------------------------------- daemon.py: lobby voice (voiceroom.py)
/// py: daemon.py:10962 LOBBY_VOICE (T2R_LOBBY_VOICE=0 turns it off: helpers then get n = 0 once)
pub static LOBBY_VOICE: LazyLock<bool> = LazyLock::new(|| crate::env_str("T2R_LOBBY_VOICE", "1") != "0");
pub static VOICE_PUSH: LazyLock<Mutex<voiceroom::Pusher>> = LazyLock::new(|| Mutex::new(voiceroom::Pusher::new()));

fn room_key_repr(k: &voiceroom::RoomKey) -> String {
    format!("('{}', {})", k.0, py::pyrepr(&serde_json::Value::String(k.1.clone())))
}

// py: daemon.py:10966 voice_room_tick: One pass; returns the frames sent [(token, payload)] (tests read it)
pub fn voice_room_tick(now: Option<f64>) -> Vec<(String, Vec<u8>)> {
    let now = now.unwrap_or_else(clock::now);
    let links: IndexMap<String, (Conn, u64)> = BRIDGES.links();
    let helpers: IndexMap<String, (Conn, u64)> = links.iter().filter(|(_, (_, sid))| *sid != 0).map(|(t, v)| (t.clone(), v.clone())).collect();
    let sids: HashMap<String, u64> = links.iter().map(|(t, (_, sid))| (t.clone(), *sid)).collect();
    let mut room_of: voiceroom::RoomMap = IndexMap::new();
    if *LOBBY_VOICE && !helpers.is_empty() {
        let elig: HashSet<String> = helpers.keys().filter(|t| !MM.in_match(t)).cloned().collect();
        let mut groups: IndexMap<String, Vec<String>> = IndexMap::new();
        if *REQUEUE && *LOBBY_KEEP && !elig.is_empty() {
            // (silence() is asked before the lock is taken: groups() asks it per ticket holder, the closure must not reach back into MM)
            let toks: Vec<String> = MM.lock.lock().rq.tokens();
            let sil: HashMap<String, f64> = toks.iter().map(|t| (t.clone(), silence(t, now))).collect();
            let (out, gone) = {
                let g = MM.lock.lock();
                let qd: Vec<String> = g.queue.keys().cloned().collect();
                g.rq.groups(&qd, &|t| sil.get(t).copied().unwrap_or_else(|| silence(t, now)), &|t| in_match_in(&g.matches, &g.departed, &g.ended, t), Some(now))
            };
            for (t, why) in gone {
                // (groups() prunes like held() does: the same log line)
                dlog!("requeue:", tag(&t), "ticket ended:", why);
            }
            for (mid, g) in out {
                if g.next.is_none() {
                    let mut v = g.srch.clone();
                    v.extend(g.post.iter().cloned());
                    groups.insert(mid, v);
                }
            }
        }
        room_of = voiceroom::rooms(&elig, &|t| PARTIES.pid(t), &|t| PARTIES.members(t), &groups);
    }
    let hid: IndexMap<String, u64> = helpers.iter().map(|(t, (c, _))| (t.clone(), c.id())).collect();
    let frames = VOICE_PUSH.lock().tick(&hid, &room_of, &|x| sids.get(x).copied().unwrap_or(0), &|t| get_name(t));
    let mut sent = Vec::new();
    for (t, _cid, pl) in frames {
        if let Some((c, _)) = helpers.get(&t) {
            let _ = push(c, &msg(voiceroom::OP, &pl), false); // never shed: only changes are sent, a lost frame would not come again
        }
        let desc = match room_of.get(&t) {
            Some((k, m)) => format!("{} in {}", m.len(), room_key_repr(k)),
            None => "none".to_string(),
        };
        dlog!("VOICE-ROOM", tag(&t), "->", desc);
        sent.push((t, pl));
    }
    sent
}

/// `str(x)` of an optional string (None prints as `None`)
fn opt_str(o: &Option<String>) -> String {
    o.clone().unwrap_or_else(|| "None".to_string())
}

// py: daemon.py voice_room_loop (every 2 s; an error is logged, the loop goes on)
pub fn voice_room_loop() {
    loop {
        std::thread::sleep(std::time::Duration::from_secs(2));
        if let Err(p) = std::panic::catch_unwind(|| voice_room_tick(None)) {
            dlog!("VOICE-ROOM ERR", super::conn::panic_text(&p));
        }
    }
}

// py: daemon.py presence_loop (every PRES_DT s)
pub fn presence_loop() {
    loop {
        std::thread::sleep(std::time::Duration::from_secs_f64(PRES_DT));
        if let Err(p) = std::panic::catch_unwind(|| presence_tick(PRES_DT)) {
            dlog!("PRESENCE", super::conn::panic_text(&p));
        }
    }
}
