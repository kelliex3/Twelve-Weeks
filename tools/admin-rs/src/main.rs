mod clan_pages;
mod daemon;
mod http;
mod logindex;
mod news;
mod py;
mod store;
mod timeutil;

use std::convert::Infallible;
use std::io::{Read, Seek, SeekFrom};
use std::net::SocketAddr;
use std::path::{Path, PathBuf};
use std::pin::Pin;
use std::sync::Arc;
use std::task::{Context, Poll};
use std::time::Duration;

use bytes::Bytes;
use http_body_util::BodyExt;
use hyper::body::{Frame, Incoming, SizeHint};
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Request, Response};
use hyper_util::rt::{TokioIo, TokioTimer};
use tokio::sync::mpsc;

use crate::http::{App, Body, Config, Req};
use crate::store::Admin;

const USAGE: &str = "usage: t2r-admin --db <t2r.db> [--port 7010] [--bind 127.0.0.1] [--daemon-log <daemon.log | auto[:dir]>] [--reports <dir>]
  [--daemon host:port] [--admin-url http://127.0.0.1:7011] [--status-file F] [--update-data <dir>] [--users F]
  [--tunnel-host H]... [--allow-host H]... [--web-secret F] [--web-admins F] [--site-url https://twelve-weeks.com]
  [--test-db F] [--test-admin-url http://127.0.0.1:7111] [--test-daemon-log F] [--test-daemon 127.0.0.1:7101]
  [--static tools/admin_site/static] [--launcher release/launcher]";

struct Args {
    vals: std::collections::HashMap<String, String>,
    multi: std::collections::HashMap<String, Vec<String>>,
}

impl Args {
    fn get(&self, k: &str, d: &str) -> String {
        self.vals.get(k).cloned().unwrap_or_else(|| d.to_string())
    }

    fn list(&self, k: &str) -> Vec<String> {
        self.multi.get(k).cloned().unwrap_or_default()
    }
}

fn parse_args() -> Result<Args, String> {
    let single = [
        "db", "port", "daemon-log", "reports", "daemon", "admin-url", "status-file", "update-data", "bind", "users", "web-secret", "web-admins",
        "test-db", "test-admin-url", "test-daemon-log", "test-daemon", "site-url", "static", "launcher",
    ];
    let multi = ["tunnel-host", "allow-host"];
    let mut a = Args { vals: Default::default(), multi: Default::default() };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        if arg == "-h" || arg == "--help" {
            println!("{}", USAGE);
            std::process::exit(0);
        }
        let Some(name) = arg.strip_prefix("--") else {
            return Err(format!("unrecognized argument: {}", arg));
        };
        let (name, val) = match name.split_once('=') {
            Some((n, v)) => (n.to_string(), v.to_string()),
            None => {
                let v = it.next().ok_or_else(|| format!("argument --{}: expected one argument", name))?;
                (name.to_string(), v)
            }
        };
        if single.contains(&name.as_str()) {
            a.vals.insert(name, val);
        } else if multi.contains(&name.as_str()) {
            a.multi.entry(name).or_default().push(val);
        } else {
            return Err(format!("unrecognized argument: --{}", name));
        }
    }
    if !a.vals.contains_key("db") {
        return Err("the following arguments are required: --db".into());
    }
    Ok(a)
}

enum RespBody {
    Full(Option<Bytes>),
    Stream(mpsc::Receiver<Bytes>),
}

impl hyper::body::Body for RespBody {
    type Data = Bytes;
    type Error = Infallible;

    fn poll_frame(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Option<Result<Frame<Bytes>, Infallible>>> {
        match self.get_mut() {
            RespBody::Full(b) => Poll::Ready(b.take().map(|b| Ok(Frame::data(b)))),
            RespBody::Stream(rx) => rx.poll_recv(cx).map(|o| o.map(|b| Ok(Frame::data(b)))),
        }
    }

    fn is_end_stream(&self) -> bool {
        matches!(self, RespBody::Full(None))
    }

    fn size_hint(&self) -> SizeHint {
        match self {
            RespBody::Full(Some(b)) => SizeHint::with_exact(b.len() as u64),
            RespBody::Full(None) => SizeHint::with_exact(0),
            RespBody::Stream(_) => SizeHint::default(),
        }
    }
}

fn stream_file(path: PathBuf, start: u64, len: u64) -> RespBody {
    let (tx, rx) = mpsc::channel::<Bytes>(2);
    tokio::task::spawn_blocking(move || {
        let Ok(mut f) = std::fs::File::open(&path) else { return };
        if f.seek(SeekFrom::Start(start)).is_err() {
            return;
        }
        let mut left = len;
        while left > 0 {
            let mut buf = vec![0u8; (256 * 1024).min(left as usize)];
            let n = match f.read(&mut buf) {
                Ok(0) | Err(_) => return,
                Ok(n) => n,
            };
            buf.truncate(n);
            left -= n as u64;
            if tx.blocking_send(Bytes::from(buf)).is_err() {
                return;
            }
        }
    });
    RespBody::Stream(rx)
}

async fn serve(app: Arc<App>, peer: SocketAddr, req: Request<Incoming>) -> Result<Response<RespBody>, Infallible> {
    let method = req.method().as_str().to_string();
    let path = req.uri().path().to_string();
    let query = req.uri().query().unwrap_or("").to_string();
    let headers: Vec<(String, String)> = req
        .headers()
        .iter()
        .map(|(k, v)| (k.as_str().to_string(), v.to_str().map(str::to_string).unwrap_or_else(|_| String::from_utf8_lossy(v.as_bytes()).into_owned())))
        .collect();
    let mut body = None;
    if method == "POST" {
        let clen = headers.iter().find(|(k, _)| k == "content-length").map(|(_, v)| v.trim().parse::<i64>());
        let limit = http::body_limit(&path);
        match clen {
            None => body = Some(Vec::new()),
            Some(Ok(n)) if n >= 0 => {
                let want = (n as usize).min(limit);
                let mut got = Vec::with_capacity(want.min(1 << 16));
                let mut inc = req.into_body();
                while got.len() < want {
                    match inc.frame().await {
                        Some(Ok(f)) => {
                            if let Ok(d) = f.into_data() {
                                got.extend_from_slice(&d);
                            }
                        }
                        _ => break,
                    }
                }
                got.truncate(want);
                body = Some(got);
            }
            _ => {}
        }
    }
    let head = method == "HEAD";
    let r = Req { method, path, query, headers, peer: peer.ip(), body };
    let app2 = app.clone();
    let resp = match tokio::task::spawn_blocking(move || app2.handle(r)).await {
        Ok(r) => r,
        Err(_) => {
            let mut b = Response::new(RespBody::Full(Some(Bytes::from_static(b"{\"error\": \"internal error\"}"))));
            *b.status_mut() = hyper::StatusCode::INTERNAL_SERVER_ERROR;
            return Ok(b);
        }
    };
    if resp.delay {
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
    let (len, rb) = match resp.body {
        Body::Bytes(b) => (b.len() as u64, if head { RespBody::Full(None) } else { RespBody::Full(Some(Bytes::from(b))) }),
        Body::File { path, start, len } => (len, if head { RespBody::Full(None) } else { stream_file(path, start, len) }),
    };
    let mut out = Response::new(rb);
    *out.status_mut() = hyper::StatusCode::from_u16(resp.code).unwrap_or(hyper::StatusCode::INTERNAL_SERVER_ERROR);
    let h = out.headers_mut();
    for (k, v) in resp.headers {
        if let (Ok(k), Ok(v)) = (hyper::header::HeaderName::from_bytes(k.as_bytes()), hyper::header::HeaderValue::from_str(&v)) {
            h.append(k, v);
        }
    }
    if let Ok(v) = hyper::header::HeaderValue::from_str(&len.to_string()) {
        h.insert(hyper::header::CONTENT_LENGTH, v);
    }
    Ok(out)
}

fn main() {
    let a = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("{}\nt2r-admin: error: {}", USAGE, e);
            std::process::exit(2);
        }
    };
    let db = a.get("db", "");
    if !Path::new(&db).is_file() {
        eprintln!("database not found: {}", db);
        std::process::exit(1);
    }
    let update_data = a.get("update-data", "");
    let reports = a.get("reports", "");
    let live = Admin::new(&db, &a.get("daemon-log", ""), &reports, &a.get("daemon", ""), &a.get("status-file", ""), &a.get("admin-url", "http://127.0.0.1:7011"), &update_data);
    let test_db = a.get("test-db", "");
    let test_admin_url = a.get("test-admin-url", "http://127.0.0.1:7111");
    let test = if !test_db.is_empty() && Path::new(&test_db).is_file() {
        println!("test server switch: db {}, admin {}", test_db, test_admin_url);
        Some(Admin::new(&test_db, &a.get("test-daemon-log", ""), &reports, &a.get("test-daemon", "127.0.0.1:7101"), "", &test_admin_url, &update_data))
    } else {
        None
    };
    let ddir = live.dbdir.clone();
    let users = a.vals.get("users").filter(|s| !s.is_empty()).map(PathBuf::from).unwrap_or_else(|| ddir.join("admin-users.txt"));
    let launcher = PathBuf::from(a.get("launcher", "release/launcher"));
    let bind = a.get("bind", "127.0.0.1");
    let port: u16 = match a.get("port", "7010").parse() {
        Ok(p) => p,
        Err(_) => {
            eprintln!("{}\nt2r-admin: error: argument --port: invalid int value", USAGE);
            std::process::exit(2);
        }
    };
    let rt = match tokio::runtime::Builder::new_current_thread().enable_all().max_blocking_threads(16).thread_keep_alive(Duration::from_secs(20)).build() {
        Ok(rt) => rt,
        Err(e) => {
            eprintln!("runtime: {}", e);
            std::process::exit(1);
        }
    };
    rt.block_on(async move {
        let listener = match tokio::net::TcpListener::bind((bind.as_str(), port)).await {
            Ok(l) => l,
            Err(e) => {
                eprintln!("cannot listen on {}:{}: {}", bind, port, e);
                std::process::exit(1);
            }
        };
        let port = listener.local_addr().map(|a| a.port()).unwrap_or(port);
        let cfg = Config {
            port,
            users_file: users.clone(),
            allow_hosts: a.list("allow-host").iter().map(|h| h.to_lowercase()).collect(),
            tunnel_hosts: a.list("tunnel-host").iter().map(|h| h.to_lowercase()).collect(),
            web_secret: a.vals.get("web-secret").filter(|s| !s.is_empty()).map(PathBuf::from).unwrap_or_else(|| ddir.join("web-secret")),
            web_admins: a.vals.get("web-admins").filter(|s| !s.is_empty()).map(PathBuf::from).unwrap_or_else(|| ddir.join("web-admins.txt")),
            site_url: a.get("site-url", "https://twelve-weeks.com"),
            static_dir: PathBuf::from(a.get("static", "tools/admin_site/static")),
            launcher_root: launcher.join("assets").is_dir().then_some(launcher),
        };
        let app = Arc::new(App::new(cfg, live, test));
        println!("Twelve Weeks admin site: http://{}:{}/   (db {}, read-only; LAN logins from {})", bind, port, db, users.display());
        // timed bans: drop ended bans.txt lines once a minute (live and test site)
        let sweep = app.clone();
        tokio::spawn(async move {
            loop {
                tokio::time::sleep(Duration::from_secs(60)).await;
                let s = sweep.clone();
                let _ = tokio::task::spawn_blocking(move || {
                    s.live.bans_expire();
                    if let Some(t) = &s.test {
                        t.bans_expire();
                    }
                })
                .await;
            }
        });
        let warm = app.clone();
        tokio::task::spawn_blocking(move || {
            warm.live.refresh_log();
            if let Some(t) = &warm.test {
                t.refresh_log();
            }
        });
        loop {
            let (stream, peer) = match listener.accept().await {
                Ok(s) => s,
                Err(_) => {
                    tokio::time::sleep(Duration::from_millis(50)).await;
                    continue;
                }
            };
            let app = app.clone();
            tokio::spawn(async move {
                let svc = service_fn(move |req| serve(app.clone(), peer, req));
                let _ = http1::Builder::new()
                    .timer(TokioTimer::new())
                    .header_read_timeout(Duration::from_secs(30))
                    .keep_alive(true)
                    .serve_connection(TokioIo::new(stream), svc)
                    .await;
            });
        }
    });
}
