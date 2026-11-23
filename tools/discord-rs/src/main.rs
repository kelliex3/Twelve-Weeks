mod bot;
mod cache;
mod card;
mod commands;
mod config;
mod desk;
mod live;
mod model;
mod net;
mod post;
mod util;

use config::Config;
use serde_json::json;
use std::path::{Path, PathBuf};
use std::process::ExitCode;
use std::sync::Arc;

const USAGE: &str = "twelveweeks-bot [--dir <bot folder>] <command>
  run                                  run the bot (buttons, welcome cards, slash commands, bug desk, live counts)
  post <channel...>                    re-post the art in those channels: welcome rules choose-your-faction announcements
                                       patch-notes lfg wlf seraphites files (REST only, the running bot is untouched)
  live-counts [status.json [launchers.json]] [--at-snapshot]
                                       print the LIVE PLAYERS channel names for that data (changes nothing)
  desk-check <reports dir> <report_codes.json> <title> <first message>
                                       print what the bug desk would answer to that forum post (no network, no writes)
  known-text <known_issues.json>       print the known-issues message
  welcome-card <out.png> <name> <avatar.png|-> <number> [username]
                                       render a welcome card with the fonts and background in <dir>/welcome-art
  commands                             print the slash command definitions
The bot folder (default: the folder of this program) holds config.json, channels.json and the state files.";

fn exe_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_else(|| PathBuf::from("."))
}

fn rt() -> Result<tokio::runtime::Runtime, String> {
    tokio::runtime::Builder::new_current_thread().enable_all().build().map_err(|e| e.to_string())
}

fn install_crypto() {
    let _ = rustls::crypto::ring::default_provider().install_default();
}

fn real_main() -> Result<(), String> {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let mut dir = exe_dir();
    if args.first().map(String::as_str) == Some("--dir") {
        if args.len() < 2 {
            return Err(USAGE.into());
        }
        dir = PathBuf::from(args.remove(1));
        args.remove(0);
    }
    let cmd = args.first().cloned().unwrap_or_else(|| "run".into());
    let rest = &args[args.len().min(1)..];
    match cmd.as_str() {
        "run" => {
            let cfg = Config::load(&dir.join("config.json"))?;
            install_crypto();
            let bot = Arc::new(bot::Bot::new(cfg, dir)?);
            rt()?.block_on(bot::run(bot))
        }
        "post" => {
            if rest.is_empty() || rest.iter().any(|n| !post::POSTABLE.contains(&n.as_str())) {
                return Err(format!("post <channel...>: one or more of {}", post::POSTABLE.join(", ")));
            }
            let cfg = Config::load(&dir.join("config.json"))?;
            install_crypto();
            rt()?.block_on(post::run(&cfg, &dir, rest))
        }
        "live-counts" => {
            let files: Vec<&String> = rest.iter().filter(|a| !a.starts_with("--")).collect();
            let status = files.first().map(|s| PathBuf::from(s.as_str())).unwrap_or_else(|| util::expand_user("~/t2r-live/data/t2r-status.json"));
            let launchers = files
                .get(1)
                .map(|s| PathBuf::from(s.as_str()))
                .unwrap_or_else(|| status.parent().map(|p| p.join("t2r-launchers.json")).unwrap_or_else(|| PathBuf::from("t2r-launchers.json")));
            live::print_names(&status, &launchers, rest.iter().any(|a| a == "--at-snapshot"));
            Ok(())
        }
        "desk-check" => {
            let [reports, codes, title, first] = rest else { return Err(USAGE.into()) };
            let mut idx = desk::CodeIndex::new(PathBuf::from(reports), PathBuf::from(codes));
            idx.scan();
            let found = desk::find_codes(&format!("{title}\n{first}"));
            let (kind, code, entry) = desk::verdict(&found, &idx);
            let p = desk::plan(&kind, code.as_deref(), entry.as_ref(), title);
            let out = json!({"codes": found, "kind": format!("{kind:?}").to_lowercase(), "code": code, "reply": p.reply, "tag": p.tag, "rename": p.rename, "lock": p.lock});
            println!("{out}");
            Ok(())
        }
        "known-text" => {
            let [path] = rest else { return Err(USAGE.into()) };
            let items: Vec<serde_json::Value> = util::read_json(Path::new(path)).unwrap_or_default();
            println!("{}", desk::known_text(&items));
            Ok(())
        }
        "welcome-card" => {
            let (out, name, avatar, number) = match rest {
                [o, n, a, num, ..] => (o, n, a, num),
                _ => return Err(USAGE.into()),
            };
            let username = rest.get(4).cloned().unwrap_or_else(|| "survivor".into());
            let av = if avatar == "-" { Vec::new() } else { std::fs::read(avatar).map_err(|e| format!("{avatar}: {e}"))? };
            let n = number.parse::<u64>().ok().filter(|n| *n > 0);
            let png = card::render(&dir.join("welcome-art"), name, &av, n, &username)?;
            std::fs::write(out, png).map_err(|e| format!("{out}: {e}"))
        }
        "report-json" => {
            let mut st = serde_json::Map::new();
            st.insert("forum".into(), json!(10));
            st.insert("guide".into(), json!(11));
            let out: Vec<serde_json::Value> = [(st, Some(55)), (serde_json::Map::new(), None)]
                .iter()
                .map(|(st, user)| {
                    let m = desk::report_message(st, 1, *user);
                    let e = &m["embeds"][0];
                    let buttons: Vec<serde_json::Value> = m["components"].as_array().into_iter().flatten().flat_map(|r| r["components"].as_array().cloned().unwrap_or_default()).map(|b| b["url"].clone()).collect();
                    json!({"content": m.get("content"), "title": e["title"], "description": e["description"], "color": e["color"], "footer": e["footer"]["text"], "buttons": buttons})
                })
                .collect();
            println!("{}", serde_json::Value::Array(out));
            Ok(())
        }
        "commands" => {
            println!("{}", serde_json::to_string_pretty(&commands::definitions(true)).map_err(|e| e.to_string())?);
            Ok(())
        }
        _ => Err(USAGE.into()),
    }
}

fn main() -> ExitCode {
    match real_main() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}
