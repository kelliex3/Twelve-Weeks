use std::collections::{HashMap, HashSet};
use std::io;
use std::net::SocketAddrV4;
use std::time::Instant;

use indexmap::IndexMap;

use crate::pyfmt::{hex_sp, os_error_repr, PyAddr};
use crate::{dlog, evt};

const BURST_S: f64 = 1.5;
const REBIND_S: f64 = 5.0;
const STALE: f64 = 15.0;
const TABLE_MAX: usize = 512;
const RELAY_MAX: usize = 4096;
const BUCKETS_MAX: usize = 4096;
const STATS_LINES: usize = 24;
const SIZE_KEYS: [&str; 6] = ["<=128", "<=256", "<=512", "<=1024", "<=1200", ">1200"];

#[derive(Clone, Copy)]
pub struct Limits {
    pub udp_max: usize,
    pub pps: f64,
    pub bps: f64,
    pub idle_s: f64,
}

impl Limits {
    pub fn from_env() -> Limits {
        Limits {
            udp_max: crate::env_parse("T2R_UDP_MAX", 2048usize),
            pps: crate::env_parse("T2R_UDP_PPS", 3000.0),
            bps: crate::env_parse("T2R_UDP_BPS", (6 * 1024 * 1024) as f64),
            idle_s: crate::env_parse("T2R_RELAY_IDLE_S", 600.0),
        }
    }
}

pub trait Net {
    fn send(&mut self, data: &[u8], to: SocketAddrV4) -> io::Result<()>;
    fn send_many(&mut self, data: &[u8], to: &[SocketAddrV4]) -> u64 {
        let mut failed = 0;
        for t in to {
            if self.send(data, *t).is_err() {
                failed += 1;
            }
        }
        failed
    }
}

struct Bucket {
    pk: f64,
    by: f64,
    t: f64,
    dropped: u64,
    last_log: f64,
}

impl Bucket {
    fn new(lim: &Limits, now: f64) -> Bucket {
        Bucket { pk: lim.pps * BURST_S, by: lim.bps * BURST_S, t: now, dropped: 0, last_log: 0.0 }
    }

    fn allow(&mut self, lim: &Limits, size: usize, now: f64) -> bool {
        let dt = (now - self.t).max(0.0);
        self.t = now;
        self.pk = (lim.pps * BURST_S).min(self.pk + dt * lim.pps);
        self.by = (lim.bps * BURST_S).min(self.by + dt * lim.bps);
        if self.pk < 1.0 || self.by < size as f64 {
            self.dropped += 1;
            return false;
        }
        self.pk -= 1.0;
        self.by -= size as f64;
        true
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
enum Seen {
    Addr(SocketAddrV4, i16),
    Relay(u32, u8, u8),
}

#[derive(Clone, Copy)]
enum Table {
    Ka,
    Gp,
}

pub struct Relay {
    lim: Limits,
    seen: HashMap<Seen, u32>,
    ka: IndexMap<SocketAddrV4, f64>,
    gp: IndexMap<SocketAddrV4, f64>,
    buckets: HashMap<SocketAddrV4, Bucket>,
    counters: IndexMap<&'static str, u64>,
    last_prune: f64,
    sizes: [u64; 6],
    pk_in: IndexMap<SocketAddrV4, u64>,
    pk_out: IndexMap<SocketAddrV4, u64>,
    max_handle_ms: f64,
    pub sock_stats: String,
    slots: IndexMap<(u32, u8), (SocketAddrV4, f64)>,
    udp_stats: IndexMap<(SocketAddrV4, i16), u64>,
    relay_stats: IndexMap<(u32, u8, u8), u64>,
    relay_types: IndexMap<(u8, u8, u8, usize), u64>,
    udp_errs: u64,
    targets: Vec<SocketAddrV4>,
}

fn bump<K: std::hash::Hash + Eq>(m: &mut IndexMap<K, u64>, k: K) -> u64 {
    let c = m.entry(k).or_insert(0);
    *c += 1;
    *c
}

fn most_common<K: Copy>(m: &IndexMap<K, u64>, n: usize) -> Vec<(K, u64)> {
    let mut v: Vec<(K, u64)> = m.iter().map(|(k, c)| (*k, *c)).collect();
    v.sort_by(|a, b| b.1.cmp(&a.1));
    v.truncate(n);
    v
}

fn take<K: Copy>(m: &mut IndexMap<K, u64>) -> Vec<(K, u64)> {
    let mut v: Vec<(K, u64)> = m.iter().map(|(k, c)| (*k, *c)).collect();
    m.clear();
    v.sort_by(|a, b| b.1.cmp(&a.1));
    v
}

fn stats_line<K>(snap: &[(K, u64)], f: impl Fn(&K, u64) -> String) -> String {
    let mut s = snap.iter().take(STATS_LINES).map(|(k, c)| f(k, *c)).collect::<Vec<_>>().join(" | ");
    if snap.len() > STATS_LINES {
        s.push_str(&format!(" | ... +{} more", snap.len() - STATS_LINES));
    }
    s
}

fn first_of(m: &IndexMap<SocketAddrV4, f64>) -> Option<SocketAddrV4> {
    let mut best: Option<(SocketAddrV4, f64)> = None;
    for (a, t) in m {
        match best {
            Some((_, bt)) if !(*t < bt) => {}
            _ => best = Some((*a, *t)),
        }
    }
    best.map(|b| b.0)
}

impl Relay {
    pub fn new(lim: Limits) -> Relay {
        Relay {
            lim,
            seen: HashMap::new(),
            ka: IndexMap::new(),
            gp: IndexMap::new(),
            buckets: HashMap::new(),
            counters: IndexMap::new(),
            last_prune: 0.0,
            sizes: [0; 6],
            pk_in: IndexMap::new(),
            pk_out: IndexMap::new(),
            max_handle_ms: 0.0,
            sock_stats: String::new(),
            slots: IndexMap::new(),
            udp_stats: IndexMap::new(),
            relay_stats: IndexMap::new(),
            relay_types: IndexMap::new(),
            udp_errs: 0,
            targets: Vec::with_capacity(TABLE_MAX),
        }
    }

    fn count(&mut self, k: &'static str) -> u64 {
        bump(&mut self.counters, k)
    }

    fn emit_targets(&mut self, data: &[u8], net: &mut dyn Net) {
        if self.targets.is_empty() {
            return;
        }
        for t in &self.targets {
            bump(&mut self.pk_out, *t);
        }
        let failed = net.send_many(data, &self.targets);
        if failed > 0 {
            *self.counters.entry("send-failed").or_insert(0) += failed;
        }
    }

    fn seen_once(&mut self, k: Seen, limit: u32) -> bool {
        let c = self.seen.entry(k).or_insert(0);
        if *c < limit {
            *c += 1;
            true
        } else {
            false
        }
    }

    fn relay_table(&mut self, which: Table, data: &[u8], src: SocketAddrV4, now: f64, net: &mut dyn Net) {
        let mut targets = std::mem::take(&mut self.targets);
        targets.clear();
        {
            let table = match which {
                Table::Ka => &mut self.ka,
                Table::Gp => &mut self.gp,
            };
            if !table.contains_key(&src) && table.len() >= TABLE_MAX {
                table.retain(|_, ts| !(now - *ts > STALE));
                while table.len() >= TABLE_MAX {
                    match first_of(table) {
                        Some(a) => {
                            table.shift_remove(&a);
                        }
                        None => break,
                    }
                }
            }
            table.insert(src, now);
            table.retain(|a, ts| {
                if now - *ts > STALE {
                    false
                } else {
                    if *a != src {
                        targets.push(*a);
                    }
                    true
                }
            });
        }
        self.targets = targets;
        self.emit_targets(data, net);
    }

    fn relay_room(&mut self, now: f64) {
        if self.slots.len() < RELAY_MAX {
            return;
        }
        self.slots.retain(|_, (_, t)| !(now - *t > 120.0));
        if self.slots.len() >= RELAY_MAX {
            let mut order: Vec<(usize, f64)> = self.slots.values().enumerate().map(|(i, (_, t))| (i, *t)).collect();
            order.sort_by(|a, b| a.1.partial_cmp(&b.1).unwrap_or(std::cmp::Ordering::Equal));
            let gone: HashSet<usize> = order.iter().take(RELAY_MAX / 4).map(|(i, _)| *i).collect();
            let mut i = 0usize;
            self.slots.retain(|_, _| {
                let keep = !gone.contains(&i);
                i += 1;
                keep
            });
            self.count("relay-evicted");
        }
    }

    fn prune(&mut self, now: f64) {
        if now - self.last_prune < 60.0 {
            return;
        }
        self.last_prune = now;
        let idle = self.lim.idle_s;
        self.slots.retain(|_, (_, t)| !(now - *t > idle));
        self.buckets.retain(|_, b| !(now - b.t > 120.0));
        if self.seen.len() > 2000 {
            self.seen.clear();
        }
    }

    fn send_error(&mut self, e: io::Error, addr: SocketAddrV4) {
        self.udp_errs += 1;
        self.count("malformed");
        if self.udp_errs <= 20 {
            dlog!("UDP-ERR", "handling", os_error_repr(&e), PyAddr(addr));
        }
    }

    pub fn handle(&mut self, data: &[u8], addr: SocketAddrV4, now: f64, net: &mut dyn Net) {
        let t0 = Instant::now();
        self.handle_inner(data, addr, now, net);
        let ms = t0.elapsed().as_secs_f64() * 1000.0;
        if ms > self.max_handle_ms {
            self.max_handle_ms = ms;
        }
    }

    fn handle_inner(&mut self, data: &[u8], addr: SocketAddrV4, now: f64, net: &mut dyn Net) {
        self.prune(now);
        if self.seen.len() > 4000 {
            self.seen.clear();
        }
        let n = data.len();
        let size_ix = match n {
            0..=128 => 0,
            129..=256 => 1,
            257..=512 => 2,
            513..=1024 => 3,
            1025..=1200 => 4,
            _ => 5,
        };
        self.sizes[size_ix] += 1;
        bump(&mut self.pk_in, addr);
        if n > self.lim.udp_max {
            let c = self.count("oversized");
            if matches!(c, 1 | 10 | 100 | 1000) {
                dlog!("UDP-DROP oversized", PyAddr(addr), format!("{}B (#{})", n, c));
            }
            return;
        }
        if !self.buckets.contains_key(&addr) {
            if self.buckets.len() > BUCKETS_MAX {
                self.buckets.retain(|_, b| !(now - b.t > 10.0));
            }
            if self.buckets.len() > BUCKETS_MAX {
                self.count("rate-limited");
                return;
            }
            self.buckets.insert(addr, Bucket::new(&self.lim, now));
        }
        let lim = self.lim;
        let mut refused = None;
        if let Some(b) = self.buckets.get_mut(&addr) {
            if !b.allow(&lim, n, now) {
                refused = Some(if now - b.last_log > 10.0 {
                    b.last_log = now;
                    Some(b.dropped)
                } else {
                    None
                });
            }
        }
        if let Some(log_dropped) = refused {
            self.count("rate-limited");
            if let Some(d) = log_dropped {
                dlog!(
                    "UDP-DROP rate limit",
                    PyAddr(addr),
                    format!("{} dropped so far (> {:.0} pkt/s or {:.0} MB/s)", d, lim.pps, lim.bps / 1e6)
                );
            }
            return;
        }
        let op: i16 = if n > 0 { data[0] as i16 } else { -1 };
        bump(&mut self.udp_stats, (addr, op));
        if n >= 5 && op == 0x03 {
            let pong = [0x04, data[1], data[2], data[3], data[4]];
            if let Err(e) = net.send(&pong, addr) {
                self.send_error(e, addr);
                return;
            }
            if self.seen_once(Seen::Addr(addr, 0x03), 2) {
                dlog!("UDP", PyAddr(addr), format!("03-keepalive {}B", n), hex_sp(&data[..n.min(16)]));
            }
            self.relay_table(Table::Ka, data, addr, now, net);
        } else if n >= 6 && op == 0x00 {
            let sid = u32::from_le_bytes([data[1], data[2], data[3], data[4]]);
            let slot = data[5];
            let cur = self.slots.get(&(sid, slot)).map(|v| v.0);
            let first = cur != Some(addr);
            if first && cur.is_none() {
                self.relay_room(now);
            }
            self.slots.insert((sid, slot), (addr, now));
            if let Err(e) = net.send(&[0x01], addr) {
                self.send_error(e, addr);
                return;
            }
            if first {
                dlog!("RELAY-HELLO", PyAddr(addr), format!("sid={:08x} slot={}", sid, slot));
                evt!("relay-hello", format!("{:08x}", sid), "slot", slot, PyAddr(addr));
            }
        } else if n >= 7 && op == 0x02 {
            let sid = u32::from_le_bytes([data[1], data[2], data[3], data[4]]);
            let (src, dst) = (data[5], data[6]);
            if let Some((cur, ts)) = self.slots.get(&(sid, src)).copied() {
                if cur != addr && now - ts < REBIND_S && cur.ip() != addr.ip() {
                    let c = self.count("hijack-refused");
                    if matches!(c, 1 | 10 | 100 | 1000) {
                        dlog!(
                            "RELAY slot claim refused",
                            PyAddr(addr),
                            format!("sid={:08x} slot={} bound to {} (#{})", sid, src, PyAddr(cur), c)
                        );
                    }
                    return;
                }
            }
            if !self.slots.contains_key(&(sid, src)) {
                self.relay_room(now);
            }
            self.slots.insert((sid, src), (addr, now));
            bump(&mut self.relay_stats, (sid, src, dst));
            if n > 13 && &data[7..11] == b"T2RO" {
                bump(&mut self.relay_types, (src, dst, data[13], n));
            }
            let mut targets = std::mem::take(&mut self.targets);
            targets.clear();
            if dst == 0xFF {
                targets.extend(self.slots.iter().filter(|((sd, sl), _)| *sd == sid && *sl != src).map(|(_, v)| v.0));
            } else if let Some((t, _)) = self.slots.get(&(sid, dst)) {
                targets.push(*t);
            }
            let none = targets.is_empty();
            self.targets = targets;
            if none {
                self.count("no-target");
            }
            self.emit_targets(data, net);
            if self.seen_once(Seen::Relay(sid, src, dst), 3) {
                dlog!(
                    "RELAY-DATA",
                    PyAddr(addr),
                    format!("sid={:08x} {}->{} {}B", sid, src, dst, n),
                    if none { "no-target" } else { "" },
                    hex_sp(&data[..n.min(24)])
                );
            }
        } else {
            if self.seen_once(Seen::Addr(addr, op), 5) {
                dlog!(
                    "GAMEPLAY",
                    PyAddr(addr),
                    format!("op=0x{:02x} {}B", data.first().copied().unwrap_or(0), n),
                    hex_sp(&data[..n.min(24)])
                );
            }
            if n > 0 {
                self.relay_table(Table::Gp, data, addr, now, net);
            } else {
                self.count("malformed");
            }
        }
    }

    pub fn health_line(&mut self) -> String {
        let n: u64 = self.sizes.iter().sum();
        let hist = SIZE_KEYS
            .iter()
            .zip(self.sizes.iter())
            .filter(|(_, c)| **c > 0)
            .map(|(k, c)| format!("{}:{}", k, c))
            .collect::<Vec<_>>()
            .join(" ");
        let peers = |m: &IndexMap<SocketAddrV4, u64>| {
            most_common(m, 6).iter().map(|(a, c)| format!("{}:{}:{}", a.ip(), a.port(), c)).collect::<Vec<_>>().join(", ")
        };
        let top_in = peers(&self.pk_in);
        let top_out = peers(&self.pk_out);
        let dash = |s: String| if s.is_empty() { "-".to_string() } else { s };
        let line = format!(
            "in {} pkts | sizes {} | slowest handle {:.2} ms | in/peer {} | out/peer {} | {}",
            n,
            dash(hist),
            self.max_handle_ms,
            dash(top_in),
            dash(top_out),
            self.sock_stats
        );
        self.sizes = [0; 6];
        self.pk_in.clear();
        self.pk_out.clear();
        self.max_handle_ms = 0.0;
        line
    }

    pub fn status_health(&mut self) {
        if !self.pk_in.is_empty() {
            let l = self.health_line();
            dlog!("UDP-HEALTH", l);
        }
        if !self.counters.is_empty() {
            let d = self.counters.iter().map(|(k, v)| format!("'{}': {}", k, v)).collect::<Vec<_>>().join(", ");
            dlog!("UDP-DROPS", format!("{{{}}}", d));
            self.counters.clear();
        }
    }

    pub fn status_stats(&mut self) {
        if !self.udp_stats.is_empty() {
            let snap = take(&mut self.udp_stats);
            let l = stats_line(&snap, |(a, op), c| {
                let op = if *op < 0 { format!("{}", op) } else { format!("{:02x}", op) };
                format!("{}:{} op0x{}:{}", a.ip(), a.port(), op, c)
            });
            dlog!("UDP-STATS", l);
        }
        if !self.relay_stats.is_empty() {
            let snap = take(&mut self.relay_stats);
            let l = stats_line(&snap, |(sid, a, b), c| format!("{:08x} {}->{}:{}", sid, a, b, c));
            dlog!("RELAY-STATS", l);
        }
        if !self.relay_types.is_empty() {
            let snap = take(&mut self.relay_types);
            let l = stats_line(&snap, |(a, b, t, n), c| format!("{}->{} t{:02x} {}B:{}", a, b, t, n, c));
            dlog!("RELAY-TYPES", l);
        }
    }
}
