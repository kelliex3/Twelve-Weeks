use std::collections::{HashMap, VecDeque};
use std::fmt::{Display, Write as _};
use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::clock;

const QUEUE_MAX: usize = 50_000;
const CATS_MAX: usize = 256;
const UNLIMITED: [&str; 4] = ["STATUS", "LOG-SUPPRESSED", "daemon", "DB"];

pub struct Config {
    pub path: PathBuf,
    pub max_bytes: u64,
    pub keep: u32,
    pub burst: f64,
    pub per_s: f64,
}

struct Cat {
    tokens: f64,
    refill: Instant,
    suppressed: u64,
    summary: Instant,
}

struct Logger {
    cfg: Config,
    queue: Mutex<VecDeque<String>>,
    cats: Mutex<HashMap<String, Cat>>,
    capture: Option<Mutex<Vec<String>>>,
}

static LOGGER: OnceLock<Logger> = OnceLock::new();

pub fn init(cfg: Config) {
    let first = LOGGER.set(Logger { cfg, queue: Mutex::new(VecDeque::new()), cats: Mutex::new(HashMap::new()), capture: None }).is_ok();
    if first {
        std::thread::Builder::new().name("log".into()).spawn(flusher).ok();
    }
}

pub fn init_capture() {
    let cfg = Config { path: PathBuf::new(), max_bytes: 0, keep: 0, burst: f64::INFINITY, per_s: 0.0 };
    let _ = LOGGER.set(Logger { cfg, queue: Mutex::new(VecDeque::new()), cats: Mutex::new(HashMap::new()), capture: Some(Mutex::new(Vec::new())) });
}

pub fn take_captured() -> Vec<String> {
    match LOGGER.get().and_then(|l| l.capture.as_ref()) {
        Some(c) => std::mem::take(&mut *lock(c)),
        None => Vec::new(),
    }
}

fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

pub fn clean(s: &str) -> String {
    s.chars()
        .map(|c| if (c as u32) < 32 || c == '\u{7f}' || c == '\u{85}' || c == '\u{2028}' || c == '\u{2029}' { ' ' } else { c })
        .collect()
}

fn cut(s: &str, n: usize) -> &str {
    match s.char_indices().nth(n) {
        Some((i, _)) => &s[..i],
        None => s,
    }
}

fn category(first: &str, second: Option<&str>) -> String {
    match second {
        Some(k) if first == "EVT" => format!("EVT {}", cut(k, 20)),
        _ => cut(first, 24).to_string(),
    }
}

fn allow(l: &Logger, cat: &str) -> bool {
    if UNLIMITED.contains(&cat) {
        return true;
    }
    let now = Instant::now();
    let mut cats = lock(&l.cats);
    if !cats.contains_key(cat) {
        if cats.len() > CATS_MAX {
            return true;
        }
        cats.insert(cat.to_string(), Cat { tokens: l.cfg.burst, refill: now, suppressed: 0, summary: now });
    }
    let Some(b) = cats.get_mut(cat) else { return true };
    let dt = now.duration_since(b.refill).as_secs_f64();
    b.tokens = l.cfg.burst.min(b.tokens + dt * l.cfg.per_s);
    b.refill = now;
    if b.tokens >= 1.0 {
        b.tokens -= 1.0;
        return true;
    }
    b.suppressed += 1;
    if now.duration_since(b.summary).as_secs_f64() >= 5.0 && b.suppressed > 0 {
        let n = b.suppressed;
        b.suppressed = 0;
        b.summary = now;
        let line = format!(
            "[{}] LOG-SUPPRESSED {} {} lines in the last 5 s (> {:.0} per second)",
            clock::hms_now(),
            cat,
            n,
            l.cfg.per_s
        );
        drop(cats);
        push(l, line);
    }
    false
}

fn push(l: &Logger, line: String) {
    if let Some(c) = &l.capture {
        lock(c).push(line);
        return;
    }
    let mut q = lock(&l.queue);
    if q.len() >= QUEUE_MAX {
        q.pop_front();
    }
    q.push_back(line);
}

pub fn line(parts: &[&dyn Display]) {
    let Some(l) = LOGGER.get() else { return };
    let mut body = String::with_capacity(96);
    let mut first = String::new();
    let mut second = None;
    for (i, p) in parts.iter().enumerate() {
        let s = p.to_string();
        if i > 0 {
            body.push(' ');
        }
        body.push_str(&s);
        if i == 0 {
            first = s;
        } else if i == 1 {
            second = Some(s);
        }
    }
    if !parts.is_empty() && l.capture.is_none() {
        let cat = category(&first, second.as_deref());
        if !allow(l, &cat) {
            return;
        }
    }
    let out = if l.capture.is_some() { clean(&body) } else { format!("[{}] {}", clock::hms_now(), clean(&body)) };
    push(l, out);
}

/// A line whose body and rate-limit category were built elsewhere (the clan worker): same limit, clock and queue as `line`.
pub fn raw(cat: &str, body: &str) {
    let Some(l) = LOGGER.get() else { return };
    if l.capture.is_none() && !allow(l, cat) {
        return;
    }
    let out = if l.capture.is_some() { clean(body) } else { format!("[{}] {}", clock::hms_now(), clean(body)) };
    push(l, out);
}

#[macro_export]
macro_rules! dlog {
    ($($p:expr),+ $(,)?) => {
        $crate::log::line(&[$(&$p as &dyn ::std::fmt::Display),+])
    };
}

#[macro_export]
macro_rules! evt {
    ($($p:expr),+ $(,)?) => {
        $crate::log::line(&[&"EVT" as &dyn ::std::fmt::Display, $(&$p as &dyn ::std::fmt::Display),+])
    };
}

fn rotate(path: &Path, keep: u32) {
    let name = path.as_os_str().to_string_lossy().into_owned();
    for i in (1..=keep).rev() {
        let src = if i == 1 { name.clone() } else { format!("{}.{}", name, i - 1) };
        let dst = format!("{}.{}", name, i);
        if Path::new(&src).exists() {
            if Path::new(&dst).exists() && fs::remove_file(&dst).is_err() {
                return;
            }
            if fs::rename(&src, &dst).is_err() {
                return;
            }
        }
    }
}

fn open_new(path: &Path, header: &str) -> Option<File> {
    let mut f = File::create(path).ok()?;
    let _ = f.write_all(header.as_bytes());
    drop(f);
    OpenOptions::new().append(true).open(path).ok()
}

fn flusher() {
    let Some(l) = LOGGER.get() else { return };
    let path = l.cfg.path.clone();
    rotate(&path, l.cfg.keep);
    let mut file = open_new(&path, &format!("# daemon {}\n", clock::ctime_now()));
    let mut size: u64 = 0;
    let mut buf = String::with_capacity(1 << 16);
    loop {
        std::thread::sleep(Duration::from_millis(500));
        loop {
            let mut rolled = false;
            {
                let mut q = lock(&l.queue);
                if q.is_empty() {
                    break;
                }
                buf.clear();
                while let Some(line) = q.pop_front() {
                    let _ = writeln!(buf, "{}", line);
                    size += line.len() as u64 + 1;
                    if size > l.cfg.max_bytes {
                        rolled = true;
                        break;
                    }
                }
            }
            if file.is_none() {
                file = OpenOptions::new().append(true).create(true).open(&path).ok();
            }
            if let Some(f) = file.as_mut() {
                if f.write_all(buf.as_bytes()).is_err() {
                    file = None;
                }
            }
            if rolled {
                drop(file.take());
                rotate(&path, l.cfg.keep);
                file = open_new(&path, &format!("# daemon log continued {}\n", clock::ctime_now()));
                size = 0;
            }
        }
    }
}
