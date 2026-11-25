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
#[allow(unused_imports)]
use HashMap as _H;
