//! py: clan_kids.py - kids are never fighters: a clan member with role "kid" is never cast in combat lore.
//! The two word-list regexes use lookbehind / lookahead in Python; here they are rewritten for the `regex` crate as an equivalent
//! "boundary char + words + boundary char" search (existence only, which is all `combat_text` needs).
use crate::clan::data;
use regex::Regex;
use serde_json::Value;
use std::collections::BTreeSet;
use std::sync::LazyLock;

const KID_ROLE: &str = "kid";
/// Python `\w` (str.isalnum() or "_").
const W: &str = r"\p{L}\p{N}_";

fn isw(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

fn tbl(name: &str) -> &'static Value {
    data::t("clan_kids", name)
}

fn pat(name: &str) -> String {
    let src = tbl(name).as_str().unwrap_or("");
    // `\w` -> the Python class; `\b` only occurs as `ran\b` / `cel[ue]\b` right before the closing lookahead `(?!\w)`, which already demands a non-word char
    src.replace("\\w", &format!("[{}]", W)).replace("\\b", "")
}

// py: clan_kids.py:60 COMBAT_RE  = (?<![\w'])(?:EN)(?![\w'])   re.I
static COMBAT_RE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(&format!("(?i)(?:^|[^{w}'])(?:{p})(?:[^{w}']|$)", w = W, p = pat("_EN"))).unwrap_or_else(|_| Regex::new("^\\b$").expect("fallback regex"))
});
// py: clan_kids.py:61 COMBAT_RE_PL = (?<!\w)(?:PL)(?!\w)   re.I
static COMBAT_RE_PL: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(&format!("(?i)(?:^|[^{w}])(?:{p})(?:[^{w}]|$)", w = W, p = pat("_PL"))).unwrap_or_else(|_| Regex::new("^\\b$").expect("fallback regex"))
});

/// The patterns compile (a broken word list would make `combat_text` always false).
pub fn regexes_ok() -> bool {
    !pat("_EN").is_empty() && COMBAT_RE.is_match("the shotgun.") && COMBAT_RE_PL.is_match("strzelba.")
}

// py: clan_kids.py:25 is_kid
pub fn is_kid(p: &Value) -> bool {
    crate::clan::clan::truthy(p) && p.get("r").and_then(|v| v.as_str()) == Some(KID_ROLE)
}

// py: clan_kids.py:29 can_fight
/// Who may be cast in combat, raid, patrol, guard or kill lore: everybody but kids.
pub fn can_fight(p: &Value) -> bool {
    !is_kid(p)
}

// py: clan_kids.py:34 fighters
pub fn fighters(people: &[Value]) -> Vec<&Value> {
    people.iter().filter(|p| can_fight(p)).collect()
}

pub fn is_combat_kind(s: &str) -> bool {
    data::set_has_str(tbl("COMBAT_KINDS"), s)
}
pub fn is_combat_pool_alife(s: &str) -> bool {
    data::set_has_str(tbl("COMBAT_POOLS_ALIFE"), s)
}
pub fn is_combat_pool_chron(s: &str) -> bool {
    data::set_has_str(tbl("COMBAT_POOLS_CHRON"), s)
}
pub fn is_combat_scene_saga(s: &str) -> bool {
    data::set_has_str(tbl("COMBAT_SCENES_SAGA"), s)
}

/// `_KEY_RE.findall(t)`: `\{(\w+?)(?:_\w+)?\}` group 1 of every match.
fn key_matches(t: &str) -> Vec<String> {
    let ch: Vec<char> = t.chars().collect();
    let n = ch.len();
    let mut out = Vec::new();
    let mut i = 0;
    while i < n {
        if ch[i] == '{' {
            let mut j = i + 1;
            while j < n && isw(ch[j]) {
                j += 1;
            }
            let l = j - (i + 1);
            if l >= 1 && j < n && ch[j] == '}' {
                // the lazy `\w+?` stops at the first length whose rest is empty or `_\w+`
                let mut a = 1;
                while a < l {
                    if ch[i + 1 + a] == '_' && l - a >= 2 {
                        break;
                    }
                    a += 1;
                }
                out.push(ch[i + 1..i + 1 + a].iter().collect());
                i = j + 1;
                continue;
            }
        }
        i += 1;
    }
    out
}

/// `_LETTER_RE.findall(t)`: `\{([ABCD])(?:_\w+)?\}`.
fn letter_matches(t: &str) -> BTreeSet<char> {
    let ch: Vec<char> = t.chars().collect();
    let n = ch.len();
    let mut out = BTreeSet::new();
    let mut i = 0;
    while i < n {
        if ch[i] == '{' && i + 1 < n && matches!(ch[i + 1], 'A'..='D') {
            let l = ch[i + 1];
            let mut j = i + 2;
            if j < n && ch[j] == '}' {
                out.insert(l);
                i = j + 1;
                continue;
            }
            if j < n && ch[j] == '_' {
                j += 1;
                let s = j;
                while j < n && isw(ch[j]) {
                    j += 1;
                }
                if j > s && j < n && ch[j] == '}' {
                    out.insert(l);
                    i = j + 1;
                    continue;
                }
            }
        }
        i += 1;
    }
    out
}

fn has_pl_letters(t: &str) -> bool {
    t.chars().any(|c| "ąćęłńóśźżĄĆĘŁŃÓŚŹŻ".contains(c))
}

// py: clan_kids.py:73 combat_text
/// True when a template / sentence (with or without {placeholders}) reads as combat, raid, patrol, danger-scavenging or weapon lore.
/// lang "en" | "pl" picks the word list; None = Polish when the text has Polish letters, else English.
pub fn combat_text(t: &str, lang: Option<&str>) -> bool {
    // a pure function of (text, language): memoised (the big regexes ran on every candidate line of every menu / day)
    static MEMO: LazyLock<parking_lot::Mutex<std::collections::HashMap<(String, u8), bool>>> = LazyLock::new(|| parking_lot::Mutex::new(std::collections::HashMap::new()));
    let lk = match lang {
        None => 0u8,
        Some("pl") => 1,
        Some(_) => 2,
    };
    if let Some(v) = MEMO.lock().get(&(t.to_string(), lk)) {
        return *v;
    }
    let r = combat_text_raw(t, lang);
    let mut m = MEMO.lock();
    if m.len() > 200_000 {
        m.clear();
    }
    m.insert((t.to_string(), lk), r);
    r
}

fn combat_text_raw(t: &str, lang: Option<&str>) -> bool {
    if t.is_empty() {
        return false;
    }
    let vars = tbl("_VARS");
    for k in key_matches(t) {
        if data::set_has_str(vars, &k) {
            return true;
        }
    }
    let pl = match lang {
        Some(l) => l == "pl",
        None => has_pl_letters(t),
    };
    if pl {
        COMBAT_RE_PL.is_match(t)
    } else {
        COMBAT_RE.is_match(t)
    }
}

// py: clan_kids.py:90 letters_in
pub fn letters_in(tpl: &str) -> BTreeSet<char> {
    letter_matches(tpl)
}

// py: clan_kids.py:94 kid_letters
/// The letters of `tpl` that stand for kids in `people` ({letter: person}).
pub fn kid_letters(tpl: &str, people: &Value) -> BTreeSet<char> {
    letters_in(tpl).into_iter().filter(|l| people.get(l.to_string()).map(is_kid).unwrap_or(false)).collect()
}

// py: clan_kids.py:99 kid_safe
/// False when any of the templates (the English one, its Polish twin ...) is combat while it names a kid.
pub fn kid_safe(tpl: &str, people: &Value, more: &[&str]) -> bool {
    let mut ks = kid_letters(tpl, people);
    for m in more {
        ks.extend(kid_letters(m, people));
    }
    if ks.is_empty() {
        return true;
    }
    !(combat_text(tpl, Some("en")) || more.iter().any(|m| !m.is_empty() && combat_text(m, Some("pl"))))
}

/// `re.split(r"(?<=[.!?])\s+", text)`.
fn split_sentences(text: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut prev: Option<char> = None;
    let mut it = text.char_indices().peekable();
    while let Some((i, c)) = it.next() {
        if crate::py::is_py_space(c) && matches!(prev, Some('.') | Some('!') | Some('?')) {
            let mut end = i + c.len_utf8();
            while let Some(&(j, d)) = it.peek() {
                if crate::py::is_py_space(d) {
                    end = j + d.len_utf8();
                    it.next();
                } else {
                    break;
                }
            }
            out.push(&text[start..i]);
            start = end;
            prev = Some(' ');
            continue;
        }
        prev = Some(c);
    }
    out.push(&text[start..]);
    out
}

/// `re.search(r"(?<!\w)" + re.escape(name) + r"(?!\w)", s)`.
fn has_word(s: &str, name: &str) -> bool {
    if name.is_empty() {
        return false;
    }
    for (i, _) in s.char_indices() {
        if s[i..].starts_with(name) {
            let before = s[..i].chars().next_back();
            let after = s[i + name.len()..].chars().next();
            if !before.map(isw).unwrap_or(false) && !after.map(isw).unwrap_or(false) {
                return true;
            }
        }
    }
    false
}

// py: clan_kids.py:108 kid_text_ok
/// False when a RENDERED line (variables filled in) names a kid in a sentence that reads as combat.
pub fn kid_text_ok(text: &str, people: &Value, lang: &str) -> bool {
    let ks: Vec<String> = match people.as_object() {
        Some(o) => o
            .values()
            .filter(|p| is_kid(p))
            .filter_map(|p| p.get("n"))
            .filter(|n| crate::clan::clan::truthy(n))
            .map(crate::clan::clan::pystr)
            .collect(),
        None => Vec::new(),
    };
    if ks.is_empty() || !combat_text(text, Some(lang)) {
        return true;
    }
    for s in split_sentences(text) {
        if combat_text(s, Some(lang)) && ks.iter().any(|n| has_word(s, n)) {
            return false;
        }
    }
    true
}

// py: clan_kids.py:119 any_kid
pub fn any_kid(people: &Value) -> bool {
    people.as_object().map(|o| o.values().any(is_kid)).unwrap_or(false)
}

// py: clan_kids.py:123 kid_names
/// Names (alive or dead) of the kids of a clan story state; dead kids keep their role in st["dead"].
pub fn kid_names(st: &Value) -> BTreeSet<String> {
    let mut out = BTreeSet::new();
    for p in st.get("people").and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[]) {
        if is_kid(p) {
            out.insert(p.get("n").map(crate::clan::clan::pystr).unwrap_or_default());
        }
    }
    for d in st.get("dead").and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[]) {
        if d.get("r").and_then(|v| v.as_str()) == Some(KID_ROLE) {
            out.insert(d.get("n").map(crate::clan::clan::pystr).unwrap_or_default());
        }
    }
    out
}
