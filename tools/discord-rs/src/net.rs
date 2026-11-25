use bytes::Bytes;
use http_body_util::{BodyExt, Full, Limited};
use hyper::header::{HeaderValue, AUTHORIZATION, CONTENT_TYPE, LOCATION, USER_AGENT};
use hyper::{Method, Request, Uri};
use hyper_rustls::HttpsConnector;
use hyper_util::client::legacy::connect::HttpConnector;
use hyper_util::client::legacy::Client;
use hyper_util::rt::TokioExecutor;
use serde_json::Value;
use std::collections::HashMap;
use std::fmt;
use std::sync::Mutex;
use std::time::{Duration, Instant};

const API: &str = "https://discord.com/api/v10";
const API_UA: &str = "DiscordBot (https://twelve-weeks.com, 1.0)";
const TIMEOUT: Duration = Duration::from_secs(30);
const MAX_BODY: usize = 64 << 20;

type Https = Client<HttpsConnector<HttpConnector>, Full<Bytes>>;

pub fn https_client() -> Https {
    let conn = hyper_rustls::HttpsConnectorBuilder::new()
        .with_webpki_roots()
        .https_or_http()
        .enable_http1()
        .build();
    Client::builder(TokioExecutor::new()).pool_idle_timeout(Duration::from_secs(60)).pool_max_idle_per_host(2).build(conn)
}

#[derive(Debug)]
pub struct ApiError {
    pub status: u16,
    pub code: i64,
    pub retry_after: f64,
    pub message: String,
}

impl fmt::Display for ApiError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.status == 0 {
            write!(f, "{}", self.message)
        } else {
            write!(f, "{} (code {}): {}", self.status, self.code, self.message)
        }
    }
}

impl ApiError {
    fn local(message: impl Into<String>) -> ApiError {
        ApiError { status: 0, code: 0, retry_after: 0.0, message: message.into() }
    }
}

pub struct File {
    pub name: String,
    pub data: Vec<u8>,
}

pub enum Body<'a> {
    Empty,
    Json(&'a Value),
    Multipart(&'a Value, &'a [File]),
}

pub struct Rest {
    http: Https,
    auth: HeaderValue,
    blocked: Mutex<HashMap<String, Instant>>,
}

fn bucket_key(method: &Method, path: &str) -> String {
    let path = path.split('?').next().unwrap_or(path);
    let mut out = String::with_capacity(path.len() + 8);
    out.push_str(method.as_str());
    let mut prev = "";
    for seg in path.trim_start_matches('/').split('/') {
        out.push('/');
        let major = matches!(prev, "channels" | "guilds" | "webhooks" | "interactions");
        if !major && !seg.is_empty() && seg.bytes().all(|b| b.is_ascii_digit()) {
            out.push_str(":id");
        } else {
            out.push_str(seg);
        }
        prev = seg;
    }
    out
}

fn encode_reason(reason: &str) -> String {
    let mut out = String::new();
    for b in reason.bytes() {
        if b.is_ascii_alphanumeric() || b"-_.~#:,()!'".contains(&b) {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

fn multipart(payload: &Value, files: &[File], boundary: &str) -> Vec<u8> {
    let mut body = Vec::new();
    body.extend_from_slice(format!("--{boundary}\r\nContent-Disposition: form-data; name=\"payload_json\"\r\nContent-Type: application/json\r\n\r\n").as_bytes());
    body.extend_from_slice(payload.to_string().as_bytes());
    body.extend_from_slice(b"\r\n");
    for (i, f) in files.iter().enumerate() {
        let name: String = f.name.chars().map(|c| if c == '"' || c == '\r' || c == '\n' { '_' } else { c }).collect();
        body.extend_from_slice(
            format!("--{boundary}\r\nContent-Disposition: form-data; name=\"files[{i}]\"; filename=\"{name}\"\r\nContent-Type: application/octet-stream\r\n\r\n").as_bytes(),
        );
        body.extend_from_slice(&f.data);
        body.extend_from_slice(b"\r\n");
    }
    body.extend_from_slice(format!("--{boundary}--\r\n").as_bytes());
    body
}

fn boundary() -> String {
    let t = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0);
    format!("twelveweeks{t:x}")
}

impl Rest {
    pub fn new(token: &str) -> Result<Rest, String> {
        let auth = HeaderValue::from_str(&format!("Bot {token}")).map_err(|_| "token has characters a header cannot carry".to_string())?;
        Ok(Rest { http: https_client(), auth, blocked: Mutex::new(HashMap::new()) })
    }

    pub async fn get(&self, path: &str) -> Result<Value, ApiError> {
        self.call(Method::GET, path, Body::Empty, None, true).await
    }

    pub async fn post(&self, path: &str, body: &Value, reason: Option<&str>) -> Result<Value, ApiError> {
        self.call(Method::POST, path, Body::Json(body), reason, true).await
    }

    pub async fn patch(&self, path: &str, body: &Value, reason: Option<&str>) -> Result<Value, ApiError> {
        self.call(Method::PATCH, path, Body::Json(body), reason, true).await
    }

    pub async fn put(&self, path: &str, body: &Value, reason: Option<&str>) -> Result<Value, ApiError> {
        self.call(Method::PUT, path, Body::Json(body), reason, true).await
    }

    pub async fn put_empty(&self, path: &str, reason: Option<&str>) -> Result<Value, ApiError> {
        self.call(Method::PUT, path, Body::Empty, reason, true).await
    }

    pub async fn delete(&self, path: &str, reason: Option<&str>) -> Result<Value, ApiError> {
        self.call(Method::DELETE, path, Body::Empty, reason, true).await
    }

    pub async fn call(&self, method: Method, path: &str, body: Body<'_>, reason: Option<&str>, retry_429: bool) -> Result<Value, ApiError> {
        let key = bucket_key(&method, path);
        let (data, ctype) = match &body {
            Body::Empty => (Vec::new(), None),
            Body::Json(v) => (v.to_string().into_bytes(), Some("application/json".to_string())),
            Body::Multipart(v, files) => {
                let b = boundary();
                (multipart(v, files, &b), Some(format!("multipart/form-data; boundary={b}")))
            }
        };
        let data = Bytes::from(data);
        let mut tries = 0;
        loop {
            tries += 1;
            let wait = self.blocked.lock().ok().and_then(|m| m.get(&key).or_else(|| m.get("global")).copied());
            if let Some(until) = wait {
                let now = Instant::now();
                if until > now {
                    tokio::time::sleep(until - now).await;
                }
            }
            let uri: Uri = format!("{API}{path}").parse().map_err(|_| ApiError::local(format!("bad path {path}")))?;
            let mut rq = Request::builder().method(method.clone()).uri(uri).header(AUTHORIZATION, self.auth.clone()).header(USER_AGENT, API_UA);
            if let Some(ct) = &ctype {
                rq = rq.header(CONTENT_TYPE, ct.as_str());
            }
            if let Some(r) = reason {
                rq = rq.header("X-Audit-Log-Reason", encode_reason(r));
            }
            let rq = rq.body(Full::new(data.clone())).map_err(|e| ApiError::local(e.to_string()))?;
            let resp = match tokio::time::timeout(TIMEOUT, self.http.request(rq)).await {
                Ok(Ok(r)) => r,
                Ok(Err(e)) => return Err(ApiError::local(format!("{method} {path}: {e}"))),
                Err(_) => return Err(ApiError::local(format!("{method} {path}: timed out"))),
            };
            let status = resp.status().as_u16();
            let hdr = |n: &str| resp.headers().get(n).and_then(|v| v.to_str().ok()).map(str::to_string);
            let remaining = hdr("x-ratelimit-remaining");
            let reset_after = hdr("x-ratelimit-reset-after").and_then(|v| v.parse::<f64>().ok());
            let global = hdr("x-ratelimit-global").is_some();
            let bytes = match tokio::time::timeout(TIMEOUT, Limited::new(resp.into_body(), MAX_BODY).collect()).await {
                Ok(Ok(b)) => b.to_bytes(),
                Ok(Err(e)) => return Err(ApiError::local(format!("{method} {path}: {e}"))),
                Err(_) => return Err(ApiError::local(format!("{method} {path}: body timed out"))),
            };
            if let (Some("0"), Some(ra)) = (remaining.as_deref(), reset_after) {
                if let Ok(mut m) = self.blocked.lock() {
                    m.insert(key.clone(), Instant::now() + Duration::from_secs_f64(ra.clamp(0.0, 600.0)));
                }
            }
            let json: Value = if bytes.is_empty() { Value::Null } else { serde_json::from_slice(&bytes).unwrap_or(Value::Null) };
            if (200..300).contains(&status) {
                return Ok(json);
            }
            let retry_after = json.get("retry_after").and_then(Value::as_f64).unwrap_or(0.0);
            let err = ApiError {
                status,
                code: json.get("code").and_then(Value::as_i64).unwrap_or(0),
                retry_after,
                message: json.get("message").and_then(Value::as_str).map(str::to_string).unwrap_or_else(|| {
                    String::from_utf8_lossy(&bytes[..bytes.len().min(200)]).into_owned()
                }),
            };
            if status == 429 {
                let until = Instant::now() + Duration::from_secs_f64(retry_after.clamp(0.0, 3600.0));
                if let Ok(mut m) = self.blocked.lock() {
                    m.insert(if global || json.get("global").and_then(Value::as_bool) == Some(true) { "global".into() } else { key.clone() }, until);
                }
                if retry_429 && tries < 4 && retry_after <= 60.0 {
                    continue;
                }
            }
            return Err(err);
        }
    }

    pub async fn fetch(&self, method: Method, url: &str, ua: &str, max: usize) -> Result<(u16, Vec<u8>), String> {
        fetch(&self.http, method, url, ua, max).await
    }
}

pub async fn fetch(http: &Https, method: Method, url: &str, ua: &str, max: usize) -> Result<(u16, Vec<u8>), String> {
    let mut url = url.to_string();
    for _ in 0..6 {
        let uri: Uri = url.parse().map_err(|_| format!("bad url {url}"))?;
        let rq = Request::builder()
            .method(method.clone())
            .uri(uri.clone())
            .header(USER_AGENT, ua)
            .body(Full::new(Bytes::new()))
            .map_err(|e| e.to_string())?;
        let resp = match tokio::time::timeout(TIMEOUT, http.request(rq)).await {
            Ok(Ok(r)) => r,
            Ok(Err(e)) => return Err(format!("{url}: {e}")),
            Err(_) => return Err(format!("{url}: timed out")),
        };
        let status = resp.status();
        if status.is_redirection() {
            if let Some(loc) = resp.headers().get(LOCATION).and_then(|v| v.to_str().ok()) {
                url = if loc.starts_with("http://") || loc.starts_with("https://") {
                    loc.to_string()
                } else if loc.starts_with('/') {
                    format!("{}://{}{}", uri.scheme_str().unwrap_or("https"), uri.authority().map(|a| a.as_str()).unwrap_or(""), loc)
                } else {
                    return Err(format!("{url}: odd redirect {loc}"));
                };
                continue;
            }
        }
        if method == Method::HEAD {
            return Ok((status.as_u16(), Vec::new()));
        }
        let body = match tokio::time::timeout(TIMEOUT, Limited::new(resp.into_body(), max).collect()).await {
            Ok(Ok(b)) => b.to_bytes().to_vec(),
            Ok(Err(e)) => return Err(format!("{url}: {e}")),
            Err(_) => return Err(format!("{url}: body timed out")),
        };
        return Ok((status.as_u16(), body));
    }
    Err(format!("{url}: too many redirects"))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn buckets_keep_major_params() {
        assert_eq!(bucket_key(&Method::PATCH, "/channels/123/messages/456"), "PATCH/channels/123/messages/:id");
        assert_eq!(bucket_key(&Method::PUT, "/guilds/1/members/2/roles/3"), "PUT/guilds/1/members/:id/roles/:id");
        assert_eq!(bucket_key(&Method::GET, "/channels/9/messages?limit=100"), "GET/channels/9/messages");
    }

    #[test]
    fn reason_is_header_safe() {
        assert_eq!(encode_reason("live player counts"), "live%20player%20counts");
        assert_eq!(encode_reason("faction pick"), "faction%20pick");
        assert_eq!(encode_reason("ü\n"), "%C3%BC%0A");
    }

    #[test]
    fn multipart_layout() {
        let v = serde_json::json!({"content": "x"});
        let body = multipart(&v, &[File { name: "a.png".into(), data: vec![1, 2] }], "B");
        let s = String::from_utf8_lossy(&body);
        assert!(s.starts_with("--B\r\nContent-Disposition: form-data; name=\"payload_json\""));
        assert!(s.contains("name=\"files[0]\"; filename=\"a.png\""));
        assert!(s.ends_with("--B--\r\n"));
    }
}
