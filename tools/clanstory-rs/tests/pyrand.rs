use t2r_clanstory::pyrand::PyRandom;

fn hex(s: &str) -> Vec<u8> {
    if s == "-" {
        return Vec::new();
    }
    (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).expect("hex")).collect()
}

fn list(s: &str) -> Vec<usize> {
    if s == "-" {
        return Vec::new();
    }
    s.split(',').map(|x| x.parse().expect("int")).collect()
}

fn int_be(v: &str) -> Vec<u8> {
    let digits = v.trim_start_matches('-');
    let mut bytes: Vec<u8> = vec![0];
    for d in digits.bytes() {
        let mut carry = (d - b'0') as u32;
        for b in bytes.iter_mut().rev() {
            let x = *b as u32 * 10 + carry;
            *b = (x & 0xff) as u8;
            carry = x >> 8;
        }
        while carry > 0 {
            bytes.insert(0, (carry & 0xff) as u8);
            carry >>= 8;
        }
    }
    bytes
}

#[test]
fn matches_cpython() {
    let data = include_str!("data/pyrand.txt");
    let mut r: Option<PyRandom> = None;
    let mut checked = 0;
    for (ln, line) in data.lines().enumerate() {
        if line.starts_with('#') || line.is_empty() {
            continue;
        }
        let f: Vec<&str> = line.split(' ').collect();
        let ctx = format!("line {}: {}", ln + 1, line);
        if f[0] == "seed" {
            r = Some(if f[1] == "int" { PyRandom::from_int_be(&int_be(f[2])) } else { PyRandom::from_bytes(&hex(f[2])) });
            continue;
        }
        let r = r.as_mut().expect("seed first");
        let p = |i: usize| -> i64 { f[i].parse().expect("int") };
        match f[0] {
            "random" => assert_eq!(r.random(), f[1].parse::<f64>().expect("float"), "{}", ctx),
            "bits" => assert_eq!(r.getrandbits(p(1) as u32), f[2].parse::<u64>().expect("u64"), "{}", ctx),
            "rto" => assert_eq!(r.randbelow(f[1].parse().expect("u64")), f[2].parse::<u64>().expect("u64"), "{}", ctx),
            "rrange" => assert_eq!(r.randrange(p(1), p(2)), Ok(p(3)), "{}", ctx),
            "randint" => assert_eq!(r.randint(p(1), p(2)), Ok(p(3)), "{}", ctx),
            "choice" => assert_eq!(r.choice_index(p(1) as usize), Ok(p(2) as usize), "{}", ctx),
            "shuffle" => {
                let mut x: Vec<usize> = (0..p(1) as usize).collect();
                r.shuffle(&mut x);
                assert_eq!(x, list(f[2]), "{}", ctx);
            }
            "sample" => assert_eq!(r.sample_indices(p(1) as usize, p(2) as usize), Ok(list(f[3])), "{}", ctx),
            "choicesu" => assert_eq!(r.choices_unweighted(p(1) as usize, p(2) as usize), list(f[3]), "{}", ctx),
            "choicesw" => {
                let w: Vec<f64> = f[1].split(',').map(|x| x.parse().expect("weight")).collect();
                assert_eq!(r.choices_weighted(&w, p(2) as usize), Ok(list(f[3])), "{}", ctx);
            }
            "rstep" => {
                let got = r.randrange_step(p(1), p(2), p(3));
                if f[4] == "err" {
                    assert!(got.is_err(), "{}", ctx);
                } else {
                    assert_eq!(got, Ok(p(4)), "{}", ctx);
                }
            }
            other => panic!("unknown op {} at {}", other, ctx),
        }
        checked += 1;
    }
    assert!(checked > 2000, "only {} checks", checked);
}

#[test]
fn small_seed_helpers_agree() {
    let mut a = PyRandom::from_u64(12345);
    let mut b = PyRandom::from_int_be(&[0x30, 0x39]);
    for _ in 0..10 {
        assert_eq!(a.next_u32(), b.next_u32());
    }
    let mut c = PyRandom::from_i64(-12345);
    let mut d = PyRandom::from_u64(12345);
    assert_eq!(c.random(), d.random());
}
