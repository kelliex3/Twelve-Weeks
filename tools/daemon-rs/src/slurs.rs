//! Slur filter for chat lines (the Rust version of slurs.py): `find(text)` = the matched pattern or "" when the line is clean.
//!
//! Python reads a text as NFKD + lower case without the combining marks; `slurs_table.rs` (generated from CPython's unicodedata by
//! verify/py/gen_slurs_table.py) holds what that makes of every non-ASCII code point, reduced to what the later steps can see.
use parking_lot::Mutex;
use std::collections::HashSet;
use std::sync::LazyLock;

use crate::py::{is_py_space, strip};
use crate::slurs_table::TABLE;

// py: slurs.py:28 _ANY
const ANY: &[&str] = &[
    "niger", "nigah", "fagot", "wetback", "raghead", "towelhead", "sandniger", "porchmonkey", "junglebuny", "jigabo", "zipperhead", "spearchucker", "chinkie",
    "beaner",
];
// py: slurs.py:31 _EDGE
const EDGE: &[&str] = &["niga", "nigaz", "nigas"];
// py: slurs.py:33 _WORD
const WORD: &[&str] = &[
    "nig", "nigs", "chink", "chinks", "chinky", "spic", "spics", "kike", "kikes", "kyke", "gook", "gooks", "coon", "coons", "paki", "pakis", "fag", "fags", "faggy",
    "tranny", "trannies", "trannys", "darkie", "darky", "darkies", "golliwog", "wog", "wogs", "nignog", "dothead", "camelfucker",
];
// py: slurs.py:37 _ALLOW
const ALLOW: &[&str] = &[
    "niger", "nigeria", "nigerian", "nigerians", "nigerien", "snigger", "sniggers", "sniggered", "sniggering", "sniggerer", "niggle", "niggles", "niggled", "niggling",
    "niggly", "fagot", "fagots", "beanery", "negroni", "negronis",
];

struct Lists {
    any: Vec<String>,
    word: HashSet<String>,
}

// py: slurs.py:42 _any / _word (module state, grown by load_extra)
static LISTS: LazyLock<Mutex<Lists>> = LazyLock::new(|| Mutex::new(Lists { any: ANY.iter().map(|s| s.to_string()).collect(), word: WORD.iter().map(|s| s.to_string()).collect() }));

/// `str.translate(_LOOK)` of one character (None = unchanged).
fn look(c: char) -> Option<char> {
    Some(match c {
        '0' => 'o',
        '1' => 'i',
        '3' => 'e',
        '4' => 'a',
        '5' => 's',
        '7' => 't',
        '8' => 'b',
        '9' => 'g',
        '@' => 'a',
        '$' => 's',
        '!' => 'i',
        '|' => 'i',
        '+' => 't',
        'а' => 'a',
        'е' => 'e',
        'о' => 'o',
        'і' => 'i',
        'с' => 'c',
        'р' => 'p',
        'х' => 'x',
        'у' => 'y',
        'к' => 'k',
        'н' => 'h',
        'г' => 'r',
        'и' => 'u',
        'в' => 'b',
        'т' => 't',
        'м' => 'm',
        'α' => 'a',
        'ε' => 'e',
        'ο' => 'o',
        'ι' => 'i',
        'κ' => 'k',
        'ν' => 'v',
        'ρ' => 'p',
        'τ' => 't',
        'υ' => 'u',
        _ => return None,
    })
}

/// `_NONLETTER.sub("", s.translate(_LOOK))`: the look-alikes mapped, everything but a-z dropped.
fn letters_of(s: &str) -> String {
    s.chars().map(|c| look(c).unwrap_or(c)).filter(|c| c.is_ascii_lowercase()).collect()
}

/// `_REP.sub(r"\1", w)`: a run of one letter becomes one letter.
fn collapse(w: &str) -> String {
    let mut out = String::with_capacity(w.len());
    let mut last: Option<char> = None;
    for c in w.chars() {
        if last != Some(c) {
            out.push(c);
        }
        last = Some(c);
    }
    out
}

// py: slurs.py:45 load_extra
/// Add patterns from a server-side file (one pattern per line, "word" = whole word, "*word" = anywhere in a word, "# ..." = comment).
/// Returns how many were added; a missing file adds none.
pub fn load_extra(path: &std::path::Path) -> i64 {
    let Ok(b) = std::fs::read(path) else { return 0 };
    let text = String::from_utf8_lossy(&b).into_owned();
    let mut n = 0i64;
    let mut g = LISTS.lock();
    for line in crate::social::universal_lines(&text) {
        let head = line.split('#').next().unwrap_or("");
        let w = strip(head).to_lowercase();
        if w.is_empty() {
            continue;
        }
        if let Some(rest) = w.strip_prefix('*') {
            let p = collapse(&letters_of(rest));
            if p.chars().count() >= 4 && !g.any.contains(&p) {
                g.any.push(p);
                n += 1;
            }
        } else {
            let p = letters_of(&w);
            if p.chars().count() >= 2 && !g.word.contains(&p) {
                g.word.insert(p);
                n += 1;
            }
        }
    }
    n
}

/// What NFKD + dropped combining marks + lower case make of one character (appended to `out`).
fn norm_char(c: char, out: &mut String) {
    if c.is_ascii() {
        out.push(c.to_ascii_lowercase());
        return;
    }
    match TABLE.binary_search_by_key(&(c as u32), |e| e.0) {
        Ok(i) => out.push_str(TABLE[i].1),
        Err(_) => out.push('?'),
    }
}

// py: slurs.py:66 _words
/// The words of a text: look-alikes mapped, everything but a-z dropped inside a word, empty words skipped.
pub fn words(text: &str) -> Vec<String> {
    let mut t = String::with_capacity(text.len());
    for c in text.chars() {
        norm_char(c, &mut t);
    }
    let mut out = Vec::new();
    for raw in t.split(is_py_space).filter(|x| !x.is_empty()) {
        let w = letters_of(raw.trim_matches('!')); // a "!" at a word's edge is punctuation ("kike!"), inside it is an "i"
        if !w.is_empty() {
            out.push(w);
        }
    }
    out
}

// py: slurs.py:76 _hit
fn hit(g: &Lists, w: &str, joined: bool) -> String {
    if ALLOW.contains(&w) {
        return String::new();
    }
    if joined {
        // words joined: only the long patterns, and only at the start ("go ok" is not "gook", "mini gerbil" is not a slur, "nig ger" is)
        let c = collapse(w);
        for p in &g.any {
            if c.starts_with(p.as_str()) {
                return p.clone();
            }
        }
        return String::new();
    }
    if g.word.contains(w) {
        return w.to_string();
    }
    let c = collapse(w);
    if g.word.contains(&c) {
        return c;
    }
    for p in &g.any {
        if c.contains(p.as_str()) {
            return p.clone();
        }
    }
    let cx = c.trim_matches('x'); // "xXniggaXx"
    for p in EDGE {
        if c.starts_with(p) || c.ends_with(p) || cx.starts_with(p) || cx.ends_with(p) {
            return p.to_string();
        }
    }
    String::new()
}

// py: slurs.py:96 find
/// The matched pattern (a short label) or "" when the line is clean.
pub fn find(text: &str) -> String {
    let ws = words(text);
    if ws.is_empty() {
        return String::new();
    }
    let g = LISTS.lock();
    let mut cand: Vec<String> = ws.clone();
    let mut glued: Vec<String> = Vec::new(); // runs of single letters glued to the word before / after ("fa g g o t")
    let mut run: Vec<&str> = Vec::new();
    let mut at = 0usize;
    for i in 0..=ws.len() {
        let w: &str = if i < ws.len() { &ws[i] } else { "" };
        if w.len() == 1 {
            if run.is_empty() {
                at = i;
            }
            run.push(w);
            continue;
        }
        if run.len() >= 2 {
            let r: String = run.concat();
            if run.len() >= 3 {
                cand.push(r.clone());
            }
            let before: &str = if at > 0 { &ws[at - 1] } else { "" };
            glued.push(format!("{}{}{}", before, r, w));
        }
        run.clear();
    }
    for w in &cand {
        let h = hit(&g, w, false);
        if !h.is_empty() {
            return h;
        }
    }
    let mut pairs: Vec<String> = Vec::new(); // "porch monkey", "nig ger"
    for k in 0..ws.len().saturating_sub(1) {
        let (a, b) = (&ws[k], &ws[k + 1]);
        if !ALLOW.contains(&a.as_str()) && !ALLOW.contains(&b.as_str()) {
            pairs.push(format!("{}{}", a, b));
        }
    }
    for w in pairs.iter().chain(glued.iter()) {
        let h = hit(&g, w, true);
        if !h.is_empty() {
            return h;
        }
    }
    String::new()
}

// ---------------------------------------------------------------- differential test
/// `t2r-daemon selftest slurs`: the Rust side of verify/py/slurs_harness.py.
pub fn selftest(input: &mut dyn std::io::BufRead, out: &mut dyn std::io::Write) -> i32 {
    use crate::c_util::{ji, js};
    use serde_json::{json, Value};
    let dir = crate::selftest::tmp_dir("slurs");
    for (i, c) in crate::c_util::script(input).iter().enumerate() {
        let op = js(c, "op");
        let mut s = Value::Null;
        let r: Value = match op {
            "find" => json!(find(js(c, "text"))),
            "words" => json!(words(js(c, "text"))),
            "extra" => {
                let p = dir.join("slurs.txt");
                let bytes: Vec<u8> = match c.get("hex").and_then(|x| x.as_str()) {
                    Some(h) => (0..h.len() / 2).filter_map(|k| u8::from_str_radix(&h[2 * k..2 * k + 2], 16).ok()).collect(),
                    None => Vec::new(),
                };
                if c.get("missing").and_then(|x| x.as_bool()).unwrap_or(false) {
                    let _ = std::fs::remove_file(&p);
                } else {
                    let _ = std::fs::write(&p, bytes);
                }
                let n = load_extra(&p);
                let g = LISTS.lock();
                let mut w: Vec<&String> = g.word.iter().collect();
                w.sort();
                s = json!({"any": g.any, "word": w});
                json!(n)
            }
            "sweep" => {
                // every code point of a range through words(): one digest
                use sha2::{Digest, Sha256};
                let (lo, hi) = (ji(c, "lo") as u32, ji(c, "hi") as u32);
                let mut h = Sha256::new();
                for cp in lo..hi {
                    let Some(ch) = char::from_u32(cp) else { continue };
                    let a = words(&format!("a{}b {}", ch, ch));
                    let b = words(&format!("!{}!kike!{} x{}!", ch, ch, ch));
                    h.update(format!("{:x}:{}|{}\n", cp, a.join(","), b.join(",")).as_bytes());
                }
                json!(h.finalize().iter().map(|b| format!("{:02x}", b)).collect::<String>())
            }
            _ => json!("?"),
        };
        crate::c_util::emit(out, i, r, s);
    }
    0
}
