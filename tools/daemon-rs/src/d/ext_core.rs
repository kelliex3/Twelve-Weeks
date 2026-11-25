// TEMPORARY: removed at integration. Stubs of other chunks' items that src/d/conn.rs calls, so the core compiles on its own.
// A chunk agent that implements one of these deletes its stub here (in its own branch).
#![allow(dead_code, unused_variables)]
use super::*;
use std::collections::HashMap;
use std::sync::LazyLock;

// K5 batch
pub fn handle_batch(conn: &Conn, addr: Addr, st: &mut ConnSt, batch: &mut Batch) -> HResult {
    unimplemented!()
}
// K1 base
pub fn tag(token: &str) -> String {
    unimplemented!()
}
// K4 mm
pub struct MMState {
    pub queue: indexmap::IndexMap<String, f64>,
}
pub struct MatchMaker {
    pub lock: parking_lot::Mutex<MMState>,
}
impl MatchMaker {
    pub fn in_match(&self, token: &str) -> bool {
        unimplemented!()
    }
}
pub static MM: LazyLock<MatchMaker> = LazyLock::new(|| unimplemented!());
// K3 sess
pub struct Bridges {}
impl Bridges {
    pub fn unregister(&self, token: &str, conn: &Conn) -> bool {
        unimplemented!()
    }
}
pub static BRIDGES: LazyLock<Bridges> = LazyLock::new(|| unimplemented!());
pub struct Sessions {}
impl Sessions {
    pub fn unregister(&self, token: &str, conn: &Conn) {
        unimplemented!()
    }
}
pub static SESSIONS: LazyLock<Sessions> = LazyLock::new(|| unimplemented!());
pub fn note_session_lost(token: &str, conn: &Conn) {
    unimplemented!()
}
// K3 (the daemon's hostmig.HostMigration instance; methods come from src/hostmig.rs)
pub struct HmStub {}
impl HmStub {
    pub fn enabled(&self) -> bool {
        unimplemented!()
    }
    pub fn has(&self, token: &str) -> bool {
        unimplemented!()
    }
    pub fn bridge_gone(&self, token: &str) {
        unimplemented!()
    }
}
pub static HM: LazyLock<HmStub> = LazyLock::new(|| unimplemented!());
#[allow(unused_imports)]
use HashMap as _H;
