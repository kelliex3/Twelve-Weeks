//! The clan engine's data tables, generated from the deployed Python modules by gen/export_clan.py (data/clan_tables.json).
//! Never edit the JSON by hand: re-run the generator on the deployed .py files.
//!
//! Encoding (see the generator): plain JSON for str/int/float/bool/null/list (Python lists AND tuples);
//! `{"$d": [[k, v], ...]}` dict with non-str keys; `{"$s": [...]}` set; `{"$re": pat, "f": flags}` regex;
//! `{"$b": hex}` bytes; `{"$fn": "mod.qualname"}` / `{"$mod": name}` a function / module inside a table (ported by hand).
//! File paths that Python built from `__file__` start with "@HERE@" (the host dir).
use serde_json::Value;
use std::sync::LazyLock;

static RAW: &str = include_str!("../../data/clan_tables.json");

static DATA: LazyLock<Value> = LazyLock::new(|| serde_json::from_str(RAW).unwrap_or(Value::Null));

static NULL: Value = Value::Null;

/// `<module>.<NAME>` as exported (Null when absent).
pub fn t(module: &str, name: &str) -> &'static Value {
    DATA.get(module).and_then(|m| m.get(name)).unwrap_or(&NULL)
}

/// The data loaded (call at start: a broken table aborts the start instead of the first clan day).
pub fn ok() -> bool {
    DATA.is_object()
}

/// Items of a `{"$d": [[k, v], ...]}` dict (or a plain object as (String key, v) pairs) in Python insertion order.
pub fn dict_items(v: &'static Value) -> Vec<(&'static Value, &'static Value)> {
    if let Some(a) = v.get("$d").and_then(|x| x.as_array()) {
        return a.iter().filter_map(|kv| Some((kv.get(0)?, kv.get(1)?))).collect();
    }
    Vec::new()
}

/// `d[key]` for a `$d` dict with int keys.
pub fn dict_get_int(v: &'static Value, key: i64) -> Option<&'static Value> {
    dict_items(v).into_iter().find(|(k, _)| k.as_i64() == Some(key)).map(|(_, x)| x)
}

/// Members of a `{"$s": [...]}` set.
pub fn set_items(v: &'static Value) -> &'static [Value] {
    v.get("$s").and_then(|x| x.as_array()).map(|a| a.as_slice()).unwrap_or(&[])
}

/// `s in SET` for a set of strings.
pub fn set_has_str(v: &'static Value, s: &str) -> bool {
    set_items(v).iter().any(|x| x.as_str() == Some(s))
}

/// The pattern and flags of an exported regex.
pub fn regex_src(v: &'static Value) -> Option<(&'static str, i64)> {
    Some((v.get("$re")?.as_str()?, v.get("f").and_then(|x| x.as_i64()).unwrap_or(0)))
}

/// Strings of a list (non-strings skipped).
pub fn strs(v: &'static Value) -> Vec<&'static str> {
    v.as_array().map(|a| a.iter().filter_map(|x| x.as_str()).collect()).unwrap_or_default()
}

/// "@HERE@/x" -> "<host dir>/x" (the working directory of the daemon = Python's module dir).
pub fn here(p: &str) -> String {
    let dir = std::env::current_dir().map(|d| d.to_string_lossy().into_owned()).unwrap_or_default();
    p.replace("@HERE@", &dir)
}
