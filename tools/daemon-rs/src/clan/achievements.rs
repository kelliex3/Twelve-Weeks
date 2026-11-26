//! achievements.py: Steam-style achievements (definitions, booking and storage). The daemon is the only place that unlocks one.
//! The table (id, group, title, desc, kind, goal, hidden, art, live) is generated data (data/clan_tables.json, achievements.ACH); only the
//! condition functions (`prog` of the count kinds, `test` of the match kinds) are coded by hand here, keyed by id (`selftest achievements` checks
//! that the hand table lists exactly the ids of the data, in order).
use crate::clan::data;
use crate::clock;
use indexmap::IndexMap;
use rusqlite::Connection;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::sync::LazyLock;

// ---- labels
pub const L_DOWNS: i64 = 1;
pub const L_EXECS: i64 = 2;
pub const L_REVIVES: i64 = 3;
pub const L_HEADSHOTS: i64 = 4;
pub const L_MARKS: i64 = 5;
pub const L_HEALS: i64 = 6;
pub const L_SPECIAL: i64 = 7;
pub const L_MOLOTOV: i64 = 8;
pub const L_NAIL: i64 = 9;
pub const L_GIFT: i64 = 10;
pub const L_MELEE_DOWN: i64 = 11;
pub const L_SHIV: i64 = 12;
pub const L_LONG: i64 = 13;
pub const L_SHORTY_DOWN: i64 = 16;
pub const L_HUNT_DOWN: i64 = 17;
pub const L_BOW_DOWN: i64 = 20;
pub const L_TIMES_DOWNED: i64 = 21;
pub const L_DEATHS: i64 = 22;
pub const L_STREAK: i64 = 23;
pub const L_TOPWEAPON: i64 = 24;
pub const L_BOTTLES: i64 = 33;
pub const L_REVOLVER_EXEC: i64 = 49;
pub const L_SE_CROWBAR: i64 = 64;
pub const L_SE_HAMMER: i64 = 65;
pub const L_SE_MACHETE: i64 = 66;
pub const L_SE_HATCHET: i64 = 67;
pub const L_SE_BAT: i64 = 68;
pub const L_SE_2X4: i64 = 69;
pub const L_SE_PIPE: i64 = 70;
pub const L_SE_WRENCH: i64 = 71;
pub const L_SE_CLUB: i64 = 72;
pub const L_SE_FISTS: i64 = 73;
pub const L_OWN_MOLOTOV: i64 = 74;
pub const L_BOW_TELL: i64 = 75;
pub const L_DODGES: i64 = 76;
pub const L_PIN_ESC: i64 = 77;
pub const L_PIN_ESC_LOW: i64 = 78;
pub const L_LAST_STANDING: i64 = 79;
pub const L_STREAK_STOP: i64 = 80;
pub const L_HURT: i64 = 81;
pub const L_RETALIATION: i64 = 82;
pub const L_TEAM_REVENGE: i64 = 83;
pub const L_RAPID: i64 = 84;
pub const L_LOCKBOX: i64 = 85;
pub const L_CRAFT: i64 = 86;
pub const L_COMEBACK: i64 = 87;
pub const L_XBOW_TELL: i64 = 88;
pub const L_COUNTER: i64 = 89;
pub const L_EXECUTED: i64 = 90;
/// SE_WEAPON_LABELS = range(L_SE_CROWBAR, L_SE_CLUB + 1): the fists (73) are NOT in it
pub const SE_WEAPON_LABELS: [i64; 9] = [64, 65, 66, 67, 68, 69, 70, 71, 72];
pub const DOWN_KIND_LABELS: [i64; 11] = [L_MOLOTOV, L_NAIL, L_MELEE_DOWN, L_LONG, 14, 15, L_SHORTY_DOWN, L_HUNT_DOWN, 18, 19, L_BOW_DOWN];
pub const D_MATCHES: i64 = 200;
pub const D_WINS: i64 = 201;
pub const D_LOSSES: i64 = 202;
pub const D_MISSION_TOP: i64 = 203;
pub const D_WON_WLF: i64 = 204;
pub const D_WON_SERA: i64 = 205;
pub const D_WON_ANY: i64 = 206;
pub const MAX_PER_MATCH: i64 = 60;
pub const WLF: i64 = 1;
pub const SERAPHITES: i64 = 2;
pub const ALIVE: i64 = 1;
pub const DEAD: i64 = 2;
pub const WON: i64 = 3;
/// 0xE2 report only (times downed, deaths, streak, top weapon, bottles)
pub const LIVE_SKIP: [i64; 5] = [21, 22, 23, 24, 33];

const SCHEMA: [&str; 2] = [
    "CREATE TABLE IF NOT EXISTS ach_count(key TEXT, lab INT, n INT DEFAULT 0, PRIMARY KEY(key, lab))",
    "CREATE TABLE IF NOT EXISTS ach(key TEXT, aid TEXT, unlocked REAL, PRIMARY KEY(key, aid))",
];

/// One achievement of the table.
#[derive(Debug)]
pub struct Ach {
    pub id: String,
    pub group: String,
    pub title: String,
    pub desc: String,
    /// "count" | "match" | "clan" | "rank"
    pub kind: String,
    pub goal: i64,
    pub hidden: bool,
    pub art: String,
    pub live: Option<(i64, i64)>,
}

/// The facts of one match for the "match" kind tests: the counters credited so far + the win / late flags.
#[derive(Clone, Debug, Default)]
pub struct MatchFacts {
    pub labs: HashMap<i64, i64>,
    pub win: Option<bool>,
    pub late: bool,
}

impl MatchFacts {
    fn get(&self, l: i64) -> i64 {
        self.labs.get(&l).copied().unwrap_or(0)
    }
}

/// How the progress of a count achievement is computed (`_c(label)`, `_all_se`, the both_12 lambda).
#[derive(Clone, Copy, Debug)]
enum Prog {
    Lab(i64),
    AllSe,
    Both12,
    None,
}

// py: achievements.py:78 the condition functions, by id (the table order of the data is checked by `check_table`)
fn prog_of(id: &str) -> Prog {
    use Prog::*;
    match id {
        "se_crowbar" => Lab(L_SE_CROWBAR),
        "se_hammer" => Lab(L_SE_HAMMER),
        "se_machete" => Lab(L_SE_MACHETE),
        "se_hatchet" => Lab(L_SE_HATCHET),
        "se_bat" => Lab(L_SE_BAT),
        "se_2x4" => Lab(L_SE_2X4),
        "se_pipe" => Lab(L_SE_PIPE),
        "se_wrench" => Lab(L_SE_WRENCH),
        "se_club" => Lab(L_SE_CLUB),
        "se_fists" => Lab(L_SE_FISTS),
        "shiv_25" => Lab(L_SHIV),
        "melee_all" => AllSe,
        "bow_50" => Lab(L_BOW_DOWN),
        "bow_tell" => Lab(L_BOW_TELL),
        "xbow_tell" => Lab(L_XBOW_TELL),
        "revolver_10" => Lab(L_REVOLVER_EXEC),
        "shorty_25" => Lab(L_SHORTY_DOWN),
        "hunting_50" => Lab(L_HUNT_DOWN),
        "long_50" => Lab(L_LONG),
        "molotov_25" => Lab(L_MOLOTOV),
        "nail_25" => Lab(L_NAIL),
        "head_100" => Lab(L_HEADSHOTS),
        "dodge_50" => Lab(L_DODGES),
        "counter_100" => Lab(L_COUNTER),
        "pin_10" => Lab(L_PIN_ESC),
        "pin_low" => Lab(L_PIN_ESC_LOW),
        "stopper_10" => Lab(L_STREAK_STOP),
        "hurt_25" => Lab(L_HURT),
        "rapid_25" => Lab(L_RAPID),
        "payback_25" => Lab(L_RETALIATION),
        "avenger_25" => Lab(L_TEAM_REVENGE),
        "last_alive" => Lab(L_LAST_STANDING),
        "raid_1" | "raid_100" | "raid_500" => Lab(D_MATCHES),
        "win_50" => Lab(D_WINS),
        "down_100" | "down_1000" => Lab(L_DOWNS),
        "exec_50" => Lab(L_SPECIAL),
        "revive_50" => Lab(L_REVIVES),
        "heal_50" => Lab(L_HEALS),
        "gift_25" => Lab(L_GIFT),
        "mark_200" => Lab(L_MARKS),
        "craft_100" => Lab(L_CRAFT),
        "bottle_50" => Lab(L_BOTTLES),
        "loss_25" => Lab(D_LOSSES),
        "weeks_12" => Lab(D_WON_ANY),
        "wlf_12" => Lab(D_WON_WLF),
        "sera_12" => Lab(D_WON_SERA),
        "both_12" => Both12,
        "missions_10" => Lab(D_MISSION_TOP),
        "darwin" => Lab(L_OWN_MOLOTOV),
        "reaper" => Lab(L_EXECUTED),
        "lockbox_10" => Lab(L_LOCKBOX),
        _ => None,
    }
}

/// The ids in table order with their condition kind, as coded by hand (checked against the data by `check_table`).
const HAND_IDS: [&str; 72] = [
    "se_crowbar", "se_hammer", "se_machete", "se_hatchet", "se_bat", "se_2x4", "se_pipe", "se_wrench", "se_club", "se_fists", "shiv_25", "melee_all", "bow_50", "bow_tell", "xbow_tell",
    "revolver_10", "shorty_25", "hunting_50", "long_50", "molotov_25", "nail_25", "head_100", "dodge_50", "counter_100", "dodge_5", "pin_10", "pin_low", "streak_10", "stopper_10", "hurt_25",
    "rapid_25", "payback_25", "avenger_25", "flawless", "last_alive", "comeback", "melee_only", "raid_1", "raid_100", "raid_500", "win_50", "down_100", "down_1000", "exec_50", "revive_50",
    "heal_50", "gift_25", "mark_200", "craft_100", "bottle_50", "loss_25", "side", "grave_1", "weeks_12", "wlf_12", "sera_12", "both_12", "wiped", "rank_1", "rank_5", "rank_10", "rank_25",
    "rank_50", "rank_100", "haven", "hold_line", "missions_10", "darwin", "reaper", "pacifist", "late_win", "lockbox_10",
];

// py: achievements.py `test=` lambdas of the "match" kind
fn test_of(id: &str, m: &MatchFacts) -> bool {
    let win = m.win == Some(true);
    match id {
        "dodge_5" => m.get(L_DODGES) >= 10,
        "streak_10" => m.get(L_STREAK) >= 10,
        "flawless" => win && m.get(L_DOWNS) >= 5 && m.get(L_DEATHS) == 0 && m.get(L_TIMES_DOWNED) == 0,
        "comeback" => win && m.get(L_COMEBACK) > 0,
        "melee_only" => win && m.get(L_DOWNS) >= 10 && m.get(L_MELEE_DOWN) >= m.get(L_DOWNS),
        "pacifist" => win && m.get(L_DOWNS) == 0 && m.get(L_REVIVES) >= 3,
        "late_win" => win && m.late,
        _ => false,
    }
}

static ACH: LazyLock<Vec<Ach>> = LazyLock::new(|| {
    let rows = data::t("achievements", "ACH").as_array().cloned().unwrap_or_default();
    rows.iter()
        .map(|a| {
            let s = |k: &str| a.get(k).and_then(|v| v.as_str()).unwrap_or("").to_string();
            Ach {
                id: s("id"),
                group: s("group"),
                title: s("title"),
                desc: s("desc"),
                kind: s("kind"),
                goal: a.get("goal").and_then(|v| v.as_i64()).unwrap_or(1),
                hidden: a.get("hidden").and_then(|v| v.as_bool()).unwrap_or(false),
                art: s("art"),
                live: a.get("live").and_then(|v| v.as_array()).and_then(|l| Some((l.first()?.as_i64()?, l.get(1)?.as_i64()?))),
            }
        })
        .collect()
});

/// The table, in order.
pub fn ach() -> &'static [Ach] {
    &ACH
}

// py: achievements.py BY_ID
pub fn by_id(id: &str) -> Option<&'static Ach> {
    ACH.iter().find(|a| a.id == id)
}

/// Selftest: the hand table lists the data's ids in the data's order, every "count" has a progress and every "match" a test; Err = the first difference.
pub fn check_table() -> Result<(), String> {
    let ints = |k: &str| -> Vec<i64> { data::t("achievements", k).as_array().map(|a| a.iter().filter_map(|x| x.as_i64()).collect()).unwrap_or_default() };
    if ints("SE_WEAPON_LABELS") != SE_WEAPON_LABELS || ints("DOWN_KIND_LABELS") != DOWN_KIND_LABELS || ints("LIVE_SKIP") != LIVE_SKIP {
        return Err("label tuples differ from the data".into());
    }
    for (k, v) in [("MAX_PER_MATCH", MAX_PER_MATCH), ("D_MATCHES", D_MATCHES), ("D_WINS", D_WINS), ("D_LOSSES", D_LOSSES), ("D_MISSION_TOP", D_MISSION_TOP), ("D_WON_WLF", D_WON_WLF), ("D_WON_SERA", D_WON_SERA), ("D_WON_ANY", D_WON_ANY),
        ("WLF", WLF), ("SERAPHITES", SERAPHITES), ("ALIVE", ALIVE), ("DEAD", DEAD), ("WON", WON), ("L_DOWNS", L_DOWNS), ("L_EXECS", L_EXECS), ("L_REVIVES", L_REVIVES), ("L_HEADSHOTS", L_HEADSHOTS), ("L_MARKS", L_MARKS), ("L_HEALS", L_HEALS),
        ("L_SPECIAL", L_SPECIAL), ("L_MOLOTOV", L_MOLOTOV), ("L_NAIL", L_NAIL), ("L_GIFT", L_GIFT), ("L_MELEE_DOWN", L_MELEE_DOWN), ("L_SHIV", L_SHIV), ("L_LONG", L_LONG), ("L_SHORTY_DOWN", L_SHORTY_DOWN), ("L_HUNT_DOWN", L_HUNT_DOWN),
        ("L_BOW_DOWN", L_BOW_DOWN), ("L_TIMES_DOWNED", L_TIMES_DOWNED), ("L_DEATHS", L_DEATHS), ("L_STREAK", L_STREAK), ("L_TOPWEAPON", L_TOPWEAPON), ("L_BOTTLES", L_BOTTLES), ("L_REVOLVER_EXEC", L_REVOLVER_EXEC), ("L_SE_CROWBAR", L_SE_CROWBAR),
        ("L_SE_HAMMER", L_SE_HAMMER), ("L_SE_MACHETE", L_SE_MACHETE), ("L_SE_HATCHET", L_SE_HATCHET), ("L_SE_BAT", L_SE_BAT), ("L_SE_2X4", L_SE_2X4), ("L_SE_PIPE", L_SE_PIPE), ("L_SE_WRENCH", L_SE_WRENCH), ("L_SE_CLUB", L_SE_CLUB),
        ("L_SE_FISTS", L_SE_FISTS), ("L_OWN_MOLOTOV", L_OWN_MOLOTOV), ("L_BOW_TELL", L_BOW_TELL), ("L_DODGES", L_DODGES), ("L_PIN_ESC", L_PIN_ESC), ("L_PIN_ESC_LOW", L_PIN_ESC_LOW), ("L_LAST_STANDING", L_LAST_STANDING),
        ("L_STREAK_STOP", L_STREAK_STOP), ("L_HURT", L_HURT), ("L_RETALIATION", L_RETALIATION), ("L_TEAM_REVENGE", L_TEAM_REVENGE), ("L_RAPID", L_RAPID), ("L_LOCKBOX", L_LOCKBOX), ("L_CRAFT", L_CRAFT), ("L_COMEBACK", L_COMEBACK),
        ("L_XBOW_TELL", L_XBOW_TELL), ("L_COUNTER", L_COUNTER), ("L_EXECUTED", L_EXECUTED)]
    {
        if data::t("achievements", k).as_i64() != Some(v) {
            return Err(format!("{} differs from the data", k));
        }
    }
    let ids: Vec<&str> = ACH.iter().map(|a| a.id.as_str()).collect();
    if ids != HAND_IDS {
        return Err(format!("hand ids differ from the data: {:?} vs {:?}", HAND_IDS, ids));
    }
    for a in ACH.iter() {
        let has = !matches!(prog_of(&a.id), Prog::None);
        if (a.kind == "count") != has {
            return Err(format!("{}: kind {} but prog {}", a.id, a.kind, has));
        }
        if a.kind == "match" {
            let hit = HAND_IDS.contains(&a.id.as_str());
            if !hit {
                return Err(format!("{}: no test", a.id));
            }
        }
    }
    Ok(())
}

fn prog(a: &Ach, cnt: &HashMap<i64, i64>) -> i64 {
    let g = |l: i64| cnt.get(&l).copied().unwrap_or(0);
    match prog_of(&a.id) {
        Prog::Lab(l) => g(l),
        Prog::AllSe => SE_WEAPON_LABELS.iter().filter(|l| g(**l) > 0).count() as i64,
        Prog::Both12 => g(D_WON_WLF).min(1) + g(D_WON_SERA).min(1),
        Prog::None => 0,
    }
}

// py: achievements.py:213 live_rules
/// [(id, kind, label, goal)] the ext checks during a match. kind: "count" = lifetime label >= goal, "allse" = the SE weapon labels with any
/// count >= goal (label 0), "match" = this match's label >= goal.
pub fn live_rules() -> Vec<(String, &'static str, i64, i64)> {
    let mut out = Vec::new();
    for a in ACH.iter() {
        let p = prog_of(&a.id);
        if a.kind == "count" {
            if let Prog::Lab(lab) = p {
                if (1..=127).contains(&lab) && !LIVE_SKIP.contains(&lab) {
                    out.push((a.id.clone(), "count", lab, a.goal));
                }
                continue;
            }
            if matches!(p, Prog::AllSe) {
                out.push((a.id.clone(), "allse", 0, a.goal));
                continue;
            }
        }
        if a.kind == "match" {
            if let Some((l, n)) = a.live {
                out.push((a.id.clone(), "match", l, n));
            }
        }
    }
    out
}

// py: achievements.py:227 ext_header
pub fn ext_header() -> String {
    let rs = live_rules();
    let mut lines: Vec<String> = vec![
        "// generated by tools/achievements.py --ext-header - do not edit. The achievements the ext checks during a match (achieve.c ach_live_tick):".to_string(),
        "// kind 0 = lifetime label >= goal, 1 = Special Execution weapons (ACH_LIVE_SE labels) with any count >= goal, 2 = this match's label >= goal.".to_string(),
        format!("#define ACH_LIVE_N {}", rs.len()),
        "#define ACH_LIVE_SE_N 9".to_string(),
        format!("static const unsigned char ACH_LIVE_SE[ACH_LIVE_SE_N] = {{{}}};", SE_WEAPON_LABELS.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(", ")),
        "static const struct { const char *id; unsigned char kind, lab; unsigned short goal; } ACH_LIVE[ACH_LIVE_N] = {".to_string(),
    ];
    for (aid, kind, lab, goal) in &rs {
        let k = match *kind {
            "count" => 0,
            "allse" => 1,
            _ => 2,
        };
        lines.push(format!("    {{\"{}\", {}, {}, {}}},", aid, k, lab, goal));
    }
    lines.push("};".to_string());
    lines.join("\n") + "\n"
}

// py: achievements.py:253 ensure
pub fn ensure(c: &Connection) {
    for s in SCHEMA {
        let _ = c.execute(s, []); // a read-only connection: the reads below then find the tables or return nothing
    }
}

// py: achievements.py:269 counters
pub fn counters(c: &Connection, key: &str) -> HashMap<i64, i64> {
    ensure(c);
    let mut out = HashMap::new();
    if let Ok(mut st) = c.prepare("SELECT lab, n FROM ach_count WHERE key=?") {
        if let Ok(rows) = st.query_map([key], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, i64>(1)?))) {
            for r in rows.flatten() {
                out.insert(r.0, r.1);
            }
        }
    }
    out
}

// py: achievements.py:274 unlocked
pub fn unlocked(c: &Connection, key: &str) -> HashMap<String, f64> {
    ensure(c);
    let mut out = HashMap::new();
    if let Ok(mut st) = c.prepare("SELECT aid, unlocked FROM ach WHERE key=?") {
        if let Ok(rows) = st.query_map([key], |r| Ok((r.get::<_, String>(0)?, r.get::<_, f64>(1)?))) {
            for r in rows.flatten() {
                out.insert(r.0, r.1);
            }
        }
    }
    out
}

// py: achievements.py:279 _add
fn add(c: &Connection, key: &str, lab: i64, n: i64) {
    if n > 0 {
        let _ = c.execute("INSERT INTO ach_count(key, lab, n) VALUES(?,?,?) ON CONFLICT(key, lab) DO UPDATE SET n = n + excluded.n", rusqlite::params![key, lab, n]);
    }
}

// py: achievements.py:284 _set_max
fn set_max(c: &Connection, key: &str, lab: i64, n: i64) {
    let _ = c.execute("INSERT INTO ach_count(key, lab, n) VALUES(?,?,?) ON CONFLICT(key, lab) DO UPDATE SET n = MAX(n, excluded.n)", rusqlite::params![key, lab, n]);
}

// py: achievements.py:288 _unlock
fn unlock(c: &Connection, key: &str, aid: &str, now: f64, got: &mut Vec<String>) {
    if by_id(aid).is_none() {
        return;
    }
    if let Ok(n) = c.execute("INSERT OR IGNORE INTO ach(key, aid, unlocked) VALUES(?,?,?)", rusqlite::params![key, aid, now]) {
        if n > 0 {
            got.push(aid.to_string());
        }
    }
}

// py: achievements.py:295 refresh
/// Unlock every counter achievement whose goal is reached (and the match ones `match_` satisfies). Returns the newly unlocked ids.
pub fn refresh(c: &Connection, key: &str, now: Option<f64>, match_: Option<&MatchFacts>, rank: Option<i64>) -> Vec<String> {
    let now = now.unwrap_or_else(clock::now);
    let cnt = counters(c, key);
    let have = unlocked(c, key);
    let mut got = Vec::new();
    for a in ACH.iter() {
        if have.contains_key(&a.id) {
            continue;
        }
        if a.kind == "count" && prog(a, &cnt) >= a.goal {
            unlock(c, key, &a.id, now, &mut got);
        } else if a.kind == "match" && match_.map(|m| test_of(&a.id, m)).unwrap_or(false) {
            unlock(c, key, &a.id, now, &mut got);
        } else if a.kind == "rank" && rank.map(|r| r >= a.goal).unwrap_or(false) {
            unlock(c, key, &a.id, now, &mut got);
        }
    }
    got
}

// py: achievements.py:311 sanitize
/// One match's counters as the daemon may trust them: labels 1..127, at most MAX_PER_MATCH each (23 = best streak is a value), the weapon-kind
/// downs never above the downs, the Special Execution weapon split never above the Special Executions / executions. The caller's order is kept.
pub fn sanitize(counts: &IndexMap<i64, i64>) -> IndexMap<i64, i64> {
    let mut out: IndexMap<i64, i64> = IndexMap::new();
    for (k, v) in counts {
        if (1..=127).contains(k) && *v > 0 && *k != L_TOPWEAPON {
            out.insert(*k, (*v).min(MAX_PER_MATCH));
        }
    }
    let downs = out.get(&L_DOWNS).copied().unwrap_or(0);
    for lab in DOWN_KIND_LABELS.iter().chain([L_BOW_TELL].iter()) {
        if let Some(v) = out.get_mut(lab) {
            *v = (*v).min(downs);
        }
    }
    let execs = out.get(&L_SPECIAL).copied().unwrap_or(0).max(out.get(&L_EXECS).copied().unwrap_or(0));
    let se: i64 = SE_WEAPON_LABELS.iter().map(|l| out.get(l).copied().unwrap_or(0)).sum();
    if se > execs {
        for lab in SE_WEAPON_LABELS {
            if let Some(v) = out.get(&lab).copied() {
                let nv = crate::py::floordiv(v * execs, se);
                if nv <= 0 {
                    out.shift_remove(&lab);
                } else {
                    out.insert(lab, nv);
                }
            }
        }
    }
    let esc = out.get(&L_PIN_ESC).copied().unwrap_or(0);
    if out.get(&L_PIN_ESC_LOW).copied().unwrap_or(0) > esc {
        out.insert(L_PIN_ESC_LOW, esc);
    }
    out
}

// py: achievements.py:337 book_match
/// Credit one match's counters. `done` = {label: n} this match already credited (updated in place); first = the match's first booking
/// (books the match / win / loss counters). Returns the newly unlocked ids.
pub fn book_match(c: &Connection, key: &str, counts: &IndexMap<i64, i64>, done: &mut HashMap<i64, i64>, win: Option<bool>, left: bool, late: bool, first: bool, now: Option<f64>) -> Vec<String> {
    ensure(c);
    let cnt = sanitize(counts);
    for (lab, n) in &cnt {
        if *lab == L_STREAK {
            if *n > done.get(lab).copied().unwrap_or(0) {
                set_max(c, key, *lab, *n);
                done.insert(*lab, *n);
            }
            continue;
        }
        let a = *n - done.get(lab).copied().unwrap_or(0);
        if a > 0 {
            add(c, key, *lab, a);
            done.insert(*lab, *n);
        }
    }
    if first && !left {
        add(c, key, D_MATCHES, 1);
        match win {
            Some(true) => add(c, key, D_WINS, 1),
            Some(false) => add(c, key, D_LOSSES, 1),
            None => {}
        }
    }
    let m = MatchFacts { labs: done.clone(), win: if left { None } else { win }, late };
    refresh(c, key, now, Some(&m), None)
}

// py: achievements.py:361 book_live
/// Op 0xAB: this match's counters so far (labels 1..127 but LIVE_SKIP), credited like book_match (only what is above `done`); no match / win / loss
/// counters, the one-match tests with the win unknown. Returns the new unlocks.
pub fn book_live(c: &Connection, key: &str, counts: &IndexMap<i64, i64>, done: &mut HashMap<i64, i64>, now: Option<f64>) -> Vec<String> {
    ensure(c);
    for (lab, n) in &sanitize(counts) {
        if LIVE_SKIP.contains(lab) {
            continue;
        }
        let a = *n - done.get(lab).copied().unwrap_or(0);
        if a > 0 {
            add(c, key, *lab, a);
            done.insert(*lab, *n);
        }
    }
    let m = MatchFacts { labs: done.clone(), win: None, late: false };
    refresh(c, key, now, Some(&m), None)
}

// py: achievements.py:377 settle
/// The match's authoritative counts arrived (the clamped 0xE2 report): a label the live reports credited above them is taken back to them (an unlock it gave stays).
pub fn settle(c: &Connection, key: &str, auth: &HashMap<i64, i64>, done: &mut HashMap<i64, i64>, labels: &[i64]) {
    ensure(c);
    for lab in labels {
        let v = auth.get(lab).copied().unwrap_or(0);
        if let Some(d) = done.get(lab).copied() {
            if d > v {
                let _ = c.execute("UPDATE ach_count SET n = MAX(0, n - ?) WHERE key=? AND lab=?", rusqlite::params![d - v, key, lab]);
                done.insert(*lab, v);
            }
        }
    }
}

// py: achievements.py:386 book_side
pub fn book_side(c: &Connection, key: &str, now: Option<f64>) -> Vec<String> {
    ensure(c);
    let mut got = Vec::new();
    unlock(c, key, "side", now.unwrap_or_else(clock::now), &mut got);
    got
}

fn gi(v: Option<&Value>, k: &str) -> i64 {
    v.and_then(|x| x.get(k)).map(|x| if crate::clan::clan::truthy(x) { crate::clan::clan::int_of(x) } else { 0 }).unwrap_or(0)
}

// py: achievements.py:394 book_clan
/// A booked clan day: before / after = clan rows (dicts), r = the day's result (clan.resolve_day / _event_step fields).
pub fn book_clan(c: &Connection, key: &str, before: Option<&Value>, after: Option<&Value>, r: Option<&Value>, rank_weeks: Option<i64>, now: Option<f64>) -> Vec<String> {
    ensure(c);
    let now = now.unwrap_or_else(clock::now);
    let mut got: Vec<String> = Vec::new();
    let died = gi(r, "died");
    if died > 0 {
        unlock(c, key, "grave_1", now, &mut got);
    }
    let (st0, st1) = (gi(before, "status"), gi(after, "status"));
    if st1 == DEAD && st0 != DEAD {
        unlock(c, key, "wiped", now, &mut got);
        if died <= 0 {
            unlock(c, key, "grave_1", now, &mut got);
        }
    }
    if st1 == WON && st0 != WON {
        add(c, key, D_WON_ANY, 1);
        let fac = gi(after, "faction");
        if fac == WLF {
            add(c, key, D_WON_WLF, 1);
        } else if fac == SERAPHITES {
            add(c, key, D_WON_SERA, 1);
        }
    }
    let size: i64 = ["healthy", "hungry", "sick"].iter().map(|k| gi(after, k)).sum();
    if size >= 100 {
        unlock(c, key, "haven", now, &mut got);
    }
    if r.and_then(|x| x.get("evk")).map(|v| crate::clan::clan::int_of(v) == 2 && v.is_number()).unwrap_or(false) && gi(r, "evt") >= 3 {
        unlock(c, key, "hold_line", now, &mut got);
    }
    if r.and_then(|x| x.get("mdone")).map(crate::clan::clan::truthy).unwrap_or(false) && gi(r, "mt") >= 3 {
        add(c, key, D_MISSION_TOP, 1);
    }
    let mut rw = rank_weeks;
    if rw.is_none() && after.map(crate::clan::clan::truthy).unwrap_or(false) {
        rw = Some(crate::py::floordiv(gi(after, "days_total"), 7));
    }
    let more = refresh(c, key, Some(now), None, rw);
    let extra: Vec<String> = more.into_iter().filter(|a| !got.contains(a)).collect();
    got.extend(extra);
    got
}

// py: achievements.py:424 rows
/// [(achievement, progress, unlocked time or 0)] in table order, for the UIs.
pub fn rows(c: &Connection, key: &str, rank_weeks: Option<i64>) -> Vec<(&'static Ach, i64, f64)> {
    let cnt = counters(c, key);
    let have = unlocked(c, key);
    let mut out = Vec::new();
    for a in ACH.iter() {
        let mut p = match a.kind.as_str() {
            "count" => a.goal.min(prog(a, &cnt)),
            "rank" => a.goal.min(rank_weeks.unwrap_or(0)),
            _ => 0,
        };
        let t = have.get(&a.id).copied().unwrap_or(0.0);
        if t != 0.0 {
            p = a.goal;
        }
        out.push((a, p, t));
    }
    out
}

// py: achievements.py:440 _clean
fn clean(s: &str) -> String {
    s.replace('|', "/").replace('\n', " ")
}

// py: achievements.py:444 wire_text
/// The 0xAA reply / launcher text. Line 1: 'ACH <unlocked> <total>'. Then one line per achievement in table order:
/// id|group|unlocked unix (0 = locked)|progress|goal|hidden|title|description. A hidden locked one is sent as '?' title / description.
pub fn wire_text(c: &Connection, key: &str, rank_weeks: Option<i64>) -> String {
    let rs = rows(c, key, rank_weeks);
    let mut lines = vec![format!("ACH {} {}", rs.iter().filter(|(_, _, t)| *t != 0.0).count(), rs.len())];
    for (a, p, t) in &rs {
        let hid = a.hidden && *t == 0.0;
        lines.push(
            [
                a.id.clone(),
                a.group.clone(),
                (*t as i64).to_string(),
                (if hid { 0 } else { *p }).to_string(),
                a.goal.to_string(),
                (if a.hidden { "1" } else { "0" }).to_string(),
                if hid { "?".to_string() } else { clean(&a.title) },
                if hid { "Hidden achievement".to_string() } else { clean(&a.desc) },
            ]
            .join("|"),
        );
    }
    lines.join("\n") + "\n"
}

// py: achievements.py:456 public_rows
/// The website profile: dicts (hidden locked ones masked).
pub fn public_rows(c: &Connection, key: &str, rank_weeks: Option<i64>) -> Vec<Value> {
    rows(c, key, rank_weeks)
        .into_iter()
        .map(|(a, p, t)| {
            let hid = a.hidden && t == 0.0;
            json!({"id": a.id, "group": a.group, "title": if hid { "?" } else { a.title.as_str() }, "desc": if hid { "Hidden achievement" } else { a.desc.as_str() },
                   "goal": a.goal, "prog": if hid { 0 } else { p }, "unlocked": t as i64, "hidden": a.hidden})
        })
        .collect()
}
