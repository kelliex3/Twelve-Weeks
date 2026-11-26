//! t2r-bot: differential-test client for the Twelve Weeks game daemon.
//!   t2r-bot list
//!   t2r-bot run <scenario> --port N --admin-port N --workdir DIR --out FILE [--host H] [--version V]
//!   t2r-bot normlog <daemon.log> [path-prefix ...]   (prints the log with the scheduling noise removed)
mod admin;
mod bot;
mod load;
mod norm;
mod proto;
mod rec;
mod scen;
mod scen2;

use std::sync::Arc;

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned()
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match args.first().map(|s| s.as_str()) {
        Some("list") => {
            for (n, d, _) in scen::all() {
                println!("{:<14} {}", n, d);
            }
        }
        Some("normlog") => {
            let p = args.get(1).cloned().unwrap_or_default();
            let t = std::fs::read(&p).map(|b| String::from_utf8_lossy(&b).to_string()).unwrap_or_default();
            // extra arguments: path prefixes (the run's work dir, the python folder) that differ between two runs: printed as <path>
            let mut t = t;
            for pre in args.iter().skip(2) {
                if !pre.is_empty() {
                    t = t.replace(pre.as_str(), "<path>");
                }
            }
            print!("{}", norm::norm_log(&t));
        }
        Some("run") => {
            let name = args.get(1).cloned().unwrap_or_default();
            let Some((_, _, f)) = scen::all().into_iter().find(|(n, _, _)| *n == name) else {
                eprintln!("unknown scenario {}", name);
                std::process::exit(2);
            };
            let rec = Arc::new(rec::Rec::default());
            let ctx = Arc::new(bot::Ctx {
                host: arg(&args, "--host").unwrap_or_else(|| "127.0.0.1".into()),
                port: arg(&args, "--port").and_then(|v| v.parse().ok()).unwrap_or(7301),
                admin_port: arg(&args, "--admin-port").and_then(|v| v.parse().ok()).unwrap_or(7311),
                workdir: arg(&args, "--workdir").unwrap_or_else(|| ".".into()).into(),
                rec: rec.clone(),
                version: arg(&args, "--version").unwrap_or_else(|| "cz1117v".into()),
                pings: Default::default(),
                versions: Default::default(),
                linux: Default::default(),
            });
            rec.script(format!("##### scenario {}", name));
            f(&ctx);
            let out = arg(&args, "--out").unwrap_or_else(|| format!("{}.frames.txt", name));
            if let Err(e) = std::fs::write(&out, rec.render()) {
                eprintln!("cannot write {}: {}", out, e);
                std::process::exit(1);
            }
            println!("wrote {}", out);
        }
        Some("load") => {
            load::run(&args);
        }
        _ => {
            eprintln!("usage: t2r-bot list | run <scenario> --port N --admin-port N --workdir DIR --out FILE | normlog FILE");
            std::process::exit(2);
        }
    }
}
