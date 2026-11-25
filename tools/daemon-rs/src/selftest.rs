//! `t2r-daemon selftest <module>`: the Rust side of the differential tests (verify/py/<module>_harness.py is the Python side).
use std::io::{BufRead, Write};

pub fn run(module: &str, input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    match module {
        // modules register here: "hostmig" => crate::hostmig::selftest(input, out),
        "lobbybook" => crate::lobbybook::selftest(input, out),
        "requeue" => crate::requeue::selftest(input, out),
        "modeq" => crate::modeq::selftest(input, out),
        "gunlimit" => crate::gunlimit::selftest(input, out),
        "interrogation" => crate::interrogation::selftest(input, out),
        "map_packs" => crate::map_packs::selftest(input, out),
        _ => {
            let _ = (input, out);
            eprintln!("selftest: unknown module {}", module);
            2
        }
    }
}
