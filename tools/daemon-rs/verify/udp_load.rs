use std::net::{SocketAddr, UdpSocket};
use std::os::fd::AsRawFd;
use std::time::{Duration, Instant};

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 5 {
        eprintln!("usage: t2r-udp-load <ip:port> <clients> <keepalives per client per s> <seconds>");
        std::process::exit(2);
    }
    let target: SocketAddr = match args[1].parse() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("bad target: {}", e);
            std::process::exit(2);
        }
    };
    let n: usize = args[2].parse().unwrap_or(65);
    let rate: f64 = args[3].parse().unwrap_or(1.0);
    let secs: f64 = args[4].parse().unwrap_or(60.0);
    let mut socks = Vec::with_capacity(n);
    for _ in 0..n {
        let s = match UdpSocket::bind("0.0.0.0:0") {
            Ok(s) => s,
            Err(e) => {
                eprintln!("bind: {}", e);
                std::process::exit(1);
            }
        };
        let _ = s.set_nonblocking(true);
        socks.push(s);
    }
    let start = Instant::now();
    let period = Duration::from_secs_f64(1.0 / rate);
    let mut next: Vec<Instant> = (0..n).map(|i| start + period.mul_f64(i as f64 / n as f64)).collect();
    let mut seq = vec![0u32; n];
    let mut fds: Vec<libc::pollfd> = socks.iter().map(|s| libc::pollfd { fd: s.as_raw_fd(), events: libc::POLLIN, revents: 0 }).collect();
    let (mut sent, mut pongs, mut relayed, mut other) = (0u64, 0u64, 0u64, 0u64);
    let mut buf = [0u8; 2048];
    let end = start + Duration::from_secs_f64(secs);
    while Instant::now() < end {
        let now = Instant::now();
        for i in 0..n {
            if now >= next[i] {
                seq[i] = seq[i].wrapping_add(1);
                let mut p = vec![0x03u8];
                p.extend_from_slice(&seq[i].to_le_bytes());
                p.extend_from_slice(&[0u8; 20]);
                if socks[i].send_to(&p, target).is_ok() {
                    sent += 1;
                }
                next[i] += period;
            }
        }
        let wait = next.iter().min().map(|t| t.saturating_duration_since(Instant::now())).unwrap_or(period);
        let ms = wait.as_millis().min(50) as i32;
        let r = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, ms) };
        if r <= 0 {
            continue;
        }
        for (i, f) in fds.iter_mut().enumerate() {
            if f.revents & libc::POLLIN != 0 {
                while let Ok((k, _)) = socks[i].recv_from(&mut buf) {
                    match buf.first() {
                        Some(0x04) if k == 5 => pongs += 1,
                        Some(0x03) => relayed += 1,
                        _ => other += 1,
                    }
                }
            }
            f.revents = 0;
        }
    }
    println!("sent {} keepalives, got {} pongs, {} relayed keepalives, {} other", sent, pongs, relayed, other);
}
