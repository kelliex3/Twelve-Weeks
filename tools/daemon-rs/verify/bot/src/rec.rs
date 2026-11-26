//! Transcript recorder. Two sections, both independent of thread timing:
//!   script  = what the (single) scenario thread did, in order: actions, RPC requests and replies, admin replies, snapshots;
//!   streams = per bot and per connection, every frame that arrived (pushes, boot replies), in arrival order. Streams are
//!             printed sorted by (bot, connection), so the interleaving of different connections never shows.
//! Timing is recorded as a bucket of the gap to the previous frame of the same stream, never as a time.
use crate::proto::{ascii, hex, op_name};
use std::collections::BTreeMap;
use std::sync::Mutex;
use std::time::Instant;

#[derive(Default)]
struct Stream {
    lines: Vec<String>,
    last: Option<(Instant, u8)>,
    /// a countdown step (0x0c) has been seen on this stream
    stepped: bool,
}

#[derive(Default)]
struct Inner {
    script: Vec<String>,
    streams: BTreeMap<(String, String), Stream>,
}

#[derive(Default)]
pub struct Rec {
    inner: Mutex<Inner>,
}

/// Gap buckets: 0 < 0.5 s, 1 < 1.7 s (1 s steps, 1.5 s polls), 2 < 3.5 s (the 2 s pause), 3 < 12 s, 4 more.
pub fn bucket(ms: u128) -> u8 {
    match ms {
        0..=499 => 0,
        500..=1699 => 1,
        1700..=3499 => 2,
        3500..=11999 => 3,
        _ => 4,
    }
}

impl Rec {
    pub fn script(&self, line: impl Into<String>) {
        self.inner.lock().unwrap().script.push(line.into());
    }

    pub fn frame(&self, bot: &str, stream: &str, dir: &str, op: u8, payload: &[u8]) {
        let mut g = self.inner.lock().unwrap();
        let s = g.streams.entry((bot.to_string(), stream.to_string())).or_default();
        let now = Instant::now();
        // a gap is only meaningful between frames of the same kind (countdown steps are 1 s apart; the first step after the match record is
        // 0.5 - 2 s, it depends on when the keepalive that re-sends the record came): b? = no bucket
        let b = match s.last {
            // (the match record is sent again by the first keepalive after the start: when that comes is the poll phase of the client)
            Some((_, lop)) if lop == op && op == 0x04 => "b?".to_string(),
            Some((t, lop)) if lop == op => format!("b{}", bucket(now.duration_since(t).as_millis())),
            Some(_) => "b?".to_string(),
            None => "b-".to_string(),
        };
        s.last = Some((now, op));
        // The first two match records (the start push and the keepalive re-send) carry the PLACEHOLDER map (first vote option). A keepalive that
        // lands between "match formed" and "vote opened" gets a different placeholder (the daemon picks one then): that race is not stable, so
        // byte 0x16 of such a record is masked; the vote options are in the 0xEB state and the final record (after the vote) is kept as it is.
        let mut payload = payload.to_vec();
        if op == 0x0c {
            s.stepped = true;
        }
        let masked = op == 0x04 && !s.stepped && payload.len() > 0x16;
        if masked {
            payload[0x16] = 0xee;
        }
        let payload = &payload[..];
        s.lines.push(format!("{} 0x{:02x} {} len={} {} ; {}{} | {}", dir, op, op_name(op), payload.len(), hex(payload), ascii(payload), if masked { " (placeholder map masked: byte 0x16 = ee)" } else { "" }, b));
    }

    pub fn note(&self, bot: &str, stream: &str, text: &str) {
        let mut g = self.inner.lock().unwrap();
        let s = g.streams.entry((bot.to_string(), stream.to_string())).or_default();
        s.lines.push(format!("-- {}", text));
    }

    pub fn render(&self) -> String {
        let g = self.inner.lock().unwrap();
        let mut out = String::new();
        out.push_str("##### SCRIPT\n");
        for l in &g.script {
            out.push_str(l);
            out.push('\n');
        }
        out.push_str("##### STREAMS\n");
        for ((bot, st), s) in &g.streams {
            out.push_str(&format!("== {} {}\n", bot, st));
            for l in &s.lines {
                out.push_str(l);
                out.push('\n');
            }
        }
        out
    }
}
