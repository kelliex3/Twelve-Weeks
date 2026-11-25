//! Rust side of the hostmig differential test (verify/py/hostmig_harness.py is the Python side): `t2r-daemon selftest hostmig < script`.
use super::*;
use serde_json::json;
use std::io::{BufRead, Write};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;

static NOW: AtomicU64 = AtomicU64::new(0);

fn clk() -> f64 {
    f64::from_bits(NOW.load(Ordering::Relaxed))
}

enum Tv {
    Int(i64),
    Bool(bool),
    Raise,
}

#[derive(Default)]
struct Tabs {
    ping: HashMap<String, Option<i64>>,
    perf: HashMap<String, Tv>,
    link: HashMap<String, Tv>,
    reg: HashMap<String, String>,
    sid: HashMap<String, i64>,
    party: HashMap<String, Vec<String>>,
    waited: HashMap<String, f64>,
    hostok: HashMap<String, bool>,
    pool_mode: Option<String>,
}

type T = Arc<Mutex<Tabs>>;

fn ping_of(t: &T, a: &str, b: &str) -> Option<i64> {
    t.lock().ping.get(&format!("{}|{}", a, b)).copied().flatten()
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

fn jstr(v: &[String]) -> Value {
    Value::Array(v.iter().map(|s| Value::from(s.as_str())).collect())
}

fn opt_s(v: Option<String>) -> Value {
    v.map(Value::from).unwrap_or(Value::Null)
}

fn gs<'a>(c: &'a Value, k: &str) -> &'a str {
    c.get(k).and_then(|v| v.as_str()).unwrap_or("")
}

fn gi(c: &Value, k: &str) -> i64 {
    c.get(k).and_then(|v| v.as_i64()).unwrap_or(0)
}

fn gf(c: &Value, k: &str) -> f64 {
    c.get(k).and_then(|v| v.as_f64()).unwrap_or(0.0)
}

fn gb(c: &Value, k: &str) -> bool {
    match c.get(k) {
        Some(Value::Bool(b)) => *b,
        Some(Value::Null) | None => false,
        Some(Value::Number(n)) => n.as_f64().is_some_and(|x| x != 0.0),
        Some(Value::String(s)) => !s.is_empty(),
        Some(Value::Array(a)) => !a.is_empty(),
        Some(Value::Object(o)) => !o.is_empty(),
    }
}

fn gsv(c: &Value, k: &str) -> Vec<String> {
    c.get(k).and_then(|v| v.as_array()).map(|a| a.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect()).unwrap_or_default()
}

fn giv(c: &Value, k: &str) -> Vec<i64> {
    c.get(k).and_then(|v| v.as_array()).map(|a| a.iter().filter_map(|x| x.as_i64()).collect()).unwrap_or_default()
}

fn int_map<X>(c: Option<&Value>, f: impl Fn(&Value) -> Option<X>) -> HashMap<i64, X> {
    c.and_then(|v| v.as_object()).map(|o| o.iter().filter_map(|(k, v)| Some((k.parse::<i64>().ok()?, f(v)?))).collect()).unwrap_or_default()
}

fn opt_i(v: &Value) -> Option<i64> {
    if v.is_null() {
        None
    } else {
        v.as_i64()
    }
}

struct Engine {
    t: T,
    hm: Option<HostMigration>,
    rp: Option<HostRepick>,
    ps: PerfScores,
    exports: HashMap<String, Value>,
}

impl Engine {
    fn new() -> Self {
        Engine { t: Arc::new(Mutex::new(Tabs::default())), hm: None, rp: None, ps: PerfScores::with_now(Box::new(clk)), exports: HashMap::new() }
    }

    fn new_hm(&mut self, c: &Value) {
        let mut o = HmOpts::new(gb(c, "enabled"));
        o.now = Box::new(clk);
        let kw = c.get("kw").cloned().unwrap_or(Value::Null);
        if let Some(x) = kw.get("alive_window").and_then(|v| v.as_f64()) {
            o.alive_window = x;
        }
        if let Some(x) = kw.get("silent_window").and_then(|v| v.as_f64()) {
            o.silent_window = x;
        }
        if let Some(x) = kw.get("lone_wait").and_then(|v| v.as_f64()) {
            o.lone_wait = x;
        }
        if let Some(x) = kw.get("min_left").and_then(|v| v.as_i64()) {
            o.min_left = x.max(0) as usize;
        }
        if let Some(x) = kw.get("settle").and_then(|v| v.as_f64()) {
            o.settle = x;
        }
        if let Some(x) = kw.get("bridge_grace").and_then(|v| v.as_f64()) {
            o.bridge_grace = x;
        }
        if let Some(x) = kw.get("unknown_keep_going").and_then(|v| v.as_bool()) {
            o.unknown_keep_going = x;
        }
        if let Some(x) = kw.get("collect").and_then(|v| v.as_f64()) {
            o.collect = x;
        }
        if let Some(x) = kw.get("min_silent").and_then(|v| v.as_f64()) {
            o.min_silent = Some(x);
        }
        let t = self.t.clone();
        if gb(c, "ping") {
            o.ping = Some(Box::new(move |a, b| ping_of(&t, a, b)));
        }
        let t = self.t.clone();
        if gb(c, "perf") {
            o.perf = Some(Box::new(move |k| match t.lock().perf.get(k) {
                Some(Tv::Int(i)) => Some(*i),
                Some(Tv::Raise) => None,
                _ => Some(0),
            }));
        }
        let t = self.t.clone();
        if gb(c, "pool") {
            o.pool = Some(Box::new(move |ts: &[String]| {
                let m = t.lock().pool_mode.clone();
                let ts = ts.to_vec();
                match m.as_deref() {
                    Some("raise") => None,
                    Some("first") => Some((ts.iter().take(1).cloned().collect(), "pool first".to_string())),
                    Some("dropfirst") => Some((ts.iter().skip(1).cloned().collect(), "pool dropfirst".to_string())),
                    Some("none") => Some((Vec::new(), "pool none".to_string())),
                    Some("last2") => Some((ts[ts.len().saturating_sub(2)..].to_vec(), "pool last2".to_string())),
                    _ => Some((ts, "pool all".to_string())),
                }
            }));
        }
        let t = self.t.clone();
        if gb(c, "link") {
            o.link_alive = Some(Box::new(move |k| match t.lock().link.get(k) {
                Some(Tv::Bool(b)) => Some(*b),
                Some(Tv::Raise) => None,
                _ => Some(true),
            }));
        }
        self.hm = Some(HostMigration::new(o));
        self.t.lock().pool_mode = c.get("pool").and_then(|v| v.as_str()).map(|s| s.to_string());
    }

    fn tab(&mut self, c: &Value) {
        let (t, k) = (gs(c, "t"), gs(c, "k").to_string());
        let v = c.get("v").cloned().unwrap_or(Value::Null);
        let mut g = self.t.lock();
        match t {
            "ping" => {
                g.ping.insert(k, opt_i(&v));
            }
            "perf" | "link" => {
                let m = if t == "perf" { &mut g.perf } else { &mut g.link };
                match &v {
                    Value::Null => {
                        m.remove(&k);
                    }
                    Value::String(_) => {
                        m.insert(k, Tv::Raise);
                    }
                    Value::Bool(b) => {
                        m.insert(k, Tv::Bool(*b));
                    }
                    x => {
                        m.insert(k, Tv::Int(x.as_i64().unwrap_or(0)));
                    }
                }
            }
            "reg" => match v.as_str() {
                Some(s) => {
                    g.reg.insert(k, s.to_string());
                }
                None => {
                    g.reg.remove(&k);
                }
            },
            "sid" => match v.as_i64() {
                Some(s) => {
                    g.sid.insert(k, s);
                }
                None => {
                    g.sid.remove(&k);
                }
            },
            "party" => match v.as_array() {
                Some(a) => {
                    g.party.insert(k, a.iter().map(|x| x.as_str().unwrap_or("").to_string()).collect());
                }
                None => {
                    g.party.remove(&k);
                }
            },
            "waited" => match v.as_f64() {
                Some(s) => {
                    g.waited.insert(k, s);
                }
                None => {
                    g.waited.remove(&k);
                }
            },
            "hostok" => match v.as_bool() {
                Some(s) => {
                    g.hostok.insert(k, s);
                }
                None => {
                    g.hostok.remove(&k);
                }
            },
            _ => {}
        }
    }

    fn rep_from(v: &Value) -> PerfRep {
        PerfRep { n250: gi(v, "n250"), n1000: gi(v, "n1000"), worst: gi(v, "worst"), avg_ms: gf(v, "avg_ms"), vram: gi(v, "vram"), t: 0.0 }
    }

    fn rep_val(r: &PerfRep, with_t: bool) -> Value {
        let mut m = Map::new();
        m.insert("avg_ms".into(), Value::from(r.avg_ms));
        m.insert("n1000".into(), Value::from(r.n1000));
        m.insert("n250".into(), Value::from(r.n250));
        if with_t {
            m.insert("t".into(), Value::from(r.t));
        }
        m.insert("vram".into(), Value::from(r.vram));
        m.insert("worst".into(), Value::from(r.worst));
        Value::Object(m)
    }

    fn call(&mut self, c: &Value) -> Value {
        let op = gs(c, "op");
        let mid = gs(c, "mid").to_string();
        let tok = gs(c, "token").to_string();
        match op {
            "new" => {
                self.new_hm(c);
                return Value::Null;
            }
            "poolmode" => {
                self.t.lock().pool_mode = c.get("mode").and_then(|v| v.as_str()).map(|s| s.to_string());
                return Value::Null;
            }
            "tab" => {
                self.tab(c);
                return Value::Null;
            }
            "rp_new" => {
                self.rp = Some(HostRepick::new(Box::new(clk), c.get("mode").and_then(|v| v.as_i64())));
                return Value::Null;
            }
            _ => {}
        }
        let t = self.t.clone();
        match op {
            "ps_report" => {
                self.ps.report(&tok, &Self::rep_from(c.get("rep").unwrap_or(&Value::Null)), gi(c, "steam"));
                return Value::Null;
            }
            "ps_prune" => return Value::from(self.ps.prune() as u64),
            "ps_get" => return self.ps.get(&tok, gi(c, "steam")).map(|r| Self::rep_val(&r, true)).unwrap_or(Value::Null),
            "ps_penalty" => return Value::from(self.ps.penalty(&tok, gi(c, "steam"))),
            "ps_dump" => return self.ps.dump(),
            "parse_perf" => {
                let h = gs(c, "hex");
                let hex: Vec<u8> = (0..h.len() / 2).filter_map(|i| u8::from_str_radix(&h[i * 2..i * 2 + 2], 16).ok()).collect();
                return match parse_perf(&hex) {
                    None => Value::Null,
                    Some((t, r)) => Value::Array(vec![Value::from(t), Self::rep_val(&r, false)]),
                };
            }
            "perf_penalty" => {
                return Value::from(match c.get("rep") {
                    Some(v) if !v.is_null() => perf_penalty(Some(&Self::rep_from(v))),
                    _ => perf_penalty(None),
                })
            }
            "host_key" => {
                let k = host_key(&giv(c, "pings"), gi(c, "pen"));
                return json!([k.0, k.1]);
            }
            "region_of" => return opt_s(region_of(c.get("loc").and_then(|v| v.as_str())).map(|s| s.to_string())),
            "parse_pins" => {
                let mut m = Map::new();
                for (k, v) in parse_pins(gs(c, "text")) {
                    m.insert(k.to_string(), Value::from(v));
                }
                return Value::Object(m);
            }
            "region_first" => {
                let toks = gsv(c, "tokens");
                let t1 = t.clone();
                let t2 = t.clone();
                let t3 = t.clone();
                let regf = move |x: &str| t1.lock().reg.get(x).cloned();
                let memf = move |x: &str| t2.lock().party.get(x).cloned().unwrap_or_else(|| vec![x.to_string()]);
                let waf = move |x: &str| t3.lock().waited.get(x).copied().unwrap_or(0.0);
                let mw = c.get("max_wait").and_then(|v| v.as_f64());
                let r = region_first(&toks, &regf, if gb(c, "members") { Some(&memf) } else { None }, if gb(c, "waited") { Some(&waf) } else { None }, mw);
                return Value::Array(vec![jstr(&r.0), opt_s(r.1)]);
            }
            "ping_fit" => {
                let pl = gsv(c, "players");
                let t1 = t.clone();
                let t2 = t.clone();
                let t3 = t.clone();
                let pf = move |a: &str, b: &str| ping_of(&t1, a, b);
                let uf = move |x: &str| t2.lock().party.get(x).cloned().unwrap_or_else(|| vec![x.to_string()]);
                let hf = move |x: &str| t3.lock().hostok.get(x).copied().unwrap_or(true);
                let r = ping_fit(&pl, &pf, gi(c, "limit"), if gb(c, "units") { Some(&uf) } else { None }, if gb(c, "hostok") { Some(&hf) } else { None });
                return Value::Array(vec![jstr(&r.0), jstr(&r.1), opt_s(r.2), Value::from(r.3)]);
            }
            "region_pool" => {
                let pl = gsv(c, "players");
                let ev = c.get("everyone").filter(|v| !v.is_null()).map(|_| gsv(c, "everyone"));
                let pins: Option<HashMap<i64, String>> = c.get("pins").filter(|v| !v.is_null()).map(|_| int_map(c.get("pins"), |v| v.as_str().map(|s| s.to_string())));
                let t1 = t.clone();
                let t2 = t.clone();
                let t3 = t.clone();
                let rf = move |x: &str| t1.lock().reg.get(x).cloned();
                let sf = move |x: &str| t2.lock().sid.get(x).copied().unwrap_or(0);
                let pf = move |a: &str, b: &str| ping_of(&t3, a, b);
                let r = region_pool(&pl, &rf, &sf, &pf, pins.as_ref(), ev.as_deref());
                return Value::Array(vec![jstr(&r.0), Value::from(r.1)]);
            }
            _ => {}
        }
        if op.starts_with("rp_") {
            return self.rp_call(c, op, &mid, &t);
        }
        let Some(hm) = self.hm.as_ref() else { return json!({"err": "AttributeError"}) };
        match op {
            "start" => hm.start(&mid, &gsv(c, "players")),
            "add" => hm.add_player(&mid, &tok, gi(c, "slot").max(0) as usize),
            "end" => hm.end(&mid),
            "has" => return Value::Bool(hm.has(&tok)),
            "given_up" => return Value::Bool(hm.given_up(&mid)),
            "pending_stale" => return Value::from(hm.pending_stale(&mid, gf(c, "secs"))),
            "host_info" => {
                return match hm.host_info(&mid) {
                    None => Value::Null,
                    Some(h) => json!({"host": h.host, "epoch": h.epoch, "pending": h.pending, "given_up": h.given_up, "alone": h.alone}),
                }
            }
            "snapshot" => {
                return match hm.snapshot(&mid) {
                    None => Value::Null,
                    Some(s) => Value::Array(vec![Value::from(s.t0), jstr(&s.players), json!(s.alive), Value::from(s.host as u64)]),
                }
            }
            "mids" => return jstr(&hm.mids()),
            "sizes" => {
                let (a, b) = hm.sizes();
                return json!([a, b]);
            }
            "export" => {
                let r = hm.export_match(&mid);
                self.exports.insert(mid.clone(), r.clone().unwrap_or(Value::Null));
                return r.unwrap_or(Value::Null);
            }
            "import_last" => {
                let src = c.get("src").and_then(|v| v.as_str()).unwrap_or(&mid).to_string();
                let rec = self.exports.get(&src).cloned().unwrap_or(Value::Null);
                if rec.is_null() {
                    hm.end(&mid);
                    return json!({"err": "TypeError"});
                }
                hm.import_match(&mid, &rec);
            }
            "import_rec" => hm.import_match(&mid, c.get("rec").unwrap_or(&Value::Null)),
            "host_token" => return opt_s(hm.host_token(&tok)),
            "depart" => hm.depart(&tok, gs(c, "reason")),
            "bridge_gone" => hm.bridge_gone(&tok),
            "bridge_back" => hm.bridge_back(&tok),
            "poll" => {
                return match hm.poll(&tok, gi(c, "flags") as u8, gi(c, "epoch") as u8) {
                    None => Value::Null,
                    Some(v) => json!(v),
                }
            }
            "replaced_age" => return hm.replaced_age(&tok).map(Value::from).unwrap_or(Value::Null),
            "freezes" => return Value::from(hm.freezes(&mid, gi(c, "slot").max(0) as usize, gf(c, "win")) as u64),
            "roster" => return hm.roster(&mid).map(|s| json!(s.iter().collect::<Vec<_>>())).unwrap_or(Value::Null),
            "stale_slots" => {
                let (a, b) = hm.stale_slots(&mid);
                return json!([a.iter().collect::<Vec<_>>(), b.iter().collect::<Vec<_>>()]);
            }
            "handover_ok" => {
                let (a, b) = hm.handover_ok(&mid);
                return json!([a, b]);
            }
            "handover" => return Value::Bool(hm.handover(&mid, gi(c, "slot"))),
            "handover_pending" => return Value::Bool(hm.handover_pending(&mid)),
            "tick" => hm.tick(),
            "hlp" => return Value::Bool(hm.host_lost_pending(&mid)),
            _ => return json!({"err": "KeyError"}),
        }
        Value::Null
    }

    fn rp_call(&mut self, c: &Value, op: &str, mid: &str, t: &T) -> Value {
        let Some(rp) = self.rp.as_ref() else { return json!({"err": "AttributeError"}) };
        match op {
            "rp_forget" => rp.forget(mid),
            "rp_due" => return Value::Bool(rp.due(mid, gf(c, "started"))),
            "rp_observe" => {
                let mut d: IndexMap<(String, String), Option<i64>> = IndexMap::new();
                for x in c.get("rtt").and_then(|v| v.as_array()).cloned().unwrap_or_default() {
                    let a = x.get(0).and_then(|v| v.as_str()).unwrap_or("").to_string();
                    let b = x.get(1).and_then(|v| v.as_str()).unwrap_or("").to_string();
                    d.insert((a, b), x.get(2).and_then(opt_i));
                }
                let v: Vec<((String, String), Option<i64>)> = d.into_iter().collect();
                rp.observe(mid, &v);
            }
            "rp_saturated" => {
                let t1 = t.clone();
                let pf = move |a: &str, b: &str| ping_of(&t1, a, b);
                let (a, b) = rp.saturated(mid, gs(c, "host"), &gsv(c, "others"), &pf);
                return json!([a, b]);
            }
            "rp_migrated" => rp.migrated(mid, &giv(c, "roster").into_iter().collect()),
            "rp_check" => {
                let roster: BTreeSet<i64> = giv(c, "roster").into_iter().collect();
                let mut scores: IndexMap<i64, Option<Key>> = IndexMap::new();
                for x in c.get("scores").and_then(|v| v.as_array()).cloned().unwrap_or_default() {
                    let s = x.get(0).and_then(|v| v.as_i64()).unwrap_or(0);
                    let k = x.get(1).and_then(|v| v.as_array()).map(|a| (a[0].as_i64().unwrap_or(0), a[1].as_i64().unwrap_or(0)));
                    scores.insert(s, k);
                }
                let labels: Option<HashMap<i64, String>> = c.get("labels").filter(|v| !v.is_null()).map(|_| int_map(c.get("labels"), |v| v.as_str().map(|s| s.to_string())));
                let info = c.get("info").filter(|v| !v.is_null()).map(|ii| {
                    let mut d: IndexMap<(i64, i64), Option<i64>> = IndexMap::new();
                    for x in ii.get("rtt").and_then(|v| v.as_array()).cloned().unwrap_or_default() {
                        d.insert((x[0].as_i64().unwrap_or(0), x[1].as_i64().unwrap_or(0)), x.get(2).and_then(opt_i));
                    }
                    CheckInfo {
                        rtt: d.into_iter().collect(),
                        perf: int_map(ii.get("perf"), |v| v.as_i64()),
                        loss: int_map(ii.get("loss"), |v| v.as_f64()),
                        age: ii.get("age").and_then(|v| v.as_f64()),
                    }
                });
                let cap = c.get("cap").filter(|v| !v.is_null()).map(|cp| CapOverride {
                    host_bad: cp.get("host_bad").and_then(|v| v.as_str()).map(|s| s.to_string()),
                    rank: int_map(cp.get("rank"), |v| v.as_i64()),
                    max_rank: cp.get("max_rank").and_then(|v| v.as_i64()),
                    text: int_map(cp.get("text"), |v| v.as_str().map(|s| s.to_string())),
                    saturated: gb(cp, "saturated"),
                });
                let r = rp.check(
                    mid,
                    gf(c, "started"),
                    &roster,
                    gi(c, "host"),
                    &scores,
                    c.get("left").and_then(|v| v.as_f64()),
                    c.get("busy").and_then(|v| v.as_str()),
                    labels.as_ref(),
                    info.as_ref(),
                    cap.as_ref(),
                );
                return match r {
                    Repick::Hold(s) => json!(["hold", s]),
                    Repick::Migrate(n) => json!(["migrate", n]),
                };
            }
            _ => return json!({"err": "KeyError"}),
        }
        Value::Null
    }
}

pub fn selftest(input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    let mut eng = Engine::new();
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
        crate::log::take_captured();
        let r = eng.call(&c);
        let logs: Vec<Value> = crate::log::take_captured().into_iter().map(Value::from).collect();
        let s = eng.hm.as_ref().map(|h| h.dump_state()).unwrap_or(Value::Null);
        let p = eng.rp.as_ref().map(|h| h.dump_state()).unwrap_or(Value::Null);
        let mut o = Map::new();
        o.insert("l".into(), Value::Array(logs));
        o.insert("p".into(), p);
        o.insert("r".into(), r);
        o.insert("s".into(), s);
        let _ = writeln!(out, "{}", py::dumps_compact(&sort_keys(Value::Object(o))));
    }
    0
}
