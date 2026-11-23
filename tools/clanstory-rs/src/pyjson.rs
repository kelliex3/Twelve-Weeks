use serde_json::Value;
use std::fmt::Write;

#[derive(Clone, Copy)]
pub struct Style {
    pub compact: bool,
    pub ascii: bool,
}

pub const COMPACT: Style = Style { compact: true, ascii: true };
pub const DEFAULT: Style = Style { compact: false, ascii: true };
pub const COMPACT_UTF8: Style = Style { compact: true, ascii: false };

pub fn float_repr(f: f64) -> String {
    if f.is_nan() {
        return "nan".into();
    }
    if f.is_infinite() {
        return if f > 0.0 { "inf".into() } else { "-inf".into() };
    }
    if f == 0.0 {
        return if f.is_sign_negative() { "-0.0".into() } else { "0.0".into() };
    }
    let e = format!("{:e}", f);
    let (mant, exp) = match e.split_once('e') {
        Some((m, x)) => (m, x.parse::<i32>().unwrap_or(0)),
        None => (e.as_str(), 0),
    };
    let neg = mant.starts_with('-');
    let mut digits: String = mant.chars().filter(|c| c.is_ascii_digit()).collect();
    if let Some(even) = tie_to_even(f, &digits, exp) {
        digits = even;
    }
    let mut out = String::with_capacity(24);
    if neg {
        out.push('-');
    }
    let n = digits.len() as i32;
    if (-4..16).contains(&exp) {
        if exp >= 0 {
            let int_len = exp + 1;
            if n <= int_len {
                out.push_str(&digits);
                for _ in 0..(int_len - n) {
                    out.push('0');
                }
                out.push_str(".0");
            } else {
                out.push_str(&digits[..int_len as usize]);
                out.push('.');
                out.push_str(&digits[int_len as usize..]);
            }
        } else {
            out.push_str("0.");
            for _ in 0..(-exp - 1) {
                out.push('0');
            }
            out.push_str(&digits);
        }
    } else {
        out.push_str(&digits[..1]);
        if n > 1 {
            out.push('.');
            out.push_str(&digits[1..]);
        }
        let _ = write!(out, "e{}{:02}", if exp < 0 { '-' } else { '+' }, exp.abs());
    }
    out
}

fn tie_to_even(f: f64, digits: &str, exp: i32) -> Option<String> {
    let n = digits.len();
    let last = digits.as_bytes().last().copied()?;
    if (last - b'0') % 2 == 0 {
        return None;
    }
    let full = format!("{:.40e}", f.abs());
    let (fm, fe) = full.split_once('e')?;
    if fe.parse::<i32>().ok()? != exp {
        return None;
    }
    let fd: String = fm.chars().filter(|c| c.is_ascii_digit()).collect();
    if fd.len() <= n || fd.as_bytes()[n] != b'5' || !fd[n + 1..].bytes().all(|b| b == b'0') {
        return None;
    }
    let low = &fd[..n];
    if low == digits {
        return None;
    }
    let back: f64 = format!("{}.{}e{}", &low[..1], &low[1..], exp).parse().ok()?;
    if back == f.abs() {
        Some(low.to_string())
    } else {
        None
    }
}

fn json_float(f: f64) -> String {
    if f.is_nan() {
        "NaN".into()
    } else if f.is_infinite() {
        if f > 0.0 { "Infinity".into() } else { "-Infinity".into() }
    } else {
        float_repr(f)
    }
}

pub fn write_str(out: &mut String, s: &str, ascii: bool) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c if ascii && (c as u32) > 0x7e => {
                let mut buf = [0u16; 2];
                for u in c.encode_utf16(&mut buf) {
                    let _ = write!(out, "\\u{:04x}", u);
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

pub fn write(out: &mut String, v: &Value, st: Style) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => {
            if let Some(i) = n.as_i64() {
                let _ = write!(out, "{}", i);
            } else if let Some(u) = n.as_u64() {
                let _ = write!(out, "{}", u);
            } else {
                out.push_str(&json_float(n.as_f64().unwrap_or(0.0)));
            }
        }
        Value::String(s) => write_str(out, s, st.ascii),
        Value::Array(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push_str(if st.compact { "," } else { ", " });
                }
                write(out, x, st);
            }
            out.push(']');
        }
        Value::Object(m) => {
            out.push('{');
            for (i, (k, x)) in m.iter().enumerate() {
                if i > 0 {
                    out.push_str(if st.compact { "," } else { ", " });
                }
                write_str(out, k, st.ascii);
                out.push_str(if st.compact { ":" } else { ": " });
                write(out, x, st);
            }
            out.push('}');
        }
    }
}

pub fn dumps(v: &Value, st: Style) -> String {
    let mut s = String::with_capacity(64);
    write(&mut s, v, st);
    s
}
