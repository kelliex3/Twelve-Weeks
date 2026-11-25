use serde_json::{json, Value};

use crate::store::Admin;
use crate::timeutil;

pub fn view(admin: &Admin, key: &str, name: &str, lang: &str) -> Value {
    let c = match admin.connect() {
        Ok(c) => c,
        Err(e) => return json!({"ok": false, "error": format!("database unavailable: {}", e)}),
    };
    let now = timeutil::now();
    json!({"ok": true, "name": name, "clan": t2r_clan::clan_view(&c, "", Some(key)),
           "feed": t2r_clan::admin_feed(&c, key, lang, now)})
}
