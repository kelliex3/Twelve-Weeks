//! Small helpers for Python-like byte handling.

/// Python `b[a:z]` (never panics; clamps like Python slicing for non-negative bounds).
pub fn sl(b: &[u8], a: usize, z: usize) -> &[u8] {
    let n = b.len();
    let a = a.min(n);
    let z = z.min(n);
    if a >= z {
        &[]
    } else {
        &b[a..z]
    }
}

/// Python `b[a:]`.
pub fn from(b: &[u8], a: usize) -> &[u8] {
    if a >= b.len() {
        &[]
    } else {
        &b[a..]
    }
}

/// `int.from_bytes(b[a:a+n], "little")` (short slices give the value of the bytes present).
pub fn le(b: &[u8], a: usize, n: usize) -> u64 {
    let mut v = 0u64;
    for (i, x) in sl(b, a, a + n).iter().enumerate() {
        v |= (*x as u64) << (8 * i);
    }
    v
}

/// `b[i]` or None.
pub fn at(b: &[u8], i: usize) -> Option<u8> {
    b.get(i).copied()
}

/// `struct.unpack_from("<f", b, a)` (None when short).
pub fn f32_le(b: &[u8], a: usize) -> Option<f32> {
    let s = b.get(a..a + 4)?;
    Some(f32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

/// `struct.unpack_from("<H", b, a)`.
pub fn u16_le(b: &[u8], a: usize) -> Option<u16> {
    let s = b.get(a..a + 2)?;
    Some(u16::from_le_bytes([s[0], s[1]]))
}

/// `struct.unpack_from("<I", b, a)`.
pub fn u32_le(b: &[u8], a: usize) -> Option<u32> {
    let s = b.get(a..a + 4)?;
    Some(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
}

/// `struct.unpack_from("<Q", b, a)`.
pub fn u64_le(b: &[u8], a: usize) -> Option<u64> {
    let s = b.get(a..a + 8)?;
    let mut x = [0u8; 8];
    x.copy_from_slice(s);
    Some(u64::from_le_bytes(x))
}
