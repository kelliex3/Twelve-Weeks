//! Admin listener client (loopback HTTP/1.0, header X-Admin-Secret) and the snapshot readers.
use crate::bot::Ctx;
use crate::norm::norm_json;
use serde_json::Value;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

pub fn secret(ctx: &Ctx) -> String {
    let p = ctx.workdir.join("admin-secret.txt");
    for _ in 0..50 {
        if let Ok(s) = std::fs::read_to_string(&p) {
            let s = s.trim().to_string();
            if s.len() >= 16 {
                return s;
            }
        }
        std::thread::sleep(Duration::from_millis(200));
    }
    String::new()
}

/// One admin call. Returns (HTTP status, body text). `secret` None = no header (the /status call needs none).
pub fn call(ctx: &Ctx, method: &str, path: &str, body: Option<&str>, use_secret: bool) -> (u16, String) {
    let Ok(mut s) = TcpStream::connect(("127.0.0.1", ctx.admin_port)) else {
        return (0, "connect failed".into());
    };
    let _ = s.set_read_timeout(Some(Duration::from_secs(10)));
    let mut req = format!("{} {} HTTP/1.0\r\nHost: 127.0.0.1\r\n", method, path);
    if use_secret {
        req.push_str(&format!("X-Admin-Secret: {}\r\n", secret(ctx)));
    }
    match body {
        Some(b) => req.push_str(&format!("Content-Type: application/json\r\nContent-Length: {}\r\n\r\n{}", b.len(), b)),
        None => req.push_str("\r\n"),
    }
    if s.write_all(req.as_bytes()).is_err() {
        return (0, "write failed".into());
    }
    let mut raw = Vec::new();
    let _ = s.read_to_end(&mut raw);
    let text = String::from_utf8_lossy(&raw).to_string();
    let (head, body) = text.split_once("\r\n\r\n").unwrap_or((&text, ""));
    let code = head.split_whitespace().nth(1).and_then(|c| c.parse().ok()).unwrap_or(0);
    (code, body.to_string())
}

/// Admin call recorded in the script with the normalised JSON reply (keys sorted).
pub fn call_rec(ctx: &Ctx, label: &str, method: &str, path: &str, body: Option<&str>, use_secret: bool) -> Option<Value> {
    let (code, txt) = call(ctx, method, path, body, use_secret);
    let mut v = serde_json::from_str::<Value>(&txt).ok();
    match v.as_mut() {
        Some(j) => {
            norm_json(j);
            ctx.rec.script(format!("admin {} {} {} -> {} {}", label, method, path, code, serde_json::to_string(&sorted(j)).unwrap_or_default()));
        }
        None => ctx.rec.script(format!("admin {} {} {} -> {} (not JSON) {}", label, method, path, code, txt.replace('\n', " "))),
    }
    v
}

pub fn sorted(v: &Value) -> Value {
    match v {
        Value::Object(m) => {
            let mut keys: Vec<&String> = m.keys().collect();
            keys.sort();
            let mut o = serde_json::Map::new();
            for k in keys {
                o.insert(k.clone(), sorted(&m[k]));
            }
            Value::Object(o)
        }
        Value::Array(a) => Value::Array(a.iter().map(sorted).collect()),
        x => x.clone(),
    }
}

fn status_ts(ctx: &Ctx) -> Option<f64> {
    let s = std::fs::read_to_string(ctx.workdir.join("t2r-status.json")).ok()?;
    serde_json::from_str::<Value>(&s).ok()?.get("ts")?.as_f64()
}

/// Waits for the NEXT write of t2r-status.json (its `ts` differs from the one on disk when this is called), then records the normalised
/// content. The daemon rewrites it every 10 s, so the snapshot reflects the state at the moment of the call or later; scenarios call it
/// only in a quiet state. `live-matches.json` (saved by the same tick) is recorded too.
pub fn snapshot(ctx: &Ctx, label: &str) {
    let ts0 = status_ts(ctx);
    let end = Instant::now() + Duration::from_secs(25);
    let mut got: Option<Value> = None;
    while Instant::now() < end {
        if let Ok(s) = std::fs::read_to_string(ctx.workdir.join("t2r-status.json")) {
            if let Ok(v) = serde_json::from_str::<Value>(&s) {
                let ts = v.get("ts").and_then(|x| x.as_f64());
                if ts.is_some() && ts != ts0 {
                    got = Some(v);
                    break;
                }
            }
        }
        std::thread::sleep(Duration::from_millis(300));
    }
    match got {
        Some(mut v) => {
            norm_json(&mut v);
            ctx.rec.script(format!("snapshot {} t2r-status.json {}", label, serde_json::to_string(&sorted(&v)).unwrap_or_default()));
        }
        None => ctx.rec.script(format!("snapshot {} t2r-status.json (not rewritten within 25 s)", label)),
    }
    file_json(ctx, label, "live-matches.json");
}

pub fn file_json(ctx: &Ctx, label: &str, name: &str) {
    match std::fs::read_to_string(ctx.workdir.join(name)) {
        Ok(s) => match serde_json::from_str::<Value>(&s) {
            Ok(mut v) => {
                norm_json(&mut v);
                // client_versions.json keeps its order (least recently launched first = the eviction order): not sorted
                let out = if name == "client_versions.json" { v.clone() } else { sorted(&v) };
                ctx.rec.script(format!("snapshot {} {} {}", label, name, serde_json::to_string(&out).unwrap_or_default()));
            }
            Err(_) => ctx.rec.script(format!("snapshot {} {} (not JSON)", label, name)),
        },
        Err(_) => ctx.rec.script(format!("snapshot {} {} (absent)", label, name)),
    }
}

/// Waits until client_versions.json / linux_clients.json hold what the bots reported (the daemon saves them 5-15 s after a change).
pub fn wait_versions(ctx: &Ctx, secs: u64) {
    let want_v: Value = Value::Object(ctx.versions.lock().unwrap().iter().map(|(k, v)| (k.clone(), Value::String(v.clone()))).collect());
    let want_l: Value = Value::Array(ctx.linux.lock().unwrap().iter().map(|k| Value::String(k.clone())).collect());
    let end = Instant::now() + Duration::from_secs(secs);
    let rd = |n: &str| std::fs::read_to_string(ctx.workdir.join(n)).ok().and_then(|s| serde_json::from_str::<Value>(&s).ok());
    while Instant::now() < end {
        let v = rd("client_versions.json").unwrap_or(Value::Object(Default::default()));
        let l = rd("linux_clients.json").unwrap_or(Value::Array(vec![]));
        if v == want_v && l == want_l {
            return;
        }
        std::thread::sleep(Duration::from_millis(400));
    }
}
