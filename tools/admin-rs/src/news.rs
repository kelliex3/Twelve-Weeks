use std::collections::HashSet;

use serde_json::{Map, Value};

use crate::py;

fn sections(v: &Value, wher: &str) -> Result<Map<String, Value>, String> {
    let Value::Object(o) = v else {
        return Err(format!("{}: sections must be an object", wher));
    };
    let mut out = Map::new();
    for (k, lines) in o {
        let Some(list) = lines.as_array().filter(|a| a.iter().all(Value::is_string)) else {
            return Err(format!("{}: section {} must be a list of text lines", wher, py::repr_str(k)));
        };
        let kept: Vec<Value> = list.iter().filter_map(Value::as_str).map(str::trim).filter(|s| !s.is_empty()).map(|s| Value::String(s.to_string())).collect();
        if !kept.is_empty() {
            out.insert(k.clone(), Value::Array(kept));
        }
    }
    Ok(out)
}

fn date_ok(d: &str) -> bool {
    let d = d.strip_suffix('\n').unwrap_or(d);
    let b = d.as_bytes();
    b.len() == 10 && b[4] == b'-' && b[7] == b'-' && b.iter().enumerate().all(|(i, c)| i == 4 || i == 7 || c.is_ascii_digit())
}

pub fn validate(entries: Option<&Value>) -> Result<Vec<Value>, String> {
    let Some(list) = entries.and_then(Value::as_array).filter(|a| a.len() <= 500) else {
        return Err("entries must be a list (max 500)".into());
    };
    let mut seen: HashSet<String> = HashSet::new();
    let mut out = Vec::with_capacity(list.len());
    for e in list {
        let Value::Object(e) = e else {
            return Err("every entry must be an object".into());
        };
        let id = match e.get("id") {
            Some(Value::Number(n)) if n.is_i64() || n.is_u64() => n.to_string(),
            _ => return Err("every entry needs an integer id".into()),
        };
        if !seen.insert(id.clone()) {
            return Err(format!("duplicate id {}", id));
        }
        for k in ["date", "build", "kind", "stamp", "headline"] {
            if let Some(v) = e.get(k) {
                if !v.is_string() {
                    return Err(format!("entry {}: {} must be text", id, k));
                }
            }
        }
        let headline = e.get("headline").and_then(Value::as_str).unwrap_or("");
        if headline.trim().is_empty() && !e.get("title").map(py::truthy).unwrap_or(false) {
            return Err(format!("entry {}: the headline is empty", id));
        }
        if let Some(d) = e.get("date").and_then(Value::as_str) {
            if !d.is_empty() && !date_ok(d) {
                return Err(format!("entry {}: date must be YYYY-MM-DD", id));
            }
        }
        let mut e = e.clone();
        if let Some(Value::String(h)) = e.get("headline") {
            let h = h.trim().to_string();
            e.insert("headline".into(), Value::String(h));
        }
        if let Some(s) = e.get("sections") {
            let s = sections(s, &format!("entry {}", id))?;
            e.insert("sections".into(), Value::Object(s));
        }
        if let Some(pl) = e.get("pl") {
            let Value::Object(pl) = pl else {
                return Err(format!("entry {}: pl must be an object", id));
            };
            let mut pl = pl.clone();
            if let Some(h) = pl.get("headline") {
                let Some(h) = h.as_str() else {
                    return Err(format!("entry {}: pl headline must be text", id));
                };
                let h = h.trim().to_string();
                pl.insert("headline".into(), Value::String(h));
            }
            if let Some(s) = pl.get("sections") {
                let s = sections(s, &format!("entry {} pl", id))?;
                pl.insert("sections".into(), Value::Object(s));
            }
            e.insert("pl".into(), Value::Object(pl));
        }
        out.push(Value::Object(e));
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn checks() {
        assert!(validate(Some(&json!([{"id": 1, "headline": " a ", "sections": {"x": [" b ", ""]}}]))).is_ok());
        assert_eq!(validate(Some(&json!([{"id": true, "headline": "a"}]))).unwrap_err(), "every entry needs an integer id");
        assert_eq!(validate(Some(&json!([{"id": 1, "headline": ""}]))).unwrap_err(), "entry 1: the headline is empty");
        assert_eq!(validate(Some(&json!([{"id": 1, "headline": "a", "date": "2026-1-1"}]))).unwrap_err(), "entry 1: date must be YYYY-MM-DD");
        assert_eq!(validate(Some(&json!([{"id": 1, "headline": "a", "sections": {"x": [1]}}]))).unwrap_err(), "entry 1: section 'x' must be a list of text lines");
        let v = validate(Some(&json!([{"id": 2, "headline": " h ", "sections": {"x": [" b ", ""], "y": []}}]))).unwrap();
        assert_eq!(v[0], json!({"id": 2, "headline": "h", "sections": {"x": ["b"]}}));
    }
}
