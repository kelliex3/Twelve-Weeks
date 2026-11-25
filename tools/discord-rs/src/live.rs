use crate::bot::Bot;
use crate::cache::{CATEGORY, VOICE};
use crate::config::LiveCfg;
use crate::log;
use crate::model::Overwrite;
use crate::util::{now_secs, read_json, truncate_chars, write_json};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::Path;
use std::sync::Arc;
use std::time::{Duration, Instant};

const STATE: &str = "live_channels.json";
const STATUS_STALE_S: f64 = 60.0;
const LAUNCHERS_STALE_S: f64 = 90.0;
const TICK: Duration = Duration::from_secs(20);
const EDIT_TIMEOUT: Duration = Duration::from_secs(20);

const VIEW: u64 = 1 << 10;
const SEND: u64 = 1 << 11;
const CONNECT: u64 = 1 << 20;
const SPEAK: u64 = 1 << 21;
const STREAM: u64 = 1 << 9;
const REACT: u64 = 1 << 6;
const INVITE: u64 = 1 << 0;
const MANAGE_CHANNELS: u64 = 1 << 4;

#[derive(Deserialize, Default)]
pub struct Status {
    #[serde(default)]
    ts: Option<f64>,
    #[serde(default)]
    players_online: Option<f64>,
    #[serde(default)]
    live: Option<Live>,
}

#[derive(Deserialize, Default)]
struct Live {
    #[serde(default)]
    players: Option<Vec<Player>>,
    #[serde(default)]
    matches: Option<Vec<Match>>,
}

#[derive(Deserialize)]
struct Player {
    #[serde(default)]
    state: Option<String>,
    #[serde(default, rename = "match")]
    match_id: Option<Value>,
}

#[derive(Deserialize)]
struct Match {
    #[serde(default)]
    id: Option<Value>,
    #[serde(default)]
    state: Option<String>,
    #[serde(default)]
    game_mode: Option<String>,
}

#[derive(Deserialize, Default)]
pub struct Launchers {
    #[serde(default)]
    ts: Option<f64>,
    #[serde(default)]
    open: Option<f64>,
}

#[derive(Default, Debug, PartialEq)]
pub struct Counts {
    pub supply_raid: Option<u64>,
    pub interrogation: Option<u64>,
    pub lobby: Option<u64>,
    pub menus: Option<u64>,
    pub launcher: Option<u64>,
    pub online: Option<u64>,
}

impl Counts {
    pub fn get(&self, key: &str) -> Option<u64> {
        match key {
            "supply-raid" => self.supply_raid,
            "interrogation" => self.interrogation,
            "lobby" => self.lobby,
            "menus" => self.menus,
            "launcher" => self.launcher,
            "online" => self.online,
            _ => None,
        }
    }

    pub fn to_json(&self) -> String {
        let f = |v: Option<u64>| v.map(|n| n.to_string()).unwrap_or_else(|| "null".into());
        format!(
            "{{\"supply-raid\": {}, \"interrogation\": {}, \"lobby\": {}, \"menus\": {}, \"launcher\": {}, \"online\": {}}}",
            f(self.supply_raid),
            f(self.interrogation),
            f(self.lobby),
            f(self.menus),
            f(self.launcher),
            f(self.online)
        )
    }
}

fn truthy(v: &Option<Value>) -> Option<&Value> {
    v.as_ref().filter(|v| !v.is_null())
}

pub fn counts(status: Option<&Status>, launchers: Option<&Launchers>, now: f64) -> Counts {
    let mut out = Counts::default();
    if let Some(s) = status.filter(|s| now - s.ts.unwrap_or(0.0) <= STATUS_STALE_S) {
        let live = s.live.as_ref();
        let matches: HashMap<String, &Match> = live
            .and_then(|l| l.matches.as_ref())
            .map(|ms| ms.iter().map(|m| (truthy(&m.id).map(|v| v.to_string()).unwrap_or_default(), m)).collect())
            .unwrap_or_default();
        let players: &[Player] = live.and_then(|l| l.players.as_deref()).unwrap_or(&[]);
        let (mut sr, mut it, mut lobby, mut menus) = (0, 0, 0, 0);
        for p in players {
            match p.state.as_deref() {
                Some("queue") => lobby += 1,
                Some("match") => {
                    let key = truthy(&p.match_id).map(|v| v.to_string()).unwrap_or_default();
                    let m = matches.get(&key);
                    match m.and_then(|m| m.state.as_deref()) {
                        Some("running") => {
                            if m.and_then(|m| m.game_mode.as_deref()) == Some("interrogation") {
                                it += 1
                            } else {
                                sr += 1
                            }
                        }
                        Some("ended") => menus += 1,
                        _ => lobby += 1,
                    }
                }
                _ => menus += 1,
            }
        }
        out.supply_raid = Some(sr);
        out.interrogation = Some(it);
        out.lobby = Some(lobby);
        out.menus = Some(menus);
        out.online = Some(if players.is_empty() { s.players_online.unwrap_or(0.0).max(0.0) as u64 } else { players.len() as u64 });
    }
    if let Some(l) = launchers.filter(|l| now - l.ts.unwrap_or(0.0) <= LAUNCHERS_STALE_S) {
        out.launcher = Some(l.open.unwrap_or(0.0).max(0.0) as u64);
    }
    out
}

pub fn label_of(template: &str) -> String {
    let head = template.split('{').next().unwrap_or("");
    let label = head.trim_end_matches([' ', ':']);
    if label.is_empty() {
        template.to_string()
    } else {
        label.to_string()
    }
}

pub fn names(c: &Counts, channels: &[(String, String)], offline: &str) -> Vec<(String, String)> {
    channels
        .iter()
        .map(|(key, tpl)| {
            let name = match c.get(key) {
                Some(n) => tpl.replace("{n}", &n.to_string()),
                None => offline.replace("{label}", &label_of(tpl)),
            };
            (key.clone(), truncate_chars(&name, 100))
        })
        .collect()
}

pub fn read_sources(cfg: &LiveCfg) -> (Option<Status>, Option<Launchers>) {
    (read_json(&cfg.status_file), read_json(&cfg.launchers_file))
}

#[derive(Serialize, Deserialize, Default)]
struct State {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    category: Option<Value>,
    #[serde(default)]
    channels: serde_json::Map<String, Value>,
}

fn lock_overwrites(everyone: u64, me: u64) -> Vec<Overwrite> {
    vec![
        Overwrite { id: everyone, kind: 0, allow: VIEW, deny: CONNECT | SPEAK | STREAM | SEND | REACT | INVITE },
        Overwrite { id: me, kind: 1, allow: VIEW | CONNECT | MANAGE_CHANNELS | SEND, deny: 0 },
    ]
}

pub fn same_overwrites(a: &[Overwrite], b: &[Overwrite]) -> bool {
    let key = |v: &[Overwrite]| {
        let mut k: Vec<(u64, u64, u64)> = v.iter().filter(|o| o.allow != 0 || o.deny != 0).map(|o| (o.id, o.allow, o.deny)).collect();
        k.sort_unstable();
        k
    };
    key(a) == key(b)
}

fn ow_json(ow: &[Overwrite]) -> Value {
    Value::Array(ow.iter().map(|o| json!({"id": o.id.to_string(), "type": o.kind, "allow": o.allow.to_string(), "deny": o.deny.to_string()})).collect())
}

struct Runner {
    bot: Arc<Bot>,
    last: HashMap<u64, Instant>,
    chans: HashMap<String, u64>,
}

impl Runner {
    fn state_path(&self) -> std::path::PathBuf {
        self.bot.dir.join(STATE)
    }

    async fn ensure(&mut self) -> Result<(), String> {
        let bot = self.bot.clone();
        let Some(cfg) = bot.cfg.live.as_ref() else { return Err("live counts not configured".into()) };
        let path = self.state_path();
        let mut st: State = read_json(&path).unwrap_or_default();
        let (guild, me) = {
            let c = bot.cache();
            (c.guild, c.me)
        };
        let ow = lock_overwrites(guild, me);
        let cat_id = st.category.as_ref().and_then(crate::util::parse_id).filter(|id| bot.cache().chans.contains_key(id));
        let cat = match cat_id {
            Some(id) => {
                let same = bot.cache().chans.get(&id).map(|c| same_overwrites(&c.overwrites, &ow)).unwrap_or(true);
                if !same {
                    bot.rest.patch(&format!("/channels/{id}"), &json!({"permission_overwrites": ow_json(&ow)}), Some("live player counts: keep locked")).await.map_err(|e| e.to_string())?;
                }
                id
            }
            None => {
                let found = bot.cache().category_named(&cfg.category).map(|c| c.id);
                let id = match found {
                    Some(id) => id,
                    None => {
                        let made = bot
                            .rest
                            .post(&format!("/guilds/{guild}/channels"), &json!({"name": cfg.category, "type": CATEGORY, "permission_overwrites": ow_json(&ow)}), Some("live player counts"))
                            .await
                            .map_err(|e| e.to_string())?;
                        let id = made.get("id").and_then(crate::util::parse_id).ok_or("category create: no id")?;
                        log!("live counts: created category {id}");
                        if let Ok(ch) = serde_json::from_value::<crate::model::Channel>(made) {
                            bot.cache().put_channel(ch);
                        }
                        if let Some(pos) = cfg.position {
                            let mut cats: Vec<u64> = bot.cache().categories().iter().map(|c| c.id).filter(|c| *c != id).collect();
                            let at = (pos.max(0) as usize).min(cats.len());
                            cats.insert(at, id);
                            let body = Value::Array(cats.iter().enumerate().map(|(i, c)| json!({"id": c.to_string(), "position": i})).collect());
                            if let Err(e) = bot.rest.patch(&format!("/guilds/{guild}/channels"), &body, Some("live player counts")).await {
                                log!("live counts: category position: {e}");
                            }
                        }
                        id
                    }
                };
                st.category = Some(json!(id));
                id
            }
        };
        let (status, launchers) = read_sources(cfg);
        let c = counts(status.as_ref(), launchers.as_ref(), now_secs());
        let wanted = names(&c, &cfg.channels, &cfg.offline);
        let templates: HashMap<&str, &str> = cfg.channels.iter().map(|(k, t)| (k.as_str(), t.as_str())).collect();
        let mut out = HashMap::new();
        for (key, name) in wanted {
            let known = st.channels.get(&key).and_then(crate::util::parse_id).filter(|id| bot.cache().chans.contains_key(id));
            let found = known.or_else(|| {
                let lab = label_of(templates.get(key.as_str()).copied().unwrap_or(""));
                let cache = bot.cache();
                let mut v: Vec<_> = cache.chans.values().filter(|c| c.kind == VOICE && c.parent == Some(cat) && c.name.starts_with(&lab)).collect();
                v.sort_by_key(|c| (c.position, c.id));
                v.first().map(|c| c.id)
            });
            let id = match found {
                Some(id) => {
                    let same = bot.cache().chans.get(&id).map(|c| same_overwrites(&c.overwrites, &ow)).unwrap_or(true);
                    if !same {
                        bot.rest.patch(&format!("/channels/{id}"), &json!({"permission_overwrites": ow_json(&ow)}), Some("live player counts: keep locked")).await.map_err(|e| e.to_string())?;
                    }
                    id
                }
                None => {
                    let made = bot
                        .rest
                        .post(
                            &format!("/guilds/{guild}/channels"),
                            &json!({"name": name, "type": VOICE, "parent_id": cat.to_string(), "permission_overwrites": ow_json(&ow)}),
                            Some("live player counts"),
                        )
                        .await
                        .map_err(|e| e.to_string())?;
                    let id = made.get("id").and_then(crate::util::parse_id).ok_or("channel create: no id")?;
                    if let Ok(ch) = serde_json::from_value::<crate::model::Channel>(made) {
                        bot.cache().put_channel(ch);
                    }
                    self.last.insert(id, Instant::now());
                    log!("live counts: created {key} {id}");
                    id
                }
            };
            st.channels.insert(key.clone(), json!(id));
            out.insert(key, id);
        }
        write_json(&path, &st).map_err(|e| format!("{}: {e}", path.display()))?;
        self.chans = out;
        Ok(())
    }

    async fn tick(&mut self) {
        let bot = self.bot.clone();
        let Some(cfg) = bot.cfg.live.as_ref() else { return };
        let (status, launchers) = read_sources(cfg);
        let c = counts(status.as_ref(), launchers.as_ref(), now_secs());
        let min_rename = Duration::from_secs_f64(cfg.min_rename_s);
        for (key, name) in names(&c, &cfg.channels, &cfg.offline) {
            let Some(&id) = self.chans.get(&key) else { continue };
            let current = bot.cache().chans.get(&id).map(|c| c.name.clone());
            if current.as_deref() == Some(name.as_str()) {
                continue;
            }
            let now = Instant::now();
            if self.last.get(&id).is_some_and(|t| now < *t + min_rename) {
                continue;
            }
            self.last.insert(id, now);
            let body = json!({"name": name});
            let path = format!("/channels/{id}");
            let call = bot.rest.call(hyper::Method::PATCH, &path, crate::net::Body::Json(&body), Some("live player counts"), false);
            match tokio::time::timeout(EDIT_TIMEOUT, call).await {
                Ok(Ok(v)) => {
                    if let Ok(ch) = serde_json::from_value::<crate::model::Channel>(v) {
                        bot.cache().put_channel(ch);
                    }
                }
                Ok(Err(e)) if e.status == 429 => {
                    let wait = Duration::from_secs_f64(e.retry_after.clamp(0.0, 3600.0));
                    if wait > min_rename {
                        self.last.insert(id, now + wait - min_rename);
                    }
                    log!("live counts: rename of {key} rate limited for {:.0} s", e.retry_after);
                }
                Ok(Err(e)) => log!("live counts: rename {key}: {e}"),
                Err(_) => log!("live counts: rename of {key} timed out"),
            }
        }
    }
}

pub async fn run(bot: Arc<Bot>) {
    let mut r = Runner { bot, last: HashMap::new(), chans: HashMap::new() };
    loop {
        let missing = r.chans.is_empty() || {
            let cache = r.bot.cache();
            r.chans.values().any(|id| !cache.chans.contains_key(id))
        };
        let ok = if missing {
            match r.ensure().await {
                Ok(()) => true,
                Err(e) => {
                    log!("live counts: {e}");
                    r.chans.clear();
                    false
                }
            }
        } else {
            true
        };
        if ok {
            r.tick().await;
        }
        tokio::time::sleep(TICK).await;
    }
}

pub fn print_names(status: &Path, launchers: &Path, at_snapshot: bool) {
    let s: Option<Status> = read_json(status);
    let mut l: Option<Launchers> = read_json(launchers);
    let mut now = now_secs();
    if at_snapshot {
        now = s.as_ref().and_then(|s| s.ts).unwrap_or(now);
        if let Some(l) = l.as_mut() {
            l.ts = Some(now);
        }
    }
    let c = counts(s.as_ref(), l.as_ref(), now);
    println!("{}", c.to_json());
    for (k, n) in names(&c, &crate::config::default_live_channels(), "{label}: offline") {
        println!("{k:<14} {n}");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn status(json: &str) -> Status {
        serde_json::from_str(json).unwrap()
    }

    #[test]
    fn counts_by_state() {
        let s = status(
            r#"{"ts": 1000, "live": {"players": [
                {"state": "match", "match": "a"}, {"state": "match", "match": "a"}, {"state": "match", "match": "b"},
                {"state": "match", "match": "c"}, {"state": "match", "match": "d"}, {"state": "match", "match": "zz"},
                {"state": "queue"}, {"state": "menu"}, {"state": null}],
              "matches": [{"id": "a", "state": "running", "game_mode": "supply-raid"},
                          {"id": "b", "state": "running", "game_mode": "interrogation"},
                          {"id": "c", "state": "ended"}, {"id": "d", "state": "loading"}]}}"#,
        );
        let l: Launchers = serde_json::from_str(r#"{"ts": 990, "open": 12, "window_s": 75.0}"#).unwrap();
        let c = counts(Some(&s), Some(&l), 1010.0);
        assert_eq!(c, Counts { supply_raid: Some(2), interrogation: Some(1), lobby: Some(3), menus: Some(3), launcher: Some(12), online: Some(9) });
        let stale = counts(Some(&s), Some(&l), 1100.0);
        assert_eq!(stale, Counts { launcher: None, ..Counts::default() });
    }

    #[test]
    fn old_daemon_without_mode_is_supply_raid() {
        let s = status(r#"{"ts": 5, "live": {"players": [{"state": "match", "match": 7}], "matches": [{"id": 7, "state": "running"}]}}"#);
        let c = counts(Some(&s), None, 5.0);
        assert_eq!((c.supply_raid, c.launcher), (Some(1), None));
    }

    #[test]
    fn names_and_offline() {
        let chans = crate::config::default_live_channels();
        let c = Counts { supply_raid: Some(6), interrogation: Some(4), lobby: Some(3), menus: Some(2), launcher: None, online: Some(15) };
        let n = names(&c, &chans, "{label}: offline");
        assert_eq!(n[0].1, "Supply Raid: 6 in match");
        assert_eq!(n[3].1, "In menus: 2");
        assert_eq!(n[4].1, "Launcher open: offline");
        assert_eq!(label_of("Supply Raid: {n} in match"), "Supply Raid");
        assert_eq!(label_of("{n} online"), "{n} online");
    }

    #[test]
    fn overwrite_compare_ignores_order_and_empty() {
        let a = lock_overwrites(1, 2);
        let mut b = lock_overwrites(1, 2);
        b.reverse();
        b.push(Overwrite { id: 3, kind: 0, allow: 0, deny: 0 });
        assert!(same_overwrites(&a, &b));
        b[0].allow |= 1;
        assert!(!same_overwrites(&a, &b));
    }
}
