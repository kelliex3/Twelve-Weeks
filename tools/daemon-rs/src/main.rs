#![allow(dead_code)]
mod clan;
mod clanw;
mod clock;
mod d;
mod log;
mod net;
mod py;
mod pyfmt;
mod rnd;
mod selftest;
mod udp;
mod util;

use std::io::{self, BufRead, Write};
use std::net::SocketAddrV4;
use std::path::PathBuf;
use std::str::FromStr;

pub fn env_parse<T: FromStr>(key: &str, default: T) -> T {
    match std::env::var(key) {
        Ok(v) => v.trim().parse().unwrap_or(default),
        Err(_) => default,
    }
}

pub fn env_str(key: &str, default: &str) -> String {
    std::env::var(key).unwrap_or_else(|_| default.to_string())
}

/// `os.environ.get(key, default) != "0"` style switches: on unless the variable is exactly "0" (default when unset).
pub fn env_on(key: &str, default: bool) -> bool {
    match std::env::var(key) {
        Ok(v) => v != "0",
        Err(_) => default,
    }
}

/// `os.environ.get(key, "0") == "1"` style switches: on only when exactly "1".
pub fn env_is1(key: &str, default: bool) -> bool {
    match std::env::var(key) {
        Ok(v) => v == "1",
        Err(_) => default,
    }
}

fn log_config(default_name: &str) -> log::Config {
    let path = match std::env::var("T2R_LOG_PATH") {
        Ok(p) if !p.is_empty() => PathBuf::from(p),
        _ => std::env::current_dir().unwrap_or_default().join(default_name),
    };
    log::Config {
        path,
        max_bytes: env_parse("T2R_LOG_MAX_BYTES", 8 * 1024 * 1024u64),
        keep: env_parse("T2R_LOG_KEEP", 5u32),
        burst: env_parse("T2R_LOG_BURST", 200.0),
        per_s: env_parse("T2R_LOG_PER_S", 40.0),
    }
}

fn run_relay() -> i32 {
    log::init(log_config("relay.log"));
    let host = env_str("T2R_HOST", "0.0.0.0");
    let port: u16 = env_parse("T2R_PORT", 7001u16);
    let lim = udp::Limits::from_env();
    dlog!("daemon", "relay start", env!("CARGO_PKG_VERSION"));
    net::udp_server(&host, port, lim)
}

struct ReplayNet {
    failing: std::collections::HashSet<SocketAddrV4>,
    out: Vec<String>,
}

impl udp::Net for ReplayNet {
    fn send(&mut self, data: &[u8], to: SocketAddrV4) -> io::Result<()> {
        if self.failing.contains(&to) {
            return Err(io::Error::from_raw_os_error(libc::ENETUNREACH));
        }
        self.out.push(format!("S {} {}", to, pyfmt::hex_sp(data).replace(' ', "")));
        Ok(())
    }
}

fn hex_decode(s: &str) -> Option<Vec<u8>> {
    if s == "-" {
        return Some(Vec::new());
    }
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).ok()).collect()
}

fn run_replay_udp() -> i32 {
    log::init_capture();
    let mut relay = udp::Relay::new(udp::Limits::from_env());
    relay.sock_stats = "sockbuf rcv 0 KB snd 0 KB".to_string();
    let mut net = ReplayNet { failing: Default::default(), out: Vec::new() };
    let stdin = io::stdin();
    let stdout = io::stdout();
    let mut w = io::BufWriter::new(stdout.lock());
    for line in stdin.lock().lines() {
        let Ok(line) = line else { break };
        let f: Vec<&str> = line.split(' ').collect();
        match f.as_slice() {
            ["P", t, a, d] => {
                let (Ok(t), Ok(a), Some(d)) = (t.parse::<f64>(), a.parse::<SocketAddrV4>(), hex_decode(d)) else {
                    eprintln!("bad line: {}", line);
                    return 2;
                };
                relay.handle(&d, a, t, &mut net);
            }
            ["T", _] => {
                relay.status_health();
                relay.status_stats();
            }
            ["F", a] => {
                if let Ok(a) = a.parse() {
                    net.failing.insert(a);
                }
            }
            ["U", a] => {
                if let Ok(a) = a.parse() {
                    net.failing.remove(&a);
                }
            }
            _ => {
                eprintln!("bad line: {}", line);
                return 2;
            }
        }
        let logs = log::take_captured();
        for s in net.out.drain(..) {
            let _ = writeln!(w, "{}", s);
        }
        for l in logs {
            let _ = writeln!(w, "L {}", l);
        }
    }
    let _ = w.flush();
    0
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let code = match args.get(1).map(|s| s.as_str()) {
        Some("relay") => run_relay(),
        Some("replay-udp") => run_replay_udp(),
        Some("selftest") => {
            log::init_capture();
            let m = args.get(2).cloned().unwrap_or_default();
            let stdin = io::stdin();
            let stdout = io::stdout();
            let mut w = io::BufWriter::new(stdout.lock());
            let c = selftest::run(&m, &mut stdin.lock(), &mut w);
            let _ = w.flush();
            c
        }
        _ => {
            eprintln!("usage: t2r-daemon relay | replay-udp < trace | selftest <module> < script");
            2
        }
    };
    std::process::exit(code);
}
