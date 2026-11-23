use crate::bot::Bot;
use crate::cache::{CATEGORY, FORUM, MANAGE_GUILD, MANAGE_THREADS};
use crate::log;
use crate::model::{Channel, Member, Message};
use crate::util::{iso_utc, parse_id, read_json, today, truncate_chars, write_json};
use serde_json::{json, Map, Value};
use std::collections::HashSet;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, SystemTime};

pub const STATE: &str = "bug_desk.json";
pub const CODES: &str = "report_codes.json";
pub const KNOWN: &str = "known_issues.json";
pub const CHANNELS: &str = "channels.json";

pub const STAFF_ROLES: [&str; 3] = ["Lead Dev", "Support Dev", "Moderator"];
pub const TAGS: [&str; 3] = ["Report found", "No logs", "Fixed"];
const RETRY_S: [u64; 3] = [20, 40, 60];
const STARTER_WAITS: [u64; 8] = [0, 2, 3, 5, 10, 20, 30, 50];

pub const FORUM_TOPIC: &str = "Start every post with your launcher report code: R-AB12 short title. Send the report first: launcher -> REPORT A BUG. Upload failed? Use R-EMPTY. Posts without a code are locked. Read the pinned post.";
pub const GUIDE_TITLE: &str = "READ FIRST: how to report a bug";
pub const GUIDE: &str = "**How to report a bug**
1. In the launcher, open **REPORT A BUG**, write what happened and send it. The launcher shows your code, like `R-AB12`. The report carries your logs; without them most bugs cannot be fixed.
2. Make a post here. The title starts with the code: `R-AB12 crash when the match ended`
3. In the post, say what happened, the map and mode, and what you did just before. Clips help a lot.

**The report did not upload** (too big, error)? Use `R-EMPTY` as the code: `R-EMPTY crash on Hospital`, and describe what happened.

Posts without a code, or with a code the server never got, are locked. Bug talk in #general is blocked: it goes here.

**Template** (copy the field names into your post)
> **Title:** `R-AB12 crash when the match ended`
> **What happened:** the game closed on the score screen
> **Map / mode:** Hospital, Supply Raid
> **What I did just before:** pressed Continue
> **How often:** every time / sometimes / once";
pub const REPORT_TITLE: &str = "How to report a bug";
pub const REPORT_TEXT: &str = "Bug reports for Twelve Weeks are handled through the launcher, so that every report reaches us with the logs needed to fix it.

**1. Send a report from the launcher**
Open the launcher, select **REPORT A BUG**, describe the problem and send it. The launcher then shows your report code, for example `R-AB12`.

**2. Post it in {forum}**
Create a post whose title begins with your code, for example `R-AB12 crash when the match ended`. Describe what happened, the map and mode, and what you were doing at the time.

If the report could not be uploaded, use `R-EMPTY` in place of the code.

Reports made anywhere else, or without a code, cannot be followed up.";
pub const GENERAL_RULE: &str = "TW bug talk in #general";
pub const GENERAL_BLOCK_MSG: &str = "Bugs go in #bug-reports. Send a report from the launcher (REPORT A BUG) first, then post its code there.";
pub const GENERAL_WORDS: [&str; 23] = [
    "bug", "bugs", "bugged", "buggy", "glitch*", "crash*", "freez*", "froze", "softlock*", "broken", "not working", "doesnt work", "doesn't work",
    "dont work", "don't work", "won't launch", "wont launch", "won't load", "wont load", "can't connect", "cant connect", "kicked me", "keeps kicking",
];
pub const GENERAL_ALLOW: [&str; 2] = ["debug*", "bugatti"];

fn word(c: char) -> bool {
    c.is_ascii_alphanumeric()
}

pub fn find_codes(text: &str) -> Vec<String> {
    let chars: Vec<char> = text.chars().collect();
    let mut out: Vec<String> = Vec::new();
    let mut i = 0;
    while i + 2 < chars.len() + 1 {
        let hit = (chars[i] == 'R' || chars[i] == 'r') && chars.get(i + 1) == Some(&'-') && (i == 0 || !word(chars[i - 1]));
        if hit {
            let rest = &chars[i + 2..];
            let four = rest.len() >= 4 && rest[..4].iter().all(|c| word(*c)) && rest.get(4).is_none_or(|c| !word(*c));
            let empty = rest.len() >= 5 && rest[..5].iter().collect::<String>().eq_ignore_ascii_case("EMPTY") && rest.get(5).is_none_or(|c| !word(*c));
            let len = if four { 4 } else if empty { 5 } else { 0 };
            if len > 0 {
                let code = format!("R-{}", rest[..len].iter().collect::<String>().to_ascii_uppercase());
                if !out.contains(&code) {
                    out.push(code);
                }
                i += 2 + len;
                continue;
            }
        }
        i += 1;
    }
    out
}

fn dir_code(name: &str) -> Option<(String, String)> {
    let b = name.as_bytes();
    if b.len() < 24 || !b[..8].iter().all(u8::is_ascii_digit) || b[8] != b'-' || !b[9..15].iter().all(u8::is_ascii_digit) {
        return None;
    }
    if &b[15..18] != b"_R-" || !b[18..22].iter().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()) || b[22] != b'_' {
        return None;
    }
    let (d, t) = (&name[..8], &name[9..15]);
    let stamp = format!("{}-{}-{}T{}:{}:{}Z", &d[..4], &d[4..6], &d[6..], &t[..2], &t[2..4], &t[4..]);
    Some((name[16..22].to_string(), stamp))
}

pub struct CodeIndex {
    pub dir: PathBuf,
    pub path: PathBuf,
    pub codes: Map<String, Value>,
    scanned: HashSet<String>,
    pending: HashSet<String>,
    mtime: Option<SystemTime>,
}

impl CodeIndex {
    pub fn new(dir: PathBuf, path: PathBuf) -> CodeIndex {
        let codes = read_json(&path).unwrap_or_default();
        CodeIndex { dir, path, codes, scanned: HashSet::new(), pending: HashSet::new(), mtime: None }
    }

    fn record(&mut self, n: &str, code: &str, stamp: &str) -> bool {
        let meta: Option<Value> = read_json(&self.dir.join(n).join("meta.json"));
        let v = meta.as_ref().and_then(|m| m.get("version")).and_then(Value::as_str).unwrap_or("").to_string();
        if meta.is_none() {
            self.pending.insert(n.to_string());
        } else {
            self.pending.remove(n);
        }
        let entry = json!({"t": stamp, "v": v, "dir": n});
        if self.codes.get(code) == Some(&entry) {
            return false;
        }
        self.codes.insert(code.to_string(), entry);
        true
    }

    pub fn scan(&mut self) -> usize {
        let mtime = std::fs::metadata(&self.dir).and_then(|m| m.modified()).ok();
        let mut new = 0;
        if mtime.is_none() || mtime != self.mtime {
            let Ok(rd) = std::fs::read_dir(&self.dir) else { return 0 };
            self.mtime = mtime;
            let mut names: Vec<String> = rd.flatten().filter_map(|e| e.file_name().to_str().map(str::to_string)).collect();
            names.sort_unstable();
            for n in names {
                if self.scanned.contains(&n) {
                    continue;
                }
                let Some((code, stamp)) = dir_code(&n) else { continue };
                self.scanned.insert(n.clone());
                let same_dir = self.codes.get(&code).and_then(|c| c.get("dir")).and_then(Value::as_str) == Some(n.as_str());
                if same_dir {
                    continue;
                }
                if self.record(&n, &code, &stamp) {
                    new += 1;
                }
            }
        }
        for n in self.pending.clone() {
            if let Some((code, stamp)) = dir_code(&n) {
                if self.dir.join(&n).join("meta.json").exists() && self.record(&n, &code, &stamp) {
                    new += 1;
                }
            }
        }
        if new > 0 {
            if let Err(e) = write_json(&self.path, &self.codes) {
                log!("bug desk: {}: {e}", self.path.display());
            }
        }
        new
    }

    pub fn get(&self, code: &str) -> Option<&Value> {
        self.codes.get(code)
    }

    pub fn report_dir(&self, code: &str) -> Option<PathBuf> {
        let d = self.codes.get(code)?.get("dir")?.as_str().filter(|d| !d.is_empty())?;
        let p = self.dir.join(d);
        p.is_dir().then_some(p)
    }
}

#[derive(Debug, PartialEq)]
pub enum Kind {
    Found,
    Empty,
    Unknown,
    None,
}

pub fn verdict(codes: &[String], index: &CodeIndex) -> (Kind, Option<String>, Option<Value>) {
    let real: Vec<&String> = codes.iter().filter(|c| c.as_str() != "R-EMPTY").collect();
    for c in &real {
        if let Some(e) = index.get(c).filter(|e| e.as_object().is_some_and(|o| !o.is_empty())) {
            return (Kind::Found, Some(c.to_string()), Some(e.clone()));
        }
    }
    if let Some(c) = real.first() {
        return (Kind::Unknown, Some(c.to_string()), None);
    }
    if codes.iter().any(|c| c == "R-EMPTY") {
        return (Kind::Empty, Some("R-EMPTY".into()), None);
    }
    (Kind::None, None, None)
}

#[derive(Debug, PartialEq)]
pub struct Plan {
    pub reply: String,
    pub tag: Option<&'static str>,
    pub rename: Option<String>,
    pub lock: bool,
}

pub fn plan(kind: &Kind, code: Option<&str>, entry: Option<&Value>, thread_name: &str) -> Plan {
    match kind {
        Kind::Found => {
            let code = code.unwrap_or("");
            let e = entry.cloned().unwrap_or(Value::Null);
            let t: String = e.get("t").and_then(Value::as_str).unwrap_or("").replace('T', " ").chars().take(16).collect();
            let v = e.get("v").and_then(Value::as_str).unwrap_or("");
            let build = if v.is_empty() { String::new() } else { format!(", build {v}") };
            let rename = (!thread_name.to_lowercase().contains(&code.to_lowercase())).then(|| truncate_chars(&format!("{code} {thread_name}"), 100));
            Plan { reply: format!("Report **{code}** found (received {t} UTC{build}). Thanks, the team will look at it."), tag: Some("Report found"), rename, lock: false }
        }
        Kind::Empty => Plan {
            reply: "**R-EMPTY**: a report without logs. Describe exactly what happened, the map and mode; clips help a lot. Next time, try REPORT A BUG once more without the clip.".into(),
            tag: Some("No logs"),
            rename: None,
            lock: false,
        },
        _ => {
            let why = if *kind == Kind::Unknown {
                format!("**{}** is not on the server. Check the code the launcher showed after sending.", code.unwrap_or("None"))
            } else {
                "There is no report code in this post.".to_string()
            };
            Plan {
                reply: format!(
                    "{why}\n\nSend the report first: launcher -> **REPORT A BUG**. Then make a **new** post whose title starts with the code, like `R-AB12 crash when the match ended`. The upload failed? Use `R-EMPTY`. Rules and template: the pinned post. This post is locked."
                ),
                tag: None,
                rename: None,
                lock: true,
            }
        }
    }
}

pub fn known_text(items: &[Value]) -> String {
    let mut lines = vec!["**Known issues** (kept up to date by the team)".to_string()];
    let fixed_of = |x: &Value| x.get("fixed").filter(|f| !f.is_null() && f.as_str() != Some("") && f.as_bool() != Some(false)).cloned();
    let text_of = |x: &Value| x.get("text").and_then(Value::as_str).unwrap_or("").to_string();
    let open: Vec<(usize, &Value)> = items.iter().enumerate().filter(|(_, x)| fixed_of(x).is_none()).map(|(i, x)| (i + 1, x)).collect();
    let fixed: Vec<(usize, &Value)> = items.iter().enumerate().filter(|(_, x)| fixed_of(x).is_some()).map(|(i, x)| (i + 1, x)).collect();
    if open.is_empty() && fixed.is_empty() {
        lines.push("Nothing listed right now.".into());
    }
    for (i, x) in &open {
        lines.push(format!("{i}. {}", text_of(x)));
    }
    if !fixed.is_empty() {
        lines.push("\n**Fixed**".into());
        for (i, x) in &fixed {
            let f = match fixed_of(x) {
                Some(Value::String(s)) => s,
                Some(v) => v.to_string(),
                None => String::new(),
            };
            lines.push(format!("{i}. ~~{}~~ fixed in build {f}", text_of(x)));
        }
    }
    truncate_chars(&lines.join("\n"), 2000)
}

pub enum KnownOp {
    Add(String),
    Fixed(i64, String),
    Remove(i64),
}

pub fn known_edit(path: &Path, op: KnownOp) -> Result<Vec<Value>, String> {
    let mut items: Vec<Value> = read_json(path).unwrap_or_default();
    let check = |n: i64, len: usize| {
        if n == 0 || n < 1 || n as usize > len {
            Err(format!("no issue number {n} (1..{len})"))
        } else {
            Ok((n - 1) as usize)
        }
    };
    match op {
        KnownOp::Add(text) => items.push(json!({"text": truncate_chars(&text, 300), "fixed": null, "added": today()})),
        KnownOp::Fixed(n, build) => {
            let i = check(n, items.len())?;
            if let Some(o) = items[i].as_object_mut() {
                o.insert("fixed".into(), json!(build));
            }
        }
        KnownOp::Remove(n) => {
            let i = check(n, items.len())?;
            items.remove(i);
        }
    }
    write_json(path, &items).map_err(|e| format!("{}: {e}", path.display()))?;
    Ok(items)
}

pub struct Desk {
    pub st: Map<String, Value>,
    pub ready: bool,
    pub index: CodeIndex,
}

impl Desk {
    pub fn new(dir: &Path, reports: PathBuf) -> Desk {
        Desk { st: read_json(&dir.join(STATE)).unwrap_or_default(), ready: false, index: CodeIndex::new(reports, dir.join(CODES)) }
    }

    pub fn id(&self, key: &str) -> Option<u64> {
        self.st.get(key).and_then(parse_id)
    }
}

pub fn report_message(st: &Map<String, Value>, guild: u64, user: Option<u64>) -> Value {
    let forum = st.get("forum").and_then(parse_id);
    let guide = st.get("guide").and_then(parse_id);
    let forum_ref = forum.map(|f| format!("<#{f}>")).unwrap_or_else(|| "#bug-reports".into());
    let mut data = json!({
        "embeds": [{"title": REPORT_TITLE, "description": REPORT_TEXT.replace("{forum}", &forum_ref), "color": 0xC8102E,
                    "footer": {"text": "Twelve Weeks  |  Bug reports"}}],
        "allowed_mentions": {"parse": [], "users": user.map(|u| vec![u.to_string()]).unwrap_or_default()},
    });
    if let Some(u) = user {
        data["content"] = json!(format!("<@{u}>"));
    }
    if let (Some(_), Some(g), true) = (forum, guide, guild != 0) {
        data["components"] = json!([{"type": 1, "components": [{"type": 2, "style": 5, "label": "Read the reporting guide",
            "url": format!("https://discord.com/channels/{guild}/{g}")}]}]);
    }
    data
}

fn save(bot: &Bot, st: &Map<String, Value>) {
    let p = bot.dir.join(STATE);
    if let Err(e) = write_json(&p, st) {
        log!("bug desk: {}: {e}", p.display());
    }
}

async fn setup(bot: &Arc<Bot>) -> Result<(), String> {
    let Some(cfg) = bot.cfg.desk.as_ref() else { return Ok(()) };
    let guild = bot.cache().guild;
    let chans_path = bot.dir.join(CHANNELS);
    let chans_before: Map<String, Value> = read_json(&chans_path).unwrap_or_default();
    let mut chans = chans_before.clone();
    let st_forum = bot.desk().id("forum");
    let forum = st_forum.filter(|f| bot.cache().chans.get(f).is_some_and(|c| c.kind == FORUM));
    let forum = match forum {
        Some(f) => f,
        None => {
            let parent = bot.cache().category_named("SUPPORT").map(|c| c.id);
            let mut body = json!({"name": "bug-reports", "type": FORUM, "topic": FORUM_TOPIC,
                "available_tags": TAGS.iter().map(|t| json!({"name": t})).collect::<Vec<_>>()});
            if let Some(p) = parent {
                body["parent_id"] = json!(p.to_string());
            }
            let made = bot.rest.post(&format!("/guilds/{guild}/channels"), &body, Some("#bug-reports forum (code required)")).await.map_err(|e| format!("forum: {e}"))?;
            let id = made.get("id").and_then(parse_id).ok_or("forum: no id")?;
            if let Ok(ch) = serde_json::from_value::<Channel>(made) {
                bot.cache().put_channel(ch);
            }
            {
                let mut d = bot.desk();
                d.st.insert("forum".into(), json!(id));
                d.st.remove("guide");
                save(bot, &d.st);
            }
            chans.insert("bug-reports".into(), json!(id));
            log!("bug desk: forum created {id}");
            id
        }
    };
    if chans != chans_before {
        if let Err(e) = write_json(&chans_path, &chans) {
            log!("bug desk: {}: {e}", chans_path.display());
        }
    }
    let guide = bot.desk().id("guide");
    let guide_ok = match guide {
        Some(g) => match bot.rest.get(&format!("/channels/{g}")).await {
            Ok(_) => true,
            Err(e) if e.status == 404 || e.status == 403 => false,
            Err(e) => return Err(format!("guide check: {e}")),
        },
        None => false,
    };
    if !guide_ok {
        let th = bot
            .rest
            .post(&format!("/channels/{forum}/threads"), &json!({"name": GUIDE_TITLE, "message": {"content": GUIDE}}), Some("bug report guide"))
            .await
            .map_err(|e| format!("guide: {e}"))?;
        let th_id = th.get("id").and_then(parse_id).ok_or("guide: no id")?;
        let items: Vec<Value> = read_json(&bot.dir.join(KNOWN)).unwrap_or_default();
        let km = bot.rest.post(&format!("/channels/{th_id}/messages"), &json!({"content": known_text(&items)}), None).await.map_err(|e| format!("known list: {e}"))?;
        let km_id = km.get("id").and_then(parse_id).ok_or("known list: no id")?;
        bot.rest.patch(&format!("/channels/{th_id}"), &json!({"flags": 2, "locked": true}), None).await.map_err(|e| format!("guide pin: {e}"))?;
        let mut d = bot.desk();
        d.st.insert("guide".into(), json!(th_id));
        d.st.insert("known_msg".into(), json!(km_id));
        save(bot, &d.st);
        log!("bug desk: guide post {th_id}");
    }
    if cfg.general_automod {
        automod(bot).await?;
    }
    bot.desk().ready = true;
    Ok(())
}

pub fn exempt_channels(bot: &Bot) -> Option<(Vec<u64>, Vec<u64>)> {
    let cache = bot.cache();
    let gen = cache.general()?.id;
    let mut chans: Vec<_> = cache.chans.values().filter(|c| c.kind != CATEGORY && c.id != gen).collect();
    chans.sort_by_key(|c| (c.position, c.id));
    let exempt: Vec<u64> = chans.iter().take(50).map(|c| c.id).collect();
    let mut roles: Vec<u64> = cache.roles.values().filter(|r| STAFF_ROLES.contains(&r.name.as_str())).map(|r| r.id).collect();
    roles.sort_unstable();
    Some((exempt, roles))
}

pub async fn automod(bot: &Arc<Bot>) -> Result<(), String> {
    let Some((exempt, roles)) = exempt_channels(bot) else {
        log!("bug desk: no #general, automod skipped");
        return Ok(());
    };
    let guild = bot.cache().guild;
    let rules = bot.rest.get(&format!("/guilds/{guild}/auto-moderation/rules")).await.map_err(|e| format!("automod: {e}"))?;
    let rule = rules.as_array().and_then(|a| a.iter().find(|r| r.get("name").and_then(Value::as_str) == Some(GENERAL_RULE)).cloned());
    let ids = |v: &[u64]| v.iter().map(|i| i.to_string()).collect::<Vec<_>>();
    match rule {
        None => {
            let body = json!({
                "name": GENERAL_RULE, "event_type": 1, "trigger_type": 1,
                "trigger_metadata": {"keyword_filter": GENERAL_WORDS, "allow_list": GENERAL_ALLOW},
                "actions": [{"type": 1, "metadata": {"custom_message": GENERAL_BLOCK_MSG}}],
                "enabled": true, "exempt_roles": ids(&roles), "exempt_channels": ids(&exempt),
            });
            bot.rest.post(&format!("/guilds/{guild}/auto-moderation/rules"), &body, Some("bug talk goes to #bug-reports")).await.map_err(|e| format!("automod: {e}"))?;
            log!("bug desk: automod rule created ({} exempt channels)", exempt.len());
        }
        Some(r) => {
            let have: HashSet<u64> = r.get("exempt_channels").and_then(Value::as_array).map(|a| a.iter().filter_map(parse_id).collect()).unwrap_or_default();
            let want: HashSet<u64> = exempt.iter().copied().collect();
            if have != want {
                let id = r.get("id").and_then(parse_id).ok_or("automod: rule without id")?;
                bot.rest
                    .patch(&format!("/guilds/{guild}/auto-moderation/rules/{id}"), &json!({"exempt_channels": ids(&exempt)}), Some("keep only #general covered"))
                    .await
                    .map_err(|e| format!("automod: {e}"))?;
                log!("bug desk: automod exempt list synced ({})", exempt.len());
            }
        }
    }
    Ok(())
}

pub async fn run(bot: Arc<Bot>) {
    loop {
        let ready = bot.desk().ready;
        if !ready {
            if let Err(e) = setup(&bot).await {
                log!("bug desk: {e}");
            }
        }
        bot.desk().index.scan();
        let wake = bot.automod_dirty.notified();
        tokio::select! {
            _ = tokio::time::sleep(Duration::from_secs(60)) => {}
            _ = wake => {
                tokio::time::sleep(Duration::from_secs(30)).await;
                let auto = bot.cfg.desk.as_ref().is_some_and(|c| c.general_automod);
                if auto && bot.desk().ready {
                    if let Err(e) = automod(&bot).await {
                        log!("bug desk: {e}");
                    }
                }
            }
        }
    }
}

fn staff(bot: &Bot, m: &Member) -> bool {
    let Some(u) = &m.user else { return false };
    if u.bot {
        return true;
    }
    let cache = bot.cache();
    let p = cache.permissions(u.id, &m.roles);
    if p & (MANAGE_GUILD | MANAGE_THREADS) != 0 {
        return true;
    }
    m.roles.iter().any(|r| cache.roles.get(r).is_some_and(|r| STAFF_ROLES.contains(&r.name.as_str())))
}

async fn starter(bot: &Bot, th: u64) -> Option<String> {
    for w in STARTER_WAITS {
        tokio::time::sleep(Duration::from_secs(w)).await;
        match bot.rest.get(&format!("/channels/{th}/messages/{th}")).await {
            Ok(v) => return serde_json::from_value::<Message>(v).ok().map(|m| m.content),
            Err(e) if e.status == 404 => continue,
            Err(e) => {
                log!("bug desk: post {th}: starter message: {e}");
                continue;
            }
        }
    }
    None
}

pub async fn on_thread_create(bot: Arc<Bot>, th: Channel) {
    let (ready, forum, guide) = {
        let d = bot.desk();
        (d.ready, d.id("forum"), d.id("guide"))
    };
    if !ready || th.parent_id.is_none() || th.parent_id != forum || Some(th.id) == guide {
        return;
    }
    let guild = bot.cache().guild;
    let owner_id = th.owner_id.unwrap_or(0);
    let owner: Option<Member> = match bot.rest.get(&format!("/guilds/{guild}/members/{owner_id}")).await {
        Ok(v) => serde_json::from_value(v).ok(),
        Err(_) => None,
    };
    if owner.as_ref().is_some_and(|m| staff(&bot, m)) {
        return;
    }
    let Some(first) = starter(&bot, th.id).await else {
        log!("bug desk: post {}: no starter message, skipped", th.id);
        return;
    };
    let name = th.name.clone().unwrap_or_default();
    let codes = find_codes(&format!("{name}\n{first}"));
    let mut v = {
        let mut d = bot.desk();
        d.index.scan();
        verdict(&codes, &d.index)
    };
    if v.0 == Kind::Unknown {
        for w in RETRY_S {
            tokio::time::sleep(Duration::from_secs(w)).await;
            let mut d = bot.desk();
            d.index.scan();
            v = verdict(&codes, &d.index);
            if v.0 != Kind::Unknown {
                break;
            }
        }
    }
    let (kind, code, entry) = v;
    let p = plan(&kind, code.as_deref(), entry.as_ref(), &name);
    if let Err(e) = bot.rest.post(&format!("/channels/{}/messages", th.id), &json!({"content": p.reply, "allowed_mentions": {"parse": []}}), None).await {
        log!("bug desk: post check: {e}");
        return;
    }
    let tags = bot.cache().chans.get(&th.parent_id.unwrap_or(0)).map(|c| c.tags.clone()).unwrap_or_default();
    let mut patch = Map::new();
    if let Some(t) = p.tag.and_then(|t| tags.iter().find(|(_, n)| n == t)) {
        if !th.applied_tags.contains(&t.0) {
            let mut applied = th.applied_tags.clone();
            applied.push(t.0);
            applied.truncate(5);
            patch.insert("applied_tags".into(), json!(applied.iter().map(|i| i.to_string()).collect::<Vec<_>>()));
        }
    }
    if let Some(n) = &p.rename {
        patch.insert("name".into(), json!(n));
    }
    if p.lock {
        patch.insert("locked".into(), json!(true));
        patch.insert("archived".into(), json!(true));
    }
    if !patch.is_empty() {
        let reason = p.lock.then_some("no valid report code");
        if let Err(e) = bot.rest.patch(&format!("/channels/{}", th.id), &Value::Object(patch), reason).await {
            log!("bug desk: post {}: edit: {e}", th.id);
        }
    }
    let code = code.unwrap_or_else(|| "None".into());
    match kind {
        Kind::Found => {
            let dir = bot.desk().index.report_dir(&code);
            if let Some(d) = dir {
                let who = owner.as_ref().and_then(|m| m.user.as_ref()).map(|u| u.tag()).unwrap_or_else(|| owner_id.to_string());
                let line = format!("https://discord.com/channels/{guild}/{} {who} {}\n", th.id, iso_utc());
                let res = std::fs::OpenOptions::new().create(true).append(true).open(d.join("discord.txt")).and_then(|mut f| f.write_all(line.as_bytes()));
                if let Err(e) = res {
                    log!("bug desk: discord.txt for {code}: {e}");
                }
            }
            log!("bug desk: post {} ok {code}", th.id);
        }
        Kind::Empty => log!("bug desk: post {} R-EMPTY", th.id),
        Kind::Unknown => log!("bug desk: post {} locked: unknown {code}", th.id),
        Kind::None => log!("bug desk: post {} locked: none None", th.id),
    }
}

pub async fn refresh_known(bot: &Bot) -> Result<(), String> {
    let (guide, msg) = {
        let d = bot.desk();
        (d.id("guide"), d.id("known_msg"))
    };
    let (Some(g), Some(m)) = (guide, msg) else { return Err("no known-issues message".into()) };
    let items: Vec<Value> = read_json(&bot.dir.join(KNOWN)).unwrap_or_default();
    let body = json!({"content": known_text(&items)});
    match bot.rest.patch(&format!("/channels/{g}/messages/{m}"), &body, None).await {
        Ok(_) => Ok(()),
        Err(e) if e.code == 50083 => {
            bot.rest.patch(&format!("/channels/{g}"), &json!({"archived": false}), None).await.map_err(|e| e.to_string())?;
            bot.rest.patch(&format!("/channels/{g}/messages/{m}"), &body, None).await.map(|_| ()).map_err(|e| e.to_string())
        }
        Err(e) => Err(e.to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("twb-{tag}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn codes_in_text() {
        assert_eq!(find_codes("R-AB12 crash"), vec!["R-AB12"]);
        assert_eq!(find_codes("r-ab12 and R-EMPTY and R-AB12"), vec!["R-AB12", "R-EMPTY"]);
        assert_eq!(find_codes("XR-AB12 R-ABCDE R-AB1 R-EMPTYX"), Vec::<String>::new());
        assert_eq!(find_codes("(R-x9z1)"), vec!["R-X9Z1"]);
        assert_eq!(find_codes("é R-1234é"), vec!["R-1234"]);
        assert_eq!(find_codes("R-"), Vec::<String>::new());
        assert_eq!(find_codes("R-R-ABCD"), vec!["R-ABCD"]);
    }

    #[test]
    fn report_dirs() {
        assert_eq!(dir_code("20261025-172418_R-T2W3_DeSyncc"), Some(("R-T2W3".into(), "2026-10-25T17:24:18Z".into())));
        assert_eq!(dir_code("20261025-172418_R-t2w3_x"), None);
        assert_eq!(dir_code("20261025-172418_R-T2W3"), None);
        assert_eq!(dir_code("2026102-172418_R-T2W3_x"), None);
    }

    #[test]
    fn index_and_verdict() {
        let d = tmpdir("idx");
        let reports = d.join("reports");
        std::fs::create_dir_all(reports.join("20261123-033454_R-MLAN_Suki")).unwrap();
        std::fs::write(reports.join("20261123-033454_R-MLAN_Suki/meta.json"), br#"{"version": "cz1117u"}"#).unwrap();
        std::fs::create_dir_all(reports.join("20261123-040000_R-NOMT_x")).unwrap();
        std::fs::write(d.join("codes.json"), br#"{"R-OLD1": {"t": "2026-10-25T17:24:18Z", "v": "", "dir": "gone"}}"#).unwrap();
        let mut idx = CodeIndex::new(reports.clone(), d.join("codes.json"));
        assert_eq!(idx.scan(), 2);
        assert_eq!(idx.scan(), 0);
        assert_eq!(idx.get("R-MLAN").unwrap()["v"], "cz1117u");
        assert_eq!(idx.get("R-NOMT").unwrap()["v"], "");
        std::fs::write(reports.join("20261123-040000_R-NOMT_x/meta.json"), br#"{"version": "abc"}"#).unwrap();
        assert_eq!(idx.scan(), 1);
        assert_eq!(idx.get("R-NOMT").unwrap()["v"], "abc");
        assert!(idx.report_dir("R-MLAN").is_some());
        assert!(idx.report_dir("R-OLD1").is_none());
        let saved: Map<String, Value> = read_json(&d.join("codes.json")).unwrap();
        assert_eq!(saved.keys().collect::<Vec<_>>(), vec!["R-OLD1", "R-MLAN", "R-NOMT"]);
        let c = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(verdict(&c(&["R-EMPTY", "R-OLD1"]), &idx).0, Kind::Found);
        assert_eq!(verdict(&c(&["R-ZZZZ", "R-MLAN"]), &idx).1.as_deref(), Some("R-MLAN"));
        assert_eq!(verdict(&c(&["R-ZZZZ", "R-EMPTY"]), &idx).0, Kind::Unknown);
        assert_eq!(verdict(&c(&["R-EMPTY"]), &idx).0, Kind::Empty);
        assert_eq!(verdict(&[], &idx).0, Kind::None);
    }

    #[test]
    fn plans() {
        let e = json!({"t": "2026-10-25T17:24:18Z", "v": "5dabd6c6", "dir": "x"});
        let p = plan(&Kind::Found, Some("R-T2W3"), Some(&e), "crash on overpass");
        assert_eq!(p.reply, "Report **R-T2W3** found (received 2026-10-25 17:24 UTC, build 5dabd6c6). Thanks, the team will look at it.");
        assert_eq!(p.rename.as_deref(), Some("R-T2W3 crash on overpass"));
        assert!(plan(&Kind::Found, Some("R-T2W3"), Some(&e), "r-t2w3 crash").rename.is_none());
        let u = plan(&Kind::Unknown, Some("R-ZZZZ"), None, "x");
        assert!(u.lock && u.reply.starts_with("**R-ZZZZ** is not on the server."));
        assert!(plan(&Kind::None, None, None, "x").reply.starts_with("There is no report code in this post.\n\n"));
        assert_eq!(plan(&Kind::Empty, Some("R-EMPTY"), None, "x").tag, Some("No logs"));
    }

    #[test]
    fn known_list() {
        assert_eq!(known_text(&[]), "**Known issues** (kept up to date by the team)\nNothing listed right now.");
        let d = tmpdir("known");
        let p = d.join("known.json");
        known_edit(&p, KnownOp::Add("Crash on Overpass".into())).unwrap();
        known_edit(&p, KnownOp::Add("Audio cuts".into())).unwrap();
        let items = known_edit(&p, KnownOp::Fixed(1, "152".into())).unwrap();
        assert_eq!(known_text(&items), "**Known issues** (kept up to date by the team)\n2. Audio cuts\n\n**Fixed**\n1. ~~Crash on Overpass~~ fixed in build 152");
        assert_eq!(known_edit(&p, KnownOp::Remove(3)).unwrap_err(), "no issue number 3 (1..2)");
        assert_eq!(known_edit(&p, KnownOp::Remove(0)).unwrap_err(), "no issue number 0 (1..2)");
        assert_eq!(known_edit(&p, KnownOp::Remove(2)).unwrap().len(), 1);
    }

    #[test]
    fn report_payload() {
        let mut st = Map::new();
        st.insert("forum".into(), json!(10));
        st.insert("guide".into(), json!(11));
        let v = report_message(&st, 1, Some(5));
        assert_eq!(v["content"], "<@5>");
        assert!(v["embeds"][0]["description"].as_str().unwrap().contains("**2. Post it in <#10>**"));
        assert_eq!(v["components"][0]["components"][0]["url"], "https://discord.com/channels/1/11");
        let w = report_message(&Map::new(), 1, None);
        assert!(w.get("components").is_none() && w.get("content").is_none());
        assert!(w["embeds"][0]["description"].as_str().unwrap().contains("in #bug-reports**"));
    }
}
