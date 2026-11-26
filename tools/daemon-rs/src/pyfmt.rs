use std::fmt;
use std::net::SocketAddrV4;

pub struct PyAddr(pub SocketAddrV4);

impl fmt::Display for PyAddr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "('{}', {})", self.0.ip(), self.0.port())
    }
}

pub fn hex_sp(b: &[u8]) -> String {
    const H: &[u8; 16] = b"0123456789abcdef";
    let mut s = String::with_capacity(b.len() * 3);
    for (i, x) in b.iter().enumerate() {
        if i > 0 {
            s.push(' ');
        }
        s.push(H[(x >> 4) as usize] as char);
        s.push(H[(x & 15) as usize] as char);
    }
    s
}

pub fn str_repr(s: &str) -> String {
    let quote = if s.contains('\'') && !s.contains('"') { '"' } else { '\'' };
    let mut out = String::with_capacity(s.len() + 2);
    out.push(quote);
    for c in s.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if c == quote => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 || c as u32 == 0x7f => out.push_str(&format!("\\x{:02x}", c as u32)),
            c if (c as u32) >= 0x80 && !printable(c) => {
                let u = c as u32;
                if u < 0x100 {
                    out.push_str(&format!("\\x{:02x}", u));
                } else if u < 0x10000 {
                    out.push_str(&format!("\\u{:04x}", u));
                } else {
                    out.push_str(&format!("\\U{:08x}", u));
                }
            }
            c => out.push(c),
        }
    }
    out.push(quote);
    out
}

/// `str.isprintable()` for a character from U+0080 up (CPython's table, see unprintable.rs).
fn printable(c: char) -> bool {
    let u = c as u32;
    crate::unprintable::RANGES.binary_search_by(|&(a, b)| if u < a { std::cmp::Ordering::Greater } else if u > b { std::cmp::Ordering::Less } else { std::cmp::Ordering::Equal }).is_err()
}

pub fn os_error_repr(e: &std::io::Error) -> String {
    let Some(code) = e.raw_os_error() else { return format!("OSError({})", str_repr(&e.to_string())) };
    let class = match code {
        libc::EAGAIN => "BlockingIOError",
        libc::EPIPE | libc::ESHUTDOWN => "BrokenPipeError",
        libc::ECONNABORTED => "ConnectionAbortedError",
        libc::ECONNREFUSED => "ConnectionRefusedError",
        libc::ECONNRESET => "ConnectionResetError",
        libc::EINTR => "InterruptedError",
        libc::EACCES | libc::EPERM => "PermissionError",
        libc::ETIMEDOUT => "TimeoutError",
        libc::ENOENT => "FileNotFoundError",
        libc::EEXIST => "FileExistsError",
        libc::ENOTDIR => "NotADirectoryError",
        libc::EISDIR => "IsADirectoryError",
        _ => "OSError",
    };
    let msg = match glibc_strerror(code) {
        Some(m) => m.to_string(),
        None => unsafe { std::ffi::CStr::from_ptr(libc::strerror(code)) }.to_string_lossy().into_owned(),
    };
    format!("{}({}, {})", class, code, str_repr(&msg))
}

/// `os.strerror(code)` as the server's Python (glibc) words it.
pub fn strerror(code: i32) -> String {
    match glibc_strerror(code) {
        Some(m) => m.to_string(),
        None => unsafe { std::ffi::CStr::from_ptr(libc::strerror(code)) }.to_string_lossy().into_owned(),
    }
}

fn glibc_strerror(code: i32) -> Option<&'static str> {
    Some(match code {
        libc::EPERM => "Operation not permitted",
        libc::EINTR => "Interrupted system call",
        libc::EBADF => "Bad file descriptor",
        libc::EAGAIN => "Resource temporarily unavailable",
        libc::ENOMEM => "Cannot allocate memory",
        libc::EACCES => "Permission denied",
        libc::EINVAL => "Invalid argument",
        libc::EMFILE => "Too many open files",
        libc::EPIPE => "Broken pipe",
        libc::EMSGSIZE => "Message too long",
        libc::EAFNOSUPPORT => "Address family not supported by protocol",
        libc::EADDRINUSE => "Address already in use",
        libc::EADDRNOTAVAIL => "Cannot assign requested address",
        libc::ENETDOWN => "Network is down",
        libc::ENETUNREACH => "Network is unreachable",
        libc::ECONNABORTED => "Software caused connection abort",
        libc::ECONNRESET => "Connection reset by peer",
        libc::ENOBUFS => "No buffer space available",
        libc::ENOTCONN => "Transport endpoint is not connected",
        libc::ETIMEDOUT => "Connection timed out",
        libc::ECONNREFUSED => "Connection refused",
        libc::EHOSTUNREACH => "No route to host",
        libc::ESHUTDOWN => "Cannot send after transport endpoint shutdown",
        _ => return None,
    })
}
