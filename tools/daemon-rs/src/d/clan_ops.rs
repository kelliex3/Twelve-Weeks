//! K2 clan glue, part 2: daemon.py:2352-2685 (booster / mission ops, feed, rank table, roles, clan page, names, the 0xE0-0xE4 clan ops,
//! leaderboard table) and 10865-10904 (admin clan reset / restore). Part 1 (state, the day, match start / end) is clan.rs.
use super::*;
use crate::clan::clan as cl;
use crate::clan::{clan_story, clan_story_pl};
use crate::clock;
use crate::py;
use crate::{names, rankbadge, roles};
use parking_lot::Mutex;
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

type DbR<T> = db::DbResult<T>;

/// `int.from_bytes(pl[64:72], "little")` (a short slice reads the bytes present)
fn hint_short(pl: &[u8]) -> i64 {
    crate::util::le(pl, 64, 8) as i64
}

// py: daemon.py:2376 meta_op
/// Ops 0xE8 read / 0xEE [64B token][u8 booster id, 0 = none] pick / 0xEF [64B token][u8 mission id] choose -> reply 0xE9 (clan.meta_text).
pub fn meta_op(op: u8, pl: &[u8]) -> DbR<Option<Vec<u8>>> {
    if pl.len() < 64 {
        return Ok(None);
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return Ok(None);
    }
    let sid_hint = if op == 0xE8 && pl.len() >= 72 { hint_short(pl) } else { 0 }; // optional steamid of the launcher's Steam account (read only)
    let text = db::with_write(|c| {
        let tid = if sid_hint != 0 { clan_read_key(c, &token, sid_hint)? } else { clan_key_r(c, &token)? };
        if op == 0xEE {
            if pl.len() < 65 {
                return Ok(None);
            }
            let mut bid = pl[64] as i64;
            if bid != 0 && cl::booster_counts(c, &tid)?.get(&bid).copied().unwrap_or(0) <= 0 {
                bid = 0; // not owned: Nothing
            }
            if bid != 0 {
                BOOST_PICK.lock().insert(token.clone(), bid);
            } else {
                BOOST_PICK.lock().remove(&token);
            }
            let name = cl::BOOSTERS.iter().find(|(i, _)| *i == bid).map(|(_, n)| n.clone()).unwrap_or_else(|| "nothing".to_string());
            dlog!("BOOSTER pick", tag(&token), name);
        } else if op == 0xEF {
            if pl.len() < 65 {
                return Ok(None);
            }
            let (_, ok) = cl::mission_choose(c, &tid, pl[64] as i64)?;
            let name = cl::mission(pl[64] as i64).map(|m| m.name.clone()).unwrap_or_else(|| "?".to_string());
            dlog!("MISSION choose", tag(&token), name, if ok { "ok" } else { "refused" });
        } else if op != 0xE8 {
            return Ok(None);
        }
        let pick = BOOST_PICK.lock().get(&token).copied().unwrap_or(0);
        let act = BOOST_ACTIVE.lock().get(&token).copied().unwrap_or(0);
        Ok(Some(cl::meta_text(c, &tid, pick, act)?))
    })?;
    let Some(mut text) = text else { return Ok(None) };
    text.push_str(&format!(" mf={}", flow_extra_secs())); // seconds after CHOOSE LOADOUT before the level start (ext timer)
    Ok(Some(msg(0xE9, text.as_bytes())))
}

// ---------------------------------------------------------------- feed (daemon.py:2412-2475)
/// daemon.py:2412 FEED_SEEN: token -> ids already sent to the main menu widget
pub static FEED_SEEN: LazyLock<Mutex<HashMap<String, HashSet<String>>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
pub const FEED_LINES: usize = 6; // lines per op 0xB4 reply (t2r-ext FEED_MAX)

// py: daemon.py:2416 feed_latest
/// Clan feed lines for the main menu widget (op 0xB4): [(id, text), ...] <= limit, each one line. [] without a clan.
pub fn feed_latest(token: &str, limit: usize, lang: &str) -> Vec<(String, String)> {
    let r = db::with_write(|c| {
        let tid = clan_key_r(c, token)?;
        FEED_SEEN.lock().entry(token.to_string()).or_default();
        Ok(clan_story::live_for_widget(c, &tid, limit, lang, None))
    });
    match r {
        Ok(rows) => rows.into_iter().map(|(id, t)| (cl::pystr(&id), t)).collect(),
        Err(e) => {
            dlog!("CLAN feed failed", e);
            Vec::new()
        }
    }
}

/// daemon.py:2431 _FEED_CACHE: (token, lang, slot) -> (monotonic time, reply bytes), 20 s
static FEED_CACHE: LazyLock<Mutex<HashMap<(String, String, i64), (f64, Vec<u8>)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

/// `bytes.decode("utf-8", "ignore")`
fn utf8_ignore(b: &[u8]) -> String {
    let mut out = String::new();
    let mut rest = b;
    loop {
        match std::str::from_utf8(rest) {
            Ok(s) => {
                out.push_str(s);
                return out;
            }
            Err(e) => {
                let (good, bad) = rest.split_at(e.valid_up_to());
                out.push_str(std::str::from_utf8(good).unwrap_or(""));
                match e.error_len() {
                    Some(n) => rest = &bad[n..],
                    None => return out,
                }
            }
        }
    }
}

// py: daemon.py:2433 feed_op
/// Op 0xB4 [64B token][u8 language] -> reply 0xB5 "n=<k>\n<id>|<text>\n..." (<= 6 lines, text <= 150 bytes of UTF-8, no '|' / newline inside).
pub fn feed_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 64 {
        return None;
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return None;
    }
    let lang = if pl.len() > 64 { clan_story_pl::norm_lang_b(&pl[64..65]) } else { "en" };
    if pl.len() > 64 {
        note_lang(&token, pl[64]);
    }
    let ck = (token.clone(), lang.to_string(), clan_story::slot_of(None));
    {
        let c = FEED_CACHE.lock();
        if let Some(hit) = c.get(&ck) {
            if clock::mono() - hit.0 < 20.0 {
                return Some(hit.1.clone());
            }
        }
    }
    {
        let mut c = FEED_CACHE.lock();
        if c.len() > 4000 {
            c.clear();
        }
    }
    let mut lines: Vec<String> = Vec::new();
    for (mid, text) in feed_latest(&token, FEED_LINES, lang).into_iter().take(FEED_LINES) {
        let t = text.replace('\n', " ").replace('|', "/");
        let tb = t.as_bytes();
        let t = utf8_ignore(&tb[..tb.len().min(150)]);
        let id: String = mid.replace('|', "").replace('\n', "").chars().take(23).collect();
        lines.push(format!("{}|{}", id, t));
    }
    let rep = msg(0xB5, format!("n={}\n{}", lines.len(), lines.join("\n")).as_bytes());
    FEED_CACHE.lock().insert(ck, (clock::mono(), rep.clone()));
    Some(rep)
}

// ---------------------------------------------------------------- rank (daemon.py:2477-2548)
/// daemon.py:2477 _RANK_TOK: token -> (monotonic time, symbol, weeks)
static RANK_TOK: LazyLock<Mutex<HashMap<String, (f64, i64, i64)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));

// py: daemon.py:2479 rank_of_token
/// (symbol 0..9, weeks 0..999) of the player behind a daemon token (his clan row, clan_ingame.player_rank); (0, 0) when unknown.
pub fn rank_of_token(token: &str) -> (i64, i64) {
    let now = clock::mono();
    if let Some(hit) = RANK_TOK.lock().get(token) {
        if now - hit.0 < 15.0 {
            return (hit.1, hit.2);
        }
    }
    let (mut rk, mut wk) = (0, 0);
    match db::db_read(|c| Ok(clan_ingame::player_rank(c, token))) {
        Ok((a, b)) => {
            rk = a;
            wk = b;
        }
        Err(e) => dlog!("RANK failed", e), // the rank is decoration
    }
    let mut t = RANK_TOK.lock();
    if t.len() > 2000 {
        t.clear();
    }
    t.insert(token.to_string(), (now, rk, wk));
    (rk, wk)
}

static ROLES: LazyLock<roles::Roles> = LazyLock::new(|| {
    let p = std::path::absolute(&db::CFG.path).unwrap_or_else(|_| std::path::PathBuf::from(&db::CFG.path));
    roles::Roles::new(p.parent().unwrap_or(std::path::Path::new("")))
});

// py: daemon.py:2500 role_of_steamid
/// tools/roles.py role code (0 none, 1 OG, 2 admin, 3 dev, 4 lead dev) of a Steam64 id; lists live next to the DB, re-read on change.
pub fn role_of_steamid(sid: i64) -> i64 {
    ROLES.role_of(sid)
}

// py: daemon.py:2512 rank_table_op
/// Op 0xBF [64B token] -> reply 0xBF (tools/rankbadge.py: one entry per recently seen player, keyed by a hash of the shown name).
pub fn rank_table_op(pl: &[u8]) -> DbR<Option<Vec<u8>>> {
    if pl.len() < 64 {
        return Ok(None);
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return Ok(None);
    }
    rankbadge::set_table_ttl(30.0); // (11-21 hotfix: 3 s re-ran a name + rank lookup per listed player every 3 s)
    let mut err: Option<db::DbErr> = None;
    let fetch = || -> Vec<rankbadge::Row> {
        let rows = db::with_write(|c| {
            let mut st = c.prepare("SELECT token, steamid FROM players ORDER BY last_seen DESC LIMIT ?")?;
            let it = st.query_map([rankbadge::LIMIT], |r| Ok((r.get::<_, String>(0)?, cl::vref_to_json(r.get_ref(1)?))))?;
            let mut v = Vec::new();
            for x in it {
                v.push(x?);
            }
            Ok(v)
        });
        match rows {
            Ok(rows) => rows
                .into_iter()
                .map(|(t, sid)| {
                    let (rank, weeks) = rank_of_token(&t);
                    rankbadge::Row { name: get_name(&t), rank, weeks, role: role_of_steamid(cl::int_of(&sid)) }
                })
                .collect(),
            Err(e) => {
                err = Some(e);
                Vec::new()
            }
        }
    };
    let txt = rankbadge::table_text(fetch);
    if let Some(e) = err {
        return Err(e);
    }
    let bytes: Vec<u8> = txt.bytes().filter(|b| *b < 128).collect(); // .encode("ascii", "ignore")
    Ok(Some(msg(0xBF, &bytes)))
}

// py: daemon.py:2529 clan13_op
/// Op 0xB6 [64B token][u8 language][u8 section][optional u32 seen journal id][optional u16 seen raid day][optional flags] -> reply 0xB7 (UTF-8 text:
/// the data of the in-game CLAN page). Read only. An empty reply text = no clan / no story yet.
pub fn clan13_op(pl: &[u8]) -> Option<Vec<u8>> {
    if pl.len() < 66 {
        return None;
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return None;
    }
    let lang = clan_story_pl::norm_lang_b(&pl[64..65]);
    note_lang(&token, pl[64]);
    let section = pl[65] as i64;
    let seen_id = if pl.len() >= 70 { crate::util::le(pl, 66, 4) as i64 } else { 0 };
    let seen_day = if pl.len() >= 72 { crate::util::le(pl, 70, 2) as i64 } else { 0 };
    let big = pl.len() >= 73 && pl[72] & 1 != 0; // PT92+ clients read 64 KB; older ones a 24 KB buffer (a bigger reply is dropped whole)
    let r = db::with_write(|c| {
        let tid = clan_key_r(c, &token)?;
        clan_story::ensure_roster(c, &tid, true); // a clan with no story yet gets its roster from the counts (additive, no feed row)
        Ok(clan_ingame::build(c, &tid, lang, section, seen_id, seen_day, Some(&token.to_lowercase())))
    });
    let text = match r {
        Ok(Ok(t)) => t,
        Ok(Err(e)) | Err(db::DbErr::Sql(rusqlite::Error::InvalidParameterName(e))) => {
            dlog!("CLAN13 failed", e);
            String::new()
        }
        Err(e) => {
            dlog!("CLAN13 failed", e);
            String::new()
        }
    };
    let mut raw = text.into_bytes();
    let cap = if big { 60000 } else { 24000 };
    if raw.len() > cap {
        let cut = raw[..cap].iter().rposition(|b| *b == b'\n'); // whole rows only
        match cut {
            Some(c) if c > 0 => raw.truncate(c + 1),
            _ => raw.truncate(cap),
        }
    }
    Some(msg(0xB7, &raw))
}

// py: daemon.py:2556 names_op
/// Ops 0xD0 read / 0xD2 set [64B token][u8 nlen][name][u8 tlen][tag] -> reply 0xD1 (names.state_text).
pub fn names_op(op: u8, pl: &[u8]) -> DbR<Option<Vec<u8>>> {
    if pl.len() < 64 {
        return Ok(None);
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return Ok(None);
    }
    let tid = name_key(&token);
    let mut err: Option<String> = None;
    if op == 0xD2 {
        let parsed = (|| -> Option<(String, String)> {
            let nl = *pl.get(64)? as usize;
            let nm = std::str::from_utf8(crate::util::sl(pl, 65, 65 + nl)).ok()?.to_string();
            let tl = *pl.get(65 + nl)? as usize;
            let tg = std::str::from_utf8(crate::util::sl(pl, 66 + nl, 66 + nl + tl)).ok()?.to_string();
            if pl.len() < 66 + nl + tl {
                return None;
            }
            Some((nm, tg))
        })();
        let Some((nm, tg)) = parsed else {
            return Ok(Some(msg(0xD1, names::state_text("", "", Some("bad request")).as_bytes())));
        };
        let (n2, t2, e2) = db::with_write(|c| {
            let taken = |n: &str| name_taken(c, &tid, n).unwrap_or(false);
            Ok(names::save(c, &tid, &nm, &tg, Some(&taken))?)
        })?;
        err = e2;
        name_dirty(Some(&token), Some(&tid)); // the shown-name cache must not keep the old name (every token under this names key)
        if err.is_none() {
            let shown = match n2.as_deref().filter(|n| !n.is_empty()) {
                Some(n) => names::display(n, t2.as_deref().unwrap_or("")),
                None => "(cleared)".to_string(),
            };
            dlog!("NAME set", tag(&token), "->", shown);
            let gn = get_name(&token);
            PRES.touch(&token, if gn.is_empty() { "PLAYER" } else { &gn });
        }
    }
    let (cn, ct) = db::with_write(|c| Ok(names::load(c, &tid)?))?;
    Ok(Some(msg(0xD1, names::state_text(&cn, &ct, err.as_deref()).as_bytes())))
}

/// daemon.py:2661 CLAN_PERK_LOGGED: the first perk state reply of a session is logged once
pub static CLAN_PERK_LOGGED: LazyLock<Mutex<HashSet<String>>> = LazyLock::new(|| Mutex::new(HashSet::new()));

// py: daemon.py:2609 clan_op
/// Ops 0xE0 / 0xE2 / 0xE4 -> reply frame (0xE1), or None for a bad payload.
pub fn clan_op(op: u8, pl: &[u8]) -> DbR<Option<Vec<u8>>> {
    if pl.len() < 64 {
        return Ok(None);
    }
    let token = tok64(pl);
    if !is_token(&token) {
        return Ok(None);
    }
    let hint = if op == 0xE0 && pl.len() >= 72 { hint_short(pl) } else { 0 };
    let st: Value = if op == 0xE0 {
        clan_state(&token, if pl.len() >= 72 { hint_short(pl) } else { 0 })?
    } else if op == 0xE2 && pl.len() >= 73 {
        let seq = crate::util::le(pl, 64, 4);
        let parts = crate::util::le(pl, 68, 4) as i64;
        let flags = pl[72] as i64;
        dlog!("CLAN report", tag(&token), "seq", seq, "parts", parts, "flags", flags);
        let mut counts: indexmap::IndexMap<i64, i64> = indexmap::IndexMap::new(); // optional tail: [u8 n] n x [u8 label id][u16 count] (missions)
        if pl.len() > 73 {
            for k in 0..(pl[73] as usize).min(16) {
                if pl.len() >= 74 + 3 * k + 3 {
                    counts.insert(pl[74 + 3 * k] as i64, u16::from_le_bytes([pl[75 + 3 * k], pl[76 + 3 * k]]) as i64);
                }
            }
        }
        clan_report(&token, parts, flags, counts)?
    } else if op == 0xE4 && pl.len() >= 65 {
        clan_side(&token, pl[64] as i64)?
    } else {
        return Ok(None);
    };
    let mut txt = cl::state_text(&st);
    // 11-18: the asker's own role for its own rank badge colour / nametag; k=v, before the name tails. EVERY 0xE1 reply carries it.
    if matches!(op, 0xE0 | 0xE2 | 0xE4) {
        let r = db::with_write(|c| {
            use rusqlite::OptionalExtension;
            let v = c.query_row("SELECT steamid FROM players WHERE token=?", [&token], |r| Ok(cl::vref_to_json(r.get_ref(0)?))).optional()?;
            Ok(v)
        });
        match r {
            Ok(v) => {
                let own = v.as_ref().filter(|v| cl::truthy(v)).map(cl::int_of).unwrap_or(0);
                let sid = if own != 0 { own } else { hint };
                let ro = role_of_steamid(sid);
                if ro != 0 {
                    txt.push_str(&format!(" ro={}", ro));
                }
            }
            Err(e) => dlog!("ROLE (state) failed", e), // the role is decoration
        }
    }
    if matches!(op, 0xE0 | 0xE2) {
        // the active supply perks + the credited member ride at the END of the state text
        let r = db::with_write(|c| {
            let key = clan_read_key(c, &token, hint)?;
            let story = clan_story::_load(c, &key);
            let perks = clan_perks::active(&st, story.as_ref());
            Ok(clan_perks::wire_text(&perks, clan_perks::loss_streak(story.as_ref())))
        });
        match r {
            Ok(tail) => {
                if !tail.is_empty() {
                    txt.push(' ');
                    txt.push_str(&tail);
                    if op == 0xE0 && CLAN_PERK_LOGGED.lock().insert(token.clone()) {
                        dlog!("clanperk: active for", tag(&token), tail);
                    }
                }
            }
            Err(e) => dlog!("CLAN perks (state) failed", e),
        }
        // member status updates (hungry / sad / sick / dead since last seen), after the perk tail
        let r = db::with_write(|c| {
            let story = clan_story::_load(c, &clan_read_key(c, &token, hint)?);
            Ok(clan_status::wire_text(story.as_ref().unwrap_or(&Value::Null)))
        });
        match r {
            Ok(tail) => {
                if !tail.is_empty() {
                    txt.push(' ');
                    txt.push_str(&tail);
                }
            }
            Err(e) => dlog!("CLAN status (state) failed", e),
        }
    }
    Ok(Some(msg(0xE1, txt.as_bytes())))
}

// ---------------------------------------------------------------- leaderboard (daemon.py:2662-2688)
static LB_CACHE: LazyLock<Mutex<(f64, Option<usize>, Vec<u8>)>> = LazyLock::new(|| Mutex::new((0.0, None, Vec::new())));
pub static LB_CACHE_S: LazyLock<f64> = LazyLock::new(|| crate::env_parse("T2R_LB_CACHE_S", 5.0));

// py: daemon.py:2664 leaderboard_table
/// Cached for LB_CACHE_S s: op 0x13 is unauthenticated and the query costs ~60 ms (a get_name per row).
pub fn leaderboard_table(top: usize) -> DbR<Vec<u8>> {
    let now = clock::now();
    {
        let c = LB_CACHE.lock();
        if c.1 == Some(top) && now - c.0 < *LB_CACHE_S {
            return Ok(c.2.clone());
        }
    }
    let v = leaderboard_table_(top)?;
    *LB_CACHE.lock() = (now, Some(top), v.clone());
    Ok(v)
}

// py: daemon.py:2673 _leaderboard_table
/// 0x14 payload = [u8 count][count x 0x30]: [32B name][u32 wins][u32 losses][f32 ratio][u32 elo]. Requested by client op 0x13.
pub fn leaderboard_table_(top: usize) -> DbR<Vec<u8>> {
    let mut rows = leaderboard(1_000_000)?;
    let toks: Vec<String> = rows.iter().map(|r| r.0.clone()).collect();
    let recs: HashMap<String, (i64, i64, i64)> = rows.iter().map(|r| (r.0.clone(), (r.4, (r.5 - r.4).max(0), 0))).collect();
    let elo_r = elo_of_tokens(&toks, Some(&recs));
    rows.sort_by_key(|r| -elo_r.get(&r.0).copied().unwrap_or(1000));
    rows.truncate(top);
    let mut body: Vec<u8> = vec![rows.len() as u8];
    for (tok, name, _kills, _deaths, wins, matches) in &rows {
        let losses = (matches - wins).max(0);
        let mut rec = [0u8; 0x30];
        let nm_s = token_name(tok).filter(|n| !n.is_empty()).unwrap_or_else(|| if name.is_empty() { "PLAYER".to_string() } else { name.clone() });
        let nm: Vec<u8> = nm_s.chars().filter(|c| (*c as u32) < 256).map(|c| c as u32 as u8).take(31).collect(); // .encode("latin1", "ignore")[:31]
        rec[..nm.len()].copy_from_slice(&nm);
        let ratio: f64 = if losses != 0 { *wins as f64 / losses as f64 } else { *wins as f64 };
        let elo = elo_r.get(tok).copied().unwrap_or(1000).max(0); // team Elo (elo_update); u32: never negative
        rec[0x20..0x24].copy_from_slice(&(*wins as u32).to_le_bytes());
        rec[0x24..0x28].copy_from_slice(&(losses as u32).to_le_bytes());
        rec[0x28..0x2c].copy_from_slice(&(ratio as f32).to_le_bytes());
        rec[0x2c..0x30].copy_from_slice(&(elo as u32).to_le_bytes());
        body.extend_from_slice(&rec);
    }
    body.push(0); // +1 so len > count*0x30
    Ok(body)
}

// ---------------------------------------------------------------- admin clan reset / restore (daemon.py:10865-10904)
pub static CLAN_RESET_DIR: LazyLock<String> = LazyLock::new(|| {
    let p = std::path::absolute(&db::CFG.path).unwrap_or_else(|_| std::path::PathBuf::from(&db::CFG.path));
    p.parent().unwrap_or(std::path::Path::new("")).join("clan-resets").to_string_lossy().into_owned()
});

/// `int(v)` of a JSON body value; Err = the text of the Python exception
fn py_int(v: &Value) -> Result<i64, String> {
    match v {
        Value::Null => Ok(0),
        Value::Bool(b) => Ok(*b as i64),
        Value::Number(n) => Ok(n.as_i64().or_else(|| n.as_u64().map(|u| u as i64)).unwrap_or_else(|| n.as_f64().map(|f| f.trunc() as i64).unwrap_or(0))),
        Value::String(s) => s.trim().replace('_', "").parse::<i64>().map_err(|_| format!("ValueError(\"invalid literal for int() with base 10: {}\")", py::repr_str(s))),
        Value::Array(_) => Err("TypeError(\"int() argument must be a string, a bytes-like object or a real number, not 'list'\")".to_string()),
        Value::Object(_) => Err("TypeError(\"int() argument must be a string, a bytes-like object or a real number, not 'dict'\")".to_string()),
    }
}

fn strerr(e: String) -> db::DbErr {
    db::DbErr::Sql(rusqlite::Error::InvalidParameterName(e))
}

fn errtext(e: db::DbErr) -> String {
    match e {
        db::DbErr::Sql(rusqlite::Error::InvalidParameterName(s)) => s,
        o => o.to_string(),
    }
}

// py: daemon.py:10867 admin_clan_reset
/// POST /clan/reset {"name", "steamid", "tokenIds": [24-hex install ids of the person], "names": false}: backs up and wipes the person's clan.
pub fn admin_clan_reset(body: &Value) -> Value {
    let res = (|| -> Result<Value, String> {
        let nm = body.get("name").filter(|v| cl::truthy(v)).map(cl::pystr).unwrap_or_else(|| "player".to_string());
        let name: String = nm.chars().take(64).collect();
        let sid = match body.get("steamid").filter(|v| cl::truthy(v)) {
            Some(v) => py_int(v)?,
            None => 0,
        };
        let tids: Vec<String> = match body.get("tokenIds").filter(|v| cl::truthy(v)) {
            Some(Value::Array(a)) => a.iter().map(cl::pystr).take(64).collect(),
            Some(Value::String(s)) => s.chars().map(|c| c.to_string()).take(64).collect(),
            _ => Vec::new(),
        };
        let names = body.get("names").is_some_and(cl::truthy);
        let r = db::with_write(|c| {
            let extra: Vec<String> = if sid > 0 {
                let mut st = c.prepare("SELECT token FROM players WHERE steamid=?")?;
                let it = st.query_map([sid], |r| r.get::<_, String>(0))?;
                let mut v = Vec::new();
                for t in it {
                    v.push(cl::token_id(&t?));
                }
                v
            } else {
                Vec::new()
            };
            let mut keys_all = tids.clone();
            keys_all.extend(extra);
            crate::clan::clan_reset::reset(c, &CLAN_RESET_DIR, &name, sid, &keys_all, names, None).map_err(strerr)
        })
        .map_err(errtext)?;
        if !r.get("ok").is_some_and(cl::truthy) {
            return Ok(r);
        }
        let ks: HashSet<String> = r.get("keys").and_then(|k| k.as_array()).map(|a| a.iter().map(cl::pystr).collect()).unwrap_or_default();
        {
            let _g = CLANLOCK.lock();
            let mut p = CLAN_PENDING.lock();
            let del: Vec<String> = p.keys().filter(|t| ks.contains(&cl::token_id(t))).cloned().collect();
            for t in del {
                p.remove(&t);
            }
        }
        let backup = r.get("backup").map(cl::pystr).unwrap_or_default();
        evt!("admin-clan-reset", name, sid, backup);
        dlog!("CLAN reset by admin:", name, "steam", sid, "rows", r.get("rows").map(cl::pystr).unwrap_or_default(), "backup", backup);
        Ok(r)
    })();
    match res {
        Ok(v) => v,
        Err(e) => {
            dlog!("ADMIN-CLAN-RESET-ERR", e);
            json!({"ok": false, "error": format!("reset failed: {}", e)})
        }
    }
}

// py: daemon.py:10892 admin_clan_restore
pub fn admin_clan_restore(body: &Value) -> Value {
    let file = body.get("file").filter(|v| cl::truthy(v)).map(cl::pystr).unwrap_or_default();
    let r = db::with_write(|c| crate::clan::clan_reset::restore(c, &CLAN_RESET_DIR, &file, None).map_err(strerr));
    match r {
        Ok(v) => {
            if v.get("ok").is_some_and(cl::truthy) {
                dlog!("CLAN restore by admin:", py::pyrepr(&v));
            }
            v
        }
        Err(e) => {
            let e = errtext(e);
            dlog!("ADMIN-CLAN-RESTORE-ERR", e);
            json!({"ok": false, "error": format!("restore failed: {}", e)})
        }
    }
}

// ---------------------------------------------------------------- selftest support
/// (selftest) the caches of this file empty
pub fn k2_reset_ops() {
    FEED_SEEN.lock().clear();
    FEED_CACHE.lock().clear();
    RANK_TOK.lock().clear();
    CLAN_PERK_LOGGED.lock().clear();
    *LB_CACHE.lock() = (0.0, None, Vec::new());
}

/// (selftest) the tokens FEED_SEEN holds
pub fn feed_seen_keys() -> Vec<String> {
    FEED_SEEN.lock().keys().cloned().collect()
}
