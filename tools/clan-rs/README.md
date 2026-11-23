# t2r_clan

Read-only clan views on the daemon's `t2r.db`, shared by the update server (`tools/files-rs`, binary `t2r-files`) and the admin site port (`tools/admin-rs`).

```toml
t2r-clan = { path = "../clan-rs" }
```

The rules and the menu ticker are ported from the clan modules the **daemon** runs (`~/t2r-live/host186/clan*.py` on fra), not from the older copies in the update server's `tools/`. `menu_lines` gives the same ids and texts as the daemon's op 0xB4 widget, A-Life lines included.

## Public API

Every function takes a `&rusqlite::Connection` the caller opens (read-only is enough; nothing is written) and, where time matters, an explicit `now: f64` (Unix seconds). No globals besides the embedded template tables; everything is `Send + Sync` and reads no files.

```rust
pub fn clan_view(conn: &Connection, player: &str, key: Option<&str>) -> serde_json::Value
```
The `/clan.json` card. `player` is the install id (24 hex) or the 64-hex name token; it is resolved to the daemon's clans-table key (`s:<steamid>` when the install is linked to a Steam account that has a clan). `key: Some(k)` (admin site) uses the clans-table key `k` as is: no id check, no Steam resolve.
Answers `{"ok": false, "error": "bad player id"}`, `{"ok": true, "hasClan": false}`, or the card: `ok, hasClan, faction, factionName, status, daysDone, daysTotal, weeksTotal, size, healthy, hungry, sick, needed, maxSize, runsWon, runsLost`, for a living clan also `week, dayOfWeek, weeks, event, eventToday, mission, boosterNext`, then `boosters`, and from the last match `lastMatch, supplies, lastMission`.

```rust
pub fn clanfeed(conn: &Connection, player: &str, limit: &str, since: &str, lang: &str, now: f64, key: Option<&str>) -> serde_json::Value
```
The `/clanfeed.json` answer the launcher uses: `{"ok", "hasClan", "lang", "faction", "factionName", "status", "daysDone", "menu": [{"id", "text"}], "slot"}`. `limit` / `since` are only validated (`{"ok": false, "error": "bad limit / since"}`); the journal feed, roster, A-Life and chronicle blocks are not produced (the launcher reads only `ok`, `hasClan` and `menu`). `key` as in `clan_view`.

```rust
pub fn menu_lines(conn: &Connection, key: &str, limit: usize, lang: &str, now: f64) -> Vec<(String, String)>
```
The menu ticker for a clans-table `key` at time `now`: up to `limit` `(id, text)` pairs (the daemon asks for 6): two stored diary rows by slot rotation, three A-Life lines, the rest live 2013 ticker lines. Ids are the row id, `a<slot><nn>` or `l<slot><nn>`; a slot is 60 s. `lang` is `"en"` or anything `norm_lang` reads as Polish (`"pl"`, `"pl-PL"`, `"1"` ...). An unknown key or a clan without status gives `[]`.

```rust
pub fn resolve_key(conn: &Connection, install_id: &str) -> String   // the daemon's clan key for an install id
pub fn token_id(token: &str) -> String                              // 64-hex token -> install id (chars 40..64), else the input lowercased
pub fn valid_player(player: &str) -> bool                           // 24 or 64 hex chars
pub fn norm_lang(lang: &str) -> &'static str                        // "en" | "pl"
pub fn slot_of(now: f64) -> i64                                     // menu time slot (60 s)
pub fn warm() -> bool                                               // parse the embedded templates now (else on first use); false if they do not load
```

Rules (`t2r_clan::rules`, the common ones re-exported at the crate root):

```rust
pub const WEEKS: i64 = 12; WIN_DAY: i64 = 83; DAYS, EVENT_LEN, BOOSTER_EVERY
pub const NONE, WLF, SERAPHITES: i64;  ALIVE, DEAD, WON: i64;  EV_NONE, EV_GROWTH, EV_ATTACK;  AN_ENEMY, AN_HORDE, GN_SPOIL
pub const GROWTH_NAMES, GROWTH_NAMES_ALL, ATTACK_NAMES, HORDE_DAYS, SPOIL_DAYS, BOOSTERS, SUPPLY_CATS, MISSIONS
pub fn needed(size: i64) -> i64
pub fn week_day(day: i64) -> (i64, i64)
pub fn event_today(day: i64) -> (i64, i64)          // (kind, name id) of the event starting on day index `day`
pub fn event_window(day: i64) -> i64                // start day of the event whose three matches contain `day`, or -1
pub fn event_name(kind: i64, name_id: i64) -> &'static str
pub fn mission(mid: i64) -> Option<&'static (i64, &'static str, [i64; 8])>
pub fn mission_goals(mid: i64, repeats: i64) -> Option<[i64; 3]>
pub fn booster_next(healthy: i64, hungry: i64, sick: i64, boost_hi: i64) -> i64
pub fn faction_name(f: i64) -> &'static str
pub fn status_name(s: i64) -> &'static str
```

The daemon's switches are taken at their defaults: A-Life on (`clan_alife.txt` absent), the horde / spoiled-supplies events on (`clan_events.txt` absent).

## Template data

`data/clan.json` is generated from the daemon's modules and embedded at build time. After the clan text changes on the daemon side, regenerate it from a copy of that directory and rebuild:

```sh
python3 -I tools/clan-rs/gen/export.py <copy of ~/t2r-live/host186>
```

## Checking against the Python

`examples/menu_dump.rs` prints `menu_lines` for every clan with a status, per language and time; the same loop over `clan_story.menu_lines(c, tid, 6, lang, now, save=False)` with the daemon's modules gives the Python side. On the 2026-11-23 db backup (617 clans, en + pl, 12 time slots) both produce the same 88,848 lines.
