use crate::model::{Channel, Guild, Overwrite, Role};
use std::collections::HashMap;

pub const TEXT: u8 = 0;
pub const VOICE: u8 = 2;
pub const CATEGORY: u8 = 4;
pub const NEWS: u8 = 5;
pub const FORUM: u8 = 15;

pub const ADMINISTRATOR: u64 = 1 << 3;
pub const MANAGE_GUILD: u64 = 1 << 5;
pub const MANAGE_THREADS: u64 = 1 << 34;

#[derive(Clone)]
pub struct Chan {
    pub id: u64,
    pub name: String,
    pub kind: u8,
    pub parent: Option<u64>,
    pub position: i64,
    pub overwrites: Vec<Overwrite>,
    pub tags: Vec<(u64, String)>,
}

impl Chan {
    pub fn is_text(&self) -> bool {
        self.kind == TEXT || self.kind == NEWS
    }
}

#[derive(Default)]
pub struct Cache {
    pub guild: u64,
    pub owner: u64,
    pub me: u64,
    pub app: u64,
    pub me_name: String,
    pub member_count: u64,
    pub roles: HashMap<u64, Role>,
    pub chans: HashMap<u64, Chan>,
}

fn is_thread(kind: u8) -> bool {
    matches!(kind, 10..=12)
}

impl Cache {
    pub fn load_guild(&mut self, g: Guild) {
        self.guild = g.id;
        self.owner = g.owner_id.unwrap_or(0);
        if let Some(n) = g.member_count {
            self.member_count = n;
        }
        self.roles = g.roles.into_iter().map(|r| (r.id, r)).collect();
        self.chans.clear();
        for c in g.channels {
            self.put_channel(c);
        }
    }

    pub fn put_channel(&mut self, c: Channel) {
        if is_thread(c.kind) {
            return;
        }
        let prev = self.chans.remove(&c.id);
        let chan = Chan {
            id: c.id,
            name: c.name.or_else(|| prev.as_ref().map(|p| p.name.clone())).unwrap_or_default(),
            kind: c.kind,
            parent: c.parent_id,
            position: c.position.or_else(|| prev.as_ref().map(|p| p.position)).unwrap_or(0),
            overwrites: c.permission_overwrites.or_else(|| prev.as_ref().map(|p| p.overwrites.clone())).unwrap_or_default(),
            tags: c
                .available_tags
                .map(|t| t.into_iter().map(|t| (t.id, t.name)).collect())
                .or_else(|| prev.map(|p| p.tags))
                .unwrap_or_default(),
        };
        self.chans.insert(chan.id, chan);
    }

    pub fn role_named(&self, name: &str) -> Option<&Role> {
        self.roles.values().filter(|r| r.name == name).min_by_key(|r| (r.position, r.id))
    }

    pub fn text_named(&self, name: &str) -> Option<&Chan> {
        self.chans.values().filter(|c| c.is_text() && c.name == name).min_by_key(|c| (c.position, c.id))
    }

    pub fn any_named(&self, name: &str) -> Option<&Chan> {
        self.chans.values().filter(|c| c.name == name).min_by_key(|c| (c.position, c.id))
    }

    pub fn categories(&self) -> Vec<&Chan> {
        let mut v: Vec<&Chan> = self.chans.values().filter(|c| c.kind == CATEGORY).collect();
        v.sort_by_key(|c| (c.position, c.id));
        v
    }

    pub fn category_named(&self, name: &str) -> Option<&Chan> {
        self.categories().into_iter().find(|c| c.name == name)
    }

    pub fn mention(&self, name: &str) -> String {
        match self.text_named(name) {
            Some(c) => format!("<#{}>", c.id),
            None => format!("#{name}"),
        }
    }

    pub fn permissions(&self, user: u64, roles: &[u64]) -> u64 {
        if user != 0 && user == self.owner {
            return u64::MAX;
        }
        let mut p = self.roles.get(&self.guild).map(|r| r.permissions).unwrap_or(0);
        for r in roles {
            if let Some(role) = self.roles.get(r) {
                p |= role.permissions;
            }
        }
        if p & ADMINISTRATOR != 0 {
            u64::MAX
        } else {
            p
        }
    }

    pub fn general(&self) -> Option<&Chan> {
        self.chans.values().filter(|c| c.kind == TEXT && c.name == "general").find(|c| {
            c.parent.and_then(|p| self.chans.get(&p)).map(|p| p.name == "COMMUNITY").unwrap_or(false)
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn guild() -> Guild {
        serde_json::from_str(
            r#"{"id": "1", "owner_id": "9", "member_count": 140,
                "roles": [{"id": "1", "name": "@everyone", "position": 0, "permissions": "1024"},
                          {"id": "20", "name": "Moderator", "position": 5, "permissions": "17179869184"},
                          {"id": "21", "name": "Admin", "position": 6, "permissions": "8"},
                          {"id": "22", "name": "LFG", "position": 2, "permissions": "0"},
                          {"id": "23", "name": "LFG", "position": 1, "permissions": "0"}],
                "channels": [{"id": "100", "type": 4, "name": "COMMUNITY", "position": 2},
                             {"id": "101", "type": 0, "name": "general", "parent_id": "100", "position": 0},
                             {"id": "102", "type": 0, "name": "general", "position": 9},
                             {"id": "103", "type": 5, "name": "rules", "position": 1},
                             {"id": "104", "type": 11, "name": "a thread", "parent_id": "103"}]}"#,
        )
        .unwrap()
    }

    #[test]
    fn lookups() {
        let mut c = Cache::default();
        c.load_guild(guild());
        assert_eq!(c.member_count, 140);
        assert_eq!(c.chans.len(), 4);
        assert_eq!(c.general().unwrap().id, 101);
        assert_eq!(c.mention("rules"), "<#103>");
        assert_eq!(c.mention("downloads"), "#downloads");
        assert_eq!(c.role_named("LFG").unwrap().id, 23);
        assert_eq!(c.permissions(5, &[20]), 1024 | MANAGE_THREADS);
        assert_eq!(c.permissions(5, &[21]), u64::MAX);
        assert_eq!(c.permissions(9, &[]), u64::MAX);
    }

    #[test]
    fn partial_update_keeps_fields() {
        let mut c = Cache::default();
        c.load_guild(guild());
        c.put_channel(serde_json::from_str(r#"{"id": "103", "type": 5, "name": "rules-2"}"#).unwrap());
        let ch = &c.chans[&103];
        assert_eq!(ch.name, "rules-2");
        assert_eq!(ch.position, 1);
    }
}
