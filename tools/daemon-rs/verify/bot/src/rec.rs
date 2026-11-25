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
            Some((t, lop)) if lop == op => format!("b{}", bucket(now.duration_since(t).as_millis())),
            Some(_) => "b?".to_string(),
            None => "b-".to_string(),
        };
        s.last = Some((now, op));
        s.lines.push(format!("{} 0x{:02x} {} len={} {} ; {} | {}", dir, op, op_name(op), payload.len(), hex(payload), ascii(payload), b));
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
