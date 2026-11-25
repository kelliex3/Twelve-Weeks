//! Player rank table for the rank badge in front of every player name (op 0xBF).
//!
//! The Rust version of rankbadge.py. The table is the same for every asker and is built at most every `table_ttl()` seconds.
use parking_lot::Mutex;
use std::sync::atomic::{AtomicU64, Ordering};

pub const LIMIT: i64 = 300; // newest players kept
pub const TABLE_TTL: f64 = 3.0;

/// `rankbadge.TABLE_TTL = x` (the daemon sets 30.0 before the first table).
static TTL_BITS: AtomicU64 = AtomicU64::new(0x4008_0000_0000_0000); // 3.0

pub fn set_table_ttl(x: f64) {
    TTL_BITS.store(x.to_bits(), Ordering::Relaxed);
}

pub fn table_ttl() -> f64 {
    f64::from_bits(TTL_BITS.load(Ordering::Relaxed))
}

/// One row of the table: (shown name, symbol, weeks[, role]). `role` 0 = none (Python: a missing or falsy 4th element).
#[derive(Clone, Debug)]
pub struct Row {
    pub name: String,
    pub rank: i64,
    pub weeks: i64,
    pub role: i64,
}

// py: rankbadge.py:17 norm
/// The key text of a shown name: ASCII letters / digits, lower case.
pub fn norm(name: &str) -> String {
    name.chars().filter(|c| c.is_ascii_alphanumeric()).map(|c| c.to_ascii_lowercase()).collect()
}

// py: rankbadge.py:22 key
/// 32-bit FNV-1a of norm(name); 0 for a name with no letters / digits (never listed).
pub fn key(name: &str) -> u32 {
    let s = norm(name);
    if s.is_empty() {
        return 0;
    }
    let mut h: u32 = 2166136261;
    for b in s.bytes() {
        h = (h ^ b as u32).wrapping_mul(16777619);
    }
    h
}

// py: rankbadge.py:31 wire
/// rows -> the reply text (first row wins for a duplicate key).
pub fn wire(rows: &[Row]) -> String {
    let mut seen = std::collections::HashSet::new();
    let mut out: Vec<String> = Vec::new();
    for row in rows {
        let role = if row.role != 0 { row.role } else { 0 };
        let k = key(&row.name);
        if k == 0 || !seen.insert(k) {
            continue;
        }
        let mut e = format!("{:08x}:{}:{}", k, row.rank.clamp(0, 9), row.weeks.clamp(0, 999));
        if 0 < role && role <= 4 {
            e.push_str(&format!(":{}", role));
        }
        out.push(e);
    }
    format!("n={} {}", out.len(), out.join(" "))
}

struct Cache {
    t: f64,
    text: String,
}

static CACHE: Mutex<Cache> = Mutex::new(Cache { t: 0.0, text: String::new() });

// py: rankbadge.py:55 table_text
/// `fetch()` -> rows; cached `table_ttl()` s. `now` = `time.monotonic()` = `crate::clock::mono()`.
pub fn table_text<F: FnOnce() -> Vec<Row>>(fetch: F) -> String {
    table_text_at(fetch, crate::clock::mono())
}

/// `table_text` with the clock value given.
pub fn table_text_at<F: FnOnce() -> Vec<Row>>(fetch: F, now: f64) -> String {
    {
        let c = CACHE.lock();
        if c.t != 0.0 && now - c.t < table_ttl() {
            return c.text.clone();
        }
    }
    let text = wire(&fetch());
    let mut c = CACHE.lock();
    c.t = now;
    c.text = text.clone();
    text
}

/// Clear the cache (the differential test starts every run with the Python module's initial `{"t": 0.0, "text": "n=0"}`).
pub fn reset_cache() {
    let mut c = CACHE.lock();
    c.t = 0.0;
    c.text = "n=0".to_string();
}

// ---------------------------------------------------------------- differential test
pub fn selftest(input: &mut dyn std::io::BufRead, out: &mut dyn std::io::Write) -> i32 {
    use crate::py::dumps_compact;
    use serde_json::{json, Value};
    reset_cache();
    let mut line = String::new();
    loop {
        line.clear();
        match input.read_line(&mut line) {
            Ok(0) | Err(_) => break,
            Ok(_) => {}
        }
        let Ok(v) = serde_json::from_str::<Value>(line.trim()) else { continue };
        if v["m"].as_str() != Some("rankbadge") {
            continue;
        }
        let rows = |v: &Value| -> Vec<Row> {
            v["rows"]
                .as_array()
                .map(|a| {
                    a.iter()
                        .map(|r| Row {
                            name: r[0].as_str().unwrap_or("").to_string(),
                            rank: r[1].as_i64().unwrap_or(0),
                            weeks: r[2].as_i64().unwrap_or(0),
                            role: r.get(3).and_then(|x| x.as_i64()).unwrap_or(0),
                        })
                        .collect()
                })
                .unwrap_or_default()
        };
        let res: Value = match v["op"].as_str().unwrap_or("") {
            "norm" => json!(norm(v["name"].as_str().unwrap_or(""))),
            "key" => json!(key(v["name"].as_str().unwrap_or(""))),
            "wire" => json!(wire(&rows(&v))),
            "ttl" => {
                set_table_ttl(v["ttl"].as_f64().unwrap_or(3.0));
                json!(null)
            }
            "table" => {
                let r = rows(&v);
                json!(table_text_at(move || r, v["now"].as_f64().unwrap_or(0.0)))
            }
            _ => json!("?"),
        };
        let _ = writeln!(out, "{}", dumps_compact(&res));
    }
    0
}
