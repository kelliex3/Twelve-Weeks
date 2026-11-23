use t2r_clanstory::{crc, pyjson};

fn unhex(s: &str) -> Vec<u8> {
    if s == "-" {
        return Vec::new();
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex")).collect()
}

#[test]
fn json_and_crc_match_cpython() {
    let data = include_str!("data/pyjson.txt");
    let mut cur: Option<serde_json::Value> = None;
    let mut n = 0;
    for (ln, line) in data.lines().enumerate() {
        if line.starts_with('#') {
            continue;
        }
        let (tag, rest) = line.split_at(2);
        let ctx = format!("line {}", ln + 1);
        match tag {
            "V " => {
                let v: serde_json::Value = serde_json::from_str(rest).expect("parse");
                assert_eq!(pyjson::dumps(&v, pyjson::COMPACT), rest, "{}", ctx);
                cur = Some(v);
            }
            "D " => assert_eq!(pyjson::dumps(cur.as_ref().expect("value"), pyjson::DEFAULT), rest, "{}", ctx),
            "U " => {
                let want = String::from_utf8(unhex(rest)).expect("utf8");
                assert_eq!(pyjson::dumps(cur.as_ref().expect("value"), pyjson::COMPACT_UTF8), want, "{}", ctx);
            }
            "F " => {
                let (bits, repr) = rest.split_once(' ').expect("float line");
                let b = unhex(bits);
                let f = f64::from_be_bytes(b.try_into().expect("8 bytes"));
                assert_eq!(pyjson::float_repr(f), repr, "{}", ctx);
            }
            "C " => {
                let (s, c) = rest.split_once(' ').expect("crc line");
                assert_eq!(crc::crc32(&unhex(s)), c.parse::<u32>().expect("u32"), "{}", ctx);
            }
            "H " => {
                let (ps, c) = rest.split_once(' ').expect("h line");
                let parts: Vec<String> = ps.split(',').map(|p| String::from_utf8(unhex(p)).expect("utf8")).collect();
                let refs: Vec<&str> = parts.iter().map(|s| s.as_str()).collect();
                assert_eq!(crc::h(&refs), c.parse::<u32>().expect("u32"), "{}", ctx);
            }
            _ => panic!("bad tag at {}", ctx),
        }
        n += 1;
    }
    assert!(n > 10000, "only {} checks", n);
}
