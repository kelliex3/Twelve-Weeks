//! K1 ops: daemon.py:7398-7984 (notices / shutdown, private-lobby stub, mode queue, requeue status, interrogation ops, abort award,
//! clan leave excuse, leaving a running match, server list + auth replies).
use super::*;
use crate::{clock, hostmig, interrogation, modeq, notices, py};
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

// py: daemon.py:7557 NOTICES
pub static NOTICES: LazyLock<notices::Notices> = LazyLock::new(notices::Notices::new);
// py: daemon.py:7373 FINAL_WAIT_S
pub static FINAL_WAIT_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_FINAL_WAIT_S", 6.0));
/// py: daemon.py:7374 EXIT_FN = os._exit (tests replace it)
/// (selftest) forget the cached update target
pub fn reset_target() {
    *TARGET.lock() = (0.0, String::new());
}
pub static EXIT_FN: LazyLock<Mutex<fn(i32)>> = LazyLock::new(|| Mutex::new(|code| unsafe { libc::_exit(code) }));

static TARGET: LazyLock<Mutex<(f64, String)>> = LazyLock::new(|| Mutex::new((0.0, String::new())));

fn is_hex64(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| matches!(c, b'0'..=b'9' | b'a'..=b'f'))
}

/// `str(x or "")` of a JSON value
fn json_str_or_empty(v: Option<&serde_json::Value>) -> String {
    use serde_json::Value;
    match v {
        None | Some(Value::Null) | Some(Value::Bool(false)) => String::new(),
        Some(Value::String(s)) => s.clone(),
        Some(Value::Number(n)) => {
            if n.as_f64() == Some(0.0) {
                String::new()
            } else {
                n.to_string()
            }
        }
        Some(Value::Array(a)) if a.is_empty() => String::new(),
        Some(Value::Object(o)) if o.is_empty() => String::new(),
        Some(o) => py::pyrepr(o),
    }
}

// py: daemon.py:7376 update_target
pub fn update_target() -> String {
    let now = clock::now();
    {
        let t = TARGET.lock();
        if !(now - t.0 > 30.0) {
            return t.1.clone();
        }
    }
    let mut v = String::new();
    let db = std::path::Path::new(&db::CFG.path);
    let abs = if db.is_absolute() { db.to_path_buf() } else { std::env::current_dir().unwrap_or_default().join(db) };
    let ud = abs.parent().unwrap_or(std::path::Path::new("")).join("update-data");
    let read = || -> Option<String> {
        let ch: serde_json::Value = serde_json::from_slice(&std::fs::read(ud.join("channels.json")).ok()?).ok()?;
        let ch = json_str_or_empty(ch.as_object()?.get("stable"));
        if ch.is_empty() {
            return Some(String::new());
        }
        // 10-19: the target is the version.txt the published manifest ships, not the manifest's name
        let m: serde_json::Value = serde_json::from_slice(&std::fs::read(ud.join("manifests").join("stable").join(format!("{}.json", ch))).ok()?).ok()?;
        let files = m.as_object()?.get("files").and_then(|f| f.as_array()).cloned().unwrap_or_default();
        let vt: Vec<&serde_json::Value> = files.iter().filter(|e| e.as_object().is_some_and(|o| o.get("path").and_then(|p| p.as_str()) == Some("version.txt"))).collect();
        if let Some(e) = vt.first() {
            let sha = e.get("sha256").and_then(|s| s.as_str()).unwrap_or("");
            if is_hex64(sha) {
                let b = std::fs::read(ud.join("blobs").join(sha)).ok()?;
                return Some(py::strip(&py::utf8_replace(&b)).to_string());
            }
        }
        Some(String::new())
    };
    if let Some(x) = read() {
        v = x;
    }
    let mut t = TARGET.lock();
    t.0 = now;
    t.1 = if v.is_empty() { server_version() } else { v };
    t.1.clone()
}

// py: daemon.py:7398 notice_items
pub fn notice_items(token: &str, lang: Option<&str>) -> Vec<notices::Item> {
    let lang = match lang.filter(|l| !l.is_empty()) {
        Some(l) => l.to_string(),
        None => lang_of(token),
    };
    let cv = client_version(token).filter(|c| !c.is_empty());
    let tv = update_target();
    let sv = server_version();
    let cv_ne_sv = cv.as_deref().is_some_and(|c| !sv.is_empty() && c != sv);
    // not in a match: its searches fail -> the 'v' notice
    let refused = cv_ne_sv && version_refusal(token).is_some_and(|r| !r.is_empty());
    let stale = cv.as_deref().is_some_and(|c| !tv.is_empty() && c != tv) || (cv_ne_sv && !refused);
    NOTICES.active(stale, refused, &lang)
}

// py: daemon.py:7405 notice_op
pub fn notice_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 64 {
        return None;
    }
    let token = crate::d::latin1(&pl[..64]);
    if !is_token(&token) {
        return None;
    }
    readopt_claim(&token);
    PRES.touch_known(&token);
    if pl.len() > 65 {
        note_lang(&token, pl[65]); // the game's TEXT language / the launcher's UI language, whoever polled last
    }
    let items = notice_items(&token, None);
    let mut all: Vec<notices::Item> = SOCIAL.notes_for(&token, if pl.len() > 65 { pl[65] as i64 } else { 0 }, pl.len() > 66 && pl[66] == 1);
    all.truncate(2); // party / friend lines first (kind p)
    all.extend(items.into_iter().take(notices::MAX_LINES - 2));
    Some(msg(notices::OP_REPLY, &notices::Notices::reply_text(&all)))
}

// py: daemon.py:7420 notice_text
pub fn notice_text(kind: &str, param: i64, text: &str, lang: &str) -> String {
    match kind {
        "u" => srv_text("update_available", lang, &[]),
        "v" => srv_text("update_refusal", lang, &[]),
        "r" => {
            let m = std::cmp::max(1, py::floordiv(param + 59, 60));
            if !text.is_empty() {
                srv_text("restart_in_why", lang, &[("m", m.to_string()), ("r", text.to_string())])
            } else {
                srv_text("restart_in", lang, &[("m", m.to_string())])
            }
        }
        "s" => srv_text("shutdown", lang, &[]),
        _ => text.to_string(),
    }
}

// py: daemon.py:7430 notice_english
pub fn notice_english(kind: &str, param: i64, text: &str) -> String {
    notice_text(kind, param, text, "en")
}

// py: daemon.py:7433 push_menu_note
pub fn push_menu_note(text: &dyn Fn(&str) -> String) -> i64 {
    let mut n = 0;
    for (tok, c) in SESSIONS.conns() {
        if MM.in_match(&tok) {
            continue;
        }
        let t = text(&lang_of(&tok));
        let mut b = t.into_bytes();
        b.truncate(0xf0);
        if c.sendall(&msg(0x11, &b)).is_ok() {
            n += 1;
        }
    }
    n
}

// py: daemon.py:7448 notice_send (text_pl "" = no Polish form, as the admin passes it)
pub fn notice_send(text: &str, minutes: f64, seconds: Option<f64>, text_pl: &str) -> Option<i64> {
    let nid = NOTICES.add_text(text, minutes, seconds, Some(text_pl));
    if let Some(id) = nid {
        let mut en = py::clean_str(text, 140);
        if en.is_empty() {
            en = py::clean_str(text_pl, 140);
        }
        let mut pl = py::clean_str(text_pl, 140);
        if pl.is_empty() {
            pl = en.clone();
        }
        let extra = if pl != en { format!("| pl: {}", pl) } else { String::new() };
        evt!("notice", id, "free text", en, extra);
        push_menu_note(&|lang| if lang == "pl" { pl.clone() } else { en.clone() });
    }
    nid
}

// py: daemon.py:7464 shutdown_schedule
pub fn shutdown_schedule(minutes: f64, reason: &str, restart: bool, reason_pl: &str) -> notices::Shutdown {
    let r = NOTICES.schedule(minutes, reason, restart, Some(reason_pl));
    evt!("shutdown-scheduled", format!("in {} min", py::fixed(minutes, 1)), if restart { "restart" } else { "stop" }, py::clean_str(reason, 100));
    let (en, pl) = (py::clean_str(reason, 100), py::clean_str(reason_pl, 100));
    let sec = (minutes * 60.0).trunc() as i64;
    push_menu_note(&|lang| {
        let r = if lang == "pl" && !pl.is_empty() { &pl } else { &en };
        notice_text("r", sec, r, lang)
    });
    r
}

// py: daemon.py:7472 shutdown_cancel
pub fn shutdown_cancel() -> bool {
    let had = NOTICES.cancel();
    if had {
        evt!("shutdown-cancelled");
        push_menu_note(&|lang| srv_text("restart_cancel", lang, &[]));
    }
    had
}

// py: daemon.py:7480 reset_running_sessions
pub fn reset_running_sessions() -> i64 {
    let mut n = 0;
    for (tok, c) in SESSIONS.conns() {
        let live = MM.in_match(&tok) && MM.mid_of(&tok).is_some_and(|m| MM.lock.lock().started.contains(&m));
        if !live {
            continue;
        }
        c.reset(); // SO_LINGER 1/0 + close: a Winsock error on the client = the ext's session re-attach
        n += 1;
    }
    if n > 0 {
        dlog!("daemon exit: reset", n, "session socket(s) of running matches (clients re-attach after the restart)");
    }
    n
}

// py: daemon.py:7502 boot_all_sessions
pub fn boot_all_sessions() -> i64 {
    let mut n = 0;
    for (_tok, c) in SESSIONS.conns() {
        c.shutdown();
        c.close();
        n += 1;
    }
    dlog!("admin boot-all: closed", n, "session socket(s) gracefully (clients leave their match)");
    n
}

// py: daemon.py:7518 final_exit
pub fn final_exit(why: &str, restart: bool) {
    NOTICES.go_final(60.0);
    evt!("shutdown", why);
    live_save(None);
    push_menu_note(&|lang| notice_text("s", 0, "", lang));
    std::thread::sleep(std::time::Duration::from_secs_f64(*FINAL_WAIT_S));
    dlog!("daemon exit:", why);
    reset_running_sessions();
    std::thread::sleep(std::time::Duration::from_millis(800)); // the log flusher writes every 0.5 s
    let f = *EXIT_FN.lock();
    f(if restart { 4 } else { 0 });
}

// py: daemon.py:7530 shutdown_tick
pub fn shutdown_tick() {
    if let Some(d) = NOTICES.due() {
        final_exit(&format!("scheduled {}", if d.restart { "restart" } else { "stop" }), d.restart);
    }
}

static SIG_FLAG: std::sync::atomic::AtomicI32 = std::sync::atomic::AtomicI32::new(0);
extern "C" fn on_sig(signum: libc::c_int) {
    SIG_FLAG.store(signum, std::sync::atomic::Ordering::SeqCst);
}

// py: daemon.py:7535 install_exit_hooks (SIGINT / SIGTERM: one final notice, then exit; no console handler on Linux)
pub fn install_exit_hooks() {
    for s in [libc::SIGINT, libc::SIGTERM] {
        unsafe {
            libc::signal(s, on_sig as extern "C" fn(libc::c_int) as libc::sighandler_t);
        }
    }
    let _ = std::thread::Builder::new().name("signals".into()).stack_size(256 * 1024).spawn(|| loop {
        std::thread::sleep(std::time::Duration::from_millis(100));
        let s = SIG_FLAG.swap(0, std::sync::atomic::Ordering::SeqCst);
        if s != 0 {
            let _ = std::thread::Builder::new().name("final-exit".into()).stack_size(256 * 1024).spawn(move || final_exit(&format!("signal {}", s), false));
        }
    });
}

// py: daemon.py:7559 MODEQ
pub static MODEQ: LazyLock<modeq::ModeQ> = LazyLock::new(modeq::ModeQ::new);

// ---------------------------------------------------------------- private matches (daemon.py:7562, T2R_PRIVATE=0 stub)
pub struct PrivLobby {
    pub rules: serde_json::Value,
    pub code: String,
    pub members: Vec<String>,
}
/// private_lobby.Lobbies of the live server (T2R_PRIVATE=0): nothing is ever private
pub struct PrivStub;
impl PrivStub {
    pub fn on_enqueue(&self, _token: &str) {}
    pub fn split_ready(&self, ready: &[String]) -> (Vec<String>, Vec<String>) {
        (ready.to_vec(), Vec::new())
    }
    pub fn launches(&self, _priv_ready: &[String]) -> Vec<(PrivLobby, Vec<String>)> {
        Vec::new()
    }
    pub fn teams_for(&self, _lb: &PrivLobby, have: &[String], dflt: &dyn Fn(&[String]) -> (Vec<String>, Vec<String>)) -> (Vec<String>, Vec<String>) {
        dflt(have)
    }
    pub fn bound(&self, _lb: &PrivLobby, _mid: &str, _players: &[String]) {}
    pub fn match_over(&self, _mid: &str) {}
    pub fn aborted(&self, _mid: &str) {}
    pub fn jip_ok(&self, _token: &str, _mid: &str) -> bool {
        true
    }
    pub fn gc(&self, _keep: &dyn Fn(&str) -> bool) {}
    pub fn rules_of_mid(&self, _mid: &str) -> Option<serde_json::Value> {
        None
    }
    /// `PRIV.rule(mid, key)`: the rule value, None = not a private match
    pub fn rule(&self, _mid: &str, _key: &str) -> Option<i64> {
        None
    }
    /// `PRIV.is_private_mid(mid)`
    pub fn is_private_mid(&self, mid: &str) -> bool {
        PRIV_MIDS.lock().contains(mid) // (K2 selftest hook; empty in production)
    }
}
pub static PRIV: PrivStub = PrivStub;
/// py: daemon.py:7563 PRIVATE_ON (this port serves private lobbies as off whatever the variable says: tools/private_lobby.py is not ported)
pub static PRIVATE_ON: LazyLock<bool> = LazyLock::new(|| crate::env_on("T2R_PRIVATE", true));

// py: daemon.py:7616 priv_op
pub fn priv_op(pl: &[u8]) -> Option<Vec<u8>> {
    // private_lobby.Lobbies.parse: [64B token][u8 ver][u8 sub][ASCII arg]
    if pl.len() < 66 {
        return None;
    }
    let token = crate::d::latin1(&pl[..64]);
    if !is_token(&token) {
        return None;
    }
    Some(msg(0xF9, b"ok=0 err=off in=0"))
}

// py: daemon.py:7664 requeue_keep_mode
pub fn requeue_keep_mode(token: &str, mid: &str) {
    if let Some((mode, t)) = MODEQ.pref_of(token) {
        if MODEQ.clock() - t <= modeq::PREF_TTL {
            MODEQ.set(token, mode);
            return;
        }
    }
    if let Some(mm) = MODEQ.match_mode(mid) {
        if let Some(i) = modeq::modes().iter().position(|m| *m == mm) {
            MODEQ.set(token, i as i64);
        }
    }
}

// py: daemon.py:7677 requeue_status_text
pub fn requeue_status_text(token: &str) -> String {
    if !*REQUEUE {
        return String::new();
    }
    let now = clock::now();
    let lb = {
        let g = MM.lock.lock();
        let qd: Vec<String> = g.queue.keys().cloned().collect();
        let sil = |t: &str| silence(t, now);
        let inm = |t: &str| in_match_in(&g.matches, &g.departed, &g.ended, t);
        g.rq.lobby(token, &qd, &sil, &inm, Some(now))
    };
    match lb {
        None => String::new(),
        // (11-23 persistent lobby) rqs = s until the next match forms anyway (0 = not held), rqb = members back, rqp = members still on the screens
        Some(l) => format!(" rq=1 rqw={} rqs={} rqb={} rqp={}", l.wait, l.left, l.back, l.post),
    }
}

/// `len(CLAN_CLOSING) > 4000: drop the oldest 1000`
fn trim_closing(m: &mut IndexMap<String, (f64, Option<String>)>) {
    if m.len() > 4000 {
        let n = m.len().min(1000);
        m.drain(..n);
    }
}

// py: daemon.py:7691 modeq_op
pub fn modeq_op(pl: &[u8]) -> Option<Vec<u8>> {
    let (token, sub, mode) = modeq::ModeQ::parse(pl)?;
    if !is_token(&token) {
        return None;
    }
    if sub == 3 {
        // (11-23) "my game window is closing" (Alt+F4 / the X during a match): its session drop is its own leave
        let mid_ = MM.mid_of(&token);
        {
            let mut cc = CLAN_CLOSING.lock();
            cc.insert(token.clone(), (clock::now(), mid_.clone()));
            trim_closing(&mut cc);
        }
        dlog!("CLAN closing", tag(&token), "mid", mid_.as_deref().unwrap_or("None"), "- its game window is closing (a session drop now counts as its own leave)");
    }
    if sub == 1 {
        if MODEQ.set(&token, mode as i64) {
            let nm = if mode as i64 == modeq::ANY { "ANY (quick match)".to_string() } else { modeq::modes()[mode as usize].clone() };
            dlog!("MODEQ", tag(&token), "picks", nm);
        } else {
            dlog!("MODEQ", tag(&token), "bad mode", mode);
        }
    }
    let fresh: Vec<String> = {
        let g = MM.lock.lock();
        let now = clock::now();
        g.queue.keys().filter(|t| now - g.seen.get(*t).copied().unwrap_or(0.0) <= *QUEUE_FRESH).cloned().collect()
    };
    let mut q = MODEQ.counts(&fresh, &|t| PARTIES.leader(t));
    // user 11-06: each mode's number also counts the players in a running match of that mode (started, not ended, not departed)
    let mut playing: i64 = 0;
    if crate::env_str("T2R_MODEQ_PLAYING", "1") != "0" {
        let live: Vec<(String, usize)> = {
            let g = MM.lock.lock();
            g.matches
                .iter()
                .filter(|(mid, _)| g.started.contains(*mid) && !g.ended.contains(*mid))
                .map(|(mid, pl)| {
                    let dep = g.departed.get(mid);
                    (mid.clone(), pl.iter().filter(|t| !dep.is_some_and(|d| d.contains(*t)) && !fresh.contains(t)).count())
                })
                .collect()
        };
        for (mid, n) in live {
            let mm = MODEQ.match_mode(&mid).unwrap_or_else(|| modeq::modes()[0].clone());
            let k = modeq::modes().iter().position(|m| *m == mm).unwrap_or(0);
            if k < q.len() {
                q[k] += n as i64;
            }
            playing += n as i64;
        }
    }
    // 11-06: the match's own mode as soon as the match is assigned (mm=<row index>), only a live match of this token (never a finished one)
    let mut mm = String::new();
    if let Some(mid) = MM.mid_of(&token) {
        let live = !MM.lock.lock().ended.contains(&mid);
        if live {
            let mut m_ = MODEQ.match_mode(&mid).unwrap_or_default();
            if m_.is_empty() && modeq::forced() {
                m_ = py::strip(&crate::env_str("T2R_MODE", "")).to_lowercase();
            }
            let k_ = modeq::modes().iter().position(|m| *m == m_).unwrap_or(0);
            mm = format!(" mm={}", k_);
        }
    }
    let rqs = requeue_status_text(&token); // 11-12: the client's auto search + WAITING FOR PLAYERS... (ext requeue.c)
    let txt = modeq::ModeQ::reply(&q, MODEQ.get(&token, PARTIES.leader(&token).as_deref()), modeq::forced(), Some(fresh.len() as i64 + playing)) + &mm + &rqs;
    Some(msg(modeq::OP_REPLY, txt.as_bytes()))
}

// py: daemon.py:7745 interrogation_op
pub fn interrogation_op(pl: &[u8]) -> Option<Vec<u8>> {
    let (token, sub, seq, args) = INT.parse_request(pl)?;
    readopt_claim(&token); // 11-03: a poll right after a daemon restart must find the restored record
    int_rebind(&token);
    Some(msg(interrogation::OP_REPLY, INT.event(&token, sub, seq, &args).as_bytes()))
}

// py: daemon.py:7756 int_rebind
pub fn int_rebind(token: &str) {
    if let Some(cur) = MM.mid_of(token) {
        INT.rebind(token, &cur, &tag(token));
    }
}

// py: daemon.py:7777 ABORT_AWARDED
pub static ABORT_AWARDED: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));

// py: daemon.py:7779 abort_award
pub fn abort_award(mid: &str, others: &[String], host: Option<&str>, why: &str) -> bool {
    if mid.is_empty() || ABORT_AWARDED.lock().contains(mid) {
        return false;
    }
    let joining: HashSet<String> = {
        let g = MM.lock.lock();
        g.joining.iter().filter(|(_, (m, _))| m == mid).map(|(t, _)| t.clone()).collect()
    };
    // a join-in-progress player that has not started yet is not 'left over' (PT73 12:00)
    let mut others: Vec<String> = others.iter().filter(|p| !joining.contains(*p)).cloned().collect();
    // 11-23 A/B: a player who closed its window is not 'left over' - its session is just still inside the re-attach window.
    let closed: Vec<String> = {
        let cd = CLAN_CLOSE_DROPS.lock();
        others.iter().filter(|p| cd.contains(&((*p).clone(), mid.to_string()))).cloned().collect()
    };
    if !closed.is_empty() {
        others.retain(|p| !closed.contains(p));
        for p in &closed {
            if clan_leave_excuse(p, mid, "session lost", None).is_none() {
                CLAN_QUITTERS.lock().insert((p.clone(), mid.to_string()));
            }
        }
        dlog!("ABORT-AWARD", mid, "not for", tags(&closed), "(closed the game window)");
    }
    if others.is_empty() {
        return false;
    }
    {
        let g = MM.lock.lock();
        if g.ended.contains(mid) || !g.started.contains(mid) {
            return false; // a normal end already ran / the match never went live
        }
    }
    ABORT_AWARDED.lock().insert(mid.to_string());
    if let Some(h) = host.filter(|h| !h.is_empty()) {
        // (11-23: a closed window / an over-cap lost link counts too) a deliberate host quit stays 'left early'
        if clan_leave_excuse(h, mid, why, None).is_none() {
            CLAN_QUITTERS.lock().insert((h.to_string(), mid.to_string()));
        }
    }
    {
        // 11-23: the players it was ended for are never penalised for it (CLAN_ABORTED)
        let mut ca = CLAN_ABORTED.lock();
        for p in &others {
            ca.insert((p.clone(), mid.to_string()), why.to_string());
        }
        if ca.len() > 2000 {
            let n = ca.len().min(500);
            ca.drain(..n);
        }
    }
    clan_match_ended(&others, mid);
    MM.end(mid);
    INT.end(mid, Some(0xFF));
    GL.end(mid);
    let hs = match host.filter(|h| !h.is_empty()) {
        Some(h) => tag(h),
        None => "?".to_string(),
    };
    evt!("abort-award", mid, "host", hs, why, "| awarded:", tags(&others));
    dlog!("ABORT-AWARD", mid, "host", hs, why, "players", tags(&others));
    true
}

// py: daemon.py:7820 clan_why_deliberate
pub fn clan_why_deliberate(why: &str) -> bool {
    CLAN_DELIBERATE_WHY.iter().any(|d| why.contains(d))
}

static CLAN_LOST_COUNTED: LazyLock<Mutex<HashSet<(String, String)>>> = LazyLock::new(|| Mutex::new(HashSet::new()));

// py: daemon.py:7825 clan_leave_excuse (None = a deliberate leave that counts as 'left early')
pub fn clan_leave_excuse(token: &str, mid: &str, why: &str, at: Option<f64>) -> Option<String> {
    if *CLAN_NO_PENALTY {
        return Some("no clan penalties on this server (T2R_CLAN_NO_PENALTY)".to_string());
    }
    if !clan_why_deliberate(why) {
        let w = why.to_lowercase();
        let lost = w.contains("session lost") || w.contains("timed out");
        let closing = {
            let cl = CLAN_CLOSING.lock().get(token).cloned();
            cl.is_some_and(|c| clock::now() - c.0 <= *CLAN_CLOSING_S && (c.1.is_none() || c.1.as_deref() == Some(mid)))
        };
        let dropped = CLAN_CLOSE_DROPS.lock().contains(&(token.to_string(), mid.to_string()));
        if lost && (dropped || closing) {
            // (11-23) closed the game window = the player's own leave: the grace windows / the update excuse below still apply
        } else {
            let kind = if w.contains("session lost") {
                "crash / lost connection (session socket lost, never came back)".to_string()
            } else if w.contains("timed out") {
                "timeout (no packets from the player)".to_string()
            } else if w.contains("never loaded") || w.contains("never started") {
                "never loaded the level".to_string()
            } else if w.contains("admin") {
                "removed by an admin".to_string()
            } else {
                format!("not its own choice ({})", why)
            };
            if lost && *CLAN_LOST_EXCUSE_MAX > 0 {
                let now = clock::now();
                let key = (token.to_string(), mid.to_string());
                let mut ex: Vec<f64> = CLAN_LOST_EXCUSES.lock().get(token).cloned().unwrap_or_default().into_iter().filter(|t| now - t < *CLAN_LOST_EXCUSE_WINDOW).collect();
                {
                    // (one match counts once: abort_award asks again for the same leave)
                    let mut lc = CLAN_LOST_COUNTED.lock();
                    if !lc.contains(&key) {
                        lc.insert(key);
                        if lc.len() > 4000 {
                            lc.clear();
                        }
                        ex.push(now);
                    }
                }
                let n = ex.len() as i64;
                CLAN_LOST_EXCUSES.lock().insert(token.to_string(), ex);
                if n > *CLAN_LOST_EXCUSE_MAX {
                    dlog!(
                        "CLAN excuse",
                        tag(token),
                        "mid",
                        mid,
                        format!(
                            "{} refused - {} such exits in the last {} h (cap {}): counts as left early",
                            kind,
                            n,
                            py::fixed(*CLAN_LOST_EXCUSE_WINDOW / 3600.0, 0),
                            *CLAN_LOST_EXCUSE_MAX
                        )
                    );
                    return None;
                }
            }
            return Some(kind);
        }
    }
    // 10-28 (lead): the new client ext force-closes the game 60 s after the server's 'update available' notice reaches it in a match
    let cv = client_version(token).filter(|c| !c.is_empty());
    let tv = update_target();
    if let Some(cv) = &cv {
        if !tv.is_empty() && *cv != tv {
            return Some(format!("playing an older build after an update was published ({}, current {})", cv, tv));
        }
    }
    let now = match at.filter(|a| *a != 0.0) {
        Some(a) => a,
        None => clock::now(),
    };
    let (st_t, ck) = {
        let g = MM.lock.lock();
        (g.started_t.get(mid).copied(), g.clock.get(mid).copied())
    };
    if let Some(st) = st_t.filter(|s| *s != 0.0) {
        if !restored_t_has(mid) && now - st < *CLAN_START_GRACE_S {
            // (a restored match has no true start time)
            return Some(format!("left {} s after the match start (< {} s: could not reach the host / bad start)", py::fixed(now - st, 0), py::fixed(*CLAN_START_GRACE_S, 0)));
        }
    }
    // 11-25 (R-LLM6, match 8277f7ee): a join in progress has the same bad-start window from ITS start.
    let js = JIP_STARTED.lock().get(token).cloned();
    if let Some((jm, jt)) = js {
        if jm == mid && 0.0 <= now - jt && now - jt < *CLAN_START_GRACE_S {
            if *JIP_UNREACH_S > 0.0 {
                jip_unreach_note(token, mid, now);
            }
            return Some(format!(
                "left {} s after its join-in-progress start (< {} s: could not reach the host / bad start)",
                py::fixed(now - jt, 0),
                py::fixed(*CLAN_START_GRACE_S, 0)
            ));
        }
    }
    if let Some(ck) = ck {
        let left = ck.0 - (now - ck.3);
        if ck.2 || (now - ck.3 < 60.0 && left < *CLAN_END_GRACE_S) {
            return Some(format!("left in the last {} s of the round (decided)", py::fixed(f64::max(0.0, left), 0)));
        }
    }
    None
}

// py: daemon.py:7898 leave_running_match
pub fn leave_running_match(token: &str, why: &str) -> bool {
    GL.release(token); // a gun limit it held (tools/gunlimit.py) is free for its team at once
    let lost = SESS_LOST.lock().remove(token);
    let all_players = MM.players_of(token);
    let migrating = HM.enabled && HM.has(token);
    let was_host = if migrating { HM.host_token(token).as_deref() == Some(token) } else { all_players.as_ref().is_some_and(|p| p.first().map(|s| s.as_str()) == Some(token)) };
    let pre_mid = MM.mid_of(token);
    if let Some(pre_mid) = &pre_mid {
        let lastd = CLAN_LAST_DEPART.lock().get(pre_mid).cloned();
        let host_lost = HM.enabled && HM.host_lost_pending(pre_mid);
        // 11-25 (R-LLM6): a lost session is removed SESSION_GRACE_S (60 s) after its socket dropped; every window below is measured from the drop
        let at = lost.as_ref().filter(|l| &l.1 == pre_mid && why.contains("session lost")).map(|l| l.0);
        let t = match at.filter(|a| *a != 0.0) {
            Some(a) => a,
            None => clock::now(),
        };
        CLAN_LEFT_AT.lock().insert((token.to_string(), pre_mid.clone()), t);
        let mut why_ex = clan_leave_excuse(token, pre_mid, why, at);
        if why_ex.is_none() && (lastd.as_ref().is_some_and(|l| l.1 != token && t - l.0 <= *CLAN_EXCUSE_S) || host_lost) {
            why_ex = Some(if host_lost {
                "host lost (migration pending)".to_string()
            } else {
                let l = lastd.as_ref().unwrap();
                format!("{} s after {} left", py::fixed(t - l.0, 0), tag(&l.1))
            });
        }
        if why_ex.is_none() && migrating {
            // 10-28 07a228ac: the host hitched, the others moved on without it, its ext heard 'replaced as host - leaving the match' and sent the
            // 0x02 itself: that leave was the server's decision, not the player's
            if let Some(rep) = HM.replaced_age(token) {
                if rep <= hostmig::cfg().replaced_excuse_s {
                    why_ex = Some(format!("replaced as host by a migration {} s before (its game left on the server's word)", py::fixed(rep, 0)));
                }
            }
        }
        let key = (token.to_string(), pre_mid.clone());
        match why_ex {
            Some(wx) => {
                if CLAN_CLOSE_DROPS.lock().contains(&key) {
                    CLAN_QUITTERS.lock().remove(&key); // (a later excuse wins over the closed-window day: clan_closed_book)
                }
                CLAN_EXCUSED.lock().insert(key.clone());
                CLAN_EXCUSE_WHY.lock().insert(key, wx.clone());
                dlog!("CLAN excuse", tag(token), "mid", pre_mid, format!("leaves ({}): {}", why, wx), "- no left-early penalty");
            }
            None => {
                CLAN_EXCUSED.lock().remove(&key);
                dlog!("CLAN leave", tag(token), "mid", pre_mid, format!("deliberate ({}) - counts as left early unless a later excuse applies", why));
            }
        }
        CLAN_LAST_DEPART.lock().insert(pre_mid.clone(), (t, token.to_string()));
    }
    if migrating {
        HM.depart(token, why);
    }
    let mid = MM.depart(token, why.starts_with("left"));
    let others: Vec<String> = mid.as_deref().and_then(|m| MM.players_of_mid(m)).unwrap_or_default();
    if let (Some(mid), false) = (&mid, others.is_empty()) {
        if was_host && !(migrating && !HM.given_up(mid)) {
            let dep: HashSet<String> = MM.lock.lock().departed.get(mid).cloned().unwrap_or_default();
            let rest: Vec<String> = others.iter().filter(|p| *p != token && !dep.contains(*p)).cloned().collect();
            abort_award(mid, &rest, Some(token), &format!("host left ({}), no migration", why));
        } else {
            // (playtest 10-17: everyone else dropped out and the one player left got "YOU LEFT EARLY - NO SUPPLIES".) One player left in a running match =
            // the match is over for them: the normal-day award, as if it ended now.
            let (dep, moving): (HashSet<String>, Vec<String>) = {
                let g = MM.lock.lock();
                let dep: HashSet<String> = g.departed.get(mid).cloned().unwrap_or_default();
                // 11-10: players the admin is moving to the other team are coming back into this match
                let mut all: Vec<String> = dep.iter().cloned().collect();
                if !dep.contains(token) {
                    all.push(token.to_string());
                }
                all.sort();
                let moving = all.into_iter().filter(|t| g.move_team.get(t).is_some_and(|m| &m.0 == mid)).collect();
                (dep, moving)
            };
            let rest: Vec<String> = others.iter().filter(|p| *p != token && !dep.contains(*p)).cloned().collect();
            if rest.len() == 1 && moving.is_empty() {
                abort_award(mid, &rest, None, &format!("everyone else left ({})", why));
            } else if rest.len() == 1 {
                dlog!("ABORT-HOLD", mid, "only", tag(&rest[0]), "left, but", tags(&moving), "are being moved by the admin - no abort award");
            }
        }
    }
    if let Some(mid) = &mid {
        let stay = if others.is_empty() { "none (match removed)".to_string() } else { tags(&others) };
        evt!("left", mid, tag(token), why, format!("| others stay: {}", stay));
        if others.is_empty() {
            HM.end(mid);
            forget_match_state(all_players.as_deref().unwrap_or(&[]));
        }
    }
    !others.is_empty()
}

// py: daemon.py:7966 forget_match_state
pub fn forget_match_state(mid_players: &[String]) {
    if mid_players.is_empty() {
        return;
    }
    let key = key_of(mid_players);
    {
        let g = MM.lock.lock();
        if g.matches.values().any(|p| key_of(p) == key) {
            return; // the same people already formed a NEW match: its state is live
        }
    }
    match_dicts_pop(&key); // VOTES (under _vote_lock), MATCH_MAP, TIMING, MATCH_P2P, SID_KEEP
}

// ---------------------------------------------------------------- replies (daemon.py:7984-8040)
// py: daemon.py:7986 build_server_lists
pub fn build_server_lists() -> Vec<u8> {
    let gip = crate::env_str("T2R_PUB_IP", "209.25.141.16").into_bytes(); // playit TCP+UDP tunnel IP
    let uport: i64 = crate::env_parse("T2R_PUB_UDP_PORT", 15389i64);
    let tpl = crate::env_str("T2R_MAPIMG", "streaming/menus/to1/map-pvp-{img}.png");
    let maps = maps_table();
    let mut browser = vec![maps.len() as u8];
    for (name, img, lvl) in &maps {
        browser.extend(lp(name.as_bytes()));
        browser.extend(lp(format_img(&tpl, img).as_bytes()));
        browser.extend(lp(lvl.as_bytes()));
        browser.push(respawns_of(lvl));
    }
    browser.push(1);
    browser.extend(lp(&gip));
    browser.extend_from_slice(&(uport as u16).to_le_bytes());
    browser
}

/// `tpl.format(img=img)`
fn format_img(tpl: &str, img: &str) -> String {
    let mut out = String::new();
    let b: Vec<char> = tpl.chars().collect();
    let mut i = 0;
    while i < b.len() {
        if b[i] == '{' && i + 1 < b.len() && b[i + 1] == '{' {
            out.push('{');
            i += 2;
        } else if b[i] == '}' && i + 1 < b.len() && b[i + 1] == '}' {
            out.push('}');
            i += 2;
        } else if b[i] == '{' && b[i..].starts_with(&['{', 'i', 'm', 'g', '}']) {
            out.push_str(img);
            i += 5;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    out
}

fn name_field(name: &str) -> Vec<u8> {
    let n = safe_name(name);
    let n = if n.is_empty() { "PLAYER".to_string() } else { n };
    let mut b = n.into_bytes();
    b.truncate(31);
    b.resize(32, 0);
    b
}

// py: daemon.py:7999 auth_reply
pub fn auth_reply(name: &str, extra: &[u8]) -> Vec<u8> {
    // PROVEN sequence (reaches MP menu): 0x10 name + 0x08 both-lists + 0x0d done. Do NOT drop the 0x0d.
    let mut v = msg(0x10, &name_field(name));
    v.extend(msg(0x08, &build_server_lists()));
    v.extend_from_slice(extra);
    v.extend(msg(0x0d, &[1]));
    v
}

// py: daemon.py:8008 auth_reply_open
pub fn auth_reply_open(name: &str, extra: &[u8]) -> Vec<u8> {
    let mut v = msg(0x10, &name_field(name));
    v.extend(msg(0x08, &build_server_lists()));
    v.extend_from_slice(extra);
    v
}

// py: daemon.py:8014 lobby_snapshot
pub fn lobby_snapshot() -> Vec<u8> {
    let mut out = msg(0x03, &[]);
    for nm in PRES.online_names() {
        let mut b = nm.into_bytes();
        b.truncate(31);
        b.resize(32, 0);
        out.extend(msg(0x04, &b)); // TODO: full 0x1b0 player-rec fields
    }
    out
}

#[allow(dead_code)]
fn _unused(_: &HashMap<String, String>) {}
