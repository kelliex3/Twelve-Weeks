//! Piracy talk escalation, ported from piracy_automod.py (record_strike) and the bot's on_automod_action.
//! Discord's own "TW piracy talk" rule blocks the message and applies the first 10 minute timeout; on a repeat
//! within a week this raises it to 30 minutes, then 1 hour. Strikes live in piracy_strikes.json (same format as Python).
use crate::bot::Bot;
use crate::log;
use crate::model::AutomodExecution;
use crate::util::{civil, now_secs, parse_id};
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::path::Path;
use std::sync::Arc;
use std::time::Duration;

pub const RULE_NAME: &str = "TW piracy talk";
pub const STEPS: [u64; 3] = [600, 1800, 3600];
pub const STRIKE_WINDOW: f64 = 7.0 * 86400.0;
pub const STRIKES_FILE: &str = "piracy_strikes.json";
const BLOCK_MESSAGE_ACTION: u8 = 1;

/// Log one hit for `user`; returns (hits within STRIKE_WINDOW incl. this one, timeout seconds for that hit).
pub fn record_strike(path: &Path, user: u64, now: f64) -> (usize, u64) {
    let mut data: BTreeMap<String, Vec<f64>> = std::fs::read(path).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
    for ts in data.values_mut() {
        ts.retain(|t| now - t < STRIKE_WINDOW);
    }
    data.retain(|_, ts| !ts.is_empty());
    let ts = data.entry(user.to_string()).or_default();
    ts.push(now);
    let n = ts.len();
    if let Err(e) = std::fs::write(path, serde_json::to_vec(&data).unwrap_or_default()) {
        log!("piracy strikes not saved: {e}");
    }
    (n, STEPS[n.min(STEPS.len()) - 1])
}

fn iso_z(secs: i64) -> String {
    let (y, mo, d, h, mi, s) = civil(secs);
    format!("{y:04}-{mo:02}-{d:02}T{h:02}:{mi:02}:{s:02}Z")
}

async fn rule_id(bot: &Bot, guild: u64) -> Result<Option<u64>, String> {
    if let Some(id) = *bot.piracy_rule.lock().unwrap_or_else(std::sync::PoisonError::into_inner) {
        return Ok(Some(id));
    }
    let rules = bot.rest.get(&format!("/guilds/{guild}/auto-moderation/rules")).await.map_err(|e| e.to_string())?;
    let id = rules
        .as_array()
        .and_then(|a| a.iter().find(|r| r.get("name").and_then(Value::as_str) == Some(RULE_NAME)))
        .and_then(|r| r.get("id"))
        .and_then(parse_id);
    *bot.piracy_rule.lock().unwrap_or_else(std::sync::PoisonError::into_inner) = id;
    Ok(id)
}

pub async fn on_execution(bot: Arc<Bot>, ex: AutomodExecution) {
    let guild = bot.cfg.guild_id;
    if ex.guild_id != guild || ex.action.kind != BLOCK_MESSAGE_ACTION {
        return;
    }
    let result: Result<(), String> = async {
        if rule_id(&bot, guild).await? != Some(ex.rule_id) {
            return Ok(());
        }
        let (n, secs) = record_strike(&bot.dir.join(STRIKES_FILE), ex.user_id, now_secs());
        log!("piracy automod: user {} hit {n} -> {secs} s", ex.user_id);
        if n < 2 {
            return Ok(());
        }
        tokio::time::sleep(Duration::from_secs(4)).await;
        let until = iso_z(now_secs() as i64 + secs as i64);
        let reason = format!("piracy talk, hit {n} within a week");
        bot.rest
            .patch(&format!("/guilds/{guild}/members/{}", ex.user_id), &json!({"communication_disabled_until": until}), Some(&reason))
            .await
            .map_err(|e| e.to_string())?;
        Ok(())
    }
    .await;
    if let Err(e) = result {
        log!("piracy automod: {e}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn steps_match_python() {
        let p = std::env::temp_dir().join(format!("tw_strikes_{}.json", std::process::id()));
        let _ = std::fs::remove_file(&p);
        let got: Vec<_> = (0..4)
            .map(|i| record_strike(&p, 1, 1000.0 + f64::from(i)))
            .chain([record_strike(&p, 2, 1000.0), record_strike(&p, 1, 1000.0 + STRIKE_WINDOW + 10.0)])
            .collect();
        assert_eq!(got, [(1, 600), (2, 1800), (3, 3600), (4, 3600), (1, 600), (1, 600)]);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn iso() {
        assert_eq!(iso_z(0), "1970-01-01T00:00:00Z");
    }
}
