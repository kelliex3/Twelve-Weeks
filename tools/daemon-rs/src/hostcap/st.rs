//! Rust side of the hostcap differential test (verify/py/hostcap_harness.py is the Python side): `t2r-daemon selftest hostcap < script`.
use super::*;
use crate::py;
use serde_json::{json, Map, Value};
use std::io::{BufRead, Write};
use std::sync::atomic::AtomicU64;
use std::sync::Arc;

static NOW: AtomicU64 = AtomicU64::new(0);

fn clk() -> f64 {
    f64::from_bits(NOW.load(Ordering::Relaxed))
}

fn sort_keys(v: Value) -> Value {
    match v {
        Value::Object(m) => {
            let mut e: Vec<(String, Value)> = m.into_iter().collect();
            e.sort_by(|a, b| a.0.cmp(&b.0));
            let mut o = Map::new();
            for (k, x) in e {
                o.insert(k, sort_keys(x));
            }
            Value::Object(o)
        }
        Value::Array(a) => Value::Array(a.into_iter().map(sort_keys).collect()),
        x => x,
    }
}

fn gs<'a>(c: &'a Value, k: &str) -> &'a str {
    c.get(k).and_then(|v| v.as_str()).unwrap_or("")
}

fn gi(c: &Value, k: &str) -> i64 {
    c.get(k).and_then(|v| v.as_i64()).unwrap_or(0)
}

fn gos<'a>(c: &'a Value, k: &str) -> Option<&'a str> {
    c.get(k).and_then(|v| v.as_str())
}

fn row_val(v: rusqlite::types::ValueRef) -> Value {
    use rusqlite::types::ValueRef as R;
    match v {
        R::Null => Value::Null,
        R::Integer(i) => Value::from(i),
        R::Real(f) => Value::from(f),
        R::Text(t) => Value::from(String::from_utf8_lossy(t).into_owned()),
        R::Blob(_) => Value::Null,
    }
}

fn dump_table(c: &Connection, t: &str) -> Value {
    let Ok(mut st) = c.prepare(&format!("SELECT * FROM {} ORDER BY token", t)) else { return Value::Null };
    let cols: Vec<Value> = st.column_names().iter().map(|s| Value::from(*s)).collect();
    let n = cols.len();
    let Ok(mut rows) = st.query([]) else { return Value::Null };
    let mut out = Vec::new();
    while let Ok(Some(r)) = rows.next() {
        out.push(Value::Array((0..n).map(|i| r.get_ref(i).map(row_val).unwrap_or(Value::Null)).collect()));
    }
    json!({"cols": cols, "rows": out})
}

struct Engine {
    path: Arc<Mutex<Option<String>>>,
    hc: Option<HostCap>,
    vtab: HashMap<String, (i64, String)>,
}

impl Engine {
    fn conn(&self) -> Res<Connection> {
        let p = self.path.lock().clone().unwrap_or_default();
        Connection::open(p).map_err(e2s)
    }

    fn call(&mut self, c: &Value) -> Value {
        let op = gs(c, "op");
        match op {
            "init" => {
                let p = gs(c, "db").to_string();
                let _ = std::fs::remove_file(&p);
                *self.path.lock() = Some(p);
                return Value::Null;
            }
            "new" => {
                let path = self.path.clone();
                self.hc = Some(HostCap::with_now(
                    Box::new(move || Connection::open(path.lock().clone().unwrap_or_default()).map_err(e2s)),
                    Box::new(clk),
                ));
                return Value::Null;
            }
            "sql" => {
                return match self.conn().and_then(|cn| cn.execute_batch(gs(c, "q")).map_err(e2s)) {
                    Ok(()) => Value::Null,
                    Err(_) => json!({"err": 1}),
                };
            }
            "vtab" => {
                self.vtab.clear();
                if let Some(o) = c.get("t").and_then(|v| v.as_object()) {
                    for (k, v) in o {
                        let a = v.as_array().cloned().unwrap_or_default();
                        self.vtab.insert(k.clone(), (a.first().and_then(|x| x.as_i64()).unwrap_or(0), a.get(1).and_then(|x| x.as_str()).unwrap_or("").to_string()));
                    }
                }
                return Value::Null;
            }
            "parse_cap" => {
                let h = gs(c, "hex");
                let b: Vec<u8> = (0..h.len() / 2).filter_map(|i| u8::from_str_radix(&h[i * 2..i * 2 + 2], 16).ok()).collect();
                return match parse_cap(&b) {
                    None => Value::Null,
                    Some(p) => json!({"avg_kb": p.avg_kb, "peak_kb": p.peak_kb, "loss10": p.loss10, "n": p.n}),
                };
            }
            "parse_oneway" => {
                let h = gs(c, "hex");
                let b: Vec<u8> = (0..h.len() / 2).filter_map(|i| u8::from_str_radix(&h[i * 2..i * 2 + 2], 16).ok()).collect();
                return match parse_oneway(&b) {
                    None => Value::Null,
                    Some(p) => json!({"quiet_s": p.quiet_s, "rx_pkts": p.rx_pkts, "tx_kb": p.tx_kb, "in_match_s": p.in_match_s}),
                };
            }
            "need_kb" => return Value::from(need_kb(gi(c, "n"))),
            "need_kb_relay" => return Value::from(need_kb_relay(gi(c, "n"))),
            _ => {}
        }
        let Some(hc) = self.hc.as_ref() else { return json!({"err": 1}) };
        let tok = gs(c, "token");
        let r: Res<Value> = (|| -> Res<Value> {
            Ok(match op {
                "get" => match hc.get(tok)? {
                    None => Value::Null,
                    Some(r) => json!({"hosted": r.hosted, "best_kb": r.best_kb, "best_n": r.best_n, "last_kb": r.last_kb, "lag_n": r.lag_n, "lag_t": r.lag_t,
                        "ceil_kb": r.ceil_kb, "last_t": r.last_t, "lag_mid": r.lag_mid}),
                },
                "lagged_in" => Value::Bool(hc.lagged_in(gos(c, "mid"), tok)?),
                "host_report" => {
                    hc.host_report(tok, gi(c, "n"), gi(c, "avg"), gi(c, "peak"), gos(c, "mid"))?;
                    Value::Null
                }
                "client_report" => Value::Bool(hc.client_report(gs(c, "mid"), gs(c, "host"), tok, gi(c, "loss10"))?),
                "note_lag" => Value::Bool(hc.note_lag(gs(c, "host"), gos(c, "mid"))?),
                "forget" => {
                    let live: std::collections::HashSet<String> = c.get("live").and_then(|v| v.as_array()).map(|a| a.iter().filter_map(|x| x.as_str()).map(|s| s.to_string()).collect()).unwrap_or_default();
                    hc.forget_matches(&live);
                    Value::Null
                }
                "proven_for" => Value::Bool(hc.proven_for(tok, gi(c, "n"))?),
                "verdict" => {
                    let v = hc.verdict(tok, gi(c, "n"))?;
                    json!([v.0, v.1])
                }
                "oneway_note" => {
                    let v = hc.oneway_note(tok, gos(c, "mid"), c.get("host").and_then(|v| v.as_bool()).unwrap_or(false))?;
                    json!([v.0, v.1])
                }
                "oneway_get" => {
                    let v = hc.oneway_get(tok, gi(c, "days"))?;
                    json!([v.0, v.1])
                }
                "pick" => {
                    let cands: Vec<(String, (i64, i64))> = c
                        .get("cands")
                        .and_then(|v| v.as_array())
                        .map(|a| {
                            a.iter()
                                .map(|x| {
                                    let k = x.get(1).and_then(|k| k.as_array()).cloned().unwrap_or_default();
                                    (x.get(0).and_then(|p| p.as_str()).unwrap_or("").to_string(), (k.first().and_then(|v| v.as_i64()).unwrap_or(0), k.get(1).and_then(|v| v.as_i64()).unwrap_or(0)))
                                })
                                .collect()
                        })
                        .unwrap_or_default();
                    let vtab = &self.vtab;
                    let db = gs(c, "src") == "db";
                    let vf = |t: &str, n: i64| -> Res<(i64, String)> {
                        if db {
                            hc.verdict(t, n)
                        } else {
                            Ok(vtab.get(t).cloned().unwrap_or((1, "unknown".to_string())))
                        }
                    };
                    let r = pick(&cands, gi(c, "n"), &vf, c.get("window").and_then(|v| v.as_i64()), &|t: &str| format!("L{}", t))?;
                    json!([r.0, r.1])
                }
                _ => return Err("KeyError".into()),
            })
        })();
        match r {
            Ok(v) => v,
            Err(_) => json!({"err": 1}),
        }
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let mut eng = Engine { path: Arc::new(Mutex::new(None)), hc: None, vtab: HashMap::new() };
    for line in input.lines() {
        let Ok(line) = line else { return 2 };
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let Ok(c) = serde_json::from_str::<Value>(line) else {
            eprintln!("bad line: {}", line);
            return 2;
        };
        if let Some(n) = c.get("now").and_then(|v| v.as_f64()) {
            NOW.store(n.to_bits(), Ordering::Relaxed);
        }
        let r = eng.call(&c);
        let s = eng.hc.as_ref().map(|h| h.dump_state()).unwrap_or(Value::Null);
        let d = if eng.path.lock().is_some() {
            match eng.conn() {
                Ok(cn) => json!({"host_cap": dump_table(&cn, "host_cap"), "oneway": dump_table(&cn, "oneway")}),
                Err(_) => Value::Null,
            }
        } else {
            Value::Null
        };
        let mut o = Map::new();
        o.insert("d".into(), d);
        o.insert("r".into(), r);
        o.insert("s".into(), s);
        let _ = writeln!(out, "{}", py::dumps_compact(&sort_keys(Value::Object(o))));
    }
    0
}
