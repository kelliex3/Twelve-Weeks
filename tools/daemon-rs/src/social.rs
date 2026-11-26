//! Friends + parties (op 0xD8 -> 0xD9), presence, party / lobby / match chat, private messages, chat mutes.
//!
//! The Rust version of social.py: pure logic over an injected host (`SocialHost`), no sockets. The `_SocialHost` adapter stays in the daemon.
//! Accounts: "s:<steamid>" when the Steam account of the token is known, else "t:<hash of the install id>".
//! Parties are in memory only; friends / seen rows / messages / the chat log live in the daemon's SQLite database.
//!
//! Where Python opens its own connection (`with self.h.db() as c`) the host provides `with_db(f)`: it opens a connection, runs `f`,
//! commits when `f` returns Ok and rolls back (and returns the error) when it returns Err, and always closes the connection.
//! Python errors that are not `sqlite3.Error` (an int too large for SQLite, ...) come back from `f` as `Err(exc())`; `handle`
//! reports them by returning `None` (Python raised: the daemon logs "SOCIAL ERR" and sends no reply).
use indexmap::IndexMap;
use parking_lot::Mutex;
use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{json, Map, Value};
use std::collections::{HashMap, HashSet, VecDeque};
use std::path::PathBuf;
use std::sync::Arc;

use crate::py::{is_py_space, repr_str, strip};

pub const OP_REQ: u8 = 0xD8;
pub const OP_REPLY: u8 = 0xD9;
pub const ACCT_CACHE_S: f64 = 10.0;
pub const S_STATE: i64 = 1;
pub const S_SEARCH: i64 = 2;
pub const S_ADD: i64 = 3;
pub const S_ACCEPT: i64 = 4;
pub const S_REMOVE: i64 = 5;
pub const S_INVITE: i64 = 6;
pub const S_JOIN: i64 = 7;
pub const S_DECLINE: i64 = 8;
pub const S_LEAVE: i64 = 9;
pub const S_KICK: i64 = 10;
pub const S_PROMOTE: i64 = 11;
pub const S_READY: i64 = 12;
pub const S_MSG: i64 = 13;
pub const S_CHAT: i64 = 14;
pub const S_PSAY: i64 = 15;
pub const S_PCHAT: i64 = 16;
pub const S_MLOG: i64 = 17;
pub const S_LSAY: i64 = 18;
pub const S_LCHAT: i64 = 19;
pub const S_MUTE: i64 = 20;
/// `SUB_NAMES[sub]`
pub fn sub_name(sub: i64) -> Option<&'static str> {
    Some(match sub {
        1 => "state",
        2 => "search",
        3 => "add",
        4 => "accept",
        5 => "remove",
        6 => "invite",
        7 => "join",
        8 => "decline",
        9 => "leave",
        10 => "kick",
        11 => "promote",
        12 => "ready",
        13 => "msg",
        14 => "chat",
        15 => "psay",
        16 => "pchat",
        17 => "mlog",
        18 => "lsay",
        19 => "lchat",
        20 => "mute",
        _ => return None,
    })
}
pub const LCHAT_IDLE_S: f64 = 900.0;
/// `WRITE_SUBS`
pub const WRITE_SUBS: [i64; 12] = [S_ADD, S_ACCEPT, S_REMOVE, S_INVITE, S_JOIN, S_DECLINE, S_LEAVE, S_KICK, S_PROMOTE, S_READY, S_MSG, S_PSAY];
pub fn is_write_sub(sub: i64) -> bool {
    WRITE_SUBS.contains(&sub)
}
pub const PRESENCE_TTL: f64 = 60.0;
pub const SEEN_WRITE_S: f64 = 30.0;
pub const INVITE_TTL: f64 = 300.0;
pub const FRIEND_MAX: i64 = 100;
pub const REQUEST_MAX: i64 = 30;
pub const OPS_PER_MIN: usize = 40;
pub const NOTE_BASE: i64 = 1000000;
pub const NOTE_KEEP_S: f64 = 30.0;
pub const SEARCH_MAX: usize = 8;
pub const PARTY_GRACE: f64 = 45.0;
/// `RANK[status]`
fn rank(status: &str) -> i64 {
    match status {
        "match" => 0,
        "search" => 1,
        "menu" => 2,
        "launcher" => 3,
        _ => 4,
    }
}
pub const MSG_LEN: usize = 200;
pub const PCHAT_KEEP: usize = 50;
pub const PCHAT_SHOW: usize = 20;
pub const MLOG_KEEP: usize = 200;
pub const RECENT_MAX: i64 = 10;
pub const MSG_PER_MIN: usize = 20;
pub const MSG_KEEP: i64 = 100;
pub const MSG_AGE_S: f64 = (30 * 86400) as f64;
pub const CHAT_SHOW: i64 = 60;
pub const CHAT_BYTES: usize = 24000;
pub const MUTE_RELOAD_S: f64 = 5.0;
pub const CHATLOG_AGE_S: f64 = (14 * 86400) as f64;
pub const CHATLOG_FLUSH_S: f64 = 5.0;
pub const MSG_NOTE_LEN: usize = 90;
pub const RESTORE_GRACE: f64 = 300.0;

/// `NOTES[kind]` = (English, Polish); `{n}` = a player name, `{t}` = a message text.
fn note_tpl(kind: &str) -> Option<(&'static str, &'static str)> {
    Some(match kind {
        "freq" => ("{n} sent you a friend request. Open the FRIENDS page of the launcher.", "{n} wysyła ci zaproszenie do znajomych. Otwórz stronę ZNAJOMI w launcherze."),
        "facc" => ("{n} accepted your friend request.", "{n} przyjmuje twoje zaproszenie do znajomych."),
        "pinv" => ("PARTY: {n} invited you to a party. Open the FRIENDS page of the launcher to join.", "GRUPA: {n} zaprasza cię do grupy. Otwórz stronę ZNAJOMI w launcherze, aby dołączyć."),
        "pjoin" => ("PARTY: {n} joined your party.", "GRUPA: {n} dołącza do grupy."),
        "pleft" => ("PARTY: {n} left the party.", "GRUPA: {n} opuszcza grupę."),
        "pkick" => ("PARTY: you were removed from the party.", "GRUPA: zostajesz usunięty z grupy."),
        "pdis" => ("PARTY: the party was disbanded.", "GRUPA: grupa została rozwiązana."),
        "plead" => ("PARTY: {n} is the party leader now.", "GRUPA: {n} jest teraz liderem grupy."),
        "pq" => ("PARTY: leader {n} is searching. Press READY UP to join.", "GRUPA: lider {n} szuka meczu. Wciśnij GOTOWY, aby dołączyć."),
        "pqm" => ("PARTY: {n} is searching. Press READY UP to join.", "GRUPA: {n} szuka meczu. Wciśnij GOTOWY, aby dołączyć."),
        "kicked" => ("{n}", "{n}"),
        "pwait" => ("PARTY: waiting for {n} to search.", "GRUPA: czekamy, aż {n} zacznie szukać."),
        "pneed" => (
            "PARTY: waiting for opponents - {n} more player(s) needed so that both teams are even.",
            "GRUPA: czekamy na przeciwników - brakuje graczy: {n}, aby drużyny były równe.",
        ),
        "pingfar" => ("No players within {n} ms right now - still searching.", "Obecnie brak graczy z pingiem do {n} ms - szukam dalej."),
        "msg" => ("MESSAGE FROM {n}: {t}", "WIADOMOŚĆ OD {n}: {t}"),
        _ => return None,
    })
}

/// `NOTES_INGAME[kind]`
fn note_tpl_ingame(kind: &str) -> Option<(&'static str, &'static str)> {
    Some(match kind {
        "freq" => ("{n} sent you a friend request. Press P to open FRIENDS.", "{n} wysyła ci zaproszenie do znajomych. Wciśnij P, aby otworzyć ZNAJOMYCH."),
        "pinv" => (
            "PARTY: {n} invited you to a party. Press P to open FRIENDS and join.",
            "GRUPA: {n} zaprasza cię do grupy. Wciśnij P, aby otworzyć ZNAJOMYCH i dołączyć.",
        ),
        _ => return None,
    })
}

// ---------------------------------------------------------------- small helpers

/// `str.split()` (no argument): runs of Python whitespace.
fn split_ws(s: &str) -> Vec<&str> {
    s.split(is_py_space).filter(|w| !w.is_empty()).collect()
}

/// `s.rstrip()`
fn rstrip(s: &str) -> &str {
    s.trim_end_matches(is_py_space)
}

fn is_ctrl(c: char) -> bool {
    let u = c as u32;
    u <= 0x1f || (0x7f..=0x9f).contains(&u) || (0x200b..=0x200f).contains(&u) || (0x2028..=0x202e).contains(&u) || (0x2060..=0x206f).contains(&u) || u == 0xfeff
}

/// `" ".join(_CTRL.sub(" ", text or "").split())`
fn ctrl_words(text: &str) -> String {
    let t: String = text.chars().map(|c| if is_ctrl(c) { ' ' } else { c }).collect();
    split_ws(&t).join(" ")
}

fn game_quote(c: char) -> Option<&'static str> {
    Some(match c {
        '\u{2018}' | '\u{2019}' => "'",
        '\u{201c}' | '\u{201d}' => "\"",
        '\u{2013}' | '\u{2014}' => "-",
        '\u{2026}' => "...",
        _ => return None,
    })
}

// py: social.py:132 ingame_text
/// A message as the game's nametag font can draw it: Latin letters only, cut to `n` characters.
pub fn ingame_text(s: &str, n: usize) -> String {
    let mut t = String::new();
    for c in s.chars() {
        match game_quote(c) {
            Some(r) => t.push_str(r),
            None => t.push(c),
        }
    }
    let kept: String = t.chars().filter(|&ch| (ch as u32) < 0x250 && ch != '|').collect();
    let s = split_ws(&kept).join(" ");
    if s.chars().count() <= n {
        s
    } else {
        let cut: String = s.chars().take(n.saturating_sub(3)).collect();
        format!("{}...", rstrip(&cut))
    }
}

// py: social.py:147 acct_hash
pub fn acct_hash(token_id: &str) -> String {
    use sha2::{Digest, Sha256};
    let h = Sha256::digest(token_id.as_bytes());
    let mut s = String::from("t:");
    for b in h.iter().take(8) {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

// py: social.py:151 _token_id
fn token_id_of(token: &str) -> String {
    let t = token.to_lowercase();
    let ch: Vec<char> = t.chars().collect();
    if ch.len() == 64 {
        ch[40..64].iter().collect()
    } else {
        t
    }
}

/// SHA-1 (hashlib.sha1) of `data`: the first 4 digest bytes.
fn sha1_head(data: &[u8]) -> [u8; 4] {
    let mut h: [u32; 5] = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
    let mut msg = data.to_vec();
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&((data.len() as u64) * 8).to_be_bytes());
    for chunk in msg.chunks(64) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes([chunk[4 * i], chunk[4 * i + 1], chunk[4 * i + 2], chunk[4 * i + 3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let (mut a, mut b, mut c, mut d, mut e) = (h[0], h[1], h[2], h[3], h[4]);
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }
    h[0].to_be_bytes()
}

/// `int(hashlib.sha1(str(key).encode()).hexdigest()[:7], 16)`
fn sha1_hex7(key: &str) -> i64 {
    let d = sha1_head(key.as_bytes());
    ((d[0] as i64) << 20) | ((d[1] as i64) << 12) | ((d[2] as i64) << 4) | ((d[3] as i64) >> 4)
}

/// Python `float(s)` (underscores between digits, inf / nan, surrounding whitespace).
fn py_float(s: &str) -> Option<f64> {
    let t = strip(s);
    if t.is_empty() {
        return None;
    }
    let b: Vec<char> = t.chars().collect();
    let mut clean = String::new();
    for (i, &c) in b.iter().enumerate() {
        if c == '_' {
            let ok = i > 0 && i + 1 < b.len() && b[i - 1].is_ascii_digit() && b[i + 1].is_ascii_digit();
            if !ok {
                return None;
            }
        } else {
            clean.push(c);
        }
    }
    let low = clean.to_ascii_lowercase();
    let body = low.trim_start_matches(['+', '-']);
    if low.len() - body.len() > 1 {
        return None;
    }
    match body {
        "inf" | "infinity" => return Some(if low.starts_with('-') { f64::NEG_INFINITY } else { f64::INFINITY }),
        "nan" => return Some(f64::NAN),
        _ => {}
    }
    if !body.chars().all(|c| c.is_ascii_digit() || matches!(c, '.' | 'e' | '+' | '-')) {
        return None;
    }
    clean.parse::<f64>().ok()
}

/// A Python exception that is not a `sqlite3.Error`, carried through `Result<_, rusqlite::Error>`.
#[derive(Debug)]
pub struct PyExc;
impl std::fmt::Display for PyExc {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "python exception")
    }
}
impl std::error::Error for PyExc {}

/// The error a host `with_db` closure returns for a Python exception that is not a `sqlite3.Error`.
pub fn exc() -> rusqlite::Error {
    rusqlite::Error::ToSqlConversionFailure(Box::new(PyExc))
}

fn is_exc(e: &rusqlite::Error) -> bool {
    matches!(e, rusqlite::Error::ToSqlConversionFailure(_))
}

/// `repr(sqlite3 exception)` as far as it can be told: `OperationalError('message')`.
pub fn exc_repr(e: &rusqlite::Error) -> String {
    let msg = match e {
        rusqlite::Error::SqliteFailure(_, Some(m)) => m.clone(),
        other => other.to_string(),
    };
    format!("OperationalError({})", repr_str(&msg))
}

/// `int(text)` that must fit a SQLite INTEGER (Python raises OverflowError binding a bigger one).
fn sql_int(s: &str) -> rusqlite::Result<i64> {
    s.parse::<i64>().map_err(|_| exc())
}

/// `int(text)` bound to `sql`: sqlite compiles the statement first (a missing table is a `sqlite3.Error`), then the bind raises for a too big int.
fn bind_int(c: &Connection, sql: &str, s: &str) -> rusqlite::Result<i64> {
    c.prepare(sql)?;
    sql_int(s)
}

/// `c.commit()`: nothing to do in autocommit mode.
fn commit(c: &Connection) -> rusqlite::Result<()> {
    if !c.is_autocommit() {
        c.execute_batch("COMMIT")?;
    }
    Ok(())
}

fn rollback(c: &Connection) {
    if !c.is_autocommit() {
        let _ = c.execute_batch("ROLLBACK");
    }
}

fn is_acct_key(s: &str) -> bool {
    // re.fullmatch(r"[st]:[0-9a-f]{1,20}", s)
    let b = s.as_bytes();
    if b.len() < 3 || b.len() > 22 || !(b[0] == b's' || b[0] == b't') || b[1] != b':' {
        return false;
    }
    b[2..].iter().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(c))
}

fn is_steam_text(s: &str) -> bool {
    // re.fullmatch(r"\d{15,20}", s) (ASCII digits only here)
    (15..=20).contains(&s.len()) && s.bytes().all(|c| c.is_ascii_digit())
}

fn is_digits(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|c| c.is_ascii_digit())
}

/// `bytes.fromhex(token)` for the plain case (even length, no spaces).
fn from_hex(s: &str) -> Option<Vec<u8>> {
    let b = s.as_bytes();
    if b.len() % 2 != 0 {
        return None;
    }
    let mut out = Vec::with_capacity(b.len() / 2);
    for p in b.chunks(2) {
        let h = (p[0] as char).to_digit(16)?;
        let l = (p[1] as char).to_digit(16)?;
        out.push((h * 16 + l) as u8);
    }
    Some(out)
}

// py: social.py:1303 token_name_fallback
/// The name inside a T2N token (the launcher's tokens carry it), else PLAYER.
pub fn token_name_fallback(token: &str) -> String {
    if let Some(raw) = from_hex(token) {
        if raw.starts_with(b"T2N:") {
            let rest = &raw[4..];
            let first = rest.split(|&b| b == 0).next().unwrap_or(&[]);
            let s: String = crate::roles::utf8_ignore(first).chars().take(15).collect();
            return if s.is_empty() { "PLAYER".to_string() } else { s };
        }
    }
    "PLAYER".to_string()
}

// py: social.py:1315 reply_json
pub fn reply_json(d: &Value) -> Vec<u8> {
    crate::py::dumps_compact_utf8(d).into_bytes()
}

// ---------------------------------------------------------------- parties

/// A party event queued for Social: (to token, kind, subject token).
pub type PartyEvent = (String, &'static str, Option<String>);

#[derive(Default)]
struct PInner {
    of: IndexMap<String, String>,
    inv: IndexMap<String, IndexMap<String, f64>>,
    leaders: IndexMap<String, String>,
    ready: HashSet<String>,
    events: Vec<PartyEvent>,
    grace: HashMap<String, f64>,
}

pub type Lookup = Box<dyn Fn(&[u8]) -> Option<String> + Send + Sync>;

/// token -> party id, leader per party, invitations {invitee token: {inviter token: time}}.
// py: social.py:168 Parties
pub struct Parties {
    pub max: i64,
    now: Box<dyn Fn() -> f64 + Send + Sync>,
    lookup: Option<Lookup>,
    g: Mutex<PInner>,
}

impl PInner {
    fn pid(&self, token: &str) -> Option<String> {
        self.of.get(token).filter(|p| !p.is_empty()).cloned()
    }

    fn mem(&self, pid: &str) -> Vec<String> {
        self.of.iter().filter(|(_, p)| p.as_str() == pid).map(|(t, _)| t.clone()).collect()
    }

    fn leader_of(&self, pid: &str) -> Option<String> {
        let mem = self.mem(pid);
        let l = self.leaders.get(pid);
        match l {
            Some(l) if mem.contains(l) => Some(l.clone()),
            _ => mem.first().cloned(),
        }
    }

    fn ev(&mut self, to: &str, kind: &'static str, subject: Option<&str>) {
        self.events.push((to.to_string(), kind, subject.map(|s| s.to_string())));
    }

    // py: social.py:279 Parties._leave
    fn leave_inner(&mut self, token: &str, why: &str) -> Option<String> {
        let pid = self.pid(token)?;
        let was_leader = self.leader_of(&pid).as_deref() == Some(token);
        self.of.shift_remove(token);
        self.ready.remove(token);
        let rest = self.mem(&pid);
        if was_leader && why == "timeout" && rest.len() >= 2 {
            self.leaders.insert(pid.clone(), rest[0].clone());
            for t in &rest {
                self.ev(t, "plead", Some(&rest[0]));
            }
        } else if was_leader {
            for t in &rest {
                self.of.shift_remove(t.as_str());
                self.ready.remove(t.as_str());
                self.ev(t, "pdis", Some(token));
            }
            self.leaders.shift_remove(&pid);
            return Some(pid);
        } else {
            for t in &rest {
                self.ev(t, "pleft", Some(token));
            }
        }
        if why == "kicked" {
            self.ev(token, "pkick", None);
        }
        if rest.len() < 2 {
            for t in &rest {
                self.of.shift_remove(t.as_str());
                self.ready.remove(t.as_str());
                self.ev(t, "pdis", Some(token));
            }
            self.leaders.shift_remove(&pid);
        }
        Some(pid)
    }
}

impl Parties {
    // py: social.py:175 Parties.__init__ (now = time.time)
    pub fn new(max_size: Option<i64>, lookup: Option<Lookup>) -> Parties {
        Parties::with_clock(max_size, lookup, Box::new(crate::clock::now))
    }

    pub fn with_clock(max_size: Option<i64>, lookup: Option<Lookup>, now: Box<dyn Fn() -> f64 + Send + Sync>) -> Parties {
        let max = match max_size {
            Some(m) if m != 0 => m.max(1),
            _ => 4,
        };
        Parties { max, now, lookup, g: Mutex::new(PInner::default()) }
    }

    // --- reads
    pub fn pid(&self, token: &str) -> Option<String> {
        self.g.lock().pid(token)
    }

    pub fn members(&self, token: &str) -> Vec<String> {
        let g = self.g.lock();
        match g.pid(token) {
            Some(pid) => g.mem(&pid),
            None => vec![token.to_string()],
        }
    }

    pub fn leader_of(&self, pid: &str) -> Option<String> {
        self.g.lock().leader_of(pid)
    }

    pub fn leader(&self, token: &str) -> Option<String> {
        let g = self.g.lock();
        g.pid(token).and_then(|pid| g.leader_of(&pid))
    }

    pub fn is_leader(&self, token: &str) -> bool {
        self.leader(token).as_deref() == Some(token)
    }

    pub fn size(&self, token: &str) -> usize {
        let g = self.g.lock();
        match g.pid(token) {
            Some(pid) => g.mem(&pid).len(),
            None => 0,
        }
    }

    /// Is `token` flagged ready (`token in PARTIES.ready`).
    pub fn is_ready(&self, token: &str) -> bool {
        self.g.lock().ready.contains(token)
    }

    /// `len(PARTIES.of)`
    pub fn of_len(&self) -> usize {
        self.g.lock().of.len()
    }

    /// `len(PARTIES.inv)`
    pub fn inv_len(&self) -> usize {
        self.g.lock().inv.len()
    }

    /// `tuple(sorted(PARTIES.leaders.items()))`
    pub fn leaders_sorted(&self) -> Vec<(String, String)> {
        let g = self.g.lock();
        let mut v: Vec<(String, String)> = g.leaders.iter().map(|(a, b)| (a.clone(), b.clone())).collect();
        v.sort();
        v
    }

    /// `list(PARTIES.grace)`
    pub fn grace_tokens(&self) -> Vec<String> {
        self.g.lock().grace.keys().cloned().collect()
    }

    // py: social.py:215 Parties.invites_for
    /// [(inviter token, age s)] of the live invitations to `token`, youngest first.
    pub fn invites_for(&self, token: &str) -> Vec<(String, f64)> {
        let now = (self.now)();
        let g = self.g.lock();
        let mut out = Vec::new();
        if let Some(d) = g.inv.get(token) {
            for (inviter, tm) in d {
                if now - tm > INVITE_TTL {
                    continue;
                }
                if let Some(pid) = g.pid(inviter) {
                    if g.leader_of(&pid).as_deref() != Some(inviter.as_str()) {
                        continue;
                    }
                }
                out.push((inviter.clone(), now - tm));
            }
        }
        out.sort_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal));
        out
    }

    // --- changes
    // py: social.py:236 Parties.drain
    pub fn drain(&self) -> Vec<PartyEvent> {
        std::mem::take(&mut self.g.lock().events)
    }

    // py: social.py:241 Parties.invite_token
    /// `None` when the invitation was stored, else an error code.
    pub fn invite_token(&self, inviter: &str, invitee: &str) -> Option<&'static str> {
        let now = (self.now)();
        let mut g = self.g.lock();
        if invitee == inviter {
            return Some("self");
        }
        let pid = g.pid(inviter);
        if let Some(pid) = &pid {
            if g.leader_of(pid).as_deref() != Some(inviter) {
                return Some("notleader");
            }
            if g.of.get(invitee).map(|p| p.as_str()) == Some(pid.as_str()) {
                return Some("already");
            }
            if g.mem(pid).len() as i64 >= self.max {
                return Some("full");
            }
        }
        g.inv.entry(invitee.to_string()).or_default().insert(inviter.to_string(), now);
        g.ev(invitee, "pinv", Some(inviter));
        None
    }

    // py: social.py:257 Parties.accept_token
    /// `Ok(party id)` or `Err(error code)`.
    pub fn accept_token(&self, invitee: &str, inviter: &str) -> Result<String, &'static str> {
        let now = (self.now)();
        let mut g = self.g.lock();
        let tm = g.inv.get(invitee).and_then(|d| d.get(inviter)).copied();
        match tm {
            Some(tm) if now - tm <= INVITE_TTL => {}
            _ => return Err("expired"),
        }
        let mut pid = g.pid(inviter);
        if let Some(p) = &pid {
            if g.leader_of(p).as_deref() != Some(inviter) {
                return Err("expired");
            }
            if g.of.get(invitee).map(|x| x.as_str()) == Some(p.as_str()) {
                g.inv.shift_remove(invitee);
                return Ok(p.clone());
            }
            if g.mem(p).len() as i64 >= self.max {
                return Err("full");
            }
        }
        if g.of.contains_key(invitee) {
            g.leave_inner(invitee, "left");
        }
        let pid = match pid.take() {
            Some(p) => p,
            None => {
                let p = crate::rnd::token_hex(3);
                g.of.insert(inviter.to_string(), p.clone());
                g.leaders.insert(p.clone(), inviter.to_string());
                p
            }
        };
        for t in g.mem(&pid) {
            if t != invitee {
                g.ev(&t, "pjoin", Some(invitee));
            }
        }
        g.of.insert(invitee.to_string(), pid.clone());
        g.inv.shift_remove(invitee);
        g.ready.insert(invitee.to_string());
        Ok(pid)
    }

    // py: social.py:283 Parties.decline_token
    pub fn decline_token(&self, invitee: &str, inviter: &str) -> bool {
        let mut g = self.g.lock();
        if let Some(d) = g.inv.get_mut(invitee) {
            if d.shift_remove(inviter).is_some() {
                if d.is_empty() {
                    g.inv.shift_remove(invitee);
                }
                return true;
            }
        }
        false
    }

    // py: social.py:329 Parties.leave
    /// The player leaves its party (a leader disbands it) and its invitations (given and received) are void. True when it was in one.
    pub fn leave(&self, token: &str) -> bool {
        let mut g = self.g.lock();
        let had = g.of.contains_key(token);
        g.leave_inner(token, "left");
        g.inv.shift_remove(token);
        let keys: Vec<String> = g.inv.keys().cloned().collect();
        for invitee in keys {
            let empty = match g.inv.get_mut(&invitee) {
                Some(d) => {
                    d.shift_remove(token);
                    d.is_empty()
                }
                None => false,
            };
            if empty {
                g.inv.shift_remove(&invitee);
            }
        }
        had
    }

    // py: social.py:340 Parties.kick
    pub fn kick(&self, leader: &str, target: &str) -> Option<&'static str> {
        let mut g = self.g.lock();
        let pid = match g.pid(leader) {
            Some(p) if g.leader_of(&p).as_deref() == Some(leader) => p,
            _ => return Some("notleader"),
        };
        if target == leader || g.of.get(target).map(|x| x.as_str()) != Some(pid.as_str()) {
            return Some("notmember");
        }
        g.leave_inner(target, "kicked");
        None
    }

    // py: social.py:350 Parties.promote
    pub fn promote(&self, leader: &str, target: &str) -> Option<&'static str> {
        let mut g = self.g.lock();
        let pid = match g.pid(leader) {
            Some(p) if g.leader_of(&p).as_deref() == Some(leader) => p,
            _ => return Some("notleader"),
        };
        if target == leader || g.of.get(target).map(|x| x.as_str()) != Some(pid.as_str()) {
            return Some("notmember");
        }
        g.leaders.insert(pid.clone(), target.to_string());
        for t in g.mem(&pid) {
            g.ev(&t, "plead", Some(target));
        }
        None
    }

    // py: social.py:362 Parties.clear_ready
    /// A match formed: its players start the next party round unready.
    pub fn clear_ready(&self, tokens: &[String]) {
        let mut g = self.g.lock();
        for t in tokens {
            g.ready.remove(t.as_str());
        }
    }

    // py: social.py:368 Parties.set_ready
    pub fn set_ready(&self, token: &str, on: bool) -> Option<&'static str> {
        let mut g = self.g.lock();
        if !g.of.contains_key(token) {
            return Some("noparty");
        }
        if on {
            g.ready.insert(token.to_string());
        } else {
            g.ready.remove(token);
        }
        None
    }

    // py: social.py:381 Parties.export
    /// The parties as JSON-able data: members in join order, leaders, ready flags.
    pub fn export(&self) -> Value {
        let g = self.g.lock();
        let of: Vec<Value> = g.of.iter().map(|(t, p)| json!([t, p])).collect();
        let vals: HashSet<&String> = g.of.values().collect();
        let mut leaders = Map::new();
        for (p, l) in &g.leaders {
            if vals.contains(p) {
                leaders.insert(p.clone(), json!(l));
            }
        }
        let mut ready: Vec<&String> = g.ready.iter().filter(|t| g.of.contains_key(t.as_str())).collect();
        ready.sort();
        json!({"of": of, "leaders": Value::Object(leaders), "ready": ready})
    }

    // py: social.py:388 Parties.restore
    /// Put saved parties back (start of the daemon). Returns the number of members restored. `now` defaults to the clock.
    pub fn restore(&self, data: &Value, now: Option<f64>) -> i64 {
        let now = now.unwrap_or_else(|| (self.now)());
        let mut n = 0i64;
        let mut g = self.g.lock();
        let mut got: IndexMap<String, Vec<String>> = IndexMap::new();
        let falsy = |v: &Value| v.is_null() || v.as_bool() == Some(false);
        let obj = match data {
            Value::Object(o) => Some(o),
            v if falsy(v) => None,
            _ => return 0,
        };
        let get = |k: &str| obj.and_then(|o| o.get(k)).cloned().unwrap_or(Value::Null);
        if let Value::Array(arr) = get("of") {
            for e in &arr {
                let (t, p) = match e {
                    Value::Array(a) if a.len() == 2 => (a[0].as_str(), a[1].as_str()),
                    _ => continue,
                };
                if let (Some(t), Some(p)) = (t, p) {
                    if !t.is_empty() && !p.is_empty() && !g.of.contains_key(t) {
                        got.entry(p.to_string()).or_default().push(t.to_string());
                    }
                }
            }
        }
        let leaders = get("leaders");
        for (p, mem) in &got {
            if g.of.values().any(|x| x == p) || mem.len() < 2 {
                continue;
            }
            for t in mem {
                g.of.insert(t.clone(), p.clone());
                g.grace.insert(t.clone(), now + RESTORE_GRACE);
                n += 1;
            }
            if let Some(l) = leaders.get(p).and_then(|x| x.as_str()) {
                if mem.iter().any(|m| m == l) {
                    g.leaders.insert(p.clone(), l.to_string());
                }
            }
        }
        if let Value::Array(r) = get("ready") {
            for t in &r {
                if let Some(t) = t.as_str() {
                    if g.grace.contains_key(t) && g.of.contains_key(t) {
                        g.ready.insert(t.to_string());
                    }
                }
            }
        }
        n
    }

    // py: social.py:421 Parties.gc
    /// Drop members that are gone (not in `keep`: a leader hands the party to the next member) and invitations older than their life.
    /// A member restored after a restart is kept for RESTORE_GRACE whatever `keep` says. Returns the number dropped.
    pub fn gc(&self, keep: &HashSet<String>, now: Option<f64>) -> i64 {
        let now = now.unwrap_or_else(|| (self.now)());
        let mut n = 0i64;
        let mut g = self.g.lock();
        let dead: Vec<String> = g.grace.iter().filter(|(t, u)| **u <= now || !g.of.contains_key(t.as_str())).map(|(t, _)| t.clone()).collect();
        for t in dead {
            g.grace.remove(&t);
        }
        let toks: Vec<String> = g.of.keys().filter(|t| !keep.contains(t.as_str()) && !g.grace.contains_key(t.as_str())).cloned().collect();
        for t in toks {
            if g.of.contains_key(&t) {
                g.leave_inner(&t, "timeout");
            }
            n += 1;
        }
        let gone: Vec<String> = {
            let vals: HashSet<&String> = g.of.values().collect();
            g.leaders.keys().filter(|p| !vals.contains(p)).cloned().collect()
        };
        for p in gone {
            g.leaders.shift_remove(&p);
        }
        let invitees: Vec<String> = g.inv.keys().cloned().collect();
        for invitee in invitees {
            let (cnt, empty) = match g.inv.get_mut(&invitee) {
                Some(d) => {
                    let del: Vec<String> = d.iter().filter(|(i, tm)| now - **tm > INVITE_TTL || !keep.contains(i.as_str())).map(|(i, _)| i.clone()).collect();
                    for i in &del {
                        d.shift_remove(i);
                    }
                    (del.len() as i64, d.is_empty())
                }
                None => (0, true),
            };
            n += cnt;
            if empty || !keep.contains(&invitee) {
                g.inv.shift_remove(&invitee);
            }
        }
        n
    }

    /// The tokens Social's party tick looks after: members, invitees and inviters.
    pub fn tracked(&self) -> HashSet<String> {
        let g = self.g.lock();
        let mut s: HashSet<String> = g.of.keys().cloned().collect();
        for (k, d) in &g.inv {
            s.insert(k.clone());
            for i in d.keys() {
                s.insert(i.clone());
            }
        }
        s
    }

    /// `set(PARTIES.of.values())`
    pub fn live_pids(&self) -> HashSet<String> {
        self.g.lock().of.values().cloned().collect()
    }

    // py: social.py:462 Parties.invite
    /// The old op 0xDA (by display name, online players only).
    pub fn invite(&self, token: &str, name: &[u8]) -> bool {
        let other = self.lookup.as_ref().and_then(|f| f(name));
        match other {
            Some(o) if !o.is_empty() => self.invite_token(token, &o).is_none(),
            _ => false,
        }
    }

    // py: social.py:466 Parties.accept
    pub fn accept(&self, token: &str, name: &[u8]) -> bool {
        let inviter = self.lookup.as_ref().and_then(|f| f(name));
        match inviter {
            Some(o) if !o.is_empty() => self.accept_token(token, &o).is_ok(),
            _ => false,
        }
    }

    // py: social.py:470 Parties.text
    pub fn text(&self, token: &str, name_of: &dyn Fn(&str) -> String) -> String {
        let pid = self.pid(token);
        let mem = if pid.is_some() { self.members(token) } else { Vec::new() };
        let inv: Vec<String> = self.invites_for(token).into_iter().map(|(t, _)| t).collect();
        let nm = |t: &String| {
            let n = name_of(t);
            (if n.is_empty() { "PLAYER".to_string() } else { n }).replace(' ', "_")
        };
        let m: Vec<String> = mem.iter().map(nm).collect();
        let i: Vec<String> = inv.iter().map(nm).collect();
        format!("n={} m={} inv={}", mem.len(), if m.is_empty() { "-".to_string() } else { m.join(",") }, if i.is_empty() { "-".to_string() } else { i.join(",") })
    }
}

// ---------------------------------------------------------------- matchmaking helpers (pure)

// py: social.py:480 held_parties
/// (tokens of `ready` whose party is whole, {party id: [members not ready]}).
pub fn held_parties(ready: &[String], parties: &Parties) -> (Vec<String>, IndexMap<String, Vec<String>>) {
    let rs: HashSet<&String> = ready.iter().collect();
    let mut ok = Vec::new();
    let mut held: IndexMap<String, Vec<String>> = IndexMap::new();
    let mut bad: HashSet<&String> = HashSet::new();
    for t in ready {
        if let Some(pid) = parties.pid(t) {
            let mem = parties.members(t);
            if mem.len() > 1 {
                let miss: Vec<String> = mem.into_iter().filter(|m| !rs.contains(m)).collect();
                if !miss.is_empty() {
                    held.insert(pid, miss);
                    bad.insert(t);
                }
            }
        }
    }
    for t in ready {
        if !bad.contains(t) {
            ok.push(t.clone());
        }
    }
    (ok, held)
}

// py: social.py:498 units_of
/// The queue-ordered units: a party is one unit (at the place of its first member), anybody else a unit of one.
pub fn units_of(ready: &[String], parties: &Parties) -> Vec<Vec<String>> {
    let mut units: Vec<Vec<String>> = Vec::new();
    let mut at: HashMap<String, usize> = HashMap::new();
    for t in ready {
        let pid = if parties.members(t).len() > 1 { parties.pid(t) } else { None };
        match pid {
            None => units.push(vec![t.clone()]),
            Some(p) => {
                if let Some(&i) = at.get(&p) {
                    units[i].push(t.clone());
                } else {
                    at.insert(p, units.len());
                    units.push(vec![t.clone()]);
                }
            }
        }
    }
    units
}

// py: social.py:512 party_need
/// Players a match needs so that a party of p stays on one team with the other team not smaller than p - 1.
pub fn party_need(p: i64) -> i64 {
    if p <= 1 {
        1
    } else {
        2 * p - 1
    }
}

// py: social.py:517 party_shortfall
/// {party id: (members, missing)} of the ready parties that are NOT in `chosen` because too few players are searching.
pub fn party_shortfall(ready: &[String], parties: &Parties, size: i64, chosen: &[String]) -> IndexMap<String, (Vec<String>, i64)> {
    let mut out = IndexMap::new();
    let cs: HashSet<&String> = chosen.iter().collect();
    for u in units_of(ready, parties) {
        if u.len() > 1 && !u.iter().any(|t| cs.contains(t)) {
            let miss = party_need(u.len() as i64) - ready.len() as i64;
            if miss > 0 && u.len() as i64 <= std::cmp::max(1, crate::py::floordiv(size, 2)) {
                out.insert(parties.pid(&u[0]).unwrap_or_default(), (u, miss));
            }
        }
    }
    out
}

// py: social.py:529 pick_units
/// The players of the next match from the ready (queue-ordered, parties whole) tokens: whole units up to `size`, a party that does not fit the
/// open slots waits, and a party of p needs at least 2p - 1 players in the match.
pub fn pick_units(ready: &[String], parties: &Parties, size: i64) -> Vec<String> {
    let half = std::cmp::max(1, crate::py::floordiv(size, 2));
    let units: Vec<Vec<String>> = units_of(ready, parties).into_iter().filter(|u| u.len() as i64 <= half || u.len() == 1).collect();
    let mut chosen: Vec<Vec<String>> = Vec::new();
    let mut total = 0i64;
    for u in &units {
        if total + u.len() as i64 <= size {
            chosen.push(u.clone());
            total += u.len() as i64;
        }
    }
    let mut dropped: Vec<Vec<String>> = Vec::new();
    loop {
        let bad: Vec<&Vec<String>> = chosen.iter().filter(|u| u.len() > 1 && total < 2 * u.len() as i64 - 1).collect();
        if bad.is_empty() {
            break;
        }
        // max(bad, key=len): the first of the longest
        let mut u = bad[0];
        for b in &bad[1..] {
            if b.len() > u.len() {
                u = b;
            }
        }
        let u = u.clone();
        if let Some(pos) = chosen.iter().position(|x| *x == u) {
            chosen.remove(pos);
        }
        total -= u.len() as i64;
        dropped.push(u);
        for v in &units {
            if !chosen.contains(v) && !dropped.contains(v) && total + v.len() as i64 <= size {
                chosen.push(v.clone());
                total += v.len() as i64;
            }
        }
    }
    let mut order: HashMap<&String, usize> = HashMap::new();
    for (i, t) in ready.iter().enumerate() {
        order.insert(t, i);
    }
    chosen.sort_by_key(|u| order.get(&u[0]).copied().unwrap_or(0));
    chosen.into_iter().flatten().collect()
}

// ---------------------------------------------------------------- friends + presence

/// What social.py needs from the daemon (`_SocialHost` in daemon.py).
pub trait SocialHost: Send + Sync {
    /// `h.now()`
    fn now(&self) -> f64;
    /// `h.log(*a)`: the parts are joined with a space like `log`.
    fn log(&self, parts: &[String]);
    /// `with h.db() as c`: open a connection, run `f`, commit on Ok, roll back and return the error on Err, close. It must NOT hold the
    /// daemon's database lock (steamid / name take it).
    fn with_db(&self, f: &mut dyn FnMut(&Connection) -> rusqlite::Result<()>) -> rusqlite::Result<()>;
    /// Steam id of the token (0 = unknown).
    fn steamid(&self, token: &str) -> u64;
    /// The shown name (empty = none).
    fn name(&self, token: &str) -> String;
    /// `PRES_TOKENS()`
    fn game_tokens(&self) -> Vec<String>;
    /// game + queued + in a match
    fn alive_tokens(&self) -> HashSet<String>;
    /// "match" | "search" | None
    fn mm_state(&self, token: &str) -> Option<&'static str>;
}

#[derive(Clone)]
struct Line {
    id: i64,
    from: String,
    name: String,
    t: String,
    ts: f64,
}

struct Note {
    id: i64,
    kind: String,
    subj: String,
    until: f64,
}

struct ClogRow {
    ts: f64,
    kind: String,
    room: String,
    acct: String,
    name: String,
    body: String,
    muted: i64,
}

/// ({account: [tokens]} in first-seen order, game token set).
pub struct Om {
    pub m: IndexMap<String, Vec<String>>,
    pub g: HashSet<String>,
}

#[derive(Clone, Copy)]
enum RateTab {
    Rate,
    Msg,
    Log,
}

struct St {
    hb: HashMap<String, f64>,
    seen_w: HashMap<String, f64>,
    adopt: HashSet<String>,
    rate: HashMap<String, Vec<f64>>,
    mrate: HashMap<String, Vec<f64>>,
    lrate: HashMap<String, Vec<f64>>,
    pchat: HashMap<String, Vec<Line>>,
    lchat: HashMap<String, Vec<Line>>,
    pseq: i64,
    mlog: VecDeque<(f64, String, String, String, String)>,
    mgc_at: f64,
    mutes: HashMap<String, (f64, String)>,
    mute_chk: (f64, Option<f64>),
    clog: Vec<ClogRow>,
    clog_at: f64,
    clog_gc: f64,
    qnote: HashMap<(String, String), f64>,
    om: (f64, Arc<Om>),
    acct_c: HashMap<String, (f64, String)>,
    ensured: bool,
    tick_at: f64,
    pt_at: f64,
    gone: HashMap<String, f64>,
}

struct Notes {
    seq: i64,
    map: HashMap<String, Vec<Note>>,
}

pub type LobbyOf = Box<dyn Fn(&str) -> Option<String> + Send + Sync>;

// py: social.py:560 Social
pub struct Social {
    h: Arc<dyn SocialHost>,
    p: Arc<Parties>,
    st: Mutex<St>,
    notes: Mutex<Notes>,
    /// set by the daemon: token -> its lobby's key
    pub lobby_of: Option<LobbyOf>,
    /// set by the daemon: token -> its match id (the chat_log room of MATCH / TEAM lines)
    pub match_of: Option<LobbyOf>,
    /// set by the daemon: mutes.txt next to the database (None = nobody is muted)
    pub mute_path: Option<PathBuf>,
}

type Res<T> = rusqlite::Result<T>;

impl Social {
    // py: social.py:570 Social.__init__
    pub fn new(host: Arc<dyn SocialHost>, parties: Arc<Parties>) -> Social {
        let now = host.now();
        let pseq = std::cmp::max(0, now.trunc() as i64 - 1700000000) * 10;
        Social {
            h: host,
            p: parties,
            st: Mutex::new(St {
                hb: HashMap::new(),
                seen_w: HashMap::new(),
                adopt: HashSet::new(),
                rate: HashMap::new(),
                mrate: HashMap::new(),
                lrate: HashMap::new(),
                pchat: HashMap::new(),
                lchat: HashMap::new(),
                pseq,
                mlog: VecDeque::new(),
                mgc_at: 0.0,
                mutes: HashMap::new(),
                mute_chk: (0.0, None),
                clog: Vec::new(),
                clog_at: 0.0,
                clog_gc: 0.0,
                qnote: HashMap::new(),
                om: (0.0, Arc::new(Om { m: IndexMap::new(), g: HashSet::new() })),
                acct_c: HashMap::new(),
                ensured: false,
                tick_at: 0.0,
                pt_at: 0.0,
                gone: HashMap::new(),
            }),
            notes: Mutex::new(Notes { seq: NOTE_BASE, map: HashMap::new() }),
            lobby_of: None,
            match_of: None,
            mute_path: None,
        }
    }

    fn log(&self, parts: &[&str]) {
        let v: Vec<String> = parts.iter().map(|s| s.to_string()).collect();
        self.h.log(&v);
    }

    /// The newest match / team chat lines (`SOCIAL.mlog`): (time, channel, account, name, text).
    pub fn mlog(&self) -> Vec<(f64, String, String, String, String)> {
        self.st.lock().mlog.iter().cloned().collect()
    }

    // --- accounts + presence
    // py: social.py:604 Social.acct
    pub fn acct(&self, token: &str) -> String {
        let sid = self.h.steamid(token);
        if sid != 0 {
            format!("s:{}", sid)
        } else {
            acct_hash(&token_id_of(token))
        }
    }

    // py: social.py:608 Social.online_map
    /// ({account: [tokens]}, game token set), cached ~1 s. Tokens of one account are listed in sorted order (Python's follow set order).
    pub fn online_map(&self) -> Arc<Om> {
        let now = self.h.now();
        {
            let s = self.st.lock();
            if now - s.om.0 < 1.0 {
                return s.om.1.clone();
            }
        }
        let g: HashSet<String> = self.h.game_tokens().into_iter().collect();
        let mut toks: Vec<String> = {
            let s = self.st.lock();
            let mut set: HashSet<String> = g.clone();
            for (t, tm) in &s.hb {
                if now - tm < PRESENCE_TTL {
                    set.insert(t.clone());
                }
            }
            set.into_iter().collect()
        };
        toks.sort();
        let mut m: IndexMap<String, Vec<String>> = IndexMap::new();
        let mut cache_new: Vec<(String, (f64, String))> = Vec::new();
        for t in &toks {
            let cached = {
                let s = self.st.lock();
                s.acct_c.get(t).cloned()
            };
            let e = match cached {
                Some(e) if now - e.0 < ACCT_CACHE_S => e,
                _ => {
                    let e = (now, self.acct(t));
                    cache_new.push((t.clone(), e.clone()));
                    e
                }
            };
            m.entry(e.1).or_default().push(t.clone());
        }
        let om = Arc::new(Om { m, g });
        let mut s = self.st.lock();
        for (t, e) in cache_new {
            s.acct_c.insert(t, e);
        }
        if s.acct_c.len() > 4 * toks.len() + 256 {
            let keep: HashSet<&String> = toks.iter().collect();
            s.acct_c.retain(|t, _| keep.contains(t));
        }
        s.om = (now, om.clone());
        om
    }

    // py: social.py:631 Social.status_of
    pub fn status_of(&self, key: &str, om: Option<&Om>) -> &'static str {
        let own;
        let om = match om {
            Some(o) => o,
            None => {
                own = self.online_map();
                &own
            }
        };
        let Some(toks) = om.m.get(key) else { return "off" };
        if toks.is_empty() {
            return "off";
        }
        let mut best = "launcher";
        for t in toks {
            let s = self.h.mm_state(t);
            if s == Some("match") {
                return "match";
            }
            if s == Some("search") {
                best = "search";
            } else if best == "launcher" && om.g.contains(t) {
                best = "menu";
            }
        }
        best
    }

    // py: social.py:645 Social.token_for
    /// The online token of an account that was active last (game first).
    pub fn token_for(&self, key: &str) -> Option<String> {
        let om = self.online_map();
        let toks = om.m.get(key)?;
        if toks.is_empty() {
            return None;
        }
        let s = self.st.lock();
        let mut v: Vec<(bool, f64, &String)> = toks.iter().map(|t| (om.g.contains(t), s.hb.get(t).copied().unwrap_or(0.0), t)).collect();
        v.sort_by(|a, b| (b.0, b.1).partial_cmp(&(a.0, a.1)).unwrap_or(std::cmp::Ordering::Equal));
        Some(v[0].2.clone())
    }

    // py: social.py:653 Social.touch
    pub fn touch(&self, token: &str) {
        let now = self.h.now();
        self.st.lock().hb.insert(token.to_string(), now);
    }

    // py: social.py:656 Social.launcher_tokens
    pub fn launcher_tokens(&self) -> HashSet<String> {
        let now = self.h.now();
        self.st.lock().hb.iter().filter(|(_, tm)| now - **tm < PRESENCE_TTL).map(|(t, _)| t.clone()).collect()
    }

    // --- storage
    // py: social.py:662 Social._ensure
    fn ensure(&self, c: &Connection) -> Res<()> {
        if self.st.lock().ensured {
            return Ok(());
        }
        self.ensure_tables(c)?;
        self.st.lock().ensured = true;
        Ok(())
    }

    // py: social.py:668 Social._ensure_tables
    pub fn ensure_tables(&self, c: &Connection) -> Res<()> {
        c.execute("CREATE TABLE IF NOT EXISTS social_friends(a TEXT, b TEXT, state INT, since REAL, PRIMARY KEY(a, b))", [])?;
        c.execute("CREATE INDEX IF NOT EXISTS social_friends_b ON social_friends(b)", [])?;
        c.execute("CREATE TABLE IF NOT EXISTS social_seen(acct TEXT PRIMARY KEY, name TEXT, last REAL)", [])?;
        c.execute("CREATE TABLE IF NOT EXISTS social_msgs(id INTEGER PRIMARY KEY AUTOINCREMENT, a TEXT, b TEXT, ts REAL, body TEXT, rd INT DEFAULT 0)", [])?;
        c.execute("CREATE INDEX IF NOT EXISTS social_msgs_to ON social_msgs(b, rd)", [])?;
        c.execute("CREATE INDEX IF NOT EXISTS social_msgs_pair ON social_msgs(a, b, id)", [])?;
        c.execute(
            "CREATE TABLE IF NOT EXISTS chat_log(id INTEGER PRIMARY KEY AUTOINCREMENT, ts REAL, kind TEXT, room TEXT, acct TEXT, name TEXT, body TEXT, \
             muted INT DEFAULT 0)",
            [],
        )?;
        c.execute("CREATE INDEX IF NOT EXISTS chat_log_ts ON chat_log(ts)", [])?;
        c.execute("CREATE INDEX IF NOT EXISTS chat_log_acct ON chat_log(acct, ts)", [])?;
        Ok(())
    }

    // --- chat moderation (mutes.txt + chat_log)
    // py: social.py:683 Social._mutes_now
    /// The mute entry of an account from mutes.txt, re-read when its write time changes (checked at most every MUTE_RELOAD_S).
    fn mute_entry(&self, who: &str) -> Option<(f64, String)> {
        let path = self.mute_path.as_ref()?;
        let now = self.h.now();
        let (at, mt) = self.st.lock().mute_chk;
        if !(now - at < MUTE_RELOAD_S && at != 0.0) {
            let m: Option<f64> = std::fs::metadata(path).ok().and_then(|md| md.modified().ok()).map(|t| match t.duration_since(std::time::UNIX_EPOCH) {
                Ok(d) => d.as_secs() as f64 + d.subsec_nanos() as f64 * 1e-9,
                Err(e) => -(e.duration().as_secs() as f64 + e.duration().subsec_nanos() as f64 * 1e-9),
            });
            if m != mt || at == 0.0 {
                let mut out: HashMap<String, (f64, String)> = HashMap::new();
                let mut failed = false;
                if m.is_some() {
                    match std::fs::read(path) {
                        Ok(b) => {
                            let text = String::from_utf8_lossy(&b).into_owned();
                            for line in universal_lines(&text) {
                                let (head, why) = match line.find('#') {
                                    Some(p) => (&line[..p], &line[p + 1..]),
                                    None => (line, ""),
                                };
                                let parts = split_ws(head);
                                if parts.is_empty() {
                                    continue;
                                }
                                let mut key = parts[0].to_string();
                                if is_digits(&key) && key.len() == 17 {
                                    key = format!("s:{}", key);
                                }
                                let until = match parts.get(1) {
                                    Some(x) => py_float(x).unwrap_or(0.0),
                                    None => 0.0,
                                };
                                out.insert(key, (until, strip(why).to_string()));
                            }
                        }
                        Err(_) => failed = true,
                    }
                }
                let mut s = self.st.lock();
                if !failed {
                    s.mutes = out;
                }
            }
            self.st.lock().mute_chk = (now, m);
        }
        self.st.lock().mutes.get(who).cloned()
    }

    // py: social.py:720 Social.mute_left
    /// 0 = not muted, -1 = muted for good, else whole seconds left.
    pub fn mute_left(&self, me: &str) -> i64 {
        let Some(e) = self.mute_entry(me) else { return 0 };
        let until = e.0;
        if until <= 0.0 {
            return -1;
        }
        let left = (until - self.h.now()).trunc() as i64;
        if left > 0 {
            left
        } else {
            0
        }
    }

    // py: social.py:730 Social._log_line
    fn log_line(&self, kind: &str, room: &str, me: &str, name: &str, body: &str, muted: bool) {
        let now = self.h.now();
        self.st.lock().clog.push(ClogRow {
            ts: now,
            kind: kind.to_string(),
            room: room.to_string(),
            acct: me.to_string(),
            name: name.to_string(),
            body: body.to_string(),
            muted: if muted { 1 } else { 0 },
        });
    }

    // py: social.py:734 Social._clog_flush
    /// Write the buffered chat_log rows (inside handle()'s transaction, or an own connection from the quick path).
    pub fn clog_flush(&self, c: Option<&Connection>, force: bool) {
        let now = self.h.now();
        let rows = {
            let mut s = self.st.lock();
            if s.clog.is_empty() || (!force && c.is_none() && now - s.clog_at < CHATLOG_FLUSH_S && s.clog.len() < 40) {
                return;
            }
            s.clog_at = now;
            std::mem::take(&mut s.clog)
        };
        let r = match c {
            None => self.h.with_db(&mut |c2| self.clog_write(c2, &rows, now)),
            Some(c) => self.clog_write(c, &rows, now),
        };
        if let Err(ex) = r {
            self.log(&["SOCIAL chat_log error", &exc_repr(&ex)]);
        }
    }

    // py: social.py:752 Social._clog_write
    fn clog_write(&self, c: &Connection, rows: &[ClogRow], now: f64) -> Res<()> {
        self.ensure(c)?;
        {
            let mut stmt = c.prepare("INSERT INTO chat_log(ts, kind, room, acct, name, body, muted) VALUES(?, ?, ?, ?, ?, ?, ?)")?;
            for r in rows {
                stmt.execute(params![r.ts, r.kind, r.room, r.acct, r.name, r.body, r.muted])?;
            }
        }
        let due = {
            let mut s = self.st.lock();
            if now - s.clog_gc > 3600.0 {
                s.clog_gc = now;
                true
            } else {
                false
            }
        };
        if due {
            c.execute("DELETE FROM chat_log WHERE ts < ?", params![now - CHATLOG_AGE_S])?;
        }
        Ok(())
    }

    // py: social.py:759 Social._adopt
    /// The token's Steam id became known: the rows of its old install account move to the Steam account. Returns false when it failed (retry later).
    fn adopt(&self, c: &Connection, token: &str, key: &str) -> bool {
        let old = acct_hash(&token_id_of(token));
        if key == old {
            return true;
        }
        let body = || -> Res<()> {
            for (col, other) in [("a", "b"), ("b", "a")] {
                let rows: Vec<(Option<String>, Option<i64>, Option<f64>)> = {
                    let mut stmt = c.prepare(&format!("SELECT {}, state, since FROM social_friends WHERE {}=?", other, col))?;
                    let r = stmt.query_map(params![old], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))?.collect::<Res<Vec<_>>>()?;
                    r
                };
                for row in rows {
                    if row.0.as_deref() != Some(key) {
                        c.execute(&format!("INSERT OR IGNORE INTO social_friends({},{},state,since) VALUES(?,?,?,?)", col, other), params![key, row.0, row.1, row.2])?;
                    }
                }
                c.execute(&format!("DELETE FROM social_friends WHERE {}=?", col), params![old])?;
            }
            let r: Option<(Option<String>, Option<f64>)> =
                c.query_row("SELECT name, last FROM social_seen WHERE acct=?", params![old], |r| Ok((r.get(0)?, r.get(1)?))).optional()?;
            if let Some(r) = r {
                c.execute("INSERT OR IGNORE INTO social_seen(acct,name,last) VALUES(?,?,?)", params![key, r.0, r.1])?;
                c.execute("DELETE FROM social_seen WHERE acct=?", params![old])?;
            }
            Ok(())
        };
        // the whole adopt is one transaction (Python's implicit one): a failure rolls everything back
        let r = c.execute_batch("SAVEPOINT adopt").and_then(|_| body()).and_then(|_| c.execute_batch("RELEASE adopt")).and_then(|_| commit(c));
        match r {
            Ok(()) => true,
            Err(ex) => {
                let _ = c.execute_batch("ROLLBACK TO adopt");
                let _ = c.execute_batch("RELEASE adopt");
                rollback(c);
                self.log(&["SOCIAL adopt failed", &exc_repr(&ex)]);
                false
            }
        }
    }

    // py: social.py:798 Social._seen
    fn seen(&self, c: &Connection, key: &str, name: &str, now: f64, force: bool) -> Res<()> {
        {
            let mut s = self.st.lock();
            if !force && now - s.seen_w.get(key).copied().unwrap_or(0.0) < SEEN_WRITE_S {
                return Ok(());
            }
            s.seen_w.insert(key.to_string(), now);
        }
        c.execute("INSERT INTO social_seen(acct,name,last) VALUES(?,?,?) ON CONFLICT(acct) DO UPDATE SET name=excluded.name,last=excluded.last", params![key, name, now])?;
        commit(c)
    }

    // py: social.py:805 Social.name_of
    fn name_of(&self, c: &Connection, key: &str, om: &Om) -> Res<String> {
        if let Some(toks) = om.m.get(key) {
            for t in toks {
                let n = self.h.name(t);
                if !n.is_empty() {
                    return Ok(n);
                }
            }
        }
        let r: Option<Option<String>> = c.query_row("SELECT name FROM social_seen WHERE acct=?", params![key], |r| r.get(0)).optional()?;
        if let Some(Some(n)) = r {
            if !n.is_empty() {
                return Ok(n);
            }
        }
        if let Some(rest) = key.strip_prefix("s:") {
            let q = "SELECT name FROM players WHERE steamid=? AND name IS NOT NULL AND name<>'' ORDER BY last_seen DESC LIMIT 1";
            let sid = bind_int(c, q, rest)?;
            let r: Option<String> = c.query_row(q, params![sid], |r| r.get(0)).optional()?;
            if let Some(n) = r {
                if !n.is_empty() {
                    return Ok(n);
                }
            }
        }
        Ok("PLAYER".to_string())
    }

    // py: social.py:818 Social.tick
    /// About every SEEN_WRITE_S: write last-seen rows of everybody online, and drop dead launcher polls. `now` defaults to the host clock.
    pub fn tick(&self, now: Option<f64>) -> i64 {
        let now = now.unwrap_or_else(|| self.h.now());
        self.party_tick(Some(now));
        {
            let mut s = self.st.lock();
            if now - s.tick_at < SEEN_WRITE_S {
                return 0;
            }
            s.tick_at = now;
            s.hb.retain(|_, tm| !(now - *tm > 10.0 * PRESENCE_TTL));
        }
        let om = self.online_map();
        let mut n = 0;
        let mut rows: Vec<(String, String)> = Vec::new();
        for (key, toks) in &om.m {
            let mut name = "PLAYER".to_string();
            for t in toks {
                let nm = self.h.name(t);
                if !nm.is_empty() {
                    name = nm;
                    break;
                }
            }
            rows.push((key.clone(), name));
        }
        let r = self.h.with_db(&mut |c| {
            self.ensure(c)?;
            for (key, name) in &rows {
                self.seen(c, key, name, now, false)?;
                n += 1;
            }
            Ok(())
        });
        if let Err(ex) = r {
            self.log(&["SOCIAL tick failed", &exc_repr(&ex)]);
        }
        n
    }

    // py: social.py:838 Social.party_tick
    /// About every 10 s: party members (and invitees) that were in neither the game nor the launcher for PARTY_GRACE s are removed.
    pub fn party_tick(&self, now: Option<f64>) -> i64 {
        let now = now.unwrap_or_else(|| self.h.now());
        {
            let mut s = self.st.lock();
            if now - s.pt_at < 10.0 {
                return 0;
            }
            s.pt_at = now;
        }
        let mut alive = self.h.alive_tokens();
        alive.extend(self.launcher_tokens());
        let tracked = self.p.tracked();
        let mut out: HashSet<String> = HashSet::new();
        {
            let mut s = self.st.lock();
            for t in &tracked {
                if alive.contains(t) {
                    s.gone.remove(t);
                } else {
                    let first = *s.gone.entry(t.clone()).or_insert(now);
                    if now - first >= PARTY_GRACE {
                        out.insert(t.clone());
                    }
                }
            }
            s.gone.retain(|t, _| tracked.contains(t));
        }
        let n = if !tracked.is_empty() {
            let mut keep = alive;
            for t in &tracked {
                if !out.contains(t) {
                    keep.insert(t.clone());
                }
            }
            self.p.gc(&keep, Some(now))
        } else {
            0
        };
        {
            let mut s = self.st.lock();
            for t in &out {
                s.gone.remove(t);
            }
        }
        self.drain();
        let live = self.p.live_pids();
        self.st.lock().pchat.retain(|k, _| live.contains(k));
        n
    }

    // --- notes (in-game lines)
    // py: social.py:867 Social.note
    pub fn note(&self, token: &str, kind: &str, subject_name: &str) {
        if token.is_empty() || note_tpl(kind).is_none() {
            return;
        }
        let now = self.h.now();
        let mut g = self.notes.lock();
        g.seq += 1;
        let id = g.seq;
        let lst = g.map.entry(token.to_string()).or_default();
        if kind == "msg" {
            let who = subject_name.split('\x1f').next().unwrap_or("").to_string();
            lst.retain(|x| !(x.kind == "msg" && x.subj.split('\x1f').next().unwrap_or("") == who));
        }
        lst.push(Note { id, kind: kind.to_string(), subj: subject_name.to_string(), until: now + NOTE_KEEP_S });
        if lst.len() > 6 {
            let cut = lst.len() - 6;
            lst.drain(..cut);
        }
    }

    // py: social.py:877 Social.note_tag
    /// The newest live in-game note id of this token (0 = none).
    pub fn note_tag(&self, token: &str) -> i64 {
        let now = self.h.now();
        let g = self.notes.lock();
        g.map.get(token).map(|l| l.iter().filter(|x| x.until > now).map(|x| x.id).max().unwrap_or(0)).unwrap_or(0)
    }

    // py: social.py:884 Social.note_acct
    pub fn note_acct(&self, key: &str, kind: &str, subject_name: &str) {
        let om = self.online_map();
        if let Some(toks) = om.m.get(key) {
            for t in toks {
                self.note(t, kind, subject_name);
            }
        }
    }

    /// (selftest) every pending note: (token, [(id, kind, subject, until)]) sorted by token
    pub fn dump_notes(&self) -> Vec<(String, Vec<(i64, String, String, f64)>)> {
        let g = self.notes.lock();
        let mut v: Vec<(String, Vec<(i64, String, String, f64)>)> = g.map.iter().map(|(t, l)| (t.clone(), l.iter().map(|n| (n.id, n.kind.clone(), n.subj.clone(), n.until)).collect())).collect();
        v.sort_by(|a, b| a.0.cmp(&b.0));
        v
    }

    // py: social.py:889 Social.notes_for
    /// [(id, 'p', seconds on screen, text)] for the notice poll of this token's game (each stays NOTE_KEEP_S; the ext shows every id once).
    pub fn notes_for(&self, token: &str, lang: i64, ingame: bool) -> Vec<(i64, &'static str, i64, String)> {
        let now = self.h.now();
        let lst: Vec<(i64, String, String)> = {
            let mut g = self.notes.lock();
            let live: Vec<Note> = g.map.remove(token).unwrap_or_default().into_iter().filter(|x| x.until > now).collect();
            let v: Vec<(i64, String, String)> = live.iter().map(|x| (x.id, x.kind.clone(), x.subj.clone())).collect();
            if !live.is_empty() {
                g.map.insert(token.to_string(), live);
            }
            v
        };
        let mut out = Vec::new();
        let start = lst.len().saturating_sub(2);
        for (nid, kind, who) in &lst[start..] {
            let tpl = if ingame { note_tpl_ingame(kind).or_else(|| note_tpl(kind)) } else { note_tpl(kind) };
            let Some(tpl) = tpl else { continue };
            let (who, body) = match who.find('\x1f') {
                Some(p) => (&who[..p], &who[p + 1..]),
                None => (who.as_str(), ""),
            };
            let txt = if lang == 1 { tpl.1 } else { tpl.0 };
            out.push((*nid, "p", 8, txt.replace("{n}", who).replace("{t}", body)));
        }
        out
    }

    // py: social.py:906 Social._drain
    /// Party events -> notes (called after every party change).
    pub fn drain(&self) {
        for (to, kind, subj) in self.p.drain() {
            let name = match &subj {
                Some(s) if !s.is_empty() => {
                    let n = self.h.name(s);
                    if n.is_empty() {
                        "PLAYER".to_string()
                    } else {
                        n
                    }
                }
                _ => String::new(),
            };
            self.note(&to, kind, &name);
        }
    }

    // --- queue hooks (daemon)
    // py: social.py:912 Social.on_queue
    /// `token` pressed FIND MATCH. `queued` = predicate token -> bool (already searching).
    pub fn on_queue(&self, token: &str, queued: &dyn Fn(&str) -> bool) -> i64 {
        let Some(pid) = self.p.pid(token) else { return 0 };
        if self.p.members(token).len() < 2 {
            return 0;
        }
        let now = self.h.now();
        {
            let mut s = self.st.lock();
            let k = (pid.clone(), token.to_string());
            if now - s.qnote.get(&k).copied().unwrap_or(0.0) < 20.0 {
                return 0;
            }
            s.qnote.insert(k, now);
            if s.qnote.len() > 200 {
                s.qnote.clear();
            }
        }
        let mut n = 0;
        let nm = self.h.name(token);
        let who = if nm.is_empty() { "PLAYER".to_string() } else { nm };
        let lead = self.p.leader(token);
        if lead.as_deref() != Some(token) {
            return 0;
        }
        for m in self.p.members(token) {
            if m == token || queued(&m) || self.p.is_ready(&m) {
                continue;
            }
            self.note(&m, "pq", &who);
            n += 1;
        }
        n
    }

    // py: social.py:933 Social.search_gate
    /// True = this FIND MATCH request must NOT queue `token` (a member's press is a READY UP toggle while the leader is not searching).
    pub fn search_gate(&self, token: &str, queued: &dyn Fn(&str) -> bool) -> bool {
        if self.p.pid(token).is_none() || self.p.members(token).len() < 2 {
            return false;
        }
        let lead = self.p.leader(token);
        if lead.as_deref() == Some(token) {
            return false;
        }
        match lead {
            Some(l) if !l.is_empty() => !queued(&l),
            _ => true,
        }
    }

    // py: social.py:946 Social.party_rows
    /// [(token, name, leader?)] of the token's party mates that are NOT searching.
    pub fn party_rows(&self, token: &str) -> Vec<(String, String, bool)> {
        if self.p.pid(token).is_none() {
            return Vec::new();
        }
        let lead = self.p.leader(token);
        self.p
            .members(token)
            .into_iter()
            .filter(|m| m != token)
            .map(|m| {
                let n = self.h.name(&m);
                let is_lead = lead.as_deref() == Some(m.as_str());
                (m, if n.is_empty() { "PLAYER".to_string() } else { n }, is_lead)
            })
            .collect()
    }

    // --- ops
    // py: social.py:954 Social._limited
    fn limited(&self, key: &str, tab: RateTab, per_min: usize) -> bool {
        let now = self.h.now();
        let mut s = self.st.lock();
        let table = match tab {
            RateTab::Rate => &mut s.rate,
            RateTab::Msg => &mut s.mrate,
            RateTab::Log => &mut s.lrate,
        };
        let mut l: Vec<f64> = table.get(key).map(|v| v.iter().copied().filter(|t| now - t < 60.0).collect()).unwrap_or_default();
        l.push(now);
        let over = l.len() > per_min;
        table.insert(key.to_string(), l);
        if table.len() > 2000 {
            table.retain(|_, v| v.last().map(|t| now - t < 60.0).unwrap_or(false));
        }
        over
    }

    // py: social.py:964 Social.handle
    /// One launcher request -> the reply dict (always a fresh state plus ok / err). `None` = Python would have raised an exception that is
    /// not a `sqlite3.Error` (the daemon then logs "SOCIAL ERR" and sends no reply).
    pub fn handle(&self, token: &str, lang: i64, sub: i64, arg: &str) -> Option<Value> {
        if matches!(sub, S_PCHAT | S_MLOG | S_LSAY | S_LCHAT | S_MUTE) {
            let res = self.quick(token, sub, arg);
            self.clog_flush(None, false);
            return Some(res);
        }
        let mut res: Map<String, Value> = Map::new();
        res.insert("ok".into(), json!(1));
        res.insert("err".into(), json!(""));
        let r = self.h.with_db(&mut |c| {
            self.ensure(c)?;
            let me = self.acct(token);
            self.adopt_once(c, token, &me);
            self.touch(token);
            let mut om = self.online_map();
            if !om.m.get(&me).map(|v| v.iter().any(|t| t == token)).unwrap_or(false) {
                {
                    let mut s = self.st.lock();
                    s.acct_c.remove(token);
                    s.om = (0.0, Arc::new(Om { m: IndexMap::new(), g: HashSet::new() }));
                }
                om = self.online_map();
            }
            let nm = self.h.name(token);
            self.seen(c, &me, &if nm.is_empty() { token_name_fallback(token) } else { nm }, self.h.now(), false)?;
            let mut err: Option<String> = None;
            if is_write_sub(sub) && self.limited(&me, RateTab::Rate, OPS_PER_MIN) {
                err = Some("rate".to_string());
            } else if sub == S_SEARCH {
                res.insert("found".into(), Value::Array(self.search(c, &me, arg, &om, false)?));
            } else if sub == S_ADD {
                err = self.add(c, token, &me, arg, &om)?;
            } else if sub == S_ACCEPT {
                err = self.accept(c, token, &me, arg, &om)?;
            } else if sub == S_REMOVE {
                err = self.remove(c, &me, arg)?;
            } else if matches!(sub, S_INVITE | S_JOIN | S_DECLINE | S_KICK | S_PROMOTE) {
                err = self.party_op(c, token, &me, sub, arg)?;
            } else if sub == S_LEAVE {
                self.p.leave(token);
            } else if sub == S_READY {
                err = self.p.set_ready(token, strip(arg) != "0").map(|s| s.to_string());
            } else if (sub == S_MSG || sub == S_PSAY) && self.mute_left(&me) != 0 {
                err = Some("muted".to_string());
            } else if sub == S_MSG {
                let (key, text) = match arg.find('\n') {
                    Some(p) => (&arg[..p], &arg[p + 1..]),
                    None => (arg, ""),
                };
                let sender = {
                    let n = self.h.name(token);
                    if n.is_empty() {
                        token_name_fallback(token)
                    } else {
                        n
                    }
                };
                err = self.msg(c, &me, strip(key), text, &sender)?;
                if err.is_none() {
                    res.insert("chat".into(), self.chat(c, &me, strip(key))?);
                }
            } else if sub == S_PSAY {
                err = self.psay(token, &me, arg).map(|s| s.to_string());
                if err.is_none() {
                    res.insert("pchat".into(), self.pchat_view(token, &me, 0));
                }
            } else if sub == S_CHAT {
                let key = strip(arg);
                if self.rel(c, &me, key)?.as_deref() != Some("friend") {
                    err = Some("notfriend".to_string());
                } else {
                    res.insert("chat".into(), self.chat(c, &me, key)?);
                }
            } else if sub != S_STATE {
                err = Some("bad".to_string());
            }
            self.drain();
            if let Some(e) = err {
                res.insert("ok".into(), json!(0));
                res.insert("err".into(), json!(e));
            }
            for (k, v) in self.state(c, token, &me, lang)? {
                res.insert(k, v);
            }
            res.insert("mute".into(), json!(self.mute_left(&me)));
            self.clog_flush(Some(c), true);
            Ok(())
        });
        match r {
            Ok(()) => Some(Value::Object(res)),
            Err(ex) if is_exc(&ex) => None,
            Err(ex) => {
                self.log(&["SOCIAL db error", &exc_repr(&ex)]);
                Some(json!({"ok": 0, "err": "db"}))
            }
        }
    }

    // --- party chat + match / team log (memory only)
    // py: social.py:1032 Social._clean
    fn clean_text(text: &str) -> String {
        ctrl_words(text).chars().take(MSG_LEN).collect()
    }

    // py: social.py:1036 Social._psay
    fn psay(&self, token: &str, me: &str, text: &str) -> Option<&'static str> {
        let Some(pid) = self.p.pid(token) else { return Some("noparty") };
        let body = Self::clean_text(text);
        if body.is_empty() {
            return Some("bad");
        }
        if self.limited(me, RateTab::Msg, MSG_PER_MIN) {
            return Some("rate");
        }
        let n = self.h.name(token);
        let name = if n.is_empty() { token_name_fallback(token) } else { n };
        let now = self.h.now();
        {
            let mut s = self.st.lock();
            s.pseq += 1;
            let id = s.pseq;
            let lst = s.pchat.entry(pid.clone()).or_default();
            lst.push(Line { id, from: me.to_string(), name: name.clone(), t: body.clone(), ts: now });
            if lst.len() > PCHAT_KEEP {
                let cut = lst.len() - PCHAT_KEEP;
                lst.drain(..cut);
            }
        }
        self.log_line("party", &pid, me, &name, &body, false);
        None
    }

    fn lines_json(lines: &[&Line], me: &str, now: f64) -> Value {
        Value::Array(
            lines
                .iter()
                .map(|x| json!({"id": x.id, "name": x.name, "t": x.t, "me": x.from == me, "age": std::cmp::max(0, (now - x.ts).trunc() as i64)}))
                .collect(),
        )
    }

    // py: social.py:1052 Social._pchat_view
    fn pchat_view(&self, token: &str, me: &str, since: i64) -> Value {
        let Some(pid) = self.p.pid(token) else { return json!({"lines": [], "last": since}) };
        let now = self.h.now();
        let lst: Vec<Line> = self.st.lock().pchat.get(&pid).cloned().unwrap_or_default();
        let after: Vec<&Line> = lst.iter().filter(|x| x.id > since).collect();
        let lines = &after[after.len().saturating_sub(PCHAT_SHOW)..];
        json!({"lines": Self::lines_json(lines, me, now), "last": lst.last().map(|x| x.id).unwrap_or(since)})
    }

    // py: social.py:1063 Social._lobby_chat
    /// S_LSAY / S_LCHAT: the LOBBY channel of the token's lobby (lobby_of); the reply has the party chat's shape + "lk".
    fn lobby_chat(&self, token: &str, me: &str, sub: i64, arg: &str) -> Value {
        let key: Option<String> = self.lobby_of.as_ref().and_then(|f| f(token)).filter(|k| !k.is_empty());
        let mut since = 0i64;
        if sub == S_LCHAT {
            let s = strip(arg);
            if is_digits(s) && s.len() < 15 {
                since = s.parse().unwrap_or(0);
            }
        }
        let Some(key) = key else {
            return json!({"ok": if sub == S_LSAY { 0 } else { 1 }, "err": if sub == S_LSAY { "nolobby" } else { "" }, "pchat": {"lines": [], "last": since}, "lk": 0});
        };
        let lk = {
            let v = sha1_hex7(&key);
            if v == 0 {
                1
            } else {
                v
            }
        };
        let now = self.h.now();
        if sub == S_LSAY {
            let body = Self::clean_text(arg);
            if body.is_empty() {
                return json!({"ok": 0, "err": "bad", "lk": lk});
            }
            if self.mute_left(me) != 0 {
                return json!({"ok": 0, "err": "muted", "lk": lk});
            }
            if self.limited(me, RateTab::Msg, MSG_PER_MIN) {
                return json!({"ok": 0, "err": "rate", "lk": lk});
            }
            let n = self.h.name(token);
            let name = if n.is_empty() { token_name_fallback(token) } else { n };
            self.log_line("lobby", &lk.to_string(), me, &name, &body, false);
            let mut s = self.st.lock();
            s.pseq += 1;
            let id = s.pseq;
            let lst = s.lchat.entry(key.clone()).or_default();
            lst.push(Line { id, from: me.to_string(), name, t: body, ts: now });
            if lst.len() > PCHAT_KEEP {
                let cut = lst.len() - PCHAT_KEEP;
                lst.drain(..cut);
            }
            s.lchat.retain(|_, v| !(v.is_empty() || now - v[v.len() - 1].ts > LCHAT_IDLE_S));
        }
        let lst: Vec<Line> = self.st.lock().lchat.get(&key).cloned().unwrap_or_default();
        let after: Vec<&Line> = lst.iter().filter(|x| x.id > since).collect();
        let lines = &after[after.len().saturating_sub(PCHAT_SHOW)..];
        json!({"ok": 1, "err": "", "lk": lk, "pchat": {"lines": Self::lines_json(lines, me, now), "last": lst.last().map(|x| x.id).unwrap_or(since)}})
    }

    // py: social.py:1098 Social._quick
    /// S_PCHAT / S_MLOG / S_LSAY / S_LCHAT / S_MUTE: the cheap path (no database, no state, no last-seen row); every reply carries "mute".
    fn quick(&self, token: &str, sub: i64, arg: &str) -> Value {
        self.touch(token);
        let me = self.acct(token);
        let mut res = self.quick1(token, &me, sub, arg);
        if let Value::Object(o) = &mut res {
            o.insert("mute".into(), json!(self.mute_left(&me)));
        }
        res
    }

    // py: social.py:1105 Social._quick1
    fn quick1(&self, token: &str, me: &str, sub: i64, arg: &str) -> Value {
        if sub == S_MUTE {
            let e = self.mute_entry(me);
            let why = match e {
                Some((_, why)) if self.mute_left(me) != 0 => why.chars().take(120).collect::<String>(),
                _ => String::new(),
            };
            return json!({"ok": 1, "err": "", "why": why});
        }
        if sub == S_PCHAT {
            let s = strip(arg);
            let since = if is_digits(s) && s.len() < 15 { s.parse().unwrap_or(0) } else { 0 };
            return json!({"ok": 1, "err": "", "pchat": self.pchat_view(token, me, since)});
        }
        if sub == S_LSAY || sub == S_LCHAT {
            return self.lobby_chat(token, me, sub, arg);
        }
        let (chan, text) = match arg.find('\n') {
            Some(p) => (&arg[..p], &arg[p + 1..]),
            None => (arg, ""),
        };
        let chan = strip(chan);
        let body = Self::clean_text(text);
        if (chan != "m" && chan != "t") || body.is_empty() {
            return json!({"ok": 0, "err": "bad"});
        }
        if self.limited(me, RateTab::Log, MSG_PER_MIN * 2) {
            return json!({"ok": 0, "err": "rate"});
        }
        let n = self.h.name(token);
        let name = if n.is_empty() { token_name_fallback(token) } else { n };
        let muted = self.mute_left(me) != 0;
        let now = self.h.now();
        {
            let mut s = self.st.lock();
            s.mlog.push_back((now, chan.to_string(), me.to_string(), name.clone(), body.clone()));
            if s.mlog.len() > MLOG_KEEP {
                s.mlog.pop_front();
            }
        }
        let kind = if chan == "m" { "match" } else { "team" };
        let rb = repr_str(&body);
        if muted {
            self.log(&["CHAT", kind, me, &name, &rb, "(muted)"]);
        } else {
            self.log(&["CHAT", kind, me, &name, &rb]);
        }
        let room = self.match_of.as_ref().and_then(|f| f(token)).unwrap_or_default();
        self.log_line(kind, &room, me, &name, &body, muted);
        if muted {
            json!({"ok": 0, "err": "muted"})
        } else {
            json!({"ok": 1, "err": ""})
        }
    }

    // py: social.py:1138 Social._adopt_once
    fn adopt_once(&self, c: &Connection, token: &str, key: &str) {
        if key.starts_with("s:") && !self.st.lock().adopt.contains(token) {
            self.st.lock().adopt.insert(token.to_string());
            if !self.adopt(c, token, key) {
                self.st.lock().adopt.remove(token); // failed (locked): try again next time
            }
        }
    }

    // py: social.py:1144 Social._rel
    /// None | 'friend' | 'out' (a asked b) | 'in' (b asked a).
    fn rel(&self, c: &Connection, a: &str, b: &str) -> Res<Option<&'static str>> {
        let r: Option<(Option<String>, Option<i64>)> = c
            .query_row("SELECT a, state FROM social_friends WHERE (a=? AND b=?) OR (a=? AND b=?)", params![a, b, b, a], |r| Ok((r.get(0)?, r.get(1)?)))
            .optional()?;
        Ok(match r {
            None => None,
            Some((ra, st)) => Some(if st == Some(1) {
                "friend"
            } else if ra.as_deref() == Some(a) {
                "out"
            } else {
                "in"
            }),
        })
    }

    // py: social.py:1151 Social._resolve
    /// A friend target typed or clicked: an account id, a 17 digit Steam id or an exact player name -> Ok(account) or Err(error code).
    fn resolve(&self, c: &Connection, text: &str, om: &Om) -> Res<Result<String, String>> {
        let text = strip(text);
        if is_acct_key(text) {
            if text.starts_with("s:") {
                let t = &text[2..];
                let q = "SELECT 1 FROM players WHERE steamid=? LIMIT 1";
                let sid = if is_digits(t) { bind_int(c, q, t)? } else { -1 };
                let found = c.query_row(q, params![sid], |_| Ok(())).optional()?.is_some();
                if !found && !om.m.contains_key(text) {
                    return Ok(Err("unknown".into()));
                }
            } else {
                let found = c.query_row("SELECT 1 FROM social_seen WHERE acct=?", params![text], |_| Ok(())).optional()?.is_some();
                if !found && !om.m.contains_key(text) {
                    return Ok(Err("unknown".into()));
                }
            }
            return Ok(Ok(text.to_string()));
        }
        if is_steam_text(text) {
            let key = format!("s:{}", text);
            let q = "SELECT 1 FROM players WHERE steamid=? LIMIT 1";
            let sid = bind_int(c, q, text)?;
            let found = c.query_row(q, params![sid], |_| Ok(())).optional()?.is_some();
            if !found && !om.m.contains_key(&key) {
                return Ok(Err("unknown".into()));
            }
            return Ok(Ok(key));
        }
        let found = self.search_raw(c, "", text, om, true)?;
        if found.len() == 1 {
            return Ok(Ok(found[0].0.clone()));
        }
        Ok(Err(if found.is_empty() { "unknown" } else { "ambiguous" }.to_string()))
    }

    // py: social.py:1171 Social._search
    /// The hits as (account, name, rel, status).
    fn search_raw(&self, c: &Connection, me: &str, text: &str, om: &Om, exact: bool) -> Res<Vec<(String, String, String, String)>> {
        let text: String = strip(text).chars().take(31).collect();
        if text.chars().count() < 2 {
            return Ok(Vec::new());
        }
        let low = text.to_lowercase();
        let like = low.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_");
        let mut hits: IndexMap<String, String> = IndexMap::new();
        let add = |hits: &mut IndexMap<String, String>, key: String, name: Option<String>| {
            let name = name.unwrap_or_default();
            if !key.is_empty() && key != me && !hits.contains_key(&key) && !name.is_empty() {
                hits.insert(key, name);
            }
        };
        if is_steam_text(&text) {
            let key = format!("s:{}", text);
            let q = "SELECT name FROM players WHERE steamid=? AND name<>'' ORDER BY last_seen DESC LIMIT 1";
            let sid = bind_int(c, q, &text)?;
            let r: Option<Option<String>> = c.query_row(q, params![sid], |r| r.get(0)).optional()?;
            if r.is_some() || om.m.contains_key(&key) {
                let nm = match r {
                    Some(n) => n,
                    None => Some(self.name_of(c, &key, om)?),
                };
                add(&mut hits, key, nm);
            }
        } else {
            for toks in om.m.values() {
                for t in toks {
                    let n = self.h.name(t);
                    let nl = n.to_lowercase();
                    if nl == low || (!exact && nl.starts_with(&low)) {
                        let a = self.acct(t);
                        add(&mut hits, a, Some(n));
                    }
                }
            }
            let arg = if exact { low.clone() } else { format!("{}%", like) };
            let cond = if exact { " = ?" } else { " LIKE ? ESCAPE '\\'" };
            {
                let mut stmt = c.prepare(&format!("SELECT token_id, name, tag FROM names WHERE lower(name){} LIMIT 40", cond))?;
                let rows = stmt.query_map(params![arg], |r| Ok((r.get::<_, Option<String>>(0)?, r.get::<_, Option<String>>(1)?, r.get::<_, Option<String>>(2)?)))?.collect::<Res<Vec<_>>>()?;
                for (tid, n, tg) in rows {
                    let tid = tid.unwrap_or_default();
                    let key = if tid.starts_with("s:") { tid.clone() } else { acct_hash(&tid) };
                    let nm = match (&tg, &n) {
                        (Some(t), _) if !t.is_empty() => Some(format!("[{}] {}", t, n.clone().unwrap_or_default())),
                        _ => n.clone(),
                    };
                    add(&mut hits, key, nm);
                }
            }
            {
                let mut stmt = c.prepare(&format!("SELECT steamid, name FROM players WHERE steamid>0 AND lower(name){} ORDER BY last_seen DESC LIMIT 40", cond))?;
                let rows = stmt.query_map(params![arg], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, Option<String>>(1)?)))?.collect::<Res<Vec<_>>>()?;
                for (sid, n) in rows {
                    add(&mut hits, format!("s:{}", sid), n);
                }
            }
            {
                let mut stmt = c.prepare(&format!("SELECT acct, name FROM social_seen WHERE lower(name){} LIMIT 40", cond))?;
                let rows = stmt.query_map(params![arg], |r| Ok((r.get::<_, Option<String>>(0)?, r.get::<_, Option<String>>(1)?)))?.collect::<Res<Vec<_>>>()?;
                for (key, n) in rows {
                    add(&mut hits, key.unwrap_or_default(), n);
                }
            }
        }
        let mut out = Vec::new();
        for (key, name) in hits.into_iter().take(SEARCH_MAX) {
            let rel = if me.is_empty() { None } else { self.rel(c, me, &key)? };
            let rel = rel.unwrap_or("").to_string();
            let status = if rel == "friend" { self.status_of(&key, Some(om)).to_string() } else { String::new() };
            out.push((key, name, rel, status));
        }
        Ok(out)
    }

    fn search(&self, c: &Connection, me: &str, text: &str, om: &Om, exact: bool) -> Res<Vec<Value>> {
        Ok(self
            .search_raw(c, me, text, om, exact)?
            .into_iter()
            .map(|(id, name, rel, status)| json!({"id": id, "name": name, "status": status, "rel": rel}))
            .collect())
    }

    // py: social.py:1218 Social._add
    fn add(&self, c: &Connection, token: &str, me: &str, arg: &str, om: &Om) -> Res<Option<String>> {
        let key = match self.resolve(c, arg, om)? {
            Ok(k) => k,
            Err(e) => return Ok(Some(e)),
        };
        if key == me {
            return Ok(Some("self".into()));
        }
        let rel = self.rel(c, me, &key)?;
        if rel == Some("friend") {
            return Ok(Some("already".into()));
        }
        if rel == Some("out") {
            return Ok(Some("pending".into()));
        }
        let now = self.h.now();
        if rel == Some("in") {
            return self.accept(c, token, me, &key, om);
        }
        let n: i64 = c.query_row("SELECT COUNT(*) FROM social_friends WHERE (a=? OR b=?) AND state=1", params![me, me], |r| r.get(0))?;
        if n >= FRIEND_MAX {
            return Ok(Some("limit".into()));
        }
        let n: i64 = c.query_row("SELECT COUNT(*) FROM social_friends WHERE a=? AND state=0", params![me], |r| r.get(0))?;
        if n >= REQUEST_MAX {
            return Ok(Some("limit".into()));
        }
        c.execute("INSERT OR REPLACE INTO social_friends(a,b,state,since) VALUES(?,?,0,?)", params![me, key, now])?;
        commit(c)?;
        let nm = self.h.name(token);
        self.note_acct(&key, "freq", if nm.is_empty() { "PLAYER" } else { &nm });
        let a = self.acct(token);
        let a12: String = a.chars().take(12).collect();
        let k14: String = key.chars().take(14).collect();
        self.log(&["SOCIAL request", &a12, "->", &k14]);
        Ok(None)
    }

    // py: social.py:1243 Social._accept
    fn accept(&self, c: &Connection, token: &str, me: &str, arg: &str, om: &Om) -> Res<Option<String>> {
        let key = match self.resolve(c, arg, om)? {
            Ok(k) => k,
            Err(e) => return Ok(Some(e)),
        };
        let r = c.query_row("SELECT 1 FROM social_friends WHERE a=? AND b=? AND state=0", params![key, me], |_| Ok(())).optional()?;
        if r.is_none() {
            return Ok(Some("expired".into()));
        }
        let n: i64 = c.query_row("SELECT COUNT(*) FROM social_friends WHERE (a=? OR b=?) AND state=1", params![me, me], |r| r.get(0))?;
        if n >= FRIEND_MAX {
            return Ok(Some("limit".into()));
        }
        c.execute("UPDATE social_friends SET state=1, since=? WHERE a=? AND b=?", params![self.h.now(), key, me])?;
        commit(c)?;
        let nm = self.h.name(token);
        self.note_acct(&key, "facc", if nm.is_empty() { "PLAYER" } else { &nm });
        Ok(None)
    }

    // py: social.py:1257 Social._remove
    fn remove(&self, c: &Connection, me: &str, arg: &str) -> Res<Option<String>> {
        let key = strip(arg);
        if !is_acct_key(key) {
            return Ok(Some("bad".into()));
        }
        c.execute("DELETE FROM social_friends WHERE (a=? AND b=?) OR (a=? AND b=?)", params![me, key, key, me])?;
        c.execute("DELETE FROM social_msgs WHERE (a=? AND b=?) OR (a=? AND b=?)", params![me, key, key, me])?;
        commit(c)?;
        Ok(None)
    }

    // --- private messages (friends only)
    // py: social.py:1267 Social._msg
    fn msg(&self, c: &Connection, me: &str, key: &str, text: &str, sender: &str) -> Res<Option<String>> {
        if !is_acct_key(key) {
            return Ok(Some("bad".into()));
        }
        if self.rel(c, me, key)? != Some("friend") {
            return Ok(Some("notfriend".into()));
        }
        let body = Self::clean_text(text);
        if body.is_empty() {
            return Ok(Some("bad".into()));
        }
        if self.limited(me, RateTab::Msg, MSG_PER_MIN) {
            return Ok(Some("rate".into()));
        }
        let now = self.h.now();
        c.execute("INSERT INTO social_msgs(a, b, ts, body, rd) VALUES(?, ?, ?, ?, 0)", params![me, key, now, body])?;
        c.execute(
            "DELETE FROM social_msgs WHERE ((a=? AND b=?) OR (a=? AND b=?)) AND id NOT IN (SELECT id FROM social_msgs WHERE (a=? AND b=?) OR (a=? AND b=?) \
             ORDER BY id DESC LIMIT ?)",
            params![me, key, key, me, me, key, key, me, MSG_KEEP],
        )?;
        let gc = {
            let mut s = self.st.lock();
            if now - s.mgc_at > 3600.0 {
                s.mgc_at = now;
                true
            } else {
                false
            }
        };
        if gc {
            c.execute("DELETE FROM social_msgs WHERE ts < ?", params![now - MSG_AGE_S])?;
        }
        commit(c)?;
        let shown = ingame_text(&body, MSG_NOTE_LEN);
        if !shown.is_empty() {
            let who = if sender.is_empty() { "PLAYER" } else { sender };
            self.note_acct(key, "msg", &format!("{}\x1f{}", who.replace('\x1f', " "), shown));
        }
        Ok(None)
    }

    // py: social.py:1291 Social._chat
    /// The conversation with one friend, oldest first; what the friend sent is marked read.
    fn chat(&self, c: &Connection, me: &str, key: &str) -> Res<Value> {
        let rows: Vec<(i64, Option<String>, Option<f64>, String)> = {
            let mut stmt = c.prepare("SELECT id, a, ts, body FROM social_msgs WHERE (a=? AND b=?) OR (a=? AND b=?) ORDER BY id DESC LIMIT ?")?;
            let r = stmt.query_map(params![me, key, key, me, CHAT_SHOW], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?)))?.collect::<Res<Vec<_>>>()?;
            r
        };
        if c.query_row("SELECT 1 FROM social_msgs WHERE a=? AND b=? AND rd=0 LIMIT 1", params![key, me], |_| Ok(())).optional()?.is_some() {
            c.execute("UPDATE social_msgs SET rd=1 WHERE a=? AND b=? AND rd=0", params![key, me])?;
            commit(c)?;
        }
        let now = self.h.now();
        let mut out: Vec<Value> = Vec::new();
        let mut size = 0usize;
        for (i, a, ts, body) in rows {
            size += body.len() + 48;
            if size > CHAT_BYTES && !out.is_empty() {
                break;
            }
            let t = ts.filter(|x| *x != 0.0).unwrap_or(now);
            out.push(json!({"id": i, "me": a.as_deref() == Some(me), "t": body, "age": std::cmp::max(0, (now - t).trunc() as i64)}));
        }
        out.reverse();
        Ok(json!({"with": key, "msgs": out}))
    }

    // py: social.py:1309 Social._party_op
    fn party_op(&self, c: &Connection, token: &str, me: &str, sub: i64, arg: &str) -> Res<Option<String>> {
        let key = strip(arg);
        if sub == S_INVITE {
            if self.rel(c, me, key)? != Some("friend") {
                return Ok(Some("notfriend".into()));
            }
            let Some(t) = self.token_for(key) else { return Ok(Some("offline".into())) };
            return Ok(self.p.invite_token(token, &t).map(|s| s.to_string()));
        }
        if sub == S_JOIN || sub == S_DECLINE {
            let inviter = self.p.invites_for(token).into_iter().map(|(i, _)| i).find(|i| self.acct(i) == key);
            let Some(inviter) = inviter else { return Ok(Some("expired".into())) };
            if sub == S_DECLINE {
                self.p.decline_token(token, &inviter);
                return Ok(None);
            }
            return Ok(self.p.accept_token(token, &inviter).err().map(|s| s.to_string()));
        }
        let has_party = self.p.pid(token).is_some();
        let target = if has_party { self.p.members(token).into_iter().find(|m| m != token && self.acct(m) == key) } else { None };
        let Some(target) = target else { return Ok(Some(if has_party { "notmember" } else { "noparty" }.to_string())) };
        Ok(if sub == S_KICK { self.p.kick(token, &target) } else { self.p.promote(token, &target) }.map(|s| s.to_string()))
    }

    // --- the reply state
    // py: social.py:1336 Social._state
    fn state(&self, c: &Connection, token: &str, me: &str, lang: i64) -> Res<Vec<(String, Value)>> {
        let _ = lang;
        let om = self.online_map();
        let now = self.h.now();
        let mut friends: Vec<(i64, String, Value)> = Vec::new();
        let mut inc: Vec<(i64, Value)> = Vec::new();
        let mut out: Vec<(i64, Value)> = Vec::new();
        let mut fset: HashSet<String> = HashSet::new();
        let mates: HashMap<String, String> = if self.p.pid(token).is_some() { self.p.members(token).into_iter().map(|t| (self.acct(&t), t)).collect() } else { HashMap::new() };
        let rows: Vec<(String, String, Option<i64>, Option<f64>)> = {
            let mut stmt = c.prepare("SELECT a, b, state, since FROM social_friends WHERE a=? OR b=?")?;
            let r = stmt
                .query_map(params![me, me], |r| Ok((r.get::<_, Option<String>>(0)?.unwrap_or_default(), r.get::<_, Option<String>>(1)?.unwrap_or_default(), r.get(2)?, r.get(3)?)))?
                .collect::<Res<Vec<_>>>()?;
            r
        };
        for (a, b, st, since) in rows {
            let other = if a == me { b.clone() } else { a.clone() };
            let name = self.name_of(c, &other, &om)?;
            let sn = since.filter(|x| *x != 0.0);
            if st == Some(1) {
                fset.insert(other.clone());
                let status = self.status_of(&other, Some(&om));
                let r: Option<Option<f64>> = c.query_row("SELECT last FROM social_seen WHERE acct=?", params![other], |r| r.get(0)).optional()?;
                let seen = if status != "off" {
                    Value::Null
                } else {
                    match r {
                        Some(Some(l)) if l != 0.0 => json!((now - l).trunc() as i64),
                        _ => Value::Null,
                    }
                };
                let lname = name.to_lowercase();
                friends.push((
                    rank(status),
                    lname,
                    json!({"id": other, "name": name, "status": status, "seen": seen, "party": mates.contains_key(&other) && other != me, "since": sn.unwrap_or(0.0).trunc() as i64}),
                ));
            } else if a == me {
                out.push(((now - sn.unwrap_or(now)).trunc() as i64, json!({"id": other, "name": name, "age": (now - sn.unwrap_or(now)).trunc() as i64})));
            } else {
                inc.push(((now - sn.unwrap_or(now)).trunc() as i64, json!({"id": other, "name": name, "age": (now - sn.unwrap_or(now)).trunc() as i64})));
            }
        }
        friends.sort_by(|x, y| (x.0, &x.1).cmp(&(y.0, &y.1)));
        inc.sort_by_key(|x| x.0);
        out.sort_by_key(|x| x.0);
        let mut party = Value::Null;
        if let Some(pid) = self.p.pid(token) {
            let members = self.p.members(token);
            if !members.is_empty() {
                let lead = self.p.leader(token);
                let mut mem: Vec<(bool, bool, String, Value)> = Vec::new();
                for t in &members {
                    let k = self.acct(t);
                    let n = self.h.name(t);
                    let n = if n.is_empty() { "PLAYER".to_string() } else { n };
                    let is_lead = lead.as_deref() == Some(t.as_str());
                    let ready = is_lead || self.p.is_ready(t) || self.h.mm_state(t) == Some("search");
                    mem.push((!is_lead, t != token, n.to_lowercase(), json!({"id": k, "name": n, "status": self.status_of(&k, Some(&om)), "leader": is_lead, "ready": ready, "me": t == token})));
                }
                mem.sort_by(|x, y| (x.0, x.1, &x.2).cmp(&(y.0, y.1, &y.2)));
                let searching = members.iter().any(|t| self.h.mm_state(t) == Some("search"));
                let lsearch = lead.as_ref().map(|l| !l.is_empty() && self.h.mm_state(l) == Some("search")).unwrap_or(false);
                let lead_acct = match &lead {
                    Some(l) if !l.is_empty() => self.acct(l),
                    _ => String::new(),
                };
                party = json!({
                    "id": pid, "leader": lead_acct, "meLeader": lead.as_deref() == Some(token), "max": self.p.max,
                    "members": mem.into_iter().map(|x| x.3).collect::<Vec<_>>(), "searching": searching, "lsearch": lsearch
                });
            }
        }
        let mut pinv = Vec::new();
        for (inviter, age) in self.p.invites_for(token) {
            let n = self.h.name(&inviter);
            pinv.push(json!({"id": self.acct(&inviter), "name": if n.is_empty() { "PLAYER".to_string() } else { n }, "age": age.trunc() as i64}));
        }
        let mut unread: IndexMap<String, i64> = IndexMap::new();
        {
            let mut stmt = c.prepare("SELECT a, COUNT(*) FROM social_msgs WHERE b=? AND rd=0 GROUP BY a")?;
            let r = stmt.query_map(params![me], |r| Ok((r.get::<_, Option<String>>(0)?.unwrap_or_default(), r.get::<_, i64>(1)?)))?.collect::<Res<Vec<_>>>()?;
            for (a, n) in r {
                unread.insert(a, n);
            }
        }
        let mut recent: Vec<Value> = Vec::new();
        let parts: Vec<(Option<String>, i64)> = {
            let mut stmt = c.prepare(
                "SELECT CASE WHEN a=? THEN b ELSE a END AS o, MAX(id) AS m FROM social_msgs WHERE a=? OR b=? GROUP BY o ORDER BY m DESC LIMIT ?",
            )?;
            let r = stmt.query_map(params![me, me, me, RECENT_MAX], |r| Ok((r.get(0)?, r.get(1)?)))?.collect::<Res<Vec<_>>>()?;
            r
        };
        if !parts.is_empty() {
            let q = format!("SELECT id, a, ts, body FROM social_msgs WHERE id IN ({})", vec!["?"; parts.len()].join(","));
            let ids: Vec<i64> = parts.iter().map(|p| p.1).collect();
            let last: HashMap<i64, (Option<String>, Option<f64>, String)> = {
                let mut stmt = c.prepare(&q)?;
                let r = stmt
                    .query_map(rusqlite::params_from_iter(ids.iter()), |r| Ok((r.get::<_, i64>(0)?, (r.get(1)?, r.get(2)?, r.get::<_, String>(3)?))))?
                    .collect::<Res<Vec<_>>>()?;
                r.into_iter().collect()
            };
            for (other, mid) in &parts {
                let other = other.clone().unwrap_or_default();
                if fset.contains(&other) {
                    if let Some((a, ts, body)) = last.get(mid) {
                        let t = ts.filter(|x| *x != 0.0).unwrap_or(now);
                        recent.push(json!({
                            "id": other, "name": self.name_of(c, &other, &om)?, "t": ingame_text(body, 60), "me": a.as_deref() == Some(me),
                            "age": std::cmp::max(0, (now - t).trunc() as i64), "unread": unread.get(&other).copied().unwrap_or(0)
                        }));
                    }
                }
            }
        }
        let nm = self.h.name(token);
        let unread_v: Map<String, Value> = unread.into_iter().map(|(k, v)| (k, json!(v))).collect();
        Ok(vec![
            ("v".into(), json!(1)),
            ("me".into(), json!({"id": me, "name": if nm.is_empty() { token_name_fallback(token) } else { nm }})),
            ("friends".into(), Value::Array(friends.into_iter().map(|x| x.2).collect())),
            ("in".into(), Value::Array(inc.into_iter().map(|x| x.1).collect())),
            ("out".into(), Value::Array(out.into_iter().map(|x| x.1).collect())),
            ("party".into(), party),
            ("pinv".into(), Value::Array(pinv)),
            ("max".into(), json!(self.p.max)),
            ("unread".into(), Value::Object(unread_v)),
            ("recent".into(), Value::Array(recent)),
        ])
    }
}

/// Lines of a text file read in text mode (universal newlines: \n, \r\n, \r).
fn universal_lines(s: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let b = s.as_bytes();
    let mut start = 0usize;
    let mut i = 0usize;
    while i < b.len() {
        if b[i] == b'\n' {
            out.push(&s[start..i]);
            start = i + 1;
        } else if b[i] == b'\r' {
            out.push(&s[start..i]);
            if i + 1 < b.len() && b[i + 1] == b'\n' {
                i += 1;
            }
            start = i + 1;
        }
        i += 1;
    }
    if start < b.len() {
        out.push(&s[start..]);
    }
    out
}
