//! Python-compatible formatting helpers, so log lines, wire text and JSON stay byte-identical to daemon.py.
//!
//! - `f(x)`         = Python `str(float)` / `repr(float)` (shortest round-trip, "1.0", "1e-05", "inf")
//! - `round_n(x,n)` = Python `round(x, n)` (correctly rounded, ties to even on the decimal value)
//! - `round0(x)`    = Python `round(x)` -> int (banker's rounding)
//! - `b(v)`         = "True" / "False"
//! - `repr_str(s)`  = Python `repr(str)`
//! - `hex_sp(b)`    = `bytes.hex(" ")`
//! - `json::*`      = `json.dumps` with the Python separators / ensure_ascii and float repr
use std::fmt::Write as _;

#[allow(unused_imports)]
pub use crate::pyfmt::{hex_sp, str_repr as repr_str};
pub use t2r_clanstory::pyjson;

/// Python `str(x)` for a float.
pub fn f(x: f64) -> String {
    t2r_clanstory::pyjson::float_repr(x)
}

/// Python `round(x, n)` for n >= 0 (result is a float). CPython rounds the exact binary value correctly;
/// formatting with enough digits and re-parsing reproduces it for the magnitudes the daemon uses.
pub fn round_n(x: f64, n: i32) -> f64 {
    if !x.is_finite() {
        return x;
    }
    // CPython: uses _Py_dg_dtoa mode 3 (correctly rounded to n decimals, half-even on the exact binary value).
    // Rust's `{:.n$}` formatting is also correctly rounded on the exact binary value (ties to even), so it matches.
    let s = format!("{:.*}", n.max(0) as usize, x);
    s.parse::<f64>().unwrap_or(x)
}

/// Python `round(x)` -> int (ties to even).
pub fn round0(x: f64) -> i64 {
    let r = x.round();
    if (x - x.trunc()).abs() == 0.5 {
        // tie: pick the even neighbour
        let t = x.trunc();
        let up = t + x.signum();
        return if (t as i64) % 2 == 0 { t as i64 } else { up as i64 };
    }
    r as i64
}

pub fn b(v: bool) -> &'static str {
    if v {
        "True"
    } else {
        "False"
    }
}

/// Python `"%.Nf" % x`.
pub fn fixed(x: f64, n: usize) -> String {
    format!("{:.*}", n, x)
}

/// Python `str(list_of_str)` e.g. "['a', 'b']".
pub fn list_str<S: AsRef<str>>(items: &[S]) -> String {
    let mut s = String::from("[");
    for (i, x) in items.iter().enumerate() {
        if i > 0 {
            s.push_str(", ");
        }
        s.push_str(&repr_str(x.as_ref()));
    }
    s.push(']');
    s
}

/// Python `str(list_of_int)` e.g. "[1, 2]".
pub fn list_int<I: std::fmt::Display>(items: &[I]) -> String {
    let mut s = String::from("[");
    for (i, x) in items.iter().enumerate() {
        if i > 0 {
            s.push_str(", ");
        }
        let _ = write!(s, "{}", x);
    }
    s.push(']');
    s
}

/// Python `str(list_of_float)`.
pub fn list_f(items: &[f64]) -> String {
    let v: Vec<String> = items.iter().map(|x| f(*x)).collect();
    format!("[{}]", v.join(", "))
}

/// `bytes.decode("utf-8", "replace")`.
pub fn utf8_replace(b: &[u8]) -> String {
    String::from_utf8_lossy(b).into_owned()
}

/// `bytes.decode("ascii", "replace")` (non-ASCII bytes become U+FFFD).
pub fn ascii_replace(b: &[u8]) -> String {
    b.iter().map(|&c| if c < 0x80 { c as char } else { '\u{fffd}' }).collect()
}

/// `s.strip()` for Python's whitespace set (str.isspace()).
pub fn strip(s: &str) -> &str {
    s.trim_matches(is_py_space)
}

pub fn is_py_space(c: char) -> bool {
    matches!(
        c,
        ' ' | '\t' | '\n' | '\r' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\x1f' | '\u{85}' | '\u{a0}' | '\u{1680}'
            | '\u{2000}'..='\u{200a}' | '\u{2028}' | '\u{2029}' | '\u{202f}' | '\u{205f}' | '\u{3000}'
    )
}

/// First `n` characters (Python `s[:n]` on str).
pub fn cut(s: &str, n: usize) -> &str {
    match s.char_indices().nth(n) {
        Some((i, _)) => &s[..i],
        None => s,
    }
}

/// Python `clean_str(s, n)`: control characters (and U+007F, U+0085, U+2028, U+2029) become spaces, strip(), cut to n characters.
pub fn clean_str(s: &str, n: usize) -> String {
    let t: String = s
        .chars()
        .map(|c| if (c as u32) < 32 || c == '\u{7f}' || c == '\u{85}' || c == '\u{2028}' || c == '\u{2029}' { ' ' } else { c })
        .collect();
    cut(strip(&t), n).to_string()
}

pub fn clean_bytes(b: &[u8], n: usize) -> String {
    clean_str(&utf8_replace(b), n)
}

/// `json.dumps(v, separators=(",", ":"))` (ensure_ascii on).
pub fn dumps_compact(v: &serde_json::Value) -> String {
    pyjson::dumps(v, pyjson::COMPACT)
}

/// `json.dumps(v)` (default ", " / ": " separators, ensure_ascii on).
pub fn dumps(v: &serde_json::Value) -> String {
    pyjson::dumps(v, pyjson::DEFAULT)
}

/// `json.dumps(v, separators=(",", ":"), ensure_ascii=False)`.
pub fn dumps_compact_utf8(v: &serde_json::Value) -> String {
    pyjson::dumps(v, pyjson::COMPACT_UTF8)
}

/// Python `int(x)` on a float (truncation toward zero).
pub fn int_f(x: f64) -> i64 {
    x.trunc() as i64
}

/// Python `//` floor division and `%` modulo for ints.
pub fn floordiv(a: i64, b: i64) -> i64 {
    let q = a / b;
    if (a % b != 0) && ((a < 0) != (b < 0)) {
        q - 1
    } else {
        q
    }
}

pub fn pymod(a: i64, b: i64) -> i64 {
    a - b * floordiv(a, b)
}

/// Python `float % float`.
pub fn fmod(a: f64, b: f64) -> f64 {
    let r = a % b;
    if r != 0.0 && ((r < 0.0) != (b < 0.0)) {
        r + b
    } else {
        r
    }
}
