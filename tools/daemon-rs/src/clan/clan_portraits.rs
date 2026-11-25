//! py: clan_portraits.py - which head a clan member gets: role- and age-aware choice from the head pool (clan_portrait_pool.POOL).
//! Pure data + hash, no I/O besides the switch. A pick is stable for a name (same name -> same head).
use crate::clan::data;
use crate::py;
use serde_json::Value;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

// py: clan_portrait_pool.py POOL rows: (id, look, faction, sex, age, tags, rig, tier)
struct Row {
    id: i64,
    look: String,
    fac: String,
    sex: String,
    tags: HashSet<String>,
}

static POOL: LazyLock<Vec<Row>> = LazyLock::new(|| {
    data::t("clan_portraits", "POOL")
        .as_array()
        .map(|a| {
            a.iter()
                .filter_map(|r| {
                    let r = r.as_array()?;
                    Some(Row {
                        id: r.first()?.as_i64()?,
                        look: r.get(1)?.as_str()?.to_string(),
                        fac: r.get(2)?.as_str()?.to_string(),
                        sex: r.get(3)?.as_str()?.to_string(),
                        tags: r.get(5)?.as_str()?.split('+').map(|s| s.to_string()).collect(),
                    })
                })
                .collect()
        })
        .unwrap_or_default()
});

static RENDERED: LazyLock<HashSet<i64>> = LazyLock::new(|| data::set_items(data::t("clan_portraits", "RENDERED")).iter().filter_map(|v| v.as_i64()).collect());

static EXPR: LazyLock<HashMap<i64, Vec<String>>> = LazyLock::new(|| {
    data::dict_items(data::t("clan_portraits", "EXPR"))
        .into_iter()
        .filter_map(|(k, v)| Some((k.as_i64()?, v.as_array()?.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect())))
        .collect()
});

type Pref = Vec<(String, i64)>;

fn pref_of(v: &Value) -> Pref {
    v.as_array().map(|a| a.iter().filter_map(|p| Some((p.get(0)?.as_str()?.to_string(), p.get(1)?.as_i64()?))).collect()).unwrap_or_default()
}

static PREF: LazyLock<HashMap<String, Pref>> = LazyLock::new(|| {
    data::t("clan_portraits", "PREF").as_object().map(|o| o.iter().map(|(k, v)| (k.clone(), pref_of(v))).collect()).unwrap_or_default()
});
static DEFAULT_PREF: LazyLock<Pref> = LazyLock::new(|| pref_of(data::t("clan_portraits", "_DEFAULT")));
static ELDER_OK: LazyLock<Vec<String>> = LazyLock::new(|| data::strs(data::t("clan_portraits", "_ELDER_OK")).into_iter().map(|s| s.to_string()).collect());

// py: clan_portraits.py:41 enabled
pub fn enabled() -> bool {
    let v = match std::env::var("T2R_PORTRAIT_POOL") {
        Ok(v) => v,
        Err(_) => match std::fs::read(data::here("@HERE@/clan_portrait_pool.txt")) {
            Ok(b) => py::strip(&String::from_utf8_lossy(&b)).to_string(),
            Err(_) => "1".to_string(),
        },
    };
    !matches!(py::strip(&v), "0" | "off" | "no")
}

// py: clan_portraits.py:50 _h
fn h(parts: &[&str]) -> u32 {
    t2r_clanstory::crc::crc32(parts.join("|").as_bytes())
}

// py: clan_portraits.py:54 _cands
/// [(row, weight)] heads of the role among the factions `sides`, own faction weighted 3x.
fn cands(role: &str, sex: &str, fk: &str, sides: &[&str], sera_civ: bool) -> Vec<(&'static Row, i64)> {
    let mut out: Vec<(&'static Row, i64)> = Vec::new();
    let pref = PREF.get(role).unwrap_or(&DEFAULT_PREF);
    for row in POOL.iter() {
        if row.sex != sex || !sides.contains(&row.fac.as_str()) {
            continue;
        }
        let mut tg = row.tags.clone();
        if sera_civ && row.fac == "S" && tg.contains("soldier") {
            tg.insert("civilian".into());
        }
        let mut w = 0;
        for (tag, wt) in pref {
            if tg.contains(tag) {
                w = w.max(*wt);
            }
        }
        if w == 0 {
            continue;
        }
        if !ELDER_OK.iter().any(|r| r == role) && tg.contains("elder") {
            continue;
        }
        if role != "kid" && (tg.contains("kid") || tg.contains("teen")) {
            continue;
        }
        if role == "kid" && !tg.contains("kid") && !tg.contains("teen") {
            continue;
        }
        out.push((row, w * if row.fac == fk { 3 } else { 1 }));
    }
    let shown: Vec<(&'static Row, i64)> = out.iter().filter(|x| RENDERED.contains(&x.0.id)).cloned().collect();
    if shown.is_empty() {
        out
    } else {
        shown
    }
}

// py: clan_portraits.py:80 candidates
/// [(model id, weight)] heads for the role, the clan's own faction only.
pub fn candidates(role: &str, sex: &str, fk: &str) -> Vec<(i64, i64)> {
    cands(role, sex, fk, &[fk], true).into_iter().map(|(r, w)| (r.id, w)).collect()
}

// py: clan_portraits.py:85 _legacy
/// The pre-11-17 candidate list (own faction + the neutral rows).
fn legacy(role: &str, sex: &str, fk: &str) -> Vec<(&'static Row, i64)> {
    cands(role, sex, fk, &[fk, "N"], false)
}

fn new_cands(role: &str, sex: &str, fk: &str) -> Vec<(&'static Row, i64)> {
    cands(role, sex, fk, &[fk], true)
}

// py: clan_portraits.py:90 _draw
fn draw(c: &[(&'static Row, i64)], name: &str, salt: &str) -> i64 {
    let tot: i64 = c.iter().map(|x| x.1).sum();
    let mut x = (h(&[salt, name]) as i64) % tot.max(1);
    for (row, w) in c {
        if x < *w {
            return row.id;
        }
        x -= w;
    }
    c.last().map(|x| x.0.id).unwrap_or(0)
}

// py: clan_portraits.py:98 _pick
fn pick_with(name: &str, role: &str, sex: &str, fk: &str, used: &[i64], cand: fn(&str, &str, &str) -> Vec<(&'static Row, i64)>) -> Option<i64> {
    let mut cs = cand(role, sex, fk);
    if cs.is_empty() {
        cs = cand(if role != "kid" { "scavenger" } else { "kid" }, sex, fk);
    }
    if cs.is_empty() {
        return None;
    }
    let first = draw(&cs, name, "head");
    if !used.contains(&first) {
        return Some(first);
    }
    let free: Vec<_> = cs.iter().filter(|x| !used.contains(&x.0.id)).cloned().collect();
    if !free.is_empty() {
        return Some(draw(&free, name, "head2"));
    }
    let wide = cand(if role != "kid" { "scavenger" } else { "kid" }, sex, fk);
    let free: Vec<_> = wide.iter().filter(|x| !used.contains(&x.0.id)).cloned().collect();
    if !free.is_empty() {
        return Some(draw(&free, name, "head3"));
    }
    Some(first)
}

// py: clan_portraits.py:114 faction_of
pub fn faction_of(model: i64) -> Option<String> {
    POOL.iter().find(|r| r.id == model).map(|r| r.fac.clone())
}

// py: clan_portraits.py:121 pick
/// The model id (pool row id) for a member, or None when the pool has no head for them. `used` = heads the earlier members already wear.
pub fn pick(name: &str, role: &str, sex: &str, fk: &str, used: &[i64]) -> Option<i64> {
    let old = pick_with(name, role, sex, fk, used, legacy);
    if let Some(o) = old {
        if faction_of(o).as_deref() == Some(fk) {
            return Some(o);
        }
    }
    let new = pick_with(name, role, sex, fk, used, new_cands);
    if new.is_some() {
        new
    } else if fk != "W" && fk != "S" {
        old
    } else {
        None
    }
}

// py: clan_portraits.py:134 look_of
pub fn look_of(model: i64) -> Option<String> {
    POOL.iter().find(|r| r.id == model).map(|r| r.look.clone())
}

// py: clan_portraits.py:141 expr_for
/// An expression a head does not have -> its stand-in, then neutral.
pub fn expr_for(model: i64, want: &str) -> String {
    let have: &[String] = EXPR.get(&model).map(|v| v.as_slice()).unwrap_or(&[]);
    if have.iter().any(|x| x == want) {
        return want.to_string();
    }
    let alt = data::t("clan_portraits", "STAND_IN").get(want).and_then(|v| v.as_str()).unwrap_or("neutral");
    if have.iter().any(|x| x == alt) {
        alt.to_string()
    } else {
        "neutral".to_string()
    }
}
