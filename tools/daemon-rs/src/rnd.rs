//! Randomness used by daemon.py: `secrets` (match ids, tokens, vote ties, map rotation) and the global `random` module.
//!
//! Test hook: with `T2R_TEST_SEED=<int>` every draw comes from one CPython-exact `random.Random(seed)` stream, and the
//! Python side of the differential tests patches `secrets` / `random` the same way (verify/py/seedhook.py), so both
//! daemons make the same choices.
use parking_lot::Mutex;
use std::sync::OnceLock;
use t2r_clanstory::pyrand::PyRandom;

static RNG: OnceLock<Mutex<PyRandom>> = OnceLock::new();
static PINNED: OnceLock<bool> = OnceLock::new();

fn rng() -> &'static Mutex<PyRandom> {
    RNG.get_or_init(|| {
        let r = match std::env::var("T2R_TEST_SEED").ok().and_then(|v| v.trim().parse::<i64>().ok()) {
            Some(s) => PyRandom::from_i64(s),
            None => PyRandom::from_entropy(),
        };
        Mutex::new(r)
    })
}

pub fn pinned() -> bool {
    *PINNED.get_or_init(|| std::env::var("T2R_TEST_SEED").map(|v| !v.trim().is_empty()).unwrap_or(false))
}

/// (tests) `random.Random(seed)` stream restarted
pub fn reseed(seed: i64) {
    *rng().lock() = PyRandom::from_i64(seed);
}

/// `random.random()`
pub fn random() -> f64 {
    rng().lock().random()
}

/// `secrets.randbelow(n)` / `random.randrange(n)` (n > 0).
pub fn randbelow(n: u64) -> u64 {
    if n == 0 {
        return 0;
    }
    rng().lock().randbelow(n)
}

/// `secrets.choice(seq)` / `random.choice(seq)` -> index.
pub fn choice_index(len: usize) -> usize {
    if len == 0 {
        return 0;
    }
    rng().lock().choice_index(len).unwrap_or(0)
}

/// `random.shuffle(x)`
pub fn shuffle<T>(x: &mut [T]) {
    rng().lock().shuffle(x)
}

/// `random.randint(a, b)`
pub fn randint(a: i64, b: i64) -> i64 {
    rng().lock().randint(a, b).unwrap_or(a)
}

/// `random.uniform(a, b)`
pub fn uniform(a: f64, b: f64) -> f64 {
    a + (b - a) * random()
}

/// `secrets.token_bytes(n)` (pinned: `random.getrandbits(8)` per byte, as verify/py/seedhook.py does).
pub fn token_bytes(n: usize) -> Vec<u8> {
    if pinned() {
        let mut r = rng().lock();
        return (0..n).map(|_| r.getrandbits(8) as u8).collect();
    }
    let mut v = vec![0u8; n];
    if getrandom::getrandom(&mut v).is_err() {
        let mut r = rng().lock();
        for b in v.iter_mut() {
            *b = r.getrandbits(8) as u8;
        }
    }
    v
}

/// `secrets.token_hex(n)`
pub fn token_hex(n: usize) -> String {
    crate::pyfmt::hex_sp(&token_bytes(n)).replace(' ', "")
}

/// `secrets.randbits(k)` (k <= 64)
pub fn randbits(k: u32) -> u64 {
    if pinned() {
        return rng().lock().getrandbits(k);
    }
    let mut b = [0u8; 8];
    let _ = getrandom::getrandom(&mut b);
    let v = u64::from_le_bytes(b);
    if k >= 64 {
        v
    } else {
        v & ((1u64 << k) - 1)
    }
}
