use crate::clan::{self, Clan};
use crate::util::{file_lines, hex, is_digits, mtime_secs, now, py_strip, read_lossy, round2, round_half_even, take_chars, utc_date};
use rusqlite::types::ValueRef;
use rusqlite::{Connection, OpenFlags, Row};
use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};
use std::collections::{HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

const CACHE_S: f64 = 60.0;
pub const MIN_RATE_MATCHES: i64 = 10;
const CAREER: [&str; 9] = ["kills", "execs", "revives", "headshots", "marked", "downs", "deaths", "streak", "reports"];
const STREAK: usize = 7;
const REPORTS: usize = 8;
const WEAPONS: [(&str, &str); 11] = [
    ("bow", "Bow"),
    ("revolver", "Revolver"),
    ("shorty", "Shorty"),
    ("hunting", "Hunting rifle"),
    ("semi", "Semi-auto rifle"),
    ("burst", "Burst rifle"),
    ("pistol", "9mm pistol"),
    ("crossbow", "Crossbow"),
    ("melee", "Melee"),
    ("bomb", "Bomb"),
    ("molotov", "Molotov"),
];
const RANK_MAX: i64 = 10;
const RANK_NAMES: [&str; 10] = ["Fresh", "Rookie", "Scrapper", "Survivor", "Blooded", "Seasoned", "Veteran", "Hunter", "Hardened", "Ghost"];
const WLF_M: &[&str] = &["militia-male-11", "militia-male-08", "militia-male-12", "militia-male-05", "militia-male-13", "militia-male-14"];
const WLF_F: &[&str] = &["militia-female-13", "militia-female-12"];
const SERA_M: &[&str] = &["scar-male-13", "scar-male-01", "scar-male-12", "scar-male-06", "scar-male-09", "scar-male-02"];
const SERA_F: &[&str] = &["scar-female-11", "scar-female-09"];

pub struct Player {
    pub id: String,
    pub steamid: i64,
    pub name: String,
    seen: f64,
    pub wins: i64,
    pub matches: i64,
    draws: i64,
    pub banned: bool,
    keys: Vec<String>,
    pub career: [i64; 9],
    weapons: Vec<(i64, i64)>,
    clan: Option<usize>,
    weeks: i64,
    runs_won: i64,
    clan_day: i64,
    clan_status: i64,
    faction: i64,
    win_rate: i64,
    pub kd: f64,
    pub elo: i64,
}

pub struct Snapshot {
    pub players: Vec<Player>,
    index: HashMap<String, usize>,
    clans: Vec<Clan>,
    pub stats_tracked: bool,
}

impl Snapshot {
    pub fn get(&self, id: &str) -> Option<&Player> {
        self.index.get(id).map(|i| &self.players[*i])
    }
}

pub struct Players {
    db: PathBuf,
    bans: PathBuf,
    cache: Mutex<Option<(f64, Arc<Snapshot>)>>,
}

fn int(v: ValueRef) -> i64 {
    match v {
        ValueRef::Integer(i) => i,
        ValueRef::Real(f) if f.is_finite() => f.trunc() as i64,
        ValueRef::Text(t) => std::str::from_utf8(t).ok().and_then(crate::util::parse_py_int).unwrap_or(0),
        _ => 0,
    }
}

fn opt_int(v: ValueRef) -> Option<i64> {
    match v {
        ValueRef::Null => None,
        other => Some(int(other)),
    }
}

fn real(v: ValueRef) -> f64 {
    match v {
        ValueRef::Integer(i) => i as f64,
        ValueRef::Real(f) => f,
        ValueRef::Text(t) => std::str::from_utf8(t).ok().and_then(|s| py_strip(s).parse().ok()).unwrap_or(0.0),
        _ => 0.0,
    }
}

fn text(v: ValueRef) -> String {
    match v {
        ValueRef::Text(t) => String::from_utf8_lossy(t).into_owned(),
        ValueRef::Integer(i) => i.to_string(),
        ValueRef::Real(f) => f.to_string(),
        _ => String::new(),
    }
}

fn col<'a>(r: &'a Row<'a>, i: usize) -> ValueRef<'a> {
    r.get_ref(i).unwrap_or(ValueRef::Null)
}

fn columns(c: &Connection, table: &str) -> rusqlite::Result<HashSet<String>> {
    let mut st = c.prepare(&format!("PRAGMA table_info(\"{}\")", table))?;
    let rows = st.query_map([], |r| r.get::<_, String>(1))?;
    rows.collect()
}

fn select(c: &Connection, table: &str, want: &[&str]) -> rusqlite::Result<String> {
    let have = columns(c, table)?;
    let list: Vec<String> = want.iter().map(|w| if have.contains(*w) { format!("\"{}\"", w) } else { "NULL".into() }).collect();
    Ok(format!("SELECT {} FROM \"{}\"", list.join(", "), table))
}

struct Bans {
    ids: HashSet<String>,
    names: HashSet<String>,
    toks: HashSet<String>,
}

fn load_bans(p: &Path) -> Bans {
    let mut b = Bans { ids: HashSet::new(), names: HashSet::new(), toks: HashSet::new() };
    let text = match read_lossy(p) {
        Some(t) => t,
        None => return b,
    };
    for line in file_lines(&text) {
        let e = py_strip(line.split('#').next().unwrap_or(""));
        let low = e.to_lowercase();
        if e.is_empty() || low.starts_with("hwid:") {
            continue;
        }
        if low.starts_with("token:") {
            let rest: String = e.chars().skip(6).collect();
            b.toks.insert(py_strip(&rest).to_lowercase());
        } else if is_digits(e) {
            b.ids.insert(e.to_string());
        } else {
            b.names.insert(low);
        }
    }
    b
}

fn pid_for(token: &str, steamid: i64) -> String {
    if steamid != 0 {
        format!("s{}", steamid)
    } else {
        let h = Sha256::digest(token.as_bytes());
        format!("p{}", hex(&h[..8]))
    }
}

fn token_key(tok: &str) -> String {
    tok.chars().skip(40).take(24).collect::<String>().to_lowercase()
}

fn build(db: &Path, bans_path: &Path) -> rusqlite::Result<Snapshot> {
    let bans = load_bans(bans_path);
    let c = Connection::open_with_flags(db, OpenFlags::SQLITE_OPEN_READ_ONLY | OpenFlags::SQLITE_OPEN_NO_MUTEX)?;
    c.busy_timeout(Duration::from_secs(3))?;
    c.execute_batch("PRAGMA cache_size=-512")?;
    let tables: HashSet<String> = {
        let mut st = c.prepare("SELECT name FROM sqlite_master WHERE type='table'")?;
        let rows = st.query_map([], |r| Ok(text(col(r, 0))))?;
        rows.collect::<rusqlite::Result<_>>()?
    };
    let mut names: HashMap<String, String> = HashMap::new();
    {
        let mut st = c.prepare("SELECT token_id, name FROM names")?;
        let mut rows = st.query([])?;
        while let Some(r) = rows.next()? {
            names.insert(text(col(r, 0)), text(col(r, 1)));
        }
    }
    struct Raw {
        token: String,
        name: String,
        wins: i64,
        matches: i64,
        draws: i64,
        steamid: i64,
        seen: f64,
    }
    let mut raw = Vec::new();
    {
        let mut st = c.prepare("SELECT token, name, kills, deaths, wins, matches, draws, steamid, last_seen FROM players")?;
        let mut rows = st.query([])?;
        while let Some(r) = rows.next()? {
            raw.push(Raw {
                token: text(col(r, 0)),
                name: text(col(r, 1)),
                wins: int(col(r, 4)),
                matches: int(col(r, 5)),
                draws: int(col(r, 6)),
                steamid: int(col(r, 7)),
                seen: real(col(r, 8)),
            });
        }
    }
    let mut clans: Vec<Clan> = Vec::new();
    let mut clan_ix: HashMap<String, usize> = HashMap::new();
    {
        let q = select(&c, "clans", &[
            "token_id", "faction", "status", "day", "healthy", "hungry", "sick", "weeks_total", "runs_won", "runs_lost", "days_total", "meta", "last_result",
        ])?;
        let mut st = c.prepare(&q)?;
        let mut rows = st.query([])?;
        while let Some(r) = rows.next()? {
            let (max_meta, mission) = clan::parse_meta(&text(col(r, 11)));
            let cl = Clan {
                faction: int(col(r, 1)),
                status: int(col(r, 2)),
                day: int(col(r, 3)),
                healthy: int(col(r, 4)),
                hungry: int(col(r, 5)),
                sick: int(col(r, 6)),
                weeks_total: int(col(r, 7)),
                runs_won: int(col(r, 8)),
                runs_lost: int(col(r, 9)),
                days_total: opt_int(col(r, 10)),
                max_meta,
                mission,
                last: clan::parse_last(&text(col(r, 12))),
                boosters: [0; 7],
            };
            let key = text(col(r, 0));
            match clan_ix.get(&key) {
                Some(i) => clans[*i] = cl,
                None => {
                    clan_ix.insert(key, clans.len());
                    clans.push(cl);
                }
            }
        }
    }
    if tables.contains("boosters") {
        let mut st = c.prepare("SELECT token_id, bid, n FROM boosters")?;
        let mut rows = st.query([])?;
        while let Some(r) = rows.next()? {
            if let Some(i) = clan_ix.get(&text(col(r, 0))) {
                let bid = int(col(r, 1));
                if (1..=7).contains(&bid) {
                    clans[*i].boosters[(bid - 1) as usize] = int(col(r, 2));
                }
            }
        }
    }
    let stats_tracked = tables.contains("pstats");
    let mut pst: HashMap<String, [i64; 9]> = HashMap::new();
    if stats_tracked {
        let mut want = vec!["token"];
        want.extend_from_slice(&CAREER);
        let q = select(&c, "pstats", &want)?;
        let mut st = c.prepare(&q)?;
        let mut rows = st.query([])?;
        while let Some(r) = rows.next()? {
            let mut v = [0i64; 9];
            for (i, x) in v.iter_mut().enumerate() {
                *x = int(col(r, i + 1));
            }
            pst.insert(text(col(r, 0)), v);
        }
    }
    let mut pwp: HashMap<String, Vec<(i64, i64)>> = HashMap::new();
    if tables.contains("pweapon") {
        let mut st = c.prepare("SELECT token, weapon, n FROM pweapon")?;
        let mut rows = st.query([])?;
        while let Some(r) = rows.next()? {
            let e = pwp.entry(text(col(r, 0))).or_default();
            let w = int(col(r, 1));
            let n = int(col(r, 2));
            match e.iter_mut().find(|x| x.0 == w) {
                Some(x) => x.1 = n,
                None => e.push((w, n)),
            }
        }
    }
    let mut elo: HashMap<String, f64> = HashMap::new();
    if tables.contains("ratings") {
        let mut st = c.prepare("SELECT key, elo, n FROM ratings")?;
        let mut rows = st.query([])?;
        while let Some(r) = rows.next()? {
            elo.insert(text(col(r, 0)), real(col(r, 1)));
        }
    }
    drop(c);

    let mut players: Vec<Player> = Vec::new();
    let mut index: HashMap<String, usize> = HashMap::new();
    for r in raw {
        let sid = r.steamid;
        let pid = pid_for(&r.token, sid);
        let i = match index.get(&pid) {
            Some(i) => *i,
            None => {
                index.insert(pid.clone(), players.len());
                players.push(Player {
                    id: pid,
                    steamid: sid,
                    name: String::new(),
                    seen: 0.0,
                    wins: 0,
                    matches: 0,
                    draws: 0,
                    banned: false,
                    keys: Vec::new(),
                    career: [0; 9],
                    weapons: Vec::new(),
                    clan: None,
                    weeks: 0,
                    runs_won: 0,
                    clan_day: 0,
                    clan_status: 0,
                    faction: 0,
                    win_rate: 0,
                    kd: 0.0,
                    elo: 0,
                });
                players.len() - 1
            }
        };
        let p = &mut players[i];
        p.wins += r.wins;
        p.matches += r.matches;
        p.draws += r.draws;
        if let Some(cs) = pst.get(&r.token) {
            for k in 0..9 {
                if k == STREAK {
                    p.career[k] = p.career[k].max(cs[k]);
                } else {
                    p.career[k] += cs[k];
                }
            }
        }
        if let Some(ws) = pwp.get(&r.token) {
            for (w, n) in ws {
                match p.weapons.iter_mut().find(|x| x.0 == *w) {
                    Some(x) => x.1 += n,
                    None => p.weapons.push((*w, *n)),
                }
            }
        }
        if r.seen >= p.seen {
            p.seen = r.seen;
            let steam_name = if sid != 0 { names.get(&format!("s:{}", sid)).filter(|n| !n.is_empty()) } else { None };
            if let Some(n) = steam_name {
                p.name = n.clone();
            } else if !r.name.is_empty() {
                p.name = r.name.clone();
            }
        }
        p.keys.push(token_key(&r.token));
        if bans.toks.contains(&r.token.to_lowercase()) || (sid != 0 && bans.ids.contains(&sid.to_string())) || bans.names.contains(&r.name.to_lowercase()) {
            p.banned = true;
        }
    }
    let tw: i64 = players.iter().map(|p| p.wins).sum();
    let tm: i64 = players.iter().map(|p| p.matches).sum();
    let p0 = if tm != 0 { (tw as f64 / tm as f64).clamp(0.1, 0.9) } else { 0.4 };
    for p in players.iter_mut() {
        if bans.names.contains(&p.name.to_lowercase()) {
            p.banned = true;
        }
        let skey = format!("s:{}", p.steamid);
        let ck = if p.steamid != 0 && clan_ix.contains_key(&skey) { Some(skey) } else { p.keys.iter().find(|k| clan_ix.contains_key(*k)).cloned() };
        p.clan = ck.and_then(|k| clan_ix.get(&k).copied());
        if let Some(cl) = p.clan.map(|i| &clans[i]) {
            p.weeks = cl.weeks_total;
            p.runs_won = cl.runs_won;
            p.clan_day = cl.day;
            p.clan_status = cl.status;
            if let Some(dt) = cl.days_total {
                p.weeks = dt.div_euclid(7);
            }
            p.faction = cl.faction;
        }
        p.win_rate = if p.matches != 0 { round_half_even(100.0 * p.wins as f64 / p.matches as f64) } else { 0 };
        let (k, d) = (p.career[0], p.career[6]);
        p.kd = if d != 0 { round2(k as f64 / d as f64) } else { k as f64 };
        let er = if p.steamid != 0 { elo.get(&format!("s{}", p.steamid)).copied() } else { p.keys.iter().find_map(|k| elo.get(&format!("t{}", k)).copied()) };
        let e = match er {
            Some(e) => e,
            None => {
                let wr = (p.wins as f64 + 0.5 * p.draws as f64 + 40.0 * p0) / (p.matches as f64 + 40.0);
                1000.0 + 400.0 * ((wr / (1.0 - wr)) / (p0 / (1.0 - p0))).log10()
            }
        };
        p.elo = round_half_even(e - 200.0 / ((p.matches + 1) as f64).sqrt());
    }
    Ok(Snapshot { players, index, clans, stats_tracked })
}

impl Players {
    pub fn new(db: PathBuf) -> Players {
        let dir = db.parent().map(Path::to_path_buf).unwrap_or_default();
        Players { db, bans: dir.join("bans.txt"), cache: Mutex::new(None) }
    }

    pub fn snapshot(&self) -> Result<Arc<Snapshot>, String> {
        let mut g = self.cache.lock().unwrap_or_else(|e| e.into_inner());
        if let Some((t, s)) = &*g {
            if now() - *t < CACHE_S {
                return Ok(s.clone());
            }
        }
        *g = None;
        let s = Arc::new(build(&self.db, &self.bans).map_err(|e| e.to_string())?);
        *g = Some((now(), s.clone()));
        Ok(s)
    }
}

fn rank_of(p: &Player) -> (i64, Value) {
    let lvl = RANK_MAX.min(p.runs_won + 1).max(1);
    let alive = p.clan_status == 1;
    (
        lvl,
        json!({
            "level": lvl,
            "tier": lvl - 1,
            "title": RANK_NAMES[(lvl - 1) as usize],
            "max": RANK_MAX,
            "runs": p.runs_won,
            "weeks": p.weeks,
            "alive": alive,
            "day": if alive { clan::WIN_DAY.min(p.clan_day) } else { 0 },
            "runDays": clan::WIN_DAY,
            "next": if lvl < RANK_MAX { Value::from(lvl + 1) } else { Value::Null },
        }),
    )
}

pub struct Ctx<'a> {
    pub snap: &'a Snapshot,
    pub leads: &'a HashSet<i64>,
    pub site: &'a Path,
}

pub fn public(cx: &Ctx, p: &Player, full: bool) -> Map<String, Value> {
    let mut o = Map::new();
    o.insert("id".into(), Value::from(p.id.as_str()));
    o.insert("name".into(), Value::from(if p.name.is_empty() { "Survivor" } else { p.name.as_str() }));
    o.insert("steam".into(), Value::Bool(p.steamid != 0));
    o.insert("wins".into(), Value::from(p.wins));
    o.insert("matches".into(), Value::from(p.matches));
    o.insert("draws".into(), Value::from(p.draws));
    o.insert("losses".into(), Value::from((p.matches - p.wins - p.draws).max(0)));
    o.insert("winRate".into(), Value::from(p.win_rate));
    o.insert("weeks".into(), Value::from(p.weeks));
    o.insert("runsWon".into(), Value::from(p.runs_won));
    o.insert("faction".into(), Value::from(match p.faction {
        1 => "WLF",
        2 => "SERAPHITES",
        _ => "",
    }));
    o.insert("elo".into(), Value::from(p.elo));
    o.insert("kd".into(), Value::from(p.kd));
    o.insert("kills".into(), Value::from(p.career[0]));
    o.insert("deaths".into(), Value::from(p.career[6]));
    o.insert("lastSeen".into(), Value::from(if p.seen != 0.0 { utc_date(p.seen.floor() as i64) } else { String::new() }));
    if p.steamid != 0 {
        o.insert("steamProfile".into(), Value::from(format!("https://steamcommunity.com/profiles/{}", p.steamid)));
    }
    let (lvl, rank) = rank_of(p);
    o.insert("rank".into(), rank);
    let lead = p.steamid != 0 && cx.leads.contains(&p.steamid);
    if lead {
        o.insert("role".into(), Value::from("lead"));
    }
    if full {
        let clan = match p.clan {
            Some(i) => cx.snap.clans[i].public(),
            None => json!({"hasClan": false}),
        };
        o.insert("clan".into(), clan);
        let mut cr = Map::new();
        for (i, k) in CAREER.iter().enumerate() {
            cr.insert((*k).into(), Value::from(p.career[i]));
        }
        let (k, d) = (p.career[0], p.career[6]);
        cr.insert("kd".into(), Value::from(if d != 0 { round2(k as f64 / d as f64) } else { k as f64 }));
        cr.insert("tracked".into(), Value::from(p.career[REPORTS]));
        o.insert("career".into(), Value::Object(cr));
        o.insert("statsTracked".into(), Value::Bool(cx.snap.stats_tracked));
        let mut fav: Option<(i64, i64)> = None;
        for (w, n) in &p.weapons {
            if fav.is_none_or(|f| *n > f.1) {
                fav = Some((*w, *n));
            }
        }
        let fw = match fav {
            Some((w, n)) if w > 0 && (w as usize) <= WEAPONS.len() => {
                let (key, name) = WEAPONS[(w - 1) as usize];
                json!({"key": key, "name": name, "matches": n})
            }
            _ => Value::Null,
        };
        o.insert("favWeapon".into(), fw);
        let h = Sha256::digest(p.id.as_bytes());
        let hv = u32::from_be_bytes([h[0], h[1], h[2], h[3]]);
        let fac = match p.faction {
            1 => "wlf",
            2 => "sera",
            _ if hv & 1 != 0 => "wlf",
            _ => "sera",
        };
        let female = hv & 2 != 0;
        let kellie = [if fac == "sera" { "kellie-ser" } else { "kellie-wlf" }];
        let pool: &[&str] = if lead {
            &kellie
        } else {
            match (fac, female) {
                ("wlf", false) => WLF_M,
                ("wlf", true) => WLF_F,
                (_, false) => SERA_M,
                (_, true) => SERA_F,
            }
        };
        let look = pool[((hv >> 2) as usize) % pool.len()];
        let rel = format!("assets/portraits/{}-{}.png", look, lvl - 1);
        let v = mtime_secs(&cx.site.join(&rel));
        o.insert("portrait".into(), Value::from(format!("/{}?v={}", rel, v)));
    }
    o
}

pub fn search(cx: &Ctx, q: &str) -> Value {
    let s = take_chars(&py_strip(q).to_lowercase(), 32).to_string();
    if s.chars().count() < 2 {
        return Value::Array(Vec::new());
    }
    let mut hits: Vec<(&Player, String)> = cx
        .snap
        .players
        .iter()
        .filter(|p| !p.banned && p.matches > 0)
        .map(|p| (p, p.name.to_lowercase()))
        .filter(|(_, n)| n.contains(&s))
        .collect();
    hits.sort_by_key(|(p, n)| (!n.starts_with(&s), -p.matches));
    Value::Array(hits.iter().take(20).map(|(p, _)| Value::Object(public(cx, p, false))).collect())
}

pub fn leaderboard(cx: &Ctx, by: &str) -> Vec<Value> {
    let mut ps: Vec<&Player> = cx.snap.players.iter().filter(|p| !p.banned && p.matches > 0).collect();
    if by == "elo" {
        ps.retain(|p| p.matches >= MIN_RATE_MATCHES);
    }
    if by == "kd" {
        ps.retain(|p| p.career[REPORTS] >= MIN_RATE_MATCHES);
    }
    match by {
        "elo" => ps.sort_by_key(|p| std::cmp::Reverse((p.elo, p.matches))),
        "kd" => ps.sort_by(|a, b| b.kd.total_cmp(&a.kd).then(b.matches.cmp(&a.matches))),
        _ => ps.sort_by_key(|p| std::cmp::Reverse((p.wins, p.matches))),
    }
    ps.iter()
        .take(50)
        .enumerate()
        .map(|(i, p)| {
            let mut o = public(cx, p, false);
            o.insert("rank".into(), Value::from(i + 1));
            Value::Object(o)
        })
        .collect()
}
