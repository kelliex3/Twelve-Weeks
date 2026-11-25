use crate::util::{expand_user, parse_id};
use serde::Deserialize;
use serde_json::Value;
use std::path::{Path, PathBuf};

pub const DEFAULT_DOWNLOADS: [(&str, &str); 2] = [
    ("Windows launcher", "https://dl.twelve-weeks.com/download/TwelveWeeks-Launcher-0.1.37.exe"),
    ("Linux / Steam Deck (AppImage)", "https://dl.twelve-weeks.com/download/TwelveWeeks-Launcher-0.1.37.AppImage"),
];

#[derive(Deserialize, Default)]
struct RawConfig {
    #[serde(default)]
    token: Option<String>,
    #[serde(default)]
    guild_id: Option<Value>,
    #[serde(default)]
    downloads: Option<Vec<(String, String)>>,
    #[serde(default)]
    live_counts: Option<RawLive>,
    #[serde(default)]
    bug_desk: Option<RawDesk>,
}

#[derive(Deserialize, Default)]
struct RawLive {
    #[serde(default)]
    enabled: Option<bool>,
    #[serde(default)]
    category: Option<String>,
    #[serde(default)]
    position: Option<i64>,
    #[serde(default)]
    status_file: Option<String>,
    #[serde(default)]
    launchers_file: Option<String>,
    #[serde(default)]
    channels: Option<Vec<(String, String)>>,
    #[serde(default)]
    offline: Option<String>,
    #[serde(default)]
    min_rename_s: Option<f64>,
}

#[derive(Deserialize, Default)]
struct RawDesk {
    #[serde(default)]
    enabled: Option<bool>,
    #[serde(default)]
    reports_dir: Option<String>,
    #[serde(default)]
    general_automod: Option<bool>,
}

pub struct LiveCfg {
    pub category: String,
    pub position: Option<i64>,
    pub status_file: PathBuf,
    pub launchers_file: PathBuf,
    pub channels: Vec<(String, String)>,
    pub offline: String,
    pub min_rename_s: f64,
}

pub struct DeskCfg {
    pub reports_dir: PathBuf,
    pub general_automod: bool,
}

pub struct Config {
    pub token: String,
    pub guild_id: u64,
    pub downloads: Vec<(String, String)>,
    pub live: Option<LiveCfg>,
    pub desk: Option<DeskCfg>,
}

fn nonempty(s: Option<String>) -> Option<String> {
    s.filter(|v| !v.is_empty())
}

pub fn default_live_channels() -> Vec<(String, String)> {
    [
        ("supply-raid", "Supply Raid: {n} in match"),
        ("interrogation", "Interrogation: {n} in match"),
        ("lobby", "In lobby: {n}"),
        ("menus", "In menus: {n}"),
        ("launcher", "Launcher open: {n}"),
    ]
    .iter()
    .map(|(a, b)| (a.to_string(), b.to_string()))
    .collect()
}

impl Config {
    pub fn load(path: &Path) -> Result<Config, String> {
        let data = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        Config::parse(&data)
    }

    pub fn parse(data: &[u8]) -> Result<Config, String> {
        let raw: RawConfig = serde_json::from_slice(data).map_err(|e| format!("config.json: {e}"))?;
        let token = nonempty(raw.token).ok_or("config.json: token missing")?;
        let guild_id = raw.guild_id.as_ref().and_then(parse_id).filter(|g| *g != 0).ok_or("config.json: guild_id missing")?;
        let downloads = raw.downloads.filter(|d| !d.is_empty()).unwrap_or_else(|| {
            DEFAULT_DOWNLOADS.iter().map(|(a, b)| (a.to_string(), b.to_string())).collect()
        });
        let live = raw.live_counts.filter(|l| l.enabled == Some(true)).map(|l| {
            let status_file = nonempty(l.status_file)
                .map(PathBuf::from)
                .unwrap_or_else(|| expand_user("~/t2r-live/data/t2r-status.json"));
            let launchers_file = nonempty(l.launchers_file).map(PathBuf::from).unwrap_or_else(|| {
                status_file.parent().map(|p| p.join("t2r-launchers.json")).unwrap_or_else(|| PathBuf::from("t2r-launchers.json"))
            });
            LiveCfg {
                category: nonempty(l.category).unwrap_or_else(|| "LIVE PLAYERS".into()),
                position: l.position,
                status_file,
                launchers_file,
                channels: l.channels.filter(|c| !c.is_empty()).unwrap_or_else(default_live_channels),
                offline: nonempty(l.offline).unwrap_or_else(|| "{label}: offline".into()),
                min_rename_s: l.min_rename_s.filter(|v| *v > 0.0).unwrap_or(310.0),
            }
        });
        let desk = raw.bug_desk.filter(|d| d.enabled == Some(true)).map(|d| DeskCfg {
            reports_dir: expand_user(&nonempty(d.reports_dir).unwrap_or_else(|| "~/t2r-live/update-data/reports".into())),
            general_automod: d.general_automod.unwrap_or(true),
        });
        Ok(Config { token, guild_id, downloads, live, desk })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn production_shape() {
        let c = Config::parse(
            br#"{"token": "x", "guild_id": 1234,
                "downloads": [["Windows launcher", "https://a/x.exe"], ["Linux", "https://a/x.AppImage"]],
                "live_counts": {"enabled": true, "category": "LIVE PLAYERS", "position": 0,
                    "channels": [["supply-raid", "Supply Raid: {n} in match"], ["lobby", "In lobby: {n}"]]},
                "bug_desk": {"enabled": true, "reports_dir": "/srv/reports", "delete_help": true, "general_automod": true}}"#,
        )
        .unwrap();
        assert_eq!(c.guild_id, 1234);
        assert_eq!(c.downloads.len(), 2);
        let l = c.live.unwrap();
        assert_eq!(l.position, Some(0));
        assert_eq!(l.channels.len(), 2);
        assert!(l.launchers_file.ends_with("t2r-launchers.json"));
        assert_eq!(l.min_rename_s, 310.0);
        let d = c.desk.unwrap();
        assert_eq!(d.reports_dir, PathBuf::from("/srv/reports"));
        assert!(d.general_automod);
    }

    #[test]
    fn string_guild_and_disabled_parts() {
        let c = Config::parse(br#"{"token": "x", "guild_id": "99", "live_counts": {"enabled": false}, "downloads": []}"#).unwrap();
        assert_eq!(c.guild_id, 99);
        assert!(c.live.is_none());
        assert!(c.desk.is_none());
        assert_eq!(c.downloads[0].0, "Windows launcher");
        assert!(Config::parse(br#"{"guild_id": 1}"#).is_err());
    }
}
