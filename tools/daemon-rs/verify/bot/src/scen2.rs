//! More scenarios: names, notices, friends and parties, clan / boosters / achievements, admin calls, gun limits, host pick by ping,
//! Interrogation, performance / mode queue / map support / hardware ids, bans, join in progress, session re-attach.
use crate::admin;
use crate::bot::{Bot, Ctx};
use crate::scen::*;
use serde_json::Value;
use std::sync::Arc;
use std::time::Duration;

pub fn list() -> Vec<(&'static str, &'static str, Scn)> {
    vec![
        ("names", "custom names and clan tags 0xD0 / 0xD2: set, taken, bad request, clear; name refresh on the session", names),
        ("notices", "notices 0xBA / 0xBB with admin POST /notice, /notice/clear, /shutdown, /shutdown/cancel; GET /notices; menu notes 0x11", notices),
        ("friends", "friends 0xD8: search, add, accept, message, chat, remove, party chat, lobby chat, mute poll", friends),
        ("party", "party 0xD8 invite / accept / ready, the party searches together and plays a match; old party op 0xDA", party),
        ("clan", "clan 0xE0 / 0xE4 / 0xE2 / 0xE8 / 0xEE / 0xEF, feed 0xB4, clan page 0xB6, achievements 0xAA / 0xA9 / 0xAB, over a played match", clan),
        ("admin", "admin listener: /status /live /notices /sizes /teams, POST /teams, /teams/move, /kick, /boot-all, /reset-session, bad secret", admin_calls),
        ("gunlimit", "gun limits 0xDE: claims, denials per team, release at the match end", gunlimit),
        ("hostpick", "host pick by Steam ping (0xF4 / 0xF5 / 0xF6): the best-connected player gets slot 0", hostpick),
        ("pinggate", "ping gate: a player 400 ms from everybody is held out of the lobby of three near players", pinggate),
        ("interro", "Interrogation mode: mode queue 0xBC picks, match in the mode, 0xDC poll / interrogated / lockbox stage / died", interro),
        ("perfmisc", "performance 0xCA, mode queue 0xBC, SP maps 0xC8, map packs 0xC6, private lobby 0xF8, hardware ids 0xB2, attest 0xA1, bad frames", perfmisc),
        ("ban", "bans.txt: banned by Steam id and by name at login and at search; ban reason text", ban),
        ("jip", "join in progress: a late searcher is put into the running match (0xE3 clock reports, 0x04 record, countdown, game start)", jip),
        ("handover", "planned host handover (admin POST /handover, T2R_HOST_REPICK=2): 0xE6 / 0xE7 epochs and alive masks while the host changes", handover),
        ("reattach", "the session socket drops during the countdown and the client logs in again (session re-attach)", reattach),
    ]
}

fn tk(b: &Bot, extra: &[u8]) -> Vec<u8> {
    let mut v = b.token.as_bytes().to_vec();
    v.extend_from_slice(extra);
    v
}

fn pid_of(ctx: &Ctx, name: &str) -> String {
    let (_, txt) = admin::call(ctx, "GET", "/live", None, true);
    let v: Value = serde_json::from_str(&txt).unwrap_or(Value::Null);
    v.pointer("/live/players")
        .and_then(|p| p.as_array())
        .and_then(|a| a.iter().find(|x| x.get("name").and_then(|n| n.as_str()) == Some(name)))
        .and_then(|x| x.get("pid").and_then(|p| p.as_str()))
        .unwrap_or("")
        .to_string()
}

fn d8(b: &Bot, lang: u8, sub: u8, arg: &str) -> Vec<(u8, Vec<u8>)> {
    let mut pl = b.token.as_bytes().to_vec();
    pl.extend_from_slice(&[1, lang, sub]);
    pl.extend_from_slice(arg.as_bytes());
    b.rpc(0xD8, &pl, Some(0xD9))
}

fn names(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 2);
    up_all(ctx, &mut bots);
    phase(ctx, "read the empty names");
    tok_op(&bots[0], 0xD0, &[], 0xD1);
    phase(ctx, "set name + tag");
    let set = |b: &Bot, n: &str, t: &str| {
        let mut x = vec![n.len() as u8];
        x.extend_from_slice(n.as_bytes());
        x.push(t.len() as u8);
        x.extend_from_slice(t.as_bytes());
        tok_op(b, 0xD2, &x, 0xD1)
    };
    set(&bots[0], "Zed", "AB");
    tok_op(&bots[0], 0xD0, &[], 0xD1);
    phase(ctx, "the other player wants the same name");
    set(&bots[1], "Zed", "CD");
    set(&bots[1], "zed", "");
    phase(ctx, "other names: too short, spaces, long, odd characters, tag only");
    set(&bots[1], "A", "");
    set(&bots[1], "Two Words", "");
    set(&bots[1], "AVeryLongNameThatIsTooLong", "TOOLONGTAG");
    set(&bots[1], "Bad<Name>", "");
    set(&bots[1], "", "TG");
    set(&bots[1], "Quinn", "QQ");
    phase(ctx, "malformed request");
    bots[1].rpc(0xD2, &tk(&bots[1], &[9, b'x']), Some(0xD1));
    phase(ctx, "clear");
    set(&bots[0], "", "");
    tok_op(&bots[0], 0xD0, &[], 0xD1);
    std::thread::sleep(Duration::from_secs(3)); // the held session polls: the name change reaches it as a 0x10
    finish(ctx, &mut bots);
}

fn notices(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 2);
    up_all(ctx, &mut bots);
    let ba = |b: &Bot, lang: u8, flags: u8| tok_op(b, 0xBA, &[0, lang, flags], 0xBB);
    phase(ctx, "no notices");
    ba(&bots[0], 0, 0);
    admin::call_rec(ctx, "n0", "GET", "/notices", None, true);
    phase(ctx, "admin notice, both languages");
    admin::call_rec(ctx, "n1", "POST", "/notice", Some(r#"{"text":"Server work tonight, short break","text_pl":"Prace na serwerze, krotka przerwa","minutes":30}"#), true);
    std::thread::sleep(Duration::from_millis(800));
    ba(&bots[0], 0, 0);
    ba(&bots[1], 1, 0);
    ba(&bots[1], 1, 1);
    phase(ctx, "second notice, seconds form; empty text");
    admin::call_rec(ctx, "n2", "POST", "/notice", Some(r#"{"text":"Second one","seconds":90}"#), true);
    admin::call_rec(ctx, "n3", "POST", "/notice", Some(r#"{"text":""}"#), true);
    ba(&bots[0], 0, 0);
    admin::call_rec(ctx, "n4", "GET", "/notices", None, true);
    phase(ctx, "clear the first");
    admin::call_rec(ctx, "n5", "POST", "/notice/clear", Some(r#"{"id":1}"#), true);
    admin::call_rec(ctx, "n5b", "POST", "/notice/clear", Some(r#"{"id":99}"#), true);
    ba(&bots[0], 0, 0);
    phase(ctx, "a planned restart, then cancelled");
    admin::call_rec(ctx, "n6", "POST", "/shutdown", Some(r#"{"minutes":5,"reason":"verify run","restart":true}"#), true);
    std::thread::sleep(Duration::from_millis(800));
    ba(&bots[0], 0, 0);
    ba(&bots[1], 1, 0);
    admin::call_rec(ctx, "n7", "GET", "/notices", None, true);
    admin::call_rec(ctx, "n8", "POST", "/shutdown/cancel", Some("{}"), true);
    ba(&bots[0], 0, 0);
    phase(ctx, "malformed poll, unknown token");
    bots[0].rpc(0xBA, &[1, 2, 3], Some(0xBB));
    bots[0].rpc(0xBA, &vec![b'z'; 64], Some(0xBB));
    std::thread::sleep(Duration::from_secs(1));
    finish(ctx, &mut bots);
}

fn friends(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 3);
    up_all(ctx, &mut bots);
    let key = |b: &Bot| format!("s:{}", b.sid);
    let (a, b, c) = (&bots[0], &bots[1], &bots[2]);
    phase(ctx, "state, search");
    d8(a, 0, 1, "");
    d8(a, 0, 2, "Bra");
    d8(a, 0, 2, "x");
    phase(ctx, "add by name, by key, self, unknown");
    d8(a, 0, 3, "Bravo");
    d8(a, 0, 3, "Bravo");
    d8(a, 0, 3, &key(a));
    d8(a, 0, 3, "Nobody");
    d8(a, 1, 3, &key(c));
    phase(ctx, "B sees the request and accepts; C removes and accepts");
    d8(b, 0, 1, "");
    d8(b, 0, 4, &key(a));
    d8(c, 0, 5, &key(a));
    d8(c, 0, 4, &key(a));
    phase(ctx, "private messages");
    d8(a, 0, 13, &format!("{}\nhello there", key(b)));
    d8(b, 0, 13, &format!("{}\nhi back <3", key(a)));
    d8(a, 0, 13, &format!("{}\n", key(b)));
    d8(a, 0, 13, &format!("{}\nnot a friend", key(c)));
    d8(a, 0, 14, &key(b));
    d8(b, 0, 14, &key(a));
    phase(ctx, "notice poll carries the friend lines (kind p)");
    tok_op(b, 0xBA, &[0, 0, 1], 0xBB);
    phase(ctx, "party chat without a party, lobby chat, mute poll, match log");
    d8(a, 0, 15, "hello party");
    d8(a, 0, 16, "0");
    d8(a, 0, 18, "hello lobby");
    d8(a, 0, 19, "0");
    d8(a, 0, 20, "");
    d8(a, 0, 17, "m\nsome match line");
    phase(ctx, "unknown sub, bad version byte");
    d8(a, 0, 99, "");
    b.rpc(0xD8, &tk(b, &[2, 0, 1]), Some(0xD9));
    phase(ctx, "remove, state");
    d8(a, 0, 5, &key(b));
    d8(a, 0, 1, "");
    d8(b, 0, 1, "");
    finish(ctx, &mut bots);
}

fn party(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    let key = |b: &Bot| format!("s:{}", b.sid);
    phase(ctx, "A and B become friends");
    d8(&bots[0], 0, 3, "Bravo");
    d8(&bots[1], 0, 4, &key(&bots[0]));
    phase(ctx, "A invites B; B joins");
    d8(&bots[0], 0, 6, &key(&bots[1]));
    d8(&bots[1], 0, 1, "");
    tok_op(&bots[1], 0xBA, &[0, 0, 1], 0xBB);
    d8(&bots[1], 0, 7, &key(&bots[0]));
    d8(&bots[0], 0, 1, "");
    phase(ctx, "party chat");
    d8(&bots[0], 0, 15, "party hello");
    d8(&bots[1], 0, 16, "0");
    phase(ctx, "B readies up, A (leader) searches, B follows, C and D search");
    d8(&bots[1], 0, 12, "1");
    let m = marks(&bots);
    bots[0].search();
    bots[1].search();
    bots[2].search();
    bots[3].search();
    let ok = wait_match(&bots, &m, 120);
    ctx.rec.script(format!("party match formed ok={}", ok));
    let mut ok2 = vote(ctx, &bots, &m, &[0, 1, 2]);
    ok2 &= wait_levelstart(&bots, &m, 90);
    ok2 &= load_all(ctx, &bots, &m);
    ctx.rec.script(format!("party flow ok={}", ok2));
    admin::call_rec(ctx, "mid", "GET", "/teams", None, true);
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    phase(ctx, "match end, party stays");
    let h = host_index(&bots);
    bots[h].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 90 + i as u32, 2, &[]);
    }
    d8(&bots[0], 0, 1, "");
    phase(ctx, "promote, kick, leave, old party op 0xDA");
    d8(&bots[0], 0, 11, &key(&bots[1]));
    d8(&bots[0], 0, 10, &key(&bots[1]));
    d8(&bots[0], 0, 9, "");
    d8(&bots[1], 0, 1, "");
    let da = |b: &Bot, sub: u8, arg: &str| {
        let mut x = vec![sub];
        x.extend_from_slice(arg.as_bytes());
        tok_op(b, 0xDA, &x, 0xDB)
    };
    da(&bots[2], 0, "");
    da(&bots[2], 1, "Delta");
    da(&bots[3], 2, "Cinder");
    da(&bots[3], 3, "");
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn clan(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 2);
    up_all(ctx, &mut bots);
    phase(ctx, "reads before any clan exists");
    tok_op(&bots[0], 0xE0, &[], 0xE1);
    tok_op(&bots[0], 0xE8, &[], 0xE9);
    tok_op(&bots[0], 0xAA, &[], 0xAA);
    phase(ctx, "choose a side (0xE4), bad side, read, boosters, mission, booster pick");
    tok_op(&bots[0], 0xE4, &[1], 0xE1);
    tok_op(&bots[1], 0xE4, &[2], 0xE1);
    tok_op(&bots[1], 0xE4, &[1], 0xE1);
    tok_op(&bots[0], 0xE4, &[9], 0xE1);
    tok_op(&bots[0], 0xE0, &[], 0xE1);
    tok_op(&bots[0], 0xE8, &[], 0xE9);
    tok_op(&bots[0], 0xEE, &[1], 0xE9);
    tok_op(&bots[0], 0xEF, &[1], 0xE9);
    tok_op(&bots[0], 0xE8, &[], 0xE9);
    phase(ctx, "feed and clan pages before a match (early in the run: the ticker slot is the clock minute)");
    for b in &bots {
        tok_op(b, 0xB4, &[0], 0xB5);
    }
    tok_op(&bots[1], 0xB4, &[1], 0xB5);
    for sec in 1..=5u8 {
        tok_op(&bots[0], 0xB6, &[0, sec, 0, 0, 0, 0, 0, 0, 1], 0xB7);
    }
    phase(ctx, "report without a match");
    e2(&bots[0], 7, 50, 2, &[]);
    phase(ctx, "match");
    run_to_game_start(ctx, &bots, &[0, 1]);
    phase(ctx, "live achievement counters, report with counts, a re-send");
    let ab = |b: &Bot, cnt: &[(u8, u16)]| {
        let mut x = vec![cnt.len() as u8];
        for (l, c) in cnt {
            x.push(*l);
            x.extend_from_slice(&c.to_le_bytes());
        }
        tok_op(b, 0xAB, &x, 0xAB)
    };
    ab(&bots[0], &[(1, 3), (2, 1)]);
    let h = host_index(&bots);
    bots[h].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    e2(&bots[0], 1, 340, 2, &[(21, 2), (22, 1), (23, 4), (24, 3), (33, 2)]);
    e2(&bots[0], 1, 340, 2, &[(21, 2), (22, 1), (23, 4), (24, 3), (33, 2)]);
    e2(&bots[1], 1, 20, 3, &[]);
    phase(ctx, "achievement report 0xA9 and the reads after the day");
    let a9 = |b: &Bot| {
        let mut x = 1u32.to_le_bytes().to_vec();
        x.push(2);
        x.extend_from_slice(&[1, 5, 0, 2, 9, 0]);
        tok_op(b, 0xA9, &x, 0xA9)
    };
    a9(&bots[0]);
    for b in &bots {
        tok_op(b, 0xAA, &[], 0xAA);
        tok_op(b, 0xE0, &[], 0xE1);
        tok_op(b, 0xE8, &[], 0xE9);
    }
    tok_op(&bots[0], 0xB4, &[0], 0xB5);
    for sec in 1..=5u8 {
        tok_op(&bots[0], 0xB6, &[0, sec, 0, 0, 0, 0, 0, 0, 1], 0xB7);
    }
    tok_op(&bots[1], 0xB6, &[1, 2, 0, 0, 0, 0, 0, 0, 0], 0xB7);
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn admin_calls(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    phase(ctx, "no secret, unknown path, bad body");
    {
        let (code, body) = admin::call(ctx, "GET", "/live", None, false);
        ctx.rec.script(format!("admin nosecret GET /live -> {} {}", code, body));
        let (code, body) = admin::call(ctx, "POST", "/kick", Some("{}"), false);
        ctx.rec.script(format!("admin nosecret POST /kick -> {} {}", code, body));
    }
    admin::call_rec(ctx, "x", "GET", "/nope", None, true);
    admin::call_rec(ctx, "x", "POST", "/nope", Some("{}"), true);
    admin::call_rec(ctx, "x", "POST", "/notice", Some("not json"), true);
    admin::call_rec(ctx, "x", "GET", "/clan/backups", None, true);
    phase(ctx, "reads with players in the menu");
    for p in ["/status", "/live", "/notices", "/sizes", "/teams"] {
        admin::call_rec(ctx, "menu", "GET", p, None, p != "/status");
    }
    phase(ctx, "team rules, then a match");
    admin::call_rec(ctx, "rules", "POST", "/teams", Some(r##"{"rules":"# test\napart Alpha Bravo\ntogether Cinder Delta"}"##), true);
    admin::call_rec(ctx, "rules", "GET", "/teams", None, true);
    run_to_game_start(ctx, &bots, &[0, 1]);
    for b in &bots {
        e3(b, 0x11, 580.0, 600.0, 0, 20, 20); // flag 0x10: the ext applies in-match team swaps
    }
    admin::call_rec(ctx, "match", "GET", "/teams", None, true);
    admin::call_rec(ctx, "match", "GET", "/live", None, true);
    phase(ctx, "move a player (in-match team swap), poll the roster, move again");
    let pid = pid_of(ctx, "Delta");
    admin::call_rec(ctx, "move", "POST", "/teams/move", Some(&format!(r#"{{"pid":"{}"}}"#, pid)), true);
    for b in &bots {
        e3(b, 0x11, 570.0, 600.0, 0, 20, 20);
    }
    admin::call_rec(ctx, "move", "POST", "/teams/move", Some(&format!(r#"{{"pid":"{}"}}"#, pid)), true);
    admin::call_rec(ctx, "move", "POST", "/teams/move", Some(r#"{"pid":"zz"}"#), true);
    for b in &bots {
        e3(b, 0x11, 560.0, 600.0, 0, 20, 20);
    }
    phase(ctx, "kick a player in the match with a reason; kick an unknown pid; reset a session");
    let pc = pid_of(ctx, "Cinder");
    admin::call_rec(ctx, "kick", "POST", "/kick", Some(&format!(r#"{{"pid":"{}","reason":"verify kick"}}"#, pc)), true);
    std::thread::sleep(Duration::from_millis(1200));
    admin::call_rec(ctx, "kick", "POST", "/kick", Some(r#"{"pid":"ffffffffffff"}"#), true);
    let pb = pid_of(ctx, "Bravo");
    admin::call_rec(ctx, "reset", "POST", "/reset-session", Some(&format!(r#"{{"pid":"{}"}}"#, pb)), true);
    std::thread::sleep(Duration::from_millis(1200));
    admin::call_rec(ctx, "after", "GET", "/live", None, true);
    phase(ctx, "boot-all");
    admin::call_rec(ctx, "boot", "POST", "/boot-all", Some("{}"), true);
    std::thread::sleep(Duration::from_millis(1200));
    admin::call_rec(ctx, "after", "GET", "/status", None, false);
    // the daemon's team_rules.txt is a file next to the daemon: leave it empty for the next scenario / run
    admin::call_rec(ctx, "rules", "POST", "/teams", Some(r#"{"rules":""}"#), true);
    finish(ctx, &mut bots);
}

fn gunlimit(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    phase(ctx, "outside a match");
    tok_op(&bots[0], 0xDE, &[1, 0], 0xDF);
    run_to_game_start(ctx, &bots, &[0, 1]);
    phase(ctx, "claims: team 0 = slots 0 and 2, team 1 = slots 1 and 3");
    for (i, mask) in [(0usize, 1u8), (2, 1), (1, 1), (3, 1), (0, 0), (2, 1), (3, 3), (1, 0)] {
        tok_op(&bots[i], 0xDE, &[mask, 0], 0xDF);
    }
    bots[0].match_end(1);
    std::thread::sleep(Duration::from_millis(1500));
    phase(ctx, "after the end");
    tok_op(&bots[1], 0xDE, &[1, 0], 0xDF);
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 10 + i as u32, 2, &[]);
    }
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn hostpick(ctx: &Arc<Ctx>) {
    // idx 1..4 = Alpha..Delta; Cinder (3) is close to everybody, Alpha (1) is the farthest from the others: Cinder must be host
    let pairs = [(1, 2, 90), (1, 3, 80), (1, 4, 95), (2, 3, 20), (2, 4, 60), (3, 4, 25)];
    for (a, b, ms) in pairs {
        ctx.set_ping(a, b, ms);
        ctx.set_ping(b, a, ms + 4);
    }
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    run_to_game_start(ctx, &bots, &[0, 1, 2]);
    ctx.rec.script(format!("host index {}", host_index(&bots)));
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    let h = host_index(&bots);
    bots[h].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 10 + i as u32, 2, &[]);
    }
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn pinggate(ctx: &Arc<Ctx>) {
    // Delta is 400 ms from everybody: the gate keeps it out of the lobby
    for a in 1..=4usize {
        for b in 1..=4usize {
            if a != b {
                ctx.set_ping(a, b, if a == 4 || b == 4 { 400 } else { 25 });
            }
        }
    }
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    phase(ctx, "three near players and one far player search");
    let m = marks(&bots);
    search_all(ctx, &bots);
    let ok = wait_match(&bots[..3], &m[..3], 90);
    ctx.rec.script(format!("near players matched ok={}", ok));
    let lv = wait_levelstart(&bots[..3], &m[..3], 90);
    ctx.rec.script(format!("near players at the level start ok={}", lv));
    std::thread::sleep(Duration::from_secs(3));
    let far = bots[3].seen(m[3], "sess", 0x04);
    ctx.rec.script(format!("far player got a record: {}", far));
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    finish(ctx, &mut bots);
}

fn interro(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    phase(ctx, "everybody picks Interrogation (mode 1)");
    for b in &bots {
        tok_op(b, 0xBC, &[1, 1], 0xBD);
    }
    for b in &bots {
        tok_op(b, 0xBC, &[0, 0], 0xBD);
    }
    run_to_game_start(ctx, &bots, &[0, 1]);
    for b in &bots {
        tok_op(b, 0xBC, &[0, 0], 0xBD);
    }
    phase(ctx, "0xDC: poll, interrogated slot 1 / 0, lockbox stage 1, died, repeated sequence number");
    let dc = |b: &Bot, sub: u8, seq: u16, args: &[u8]| {
        let mut x = vec![sub];
        x.extend_from_slice(&seq.to_le_bytes());
        x.extend_from_slice(args);
        tok_op(b, 0xDC, &x, 0xDD)
    };
    for b in &bots {
        dc(b, 0, 0, &[]);
    }
    dc(&bots[0], 1, 1, &[1]);
    dc(&bots[0], 1, 1, &[1]);
    dc(&bots[0], 1, 2, &[3]);
    dc(&bots[1], 1, 1, &[0]);
    dc(&bots[2], 3, 1, &[1]);
    dc(&bots[0], 2, 3, &[1]);
    dc(&bots[0], 0, 0, &[40]);
    for b in &bots {
        dc(b, 0, 0, &[]);
    }
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    phase(ctx, "end");
    let h = host_index(&bots);
    bots[h].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 15 + i as u32, 2, &[]);
    }
    dc(&bots[0], 0, 0, &[]);
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn perfmisc(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 2);
    up_all(ctx, &mut bots);
    let (a, b) = (&bots[0], &bots[1]);
    phase(ctx, "performance report 0xCA: clean, stuttering, severe, wrong version, short");
    let ca = |x: &Bot, n250: u8, n1000: u8, worst: u16, avg: u16, fl: u8| {
        let mut p = vec![1, n250, n1000];
        p.extend_from_slice(&worst.to_le_bytes());
        p.extend_from_slice(&avg.to_le_bytes());
        p.push(fl);
        tok_op(x, 0xCA, &p, 0xCB)
    };
    ca(a, 0, 0, 20, 160, 0);
    ca(a, 7, 1, 400, 330, 0);
    ca(b, 15, 4, 1500, 500, 1);
    a.rpc(0xCA, &tk(a, &[2, 0, 0, 0, 0, 0, 0, 0]), Some(0xCB));
    a.rpc(0xCA, &tk(a, &[1, 0]), Some(0xCB));
    phase(ctx, "mode queue 0xBC: poll, pick, bad mode, window closing (sub 3)");
    tok_op(a, 0xBC, &[0, 0], 0xBD);
    tok_op(a, 0xBC, &[1, 1], 0xBD);
    tok_op(a, 0xBC, &[1, 77], 0xBD);
    tok_op(a, 0xBC, &[1, 255], 0xBD);
    tok_op(a, 0xBC, &[3, 0], 0xBD);
    tok_op(b, 0xBC, &[0, 0], 0xBD);
    phase(ctx, "SP maps 0xC8, map packs 0xC6, private lobby 0xF8, hardware ids 0xB2");
    tok_op(a, 0xC8, &[0xff, 0x01, 0, 0], 0xC9);
    tok_op(a, 0xC8, &[0xff, 0x01, 0, 0, 1], 0xC9);
    tok_op(b, 0xC8, &[0x00, 0x00, 0, 0], 0xC9);
    tok_op(a, 0xC8, &[1, 2], 0xC9);
    tok_op(a, 0xC6, &[2, 1, 0, 0, 0, 2, 0, 0, 0], 0xC7);
    tok_op(a, 0xF8, &[1, 0], 0xF9);
    let mut hw = vec![1, 3];
    hw.extend_from_slice(&[0x11; 12]);
    hw.extend_from_slice(&[0x22; 12]);
    tok_op(a, 0xB2, &hw, 0xB3);
    tok_op(a, 0xB2, &[1, 0], 0xB3);
    tok_op(a, 0xB2, &[2, 3], 0xB3);
    tok_op(a, 0xB2, &[1, 9, 1, 2, 3], 0xB3);
    phase(ctx, "attest 0xA1 with no valid report; frames the daemon must survive");
    tok_op(a, 0xA1, &[1, 0, 0], 0xA3);
    a.rpc(0xA1, &[1, 2, 3], Some(0xA3));
    a.rpc(0x7e, &[1, 2, 3], None);
    a.rpc_raw(&[0, 0, 0, 0], 0, &[], None);
    a.rpc_raw(&[5, 0, 0x13], 0x13, &[], None); // a frame that never completes
    phase(ctx, "online count and leaderboard after all that");
    for x in &bots {
        x.poll_online();
        x.rpc(0x13, &[], Some(0x14));
    }
    std::thread::sleep(Duration::from_secs(1));
    finish(ctx, &mut bots);
}

fn ban(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    let ver = ctx.version.clone();
    let ban_path = ctx.workdir.join("bans.txt");
    let _ = std::fs::write(&ban_path, format!("# bans of the verify run\n{} # reason: cheating in a test\nDelta\n", bots[1].sid));
    std::thread::sleep(Duration::from_millis(1200));
    phase(ctx, "a clean player");
    bots[0].helper_up();
    bots[0].version(&ver, false);
    bots[0].boot();
    bots[0].search();
    bots[0].leave();
    phase(ctx, "banned by Steam id (with a reason): boot is refused");
    bots[1].helper_up();
    bots[1].version(&ver, false);
    bots[1].boot();
    std::thread::sleep(Duration::from_millis(500));
    phase(ctx, "banned by name: boot refused too");
    bots[3].helper_up();
    bots[3].version(&ver, false);
    bots[3].boot();
    std::thread::sleep(Duration::from_millis(500));
    phase(ctx, "banned after login: the search is refused; the ban is lifted and the search works");
    bots[2].helper_up();
    bots[2].version(&ver, false);
    bots[2].boot();
    let _ = std::fs::write(&ban_path, format!("{}\n", bots[2].sid));
    std::thread::sleep(Duration::from_millis(1200));
    bots[2].search();
    std::thread::sleep(Duration::from_millis(500));
    let _ = std::fs::write(&ban_path, "# nobody\n");
    std::thread::sleep(Duration::from_millis(1200));
    bots[2].search();
    bots[2].leave();
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    finish(ctx, &mut bots);
    let _ = std::fs::remove_file(&ban_path);
}

fn jip(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    run_to_game_start(ctx, &bots[..3], &[0, 1]);
    phase(ctx, "the match runs: clock reports every 2.5 s for 20 s");
    for round in 0..8 {
        for b in bots.iter().take(3) {
            e3(b, 1, 590.0 - 2.5 * round as f32, 600.0, 0, 20, 20);
        }
        std::thread::sleep(Duration::from_millis(2500));
    }
    phase(ctx, "a late searcher joins the running match");
    let m = bots[3].mark();
    bots[3].search();
    let mut got = false;
    for round in 0..10 {
        for b in bots.iter().take(3) {
            e3(b, 1, 560.0 - 2.5 * round as f32, 600.0, 0, 19, 20);
        }
        if bots[3].wait_op(m, "sess", 0x04, 1).is_some() {
            got = true;
            break;
        }
    }
    ctx.rec.script(format!("late searcher got the match record: {}", got));
    if got {
        std::thread::sleep(Duration::from_secs(8));
        phase(ctx, "joiner loads, game start for the joiner, rosters");
        bots[3].loaded();
        let ok = bots[3].wait_op(m, "sess", 0x0a, 20).is_some();
        ctx.rec.script(format!("joiner game start: {}", ok));
        for b in &bots {
            e3(b, 5, 500.0, 600.0, 0, 19, 20);
        }
    }
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    bots[0].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 30 + i as u32, 2, &[]);
    }
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn reattach(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 2);
    up_all(ctx, &mut bots);
    let m = marks(&bots);
    search_all(ctx, &bots);
    wait_match(&bots, &m, 120);
    let ok = wait_levelstart(&bots, &m, 90);
    ctx.rec.script(format!("level start ok={}", ok));
    phase(ctx, "launched: Bravo reports loaded and its socket drops; Alpha loads, so the game start goes out while Bravo is away");
    let m1 = bots[1].mark();
    bots[1].loaded();
    std::thread::sleep(Duration::from_millis(300));
    bots[1].drop_sess();
    std::thread::sleep(Duration::from_millis(800));
    bots[0].loaded();
    std::thread::sleep(Duration::from_millis(2500));
    phase(ctx, "Bravo logs in again: session re-attach, the missed 0x0a is sent again");
    bots[1].boot();
    let got = bots[1].wait_op(m1, "sess", 0x0a, 15).is_some();
    ctx.rec.script(format!("re-attached session got the game start again: {}", got));
    std::thread::sleep(Duration::from_millis(1500));
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    let h = host_index(&bots);
    bots[h].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 10 + i as u32, 2, &[]);
    }
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}

fn handover(ctx: &Arc<Ctx>) {
    let mut bots = mkbots(ctx, 4);
    up_all(ctx, &mut bots);
    run_to_game_start(ctx, &bots, &[0, 1]);
    for b in &bots {
        e3(b, 1, 560.0, 600.0, 0, 20, 20);
    }
    phase(ctx, "host migration polls before (every ext says it can do a handover)");
    for b in &bots {
        e6(b, 2, 0);
    }
    phase(ctx, "hand the match over to slot 1 (admin), a refused one, then poll");
    admin::call_rec(ctx, "h1", "POST", "/handover", Some(r#"{"slot":1}"#), true);
    admin::call_rec(ctx, "h2", "POST", "/handover", Some(r#"{"slot":9}"#), true);
    let mut epoch = 0u8;
    let mut state = 0u8;
    for _round in 0..12 {
        for b in &bots {
            // flag 2 = this ext can do a planned handover, flag 4 = prepared (it has seen state 3)
            let r = e6(b, 2 | if state == 3 { 4 } else { 0 }, epoch);
            if let Some((_, pl)) = r.first() {
                if pl.len() >= 5 && pl[0] == 1 {
                    epoch = pl[2];
                    state = pl[4];
                }
            }
        }
        std::thread::sleep(Duration::from_millis(700));
    }
    admin::snapshot(ctx, "after handover");
    admin::call_rec(ctx, "mid", "GET", "/live", None, true);
    phase(ctx, "end by the new host");
    let nh = bots.iter().position(|b| b.my_slot() == Some(1)).unwrap_or(1);
    bots[nh].match_end(0);
    std::thread::sleep(Duration::from_millis(1500));
    for (i, b) in bots.iter().enumerate() {
        e2(b, 1, 10 + i as u32, 2, &[]);
    }
    std::thread::sleep(Duration::from_secs(11));
    finish(ctx, &mut bots);
}
