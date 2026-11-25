//! Client of the Python clan worker (`worker/clan_worker.py`): phase 1 of the port keeps the clan engine in Python.
//!
//! Two worker processes: "rw" for calls that write (the caller holds the daemon's DB lock, like daemon.py held `_dblock`)
//! and "ro" for pure reads (daemon.py's `db_read` path), so a long match-end booking never delays the menu polls.
//! A call that gets no answer within `T2R_CLAN_WORKER_TIMEOUT_S` (default 20 s) kills that worker (by its PID) and fails;
//! the next call starts a fresh one.
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Write};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError};
use std::sync::LazyLock;
use std::time::Duration;

use crate::{dlog, env_parse, env_str};

struct Proc {
    child: Child,
    stdin: ChildStdin,
    rx: Receiver<String>,
}

struct Worker {
    name: &'static str,
    p: Mutex<(Option<Proc>, u64)>,
}

static RW: LazyLock<Worker> = LazyLock::new(|| Worker { name: "rw", p: Mutex::new((None, 0)) });
static RO: LazyLock<Worker> = LazyLock::new(|| Worker { name: "ro", p: Mutex::new((None, 0)) });

fn script() -> String {
    env_str("T2R_CLAN_WORKER", "clan_worker.py")
}

fn spawn(name: &str) -> Option<Proc> {
    let py = env_str("T2R_PYTHON", "python3");
    let mut child = match Command::new(&py)
        .arg("-u")
        .arg(script())
        .env("T2R_CLAN_WORKER_ROLE", name)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            dlog!("CLANW", name, "cannot start the clan worker:", e);
            return None;
        }
    };
    let stdin = child.stdin.take()?;
    let stdout = child.stdout.take()?;
    let (tx, rx) = channel();
    let tname = format!("clanw-{}", name);
    let ok = std::thread::Builder::new()
        .name(tname)
        .stack_size(128 * 1024)
        .spawn(move || {
            let mut r = BufReader::new(stdout);
            let mut line = String::new();
            loop {
                line.clear();
                match r.read_line(&mut line) {
                    Ok(0) | Err(_) => break,
                    Ok(_) => {
                        if tx.send(line.clone()).is_err() {
                            break;
                        }
                    }
                }
            }
        })
        .is_ok();
    if !ok {
        kill(&mut child);
        return None;
    }
    dlog!("CLANW", name, "clan worker started, pid", child.id());
    Some(Proc { child, stdin, rx })
}

fn kill(child: &mut Child) {
    // by PID (our own child), never by name
    let _ = child.kill();
    let _ = child.wait();
}

/// Result of one worker call: the value and the log lines it produced (already written to the daemon log).
pub struct Reply {
    pub r: Value,
}

#[derive(Debug)]
pub enum CallErr {
    /// the worker is not running / died / timed out
    Down(String),
    /// the Python code raised: the repr of the exception (daemon.py logged `repr(e)` / `e` of the same exception)
    Raised(String),
}

impl std::fmt::Display for CallErr {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            CallErr::Down(s) => write!(f, "{}", s),
            CallErr::Raised(s) => write!(f, "{}", s),
        }
    }
}

fn call_on(w: &Worker, func: &str, args: Value) -> Result<Reply, CallErr> {
    let timeout = Duration::from_secs_f64(env_parse("T2R_CLAN_WORKER_TIMEOUT_S", 20.0f64).max(1.0));
    let mut g = w.p.lock();
    if g.0.is_none() {
        g.0 = spawn(w.name);
    }
    g.1 += 1;
    let id = g.1;
    let Some(p) = g.0.as_mut() else { return Err(CallErr::Down("clan worker not running".into())) };
    let req = json!({"id": id, "fn": func, "a": args});
    let line = format!("{}\n", req);
    if p.stdin.write_all(line.as_bytes()).and_then(|_| p.stdin.flush()).is_err() {
        if let Some(mut p) = g.0.take() {
            kill(&mut p.child);
        }
        return Err(CallErr::Down("clan worker pipe closed".into()));
    }
    loop {
        match p.rx.recv_timeout(timeout) {
            Ok(s) => {
                let Ok(v) = serde_json::from_str::<Value>(&s) else { continue };
                if v.get("id").and_then(|x| x.as_u64()) != Some(id) {
                    continue; // a late answer of a call that timed out
                }
                if let Some(lines) = v.get("log").and_then(|x| x.as_array()) {
                    for l in lines {
                        if let (Some(cat), Some(body)) = (l.get(0).and_then(|x| x.as_str()), l.get(1).and_then(|x| x.as_str())) {
                            crate::log::raw(cat, body);
                        }
                    }
                }
                if v.get("ok").and_then(|x| x.as_bool()) == Some(true) {
                    return Ok(Reply { r: v.get("r").cloned().unwrap_or(Value::Null) });
                }
                let e = v.get("err").and_then(|x| x.as_str()).unwrap_or("?").to_string();
                return Err(CallErr::Raised(e));
            }
            Err(RecvTimeoutError::Timeout) => {
                dlog!("CLANW", w.name, "call", func, "timed out - restarting the clan worker");
                if let Some(mut p) = g.0.take() {
                    kill(&mut p.child);
                }
                return Err(CallErr::Down("clan worker timeout".into()));
            }
            Err(RecvTimeoutError::Disconnected) => {
                dlog!("CLANW", w.name, "clan worker exited during", func);
                if let Some(mut p) = g.0.take() {
                    kill(&mut p.child);
                }
                return Err(CallErr::Down("clan worker exited".into()));
            }
        }
    }
}

/// A call that writes. The CALLER must hold the daemon DB lock (`crate::d::db::lock()`) around it, like `with _dblock, dbc() as c:`.
pub fn write(func: &str, args: Value) -> Result<Reply, CallErr> {
    call_on(&RW, func, args)
}

/// A pure read (read-only connection in the worker, no DB lock).
pub fn read(func: &str, args: Value) -> Result<Reply, CallErr> {
    call_on(&RO, func, args)
}

/// Start both workers now (daemon start) and check they answer.
pub fn start() -> bool {
    let a = read("ping", json!({})).is_ok();
    let b = write("ping", json!({})).is_ok();
    a && b
}

/// Stop both workers (daemon exit).
pub fn stop() {
    for w in [&*RW, &*RO] {
        if let Some(mut p) = w.p.lock().0.take() {
            drop(p.stdin);
            kill(&mut p.child);
        }
    }
}
