use crate::rules::{self, ALIVE, EVENT_LEN, EV_ATTACK, WEEKS, WIN_DAY};
use crate::story::{menu_lines, norm_lang, slot_of};
use crate::text::{py_int, truthy};
use rusqlite::types::Value as Sql;
use rusqlite::{params, Connection, OptionalExtension};
use serde_json::{json, Map, Value};

pub const FEED_MAX: i64 = 100;

fn is_hex(s: &str) -> bool {
    s.bytes().all(|c| c.is_ascii_hexdigit())
}

pub fn valid_player(player: &str) -> bool {
    let p = player.strip_suffix('\n').unwrap_or(player);
    (p.len() == 24 || p.len() == 64) && is_hex(p)
}

pub fn token_id(token: &str) -> String {
    rules::token_id(token)
}

pub fn resolve_key(c: &Connection, tid: &str) -> String {
    let sids: Option<Vec<Sql>> = (|| {
        let mut st = c.prepare("SELECT DISTINCT steamid FROM players WHERE substr(token,41,24)=? AND steamid>0").ok()?;
        let rows = st.query_map(params![tid], |r| r.get::<_, Sql>(0)).ok()?;
        rows.collect::<Result<Vec<_>, _>>().ok()
    })();
    for sid in sids.unwrap_or_default() {
        let n = match sql_json(sid).and_then(|v| py_int(&v)) {
            Some(n) => n,
            None => continue,
        };
        let key = format!("s:{}", n);
        let hit: Option<i64> = c.query_row("SELECT 1 FROM clans WHERE token_id=?", params![key], |r| r.get(0)).optional().ok().flatten();
        if hit.is_some() {
            return key;
        }
    }
    tid.to_string()
}

fn sql_json(v: Sql) -> Option<Value> {
    Some(match v {
        Sql::Null => Value::Null,
        Sql::Integer(i) => Value::from(i),
        Sql::Real(f) => serde_json::Number::from_f64(f).map(Value::Number).unwrap_or(Value::Null),
        Sql::Text(s) => Value::String(s),
        Sql::Blob(_) => return None,
    })
}

fn json_obj(v: &Value) -> Map<String, Value> {
    match v {
        Value::String(s) if !s.is_empty() => match serde_json::from_str::<Value>(s) {
            Ok(Value::Object(m)) => m,
            _ => Map::new(),
        },
        _ => Map::new(),
    }
}

fn key_of(player: &str, key: Option<&str>) -> Result<String, Value> {
    match key {
        Some(k) => Ok(k.to_string()),
        None if valid_player(player) => Ok(token_id(player)),
        None => Err(json!({"ok": false, "error": "bad player id"})),
    }
}

pub fn clan_view(c: &Connection, player: &str, key: Option<&str>) -> Value {
    let mut tid = match key_of(player, key) {
        Ok(t) => t,
        Err(e) => return e,
    };
    if key.is_none() {
        tid = resolve_key(c, &tid);
    }
    let row: Option<Map<String, Value>> = (|| {
        let mut st = c.prepare("SELECT * FROM clans WHERE token_id=?").ok()?;
        let names: Vec<String> = st.column_names().iter().map(|s| s.to_string()).collect();
        st.query_row(params![tid], |r| {
            let mut m = Map::new();
            for (i, n) in names.iter().enumerate() {
                m.insert(n.clone(), sql_json(r.get::<_, Sql>(i)?).unwrap_or(Value::Null));
            }
            Ok(m)
        })
        .optional()
        .ok()?
    })();
    let mut boosters: Vec<(i64, i64)> = Vec::new();
    if let Ok(mut st) = c.prepare("SELECT bid, n FROM boosters WHERE token_id=?") {
        if let Ok(rows) = st.query_map(params![tid], |r| Ok((r.get::<_, Sql>(0)?, r.get::<_, Sql>(1)?))) {
            for (b, n) in rows.flatten() {
                if let (Some(b), Some(n)) = (sql_json(b).and_then(|v| py_int(&v)), sql_json(n).and_then(|v| py_int(&v))) {
                    match boosters.iter_mut().find(|x| x.0 == b) {
                        Some(x) => x.1 = n,
                        None => boosters.push((b, n)),
                    }
                }
            }
        }
    }
    let k = match row {
        Some(k) => k,
        None => return json!({"ok": true, "hasClan": false}),
    };
    let zero = Value::from(0);
    let g = |name: &str| -> Value { k.get(name).filter(|v| !v.is_null()).cloned().unwrap_or_else(|| zero.clone()) };
    let gi = |name: &str| -> i64 { py_int(&g(name)).unwrap_or(0) };
    let (healthy, hungry, sick) = (gi("healthy"), gi("hungry"), gi("sick"));
    let size = healthy + hungry + sick;
    let (day, status, faction) = (gi("day"), gi("status"), gi("faction"));
    let meta = json_obj(&k.get("meta").filter(|v| !v.is_null()).cloned().unwrap_or_else(|| Value::from("")));
    let last = json_obj(&k.get("last_result").filter(|v| !v.is_null()).cloned().unwrap_or_else(|| Value::from("")));
    let max_seen = meta.get("max").and_then(py_int).unwrap_or(0);
    let mut out = Map::new();
    out.insert("ok".into(), json!(true));
    out.insert("hasClan".into(), json!(status != 0));
    out.insert("faction".into(), json!(faction));
    out.insert("factionName".into(), json!(rules::faction_name(faction)));
    out.insert("status".into(), json!(rules::status_name(status)));
    out.insert("daysDone".into(), json!(day));
    out.insert("daysTotal".into(), json!(WIN_DAY));
    out.insert("weeksTotal".into(), json!(gi("weeks_total")));
    out.insert("size".into(), json!(size));
    out.insert("healthy".into(), json!(healthy));
    out.insert("hungry".into(), json!(hungry));
    out.insert("sick".into(), json!(sick));
    out.insert("needed".into(), json!(rules::needed(size)));
    out.insert("maxSize".into(), json!(max_seen.max(size)));
    out.insert("runsWon".into(), json!(gi("runs_won")));
    out.insert("runsLost".into(), json!(gi("runs_lost")));
    if status == ALIVE {
        let (wk, dw) = rules::week_day(day);
        out.insert("week".into(), json!(wk));
        out.insert("dayOfWeek".into(), json!(dw));
        out.insert("weeks".into(), json!(WEEKS));
        let s = rules::event_window(day);
        let ev = if s >= 0 {
            let (kind, name) = rules::event_today(s);
            json!({"kind": if kind == EV_ATTACK { "attack" } else { "growth" }, "name": rules::event_name(kind, name), "startDay": s, "dayOfEvent": day - s + 1, "length": EVENT_LEN})
        } else {
            Value::Null
        };
        out.insert("event".into(), ev);
        let (ek, en) = rules::event_today(day);
        out.insert(
            "eventToday".into(),
            if ek != 0 { json!({"kind": if ek == EV_ATTACK { "attack" } else { "growth" }, "name": rules::event_name(ek, en)}) } else { Value::Null },
        );
        let mission = meta.get("m").filter(|m| truthy(Some(m))).and_then(|m| {
            let mid = m.get("id").and_then(py_int).unwrap_or(0);
            let (_, name, _) = rules::mission(mid)?;
            let rep = meta.get("rep").and_then(|r| r.get(mid.to_string())).and_then(py_int).unwrap_or(0);
            Some(json!({"id": mid, "name": name, "progress": m.get("prog").and_then(py_int).unwrap_or(0), "daysLeft": m.get("left").and_then(py_int).unwrap_or(0),
                        "goals": rules::mission_goals(mid, rep)?, "inEvent": m.get("ev").and_then(py_int).unwrap_or(-1) >= 0}))
        });
        out.insert("mission".into(), mission.unwrap_or(Value::Null));
        out.insert("boosterNext".into(), json!(rules::booster_next(healthy, hungry, sick, gi("boost_hi"))));
    }
    let bl: Vec<Value> = rules::BOOSTERS.iter().map(|(b, n)| json!({"id": b, "name": n, "count": boosters.iter().find(|x| x.0 == *b).map_or(0, |x| x.1)})).collect();
    out.insert("boosters".into(), Value::Array(bl));
    if !last.is_empty() {
        let lv = |k: &str, d: Value| last.get(k).cloned().unwrap_or(d);
        out.insert(
            "lastMatch".into(),
            json!({"day": lv("day", json!(-1)), "parts": lv("parts", json!(0)), "supplies": lv("supplies", json!(0)), "needed": lv("needed", json!(0)),
                   "gained": lv("gained", json!(0)), "died": lv("died", json!(0)), "newHungry": lv("new_hungry", json!(0)), "newSick": lv("new_sick", json!(0)),
                   "healed": lv("healed", json!(0)), "leftEarly": truthy(last.get("left"))}),
        );
        if let Some(Value::Object(sup)) = last.get("sup") {
            if truthy(sup.get("need")) {
                let sv = |k: &str, d: Value| sup.get(k).cloned().unwrap_or(d);
                out.insert(
                    "supplies".into(),
                    json!({"cats": rules::SUPPLY_CATS, "got": sv("got", json!({})), "need": sv("need", json!({})), "short": sv("short", json!({})),
                           "drawn": sv("drawn", json!({})), "net": sv("net", json!({})), "stock": sv("stock", json!({})), "days": sv("days", json!({})),
                           "cause": sv("cause", json!(""))}),
                );
            }
        }
        if truthy(last.get("mi")) {
            if let Some(mi) = last.get("mi").and_then(py_int) {
                let name = rules::mission(mi).map_or("?", |m| m.1);
                let earned = last.get("boosters").and_then(Value::as_array).map_or(0, |a| a.len());
                out.insert(
                    "lastMission".into(),
                    json!({"id": mi, "name": name, "progress": lv("mp", json!(0)), "tier": lv("mt", json!(0)), "boostersEarned": earned}),
                );
            }
        }
    }
    Value::Object(out)
}

pub fn clanfeed(c: &Connection, player: &str, limit: &str, since: &str, lang: &str, now: f64, key: Option<&str>) -> Value {
    let mut tid = match key_of(player, key) {
        Ok(t) => t,
        Err(e) => return e,
    };
    let lim = py_int(&Value::from(limit));
    let snc = py_int(&Value::from(since));
    if lim.is_none() || snc.is_none() {
        return json!({"ok": false, "error": "bad limit / since"});
    }
    if key.is_none() {
        tid = resolve_key(c, &tid);
    }
    let row: Option<(Sql, Sql, Sql)> = c
        .query_row("SELECT faction, status, day FROM clans WHERE token_id=?", params![tid], |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?)))
        .optional()
        .ok()
        .flatten();
    let (f, s, d) = match row {
        Some(r) => r,
        None => return json!({"ok": true, "hasClan": false}),
    };
    let num = |v: Sql| sql_json(v).and_then(|x| py_int(&x));
    let status = num(s).unwrap_or(0);
    if status == 0 {
        return json!({"ok": true, "hasClan": false});
    }
    let faction = num(f).unwrap_or(0);
    let menu: Vec<Value> = menu_lines(c, &tid, 6, lang, now).into_iter().map(|(id, text)| json!({"id": id, "text": text})).collect();
    json!({"ok": true, "hasClan": true, "lang": norm_lang(lang), "faction": faction, "factionName": rules::faction_name(faction),
           "status": rules::status_name(status), "daysDone": num(d).unwrap_or(0), "menu": menu, "slot": slot_of(now)})
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ids() {
        assert!(valid_player(&"a".repeat(24)));
        assert!(valid_player(&"F".repeat(64)));
        assert!(!valid_player(&"g".repeat(24)));
        assert!(!valid_player(""));
        assert_eq!(token_id(&format!("{}{}", "0".repeat(40), "AB".repeat(12))), "ab".repeat(12));
    }
}
