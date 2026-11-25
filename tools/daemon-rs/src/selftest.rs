//! `t2r-daemon selftest <module>`: the Rust side of the differential tests (verify/py/<module>_harness.py is the Python side).
use std::io::{BufRead, Write};

pub fn run(module: &str, input: &mut dyn BufRead, out: &mut dyn Write) -> i32 {
    match module {
        "hostmig" => crate::hostmig::selftest(input, out),
        "hostcap" => crate::hostcap::selftest(input, out),
        _ => {
            let _ = (input, out);
            eprintln!("selftest: unknown module {}", module);
            2
        }
    }
}
