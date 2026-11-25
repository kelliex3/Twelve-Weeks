//! Normalisation of everything that carries a wall-clock or scheduling artefact, so two runs of the same scenario compare equal.
use serde_json::Value;

/// Keys whose numeric value is a time, an age or a duration measured by the daemon's clock.
const TIME_KEYS: &[&str] = &[
    "ts", "now", "endsAt", "uptime_s", "since", "started", "age", "age_s", "elapsed", "left", "t", "last_seen", "formed", "launched",
    "started_at", "age_ms", "idle", "idle_s", "queued_s", "wait", "waited", "created", "updated", "at", "time", "clock", "mage", "match_age",
    "t_start", "start", "clock_age", "poll_age", "seen",
];

fn is_epoch(x: f64) -> bool {
    (1.4e9..2.6e9).contains(&x)
}

pub fn norm_json(v: &mut Value) {
    match v {
        Value::Object(m) => {
            m.remove("threads"); // /sizes: the thread count of the Python process
            if m.get("kind").and_then(|k| k.as_str()) == Some("r") && m.get("param").map(|p| p.is_number()).unwrap_or(false) {
                m.insert("param".into(), Value::String("<T>".into())); // a planned restart: seconds left
            }
            for (k, x) in m.iter_mut() {
                if x.is_number() && (TIME_KEYS.contains(&k.as_str())) {
                    *x = Value::String("<T>".into());
                } else {
                    norm_json(x);
                }
            }
        }
        Value::Array(a) => {
            for x in a.iter_mut() {
                norm_json(x);
            }
        }
        Value::Number(n) => {
            if let Some(f) = n.as_f64() {
                if is_epoch(f) {
                    *v = Value::String("<T>".into());
                }
            }
        }
        _ => {}
    }
}

/// Log categories that depend on scheduling, counters of the run or the machine, not on the protocol.
const DROP_PREFIX: &[&str] = &[
    "STATUS", "UDP", "SKILL lookup failed", "LOG-SUPPRESSED", "DB ", "DB:", "# daemon", "load timing:", "NET ", "NET-", "RELAY", "ADMIN listening", "daemon start",
    "daemon version", "DB check", "DB backup", "GC", "ident backfill", "TCP-LIVE",
];

/// Categories written by one connection thread per player, in the order the threads happen to run (keepalive phases, loaded reports):
/// their lines are sorted.
const UNORDERED: &[&str] = &["START-ON-KEEPALIVE", "LOBBY-PARTY", "LOADED", "EVT loaded", "NAME-REFRESH", "EVT session-lost", "SESSION"];

fn strip_stamp(l: &str) -> &str {
    let b = l.as_bytes();
    if b.len() > 11 && b[0] == b'[' && b[3] == b':' && b[6] == b':' && b[9] == b']' && b[10] == b' ' {
        &l[11..]
    } else {
        l
    }
}

/// Replaces the port of every `('ip', port)` tuple and every `ip:port` with P; masks `NN.N s` / `NN s` durations after the words
/// "after", "for", "in", "+" and the like by a placeholder digit run only where it follows a space and precedes " s" / " ms".
fn mask_line(l: &str) -> String {
    let mut out = String::with_capacity(l.len());
    let b: Vec<char> = l.chars().collect();
    let mut i = 0;
    while i < b.len() {
        // ('127.0.0.1', 12345)
        if b[i] == '\'' && i + 1 < b.len() {
            // look for "', " followed by digits and ')'
            let rest: String = b[i..].iter().take(12).collect();
            if rest.starts_with("', ") {
                let mut j = i + 3;
                let st = j;
                while j < b.len() && b[j].is_ascii_digit() {
                    j += 1;
                }
                if j > st && j < b.len() && b[j] == ')' {
                    out.push_str("', P");
                    i = j;
                    continue;
                }
            }
        }
        // durations: <digits>[.<digits>] followed by " s" / " ms" / "s)" -> N
        if b[i].is_ascii_digit() && (i == 0 || !(b[i - 1].is_ascii_alphanumeric() || b[i - 1] == '.' || b[i - 1] == '_' || b[i - 1] == '-')) {
            let mut j = i;
            while j < b.len() && (b[j].is_ascii_digit() || b[j] == '.') {
                j += 1;
            }
            let tail: String = b[j..].iter().take(4).collect();
            let after = |n: usize| tail.chars().nth(n).map(|c| !(c.is_ascii_alphanumeric() || c == '=' || c == '_')).unwrap_or(true);
            if (tail.starts_with(" s") && after(2)) || (tail.starts_with(" ms") && after(3)) {
                out.push('N');
                i = j;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    out
}

/// Lines grouped by category (first word; "EVT <kind>" for events), arrival order kept inside a category, consecutive repeats collapsed:
/// the order in which unrelated threads log is scheduling, the order inside one category is causal.
pub fn norm_log(text: &str) -> String {
    let mut groups: std::collections::BTreeMap<String, Vec<String>> = Default::default();
    for raw in text.lines() {
        let l = strip_stamp(raw);
        if DROP_PREFIX.iter().any(|p| l.starts_with(p)) {
            continue;
        }
        let mut m = mask_line(l);
        // how a session connection ended is a race between the daemon's reader and whatever closed it (FIN, kick, boot-all)
        for why in ["error OSError(9, 'Bad file descriptor')", "peer closed (FIN)", "error ConnectionResetError(104, 'Connection reset by peer')"] {
            m = m.replace(why, "closed");
        }
        // seconds of silence per player: `silent-s {'Alpha': 27}`
        if let Some(p) = m.find("silent-s {") {
            let (head, tail) = m.split_at(p);
            let masked: String = tail.chars().map(|c| if c.is_ascii_digit() { 'N' } else { c }).collect();
            m = format!("{}{}", head, masked);
        }
        let mut it = m.split_whitespace();
        let first = it.next().unwrap_or("").to_string();
        let cat = if first == "EVT" { format!("EVT {}", it.next().unwrap_or("")) } else { first };
        let g = groups.entry(cat).or_default();
        if g.last().map(|x| x != &m).unwrap_or(true) {
            g.push(m);
        }
    }
    let mut out = String::new();
    for (cat, mut g) in groups {
        if UNORDERED.contains(&cat.as_str()) {
            g.sort();
        }
        for l in g {
            out.push_str(&l);
            out.push_str("\n");
        }
    }
    out
}

/// Replaces every 10-digit run that starts with 1 (a unix time) by <T>.
pub fn mask_epoch(t: &str) -> String {
    let b: Vec<char> = t.chars().collect();
    let mut out = String::with_capacity(t.len());
    let mut i = 0;
    while i < b.len() {
        if b[i].is_ascii_digit() {
            let mut j = i;
            while j < b.len() && b[j].is_ascii_digit() {
                j += 1;
            }
            if j - i == 10 && b[i] == '1' && !(i > 0 && b[i - 1] == '.') && !(j < b.len() && b[j] == '.') {
                out.push_str("<T>");
            } else {
                out.extend(b[i..j].iter());
            }
            i = j;
        } else {
            out.push(b[i]);
            i += 1;
        }
    }
    out
}
