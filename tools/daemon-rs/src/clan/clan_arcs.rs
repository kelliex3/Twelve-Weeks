//! py: clan_arcs.py - clan story arcs: small state machines that run over several clan days (see the Python module doc). The ARCS table of the Python holds
//! functions (conditions, effects): hand-ported below; its texts are the generated `clan_arcs_text` tables. `cs` of the Python = `st["ch"]`.
use crate::clan::clan::{int_of, pystr, truthy};
use crate::clan::clan_chronicle::{self as ch, has_word, Day};
use crate::clan::ext_clan_chronicle::{self as ext, lex};
use crate::clan::{clan_kids as kd, data};
use serde_json::{json, Map, Value};
use std::collections::{BTreeSet, HashMap, HashSet};
use std::sync::LazyLock;
use t2r_clanstory::pyrand::PyRandom;

pub const MAX_ACTIVE: usize = 3;
pub const ARC_ROWS: usize = 2;
pub const COOLDOWN: i64 = 6;
pub const MAX_AGE: i64 = 14;
pub const LOG_KEEP: usize = 14;

fn tc(name: &str) -> &'static Value {
    data::t("clan_arcs", name)
}
fn at(name: &str) -> &'static Value {
    data::t("clan_arcs_text", name)
}

fn vi(v: &Value, k: &str) -> i64 {
    v.get(k).map(int_of).unwrap_or(0)
}
fn vs<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(|x| x.as_str()).unwrap_or("")
}
fn h(parts: &[String]) -> i64 {
    ch::h(parts) as i64
}

/// The generated constants equal the ones coded here.
pub fn consts_ok() -> Result<(), &'static str> {
    let chk = |n: &'static str, v: i64| if tc(n).as_i64() == Some(v) { Ok(()) } else { Err(n) };
    chk("MAX_ACTIVE", MAX_ACTIVE as i64)?;
    chk("ARC_ROWS", ARC_ROWS as i64)?;
    chk("COOLDOWN", COOLDOWN)?;
    chk("MAX_AGE", MAX_AGE)?;
    chk("LOG_KEEP", LOG_KEEP as i64)?;
    let w = tc("START_W").as_object().ok_or("START_W")?;
    if w.len() != START_W.len() || START_W.iter().any(|(k, x)| w.get(*k).and_then(|v| v.as_f64()) != Some(*x)) {
        return Err("START_W");
    }
    Ok(())
}

// ------------------------------------------------------------------ the day's context
pub struct Ctx<'a> {
    pub d: &'a mut Day,
    pub st: &'a mut Value,
    pub newc: &'a Value,
    pub r: Value,
    pub matchv: &'a Value,
    pub day: i64,
    pub morale: i64,
    pub byid: HashMap<i64, Value>,
}

impl Ctx<'_> {
    // py: clan_arcs.py:355 _Ctx.person
    pub fn person(&self, a: &Value, l: char) -> Option<&Value> {
        let id = a.get("c")?.get(l.to_string())?;
        self.byid.get(&int_of(id))
    }
    pub fn alive(&self, a: &Value, l: char) -> bool {
        a.get("c").and_then(|c| c.get(l.to_string())).map(|id| self.byid.contains_key(&int_of(id))).unwrap_or(false)
    }
    pub fn state(&self, a: &Value, l: char) -> Option<String> {
        self.person(a, l).map(|p| vs(p, "s").to_string())
    }
    fn cs(&self) -> &Value {
        &self.st["ch"]
    }
    fn inv(&self, k: &str) -> i64 {
        self.cs()["inv"].get(k).map(int_of).unwrap_or(0)
    }
    fn set_inv(&mut self, k: &str, v: i64) {
        self.st["ch"]["inv"][k] = json!(v);
    }
    /// The first stocked medicine in key order, or None.
    pub fn med_key(&self) -> Option<String> {
        let mut items: Vec<(&String, i64)> = self.cs()["inv"].as_object()?.iter().map(|(k, v)| (k, int_of(v))).collect();
        items.sort();
        items.into_iter().find(|(k, n)| *n > 0 && matches!(k.as_str(), "abx" | "bandage" | "alcohol")).map(|(k, _)| k.clone())
    }
    fn bump_morale(&mut self, n: i64) {
        let m = (self.st.get("morale").map(int_of).unwrap_or(50) + n).min(100);
        self.st["morale"] = json!(m);
    }
}

// ------------------------------------------------------------------ conditions (c = Ctx, a = arc)
pub type Cond = Box<dyn Fn(&Ctx, &Value) -> bool + Send + Sync>;
pub type Fx = Box<dyn Fn(&mut Ctx, &mut Value) -> Result<(), String> + Send + Sync>;

fn days(c: &Ctx, a: &Value) -> i64 {
    c.day - vi(a, "t")
}
fn dead(l: char) -> Cond {
    Box::new(move |c, a| !c.alive(a, l))
}
fn alive(l: char) -> Cond {
    Box::new(move |c, a| c.alive(a, l))
}
fn after(n: i64, conds: Vec<Cond>) -> Cond {
    Box::new(move |c, a| days(c, a) >= n && conds.iter().all(|f| f(c, a)))
}
fn sick(l: char, yes: bool) -> Cond {
    Box::new(move |c, a| (c.state(a, l).as_deref() == Some("s")) == yes)
}
fn sup_ok(c: &Ctx, _a: &Value) -> bool {
    vi(&c.r, "supplies") >= c.r.get("needed").map(int_of).unwrap_or(1)
}
fn morale_ge(n: i64) -> Cond {
    Box::new(move |c, _| c.morale >= n)
}
fn hmod(m: i64, k: i64) -> Cond {
    Box::new(move |_, a| vi(a, "h") % m == k)
}
fn both(fs: Vec<Cond>) -> Cond {
    Box::new(move |c, a| fs.iter().all(|f| f(c, a)))
}
fn has_med(c: &Ctx, a: &Value) -> bool {
    !a.get("v").and_then(|v| v.get("treated")).map(truthy).unwrap_or(false) && c.med_key().is_some()
}
fn ormorale_or_hash(n: i64, m: i64) -> Cond {
    Box::new(move |c, a| c.morale >= n || vi(a, "h") % m != 0)
}
fn steals(c: &Ctx, a: &Value) -> bool {
    vi(a, "h") % 3 == 0 && c.morale <= 55
}
/// The arc's own three-way die (independent of hmod): which ending a spore story takes.
fn h3(k: i64) -> Cond {
    Box::new(move |_, a| (vi(a, "h") >> 5) % 3 == k)
}
fn stranger_ok(c: &Ctx, a: &Value) -> bool {
    (c.morale >= 50 && sup_ok(c, a)) || vi(a, "h") % 4 == 0
}
fn has_food(c: &Ctx, _a: &Value) -> bool {
    ["pine", "beans"].iter().any(|k| c.inv(k) > 0)
}
fn f(g: fn(&Ctx, &Value) -> bool) -> Cond {
    Box::new(g)
}

fn fx_cake(c: &mut Ctx, a: &mut Value) -> Result<(), String> {
    if let Some(k) = ["pine", "beans"].into_iter().find(|k| c.inv(k) > 0) {
        a["v"]["it"] = json!(k);
        let n = c.inv(k);
        c.set_inv(k, (n - 1).max(0));
    }
    c.bump_morale(3);
    Ok(())
}
fn fx_trade(c: &mut Ctx, _a: &mut Value) -> Result<(), String> {
    let n = c.inv("bandage");
    c.set_inv("bandage", n + 2);
    c.bump_morale(2);
    Ok(())
}
fn fx_pendant(c: &mut Ctx, _a: &mut Value) -> Result<(), String> {
    c.bump_morale(2);
    Ok(())
}
/// The end of the spore story: the person leaves the roster for good (turned, mercy or a walk into the dark).
fn fx_spore_end(with_helper: bool) -> Fx {
    Box::new(move |c, a| {
        let Some(av) = c.person(a, 'A').cloned() else { return Ok(()) };
        let b = c.person(a, 'B').cloned();
        let helper = if with_helper { b.as_ref().filter(|b| !kd::is_kid(b)) } else { None };
        ext::story::finish_member(c.st, &av, c.day, "turned", helper);
        let sid = vs(a, "id").to_string();
        let mut rng = PyRandom::from_i64(h(&["spare".to_string(), sid, c.day.to_string()]));
        ext::story::spare_sick(&mut rng, c.st, c.day);
        c.byid.remove(&vi(&av, "i"));
        c.d.spot.insert(vi(&av, "i"));
        Ok(())
    })
}
fn fx_treat(c: &mut Ctx, a: &mut Value) -> Result<(), String> {
    let k = c.med_key().ok_or("KeyError: None")?;
    a["v"]["it"] = json!(k);
    a["v"]["treated"] = json!(1);
    let n = c.inv(&k);
    c.set_inv(&k, (n - 1).max(0));
    Ok(())
}
fn fx_share(c: &mut Ctx, _a: &mut Value) -> Result<(), String> {
    c.bump_morale(3);
    Ok(())
}
fn fx_bandages(c: &mut Ctx, _a: &mut Value) -> Result<(), String> {
    let n = c.inv("bandage");
    c.set_inv("bandage", n + 2);
    Ok(())
}

/// A node of an arc: `N(nxt, away, doom, fx, ok_dead, mood)`.
pub struct Node {
    pub nxt: Vec<(Cond, &'static str)>,
    pub away: &'static str,
    pub doom: Vec<(char, &'static str)>,
    pub fx: Option<Fx>,
    pub ok_dead: &'static str,
    pub mood: &'static str,
}

fn nd() -> Node {
    Node { nxt: Vec::new(), away: "", doom: Vec::new(), fx: None, ok_dead: "", mood: "worried" }
}
impl Node {
    fn nxt(mut self, v: Vec<(Cond, &'static str)>) -> Node {
        self.nxt = v;
        self
    }
    fn away(mut self, a: &'static str) -> Node {
        self.away = a;
        self
    }
    fn doom(mut self, d: &[(char, &'static str)]) -> Node {
        self.doom = d.to_vec();
        self
    }
    fn fx(mut self, g: fn(&mut Ctx, &mut Value) -> Result<(), String>) -> Node {
        self.fx = Some(Box::new(g));
        self
    }
    fn fxb(mut self, g: Fx) -> Node {
        self.fx = Some(g);
        self
    }
    fn ok(mut self, o: &'static str) -> Node {
        self.ok_dead = o;
        self
    }
    fn mood(mut self, m: &'static str) -> Node {
        self.mood = m;
        self
    }
}

type Nodes = HashMap<&'static str, Node>;

fn nodes(v: Vec<(&'static str, Node)>) -> Nodes {
    v.into_iter().collect()
}

// py: clan_arcs.py:166 ARCS
pub static ARCS: LazyLock<HashMap<&'static str, Nodes>> = LazyLock::new(|| {
    let mut m: HashMap<&'static str, Nodes> = HashMap::new();
    m.insert(
        "missing",
        nodes(vec![
            ("n0", nd().nxt(vec![(after(1, vec![alive('A')]), "search")]).away("A").mood("afraid")),
            (
                "search",
                nd()
                    .nxt(vec![
                        (both(vec![dead('A'), hmod(3, 2)]), "found_gut"),
                        (dead('A'), "found_dead"),
                        (after(2, vec![alive('A'), f(sup_ok)]), "alive"),
                        (after(4, vec![alive('A')]), "alive_late"),
                    ])
                    .away("AB")
                    .doom(&[('A', "bite")])
                    .mood("afraid"),
            ),
            ("found_dead", nd().nxt(vec![(after(1, vec![alive('B'), hmod(2, 0)]), "mercy"), (after(1, vec![alive('B')]), "despair")]).ok("A").mood("grieving")),
            ("mercy", nd().ok("A").mood("grieving")),
            ("found_gut", nd().nxt(vec![(after(1, vec![alive('B')]), "mercy_gut")]).ok("A").mood("grieving")),
            ("mercy_gut", nd().ok("A").mood("grieving")),
            ("despair", nd().nxt(vec![(dead('B'), "turned"), (after(3, vec![alive('B')]), "pulled")]).away("B").doom(&[('B', "bite")]).ok("A").mood("grieving")),
            ("turned", nd().ok("AB").mood("grieving")),
            ("pulled", nd().ok("A").mood("grieving")),
            ("alive", nd().mood("hopeful")),
            ("alive_late", nd().mood("hopeful")),
        ]),
    );
    m.insert(
        "spore",
        nodes(vec![
            ("sp0", nd().nxt(vec![(dead('A'), "remembered"), (after(1, vec![hmod(2, 0)]), "hide"), (after(1, vec![]), "confess")]).mood("afraid")),
            ("hide", nd().nxt(vec![(dead('A'), "remembered"), (after(1, vec![]), "signs")]).mood("afraid")),
            ("confess", nd().nxt(vec![(dead('A'), "remembered"), (after(1, vec![]), "signs")]).mood("grieving")),
            (
                "signs",
                nd().nxt(vec![(dead('A'), "remembered"), (after(1, vec![h3(0)]), "turn"), (after(1, vec![h3(1)]), "mercy"), (after(1, vec![]), "walk")]).mood("grim"),
            ),
            ("turn", nd().nxt(vec![(after(1, vec![alive('B')]), "after")]).fxb(fx_spore_end(true)).ok("A").mood("grieving")),
            ("mercy", nd().nxt(vec![(after(1, vec![alive('B')]), "after")]).fxb(fx_spore_end(true)).ok("A").mood("grieving")),
            ("walk", nd().nxt(vec![(after(1, vec![alive('B')]), "after")]).fxb(fx_spore_end(false)).ok("A").mood("grieving")),
            ("remembered", nd().nxt(vec![(after(1, vec![alive('B')]), "after")]).ok("A").mood("grieving")),
            ("after", nd().ok("A").mood("grieving")),
        ]),
    );
    m.insert(
        "stranger",
        nodes(vec![
            ("st0", nd().nxt(vec![(after(1, vec![f(stranger_ok)]), "in"), (after(1, vec![]), "out")]).mood("worried")),
            ("in", nd().nxt(vec![(after(2, vec![hmod(2, 0)]), "gift"), (after(2, vec![]), "cough")]).mood("hopeful").fx(fx_share)),
            ("gift", nd().mood("hopeful")),
            ("cough", nd().mood("calm")),
            ("out", nd().nxt(vec![(after(3, vec![]), "echo")]).mood("grim")),
            ("echo", nd().mood("calm")),
        ]),
    );
    m.insert(
        "caravan",
        nodes(vec![
            ("car0", nd().nxt(vec![(after(1, vec![f(sup_ok)]), "trade"), (after(1, vec![morale_ge(55)]), "trade"), (after(1, vec![]), "poor")]).mood("hopeful")),
            ("trade", nd().nxt(vec![(after(2, vec![]), "news")]).fx(fx_trade).mood("cheerful")),
            ("news", nd().mood("hopeful")),
            ("poor", nd().mood("grim")),
        ]),
    );
    m.insert(
        "pendant",
        nodes(vec![
            ("fp0", nd().nxt(vec![(after(1, vec![]), "tell")]).mood("calm").fx(fx_pendant)),
            ("tell", nd().nxt(vec![(after(1, vec![morale_ge(50)]), "worn"), (after(1, vec![]), "tin")]).mood("calm")),
            ("worn", nd().mood("hopeful")),
            ("tin", nd().mood("calm")),
        ]),
    );
    m.insert(
        "birthday",
        nodes(vec![
            ("bd0", nd().nxt(vec![(after(1, vec![f(has_food)]), "cake"), (after(1, vec![]), "promise")]).mood("cheerful")),
            ("cake", nd().fx(fx_cake).mood("cheerful")),
            ("promise", nd().nxt(vec![(after(3, vec![]), "kept")]).mood("calm")),
            ("kept", nd().mood("hopeful")),
        ]),
    );
    m.insert(
        "sick",
        nodes(vec![
            (
                "s0",
                nd().nxt(vec![(dead('A'), "death"), (sick('A', false), "recover"), (after(1, vec![sick('A', true), f(has_med)]), "treat"), (after(2, vec![sick('A', true)]), "hold")]),
            ),
            ("treat", nd().nxt(vec![(dead('A'), "death"), (sick('A', false), "recover"), (after(2, vec![sick('A', true)]), "hold")]).fx(fx_treat).mood("hopeful")),
            (
                "hold",
                nd().nxt(vec![(dead('A'), "death"), (sick('A', false), "recover"), (after(2, vec![sick('A', true), f(has_med)]), "treat")]),
            ),
            ("recover", nd().mood("hopeful")),
            ("death", nd().ok("A").mood("grieving")),
        ]),
    );
    m.insert(
        "hung",
        nodes(vec![
            ("h0", nd().nxt(vec![(after(1, vec![]), "confront")])),
            ("confront", nd().nxt(vec![(after(1, vec![ormorale_or_hash(45, 3)]), "forgiven"), (after(1, vec![]), "banish")])),
            ("forgiven", nd().mood("hopeful")),
            ("banish", nd().nxt(vec![(dead('A'), "gone"), (after(3, vec![alive('A')]), "relent")]).doom(&[('A', "walkout")]).mood("grim")),
            ("gone", nd().ok("A").mood("grim")),
            ("relent", nd().mood("hopeful")),
        ]),
    );
    m.insert(
        "new",
        nodes(vec![
            ("w0", nd().nxt(vec![(after(2, vec![]), "test")]).mood("hopeful")),
            ("test", nd().nxt(vec![(after(1, vec![f(steals)]), "steals"), (after(1, vec![]), "trust")])),
            ("trust", nd().mood("hopeful")),
            ("steals", nd().nxt(vec![(dead('A'), "gone"), (after(3, vec![alive('A')]), "returns")]).doom(&[('A', "walkout")]).mood("grim")),
            ("gone", nd().ok("A").mood("grim")),
            ("returns", nd().mood("hopeful")),
        ]),
    );
    m.insert(
        "keep",
        nodes(vec![
            ("k0", nd().nxt(vec![(after(2, vec![]), "found")]).mood("cheerful")),
            ("found", nd().nxt(vec![(after(1, vec![morale_ge(50)]), "share"), (after(1, vec![]), "hold")])),
            ("share", nd().fx(fx_share).mood("cheerful")),
            ("hold", nd().mood("calm")),
        ]),
    );
    m.insert(
        "birth",
        nodes(vec![
            ("b0", nd().nxt(vec![(after(3, vec![]), "prep")]).mood("hopeful")),
            ("prep", nd().nxt(vec![(after(3, vec![morale_ge(40)]), "born"), (after(3, vec![]), "hard")]).mood("hopeful")),
            ("born", nd().mood("cheerful")),
            ("hard", nd().mood("hopeful")),
        ]),
    );
    m.insert(
        "wish",
        nodes(vec![
            ("o0", nd().nxt(vec![(after(1, vec![]), "o1")]).mood("calm")),
            ("o1", nd().nxt(vec![(dead('A'), "after"), (after(3, vec![alive('A')]), "done")]).mood("hopeful")),
            ("done", nd().mood("cheerful")),
            ("after", nd().ok("A").mood("grieving")),
        ]),
    );
    m.insert(
        "blame",
        nodes(vec![
            ("q0", nd().nxt(vec![(after(2, vec![f(sup_ok)]), "made"), (after(2, vec![]), "split")]).mood("worried")),
            ("made", nd().mood("hopeful")),
            ("split", nd().mood("grim")),
        ]),
    );
    m.insert(
        "betray",
        nodes(vec![
            ("t0", nd().nxt(vec![(after(1, vec![]), "t1")]).mood("afraid")),
            ("t1", nd().nxt(vec![(after(1, vec![hmod(2, 0)]), "expose"), (after(1, vec![]), "innocent")]).mood("afraid")),
            ("expose", nd().nxt(vec![(dead('A'), "gone"), (after(3, vec![alive('A')]), "pardon")]).doom(&[('A', "walkout")]).mood("grim")),
            ("gone", nd().ok("A").mood("grim")),
            ("pardon", nd().mood("calm")),
            ("innocent", nd().fx(fx_bandages).mood("calm")),
        ]),
    );
    m
});

pub const FIRST: [(&str, &str); 14] = [
    ("missing", "n0"),
    ("sick", "s0"),
    ("hung", "h0"),
    ("new", "w0"),
    ("keep", "k0"),
    ("birth", "b0"),
    ("wish", "o0"),
    ("blame", "q0"),
    ("betray", "t0"),
    ("spore", "sp0"),
    ("stranger", "st0"),
    ("caravan", "car0"),
    ("pendant", "fp0"),
    ("birthday", "bd0"),
];

fn first_of(kind: &str) -> &'static str {
    FIRST.iter().find(|(k, _)| *k == kind).map(|(_, n)| *n).unwrap_or("")
}

/// The hand-ported ARCS table has the shape of the generated one (nodes, targets, away, doom, ok_dead, mood, which have an effect); the conditions are
/// checked by the differential test. Also: every node has text and every target exists.
pub fn arcs_ok() -> Result<(), String> {
    let want = tc("ARCS").as_object().ok_or("ARCS")?;
    if want.len() != ARCS.len() {
        return Err("arc count".into());
    }
    for (kind, wnodes) in want {
        let mine = ARCS.get(kind.as_str()).ok_or_else(|| format!("arc {}", kind))?;
        let wn = wnodes.as_object().ok_or("nodes")?;
        if wn.len() != mine.len() {
            return Err(format!("{}: node count", kind));
        }
        for (name, w) in wn {
            let m = mine.get(name.as_str()).ok_or_else(|| format!("{}.{}", kind, name))?;
            let tag = |s: &str| format!("{}.{}: {}", kind, name, s);
            if vs(w, "away") != m.away || vs(w, "ok_dead") != m.ok_dead || vs(w, "mood") != m.mood {
                return Err(tag("away/ok_dead/mood"));
            }
            let wd: Vec<(String, String)> = w.get("doom").and_then(|d| d.as_object()).map(|d| d.iter().map(|(k, v)| (k.clone(), pystr(v))).collect()).unwrap_or_default();
            let md: Vec<(String, String)> = m.doom.iter().map(|(k, v)| (k.to_string(), v.to_string())).collect();
            if wd != md {
                return Err(tag("doom"));
            }
            if w.get("fx").map(|x| !x.is_null()).unwrap_or(false) != m.fx.is_some() {
                return Err(tag("fx"));
            }
            let wt: Vec<String> = w.get("nxt").and_then(|a| a.as_array()).map(|a| a.iter().map(|p| pystr(&p[1])).collect()).unwrap_or_default();
            let mt: Vec<String> = m.nxt.iter().map(|(_, t)| t.to_string()).collect();
            if wt != mt {
                return Err(tag("targets"));
            }
            if !at("TEXT").as_object().map(|t| t.contains_key(&format!("{}.{}", kind, name))).unwrap_or(false) {
                return Err(tag("no text"));
            }
            for (_, t) in &m.nxt {
                if !mine.contains_key(t) {
                    return Err(tag("unknown target"));
                }
            }
        }
        if !mine.contains_key(first_of(kind)) {
            return Err(format!("{}: FIRST", kind));
        }
    }
    let wf = tc("ARC_FIGHT").as_object().ok_or("ARC_FIGHT")?;
    for (k, v) in wf {
        let want: BTreeSet<char> = data::set_items(v).iter().filter_map(|x| x.as_str().and_then(|s| s.chars().next())).collect();
        if ARC_FIGHT.get(k.as_str()) != Some(&want) {
            return Err(format!("ARC_FIGHT {}", k));
        }
    }
    Ok(())
}

pub const GENDER_F: [&str; 1] = ["birth"];

/// kids are no fighters: the letters of an arc whose node texts (EN or PL) read as combat, raid, patrol or danger may only be grown people.
pub static ARC_FIGHT: LazyLock<HashMap<String, BTreeSet<char>>> = LazyLock::new(|| {
    let mut out = HashMap::new();
    if let Some(a) = tc("ARCS").as_object() {
        for (k, nodes_) in a {
            let mut l: BTreeSet<char> = BTreeSet::new();
            for n in nodes_.as_object().map(|o| o.keys().cloned().collect::<Vec<_>>()).unwrap_or_default() {
                if let Some(texts) = at("TEXT").get(format!("{}.{}", k, n)).and_then(|t| t.as_array()) {
                    for (t, lg) in texts.iter().zip(["en", "pl", "en", "pl"]) {
                        let t = t.as_str().unwrap_or("");
                        if kd::combat_text(t, Some(lg)) {
                            l.extend(kd::letters_in(t));
                        }
                    }
                }
            }
            out.insert(k.clone(), l);
        }
    }
    out
});

// py: clan_arcs.py:341 rel_key
/// What A is to B (their bond type is symmetric): kin splits by life stage, mentor by rank, sweet = partner.
pub fn rel_key(a: &Value, b: &Value, bt: &str) -> &'static str {
    let rank = |p: &Value| -> i64 {
        match vs(p, "r") {
            "kid" => 0,
            "elder" => 2,
            _ => 1,
        }
    };
    match bt {
        "kin" => {
            let (x, y) = (rank(a), rank(b));
            if x == y {
                "sibling"
            } else if x > y {
                "parent"
            } else {
                "child"
            }
        }
        "mentor" => {
            let (x, y) = (rank(a), rank(b));
            if x > y {
                "mentor"
            } else if x < y {
                "apprentice"
            } else {
                "friend"
            }
        }
        "sweet" => "partner",
        _ => "friend",
    }
}

fn adult(p: &Value) -> bool {
    !matches!(vs(p, "r"), "kid" | "elder")
}

// ------------------------------------------------------------------ starting an arc
// py: clan_arcs.py:370 _free
fn free(c: &Ctx, exclude: &[i64], pred: &dyn Fn(&Value) -> bool) -> Vec<Value> {
    let cs = c.cs();
    let mut busy: HashSet<i64> = HashSet::new();
    for a in cs.get("arcs").and_then(|v| v.as_array()).map(|a| a.as_slice()).unwrap_or(&[]) {
        if let Some(co) = a.get("c").and_then(|x| x.as_object()) {
            busy.extend(co.values().map(int_of));
        }
    }
    let away: HashSet<&String> = cs.get("away").and_then(|v| v.as_object()).map(|o| o.keys().collect()).unwrap_or_default();
    c.st.get("people")
        .and_then(|v| v.as_array())
        .map(|a| a.as_slice())
        .unwrap_or(&[])
        .iter()
        .filter(|p| {
            let i = vi(p, "i");
            !busy.contains(&i) && !exclude.contains(&i) && !away.contains(&i.to_string()) && pred(p)
        })
        .cloned()
        .collect()
}

fn people_of(c: &Ctx) -> Vec<Value> {
    c.st.get("people").and_then(|v| v.as_array()).cloned().unwrap_or_default()
}

// py: clan_arcs.py:376 _pair
/// The bonded partner of A (from the persona ring) when free, else any other free adult.
fn pair(c: &mut Ctx, a: &Value, fight: bool) -> (Option<Value>, Option<&'static str>) {
    let fk = c.d.fk;
    let people = people_of(c);
    let ps = ch::persona(a, &people, fk, Some(&mut *c.st));
    let fr = free(c, &[vi(a, "i")], &|_| true);
    let mut b = ps.bond.and_then(|id| fr.iter().find(|p| vi(p, "i") == id).cloned());
    if let Some(bb) = &b {
        if fight && !kd::can_fight(bb) {
            b = None;
        }
    }
    let mut bt: &'static str = ps.bt.unwrap_or("kin");
    if b.is_none() {
        let mut pool: Vec<&Value> = fr.iter().filter(|p| vs(p, "s") != "s" && adult(p)).collect();
        if pool.is_empty() {
            return (None, None);
        }
        pool.sort_by_key(|p| h(&["pairb".to_string(), vs(a, "n").to_string(), vs(p, "n").to_string()]));
        let bb = pool[0].clone();
        let mut names = [vs(a, "n").to_string(), vs(&bb, "n").to_string()];
        names.sort();
        bt = ch::BOND_TYPES[(h(&["bond".to_string(), names[0].clone(), names[1].clone()]) % 4) as usize];
        b = Some(bb);
    }
    (b, Some(bt))
}

fn len_of(name: &str) -> i64 {
    at(name).as_array().map(|a| a.len() as i64).unwrap_or(1).max(1)
}

// py: clan_arcs.py:393 _make
fn make(c: &mut Ctx, kind: &str, a: &Value, b: Option<&Value>, bt: &str, v: Map<String, Value>) -> Value {
    let n = c.cs().get("arc_n").map(int_of).unwrap_or(0) + 1;
    c.st["ch"]["arc_n"] = json!(n);
    let rel = match b {
        Some(bb) => rel_key(a, bb, bt),
        None => "friend",
    };
    if let (Some(bb), true) = (b, matches!(rel, "sibling" | "parent" | "child")) {
        let l = a.get("l").cloned().unwrap_or(Value::Null);
        if let Some(arr) = c.st.get_mut("people").and_then(|x| x.as_array_mut()) {
            if let Some(p) = arr.iter_mut().find(|p| vi(p, "i") == vi(bb, "i")) {
                p["l"] = l;
            }
        }
    }
    let hv = h(&[c.st.get("seed").map(pystr).unwrap_or_default(), kind.to_string(), n.to_string(), vs(a, "n").to_string()]);
    let mut aobj = Map::new();
    aobj.insert("id".into(), json!(format!("a{}", n)));
    aobj.insert("k".into(), json!(kind));
    aobj.insert("n".into(), Value::Null);
    aobj.insert("t".into(), json!(c.day));
    aobj.insert("d0".into(), json!(c.day));
    let mut cc = Map::new();
    cc.insert("A".into(), a.get("i").cloned().unwrap_or(Value::Null));
    let mut pp = Map::new();
    pp.insert("A".into(), json!([a["n"], a["g"], a["r"]]));
    if let Some(bb) = b {
        cc.insert("B".into(), bb.get("i").cloned().unwrap_or(Value::Null));
        pp.insert("B".into(), json!([bb["n"], bb["g"], bb["r"]]));
    }
    aobj.insert("c".into(), Value::Object(cc));
    aobj.insert("p".into(), Value::Object(pp));
    let mut v = v;
    v.entry("task").or_insert_with(|| json!(hv % len_of("TASKS")));
    v.entry("place").or_insert_with(|| json!((hv >> 4) % len_of("PLACES")));
    v.entry("wish").or_insert_with(|| json!((hv >> 8) % len_of("WISHES")));
    aobj.insert("v".into(), Value::Object(v));
    aobj.insert("h".into(), json!(hv));
    aobj.insert("rel".into(), json!(rel));
    aobj.insert("steps".into(), json!(0));
    Value::Object(aobj)
}

// py: clan_arcs.py:412 _start
fn start(c: &mut Ctx, kind: &str) -> Result<Option<Value>, String> {
    let a = start0(c, kind)?;
    if let Some(a) = a {
        let people = people_of(c);
        if let Some(letters) = ARC_FIGHT.get(kind) {
            for l in letters {
                if let Some(id) = a.get("c").and_then(|x| x.get(l.to_string())).map(int_of) {
                    if let Some(p) = people.iter().find(|p| vi(p, "i") == id) {
                        if !kd::can_fight(p) {
                            return Ok(None);
                        }
                    }
                }
            }
        }
        return Ok(Some(a));
    }
    Ok(None)
}

fn pick(c: &Ctx, kind: &str, pool: Vec<Value>) -> Option<Value> {
    if pool.is_empty() {
        return None;
    }
    let mut v: Vec<(i64, i64, i64, usize)> = pool
        .iter()
        .enumerate()
        .map(|(ix, p)| {
            (ext::saga::fit(c.cs(), kind, p), c.d.used.get(&format!("P{}", vi(p, "i"))).copied().unwrap_or(-999), h(&["arcst".to_string(), c.day.to_string(), vs(p, "n").to_string()]), ix)
        })
        .collect();
    v.sort();
    Some(pool[v[0].3].clone())
}

// py: clan_arcs.py:420 _start0
#[allow(clippy::too_many_lines)]
fn start0(c: &mut Ctx, kind: &str) -> Result<Option<Value>, String> {
    let u = vi(c.newc, "hungry");
    let deficit = vi(&c.r, "supplies") < vi(&c.r, "needed");
    let has_map = truthy(c.matchv) && c.matchv.get("map").map(truthy).unwrap_or(false);
    let np = c.st.get("people").and_then(|v| v.as_array()).map(|a| a.len()).unwrap_or(0);
    let ok_s = |p: &Value| adult(p) && vs(p, "s") != "s";
    let nomap = Map::new();
    Ok(match kind {
        "missing" => {
            if c.day < 2 || np < 6 {
                return Ok(None);
            }
            let pool = free(c, &[], &|p| adult(p) && matches!(vs(p, "s"), "h" | "u"));
            let Some(a) = pick(c, kind, pool) else { return Ok(None) };
            let (b, bt) = pair(c, &a, true);
            b.map(|b| make(c, kind, &a, Some(&b), bt.unwrap_or("kin"), nomap))
        }
        "spore" => {
            let pool = free(c, &[], &|p| vs(p, "s") == "s" && vs(p, "il") == "spores" && !kd::is_kid(p));
            let Some(a) = pick(c, kind, pool) else { return Ok(None) };
            let (b, bt) = pair(c, &a, false);
            let Some(b) = b else { return Ok(None) };
            let mut arc = make(c, kind, &a, Some(&b), bt.unwrap_or("kin"), nomap);
            arc["v"]["spot"] = json!(vi(&arc, "h") % len_of("SPOTS"));
            Some(arc)
        }
        "stranger" => {
            if np < 5 {
                return Ok(None);
            }
            let pool = free(c, &[], &|p| ok_s(p) && kd::can_fight(p));
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &ok_s);
                    pick(c, kind, pool)
                }
                None => None,
            };
            match (a, b) {
                (Some(a), Some(b)) => Some(make(c, kind, &a, Some(&b), "friend", nomap)),
                _ => None,
            }
        }
        "caravan" => {
            if !has_map || np < 5 {
                return Ok(None);
            }
            let pool = free(c, &[], &ok_s);
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &ok_s);
                    pick(c, kind, pool)
                }
                None => None,
            };
            match (a, b) {
                (Some(a), Some(b)) => Some(make(c, kind, &a, Some(&b), "friend", nomap)),
                _ => None,
            }
        }
        "pendant" => {
            if !has_map || np < 5 {
                return Ok(None);
            }
            let pool = free(c, &[], &ok_s);
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &|p| matches!(vs(p, "r"), "elder" | "medic" | "radio" | "cook") && vs(p, "s") != "s");
                    pick(c, kind, pool)
                }
                None => None,
            };
            match (a, b) {
                (Some(a), Some(b)) => Some(make(c, kind, &a, Some(&b), "friend", nomap)),
                _ => None,
            }
        }
        "birthday" => {
            let pool = free(c, &[], &|p| vs(p, "r") == "kid" && vs(p, "s") != "s");
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &ok_s);
                    pick(c, kind, pool)
                }
                None => None,
            };
            match (a, b) {
                (Some(a), Some(b)) => Some(make(c, kind, &a, Some(&b), "mentor", nomap)),
                _ => None,
            }
        }
        "sick" => {
            let pool = free(c, &[], &|p| vs(p, "s") == "s" && vs(p, "il") != "spores");
            let Some(a) = pick(c, kind, pool) else { return Ok(None) };
            let (b, bt) = pair(c, &a, false);
            b.map(|b| make(c, kind, &a, Some(&b), bt.unwrap_or("kin"), nomap))
        }
        "hung" => {
            if !(u != 0 && deficit) {
                return Ok(None);
            }
            let pool = free(c, &[], &|p| adult(p) && vs(p, "s") == "u");
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &|p| matches!(vs(p, "r"), "cook" | "guard" | "medic") && vs(p, "s") != "s");
                    pick(c, kind, pool)
                }
                None => None,
            };
            match (a, b) {
                (Some(a), Some(b)) => Some(make(c, kind, &a, Some(&b), "friend", nomap)),
                _ => None,
            }
        }
        "new" => {
            let day = c.day;
            let pool = free(c, &[], &|p| vi(p, "jd") == day && vs(p, "s") != "s");
            let Some(a) = pick(c, kind, pool) else { return Ok(None) };
            let pool = free(c, &[vi(&a, "i")], &ok_s);
            pick(c, kind, pool).map(|b| make(c, kind, &a, Some(&b), "friend", nomap))
        }
        "keep" => {
            let day = c.day;
            let mut old: Vec<String> = c.cs()["keep"].as_object().map(|o| o.iter().filter(|(_, v)| vi(v, "d") < day).map(|(k, _)| k.clone()).collect()).unwrap_or_default();
            if old.is_empty() {
                return Ok(None);
            }
            let pool = free(c, &[], &|p| vs(p, "r") == "kid");
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &|p| matches!(vs(p, "r"), "elder" | "guard" | "cook" | "medic" | "scout"));
                    pick(c, kind, pool)
                }
                None => None,
            };
            old.sort();
            let k = old[(h(&[c.day.to_string(), "kp".to_string()]) as usize) % old.len()].clone();
            match (a, b) {
                (Some(a), Some(b)) => {
                    let mut v = Map::new();
                    v.insert("keep".into(), json!(k));
                    Some(make(c, kind, &a, Some(&b), "mentor", v))
                }
                _ => None,
            }
        }
        "birth" => {
            if c.morale < 55 || np < 10 {
                return Ok(None);
            }
            let mut cand = free(c, &[], &|p| vs(p, "g") == "f" && adult(p) && vs(p, "s") == "h");
            cand.sort_by_key(|p| h(&["birth".to_string(), c.day.to_string(), vs(p, "n").to_string()]));
            for a in cand {
                let people = people_of(c);
                let ps = ch::persona(&a, &people, c.d.fk, Some(&mut *c.st));
                let fr = free(c, &[vi(&a, "i")], &|_| true);
                let b = fr.into_iter().find(|p| Some(vi(p, "i")) == ps.bond && ps.bt == Some("sweet") && adult(p) && vs(p, "g") != "f");
                if let Some(b) = b {
                    return Ok(Some(make(c, kind, &a, Some(&b), "sweet", nomap)));
                }
            }
            None
        }
        "wish" => {
            let pool = free(c, &[], &|p| vs(p, "r") == "elder" && vs(p, "s") != "s");
            let Some(a) = pick(c, kind, pool) else { return Ok(None) };
            let (b, bt) = pair(c, &a, false);
            b.map(|b| make(c, kind, &a, Some(&b), bt.unwrap_or("kin"), nomap))
        }
        "blame" => {
            if !has_map {
                return Ok(None);
            }
            let m = c.matchv;
            let kills = m.get("kills").filter(|v| truthy(v)).map(int_of).unwrap_or(0);
            let deaths = m.get("deaths").filter(|v| !v.is_null()).map(int_of);
            let bad = deaths.map(|d| d > kills).unwrap_or(false) || (deaths.is_none() && deficit && kills <= 3);
            if !bad {
                return Ok(None);
            }
            let pool = free(c, &[], &ok_s);
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &ok_s);
                    pick(c, kind, pool)
                }
                None => None,
            };
            match (a, b) {
                (Some(a), Some(b)) => Some(make(c, kind, &a, Some(&b), "rival", nomap)),
                _ => None,
            }
        }
        "betray" => {
            let recent = c.st.get("dead").and_then(|v| v.as_array()).map(|a| a.iter().filter(|x| vi(x, "d") >= c.day - 3).count()).unwrap_or(0);
            if c.morale > 45 || recent == 0 || np < 6 {
                return Ok(None);
            }
            let pool = free(c, &[], &ok_s);
            let a = pick(c, kind, pool);
            let b = match &a {
                Some(a) => {
                    let pool = free(c, &[vi(a, "i")], &ok_s);
                    pick(c, kind, pool)
                }
                None => None,
            };
            match (a, b) {
                (Some(a), Some(b)) => Some(make(c, kind, &a, Some(&b), "friend", nomap)),
                _ => None,
            }
        }
        _ => None,
    })
}

pub const START_W: [(&str, f64); 14] = [
    ("missing", 3.0),
    ("sick", 4.0),
    ("hung", 3.0),
    ("new", 5.0),
    ("keep", 3.0),
    ("birth", 1.5),
    ("wish", 2.0),
    ("blame", 3.0),
    ("betray", 2.0),
    ("spore", 12.0),
    ("stranger", 2.0),
    ("caravan", 2.0),
    ("pendant", 1.5),
    ("birthday", 1.5),
];

// ------------------------------------------------------------------ writing a step
pub const DARK: [&str; 14] = ["found_gut", "mercy_gut", "found_dead", "mercy", "despair", "turned", "death", "after", "banish", "gone", "expose", "turn", "walk", "remembered"];

// py: clan_arcs.py:555 _release
fn release(c: &mut Ctx, a: &mut Value) {
    let am: Vec<String> = a["v"].get("am").and_then(|x| x.as_array()).map(|x| x.iter().map(pystr).collect()).unwrap_or_default();
    for l in am {
        if let Some(id) = a["c"].get(&l).map(int_of) {
            if let Some(aw) = c.st["ch"]["away"].as_object_mut() {
                aw.shift_remove(&id.to_string());
            }
        }
    }
    let dm: Vec<i64> = a["v"].get("dm").and_then(|x| x.as_array()).map(|x| x.iter().map(int_of).collect()).unwrap_or_default();
    for i in dm {
        if let Some(d) = c.st["ch"]["doom"].as_object_mut() {
            d.shift_remove(&i.to_string());
        }
    }
    a["v"]["am"] = json!([]);
    a["v"]["dm"] = json!([]);
}

// py: clan_arcs.py:563 _enter
fn enter(c: &mut Ctx, a: &mut Value, node: &str) -> Result<(), String> {
    let kind = vs(a, "k").to_string();
    let nd = ARCS.get(kind.as_str()).and_then(|n| n.get(node)).ok_or_else(|| format!("KeyError: {}.{}", kind, node))?;
    release(c, a);
    for l in nd.away.chars() {
        if let Some(id) = a["c"].get(l.to_string()).cloned() {
            c.st["ch"]["away"][int_of(&id).to_string()] = a["id"].clone();
            if let Some(am) = a["v"].get_mut("am").and_then(|x| x.as_array_mut()) {
                am.push(json!(l.to_string()));
            }
        }
    }
    for (l, cause) in &nd.doom {
        if let Some(id) = a["c"].get(l.to_string()).cloned() {
            c.st["ch"]["doom"][int_of(&id).to_string()] = json!(cause);
            if let Some(dm) = a["v"].get_mut("dm").and_then(|x| x.as_array_mut()) {
                dm.push(id);
            }
        }
    }
    if let Some(fx) = &nd.fx {
        fx(c, a)?;
    }
    a["n"] = json!(node);
    a["t"] = json!(c.day);
    a["steps"] = json!(vi(a, "steps") + 1);
    Ok(())
}

// py: clan_arcs.py:577 row_for
pub fn row_for(c: &Ctx, a: &Value, node: &str) -> Result<Value, String> {
    let kind = vs(a, "k");
    let v = &a["v"];
    let mut sym = Map::new();
    sym.insert("f".into(), json!(c.d.fk));
    sym.insert("day".into(), json!(c.day));
    sym.insert("sup".into(), json!(vi(&c.r, "supplies")));
    sym.insert("need".into(), json!(vi(&c.r, "needed")));
    sym.insert("rel".into(), a["rel"].clone());
    sym.insert("task".into(), v["task"].clone());
    sym.insert("place".into(), v["place"].clone());
    sym.insert("wish".into(), v["wish"].clone());
    if truthy(c.matchv) && c.matchv.get("map").map(truthy).unwrap_or(false) {
        sym.insert("map".into(), json!(pystr(&c.matchv["map"])));
    }
    if v.get("keep").map(truthy).unwrap_or(false) {
        sym.insert("keep".into(), v["keep"].clone());
    }
    if v.get("spot").is_some() {
        sym.insert("spot".into(), v["spot"].clone());
    }
    if v.get("it").map(truthy).unwrap_or(false) {
        sym.insert("it".into(), v["it"].clone());
        sym.insert("itn".into(), json!(1));
    }
    let lid = format!("arc/{}.{}", kind, node);
    let params = json!({"p": a["p"].clone(), "s": Value::Object(sym), "a": a["id"].clone(), "n": a["steps"].clone()});
    let text = render_msg(&lid, &params, "en").ok_or("render_msg None")?;
    let nd = ARCS.get(kind).and_then(|n| n.get(node)).ok_or("KeyError: node")?;
    let names: Vec<Value> = a["p"].as_object().map(|o| o.values().filter(|v| has_word(&text, v.get(0).map(|x| pystr(x)).as_deref().unwrap_or(""))).map(|v| v[0].clone()).collect()).unwrap_or_default();
    Ok(json!({"kind": "chron", "mood": nd.mood, "text": text, "names": names, "lid": lid, "params": params}))
}

// py: clan_arcs.py:497 _vars
fn vars_arc(sym: &Value, people: &Value, lang: &str) -> Result<ext::Vars, String> {
    let mut v = ch::vars_of_sym(sym, lang)?;
    let pl = lang == "pl";
    let ag = if people.get("A").and_then(|p| p.get("g")).and_then(|x| x.as_str()) == Some("f") { "f" } else { "m" };
    if let Some(rel) = sym.get("rel").and_then(|x| x.as_str()) {
        if let Some(e) = at("REL").get(rel).and_then(|r| r.get(ag)) {
            let g = |i: usize| e.get(i).map(pystr).unwrap_or_default();
            v.insert("rel".into(), if pl { g(1) } else { g(0) });
            v.insert("rel_nom".into(), g(1));
            v.insert("rel_acc".into(), g(2));
            v.insert("rel_gen".into(), g(3));
        }
    }
    let pick_t = |name: &str, key: &str| -> Value {
        let arr = at(name).as_array().map(|a| a.as_slice()).unwrap_or(&[]);
        let n = arr.len().max(1) as i64;
        let ix = crate::py::pymod(sym.get(key).map(int_of).unwrap_or(0), n) as usize;
        arr.get(ix).cloned().unwrap_or(Value::Null)
    };
    let t = pick_t("TASKS", "task");
    let pc = pick_t("PLACES", "place");
    let w = pick_t("WISHES", "wish");
    let sp = pick_t("SPOTS", "spot");
    let g = |x: &Value, i: usize| x.get(i).map(pystr).unwrap_or_default();
    let ix = if pl { 1 } else { 0 };
    v.insert("spot".into(), g(&sp, ix));
    v.insert("task".into(), g(&t, ix));
    v.insert("place".into(), g(&pc, ix));
    v.insert("place_gen".into(), g(&pc, 1));
    v.insert("wish".into(), g(&w, ix));
    if let Some(kk) = sym.get("keep").and_then(|x| x.as_str()) {
        if let Some(k) = at("KEEPS").get(kk) {
            v.insert("keep".into(), g(k, ix));
            v.insert("keep_acc".into(), g(k, 1));
            v.insert("keep_ins".into(), g(k, 2));
            v.insert("keep_gen".into(), g(k, 3));
        }
    }
    v.entry("map".into()).or_insert_with(String::new);
    Ok(v)
}

fn render_msg_inner(lid: &str, params: &Value, lang: &str) -> Result<Option<String>, String> {
    if !lid.starts_with("arc/") || !params.is_object() {
        return Ok(None);
    }
    let key = &lid[4..];
    let Some(tx) = at("TEXT").get(key) else { return Ok(None) };
    let lang = if ch::norm_lang_pl(lang) { "pl" } else { "en" };
    let people = ch::people_from_params(params);
    let empty = json!({});
    let sym = params.get("s").unwrap_or(&empty);
    let ts = |i: usize| tx.get(i).and_then(|x| x.as_str()).unwrap_or("");
    let fk = sym.get("f").map(pystr).unwrap_or_else(|| "W".to_string());
    let vars = vars_arc(sym, &people, lang)?;
    let pl = lang == "pl";
    let head = lex::render(if pl { ts(1) } else { ts(0) }, &people, &vars, lang, &fk)?.to_uppercase();
    let mut body = lex::render(if pl { ts(3) } else { ts(2) }, &people, &vars, lang, &fk)?;
    let tie = params.get(if pl { "tie_pl" } else { "tie" });
    if let Some(t) = tie.filter(|t| truthy(t)) {
        body = format!("{} {}", body, pystr(t));
    }
    Ok(Some(format!("{} {}: {}. {}", if pl { "DZIEŃ" } else { "DAY" }, sym.get("day").map(int_of).unwrap_or(0), head, body)))
}

// py: clan_arcs.py:524 render_msg
/// A stored arc row ("arc/<kind>.<node>") as "DAY n: HEADLINE. Vignette." in a language; None for an unknown node (or when Python would raise).
pub fn render_msg(lid: &str, params: &Value, lang: &str) -> Option<String> {
    render_msg_inner(lid, params, lang).ok().flatten()
}

/// `render_msg` with the Python exceptions as `Err`.
pub fn render_msg_res(lid: &str, params: &Value, lang: &str) -> Result<Option<String>, String> {
    render_msg_inner(lid, params, lang)
}

// ------------------------------------------------------------------ the daily run
// py: clan_arcs.py:547 run
/// Advance the running arcs and maybe start one; returns the day's arc rows (at most ARC_ROWS). `d` = the chronicle's day object.
pub fn run(d: &mut Day, st: &mut Value, newc: &Value, r: &Value, matchv: &Value, rng: &mut PyRandom) -> Result<Vec<Value>, String> {
    {
        let cs = ch::ch_state(st);
        let co = cs.as_object_mut().ok_or("ch")?;
        co.entry("arcs").or_insert_with(|| json!([]));
        co.entry("away").or_insert_with(|| json!({}));
        co.entry("doom").or_insert_with(|| json!({}));
        co.entry("arc_hist").or_insert_with(|| json!({}));
        co.entry("arc_log").or_insert_with(|| json!([]));
    }
    let byid: HashMap<i64, Value> = st.get("people").and_then(|v| v.as_array()).map(|a| a.iter().map(|p| (vi(p, "i"), p.clone())).collect()).unwrap_or_default();
    let day = vi(newc, "day");
    let morale = st.get("morale").map(int_of).unwrap_or(50);
    let r_use = if truthy(r) { r.clone() } else { json!({}) };
    let mut c = Ctx { d, st, newc, r: r_use, matchv, day, morale, byid };
    let mut rows: Vec<Value> = Vec::new();
    let arc_ids: Vec<String> = c.cs()["arcs"].as_array().map(|a| a.iter().map(|x| vs(x, "id").to_string()).collect()).unwrap_or_default();
    for aid in arc_ids {
        let Some(ix) = arc_index(&c, &aid) else { continue };
        let mut a = c.cs()["arcs"][ix].clone();
        if vi(&a, "t") == c.day && vi(&a, "steps") >= 1 && !a["n"].is_null() && vi(&a, "d0") == c.day {
            continue;
        }
        let kind = vs(&a, "k").to_string();
        let ndn = a["n"].as_str().unwrap_or("").to_string();
        let nd = ARCS.get(kind.as_str()).and_then(|n| n.get(ndn.as_str())).ok_or_else(|| format!("KeyError: {}.{}", kind, ndn))?;
        let fired = nd.nxt.iter().find(|(cond, _)| cond(&c, &a)).map(|(_, t)| *t);
        let Some(fired) = fired else {
            let need_dead = a["c"].as_object().map(|o| o.keys().any(|l| !c.alive(&a, l.chars().next().unwrap_or(' ')) && !nd.ok_dead.contains(l.as_str()))).unwrap_or(false);
            if need_dead || c.day - vi(&a, "d0") > MAX_AGE {
                release(&mut c, &mut a);
                if let Some(ix) = arc_index(&c, &aid) {
                    c.st["ch"]["arcs"].as_array_mut().map(|v| v.remove(ix));
                }
            }
            continue;
        };
        if rows.len() >= ARC_ROWS {
            continue;
        }
        enter(&mut c, &mut a, fired)?;
        c.st["ch"]["arcs"][ix] = a.clone();
        rows.push(row_for(&c, &a, fired)?);
        for i in a["c"].as_object().map(|o| o.values().map(int_of).collect::<Vec<_>>()).unwrap_or_default() {
            c.d.used.insert(format!("P{}", i), c.day);
        }
        let ends = ARCS.get(kind.as_str()).and_then(|n| n.get(fired)).map(|n| n.nxt.is_empty()).unwrap_or(true);
        if ends {
            release(&mut c, &mut a);
            if let Some(ix) = arc_index(&c, &aid) {
                c.st["ch"]["arcs"].as_array_mut().map(|v| v.remove(ix));
            }
            let b0 = a["p"].get("B").and_then(|b| b.get(0)).cloned().unwrap_or(json!(""));
            let e = json!({"id": a["id"], "k": a["k"], "d0": a["d0"], "d1": c.day, "A": a["p"]["A"][0], "B": b0, "end": fired});
            if let Some(l) = c.st["ch"]["arc_log"].as_array_mut() {
                l.push(e);
                let n = l.len();
                if n > LOG_KEEP {
                    l.drain(..n - LOG_KEEP);
                }
            }
        }
    }
    let n_arcs = c.cs()["arcs"].as_array().map(|a| a.len()).unwrap_or(0);
    if n_arcs < MAX_ACTIVE && rows.len() < ARC_ROWS && rng.random() < (if n_arcs == 0 { 0.55 } else { 0.35 }) {
        let hist = c.cs()["arc_hist"].clone();
        let running: Vec<String> = c.cs()["arcs"].as_array().map(|a| a.iter().map(|x| vs(x, "k").to_string()).collect()).unwrap_or_default();
        let mut kinds: Vec<(&str, f64)> = START_W
            .iter()
            .filter(|(k, _)| c.day - hist.get(*k).map(int_of).unwrap_or(-99) >= COOLDOWN && !running.iter().any(|r| r == k))
            .copied()
            .collect();
        rng.shuffle(&mut kinds);
        let mut keyed: Vec<(f64, &str)> = kinds.iter().map(|(k, w)| (-rng.random() * w, *k)).collect();
        keyed.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap_or(std::cmp::Ordering::Equal));
        for (_, k) in keyed {
            let Some(mut a) = start(&mut c, k)? else { continue };
            c.st["ch"]["arcs"].as_array_mut().map(|v| v.push(a.clone()));
            c.st["ch"]["arc_hist"][k] = json!(c.day);
            let first = first_of(k);
            enter(&mut c, &mut a, first)?;
            let last = c.cs()["arcs"].as_array().map(|v| v.len()).unwrap_or(1) - 1;
            c.st["ch"]["arcs"][last] = a.clone();
            rows.push(row_for(&c, &a, first)?);
            for i in a["c"].as_object().map(|o| o.values().map(int_of).collect::<Vec<_>>()).unwrap_or_default() {
                c.d.used.insert(format!("P{}", i), c.day);
            }
            break;
        }
    }
    Ok(rows)
}

fn arc_index(c: &Ctx, id: &str) -> Option<usize> {
    c.cs()["arcs"].as_array()?.iter().position(|a| vs(a, "id") == id)
}

// py: clan_arcs.py:589 thread
/// The finished arcs of one arc id (the log kept for the clan screen).
pub fn thread(st: &Value, arc_id: &str) -> Vec<Value> {
    st.get("ch").and_then(|c| c.get("arc_log")).and_then(|l| l.as_array()).map(|l| l.iter().filter(|e| vs(e, "id") == arc_id).cloned().collect()).unwrap_or_default()
}
