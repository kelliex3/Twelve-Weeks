//! The daemon's sqlite access (daemon.py `_dblock`, `db()`, `dbc()`, `db_read`, `db_backup`, `db_guard`, `db_maintenance`).
//!
//! Differences from Python, same rows and results:
//! - ONE long-lived write connection behind `DBLOCK` instead of a new connection per `dbc()`; read-only connections are pooled
//!   (as `_ro_pool`) and also long-lived.
//! - `T2R_DB_WAL=1` switches the file to WAL. Then the backup copies from its own read snapshot WITHOUT `DBLOCK`: requests and
//!   writers never wait for it. Without WAL the backup holds `DBLOCK` for the page copy like Python (a rollback-journal reader
//!   would block the writer's commit past the busy timeout otherwise).
use parking_lot::{Mutex, MutexGuard};
use rusqlite::{Connection, OpenFlags};
use std::cell::Cell;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::LazyLock;
use std::time::{Duration, Instant};

use crate::{dlog, env_parse, env_str};

pub struct Cfg {
    pub path: String,
    pub bak_path: String,
    pub backup_s: f64,
    pub maint_s: f64,
    pub ro_reads: bool,
    pub wal: bool,
}

// py: daemon.py:151 DB_PATH (the daemon dir default: <script dir>/t2r.db; the Rust binary uses the working directory)
pub static CFG: LazyLock<Cfg> = LazyLock::new(|| {
    let path = match std::env::var("T2R_DB") {
        Ok(p) if !p.is_empty() => p,
        _ => std::env::current_dir().unwrap_or_default().join("t2r.db").to_string_lossy().into_owned(),
    };
    Cfg {
        bak_path: format!("{}.bak", path),
        path,
        backup_s: env_parse("T2R_DB_BACKUP_S", 1800.0),
        maint_s: env_parse("T2R_DB_MAINT_S", 24.0 * 3600.0),
        ro_reads: env_str("T2R_DB_RO_READS", "1") != "0",
        wal: env_str("T2R_DB_WAL", "0") == "1",
    }
});

#[derive(Debug)]
pub enum DbErr {
    /// RequestAbandoned: the request's deadline passed while waiting for DBLOCK
    Abandoned,
    Sql(rusqlite::Error),
}

impl From<rusqlite::Error> for DbErr {
    fn from(e: rusqlite::Error) -> DbErr {
        DbErr::Sql(e)
    }
}

impl std::fmt::Display for DbErr {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DbErr::Abandoned => write!(f, "RequestAbandoned()"),
            DbErr::Sql(e) => write!(f, "{}", e),
        }
    }
}

pub type DbResult<T> = Result<T, DbErr>;

thread_local! {
    /// py: _req.deadline (monotonic instant after which this thread's request is not worth serving; None = no deadline)
    static DEADLINE: Cell<Option<Instant>> = const { Cell::new(None) };
    /// 11-27: this thread holds DBLOCK now (a second lock() on the same thread would wait for itself forever: it is refused instead)
    static HELD: Cell<bool> = const { Cell::new(false) };
}

pub fn set_deadline(d: Option<Instant>) {
    DEADLINE.with(|c| c.set(d));
}

pub fn deadline() -> Option<Instant> {
    DEADLINE.with(|c| c.get())
}

/// py: ABANDONED (requests dropped at a _dblock wait)
pub static ABANDONED: AtomicU64 = AtomicU64::new(0);
/// py: RO_READS [lock-free reads, fallbacks]
pub static RO_READS: [AtomicU64; 2] = [AtomicU64::new(0), AtomicU64::new(0)];

struct Writer {
    conn: Option<Connection>,
    ident: Option<(u64, bool)>,
}

static DBLOCK: LazyLock<Mutex<Writer>> = LazyLock::new(|| Mutex::new(Writer { conn: None, ident: None }));

fn db_ident(path: &str) -> Option<(u64, bool)> {
    let md = std::fs::metadata(path).ok()?;
    #[cfg(unix)]
    let ino = std::os::unix::fs::MetadataExt::ino(&md);
    #[cfg(not(unix))]
    let ino = 0u64;
    Some((ino, md.len() > 0))
}

pub fn open_rw(path: &str) -> rusqlite::Result<Connection> {
    let c = Connection::open(path)?;
    c.busy_timeout(Duration::from_secs(10))?;
    c.set_prepared_statement_cache_capacity(128); // (Python sqlite3: cached_statements=128 per connection)
    c.execute_batch("PRAGMA synchronous=FULL")?;
    if CFG.wal {
        let _: String = c.query_row("PRAGMA journal_mode=WAL", [], |r| r.get(0))?;
    }
    schema(&c)?;
    Ok(c)
}

// py: daemon.py:628 db() - the base schema (players, hwids + indexes); the other tables are created by their modules
fn schema(c: &Connection) -> rusqlite::Result<()> {
    c.execute_batch(
        "CREATE TABLE IF NOT EXISTS players(
        token TEXT PRIMARY KEY, name TEXT, loadout BLOB,
        kills INT DEFAULT 0, deaths INT DEFAULT 0, wins INT DEFAULT 0,
        matches INT DEFAULT 0, steamid INTEGER DEFAULT 0, last_seen REAL)",
    )?;
    let cols: Vec<String> = {
        let mut s = c.prepare("PRAGMA table_info(players)")?;
        let v = s.query_map([], |r| r.get::<_, String>(1))?.filter_map(|x| x.ok()).collect();
        v
    };
    if !cols.iter().any(|x| x == "steamid") {
        c.execute_batch("ALTER TABLE players ADD COLUMN steamid INTEGER DEFAULT 0")?;
    }
    c.execute_batch(
        "CREATE TABLE IF NOT EXISTS hwids(
        hash TEXT, token TEXT, steamid INTEGER DEFAULT 0, name TEXT, first_seen REAL, last_seen REAL, PRIMARY KEY(hash, token))",
    )?;
    if !cols.iter().any(|x| x == "parts_total") {
        c.execute_batch("ALTER TABLE players ADD COLUMN parts_total INTEGER DEFAULT 0")?;
        c.execute_batch("ALTER TABLE players ADD COLUMN parts_n INTEGER DEFAULT 0")?;
    }
    if !cols.iter().any(|x| x == "draws") {
        c.execute_batch("ALTER TABLE players ADD COLUMN draws INTEGER DEFAULT 0")?;
    }
    c.execute_batch(
        "CREATE INDEX IF NOT EXISTS players_steamid ON players(steamid);
         CREATE INDEX IF NOT EXISTS hwids_token ON hwids(token);
         CREATE INDEX IF NOT EXISTS hwids_steamid ON hwids(steamid);",
    )?;
    Ok(())
}

/// `with _dblock:` (the lock alone, for code that opens nothing itself: the clan worker calls). Honours the thread's deadline.
pub struct DbLock<'a> {
    g: MutexGuard<'a, Writer>,
    t0: Instant,
    at: &'static std::panic::Location<'static>,
}

/// Diagnostic: T2R_DB_HOLD_LOG_MS = N logs every DB lock wait or hold of N ms or more with its call site (DB-WAIT / DB-HOLD); 0 = off.
/// 11-27: on by default at 2000 ms (the title-page loops: a lock held that long makes the boot reply miss the client's 15 s window).
static HOLD_LOG_MS: LazyLock<u64> = LazyLock::new(|| env_parse("T2R_DB_HOLD_LOG_MS", 2000u64));

impl Drop for DbLock<'_> {
    fn drop(&mut self) {
        HELD.with(|h| h.set(false));
        if *HOLD_LOG_MS > 0 {
            let ms = self.t0.elapsed().as_millis() as u64;
            if ms >= *HOLD_LOG_MS {
                dlog!("DB-HOLD", format!("{} ms", ms), format!("{}:{}", self.at.file(), self.at.line()), std::thread::current().name().unwrap_or("-"));
            }
        }
    }
}

#[track_caller]
fn locked(g: MutexGuard<'static, Writer>, w0: Instant) -> DbLock<'static> {
    let at = std::panic::Location::caller();
    if *HOLD_LOG_MS > 0 {
        let ms = w0.elapsed().as_millis() as u64;
        if ms >= *HOLD_LOG_MS {
            dlog!("DB-WAIT", format!("{} ms", ms), format!("{}:{}", at.file(), at.line()), std::thread::current().name().unwrap_or("-"));
        }
    }
    HELD.with(|h| h.set(true));
    DbLock { g, t0: Instant::now(), at }
}

impl<'a> DbLock<'a> {
    /// The write connection (opened lazily; reopened when t2r.db was replaced).
    pub fn conn(&mut self) -> rusqlite::Result<&Connection> {
        let id = db_ident(&CFG.path);
        let stale = self.g.conn.is_none() || (id.is_some() && self.g.ident.is_some() && id != self.g.ident);
        if stale {
            self.g.conn = None;
            let c = open_rw(&CFG.path)?;
            self.g.conn = Some(c);
            self.g.ident = db_ident(&CFG.path);
        }
        match self.g.conn.as_ref() {
            Some(c) => Ok(c),
            None => Err(rusqlite::Error::InvalidQuery),
        }
    }
    /// Close the write connection (before t2r.db is replaced).
    pub fn close(&mut self) {
        self.g.conn = None;
        self.g.ident = None;
    }
}

#[track_caller]
pub fn lock() -> DbResult<DbLock<'static>> {
    if HELD.with(|h| h.get()) {
        // (DBLOCK is not re-entrant: this thread already holds it, e.g. a db_read fallback inside a write. Waiting would never end.)
        let at = std::panic::Location::caller();
        dlog!("DB-REENTRY refused", format!("{}:{}", at.file(), at.line()), std::thread::current().name().unwrap_or("-"));
        ABANDONED.fetch_add(1, Ordering::Relaxed);
        return Err(DbErr::Abandoned);
    }
    let w0 = Instant::now();
    match deadline() {
        None => Ok(locked(DBLOCK.lock(), w0)),
        Some(dl) => {
            let now = Instant::now();
            if dl <= now {
                ABANDONED.fetch_add(1, Ordering::Relaxed);
                return Err(DbErr::Abandoned);
            }
            match DBLOCK.try_lock_for(dl - now) {
                Some(g) => Ok(locked(g, w0)),
                None => {
                    ABANDONED.fetch_add(1, Ordering::Relaxed);
                    Err(DbErr::Abandoned)
                }
            }
        }
    }
}

/// `with _dblock, dbc() as c:` - commit on Ok, roll back on Err.
#[track_caller]
pub fn with_write<R>(f: impl FnOnce(&Connection) -> DbResult<R>) -> DbResult<R> {
    let mut l = lock()?;
    let c = l.conn()?;
    // 11-27: IMMEDIATE, not a deferred BEGIN. A deferred transaction starts as a reader at its first SELECT; its first write then has to
    // upgrade, and when another connection (the separate HOSTCAP writes, t2r.db tools) holds or has just committed a write, SQLite fails that
    // upgrade AT ONCE with "database is locked" (no busy wait: waiting could deadlock). Live 11-27: ~950 such errors in 80 min (clan
    // snapshots, career stats, ELO, IDENT, clan match stats lost; 8 snapshot failures inside one second while DBLOCK serialises them).
    // Python never had it: its sqlite3 module ran the SELECTs outside a transaction and began at the first write. IMMEDIATE takes the write
    // lock first, through the busy timeout like any plain write.
    c.execute_batch("BEGIN IMMEDIATE")?;
    match f(c) {
        Ok(v) => match c.execute_batch("COMMIT") {
            Ok(()) => Ok(v),
            Err(e) => {
                let _ = c.execute_batch("ROLLBACK");
                Err(DbErr::Sql(e))
            }
        },
        Err(e) => {
            let _ = c.execute_batch("ROLLBACK");
            Err(e)
        }
    }
}

struct Ro {
    conn: Connection,
    ident: (u64, bool),
}

static RO_POOL: LazyLock<Mutex<Vec<Ro>>> = LazyLock::new(|| Mutex::new(Vec::new()));
const RO_POOL_MAX: usize = 8;

fn ro_get() -> rusqlite::Result<Ro> {
    let ident = match db_ident(&CFG.path) {
        Some(i) if i.1 => i,
        _ => return Err(rusqlite::Error::InvalidPath(CFG.path.clone().into())),
    };
    loop {
        let e = RO_POOL.lock().pop();
        match e {
            None => break,
            Some(e) if e.ident == ident => return Ok(e),
            Some(_) => continue,
        }
    }
    let conn = Connection::open_with_flags(&CFG.path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX | OpenFlags::SQLITE_OPEN_URI)?;
    conn.busy_timeout(Duration::from_secs(10))?;
    conn.set_prepared_statement_cache_capacity(128);
    Ok(Ro { conn, ident })
}

fn ro_put(e: Ro) {
    let mut p = RO_POOL.lock();
    if p.len() < RO_POOL_MAX {
        p.push(e);
    }
}

pub fn ro_close_all() {
    RO_POOL.lock().clear();
}

/// py: db_read(fn) - a pure read on a pooled read-only connection without DBLOCK; any sqlite error falls back to the locked path.
#[track_caller]
pub fn db_read<R>(f: impl Fn(&Connection) -> rusqlite::Result<R>) -> DbResult<R> {
    if CFG.ro_reads {
        if let Ok(e) = ro_get() {
            match f(&e.conn) {
                Ok(v) => {
                    RO_READS[0].fetch_add(1, Ordering::Relaxed);
                    ro_put(e);
                    return Ok(v);
                }
                Err(_) => {
                    RO_READS[1].fetch_add(1, Ordering::Relaxed);
                }
            }
        } else {
            RO_READS[1].fetch_add(1, Ordering::Relaxed);
        }
    }
    with_write(|c| Ok(f(c)?))
}

// py: daemon.py:791 _db_ok
fn db_ok(path: &str, quick: bool) -> bool {
    match std::fs::metadata(path) {
        Ok(m) if m.len() > 0 => {}
        _ => return false,
    }
    let Ok(c) = Connection::open_with_flags(path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX) else { return false };
    let q = if quick { "PRAGMA quick_check" } else { "PRAGMA integrity_check" };
    matches!(c.query_row(q, [], |r| r.get::<_, String>(0)), Ok(s) if s == "ok")
}

// py: daemon.py:803 db_guard
pub fn db_guard() -> String {
    let path = &CFG.path;
    if !std::path::Path::new(path).exists() {
        return "no db yet".into();
    }
    // PRAGMA quick_check of a large t2r.db can take tens of seconds on a busy host, and TCP only listens after it. A planned restart
    // (the deploy script) leaves <db>.skipcheck: one start without the check.
    // A crash restart has no such file and checks as before.
    let skip = format!("{}.skipcheck", path);
    if std::path::Path::new(&skip).exists() {
        let _ = std::fs::remove_file(&skip);
        return "skipped (planned restart, <db>.skipcheck)".into();
    }
    if db_ok(path, true) {
        return "ok".into();
    }
    if db_ok(&CFG.bak_path, true) {
        ro_close_all();
        if let Ok(mut l) = lock() {
            l.close();
        }
        let bad = format!("{}.corrupt-{}", path, crate::clock::now() as i64);
        if std::fs::rename(path, &bad).is_err() {
            return "CORRUPT and no usable backup".into();
        }
        // WAL: the damaged file's -wal / -shm go with it (a stale -wal would be replayed onto the restored copy)
        for sfx in ["-wal", "-shm"] {
            let side = format!("{}{}", path, sfx);
            if std::path::Path::new(&side).exists() {
                let _ = std::fs::rename(&side, format!("{}{}", bad, sfx));
            }
        }
        let rest = format!("{}.restore", path);
        if std::fs::copy(&CFG.bak_path, &rest).is_ok() {
            if let Ok(f) = std::fs::OpenOptions::new().read(true).write(true).open(&rest) {
                let _ = f.sync_all();
            }
            let _ = std::fs::rename(&rest, path);
        }
        let base = std::path::Path::new(&bad).file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default();
        return format!("CORRUPT - restored from the backup (damaged file kept as {})", base);
    }
    "CORRUPT and no usable backup".into()
}

fn backup_copy(src: &Connection, tmp: &str) -> rusqlite::Result<()> {
    let mut dst = Connection::open(tmp)?;
    let b = rusqlite::backup::Backup::new(src, &mut dst)?;
    // one step (-1): a WAL reader keeps its snapshot for the whole copy, writers go on committing to the WAL
    b.step(-1)?;
    drop(b);
    // a copy of a WAL file is a WAL file again: the backup is kept as a plain rollback-journal file (like Python's), so that opening it
    // (db_ok, a restore) needs no -wal / -shm next to it
    let _: String = dst.query_row("PRAGMA journal_mode=DELETE", [], |r| r.get(0))?;
    Ok(())
}

// py: daemon.py:821 db_backup
pub fn db_backup() -> bool {
    let tmp = format!("{}.tmp", CFG.bak_path);
    match std::fs::metadata(&CFG.path) {
        Ok(m) if m.len() > 0 => {}
        _ => return false,
    }
    let r = if CFG.wal {
        // own read connection, no DBLOCK
        Connection::open_with_flags(&CFG.path, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX)
            .and_then(|src| {
                src.busy_timeout(Duration::from_secs(10))?;
                backup_copy(&src, &tmp)
            })
            .map_err(DbErr::Sql)
    } else {
        lock().and_then(|_l| {
            let src = Connection::open(&CFG.path)?;
            src.busy_timeout(Duration::from_secs(10))?;
            backup_copy(&src, &tmp).map_err(DbErr::Sql)
        })
    };
    if let Err(e) = r {
        dlog!("DB backup failed", format!("{:?}", e));
        let _ = std::fs::remove_file(&tmp);
        return false;
    }
    if !db_ok(&tmp, true) {
        let _ = std::fs::remove_file(&tmp);
        return false;
    }
    if let Ok(f) = std::fs::OpenOptions::new().read(true).write(true).open(&tmp) {
        let _ = f.sync_all();
    }
    if let Err(e) = std::fs::rename(&tmp, &CFG.bak_path) {
        dlog!("DB backup failed", format!("{:?}", e));
        return false;
    }
    true
}

// py: daemon.py:853 db_maintenance
pub fn db_maintenance(force: bool) -> String {
    let r = lock().and_then(|_l| {
        let c = Connection::open(&CFG.path)?;
        c.busy_timeout(Duration::from_secs(10))?;
        c.execute_batch("PRAGMA optimize")?;
        let pages: i64 = c.query_row("PRAGMA page_count", [], |r| r.get(0))?;
        let free: i64 = c.query_row("PRAGMA freelist_count", [], |r| r.get(0))?;
        if pages != 0 && (force || (pages >= 256 && free * 4 > pages)) {
            c.execute_batch("VACUUM")?;
            return Ok(format!("vacuumed ({} of {} pages were free)", free, pages));
        }
        Ok(format!("ok ({} of {} pages free)", free, pages))
    });
    match r {
        Ok(s) => s,
        Err(e) => format!("failed: {}", e),
    }
}

static MAINT_LAST: Mutex<f64> = Mutex::new(0.0);

// py: daemon.py:873 db_maintenance_tick
pub fn db_maintenance_tick(now: f64) -> Option<String> {
    {
        let mut last = MAINT_LAST.lock();
        if *last == 0.0 {
            *last = now - CFG.maint_s + 3600.0;
            return None;
        }
        if now - *last < CFG.maint_s {
            return None;
        }
        *last = now;
    }
    let r = db_maintenance(false);
    dlog!("DB maintenance:", r);
    Some(r)
}
