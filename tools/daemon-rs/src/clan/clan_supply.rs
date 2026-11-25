//! py: clan_supply.py - clan supplies per category (food, water, medicine, ammo, materials), imported by clan::apply_match.
use crate::clan::clan::{int_of, Counts};
use crate::clan::data;
use crate::py;
use serde_json::{Map, Value};

// py: clan_supply.py:21 CATS / SURVIVAL / BANK_DAYS / STOCK_CAP (checked against the generated table by `consts_ok`)
pub const CATS: [&str; 5] = ["food", "water", "med", "ammo", "mat"];
pub const SURVIVAL: [&str; 3] = ["food", "water", "med"];
pub const BANK_DAYS: i64 = 3;
pub const STOCK_CAP: i64 = 999;

pub fn consts_ok() -> bool {
    data::strs(data::t("clan_supply", "CATS")) == CATS
        && data::strs(data::t("clan_supply", "SURVIVAL")) == SURVIVAL
        && data::t("clan_supply", "BANK_DAYS").as_i64() == Some(BANK_DAYS)
        && data::t("clan_supply", "STOCK_CAP").as_i64() == Some(STOCK_CAP)
}

// py: clan_supply.py:27 split
/// Largest-remainder split of an integer by weights; the parts add up to `total`.
pub fn split(total: i64, weights: &[i64]) -> Vec<i64> {
    let total = total.max(0);
    let sum: i64 = weights.iter().sum();
    let s = if sum as f64 == 0.0 { 1.0 } else { sum as f64 };
    let raw: Vec<f64> = weights.iter().map(|&w| (total * w) as f64 / s).collect();
    let mut out: Vec<i64> = raw.iter().map(|&x| x as i64).collect();
    let rest = total - out.iter().sum::<i64>();
    let mut order: Vec<(f64, usize)> = (0..raw.len()).map(|i| (raw[i] - out[i] as f64, i)).collect();
    order.sort_by(|a, b| (-a.0).partial_cmp(&(-b.0)).unwrap_or(std::cmp::Ordering::Equal).then(a.1.cmp(&b.1)));
    for (_f, i) in order.into_iter().take(rest.max(0) as usize) {
        out[i] += 1;
    }
    out
}

// py: clan_supply.py:38 need_split
/// {food, water, med} of the day's need (sums to `need`); n = clan size (medicine is at least 1 from 3 members up).
pub fn need_split(need: i64, n: Option<i64>) -> [i64; 3] {
    let need = need.max(0);
    let nn = n.unwrap_or(need);
    let floor_m = if nn >= 3 && need >= 3 { 1 } else { 0 };
    let nm = floor_m.max(py::round0(need as f64 * 0.2));
    let nw = py::round0(need as f64 * 0.3);
    let nf = (need - nm - nw).max(0);
    [nf, nw, nm]
}

// py: clan_supply.py:48 use_of
/// What the people use up of ammo / materials in a day: [ammo, mat].
pub fn use_of(n: i64) -> [i64; 2] {
    let n = n.max(0);
    [(n + 2) / 3, (n + 3) / 4]
}

// py: clan_supply.py:54 intake
/// The goods the match brought home per category [food, water, med, ammo, mat] (see the module doc of the Python).
pub fn intake(supplies: i64, parts: i64, counts: &Counts, left: bool) -> [i64; 5] {
    if left {
        return [0; 5];
    }
    let g = |k: i64| counts.get(&k).copied().unwrap_or(0).max(0);
    let (kills, execs, revives, hs, marked, downed) = (g(1), g(2), g(3), g(4), g(5), g(21));
    let s = split(supplies, &[50 + 2 * kills + 3 * execs, 30 + 2 * marked, 20 + 8 * revives + 2 * downed]);
    [s[0], s[1], s[2], kills + 2 * hs, parts.max(0) / 100 + execs]
}

/// `dict` of the five categories in CATS order.
pub fn cats_value(a: &[i64; 5]) -> Value {
    let mut m = Map::new();
    for (i, k) in CATS.iter().enumerate() {
        m.insert((*k).to_string(), Value::from(a[i]));
    }
    Value::Object(m)
}

/// `intake()` as the dict Python returns.
pub fn intake_value(supplies: i64, parts: i64, counts: &Counts, left: bool) -> Value {
    cats_value(&intake(supplies, parts, counts, left))
}

// py: clan_supply.py:68 book
/// One day. `stock` = {cat: n} on hand before (anything that is not an object counts as empty); returns (new stock, record).
pub fn book(stock: Option<&Value>, supplies: i64, need: i64, n_before: i64, parts: i64, counts: &Counts, left: bool, forgive: bool) -> (Value, Value) {
    let mut st = [0i64; 5];
    if let Some(o) = stock.and_then(|v| v.as_object()) {
        for (i, k) in CATS.iter().enumerate() {
            st[i] = o.get(*k).map(int_of).unwrap_or(0).max(0);
        }
    }
    let got = intake(supplies, parts, counts, left);
    let ns = need_split(need, Some(n_before));
    let us = use_of(n_before);
    let nd: [i64; 5] = [ns[0], ns[1], ns[2], us[0], us[1]];
    // trade among the survival goods: the over-supplied ones cover the short ones
    let mut short = [0i64; 5];
    let mut spare = [0i64; 3];
    for k in 0..3 {
        short[k] = (nd[k] - got[k]).max(0);
        spare[k] = (got[k] - nd[k]).max(0);
    }
    let mut pool: i64 = spare.iter().sum();
    let mut ord: Vec<usize> = (0..3).collect();
    ord.sort_by(|&a, &b| (-short[a]).cmp(&(-short[b])).then(SURVIVAL[a].cmp(SURVIVAL[b])));
    for &k in &ord {
        let take = short[k].min(pool);
        short[k] -= take;
        pool -= take;
    }
    // whatever spare is left after covering the short ones is the leftover of the day
    let mut left_over = [0i64; 3];
    let mut to_take: i64 = spare.iter().sum::<i64>() - pool;
    let mut ord2: Vec<usize> = (0..3).collect();
    ord2.sort_by(|&a, &b| (-spare[a]).cmp(&(-spare[b])).then(SURVIVAL[a].cmp(SURVIVAL[b])));
    for &k in &ord2 {
        let lend = spare[k].min(to_take);
        to_take -= lend;
        left_over[k] = spare[k] - lend;
    }
    if forgive {
        short = [0; 5];
    }
    let mut gross = short;
    let mut drawn = [0i64; 5];
    let mut net = [0i64; 5];
    let mut new = st;
    for k in 0..3 {
        let d = new[k].min(short[k]);
        drawn[k] = d;
        net[k] = short[k] - d;
        new[k] -= d;
        new[k] = (BANK_DAYS * nd[k].max(1)).min(new[k] + left_over[k]);
    }
    for k in 3..5 {
        let have = new[k] + got[k];
        if forgive {
            // a forgiven day uses only what the match brought, never the stock
            short[k] = 0;
            gross[k] = 0;
            drawn[k] = 0;
            net[k] = 0;
            new[k] = STOCK_CAP.min(new[k] + (got[k] - nd[k]).max(0));
            continue;
        }
        short[k] = (nd[k] - have).max(0);
        gross[k] = short[k];
        drawn[k] = new[k].min((nd[k] - got[k]).max(0));
        net[k] = short[k];
        new[k] = STOCK_CAP.min((have - nd[k]).max(0));
    }
    let mut days = [0i64; 5];
    for k in 0..5 {
        days[k] = if nd[k] > 0 { new[k].div_euclid(nd[k]) } else { 0 };
    }
    let mut cause = "";
    let mut best: Option<(i64, bool, &str)> = None;
    for k in 0..3 {
        if gross[k] > 0 {
            let key = (gross[k], SURVIVAL[k] == "med", SURVIVAL[k]);
            if best.is_none() || key > best.unwrap_or(key) {
                best = Some(key);
            }
        }
    }
    if let Some((_, _, name)) = best {
        cause = name;
    }
    let mut rec = Map::new();
    rec.insert("got".into(), cats_value(&got));
    rec.insert("need".into(), cats_value(&nd));
    rec.insert("short".into(), cats_value(&gross));
    rec.insert("drawn".into(), cats_value(&drawn));
    rec.insert("net".into(), cats_value(&net));
    rec.insert("stock".into(), cats_value(&new));
    rec.insert("days".into(), cats_value(&days));
    rec.insert("cause".into(), Value::String(cause.to_string()));
    (cats_value(&new), Value::Object(rec))
}
