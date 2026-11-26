// TEMPORARY: removed at integration. Stubs of the items of OTHER chunks that the K1 (base / ops) chunk calls, with the signatures it expects.
// Bodies are `unimplemented!()` (the K1 selftest swaps the few it needs for scripted tables, see k1_selftest.rs).
#![allow(dead_code, unused_variables)]
use super::*;
use indexmap::IndexMap;
use parking_lot::Mutex;
use std::collections::{HashMap, HashSet};
use std::sync::LazyLock;

// ---- KP private: daemon.py:1038 HWID_HEX (the width of a hardware id entry in bans.txt)
pub const HWID_HEX: usize = 24;

// ---- K3 sess: languages, texts, versions, sessions, per-match dictionaries
/// `CLIENT_LANGS.get(token, "en")`
pub fn lang_of(token: &str) -> String {
    unimplemented!()
}
/// `note_lang(token, lang)` (lang = the raw byte of the request)
pub fn note_lang(token: &str, lang: u8) {
    // (K2 selftest: no-op until K3 lands)
}
/// `srv_text(key, lang, **kw)`: kw = [(name, value)]
pub fn srv_text(key: &str, lang: &str, kw: &[(&str, String)]) -> String {
    unimplemented!()
}
/// `srv_note(key, token)`: the op 0x11 note bytes of a text for this token's language
pub fn srv_note(key: &str, token: &str) -> Vec<u8> {
    unimplemented!()
}
/// `CLIENT_VERSIONS.get(token)`
pub fn client_version(token: &str) -> Option<String> {
    unimplemented!()
}
/// `SERVER_VERSION` ("" = unknown)
pub fn server_version() -> String {
    "sv-test".to_string() // (stub value: the K1 selftest sets the same in daemon.py)
}
/// `version_refusal(token)`: the note a NEW search is refused with
pub fn version_refusal(token: &str) -> Option<String> {
    unimplemented!()
}
impl Sessions {
    /// `list(SESSIONS.by_token.items())`
    pub fn conns(&self) -> Vec<(String, Conn)> {
        unimplemented!()
    }
}
/// daemon.py:3088 SESS_LOST: token -> (time the session conn was lost, match id)
pub static SESS_LOST: LazyLock<Mutex<HashMap<String, (f64, String)>>> = LazyLock::new(|| Mutex::new(HashMap::new()));
/// `VOTES.pop(key) (under _vote_lock); MATCH_MAP.pop(key); TIMING.pop(key); MATCH_P2P.pop(key); SID_KEEP.pop(key)`
pub fn match_dicts_pop(key: &Key) {
    TEST_MM.lock().remove(key);
}
/// `MAPS`: [(name, image, level)]
pub fn maps_table() -> Vec<(String, String, String)> {
    unimplemented!()
}
/// `RESPAWNS.get(level, 0)`
pub fn respawns_of(level: &str) -> u8 {
    unimplemented!()
}
impl PresStub {
    /// `[p.get("name", "PLAYER") for p in PRES.online()]`
    pub fn online_names(&self) -> Vec<String> {
        unimplemented!()
    }
}

// ---- K6 loop
/// `live_save(now=None)` (daemon.py:9741)
pub fn live_save(now: Option<f64>) {
    unimplemented!()
}
/// `mid in RESTORED_T` (daemon.py:9895)
pub fn restored_t_has(mid: &str) -> bool {
    false
}
