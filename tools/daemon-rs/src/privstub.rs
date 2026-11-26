//! Public build: neutral stand-ins for the private hooks (identity notes, hardware / IP bans, install check, anti-cheat ops).
//! The real implementations are not part of this repository; the daemon calls the same names either way (`crate::private`).
//! Here no ban or check ever fires and the ops answer "not available".
use crate::d::*;
use indexmap::IndexMap;
use std::collections::HashMap;
use std::io::{BufRead, Write};

/// width (hex chars) of a hardware id entry in bans.txt
pub const HWID_HEX: usize = 24;
/// anti-cheat finding kind: a clan report for a match the token was not in
pub const S_ORPHAN: i64 = 104;

/// the match id argument of `ac_server_event`: a `&str`, a `&String` or an `Option` of either
pub trait MidArg {
    fn mid(&self) -> Option<&str>;
}
impl MidArg for &str {
    fn mid(&self) -> Option<&str> {
        Some(self)
    }
}
impl MidArg for &String {
    fn mid(&self) -> Option<&str> {
        Some(self.as_str())
    }
}
impl MidArg for Option<&str> {
    fn mid(&self) -> Option<&str> {
        *self
    }
}
impl MidArg for Option<&String> {
    fn mid(&self) -> Option<&str> {
        self.map(|s| s.as_str())
    }
}

/// the anti-cheat store of the private build; here there is none (`AC` is `None`)
pub struct AcStub;
impl AcStub {
    pub fn rate_ok(&self, _token: &str) -> bool {
        true
    }
    pub fn first_time(&self, _token: &str, _mid: &str, _parts: i64, _counts: &IndexMap<i64, i64>) -> bool {
        false
    }
    /// -> (parts, counts, issues = [(kind, weight, evidence)])
    pub fn check_report(&self, _token: &str, _mid: &str, parts: i64, counts: IndexMap<i64, i64>, _flags: i64) -> (i64, IndexMap<i64, i64>, Vec<(i64, f64, String)>) {
        (parts, counts, Vec::new())
    }
    pub fn note_report(&self, _token: &str, _mid: &str, _parts: i64, _counts: &IndexMap<i64, i64>) -> Vec<(i64, f64, String)> {
        Vec::new()
    }
    pub fn match_started(&self, _mid: &str, _slots: &[String], _fresh: &[String], _names: &HashMap<String, String>, _sids: &HashMap<String, u64>) {}
    pub fn match_ended(&self, _mid: &str) {}
}
pub static AC: Option<AcStub> = None;

/// `ac_server_event(token, mid, kind, w, evidence)`
pub fn ac_server_event(_token: &str, _mid: impl MidArg, _kind: i64, _w: f64, _evidence: &str) {}
/// a login's address check: `Some((kind, weight, evidence))` = a shared token
pub fn ac_note_login_ip(_token: &str, _ip: &str) -> Option<(i64, f64, String)> {
    None
}
/// the four anti-cheat ops (0xCC / 0xCE / 0xD4 / 0xD6): the reply, None = nothing to send
pub fn ac_op(_op: u8, _pl: &[u8]) -> Option<Vec<u8>> {
    None
}
/// every status tick
pub fn ac_tick() {}
/// the hardware id report (op 0xB2): the reply
pub fn hwid_op(_pl: &[u8], _ip: Option<&str>) -> Vec<u8> {
    msg(0xB3, b"ok=0")
}
/// the install file report (op 0xA1): the reply
pub fn attest_op(_pl: &[u8], _addr: &Addr) -> Vec<u8> {
    msg(0xA3, b"ok=0")
}
/// `None` = this login / search goes on; else (reason for the log, op 0x11 note bytes). `boot` = the boot-handshake call
pub fn install_refusal(_token: &str, _ip: Option<&str>, _boot: bool) -> Option<(String, Vec<u8>)> {
    None
}
/// the identity id of a login (None = not recorded)
pub fn ident_note(_token: &str, _steamid: u64, _ip: Option<&str>) -> Option<String> {
    None
}
pub fn ident_pid(_token: &str) -> Option<String> {
    None
}
/// once at start
pub fn ident_backfill() {}
/// the ban list lookups: `ip_banned` = the entry that covers the address
pub fn is_banned(_token: &str, _name: Option<&str>, _ip: Option<&str>) -> bool {
    false
}
pub fn ip_banned(_ip: &str) -> Option<String> {
    None
}
/// `t2r-daemon selftest kp` (private build only)
pub fn selftest(_input: &mut dyn BufRead, _out: &mut dyn Write) -> i32 {
    eprintln!("selftest kp: private build only");
    2
}
