//! `t2r-daemon selftest <module>`: the Rust side of the differential tests (verify/py/<module>_harness.py is the Python side).
use std::io::{BufRead, Write};
use std::path::PathBuf;

/// The scratch folder of one module's test run: `$T2R_VERIFY_DIR/<name>` (default: the system temp folder), emptied.
pub fn tmp_dir(name: &str) -> PathBuf {
    let base = std::env::var("T2R_VERIFY_DIR").map(PathBuf::from).unwrap_or_else(|_| std::env::temp_dir());
    let d = base.join(name);
    let _ = std::fs::remove_dir_all(&d);
    let _ = std::fs::create_dir_all(&d);
    d
}

/// A fresh SQLite file for one module's test run.
pub fn tmp_db(name: &str) -> PathBuf {
    tmp_dir(name).join("t.db")
}

pub fn run(module: &str, input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    match module {
        // modules register here: "hostmig" => crate::hostmig::selftest(input, out),
        "hostmig" => crate::hostmig::selftest(input, out),
        "hostcap" => crate::hostcap::selftest(input, out),
        "names" => crate::names::selftest(input, out),
        "rankbadge" => crate::rankbadge::selftest(input, out),
        "notices" => crate::notices::selftest(input, out),
        "roles" => crate::roles::selftest(input, out),
        "social" => crate::social_selftest::selftest(input, out),
        "lobbybook" => crate::lobbybook::selftest(input, out),
        "requeue" => crate::requeue::selftest(input, out),
        "modeq" => crate::modeq::selftest(input, out),
        "gunlimit" => crate::gunlimit::selftest(input, out),
        "interrogation" => crate::interrogation::selftest(input, out),
        "map_packs" => crate::map_packs::selftest(input, out),
        "clan_chronicle" | "clan_arcs" => crate::clan::selftest_chronicle::selftest(input, out),
        "clan_saga" | "clan_loot" => crate::clan::selftest_saga::selftest(input, out),
        "clan_base" => crate::clan::selftest_base::selftest(input, out),
        "clan_story" => crate::clan::selftest_story::selftest(input, out),
        "k1" => crate::d::k1_selftest::selftest(input, out),
        "clan_alife" | "clan_journal" => crate::clan::selftest_alife::selftest(input, out),
        "k4" => crate::d::k4_selftest::selftest(input, out),
        _ => {
            let _ = (input, out);
            eprintln!("selftest: unknown module {}", module);
            2
        }
    }
}
