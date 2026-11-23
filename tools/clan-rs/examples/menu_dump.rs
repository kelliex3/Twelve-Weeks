use rusqlite::{Connection, OpenFlags};
use serde_json::json;

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    if a.len() < 2 {
        eprintln!("usage: menu_dump <t2r.db> <now> [<now> ...]");
        std::process::exit(2);
    }
    let c = match Connection::open_with_flags(&a[0], OpenFlags::SQLITE_OPEN_READ_ONLY) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("{}", e);
            std::process::exit(1);
        }
    };
    let tids: Vec<String> = match c.prepare("SELECT token_id FROM clans WHERE status!=0 ORDER BY token_id") {
        Ok(mut st) => st.query_map([], |r| r.get(0)).map(|r| r.flatten().collect()).unwrap_or_default(),
        Err(_) => Vec::new(),
    };
    let nows: Vec<f64> = a[1..].iter().filter_map(|x| x.parse().ok()).collect();
    let t0 = std::time::Instant::now();
    for tid in &tids {
        for lang in ["en", "pl"] {
            for now in &nows {
                let m = t2r_clan::menu_lines(&c, tid, 6, lang, *now);
                println!("{}", json!({"tid": tid, "lang": lang, "now": now, "menu": m}));
            }
        }
    }
    eprintln!("{} calls in {:.2}s", tids.len() * 2 * nows.len(), t0.elapsed().as_secs_f64());
}
