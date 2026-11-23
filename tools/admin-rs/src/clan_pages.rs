use serde_json::{json, Value};

use crate::store::Admin;

pub fn view(_admin: &Admin, _key: &str, _name: &str, _lang: &str) -> Value {
    json!({"ok": false, "error": "clan view not available yet (waiting for the t2r_clan crate)"})
}
