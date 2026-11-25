use serde_json::{Map, Number, Value};

pub fn head(s: &str, n: usize) -> &str {
    match s.char_indices().nth(n) {
        Some((i, _)) => &s[..i],
        None => s,
    }
}

pub fn is_linebreak(c: char) -> bool {
    matches!(c, '\n' | '\r' | '\x0b' | '\x0c' | '\x1c' | '\x1d' | '\x1e' | '\u{85}' | '\u{2028}' | '\u{2029}')
}

pub fn splitlines(s: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut it = s.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        if is_linebreak(c) {
            out.push(&s[start..i]);
            let mut end = i + c.len_utf8();
            if c == '\r' {
                if let Some(&(j, '\n')) = it.peek() {
                    it.next();
                    end = j + 1;
                }
            }
            start = end;
        }
    }
    if start < s.len() {
        out.push(&s[start..]);
    }
    out
}

pub fn round(x: f64) -> i64 {
    x.round_ties_even() as i64
}

pub fn collapse_ws(s: &str) -> String {
    let mut out = String::with_capacity(s.len());
    let mut ws = false;
    for c in s.chars() {
        if c.is_whitespace() {
            if !ws {
                out.push(' ');
            }
            ws = true;
        } else {
            out.push(c);
            ws = false;
        }
    }
    out
}

pub fn is_digits(s: &str) -> bool {
    !s.is_empty() && s.chars().all(|c| c.is_ascii_digit())
}

pub fn truthy(v: &Value) -> bool {
    match v {
        Value::Null => false,
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().map(|f| f != 0.0).unwrap_or(false),
        Value::String(s) => !s.is_empty(),
        Value::Array(a) => !a.is_empty(),
        Value::Object(o) => !o.is_empty(),
    }
}

pub fn repr_str(s: &str) -> String {
    let q = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut out = String::with_capacity(s.len() + 2);
    out.push(q);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == q => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => out.push_str(&format!("\\x{:02x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push(q);
    out
}

fn num_str(n: &Number) -> String {
    if n.is_f64() {
        let f = n.as_f64().unwrap_or(0.0);
        if f.is_finite() && f.fract() == 0.0 && f.abs() < 1e16 {
            return format!("{:.1}", f);
        }
        return format!("{}", f);
    }
    n.to_string()
}

fn repr(v: &Value) -> String {
    match v {
        Value::Null => "None".into(),
        Value::Bool(true) => "True".into(),
        Value::Bool(false) => "False".into(),
        Value::Number(n) => num_str(n),
        Value::String(s) => repr_str(s),
        Value::Array(a) => format!("[{}]", a.iter().map(repr).collect::<Vec<_>>().join(", ")),
        Value::Object(o) => format!("{{{}}}", o.iter().map(|(k, v)| format!("{}: {}", repr_str(k), repr(v))).collect::<Vec<_>>().join(", ")),
    }
}

pub fn to_str(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        other => repr(other),
    }
}

pub fn get_str(o: &Map<String, Value>, k: &str, default: &str) -> String {
    o.get(k).map(to_str).unwrap_or_else(|| default.to_string())
}

pub fn to_int(v: &Value) -> Option<i64> {
    match v {
        Value::Bool(b) => Some(*b as i64),
        Value::Number(n) => n.as_i64().or_else(|| n.as_f64().filter(|f| f.is_finite()).map(|f| f.trunc() as i64)),
        Value::String(s) => s.trim().replace('_', "").parse::<i64>().ok(),
        _ => None,
    }
}

pub fn to_float(v: &Value) -> Option<f64> {
    let f = match v {
        Value::Bool(b) => Some(*b as i64 as f64),
        Value::Number(n) => n.as_f64(),
        Value::String(s) => s.trim().parse::<f64>().ok(),
        _ => None,
    }?;
    f.is_finite().then_some(f)
}

pub fn float(f: f64) -> Value {
    Number::from_f64(f).map(Value::Number).unwrap_or(Value::Null)
}

pub fn opt_float(f: Option<f64>) -> Value {
    f.map(float).unwrap_or(Value::Null)
}

pub fn num_f64(v: &Value) -> Option<f64> {
    match v {
        Value::Number(n) => n.as_f64(),
        Value::Bool(b) => Some(*b as i64 as f64),
        _ => None,
    }
}

pub fn sqlv(v: rusqlite::types::Value) -> Value {
    use rusqlite::types::Value as S;
    match v {
        S::Null => Value::Null,
        S::Integer(i) => Value::from(i),
        S::Real(f) => float(f),
        S::Text(t) => Value::String(t),
        S::Blob(b) => Value::String(String::from_utf8_lossy(&b).into_owned()),
    }
}

pub fn sql_int(v: &rusqlite::types::Value) -> i64 {
    use rusqlite::types::Value as S;
    match v {
        S::Integer(i) => *i,
        S::Real(f) if f.is_finite() => f.trunc() as i64,
        S::Text(t) => t.trim().parse().unwrap_or(0),
        _ => 0,
    }
}

pub fn sql_f64(v: &rusqlite::types::Value) -> f64 {
    use rusqlite::types::Value as S;
    match v {
        S::Integer(i) => *i as f64,
        S::Real(f) => *f,
        S::Text(t) => t.trim().parse().unwrap_or(0.0),
        _ => 0.0,
    }
}

pub fn sql_text(v: &rusqlite::types::Value) -> Option<String> {
    use rusqlite::types::Value as S;
    match v {
        S::Text(t) => Some(t.clone()),
        S::Blob(b) => Some(String::from_utf8_lossy(b).into_owned()),
        S::Integer(i) => Some(i.to_string()),
        S::Real(f) => Some(f.to_string()),
        S::Null => None,
    }
}

pub fn dumps_indent1(v: &Value) -> String {
    let mut buf = Vec::new();
    let fmt = serde_json::ser::PrettyFormatter::with_indent(b" ");
    let mut ser = serde_json::Serializer::with_formatter(&mut buf, fmt);
    if serde::Serialize::serialize(v, &mut ser).is_err() {
        return "null".into();
    }
    String::from_utf8(buf).unwrap_or_default()
}

pub fn literal_list(s: &str) -> Vec<String> {
    let mut p = Lit { s: s.as_bytes(), i: 0, src: s };
    match p.list() {
        Some(v) => {
            p.ws();
            if p.i == p.s.len() {
                v
            } else {
                Vec::new()
            }
        }
        None => Vec::new(),
    }
}

pub fn literal_dict_max(s: &str) -> Option<Option<Value>> {
    let mut p = Lit { s: s.as_bytes(), i: 0, src: s };
    let vals = p.dict()?;
    p.ws();
    if p.i != p.s.len() {
        return None;
    }
    let mut best: Option<(f64, Value)> = None;
    for v in vals {
        let f = num_f64(&v)?;
        if best.as_ref().map(|(b, _)| f > *b).unwrap_or(true) {
            best = Some((f, v));
        }
    }
    Some(best.map(|(_, v)| v))
}

struct Lit<'a> {
    s: &'a [u8],
    i: usize,
    src: &'a str,
}

impl Lit<'_> {
    fn ws(&mut self) {
        while self.i < self.s.len() && matches!(self.s[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }

    fn eat(&mut self, c: u8) -> bool {
        self.ws();
        if self.s.get(self.i) == Some(&c) {
            self.i += 1;
            true
        } else {
            false
        }
    }

    fn list(&mut self) -> Option<Vec<String>> {
        let close = if self.eat(b'[') {
            b']'
        } else if self.eat(b'(') {
            b')'
        } else {
            return None;
        };
        let mut out = Vec::new();
        if self.eat(close) {
            return Some(out);
        }
        loop {
            out.push(self.item()?);
            if self.eat(b',') {
                if self.eat(close) {
                    return Some(out);
                }
                continue;
            }
            return self.eat(close).then_some(out);
        }
    }

    fn item(&mut self) -> Option<String> {
        self.ws();
        match self.s.get(self.i)? {
            b'\'' | b'"' => self.string(),
            _ => self.number().map(|v| to_str(&v)),
        }
    }

    fn dict(&mut self) -> Option<Vec<Value>> {
        if !self.eat(b'{') {
            return None;
        }
        let mut out = Vec::new();
        if self.eat(b'}') {
            return Some(out);
        }
        loop {
            self.ws();
            match self.s.get(self.i)? {
                b'\'' | b'"' => {
                    self.string()?;
                }
                _ => {
                    self.number()?;
                }
            }
            if !self.eat(b':') {
                return None;
            }
            self.ws();
            let v = match self.s.get(self.i)? {
                b'\'' | b'"' => Value::String(self.string()?),
                _ => self.number()?,
            };
            out.push(v);
            if self.eat(b',') {
                if self.eat(b'}') {
                    return Some(out);
                }
                continue;
            }
            return self.eat(b'}').then_some(out);
        }
    }

    fn number(&mut self) -> Option<Value> {
        let start = self.i;
        while self.i < self.s.len() && matches!(self.s[self.i], b'0'..=b'9' | b'-' | b'+' | b'.' | b'e' | b'E' | b'_') {
            self.i += 1;
        }
        let t = self.src.get(start..self.i)?.replace('_', "");
        if t.is_empty() {
            return None;
        }
        if let Ok(i) = t.parse::<i64>() {
            return Some(Value::from(i));
        }
        t.parse::<f64>().ok().filter(|f| f.is_finite()).map(float)
    }

    fn string(&mut self) -> Option<String> {
        let q = self.s[self.i];
        self.i += 1;
        let mut out = String::new();
        loop {
            let rest = self.src.get(self.i..)?;
            let c = rest.chars().next()?;
            self.i += c.len_utf8();
            if c as u32 == q as u32 {
                return Some(out);
            }
            if c == '\n' {
                return None;
            }
            if c != '\\' {
                out.push(c);
                continue;
            }
            let e = self.src.get(self.i..)?.chars().next()?;
            self.i += e.len_utf8();
            match e {
                '\\' => out.push('\\'),
                '\'' => out.push('\''),
                '"' => out.push('"'),
                'n' => out.push('\n'),
                'r' => out.push('\r'),
                't' => out.push('\t'),
                'a' => out.push('\x07'),
                'b' => out.push('\x08'),
                'f' => out.push('\x0c'),
                'v' => out.push('\x0b'),
                '0'..='7' => {
                    let mut n = e.to_digit(8)?;
                    for _ in 0..2 {
                        match self.s.get(self.i) {
                            Some(d @ b'0'..=b'7') => {
                                n = n * 8 + (d - b'0') as u32;
                                self.i += 1;
                            }
                            _ => break,
                        }
                    }
                    out.push(char::from_u32(n)?);
                }
                'x' | 'u' | 'U' => {
                    let n = match e {
                        'x' => 2,
                        'u' => 4,
                        _ => 8,
                    };
                    let h = self.src.get(self.i..self.i + n)?;
                    self.i += n;
                    out.push(char::from_u32(u32::from_str_radix(h, 16).ok()?)?);
                }
                '\n' => {}
                other => {
                    out.push('\\');
                    out.push(other);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn lists() {
        assert_eq!(literal_list("['a', \"it's\", 'b\\'c']"), vec!["a", "it's", "b'c"]);
        assert_eq!(literal_list("[]"), Vec::<String>::new());
        assert_eq!(literal_list("['a'"), Vec::<String>::new());
        assert_eq!(literal_list("['\\xe9t\\u00e9']"), vec!["\u{e9}t\u{e9}"]);
    }

    #[test]
    fn dicts() {
        assert_eq!(literal_dict_max("{'a<>b': 47, 'a<>c': 84}"), Some(Some(Value::from(84))));
        assert_eq!(literal_dict_max("{}"), Some(None));
        assert_eq!(literal_dict_max("{'a': }"), None);
    }

    #[test]
    fn rounding() {
        assert_eq!(round(2.5), 2);
        assert_eq!(round(3.5), 4);
        assert_eq!(round(-0.4), 0);
    }

    #[test]
    fn reprs() {
        assert_eq!(repr_str("it's"), "\"it's\"");
        assert_eq!(to_str(&serde_json::json!([1, "a", null, true])), "[1, 'a', None, True]");
        assert_eq!(to_str(&serde_json::json!(1.0)), "1.0");
    }
}
