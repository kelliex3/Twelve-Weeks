//! Wire helpers: `[u16 len LE][op][payload]` frames (len counts op + payload), hex, readable previews.

pub fn frame(op: u8, payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(payload.len() + 3);
    v.extend_from_slice(&((payload.len() as u16 + 1).to_le_bytes()));
    v.push(op);
    v.extend_from_slice(payload);
    v
}

/// Cuts complete frames off the front of a stream (a zero length is skipped, as the daemon does).
#[derive(Default)]
pub struct Splitter {
    buf: Vec<u8>,
}

impl Splitter {
    pub fn push(&mut self, data: &[u8]) -> Vec<(u8, Vec<u8>)> {
        self.buf.extend_from_slice(data);
        let mut out = Vec::new();
        let mut i = 0usize;
        while self.buf.len() - i >= 2 {
            let ln = u16::from_le_bytes([self.buf[i], self.buf[i + 1]]) as usize;
            if ln == 0 {
                i += 2;
                continue;
            }
            if self.buf.len() - i < ln + 2 {
                break;
            }
            out.push((self.buf[i + 2], self.buf[i + 3..i + 2 + ln].to_vec()));
            i += 2 + ln;
        }
        self.buf.drain(..i);
        out
    }
}

pub fn hex(b: &[u8]) -> String {
    let mut s = String::with_capacity(b.len() * 2);
    for x in b {
        s.push_str(&format!("{:02x}", x));
    }
    s
}

/// Printable ASCII rendering (non-printable bytes become '.'), for the human column of a transcript line.
pub fn ascii(b: &[u8]) -> String {
    b.iter().map(|&c| if (0x20..0x7f).contains(&c) { c as char } else { '.' }).collect()
}

pub fn pad(s: &str, n: usize) -> Vec<u8> {
    let mut v = s.as_bytes().to_vec();
    v.truncate(n);
    v.resize(n, 0);
    v
}

pub fn op_name(op: u8) -> &'static str {
    match op {
        0x01 => "find-match",
        0x02 => "leave",
        0x03 => "searching",
        0x04 => "match-record",
        0x05 => "keepalive",
        0x08 => "lists",
        0x09 => "loaded",
        0x0a => "game-start",
        0x0c => "countdown",
        0x0d => "close",
        0x0e => "announce",
        0x0f => "login",
        0x10 => "name",
        0x11 => "note",
        0x12 => "match-end",
        0x13 => "leaderboard-req",
        0x14 => "leaderboard",
        0x15 => "poll",
        0x16 => "online",
        0x17 => "search-list",
        0xa1 => "attest",
        0xa3 => "attest-reply",
        0xa9 | 0xaa | 0xab => "achievements",
        0xb0 => "version",
        0xb1 => "version-reply",
        0xb2 => "hwid",
        0xb3 => "hwid-reply",
        0xb4 => "feed",
        0xb5 => "feed-reply",
        0xb6 => "clan-page",
        0xb7 => "clan-page-reply",
        0xba => "notices",
        0xbb => "notices-reply",
        0xbc => "mode-queue",
        0xbd => "mode-queue-reply",
        0xbf => "rank-table",
        0xc8 => "sp-maps",
        0xc9 => "sp-maps-reply",
        0xca => "perf",
        0xcb => "perf-reply",
        0xd0 => "name-read",
        0xd1 => "name-reply",
        0xd2 => "name-set",
        0xd8 => "social",
        0xd9 => "social-reply",
        0xda => "party-old",
        0xdb => "party-old-reply",
        0xdc => "interrogation",
        0xdd => "interrogation-reply",
        0xde => "gun-limits",
        0xdf => "gun-limits-reply",
        0xe0 => "clan-read",
        0xe1 => "clan-reply",
        0xe2 => "clan-report",
        0xe3 => "roster-clock",
        0xe4 => "clan-side",
        0xe5 => "roster",
        0xe6 => "host-migration",
        0xe7 => "host-migration-reply",
        0xe8 => "boosters",
        0xe9 => "boosters-reply",
        0xea => "vote-read",
        0xeb => "vote-state",
        0xec => "vote-cast",
        0xee => "booster-pick",
        0xef => "mission-pick",
        0xf0 => "helper-register",
        0xf1 => "helper-ack",
        0xf2 => "p2p-slots",
        0xf3 => "helper-keepalive",
        0xf4 => "ping-location",
        0xf5 => "ping-location-fwd",
        0xf6 => "ping-ms",
        0xf8 => "private",
        0xf9 => "private-reply",
        _ => "?",
    }
}
