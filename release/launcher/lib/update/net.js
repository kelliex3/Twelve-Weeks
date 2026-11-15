/**
 * @file HTTP transport, JSON caching and manifest fetching.
 */
'use strict';

const http = require('http');
const https = require('https');
const { safeRel, tunables, sleep, isNetErr, baseUrl, DECODERS } = require('./util');

/**
 * Perform a cached HTTP request with timeout and retry policy.
 * @param {string} url
 * @param {{headers?: object, timeout?: number, method?: string}} [options]
 * @returns {Promise<unknown>}
 */
function request(url, { headers = {}, timeout = 15000, method = 'GET' } = {}) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const lib = u.protocol === 'https:' ? https : http;
    const req = lib.request(
      u,
      {
        method,
        headers: { 'User-Agent': 'TwelveWeeks-Launcher', ...headers },
        timeout,
      },
      (res) => {
        if ([301, 302, 307, 308].includes(res.statusCode) && res.headers.location) {
          res.resume();
          return resolve(
            request(new URL(res.headers.location, url).toString(), {
              headers,
              timeout,
              method,
            }),
          );
        }
        resolve(res);
      },
    );
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject);
    req.end();
  });
}

// The update server is down until its tunnel exists: one failed network call marks it unreachable for a minute, so the other startup calls
// (status, manifest, news, clan, feed) fail at once instead of each waiting for its own timeout. An explicit user action calls resetDown().
let downUntil = 0;
/**
 * Clear the cached offline state after a successful request.
 * @returns {void}
 */
const resetDown = () => {
  downUntil = 0;
};
// (a new player saw 'update server unreachable' while the host answered here in 0.7 s: the playit relay is far from some players and a cold
// connection takes longer than the old 2.5 s cap; one such timeout marked the host down for a minute, the manifest included.) The cap is 8 s now and
// the manifest asks with force (it never takes the shortcut): only the cosmetic calls (status, news, clan, feed) skip a host that just failed.
/**
 * Fetch and cache a JSON document.
 * @param {string} url
 * @param {number} [timeout]
 * @param {{force?: boolean}} [options]
 * @returns {Promise<unknown>}
 */
async function getJson(url, timeout = 2500, { force = false } = {}) {
  if (!force && Date.now() < downUntil) throw new Error('update server unreachable');
  let res;
  try {
    res = await request(url, {
      timeout: Math.min(timeout, 8000),
      headers: { Accept: 'application/json' },
    });
  } catch (e) {
    if (isNetErr(e)) downUntil = Date.now() + 60000;
    throw e;
  }
  if (res.statusCode !== 200) {
    res.resume();
    throw new Error(`HTTP ${res.statusCode}`);
  }
  const chunks = [];
  let n = 0;
  for await (const c of res) {
    n += c.length;
    if (n > 8 * 1024 * 1024) throw new Error('response too large');
    chunks.push(c);
  }
  return JSON.parse(Buffer.concat(chunks).toString('utf8'));
}

/**
 * Fetch a small binary file (news media) into memory.
 * @param {string} url
 * @param {number} [timeout]
 * @param {number} [max] size cap in bytes
 * @returns {Promise<Buffer>}
 */
async function getBuffer(url, timeout = 8000, max = 6 * 1024 * 1024) {
  const res = await request(url, { timeout: Math.min(timeout, 15000) });
  if (res.statusCode !== 200) {
    res.resume();
    throw new Error(`HTTP ${res.statusCode}`);
  }
  const chunks = [];
  let n = 0;
  for await (const c of res) {
    n += c.length;
    if (n > max) throw new Error('file too large');
    chunks.push(c);
  }
  return Buffer.concat(chunks);
}

/**
 * Validate and normalize a decoded update manifest.
 * @param {object} m
 * @param {string} base
 * @returns {object}
 */
function validateManifest(m, base) {
  if (!m || !Array.isArray(m.files)) throw new Error('Bad manifest (no files list).');
  const root = baseUrl(base);
  for (const f of m.files) {
    f.path = safeRel(f.path);
    if (!/^[0-9a-f]{64}$/i.test(f.sha256 || '') || !Number.isFinite(f.size))
      throw new Error(`Bad manifest entry: ${f.path}`);
    f.sha256 = f.sha256.toLowerCase();
    f.url = new URL(f.url || `/files/${f.sha256}`, root + '/').toString();
    // compressed transfer (optional): only used when the codec is one this runtime can decode and the entry is sane
    if (f.zurl && DECODERS[f.enc] && Number.isFinite(f.zsize) && f.zsize > 0)
      f.zurl = new URL(f.zurl, root + '/').toString();
    else {
      delete f.zurl;
      delete f.zsize;
      delete f.enc;
    }
  }
  return m;
}

/**
 * Fetch and validate the update manifest for a channel.
 * @param {string} base
 * @param {string} channel
 * @param {{retries?: number}} [options]
 * @returns {Promise<object>}
 */
async function fetchManifest(base, channel, { retries = 0 } = {}) {
  let m;
  for (let i = 0; ; i++) {
    try {
      m = await getJson(
        `${baseUrl(base)}/manifest.json?channel=${encodeURIComponent(channel || 'stable')}`,
        8000,
        { force: true },
      );
      break;
    } catch (e) {
      // an explicit update retries a few times (a restarting server / tunnel is back within seconds); the startup check does not wait
      if (i >= retries || !(isNetErr(e) || /HTTP 5\d\d/.test(e.message))) throw e;
      resetDown();
      await sleep(tunables.netBackoffMs * (i + 1));
    }
  }
  return validateManifest(m, base);
}

module.exports = { request, resetDown, getJson, getBuffer, fetchManifest };
