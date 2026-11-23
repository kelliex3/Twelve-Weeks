use std::collections::VecDeque;
use std::fs::File;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard, OnceLock};

use regex::Regex;
use serde_json::Value;

use crate::py;
use crate::timeutil::{self, localtime, mktime};

const MAX_EVENTS: usize = 40000;
const MAX_ERRORS: usize = 300;
const FOLLOW_EVERY: f64 = 30.0;
const ERR_KEYS: [&str; 8] = ["err", "traceback", "fatal", "exception", "not saved", "not started", "corrupt", "crash"];

struct Res {
    err: Regex,
    header: Regex,
    hostnet: Regex,
    ping: Regex,
    map: Regex,
    jipname: Regex,
    winner: Regex,
    after: Regex,
    left: Regex,
    status_kv: Regex,
    announce: Regex,
}

fn res() -> &'static Res {
    static R: OnceLock<Res> = OnceLock::new();
    R.get_or_init(|| {
        let re = |s: &str| Regex::new(s).unwrap_or_else(|_| Regex::new("$^").expect("empty regex"));
        Res {
            err: re(r"(?i)ERR\b|ERROR|Traceback|FATAL|Exception|HANDLER ERR|-ERR\b|not saved|not started|CORRUPT|crash"),
            header: re(r"^# daemon (log continued )?(\w{3} \w{3} +\d+ \d\d:\d\d:\d\d \d{4})"),
            hostnet: re(r" host (.*?) net (\S+) ping-ms "),
            ping: re(r"ping-ms (\{[^}]*\})"),
            map: re(r" map (\S+)"),
            jipname: re(r"^\S+ (.*?) slot "),
            winner: re(r"winner team (\d+)"),
            after: re(r"after (\d+) s"),
            left: re(r"^\S+ (.*?) (left \(0x02\)|timed out|admin kick|jip never loaded|[^|]*?)(?: \||$)"),
            status_kv: re(r"(\w+)=(\d+)"),
            announce: re(r"^ANNOUNCE \(.*?\) (.*)$"),
        }
    })
}

pub fn err_re() -> &'static Regex {
    &res().err
}

enum Ev {
    Start,
    Formed { mid: Box<str>, players: Vec<String>, hostnet: Option<(String, String)>, ping: Option<Option<Value>> },
    GameStart { mid: Box<str>, map: Option<String> },
    Jip { mid: Box<str>, players: Vec<String>, name: Option<String> },
    End { mid: Box<str>, winner: Option<i64>, secs: Option<i64>, players: Vec<String> },
    Left { mid: Box<str>, rest: Box<str>, removed: bool },
    Close { mid: Box<str>, why: Box<str> },
    Other,
}

struct Event {
    ts: f64,
    ev: Ev,
}

#[derive(Clone)]
pub struct Left {
    pub name: String,
    pub why: String,
}

#[derive(Clone)]
pub struct Match {
    pub id: String,
    pub formed: f64,
    pub started: Option<f64>,
    pub ended: Option<f64>,
    pub closed: Option<f64>,
    pub state: &'static str,
    pub map: Option<String>,
    pub players: Vec<String>,
    pub host: Option<String>,
    pub net: Option<String>,
    pub ping_ms: Option<Value>,
    pub winner: Option<i64>,
    pub secs: Option<i64>,
    pub left: Vec<Left>,
    pub closed_why: String,
    pub jip: Vec<String>,
}

#[derive(Clone, Copy)]
pub struct Status {
    pub ts: f64,
    pub online: i64,
    pub queue: i64,
    pub matches: i64,
}

pub struct ErrLine {
    pub ts: f64,
    pub line: String,
}

pub struct LogState {
    pub path: PathBuf,
    followed: f64,
    checked: f64,
    pos: u64,
    sig: Option<u64>,
    events: VecDeque<Event>,
    pub errors: VecDeque<ErrLine>,
    pub announces: VecDeque<(f64, String)>,
    pub status: Option<Status>,
    pub started: Option<f64>,
    pub version: String,
    pub listen: String,
    cur_date: Option<(i32, i32, i32)>,
    last_secs: Option<i64>,
    loaded: bool,
    gen: u64,
    derived: Option<(u64, Arc<Vec<Match>>)>,
    lower: String,
}

pub struct LogIndex {
    pub follow: Option<PathBuf>,
    st: Mutex<LogState>,
}

fn visible_dirs(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(rd) = std::fs::read_dir(dir) {
        for e in rd.flatten() {
            let name = e.file_name();
            let n = name.to_string_lossy();
            if n.starts_with('.') {
                continue;
            }
            if e.path().is_dir() {
                out.push(e.path());
            }
        }
    }
    out
}

fn candidates(dist: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for d in visible_dirs(dist) {
        let p = d.join("host").join("daemon.log");
        if p.exists() {
            out.push(p);
        }
        for d2 in visible_dirs(&d) {
            let p = d2.join("host").join("daemon.log");
            if p.exists() {
                out.push(p);
            }
        }
        if d.file_name().map(|n| n.to_string_lossy().starts_with("host")).unwrap_or(false) {
            let p = d.join("daemon.log");
            if p.exists() {
                out.push(p);
            }
        }
    }
    out.sort();
    out.dedup();
    out
}

fn file_mtime(p: &Path) -> Option<f64> {
    std::fs::metadata(p).ok().map(|m| timeutil::mtime(&m))
}

pub fn newest_daemon_log(dist: &Path) -> PathBuf {
    let mut best = PathBuf::new();
    let mut bt = -1.0;
    for p in candidates(dist) {
        if let Some(t) = file_mtime(&p) {
            if t > bt {
                best = p;
                bt = t;
            }
        }
    }
    best
}

fn prior_daemon_logs(dist: &Path, current: &Path, keep: usize) -> Vec<PathBuf> {
    let mut found: Vec<(f64, PathBuf)> = candidates(dist)
        .into_iter()
        .filter(|p| p != current)
        .filter_map(|p| file_mtime(&p).map(|t| (t, p)))
        .collect();
    found.sort_by(|a, b| a.0.total_cmp(&b.0).then_with(|| a.1.cmp(&b.1)));
    let skip = found.len().saturating_sub(keep);
    found.into_iter().skip(skip).map(|(_, p)| p).collect()
}

pub fn rotated(path: &Path) -> Vec<PathBuf> {
    let (Some(dir), Some(name)) = (path.parent(), path.file_name()) else {
        return Vec::new();
    };
    let prefix = format!("{}.", name.to_string_lossy());
    let dir = if dir.as_os_str().is_empty() { Path::new(".") } else { dir };
    let mut rot: Vec<(u64, PathBuf)> = Vec::new();
    if let Ok(rd) = std::fs::read_dir(dir) {
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().into_owned();
            if let Some(suf) = n.strip_prefix(&prefix) {
                if py::is_digits(suf) {
                    if let Ok(k) = suf.parse::<u64>() {
                        rot.push((k, path.with_file_name(&n)));
                    }
                }
            }
        }
    }
    rot.sort_by(|a, b| b.cmp(a));
    rot.into_iter().map(|(_, p)| p).collect()
}

fn is_py_linebreak(c: char) -> bool {
    matches!(c, '\n' | '\r' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}' | '\u{2029}')
}

fn ends_with_digits(s: &str, n: usize) -> bool {
    s.len() >= n && s.as_bytes()[..n].iter().all(|b| b.is_ascii_digit())
}

fn parse_line(raw: &str) -> Option<(i64, i64, i64, &str)> {
    let b = raw.as_bytes();
    if b.len() < 11 || b[0] != b'[' || b[3] != b':' || b[6] != b':' || b[9] != b']' || b[10] != b' ' {
        return None;
    }
    if !ends_with_digits(&raw[1..], 2) || !ends_with_digits(&raw[4..], 2) || !ends_with_digits(&raw[7..], 2) {
        return None;
    }
    let n = |i: usize| ((b[i] - b'0') as i64) * 10 + (b[i + 1] - b'0') as i64;
    Some((n(1), n(4), n(7), &raw[11..]))
}

fn bracket(body: &str, key: &str) -> Vec<String> {
    let pat = format!("{} [", key);
    let Some(i) = body.find(&pat) else {
        return Vec::new();
    };
    match body[i..].find(']') {
        Some(j) if i + j > 0 => {
            let start = i + key.len() + 1;
            let end = i + j + 1;
            if start <= end {
                py::literal_list(&body[start..end])
            } else {
                Vec::new()
            }
        }
        _ => Vec::new(),
    }
}

fn first_word(b: &str) -> &str {
    b.split(' ').next().unwrap_or("")
}

fn cap1(re: &Regex, s: &str) -> Option<String> {
    re.captures(s).and_then(|c| c.get(1)).map(|m| m.as_str().to_string())
}

fn cap_int(re: &Regex, s: &str) -> Option<i64> {
    cap1(re, s).and_then(|v| v.parse::<i64>().ok())
}

fn parse_event(kind: &str, b: &str) -> Ev {
    let r = res();
    let mid: Box<str> = first_word(b).into();
    match kind {
        "match-formed" if !mid.is_empty() => {
            let hostnet = r.hostnet.captures(b).and_then(|c| Some((c.get(1)?.as_str().to_string(), c.get(2)?.as_str().to_string())));
            let ping = r.ping.captures(b).and_then(|c| c.get(1)).and_then(|m| py::literal_dict_max(m.as_str()));
            Ev::Formed { mid, players: bracket(b, "players"), hostnet, ping }
        }
        "game-start" => Ev::GameStart { mid, map: cap1(&r.map, b) },
        "jip-join" => Ev::Jip { mid, players: bracket(b, "players"), name: cap1(&r.jipname, b) },
        "match-end" => Ev::End { mid, winner: cap_int(&r.winner, b), secs: cap_int(&r.after, b), players: bracket(b, "players") },
        "left" => Ev::Left { mid, rest: b.into(), removed: b.contains("none (match removed)") },
        "freed" => Ev::Close { mid, why: "freed".into() },
        "reaped" => {
            let why = match b.split_once(' ') {
                Some((_, rest)) => format!("reaped: {}", py::head(rest, 80)),
                None => "reaped".to_string(),
            };
            Ev::Close { mid, why: why.into() }
        }
        _ => Ev::Other,
    }
}

fn parse_left(rest: &str, players: &[String]) -> Option<Left> {
    let after_mid = rest.split_once(' ').map(|(_, r)| r)?;
    let mut best: Option<&str> = None;
    for n in players {
        if !n.is_empty() && after_mid.len() > n.len() && after_mid.starts_with(n.as_str()) && after_mid[n.len()..].starts_with(' ') {
            if best.map(|b| n.len() > b.len()).unwrap_or(true) {
                best = Some(n);
            }
        }
    }
    if let Some(n) = best {
        let tail = &after_mid[n.len() + 1..];
        let why = match tail.find(" |") {
            Some(i) => &tail[..i],
            None => tail,
        };
        if !why.contains('|') {
            return Some(Left { name: n.to_string(), why: why.trim().to_string() });
        }
    }
    let c = res().left.captures(rest)?;
    Some(Left { name: c.get(1)?.as_str().to_string(), why: c.get(2)?.as_str().trim().to_string() })
}

fn new_match(id: &str, ts: f64) -> Match {
    Match {
        id: id.to_string(),
        formed: ts,
        started: None,
        ended: None,
        closed: None,
        state: "loading",
        map: None,
        players: Vec::new(),
        host: None,
        net: None,
        ping_ms: None,
        winner: None,
        secs: None,
        left: Vec::new(),
        closed_why: String::new(),
        jip: Vec::new(),
    }
}

fn close(m: &mut Match, ts: f64, why: &str) {
    if m.closed.is_none() {
        m.closed = Some(ts);
        m.closed_why = why.to_string();
        if m.state != "ended" {
            m.state = "closed";
        }
    }
}

fn derive(events: &VecDeque<Event>) -> Vec<Match> {
    let mut ms: Vec<Match> = Vec::new();
    let mut idx: std::collections::HashMap<Box<str>, usize> = std::collections::HashMap::new();
    for e in events {
        let ts = e.ts;
        match &e.ev {
            Ev::Start => {
                for m in ms.iter_mut() {
                    if m.closed.is_none() {
                        close(m, ts, "server restart");
                    }
                }
            }
            Ev::Formed { mid, players, hostnet, ping } => {
                let i = *idx.entry(mid.clone()).or_insert_with(|| {
                    ms.push(new_match(mid, ts));
                    ms.len() - 1
                });
                let m = &mut ms[i];
                m.formed = ts;
                m.players = players.clone();
                if let Some((h, n)) = hostnet {
                    m.host = Some(h.clone());
                    m.net = Some(n.clone());
                }
                if let Some(p) = ping {
                    m.ping_ms = p.clone();
                }
            }
            Ev::GameStart { mid, map } => {
                if let Some(&i) = idx.get(mid) {
                    let m = &mut ms[i];
                    m.started = Some(ts);
                    if m.state == "loading" {
                        m.state = "running";
                    }
                    if map.is_some() {
                        m.map = map.clone();
                    }
                }
            }
            Ev::Jip { mid, players, name } => {
                if let Some(&i) = idx.get(mid) {
                    let m = &mut ms[i];
                    if !players.is_empty() {
                        m.players = players.clone();
                    }
                    if let Some(n) = name {
                        m.jip.push(n.clone());
                    }
                }
            }
            Ev::End { mid, winner, secs, players } => {
                if let Some(&i) = idx.get(mid) {
                    let m = &mut ms[i];
                    m.winner = *winner;
                    m.secs = *secs;
                    if !players.is_empty() {
                        m.players = players.clone();
                    }
                    m.ended = Some(ts);
                    m.state = "ended";
                }
            }
            Ev::Left { mid, rest, removed } => {
                if let Some(&i) = idx.get(mid) {
                    let m = &mut ms[i];
                    if let Some(l) = parse_left(rest, &m.players) {
                        m.left.push(l);
                    }
                    if *removed {
                        close(m, ts, "everyone left");
                    }
                }
            }
            Ev::Close { mid, why } => {
                if let Some(&i) = idx.get(mid) {
                    close(&mut ms[i], ts, why);
                }
            }
            Ev::Other => {}
        }
    }
    ms
}

pub fn team_names(m: &Match) -> (Vec<String>, Vec<String>) {
    let mut t = (Vec::new(), Vec::new());
    for (i, n) in m.players.iter().enumerate() {
        if i % 2 == 0 {
            t.0.push(n.clone());
        } else {
            t.1.push(n.clone());
        }
    }
    t
}

pub fn result_for(m: &Match, name: &str) -> &'static str {
    let Some(w) = m.winner else {
        return "";
    };
    if m.state != "ended" {
        return "";
    }
    let Some(pos) = m.players.iter().position(|p| p == name) else {
        return "";
    };
    match w {
        2 => "D",
        0 | 1 => {
            if (pos as i64) % 2 == w {
                "W"
            } else {
                "L"
            }
        }
        _ => "",
    }
}

impl LogState {
    fn new(path: PathBuf, followed: f64) -> Self {
        LogState {
            path,
            followed,
            checked: 0.0,
            pos: 0,
            sig: None,
            events: VecDeque::new(),
            errors: VecDeque::new(),
            announces: VecDeque::new(),
            status: None,
            started: None,
            version: String::new(),
            listen: String::new(),
            cur_date: None,
            last_secs: None,
            loaded: false,
            gen: 0,
            derived: None,
            lower: String::new(),
        }
    }

    fn reset(&mut self) {
        self.pos = 0;
        self.sig = None;
        self.events.clear();
        self.errors.clear();
        self.announces.clear();
        self.status = None;
        self.started = None;
        self.version.clear();
        self.listen.clear();
        self.cur_date = None;
        self.last_secs = None;
        self.loaded = false;
        self.gen += 1;
        self.derived = None;
    }

    pub fn earliest(&self) -> Option<f64> {
        self.events.front().map(|e| e.ts)
    }

    pub fn matches(&mut self) -> Arc<Vec<Match>> {
        if let Some((g, m)) = &self.derived {
            if *g == self.gen {
                return m.clone();
            }
        }
        let m = Arc::new(derive(&self.events));
        self.derived = Some((self.gen, m.clone()));
        m
    }

    fn set_header(&mut self, text: &str) -> Option<(f64, bool)> {
        let c = res().header.captures(text)?;
        let (y, mo, d, h, mi, s) = timeutil::parse_ctime(c.get(2)?.as_str())?;
        self.cur_date = Some((y, mo, d));
        self.last_secs = Some((h * 3600 + mi * 60 + s) as i64);
        Some((mktime(y, mo, d, h, mi, s), c.get(1).is_some()))
    }

    fn ts(&mut self, hh: i64, mm: i64, ss: i64) -> f64 {
        let secs = hh * 3600 + mm * 60 + ss;
        let (mut y, mut mo, mut d) = self.cur_date.unwrap_or((1970, 1, 1));
        if let Some(last) = self.last_secs {
            if secs < last - 600 {
                let t = mktime(y, mo, d, 12, 0, 0) + 86400.0;
                let lt = localtime(t);
                self.cur_date = Some((lt.year, lt.month, lt.day));
                y = lt.year;
                mo = lt.month;
                d = lt.day;
            }
        }
        self.last_secs = Some(secs);
        mktime(y, mo, d, hh as i32, mm as i32, ss as i32)
    }

    fn feed_line(&mut self, raw: &str, mtime: f64) {
        if raw.starts_with("# daemon") {
            if let Some((t, cont)) = self.set_header(raw) {
                if !cont {
                    self.started = Some(t);
                    self.events.push_back(Event { ts: t, ev: Ev::Start });
                    self.gen += 1;
                }
            }
            return;
        }
        let Some((hh, mm, ss, body)) = parse_line(raw) else {
            return;
        };
        if self.cur_date.is_none() {
            let lt = localtime(mtime);
            self.cur_date = Some((lt.year, lt.month, lt.day));
            let first = hh * 3600 + mm * 60 + ss;
            if first > (lt.hour * 3600 + lt.min * 60 + lt.sec) as i64 + 120 {
                let t = mktime(lt.year, lt.month, lt.day, 12, 0, 0) - 86400.0;
                let l2 = localtime(t);
                self.cur_date = Some((l2.year, l2.month, l2.day));
            }
        }
        let ts = self.ts(hh, mm, ss);
        if let Some(rest) = body.strip_prefix("EVT ") {
            let (kind, b) = rest.split_once(' ').unwrap_or((rest, ""));
            self.events.push_back(Event { ts, ev: parse_event(kind, b) });
            self.gen += 1;
        } else if body.starts_with("STATUS ") {
            let mut online = None;
            let mut queue = None;
            let mut matches = None;
            for c in res().status_kv.captures_iter(body) {
                let (Some(k), Some(v)) = (c.get(1), c.get(2)) else {
                    continue;
                };
                let v = v.as_str().parse::<i64>().ok();
                match k.as_str() {
                    "online" => online = Some(v),
                    "queue" => queue = Some(v),
                    "matches" => matches = Some(v),
                    _ => {}
                }
            }
            if let Some(Some(o)) = online {
                self.status = Some(Status { ts, online: o, queue: queue.flatten().unwrap_or(0), matches: matches.flatten().unwrap_or(0) });
            }
        } else if body.starts_with("ANNOUNCE ") {
            if let Some(n) = res().announce.captures(body).and_then(|c| c.get(1)) {
                self.announces.push_back((ts, n.as_str().to_string()));
                if self.announces.len() > 600 {
                    self.announces.drain(..200);
                }
            }
        } else if let Some(rest) = body.strip_prefix("daemon version ") {
            if let Some(v) = rest.split_whitespace().next() {
                self.version = v.to_string();
            }
        } else if let Some(rest) = body.strip_prefix("TCP LISTEN ") {
            if let Some(v) = rest.split_whitespace().next() {
                self.listen = v.to_string();
            }
        }
        if body.starts_with("TCP (") || body.starts_with("UDP (") || py::head(body, 40).contains("recv") {
            return;
        }
        self.lower.clear();
        self.lower.push_str(body);
        self.lower.make_ascii_lowercase();
        if ERR_KEYS.iter().any(|k| self.lower.contains(k)) && res().err.is_match(body) {
            self.errors.push_back(ErrLine { ts, line: py::head(body, 300).to_string() });
            if self.errors.len() > MAX_ERRORS {
                self.errors.drain(..100);
            }
        }
    }

    fn read_new(&mut self, path: &Path, from: u64) -> u64 {
        let Ok(md) = std::fs::metadata(path) else {
            return from;
        };
        let size = md.len();
        let mtime = timeutil::mtime(&md);
        if size <= from {
            return from;
        }
        let Ok(mut f) = File::open(path) else {
            return from;
        };
        if f.seek(SeekFrom::Start(from)).is_err() {
            return from;
        }
        let mut rd = BufReader::with_capacity(1 << 16, f.take(size - from));
        let mut buf: Vec<u8> = Vec::with_capacity(512);
        let mut consumed: u64 = 0;
        let before = self.events.len();
        loop {
            buf.clear();
            let n = match rd.read_until(b'\n', &mut buf) {
                Ok(n) => n,
                Err(_) => break,
            };
            if n == 0 || buf.last() != Some(&b'\n') {
                break;
            }
            consumed += n as u64;
            let text = String::from_utf8_lossy(&buf[..n - 1]);
            if text.contains(is_py_linebreak) {
                for piece in text.split(is_py_linebreak) {
                    if !piece.is_empty() {
                        self.feed_line(piece, mtime);
                    }
                }
            } else {
                self.feed_line(&text, mtime);
            }
        }
        if self.events.len() > MAX_EVENTS {
            self.events.drain(..MAX_EVENTS / 4);
            self.gen += 1;
        }
        if self.events.len() != before {
            self.gen += 1;
        }
        from + consumed
    }

    fn rotated_from(&self, ino: u64) -> Option<PathBuf> {
        rotated(&self.path).into_iter().rev().find(|p| std::fs::metadata(p).map(|m| m.ino() == ino).unwrap_or(false))
    }
}

impl LogIndex {
    pub fn new(spec: &str) -> Self {
        let now = timeutil::now();
        if spec == "auto" || spec.starts_with("auto:") {
            let dir = &spec[4..];
            let dir = dir.strip_prefix(':').unwrap_or(dir);
            let follow = if dir.is_empty() { PathBuf::from("dist") } else { PathBuf::from(dir) };
            let path = newest_daemon_log(&follow);
            LogIndex { follow: Some(follow), st: Mutex::new(LogState::new(path, now)) }
        } else {
            LogIndex { follow: None, st: Mutex::new(LogState::new(PathBuf::from(spec), now)) }
        }
    }

    pub fn lock(&self) -> MutexGuard<'_, LogState> {
        self.st.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn refresh(&self) {
        let guard = match self.st.try_lock() {
            Ok(g) => Some(g),
            Err(std::sync::TryLockError::Poisoned(e)) => Some(e.into_inner()),
            Err(std::sync::TryLockError::WouldBlock) => None,
        };
        let mut st = match guard {
            Some(g) => g,
            None => {
                let g = self.lock();
                if g.loaded {
                    return;
                }
                g
            }
        };
        if st.path.as_os_str().is_empty() && self.follow.is_none() {
            return;
        }
        let now = timeutil::now();
        if now - st.checked < 1.5 {
            return;
        }
        st.checked = now;
        if let Some(follow) = &self.follow {
            if now - st.followed >= FOLLOW_EVERY || st.path.as_os_str().is_empty() {
                st.followed = now;
                let p = newest_daemon_log(follow);
                if !p.as_os_str().is_empty() && p != st.path {
                    st.path = p;
                    st.reset();
                }
            }
        }
        if st.path.as_os_str().is_empty() {
            return;
        }
        let Ok(md) = std::fs::metadata(&st.path) else {
            return;
        };
        let sig = md.ino();
        if st.loaded {
            if let Some(old_sig) = st.sig {
                if sig != old_sig {
                    match st.rotated_from(old_sig) {
                        Some(old) => {
                            let pos = st.pos;
                            st.read_new(&old, pos);
                            st.sig = Some(sig);
                            st.pos = 0;
                        }
                        None => st.reset(),
                    }
                }
            }
        }
        if let Some(s) = st.sig {
            if md.len() < st.pos || sig != s {
                st.reset();
            }
        }
        if !st.loaded {
            st.sig = Some(sig);
            let mut chain = Vec::new();
            if let Some(follow) = &self.follow {
                for pp in prior_daemon_logs(follow, &st.path, 4) {
                    chain.extend(rotated(&pp));
                    chain.push(pp);
                }
            }
            chain.extend(rotated(&st.path));
            for p in chain {
                st.cur_date = None;
                st.last_secs = None;
                st.read_new(&p, 0);
            }
            st.cur_date = None;
            st.last_secs = None;
            st.loaded = true;
        }
        st.sig = Some(sig);
        let path = st.path.clone();
        let pos = st.pos;
        st.pos = st.read_new(&path, pos);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn left_names() {
        let players = vec!["Skull Taffy".to_string(), "use".to_string()];
        let l = parse_left("762cfec4 Skull Taffy left (0x02) | others stay: ['a']", &players).expect("left");
        assert_eq!((l.name.as_str(), l.why.as_str()), ("Skull Taffy", "left (0x02)"));
        let l = parse_left("762cfec4 Bob timed out", &[]).expect("left");
        assert_eq!((l.name.as_str(), l.why.as_str()), ("Bob", "timed out"));
        let l = parse_left("762cfec4 Ann Lee session lost | others stay: none (match removed)", &[]).expect("left");
        assert_eq!((l.name.as_str(), l.why.as_str()), ("Ann", "Lee session lost"));
    }

    #[test]
    fn brackets() {
        assert_eq!(bracket("abc players ['a', 'b'] host a", "players"), vec!["a", "b"]);
        assert_eq!(bracket("abc host a", "players"), Vec::<String>::new());
    }
}
