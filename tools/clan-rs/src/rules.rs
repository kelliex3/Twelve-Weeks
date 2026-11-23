pub const WEEKS: i64 = 12;
pub const DAYS: i64 = WEEKS * 7;
pub const WIN_DAY: i64 = 83;
pub const EVENT_LEN: i64 = 3;
pub const BOOSTER_EVERY: i64 = 5;

pub const NONE: i64 = 0;
pub const WLF: i64 = 1;
pub const SERAPHITES: i64 = 2;
pub const ALIVE: i64 = 1;
pub const DEAD: i64 = 2;
pub const WON: i64 = 3;

pub const EV_NONE: i64 = 0;
pub const EV_GROWTH: i64 = 1;
pub const EV_ATTACK: i64 = 2;
pub const AN_ENEMY: i64 = 0;
pub const AN_HORDE: i64 = 1;
pub const GN_SPOIL: i64 = 5;

pub const GROWTH_NAMES: [&str; 5] = ["FRIENDLY SURVIVORS", "DYSENTERY OUTBREAK", "MALARIA OUTBREAK", "RESCUE ALLIES", "MARAUDER ATTACK"];
pub const GROWTH_NAMES_ALL: [&str; 6] = ["FRIENDLY SURVIVORS", "DYSENTERY OUTBREAK", "MALARIA OUTBREAK", "RESCUE ALLIES", "MARAUDER ATTACK", "SPOILED SUPPLIES"];
pub const ATTACK_NAMES: [&str; 2] = ["ATTACK", "INFECTED HORDE"];
pub const HORDE_DAYS: [i64; 2] = [30, 65];
pub const SPOIL_DAYS: [i64; 2] = [45, 70];
pub const ATTACK_DAYS: [i64; 7] = [15, 30, 35, 50, 65, 75, 80];

pub const BOOSTERS: [(i64, &str); 7] = [
    (1, "Starting Ammo Multiplier"),
    (2, "Cheaper Ammo"),
    (3, "Cheaper Armor"),
    (4, "Cheaper Purchased Weapons"),
    (5, "Starting Small Firearm Upgrade"),
    (6, "Starting Large Firearm Upgrade"),
    (7, "Increased Part Earnings"),
];

pub const SUPPLY_CATS: [&str; 5] = ["food", "water", "med", "ammo", "mat"];

pub const MISSIONS: [(i64, &str, [i64; 8]); 33] = [
    (1, "Down Enemies", [3, 6, 9, 15, 20, 25, 35, 45]),
    (2, "Executions", [2, 4, 7, 12, 15, 20, 35, 40]),
    (3, "Revives", [2, 4, 6, 8, 10, 15, 20, 25]),
    (4, "Headshots", [2, 4, 6, 8, 10, 12, 16, 20]),
    (5, "Mark Enemies", [7, 14, 21, 28, 35, 42, 48, 56]),
    (6, "Heal Teammates", [5, 10, 15, 20, 25, 30, 35, 40]),
    (7, "Special Executions", [2, 4, 6, 8, 10, 12, 16, 20]),
    (8, "Down with Molotovs", [2, 4, 6, 8, 10, 12, 16, 20]),
    (9, "Down with Nail Bombs", [2, 4, 6, 8, 10, 12, 16, 20]),
    (10, "Give Crafting Items", [2, 4, 6, 8, 10, 12, 16, 20]),
    (11, "Down with Melee", [3, 6, 9, 12, 15, 18, 25, 30]),
    (12, "Shiv Executions", [1, 2, 3, 5, 7, 9, 12, 15]),
    (13, "Downs from Long Range", [2, 4, 6, 8, 10, 16, 20, 24]),
    (14, "Downs with 9mm Pistol", [3, 6, 9, 15, 20, 25, 35, 45]),
    (15, "Downs with Revolver", [3, 6, 9, 15, 20, 25, 35, 45]),
    (16, "Downs with Shorty", [3, 6, 9, 15, 20, 25, 35, 45]),
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

pub fn faction_name(f: i64) -> &'static str {
    match f {
        WLF => "WLF",
        SERAPHITES => "SERAPHITES",
        _ => "",
    }
}

pub fn status_name(s: i64) -> &'static str {
    match s {
        ALIVE => "alive",
        DEAD => "dead",
        WON => "won",
        _ => "none",
    }
}

pub fn token_id(token: &str) -> String {
    let t = token.to_lowercase();
    if t.chars().count() == 64 {
        t.chars().skip(40).collect()
    } else {
        t
    }
}

pub fn needed(n: i64) -> i64 {
    if n > 0 {
        5 + 2 * n - (6 * n).div_euclid(5)
    } else {
        0
    }
}

pub fn week_day(day: i64) -> (i64, i64) {
    let d = day.min(DAYS - 1);
    (d.div_euclid(7) + 1, d.rem_euclid(7) + 1)
}

pub fn event_kind(day: i64) -> Option<i64> {
    if ATTACK_DAYS.contains(&day) {
        Some(EV_ATTACK)
    } else if (5..81).contains(&day) && day % 5 == 0 {
        Some(EV_GROWTH)
    } else {
        None
    }
}

pub fn event_today(day: i64) -> (i64, i64) {
    match event_kind(day) {
        None => (EV_NONE, 0),
        Some(EV_GROWTH) => (EV_GROWTH, if SPOIL_DAYS.contains(&day) { GN_SPOIL } else { day.div_euclid(5).rem_euclid(GROWTH_NAMES.len() as i64) }),
        Some(_) => (EV_ATTACK, if HORDE_DAYS.contains(&day) { AN_HORDE } else { AN_ENEMY }),
    }
}

pub fn event_window(day: i64) -> i64 {
    let mut s = day;
    while s > (day - EVENT_LEN).max(-1) {
        if event_kind(s).is_some() {
            return s;
        }
        s -= 1;
    }
    -1
}

pub fn event_name(kind: i64, name_id: i64) -> &'static str {
    match kind {
        EV_ATTACK => ATTACK_NAMES[if (0..ATTACK_NAMES.len() as i64).contains(&name_id) { name_id as usize } else { 0 }],
        EV_GROWTH => GROWTH_NAMES_ALL[name_id.rem_euclid(GROWTH_NAMES_ALL.len() as i64) as usize],
        _ => "",
    }
}

pub fn mission(mid: i64) -> Option<&'static (i64, &'static str, [i64; 8])> {
    MISSIONS.iter().find(|m| m.0 == mid)
}

pub fn mission_goals(mid: i64, repeats: i64) -> Option<[i64; 3]> {
    let g = &mission(mid)?.2;
    let r = repeats.max(0);
    let at = |t: i64| g[((r + t).min(g.len() as i64 - 1)) as usize];
    Some([at(0), at(1), at(2)])
}

pub fn booster_next(healthy: i64, hungry: i64, sick: i64, boost_hi: i64) -> i64 {
    let size = healthy + hungry + sick;
    ((boost_hi.max(size.div_euclid(BOOSTER_EVERY)) + 1) * BOOSTER_EVERY - size).max(1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn calendar_matches_clan_py() {
        let want: Vec<(i64, i64)> = vec![(1, 1), (1, 2), (2, 0), (1, 4), (1, 0), (2, 1), (2, 0), (1, 3), (1, 5), (2, 0), (1, 1), (1, 2), (2, 1), (1, 5), (2, 0), (2, 0)];
        let got: Vec<(i64, i64)> = (0..84).filter(|d| event_kind(*d).is_some()).map(event_today).collect();
        assert_eq!(got, want);
        assert_eq!(event_window(17), 15);
        assert_eq!(event_window(18), -1);
        assert_eq!(event_window(1), -1);
        assert_eq!(needed(5), 9);
        assert_eq!(needed(38), 36);
        assert_eq!(week_day(83), (12, 7));
        assert_eq!(week_day(90), (12, 7));
        assert_eq!(booster_next(5, 0, 0, 1), 5);
        assert_eq!(mission_goals(1, 7), Some([45, 45, 45]));
        assert_eq!(event_name(EV_GROWTH, GN_SPOIL), "SPOILED SUPPLIES");
        assert_eq!(token_id(&"A".repeat(40).chars().chain("B".repeat(24).chars()).collect::<String>()), "b".repeat(24));
    }
}
