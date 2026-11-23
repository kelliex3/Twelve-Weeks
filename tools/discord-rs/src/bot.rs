use crate::cache::Cache;
use crate::config::Config;
use crate::desk::Desk;
use crate::log;
use crate::model::{Channel, Envelope, Guild, Interaction, Member, MemberRemove, Ready, RoleDelete, RoleEvent};
use crate::net::{Body, File, Rest};
use crate::{commands, desk, live};
use hyper::Method;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::future::{poll_fn, Future};
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::{Duration, Instant};
use tokio::sync::Notify;
use twilight_gateway::{Intents, Message, Shard, ShardId};

pub const EPHEMERAL: u64 = 1 << 6;
pub const AVATAR_UA: &str = "TwelveWeeksBot/1.0";

pub struct Bot {
    pub cfg: Config,
    pub dir: PathBuf,
    pub rest: Rest,
    cache: Mutex<Cache>,
    desk: Mutex<Desk>,
    pub lfg_last: Mutex<HashMap<u64, Instant>>,
    pub automod_dirty: Notify,
    started: AtomicBool,
}

fn parse<'a, T: serde::Deserialize<'a>>(d: &'a serde_json::value::RawValue) -> Option<T> {
    serde_json::from_str(d.get()).ok()
}

impl Bot {
    pub fn new(cfg: Config, dir: PathBuf) -> Result<Bot, String> {
        let rest = Rest::new(&cfg.token)?;
        let reports = cfg.desk.as_ref().map(|d| d.reports_dir.clone()).unwrap_or_default();
        let desk = Desk::new(&dir, reports);
        Ok(Bot {
            cfg,
            dir,
            rest,
            cache: Mutex::new(Cache::default()),
            desk: Mutex::new(desk),
            lfg_last: Mutex::new(HashMap::new()),
            automod_dirty: Notify::new(),
            started: AtomicBool::new(false),
        })
    }

    pub fn cache(&self) -> MutexGuard<'_, Cache> {
        self.cache.lock().unwrap_or_else(PoisonError::into_inner)
    }

    pub fn desk(&self) -> MutexGuard<'_, Desk> {
        self.desk.lock().unwrap_or_else(PoisonError::into_inner)
    }

    pub fn handle(self: &Arc<Self>, text: &str) {
        let Ok(env) = serde_json::from_str::<Envelope>(text) else { return };
        if env.op != 0 {
            return;
        }
        let (Some(t), Some(d)) = (env.t, env.d) else { return };
        let guild = self.cfg.guild_id;
        match t {
            "READY" => {
                if let Some(r) = parse::<Ready>(d) {
                    let mut c = self.cache();
                    c.me = r.user.id;
                    c.app = r.application.id;
                    c.me_name = r.user.tag();
                    log!("logged in as {} (run)", c.me_name);
                }
            }
            "GUILD_CREATE" => {
                let Some(g) = parse::<Guild>(d) else { return };
                if g.id != guild || g.unavailable == Some(true) {
                    return;
                }
                self.cache().load_guild(g);
                if !self.started.swap(true, Ordering::SeqCst) {
                    self.on_start();
                }
            }
            "GUILD_ROLE_CREATE" | "GUILD_ROLE_UPDATE" => {
                if let Some(r) = parse::<RoleEvent>(d).filter(|r| r.guild_id == guild) {
                    self.cache().roles.insert(r.role.id, r.role);
                }
            }
            "GUILD_ROLE_DELETE" => {
                if let Some(r) = parse::<RoleDelete>(d).filter(|r| r.guild_id == guild) {
                    self.cache().roles.remove(&r.role_id);
                }
            }
            "CHANNEL_CREATE" | "CHANNEL_UPDATE" | "CHANNEL_DELETE" => {
                let Some(c) = parse::<Channel>(d).filter(|c| c.guild_id == Some(guild)) else { return };
                let structural = t != "CHANNEL_UPDATE";
                if t == "CHANNEL_DELETE" {
                    self.cache().chans.remove(&c.id);
                } else {
                    self.cache().put_channel(c);
                }
                if structural && self.cfg.desk.is_some() {
                    self.automod_dirty.notify_one();
                }
            }
            "THREAD_CREATE" => {
                let Some(c) = parse::<Channel>(d).filter(|c| c.guild_id == Some(guild)) else { return };
                if c.newly_created == Some(true) && self.cfg.desk.is_some() {
                    tokio::spawn(desk::on_thread_create(self.clone(), c));
                }
            }
            "GUILD_MEMBER_ADD" => {
                let Some(m) = parse::<Member>(d).filter(|m| m.guild_id == Some(guild)) else { return };
                let count = {
                    let mut c = self.cache();
                    c.member_count += 1;
                    c.member_count
                };
                if m.user.as_ref().is_some_and(|u| !u.bot) {
                    let bot = self.clone();
                    tokio::spawn(async move {
                        if let Err(e) = welcome(&bot, &m, count).await {
                            log!("welcome card: {e}");
                        }
                    });
                }
            }
            "GUILD_MEMBER_REMOVE" => {
                if parse::<MemberRemove>(d).is_some_and(|m| m.guild_id == guild) {
                    let mut c = self.cache();
                    c.member_count = c.member_count.saturating_sub(1);
                }
            }
            "INTERACTION_CREATE" => {
                if let Some(it) = parse::<Interaction>(d) {
                    if it.guild_id == Some(guild) {
                        tokio::spawn(interaction(self.clone(), it));
                    }
                }
            }
            _ => {}
        }
    }

    fn on_start(self: &Arc<Self>) {
        let bot = self.clone();
        tokio::spawn(async move {
            let (app, guild) = {
                let c = bot.cache();
                (c.app, c.guild)
            };
            let cmds = commands::definitions(bot.cfg.desk.is_some());
            if let Err(e) = bot.rest.put(&format!("/applications/{app}/guilds/{guild}/commands"), &cmds, None).await {
                log!("slash commands: sync failed: {e}");
            }
        });
        if self.cfg.desk.is_some() {
            tokio::spawn(desk::run(self.clone()));
        }
        if self.cfg.live.is_some() {
            tokio::spawn(live::run(self.clone()));
        }
    }

    pub async fn respond(&self, it: &Interaction, kind: u8, data: Value) -> Result<Value, crate::net::ApiError> {
        self.rest.post(&format!("/interactions/{}/{}/callback", it.id, it.token), &json!({"type": kind, "data": data}), None).await
    }

    pub async fn reply(&self, it: &Interaction, content: &str, ephemeral: bool) {
        let mut data = json!({"content": content, "allowed_mentions": {"parse": []}});
        if ephemeral {
            data["flags"] = json!(EPHEMERAL);
        }
        if let Err(e) = self.respond(it, 4, data).await {
            log!("interaction reply: {e}");
        }
    }

    pub async fn defer(&self, it: &Interaction, ephemeral: bool) -> bool {
        let data = if ephemeral { json!({"flags": EPHEMERAL}) } else { json!({}) };
        match self.respond(it, 5, data).await {
            Ok(_) => true,
            Err(e) => {
                log!("interaction defer: {e}");
                false
            }
        }
    }

    pub async fn followup(&self, it: &Interaction, data: Value, files: &[File]) -> Result<Value, crate::net::ApiError> {
        let path = format!("/webhooks/{}/{}", it.application_id, it.token);
        let body = if files.is_empty() { Body::Json(&data) } else { Body::Multipart(&data, files) };
        self.rest.call(Method::POST, &path, body, None, true).await
    }

    pub async fn edit_original(&self, it: &Interaction, content: &str) {
        let path = format!("/webhooks/{}/{}/messages/@original", it.application_id, it.token);
        if let Err(e) = self.rest.patch(&path, &json!({"content": content, "allowed_mentions": {"parse": []}}), None).await {
            log!("interaction edit: {e}");
        }
    }

    pub async fn reply_after<F: Future<Output = String>>(&self, it: &Interaction, work: F) {
        let mut work = std::pin::pin!(work);
        tokio::select! {
            text = &mut work => self.reply(it, &text, true).await,
            _ = tokio::time::sleep(Duration::from_secs(2)) => {
                let deferred = self.defer(it, true).await;
                let text = work.await;
                if deferred {
                    self.edit_original(it, &text).await;
                }
            }
        }
    }

    pub fn welcome_text(&self, user: u64) -> String {
        let c = self.cache();
        format!(
            "<@{user}> welcome to **Twelve Weeks**. Read {}, pick your side in {}, grab the launcher in {}.",
            c.mention("rules"),
            c.mention("choose-your-faction"),
            c.mention("downloads")
        )
    }

    pub async fn card(&self, m: &Member, number: u64) -> Result<Vec<u8>, String> {
        let guild = self.cfg.guild_id;
        let avatar = match m.avatar_url(guild) {
            Some(url) => match self.rest.fetch(Method::GET, &url, AVATAR_UA, 16 << 20).await {
                Ok((200, b)) => b,
                _ => Vec::new(),
            },
            None => Vec::new(),
        };
        let name = m.display_name();
        let username = m.user.as_ref().map(|u| u.username.clone()).unwrap_or_else(|| "survivor".into());
        let art = self.dir.join("welcome-art");
        let n = (number > 0).then_some(number);
        tokio::task::spawn_blocking(move || crate::card::render(&art, &name, &avatar, n, &username)).await.map_err(|e| e.to_string())?
    }
}

async fn welcome(bot: &Bot, m: &Member, count: u64) -> Result<(), String> {
    let Some(chan) = bot.cache().text_named("new-survivors").map(|c| c.id) else { return Ok(()) };
    let Some(user) = m.user.as_ref().map(|u| u.id) else { return Ok(()) };
    let png = bot.card(m, count).await?;
    let data = json!({
        "content": bot.welcome_text(user),
        "allowed_mentions": {"parse": [], "users": [user.to_string()]},
        "attachments": [{"id": 0, "filename": "welcome.png"}],
    });
    let files = [File { name: "welcome.png".into(), data: png }];
    bot.rest.call(Method::POST, &format!("/channels/{chan}/messages"), Body::Multipart(&data, &files), None, true).await.map_err(|e| e.to_string())?;
    Ok(())
}

async fn interaction(bot: Arc<Bot>, it: Interaction) {
    match it.kind {
        2 => commands::command(&bot, &it).await,
        3 => {
            let cid = it.data.as_ref().and_then(|d| d.get("custom_id")).and_then(Value::as_str).unwrap_or("").to_string();
            commands::button(&bot, &it, &cid).await
        }
        _ => {}
    }
}

pub async fn run(bot: Arc<Bot>) -> Result<(), String> {
    let mut shard = Shard::new(ShardId::ONE, bot.cfg.token.clone(), Intents::GUILDS | Intents::GUILD_MEMBERS);
    loop {
        let next = poll_fn(|cx| futures_core::Stream::poll_next(Pin::new(&mut shard), cx)).await;
        match next {
            None => return Err("gateway closed for good (token or intents rejected?)".into()),
            Some(Err(e)) => log!("gateway: {e}"),
            Some(Ok(Message::Close(frame))) => log!("gateway: closed {}", frame.map(|f| format!("{} {}", f.code, f.reason)).unwrap_or_default()),
            Some(Ok(Message::Text(text))) => bot.handle(&text),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recorded_session_updates_cache() {
        let _ = rustls::crypto::ring::default_provider().install_default();
        let cfg = Config::parse(br#"{"token": "test", "guild_id": "1555300000000000000"}"#).unwrap();
        let bot = Arc::new(Bot::new(cfg, std::env::temp_dir()).unwrap());
        bot.started.store(true, Ordering::SeqCst);
        let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
        rt.block_on(async {
            for line in include_str!("../tests/data/gateway_session.jsonl").lines() {
                bot.handle(line);
            }
        });
        let c = bot.cache();
        assert_eq!((c.me, c.app, c.me_name.as_str()), (1567900000000000000, 1567900000000000000, "Twelve Weeks#5416"));
        assert_eq!(c.guild, 1555300000000000000);
        assert_eq!(c.owner, 193061500000000002);
        assert_eq!(c.member_count, 140);
        assert_eq!(c.role_named("LFG").map(|r| r.id), Some(1574000000000000009));
        assert!(c.role_named("Sneak Peeks").is_none());
        assert_eq!(c.chans[&1573700470080405524].name, "Supply Raid: 31 in match");
        assert!(c.chans.contains_key(&1574100000000000001));
        assert!(!c.chans.contains_key(&1567955311925530704));
        assert!(!c.chans.contains_key(&1573911815489060946));
        assert_eq!(c.mention("rules"), "#rules");
        assert_eq!(c.mention("downloads"), "<#1555319465418432622>");
        assert_eq!(c.text_named("new-survivors").map(|ch| ch.id), Some(1573396361892590000));
        assert_eq!(c.chans[&1573911814498951221].tags[0].1, "Report found");
        assert_eq!(c.general().map(|g| g.id), Some(1573396361892597932));
        let ow = &c.chans[&1573700465286447145].overwrites;
        assert!(crate::live::same_overwrites(ow, &c.chans[&1573700470080405524].overwrites));
    }
}
