use crate::http::{http_date, parse_range, send_json, Request};
use crate::Shared;
use std::fs::File;
use std::io::{self, Write};
use std::net::TcpStream;
use std::path::Path;
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

const CHUNK: u64 = 1 << 20;

pub fn is_sha(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
}

pub fn is_blob_name(s: &str) -> bool {
    if is_sha(s) {
        return true;
    }
    match s.split_once('.') {
        Some((h, e)) => is_sha(h) && (e == "gz" || e == "br"),
        None => false,
    }
}

pub struct FileReply<'a> {
    pub etag: &'a str,
    pub name: Option<&'a str>,
    pub ctype: &'a str,
    pub cache: Option<&'a str>,
    pub missing: &'a str,
    pub max_size: Option<u64>,
}

fn throttle(sh: &Shared, n: u64) {
    let b = match &sh.bucket {
        Some(b) => b,
        None => return,
    };
    let rate = sh.cfg.rate_mbit as f64 * 1e6 / 8.0;
    loop {
        let wait = {
            let mut g = b.lock().unwrap_or_else(|e| e.into_inner());
            let now = Instant::now();
            let el = now.duration_since(g.1).as_secs_f64();
            g.0 = (g.0 + el * rate).min(rate);
            g.1 = now;
            if g.0 >= n as f64 {
                g.0 -= n as f64;
                return;
            }
            (n as f64 - g.0) / rate
        };
        std::thread::sleep(Duration::from_secs_f64(wait.min(0.5)));
    }
}

#[cfg(target_os = "linux")]
fn send_range(sh: &Shared, s: &mut TcpStream, f: &File, start: u64, len: u64) -> io::Result<()> {
    use std::os::unix::io::AsRawFd;
    let mut off = start as libc::off_t;
    let mut left = len;
    while left > 0 {
        let want = left.min(CHUNK);
        throttle(sh, want);
        let r = unsafe { libc::sendfile(s.as_raw_fd(), f.as_raw_fd(), &mut off, want as usize) };
        if r < 0 {
            let e = io::Error::last_os_error();
            if e.kind() == io::ErrorKind::Interrupted {
                continue;
            }
            return Err(e);
        }
        if r == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "file shrank"));
        }
        left -= r as u64;
        sh.stats.sent.fetch_add(r as u64, Ordering::Relaxed);
    }
    Ok(())
}

#[cfg(not(target_os = "linux"))]
fn send_range(sh: &Shared, s: &mut TcpStream, f: &File, start: u64, len: u64) -> io::Result<()> {
    use std::io::{Read, Seek, SeekFrom};
    let mut f = f;
    f.seek(SeekFrom::Start(start))?;
    let mut buf = vec![0u8; CHUNK as usize];
    let mut left = len;
    while left > 0 {
        let want = left.min(CHUNK) as usize;
        throttle(sh, want as u64);
        let n = f.read(&mut buf[..want])?;
        if n == 0 {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "file shrank"));
        }
        s.write_all(&buf[..n])?;
        left -= n as u64;
        sh.stats.sent.fetch_add(n as u64, Ordering::Relaxed);
    }
    Ok(())
}

pub fn serve_file(sh: &Shared, s: &mut TcpStream, req: &Request, path: &Path, r: &FileReply) -> io::Result<()> {
    let head_only = req.method == "HEAD";
    sh.stats.file_reqs.fetch_add(1, Ordering::Relaxed);
    let f = match File::open(path) {
        Ok(f) if f.metadata().map(|m| m.is_file() && r.max_size.map_or(true, |x| m.len() <= x)).unwrap_or(false) => f,
        _ => {
            sh.stats.file_404.fetch_add(1, Ordering::Relaxed);
            return send_json(s, 404, r.missing, head_only);
        }
    };
    let size = f.metadata()?.len();
    let rng = match parse_range(req.header("range"), size) {
        Ok(x) => x,
        Err(()) => {
            let h = format!(
                "HTTP/1.1 416 Requested Range Not Satisfiable\r\nServer: t2r-files/2\r\nDate: {}\r\nContent-Range: bytes */{}\r\nContent-Length: 0\r\n\r\n",
                http_date(),
                size
            );
            return s.write_all(h.as_bytes());
        }
    };
    let (start, end) = rng.unwrap_or((0, size.saturating_sub(1)));
    let len = if size == 0 { 0 } else { end - start + 1 };
    let mut h = format!(
        "HTTP/1.1 {}\r\nServer: t2r-files/2\r\nDate: {}\r\nContent-Type: {}\r\n",
        if rng.is_some() { "206 Partial Content" } else { "200 OK" },
        http_date(),
        r.ctype
    );
    if let Some(c) = r.cache {
        h += &format!("Cache-Control: {}\r\n", c);
    }
    h += &format!("Accept-Ranges: bytes\r\nETag: \"{}\"\r\n", r.etag);
    if let Some(n) = r.name {
        h += &format!("Content-Disposition: attachment; filename=\"{}\"\r\n", n.replace(['"', '\r', '\n'], ""));
    }
    h += &format!("Content-Length: {}\r\n", len);
    if rng.is_some() {
        h += &format!("Content-Range: bytes {}-{}/{}\r\n", start, end, size);
    } else {
        sh.stats.file_full.fetch_add(1, Ordering::Relaxed);
    }
    h += "\r\n";
    s.write_all(h.as_bytes())?;
    if head_only || len == 0 {
        return Ok(());
    }
    send_range(sh, s, &f, start, len)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names() {
        let h = "a".repeat(64);
        assert!(is_blob_name(&h));
        assert!(is_blob_name(&format!("{}.gz", h)));
        assert!(!is_blob_name(&format!("{}.zip", h)));
        assert!(!is_blob_name("../../etc/passwd"));
        assert!(!is_blob_name(&"A".repeat(64)));
        assert!(!is_sha(&format!("{}/x", "a".repeat(62))));
    }
}
