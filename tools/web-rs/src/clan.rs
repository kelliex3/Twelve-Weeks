use crate::util::{py_int, py_truthy};
use serde_json::{json, Map, Value};

pub const WIN_DAY: i64 = 83;
const WEEKS: i64 = 12;
const DAYS: i64 = WEEKS * 7;
const EVENT_LEN: i64 = 3;
const ALIVE: i64 = 1;
const GROWTH_NAMES: [&str; 3] = ["FRIENDLY GROUP", "RESCUED HOSTAGES", "RESCUE ALLIES"];
const ATTACK_DAYS: [i64; 7] = [15, 30, 35, 50, 65, 75, 80];
const BOOSTERS: [&str; 7] = [
    "Starting Ammo Multiplier",
    "Cheaper Ammo",
    "Cheaper Armor",
    "Cheaper Purchased Weapons",
    "Starting Small Firearm Upgrade",
    "Starting Large Firearm Upgrade",
    "Increased Part Earnings",
];
const MISSIONS: [(i64, &str, [i64; 8]); 33] = [
    (1, "Down Enemies", [3, 6, 9, 15, 20, 25, 35, 45]),
    (2, "Executions", [2, 4, 7, 12, 15, 20, 35, 40]),
    (3, "Revives", [2, 4, 6, 8, 10, 15, 20, 25]),
    (4, "Headshots", [2, 4, 6, 8, 10, 12, 16, 20]),
    (5, "Mark Enemies", [7, 14, 21, 28, 35, 42, 48, 56]),
    (6, "Heal Teammates", [5, 10, 15, 20, 25, 30, 35, 40]),
    (7, "Special Executions", [2, 4, 6, 8, 10, 12, 16, 20]),
    (8, "Molotov Executions", [2, 4, 6, 8, 10, 12, 16, 20]),
    (9, "Downs with Bombs", [2, 4, 6, 8, 10, 12, 16, 20]),
    (10, "Gift Crafting Items", [2, 4, 6, 8, 10, 12, 16, 20]),
    (11, "Downs with Melee", [3, 6, 9, 12, 15, 18, 25, 30]),
    (12, "Shiv Executions", [1, 2, 3, 5, 7, 9, 12, 15]),
    (13, "Downs from Long Range", [2, 4, 6, 8, 10, 16, 20, 24]),
    (14, "Downs with 9mm Pistol", [3, 6, 9, 15, 20, 25, 35, 45]),
    (15, "Downs with Revolver", [3, 6, 9, 15, 20, 25, 35, 45]),
    (16, "Downs with the Shorty", [3, 6, 9, 15, 20, 25, 35, 45]),
    (17, "Downs with Hunting Rifle", [3, 6, 9, 15, 20, 25, 35, 45]),
    (18, "Downs with Semi Auto Rifle", [3, 6, 9, 15, 20, 25, 35, 45]),
    (19, "Downs with Burst Rifle", [3, 6, 9, 15, 20, 25, 35, 45]),
    (20, "Downs with Bow", [3, 6, 9, 15, 20, 25, 35, 45]),
    (40, "Variable Rifle Executions", [2, 3, 5, 7, 9, 12, 15, 20]),
    (41, "Hunting Rifle Executions", [1, 2, 3, 5, 7, 9, 12, 15]),
    (42, "Bow Executions", [1, 2, 3, 5, 7, 9, 12, 15]),
    (43, "Full-Auto Rifle Executions", [2, 4, 6, 8, 10, 12, 16, 20]),
    (44, "Semi-Auto Rifle Executions", [2, 3, 5, 7, 9, 12, 15, 20]),
    (45, "Tactical Shotgun Executions", [2, 3, 5, 7, 9, 12, 15, 20]),
    (46, "Frontier Rifle Executions", [1, 2, 3, 5, 7, 9, 12, 15]),
    (47, "Burst Rifle Executions", [2, 3, 5, 7, 9, 12, 15, 20]),
    (48, "9mm Pistol Executions", [2, 4, 6, 8, 10, 12, 16, 20]),
    (49, "Revolver Executions", [1, 2, 3, 5, 7, 9, 12, 15]),
    (50, "Burst Pistol Executions", [2, 4, 6, 8, 10, 12, 16, 20]),
    (51, "Enforcer Executions", [2, 3, 5, 7, 9, 12, 15, 20]),
    (52, "Shorty Executions", [1, 2, 3, 5, 7, 9, 12, 15]),
];
const LAST_KEYS: [(&str, &str, i64); 9] = [
    ("day", "day", -1),
    ("parts", "parts", 0),
    ("supplies", "supplies", 0),
    ("needed", "needed", 0),
    ("gained", "gained", 0),
    ("died", "died", 0),
    ("newHungry", "new_hungry", 0),
    ("newSick", "new_sick", 0),
    ("healed", "healed", 0),
];

pub struct Mission {
    id: i64,
    prog: i64,
    left: i64,
    ev: i64,
    rep: i64,
}

pub struct Clan {
    pub faction: i64,
    pub status: i64,
    pub day: i64,
    pub healthy: i64,
    pub hungry: i64,
    pub sick: i64,
    pub weeks_total: i64,
    pub runs_won: i64,
    pub runs_lost: i64,
    pub days_total: Option<i64>,
    pub max_meta: i64,
    pub mission: Option<Mission>,
    pub last: Option<Box<[Value; 10]>>,
    pub boosters: [i64; 7],
}

fn json_obj(s: &str) -> Map<String, Value> {
    match serde_json::from_str::<Value>(s) {
        Ok(Value::Object(m)) => m,
        _ => Map::new(),
    }
}

fn mission(id: i64) -> Option<&'static (i64, &'static str, [i64; 8])> {
    MISSIONS.iter().find(|m| m.0 == id)
}

pub fn parse_meta(s: &str) -> (i64, Option<Mission>) {
    let meta = json_obj(s);
    let max = meta.get("max").map_or(Some(0), py_int).unwrap_or(0);
    let m = match meta.get("m") {
        Some(Value::Object(m)) if !m.is_empty() => m,
        _ => return (max, None),
    };
    let id = match m.get("id").map_or(Some(0), py_int) {
        Some(id) if mission(id).is_some() => id,
        _ => return (max, None),
    };
    let int = |k: &str, d: i64| m.get(k).map_or(Some(d), py_int);
    let rep = match meta.get("rep") {
        None => Some(0),
        Some(Value::Object(r)) => r.get(&id.to_string()).map_or(Some(0), py_int),
        Some(_) => None,
    };
    match (int("prog", 0), int("left", 0), int("ev", -1), rep) {
        (Some(prog), Some(left), Some(ev), Some(rep)) => (max, Some(Mission { id, prog, left, ev, rep })),
        _ => (max, None),
    }
}

pub fn parse_last(s: &str) -> Option<Box<[Value; 10]>> {
    let last = json_obj(s);
    if last.is_empty() {
        return None;
    }
    let mut out: [Value; 10] = Default::default();
    for (i, (_, k, d)) in LAST_KEYS.iter().enumerate() {
        out[i] = last.get(*k).cloned().unwrap_or_else(|| Value::from(*d));
    }
    out[9] = Value::Bool(last.get("left").is_some_and(py_truthy));
    Some(Box::new(out))
}

fn event_attack(d: i64) -> Option<bool> {
    if (5..=80).contains(&d) && d % 5 == 0 {
        Some(ATTACK_DAYS.contains(&d))
    } else {
        None
    }
}

fn event_window(day: i64) -> i64 {
    let lo = (-1i64).max(day - EVENT_LEN);
    let mut s = day;
    while s > lo {
        if event_attack(s).is_some() {
            return s;
        }
        s -= 1;
    }
    -1
}

impl Clan {
    pub fn public(&self) -> Value {
        let size = self.healthy + self.hungry + self.sick;
        let alive = self.status == ALIVE;
        let mut o = Map::new();
        o.insert("hasClan".into(), Value::Bool(self.status != 0));
        o.insert("factionName".into(), Value::from(match self.faction {
            1 => "WLF",
            2 => "SERAPHITES",
            _ => "",
        }));
        o.insert("status".into(), Value::from(match self.status {
            0 => "none",
            1 => "alive",
            2 => "dead",
            3 => "won",
            _ => "none",
        }));
        o.insert("daysDone".into(), Value::from(self.day));
        o.insert("daysTotal".into(), Value::from(WIN_DAY));
        o.insert("weeksTotal".into(), Value::from(self.weeks_total));
        o.insert("size".into(), Value::from(size));
        o.insert("healthy".into(), Value::from(self.healthy));
        o.insert("hungry".into(), Value::from(self.hungry));
        o.insert("sick".into(), Value::from(self.sick));
        o.insert("maxSize".into(), Value::from(self.max_meta.max(size)));
        o.insert("runsWon".into(), Value::from(self.runs_won));
        o.insert("runsLost".into(), Value::from(self.runs_lost));
        if alive {
            let d = self.day.min(DAYS - 1);
            o.insert("week".into(), Value::from(d.div_euclid(7) + 1));
            o.insert("dayOfWeek".into(), Value::from(d.rem_euclid(7) + 1));
            o.insert("weeks".into(), Value::from(WEEKS));
            let s = event_window(self.day);
            let ev = match event_attack(s) {
                Some(attack) if s >= 0 => json!({
                    "kind": if attack { "attack" } else { "growth" },
                    "name": if attack { "ATTACK" } else { GROWTH_NAMES[(s / 5) as usize % GROWTH_NAMES.len()] },
                    "startDay": s,
                    "dayOfEvent": self.day - s + 1,
                    "length": EVENT_LEN,
                }),
                _ => Value::Null,
            };
            o.insert("event".into(), ev);
            let mi = match &self.mission {
                Some(m) => match mission(m.id) {
                    Some((_, name, goals)) => {
                        let r = m.rep.max(0);
                        let g: Vec<i64> = (0..3).map(|t| goals[((r + t) as usize).min(goals.len() - 1)]).collect();
                        json!({
                            "id": m.id,
                            "name": name,
                            "progress": m.prog,
                            "daysLeft": m.left,
                            "goals": g,
                            "inEvent": m.ev >= 0,
                        })
                    }
                    None => Value::Null,
                },
                None => Value::Null,
            };
            o.insert("mission".into(), mi);
        }
        let boosters: Vec<Value> = BOOSTERS
            .iter()
            .enumerate()
            .map(|(i, n)| json!({"id": i + 1, "name": n, "count": self.boosters[i]}))
            .collect();
        o.insert("boosters".into(), Value::Array(boosters));
        if let Some(last) = &self.last {
            let mut lm = Map::new();
            for (i, (k, _, _)) in LAST_KEYS.iter().enumerate() {
                lm.insert((*k).into(), last[i].clone());
            }
            lm.insert("leftEarly".into(), last[9].clone());
            o.insert("lastMatch".into(), Value::Object(lm));
        }
        Value::Object(o)
    }
}
