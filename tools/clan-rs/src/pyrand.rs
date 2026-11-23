use sha2::{Digest, Sha512};

const N: usize = 624;
const M: usize = 397;

pub struct PyRandom {
    mt: [u32; N],
    idx: usize,
}

impl PyRandom {
    pub fn new(seed: &str) -> PyRandom {
        let mut bytes = seed.as_bytes().to_vec();
        let digest = Sha512::digest(seed.as_bytes());
        bytes.extend_from_slice(&digest);
        let start = bytes.iter().position(|b| *b != 0).unwrap_or(bytes.len());
        let sig = &bytes[start..];
        let mut key: Vec<u32> = Vec::with_capacity(sig.len() / 4 + 1);
        let mut end = sig.len();
        while end > 0 {
            let from = end.saturating_sub(4);
            let mut w = 0u32;
            for b in &sig[from..end] {
                w = (w << 8) | *b as u32;
            }
            key.push(w);
            end = from;
        }
        if key.is_empty() {
            key.push(0);
        }
        let mut r = PyRandom { mt: [0; N], idx: N };
        r.init_by_array(&key);
        r
    }

    fn init_genrand(&mut self, s: u32) {
        self.mt[0] = s;
        for i in 1..N {
            let prev = self.mt[i - 1];
            self.mt[i] = 1812433253u32.wrapping_mul(prev ^ (prev >> 30)).wrapping_add(i as u32);
        }
        self.idx = N;
    }

    fn init_by_array(&mut self, key: &[u32]) {
        self.init_genrand(19650218);
        let mut i = 1usize;
        let mut j = 0usize;
        let mut k = N.max(key.len());
        while k > 0 {
            let prev = self.mt[i - 1];
            self.mt[i] = (self.mt[i] ^ (prev ^ (prev >> 30)).wrapping_mul(1664525)).wrapping_add(key[j]).wrapping_add(j as u32);
            i += 1;
            j += 1;
            if i >= N {
                self.mt[0] = self.mt[N - 1];
                i = 1;
            }
            if j >= key.len() {
                j = 0;
            }
            k -= 1;
        }
        k = N - 1;
        while k > 0 {
            let prev = self.mt[i - 1];
            self.mt[i] = (self.mt[i] ^ (prev ^ (prev >> 30)).wrapping_mul(1566083941)).wrapping_sub(i as u32);
            i += 1;
            if i >= N {
                self.mt[0] = self.mt[N - 1];
                i = 1;
            }
            k -= 1;
        }
        self.mt[0] = 0x8000_0000;
    }

    fn next_u32(&mut self) -> u32 {
        if self.idx >= N {
            for kk in 0..N {
                let y = (self.mt[kk] & 0x8000_0000) | (self.mt[(kk + 1) % N] & 0x7fff_ffff);
                let mut v = self.mt[(kk + M) % N] ^ (y >> 1);
                if y & 1 != 0 {
                    v ^= 0x9908_b0df;
                }
                self.mt[kk] = v;
            }
            self.idx = 0;
        }
        let mut y = self.mt[self.idx];
        self.idx += 1;
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c_5680;
        y ^= (y << 15) & 0xefc6_0000;
        y ^= y >> 18;
        y
    }

    pub fn random(&mut self) -> f64 {
        let a = (self.next_u32() >> 5) as f64;
        let b = (self.next_u32() >> 6) as f64;
        (a * 67108864.0 + b) * (1.0 / 9007199254740992.0)
    }

    fn getrandbits(&mut self, k: u32) -> u32 {
        if k == 0 {
            0
        } else {
            self.next_u32() >> (32 - k)
        }
    }

    pub fn randbelow(&mut self, n: usize) -> usize {
        let k = usize::BITS - n.leading_zeros();
        let mut r = self.getrandbits(k) as usize;
        while r >= n {
            r = self.getrandbits(k) as usize;
        }
        r
    }

    pub fn choice<'a, T>(&mut self, v: &'a [T]) -> Option<&'a T> {
        if v.is_empty() {
            return None;
        }
        let i = self.randbelow(v.len());
        v.get(i)
    }

    pub fn shuffle<T>(&mut self, v: &mut [T]) {
        for i in (1..v.len()).rev() {
            let j = self.randbelow(i + 1);
            v.swap(i, j);
        }
    }

    pub fn choices_weighted(&mut self, weights: &[f64]) -> Option<usize> {
        if weights.is_empty() {
            return None;
        }
        let mut cum = Vec::with_capacity(weights.len());
        let mut acc = 0.0f64;
        for (i, w) in weights.iter().enumerate() {
            acc = if i == 0 { *w } else { acc + w };
            cum.push(acc);
        }
        let total = acc;
        if total <= 0.0 || !total.is_finite() {
            return None;
        }
        let x = self.random() * total;
        let hi = weights.len() - 1;
        let pos = cum[..hi].partition_point(|c| *c <= x);
        Some(pos)
    }

    pub fn sample_indices(&mut self, n: usize, k: usize) -> Option<Vec<usize>> {
        if k > n {
            return None;
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
                let j = self.randbelow(n - i);
                out.push(pool[j]);
                pool[j] = pool[n - i - 1];
            }
        } else {
            let mut selected = std::collections::HashSet::new();
            for _ in 0..k {
                let mut j = self.randbelow(n);
                while selected.contains(&j) {
                    j = self.randbelow(n);
                }
                selected.insert(j);
                out.push(j);
            }
        }
        Some(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn matches_cpython() {
        let mut r = PyRandom::new("abc");
        assert_eq!(r.random(), 0.7720246314157545);
        assert_eq!(r.randbelow(1000), 571);
        assert_eq!(r.choices_weighted(&[1.0, 0.3, 1.6]), Some(2));
        let mut x: Vec<u32> = (0..10).collect();
        r.shuffle(&mut x);
        assert_eq!(x, vec![4, 3, 6, 8, 1, 0, 7, 2, 9, 5]);
        assert_eq!(r.sample_indices(12, 2), Some(vec![11, 10]));
        assert_eq!(r.sample_indices(100, 7), Some(vec![94, 0, 96, 9, 40, 34, 86]));
        assert_eq!(PyRandom::new("line:0123456789abcdef01234567:1:deadbeef:5:42").random(), 0.7334571002526982);
        assert_eq!(PyRandom::new("").random(), 0.9602256525641875);
        assert_eq!(PyRandom::new("zażółć").random(), 0.7229032685708516);
    }
}
