//! TEMPORARY: removed at integration. The one outside call of clan_journal that the differential test cannot run for real yet: `clan_chronicle.public` (the
//! real port needs clan_status, which is ported by another agent). In the test (`set_test_fake`) it answers with the deterministic fake of
//! verify/py/clan_alife_harness.py; otherwise it calls the real chronicle. At integration: call `clan_chronicle::public(c, tid, "en", 40)` directly.
use rusqlite::Connection;
use serde_json::{json, Value};
use std::sync::atomic::{AtomicBool, Ordering};

static FAKE: AtomicBool = AtomicBool::new(false);

/// Test only: the stand-in answers with the harness fake.
pub fn set_test_fake(on: bool) {
    FAKE.store(on, Ordering::SeqCst);
}

// py: clan_chronicle.py public (called by clan_journal.backfill as CH.public(c, tid, "en"))
pub fn chronicle_public(c: &Connection, tid: &str) -> Result<Value, String> {
    if FAKE.load(Ordering::SeqCst) {
        let maps = ["Hospital", "Motel", "Downtown", "Lakeside", "Bookstore (Part I)"];
        let wins = [Value::Bool(true), Value::Bool(false), Value::Null];
        let ms: Vec<Value> = (1..30i64).map(|d| json!({"d": d, "map": maps[(d % 5) as usize], "win": wins[(d % 3) as usize].clone(), "sup": (d * 7) % 40, "need": 20 + d % 9})).collect();
        return Ok(json!({"matches": ms}));
    }
    crate::clan::clan_chronicle::public(c, tid, "en", 40)
}
