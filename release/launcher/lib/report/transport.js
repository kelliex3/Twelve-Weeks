/**
 * @file Report upload transport, retry policy and durable send queue.
 */
'use strict';

const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');
const { QUEUE_MAX_FILES, QUEUE_MAX_BYTES, QUEUE_MAX_AGE } = require('./constants');

/**
 * Upload a report body with progress reporting.
 * @param {string} url
 * @param {Buffer} body
 * @param {Function} onProgress
 * @param {object} [opts]
 * @returns {Promise<object>}
 */
function post(url, body, onProgress, opts = {}) {
  return new Promise((resolve, reject) => {
    const u = new URL(url),
      lib = u.protocol === 'https:' ? https : http;
    const req = lib.request(
      u,
      {
        method: 'POST',
        headers: {
          'Content-Type': 'application/zip',
          'Content-Length': body.length,
          'User-Agent': 'TwelveWeeks-Launcher',
        },
        timeout: opts.timeout || 120000,
      },
      (res) => {
        const ch = [];
        res.on('data', (c) => ch.push(c));
        res.on('error', reject);
        res.on('aborted', () => reject(new Error('connection lost')));
        res.on('end', () => {
          let j = null;
          try {
            j = JSON.parse(Buffer.concat(ch).toString('utf8'));
          } catch {
            /* not json */
          }
          resolve({ status: res.statusCode, json: j });
        });
      },
    );
    req.on('timeout', () => req.destroy(new Error('timeout')));
    req.on('error', reject);
    let o = 0;
    const step = 256 * 1024;
    (function write() {
      while (o < body.length) {
        const end = Math.min(body.length, o + step),
          ok = req.write(body.subarray(o, end));
        o = end;
        if (onProgress) onProgress(o, body.length);
        if (!ok) return req.once('drain', write);
      }
      req.end();
    })();
  });
}

// ---- offline at send time: the finished zip is kept in <dir> and sent later. flushQueue() runs when the update server answers again.
/**
 * Check whether an upload error is retryable.
 * @param {unknown} e
 * @returns {boolean}
 */
const retryable = (e) =>
  !e ||
  /timeout|ECONN|ENOTFOUND|EHOSTUNREACH|ENETUNREACH|EAI_AGAIN|EPIPE|connection lost|socket hang up|ETIMEDOUT/i.test(
    String(e.code || e.message || e),
  );
/**
 * Check whether an HTTP status is retryable.
 * @param {number} st
 * @returns {boolean}
 */
const retryableStatus = (st) => st === 0 || st === 408 || st === 429 || st >= 500;
/**
 * List queued report archives.
 * @param {string} dir
 * @returns {object[]}
 */
function queueList(dir) {
  try {
    return fs
      .readdirSync(dir)
      .filter((n) => /^\d+-[0-9a-f]+\.zip$/.test(n))
      .map((n) => {
        const p = path.join(dir, n),
          st = fs.statSync(p);
        return { n, p, size: st.size, ts: parseInt(n, 10) };
      })
      .sort((a, b) => a.ts - b.ts);
  } catch {
    return [];
  }
}
/**
 * Persist a report archive in the durable send queue.
 * @param {string} dir
 * @param {Buffer} zip
 * @param {number} [now]
 * @returns {object}
 */
function enqueue(dir, zip, now = Date.now()) {
  fs.mkdirSync(dir, { recursive: true });
  const name = `${now}-${require('crypto').randomBytes(4).toString('hex')}.zip`,
    p = path.join(dir, name),
    tmp = p + '.tmp';
  fs.writeFileSync(tmp, zip);
  fs.renameSync(tmp, p);
  // bounded: stale ones and the oldest beyond the caps go (a player offline for weeks does not fill the drive with reports)
  let q = queueList(dir);
  for (const x of q.filter((x) => now - x.ts > QUEUE_MAX_AGE)) {
    try {
      fs.unlinkSync(x.p);
    } catch {}
  }
  q = queueList(dir);
  while (
    q.length > 1 &&
    (q.length > QUEUE_MAX_FILES || q.reduce((a, x) => a + x.size, 0) > QUEUE_MAX_BYTES)
  ) {
    const x = q.shift();
    try {
      fs.unlinkSync(x.p);
    } catch {}
  }
  return p;
}
// sends every queued report; { sent: [codes], kept: n, dropped: n }. A report the server refuses for good (400/413) is dropped; network trouble /
// 429 / 5xx keeps it for the next time. Stops at the first retryable failure (the server is down: the rest would fail the same way).
/**
 * Retry queued report uploads.
 * @param {string} dir
 * @param {string} host
 * @param {Function} [poster]
 * @returns {Promise<object>}
 */
async function flushQueue(dir, host, poster = post) {
  const out = { sent: [], kept: 0, dropped: 0 };
  const q = queueList(dir);
  for (let i = 0; i < q.length; i++) {
    const x = q[i];
    let r;
    try {
      r = await poster(`${String(host).replace(/\/+$/, '')}/report`, fs.readFileSync(x.p));
    } catch (e) {
      if (retryable(e)) {
        out.kept += q.length - i;
        return out;
      }
      r = { status: 0, json: null };
    }
    if (r.status === 200 && r.json && r.json.ok) {
      out.sent.push(r.json.code);
      try {
        fs.unlinkSync(x.p);
      } catch {}
      continue;
    }
    if (retryableStatus(r.status)) {
      out.kept += q.length - i;
      return out;
    }
    out.dropped++;
    try {
      fs.unlinkSync(x.p);
    } catch {}
  }
  return out;
}

module.exports = {
  post,
  retryable,
  retryableStatus,
  queueList,
  enqueue,
  flushQueue,
};
