//! Public build: stand-ins for the private admin-site parts (player reports). The real ones are not in this repository;
//! here the PLAYER REPORTS tab stays empty.
use std::path::{Path, PathBuf};

use serde_json::Value;

pub const AVAILABLE: bool = false;

pub fn ac_db(dbdir: &Path) -> PathBuf {
    dbdir.join("review").join("ac.db")
}

pub fn preports(_db: &Path, _limit: usize) -> Vec<Value> {
    Vec::new()
}

pub fn preports_against(_db: &Path, _tokens: &[String], _steamid: i64) -> Vec<Value> {
    Vec::new()
}

pub fn preport(_db: &Path, _id: i64) -> Option<Value> {
    None
}

pub fn demo_file(_db: &Path, _report_id: i64, _demo_id: i64) -> Option<(PathBuf, u64)> {
    None
}
