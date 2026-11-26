//! Shadow load: G groups of SIZE bots play matches in a loop (helper, version, boot, search, vote, load, game start, roster polls, host
//! match end, clan reports, menu reads), IDLE more bots sit in the menu polling (keepalive every 1.5 s, leaderboard / rank table / clan read
//! every ~20 s). Nothing is recorded; a counter line goes to stdout every 30 s. Each bot has its own token / Steam id (idx = group * 100 + i).
use crate::bot::{Bot, Ctx};
use crate::scen;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

static CYCLES: AtomicU64 = AtomicU64::new(0);
static OK: AtomicU64 = AtomicU64::new(0);
static FAILS: AtomicU64 = AtomicU64::new(0);
static MENU_RPCS: AtomicU64 = AtomicU64::new(0);
static EMPTY_RPCS: AtomicU64 = AtomicU64::new(0);

fn arg(args: &[String], name: &str) -> Option<String> {
    args.iter().position(|a| a == name).and_then(|i| args.get(i + 1)).cloned()
}

fn menu_reads(b: &Bot) {
    for (op, pl, exp) in [(0x13u8, Vec::new(), 0x14u8), (0xBF, b.token.as_bytes().to_vec(), 0xBF), (0xE0, b.token.as_bytes().to_vec(), 0xE1)] {
        let r = b.rpc(op, &pl, Some(exp));
        MENU_RPCS.fetch_add(1, Ordering::Relaxed);
        if r.is_empty() {
            EMPTY_RPCS.fetch_add(1, Ordering::Relaxed);
        }
    }
}

fn group(ctx: Arc<Ctx>, g: usize, size: usize, stop: Arc<AtomicBool>) {
    let mut round = 0usize;
    while !stop.load(Ordering::Relaxed) {
        round += 1;
        let mut bots: Vec<Bot> = (0..size).map(|i| Bot::new(&ctx, g * 100 + i + 1, &format!("L{}x{}", g, i))).collect();
        for b in bots.iter_mut() {
            b.helper_up();
            b.version(&ctx.version, false);
            b.boot();
        }
        let ok = scen::run_to_game_start(&ctx, &bots, &[0, 1, 2]);
        CYCLES.fetch_add(1, Ordering::Relaxed);
        if ok {
            OK.fetch_add(1, Ordering::Relaxed);
            let h = scen::host_index(&bots);
            for _ in 0..6 {
                for (i, b) in bots.iter().enumerate() {
                    scen::e3(b, 1, 500.0, 600.0, (i % 2) as u8, 20, 20);
                    scen::e6(b, 0, 0);
                }
                std::thread::sleep(Duration::from_secs(5));
            }
            bots[h].match_end((round % 2) as u8);
            std::thread::sleep(Duration::from_millis(1500));
            for (i, b) in bots.iter().enumerate() {
                scen::e2(b, 1, 100 + 10 * i as u32, 2, &[]);
            }
            std::thread::sleep(Duration::from_secs(10));
        } else {
            FAILS.fetch_add(1, Ordering::Relaxed);
            for b in &bots {
                b.leave();
            }
        }
        for b in bots.iter().take(3) {
            menu_reads(b);
        }
        for b in bots.iter_mut() {
            b.kill();
        }
        std::thread::sleep(Duration::from_secs(3));
    }
}

fn idle(ctx: Arc<Ctx>, i: usize, stop: Arc<AtomicBool>) {
    let mut b = Bot::new(&ctx, 9000 + i, &format!("Idle{}", i));
    b.helper_up();
    b.version(&ctx.version, false);
    b.boot();
    let mut n = 0u64;
    while !stop.load(Ordering::Relaxed) {
        std::thread::sleep(Duration::from_secs(1));
        n += 1;
        if n % 20 == (i as u64 % 20) {
            menu_reads(&b);
        }
        if n % 600 == 0 {
            // reconnect now and then (a menu restart)
            b.kill();
            b = Bot::new(&ctx, 9000 + i, &format!("Idle{}", i));
            b.helper_up();
            b.version(&ctx.version, false);
            b.boot();
        }
    }
    b.kill();
}

pub fn run(args: &[String]) {
    let rec = Arc::new(crate::rec::Rec::default());
    rec.off.store(true, Ordering::Relaxed);
    let ctx = Arc::new(Ctx {
        host: arg(args, "--host").unwrap_or_else(|| "127.0.0.1".into()),
        port: arg(args, "--port").and_then(|v| v.parse().ok()).unwrap_or(7301),
        admin_port: arg(args, "--admin-port").and_then(|v| v.parse().ok()).unwrap_or(7311),
        workdir: ".".into(),
        rec,
        version: arg(args, "--version").unwrap_or_else(|| "cz1117v".into()),
        pings: Default::default(),
        versions: Default::default(),
        linux: Default::default(),
    });
    let groups: usize = arg(args, "--groups").and_then(|v| v.parse().ok()).unwrap_or(8);
    let size: usize = arg(args, "--size").and_then(|v| v.parse().ok()).unwrap_or(8);
    let nidle: usize = arg(args, "--idle").and_then(|v| v.parse().ok()).unwrap_or(40);
    let secs: u64 = arg(args, "--secs").and_then(|v| v.parse().ok()).unwrap_or(3600);
    let stop = Arc::new(AtomicBool::new(false));
    let mut hs = Vec::new();
    for i in 0..nidle {
        let (c, s) = (ctx.clone(), stop.clone());
        hs.push(std::thread::Builder::new().stack_size(256 * 1024).spawn(move || idle(c, i, s)).unwrap());
        std::thread::sleep(Duration::from_millis(100));
    }
    for g in 0..groups {
        let (c, s) = (ctx.clone(), stop.clone());
        hs.push(std::thread::Builder::new().stack_size(256 * 1024).spawn(move || group(c, g, size, s)).unwrap());
        std::thread::sleep(Duration::from_secs(7)); // staggered, so groups mostly form their own matches
    }
    let t0 = Instant::now();
    while t0.elapsed().as_secs() < secs {
        std::thread::sleep(Duration::from_secs(30));
        println!(
            "t={} cycles={} ok={} fail={} menu_rpcs={} empty={}",
            t0.elapsed().as_secs(),
            CYCLES.load(Ordering::Relaxed),
            OK.load(Ordering::Relaxed),
            FAILS.load(Ordering::Relaxed),
            MENU_RPCS.load(Ordering::Relaxed),
            EMPTY_RPCS.load(Ordering::Relaxed)
        );
    }
    stop.store(true, Ordering::Relaxed);
    for h in hs {
        let _ = h.join();
    }
    println!("done");
}
