use serde::{Deserialize, Deserializer};
use serde_json::value::RawValue;

pub fn id<'de, D: Deserializer<'de>>(d: D) -> Result<u64, D::Error> {
    #[derive(Deserialize)]
    #[serde(untagged)]
    enum Raw<'a> {
        S(&'a str),
        Owned(String),
        N(u64),
    }
    match Raw::deserialize(d)? {
        Raw::S(s) => s.parse().map_err(serde::de::Error::custom),
        Raw::Owned(s) => s.parse().map_err(serde::de::Error::custom),
        Raw::N(n) => Ok(n),
    }
}

pub fn opt_id<'de, D: Deserializer<'de>>(d: D) -> Result<Option<u64>, D::Error> {
    #[derive(Deserialize)]
    struct W(#[serde(deserialize_with = "id")] u64);
    Ok(Option::<W>::deserialize(d)?.map(|w| w.0))
}

pub fn ids<'de, D: Deserializer<'de>>(d: D) -> Result<Vec<u64>, D::Error> {
    #[derive(Deserialize)]
    struct W(#[serde(deserialize_with = "id")] u64);
    Ok(Option::<Vec<W>>::deserialize(d)?.unwrap_or_default().into_iter().map(|w| w.0).collect())
}

pub fn bits<'de, D: Deserializer<'de>>(d: D) -> Result<u64, D::Error> {
    Ok(Option::<String>::deserialize(d)?.and_then(|s| s.parse().ok()).unwrap_or(0))
}

#[derive(Deserialize)]
pub struct Envelope<'a> {
    pub op: u8,
    #[serde(borrow, default)]
    pub t: Option<&'a str>,
    #[serde(borrow, default)]
    pub d: Option<&'a RawValue>,
}

#[derive(Deserialize, Clone, Default)]
pub struct User {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(default)]
    pub username: String,
    #[serde(default)]
    pub discriminator: Option<String>,
    #[serde(default)]
    pub global_name: Option<String>,
    #[serde(default)]
    pub avatar: Option<String>,
    #[serde(default)]
    pub bot: bool,
}

impl User {
    pub fn tag(&self) -> String {
        match self.discriminator.as_deref() {
            Some(d) if d != "0" && !d.is_empty() => format!("{}#{}", self.username, d),
            _ => self.username.clone(),
        }
    }
}

#[derive(Deserialize, Clone, Default)]
pub struct Member {
    #[serde(default)]
    pub user: Option<User>,
    #[serde(default)]
    pub nick: Option<String>,
    #[serde(default)]
    pub avatar: Option<String>,
    #[serde(default, deserialize_with = "ids")]
    pub roles: Vec<u64>,
    #[serde(default, deserialize_with = "opt_id")]
    pub guild_id: Option<u64>,
}

impl Member {
    pub fn display_name(&self) -> String {
        if let Some(n) = self.nick.as_deref().filter(|n| !n.is_empty()) {
            return n.to_string();
        }
        match &self.user {
            Some(u) => u.global_name.clone().filter(|g| !g.is_empty()).unwrap_or_else(|| u.username.clone()),
            None => String::new(),
        }
    }

    pub fn avatar_url(&self, guild: u64) -> Option<String> {
        let u = self.user.as_ref()?;
        if let Some(h) = &self.avatar {
            return Some(format!("https://cdn.discordapp.com/guilds/{guild}/users/{}/avatars/{h}.png?size=256", u.id));
        }
        if let Some(h) = &u.avatar {
            return Some(format!("https://cdn.discordapp.com/avatars/{}/{h}.png?size=256", u.id));
        }
        let idx = match u.discriminator.as_deref().and_then(|d| d.parse::<u64>().ok()) {
            Some(d) if d != 0 => d % 5,
            _ => (u.id >> 22) % 6,
        };
        Some(format!("https://cdn.discordapp.com/embed/avatars/{idx}.png"))
    }
}

#[derive(Deserialize, Clone)]
pub struct Overwrite {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(rename = "type", default)]
    pub kind: u8,
    #[serde(default, deserialize_with = "bits")]
    pub allow: u64,
    #[serde(default, deserialize_with = "bits")]
    pub deny: u64,
}

#[derive(Deserialize, Clone)]
pub struct Tag {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(default)]
    pub name: String,
}

#[derive(Deserialize, Clone)]
pub struct Channel {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(rename = "type", default)]
    pub kind: u8,
    #[serde(default)]
    pub name: Option<String>,
    #[serde(default, deserialize_with = "opt_id")]
    pub guild_id: Option<u64>,
    #[serde(default, deserialize_with = "opt_id")]
    pub parent_id: Option<u64>,
    #[serde(default)]
    pub position: Option<i64>,
    #[serde(default)]
    pub permission_overwrites: Option<Vec<Overwrite>>,
    #[serde(default)]
    pub available_tags: Option<Vec<Tag>>,
    #[serde(default)]
    pub newly_created: Option<bool>,
    #[serde(default, deserialize_with = "opt_id")]
    pub owner_id: Option<u64>,
    #[serde(default, deserialize_with = "ids")]
    pub applied_tags: Vec<u64>,
}

#[derive(Deserialize, Clone)]
pub struct Role {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub position: i64,
    #[serde(default, deserialize_with = "bits")]
    pub permissions: u64,
}

#[derive(Deserialize)]
pub struct Guild {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(default)]
    pub unavailable: Option<bool>,
    #[serde(default, deserialize_with = "opt_id")]
    pub owner_id: Option<u64>,
    #[serde(default)]
    pub member_count: Option<u64>,
    #[serde(default)]
    pub roles: Vec<Role>,
    #[serde(default)]
    pub channels: Vec<Channel>,
}

#[derive(Deserialize)]
pub struct Ready {
    pub user: User,
    pub application: ReadyApp,
    #[serde(default)]
    pub guilds: Vec<ReadyGuild>,
}

#[derive(Deserialize)]
pub struct ReadyGuild {
    #[serde(deserialize_with = "id")]
    pub id: u64,
}

#[derive(Deserialize)]
pub struct ReadyApp {
    #[serde(deserialize_with = "id")]
    pub id: u64,
}

#[derive(Deserialize)]
pub struct RoleEvent {
    #[serde(deserialize_with = "id")]
    pub guild_id: u64,
    pub role: Role,
}

#[derive(Deserialize)]
pub struct RoleDelete {
    #[serde(deserialize_with = "id")]
    pub guild_id: u64,
    #[serde(deserialize_with = "id")]
    pub role_id: u64,
}

#[derive(Deserialize)]
pub struct MemberRemove {
    #[serde(deserialize_with = "id")]
    pub guild_id: u64,
}

#[derive(Deserialize, Clone)]
pub struct Attachment {
    #[serde(default)]
    pub filename: String,
    #[serde(default)]
    pub url: String,
}

#[derive(Deserialize)]
pub struct Interaction {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(deserialize_with = "id")]
    pub application_id: u64,
    #[serde(rename = "type")]
    pub kind: u8,
    pub token: String,
    #[serde(default, deserialize_with = "opt_id")]
    pub guild_id: Option<u64>,
    #[serde(default)]
    pub channel: Option<Channel>,
    #[serde(default, deserialize_with = "opt_id")]
    pub channel_id: Option<u64>,
    #[serde(default)]
    pub member: Option<Member>,
    #[serde(default)]
    pub data: Option<serde_json::Value>,
}

impl Interaction {
    pub fn user_id(&self) -> u64 {
        self.member.as_ref().and_then(|m| m.user.as_ref()).map(|u| u.id).unwrap_or(0)
    }

    pub fn channel_id(&self) -> u64 {
        self.channel.as_ref().map(|c| c.id).or(self.channel_id).unwrap_or(0)
    }
}

#[derive(Deserialize)]
pub struct Message {
    #[serde(deserialize_with = "id")]
    pub id: u64,
    #[serde(default)]
    pub content: String,
    #[serde(default)]
    pub author: Option<User>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn member_names_and_avatars() {
        let m: Member = serde_json::from_str(
            r#"{"user": {"id": "530770100000000001", "username": "abby", "discriminator": "0", "global_name": "Abby A", "avatar": "abc"}, "nick": null, "roles": ["1", "2"]}"#,
        )
        .unwrap();
        assert_eq!(m.display_name(), "Abby A");
        assert_eq!(m.roles, vec![1, 2]);
        assert_eq!(m.avatar_url(5).unwrap(), "https://cdn.discordapp.com/avatars/530770100000000001/abc.png?size=256");
        let n: Member = serde_json::from_str(r#"{"user": {"id": "530770100000000001", "username": "abby"}, "nick": "Nick", "avatar": "g1"}"#).unwrap();
        assert_eq!(n.display_name(), "Nick");
        assert_eq!(n.avatar_url(5).unwrap(), "https://cdn.discordapp.com/guilds/5/users/530770100000000001/avatars/g1.png?size=256");
        let d: Member = serde_json::from_str(r#"{"user": {"id": "530770100000000001", "username": "abby", "discriminator": "0"}}"#).unwrap();
        assert_eq!(d.avatar_url(5).unwrap(), format!("https://cdn.discordapp.com/embed/avatars/{}.png", (530770100000000001u64 >> 22) % 6));
    }

    #[test]
    fn channel_from_gateway() {
        let c: Channel = serde_json::from_str(
            r#"{"id": "10", "type": 15, "name": "bug-reports", "parent_id": "3", "position": 1, "guild_id": "1",
                "permission_overwrites": [{"id": "1", "type": 0, "allow": "1024", "deny": "2048"}],
                "available_tags": [{"id": "77", "name": "Report found", "moderated": false, "emoji_id": null, "emoji_name": null}]}"#,
        )
        .unwrap();
        assert_eq!(c.kind, 15);
        assert_eq!(c.parent_id, Some(3));
        let ow = c.permission_overwrites.unwrap();
        assert_eq!((ow[0].allow, ow[0].deny), (1024, 2048));
        assert_eq!(c.available_tags.unwrap()[0].id, 77);
    }
}
