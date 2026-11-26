//! Randomness used by daemon.py: `secrets` (match ids, tokens, vote ties, map rotation) and the global `random` module.
//!
//! Every draw names its Python call site: `key` = "<module>:<function>" of the Python function that calls `secrets.*` / `random.*`
//! ("daemon" for daemon.py, "<module>" for module-level code), e.g. `rnd::randbelow("daemon:vote_options", n)`.
//!
//! Test hooks (the Python side is verify/py/seedhook.py, which patches `secrets` / `random` the same way):
//! - `T2R_TEST_SEED=<int>`: every draw comes from a CPython-exact `random.Random(seed)` stream (token bytes = `getrandbits(8)` per byte).
//! - `T2R_TEST_SEED_STREAMS=1` (with the seed): one stream PER KEY = `random.Random("<seed>/<key>")` (timer-driven draws must not shift
//!   the others).
//! - An unseeded `random.Random()` instance of the Python (clan.rng, Interrogation.rng) = `unseeded()` = `random.Random(seed + 1)`.
//! Without `T2R_TEST_SEED` the key is ignored and the draws use the OS entropy (secrets) / one entropy-seeded stream (random).
use parking_lot::Mutex;
use std::collections::HashMap;
use std::sync::OnceLock;
use t2r_clanstory::pyrand::PyRandom;

static RNG: OnceLock<Mutex<PyRandom>> = OnceLock::new();
static STREAMS: OnceLock<Mutex<HashMap<String, PyRandom>>> = OnceLock::new();
static SEED: OnceLock<Option<i64>> = OnceLock::new();
static PER_KEY: OnceLock<bool> = OnceLock::new();

/// `T2R_TEST_SEED` (None = not pinned).
pub fn test_seed() -> Option<i64> {
    *SEED.get_or_init(|| std::env::var("T2R_TEST_SEED").ok().and_then(|v| v.trim().parse::<i64>().ok()))
}

pub fn pinned() -> bool {
    test_seed().is_some()
}

fn per_key() -> bool {
    *PER_KEY.get_or_init(|| pinned() && std::env::var("T2R_TEST_SEED_STREAMS").map(|v| v.trim() == "1").unwrap_or(false))
}

fn rng() -> &'static Mutex<PyRandom> {
    RNG.get_or_init(|| {
        Mutex::new(match test_seed() {
            Some(s) => PyRandom::from_i64(s),
            None => PyRandom::from_entropy(),
        })
    })
}

/// Run `f` on the stream of `key` (the one shared stream unless T2R_TEST_SEED_STREAMS=1).
fn with<R>(key: &str, f: impl FnOnce(&mut PyRandom) -> R) -> R {
    if per_key() {
        let mut m = STREAMS.get_or_init(|| Mutex::new(HashMap::new())).lock();
        let seed = test_seed().unwrap_or(0);
        let r = m.entry(key.to_string()).or_insert_with(|| PyRandom::from_str(&format!("{}/{}", seed, key)));
        return f(r);
    }
    f(&mut rng().lock())
}

/// A fresh generator for an unseeded Python `random.Random()` instance: `Random(T2R_TEST_SEED + 1)` when pinned, else entropy.
pub fn unseeded() -> PyRandom {
    match test_seed() {
        Some(s) => PyRandom::from_i64(s + 1),
        None => PyRandom::from_entropy(),
    }
}

/// (tests) the shared stream restarted at `random.Random(seed)`; the per-key streams are dropped.
pub fn reseed(seed: i64) {
    *rng().lock() = PyRandom::from_i64(seed);
    if let Some(m) = STREAMS.get() {
        m.lock().clear();
    }
}

/// `random.random()`
pub fn random(key: &str) -> f64 {
    with(key, |r| r.random())
}

/// `secrets.randbelow(n)` / `random.randrange(n)` (n > 0).
pub fn randbelow(key: &str, n: u64) -> u64 {
    if n == 0 {
        return 0;
    }
    with(key, |r| r.randbelow(n))
}

/// `secrets.choice(seq)` / `random.choice(seq)` -> index.
pub fn choice_index(key: &str, len: usize) -> usize {
    if len == 0 {
        return 0;
    }
    with(key, |r| r.choice_index(len).unwrap_or(0))
}

/// `random.shuffle(x)`
pub fn shuffle<T>(key: &str, x: &mut [T]) {
    with(key, |r| r.shuffle(x))
}

/// `random.randint(a, b)`
pub fn randint(key: &str, a: i64, b: i64) -> i64 {
    with(key, |r| r.randint(a, b).unwrap_or(a))
}

/// `random.uniform(a, b)`
pub fn uniform(key: &str, a: f64, b: f64) -> f64 {
    a + (b - a) * random(key)
}

/// `secrets.token_bytes(n)` (pinned: `random.getrandbits(8)` per byte, as verify/py/seedhook.py does).
pub fn token_bytes(key: &str, n: usize) -> Vec<u8> {
    if pinned() {
        return with(key, |r| (0..n).map(|_| r.getrandbits(8) as u8).collect());
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
pub fn token_hex(key: &str, n: usize) -> String {
    crate::pyfmt::hex_sp(&token_bytes(key, n)).replace(' ', "")
}

/// `secrets.randbits(k)` (k <= 64)
pub fn randbits(key: &str, k: u32) -> u64 {
    if pinned() {
        return with(key, |r| r.getrandbits(k));
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
