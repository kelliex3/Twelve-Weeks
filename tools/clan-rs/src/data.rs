use serde::Deserialize;
use std::collections::{HashMap, HashSet};
use std::sync::OnceLock;

#[derive(Deserialize)]
pub struct Faction {
    pub plural: String,
    pub enemy: String,
    pub enemies: String,
    pub leader: String,
    pub places: Vec<String>,
}

#[derive(Deserialize)]
pub struct TplInfo {
    pub l: String,
    pub k: String,
    pub c: bool,
    pub w: bool,
    pub s: bool,
}

#[derive(Deserialize)]
pub struct Story {
    pub first: Vec<String>,
    pub female: HashSet<String>,
    pub extra_w: Vec<(String, String)>,
    pub extra_s: Vec<(String, String)>,
    pub neutral: HashSet<String>,
    pub last: Vec<String>,
    pub roles: Vec<(String, String, String, f64)>,
    pub illness: Vec<(String, String, String, String, f64, f64)>,
    pub faction: HashMap<String, Faction>,
    pub infected: Vec<String>,
    pub rare: String,
    pub rare_chance: f64,
    pub healthy: Vec<String>,
    pub sick: Vec<String>,
    pub hungry: Vec<String>,
    pub kid_plain: HashMap<String, (String, String)>,
    pub rich: Vec<(String, String)>,
    pub solo: HashMap<String, Vec<(String, String)>>,
    pub tpl_pool: HashMap<String, String>,
    pub cause_group: HashMap<String, String>,
    pub solo_death: HashMap<String, (String, String, String)>,
    pub feeling: HashSet<String>,
    pub crowd_re: String,
    pub group_re: String,
    pub tpls: HashMap<String, TplInfo>,
    pub pron: HashMap<String, HashMap<String, String>>,
    pub nums: Vec<String>,
    pub numw: Vec<String>,
    pub gainw: Vec<String>,
}

#[derive(Deserialize)]
pub struct Pl {
    pub reg: HashMap<String, (String, String, String)>,
    pub places: HashMap<String, Vec<String>>,
    pub enemy: HashMap<String, Vec<String>>,
    pub inf: HashMap<String, Vec<String>>,
    pub ill: HashMap<String, Vec<String>>,
    pub cure: HashMap<String, Vec<String>>,
    pub ago: HashMap<String, String>,
    pub card_nom: Vec<String>,
    pub card_gen: Vec<String>,
    pub special: HashMap<String, String>,
    pub roles: HashMap<String, [[String; 2]; 2]>,
}

#[derive(Deserialize)]
pub struct AlTpl {
    pub s: String,
    pub t: String,
    pub en: String,
    pub pl: String,
    pub l: String,
    pub ve: Vec<String>,
    pub vp: Vec<String>,
    pub k: String,
    pub c: bool,
}

#[derive(Deserialize)]
pub struct Alife {
    pub reg: HashMap<String, AlTpl>,
    pub by_pool: HashMap<String, Vec<String>>,
    pub traits: Vec<String>,
    pub fact_w: HashMap<String, i64>,
    pub tail_ok: HashSet<String>,
    pub persistent: HashSet<String>,
    pub daily_facts: usize,
    pub ticker_share: usize,
    pub min_clan: usize,
    pub mem_causes: HashSet<String>,
    pub places: HashMap<String, HashMap<String, Vec<String>>>,
    pub goods: HashMap<String, Vec<String>>,
    pub projects: HashMap<String, Vec<String>>,
    pub groups: HashMap<String, Vec<String>>,
    pub injuries: HashMap<String, Vec<String>>,
    pub keeps: HashMap<String, Vec<String>>,
    pub jobs: Vec<Vec<String>>,
    pub origins: Vec<Vec<String>>,
    pub skills: HashMap<String, Vec<String>>,
    pub prog: HashMap<String, Vec<String>>,
    pub seasons: Vec<(String, String)>,
    pub weather: Vec<String>,
    pub job_combat: HashSet<String>,
    pub origin_combat: HashSet<String>,
    pub skill_combat: HashSet<String>,
    pub ze_re: String,
    pub combat_en: String,
    pub combat_pl: String,
    pub combat_vars: HashSet<String>,
}

#[derive(Deserialize)]
pub struct Chron {
    pub reg: HashMap<String, (String, String)>,
    pub whens: Vec<(String, String)>,
    #[serde(rename = "use")]
    pub use_: HashMap<String, (String, String, String, String, String, String, String, f64)>,
    pub keep: HashMap<String, (String, String, f64)>,
    pub map_short: HashMap<String, String>,
    pub map_pl: HashMap<String, String>,
}

#[derive(Deserialize)]
pub struct Data {
    pub story: Story,
    pub pl: Pl,
    pub alife: Alife,
    pub chron: Chron,
}

pub struct Res {
    pub crowd: regex::Regex,
    pub group: regex::Regex,
    pub pl_group: regex::Regex,
    pub ze: regex::Regex,
    pub combat_en: regex::Regex,
    pub combat_pl: regex::Regex,
    pub sentence: regex::Regex,
}

static RAW: &str = include_str!("../data/clan.json");

pub fn data() -> Option<&'static Data> {
    static D: OnceLock<Option<Data>> = OnceLock::new();
    D.get_or_init(|| serde_json::from_str(RAW).ok()).as_ref()
}

pub fn res() -> Option<&'static Res> {
    static R: OnceLock<Option<Res>> = OnceLock::new();
    R.get_or_init(|| {
        let d = data()?;
        let re = |p: &str| regex::Regex::new(p).ok();
        Some(Res {
            crowd: re(&format!("(?i){}", d.story.crowd_re))?,
            group: re(&format!("(?i){}", d.story.group_re))?,
            pl_group: re(r"(?i)\b(?:my|nas|nam|nami|nasz\w*|nikt\w*|wszyscy|wszystkich|\w+(?:śmy|emy|imy|amy|ymy))\b")?,
            ze: re(&d.alife.ze_re)?,
            combat_en: re(&format!(r"(?i)(?:^|[^\w'])(?:{})(?:$|[^\w'])", d.alife.combat_en))?,
            combat_pl: re(&format!(r"(?i)(?:^|\W)(?:{})(?:$|\W)", d.alife.combat_pl))?,
            sentence: re(r"[^.!?]*[.!?]+(?:\s+|$)|[^.!?]+$")?,
        })
    })
    .as_ref()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn loads() {
        let d = data().expect("clan.json parses");
        assert!(d.pl.reg.len() > 1000);
        assert!(d.alife.reg.len() > 500);
        assert!(res().is_some());
    }
}
