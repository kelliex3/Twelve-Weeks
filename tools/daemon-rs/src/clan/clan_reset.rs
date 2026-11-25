//! py: clan_reset.py - admin clan reset (wipe one player's clan to a fresh start with an automatic backup) and undo it again.
//! Pure logic on an open sqlite connection; the caller commits. Errors are `Err(message)` (Python raises).
use crate::clan::clan::{int_of, to_sql, vref_to_json};
use crate::clan::data;
use crate::py;
use rusqlite::Connection;
use serde_json::{json, Map, Value};
use std::collections::HashSet;
use std::path::Path;

fn tables() -> Vec<(String, String)> {
    data::t("clan_reset", "TABLES")
        .as_array()
        .map(|a| a.iter().filter_map(|p| Some((p.get(0)?.as_str()?.to_string(), p.get(1)?.as_str()?.to_string()))).collect())
        .unwrap_or_default()
}

fn names_table() -> (String, String) {
    let p = data::t("clan_reset", "NAMES_TABLE");
    (p.get(0).and_then(|x| x.as_str()).unwrap_or("names").to_string(), p.get(1).and_then(|x| x.as_str()).unwrap_or("token_id").to_string())
}

fn err<E: std::fmt::Display>(e: E) -> String {
    e.to_string()
}

/// `HEX24.match(t)`: `^[0-9a-f]{24}$` (Python's `$` also accepts one trailing newline).
fn hex24(t: &str) -> bool {
    let t = t.strip_suffix('\n').unwrap_or(t);
    t.len() == 24 && t.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}

// py: clan_reset.py:28 keys_for
/// Every clan key of one person: 's:<steamid>' plus the (24 hex) install ids. Anything malformed is ignored.
pub fn keys_for(steamid: i64, token_ids: &[String]) -> Vec<String> {
    let mut ks: Vec<String> = Vec::new();
    if steamid > 0 {
        ks.push(format!("s:{}", steamid));
    }
    for t in token_ids {
        let t = t.to_lowercase();
        if hex24(&t) && !ks.contains(&t) {
            ks.push(t);
        }
    }
    ks
}

fn table_names(c: &Connection) -> Result<HashSet<String>, String> {
    let mut st = c.prepare("SELECT name FROM sqlite_master WHERE type='table'").map_err(err)?;
    let rows = st.query_map([], |r| r.get::<_, String>(0)).map_err(err)?;
    rows.collect::<Result<HashSet<String>, _>>().map_err(err)
}

fn all_tables(names: bool) -> Vec<(String, String)> {
    let mut t = tables();
    if names {
        t.push(names_table());
    }
    t
}

// py: clan_reset.py:47 snapshot
/// {table: [row dicts]} for the keys (only tables that exist and have rows).
pub fn snapshot(c: &Connection, keys: &[String], names: bool) -> Result<Value, String> {
    let have = table_names(c)?;
    let mut out = Map::new();
    for (tab, col) in all_tables(names) {
        if !have.contains(&tab) || keys.is_empty() {
            continue;
        }
        let sql = format!("SELECT * FROM {} WHERE {} IN ({})", tab, col, vec!["?"; keys.len()].join(","));
        let mut st = c.prepare(&sql).map_err(err)?;
        let cols: Vec<String> = st.column_names().iter().map(|s| (*s).to_string()).collect();
        let rows: Vec<Value> = st
            .query_map(rusqlite::params_from_iter(keys.iter()), |r| {
                let mut m = Map::new();
                for (i, k) in cols.iter().enumerate() {
                    m.insert(k.clone(), vref_to_json(r.get_ref(i)?));
                }
                Ok(Value::Object(m))
            })
            .map_err(err)?
            .collect::<Result<_, _>>()
            .map_err(err)?;
        if !rows.is_empty() {
            out.insert(tab, Value::Array(rows));
        }
    }
    Ok(Value::Object(out))
}

/// `re.sub(r"[^A-Za-z0-9._-]+", "_", name or "player")[:40] or "player"`.
fn safe(name: &str) -> String {
    let name = if name.is_empty() { "player" } else { name };
    let mut s = String::new();
    let mut in_bad = false;
    for ch in name.chars() {
        if ch.is_ascii_alphanumeric() || ch == '.' || ch == '_' || ch == '-' {
            s.push(ch);
            in_bad = false;
        } else if !in_bad {
            s.push('_');
            in_bad = true;
        }
    }
    let s = py::cut(&s, 40).to_string();
    if s.is_empty() {
        "player".into()
    } else {
        s
    }
}

/// `time.strftime(fmt, time.localtime(now))` for the two formats used here: (Y, m, d, H, M, S).
#[allow(deprecated)]
fn local_parts(now: f64) -> (i32, i32, i32, i32, i32, i32) {
    let tt = now.floor() as libc::time_t;
    let mut tm: libc::tm = unsafe { std::mem::zeroed() };
    let ok = unsafe { !libc::localtime_r(&tt, &mut tm).is_null() };
    if !ok {
        return (1970, 1, 1, 0, 0, 0);
    }
    (tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
}

fn stamp_file(now: f64) -> String {
    let (y, mo, d, h, mi, s) = local_parts(now);
    format!("{:04}{:02}{:02}-{:02}{:02}{:02}", y, mo, d, h, mi, s)
}

fn stamp_log(now: f64) -> String {
    let (y, mo, d, h, mi, s) = local_parts(now);
    format!("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", y, mo, d, h, mi, s)
}

// py: clan_reset.py:70 write_backup
/// Writes `<dir>/<stamp>-<name>-<steamid>[-n].json` and returns its path.
pub fn write_backup(backup_dir: &str, name: Option<&str>, steamid: i64, keys: &[String], snap: &Value, reason: &str, now: Option<f64>) -> Result<String, String> {
    let now = now.unwrap_or_else(crate::clock::now);
    std::fs::create_dir_all(backup_dir).map_err(err)?;
    let sid = if steamid != 0 { steamid.to_string() } else { "nosteam".to_string() };
    let base = format!("{}-{}-{}", stamp_file(now), safe(name.unwrap_or("")), safe(&sid));
    let mut path = Path::new(backup_dir).join(format!("{}.json", base));
    let mut n = 1;
    while path.exists() {
        n += 1;
        path = Path::new(backup_dir).join(format!("{}-{}.json", base, n));
    }
    let doc = json!({"v": 1, "ts": now, "reason": reason, "name": name, "steamid": steamid, "keys": keys, "tables": snap});
    let tmp = format!("{}.tmp", path.to_string_lossy());
    std::fs::write(&tmp, py::dumps_compact_utf8(&doc).as_bytes()).map_err(err)?;
    std::fs::rename(&tmp, &path).map_err(err)?;
    Ok(path.to_string_lossy().into_owned())
}

// py: clan_reset.py:85 audit
pub fn audit(backup_dir: &str, line: &str, now: Option<f64>) -> Result<(), String> {
    use std::io::Write;
    let now = now.unwrap_or_else(crate::clock::now);
    std::fs::create_dir_all(backup_dir).map_err(err)?;
    let mut f = std::fs::OpenOptions::new().append(true).create(true).open(Path::new(backup_dir).join("audit.log")).map_err(err)?;
    f.write_all(format!("{} {}\n", stamp_log(now), line.replace('\n', " ")).as_bytes()).map_err(err)
}

fn basename(p: &str) -> String {
    Path::new(p).file_name().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default()
}

fn count_rows(snap: &Value) -> Value {
    let mut m = Map::new();
    if let Some(o) = snap.as_object() {
        for (t, r) in o {
            m.insert(t.clone(), Value::from(r.as_array().map(|a| a.len()).unwrap_or(0) as i64));
        }
    }
    Value::Object(m)
}

// py: clan_reset.py:93 reset
/// Back up and wipe. Returns {"ok", "backup", "keys", "rows": {table: n}, "had_clan"}; the caller commits.
pub fn reset(c: &Connection, backup_dir: &str, name: &str, steamid: i64, token_ids: &[String], names: bool, now: Option<f64>) -> Result<Value, String> {
    let keys = keys_for(steamid, token_ids);
    if keys.is_empty() {
        return Ok(json!({"ok": false, "error": "no clan key for this player"}));
    }
    let snap = snapshot(c, &keys, names)?;
    let had = snap.get("clans").map(crate::clan::clan::truthy).unwrap_or(false);
    let path = write_backup(backup_dir, Some(name), steamid, &keys, &snap, "admin reset", now)?;
    let have = table_names(c)?;
    for (tab, col) in all_tables(names) {
        if have.contains(&tab) {
            let sql = format!("DELETE FROM {} WHERE {} IN ({})", tab, col, vec!["?"; keys.len()].join(","));
            c.execute(&sql, rusqlite::params_from_iter(keys.iter())).map_err(err)?;
        }
    }
    let rows = count_rows(&snap);
    audit(
        backup_dir,
        &format!(
            "RESET {} steam={} keys={} rows={} names={} backup={}",
            name,
            steamid,
            keys.len(),
            py::pyrepr(&rows),
            py::b(names),
            basename(&path)
        ),
        now,
    )?;
    Ok(json!({"ok": true, "backup": basename(&path), "keys": keys, "rows": rows, "had_clan": had}))
}

fn read_json(path: &Path) -> Option<Value> {
    let b = std::fs::read(path).ok()?;
    serde_json::from_str::<Value>(std::str::from_utf8(&b).ok()?).ok()
}

// py: clan_reset.py:113 list_backups
pub fn list_backups(backup_dir: &str, steamid: Option<i64>) -> Value {
    let mut out: Vec<Value> = Vec::new();
    let Ok(rd) = std::fs::read_dir(backup_dir) else { return Value::Array(out) };
    let mut fs: Vec<String> = rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).filter(|f| f.ends_with(".json")).collect();
    fs.sort();
    for fname in fs {
        let Some(d) = read_json(&Path::new(backup_dir).join(&fname)) else { continue };
        if !d.is_object() {
            continue;
        }
        if let Some(sid) = steamid.filter(|s| *s != 0) {
            if d.get("steamid").map(int_of).unwrap_or(0) != sid {
                continue;
            }
        }
        let tabs = d.get("tables").filter(|v| crate::clan::clan::truthy(v)).cloned().unwrap_or_else(|| json!({}));
        out.push(json!({
            "file": fname,
            "ts": d.get("ts").cloned().unwrap_or(Value::Null),
            "name": d.get("name").cloned().unwrap_or(Value::Null),
            "steamid": d.get("steamid").cloned().unwrap_or(Value::Null),
            "reason": d.get("reason").cloned().unwrap_or(Value::Null),
            "rows": count_rows(&tabs)
        }));
    }
    Value::Array(out)
}

// py: clan_reset.py:130 restore
/// Put a backup back (what is there now is backed up first, so a restore is undoable too).
pub fn restore(c: &Connection, backup_dir: &str, fname: &str, now: Option<f64>) -> Result<Value, String> {
    let valid = {
        let f = fname.strip_suffix('\n').unwrap_or(fname);
        f.ends_with(".json") && f.len() > 5 && f[..f.len() - 5].bytes().all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
    };
    if !valid || fname.starts_with('.') {
        return Ok(json!({"ok": false, "error": "bad backup name"}));
    }
    let Some(d) = read_json(&Path::new(backup_dir).join(fname)) else {
        return Ok(json!({"ok": false, "error": "backup not found"}));
    };
    let keys: Vec<String> = d.get("keys").and_then(|v| v.as_array()).map(|a| a.iter().filter_map(|k| k.as_str().map(|s| s.to_string())).collect()).unwrap_or_default();
    let tabs = d.get("tables").filter(|v| crate::clan::clan::truthy(v)).cloned().unwrap_or_else(|| json!({}));
    let cur = snapshot(c, &keys, tabs.get("names").is_some())?;
    let name = d.get("name").and_then(|v| v.as_str());
    let steam = d.get("steamid").map(int_of).unwrap_or(0);
    let pre = write_backup(backup_dir, name, steam, &keys, &cur, &format!("before restore of {}", fname), now)?;
    let have = table_names(c)?;
    let mut n = 0i64;
    let allowed = all_tables(true);
    if let Some(o) = tabs.as_object() {
        for (tab, rows) in o {
            if !have.contains(tab) {
                continue;
            }
            let Some((_, col)) = allowed.iter().find(|(t, _)| t == tab) else { continue };
            let sql = format!("DELETE FROM {} WHERE {} IN ({})", tab, col, vec!["?"; keys.len()].join(","));
            c.execute(&sql, rusqlite::params_from_iter(keys.iter())).map_err(err)?;
            for r in rows.as_array().map(|a| a.as_slice()).unwrap_or(&[]) {
                let ro = r.as_object().cloned().unwrap_or_default();
                let cols: Vec<&String> = ro.keys().filter(|k| !k.is_empty() && k.bytes().all(|b| b.is_ascii_lowercase() || b == b'_')).collect();
                let vals: Vec<rusqlite::types::Value> = cols.iter().map(|k| to_sql(&ro[k.as_str()])).collect();
                let sql = format!(
                    "INSERT OR REPLACE INTO {}({}) VALUES({})",
                    tab,
                    cols.iter().map(|s| s.as_str()).collect::<Vec<_>>().join(","),
                    vec!["?"; cols.len()].join(",")
                );
                c.execute(&sql, rusqlite::params_from_iter(vals)).map_err(err)?;
                n += 1;
            }
        }
    }
    audit(
        backup_dir,
        &format!(
            "RESTORE {} steam={} from={} rows={} (previous state saved as {})",
            d.get("name").map(crate::clan::clan::pystr).unwrap_or_else(|| "None".into()),
            d.get("steamid").map(crate::clan::clan::pystr).unwrap_or_else(|| "None".into()),
            fname,
            n,
            basename(&pre)
        ),
        now,
    )?;
    Ok(json!({"ok": true, "restored": n, "name": d.get("name").cloned().unwrap_or(Value::Null), "before": basename(&pre)}))
}
