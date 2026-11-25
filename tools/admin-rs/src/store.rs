use std::collections::{BTreeSet, HashMap, HashSet};
use std::net::{TcpStream, ToSocketAddrs};
use std::os::unix::fs::MetadataExt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use rusqlite::{Connection, OpenFlags};
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};

use crate::daemon::{self, CallErr};
use crate::logindex::{self, LogIndex, LogState, Match};
use crate::py;
use crate::timeutil;
use t2r_clan::rules::{faction_name, needed, status_name, token_id, week_day, ALIVE, WEEKS, WIN_DAY};

const SNAPSHOT_FRESH: f64 = 35.0;
const EARLY: [&str; 3] = ["timed out", "admin kick", "jip never loaded"];

type Sig = Option<(i64, i64, u64, u64)>;

fn sig(p: &Path) -> Sig {
    std::fs::metadata(p).ok().map(|m| (m.mtime(), m.mtime_nsec(), m.size(), m.ino()))
}

pub fn pid_of(key: &str) -> String {
    let d = Sha256::digest(key.as_bytes());
    d.iter().take(5).map(|b| format!("{:02x}", b)).collect()
}

pub fn pretty_level(lvl: &str) -> String {
    let s = lvl.strip_prefix("to1-").or_else(|| lvl.strip_prefix("to2-")).unwrap_or(lvl).replace('-', " ");
    let mut out = String::with_capacity(s.len());
    let mut prev = false;
    for c in s.chars() {
        let cased = c.is_lowercase() || c.is_uppercase();
        if cased {
            if prev {
                out.extend(c.to_lowercase());
            } else {
                out.extend(c.to_uppercase());
            }
        } else {
            out.push(c);
        }
        prev = cased;
    }
    out
}

fn token_embedded_name(token: &str) -> String {
    let b = token.as_bytes();
    if b.len() % 2 != 0 {
        return String::new();
    }
    let mut bytes = Vec::with_capacity(b.len() / 2);
    for ch in b.chunks(2) {
        let Ok(s) = std::str::from_utf8(ch) else {
            return String::new();
        };
        let Ok(v) = u8::from_str_radix(s, 16) else {
            return String::new();
        };
        bytes.push(v);
    }
    if !bytes.starts_with(b"T2N:") {
        return String::new();
    }
    let rest = &bytes[4..];
    let end = rest.iter().position(|&c| c == 0).unwrap_or(rest.len());
    String::from_utf8_lossy(&rest[..end]).into_owned()
}

fn norm(n: &str) -> String {
    n.to_lowercase().split_whitespace().collect::<Vec<_>>().join(" ")
}

fn redact(line: &str) -> String {
    use std::sync::OnceLock;
    static R: OnceLock<(regex::Regex, regex::Regex)> = OnceLock::new();
    let (run, dump) = R.get_or_init(|| {
        (
            regex::Regex::new(r"\b[0-9a-fA-F]{24,}\b").expect("hex run"),
            regex::Regex::new(r"(?:\b[0-9a-f]{2}\b ){6,}[0-9a-f]{2}\b(?: \.\.\.\(\+\d+B\))?").expect("hex dump"),
        )
    });
    let a = run.replace_all(line, "<id>");
    dump.replace_all(&a, "<bytes>").into_owned()
}

fn clean_kick_reason(s: &str) -> String {
    let kept: String = s.chars().filter(|&c| c >= ' ' && c != '\x7f').collect();
    let c = py::collapse_ws(&kept);
    py::head(c.trim(), 120).trim().to_string()
}

fn clean_ban(s: &str, n: usize) -> String {
    let kept: String = s.chars().filter(|&c| c >= ' ' && c != '\x7f' && c != '#').collect();
    py::head(py::collapse_ws(&kept).trim(), n).to_string()
}

fn valid_pid(p: &str) -> bool {
    (4..=64).contains(&p.len()) && p.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

fn copy2(src: &Path, dst: &Path) -> std::io::Result<()> {
    std::fs::copy(src, dst)?;
    if let Ok(md) = std::fs::metadata(src) {
        let times = [
            libc::timespec { tv_sec: md.atime() as _, tv_nsec: md.atime_nsec() as _ },
            libc::timespec { tv_sec: md.mtime() as _, tv_nsec: md.mtime_nsec() as _ },
        ];
        if let Ok(c) = std::ffi::CString::new(dst.as_os_str().as_encoded_bytes()) {
            unsafe {
                libc::utimensat(libc::AT_FDCWD, c.as_ptr(), times.as_ptr(), 0);
            }
        }
    }
    Ok(())
}

pub fn write_atomic(p: &Path, data: &[u8]) -> std::io::Result<()> {
    use std::io::Write;
    let mut tmp = p.as_os_str().to_owned();
    tmp.push(".tmp");
    let tmp = PathBuf::from(tmp);
    {
        let mut f = std::fs::File::create(&tmp)?;
        f.write_all(data)?;
        f.flush()?;
        f.sync_all()?;
    }
    std::fs::rename(&tmp, p)
}

pub struct Group {
    pub id: String,
    pub steamid: i64,
    tokens: Vec<String>,
    pub names: BTreeSet<String>,
    pub kills: i64,
    pub deaths: i64,
    pub wins: i64,
    pub matches: i64,
    pub draws: i64,
    pub parts_total: i64,
    pub parts_n: i64,
    pub last_seen: f64,
    pub version: Value,
    pub platform: &'static str,
    ver_t: f64,
    name: String,
    name_t: (i32, f64),
    pub tag: String,
    pub display: String,
    pub clan_key: Option<String>,
    pub clan: Option<Map<String, Value>>,
    pub losses: i64,
    pub token_ids: Vec<String>,
    pub ntokens: usize,
    pub alias: HashSet<String>,
}

pub struct Groups {
    pub list: Vec<Group>,
    by_id: HashMap<String, usize>,
}

impl Groups {
    pub fn get(&self, id: &str) -> Option<&Group> {
        self.by_id.get(id).map(|&i| &self.list[i])
    }
}

#[derive(Clone)]
struct Live {
    src: &'static str,
    ver: String,
    snap: Option<Map<String, Value>>,
    age: Option<f64>,
}

struct Cached<T> {
    at: Instant,
    key: Vec<Sig>,
    val: T,
}

struct State {
    probe: Option<(Instant, Value)>,
    live: Option<(Instant, Live)>,
    players: Option<Cached<Arc<Groups>>>,
    clans: Option<Cached<Arc<Value>>>,
    seen_online: HashMap<String, f64>,
    seen_state: HashMap<String, (String, f64)>,
    alife_sum: HashMap<String, (f64, Value)>,
    acct_index: Option<(Instant, Arc<HashMap<String, (String, String)>>)>,
    metas: HashMap<PathBuf, (Sig, Option<Arc<Map<String, Value>>>)>,
}

pub struct Admin {
    pub db: PathBuf,
    pub dbdir: PathBuf,
    reports: String,
    pub log: Option<LogIndex>,
    daemon_log: String,
    daemon: String,
    status_file: PathBuf,
    admin_url: String,
    secret_file: PathBuf,
    update_data: String,
    st: Mutex<State>,
    file_lock: Mutex<()>,
}

pub struct LiveView {
    pub source: &'static str,
    pub age: Value,
    pub server_version: String,
    pub snap: Map<String, Value>,
    pub started: Value,
}

fn arr<'a>(m: &'a Map<String, Value>, k: &str) -> &'a [Value] {
    m.get(k).and_then(Value::as_array).map(Vec::as_slice).unwrap_or(&[])
}

fn s_of<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(Value::as_str).unwrap_or("")
}

fn call_fail(e: CallErr, tail: &str) -> Value {
    match e {
        CallErr::NoSecret => json!({"ok": false, "error": daemon::NO_SECRET}),
        CallErr::Net(r) => json!({"ok": false, "error": format!("daemon admin listener not reachable ({}){}", r, tail)}),
    }
}

impl Admin {
    #[allow(clippy::too_many_arguments)]
    pub fn new(db: &str, daemon_log: &str, reports: &str, daemon: &str, status_file: &str, admin_url: &str, update_data: &str) -> Self {
        let db = std::path::absolute(db).unwrap_or_else(|_| PathBuf::from(db));
        let dbdir = db.parent().map(Path::to_path_buf).unwrap_or_default();
        Admin {
            status_file: if status_file.is_empty() { dbdir.join("t2r-status.json") } else { PathBuf::from(status_file) },
            secret_file: dbdir.join("admin-secret.txt"),
            admin_url: if admin_url.is_empty() { "http://127.0.0.1:7011".into() } else { admin_url.into() },
            db,
            dbdir,
            reports: reports.into(),
            log: if daemon_log.is_empty() { None } else { Some(LogIndex::new(daemon_log)) },
            daemon_log: daemon_log.into(),
            daemon: daemon.into(),
            update_data: update_data.into(),
            st: Mutex::new(State {
                probe: None,
                live: None,
                players: None,
                clans: None,
                seen_online: HashMap::new(),
                seen_state: HashMap::new(),
                alife_sum: HashMap::new(),
                acct_index: None,
                metas: HashMap::new(),
            }),
            file_lock: Mutex::new(()),
        }
    }

    fn state(&self) -> MutexGuard<'_, State> {
        self.st.lock().unwrap_or_else(|e| e.into_inner())
    }

    pub fn connect(&self) -> rusqlite::Result<Connection> {
        let c = Connection::open_with_flags(&self.db, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX)?;
        c.busy_timeout(Duration::from_millis(1500))?;
        c.execute_batch("PRAGMA query_only=ON; PRAGMA cache_size=-512;")?;
        Ok(c)
    }

    fn read_json(&self, name: &str) -> Option<Value> {
        let b = std::fs::read(self.dbdir.join(name)).ok()?;
        serde_json::from_slice(&b).ok()
    }

    fn secret(&self) -> String {
        std::fs::read(&self.secret_file).map(|b| String::from_utf8_lossy(&b).trim().to_string()).unwrap_or_default()
    }

    fn call(&self, path: &str, data: Option<&Value>, timeout: f64) -> Result<Value, CallErr> {
        daemon::call(&self.admin_url, &self.secret(), path, data, timeout)
    }

    pub fn cur_log(&self) -> String {
        match &self.log {
            Some(l) if l.follow.is_some() => l.lock().path.to_string_lossy().into_owned(),
            _ => self.daemon_log.clone(),
        }
    }

    pub fn refresh_log(&self) -> bool {
        match &self.log {
            Some(l) => {
                l.refresh();
                true
            }
            None => false,
        }
    }

    fn daemon_addr(&self) -> (String, u16) {
        let mut a = self.daemon.clone();
        if a.is_empty() {
            if let Some(l) = &self.log {
                a = l.lock().listen.clone();
            }
        }
        if a.is_empty() {
            a = "127.0.0.1:7001".into();
        }
        let (host, port) = a.rsplit_once(':').unwrap_or(("", a.as_str()));
        match port.trim().parse::<u16>() {
            Ok(p) => (if host.is_empty() { "127.0.0.1".into() } else { host.to_string() }, p),
            Err(_) => ("127.0.0.1".into(), 7001),
        }
    }

    pub fn probe(&self) -> Value {
        if let Some((t, v)) = &self.state().probe {
            if t.elapsed().as_secs_f64() < 4.0 {
                return v.clone();
            }
        }
        let (host, port) = self.daemon_addr();
        let addr = format!("{}:{}", host, port);
        let t0 = timeutil::now();
        let target = format!("{}:{}", host.trim_start_matches('[').trim_end_matches(']'), port);
        let target = if host.contains(':') && !host.starts_with('[') { format!("[{}]:{}", host, port) } else { target };
        let ok = target
            .to_socket_addrs()
            .map(|it| it.into_iter().any(|sa| TcpStream::connect_timeout(&sa, Duration::from_millis(1500)).is_ok()))
            .unwrap_or(false);
        let v = if ok {
            json!({"open": true, "ms": py::round((timeutil::now() - t0) * 1000.0), "addr": addr})
        } else {
            json!({"open": false, "ms": null, "addr": addr})
        };
        self.state().probe = Some((Instant::now(), v.clone()));
        v
    }

    fn live(&self) -> Live {
        if let Some((t, v)) = &self.state().live {
            if t.elapsed().as_secs_f64() < 2.0 {
                return v.clone();
            }
        }
        let v = self.live_fetch();
        self.state().live = Some((Instant::now(), v.clone()));
        v
    }

    fn live_fetch(&self) -> Live {
        let sec = self.secret();
        if !sec.is_empty() {
            if let Ok(Value::Object(j)) = daemon::call(&self.admin_url, &sec, "/live", None, 1.5) {
                if j.get("ok").map(py::truthy).unwrap_or(false) {
                    if let Some(Value::Object(l)) = j.get("live") {
                        if !l.is_empty() {
                            let ver = j.get("server_version").map(py::to_str).unwrap_or_default();
                            return Live { src: "live", ver, snap: Some(l.clone()), age: Some(0.0) };
                        }
                    }
                }
            }
        }
        if let Ok(b) = std::fs::read(&self.status_file) {
            if let Ok(Value::Object(d)) = serde_json::from_slice::<Value>(&b) {
                let ts = d.get("ts").and_then(py::to_float).unwrap_or(0.0);
                let age = timeutil::now() - ts;
                if age < SNAPSHOT_FRESH {
                    if let Some(Value::Object(l)) = d.get("live") {
                        if !l.is_empty() {
                            let ver = d.get("server_version").map(py::to_str).unwrap_or_default();
                            return Live { src: "snapshot", ver, snap: Some(l.clone()), age: Some(age) };
                        }
                    }
                }
            }
        }
        Live { src: "log", ver: String::new(), snap: None, age: None }
    }

    fn eff<'a>(m: &'a Match, now: f64, st: Option<logindex::Status>) -> (Option<f64>, &'a str, &'static str) {
        let idle = st.map(|s| now - s.ts < 45.0 && s.matches == 0).unwrap_or(false);
        if m.closed.is_none() && ((m.state == "loading" && now - m.formed > 300.0) || idle) {
            let closed = if m.state == "loading" { m.formed } else { st.map(|s| s.ts).unwrap_or(now) };
            let why = if m.started.is_none() { "never started" } else { "ended without a result" };
            let state = if m.state != "ended" { "closed" } else { m.state };
            return (Some(closed), why, state);
        }
        (m.closed, m.closed_why.as_str(), m.state)
    }

    fn derived_live(&self, ls: &mut LogState) -> Map<String, Value> {
        let now = timeutil::now();
        let ms = ls.matches();
        let st = ls.status;
        let fresh = st.map(|s| now - s.ts < 45.0).unwrap_or(false);
        let mut names_in: BTreeSet<String> = BTreeSet::new();
        let mut out_m = Vec::new();
        for m in ms.iter() {
            let (closed, _, state) = Self::eff(m, now, st);
            if closed.is_some() || !(state == "loading" || state == "running") {
                continue;
            }
            let gone: HashSet<&str> = m.left.iter().filter(|l| l.why != "back").map(|l| l.name.as_str()).collect();
            let mut pl = Vec::new();
            for (i, n) in m.players.iter().enumerate() {
                let left = gone.contains(n.as_str());
                if !left {
                    names_in.insert(n.clone());
                }
                pl.push(json!({"name": n, "pid": null, "team": i % 2, "left": left, "host": m.host.as_deref() == Some(n.as_str()), "ping_ms": null}));
            }
            let map = match &m.map {
                Some(l) if !l.is_empty() => json!({"level": l, "name": pretty_level(l)}),
                _ => Value::Null,
            };
            out_m.push(json!({"id": m.id, "state": state, "age_s": py::round(now - m.formed), "map": map, "mode": "", "net": m.net,
                              "players": pl, "clock": null, "lives": null}));
        }
        let recent: BTreeSet<String> = ls.announces.iter().filter(|(ts, n)| now - ts < 60.0 && !names_in.contains(n)).map(|(_, n)| n.clone()).collect();
        let row = |n: &String, state: &str| {
            json!({"name": n, "pid": null, "steamid": 0, "state": state, "match": null, "idle_s": null, "queued_s": null, "version": "", "platform": "",
                   "steam_helper": null})
        };
        let mut players: Vec<Value> = names_in.iter().map(|n| row(n, "match")).collect();
        players.extend(recent.iter().map(|n| row(n, "menu")));
        if fresh && st.map(|s| s.online == 0).unwrap_or(false) {
            players.clear();
        }
        let counts = match st {
            Some(s) if fresh => json!({"online": s.online, "queue": s.queue, "matches": s.matches}),
            _ => Value::Null,
        };
        let mut out = Map::new();
        out.insert("players".into(), Value::Array(players));
        out.insert("matches".into(), Value::Array(out_m));
        out.insert("queue".into(), json!([]));
        out.insert("counts".into(), counts);
        out.insert("started".into(), py::opt_float(ls.started));
        out.insert("approx".into(), Value::Bool(true));
        out
    }

    pub fn live_view(&self) -> LiveView {
        let lv = self.live();
        let has_log = self.refresh_log();
        let mut server_version = lv.ver.clone();
        let mut snap;
        let started;
        if let (Some(s), true) = (&lv.snap, lv.src == "live" || lv.src == "snapshot") {
            snap = s.clone();
            let ended = |m: &Value| s_of(m, "state") == "ended";
            let counts = json!({"online": arr(&snap, "players").len(), "queue": arr(&snap, "queue").len(),
                                "matches": arr(&snap, "matches").iter().filter(|m| !ended(m)).count()});
            snap.insert("counts".into(), counts);
            started = snap.get("started").cloned().unwrap_or(Value::Null);
        } else {
            snap = match &self.log {
                Some(l) if has_log => {
                    let mut g = l.lock();
                    self.derived_live(&mut g)
                }
                _ => {
                    let mut m = Map::new();
                    m.insert("players".into(), json!([]));
                    m.insert("matches".into(), json!([]));
                    m.insert("queue".into(), json!([]));
                    m.insert("counts".into(), Value::Null);
                    m
                }
            };
            started = snap.get("started").cloned().unwrap_or(Value::Null);
        }
        if server_version.is_empty() {
            if let Some(l) = &self.log {
                server_version = l.lock().version.clone();
            }
        }
        let now = timeutil::now();
        {
            let mut st = self.state();
            let cur: HashSet<String> = arr(&snap, "players").iter().filter_map(|p| p.get("pid").filter(|v| py::truthy(v)).map(py::to_str)).collect();
            st.seen_online.retain(|k, _| cur.contains(k));
            st.seen_state.retain(|k, _| cur.contains(k));
            if let Some(Value::Array(ps)) = snap.get_mut("players") {
                for p in ps.iter_mut() {
                    let Some(po) = p.as_object_mut() else { continue };
                    if !po.get("pid").map(py::truthy).unwrap_or(false) {
                        continue;
                    }
                    let k = po.get("pid").map(py::to_str).unwrap_or_default();
                    let state = po.get("state").map(py::to_str).unwrap_or_default();
                    let so = *st.seen_online.entry(k.clone()).or_insert(now);
                    if st.seen_state.get(&k).map(|(s, _)| s != &state).unwrap_or(true) {
                        st.seen_state.insert(k.clone(), (state, now));
                    }
                    let ss = st.seen_state.get(&k).map(|(_, t)| *t).unwrap_or(now);
                    po.insert("online_s".into(), Value::from(py::round(now - so)));
                    po.insert("state_s".into(), Value::from(py::round(now - ss)));
                }
            }
        }
        LiveView { source: lv.src, age: py::opt_float(lv.age), server_version, snap, started }
    }

    fn db_sigs(&self) -> Vec<Sig> {
        let mut wal = self.db.as_os_str().to_owned();
        wal.push("-wal");
        vec![sig(&self.db), sig(Path::new(&wal)), sig(&self.dbdir.join("client_versions.json")), sig(&self.dbdir.join("linux_clients.json"))]
    }

    pub fn player_groups(&self) -> Result<Arc<Groups>, String> {
        if let Some(c) = &self.state().players {
            if c.at.elapsed().as_secs_f64() < 4.0 {
                return Ok(c.val.clone());
            }
        }
        let key = self.db_sigs();
        {
            let mut st = self.state();
            if let Some(c) = &mut st.players {
                if c.key == key {
                    c.at = Instant::now();
                    return Ok(c.val.clone());
                }
            }
        }
        match self.build_groups() {
            Ok(g) => {
                let g = Arc::new(g);
                self.state().players = Some(Cached { at: Instant::now(), key, val: g.clone() });
                Ok(g)
            }
            Err(e) => match &self.state().players {
                Some(c) => Ok(c.val.clone()),
                None => Err(e),
            },
        }
    }

    fn build_groups(&self) -> Result<Groups, String> {
        use rusqlite::types::Value as S;
        let err = |e: rusqlite::Error| format!("OperationalError: {}", e);
        let c = self.connect().map_err(err)?;
        let mut rows: Vec<(String, String, [i64; 7], i64, f64)> = Vec::new();
        {
            let mut q = c
                .prepare("SELECT token,name,kills,deaths,wins,matches,steamid,last_seen,parts_total,parts_n,draws FROM players WHERE length(token)=64")
                .map_err(err)?;
            let mut it = q.query([]).map_err(err)?;
            while let Some(r) = it.next().map_err(err)? {
                let g = |i: usize| r.get::<_, S>(i).unwrap_or(S::Null);
                let tok = py::sql_text(&g(0)).unwrap_or_default();
                let name = py::sql_text(&g(1)).unwrap_or_default();
                let nums = [py::sql_int(&g(2)), py::sql_int(&g(3)), py::sql_int(&g(4)), py::sql_int(&g(5)), py::sql_int(&g(8)), py::sql_int(&g(9)), py::sql_int(&g(10))];
                rows.push((tok, name, nums, py::sql_int(&g(6)), py::sql_f64(&g(7))));
            }
        }
        let mut names: HashMap<String, (String, String)> = HashMap::new();
        if let Ok(mut q) = c.prepare("SELECT token_id,name,tag FROM names") {
            if let Ok(mut it) = q.query([]) {
                while let Ok(Some(r)) = it.next() {
                    let g = |i: usize| r.get::<_, S>(i).ok().and_then(|v| py::sql_text(&v)).unwrap_or_default();
                    names.insert(g(0), (g(1), g(2)));
                }
            }
        }
        let mut clans: HashMap<String, Vec<S>> = HashMap::new();
        if let Ok(mut q) = c.prepare("SELECT token_id,faction,status,day,healthy,hungry,sick,weeks_total,runs_won,runs_lost,last_result,meta,updated FROM clans") {
            if let Ok(mut it) = q.query([]) {
                while let Ok(Some(r)) = it.next() {
                    let v: Vec<S> = (0..13).map(|i| r.get::<_, S>(i).unwrap_or(S::Null)).collect();
                    let k = py::sql_text(&v[0]).unwrap_or_default();
                    clans.insert(k, v);
                }
            }
        }
        drop(c);
        let versions = match self.read_json("client_versions.json") {
            Some(Value::Object(m)) => m,
            _ => Map::new(),
        };
        let linux: HashSet<String> = match self.read_json("linux_clients.json") {
            Some(Value::Array(a)) => a.iter().filter_map(|v| v.as_str().map(str::to_string)).collect(),
            _ => HashSet::new(),
        };
        let mut steam_of_name: HashMap<String, BTreeSet<i64>> = HashMap::new();
        let embedded: Vec<String> = rows.iter().map(|r| token_embedded_name(&r.0)).collect();
        for (i, (_, name, _, sid, _)) in rows.iter().enumerate() {
            if *sid > 0 {
                for n in [norm(name), norm(&embedded[i])] {
                    if !n.is_empty() {
                        steam_of_name.entry(n).or_default().insert(*sid);
                    }
                }
            }
        }
        let mut groups: Vec<Group> = Vec::new();
        let mut gidx: HashMap<String, usize> = HashMap::new();
        for (i, (tok, name, nums, sid0, ls)) in rows.iter().enumerate() {
            let key = if *sid0 > 0 {
                format!("s{}", sid0)
            } else {
                let mut nm = norm(name);
                if nm.is_empty() {
                    nm = norm(&embedded[i]);
                }
                if nm.is_empty() {
                    format!("t{}", tok.get(40..64).unwrap_or(""))
                } else {
                    match steam_of_name.get(&nm) {
                        Some(o) if o.len() == 1 => format!("s{}", o.iter().next().copied().unwrap_or(0)),
                        _ => format!("n:{}:{}", nm, if linux.contains(tok) { "linux" } else { "windows" }),
                    }
                }
            };
            let sid_orig = *sid0 > 0;
            let mut sid = *sid0;
            if key.starts_with('s') && !sid_orig {
                sid = key[1..].parse().unwrap_or(0);
            }
            let gi = *gidx.entry(key.clone()).or_insert_with(|| {
                groups.push(Group {
                    id: pid_of(&key),
                    steamid: sid,
                    tokens: Vec::new(),
                    names: BTreeSet::new(),
                    kills: 0,
                    deaths: 0,
                    wins: 0,
                    matches: 0,
                    draws: 0,
                    parts_total: 0,
                    parts_n: 0,
                    last_seen: 0.0,
                    version: Value::String(String::new()),
                    platform: "windows",
                    ver_t: 0.0,
                    name: String::new(),
                    name_t: (-1, -1.0),
                    tag: String::new(),
                    display: String::new(),
                    clan_key: None,
                    clan: None,
                    losses: 0,
                    token_ids: Vec::new(),
                    ntokens: 0,
                    alias: HashSet::new(),
                });
                groups.len() - 1
            });
            let g = &mut groups[gi];
            g.tokens.push(tok.clone());
            if g.steamid == 0 {
                g.steamid = sid;
            }
            if !name.is_empty() {
                g.names.insert(name.clone());
            }
            if !embedded[i].is_empty() {
                g.names.insert(embedded[i].clone());
            }
            g.kills += nums[0];
            g.deaths += nums[1];
            g.wins += nums[2];
            g.matches += nums[3];
            g.draws += nums[6];
            g.parts_total += nums[4];
            g.parts_n += nums[5];
            let nt = (if sid_orig { 1 } else { 0 }, *ls);
            if !name.trim().is_empty() && (nt.0 > g.name_t.0 || (nt.0 == g.name_t.0 && nt.1 > g.name_t.1)) {
                g.name = name.trim().to_string();
                g.name_t = nt;
            }
            g.last_seen = g.last_seen.max(*ls);
            if let Some(v) = versions.get(tok) {
                if *ls >= g.ver_t {
                    g.version = v.clone();
                    g.ver_t = *ls;
                }
            }
            if linux.contains(tok) {
                g.platform = "linux";
            }
        }
        let mut list = Vec::with_capacity(groups.len());
        let mut by_id = HashMap::new();
        for mut g in groups {
            let mut tids: Vec<String> = Vec::new();
            if g.steamid != 0 {
                tids.push(format!("s:{}", g.steamid));
            }
            g.token_ids = g.tokens.iter().map(|t| token_id(t)).collect();
            tids.extend(g.token_ids.iter().cloned());
            let mut best: Option<(&Vec<S>, (bool, f64, f64, f64))> = None;
            for t in &tids {
                if let Some(r) = clans.get(t) {
                    let k = (py::sql_int(&r[2]) == ALIVE, py::sql_f64(&r[7]), py::sql_f64(&r[3]), py::sql_f64(&r[12]));
                    let better = match &best {
                        None => true,
                        Some((_, b)) => k.0 > b.0 || (k.0 == b.0 && (k.1 > b.1 || (k.1 == b.1 && (k.2 > b.2 || (k.2 == b.2 && k.3 > b.3))))),
                    };
                    if better {
                        best = Some((r, k));
                    }
                }
            }
            let custom = tids.iter().find_map(|t| names.get(t).filter(|(n, _)| !n.is_empty()));
            if let Some((n, tg)) = custom {
                g.names.insert(n.clone());
                g.tag = tg.clone();
                g.display = if tg.is_empty() { n.clone() } else { format!("[{}] {}", tg, n) };
            } else {
                g.tag = String::new();
                g.display = if !g.name.is_empty() { g.name.clone() } else { g.names.iter().next().cloned().unwrap_or_else(|| "PLAYER".into()) };
            }
            if let Some((r, _)) = best {
                g.clan_key = py::sql_text(&r[0]);
                g.clan = Some(clan_row(r));
            }
            g.losses = (g.matches - g.wins - g.draws).max(0);
            g.ntokens = g.tokens.len();
            g.alias = g.names.iter().map(|n| n.to_lowercase()).collect();
            g.alias.insert(g.display.to_lowercase());
            if g.display == "PLAYER" && g.matches == 0 && g.clan.is_none() {
                continue;
            }
            by_id.entry(g.id.clone()).or_insert(list.len());
            list.push(g);
        }
        Ok(Groups { list, by_id })
    }

    pub fn name_for_steam(&self, sid: i64) -> Option<String> {
        let pg = self.player_groups().ok()?;
        pg.list.iter().find(|g| g.steamid == sid && !g.display.is_empty()).map(|g| g.display.clone())
    }

    fn latest_version(&self) -> String {
        if self.update_data.is_empty() {
            return String::new();
        }
        let Ok(b) = std::fs::read(Path::new(&self.update_data).join("channels.json")) else {
            return String::new();
        };
        match serde_json::from_slice::<Value>(&b) {
            Ok(Value::Object(ch)) => match ch.get("stable").filter(|v| py::truthy(v)) {
                Some(v) => py::to_str(v),
                None => ch.values().next().map(py::to_str).unwrap_or_default(),
            },
            _ => String::new(),
        }
    }

    fn matches_now(&self) -> (Vec<Match>, Option<logindex::Status>, f64) {
        let now = timeutil::now();
        match &self.log {
            Some(l) => {
                l.refresh();
                let mut g = l.lock();
                let st = g.status;
                let ms = g.matches();
                let out = ms
                    .iter()
                    .map(|m| {
                        let (closed, why, state) = Self::eff(m, now, st);
                        if closed == m.closed && state == m.state {
                            m.clone()
                        } else {
                            let mut c = m.clone();
                            c.closed = closed;
                            c.closed_why = why.to_string();
                            c.state = state;
                            c
                        }
                    })
                    .collect();
                (out, st, now)
            }
            None => (Vec::new(), None, now),
        }
    }

    pub fn overview(&self) -> Result<Value, String> {
        let lv = self.live_view();
        let snap = &lv.snap;
        let probe = self.probe();
        let now = timeutil::now();
        let counts = snap.get("counts").and_then(Value::as_object).cloned().unwrap_or_default();
        let players = arr(snap, "players");
        let mut by_state: Vec<(String, i64)> = vec![("menu".into(), 0), ("queue".into(), 0), ("match".into(), 0)];
        for p in players {
            let s = p.get("state").map(py::to_str).unwrap_or_default();
            match by_state.iter_mut().find(|(k, _)| *k == s) {
                Some(e) => e.1 += 1,
                None => by_state.push((s, 1)),
            }
        }
        let bs = |k: &str| by_state.iter().find(|(s, _)| s == k).map(|e| e.1).unwrap_or(0);
        let mut online = counts.get("online").cloned().unwrap_or(Value::Null);
        if online.is_null() && (lv.source != "log" || !players.is_empty()) {
            online = Value::from(players.len());
        }
        let server_up = probe.get("open").and_then(Value::as_bool).unwrap_or(false) || matches!(lv.source, "live" | "snapshot");
        let (ms, status, _) = self.matches_now();
        let (log_started, errs) = match &self.log {
            Some(l) => {
                let g = l.lock();
                let errs: Vec<Value> = g.errors.iter().rev().take(8).map(|e| json!({"ts": e.ts, "line": redact(&e.line)})).collect();
                (g.started, errs)
            }
            None => (None, Vec::new()),
        };
        let fresh_log = status.map(|s| now - s.ts < 45.0).unwrap_or(false);
        let started = if py::truthy(&lv.started) { lv.started.clone() } else { py::opt_float(if self.log.is_some() { log_started } else { None }) };
        let uptime = match py::num_f64(&started) {
            Some(s) if s != 0.0 && server_up => Value::from(py::round(now - s)),
            _ => Value::Null,
        };
        let latest = self.latest_version();
        let sv = lv.server_version.clone();
        let pg = if self.db.exists() { Some(self.player_groups()?) } else { None };
        let mut clients: Map<String, Value> = Map::new();
        for p in players {
            let v = p.get("version").filter(|v| py::truthy(v)).map(py::to_str).unwrap_or_else(|| "?".into());
            let n = clients.get(&v).and_then(Value::as_i64).unwrap_or(0) + 1;
            clients.insert(v, Value::from(n));
        }
        let outdated: i64 = clients.iter().filter(|(v, _)| !sv.is_empty() && *v != &sv && v.as_str() != "?").map(|(_, n)| n.as_i64().unwrap_or(0)).sum();
        let matches_running = match counts.get("matches") {
            Some(v) if !v.is_null() => v.clone(),
            _ => Value::from(arr(snap, "matches").iter().filter(|m| s_of(m, "state") != "ended").count()),
        };
        let mut notes = Vec::new();
        if lv.source == "log" {
            notes.push("No live feed from the daemon (it needs a restart on the build with the admin listener). Online / match data is reconstructed from the log and is approximate.");
        } else if lv.source == "snapshot" {
            notes.push("Data comes from the daemon's 10 s snapshot (the admin listener is not reachable or no admin-secret.txt: kick is unavailable).");
        }
        let mut seen: Vec<(String, bool)> = Vec::new();
        let mut seen_idx: HashMap<String, usize> = HashMap::new();
        if let Some(pg) = &pg {
            for g in &pg.list {
                if let Some(c) = &g.clan {
                    let k = c.get("key").map(py::to_str).unwrap_or_default();
                    let alive = c.get("status").and_then(Value::as_str) == Some("alive");
                    match seen_idx.get(&k) {
                        Some(&i) => seen[i].1 = alive,
                        None => {
                            seen_idx.insert(k.clone(), seen.len());
                            seen.push((k, alive));
                        }
                    }
                }
            }
        }
        let reports_new = if self.reports.is_empty() { 0 } else { self.report_metas().iter().filter(|(_, m)| !m.get("handled").map(py::truthy).unwrap_or(false)).count() };
        let ended: Vec<&Match> = ms.iter().filter(|m| m.state == "ended").collect();
        Ok(json!({
            "now": now, "source": lv.source, "snapshotAge": lv.age, "notes": notes,
            "server": {"up": server_up, "probe": probe, "version": sv, "latest": latest, "uptime_s": uptime, "started": started,
                       "logFresh": fresh_log, "logPath": self.cur_log()},
            "online": {"total": online, "menu": bs("menu"),
                       "queue": match counts.get("queue") { Some(v) if !v.is_null() => v.clone(), _ => Value::from(bs("queue")) },
                       "match": bs("match")},
            "matchesRunning": matches_running,
            "clients": {"versions": clients, "outdated": outdated},
            "totals": {"players": pg.as_ref().map(|p| p.list.len()).unwrap_or(0), "clans": seen.len(), "clansAlive": seen.iter().filter(|s| s.1).count(),
                       "matchesLogged": ended.len(), "matches24h": ended.iter().filter(|m| m.ended.map(|e| now - e < 86400.0).unwrap_or(false)).count(),
                       "reportsNew": reports_new},
            "errors": errs, "canKick": lv.source == "live"
        }))
    }

    fn online_rows(&self, lv: &LiveView, pg: Option<&Groups>) -> Vec<Value> {
        let sv = &lv.server_version;
        let mut pings: HashMap<String, Value> = HashMap::new();
        for m in arr(&lv.snap, "matches") {
            for mp in m.get("players").and_then(Value::as_array).map(Vec::as_slice).unwrap_or(&[]) {
                let ping = mp.get("ping_ms").cloned().unwrap_or(Value::Null);
                if !ping.is_null() && mp.get("pid").map(py::truthy).unwrap_or(false) {
                    pings.insert(py::to_str(&mp["pid"]), ping);
                }
            }
        }
        let by_steam: HashMap<i64, &Group> = pg.map(|pg| pg.list.iter().filter(|g| g.steamid != 0).map(|g| (g.steamid, g)).collect()).unwrap_or_default();
        let mut out: Vec<Map<String, Value>> = Vec::new();
        for p in arr(&lv.snap, "players") {
            let Some(po) = p.as_object() else { continue };
            let name = po.get("name").map(py::to_str).unwrap_or_default();
            let lname = name.to_lowercase();
            let g = po
                .get("steamid")
                .and_then(Value::as_i64)
                .and_then(|s| by_steam.get(&s).copied())
                .or_else(|| pg.and_then(|pg| pg.list.iter().find(|x| x.alias.contains(&lname))));
            let mut q = po.clone();
            let pid = po.get("pid").map(py::to_str);
            q.insert("ping".into(), pid.as_ref().and_then(|k| pings.get(k)).cloned().unwrap_or(Value::Null));
            if let Some(g) = g {
                q.insert("playerId".into(), Value::String(g.id.clone()));
                let clan = match &g.clan {
                    Some(c) if c.get("status").and_then(Value::as_str) == Some("alive") => {
                        json!({"factionName": c["factionName"], "week": c["week"], "day": c["dayOfWeek"], "size": c["size"]})
                    }
                    _ => Value::Null,
                };
                q.insert("clan".into(), clan);
                q.insert("tag".into(), Value::String(g.tag.clone()));
                if !q.get("version").map(py::truthy).unwrap_or(false) {
                    q.insert("version".into(), g.version.clone());
                }
                if !q.get("platform").map(py::truthy).unwrap_or(false) {
                    q.insert("platform".into(), Value::String(g.platform.into()));
                }
            }
            let ver = q.get("version").cloned().unwrap_or(Value::Null);
            let outdated = !sv.is_empty() && py::truthy(&ver) && py::to_str(&ver) != *sv;
            q.insert("outdated".into(), Value::Bool(outdated));
            out.push(q);
        }
        let order = |s: &str| match s {
            "match" => 0,
            "queue" => 1,
            "menu" => 2,
            _ => 3,
        };
        let rank = |q: &Map<String, Value>| (order(q.get("state").and_then(Value::as_str).unwrap_or("")), q.get("idle_s").and_then(py::num_f64).unwrap_or(0.0));
        let mut best: Vec<(String, Map<String, Value>)> = Vec::new();
        for q in out {
            let k = match q.get("playerId") {
                Some(v) if py::truthy(v) => py::to_str(v),
                _ => format!("n:{}", q.get("name").map(py::to_str).unwrap_or_default().to_lowercase()),
            };
            match best.iter_mut().find(|(bk, _)| *bk == k) {
                Some(slot) => {
                    let (a, b) = (rank(&q), rank(&slot.1));
                    if a.0 < b.0 || (a.0 == b.0 && a.1 < b.1) {
                        slot.1 = q;
                    }
                }
                None => best.push((k, q)),
            }
        }
        let mut rows: Vec<Map<String, Value>> = best.into_iter().map(|(_, q)| q).collect();
        rows.sort_by(|a, b| {
            let ka = (order(a.get("state").and_then(Value::as_str).unwrap_or("")), a.get("name").map(py::to_str).unwrap_or_default().to_lowercase());
            let kb = (order(b.get("state").and_then(Value::as_str).unwrap_or("")), b.get("name").map(py::to_str).unwrap_or_default().to_lowercase());
            ka.cmp(&kb)
        });
        rows.into_iter().map(Value::Object).collect()
    }

    pub fn players_online(&self) -> Result<Value, String> {
        let lv = self.live_view();
        let pg = if self.db.exists() { Some(self.player_groups()?) } else { None };
        let rows = self.online_rows(&lv, pg.as_deref());
        Ok(json!({"source": lv.source, "players": rows, "serverVersion": lv.server_version, "canKick": lv.source == "live"}))
    }

    pub fn players_all(&self, q: &str) -> Result<Value, String> {
        let limit = 300;
        let pg = self.player_groups()?;
        let ql = q.to_lowercase().trim().to_string();
        let lv = self.live_view();
        let online_names: HashSet<String> = arr(&lv.snap, "players").iter().map(|p| p.get("name").map(py::to_str).unwrap_or_default().to_lowercase()).collect();
        let online_sid: HashSet<i64> = arr(&lv.snap, "players").iter().filter_map(|p| p.get("steamid").and_then(Value::as_i64)).filter(|s| *s != 0).collect();
        let (ban_ids, ban_names) = self.banned_index();
        let muted = self.muted_keys();
        let mut rows: Vec<(f64, Value)> = Vec::new();
        let mut hidden = 0;
        for g in &pg.list {
            let sids = g.steamid.to_string();
            if !ql.is_empty() && !(g.display.to_lowercase().contains(&ql) || g.alias.iter().any(|a| a.contains(&ql)) || sids.contains(&ql)) {
                continue;
            }
            let online = online_sid.contains(&g.steamid) || g.alias.iter().any(|a| online_names.contains(a));
            if ql.is_empty() && !online && g.matches == 0 {
                hidden += 1;
                continue;
            }
            let c = g.clan.as_ref();
            let cf = |k: &str, d: Value| c.and_then(|c| c.get(k).cloned()).unwrap_or(d);
            let banned = ban_ids.contains(&sids) || g.alias.iter().any(|a| ban_names.contains(a));
            rows.push((
                g.last_seen,
                json!({"id": g.id, "name": g.display, "steamid": g.steamid, "matches": g.matches, "wins": g.wins, "losses": g.losses, "draws": g.draws,
                       "lastSeen": g.last_seen, "version": g.version, "platform": g.platform, "online": online, "banned": banned,
                       "muted": mute_keys(g).iter().any(|k| muted.has(k)),
                       "faction": cf("factionName", json!("")), "clanStatus": cf("status", json!("")), "week": cf("week", json!(0)),
                       "day": cf("dayOfWeek", json!(0)), "size": cf("size", json!(0))}),
            ));
        }
        rows.sort_by(|a, b| b.0.total_cmp(&a.0));
        let shown = rows.len().min(limit);
        let players: Vec<Value> = rows.into_iter().take(limit).map(|r| r.1).collect();
        Ok(json!({"total": pg.list.len(), "shown": shown, "hidden": hidden, "players": players,
                  "hasKd": pg.list.iter().any(|g| g.kills != 0 || g.deaths != 0)}))
    }

    fn read_feed(c: &Connection, key: &str) -> Vec<Value> {
        use rusqlite::types::Value as S;
        let mut out = Vec::new();
        let Ok(mut q) = c.prepare("SELECT id,day,week,kind,mood,faction,text,names,ts,lid,params FROM clan_feed WHERE token_id=? AND id>0 ORDER BY id DESC LIMIT 40") else {
            return out;
        };
        let Ok(mut it) = q.query([key]) else {
            return out;
        };
        let mut rows = Vec::new();
        while let Ok(Some(r)) = it.next() {
            rows.push((0..11).map(|i| r.get::<_, S>(i).unwrap_or(S::Null)).collect::<Vec<S>>());
        }
        for r in rows {
            let names: Value = serde_json::from_str(&py::sql_text(&r[7]).filter(|s| !s.is_empty()).unwrap_or_else(|| "[]".into())).unwrap_or(json!([]));
            let mut m = Map::new();
            for (k, i) in [("id", 0), ("day", 1), ("week", 2), ("kind", 3), ("mood", 4), ("faction", 5), ("text", 6)] {
                m.insert(k.into(), py::sqlv(r[i].clone()));
            }
            m.insert("names".into(), names);
            m.insert("ts".into(), py::sqlv(r[8].clone()));
            m.insert("lang".into(), json!("en"));
            let lid = py::sql_text(&r[9]).unwrap_or_default();
            let params = || -> Map<String, Value> {
                let p = py::sql_text(&r[10]).filter(|s| !s.is_empty()).unwrap_or_else(|| "{}".into());
                match serde_json::from_str::<Value>(&p) {
                    Ok(Value::Object(o)) => o,
                    _ => Map::new(),
                }
            };
            if let Some(rest) = lid.strip_prefix("ch/") {
                let s = rest.split('+').next().unwrap_or("").split('~').next().unwrap_or("");
                m.insert("style".into(), json!(s));
            } else if lid.starts_with("cn/") {
                m.insert("style".into(), json!("saga_recap"));
            } else if lid.starts_with("pk/") {
                m.insert("style".into(), json!("perk"));
            } else if let Some(rest) = lid.strip_prefix("sg/") {
                m.insert("style".into(), json!(format!("saga_{}", rest)));
                let pj = params();
                if let Some(a) = pj.get("a").filter(|v| py::truthy(v)) {
                    m.insert("arc".into(), a.clone());
                }
                if let Some(map) = pj.get("s").and_then(Value::as_object).and_then(|s| s.get("map")).filter(|v| py::truthy(v)) {
                    m.insert("map".into(), map.clone());
                }
            } else if let Some(rest) = lid.strip_prefix("arc/") {
                m.insert("style".into(), json!(format!("arc_{}", rest.split('.').next().unwrap_or(""))));
                let pj = params();
                m.insert("arc".into(), pj.get("a").cloned().unwrap_or(Value::Null));
                m.insert("step".into(), pj.get("n").cloned().unwrap_or(Value::Null));
            }
            out.push(Value::Object(m));
        }
        let done: Option<i64> = c
            .query_row("SELECT day FROM clans WHERE token_id=?", [key], |r| r.get::<_, S>(0))
            .ok()
            .filter(|v| !matches!(v, S::Null))
            .map(|v| py::sql_int(&v));
        if let Some(done) = done {
            out.retain(|m| m.get("day").and_then(py::to_int).unwrap_or(0) <= done);
        }
        out
    }

    pub fn player(&self, pid: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        let (ms, _, _) = self.matches_now();
        let mut hist = Vec::new();
        for m in ms.iter().rev() {
            let Some(nm) = m.players.iter().find(|n| g.alias.contains(&n.to_lowercase())) else { continue };
            if m.started.is_none() {
                continue;
            }
            hist.push(json!({"id": m.id, "when": m.formed, "map": pretty_level(m.map.as_deref().unwrap_or("")), "state": m.state,
                             "result": logindex::result_for(m, nm), "secs": m.secs, "players": m.players.len(),
                             "left": m.left.iter().any(|l| &l.name == nm && EARLY.contains(&l.why.as_str()))}));
            if hist.len() >= 40 {
                break;
            }
        }
        let mut feed = Vec::new();
        let mut people = json!({});
        if let Some(k) = &g.clan_key {
            if self.db.exists() {
                if let Ok(c) = self.connect() {
                    feed = Self::read_feed(&c, k);
                    people = t2r_clan::read_people(&c, k, 40);
                    let pl = t2r_clan::feed_pl_texts(&c, k, 40);
                    for m in feed.iter_mut() {
                        let id = m.get("id").and_then(Value::as_i64).unwrap_or(0);
                        if let Some(o) = m.as_object_mut() {
                            o.insert("textPl".into(), json!(pl.get(&id).cloned().unwrap_or_default()));
                        }
                    }
                }
            }
        }
        let morale = people.get("morale").cloned().unwrap_or(Value::Null);
        let people_rows: Vec<Value> = people.get("people").and_then(Value::as_array).map(|a| a.iter().take(12).cloned().collect()).unwrap_or_default();
        let alive_n = people.get("alive").cloned().unwrap_or(Value::Null);
        let sid = g.steamid.to_string();
        let mut reps = Vec::new();
        for (_, m) in self.report_metas() {
            let name = m.get("name").filter(|v| py::truthy(v)).map(py::to_str).unwrap_or_default().to_lowercase();
            let steam_hit = g.steamid != 0 && m.get("steamId").map(py::to_str).unwrap_or_default() == sid;
            if g.alias.contains(&name) || steam_hit {
                let text = m.get("text").filter(|v| py::truthy(v)).map(py::to_str).unwrap_or_default();
                reps.push(json!({"code": m.get("code"), "received": m.get("received"), "text": py::head(&text, 160),
                                 "handled": m.get("handled").map(py::truthy).unwrap_or(false)}));
            }
        }
        let lv = self.live_view();
        let online = self.online_rows(&lv, Some(&pg)).into_iter().find(|p| p.get("playerId").and_then(Value::as_str) == Some(pid)).unwrap_or(Value::Null);
        let can_kick = online.get("pid").map(py::truthy).unwrap_or(false) && lv.source == "live";
        let avg = if g.parts_n != 0 {
            let pm = g.parts_total as f64 / g.parts_n as f64;
            if pm != 0.0 {
                Value::from(py::round(pm))
            } else {
                Value::Null
            }
        } else {
            Value::Null
        };
        let earliest = self.log.as_ref().and_then(|l| l.lock().earliest());
        Ok(json!({"ok": true, "id": pid, "name": g.display, "tag": g.tag, "aliases": g.names.iter().collect::<Vec<_>>(), "steamid": g.steamid,
                  "installs": g.ntokens, "matches": g.matches, "wins": g.wins, "losses": g.losses, "draws": g.draws, "kills": g.kills, "deaths": g.deaths,
                  "avgParts": avg, "lastSeen": g.last_seen, "version": g.version, "platform": g.platform, "clan": g.clan, "morale": morale,
                  "people": people_rows, "alive": alive_n, "feed": feed, "alife": null, "history": hist, "reports": reps, "online": online, "ban": self.ban_state(g), "mute": self.mute_state(g),
                  "alifeServerOn": self.alife_on(), "matchLogFrom": py::opt_float(earliest), "canKick": can_kick}))
    }

    // clan_alife.enabled(): env T2R_CLAN_ALIFE wins, else the first word of tools/clan_alife.txt next to the data dir (default off)
    fn alife_on(&self) -> bool {
        let yes = |w: &str| matches!(w.trim().to_lowercase().as_str(), "1" | "on" | "true" | "yes");
        if let Ok(e) = std::env::var("T2R_CLAN_ALIFE") {
            if !e.trim().is_empty() {
                return yes(&e);
            }
        }
        let Some(root) = self.db.parent().and_then(Path::parent) else { return false };
        let txt = std::fs::read(root.join("tools").join("clan_alife.txt")).map(|b| String::from_utf8_lossy(&b).into_owned()).unwrap_or_default();
        yes(txt.split_whitespace().next().unwrap_or(""))
    }

    pub fn matches(&self) -> Result<Value, String> {
        let lv = self.live_view();
        let live: Vec<Value> = arr(&lv.snap, "matches").iter().filter(|m| s_of(m, "state") != "ended").cloned().collect();
        let (ms, _, _) = self.matches_now();
        let mut hist = Vec::new();
        for m in ms.iter().rev() {
            if m.closed.is_none() || m.started.is_none() {
                continue;
            }
            let (t0, t1) = logindex::team_names(m);
            let res = if m.state == "ended" {
                match m.winner {
                    Some(0) => "TEAM 1 WON".to_string(),
                    Some(1) => "TEAM 2 WON".to_string(),
                    Some(2) => "DRAW".to_string(),
                    _ => "RESULT UNKNOWN".to_string(),
                }
            } else {
                format!("NOT FINISHED ({})", if m.closed_why.is_empty() { "closed" } else { &m.closed_why })
            };
            let left: Vec<String> = m.left.iter().filter(|l| EARLY.contains(&l.why.as_str())).map(|l| format!("{} ({})", l.name, l.why)).collect();
            hist.push(json!({"id": m.id, "formed": m.formed, "started": m.started, "ended": m.ended, "map": pretty_level(m.map.as_deref().unwrap_or("")),
                             "net": m.net, "ping_ms": m.ping_ms, "team1": t0, "team2": t1, "result": res, "winner": m.winner, "secs": m.secs,
                             "host": m.host, "state": m.state, "left": left, "joined": m.jip}));
            if hist.len() >= 60 {
                break;
            }
        }
        Ok(json!({"source": lv.source, "live": live, "history": hist, "approx": lv.source == "log", "canKick": lv.source == "live"}))
    }

    pub fn clanview(&self, pid: &str, lang: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        let Some(k) = &g.clan_key else {
            return Ok(json!({"ok": true, "name": g.display, "clan": {"ok": true, "hasClan": false}, "feed": {"ok": true, "hasClan": false, "feed": []}}));
        };
        let lang = if lang == "pl" { "pl" } else { "en" };
        Ok(crate::clan_pages::view(self, k, &g.display, lang))
    }

    pub fn clans(&self) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let key = self.db_sigs();
        {
            let mut st = self.state();
            if let Some(c) = &mut st.clans {
                if c.at.elapsed().as_secs_f64() < 10.0 {
                    return Ok((*c.val).clone());
                }
                if c.key == key {
                    c.at = Instant::now();
                    return Ok((*c.val).clone());
                }
            }
        }
        let v = Arc::new(self.build_clans(&pg));
        self.state().clans = Some(Cached { at: Instant::now(), key, val: v.clone() });
        Ok((*v).clone())
    }

    fn build_clans(&self, pg: &Groups) -> Value {
        use rusqlite::types::Value as S;
        let mut rows: Vec<(Map<String, Value>, String)> = Vec::new();
        let mut seen = HashSet::new();
        for g in &pg.list {
            let Some(c) = &g.clan else { continue };
            let k = c.get("key").map(py::to_str).unwrap_or_default();
            if !seen.insert(k.clone()) {
                continue;
            }
            let mut r = c.clone();
            r.insert("owner".into(), json!(g.display));
            r.insert("ownerId".into(), json!(g.id));
            r.insert("key".into(), Value::Null);
            r.insert("lastSeen".into(), json!(g.last_seen));
            rows.push((r, k));
        }
        let skey = |r: &Map<String, Value>| {
            let alive = r.get("status").and_then(Value::as_str) == Some("alive");
            let w = r.get("weeksTotal").and_then(Value::as_i64).unwrap_or(0) * 100 + r.get("day").and_then(Value::as_i64).unwrap_or(0);
            (!alive, -w)
        };
        rows.sort_by_key(|(r, _)| skey(r));
        let mut owners: HashMap<&str, &str> = HashMap::new();
        for g in &pg.list {
            if let Some(k) = &g.clan_key {
                owners.insert(k.as_str(), g.display.as_str());
            }
        }
        let conn = self.connect();
        let mut feed: Option<Vec<Value>> = None;
        if let Ok(c) = &conn {
            let mut items = Vec::new();
            let ok = (|| -> rusqlite::Result<()> {
                let mut q = c.prepare("SELECT token_id,id,ts,day,week,kind,mood,faction,text FROM clan_feed WHERE rowid IN (SELECT rowid FROM clan_feed ORDER BY ts DESC LIMIT 40) ORDER BY ts DESC")?;
                let mut it = q.query([])?;
                while let Some(r) = it.next()? {
                    let v: Vec<S> = (0..9).map(|i| r.get::<_, S>(i).unwrap_or(S::Null)).collect();
                    let owner = py::sql_text(&v[0]).and_then(|k| owners.get(k.as_str()).map(|s| s.to_string())).unwrap_or_else(|| "?".into());
                    let text = match py::sql_text(&v[8]) {
                        Some(t) if t.chars().count() >= 400 => Value::String(format!("{}...", py::head(&t, 400))),
                        _ => py::sqlv(v[8].clone()),
                    };
                    items.push(json!({"owner": owner, "id": py::sqlv(v[1].clone()), "ts": py::sqlv(v[2].clone()), "day": py::sqlv(v[3].clone()),
                                      "week": py::sqlv(v[4].clone()), "kind": py::sqlv(v[5].clone()), "mood": py::sqlv(v[6].clone()),
                                      "faction": py::sqlv(v[7].clone()), "text": text}));
                }
                Ok(())
            })();
            if ok.is_ok() {
                feed = Some(items);
            }
        }
        if let Ok(c) = &conn {
            let sums = self.alife_summaries(c, rows.iter().map(|(_, k)| k.as_str())).unwrap_or_default();
            for (r, k) in rows.iter_mut() {
                r.insert("alife".into(), sums.get(k.as_str()).cloned().unwrap_or_else(|| json!({})));
            }
        }
        let rows: Vec<Map<String, Value>> = rows.into_iter().map(|(r, _)| r).collect();
        let alive = |r: &Map<String, Value>| r.get("status").and_then(Value::as_str) == Some("alive");
        let fac = |r: &Map<String, Value>, f: &str| alive(r) && r.get("factionName").and_then(Value::as_str) == Some(f);
        json!({"clans": rows, "feed": feed, "alive": rows.iter().filter(|r| alive(r)).count(),
               "byFaction": {"WLF": rows.iter().filter(|r| fac(r, "WLF")).count(), "SERAPHITES": rows.iter().filter(|r| fac(r, "SERAPHITES")).count()},
               "atRisk": rows.iter().filter(|r| r.get("risk").and_then(Value::as_array).map(|a| !a.is_empty()).unwrap_or(false)).count(),
               "feedAvailable": feed.is_some()})
    }

    fn alife_summaries<'a>(&self, c: &Connection, keys: impl Iterator<Item = &'a str>) -> Option<HashMap<String, Value>> {
        use rusqlite::types::Value as S;
        let mut upd: HashMap<String, f64> = HashMap::new();
        {
            let mut q = c.prepare("SELECT token_id,updated FROM clan_alife").ok()?;
            let mut it = q.query([]).ok()?;
            while let Ok(Some(r)) = it.next() {
                let k = r.get::<_, S>(0).ok().and_then(|v| py::sql_text(&v)).unwrap_or_default();
                let u = r.get::<_, S>(1).map(|v| py::sql_f64(&v)).unwrap_or(0.0);
                upd.insert(k, u);
            }
        }
        let mut out = HashMap::new();
        for k in keys {
            let Some(&u) = upd.get(k) else {
                out.insert(k.to_string(), json!({}));
                continue;
            };
            if let Some((t, v)) = self.state().alife_sum.get(k) {
                if *t == u {
                    out.insert(k.to_string(), v.clone());
                    continue;
                }
            }
            let sm = c
                .query_row("SELECT state FROM clan_alife WHERE token_id=?", [k], |r| r.get::<_, String>(0))
                .ok()
                .and_then(|s| serde_json::from_str::<Value>(&s).ok())
                .and_then(|v| match v {
                    Value::Object(al) if al.get("v").and_then(Value::as_i64) == Some(1) => Some(al),
                    _ => None,
                })
                .map(|al| {
                    let len = |k: &str| match al.get(k) {
                        Some(Value::Array(a)) => a.len(),
                        Some(Value::Object(o)) => o.len(),
                        Some(Value::String(s)) => s.chars().count(),
                        _ => 0,
                    };
                    json!({"day": al.get("day").cloned().unwrap_or(Value::Null), "threads": len("T"), "people": len("P"),
                           "rows": al.get("stat").and_then(|s| s.get("lines")).cloned().unwrap_or(Value::Null), "templatesUsed": len("mem")})
                })
                .unwrap_or_else(|| json!({}));
            self.state().alife_sum.insert(k.to_string(), (u, sm.clone()));
            out.insert(k.to_string(), sm);
        }
        Some(out)
    }

    pub fn report_metas(&self) -> Vec<(PathBuf, Arc<Map<String, Value>>)> {
        if self.reports.is_empty() {
            return Vec::new();
        }
        let Ok(rd) = std::fs::read_dir(&self.reports) else {
            return Vec::new();
        };
        let mut dirs: Vec<String> = rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        dirs.sort_by(|a, b| b.cmp(a));
        let mut out = Vec::new();
        let mut live: HashSet<PathBuf> = HashSet::new();
        for d in dirs {
            let dir = Path::new(&self.reports).join(&d);
            let p = dir.join("meta.json");
            let s = sig(&p);
            if s.is_none() || !p.is_file() {
                continue;
            }
            live.insert(p.clone());
            let hit = self.state().metas.get(&p).filter(|(k, _)| *k == s).map(|(_, v)| v.clone());
            let meta = match hit {
                Some(v) => v,
                None => {
                    let v = std::fs::read(&p).ok().and_then(|b| serde_json::from_slice::<Value>(&b).ok()).and_then(|v| match v {
                        Value::Object(o) => Some(Arc::new(o)),
                        _ => None,
                    });
                    self.state().metas.insert(p.clone(), (s, v.clone()));
                    v
                }
            };
            if let Some(m) = meta {
                out.push((dir, m));
            }
        }
        self.state().metas.retain(|k, _| live.contains(k));
        out
    }

    pub fn reports_list(&self) -> Value {
        let mut rows = Vec::new();
        for (_, m) in self.report_metas() {
            let text = m.get("text").and_then(Value::as_str).unwrap_or("").trim();
            let first = py::splitlines(text).first().copied().unwrap_or("");
            let files = m.get("files").and_then(Value::as_array).map(Vec::len).unwrap_or(0);
            rows.push(json!({"code": m.get("code"), "received": m.get("received"), "name": m.get("name"), "version": m.get("version"),
                             "mapMode": m.get("mapMode"), "text": if text.is_empty() { "" } else { py::head(first, 120) },
                             "handled": m.get("handled").map(py::truthy).unwrap_or(false), "files": files}));
        }
        let new = rows.iter().filter(|r| r.get("handled") != Some(&Value::Bool(true))).count();
        json!({"reports": rows, "configured": !self.reports.is_empty(), "new": new})
    }

    fn find_report(&self, code: &str) -> Option<(PathBuf, Arc<Map<String, Value>>)> {
        let code = code.to_uppercase();
        let b = code.as_bytes();
        if b.len() != 6 || !code.starts_with("R-") || !b[2..].iter().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()) {
            return None;
        }
        self.report_metas().into_iter().find(|(_, m)| m.get("code").map(py::to_str).unwrap_or_default().to_uppercase() == code)
    }

    fn files_of(m: &Map<String, Value>) -> &[Value] {
        m.get("files").and_then(Value::as_array).map(Vec::as_slice).unwrap_or(&[])
    }

    fn safe_name(name: &str) -> bool {
        !name.is_empty() && name != "." && name != ".." && !name.contains('/') && !name.contains('\\') && !name.contains('\0')
    }

    pub fn report(&self, code: &str) -> Value {
        let Some((_, m)) = self.find_report(code) else {
            return json!({"ok": false, "error": "no such report"});
        };
        let mut meta = (*m).clone();
        meta.remove("clipKey");
        let exts = [".txt", ".log", ".json", ".md", ".ini", ".cfg", ".csv", ".xml"];
        let files: Vec<Value> = Self::files_of(&m)
            .iter()
            .map(|x| {
                let mut o = x.as_object().cloned().unwrap_or_default();
                let name = o.get("name").map(py::to_str).unwrap_or_default().to_lowercase();
                let kind = o.get("kind").cloned().unwrap_or(Value::Null);
                o.insert("text".into(), Value::Bool(exts.iter().any(|e| name.ends_with(e))));
                o.insert("image".into(), Value::Bool(kind == json!("screenshot")));
                o.insert("video".into(), Value::Bool(kind == json!("video")));
                Value::Object(o)
            })
            .collect();
        json!({"ok": true, "meta": meta, "files": files})
    }

    pub fn report_video(&self, code: &str, name: &str) -> Option<(PathBuf, &'static str, u64)> {
        let (d, m) = self.find_report(code)?;
        if !Self::safe_name(name) {
            return None;
        }
        Self::files_of(&m).iter().find(|x| x.get("name").and_then(Value::as_str) == Some(name) && x.get("kind") == Some(&json!("video")))?;
        let p = self.report_local_path(&d, name)?;
        let mut h = [0u8; 64];
        let n = {
            use std::io::Read;
            let mut f = std::fs::File::open(&p).ok()?;
            let mut n = 0;
            while n < 64 {
                match f.read(&mut h[n..]) {
                    Ok(0) => break,
                    Ok(k) => n += k,
                    Err(_) => return None,
                }
            }
            n
        };
        let h = &h[..n];
        let size = std::fs::metadata(&p).ok()?.len();
        let ct = if h.len() >= 12 && &h[4..8] == b"ftyp" {
            if &h[8..12] == b"qt  " {
                "video/quicktime"
            } else {
                "video/mp4"
            }
        } else if h.starts_with(&[0x1A, 0x45, 0xDF, 0xA3]) {
            if h.windows(4).any(|w| w == b"webm") {
                "video/webm"
            } else {
                "video/x-matroska"
            }
        } else {
            return None;
        };
        Some((p, ct, size))
    }

    pub fn report_image(&self, code: &str, name: &str) -> Option<(&'static str, Vec<u8>)> {
        use std::io::Read;
        let (d, m) = self.find_report(code)?;
        if !Self::safe_name(name) {
            return None;
        }
        Self::files_of(&m).iter().find(|x| x.get("name").and_then(Value::as_str) == Some(name) && x.get("kind") == Some(&json!("screenshot")))?;
        let f = std::fs::File::open(self.report_local_path(&d, name)?).ok()?;
        let mut data = Vec::new();
        f.take(8 * 1024 * 1024 + 1).read_to_end(&mut data).ok()?;
        if data.len() > 8 * 1024 * 1024 {
            return None;
        }
        let h = &data[..data.len().min(16)];
        let ct = if h.starts_with(b"\x89PNG\r\n\x1a\n") {
            "image/png"
        } else if h.starts_with(b"\xff\xd8\xff") {
            "image/jpeg"
        } else if h.len() >= 12 && h.starts_with(b"RIFF") && &h[8..12] == b"WEBP" {
            "image/webp"
        } else if h.starts_with(b"GIF87a") || h.starts_with(b"GIF89a") {
            "image/gif"
        } else if h.starts_with(b"BM") {
            "image/bmp"
        } else {
            return None;
        };
        Some((ct, data))
    }

    pub fn report_file(&self, code: &str, name: &str) -> Option<Value> {
        use std::io::{Read, Seek, SeekFrom};
        let limit: u64 = 96 * 1024;
        let (d, m) = self.find_report(code)?;
        if !Self::safe_name(name) {
            return None;
        }
        Self::files_of(&m).iter().find(|x| x.get("name").and_then(Value::as_str) == Some(name))?;
        let p = self.report_local_path(&d, name)?;
        let size = std::fs::metadata(&p).ok()?.len();
        let mut f = std::fs::File::open(&p).ok()?;
        if size > limit {
            f.seek(SeekFrom::Start(size - limit)).ok()?;
        }
        let mut data = Vec::new();
        f.take(limit).read_to_end(&mut data).ok()?;
        Some(json!({"name": name, "size": size, "truncated": size > limit, "text": String::from_utf8_lossy(&data)}))
    }

    pub fn mark_handled(&self, code: &str, handled: bool) -> Result<Value, String> {
        let Some((d, _)) = self.find_report(code) else {
            return Ok(json!({"ok": false, "error": "no such report"}));
        };
        let _g = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        let p = d.join("meta.json");
        let b = std::fs::read(&p).map_err(|e| format!("OSError: {}", e))?;
        let Ok(Value::Object(mut m)) = serde_json::from_slice::<Value>(&b) else {
            return Ok(json!({"ok": false, "error": "no such report"}));
        };
        m.insert("handled".into(), Value::Bool(handled));
        write_atomic(&p, py::dumps_indent1(&Value::Object(m)).as_bytes()).map_err(|e| format!("OSError: {}", e))?;
        Ok(json!({"ok": true, "handled": handled}))
    }

    pub fn logs(&self, q: &str, level: &str, which: &str, n: i64, quiet: bool) -> Value {
        use std::io::{Read, Seek, SeekFrom};
        use std::sync::OnceLock;
        static R: OnceLock<(regex::Regex, regex::Regex)> = OnceLock::new();
        let (noise, warn) = R.get_or_init(|| {
            (
                regex::Regex::new(r"^\[[\d:]+\] (TCP \(|UDP \(|RELAY-|UDP-|STATUS |PERF |DB backup ok|MATCHMAKE |SESSION )").expect("noise"),
                regex::Regex::new(r"(?i)timed out|silent|drop|refused|mismatch|stall|timeout|reaped|WARN|admin-kick|abort").expect("warn"),
            )
        });
        let err = logindex::err_re();
        self.refresh_log();
        let base = self.cur_log();
        if base.is_empty() {
            return json!({"ok": false, "error": "no --daemon-log configured"});
        }
        let mut path = base.clone();
        if !which.is_empty() && which != "0" {
            if !py::is_digits(which) || which.parse::<u64>().map(|v| v > 9).unwrap_or(true) {
                return json!({"ok": false, "error": "bad log file"});
            }
            path = format!("{}.{}", base, which);
        }
        let basename = Path::new(&path).file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let fail = |e: std::io::Error| json!({"ok": false, "error": format!("cannot read {}: {}: '{}'", basename, crate::daemon::io_reason(&e), path)});
        let md = match std::fs::metadata(&path) {
            Ok(m) => m,
            Err(e) => return fail(e),
        };
        let size = md.len();
        let cap: u64 = 3 * 1024 * 1024;
        let mut data = Vec::new();
        let read = std::fs::File::open(&path).and_then(|mut f| {
            f.seek(SeekFrom::Start(size.saturating_sub(cap)))?;
            f.read_to_end(&mut data)
        });
        if let Err(e) = read {
            return fail(e);
        }
        let text = String::from_utf8_lossy(&data);
        let mut lines = py::splitlines(&text);
        if size > cap && !lines.is_empty() {
            lines.remove(0);
        }
        let ql = q.to_lowercase();
        let mut out: Vec<Value> = Vec::new();
        for ln in lines {
            let is_err = err.is_match(ln);
            if quiet && noise.is_match(ln) && !is_err {
                continue;
            }
            let evt = py::head(ln, 14).contains(" EVT ");
            let cls = if is_err && !ln.starts_with("TCP (") && !ln.starts_with("UDP (") {
                "err"
            } else if warn.is_match(ln) {
                "warn"
            } else if evt {
                "evt"
            } else {
                ""
            };
            if level == "err" && cls != "err" {
                continue;
            }
            if level == "warn" && cls != "err" && cls != "warn" {
                continue;
            }
            if level == "evt" && cls != "evt" && !evt {
                continue;
            }
            let red = redact(ln);
            if !ql.is_empty() && !red.to_lowercase().contains(&ql) {
                continue;
            }
            out.push(json!({"t": py::head(&red, 420), "c": cls}));
        }
        let matched = out.len();
        let start = if n > 0 { matched.saturating_sub(n as usize) } else { (-n as usize).min(matched) };
        let shown: Vec<Value> = out.into_iter().skip(start).collect();
        let mut files: Vec<String> = logindex::rotated(Path::new(&base))
            .iter()
            .filter_map(|p| p.file_name().map(|n| n.to_string_lossy().into_owned()))
            .filter_map(|n| n.rsplit('.').next().map(str::to_string))
            .collect();
        files.sort();
        files.insert(0, "0".into());
        json!({"ok": true, "lines": shown, "matched": matched, "file": basename, "files": files, "size": size, "mtime": timeutil::mtime(&md)})
    }

    pub fn notices(&self) -> Value {
        match self.call("/notices", None, 1.5) {
            Ok(v) => v,
            Err(e) => json!({"ok": false, "error": e.reason(), "notices": []}),
        }
    }

    fn notice_call(&self, path: &str, data: Value) -> Value {
        self.call(path, Some(&data), 5.0).unwrap_or_else(|e| call_fail(e, ""))
    }

    pub fn notice_send(&self, text: &str, seconds: f64) -> Value {
        let s = if seconds != 0.0 { seconds } else { 8.0 };
        self.notice_call("/notice", json!({"text": py::head(text, 300), "seconds": s.min(600.0).max(2.0)}))
    }

    pub fn notice_clear(&self, nid: i64) -> Value {
        self.notice_call("/notice/clear", json!({"id": nid}))
    }

    pub fn shutdown_schedule(&self, minutes: f64, reason: &str, restart: bool) -> Value {
        self.notice_call("/shutdown", json!({"minutes": minutes.min(1440.0).max(0.0), "reason": py::head(reason, 100), "restart": restart}))
    }

    pub fn shutdown_cancel(&self) -> Value {
        self.notice_call("/shutdown/cancel", json!({}))
    }

    pub fn teams(&self) -> Value {
        self.call("/teams", None, 3.0).unwrap_or_else(|e| call_fail(e, ""))
    }

    pub fn teams_move(&self, pid: &str) -> Value {
        if !valid_pid(pid) {
            return json!({"ok": false, "error": "bad id"});
        }
        self.notice_call("/teams/move", json!({"pid": pid}))
    }

    pub fn teams_set(&self, rules: &str) -> Value {
        self.notice_call("/teams", json!({"rules": py::head(rules, 4000)}))
    }

    pub fn kick(&self, pid: &str, reason: &str) -> Value {
        if !valid_pid(pid) {
            return json!({"ok": false, "error": "bad id"});
        }
        let reason = clean_kick_reason(reason);
        let mut body = Map::new();
        body.insert("pid".into(), json!(pid));
        if !reason.is_empty() {
            body.insert("reason".into(), json!(reason));
        }
        self.call("/kick", Some(&Value::Object(body)), 5.0)
            .unwrap_or_else(|e| call_fail(e, ". It exists on the daemon build with the admin op and needs a daemon restart."))
    }

    fn bans_path(&self) -> PathBuf {
        self.dbdir.join("bans.txt")
    }

    fn bans_lines(&self) -> Vec<String> {
        match std::fs::read(self.bans_path()) {
            Ok(b) => {
                let t = String::from_utf8_lossy(&b).replace('\u{fffd}', "");
                py::splitlines(&t).into_iter().map(str::to_string).collect()
            }
            Err(_) => vec!["# bans (one per line: steam id or name)".into()],
        }
    }

    fn ban_entry(line: &str) -> &str {
        line.split('#').next().unwrap_or("").trim()
    }

    fn ban_hit(line: &str, sid: i64, names: &HashSet<String>) -> bool {
        let e = Self::ban_entry(line);
        !e.is_empty() && ((sid != 0 && e == sid.to_string()) || (!py::is_digits(e) && names.contains(&e.to_lowercase())))
    }

    pub fn ban_state(&self, g: &Group) -> Value {
        for line in self.bans_lines() {
            if Self::ban_hit(&line, g.steamid, &g.alias) {
                let c = line.split_once('#').map(|(_, c)| c).unwrap_or("");
                let reason = c.split_once("reason:").map(|(_, r)| r.trim()).unwrap_or("");
                return json!({"reason": reason, "line": line.trim()});
            }
        }
        Value::Null
    }

    fn banned_index(&self) -> (HashSet<String>, HashSet<String>) {
        let mut ids = HashSet::new();
        let mut names = HashSet::new();
        for line in self.bans_lines() {
            let e = Self::ban_entry(&line);
            let low = e.to_lowercase();
            if e.is_empty() || low.starts_with("hwid:") || low.starts_with("token:") {
                continue;
            }
            if py::is_digits(e) {
                ids.insert(e.to_string());
            } else {
                names.insert(low);
            }
        }
        (ids, names)
    }

    fn bans_write(&self, lines: &[String]) -> Result<(), String> {
        let p = self.bans_path();
        if p.exists() {
            let mut bak = p.as_os_str().to_owned();
            bak.push(".bak-admin");
            copy2(&p, Path::new(&bak)).map_err(|e| format!("OSError: {}", e))?;
        }
        let data = format!("{}\n", lines.join("\n").trim_end_matches('\n'));
        write_atomic(&p, data.as_bytes()).map_err(|e| format!("OSError: {}", e))
    }

    fn audit(&self, line: &str) {
        use std::io::Write;
        if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(self.dbdir.join("admin-actions.log")) {
            let _ = writeln!(f, "{} {}", timeutil::datetime_str(timeutil::now()), line);
        }
    }

    pub fn ban(&self, pid: &str, reason: &str, who: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        let _guard = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        if !self.ban_state(g).is_null() {
            return Ok(json!({"ok": false, "error": "already banned"}));
        }
        let (reason, who, name) = (clean_ban(reason, 160), clean_ban(who, 60), clean_ban(&g.display, 60));
        let tail = format!(
            "{} - admin site {}{}{}",
            name,
            timeutil::date_str(timeutil::now()),
            if who.is_empty() { String::new() } else { format!(" by {}", who) },
            if reason.is_empty() { String::new() } else { format!(" | reason: {}", reason) }
        );
        let entry = if g.steamid != 0 {
            format!("{}  # {}", g.steamid, tail)
        } else if !name.is_empty() && !py::is_digits(&name) {
            format!("{}  # {}", name, tail)
        } else {
            return Ok(json!({"ok": false, "error": "this player has no Steam id or usable name to ban"}));
        };
        let mut lines = self.bans_lines();
        lines.push(entry.clone());
        self.bans_write(&lines)?;
        let sid_text = if g.steamid != 0 { g.steamid.to_string() } else { "no steam id".into() };
        self.audit(&format!("BAN {} ({}) by {}: {}", name, sid_text, if who.is_empty() { "?" } else { &who }, if reason.is_empty() { "-" } else { &reason }));
        drop(_guard);
        let lv = self.live_view();
        let online = self.online_rows(&lv, Some(&pg)).into_iter().find(|p| p.get("playerId").and_then(Value::as_str) == Some(pid) && p.get("pid").map(py::truthy).unwrap_or(false));
        let kicked = match online {
            Some(o) => {
                let kr = if reason.is_empty() { "Banned".to_string() } else { format!("Banned: {}", reason) };
                let r = self.kick(&py::to_str(&o["pid"]), &kr);
                r.get("ok").cloned().unwrap_or(Value::Bool(false))
            }
            None => Value::Null,
        };
        Ok(json!({"ok": true, "name": name, "entry": entry, "kicked": kicked}))
    }

    pub fn unban(&self, pid: &str, who: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        let _guard = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        let lines = self.bans_lines();
        let keep: Vec<String> = lines.iter().filter(|l| !Self::ban_hit(l, g.steamid, &g.alias)).cloned().collect();
        if keep.len() == lines.len() {
            return Ok(json!({"ok": false, "error": "not banned"}));
        }
        self.bans_write(&keep)?;
        let removed = lines.len() - keep.len();
        let sid_text = if g.steamid != 0 { g.steamid.to_string() } else { "no steam id".into() };
        self.audit(&format!("UNBAN {} ({}) by {}: removed {} line(s)", g.display, sid_text, if who.is_empty() { "?" } else { who }, removed));
        Ok(json!({"ok": true, "name": g.display, "removed": removed}))
    }

    fn drop_caches(&self) {
        let mut st = self.state();
        st.players = None;
        st.clans = None;
    }

    pub fn clan_reset(&self, pid: &str, confirm: &str, names: bool) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        if confirm.trim().to_lowercase() != g.display.trim().to_lowercase() {
            return Ok(json!({"ok": false, "error": "the typed name does not match"}));
        }
        let body = json!({"name": g.display, "steamid": g.steamid, "tokenIds": g.token_ids, "names": names});
        let r = match self.call("/clan/reset", Some(&body), 15.0) {
            Ok(r) => r,
            Err(e) => return Ok(call_fail(e, ". The clan reset needs the daemon build that has it (restart).")),
        };
        self.drop_caches();
        Ok(r)
    }

    pub fn clan_backups(&self, pid: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        Ok(match self.call(&format!("/clan/backups?steamid={}", g.steamid), None, 5.0) {
            Ok(v) => v,
            Err(e) => json!({"ok": false, "error": e.reason(), "backups": []}),
        })
    }

    pub fn clan_restore(&self, pid: &str, file: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        if pg.get(pid).is_none() {
            return Ok(json!({"ok": false, "error": "no such player"}));
        }
        let r = match self.call("/clan/restore", Some(&json!({"file": file})), 15.0) {
            Ok(r) => r,
            Err(e) => return Ok(json!({"ok": false, "error": e.reason()})),
        };
        self.drop_caches();
        Ok(r)
    }

    fn news_path(&self) -> Option<PathBuf> {
        (!self.update_data.is_empty()).then(|| Path::new(&self.update_data).join("news.json"))
    }

    pub fn news_get(&self) -> Value {
        let Some(p) = self.news_path() else {
            return json!({"ok": false, "error": "the admin site was started without --update-data, so it cannot find news.json"});
        };
        let v = match std::fs::read(&p) {
            Ok(b) => match serde_json::from_slice::<Value>(&b) {
                Ok(v) => v,
                Err(e) => return json!({"ok": false, "error": format!("news.json unreadable: {}", e)}),
            },
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => json!([]),
            Err(e) => return json!({"ok": false, "error": format!("news.json unreadable: {}", e)}),
        };
        json!({"ok": true, "entries": if v.is_array() { v } else { json!([]) }, "path": p.to_string_lossy()})
    }

    pub fn news_save(&self, entries: Option<&Value>) -> Result<Value, String> {
        let Some(p) = self.news_path() else {
            return Ok(json!({"ok": false, "error": "the admin site was started without --update-data"}));
        };
        let out = match crate::news::validate(entries) {
            Ok(v) => v,
            Err(e) => return Ok(json!({"ok": false, "error": e})),
        };
        let data = format!("{}\n", py::dumps_indent1(&Value::Array(out.clone())));
        let _g = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        let mut backup = String::new();
        if p.is_file() {
            let bdir = p.parent().map(|d| d.join("news-backups")).unwrap_or_else(|| PathBuf::from("news-backups"));
            std::fs::create_dir_all(&bdir).map_err(|e| format!("OSError: {}", e))?;
            let stamp = timeutil::stamp_str(timeutil::now());
            let mut b = bdir.join(format!("news-{}.json", stamp));
            let mut n = 1;
            while b.exists() {
                n += 1;
                b = bdir.join(format!("news-{}-{}.json", stamp, n));
            }
            copy2(&p, &b).map_err(|e| format!("OSError: {}", e))?;
            backup = b.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            let mut old: Vec<String> = std::fs::read_dir(&bdir)
                .map(|rd| rd.flatten().map(|e| e.file_name().to_string_lossy().into_owned()).filter(|f| f.starts_with("news-") && f.ends_with(".json")).collect())
                .unwrap_or_default();
            old.sort();
            let cut = old.len().saturating_sub(20);
            for f in &old[..cut] {
                let _ = std::fs::remove_file(bdir.join(f));
            }
        }
        write_atomic(&p, data.as_bytes()).map_err(|e| format!("OSError: {}", e))?;
        Ok(json!({"ok": true, "count": out.len(), "backup": backup, "entries": out}))
    }
}

fn clan_row(r: &[rusqlite::types::Value]) -> Map<String, Value> {
    let int = |i: usize| py::sql_int(&r[i]);
    let (fac, status, day, h, u, s) = (int(1), int(2), int(3), int(4), int(5), int(6));
    let size = h + u + s;
    let lr = match serde_json::from_str::<Value>(&py::sql_text(&r[10]).filter(|t| !t.is_empty()).unwrap_or_else(|| "{}".into())) {
        Ok(Value::Object(o)) => o,
        _ => Map::new(),
    };
    let alive = status == ALIVE;
    let (wk, dw) = if alive { week_day(day) } else { (0, 0) };
    let mut risk: Vec<String> = Vec::new();
    if alive {
        if size <= 2 {
            risk.push("tiny clan".into());
        }
        if size != 0 && ((u + s) as f64) >= 0.6 * size as f64 {
            risk.push(format!("{} of {} hungry or sick", u + s, size));
        }
        let died = lr.get("died").filter(|v| py::truthy(v)).and_then(py::to_int).unwrap_or(0);
        if died > 0 {
            risk.push(format!("{} died last match", died));
        }
    }
    let mut m = Map::new();
    m.insert("faction".into(), json!(fac));
    m.insert("factionName".into(), json!(faction_name(fac)));
    m.insert("status".into(), json!(status_name(status)));
    m.insert("day".into(), json!(day));
    m.insert("daysTotal".into(), json!(WIN_DAY));
    m.insert("week".into(), json!(wk));
    m.insert("dayOfWeek".into(), json!(dw));
    m.insert("weeks".into(), json!(WEEKS));
    m.insert("size".into(), json!(size));
    m.insert("healthy".into(), json!(h));
    m.insert("hungry".into(), json!(u));
    m.insert("sick".into(), json!(s));
    m.insert("needed".into(), json!(needed(size)));
    m.insert("weeksTotal".into(), json!(int(7)));
    m.insert("runsWon".into(), json!(int(8)));
    m.insert("runsLost".into(), json!(int(9)));
    m.insert("lastParts".into(), lr.get("parts").cloned().unwrap_or(Value::Null));
    m.insert("lastSupplies".into(), lr.get("supplies").cloned().unwrap_or(Value::Null));
    m.insert("lastDied".into(), lr.get("died").cloned().unwrap_or(Value::Null));
    m.insert("risk".into(), json!(risk));
    m.insert("updated".into(), py::sqlv(r[12].clone()));
    m.insert("key".into(), py::sqlv(r[0].clone()));
    m
}

// ---- chat mutes (<db dir>/mutes.txt, re-read by the daemon), chat page, match page, report files on storage boxes ----

/// The daemon's account keys of a player group (social.Social.acct): the Steam account, else every install account.
fn mute_keys(g: &Group) -> Vec<String> {
    if g.steamid != 0 {
        return vec![format!("s:{}", g.steamid)];
    }
    g.token_ids.iter().map(|t| token_key(t)).collect()
}

/// "t:" + the first 16 hex characters of sha256(token id).
fn token_key(tid: &str) -> String {
    let d = Sha256::digest(tid.as_bytes());
    let mut s = String::with_capacity(18);
    s.push_str("t:");
    for b in &d[..8] {
        s.push_str(&format!("{:02x}", b));
    }
    s
}

/// (account, until, note) or None for a comment / blank line.
fn mute_parse(line: &str) -> Option<(String, f64, String)> {
    let (head, note) = line.split_once('#').unwrap_or((line, ""));
    let mut parts = head.split_whitespace();
    let first = parts.next()?;
    let key = if first.len() == 17 && first.bytes().all(|b| b.is_ascii_digit()) { format!("s:{}", first) } else { first.to_string() };
    let until = parts.next().map(|p| p.parse::<f64>().unwrap_or(0.0)).unwrap_or(0.0);
    Some((key, until, note.trim().to_string()))
}

fn mute_live(until: f64, now: f64) -> bool {
    until <= 0.0 || until > now
}

/// Python's "%g" for a duration in minutes.
fn fmt_g(x: f64) -> String {
    if x == 0.0 {
        return "0".into();
    }
    let e = format!("{:.5e}", x);
    let (mant, exp) = e.split_once('e').unwrap_or((&e, "0"));
    let exp: i32 = exp.parse().unwrap_or(0);
    let trim = |s: &str| -> String {
        if s.contains('.') {
            s.trim_end_matches('0').trim_end_matches('.').to_string()
        } else {
            s.to_string()
        }
    };
    if !(-4..6).contains(&exp) {
        return format!("{}e{}{:02}", trim(mant), if exp < 0 { '-' } else { '+' }, exp.abs());
    }
    trim(&format!("{:.*}", (5 - exp).max(0) as usize, x))
}

fn expand_user(p: &str) -> String {
    if p == "~" || p.starts_with("~/") {
        if let Ok(h) = std::env::var("HOME") {
            if !h.is_empty() {
                return format!("{}{}", h.trim_end_matches('/'), &p[1..]);
            }
        }
    }
    p.to_string()
}

static REPORT_FETCH_LOCK: Mutex<()> = Mutex::new(());

/// {account: until} of every mute still running, in the order the lines name them (a dict: a repeated account keeps its first place).
pub struct Muted {
    order: Vec<(String, f64)>,
    idx: HashMap<String, usize>,
}

impl Muted {
    pub fn has(&self, k: &str) -> bool {
        self.idx.contains_key(k)
    }
}

impl Admin {
    fn mutes_path(&self) -> PathBuf {
        self.dbdir.join("mutes.txt")
    }

    fn mutes_lines(&self) -> Vec<String> {
        match std::fs::read(self.mutes_path()) {
            Ok(b) => {
                let t = String::from_utf8_lossy(&b).replace('\u{fffd}', "");
                py::splitlines(&t).into_iter().map(str::to_string).collect()
            }
            Err(_) => vec!["# chat mutes (one per line: <account> <until unix time, 0 = for good>  # note)".into()],
        }
    }

    pub fn muted_keys(&self) -> Muted {
        let now = timeutil::now();
        let mut m = Muted { order: Vec::new(), idx: HashMap::new() };
        for line in self.mutes_lines() {
            if let Some((k, until, _)) = mute_parse(&line) {
                if mute_live(until, now) {
                    match m.idx.get(&k) {
                        Some(&i) => m.order[i].1 = until,
                        None => {
                            m.idx.insert(k.clone(), m.order.len());
                            m.order.push((k, until));
                        }
                    }
                }
            }
        }
        m
    }

    /// Null or {"until", "left" (s, -1 = for good), "reason", "line"} for a player group.
    pub fn mute_state(&self, g: &Group) -> Value {
        let keys: HashSet<String> = mute_keys(g).into_iter().collect();
        let now = timeutil::now();
        for line in self.mutes_lines() {
            if let Some((k, until, note)) = mute_parse(&line) {
                if keys.contains(&k) && mute_live(until, now) {
                    let reason = note.split_once("reason:").map(|(_, r)| r.trim()).unwrap_or("");
                    return json!({"until": until as i64, "left": if until <= 0.0 { -1 } else { (until - now) as i64 }, "reason": reason, "line": line.trim()});
                }
            }
        }
        Value::Null
    }

    fn mutes_write(&self, lines: &[String]) -> Result<(), String> {
        let p = self.mutes_path();
        if p.exists() {
            let mut bak = p.as_os_str().to_owned();
            bak.push(".bak-admin");
            copy2(&p, Path::new(&bak)).map_err(|e| format!("OSError: {}", e))?;
        }
        let data = format!("{}\n", lines.join("\n").trim_end_matches('\n'));
        write_atomic(&p, data.as_bytes()).map_err(|e| format!("OSError: {}", e))
    }

    /// Mute a player for `minutes` (0 = for good). A new mute replaces the player's old one.
    pub fn mute(&self, pid: &str, minutes: &Value, reason: &str, who: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        let keys = mute_keys(g);
        if keys.is_empty() {
            return Ok(json!({"ok": false, "error": "this player has no account to mute"}));
        }
        let minutes = if py::truthy(minutes) { py::to_float(minutes) } else { Some(0.0) };
        let minutes = match minutes {
            Some(m) if (0.0..=10.0 * 365.0 * 1440.0).contains(&m) => m,
            _ => return Ok(json!({"ok": false, "error": "bad duration"})),
        };
        let (reason, who, name) = (clean_ban(reason, 160), clean_ban(who, 60), clean_ban(&g.display, 60));
        let now = timeutil::now();
        let until = if minutes != 0.0 { (now + minutes * 60.0) as i64 } else { 0 };
        let l = timeutil::localtime(now);
        let tail = format!(
            "{} - admin site {:04}-{:02}-{:02} {:02}:{:02}{}{}",
            name,
            l.year,
            l.month,
            l.day,
            l.hour,
            l.min,
            if who.is_empty() { String::new() } else { format!(" by {}", who) },
            if reason.is_empty() { String::new() } else { format!(" | reason: {}", reason) }
        );
        let ks: HashSet<&str> = keys.iter().map(String::as_str).collect();
        let _guard = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        let mut keep: Vec<String> = Vec::new();
        for line in self.mutes_lines() {
            if let Some((k, u, _)) = mute_parse(&line) {
                if ks.contains(k.as_str()) || !mute_live(u, now) {
                    continue; // the player's old mute, or one that ran out
                }
            }
            keep.push(line);
        }
        keep.extend(keys.iter().map(|k| format!("{} {}  # {}", k, until, tail)));
        self.mutes_write(&keep)?;
        self.audit(&format!(
            "MUTE {} ({}) for {} by {}: {}",
            name,
            keys.join(","),
            if minutes != 0.0 { format!("{} min", fmt_g(minutes)) } else { "good".into() },
            if who.is_empty() { "?" } else { &who },
            if reason.is_empty() { "-" } else { &reason }
        ));
        Ok(json!({"ok": true, "name": name, "until": until}))
    }

    pub fn unmute(&self, pid: &str, who: &str) -> Result<Value, String> {
        let pg = self.player_groups()?;
        let Some(g) = pg.get(pid) else {
            return Ok(json!({"ok": false, "error": "no such player"}));
        };
        let ks: HashSet<String> = mute_keys(g).into_iter().collect();
        let _guard = self.file_lock.lock().unwrap_or_else(|e| e.into_inner());
        let lines = self.mutes_lines();
        let keep: Vec<String> = lines.iter().filter(|l| !mute_parse(l).map(|e| ks.contains(&e.0)).unwrap_or(false)).cloned().collect();
        if keep.len() == lines.len() {
            return Ok(json!({"ok": false, "error": "not muted"}));
        }
        self.mutes_write(&keep)?;
        self.audit(&format!("UNMUTE {} by {}", g.display, if who.is_empty() { "?" } else { who }));
        Ok(json!({"ok": true, "name": g.display}))
    }

    /// {daemon account key: (player id, shown name)} for the chat page (cached 30 s).
    fn acct_index(&self) -> Result<Arc<HashMap<String, (String, String)>>, String> {
        if let Some((at, v)) = &self.state().acct_index {
            if at.elapsed().as_secs_f64() < 30.0 {
                return Ok(v.clone());
            }
        }
        let pg = self.player_groups()?;
        let mut out = HashMap::new();
        for g in &pg.list {
            for k in mute_keys(g) {
                out.insert(k, (g.id.clone(), g.display.clone()));
            }
        }
        let v = Arc::new(out);
        self.state().acct_index = Some((Instant::now(), v.clone()));
        Ok(v)
    }

    /// Chat lines from chat_log (MATCH / TEAM / PARTY / LOBBY), newest first, plus the running mutes.
    pub fn chat(&self, q: &str, kind: &str, pid: &str, before: i64, n: i64) -> Result<Value, String> {
        use rusqlite::types::Value as S;
        let n = if n == 0 { 200 } else { n }.clamp(1, 500);
        let mut wh: Vec<String> = Vec::new();
        let mut args: Vec<S> = Vec::new();
        if ["match", "team", "party", "lobby"].contains(&kind) {
            wh.push("kind=?".into());
            args.push(S::Text(kind.into()));
        }
        if before != 0 {
            wh.push("id<?".into());
            args.push(S::Integer(before));
        }
        if !pid.is_empty() {
            let keys = self.player_groups()?.get(pid).map(mute_keys).unwrap_or_default();
            if keys.is_empty() {
                return Ok(json!({"lines": [], "mutes": [], "player": null}));
            }
            wh.push(format!("acct IN ({})", vec!["?"; keys.len()].join(",")));
            args.extend(keys.into_iter().map(S::Text));
        }
        if !q.is_empty() {
            wh.push("(body LIKE ? ESCAPE '\\' OR name LIKE ? ESCAPE '\\')".into());
            let lq = format!("%{}%", q.replace('\\', "\\\\").replace('%', "\\%").replace('_', "\\_"));
            args.push(S::Text(lq.clone()));
            args.push(S::Text(lq));
        }
        let sql = format!(
            "SELECT id, ts, kind, room, acct, name, body, muted FROM chat_log{} ORDER BY id DESC LIMIT ?",
            if wh.is_empty() { String::new() } else { format!(" WHERE {}", wh.join(" AND ")) }
        );
        args.push(S::Integer(n));
        let c = self.connect().map_err(|e| format!("OperationalError: {}", e))?;
        // (no chat_log yet: the daemon makes it on the first chat line)
        let rows: Vec<Vec<S>> = (|| -> rusqlite::Result<Vec<Vec<S>>> {
            let mut st = c.prepare(&sql)?;
            let it = st.query_map(rusqlite::params_from_iter(args.iter()), |r| (0..8).map(|i| r.get::<_, S>(i)).collect::<rusqlite::Result<Vec<S>>>())?;
            it.collect()
        })()
        .unwrap_or_default();
        let idx = self.acct_index()?;
        let muted = self.muted_keys();
        let text = |v: &S| py::sql_text(v).unwrap_or_default();
        let mut lines = Vec::new();
        for r in &rows {
            let acct = py::sql_text(&r[4]).unwrap_or_default();
            let p = idx.get(&acct);
            lines.push(json!({"id": py::sqlv(r[0].clone()), "ts": py::sqlv(r[1].clone()), "kind": py::sqlv(r[2].clone()), "room": text(&r[3]),
                              "name": text(&r[5]), "text": text(&r[6]), "pid": p.map(|p| p.0.as_str()).unwrap_or(""),
                              "player": p.map(|p| p.1.as_str()).unwrap_or(""), "sentMuted": py::sql_int(&r[7]) != 0, "mutedNow": muted.has(&acct)}));
        }
        let now = timeutil::now();
        let mut mutes: Vec<(i64, Value)> = Vec::new();
        for (k, until) in &muted.order {
            let p = idx.get(k);
            let left = if *until <= 0.0 { -1 } else { (until - now) as i64 };
            mutes.push((left, json!({"pid": p.map(|p| p.0.as_str()).unwrap_or(""), "name": p.map(|p| p.1.as_str()).unwrap_or(k.as_str()),
                                     "until": *until as i64, "left": left})));
        }
        mutes.sort_by_key(|m| (m.0 != -1, m.0));
        let mutes: Vec<Value> = mutes.into_iter().map(|m| m.1).collect();
        let player = if pid.is_empty() { Value::Null } else { self.player_groups()?.get(pid).map(|g| Value::String(g.display.clone())).unwrap_or(Value::Null) };
        Ok(json!({"lines": lines, "mutes": mutes, "player": player}))
    }

    /// One match for its own page: players with ban / mute state and kick target, the match's MATCH / TEAM chat, the match info.
    pub fn match_detail(&self, mid: &str) -> Result<Value, String> {
        use rusqlite::types::Value as S;
        let mid = mid.trim();
        if !(1..=32).contains(&mid.len()) || !mid.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'-') {
            return Ok(json!({"ok": false, "error": "bad match id"}));
        }
        let lv = self.live_view();
        let live = arr(&lv.snap, "matches").iter().find(|m| s_of(m, "id") == mid).cloned();
        let (ms, _, _) = self.matches_now();
        let lm = ms.iter().find(|m| m.id == mid);
        let pg = self.player_groups()?;
        let mut by_pid: HashMap<String, Option<String>> = HashMap::new();
        for p in self.online_rows(&lv, Some(&pg)) {
            if p.get("pid").map(py::truthy).unwrap_or(false) {
                by_pid.insert(py::to_str(&p["pid"]), p.get("playerId").filter(|v| py::truthy(v)).map(py::to_str));
            }
        }
        let group_of = |name: &str, pid: Option<&str>| -> Option<&Group> {
            if let Some(gid) = pid.and_then(|p| by_pid.get(p)).and_then(|g| g.as_deref()) {
                if let Some(g) = pg.get(gid) {
                    return Some(g);
                }
            }
            let n = name.to_lowercase();
            pg.list.iter().find(|g| g.alias.contains(&n))
        };
        let muted = self.muted_keys();
        let src: Vec<Map<String, Value>> = if let Some(l) = &live {
            l.get("players").and_then(Value::as_array).map(|a| a.iter().filter_map(|p| p.as_object().cloned()).collect()).unwrap_or_default()
        } else if let Some(m) = lm {
            let gone: HashSet<&str> = m.left.iter().filter(|l| l.why != "back").map(|l| l.name.as_str()).collect();
            m.players
                .iter()
                .enumerate()
                .map(|(i, n)| {
                    let mut o = Map::new();
                    o.insert("name".into(), json!(n));
                    o.insert("pid".into(), Value::Null);
                    o.insert("team".into(), json!(i % 2));
                    o.insert("left".into(), json!(gone.contains(n.as_str())));
                    o.insert("host".into(), json!(m.host.as_deref() == Some(n.as_str())));
                    o.insert("ping_ms".into(), Value::Null);
                    o
                })
                .collect()
        } else {
            Vec::new()
        };
        let mut players = Vec::new();
        for mut p in src {
            let name = p.get("name").filter(|v| py::truthy(v)).map(py::to_str).unwrap_or_default();
            let ppid = p.get("pid").filter(|v| py::truthy(v)).map(py::to_str);
            let g = group_of(&name, ppid.as_deref());
            p.insert("playerId".into(), json!(g.map(|g| g.id.as_str()).unwrap_or("")));
            p.insert("display".into(), json!(g.map(|g| g.display.as_str()).unwrap_or(name.as_str())));
            p.insert("steamid".into(), json!(g.map(|g| g.steamid).unwrap_or(0)));
            p.insert("banned".into(), json!(g.map(|g| !self.ban_state(g).is_null()).unwrap_or(false)));
            p.insert("muted".into(), json!(g.map(|g| mute_keys(g).iter().any(|k| muted.has(k))).unwrap_or(false)));
            let kick = live.is_some() && ppid.is_some() && !p.get("left").map(py::truthy).unwrap_or(false) && lv.source == "live";
            p.insert("kick".into(), json!(kick));
            players.push(Value::Object(p));
        }
        let c = self.connect().map_err(|e| format!("OperationalError: {}", e))?;
        let rows: Vec<Vec<S>> = (|| -> rusqlite::Result<Vec<Vec<S>>> {
            let mut st = c.prepare("SELECT id, ts, kind, acct, name, body, muted FROM chat_log WHERE room=? AND kind IN ('match','team') ORDER BY id LIMIT 600")?;
            let it = st.query_map([mid], |r| (0..7).map(|i| r.get::<_, S>(i)).collect::<rusqlite::Result<Vec<S>>>())?;
            it.collect()
        })()
        .unwrap_or_default();
        if live.is_none() && lm.is_none() && rows.is_empty() {
            return Ok(json!({"ok": false, "error": "no such match in the live feed, the retained logs or the chat log"}));
        }
        let idx = self.acct_index()?;
        let text = |v: &S| py::sql_text(v).unwrap_or_default();
        let chat: Vec<Value> = rows
            .iter()
            .map(|r| {
                let acct = py::sql_text(&r[3]).unwrap_or_default();
                json!({"id": py::sqlv(r[0].clone()), "ts": py::sqlv(r[1].clone()), "kind": py::sqlv(r[2].clone()), "name": text(&r[4]), "text": text(&r[5]),
                       "pid": idx.get(&acct).map(|p| p.0.as_str()).unwrap_or(""), "sentMuted": py::sql_int(&r[6]) != 0, "mutedNow": muted.has(&acct)})
            })
            .collect();
        let mut info = Map::new();
        info.insert("id".into(), json!(mid));
        info.insert("live".into(), json!(live.is_some()));
        let state = match (&live, lm) {
            (Some(l), _) => l.get("state").cloned().unwrap_or_else(|| json!("ended")),
            (None, Some(m)) => json!(m.state),
            _ => json!("ended"),
        };
        info.insert("state".into(), state);
        if let Some(l) = &live {
            let g = |k: &str| l.get(k).filter(|v| py::truthy(v)).cloned();
            let map = l.get("map").filter(|v| py::truthy(v)).and_then(|m| m.get("name")).filter(|v| py::truthy(v)).cloned().unwrap_or_else(|| json!("Map not chosen yet"));
            info.insert("map".into(), map);
            info.insert("mode".into(), g("game_mode").or_else(|| g("mode")).unwrap_or_else(|| json!("")));
            for k in ["net", "age_s", "clock", "lives"] {
                info.insert(k.into(), l.get(k).cloned().unwrap_or(Value::Null));
            }
        }
        if let Some(m) = lm {
            let res = if m.state == "ended" {
                match m.winner {
                    Some(0) => "TEAM 1 WON",
                    Some(1) => "TEAM 2 WON",
                    Some(2) => "DRAW",
                    _ => "RESULT UNKNOWN",
                }
                .to_string()
            } else if m.closed.is_some() {
                format!("NOT FINISHED ({})", if m.closed_why.is_empty() { "closed" } else { &m.closed_why })
            } else {
                String::new()
            };
            let left_log: Vec<String> = m.left.iter().filter(|l| EARLY.contains(&l.why.as_str())).map(|l| format!("{} ({})", l.name, l.why)).collect();
            for (k, v) in [
                ("formed", json!(m.formed)),
                ("started", json!(m.started)),
                ("ended", json!(m.ended)),
                ("result", json!(res)),
                ("winner", json!(m.winner)),
                ("secs", json!(m.secs)),
                ("host", json!(m.host)),
                ("ping_ms", json!(m.ping_ms)),
                ("left_log", json!(left_log)),
                ("joined", json!(m.jip)),
            ] {
                info.insert(k.into(), v);
            }
            if !info.contains_key("map") {
                info.insert(
                    "map".into(),
                    json!(match m.map.as_deref() {
                        Some(l) if !l.is_empty() => pretty_level(l),
                        _ => "?".to_string(),
                    }),
                );
            }
            if !info.contains_key("net") {
                info.insert("net".into(), json!(m.net));
            }
        }
        Ok(json!({"ok": true, "match": info, "players": players, "chat": chat, "canKick": lv.source == "live"}))
    }

    // Bug-report files can live on a storage box (<report>/store.json says which box and which files); a moved file is copied back into a
    // small local cache on first use. T2R_REPORT_BOXES = the boxes JSON file the mover uses.
    fn report_boxes() -> Option<Value> {
        let path = std::env::var("T2R_REPORT_BOXES").ok().filter(|p| !p.is_empty())?;
        serde_json::from_slice(&std::fs::read(expand_user(&path)).ok()?).ok()
    }

    fn report_cache_dir() -> PathBuf {
        match std::env::var("T2R_REPORT_CACHE") {
            Ok(p) if !p.is_empty() => PathBuf::from(p),
            _ => PathBuf::from(expand_user("~/.cache/t2r-report-cache")),
        }
    }

    fn report_cache_max() -> u64 {
        std::env::var("T2R_REPORT_CACHE_MB").ok().and_then(|s| s.trim().parse::<u64>().ok()).unwrap_or(800) << 20
    }

    fn report_cache_trim(keep: &Path) {
        fn walk(dir: &Path, out: &mut Vec<(f64, u64, PathBuf)>) {
            let Ok(rd) = std::fs::read_dir(dir) else { return };
            for e in rd.flatten() {
                let p = e.path();
                match e.file_type() {
                    Ok(t) if t.is_dir() => walk(&p, out),
                    Ok(_) => {
                        if let Ok(md) = std::fs::metadata(&p) {
                            out.push((md.atime() as f64 + md.atime_nsec() as f64 / 1e9, md.len(), p));
                        }
                    }
                    Err(_) => {}
                }
            }
        }
        let mut ents = Vec::new();
        walk(&Self::report_cache_dir(), &mut ents);
        let mut total: u64 = ents.iter().map(|e| e.1).sum();
        let max = Self::report_cache_max();
        ents.sort_by(|a, b| a.0.total_cmp(&b.0).then(a.1.cmp(&b.1)).then_with(|| a.2.cmp(&b.2)));
        for (_, size, p) in ents {
            if total <= max {
                break;
            }
            if p == keep {
                continue;
            }
            if std::fs::remove_file(&p).is_ok() {
                total -= size;
            }
        }
    }

    /// Readable local path of files/<name> of report folder d: the file itself, or a cached copy fetched from its storage box.
    fn report_local_path(&self, d: &Path, name: &str) -> Option<PathBuf> {
        if name.is_empty() || name.contains('/') || name.contains('\\') || name.starts_with('.') {
            return None;
        }
        let p = d.join("files").join(name);
        if p.is_file() {
            return Some(p);
        }
        let st: Value = serde_json::from_slice(&std::fs::read(d.join("store.json")).ok()?).ok()?;
        let want = u64::try_from(py::to_int(st.get("files")?.get(name)?)?).ok()?;
        let boxname = st.get("box")?.clone();
        let folder = d.file_name()?.to_string_lossy().into_owned();
        let cp = Self::report_cache_dir().join(&folder).join(name);
        let _g = REPORT_FETCH_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        if std::fs::metadata(&cp).map(|m| m.len() == want).unwrap_or(false) {
            if let Ok(c) = std::ffi::CString::new(cp.as_os_str().as_encoded_bytes()) {
                unsafe {
                    libc::utimensat(libc::AT_FDCWD, c.as_ptr(), std::ptr::null(), 0);
                }
            }
            return Some(cp);
        }
        let cfg = Self::report_boxes()?;
        let bx = cfg.get("boxes")?.as_array()?.iter().find(|b| b.get("name") == Some(&boxname))?;
        let host = py::to_str(bx.get("host")?);
        std::fs::create_dir_all(cp.parent()?).ok()?;
        let mut tmp = cp.as_os_str().to_owned();
        tmp.push(".part");
        let tmp = PathBuf::from(tmp);
        let cfg_s = |k: &str, d: &str| expand_user(&cfg.get(k).filter(|v| py::truthy(v)).map(py::to_str).unwrap_or_else(|| d.to_string()));
        let ssh = format!(
            "ssh -i {} -o BatchMode=yes -o StrictHostKeyChecking=yes -o UserKnownHostsFile={} -o ConnectTimeout=10",
            cfg_s("key", "~/.ssh/t2r_reports"),
            cfg_s("known_hosts", "~/.ssh/known_hosts_reports")
        );
        let fetched = (|| -> Option<()> {
            let mut ch = std::process::Command::new("rsync")
                .args(["-t", "--timeout=60", "-e", &ssh, &format!("{}:{}/files/{}", host, folder, name)])
                .arg(&tmp)
                .stdin(std::process::Stdio::null())
                .stdout(std::process::Stdio::null())
                .stderr(std::process::Stdio::null())
                .spawn()
                .ok()?;
            let t0 = Instant::now();
            let status = loop {
                match ch.try_wait().ok()? {
                    Some(s) => break s,
                    None if t0.elapsed() > Duration::from_secs(300) => {
                        let _ = ch.kill();
                        let _ = ch.wait();
                        return None;
                    }
                    None => std::thread::sleep(Duration::from_millis(100)),
                }
            };
            if !status.success() || std::fs::metadata(&tmp).ok()?.len() != want {
                return None;
            }
            std::fs::rename(&tmp, &cp).ok()
        })();
        if fetched.is_none() {
            let _ = std::fs::remove_file(&tmp);
            return None;
        }
        Self::report_cache_trim(&cp);
        Some(cp)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn levels() {
        assert_eq!(pretty_level("to1-into-the-city"), "Into The City");
        assert_eq!(pretty_level("to2-o'brien-2nd"), "O'Brien 2Nd");
    }

    #[test]
    fn rules() {
        assert_eq!(needed(5), 9);
        assert_eq!(needed(25), 25);
        assert_eq!(needed(38), 36);
        assert_eq!(week_day(0), (1, 1));
        assert_eq!(week_day(90), (12, 7));
        assert_eq!(pid_of("s1"), pid_of("s1"));
        assert_eq!(pid_of("abc").len(), 10);
    }

    #[test]
    fn mute_lines() {
        assert_eq!(mute_parse("# comment"), None);
        assert_eq!(mute_parse("   "), None);
        let (k, u, n) = mute_parse("s:76561198000000001 1700000000  # Bob - admin site 2026-11-24 12:00 by Mai | reason: spam").unwrap();
        assert_eq!((k.as_str(), u, n.as_str()), ("s:76561198000000001", 1700000000.0, "Bob - admin site 2026-11-24 12:00 by Mai | reason: spam"));
        let (k, u, _) = mute_parse("76561198000000001").unwrap();
        assert_eq!((k.as_str(), u), ("s:76561198000000001", 0.0));
        let (_, u, _) = mute_parse("t:abc xyz").unwrap();
        assert_eq!(u, 0.0);
        assert!(mute_live(0.0, 5.0) && mute_live(6.0, 5.0) && !mute_live(5.0, 5.0));
    }

    #[test]
    fn mute_key_hash() {
        // sha256("abc") = ba7816bf8f01cfea414140de5dae2223...
        assert_eq!(token_key("abc"), "t:ba7816bf8f01cfea");
        assert_eq!(token_key("").len(), 18);
    }

    #[test]
    fn percent_g() {
        assert_eq!(fmt_g(30.0), "30");
        assert_eq!(fmt_g(1.5), "1.5");
        assert_eq!(fmt_g(0.25), "0.25");
        assert_eq!(fmt_g(123456.0), "123456");
        assert_eq!(fmt_g(1234567.0), "1.23457e+06");
        assert_eq!(fmt_g(0.00001), "1e-05");
    }
}
