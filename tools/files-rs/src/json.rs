//! Minimal JSON for the poll replies (std only): parse into an order-keeping value, write it back byte-for-byte like Python's
//! `json.dumps(obj, separators=(",", ":"))` (ensure_ascii: every non-ASCII character as \uXXXX, surrogate pairs above U+FFFF).
//! Numbers keep their source text: the data files are written by Python's json.dump, whose number text json.dumps reproduces.

#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Num(String),
    Str(String),
    Arr(Vec<Json>),
    Obj(Vec<(String, Json)>),
}

impl Json {
    pub fn get(&self, key: &str) -> Option<&Json> {
        match self {
            Json::Obj(v) => v.iter().find(|(k, _)| k == key).map(|(_, x)| x),
            _ => None,
        }
    }
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Json::Str(s) => Some(s),
            _ => None,
        }
    }
    /// Python truthiness (None, False, 0, "", [], {} are false).
    pub fn truthy(&self) -> bool {
        match self {
            Json::Null => false,
            Json::Bool(b) => *b,
            Json::Num(n) => n.parse::<f64>().map(|x| x != 0.0).unwrap_or(true),
            Json::Str(s) => !s.is_empty(),
            Json::Arr(a) => !a.is_empty(),
            Json::Obj(o) => !o.is_empty(),
        }
    }
    /// Set (replace in place, like a Python dict assignment) or append.
    pub fn set(&mut self, key: &str, val: Json) {
        if let Json::Obj(v) = self {
            if let Some(e) = v.iter_mut().find(|(k, _)| k == key) {
                e.1 = val;
            } else {
                v.push((key.to_string(), val));
            }
        }
    }
    pub fn remove(&mut self, key: &str) {
        if let Json::Obj(v) = self {
            v.retain(|(k, _)| k != key);
        }
    }
    pub fn str(s: &str) -> Json {
        Json::Str(s.to_string())
    }
}

pub fn parse(text: &str) -> Option<Json> {
    let b = text.as_bytes();
    let mut p = P { b, i: 0, depth: 0 };
    p.ws();
    let v = p.value()?;
    p.ws();
    if p.i != b.len() {
        return None;
    }
    Some(v)
}

struct P<'a> {
    b: &'a [u8],
    i: usize,
    depth: u32,
}

impl<'a> P<'a> {
    fn ws(&mut self) {
        while self.i < self.b.len() && matches!(self.b[self.i], b' ' | b'\t' | b'\n' | b'\r') {
            self.i += 1;
        }
    }
    fn lit(&mut self, s: &[u8]) -> Option<()> {
        if self.b[self.i..].starts_with(s) {
            self.i += s.len();
            Some(())
        } else {
            None
        }
    }
    fn value(&mut self) -> Option<Json> {
        if self.i >= self.b.len() {
            return None;
        }
        match self.b[self.i] {
            b'n' => self.lit(b"null").map(|_| Json::Null),
            b't' => self.lit(b"true").map(|_| Json::Bool(true)),
            b'f' => self.lit(b"false").map(|_| Json::Bool(false)),
            b'"' => self.string().map(Json::Str),
            b'[' => {
                self.depth += 1;
                if self.depth > 64 {
                    return None;
                }
                self.i += 1;
                let mut v = Vec::new();
                self.ws();
                if self.b.get(self.i) == Some(&b']') {
                    self.i += 1;
                    self.depth -= 1;
                    return Some(Json::Arr(v));
                }
                loop {
                    self.ws();
                    v.push(self.value()?);
                    self.ws();
                    match self.b.get(self.i)? {
                        b',' => self.i += 1,
                        b']' => {
                            self.i += 1;
                            self.depth -= 1;
                            return Some(Json::Arr(v));
                        }
                        _ => return None,
                    }
                }
            }
            b'{' => {
                self.depth += 1;
                if self.depth > 64 {
                    return None;
                }
                self.i += 1;
                let mut v: Vec<(String, Json)> = Vec::new();
                self.ws();
                if self.b.get(self.i) == Some(&b'}') {
                    self.i += 1;
                    self.depth -= 1;
                    return Some(Json::Obj(v));
                }
                loop {
                    self.ws();
                    if self.b.get(self.i) != Some(&b'"') {
                        return None;
                    }
                    let k = self.string()?;
                    self.ws();
                    if self.b.get(self.i) != Some(&b':') {
                        return None;
                    }
                    self.i += 1;
                    self.ws();
                    let x = self.value()?;
                    // a repeated key: Python's json.load keeps the LAST value, at the FIRST key's position
                    if let Some(e) = v.iter_mut().find(|(kk, _)| *kk == k) {
                        e.1 = x;
                    } else {
                        v.push((k, x));
                    }
                    self.ws();
                    match self.b.get(self.i)? {
                        b',' => self.i += 1,
                        b'}' => {
                            self.i += 1;
                            self.depth -= 1;
                            return Some(Json::Obj(v));
                        }
                        _ => return None,
                    }
                }
            }
            b'-' | b'0'..=b'9' => {
                let st = self.i;
                if self.b[self.i] == b'-' {
                    self.i += 1;
                }
                while self.i < self.b.len() && matches!(self.b[self.i], b'0'..=b'9' | b'.' | b'e' | b'E' | b'+' | b'-') {
                    self.i += 1;
                }
                let t = std::str::from_utf8(&self.b[st..self.i]).ok()?;
                t.parse::<f64>().ok()?;
                Some(Json::Num(t.to_string()))
            }
            _ => None,
        }
    }
    fn hex4(&mut self) -> Option<u32> {
        let h = std::str::from_utf8(self.b.get(self.i..self.i + 4)?).ok()?;
        self.i += 4;
        u32::from_str_radix(h, 16).ok()
    }
    fn string(&mut self) -> Option<String> {
        self.i += 1; // opening quote
        let mut out = String::new();
        loop {
            let c = *self.b.get(self.i)?;
            match c {
                b'"' => {
                    self.i += 1;
                    return Some(out);
                }
                b'\\' => {
                    self.i += 1;
                    let e = *self.b.get(self.i)?;
                    self.i += 1;
                    match e {
                        b'"' => out.push('"'),
                        b'\\' => out.push('\\'),
                        b'/' => out.push('/'),
                        b'b' => out.push('\u{8}'),
                        b'f' => out.push('\u{c}'),
                        b'n' => out.push('\n'),
                        b'r' => out.push('\r'),
                        b't' => out.push('\t'),
                        b'u' => {
                            let mut cp = self.hex4()?;
                            if (0xD800..0xDC00).contains(&cp) && self.b.get(self.i) == Some(&b'\\') && self.b.get(self.i + 1) == Some(&b'u') {
                                let save = self.i;
                                self.i += 2;
                                let lo = self.hex4()?;
                                if (0xDC00..0xE000).contains(&lo) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    self.i = save;
                                }
                            }
                            out.push(char::from_u32(cp).unwrap_or('\u{FFFD}'));
                        }
                        _ => return None,
                    }
                }
                _ => {
                    // copy one UTF-8 sequence
                    let len = match c {
                        0x00..=0x7F => 1,
                        0xC0..=0xDF => 2,
                        0xE0..=0xEF => 3,
                        _ => 4,
                    };
                    let s = std::str::from_utf8(self.b.get(self.i..self.i + len)?).ok()?;
                    out.push_str(s);
                    self.i += len;
                }
            }
        }
    }
}

pub fn dumps(v: &Json) -> String {
    let mut s = String::new();
    write(v, &mut s);
    s
}

fn write_str(t: &str, out: &mut String) {
    out.push('"');
    for ch in t.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 || (c as u32) > 0x7E => {
                let mut buf = [0u16; 2];
                for u in c.encode_utf16(&mut buf) {
                    out.push_str(&format!("\\u{:04x}", u));
                }
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

fn write(v: &Json, out: &mut String) {
    match v {
        Json::Null => out.push_str("null"),
        Json::Bool(true) => out.push_str("true"),
        Json::Bool(false) => out.push_str("false"),
        Json::Num(n) => out.push_str(n),
        Json::Str(t) => write_str(t, out),
        Json::Arr(a) => {
            out.push('[');
            for (i, x) in a.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write(x, out);
            }
            out.push(']');
        }
        Json::Obj(o) => {
            out.push('{');
            for (i, (k, x)) in o.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                write_str(k, out);
                out.push(':');
                write(x, out);
            }
            out.push('}');
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_like_python() {
        let t = "{\"a\": 1, \"b\": [true, false, null, -2.5e3], \"c\": \"\u{142}\u{f3}d\u{17a} \\\"q\\\" \\n \\ud83d\\ude00 /\", \"d\": {}, \"e\": []}";
        let v = parse(t).unwrap();
        assert_eq!(
            dumps(&v),
            "{\"a\":1,\"b\":[true,false,null,-2.5e3],\"c\":\"\\u0142\\u00f3d\\u017a \\\"q\\\" \\n \\ud83d\\ude00 /\",\"d\":{},\"e\":[]}"
        );
    }

    #[test]
    fn rejects_garbage() {
        assert!(parse("{").is_none());
        assert!(parse("[1,]").is_none());
        assert!(parse("{\"a\" 1}").is_none());
        assert!(parse("1 2").is_none());
        assert!(parse(&"[".repeat(100)).is_none());
    }

    #[test]
    fn duplicate_key_like_python() {
        assert_eq!(dumps(&parse("{\"a\":1,\"b\":2,\"a\":3}").unwrap()), "{\"a\":3,\"b\":2}");
    }

    #[test]
    fn set_remove_truthy() {
        let mut v = parse("{\"x\":0,\"y\":\"\"}").unwrap();
        assert!(!v.get("x").unwrap().truthy() && !v.get("y").unwrap().truthy());
        v.set("x", Json::str("u"));
        v.set("z", Json::Null);
        v.remove("y");
        assert_eq!(dumps(&v), "{\"x\":\"u\",\"z\":null}");
    }
}
