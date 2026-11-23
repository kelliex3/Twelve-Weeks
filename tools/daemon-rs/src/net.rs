use std::io;
use std::net::{IpAddr, Ipv4Addr, SocketAddr, SocketAddrV4, UdpSocket};
use std::os::fd::AsRawFd;
use std::time::{Duration, Instant};

use socket2::{Domain, Protocol, Socket, Type};

use crate::pyfmt::os_error_repr;
use crate::udp::{Limits, Net, Relay};
use crate::{clock, dlog, env_parse};

const BATCH: usize = 64;

pub fn bind_exclusive(s: &Socket, kind: &str, host: &str, port: u16) -> io::Result<()> {
    let ip: Ipv4Addr = host.parse().unwrap_or(Ipv4Addr::UNSPECIFIED);
    let addr = SocketAddr::new(IpAddr::V4(ip), port);
    s.set_reuse_address(true)?;
    let retry_s: f64 = env_parse("T2R_BIND_RETRY_S", 30.0);
    let t0 = Instant::now();
    let mut next = 1.0;
    loop {
        match s.bind(&addr.into()) {
            Ok(()) => {
                let el = t0.elapsed().as_secs_f64();
                if el > 0.3 {
                    println!("BIND {} {}:{} ok after {:.1}s of retries", kind, host, port, el);
                }
                return Ok(());
            }
            Err(e) => {
                let el = t0.elapsed().as_secs_f64();
                if el >= retry_s {
                    println!(
                        "FATAL {} {}:{} already in use after {:.0}s (is another server running?): {}",
                        kind, host, port, retry_s, e
                    );
                    return Err(e);
                }
                if el >= next {
                    println!("BIND {} {}:{} busy ({}); retrying for up to {:.0}s more", kind, host, port, e, retry_s - el);
                    next = el + 5.0;
                }
                std::thread::sleep(Duration::from_millis(250));
            }
        }
    }
}

pub struct UdpNet {
    sock: UdpSocket,
    addrs: Vec<libc::sockaddr_in>,
    iov: Vec<libc::iovec>,
    msgs: Vec<libc::mmsghdr>,
}

fn sockaddr(a: SocketAddrV4) -> libc::sockaddr_in {
    let mut s: libc::sockaddr_in = unsafe { std::mem::zeroed() };
    s.sin_family = libc::AF_INET as libc::sa_family_t;
    s.sin_port = a.port().to_be();
    s.sin_addr = libc::in_addr { s_addr: u32::from_ne_bytes(a.ip().octets()) };
    s
}

impl UdpNet {
    pub fn new(sock: UdpSocket) -> UdpNet {
        UdpNet { sock, addrs: Vec::with_capacity(BATCH), iov: Vec::with_capacity(BATCH), msgs: Vec::with_capacity(BATCH) }
    }

    fn send_chunk(&mut self, data: &[u8], to: &[SocketAddrV4]) -> u64 {
        self.addrs.clear();
        self.iov.clear();
        self.msgs.clear();
        for t in to {
            self.addrs.push(sockaddr(*t));
            self.iov.push(libc::iovec { iov_base: data.as_ptr() as *mut libc::c_void, iov_len: data.len() });
        }
        for i in 0..to.len() {
            let mut m: libc::mmsghdr = unsafe { std::mem::zeroed() };
            m.msg_hdr.msg_name = &mut self.addrs[i] as *mut libc::sockaddr_in as *mut libc::c_void;
            m.msg_hdr.msg_namelen = std::mem::size_of::<libc::sockaddr_in>() as libc::socklen_t;
            m.msg_hdr.msg_iov = &mut self.iov[i] as *mut libc::iovec;
            m.msg_hdr.msg_iovlen = 1;
            self.msgs.push(m);
        }
        let fd = self.sock.as_raw_fd();
        let mut failed = 0u64;
        let mut start = 0usize;
        while start < to.len() {
            let left = (to.len() - start) as libc::c_uint;
            let r = unsafe { libc::sendmmsg(fd, self.msgs.as_mut_ptr().add(start), left, 0) };
            if r > 0 {
                start += r as usize;
            } else {
                let e = io::Error::last_os_error();
                if r < 0 && e.raw_os_error() == Some(libc::EINTR) {
                    continue;
                }
                failed += 1;
                start += 1;
            }
        }
        failed
    }
}

impl Net for UdpNet {
    fn send(&mut self, data: &[u8], to: SocketAddrV4) -> io::Result<()> {
        self.sock.send_to(data, to).map(|_| ())
    }

    fn send_many(&mut self, data: &[u8], to: &[SocketAddrV4]) -> u64 {
        to.chunks(BATCH).map(|c| self.send_chunk(data, c)).sum()
    }
}

pub fn udp_server(host: &str, port: u16, lim: Limits) -> i32 {
    let s = match Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP)) {
        Ok(s) => s,
        Err(e) => {
            println!("FATAL UDP socket: {}", e);
            return 3;
        }
    };
    if bind_exclusive(&s, "UDP", host, port).is_err() {
        return 3;
    }
    dlog!("UDP LISTEN", format!("{}:{}", host, port));
    if let Err(e) = s.set_recv_buffer_size(8 * 1024 * 1024) {
        dlog!("UDP", "setsockopt failed", libc::SO_RCVBUF, e);
    }
    if let Err(e) = s.set_send_buffer_size(4 * 1024 * 1024) {
        dlog!("UDP", "setsockopt failed", libc::SO_SNDBUF, e);
    }
    let rcv = s.recv_buffer_size().unwrap_or(0);
    let snd = s.send_buffer_size().unwrap_or(0);
    dlog!("UDP", format!("socket buffers: rcv {} snd {} bytes", rcv, snd));
    let sock: UdpSocket = s.into();
    if sock.set_read_timeout(Some(Duration::from_millis(500))).is_err() {
        return 3;
    }
    let recv = match sock.try_clone() {
        Ok(r) => r,
        Err(_) => return 3,
    };
    let mut net = UdpNet::new(sock);
    let mut relay = Relay::new(lim);
    relay.sock_stats = format!("sockbuf rcv {} KB snd {} KB", rcv / 1024, snd / 1024);
    let mut buf = vec![0u8; 65536];
    let mut next_tick = Instant::now() + Duration::from_secs(10);
    loop {
        match recv.recv_from(&mut buf) {
            Ok((n, SocketAddr::V4(a))) => relay.handle(&buf[..n], a, clock::now(), &mut net),
            Ok(_) => {}
            Err(e) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted) => {}
            Err(e) => dlog!("UDP-ERR", os_error_repr(&e)),
        }
        if Instant::now() >= next_tick {
            next_tick += Duration::from_secs(10);
            relay.status_health();
            relay.status_stats();
        }
    }
}
