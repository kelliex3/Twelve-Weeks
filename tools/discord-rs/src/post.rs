use crate::config::Config;
use crate::desk::CHANNELS;
use crate::model::Message;
use crate::net::{Body, File, Rest};
use crate::util::{parse_id, read_json};
use hyper::Method;
use serde_json::{json, Map, Value};
use std::path::Path;
use std::time::Duration;

pub const POSTABLE: [&str; 9] = ["welcome", "rules", "choose-your-faction", "announcements", "patch-notes", "lfg", "wlf", "seraphites", "files"];

fn banner(name: &str) -> Option<&'static str> {
    Some(match name {
        "welcome" => "banner-welcome.png",
        "rules" => "banner-rules.png",
        "choose-your-faction" => "banner-choose-faction.png",
        "announcements" => "banner-announcements.png",
        "patch-notes" => "banner-patch-notes.png",
        "files" => "banner-files.png",
        "lfg" => "banner-lfg.png",
        "wlf" => "banner-wlf.png",
        "seraphites" => "banner-seraphites.png",
        _ => return None,
    })
}

pub fn role_buttons() -> Value {
    json!([
        {"type": 1, "components": [
            {"type": 2, "style": 2, "label": "WLF", "custom_id": "tw:faction:wlf"},
            {"type": 2, "style": 4, "label": "Seraphites", "custom_id": "tw:faction:ser"}]},
        {"type": 1, "components": [
            {"type": 2, "style": 1, "label": "Sneak peek pings", "custom_id": "tw:ping:sneak"},
            {"type": 2, "style": 3, "label": "LFG pings", "custom_id": "tw:ping:lfg"}]}
    ])
}

pub fn download_buttons(downloads: &[(String, String)]) -> Value {
    let rows: Vec<Value> = downloads
        .chunks(5)
        .map(|c| json!({"type": 1, "components": c.iter().map(|(l, u)| json!({"type": 2, "style": 5, "label": l, "url": u})).collect::<Vec<_>>()}))
        .collect();
    Value::Array(rows)
}

pub struct Item {
    pub file: String,
    pub components: Value,
}

pub fn items(name: &str, downloads: &[(String, String)]) -> Vec<Item> {
    let plain = |f: &str| Item { file: f.to_string(), components: json!([]) };
    let banner_item = || banner(name).map(plain).into_iter().collect::<Vec<_>>();
    match name {
        "welcome" => vec![plain("welcome-hero.png"), plain("welcome-guide.png")],
        "rules" => {
            let mut v = banner_item();
            v.push(plain("rules.png"));
            v
        }
        "choose-your-faction" => {
            let mut v = banner_item();
            v.push(Item { file: "choose-your-faction.png".into(), components: role_buttons() });
            v
        }
        "files" => vec![Item { file: "banner-files.png".into(), components: download_buttons(downloads) }],
        _ => banner_item(),
    }
}

async fn history(rest: &Rest, chan: u64, all: bool) -> Result<Vec<Message>, String> {
    let mut out: Vec<Message> = Vec::new();
    let mut after = 0u64;
    loop {
        let page = rest.get(&format!("/channels/{chan}/messages?limit=100&after={after}")).await.map_err(|e| e.to_string())?;
        let mut msgs: Vec<Message> = serde_json::from_value(page).map_err(|e| e.to_string())?;
        msgs.sort_by_key(|m| m.id);
        let n = msgs.len();
        if let Some(last) = msgs.last() {
            after = last.id;
        }
        out.extend(msgs);
        if !all || n < 100 {
            return Ok(out);
        }
    }
}

fn payload(it: &Item) -> Value {
    json!({"content": null, "embeds": [], "components": it.components, "attachments": [{"id": 0, "filename": it.file}], "allowed_mentions": {"parse": []}})
}

async fn repost(rest: &Rest, me: u64, art: &Path, name: &str, chan: u64, items: &[Item], wipe: bool) -> Result<(), String> {
    let first = history(rest, chan, false).await?;
    let mine: Vec<u64> = first.iter().filter(|m| m.author.as_ref().map(|a| a.id) == Some(me)).map(|m| m.id).collect();
    if wipe {
        for m in history(rest, chan, true).await? {
            if m.author.as_ref().map(|a| a.id) != Some(me) {
                rest.delete(&format!("/channels/{chan}/messages/{}", m.id), None).await.map_err(|e| e.to_string())?;
                tokio::time::sleep(Duration::from_millis(400)).await;
            }
        }
    }
    let mut files = Vec::new();
    for it in items {
        let p = art.join(&it.file);
        let data = std::fs::read(&p).map_err(|e| format!("{}: {e}", p.display()))?;
        files.push(File { name: it.file.clone(), data });
    }
    if mine.len() == items.len() {
        for ((m, it), f) in mine.iter().zip(items).zip(files) {
            let body = payload(it);
            rest.call(Method::PATCH, &format!("/channels/{chan}/messages/{m}"), Body::Multipart(&body, &[f]), None, true).await.map_err(|e| e.to_string())?;
            tokio::time::sleep(Duration::from_millis(600)).await;
        }
        println!("#{name}: {} post(s) updated in place", items.len());
    } else {
        for m in &mine {
            rest.delete(&format!("/channels/{chan}/messages/{m}"), None).await.map_err(|e| e.to_string())?;
        }
        for (it, f) in items.iter().zip(files) {
            let body = payload(it);
            rest.call(Method::POST, &format!("/channels/{chan}/messages"), Body::Multipart(&body, &[f]), None, true).await.map_err(|e| e.to_string())?;
            tokio::time::sleep(Duration::from_millis(600)).await;
        }
        println!("#{name}: posted {}", items.len());
    }
    Ok(())
}

pub async fn run(cfg: &Config, dir: &Path, names: &[String]) -> Result<(), String> {
    let rest = Rest::new(&cfg.token)?;
    let me = rest.get("/users/@me").await.map_err(|e| e.to_string())?.get("id").and_then(parse_id).ok_or("no bot user id")?;
    let ids: Map<String, Value> = read_json(&dir.join(CHANNELS)).unwrap_or_default();
    let chans = rest.get(&format!("/guilds/{}/channels", cfg.guild_id)).await.map_err(|e| e.to_string())?;
    let chans = chans.as_array().cloned().unwrap_or_default();
    let art = dir.join("art");
    for n in names {
        let by_id = ids.get(n).and_then(parse_id).filter(|id| chans.iter().any(|c| c.get("id").and_then(parse_id) == Some(*id)));
        let by_name = || {
            let mut text: Vec<&Value> = chans
                .iter()
                .filter(|c| matches!(c.get("type").and_then(Value::as_u64), Some(0) | Some(5)) && c.get("name").and_then(Value::as_str) == Some(n.as_str()))
                .collect();
            text.sort_by_key(|c| (c.get("position").and_then(Value::as_i64).unwrap_or(0), c.get("id").and_then(parse_id).unwrap_or(0)));
            text.first().and_then(|c| c.get("id")).and_then(parse_id)
        };
        let Some(chan) = by_id.or_else(by_name) else {
            println!("#{n} not found");
            continue;
        };
        if n == "files" {
            let mut dead = None;
            for (_, url) in &cfg.downloads {
                match rest.fetch(Method::HEAD, url, "TwelveWeeksBot/1.0", 0).await {
                    Ok((200, _)) => {}
                    _ => {
                        dead = Some(url.clone());
                        break;
                    }
                }
            }
            if let Some(url) = dead {
                println!("#downloads left as is: download link not live yet ({url})");
                continue;
            }
        }
        if let Err(e) = repost(&rest, me, &art, n, chan, &items(n, &cfg.downloads), n == "files").await {
            println!("#{n}: POST FAILED: {e}");
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn post_items() {
        let dl = vec![("Windows launcher".to_string(), "https://x/a.exe".to_string()), ("Linux".to_string(), "https://x/a.AppImage".to_string())];
        let names = |n: &str| items(n, &dl).iter().map(|i| i.file.clone()).collect::<Vec<_>>();
        assert_eq!(names("welcome"), ["welcome-hero.png", "welcome-guide.png"]);
        assert_eq!(names("rules"), ["banner-rules.png", "rules.png"]);
        assert_eq!(names("choose-your-faction"), ["banner-choose-faction.png", "choose-your-faction.png"]);
        assert_eq!(names("wlf"), ["banner-wlf.png"]);
        let f = items("files", &dl);
        assert_eq!(f[0].components[0]["components"][1]["url"], "https://x/a.AppImage");
        assert_eq!(items("choose-your-faction", &dl)[1].components[1]["components"][1]["custom_id"], "tw:ping:lfg");
    }
}
