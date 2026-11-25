//! Host upload capacity history (port of hostcap.py): what upload a player has PROVEN as a host and whether a match lagged while
//! that player hosted. The host pick / re-pick consult it. One sqlite table (host_cap), plus oneway alarms (table oneway).
//!
//! Deviation: Python raises sqlite errors up to the caller; here they come back as `Err(String)` (the daemon logs "HOSTCAP ERR").
use parking_lot::Mutex;
use rusqlite::types::Value as Sql;
use rusqlite::Connection;
use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::LazyLock;

use crate::clock;

#[path = "hostcap/st.rs"]
mod st;
pub use st::selftest;

pub type Res<T> = Result<T, String>;

pub const MIN_PLAYERS: i64 = 3;
pub const SCHEMA: i64 = 2;

pub struct Cfg {
    pub mode: i64,
    pub need_kb_per_extra: i64,
    pub window_ms: i64,
    pub lag_loss10: i64,
    pub lag_days: i64,
    pub lag_window_ms: i64,
    pub need_kb_per_pair: i64,
}

pub fn cfg() -> &'static Cfg {
    static C: LazyLock<Cfg> = LazyLock::new(|| Cfg {
        mode: crate::env_parse("T2R_HOST_CAP", 2i64),
        need_kb_per_extra: crate::env_parse("T2R_HOST_CAP_KB_PER_EXTRA", 225i64),
        window_ms: crate::env_parse("T2R_HOST_CAP_WINDOW_MS", 40i64),
        lag_loss10: crate::env_parse("T2R_HOST_CAP_LOSS10", 30i64),
        lag_days: crate::env_parse("T2R_HOST_CAP_LAG_DAYS", 30i64),
        lag_window_ms: crate::env_parse("T2R_HOST_CAP_LAG_WINDOW_MS", 150i64),
        need_kb_per_pair: crate::env_parse("T2R_HOST_CAP_KB_PER_PAIR", 46i64),
    });
    &C
}

/// hostcap.MODE
pub fn mode() -> i64 {
    cfg().mode
}

/// py: hostcap.py:48 need_kb
pub fn need_kb(n_players: i64) -> i64 {
    cfg().need_kb_per_extra * std::cmp::max(0, n_players - 1)
}

/// py: hostcap.py:53 need_kb_relay
pub fn need_kb_relay(n_players: i64) -> i64 {
    let extra = std::cmp::max(0, n_players - 1);
    std::cmp::max(need_kb(n_players), cfg().need_kb_per_pair * extra * extra)
}

#[derive(Debug, Clone, PartialEq)]
pub struct CapTrailer {
    pub avg_kb: i64,
    pub peak_kb: i64,
    pub loss10: i64,
    pub n: i64,
}

/// py: hostcap.py:59 parse_cap  (the optional trailer of an op 0xCA payload)
pub fn parse_cap(pl: &[u8]) -> Option<CapTrailer> {
    if pl.len() < 72 + 7 || pl[72] != 1 {
        return None;
    }
    Some(CapTrailer { avg_kb: crate::util::le(pl, 73, 2) as i64, peak_kb: crate::util::le(pl, 75, 2) as i64, loss10: pl[77] as i64, n: pl[78] as i64 })
}

#[derive(Debug, Clone, PartialEq)]
pub struct OneWay {
    pub quiet_s: i64,
    pub rx_pkts: i64,
    pub tx_kb: i64,
    pub in_match_s: i64,
}

/// py: hostcap.py:67 parse_oneway
pub fn parse_oneway(pl: &[u8]) -> Option<OneWay> {
    if pl.len() < 72 + 7 + 7 || pl[71] & 2 == 0 || pl[72] != 1 || pl[79] != 1 {
        return None;
    }
    Some(OneWay { quiet_s: pl[80] as i64, rx_pkts: crate::util::le(pl, 81, 2) as i64, tx_kb: crate::util::le(pl, 83, 2) as i64, in_match_s: pl[85] as i64 })
}

#[derive(Debug, Clone, Default, PartialEq)]
pub struct CapRow {
    pub hosted: i64,
    pub best_kb: i64,
    pub best_n: i64,
    pub last_kb: i64,
    pub lag_n: i64,
    pub lag_t: f64,
    pub ceil_kb: i64,
    pub last_t: f64,
    pub lag_mid: Option<String>,
}

type ConnFn = Box<dyn Fn() -> Res<Connection> + Send + Sync>;
type NowFn = Box<dyn Fn() -> f64 + Send + Sync>;

/// py: hostcap.py:77 HostCap  (connect() -> sqlite connection, the daemon's db())
pub struct HostCap {
    connect: ConnFn,
    now: NowFn,
    schema_ok: AtomicBool,
    streak: Mutex<HashMap<(String, String), i64>>,
    mlag: Mutex<HashMap<String, String>>,
}

fn e2s(e: rusqlite::Error) -> String {
    e.to_string()
}

impl HostCap {
    pub fn new(connect: ConnFn) -> Self {
        Self::with_now(connect, Box::new(clock::now))
    }

    pub fn with_now(connect: ConnFn, now: NowFn) -> Self {
        HostCap { connect, now, schema_ok: AtomicBool::new(false), streak: Mutex::new(HashMap::new()), mlag: Mutex::new(HashMap::new()) }
    }

    fn now(&self) -> f64 {
        (self.now)()
    }

    /// py: hostcap.py:90 _ensure
    fn ensure(&self, c: &Connection) -> rusqlite::Result<()> {
        c.execute_batch(
            "CREATE TABLE IF NOT EXISTS host_cap(token TEXT PRIMARY KEY, hosted INT DEFAULT 0, best_kb INT DEFAULT 0,
            best_n INT DEFAULT 0, last_kb INT DEFAULT 0, lag_n INT DEFAULT 0, lag_t REAL DEFAULT 0, ceil_kb INT DEFAULT 0, last_t REAL DEFAULT 0)",
        )?;
        let mut has = false;
        {
            let mut st = c.prepare("PRAGMA table_info(host_cap)")?;
            let rows = st.query_map([], |r| r.get::<_, String>(1))?;
            for n in rows {
                if n? == "lag_mid" {
                    has = true;
                }
            }
        }
        if !has {
            c.execute_batch("ALTER TABLE host_cap ADD COLUMN lag_mid TEXT")?;
        }
        self.schema_ok.store(true, Ordering::Relaxed);
        Ok(())
    }

    /// py: hostcap.py:99 _q
    fn q<T>(&self, f: impl Fn(&Connection) -> rusqlite::Result<T>) -> Res<T> {
        let c = (self.connect)()?;
        if !self.schema_ok.load(Ordering::Relaxed) {
            self.ensure(&c).map_err(e2s)?;
        }
        match f(&c) {
            Ok(v) => Ok(v),
            Err(e) => {
                let s = e.to_string();
                if !s.contains("no such table") && !s.contains("no such column") {
                    return Err(s);
                }
                self.ensure(&c).map_err(e2s)?;
                f(&c).map_err(e2s)
            }
        }
    }

    /// py: hostcap.py:113 get
    pub fn get(&self, token: &str) -> Res<Option<CapRow>> {
        self.q(|c| {
            let mut st = c.prepare("SELECT hosted,best_kb,best_n,last_kb,lag_n,lag_t,ceil_kb,last_t,lag_mid FROM host_cap WHERE token=?")?;
            let mut rows = st.query([token])?;
            match rows.next()? {
                None => Ok(None),
                Some(r) => Ok(Some(CapRow {
                    hosted: r.get::<_, Option<i64>>(0)?.unwrap_or(0),
                    best_kb: r.get::<_, Option<i64>>(1)?.unwrap_or(0),
                    best_n: r.get::<_, Option<i64>>(2)?.unwrap_or(0),
                    last_kb: r.get::<_, Option<i64>>(3)?.unwrap_or(0),
                    lag_n: r.get::<_, Option<i64>>(4)?.unwrap_or(0),
                    lag_t: r.get::<_, Option<f64>>(5)?.unwrap_or(0.0),
                    ceil_kb: r.get::<_, Option<i64>>(6)?.unwrap_or(0),
                    last_t: r.get::<_, Option<f64>>(7)?.unwrap_or(0.0),
                    lag_mid: r.get::<_, Option<String>>(8)?,
                })),
            }
        })
    }

    /// py: hostcap.py:119 lagged_in
    pub fn lagged_in(&self, mid: Option<&str>, token: &str) -> Res<bool> {
        let Some(mid) = mid else { return Ok(false) };
        if self.mlag.lock().get(mid).map(|x| x.as_str()) == Some(token) {
            return Ok(true);
        }
        let r = self.get(token)?;
        Ok(r.is_some_and(|r| r.lag_mid.as_deref() == Some(mid)))
    }

    /// py: hostcap.py:127 _upd
    fn upd(&self, token: &str, kw: &[(&str, Sql)]) -> Res<()> {
        let sql = format!("UPDATE host_cap SET {} WHERE token=?", kw.iter().map(|(k, _)| format!("{}=?", k)).collect::<Vec<_>>().join(","));
        self.q(|c| {
            c.execute("INSERT OR IGNORE INTO host_cap(token) VALUES(?)", [token])?;
            let mut params: Vec<Sql> = kw.iter().map(|(_, v)| v.clone()).collect();
            params.push(Sql::Text(token.to_string()));
            c.execute(&sql, rusqlite::params_from_iter(params.iter()))?;
            Ok(())
        })
    }

    /// py: hostcap.py:137 oneway_note  -> (alarms, as host)
    pub fn oneway_note(&self, token: &str, mid: Option<&str>, host: bool) -> Res<(i64, i64)> {
        let now = self.now();
        self.q(|c| {
            c.execute("CREATE TABLE IF NOT EXISTS oneway(token TEXT PRIMARY KEY, n INT DEFAULT 0, host_n INT DEFAULT 0, last_t REAL DEFAULT 0, last_mid TEXT)", [])?;
            c.execute("INSERT OR IGNORE INTO oneway(token) VALUES(?)", [token])?;
            c.execute(
                "UPDATE oneway SET n=n+1, host_n=host_n+?, last_t=?, last_mid=? WHERE token=?",
                rusqlite::params![if host { 1i64 } else { 0 }, now, mid, token],
            )?;
            c.query_row("SELECT n, host_n FROM oneway WHERE token=?", [token], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?)))
        })
    }

    /// py: hostcap.py:147 oneway_get  (alarms, as host) in the last `days` days, or (0, 0)
    pub fn oneway_get(&self, token: &str, days: i64) -> Res<(i64, i64)> {
        let now = self.now();
        self.q(|c| {
            let r = c.query_row("SELECT n, host_n, last_t FROM oneway WHERE token=?", [token], |r| {
                Ok((r.get::<_, Option<i64>>(0)?.unwrap_or(0), r.get::<_, Option<i64>>(1)?.unwrap_or(0), r.get::<_, Option<f64>>(2)?.unwrap_or(0.0)))
            });
            match r {
                Ok((n, hn, t)) if now - t <= (days * 86400) as f64 => Ok((n, hn)),
                _ => Ok((0, 0)),
            }
        })
    }

    /// py: hostcap.py:160 host_report  (the match HOST's own report: its real tx)
    pub fn host_report(&self, token: &str, n_players: i64, avg_kb: i64, _peak_kb: i64, mid: Option<&str>) -> Res<()> {
        if n_players < MIN_PLAYERS || avg_kb <= 0 {
            return Ok(());
        }
        let r = self.get(token)?.unwrap_or_default();
        let mut kw: Vec<(&str, Sql)> = vec![("last_kb", Sql::Integer(avg_kb)), ("last_t", Sql::Real(self.now())), ("hosted", Sql::Integer(r.hosted + 1))];
        let lagged = self.lagged_in(mid, token)?;
        if !lagged && avg_kb > r.best_kb {
            kw.push(("best_kb", Sql::Integer(avg_kb)));
            kw.push(("best_n", Sql::Integer(n_players)));
        }
        self.upd(token, &kw)
    }

    /// py: hostcap.py:172 client_report  (2 reports in a row >= LAG_LOSS10 = one lag record for the host, once per match)
    pub fn client_report(&self, mid: &str, host_token: &str, token: &str, loss10: i64) -> Res<bool> {
        let k = (mid.to_string(), token.to_string());
        if loss10 < cfg().lag_loss10 {
            self.streak.lock().insert(k, 0);
            return Ok(false);
        }
        let n = {
            let mut s = self.streak.lock();
            let e = s.entry(k).or_insert(0);
            *e += 1;
            *e
        };
        if n < 2 || self.lagged_in(Some(mid), host_token)? {
            return Ok(false);
        }
        self.note_lag(host_token, Some(mid))
    }

    /// py: hostcap.py:181 note_lag
    pub fn note_lag(&self, host_token: &str, mid: Option<&str>) -> Res<bool> {
        if let Some(m) = mid {
            self.mlag.lock().insert(m.to_string(), host_token.to_string());
        }
        let r = self.get(host_token)?.unwrap_or_default();
        let mut kw: Vec<(&str, Sql)> = vec![("lag_n", Sql::Integer(r.lag_n + 1)), ("lag_t", Sql::Real(self.now()))];
        if let Some(m) = mid {
            kw.push(("lag_mid", Sql::Text(m.to_string())));
        }
        let last = r.last_kb;
        if last > 0 {
            kw.push(("ceil_kb", Sql::Integer(if r.ceil_kb != 0 { std::cmp::min(last, r.ceil_kb) } else { last })));
        }
        self.upd(host_token, &kw)?;
        Ok(true)
    }

    /// py: hostcap.py:193 forget_matches
    pub fn forget_matches(&self, live_mids: &std::collections::HashSet<String>) {
        self.streak.lock().retain(|k, _| live_mids.contains(&k.0));
        self.mlag.lock().retain(|k, _| live_mids.contains(k));
    }

    /// py: hostcap.py:198 proven_for
    pub fn proven_for(&self, token: &str, n_players: i64) -> Res<bool> {
        let Some(r) = self.get(token)? else { return Ok(false) };
        if r.best_kb == 0 || r.best_kb < need_kb_relay(n_players) {
            return Ok(false);
        }
        Ok(!(r.lag_n != 0 && self.now() - r.lag_t <= (cfg().lag_days * 86400) as f64))
    }

    /// py: hostcap.py:205 verdict  -> (rank, text): 0 proven enough, 1 unknown / unproven, 2 lagged as host
    pub fn verdict(&self, token: &str, n_players: i64) -> Res<(i64, String)> {
        let r = self.get(token)?;
        let need = need_kb(n_players);
        let Some(r) = r else { return Ok((1, "unknown".into())) };
        let recent_lag = r.lag_n != 0 && self.now() - r.lag_t <= (cfg().lag_days * 86400) as f64;
        if recent_lag {
            let ceil = r.ceil_kb;
            if ceil == 0 || need as f64 > ceil as f64 * 0.9 {
                if r.best_kb >= need {
                    return Ok((1, format!("proven {} KB/s (need {}) but lagged x{} at ~{} KB/s", r.best_kb, need, r.lag_n, ceil)));
                }
                return Ok((2, format!("lagged x{} at ~{} KB/s (need {})", r.lag_n, ceil, need)));
            }
        }
        if r.best_kb >= need {
            return Ok((0, format!("proven {} KB/s (need {})", r.best_kb, need)));
        }
        if r.best_kb != 0 {
            return Ok((1, format!("unproven: best {} KB/s at {} players (need {})", r.best_kb, r.best_n, need)));
        }
        Ok((1, "unknown".into()))
    }

    /// Test hook: the in-memory streak / mlag maps as JSON (keys sorted).
    #[allow(dead_code)]
    pub fn dump_state(&self) -> serde_json::Value {
        use serde_json::Value;
        let s = self.streak.lock();
        let mut sv: Vec<(&(String, String), &i64)> = s.iter().collect();
        sv.sort();
        let m = self.mlag.lock();
        let mut mv: Vec<(&String, &String)> = m.iter().collect();
        mv.sort();
        serde_json::json!({
            "mlag": mv.iter().map(|(k, v)| Value::Array(vec![Value::from(k.as_str()), Value::from(v.as_str())])).collect::<Vec<_>>(),
            "streak": sv.iter().map(|(k, v)| Value::Array(vec![Value::from(k.0.as_str()), Value::from(k.1.as_str()), Value::from(**v)])).collect::<Vec<_>>(),
            "schema_ok": self.schema_ok.load(Ordering::Relaxed),
        })
    }
}

/// py: hostcap.py:228 pick. cands: [(player, key)] with key = hostmig::host_key. Returns (chosen, lines).
pub fn pick(
    cands: &[(String, (i64, i64))],
    n_players: i64,
    verdict: &dyn Fn(&str, i64) -> Res<(i64, String)>,
    window_ms: Option<i64>,
    label: &dyn Fn(&str) -> String,
) -> Res<(Option<String>, Vec<String>)> {
    let c = cfg();
    let w = window_ms.unwrap_or(c.window_ms);
    let mut ranked: Vec<&(String, (i64, i64))> = cands.iter().collect();
    ranked.sort_by_key(|x| x.1);
    if ranked.is_empty() {
        return Ok((None, Vec::new()));
    }
    let best = ranked[0];
    let wide: Vec<&(String, (i64, i64))> = ranked.iter().copied().filter(|x| x.1 .0 - best.1 .0 <= std::cmp::max(w, c.lag_window_ms)).collect();
    let mut vs: HashMap<&str, (i64, String)> = HashMap::new();
    for x in &wide {
        vs.insert(x.0.as_str(), verdict(&x.0, n_players)?);
    }
    let mut near: Vec<&(String, (i64, i64))> = wide.iter().copied().filter(|x| x.1 .0 - best.1 .0 <= w).collect();
    let Some(mut chosen) = minby(&near, &vs) else { return Err("min() arg is an empty sequence".into()) };
    if vs[chosen.0.as_str()].0 >= 2 {
        let ok: Vec<&(String, (i64, i64))> = wide.iter().copied().filter(|x| vs[x.0.as_str()].0 < 2).collect();
        if !ok.is_empty() {
            chosen = minby(&ok, &vs).unwrap_or(chosen);
            let extra: Vec<&(String, (i64, i64))> = ok.iter().copied().filter(|x| !near.contains(x)).collect();
            near.extend(extra);
        }
    }
    let lines: Vec<String> = near.iter().map(|x| format!("{} worst {} total {}: {}", label(&x.0), x.1 .0, x.1 .1, vs[x.0.as_str()].1)).collect();
    Ok((Some(chosen.0.clone()), lines))
}

type Cand = (String, (i64, i64));

/// min(v, key=(rank, key)): the first minimal element.
fn minby<'a>(v: &[&'a Cand], vs: &HashMap<&str, (i64, String)>) -> Option<&'a Cand> {
    let mut it = v.iter().copied();
    let mut b = it.next()?;
    let mut bk = (vs[b.0.as_str()].0, b.1);
    for x in it {
        let k = (vs[x.0.as_str()].0, x.1);
        if k < bk {
            bk = k;
            b = x;
        }
    }
    Some(b)
}
