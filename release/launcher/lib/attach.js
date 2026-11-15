/**
 * @file Report attachments: inspect, copy, encode and upload clips.
 */
'use strict';
// REPORT A BUG attachments: optional screenshots (png/jpg/webp/gif/bmp) and video clips (mp4/mov/webm/mkv). The type comes from the file's first bytes, never
// its name; the caps match the update server (tools/update_server/report_store.py), which enforces them again. Images travel inside the report zip as
// files/screenshot-N.<ext>; clips are streamed to the server afterwards, one PUT /report-clip each (uploadClip), so a clip is never held in memory.
const fs = require('fs');
const path = require('path');
const http = require('http');
const https = require('https');

const IMG_MAX_FILES = 5,
  IMG_MAX_EACH = 8 * 1024 * 1024;
const CLIP_MAX_FILES = 2,
  CLIP_MAX_EACH = 200 * 1024 * 1024,
  CLIP_MAX_TOTAL = 250 * 1024 * 1024;
const EXTS = ['png', 'jpg', 'jpeg', 'webp', 'gif', 'bmp', 'mp4', 'mov', 'webm', 'mkv'];
const MIME = {
  png: 'image/png',
  jpg: 'image/jpeg',
  webp: 'image/webp',
  gif: 'image/gif',
  bmp: 'image/bmp',
  mp4: 'video/mp4',
  mov: 'video/quicktime',
  webm: 'video/webm',
  mkv: 'video/x-matroska',
};

/**
 * Detect the media type of a buffer.
 * @param {string} h
 * @returns {string|null}
 */
function sniff(h) {
  // first bytes -> extension or ''
  if (
    h.length >= 8 &&
    h.subarray(0, 8).equals(Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]))
  )
    return 'png';
  if (h.length >= 3 && h[0] === 0xff && h[1] === 0xd8 && h[2] === 0xff) return 'jpg';
  if (
    h.length >= 12 &&
    h.toString('latin1', 0, 4) === 'RIFF' &&
    h.toString('latin1', 8, 12) === 'WEBP'
  )
    return 'webp';
  if (h.length >= 6 && /^GIF8[79]a$/.test(h.toString('latin1', 0, 6))) return 'gif';
  if (h.length >= 14 && h[0] === 0x42 && h[1] === 0x4d) return 'bmp';
  if (h.length >= 12 && h.toString('latin1', 4, 8) === 'ftyp')
    return h.toString('latin1', 8, 12) === 'qt  ' ? 'mov' : 'mp4';
  if (h.length >= 4 && h[0] === 0x1a && h[1] === 0x45 && h[2] === 0xdf && h[3] === 0xa3)
    return h.toString('latin1', 0, 64).includes('webm') ? 'webm' : 'mkv';
  return '';
}
/**
 * Check whether a media type is a supported video.
 * @param {string} kind
 * @returns {boolean}
 */
const isVideo = (ext) => ['mp4', 'mov', 'webm', 'mkv'].includes(ext);
/**
 * Format a byte count as megabytes.
 * @param {number} n
 * @returns {string}
 */
const mb = (n) => Math.round(n / 1048576);

// -> { ok:true, item:{name,path,size,ext,kind} } | { ok:false, reason }. Messages are English keys (translated in en.json / pl.json).
/**
 * Inspect an attachment candidate.
 * @param {string} file
 * @returns {object}
 */
function inspect(file) {
  const name = path.basename(String(file || ''));
  const nameExt = path.extname(name).slice(1).toLowerCase();
  if (!EXTS.includes(nameExt))
    return {
      ok: false,
      name,
      reason: 'Not a supported file (png, jpg, webp, gif, bmp, mp4, mov, webm, mkv).',
    };
  let st,
    fd,
    head = Buffer.alloc(0);
  try {
    st = fs.statSync(file);
    if (!st.isFile()) return { ok: false, name, reason: 'Not a file.' };
    fd = fs.openSync(file, 'r');
    const b = Buffer.alloc(64);
    const n = fs.readSync(fd, b, 0, 64, 0);
    head = b.subarray(0, n);
  } catch {
    return { ok: false, name, reason: 'The file could not be read.' };
  } finally {
    if (fd !== undefined)
      try {
        fs.closeSync(fd);
      } catch {
        /* ignore */
      }
  }
  const ext = sniff(head);
  if (!ext)
    return {
      ok: false,
      name,
      reason: 'The file is not a real image or video of that type.',
    };
  const kind = isVideo(ext) ? 'video' : 'image';
  if (kind === 'image' && st.size > IMG_MAX_EACH)
    return {
      ok: false,
      name,
      reason: 'Screenshot too big - it must be under 8 MB.',
    };
  if (kind === 'video' && st.size > CLIP_MAX_EACH)
    return {
      ok: false,
      name,
      reason: 'Clip too big - trim it to under 200 MB',
    };
  if (st.size <= 0) return { ok: false, name, reason: 'The file is empty.' };
  return { ok: true, item: { name, path: file, size: st.size, ext, kind } };
}

// adds `files` to `list` (mutated) under the count / size caps; returns [{name, reason}] for every refused one
/**
 * Add a list of attachment candidates.
 * @param {object[]} list
 * @param {object} files
 * @returns {void}
 */
function addAll(list, files) {
  const rejected = [];
  for (const f of files) {
    const r = inspect(f);
    if (!r.ok) {
      rejected.push({ name: r.name, reason: r.reason });
      continue;
    }
    const it = r.item;
    if (list.some((x) => path.resolve(x.path) === path.resolve(it.path))) continue;
    const same = list.filter((x) => x.kind === it.kind);
    if (it.kind === 'image' && same.length >= IMG_MAX_FILES) {
      rejected.push({
        name: it.name,
        reason: 'At most 5 screenshots per report.',
      });
      continue;
    }
    if (it.kind === 'video') {
      if (same.length >= CLIP_MAX_FILES) {
        rejected.push({ name: it.name, reason: 'At most 2 clips per report.' });
        continue;
      }
      if (same.reduce((a, x) => a + x.size, 0) + it.size > CLIP_MAX_TOTAL) {
        rejected.push({
          name: it.name,
          reason: 'Clips together are too big - 250 MB at most per report.',
        });
        continue;
      }
    }
    it.id = Math.random().toString(36).slice(2, 10);
    list.push(it);
  }
  return rejected;
}

// screenshot entries for rpt.build: [{name:'screenshot-1.png', data}]. Re-checks the bytes at send time (the file may have changed since it was added).
/**
 * Collect screenshot entries for a report.
 * @param {object[]} list
 * @returns {object[]}
 */
function screenshotEntries(list) {
  const out = [];
  for (const it of list.filter((x) => x.kind === 'image')) {
    const r = inspect(it.path);
    if (!r.ok || r.item.kind !== 'image') continue;
    out.push({
      name: `screenshot-${out.length + 1}.${r.item.ext}`,
      data: fs.readFileSync(it.path),
    });
  }
  return out;
}

// streams one clip: PUT /report-clip?code&key, body piped from disk (64 KB pieces, back-pressure), idle timeout 3 min (the playit relay is slow, not dead).
// onProgress(sent, total). -> { status, json }
/**
 * Upload a clip attachment.
 * @param {string} host
 * @param {string} code
 * @param {string} key
 * @param {string} file
 * @param {Function} onProgress
 * @param {object} [opts]
 * @returns {Promise<object>}
 */
function uploadClip(host, code, key, file, onProgress, opts = {}) {
  return new Promise((resolve, reject) => {
    let st;
    try {
      st = fs.statSync(file);
    } catch (e) {
      return reject(e);
    }
    const u = new URL(
        `${host.replace(/\/+$/, '')}/report-clip?code=${encodeURIComponent(code)}&key=${encodeURIComponent(key)}`,
      ),
      lib = u.protocol === 'https:' ? https : http;
    const req = lib.request(
      u,
      {
        method: 'PUT',
        headers: {
          'Content-Type': 'application/octet-stream',
          'Content-Length': st.size,
          'User-Agent': 'TwelveWeeks-Launcher',
        },
        timeout: opts.timeout || 180000,
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
    req.on('error', (e) => reject(e));
    const rs = fs.createReadStream(file, { highWaterMark: 64 * 1024 });
    let sent = 0,
      last = 0;
    rs.on('data', (c) => {
      sent += c.length;
      if (onProgress && (sent - last > 512 * 1024 || sent === st.size)) {
        last = sent;
        onProgress(sent, st.size);
      }
    });
    rs.on('error', (e) => req.destroy(e));
    rs.pipe(req);
  });
}

module.exports = {
  IMG_MAX_FILES,
  IMG_MAX_EACH,
  CLIP_MAX_FILES,
  CLIP_MAX_EACH,
  CLIP_MAX_TOTAL,
  MIME,
  sniff,
  inspect,
  addAll,
  screenshotEntries,
  uploadClip,
  mb,
};
