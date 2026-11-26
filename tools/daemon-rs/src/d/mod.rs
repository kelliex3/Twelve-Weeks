//! daemon.py, split by area. Every section file starts with `use super::*;` and every public item is re-exported here, so the
//! sections call each other by the Python names (as the one Python namespace did).
#![allow(unused_imports)]
pub use crate::{dlog, evt};
// KP: the private hooks (identity / hardware / bans / install check / anti-cheat): crate::private = private/ (feature "private") or the neutral src/privstub.rs
pub use crate::private::{ac_note_login_ip, ac_op, ac_server_event, ac_tick, attest_op, hwid_op, ident_backfill, ident_note, ident_pid, install_refusal, ip_banned, is_banned, MidArg, AC, HWID_HEX, S_ORPHAN};
pub mod db;
pub mod conn;
pub use conn::*;
pub mod ext_core;
pub use ext_core::*;
pub mod ext_k4;
pub use ext_k4::*;
pub mod ext_k1;
pub use ext_k1::*;
pub mod ext_k2;
pub use ext_k2::*;
pub mod clan;
pub use clan::*;
pub mod clan_ops;
pub use clan_ops::*;
pub mod ext_k3;
pub use ext_k3::*;
pub mod base;
pub use base::*;
pub mod ops;
pub use ops::*;
pub mod mmlock;
pub use mmlock::*;
pub mod mm;
pub use mm::*;
pub mod teams;
pub use teams::*;
pub mod jip;
pub use jip::*;
pub mod texts;
pub mod sess;
pub use sess::*;
pub mod maps;
pub use maps::*;
pub mod flow;
pub use flow::*;
pub mod ping;
pub use ping::*;
pub mod k4_selftest;
pub mod k1_selftest;
pub mod k2_selftest;
pub mod k3_selftest;
