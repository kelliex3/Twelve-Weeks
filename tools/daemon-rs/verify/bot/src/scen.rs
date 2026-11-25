//! Scenarios. Each one is a sequential script (one thread) over a few bots; frames that arrive on their own (match start, countdown,
//! game start) are collected by the bots' reader threads and recorded per bot / connection (see rec.rs).
use crate::admin;
use crate::bot::{Bot, Ctx};
use std::sync::Arc;
use std::time::{Duration, Instant};

pub type Scn = fn(&Arc<Ctx>);

pub const NAMES: [&str; 8] = ["Alpha", "Bravo", "Cinder", "Delta", "Echo", "Frost", "Gamma", "Hotel"];

pub fn all() -> Vec<(&'static str, &'static str, Scn)> {
    let mut v: Vec<(&'static str, &'static str, Scn)> = vec![
        ("boot", "login 0x0f / announce 0x0e, helper 0xF0, online count 0x15, leaderboard 0x13 on an empty db, admin /status", boot),
        ("version", "0xB0 version report: match, mismatch, malformed, Linux marker; search gates (no helper, no version, wrong version)", version),
        ("searchleave", "search 0x01 and leave 0x02 (queue ack lists, dequeue, search again)", searchleave),
        ("match2", "a match of 2: countdown, vote 0xEA / 0xEC, loaded 0x09, game start 0x0a, roster 0xE3, end 0x12, clan report 0xE2", match2),
        ("match8", "a match of 8 with the same flow, then Elo (45 s), leaderboard 0x13, rank table 0xBF, admin /live /teams", match8),
        ("leavemid", "a player leaves mid-match (0x02, then a closed session), the match goes on; rejoin by searching again", leavemid),
        ("hostloss", "the host disconnects mid-match: host migration 0xE6 / 0xE7, helper loss, match end by the new host", hostloss),
    ];
    v.extend(crate::scen2::list());
    v
}

pub fn phase(ctx: &Ctx, name: &str) {
    ctx.rec.script(format!("== {}", name));
}

pub fn mkbots(ctx: &Arc<Ctx>, n: usize) -> Vec<Bot> {
    (0..n).map(|i| Bot::new(ctx, i + 1, NAMES[i % NAMES.len()])).collect()
}

/// Helper (0xF0), version report (0xB0) and the held menu session (0x0f + 0x0e), one bot after the other.
pub fn up_all(ctx: &Arc<Ctx>, bots: &mut [Bot]) {
    phase(ctx, "up: helper, version, boot");
    let ver = ctx.version.clone();
    for b in bots.iter_mut() {
        b.helper_up();
        b.version(&ver, false);
        b.boot();
    }
}

pub fn search_all(ctx: &Arc<Ctx>, bots: &[Bot]) {
    phase(ctx, "search");
    for b in bots {
        b.search();
    }
}

/// Every bot got a match record (0x04) after its mark.
pub fn wait_match(bots: &[Bot], marks: &[usize], secs: u64) -> bool {
    let end = Instant::now() + Duration::from_secs(secs);
    let mut ok = true;
    for (b, m) in bots.iter().zip(marks) {
        let left = end.saturating_duration_since(Instant::now()).as_secs().max(1);
        ok &= b.wait_op(*m, "sess", 0x04, left).is_some();
    }
    ok
}

pub fn marks(bots: &[Bot]) -> Vec<usize> {
    bots.iter().map(|b| b.mark()).collect()
}

/// The vote window: every bot reads the state (0xEA), casts a vote (0xEC) and reads it back.
pub fn vote(ctx: &Arc<Ctx>, bots: &[Bot], marks: &[usize], choices: &[u8]) -> bool {
    phase(ctx, "vote");
    let mut ok = true;
    for (b, m) in bots.iter().zip(marks) {
        // the first vote step: 0x0c [2][160 + n]
        let got = b.sh.wait(*m, Duration::from_secs(60), |h| h.stream == "sess" && h.op == 0x0c && h.pl.len() == 2 && h.pl[0] == 2 && (161..200).contains(&h.pl[1]));
        ok &= got.is_some();
    }
    for (i, b) in bots.iter().enumerate() {
        b.rpc(0xEA, b.token.as_bytes(), Some(0xEB));
        let mut pl = b.token.as_bytes().to_vec();
        pl.push(choices[i % choices.len()]);
        b.rpc(0xEC, &pl, Some(0xEB));
    }
    for b in bots {
        b.rpc(0xEA, b.token.as_bytes(), Some(0xEB));
    }
    ok
}

/// The level start step (2, 0) reached every bot.
pub fn wait_levelstart(bots: &[Bot], marks: &[usize], secs: u64) -> bool {
    let end = Instant::now() + Duration::from_secs(secs);
    let mut ok = true;
    for (b, m) in bots.iter().zip(marks) {
        let left = end.saturating_duration_since(Instant::now()).as_secs().max(1);
        ok &= b.sh.wait(*m, Duration::from_secs(left), |h| h.stream == "sess" && h.op == 0x0c && h.pl == [2, 0]).is_some();
    }
    ok
}

/// Every bot sends 0x09; waits for 0x0a on each.
pub fn load_all(ctx: &Arc<Ctx>, bots: &[Bot], marks: &[usize]) -> bool {
    phase(ctx, "loaded -> game start");
    for b in bots {
        b.loaded();
        std::thread::sleep(Duration::from_millis(150)); // one connection thread per bot on the daemon: keep their order
    }
    let mut ok = true;
    for (b, m) in bots.iter().zip(marks) {
        ok &= b.wait_op(*m, "sess", 0x0a, 30).is_some();
    }
    // the daemon sends 0x0a first and books the start (stats, clan day, host migration, mode) right after: let that finish before the next request
    std::thread::sleep(Duration::from_millis(1500));
    ok
}

/// 0xE3 roster / clock report of one bot: flags bit0 = clock valid.
pub fn e3(b: &Bot, flags: u8, left: f32, dur: f32, team: u8, ally: u8, enemy: u8) -> Vec<(u8, Vec<u8>)> {
    let mut pl = b.token.as_bytes().to_vec();
    pl.push(flags);
    pl.extend_from_slice(&left.to_le_bytes());
    pl.extend_from_slice(&dur.to_le_bytes());
    pl.push(team);
    pl.push(ally);
    pl.push(enemy);
    b.rpc(0xE3, &pl, Some(0xE5))
}

pub fn e6(b: &Bot, flags: u8, epoch: u8) -> Vec<(u8, Vec<u8>)> {
    let mut pl = b.token.as_bytes().to_vec();
    pl.push(flags);
    pl.push(epoch);
    b.rpc(0xE6, &pl, Some(0xE7))
}

/// 0xE2 match report: [tok][u32 seq][u32 parts][u8 flags]
pub fn e2(b: &Bot, seq: u32, parts: u32, flags: u8, counts: &[(u8, u16)]) -> Vec<(u8, Vec<u8>)> {
    let mut pl = b.token.as_bytes().to_vec();
    pl.extend_from_slice(&seq.to_le_bytes());
    pl.extend_from_slice(&parts.to_le_bytes());
    pl.push(flags);
    if !counts.is_empty() {
        pl.push(counts.len() as u8);
        for (l, c) in counts {
            pl.push(*l);
            pl.extend_from_slice(&c.to_le_bytes());
        }
    }
    b.rpc(0xE2, &pl, Some(0xE1))
}

pub fn tok_op(b: &Bot, op: u8, extra: &[u8], expect: u8) -> Vec<(u8, Vec<u8>)> {
    let mut pl = b.token.as_bytes().to_vec();
    pl.extend_from_slice(extra);
    b.rpc(op, &pl, Some(expect))
}

/// Settles the version table / status file, takes the final snapshot.
pub fn finish(ctx: &Arc<Ctx>, bots: &mut Vec<Bot>) {
    phase(ctx, "end");
    admin::wait_versions(ctx, 30);
    admin::file_json(ctx, "versions", "client_versions.json");
    admin::file_json(ctx, "linux", "linux_clients.json");
    admin::snapshot(ctx, "end");
    admin::call_rec(ctx, "end", "GET", "/live", None, true);
    for b in bots.iter_mut() {
        b.kill();
        std::thread::sleep(Duration::from_millis(400)); // one close at a time: the daemon logs them in this order
    }
}

/// The whole match flow for the bots that are all searching: formed -> vote -> level start -> loaded -> game start.
pub fn run_to_game_start(ctx: &Arc<Ctx>, bots: &[Bot], choices: &[u8]) -> bool {
    let m = marks(bots);
    search_all(ctx, bots);
    phase(ctx, "match formed");
    let mut ok = wait_match(bots, &m, 120);
    ok &= vote(ctx, bots, &m, choices);
    ok &= wait_levelstart(bots, &m, 90);
    ok &= load_all(ctx, bots, &m);
    ctx.rec.script(format!("flow ok={}", ok));
    ok
}

/// Host = the bot whose match record says slot 0.
pub fn host_index(bots: &[Bot]) -> usize {
    bots.iter().position(|b| b.my_slot() == Some(0)).unwrap_or(0)
}

// ------------------------------------------------------------------------------------------------ scenarios

fn boot(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 2);
    phase(ctx, "admin status before any player");
    admin::call_rec(ctx, "pre", "GET", "/status", None, false);
    phase(ctx, "forbidden without the secret");
    admin::call_rec(ctx, "nosecret", "GET", "/live", None, false);
    up_all(ctx, &mut bots);
    phase(ctx, "online count, leaderboard");
    for b in &bots {
        b.poll_online();
        b.rpc(0x13, &[], Some(0x14));
    }
    phase(ctx, "second login of the same token on a short connection (a profile frame refresh)");
    let b = &bots[0];
    let mut pl = vec![0x1c];
    pl.extend_from_slice(b.token.as_bytes());
    pl.extend_from_slice(&[0x01, 0x02, 0x07]);
    b.rpc(0x0f, &pl, Some(0x0d));
    finish(ctx, &mut bots);
    admin::call_rec(ctx, "post", "GET", "/status", None, false);
}

fn version(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    phase(ctx, "searching with no helper and no version report");
    {
        let b = &mut bots[0];
        b.boot();
        b.search();
        std::thread::sleep(Duration::from_millis(800));
    }
    phase(ctx, "helper but no version report (REQUIRE_VERSION)");
    {
        let b = &mut bots[1];
        b.helper_up();
        b.boot();
        b.search();
        std::thread::sleep(Duration::from_millis(800));
    }
    phase(ctx, "helper + matching version");
    let ver = ctx.version.clone();
    {
        let b = &mut bots[2];
        b.helper_up();
        b.version(&ver, false);
        b.boot();
        b.search();
        b.leave();
    }
    phase(ctx, "wrong version, malformed, long, empty, Linux marker");
    {
        let b = &mut bots[3];
        b.helper_up();
        b.version("cz1000x", false);
        b.rpc(0xB0, &[0], Some(0xB1));
        b.rpc(0xB0, &[3, b'a', b'b'], Some(0xB1));
        b.rpc(0xB0, &[4, b'a', b' ', b'b', b'c'], Some(0xB1));
        b.version("cz1117v", true);
        b.version(&ver, false);
        b.boot();
    }
    phase(ctx, "wrong-version player searches");
    {
        let b = &mut bots[3];
        b.version("cz1000x", false);
        b.search();
        std::thread::sleep(Duration::from_millis(800));
    }
    finish(ctx, &mut bots);
}

fn searchleave(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 3);
    up_all(ctx, &mut bots);
    phase(ctx, "A searches alone");
    bots[0].search();
    bots[0].poll();
    phase(ctx, "B searches: both see two names");
    bots[1].search();
    bots[0].poll();
    bots[1].poll();
    phase(ctx, "B leaves (0x02)");
    bots[1].leave();
    bots[0].poll();
    phase(ctx, "B searches again, C searches, A leaves");
    bots[1].search();
    bots[2].search();
    bots[0].leave();
    bots[1].poll();
    bots[2].poll();
    phase(ctx, "online count and mode queue counts");
    for b in &bots {
        tok_op(b, 0xBC, &[0, 0], 0xBD);
    }
    admin::call_rec(ctx, "mid", "GET", "/status", None, false);
    finish(ctx, &mut bots);
}

fn match_flow(ctx: &Arc<Ctx>, n: usize, with_elo: bool) {
    let mut bots = mkbots(ctx, n);
    up_all(ctx, &mut bots);
    let ok = run_to_game_start(ctx, &bots, &[0, 1, 2]);
    phase(ctx, "in the match: roster / clock polls, host migration poll");
    let h = host_index(&bots);
    ctx.rec.script(format!("host index {} ok={}", h, ok));
    for (i, b) in bots.iter().enumerate() {
        e3(b, 1, 590.0, 600.0, (i % 2) as u8, 20, 20);
    }
    for b in &bots {
        e3(b, 1, 560.0, 600.0, 0xFF, 0xFF, 0xFF);
    }
    e6(&bots[h], 0, 0);
    e6(&bots[(h + 1) % n], 0, 0);
    admin::snapshot(ctx, "in match");
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    admin::call_rec(ctx, "mid", "GET", "/teams", None, true);
    admin::call_rec(ctx, "mid", "GET", "/sizes", None, true);
    for b in &bots {
        tok_op(b, 0xDE, &[0, 0], 0xDF);
    }
    phase(ctx, "match end: the host reports team 0 won");
    bots[h].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    phase(ctx, "clan reports (match end seen)");
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 120 + 10 * i as u32, 2, &[]);
    }
    phase(ctx, "after the end: roster poll");
    e3(&bots[0], 2, 0.0, 0.0, 0, 0, 0);
    if with_elo {
        phase(ctx, "wait: Elo update (45 s) and the match is freed (9 s)");
        std::thread::sleep(Duration::from_secs(48));
        phase(ctx, "leaderboard, rank table, clan read");
        for b in bots.iter().take(2) {
            b.rpc(0x13, &[], Some(0x14));
            tok_op(b, 0xBF, &[], 0xBF);
            tok_op(b, 0xE0, &[], 0xE1);
        }
        admin::call_rec(ctx, "after", "GET", "/live", None, true);
    } else {
        std::thread::sleep(Duration::from_secs(11));
    }
    finish(ctx, &mut bots);
}

fn match2(ctx: &Arc<Ctx>) {
    match_flow(ctx, 2, false);
}

fn match8(ctx: &Arc<Ctx>) {
    match_flow(ctx, 8, true);
}

fn leavemid(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    run_to_game_start(ctx, &bots, &[0, 1]);
    for b in &bots {
        e3(b, 1, 580.0, 600.0, 0, 20, 20);
    }
    phase(ctx, "Delta leaves with 0x02 (the others play on)");
    bots[3].leave();
    std::thread::sleep(Duration::from_millis(1500));
    e3(&bots[0], 1, 570.0, 600.0, 0, 19, 20);
    phase(ctx, "Delta's session closes");
    bots[3].drop_sess();
    std::thread::sleep(Duration::from_millis(1500));
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    phase(ctx, "Delta searches again (join in progress or a new queue)");
    let m = bots[3].mark();
    bots[3].boot();
    bots[3].search();
    let got = bots[3].wait_op(m, "sess", 0x04, 25).is_some();
    ctx.rec.script(format!("delta rejoin record={}", got));
    if got {
        e3(&bots[0], 1, 560.0, 600.0, 0, 19, 20);
        e3(&bots[3], 4, 560.0, 600.0, 0, 19, 20);
    }
    phase(ctx, "match end");
    bots[0].match_end(1);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 60 + i as u32, 2, &[]);
    }
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn hostloss(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    run_to_game_start(ctx, &bots, &[0, 1, 2]);
    for b in &bots {
        e3(b, 1, 580.0, 600.0, 0, 20, 20);
    }
    let h = host_index(&bots);
    phase(ctx, "host migration poll before the loss");
    for b in &bots {
        e6(b, 0, 0);
    }
    phase(ctx, "the host's helper drops, then its session");
    bots[h].drop_bridge();
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        if i != h {
            e6(b, 0, 0);
        }
    }
    bots[h].drop_sess();
    bots[h].kill();
    std::thread::sleep(Duration::from_secs(3));
    phase(ctx, "migration polls after the loss: the survivors report the host silent (flag 1), 12 rounds 2 s apart");
    let mut new_host_slot: u8 = 0;
    let mut epoch: u8 = 0;
    for _round in 0..12u8 {
        for (i, b) in bots.iter().enumerate() {
            if i != h {
                let r = e6(b, 1 | 2, epoch);
                if let Some((_, pl)) = r.first() {
                    if pl.len() >= 5 && pl[0] == 1 {
                        new_host_slot = pl[1];
                        epoch = pl[2];
                    }
                }
            }
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    ctx.rec.script(format!("host slot after the loss: {}", new_host_slot));
    admin::snapshot(ctx, "after host loss");
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    phase(ctx, "the new host (or a survivor) ends the match");
    let nh = bots.iter().enumerate().position(|(i, b)| i != h && b.my_slot() == Some(new_host_slot)).unwrap_or((h + 1) % bots.len());
    bots[nh].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        if i != h {
            e2(b, 1, 50 + i as u32, 2, &[]);
        }
    }
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}
