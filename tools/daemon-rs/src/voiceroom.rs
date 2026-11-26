//! Lobby voice rooms (the Rust version of voiceroom.py). The daemon carries no audio: it only tells each player's Steam helper who is in
//! its voice room. Room of a token = its party (2+ members with a helper), else its requeue group, else none; at most CAP members.
//! Frame to a helper: 0xF7 [u32 room id][u8 n][n x (u64 steamid LE, u8 name len, name utf-8)], n = 0 = "no room".
use indexmap::IndexMap;
use std::collections::{HashMap, HashSet};

pub const CAP: usize = 8;
pub const OP: u8 = 0xF7;

/// A room key: ("p", party id) or ("r", match id).
#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub struct RoomKey(pub char, pub String);

pub type RoomMap = IndexMap<String, (RoomKey, Vec<String>)>;

// py: voiceroom.py:24 rooms
/// eligible: tokens with a live helper that are not in a match. party_pid(t) -> party id; party_members(t) -> [tokens];
/// rq_groups: {match id: [tokens]} (open requeue groups). Returns {token: (room key, [member tokens sorted, <= CAP])} for every token in a room of 2+.
pub fn rooms(
    eligible: &HashSet<String>,
    party_pid: &dyn Fn(&str) -> Option<String>,
    party_members: &dyn Fn(&str) -> Vec<String>,
    rq_groups: &IndexMap<String, Vec<String>>,
) -> RoomMap {
    let mut out: RoomMap = IndexMap::new();
    let mut done: HashSet<String> = HashSet::new();
    let mut el: Vec<&String> = eligible.iter().collect();
    el.sort();
    for t in el {
        if done.contains(t) {
            continue;
        }
        let Some(pid) = party_pid(t) else { continue };
        let mut mem: Vec<String> = party_members(t).into_iter().filter(|m| eligible.contains(m)).collect();
        mem.sort();
        for m in &mem {
            done.insert(m.clone());
        }
        mem.truncate(CAP);
        if mem.len() >= 2 {
            for m in &mem {
                out.insert(m.clone(), (RoomKey('p', pid.clone()), mem.clone()));
            }
        }
    }
    let mut mids: Vec<&String> = rq_groups.keys().collect();
    mids.sort();
    for mid in mids {
        let set: HashSet<String> = rq_groups[mid].iter().filter(|m| eligible.contains(*m) && !out.contains_key(*m)).cloned().collect();
        let mut mem: Vec<String> = set.into_iter().collect();
        mem.sort();
        mem.truncate(CAP);
        if mem.len() >= 2 {
            for m in &mem {
                if !out.contains_key(m) {
                    out.insert(m.clone(), (RoomKey('r', mid.clone()), mem.clone()));
                }
            }
        }
    }
    out
}

/// `zlib.crc32`
fn crc32(b: &[u8]) -> u32 {
    let mut c = 0xFFFF_FFFFu32;
    for &x in b {
        c ^= x as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { (c >> 1) ^ 0xEDB8_8320 } else { c >> 1 };
        }
    }
    !c
}

// py: voiceroom.py:51 room_id
pub fn room_id(key: &RoomKey) -> u32 {
    crc32(format!("({}, {})", crate::py::repr_str(&key.0.to_string()), crate::py::repr_str(&key.1)).as_bytes())
}

// py: voiceroom.py:55 payload
/// 0xF7 payload for one room (the same bytes for every member). `key` = None only with no members.
pub fn payload(key: Option<&RoomKey>, members: &[String], steamid: &dyn Fn(&str) -> u64, name: &dyn Fn(&str) -> String) -> Vec<u8> {
    if members.is_empty() {
        return vec![0; 5];
    }
    let mut b: Vec<u8> = key.map(room_id).unwrap_or(0).to_le_bytes().to_vec();
    b.push(members.len() as u8);
    for t in members {
        let n = name(t);
        let full = if n.is_empty() { "PLAYER".to_string() } else { n };
        let nb = full.as_bytes();
        let nb = &nb[..nb.len().min(24)];
        b.extend_from_slice(&steamid(t).to_le_bytes());
        b.push(nb.len() as u8);
        b.extend_from_slice(nb);
    }
    b
}

// py: voiceroom.py:68 Pusher
/// Keeps the last payload sent to each helper connection; tick() returns [(token, conn, payload)] to send now.
#[derive(Default)]
pub struct Pusher {
    /// token -> (id of the connection, payload)
    pub last: HashMap<String, (u64, Vec<u8>)>,
}

impl Pusher {
    // py: voiceroom.py:71 Pusher.__init__
    pub fn new() -> Pusher {
        Pusher::default()
    }

    // py: voiceroom.py:74 Pusher.tick
    /// helpers: {token: connection id} (every live helper, in the daemon's order). Returns the frames to send (changes only).
    pub fn tick(&mut self, helpers: &IndexMap<String, u64>, room_of: &RoomMap, steamid: &dyn Fn(&str) -> u64, name: &dyn Fn(&str) -> String) -> Vec<(String, u64, Vec<u8>)> {
        let mut out = Vec::new();
        for (t, &c) in helpers {
            let r = room_of.get(t);
            let pl = match r {
                Some((k, m)) => payload(Some(k), m, steamid, name),
                None => payload(None, &[], steamid, name),
            };
            let prev = self.last.get(t);
            if prev.is_none() && r.is_none() {
                self.last.insert(t.clone(), (c, pl)); // a helper that never had a room: nothing to clear
                continue;
            }
            if let Some(p) = prev {
                if p.0 == c && p.1 == pl {
                    continue;
                }
                if p.0 != c && r.is_none() {
                    self.last.insert(t.clone(), (c, pl)); // reconnected helper without a room: its state is empty already
                    continue;
                }
            }
            self.last.insert(t.clone(), (c, pl.clone()));
            out.push((t.clone(), c, pl));
        }
        self.last.retain(|t, _| helpers.contains_key(t));
        out
    }
}

// ---------------------------------------------------------------- differential test
fn str_list(v: Option<&serde_json::Value>) -> Vec<String> {
    v.and_then(|x| x.as_array()).map(|a| a.iter().filter_map(|x| x.as_str().map(|s| s.to_string())).collect()).unwrap_or_default()
}

/// `t2r-daemon selftest voiceroom`: the Rust side of verify/py/voiceroom_harness.py.
pub fn selftest(input: &mut dyn std::io::BufRead, out: &mut dyn std::io::Write) -> i32 {
    use crate::c_util::{js, jstrs};
    use serde_json::{json, Value};
    let mut push = Pusher::new();
    let mut conns: Vec<String> = Vec::new(); // connection names; the index + 1 is the id
    let hexs = |b: &[u8]| b.iter().map(|x| format!("{:02x}", x)).collect::<String>();
    for (i, c) in crate::c_util::script(input).iter().enumerate() {
        let eligible: HashSet<String> = jstrs(c, "eligible").into_iter().collect();
        let pids: HashMap<String, String> = c
            .get("pid")
            .and_then(|x| x.as_object())
            .map(|o| o.iter().filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string()))).collect())
            .unwrap_or_default();
        let groups: HashMap<String, Vec<String>> = c.get("groups").and_then(|x| x.as_object()).map(|o| o.iter().map(|(k, v)| (k.clone(), str_list(Some(v)))).collect()).unwrap_or_default();
        let rq: IndexMap<String, Vec<String>> = c.get("rq").and_then(|x| x.as_object()).map(|o| o.iter().map(|(k, v)| (k.clone(), str_list(Some(v)))).collect()).unwrap_or_default();
        let sids: HashMap<String, u64> = c.get("sid").and_then(|x| x.as_object()).map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_u64().unwrap_or(0))).collect()).unwrap_or_default();
        let names: HashMap<String, String> =
            c.get("names").and_then(|x| x.as_object()).map(|o| o.iter().map(|(k, v)| (k.clone(), v.as_str().unwrap_or("").to_string())).collect()).unwrap_or_default();
        let party_pid = |t: &str| pids.get(t).cloned();
        let party_members = |t: &str| pids.get(t).and_then(|p| groups.get(p)).cloned().unwrap_or_default();
        let steamid = |t: &str| sids.get(t).copied().unwrap_or(0);
        let name = |t: &str| names.get(t).cloned().unwrap_or_default();
        let room_json = |m: &RoomMap| -> Value { Value::Array(m.iter().map(|(t, (k, mem))| json!([t, k.0.to_string(), k.1, room_id(k), mem])).collect()) };
        let r: Value = match js(c, "op") {
            "rooms" => room_json(&rooms(&eligible, &party_pid, &party_members, &rq)),
            "payload" => {
                let key = c.get("key").and_then(|k| k.as_array()).map(|a| RoomKey(a[0].as_str().unwrap_or("p").chars().next().unwrap_or('p'), a[1].as_str().unwrap_or("").to_string()));
                json!(hexs(&payload(key.as_ref(), &jstrs(c, "members"), &steamid, &name)))
            }
            "tick" => {
                let mut helpers: IndexMap<String, u64> = IndexMap::new();
                if let Some(a) = c.get("helpers").and_then(|x| x.as_array()) {
                    for p in a {
                        let (Some(t), Some(cn)) = (p.get(0).and_then(|x| x.as_str()), p.get(1).and_then(|x| x.as_str())) else { continue };
                        let id = match conns.iter().position(|x| x == cn) {
                            Some(k) => k + 1,
                            None => {
                                conns.push(cn.to_string());
                                conns.len()
                            }
                        };
                        helpers.insert(t.to_string(), id as u64);
                    }
                }
                let rm = rooms(&eligible, &party_pid, &party_members, &rq);
                let frames = push.tick(&helpers, &rm, &steamid, &name);
                Value::Array(frames.into_iter().map(|(t, cid, pl)| json!([t, conns[cid as usize - 1], hexs(&pl)])).collect())
            }
            _ => json!("?"),
        };
        let mut last: Vec<(&String, &(u64, Vec<u8>))> = push.last.iter().collect();
        last.sort_by(|a, b| a.0.cmp(b.0));
        let s = json!(last.into_iter().map(|(t, (cid, pl))| json!([t, conns[*cid as usize - 1], hexs(pl)])).collect::<Vec<_>>());
        crate::c_util::emit(out, i, r, s);
    }
    0
}
