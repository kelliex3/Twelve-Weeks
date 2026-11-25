use rusqlite::{Connection, OpenFlags};
use serde_json::json;

// usage: admin_dump <t2r.db> <now>: one JSON line per clan key and language with the admin feed block
fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 2 {
        eprintln!("usage: admin_dump <t2r.db> <now>");
        std::process::exit(2);
    }
    let c = Connection::open_with_flags(&a[0], OpenFlags::SQLITE_OPEN_READ_ONLY).expect("open");
    let now: f64 = a[1].parse().unwrap_or(0.0);
    let lim: usize = a.get(2).and_then(|x| x.parse().ok()).unwrap_or(usize::MAX);
    let tids: Vec<String> = c.prepare("SELECT token_id FROM clans WHERE status!=0 ORDER BY token_id").unwrap().query_map([], |r| r.get(0)).unwrap().flatten().take(lim).collect();
    for tid in &tids {
        for lang in ["en", "pl"] {
            println!("{}", json!({"tid": tid, "lang": lang, "feed": t2r_clan::admin_feed(&c, tid, lang, now)}));
        }
    }
}
