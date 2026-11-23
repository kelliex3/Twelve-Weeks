mod alife;
mod data;
pub mod pyrand;
pub mod rules;
mod story;
mod text;
mod view;

pub use rules::{booster_next, event_name, event_today, event_window, mission_goals, needed, token_id, week_day};
pub use rules::{ALIVE, DEAD, NONE, SERAPHITES, WEEKS, WIN_DAY, WLF, WON};
pub use story::{norm_lang, slot_of};
pub use view::{clan_view, clanfeed, resolve_key, valid_player, FEED_MAX};

pub fn menu_lines(conn: &rusqlite::Connection, key: &str, limit: usize, lang: &str, now: f64) -> Vec<(String, String)> {
    story::menu_lines(conn, key, limit, lang, now)
}

pub fn warm() -> bool {
    data::data().is_some()
}
