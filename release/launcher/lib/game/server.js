/**
 * @file Server address parsing, resolution and probing.
 */
'use strict';

const net = require('net');
const dns = require('dns');
const { withTimeout } = require('./util');
const hosts = require('../update/hosts');

/**
 * Parse a host:port server string.
 * @param {string} s
 * @returns {{ip: string, port: number}|null}
 */
function parseServer(s) {
  s = String(s || '')
    .replace(/^\s*[a-zA-Z]+:\/\//, '')
    .trim()
    .replace(/\/+$/, '');
  const m = /^([^:\s]+):(\d+)$/.exec(s);
  if (!m)
    throw new Error(
      `The server address '${s}' is wrong. It must look like name:port (for example abc-123.joinmc.link:12345).`,
    );
  const port = parseInt(m[2], 10);
  if (port < 1 || port > 65535)
    throw new Error(`The server port ${port} in '${s}' is not valid (1-65535).`);
  return { host: m[1], port };
}

/**
 * Resolve and validate the configured server target.
 * @param {string} s
 * @returns {Promise<object|null>}
 */
// A TCP connect to ip:port within ms (true = something accepted it). tests: deps.tcpOpen replaces it
function tcpOpen(ip, port, ms) {
  return new Promise((resolve) => {
    const sock = net.connect({ host: ip, port });
    const done = (ok) => {
      sock.destroy();
      resolve(ok);
    };
    sock.setTimeout(ms, () => done(false));
    sock.once('connect', () => done(true));
    sock.once('error', () => done(false));
  });
}

async function resolveServer(s, deps = {}) {
  const { host, port } = parseServer(s);
  if (net.isIPv4(host)) return { ip: host, port };
  const lookup = deps.lookup || ((h) => dns.promises.lookup(h, { family: 4 }));
  const open = deps.tcpOpen || tcpOpen;
  try {
    const r = await withTimeout(lookup(host), 30000, `Looking up ${host}`);
    // Some providers' DNS filters answer our own name with their block page
    // address (a foreign TLS certificate, nothing on the game port). The lookup "works", so the game was sent there and the player saw
    // "Cannot reach the server". Our own name whose answer is not the server's IP and does not take the game port: the server's own IP,
    // when that one does.
    const known = hosts.knownIp(host);
    if (
      known &&
      r.address !== known &&
      !(await open(r.address, port, 4000)) &&
      (await open(known, port, 4000))
    )
      return {
        ip: known,
        port,
        viaKnownIp: true,
        lookupError: `${host} resolved to ${r.address}, which does not answer on port ${port} (a provider filter or DNS block)`,
      };
    return { ip: r.address, port };
  } catch (e) {
    // our own server name: fall back to its bare IP (a local DNS or filter can fail on the game server name only)
    const known = hosts.knownIp(host);
    if (known)
      return {
        ip: known,
        port,
        viaKnownIp: true,
        lookupError: (e && (e.code || e.message)) || 'lookup failed',
      };
    if (e && e.code === 'ETIMEDOUT')
      throw new Error(
        `Looking up the server name '${host}' took longer than 30 s. Your internet connection or DNS is very slow right now - try again in a moment.`,
        { cause: e },
      );
    throw new Error(
      `The server name '${host}' was not found. Check the address for typos and that your internet connection works.`,
      { cause: e },
    );
  }
}

// 0xB0 version probe, same frames as Test-Server in T2R-Play.ps1. Returns {reachable, versionOk, serverVersion, text, older}
/**
 * Probe the game server with a protocol handshake.
 * @param {string} ip
 * @param {number} port
 * @param {string} token
 * @param {string} version
 * @param {number} [timeoutMs]
 * @param {boolean} [linuxClient]
 * @returns {Promise<object>}
 */
function probeServer(ip, port, token, version, timeoutMs = 6000, linuxClient = false) {
  return new Promise((resolve) => {
    const res = {
      reachable: false,
      versionOk: null,
      serverVersion: null,
      text: '',
      older: false,
    };
    const sock = net.connect({ host: ip, port });
    let done = false;
    const finish = () => {
      if (!done) {
        done = true;
        sock.destroy();
        resolve(res);
      }
    };
    sock.setTimeout(timeoutMs, finish);
    sock.on('error', finish);
    sock.on('connect', () => {
      res.reachable = true;
      if (!version || !/^[A-Za-z0-9._+-]{1,40}$/.test(version)) return finish();
      // a Linux client's report ends with 'L' after the 64-hex token (the server lets that token queue even if its Steam helper is not up yet)
      const payload = Buffer.concat([
        Buffer.from([version.length]),
        Buffer.from(version, 'ascii'),
        Buffer.from(token || '', 'ascii'),
        Buffer.from(linuxClient && token ? 'L' : '', 'ascii'),
      ]);
      const len = payload.length + 1;
      sock.write(Buffer.concat([Buffer.from([len & 0xff, len >> 8, 0xb0]), payload]));
      sock.setTimeout(4000, () => {
        res.older = true;
        finish();
      });
    });
    sock.on('data', (b) => {
      if (b.length < 4 || b[2] !== 0xb1) {
        res.older = true;
        return finish();
      }
      res.text = b.toString('utf8', 3);
      res.versionOk = !/ok=0/.test(res.text);
      const m = /server=(\S+)/.exec(res.text);
      res.serverVersion = m ? m[1] : null;
      finish();
    });
    sock.on('close', finish);
  });
}

module.exports = { parseServer, resolveServer, probeServer };
