'use strict';
// lib/update/hosts.js (launcher 0.1.46): small requests on the HTTPS address, big files and clips on the direct one, each falling back to
// the other when a filter blocks it (connection refused, redirect, HTML block page). Two local servers stand in for the two addresses.
const assert = require('assert');
const http = require('http');
const fs = require('fs');
const path = require('path');
const mkTmp = require('./tmp');
const hosts = require('../lib/update/hosts');
const net = require('../lib/update/net');
const attach = require('../lib/attach');
const transport = require('../lib/report/transport');

let passed = 0;
const T = async (name, fn) => {
  try {
    await fn();
    passed++;
    console.log('  ok  ' + name);
  } catch (e) {
    console.log('FAIL  ' + name + '\n' + ((e && e.stack) || e));
    process.exitCode = 1;
  }
};

function server(name, mode) {
  const hits = [];
  const s = http.createServer((req, res) => {
    hits.push(req.method + ' ' + req.url);
    req.resume();
    req.on('end', () => {
      if (mode.block) {
        if (mode.block === '303') {
          res.writeHead(303); // a filter's See Other, here without a Location
          return res.end();
        }
        if (mode.block === 'redirect') {
          res.writeHead(302, { Location: 'http://blocked.example/warning' });
          return res.end();
        }
        res.writeHead(200, { 'Content-Type': 'text/html' });
        return res.end('<html>blocked</html>');
      }
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ from: name, url: req.url }));
    });
  });
  return new Promise((r) => s.listen(0, () => r({ s, hits, port: s.address().port, mode })));
}

const body = async (res) => {
  let t = '';
  for await (const c of res) t += c;
  return JSON.parse(t);
};

(async () => {
  await T('unconfigured: every url left as it is', () => {
    hosts.configure();
    assert.strictEqual(hosts.route('http://play.twelve-weeks.com:7002/files/ab'), 'http://play.twelve-weeks.com:7002/files/ab');
    assert.strictEqual(hosts.fail('http://play.twelve-weeks.com:7002/files/ab'), null);
  });

  await T('routing: json to api (https), big paths to direct, other hosts untouched', () => {
    hosts.configure({ api: 'https://dl.twelve-weeks.com', direct: 'http://play.twelve-weeks.com:7002' });
    assert.strictEqual(hosts.route('http://dl.twelve-weeks.com/files/ab'), 'http://play.twelve-weeks.com:7002/files/ab');
    assert.strictEqual(hosts.route('http://play.twelve-weeks.com:7002/manifest.json?channel=stable'), 'https://dl.twelve-weeks.com/manifest.json?channel=stable');
    assert.strictEqual(hosts.route('https://dl.twelve-weeks.com/part1-files/cd?sid=1'), 'http://play.twelve-weeks.com:7002/part1-files/cd?sid=1');
    assert.strictEqual(hosts.route('https://dl.twelve-weeks.com/report-clip?code=R-AB12&key=k'), 'http://play.twelve-weeks.com:7002/report-clip?code=R-AB12&key=k');
    assert.strictEqual(hosts.route('https://dl.twelve-weeks.com/report'), 'https://dl.twelve-weeks.com/report');
    const gh = 'https://github.com/DeepQuantum/twelve-weeks-releases/releases/download/build-198/abc';
    assert.strictEqual(hosts.route(gh), gh);
    assert.strictEqual(hosts.route('http://play.twelve-weeks.com:7101/manifest.json'), 'http://play.twelve-weeks.com:7101/manifest.json'); // test server
    assert.ok(hosts.sameServer('http://dl.twelve-weeks.com/part1-files/x', 'https://dl.twelve-weeks.com'));
    assert.ok(!hosts.sameServer('https://evil.example/part1-files/x', 'https://dl.twelve-weeks.com'));
  });

  await T('a failed address is skipped for 10 minutes, then tried again', () => {
    hosts.configure({ api: 'https://dl.twelve-weeks.com', direct: 'http://play.twelve-weeks.com:7002' });
    const t = 1e12;
    assert.strictEqual(hosts.fail('http://play.twelve-weeks.com:7002/files/ab', t), 'https://dl.twelve-weeks.com/files/ab');
    assert.strictEqual(hosts.route('http://play.twelve-weeks.com:7002/files/ab', t + 1000), 'https://dl.twelve-weeks.com/files/ab');
    assert.strictEqual(hosts.route('http://play.twelve-weeks.com:7002/files/ab', t + hosts.DOWN_MS + 1), 'http://play.twelve-weeks.com:7002/files/ab');
    // both down: no retry url, the request keeps its own address
    hosts.fail('https://dl.twelve-weeks.com/manifest.json', t);
    assert.strictEqual(hosts.fail('http://play.twelve-weeks.com:7002/files/ab', t), null);
  });

  const api = await server('api', {});
  const direct = await server('direct', {});
  const cfg = () =>
    hosts.configure({ api: `http://127.0.0.1:${api.port}`, direct: `http://localhost:${direct.port}` });
  const A = () => `http://127.0.0.1:${api.port}`;

  await T('live: manifest from api, blob link written with the api address goes to direct', async () => {
    cfg();
    net.resetDown();
    assert.strictEqual((await net.getJson(`${A()}/manifest.json`, 3000)).from, 'api');
    const r = await net.request(`${A()}/files/abc`);
    assert.strictEqual((await body(r)).from, 'direct');
  });

  await T('live: direct redirects (filter block page) -> blob fetched from api, direct skipped afterwards', async () => {
    cfg();
    direct.mode.block = 'redirect';
    const r = await net.request(`${A()}/files/abc`);
    assert.strictEqual((await body(r)).from, 'api');
    const n = direct.hits.length;
    assert.strictEqual((await body(await net.request(`${A()}/files/def`))).from, 'api');
    assert.strictEqual(direct.hits.length, n, 'direct not asked again while marked down');
    direct.mode.block = null;
  });

  await T('live: api serves an HTML block page -> json from direct', async () => {
    cfg();
    net.resetDown();
    api.mode.block = 'html';
    assert.strictEqual((await net.getJson(`${A()}/status.json`, 3000)).from, 'direct');
    api.mode.block = null;
  });

  await T('live: clip upload to direct, report zip to api; api refusing connections -> report on direct', async () => {
    cfg();
    const dir = mkTmp('hosts-');
    const clip = path.join(dir, 'c.mp4');
    fs.writeFileSync(clip, Buffer.alloc(1000));
    const up = await attach.uploadClip(A(), 'R-AB12', 'k', clip, null);
    assert.strictEqual(up.json.from, 'direct');
    const rep = await transport.post(`${A()}/report`, Buffer.from('zip'), null);
    assert.strictEqual(rep.json.from, 'api');
    // api address gone (connection refused): the report goes to the direct address
    const deadPort = api.port;
    await new Promise((r) => api.s.close(r));
    hosts.configure({ api: `http://127.0.0.1:${deadPort}`, direct: `http://localhost:${direct.port}` });
    const rep2 = await transport.post(`http://127.0.0.1:${deadPort}/report`, Buffer.from('zip'), null);
    assert.strictEqual(rep2.json.from, 'direct');
    net.resetDown();
    assert.strictEqual((await net.getJson(`http://127.0.0.1:${deadPort}/manifest.json`, 3000)).from, 'direct');
  });

  await T('a kept-alive connection the server closed silently is retried on a new one', async () => {
    // raw server: the first request on a connection gets a keep-alive answer; a second one on the same connection is dropped without an
    // answer (the :7002 front closes after a proxied reply without saying so)
    const rawNet = require('net');
    let conns = 0;
    const socks = new Set();
    const raw = rawNet.createServer((sock) => {
      conns++;
      socks.add(sock);
      let n = 0;
      sock.on('data', () => {
        if (n++ > 0) return sock.destroy();
        sock.write(
          ['HTTP/1.1 200 OK', 'Content-Type: application/json', 'Content-Length: 11', 'Connection: keep-alive', '', '{"ok":true}'].join('\r\n'),
        );
      });
    });
    await new Promise((r) => raw.listen(0, r));
    hosts.configure();
    const u = `http://127.0.0.1:${raw.address().port}/status.json`;
    net.resetDown();
    assert.deepStrictEqual(await net.getJson(u, 3000, { force: true }), { ok: true });
    assert.deepStrictEqual(await net.getJson(u, 3000, { force: true }), { ok: true });
    assert.strictEqual(conns, 2, 'second request went out again on a new connection');
    for (const k of socks) k.destroy();
    raw.close();
  });

  direct.s.close();

  // 0.1.49: three addresses (api, tls, direct)
  const a3 = await server('api', {});
  const t3 = await server('tls', {});
  const d3 = await server('direct', {});
  const cfg3 = () =>
    hosts.configure({ api: `http://127.0.0.1:${a3.port}`, tls: `http://localhost:${t3.port}`, direct: `http://localhost:${d3.port}` });
  const A3 = () => `http://127.0.0.1:${a3.port}`;

  await T('three: whose tells tls and direct apart by port', async () => {
    cfg3();
    assert.strictEqual(hosts.whose(new URL(`http://localhost:${t3.port}/x`)), 'tls');
    assert.strictEqual(hosts.whose(new URL(`http://localhost:${d3.port}/x`)), 'direct');
    assert.strictEqual(hosts.whose(new URL(`${A3()}/x`)), 'api');
  });

  await T('three: small from api, big from tls', async () => {
    cfg3();
    net.resetDown();
    assert.strictEqual((await net.getJson(`${A3()}/manifest.json`, 3000)).from, 'api');
    assert.strictEqual((await body(await net.request(`${A3()}/files/abc`))).from, 'tls');
  });

  await T('three: api HTML block page -> json from tls, not direct', async () => {
    cfg3();
    net.resetDown();
    a3.mode.block = 'html';
    const n = d3.hits.length;
    assert.strictEqual((await net.getJson(`${A3()}/status.json`, 3000)).from, 'tls');
    assert.strictEqual(d3.hits.length, n);
    a3.mode.block = null;
  });

  await T('three: api blocked AND tls redirecting -> json still from direct', async () => {
    cfg3();
    net.resetDown();
    a3.mode.block = 'html';
    t3.mode.block = 'redirect';
    assert.strictEqual((await net.getJson(`${A3()}/manifest.json`, 3000)).from, 'direct');
    a3.mode.block = t3.mode.block = null;
  });

  await T('three: report with api + tls refusing connections -> direct', async () => {
    const deadA = a3.port;
    const deadT = t3.port;
    await new Promise((r) => a3.s.close(r));
    await new Promise((r) => t3.s.close(r));
    hosts.configure({ api: `http://127.0.0.1:${deadA}`, tls: `http://localhost:${deadT}`, direct: `http://localhost:${d3.port}` });
    net.resetDown();
    const rep = await transport.post(`http://127.0.0.1:${deadA}/report`, Buffer.from('zip'), null);
    assert.strictEqual(rep.json.from, 'direct');
    net.resetDown();
    assert.strictEqual((await body(await net.request(`http://127.0.0.1:${deadA}/files/abc`))).from, 'direct');
  });

  d3.s.close();

  // 0.1.50: four addresses (api, tls, direct, ip); ip is the last try for every request
  const a4 = await server('api', {});
  const t4 = await server('tls', {});
  const d4 = await server('direct', {});
  const i4 = await server('ip', {});
  const cfg4 = (ports = {}) =>
    hosts.configure({
      api: `http://127.0.0.1:${ports.a || a4.port}`,
      tls: `http://localhost:${ports.t || t4.port}`,
      direct: `http://localhost:${ports.d || d4.port}`,
      ip: `http://localhost:${i4.port}`,
    });
  const A4 = () => `http://127.0.0.1:${a4.port}`;

  await T('four: whose knows ip; ip never picked while the others are up', async () => {
    cfg4();
    net.resetDown();
    assert.strictEqual(hosts.whose(new URL(`http://localhost:${i4.port}/x`)), 'ip');
    assert.strictEqual(hosts.whose(new URL(`http://localhost:${d4.port}/x`)), 'direct');
    assert.strictEqual((await net.getJson(`${A4()}/manifest.json`, 3000)).from, 'api');
    assert.strictEqual((await body(await net.request(`${A4()}/files/abc`))).from, 'tls');
    assert.strictEqual(i4.hits.length, 0);
  });

  await T('four: ip links (manifest fetched by IP) are ours and route back to api / tls', async () => {
    cfg4();
    net.resetDown();
    assert.strictEqual((await net.getJson(`http://localhost:${i4.port}/manifest.json`, 3000)).from, 'api');
    assert.strictEqual((await body(await net.request(`http://localhost:${i4.port}/files/abc`))).from, 'tls');
    assert.ok(hosts.sameServer(`http://localhost:${i4.port}/x`, `${A4()}/y`));
  });

  await T('four: api HTML, tls redirect, direct HTML -> json from ip', async () => {
    cfg4();
    net.resetDown();
    a4.mode.block = 'html';
    t4.mode.block = 'redirect';
    d4.mode.block = 'html';
    assert.strictEqual((await net.getJson(`${A4()}/manifest.json`, 3000)).from, 'ip');
    a4.mode.block = t4.mode.block = d4.mode.block = null;
  });

  await T('four: 303 See Other (no Location needed) on api, tls, direct -> json, big file and report from ip; all four 303 = saved for later', async () => {
    for (const s of [a4, t4, d4]) s.mode.block = '303';
    cfg4();
    net.resetDown();
    assert.strictEqual((await net.getJson(`${A4()}/manifest.json`, 3000)).from, 'ip');
    cfg4();
    net.resetDown();
    assert.strictEqual((await body(await net.request(`${A4()}/files/abc`))).from, 'ip');
    cfg4();
    net.resetDown();
    assert.strictEqual((await transport.post(`${A4()}/report`, Buffer.from('zip'), null)).json.from, 'ip');
    i4.mode.block = '303';
    cfg4();
    net.resetDown();
    const e = await transport.post(`${A4()}/report`, Buffer.from('zip'), null).catch((x) => x);
    assert.ok(e instanceof Error && /redirected \(blocked\?\).*HTTP 303/.test(e.message), String(e && e.message));
    assert.ok(transport.retryable(e), 'a redirect on every address keeps the report for later');
    a4.mode.block = t4.mode.block = d4.mode.block = i4.mode.block = null;
  });

  await T('four: api, tls, direct refusing connections -> json, big file and report from ip', async () => {
    const dead = { a: a4.port, t: t4.port, d: d4.port };
    await new Promise((r) => a4.s.close(r));
    await new Promise((r) => t4.s.close(r));
    await new Promise((r) => d4.s.close(r));
    cfg4(dead);
    net.resetDown();
    assert.strictEqual((await net.getJson(`http://127.0.0.1:${dead.a}/manifest.json`, 3000)).from, 'ip');
    cfg4(dead);
    net.resetDown();
    assert.strictEqual((await body(await net.request(`http://127.0.0.1:${dead.a}/files/abc`))).from, 'ip');
    cfg4(dead);
    net.resetDown();
    const rep = await transport.post(`http://127.0.0.1:${dead.a}/report`, Buffer.from('zip'), null);
    assert.strictEqual(rep.json.from, 'ip');
  });

  await T('four: everything down -> error after four tries, no loop', async () => {
    const dead = i4.port;
    await new Promise((r) => i4.s.close(r));
    hosts.configure({ api: 'http://127.0.0.1:1', tls: 'http://localhost:2', direct: 'http://localhost:3', ip: `http://localhost:${dead}` });
    net.resetDown();
    let tries = 0;
    await assert.rejects(hosts.withFallback('http://127.0.0.1:1/manifest.json', async () => {
      tries++;
      throw Object.assign(new Error('connect ECONNREFUSED'), { code: 'ECONNREFUSED' });
    }));
    assert.strictEqual(tries, 4);
    const h = hosts.history().slice(-4);
    assert.deepStrictEqual(h.map((x) => x.k), ['api', 'tls', 'direct', 'ip']);
    assert.deepStrictEqual(h.map((x) => x.next), ['tls', 'direct', 'ip', null]);
    assert.ok(h.every((x) => x.path === '/manifest.json' && /ECONNREFUSED/.test(x.err)));
  });

  await T('history: never the query string, at most 40 entries', async () => {
    hosts.configure({ api: 'http://127.0.0.1:1', direct: 'http://localhost:3' });
    for (let i = 0; i < 45; i++) hosts.note('http://127.0.0.1:1/report-clip?code=R-AB12&key=secret', new Error('read ECONNRESET'), null);
    const h = hosts.history();
    assert.strictEqual(h.length, 40);
    assert.ok(h.every((x) => !/secret|key=/.test(JSON.stringify(x))));
    assert.strictEqual(h[39].path, '/report-clip');
  });

  await T('netprobe: JSON, block page, refused address and the game port are told apart; report carries network.txt', async () => {
    const { netProbe } = require('../lib/report/netprobe');
    const { buildModel } = require('../lib/report/build');
    const ok = await server('ok', {});
    const blk = await server('blk', { block: 'html' });
    const gone = await server('gone', {});
    await new Promise((r) => gone.s.close(r));
    try {
    hosts.configure({ api: `http://127.0.0.1:${ok.port}`, tls: `http://127.0.0.1:${blk.port}`, direct: `http://localhost:${gone.port}`, ip: `http://127.0.0.1:${ok.port}` });
    const txt = await netProbe({ gameServer: `127.0.0.1:${ok.port}` });
    assert.match(txt, /update api .*\n\s+dns 127\.0\.0\.1 \(literal\)\n\s+get \/status\.json: HTTP 200, application\/json, \d+ B - JSON ok/);
    assert.match(txt, /update tls .*\n.*\n\s+get \/status\.json: HTTP 200, text\/html.* - HTML \(block page\?\) title "" text "blocked"/);
    assert.match(txt, /update direct .*\n.*\n\s+get \/status\.json: FAILED \(ECONNREFUSED/);
    assert.match(txt, /game server 127\.0\.0\.1:\d+\n.*\n\s+TCP open/);
    assert.match(txt, /recent update-server request failures \(newest last, \d+\):/);
    const m = buildModel([], { text: 't' }, 'sys', [], [{ name: 'network.txt', text: txt + 'tok ' + 'ab'.repeat(32) }]);
    const e = m.entries.find((x) => x.name === 'files/network.txt');
    assert.ok(e && /JSON ok/.test(e.data.toString()) && !/(ab){32}/.test(e.data.toString()), 'network.txt present and scrubbed');
    hosts.configure();
    const unconf = await netProbe({ fileHost: `http://127.0.0.1:${ok.port}` });
    assert.match(unconf, /update file +http:\/\/127\.0\.0\.1:\d+\n.*\n.*JSON ok/);
    } finally {
      await new Promise((r) => ok.s.close(r));
      await new Promise((r) => blk.s.close(r));
    }
  });

  await T('game server name: a failed lookup of our own name falls back to the server IP, any other name still fails', async () => {
    const { resolveServer } = require('../lib/game/server');
    hosts.configure({ api: 'https://dl.example.invalid', direct: 'http://play.example.invalid:7002', tls: 'https://play.example.invalid', ip: 'http://203.0.113.7:7002' });
    try {
      const r = await resolveServer('play.example.invalid:7001');
      assert.strictEqual(r.ip, '203.0.113.7');
      assert.strictEqual(r.port, 7001);
      assert.strictEqual(r.viaKnownIp, true);
      assert.ok(r.lookupError);
      await assert.rejects(resolveServer('other.example.invalid:7001'), /The server name 'other\.example\.invalid' was not found/);
      assert.strictEqual(hosts.knownIp('dl.example.invalid'), null);
      hosts.configure();
      await assert.rejects(resolveServer('play.example.invalid:7001'), /was not found/);
      assert.strictEqual(hosts.knownIp('play.example.invalid'), null);
    } finally {
      hosts.configure();
    }
  });

  await T('game server name: our own name answered with a block page address falls back to the server IP', async () => {
    const { resolveServer } = require('../lib/game/server');
    hosts.configure({ api: 'https://dl.example.invalid', direct: 'http://play.example.invalid:7002', tls: 'https://play.example.invalid', ip: 'http://203.0.113.7:7002' });
    const up = (set) => async (ip) => set.includes(ip);
    try {
      const lookup = async () => ({ address: '198.51.100.9' });
      let r = await resolveServer('play.example.invalid:7001', { lookup, tcpOpen: up(['203.0.113.7']) });
      assert.strictEqual(r.ip, '203.0.113.7');
      assert.strictEqual(r.viaKnownIp, true);
      assert.ok(/198\.51\.100\.9/.test(r.lookupError));
      r = await resolveServer('play.example.invalid:7001', { lookup, tcpOpen: up(['198.51.100.9', '203.0.113.7']) });
      assert.strictEqual(r.ip, '198.51.100.9');           // the answered address takes the port: kept (the server may have moved)
      r = await resolveServer('play.example.invalid:7001', { lookup, tcpOpen: up([]) });
      assert.strictEqual(r.ip, '198.51.100.9');           // neither answers: no switch (the probe reports it)
      r = await resolveServer('other.example.invalid:7001', { lookup, tcpOpen: up(['203.0.113.7']) });
      assert.strictEqual(r.ip, '198.51.100.9');           // not our name: never switched
      r = await resolveServer('play.example.invalid:7001', { lookup: async () => ({ address: '203.0.113.7' }), tcpOpen: up([]) });
      assert.strictEqual(r.ip, '203.0.113.7');
      assert.ok(!r.viaKnownIp);
    } finally {
      hosts.configure();
    }
  });

  hosts.configure();
  console.log(`hosts-test: ${passed} passed`);
})();
