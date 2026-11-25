use sha2::{Digest, Sha512};

const N: usize = 624;
const M: usize = 397;
const MATRIX_A: u32 = 0x9908_b0df;
const UPPER: u32 = 0x8000_0000;
const LOWER: u32 = 0x7fff_ffff;

#[derive(Clone)]
pub struct PyRandom {
    mt: [u32; N],
    mti: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RandError {
    Empty,
    Range,
    Weights,
}

fn floor_div(a: i64, b: i64) -> i64 {
    let q = a / b;
    if (a % b != 0) && ((a < 0) != (b < 0)) {
        q - 1
    } else {
        q
    }
}

fn words_le(bytes_be: &[u8]) -> Vec<u32> {
    let mut start = 0;
    while start < bytes_be.len() && bytes_be[start] == 0 {
        start += 1;
    }
    let digits = &bytes_be[start..];
    if digits.is_empty() {
        return vec![0];
    }
    let mut out = Vec::with_capacity(digits.len() / 4 + 1);
    let mut end = digits.len();
    while end > 0 {
        let lo = end.saturating_sub(4);
        let mut w = 0u32;
        for b in &digits[lo..end] {
            w = (w << 8) | *b as u32;
        }
        out.push(w);
        end = lo;
    }
    out
}

impl PyRandom {
    fn init_genrand(s: u32) -> PyRandom {
        let mut mt = [0u32; N];
        mt[0] = s;
        for i in 1..N {
            mt[i] = 1_812_433_253u32.wrapping_mul(mt[i - 1] ^ (mt[i - 1] >> 30)).wrapping_add(i as u32);
        }
        PyRandom { mt, mti: N }
    }

    fn init_by_array(key: &[u32]) -> PyRandom {
        let mut r = PyRandom::init_genrand(19_650_218);
        let mt = &mut r.mt;
        let klen = key.len().max(1);
        let (mut i, mut j) = (1usize, 0usize);
        let mut k = N.max(klen);
        while k > 0 {
            let kj = key.get(j).copied().unwrap_or(0);
            mt[i] = (mt[i] ^ (mt[i - 1] ^ (mt[i - 1] >> 30)).wrapping_mul(1_664_525)).wrapping_add(kj).wrapping_add(j as u32);
            i += 1;
            j += 1;
            if i >= N {
                mt[0] = mt[N - 1];
                i = 1;
            }
            if j >= klen {
                j = 0;
            }
            k -= 1;
        }
        k = N - 1;
        while k > 0 {
            mt[i] = (mt[i] ^ (mt[i - 1] ^ (mt[i - 1] >> 30)).wrapping_mul(1_566_083_941)).wrapping_sub(i as u32);
            i += 1;
            if i >= N {
                mt[0] = mt[N - 1];
                i = 1;
            }
            k -= 1;
        }
        mt[0] = 0x8000_0000;
        r
    }

    pub fn from_int_be(magnitude_be: &[u8]) -> PyRandom {
        PyRandom::init_by_array(&words_le(magnitude_be))
    }

    pub fn from_u64(seed: u64) -> PyRandom {
        PyRandom::from_int_be(&seed.to_be_bytes())
    }

    pub fn from_i64(seed: i64) -> PyRandom {
        PyRandom::from_u64(seed.unsigned_abs())
    }

    pub fn from_str(seed: &str) -> PyRandom {
        PyRandom::from_bytes(seed.as_bytes())
    }

    pub fn from_bytes(seed: &[u8]) -> PyRandom {
        let digest = Sha512::digest(seed);
        let mut all = Vec::with_capacity(seed.len() + 64);
        all.extend_from_slice(seed);
        all.extend_from_slice(&digest);
        PyRandom::from_int_be(&all)
    }

    pub fn from_entropy() -> PyRandom {
        let mut buf = [0u8; 32];
        let ok = std::fs::File::open("/dev/urandom").and_then(|mut f| std::io::Read::read_exact(&mut f, &mut buf)).is_ok();
        if !ok {
            let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0);
            buf[..16].copy_from_slice(&t.to_be_bytes());
            buf[16..24].copy_from_slice(&(std::process::id() as u64).to_be_bytes());
        }
        PyRandom::from_int_be(&buf)
    }

    pub fn next_u32(&mut self) -> u32 {
        if self.mti >= N {
            let mt = &mut self.mt;
            let mag = |y: u32| if y & 1 != 0 { MATRIX_A } else { 0 };
            for kk in 0..N - M {
                let y = (mt[kk] & UPPER) | (mt[kk + 1] & LOWER);
                mt[kk] = mt[kk + M] ^ (y >> 1) ^ mag(y);
            }
            for kk in N - M..N - 1 {
                let y = (mt[kk] & UPPER) | (mt[kk + 1] & LOWER);
                mt[kk] = mt[kk + M - N] ^ (y >> 1) ^ mag(y);
            }
            let y = (mt[N - 1] & UPPER) | (mt[0] & LOWER);
            mt[N - 1] = mt[M - 1] ^ (y >> 1) ^ mag(y);
            self.mti = 0;
        }
        let mut y = self.mt[self.mti];
        self.mti += 1;
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c_5680;
        y ^= (y << 15) & 0xefc6_0000;
        y ^= y >> 18;
        y
    }

    pub fn random(&mut self) -> f64 {
        let a = (self.next_u32() >> 5) as f64;
        let b = (self.next_u32() >> 6) as f64;
        (a * 67_108_864.0 + b) * (1.0 / 9_007_199_254_740_992.0)
    }

    pub fn getrandbits(&mut self, k: u32) -> u64 {
        if k == 0 {
            return 0;
        }
        if k <= 32 {
            return (self.next_u32() >> (32 - k)) as u64;
        }
        let k = k.min(64);
        let lo = self.next_u32() as u64;
        let hi = (self.next_u32() >> (64 - k)) as u64;
        lo | (hi << 32)
    }

    pub fn randbelow(&mut self, n: u64) -> u64 {
        if n == 0 {
            return 0;
        }
        let k = 64 - n.leading_zeros();
        let mut r = self.getrandbits(k);
        while r >= n {
            r = self.getrandbits(k);
        }
        r
    }

    pub fn randrange(&mut self, start: i64, stop: i64) -> Result<i64, RandError> {
        let width = stop.checked_sub(start).ok_or(RandError::Range)?;
        if width <= 0 {
            return Err(RandError::Range);
        }
        Ok(start + self.randbelow(width as u64) as i64)
    }

    pub fn randrange_step(&mut self, start: i64, stop: i64, step: i64) -> Result<i64, RandError> {
        if step == 1 {
            return self.randrange(start, stop);
        }
        if step == 0 {
            return Err(RandError::Range);
        }
        let width = stop.checked_sub(start).ok_or(RandError::Range)?;
        let n = if step > 0 { floor_div(width + step - 1, step) } else { floor_div(width + step + 1, step) };
        if n <= 0 {
            return Err(RandError::Range);
        }
        Ok(start + step * self.randbelow(n as u64) as i64)
    }

    pub fn randrange_to(&mut self, stop: i64) -> Result<i64, RandError> {
        if stop > 0 {
            Ok(self.randbelow(stop as u64) as i64)
        } else {
            Err(RandError::Range)
        }
    }

    pub fn randint(&mut self, a: i64, b: i64) -> Result<i64, RandError> {
        self.randrange(a, b.checked_add(1).ok_or(RandError::Range)?)
    }

    pub fn choice_index(&mut self, len: usize) -> Result<usize, RandError> {
        if len == 0 {
            return Err(RandError::Empty);
        }
        Ok(self.randbelow(len as u64) as usize)
    }

    pub fn choice<'a, T>(&mut self, seq: &'a [T]) -> Result<&'a T, RandError> {
        let i = self.choice_index(seq.len())?;
        Ok(&seq[i])
    }

    pub fn shuffle<T>(&mut self, x: &mut [T]) {
        let mut i = x.len();
        while i > 1 {
            i -= 1;
            let j = self.randbelow(i as u64 + 1) as usize;
            x.swap(i, j);
        }
    }

    pub fn sample_indices(&mut self, n: usize, k: usize) -> Result<Vec<usize>, RandError> {
        if k > n {
            return Err(RandError::Range);
        }
        let mut setsize = 21usize;
        if k > 5 {
            let e = ((k * 3) as f64).ln() / 4f64.ln();
            setsize += 4usize.pow(e.ceil() as u32);
        }
        let mut out = Vec::with_capacity(k);
        if n <= setsize {
            let mut pool: Vec<usize> = (0..n).collect();
            for i in 0..k {
                let j = self.randbelow((n - i) as u64) as usize;
                out.push(pool[j]);
                pool[j] = pool[n - i - 1];
            }
        } else {
            let mut selected = std::collections::HashSet::with_capacity(k);
            for _ in 0..k {
                let mut j = self.randbelow(n as u64) as usize;
                while selected.contains(&j) {
                    j = self.randbelow(n as u64) as usize;
                }
                selected.insert(j);
                out.push(j);
            }
        }
        Ok(out)
    }

    pub fn sample<T: Clone>(&mut self, population: &[T], k: usize) -> Result<Vec<T>, RandError> {
        Ok(self.sample_indices(population.len(), k)?.into_iter().map(|i| population[i].clone()).collect())
    }

    pub fn choices_unweighted(&mut self, n: usize, k: usize) -> Vec<usize> {
        let nf = n as f64;
        (0..k).map(|_| (self.random() * nf).floor() as usize).collect()
    }

    pub fn choices_weighted(&mut self, weights: &[f64], k: usize) -> Result<Vec<usize>, RandError> {
        let mut cum = Vec::with_capacity(weights.len());
        let mut acc = 0.0f64;
        for (i, w) in weights.iter().enumerate() {
            acc = if i == 0 { *w } else { acc + *w };
            cum.push(acc);
        }
        self.choices_cum(&cum, k)
    }

    pub fn choices_cum(&mut self, cum: &[f64], k: usize) -> Result<Vec<usize>, RandError> {
        let Some(total) = cum.last().copied() else { return Err(RandError::Empty) };
        if !(total > 0.0) || !total.is_finite() {
            return Err(RandError::Weights);
        }
        let hi = cum.len() - 1;
        Ok((0..k)
            .map(|_| {
                let x = self.random() * total;
                let (mut lo, mut h) = (0usize, hi);
                while lo < h {
                    let mid = (lo + h) / 2;
                    if x < cum[mid] {
                        h = mid;
                    } else {
                        lo = mid + 1;
                    }
                }
                lo
            })
            .collect())
    }
}
