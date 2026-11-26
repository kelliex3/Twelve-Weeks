// TEMPORARY: removed at integration. Stubs of other chunks' items that src/d/conn.rs calls, so the core compiles on its own.
// A chunk agent that implements one of these deletes its stub here (in its own branch).
#![allow(dead_code, unused_variables)]
use super::*;

// K5 batch
pub fn handle_batch(conn: &Conn, addr: Addr, st: &mut ConnSt, batch: &mut Batch) -> HResult {
    unimplemented!()
}
