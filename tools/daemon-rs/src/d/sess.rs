//! K3 sess: presence, Steam helper registry, held session connections, session re-attach, client versions / languages / server texts.
//! daemon.py:2686-2755 (Presence, PEERS, LOADED), 2906-2984 (Bridges, adopt_bridge_name, no_helper_note), 3088-3513 (Sessions, session lost /
//! re-attach, versions, languages, texts).
use super::*;
use crate::{clock, py};
use indexmap::{IndexMap, IndexSet};
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, LazyLock};

// ---------------------------------------------------------------- Presence
/// one `PRES.players[token]` dict
#[derive(Clone, Debug)]
pub struct PresEntry {
    pub name: String,
    pub last: f64,
}

pub struct PresInner {
    pub udp_seen: IndexMap<Addr, f64>,
    pub players: IndexMap<String, PresEntry>,
}

// py: daemon.py:2686 class Presence
pub struct Presence {
    inner: Mutex<PresInner>,
}

impl Presence {
    pub const UDP_SEEN_MAX: usize = 2048;
    fn new() -> Presence {
        Presence { inner: Mutex::new(PresInner { udp_seen: IndexMap::new(), players: IndexMap::new() }) }
    }
    /// `with PRES.lock:` for the code outside the class that reads / edits `PRES.players` / `udp_seen` directly
    pub fn with<R>(&self, f: impl FnOnce(&mut PresInner) -> R) -> R {
        f(&mut self.inner.lock())
    }
    // py: daemon.py:2693 ping
    pub fn ping(&self, addr: Addr) {
        let now = clock::now();
        let mut g = self.inner.lock();
        g.udp_seen.insert(addr, now);
        if g.udp_seen.len() > Self::UDP_SEEN_MAX {
            let old: Vec<Addr> = g.udp_seen.iter().filter(|(_, t)| now - **t > 4.0 * *KEEPALIVE_TTL).map(|(a, _)| *a).collect();
            for a in old {
                g.udp_seen.shift_remove(&a);
            }
            if g.udp_seen.len() > Self::UDP_SEEN_MAX {
                let mut ks: Vec<(Addr, f64)> = g.udp_seen.iter().map(|(a, t)| (*a, *t)).collect();
                ks.sort_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal));
                let n = ks.len() / 2;
                for (a, _) in ks.into_iter().take(n) {
                    g.udp_seen.shift_remove(&a);
                }
            }
        }
    }
    // py: daemon.py:2702 prune
    pub fn prune(&self, max_age: f64, keep: &HashSet<String>) -> usize {
        let now = clock::now();
        let mut g = self.inner.lock();
        let old: Vec<String> = g.players.iter().filter(|(t, p)| now - p.last > max_age && !keep.contains(*t)).map(|(t, _)| t.clone()).collect();
        for t in &old {
            g.players.shift_remove(t);
        }
        let stale: Vec<Addr> = g.udp_seen.iter().filter(|(_, t)| now - **t > 4.0 * *KEEPALIVE_TTL).map(|(a, _)| *a).collect();
        for a in stale {
            g.udp_seen.shift_remove(&a);
        }
        old.len()
    }
    // py: daemon.py:2713 touch
    pub fn touch(&self, token: &str, name: &str) {
        let name = safe_name(name);
        let now = clock::now();
        let mut g = self.inner.lock();
        match g.players.get_mut(token) {
            Some(p) => {
                p.name = name;
                p.last = now;
            }
            None => {
                g.players.insert(token.to_string(), PresEntry { name, last: now });
            }
        }
    }
    // py: daemon.py:2717 touch_known
    pub fn touch_known(&self, token: &str) {
        let now = clock::now();
        if let Some(p) = self.inner.lock().players.get_mut(token) {
            p.last = now;
        }
    }
    // py: daemon.py:2722 drop
    pub fn drop_token(&self, token: &str) {
        self.inner.lock().players.shift_remove(token);
    }
    // py: daemon.py:2725 name_of
    pub fn name_of(&self, token: &str) -> String {
        self.inner.lock().players.get(token).map(|p| p.name.clone()).unwrap_or_else(|| "PLAYER".to_string())
    }
    // py: daemon.py:2729 online (the names of the online entries, in table order)
    pub fn online_names(&self) -> Vec<String> {
        let now = clock::now();
        self.inner.lock().players.values().filter(|p| now - p.last < *KEEPALIVE_TTL).map(|p| p.name.clone()).collect()
    }
    // py: daemon.py:2734 count
    pub fn count(&self) -> usize {
        let now = clock::now();
        self.inner.lock().players.values().filter(|p| now - p.last < *KEEPALIVE_TTL).count()
    }
    /// `[(token, p.get("last", 0))]` of every `PRES.players` entry, one lock pass
    pub fn players_last(&self) -> Vec<(String, f64)> {
        if WORLD_ON.load(Ordering::Relaxed) {
            let now = clock::now();
            return WORLD.lock().online.iter().map(|t| (t.clone(), now)).collect();
        }
        self.inner.lock().players.iter().map(|(t, p)| (t.clone(), p.last)).collect()
    }
    /// `PRES.players.get(token, {}).get("last", 0)`
    pub fn last_of(&self, token: &str) -> f64 {
        self.inner.lock().players.get(token).map(|p| p.last).unwrap_or(0.0)
    }
    /// `token in PRES.players`
    pub fn has(&self, token: &str) -> bool {
        self.inner.lock().players.contains_key(token)
    }
    /// `len(PRES.players)`, `len(PRES.udp_seen)`
    pub fn sizes(&self) -> (usize, usize) {
        let g = self.inner.lock();
        (g.players.len(), g.udp_seen.len())
    }
}

pub static PRES: LazyLock<Presence> = LazyLock::new(Presence::new);

// py: daemon.py:2737 PEERS / peer_seen: player-IP -> latest UDP (ip, port)
pub const PEERS_MAX: usize = 2048;
pub static PEERS: LazyLock<Mutex<IndexMap<String, Addr>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));
pub fn peer_seen(ip: &str, addr: Addr) {
    let mut p = PEERS.lock();
    p.shift_remove(ip);
    p.insert(ip.to_string(), addr);
    if p.len() > PEERS_MAX {
        let n = p.len() / 4;
        let ks: Vec<String> = p.keys().take(n).cloned().collect();
        for k in ks {
            p.shift_remove(&k);
        }
    }
}

/// daemon.py:2750 LOADED: tokens that sent op 0x09 (level loaded / ready) for their current match
pub static LOADED: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));
/// daemon.py:2751 LOADED_T: token -> time of its last op 0x09
pub static LOADED_T: LazyLock<Mutex<HashMap<String, f64>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// ---------------------------------------------------------------- Bridges (Steam helpers)
pub struct Bridge {
    pub conn: Conn,
    pub steamid: u64,
    pub port: i64,
    pub ip: Option<String>,
    pub name: Option<String>,
}

pub struct BridgesInner {
    pub by_token: IndexMap<String, Bridge>,
    pub by_sid: HashMap<u64, String>,
}

// py: daemon.py:2906 class Bridges
pub struct Bridges {
    inner: Mutex<BridgesInner>,
}

impl Bridges {
    fn new() -> Bridges {
        Bridges { inner: Mutex::new(BridgesInner { by_token: IndexMap::new(), by_sid: HashMap::new() }) }
    }
    /// `with BRIDGES.lock:` for code that reads `by_token` / `by_sid` directly
    pub fn with<R>(&self, f: impl FnOnce(&mut BridgesInner) -> R) -> R {
        f(&mut self.inner.lock())
    }
    // py: daemon.py:2911 register
    pub fn register(&self, token: &str, conn: Conn, steamid: u64, port: i64, ip: Option<&str>, name: Option<&str>) {
        let mut g = self.inner.lock();
        if let Some(old) = g.by_token.get(token) {
            let osid = old.steamid;
            if g.by_sid.get(&osid).map(|s| s.as_str()) == Some(token) {
                g.by_sid.remove(&osid);
            }
        }
        g.by_token.insert(token.to_string(), Bridge { conn, steamid, port, ip: ip.map(|s| s.to_string()), name: name.map(|s| s.to_string()) });
        g.by_sid.insert(steamid, token.to_string());
    }
    // py: daemon.py:2918 links
    pub fn links(&self) -> IndexMap<String, (Conn, u64)> {
        self.inner.lock().by_token.iter().map(|(t, b)| (t.clone(), (b.conn.clone(), b.steamid))).collect()
    }
    // py: daemon.py:2922 tokens_for_ip
    pub fn tokens_for_ip(&self, ip: &str) -> Vec<String> {
        self.inner.lock().by_token.iter().filter(|(_, b)| b.ip.as_deref() == Some(ip)).map(|(t, _)| t.clone()).collect()
    }
    // py: daemon.py:2926 name_for_ip
    pub fn name_for_ip(&self, ip: &str) -> Option<String> {
        let g = self.inner.lock();
        let mut names: IndexSet<String> = IndexSet::new();
        for b in g.by_token.values() {
            if b.ip.as_deref() == Some(ip) {
                if let Some(n) = b.name.as_ref().filter(|n| !n.is_empty()) {
                    names.insert(n.clone());
                }
            }
        }
        if names.len() == 1 {
            names.into_iter().next()
        } else {
            None
        }
    }
    // py: daemon.py:2932 port
    pub fn port(&self, token: &str) -> i64 {
        self.inner.lock().by_token.get(token).map(|b| b.port).unwrap_or(0)
    }
    // py: daemon.py:2936 unregister(token, conn)
    pub fn unregister(&self, token: &str, conn: &Conn) -> bool {
        self.unregister_if(token, Some(conn))
    }
    /// `unregister(token)` / `unregister(token, conn)`: with `conn` only when it is still THAT connection
    pub fn unregister_if(&self, token: &str, conn: Option<&Conn>) -> bool {
        let mut g = self.inner.lock();
        let Some(b) = g.by_token.get(token) else { return false };
        if let Some(c) = conn {
            if !Arc::ptr_eq(&b.conn, c) {
                return false;
            }
        }
        let sid = b.steamid;
        g.by_token.shift_remove(token);
        if g.by_sid.get(&sid).map(|s| s.as_str()) == Some(token) {
            g.by_sid.remove(&sid);
        }
        true
    }
    // py: daemon.py:2948 steamid
    pub fn steamid(&self, token: &str) -> u64 {
        self.inner.lock().by_token.get(token).map(|b| b.steamid).unwrap_or(0)
    }
    // py: daemon.py:2952 conn
    pub fn conn(&self, token: &str) -> Option<Conn> {
        self.inner.lock().by_token.get(token).map(|b| b.conn.clone())
    }
    /// `BRIDGES.by_sid.get(steamid)` when that token is registered
    pub fn token_of_sid(&self, steamid: u64) -> Option<String> {
        let g = self.inner.lock();
        let t = g.by_sid.get(&steamid)?;
        if g.by_token.contains_key(t) {
            Some(t.clone())
        } else {
            None
        }
    }
    /// `set(BRIDGES.by_token)`
    pub fn tokens(&self) -> Vec<String> {
        self.inner.lock().by_token.keys().cloned().collect()
    }
    /// `len(BRIDGES.by_token)`
    pub fn len(&self) -> usize {
        self.inner.lock().by_token.len()
    }
}

pub static BRIDGES: LazyLock<Bridges> = LazyLock::new(Bridges::new);
/// daemon.py:2956 NAMELESS_BY_IP: IP -> last game token without an embedded T2N name
pub static NAMELESS_BY_IP: LazyLock<Mutex<IndexMap<String, String>>> = LazyLock::new(|| Mutex::new(IndexMap::new()));

// py: daemon.py:2958 adopt_bridge_name
pub fn adopt_bridge_name(token: &str, ip: &str) {
    if token.is_empty() || token_name(token).is_some_and(|n| !n.is_empty()) || token.chars().count() != 64 {
        return;
    }
    NAMELESS_BY_IP.lock().insert(ip.to_string(), token.to_string());
    if let Some(nm) = BRIDGES.name_for_ip(ip).filter(|n| !n.is_empty()) {
        if get_name(token) != safe_name(&nm) {
            let _ = set_name(token, &nm);
            dlog!("NAME-FROM-BRIDGE", ip, tag(token), "->", nm);
        }
    }
}

// py: daemon.py:2972 no_helper_note: (note key, helper name)
pub fn no_helper_note(token: &str, ip: &str) -> (&'static str, Option<String>) {
    let helper = if token_name(token).is_some_and(|n| !n.is_empty()) { None } else { BRIDGES.name_for_ip(ip) };
    match helper {
        Some(h) if !h.is_empty() => ("files_changed", Some(h)),
        _ => ("steam_off", None),
    }
}

// ---------------------------------------------------------------- Sessions
// py: daemon.py:3088 class Sessions
pub struct Sessions {
    by_token: Mutex<IndexMap<String, Conn>>,
}

impl Sessions {
    fn new() -> Sessions {
        Sessions { by_token: Mutex::new(IndexMap::new()) }
    }
    pub fn register(&self, token: &str, conn: Conn) {
        self.by_token.lock().insert(token.to_string(), conn);
    }
    pub fn unregister(&self, token: &str, conn: &Conn) {
        self.unregister_if(token, Some(conn));
    }
    /// `unregister(token)` / `unregister(token, conn)`
    pub fn unregister_if(&self, token: &str, conn: Option<&Conn>) {
        let mut g = self.by_token.lock();
        if let Some(c) = g.get(token) {
            if conn.is_none_or(|x| Arc::ptr_eq(c, x)) {
                g.shift_remove(token);
            }
        }
    }
    pub fn conn(&self, token: &str) -> Option<Conn> {
        self.by_token.lock().get(token).cloned()
    }
    /// `list(SESSIONS.by_token.items())`
    pub fn conns(&self) -> Vec<(String, Conn)> {
        self.by_token.lock().iter().map(|(t, c)| (t.clone(), c.clone())).collect()
    }
    /// `with SESSIONS.lock:` for code that reads `by_token` directly
    pub fn with<R>(&self, f: impl FnOnce(&mut IndexMap<String, Conn>) -> R) -> R {
        f(&mut self.by_token.lock())
    }
}

pub static SESSIONS: LazyLock<Sessions> = LazyLock::new(Sessions::new);

// ---- session socket lost in a running match (10-25 PT89 live test)
pub static SESSION_GRACE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_SESSION_GRACE", 60.0));
/// daemon.py:3098 SESS_LOST: token -> (time the session conn was lost, match id)
pub static SESS_LOST: LazyLock<Mutex<HashMap<String, (f64, String)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:3100 clan_closed_book
pub fn clan_closed_book(token: &str, mid: &str) {
    if clan_match_get(token).as_deref() != Some(mid) || CLAN_DONE.lock().get(token).map(|s| s.as_str()) == Some(mid) {
        return;
    }
    let ex = clan_leave_excuse(token, mid, "session lost", None);
    if let Some(ex) = ex {
        dlog!("CLAN closed", tag(token), "mid", mid, format!("- excused ({}): no day", ex));
        return;
    }
    CLAN_QUITTERS.lock().insert((token.to_string(), mid.to_string()));
    clan_pending_setdefault(token, mid);
    clan_due_set(token, clock::now() + *SESSION_GRACE_S + 20.0);
    dlog!(
        "CLAN closed",
        tag(token),
        "mid",
        mid,
        format!("- no report comes from a closed game: left-early day (0 parts) booked in {} s", py::fixed(*SESSION_GRACE_S + 20.0, 0))
    );
}

/// the `running` test of a launched, not ended match that `token` has not left (MM.lock inside)
pub fn match_running(mid: &str, token: &str) -> bool {
    let g = MM.lock.lock();
    g.launched.contains(mid) && !g.ended.contains(mid) && !g.departed.get(mid).is_some_and(|d| d.contains(token))
}

// py: daemon.py:3115 note_session_lost
pub fn note_session_lost(token: &str, conn: &Conn) -> bool {
    match SESSIONS.conn(token) {
        Some(c) if Arc::ptr_eq(&c, conn) => {}
        _ => return false, // replaced by a newer conn already (re-attached): this close is the stale one
    }
    let Some(mid) = MM.mid_of(token) else { return false };
    if !match_running(&mid, token) {
        return false;
    }
    SESS_LOST.lock().insert(token.to_string(), (clock::now(), mid.clone()));
    let cl = CLAN_CLOSING.lock().get(token).cloned();
    if let Some(cl) = cl {
        let now = clock::now();
        if now - cl.0 <= *CLAN_CLOSING_S && cl.1.as_deref().is_none_or(|m| m == mid) {
            // (11-23 A/B) the leave verdict comes after the re-attach window (SESSION_GRACE_S), long after CLAN_CLOSING_S: decide 'closed' now
            {
                let mut d = CLAN_CLOSE_DROPS.lock();
                if d.len() > 4000 {
                    d.clear();
                }
                d.insert((token.to_string(), mid.clone()));
            }
            dlog!(
                "CLAN closed",
                tag(token),
                "mid",
                mid,
                format!("- its session dropped {} s after its window-close notice: its own leave", py::fixed(clock::now() - cl.0, 1))
            );
            clan_closed_book(token, &mid);
        }
    }
    evt!("session-lost", mid, tag(token), format!("session socket lost in a running match; waiting {} s for it to re-attach", py::fixed(*SESSION_GRACE_S, 0)));
    true
}

/// What `session_reattach` set in the `st` dict beyond `ConnSt` (`st["menu"] = True; st["sent_name"] = shown; st["started"] = mid`)
#[derive(Debug, Clone)]
pub struct ReattachOut {
    pub sent_name: String,
    pub started: String,
}

// py: daemon.py:3141 session_reattach: Ok(None) = not a re-attach (python False); Ok(Some(out)) = handled (python True), st["menu"] = True
pub fn session_reattach(conn: &Conn, addr: Addr, st: &mut ConnSt) -> Result<Option<ReattachOut>, HErr> {
    let Some(tok) = st.token.clone().filter(|t| t.chars().count() == 64) else { return Ok(None) };
    if !MM.in_match(&tok) {
        return Ok(None);
    }
    let Some(mid) = MM.mid_of(&tok) else { return Ok(None) };
    if !match_running(&mid, &tok) {
        return Ok(None);
    }
    let lost = SESS_LOST.lock().get(&tok).cloned();
    let old = SESSIONS.conn(&tok);
    let now = clock::now();
    let fresh_lost = lost.as_ref().is_some_and(|l| l.1 == mid && now - l.0 <= *SESSION_GRACE_S + 15.0);
    let replaced = old.as_ref().is_some_and(|o| !Arc::ptr_eq(o, conn));
    if !fresh_lost && !replaced {
        return Ok(None); // nothing was lost: an ordinary menu announce
    }
    let shown = {
        let g = get_name(&tok);
        if g.is_empty() {
            st.name.clone()
        } else {
            g
        }
    };
    PRES.touch(&tok, &shown);
    SESSIONS.register(&tok, conn.clone());
    st.session = true;
    st.authed = true;
    SESS_LOST.lock().remove(&tok);
    let cnt = std::cmp::max(1, PRES.count() as u32);
    conn.sendall(&auth_reply_open(&shown, &msg(0x16, &cnt.to_le_bytes())))?;
    let why = match &lost {
        Some(l) => format!("session socket back after {} s", py::fixed(clock::now() - l.0, 1)),
        None => "(replaced a half-open socket)".to_string(),
    };
    dlog!("REATTACH", pa(addr), tag(&tok), "match", mid, why);
    let why2 = match &lost {
        Some(l) => format!("session socket re-attached after {} s", py::fixed(clock::now() - l.0, 1)),
        None => "(replaced a half-open socket)".to_string(),
    };
    evt!("session-back", mid, tag(&tok), why2);
    if let Some(l) = &lost {
        if l.1 == mid {
            reattach_missed_start(&tok, &mid, l.0, conn);
        }
    }
    Ok(Some(ReattachOut { sent_name: shown, started: mid }))
}

pub const REATTACH_START_DELAY_S: f64 = 2.0;

// py: daemon.py:3180 reattach_missed_start
pub fn reattach_missed_start(tok: &str, mid: &str, lost_at: f64, conn: &Conn) -> bool {
    let js = JIP_STARTED.lock().get(tok).cloned();
    let t_start = {
        let g = MM.lock.lock();
        match &js {
            Some(js) if js.0 == mid => Some(js.1), // a joiner: its own GAME-START
            _ if g.joining.contains_key(tok) => None, // a joiner whose own GAME-START has not gone out yet: maybe_game_start sends it
            _ => {
                if g.started.contains(mid) {
                    g.started_t.get(mid).copied()
                } else {
                    None
                }
            }
        }
    };
    let Some(t_start) = t_start else { return false };
    if t_start < lost_at {
        return false;
    }
    let (tok, mid, conn) = (tok.to_string(), mid.to_string(), conn.clone());
    let _ = std::thread::Builder::new().name("reattach-start".into()).stack_size(256 * 1024).spawn(move || {
        std::thread::sleep(std::time::Duration::from_secs_f64(REATTACH_START_DELAY_S));
        let running = match_running(&mid, &tok);
        let still = SESSIONS.conn(&tok).is_some_and(|c| Arc::ptr_eq(&c, &conn));
        if !running || MM.mid_of(&tok).as_deref() != Some(mid.as_str()) || !still {
            dlog!("REATTACH start: not re-sent to", tag(&tok), "match", mid, "(left / ended / socket replaced)");
            return;
        }
        if let Err(e) = conn.sendall(&msg(0x0a, b"\x01")) {
            dlog!("REATTACH start: send to", tag(&tok), "failed", crate::pyfmt::os_error_repr(&e));
            return;
        }
        dlog!(
            "REATTACH start: GAME-START 0x0a re-sent to",
            tag(&tok),
            "match",
            mid,
            format!("(it went out {} s after the socket was lost)", py::fixed(t_start - lost_at, 1))
        );
        evt!("session-back-start", mid, tag(&tok), "GAME-START re-sent: it went out while the session socket was down");
    });
    true
}

// ---------------------------------------------------------------- client / server version check
pub const OP_VERSION: u8 = 0xB0;
pub const OP_VERSION_REPLY: u8 = 0xB1;

pub fn db_dir() -> PathBuf {
    let p = PathBuf::from(&db::CFG.path);
    let a = std::path::absolute(&p).unwrap_or(p);
    a.parent().map(|x| x.to_path_buf()).unwrap_or_default()
}

// py: daemon.py:3228 _read_server_version
fn read_server_version() -> String {
    if let Ok(v) = std::env::var("T2R_VERSION") {
        return py::strip(&v).to_string();
    }
    match std::fs::read(crate::gen_modes::tools_dir().join("version.txt")) {
        Ok(b) => py::strip(&String::from_utf8_lossy(&b)).to_string(),
        Err(_) => String::new(),
    }
}

/// daemon.py:3236 SERVER_VERSION ("" = unknown)
pub static SERVER_VERSION: LazyLock<Mutex<String>> = LazyLock::new(|| Mutex::new(read_server_version()));
/// `SERVER_VERSION`
pub fn server_version() -> String {
    SERVER_VERSION.lock().clone()
}
/// (selftest) `d.SERVER_VERSION = v`
pub fn set_server_version(v: &str) {
    *SERVER_VERSION.lock() = v.to_string();
}
pub static REQUIRE_VERSION: LazyLock<bool> = LazyLock::new(|| crate::env_str("T2R_REQUIRE_VERSION", "0") == "1");

pub static VERSIONS_PATH: LazyLock<PathBuf> = LazyLock::new(|| db_dir().join("client_versions.json"));
pub static LINUX_PATH: LazyLock<PathBuf> = LazyLock::new(|| db_dir().join("linux_clients.json"));
pub static LANGS_PATH: LazyLock<PathBuf> = LazyLock::new(|| db_dir().join("client_langs.json"));

fn load_json(p: &std::path::Path) -> Option<serde_json::Value> {
    serde_json::from_slice(&std::fs::read(p).ok()?).ok()
}

/// daemon.py:3238 CLIENT_VERSIONS: token -> version string the launcher reported (oldest launch first)
pub static CLIENT_VERSIONS: LazyLock<Mutex<IndexMap<String, String>>> = LazyLock::new(|| {
    let mut m = IndexMap::new();
    if let Some(serde_json::Value::Object(o)) = load_json(&VERSIONS_PATH) {
        for (k, v) in o {
            m.insert(k, py_str(&v));
        }
    }
    Mutex::new(m)
});
/// daemon.py:3254 LINUX_CLIENTS
pub static LINUX_CLIENTS: LazyLock<Mutex<IndexSet<String>>> = LazyLock::new(|| {
    let mut s = IndexSet::new();
    if let Some(serde_json::Value::Array(a)) = load_json(&LINUX_PATH) {
        for v in a {
            s.insert(py_str(&v));
        }
    }
    Mutex::new(s)
});
/// daemon.py:3293 CLIENT_LANGS: token -> "pl" (English tokens have no entry)
pub static CLIENT_LANGS: LazyLock<Mutex<IndexMap<String, String>>> = LazyLock::new(|| {
    let mut m = IndexMap::new();
    if let Some(serde_json::Value::Object(o)) = load_json(&LANGS_PATH) {
        for (k, v) in o {
            if v.as_str() == Some("pl") {
                m.insert(k, "pl".to_string());
            }
        }
    }
    Mutex::new(m)
});
pub const CLIENT_LANGS_MAX: usize = 2000;
pub const CLIENT_VERSIONS_MAX: usize = 10000;
pub const LINUX_CLIENTS_MAX: usize = 500;

/// `str(x)` of a JSON value read back from a table file
fn py_str(v: &serde_json::Value) -> String {
    match v {
        serde_json::Value::String(s) => s.clone(),
        o => py::pyrepr(o),
    }
}

fn save_json(p: &std::path::Path, v: &serde_json::Value, what: &str) {
    let mut tmp = p.as_os_str().to_os_string();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    let r = std::fs::write(&tmp, py::dumps(v).as_bytes()).and_then(|_| std::fs::rename(&tmp, p));
    if let Err(e) = r {
        dlog!(what, "table not saved:", crate::pyfmt::os_error_repr(&e));
    }
}

// py: daemon.py:3263 _save_linux_clients
pub fn save_linux_clients() {
    let mut v: Vec<String> = LINUX_CLIENTS.lock().iter().cloned().collect();
    v.sort();
    save_json(&LINUX_PATH, &serde_json::json!(v), "LINUX");
}

// py: daemon.py:3272 _save_client_versions
pub fn save_client_versions() {
    let m: serde_json::Map<String, serde_json::Value> = CLIENT_VERSIONS.lock().iter().map(|(k, v)| (k.clone(), serde_json::Value::String(v.clone()))).collect();
    save_json(&VERSIONS_PATH, &serde_json::Value::Object(m), "VERSION");
}

// py: daemon.py:3327 _save_client_langs
pub fn save_client_langs() {
    let m: serde_json::Map<String, serde_json::Value> = CLIENT_LANGS.lock().iter().map(|(k, v)| (k.clone(), serde_json::Value::String(v.clone()))).collect();
    save_json(&LANGS_PATH, &serde_json::Value::Object(m), "LANG");
}

const VER_OK: &str = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-+";

// py: daemon.py:3281 parse_version_request: (version, token or None)
pub fn parse_version_request(pl: &[u8]) -> Option<(String, Option<String>)> {
    if pl.is_empty() || pl[0] == 0 || pl.len() < 1 + pl[0] as usize {
        return None;
    }
    let n = pl[0] as usize;
    let ver = latin1(&pl[1..1 + n]);
    if ver.is_empty() || ver.chars().count() > 40 || ver.chars().any(|c| !VER_OK.contains(c)) {
        return None;
    }
    let rest = latin1(crate::util::sl(pl, 1 + n, 1 + n + 64));
    let tok = if rest.chars().count() == 64 && rest.chars().all(|c| c.is_ascii_hexdigit()) { Some(rest) } else { None };
    Some((ver, tok))
}

// py: daemon.py:3305 norm_lang
pub enum LangIn<'a> {
    None,
    Int(i64),
    Bytes(&'a [u8]),
    Str(&'a str),
}

pub fn norm_lang_in(x: LangIn) -> &'static str {
    match x {
        LangIn::Bytes(b) => norm_lang_in(LangIn::Int(b.first().copied().unwrap_or(0) as i64)),
        LangIn::Int(i) => {
            if i == 1 {
                "pl"
            } else {
                "en"
            }
        }
        LangIn::None => "en",
        LangIn::Str(s) => {
            let s = py::strip(s).to_lowercase().replace('_', "-");
            if matches!(s.as_str(), "1" | "pl" | "pol" | "polish") || s.starts_with("pl-") {
                "pl"
            } else {
                "en"
            }
        }
    }
}

/// `norm_lang(x)` for a string
pub fn norm_lang(x: &str) -> &'static str {
    norm_lang_in(LangIn::Str(x))
}

// py: daemon.py:3317 lang_of
pub fn lang_of(token: &str) -> String {
    CLIENT_LANGS.lock().get(token).cloned().unwrap_or_else(|| "en".to_string())
}

// py: daemon.py:3321 note_lang (lang = the raw byte of the request)
pub fn note_lang(token: &str, lang: u8) {
    if token.chars().count() != 64 || !token.chars().all(|c| c.is_ascii_hexdigit()) {
        return;
    }
    let lang = norm_lang_in(LangIn::Int(lang as i64));
    {
        let cur = CLIENT_LANGS.lock().get(token).map(|s| s.as_str() == "pl").unwrap_or(false);
        if lang == if cur { "pl" } else { "en" } {
            return;
        }
    }
    let known = CLIENT_LANGS.lock().contains_key(token) || CLIENT_VERSIONS.lock().contains_key(token);
    if !known && !token_known(token) {
        return;
    }
    {
        let mut l = CLIENT_LANGS.lock();
        if lang == "pl" {
            if !l.contains_key(token) && l.len() >= CLIENT_LANGS_MAX {
                l.shift_remove_index(0); // oldest entry out
            }
            l.insert(token.to_string(), "pl".to_string());
        } else {
            l.shift_remove(token);
        }
    }
    save_soon();
}

// ---------------------------------------------------------------- texts
// py: daemon.py:3402 srv_text(key, lang, **kw): kw = [(name, value)]; {placeholders} filled from kw (values are never re-parsed)
pub fn srv_text(key: &str, lang: &str, kw: &[(&str, String)]) -> String {
    let Some((_, en, pl)) = texts::SRV_TEXT.iter().find(|r| r.0 == key) else { return String::new() };
    let t = if norm_lang(lang) == "pl" { *pl } else { *en };
    if kw.is_empty() {
        return t.to_string();
    }
    let mut out = String::with_capacity(t.len() + 16);
    let mut rest = t;
    while let Some(i) = rest.find('{') {
        out.push_str(&rest[..i]);
        let after = &rest[i + 1..];
        match after.find('}') {
            Some(j) => {
                let name = &after[..j];
                match kw.iter().find(|(k, _)| *k == name) {
                    Some((_, v)) => out.push_str(v),
                    None => {
                        out.push('{');
                        out.push_str(name);
                        out.push('}');
                    }
                }
                rest = &after[j + 1..];
            }
            None => {
                out.push('{');
                rest = after;
            }
        }
    }
    out.push_str(rest);
    out
}

// py: daemon.py:3408 srv_note: the op 0x11 note bytes of a text for this token's language
pub fn srv_note(key: &str, token: &str) -> Vec<u8> {
    srv_note_kw(key, token, &[])
}
pub fn srv_note_kw(key: &str, token: &str, kw: &[(&str, String)]) -> Vec<u8> {
    let s = srv_text(key, &lang_of(token), kw);
    let b = s.into_bytes();
    b[..b.len().min(0xf0)].to_vec()
}

// py: daemon.py:3412 version_mismatch_text
pub fn version_mismatch_text(client_ver: &str, linux: bool, lang: &str) -> String {
    srv_text(if linux { "ver_mismatch_linux" } else { "ver_mismatch" }, lang, &[("a", client_ver.to_string()), ("b", server_version())])
}

/// `CLIENT_VERSIONS.get(token)`
pub fn client_version(token: &str) -> Option<String> {
    CLIENT_VERSIONS.lock().get(token).cloned()
}

// py: daemon.py:3417 version_problem
pub fn version_problem(token: &str) -> Option<String> {
    let sv = server_version();
    if sv.is_empty() {
        return None;
    }
    let Some(cv) = client_version(token) else {
        return if *REQUIRE_VERSION { Some(srv_text("ver_missing", &lang_of(token), &[])) } else { None };
    };
    if cv == sv {
        None
    } else {
        Some(version_mismatch_text(&cv, LINUX_CLIENTS.lock().contains(token), &lang_of(token)))
    }
}

// py: daemon.py:3426 version_refusal
pub fn version_refusal(token: &str) -> Option<String> {
    let vp = version_problem(token)?;
    if vp.is_empty() {
        return None;
    }
    // only a match still loading counts: a search from a match that already started drops that match (the stale-match rule in the
    // MATCHMAKE handler), so it is a NEW search and refused
    if MM.in_match(token) && !(LOADED.lock().contains(token) || MM.match_age(token) > 120.0) {
        return None;
    }
    Some(srv_text("update_refusal", &lang_of(token), &[]))
}

// py: daemon.py:3447 _versions_evict_one: drop the least recently launched token that is not online (no session socket, not searching, not in a match)
fn versions_evict_one() {
    let first64: Vec<String> = CLIENT_VERSIONS.lock().keys().take(64).cloned().collect();
    for t in &first64 {
        let online = SESSIONS.conn(t).is_some() || MM.lock.lock().queue.contains_key(t) || MM.in_match(t);
        if !online {
            CLIENT_VERSIONS.lock().shift_remove(t);
            return;
        }
    }
    let mut v = CLIENT_VERSIONS.lock();
    if let Some(t) = v.keys().next().cloned() {
        v.shift_remove(&t);
    }
}

static SAVE_DUE: AtomicU64 = AtomicU64::new(0);
// py: daemon.py:3465 _save_soon: debounced, the tables are written at most every 5 s by the status loop
pub fn save_soon() {
    let cur = f64::from_bits(SAVE_DUE.load(Ordering::Relaxed));
    if cur == 0.0 {
        SAVE_DUE.store((clock::now() + 5.0).to_bits(), Ordering::Relaxed);
    }
}
// py: daemon.py:3468 _save_tables_if_due
pub fn save_tables_if_due(now: Option<f64>) {
    let now = now.unwrap_or_else(clock::now);
    let due = f64::from_bits(SAVE_DUE.load(Ordering::Relaxed));
    if due != 0.0 && now >= due {
        SAVE_DUE.store(0f64.to_bits(), Ordering::Relaxed);
        save_client_versions();
        save_linux_clients();
        save_client_langs();
    }
}
/// (selftest) the pending save time (0.0 = none)
pub fn save_due() -> f64 {
    f64::from_bits(SAVE_DUE.load(Ordering::Relaxed))
}

// py: daemon.py:3474 version_op: op 0xB0 -> the 0xB1 reply frame
pub fn version_op(pl: &[u8], addr: Option<&Addr>) -> Vec<u8> {
    let r = parse_version_request(pl);
    let sv = server_version();
    let srv = if sv.is_empty() { "dev".to_string() } else { sv.clone() };
    let Some((ver, mut tok)) = r else {
        return msg(OP_VERSION_REPLY, format!("ok=0 server={} client=? msg={}", srv, srv_text("ver_malformed", "en", &[])).as_bytes());
    };
    if let (Some(t), Some(a)) = (tok.clone(), addr) {
        if !CLIENT_VERSIONS.lock().contains_key(&t) && !LINUX_CLIENTS.lock().contains(&t) && !token_known(&t) && !NEW_TOKENS.allow(&ip_of(a), None) {
            dlog!("VERSION", pa(*a), "refused to record: too many new identities from this address"); // (a flood of random tokens grew client_versions.json)
            tok = None;
        }
    }
    let n = pl[0] as usize;
    if let Some(t) = &tok {
        if pl.len() >= 1 + n + 65 && pl[1 + n + 64..1 + n + 65] == *b"L" {
            let add = {
                let l = LINUX_CLIENTS.lock();
                !l.contains(t) && l.len() < LINUX_CLIENTS_MAX
            };
            if add {
                LINUX_CLIENTS.lock().insert(t.clone());
                save_soon();
                evt!("client-linux", tag(t), "- may queue without a Steam helper (server relay)");
            }
        }
    }
    if let Some(t) = &tok {
        let cur = client_version(t);
        if cur.as_deref() != Some(ver.as_str()) {
            evt!("client-version", tag(t), ver, "server", srv);
            let need_evict = {
                let v = CLIENT_VERSIONS.lock();
                !v.contains_key(t) && v.len() >= CLIENT_VERSIONS_MAX
            };
            if need_evict {
                versions_evict_one();
            }
            let mut v = CLIENT_VERSIONS.lock();
            v.shift_remove(t);
            v.insert(t.clone(), ver.clone());
            drop(v);
            save_soon();
        } else {
            let mut v = CLIENT_VERSIONS.lock();
            if v.keys().next_back().map(|k| k.as_str()) != Some(t.as_str()) {
                // R-SLZ8 / R-YKLX 11-25: each launch moves the token to the newest end (the json keeps the order)
                if let Some(x) = v.shift_remove(t) {
                    v.insert(t.clone(), x);
                }
                drop(v);
                save_soon();
            }
        }
    }
    let ok = sv.is_empty() || ver == sv;
    if !ok {
        dlog!("VERSION mismatch: client", ver, "server", sv, tok.as_deref().map(tag).unwrap_or_default());
    }
    let msg_t = if ok {
        String::new()
    } else {
        version_mismatch_text(&ver, false, &match &tok {
            Some(t) => lang_of(t),
            None => "en".to_string(),
        })
    };
    let text = format!("ok={} server={} client={} msg={}", if ok { 1 } else { 0 }, srv, ver, msg_t);
    let b = text.into_bytes();
    msg(OP_VERSION_REPLY, &b[..b.len().min(400)])
}
