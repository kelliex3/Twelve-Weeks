use crate::bot::{Bot, AVATAR_UA, EPHEMERAL};
use crate::desk::{self, KnownOp};
use crate::log;
use crate::model::{Attachment, Interaction, Member};
use crate::net::{Body, File};
use crate::util::{parse_id, read_json, truncate_chars};
use hyper::Method;
use serde_json::{json, Map, Value};
use std::time::{Duration, Instant};

pub const CREAM: u32 = 0xD6CEB8;
const MANAGE_GUILD: &str = "32";
const MANAGE_MESSAGES: &str = "8192";
const LFG_COOLDOWN: Duration = Duration::from_secs(300);
const IMAGE_EXT: [&str; 5] = [".png", ".jpg", ".jpeg", ".gif", ".webp"];
const FAILED: &str = "Something went wrong, try again or ask a moderator.";

fn opt(name: &str, kind: u8, required: bool, description: &str) -> Value {
    json!({"name": name, "type": kind, "required": required, "description": description})
}

const STR: u8 = 3;
const INT: u8 = 4;
const BOOL: u8 = 5;
const USER: u8 = 6;
const FILE: u8 = 11;
const DOTS: &str = "\u{2026}";

fn cmd(name: &str, description: &str, perms: Option<&str>, options: Vec<Value>) -> Value {
    json!({"name": name, "type": 1, "description": description, "options": options,
           "default_member_permissions": perms, "contexts": [0], "nsfw": false})
}

pub fn definitions(desk: bool) -> Value {
    let mut v = vec![
        cmd("lfg", "Ping everyone with the LFG role (only in #lfg)", None, vec![opt("note", STR, false, DOTS)]),
        cmd(
            "sneakpeek",
            "Post a sneak peek and ping the Sneak Peeks role",
            Some(MANAGE_GUILD),
            vec![opt("text", STR, true, DOTS), opt("media", FILE, false, DOTS), opt("media2", FILE, false, DOTS), opt("title", STR, false, DOTS)],
        ),
        cmd(
            "announce",
            "Post an announcement",
            Some(MANAGE_GUILD),
            vec![opt("title", STR, true, DOTS), opt("text", STR, true, DOTS), opt("media", FILE, false, DOTS), opt("ping_everyone", BOOL, false, DOTS)],
        ),
        cmd(
            "patchnotes",
            "Post patch notes (use \\n for new lines)",
            Some(MANAGE_GUILD),
            vec![opt("version", STR, true, DOTS), opt("text", STR, true, DOTS), opt("media", FILE, false, DOTS)],
        ),
        cmd("welcome-preview", "Show your own #new-survivors welcome card (only you see it)", Some(MANAGE_GUILD), vec![]),
        cmd(
            "files",
            "Post launcher downloads in #downloads",
            Some(MANAGE_GUILD),
            vec![opt("version", STR, true, DOTS), opt("windows", FILE, false, DOTS), opt("linux", FILE, false, DOTS), opt("note", STR, false, DOTS)],
        ),
    ];
    if desk {
        let sub = |name: &str, description: &str, options: Vec<Value>| json!({"name": name, "type": 1, "description": description, "options": options});
        v.push(cmd(
            "known-issue",
            "The known-issues list in the pinned #bug-reports post",
            Some(MANAGE_GUILD),
            vec![
                sub("add", "Add a known issue", vec![opt("text", STR, true, DOTS)]),
                sub("fixed", "Mark a known issue fixed in a build", vec![opt("number", INT, true, DOTS), opt("build", STR, true, DOTS)]),
                sub("remove", "Remove a known issue from the list", vec![opt("number", INT, true, DOTS)]),
            ],
        ));
        v.push(cmd(
            "report",
            "Post the official how-to-report-a-bug instructions in this channel",
            Some(MANAGE_MESSAGES),
            vec![opt("user", USER, false, "Player to mention (optional)")],
        ));
    }
    Value::Array(v)
}

struct Args<'a> {
    opts: Map<String, Value>,
    data: &'a Value,
}

impl<'a> Args<'a> {
    fn new(data: &'a Value, opts: Option<&Value>) -> Args<'a> {
        let mut m = Map::new();
        for o in opts.and_then(Value::as_array).into_iter().flatten() {
            if let Some(n) = o.get("name").and_then(Value::as_str) {
                m.insert(n.to_string(), o.get("value").cloned().unwrap_or(Value::Null));
            }
        }
        Args { opts: m, data }
    }

    fn str(&self, n: &str) -> Option<String> {
        self.opts.get(n).and_then(Value::as_str).map(str::to_string)
    }

    fn int(&self, n: &str) -> Option<i64> {
        self.opts.get(n).and_then(Value::as_i64)
    }

    fn bool(&self, n: &str) -> bool {
        self.opts.get(n).and_then(Value::as_bool).unwrap_or(false)
    }

    fn id(&self, n: &str) -> Option<u64> {
        self.opts.get(n).and_then(parse_id)
    }

    fn attachment(&self, n: &str) -> Option<Attachment> {
        let id = self.opts.get(n).and_then(Value::as_str)?;
        let a = self.data.get("resolved")?.get("attachments")?.get(id)?;
        serde_json::from_value(a.clone()).ok()
    }
}

pub enum Ping {
    None,
    Role(Option<u64>),
    Everyone,
}

fn embed(title: Option<&str>, text: &str, image: Option<&str>) -> Value {
    let mut e = json!({"description": text, "color": CREAM});
    if let Some(t) = title {
        e["title"] = json!(t);
    }
    if let Some(i) = image {
        e["image"] = json!({"url": format!("attachment://{i}")});
    }
    e
}

pub fn post_payload(title: Option<&str>, text: &str, files: &[String], ping: &Ping) -> Value {
    let image = files.iter().find(|f| {
        let l = f.to_lowercase();
        IMAGE_EXT.iter().any(|e| l.ends_with(e))
    });
    let (content, allowed) = match ping {
        Ping::Role(Some(r)) => (Some(format!("<@&{r}>")), json!({"parse": [], "roles": [r.to_string()]})),
        Ping::Everyone => (Some("@everyone".to_string()), json!({"parse": ["everyone"]})),
        _ => (None, json!({"parse": []})),
    };
    let mut data = json!({
        "embeds": [embed(title, text, image.map(String::as_str))],
        "allowed_mentions": allowed,
        "attachments": files.iter().enumerate().map(|(i, f)| json!({"id": i, "filename": f})).collect::<Vec<_>>(),
    });
    if let Some(c) = content {
        data["content"] = json!(c);
    }
    data
}

fn channel_for(bot: &Bot, name: &str) -> Option<(u64, u8)> {
    let ids: Map<String, Value> = read_json(&bot.dir.join(desk::CHANNELS)).unwrap_or_default();
    let cache = bot.cache();
    ids.get(name).and_then(parse_id).and_then(|id| cache.chans.get(&id)).or_else(|| cache.any_named(name)).map(|c| (c.id, c.kind))
}

async fn post(bot: &Bot, it: &Interaction, name: &str, title: Option<&str>, text: &str, atts: Vec<Attachment>, ping: Ping) {
    let Some((chan, kind)) = channel_for(bot, name) else {
        bot.reply(it, &format!("#{name} not found."), true).await;
        return;
    };
    if !bot.defer(it, true).await {
        return;
    }
    let result = async {
        let mut files = Vec::new();
        for a in &atts {
            let (status, data) = bot.rest.fetch(Method::GET, &a.url, AVATAR_UA, 512 << 20).await?;
            if status != 200 {
                return Err(format!("could not download {} ({status})", a.filename));
            }
            files.push(File { name: a.filename.clone(), data });
        }
        let names: Vec<String> = files.iter().map(|f| f.name.clone()).collect();
        let data = post_payload(title, text, &names, &ping);
        let body = if files.is_empty() { Body::Json(&data) } else { Body::Multipart(&data, &files) };
        let msg = bot.rest.call(Method::POST, &format!("/channels/{chan}/messages"), body, None, true).await.map_err(|e| {
            if e.status == 413 {
                "the files are too big for Discord's upload limit".to_string()
            } else {
                e.to_string()
            }
        })?;
        if kind == crate::cache::NEWS {
            if let Some(m) = msg.get("id").and_then(parse_id) {
                let _ = bot.rest.call(Method::POST, &format!("/channels/{chan}/messages/{m}/crosspost"), Body::Empty, None, true).await;
            }
        }
        Ok::<(), String>(())
    }
    .await;
    let text = match result {
        Ok(()) => format!("Posted in <#{chan}>."),
        Err(e) => {
            log!("/{name} post: {e}");
            format!("Not posted: {e}.")
        }
    };
    if let Err(e) = bot.followup(it, json!({"content": text, "flags": EPHEMERAL}), &[]).await {
        log!("interaction followup: {e}");
    }
}

pub fn lfg_message(role: u64, user: u64, note: &str) -> String {
    let note = truncate_chars(&note.replace('@', "@\u{200b}"), 200);
    if note.is_empty() {
        format!("<@&{role}> <@{user}> is looking for a group.")
    } else {
        format!("<@&{role}> <@{user}> is looking for a group: {note}")
    }
}

async fn lfg(bot: &Bot, it: &Interaction, a: &Args<'_>) {
    let chan_name = it.channel.as_ref().and_then(|c| c.name.clone()).or_else(|| bot.cache().chans.get(&it.channel_id()).map(|c| c.name.clone()));
    if chan_name.as_deref() != Some("lfg") {
        let m = bot.cache().text_named("lfg").map(|c| format!("<#{}>", c.id)).unwrap_or_else(|| "#lfg".into());
        return bot.reply(it, &format!("Use /lfg in {m}."), true).await;
    }
    let Some(role) = bot.cache().role_named("LFG").map(|r| r.id) else {
        return bot.reply(it, "Nobody has LFG pings on yet (the button is in #choose-your-faction).", true).await;
    };
    let user = it.user_id();
    let now = Instant::now();
    let cooling = {
        let mut last = bot.lfg_last.lock().unwrap_or_else(std::sync::PoisonError::into_inner);
        last.retain(|_, t| now.duration_since(*t) < LFG_COOLDOWN);
        let cooling = last.contains_key(&user);
        if !cooling {
            last.insert(user, now);
        }
        cooling
    };
    if cooling {
        return bot.reply(it, "You can ping LFG once every 5 minutes.", true).await;
    }
    let data = json!({"content": lfg_message(role, user, &a.str("note").unwrap_or_default()),
                      "allowed_mentions": {"parse": [], "roles": [role.to_string()], "users": [user.to_string()]}});
    if let Err(e) = bot.respond(it, 4, data).await {
        log!("/lfg: {e}");
    }
}

async fn known(bot: &Bot, it: &Interaction, sub: &Value) {
    if !bot.desk().ready {
        return bot.reply(it, "Bug desk is not running.", true).await;
    }
    let a = Args::new(sub, sub.get("options"));
    let op = match sub.get("name").and_then(Value::as_str) {
        Some("add") => KnownOp::Add(a.str("text").unwrap_or_default()),
        Some("fixed") => KnownOp::Fixed(a.int("number").unwrap_or(0), a.str("build").unwrap_or_default()),
        Some("remove") => KnownOp::Remove(a.int("number").unwrap_or(0)),
        _ => return,
    };
    let items = match desk::known_edit(&bot.dir.join(desk::KNOWN), op) {
        Ok(i) => i,
        Err(e) => return bot.reply(it, &e, true).await,
    };
    if !bot.defer(it, true).await {
        return;
    }
    let text = match desk::refresh_known(bot).await {
        Ok(()) => truncate_chars(&desk::known_text(&items), 1900),
        Err(e) => {
            log!("/known-issue: {e}");
            format!("Saved, but the pinned list was not updated: {e}")
        }
    };
    if let Err(e) = bot.followup(it, json!({"content": text, "flags": EPHEMERAL, "allowed_mentions": {"parse": []}}), &[]).await {
        log!("interaction followup: {e}");
    }
}

pub async fn command(bot: &Bot, it: &Interaction) {
    let Some(data) = it.data.as_ref() else { return };
    let name = data.get("name").and_then(Value::as_str).unwrap_or("");
    let a = Args::new(data, data.get("options"));
    match name {
        "lfg" => lfg(bot, it, &a).await,
        "sneakpeek" => {
            let role = bot.cache().role_named("Sneak Peeks").map(|r| r.id);
            let atts = [a.attachment("media"), a.attachment("media2")].into_iter().flatten().collect();
            let title = a.str("title").unwrap_or_else(|| "Sneak peek".into());
            post(bot, it, "sneak-peeks", Some(&title), &a.str("text").unwrap_or_default(), atts, Ping::Role(role)).await
        }
        "announce" => {
            let ping = if a.bool("ping_everyone") { Ping::Everyone } else { Ping::None };
            let atts = a.attachment("media").into_iter().collect();
            post(bot, it, "announcements", Some(&a.str("title").unwrap_or_default()), &a.str("text").unwrap_or_default(), atts, ping).await
        }
        "patchnotes" => {
            let title = format!("Patch notes {}", a.str("version").unwrap_or_default());
            let text = a.str("text").unwrap_or_default().replace("\\n", "\n");
            post(bot, it, "patch-notes", Some(&title), &text, a.attachment("media").into_iter().collect(), Ping::None).await
        }
        "files" => {
            let (win, lin) = (a.attachment("windows"), a.attachment("linux"));
            let mut lines = vec![format!("**Launcher {}**", a.str("version").unwrap_or_default())];
            if let Some(n) = a.str("note").filter(|n| !n.is_empty()) {
                lines.push(n);
            }
            if win.is_some() {
                lines.push("Windows: the `.exe` below".into());
            }
            if lin.is_some() {
                lines.push("Linux / Steam Deck: the `.AppImage` below (chmod +x, then run)".into());
            }
            post(bot, it, "files", None, &lines.join("\n"), [win, lin].into_iter().flatten().collect(), Ping::None).await
        }
        "welcome-preview" => {
            if !bot.defer(it, true).await {
                return;
            }
            let m = it.member.clone().unwrap_or_default();
            let count = bot.cache().member_count;
            let (data, files) = match bot.card(&m, count).await {
                Ok(png) => (
                    json!({"content": bot.welcome_text(it.user_id()), "flags": EPHEMERAL, "allowed_mentions": {"parse": []},
                           "attachments": [{"id": 0, "filename": "welcome.png"}]}),
                    vec![File { name: "welcome.png".into(), data: png }],
                ),
                Err(e) => {
                    log!("welcome card: {e}");
                    (json!({"content": format!("Welcome card failed: {e}"), "flags": EPHEMERAL}), vec![])
                }
            };
            if let Err(e) = bot.followup(it, data, &files).await {
                log!("interaction followup: {e}");
            }
        }
        "known-issue" => {
            let sub = data.get("options").and_then(Value::as_array).and_then(|o| o.first()).cloned().unwrap_or(Value::Null);
            known(bot, it, &sub).await
        }
        "report" => {
            let user = a.id("user");
            let st = {
                let d = bot.desk();
                if d.ready {
                    d.st.clone()
                } else {
                    read_json(&bot.dir.join(desk::STATE)).unwrap_or_default()
                }
            };
            if let Err(e) = bot.respond(it, 4, desk::report_message(&st, bot.cfg.guild_id, user)).await {
                log!("/report: {e}");
            }
        }
        _ => {}
    }
}

async fn set_role(bot: &Bot, user: u64, role: u64, on: bool, reason: &str) -> Result<(), String> {
    let path = format!("/guilds/{}/members/{user}/roles/{role}", bot.cfg.guild_id);
    let r = if on { bot.rest.put_empty(&path, Some(reason)).await } else { bot.rest.delete(&path, Some(reason)).await };
    r.map(|_| ()).map_err(|e| e.to_string())
}

async fn faction(bot: &Bot, it: &Interaction, m: &Member, name: &str) {
    let other_name = if name == "WLF" { "Seraphites" } else { "WLF" };
    let (want, other) = {
        let c = bot.cache();
        (c.role_named(name).map(|r| r.id), c.role_named(other_name).map(|r| r.id))
    };
    let Some(want) = want else { return bot.reply(it, "That role is missing, ask a moderator.", true).await };
    if m.roles.contains(&want) {
        return bot.reply(it, &format!("You're already {name}."), true).await;
    }
    let user = it.user_id();
    let line = if name == "WLF" {
        "Welcome to the WLF. May your survival be long. May your death be swift."
    } else {
        "Welcome, sister. Welcome, brother. The Prophet watches over you."
    };
    bot.reply_after(it, async {
        if let Some(o) = other.filter(|o| m.roles.contains(o)) {
            if let Err(e) = set_role(bot, user, o, false, "faction switch").await {
                log!("faction: {e}");
                return FAILED.to_string();
            }
        }
        match set_role(bot, user, want, true, "faction pick").await {
            Ok(()) => line.to_string(),
            Err(e) => {
                log!("faction: {e}");
                FAILED.to_string()
            }
        }
    })
    .await
}

pub fn toggle_text(role: &str, on: bool) -> &'static str {
    match (role, on) {
        ("Sneak Peeks", true) => "Sneak peek pings **on**. You'll be pinged in #sneak-peeks.",
        ("Sneak Peeks", false) => "Sneak peek pings **off**.",
        (_, true) => "LFG pings **on**. Anyone looking for a group in #lfg can ping you with `/lfg`.",
        (_, false) => "LFG pings **off**.",
    }
}

async fn toggle(bot: &Bot, it: &Interaction, m: &Member, name: &'static str) {
    let role = bot.cache().role_named(name).map(|r| r.id);
    let user = it.user_id();
    if role.is_none() && name != "LFG" {
        return bot.reply(it, "That role is missing, ask a moderator.", true).await;
    }
    bot.reply_after(it, async {
        let role = match role {
            Some(r) => r,
            None => {
                let made = bot
                    .rest
                    .post(&format!("/guilds/{}/roles", bot.cfg.guild_id), &json!({"name": "LFG", "mentionable": false}), Some("LFG pings button (first use)"))
                    .await;
                match made.ok().and_then(|v| serde_json::from_value::<crate::model::Role>(v).ok()) {
                    Some(r) => {
                        let id = r.id;
                        bot.cache().roles.insert(id, r);
                        id
                    }
                    None => return FAILED.to_string(),
                }
            }
        };
        let on = !m.roles.contains(&role);
        let reason = match (name, on) {
            ("Sneak Peeks", true) => "sneak peek pings on",
            ("Sneak Peeks", false) => "sneak peek pings off",
            (_, true) => "LFG pings on",
            (_, false) => "LFG pings off",
        };
        match set_role(bot, user, role, on, reason).await {
            Ok(()) => toggle_text(name, on).to_string(),
            Err(e) => {
                log!("{name} toggle: {e}");
                FAILED.to_string()
            }
        }
    })
    .await
}

pub async fn button(bot: &Bot, it: &Interaction, cid: &str) {
    let m = it.member.clone().unwrap_or_default();
    match cid {
        "tw:faction:wlf" => faction(bot, it, &m, "WLF").await,
        "tw:faction:ser" => faction(bot, it, &m, "Seraphites").await,
        "tw:ping:sneak" => toggle(bot, it, &m, "Sneak Peeks").await,
        "tw:ping:lfg" => toggle(bot, it, &m, "LFG").await,
        _ => {}
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn command_set() {
        let all = definitions(true);
        let names: Vec<&str> = all.as_array().unwrap().iter().map(|c| c["name"].as_str().unwrap()).collect();
        assert_eq!(names, ["lfg", "sneakpeek", "announce", "patchnotes", "welcome-preview", "files", "known-issue", "report"]);
        assert_eq!(definitions(false).as_array().unwrap().len(), 6);
        for c in all.as_array().unwrap() {
            let opts = c["options"].as_array().unwrap();
            let first_optional = opts.iter().position(|o| o["required"] == false).unwrap_or(opts.len());
            assert!(opts[first_optional..].iter().all(|o| o["required"] == false), "{}", c["name"]);
        }
    }

    #[test]
    fn lfg_text() {
        assert_eq!(lfg_message(1, 2, ""), "<@&1> <@2> is looking for a group.");
        assert_eq!(lfg_message(1, 2, "ranked @everyone"), "<@&1> <@2> is looking for a group: ranked @\u{200b}everyone");
        assert_eq!(lfg_message(1, 2, &"x".repeat(300)).chars().count(), "<@&1> <@2> is looking for a group: ".len() + 200);
    }

    #[test]
    fn post_payloads() {
        let p = post_payload(Some("Sneak peek"), "new map", &["clip.MP4".into(), "shot.PNG".into()], &Ping::Role(Some(7)));
        assert_eq!(p["content"], "<@&7>");
        assert_eq!(p["allowed_mentions"]["roles"][0], "7");
        assert_eq!(p["embeds"][0]["image"]["url"], "attachment://shot.PNG");
        assert_eq!(p["attachments"][1]["id"], 1);
        let q = post_payload(None, "x", &[], &Ping::Everyone);
        assert_eq!(q["content"], "@everyone");
        assert!(q["embeds"][0].get("title").is_none());
        let r = post_payload(None, "x", &[], &Ping::Role(None));
        assert!(r.get("content").is_none());
    }

    #[test]
    fn recorded_command_payload() {
        let it: Interaction = serde_json::from_str(include_str!("../tests/data/interaction_sneakpeek.json")).unwrap();
        let data = it.data.as_ref().unwrap();
        let a = Args::new(data, data.get("options"));
        assert_eq!(a.str("text").as_deref(), Some("New map soon"));
        let att = a.attachment("media").unwrap();
        assert_eq!(att.filename, "teaser.png");
        assert!(a.attachment("media2").is_none());
        assert_eq!(it.user_id(), 530770100000000001);
        assert_eq!(it.member.as_ref().unwrap().roles, vec![1567955300000000001]);
    }
}
