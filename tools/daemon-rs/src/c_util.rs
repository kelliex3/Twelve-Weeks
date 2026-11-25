//! Shared bits of the lobby / queue / mode modules (lobbybook, requeue, modeq, gunlimit, interrogation, map_packs):
//! the injectable clock (`now=time.time` in the Python constructors), Python float floor division, and the line protocol of the
//! differential tests (`selftest <module>` reads one JSON call per line and prints one JSON result line per call).
use crate::py;
use serde_json::{Map, Value};
use std::io::{BufRead, Write};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

/// The `now=` callable of the Python constructors.
pub type Now = Arc<dyn Fn() -> f64 + Send + Sync>;

/// `now=time.time`.
pub fn sys_now() -> Now {
    Arc::new(crate::clock::now)
}

/// A clock that only moves when the test script says so.
#[derive(Clone)]
pub struct Frozen(Arc<AtomicU64>);

impl Frozen {
    pub fn new() -> Frozen {
        Frozen(Arc::new(AtomicU64::new(0f64.to_bits())))
    }
    pub fn set(&self, t: f64) {
        self.0.store(t.to_bits(), Ordering::SeqCst);
    }
    pub fn get(&self) -> f64 {
        f64::from_bits(self.0.load(Ordering::SeqCst))
    }
    pub fn now(&self) -> Now {
        let c = self.clone();
        Arc::new(move || c.get())
    }
}

/// Python float `a // b` (CPython float_floor_div).
pub fn float_floordiv(a: f64, b: f64) -> f64 {
    if b == 0.0 {
        return f64::NAN;
    }
    let mut m = a % b;
    let mut div = (a - m) / b;
    if m != 0.0 && ((b < 0.0) != (m < 0.0)) {
        m += b;
        let _ = m;
        div -= 1.0;
    }
    if div != 0.0 {
        let mut fl = div.floor();
        if div - fl > 0.5 {
            fl += 1.0;
        }
        fl
    } else {
        (0.0f64).copysign(a / b)
    }
}

/// Python `bytes.decode("latin1", "ignore")`.
pub fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

/// Python `s[:n]` on a str.
pub fn head(s: &str, n: usize) -> &str {
    py::cut(s, n)
}

// ------------------------------------------------------------------ differential test protocol

/// `sort_keys=True`.
pub fn canon(v: Value) -> Value {
    match v {
        Value::Object(m) => {
            let mut keys: Vec<String> = m.keys().cloned().collect();
            keys.sort();
            let mut out = Map::new();
            let mut m = m;
            for k in keys {
                if let Some(x) = m.remove(&k) {
                    out.insert(k, canon(x));
                }
            }
            Value::Object(out)
        }
        Value::Array(a) => Value::Array(a.into_iter().map(canon).collect()),
        o => o,
    }
}

/// The script: one JSON object per non-empty line.
pub fn script(input: &mut dyn BufRead) -> Vec<Value> {
    let mut v = Vec::new();
    let mut line = String::new();
    while let Ok(n) = input.read_line(&mut line) {
        if n == 0 {
            break;
        }
        let t = line.trim();
        if !t.is_empty() {
            if let Ok(x) = serde_json::from_str::<Value>(t) {
                v.push(x);
            }
        }
        line.clear();
    }
    v
}

/// One result line: {"i": call number, "l": captured log lines, "r": result, "s": state}.
pub fn emit(out: &mut dyn Write, i: usize, r: Value, s: Value) {
    let logs: Vec<Value> = crate::log::take_captured().into_iter().map(Value::String).collect();
    let mut m = Map::new();
    m.insert("i".into(), Value::from(i as i64));
    m.insert("l".into(), Value::Array(logs));
    m.insert("r".into(), r);
    m.insert("s".into(), s);
    let _ = writeln!(out, "{}", py::dumps_compact(&canon(Value::Object(m))));
}

pub fn jf(v: &Value, k: &str) -> f64 {
    v.get(k).and_then(|x| x.as_f64()).unwrap_or(0.0)
}
pub fn ji(v: &Value, k: &str) -> i64 {
    v.get(k).and_then(|x| x.as_i64()).unwrap_or(0)
}
pub fn js<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("")
}
pub fn jb(v: &Value, k: &str) -> bool {
    v.get(k).and_then(|x| x.as_bool()).unwrap_or(false)
}
pub fn jstrs(v: &Value, k: &str) -> Vec<String> {
    v.get(k).and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|s| s.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}
pub fn strs<S: AsRef<str>>(v: &[S]) -> Value {
    Value::Array(v.iter().map(|s| Value::String(s.as_ref().to_string())).collect())
}
