use crate::util::{ct_eq, hex, is_digits, now, parse_qs, py_strip, qget, urlencode};
use hmac::{Hmac, Mac};
use sha2::Sha256;
use std::io::Read;
use std::time::Duration;

pub const STEAM_OPENID: &str = "https://steamcommunity.com/openid/login";
pub const SESSION_DAYS: i64 = 30;

pub fn hmac_hex(key: &[u8], msg: &str) -> String {
    match Hmac::<Sha256>::new_from_slice(key) {
        Ok(mut m) => {
            m.update(msg.as_bytes());
            hex(&m.finalize().into_bytes())
        }
        Err(_) => String::new(),
    }
}

pub fn sign(secret: &[u8], sid: &str, exp: i64) -> String {
    hmac_hex(secret, &format!("{}.{}", sid, exp))
}

pub fn csrf_for(secret: &[u8], mac: &str) -> String {
    hmac_hex(secret, &format!("csrf.{}", mac))
}

pub fn session_info(secret: &[u8], cookie: Option<&str>) -> Option<(i64, String)> {
    for part in cookie.unwrap_or("").split(';') {
        let part = py_strip(part);
        let (k, v) = part.split_once('=').unwrap_or((part, ""));
        if k != "tw_s" {
            continue;
        }
        let bits: Vec<&str> = v.split('.').collect();
        if bits.len() != 3 {
            return None;
        }
        let (sid, exp, mac) = (bits[0], bits[1], bits[2]);
        let exp: i64 = crate::util::parse_py_int(exp)?;
        if (exp as f64) > now() && ct_eq(mac.as_bytes(), sign(secret, sid, exp).as_bytes()) && is_digits(sid) {
            if let Ok(n) = sid.parse::<i64>() {
                return Some((n, mac.to_string()));
            }
        }
    }
    None
}

pub fn hostname(url: &str) -> String {
    let rest = match url.find("://") {
        Some(i) => &url[i + 3..],
        None => return String::new(),
    };
    let end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let netloc = &rest[..end];
    let hp = netloc.rsplit_once('@').map_or(netloc, |x| x.1);
    let host = if let Some(r) = hp.strip_prefix('[') {
        r.split(']').next().unwrap_or("")
    } else {
        hp.split(':').next().unwrap_or("")
    };
    host.to_lowercase()
}

pub fn cookie_domain(public_url: &str) -> String {
    let h = hostname(public_url);
    if !h.contains('.') || h.bytes().all(|b| b.is_ascii_digit() || b == b'.') || h.contains(':') {
        return String::new();
    }
    h
}

pub fn valid_next(url: &str, public_host: &str, origins: &[String]) -> Option<String> {
    if url.is_empty() || url.len() > 300 || url.bytes().any(|b| !(33..=126).contains(&b)) || url.contains('\\') || url.contains('%') {
        return None;
    }
    let colon = url.find(':')?;
    let scheme = &url[..colon];
    if !scheme.starts_with(|c: char| c.is_ascii_alphabetic()) || !scheme.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'+' || b == b'-' || b == b'.') {
        return None;
    }
    if !scheme.eq_ignore_ascii_case("https") {
        return None;
    }
    let rest = &url[colon + 1..];
    let rest = rest.strip_prefix("//")?;
    let end = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let netloc = &rest[..end];
    let tail = &rest[end..];
    if netloc.is_empty() || netloc.contains('@') || netloc.contains(':') || netloc.contains('[') || netloc.contains(']') {
        return None;
    }
    let host = netloc.to_lowercase();
    if public_host.is_empty() || !host.ends_with(&format!(".{}", public_host)) || !origins.iter().any(|o| *o == format!("https://{}", host)) {
        return None;
    }
    let (before_frag, frag) = tail.split_once('#').unwrap_or((tail, ""));
    let path = before_frag.split_once('?').map_or(before_frag, |x| x.0);
    if path.starts_with("//") || (!path.is_empty() && !path.starts_with('/')) || !frag.is_empty() {
        return None;
    }
    Some(url.to_string())
}

pub fn steam_verify(q: &[(String, String)], public_url: &str) -> Option<i64> {
    if qget(q, "openid.mode") != Some("id_res") || qget(q, "openid.op_endpoint") != Some(STEAM_OPENID) {
        return None;
    }
    if !qget(q, "openid.return_to").unwrap_or("").starts_with(&format!("{}/auth/steam/return", public_url)) {
        return None;
    }
    let claimed = qget(q, "openid.claimed_id").unwrap_or("");
    let id = claimed.strip_prefix("https://steamcommunity.com/openid/id/")?;
    if id.len() != 17 || !id.starts_with("7656") || !is_digits(id) {
        return None;
    }
    let sid: i64 = id.parse().ok()?;
    let mut pairs: Vec<(String, String)> = q.iter().filter(|(k, _)| k.starts_with("openid.")).cloned().collect();
    for p in pairs.iter_mut() {
        if p.0 == "openid.mode" {
            p.1 = "check_authentication".into();
        }
    }
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(10)).user_agent("twelve-weeks-web").build();
    let resp = agent.post(STEAM_OPENID).set("Content-Type", "application/x-www-form-urlencoded").send_string(&urlencode(&pairs)).ok()?;
    let mut body = Vec::new();
    resp.into_reader().take(1 << 20).read_to_end(&mut body).ok()?;
    if String::from_utf8_lossy(&body).contains("is_valid:true") {
        Some(sid)
    } else {
        None
    }
}

pub fn return_next(return_to: &str) -> String {
    let t = return_to.split_once('#').map_or(return_to, |x| x.0);
    let q = t.split_once('?').map_or("", |x| x.1);
    qget(&parse_qs(q), "next").unwrap_or("").to_string()
}
