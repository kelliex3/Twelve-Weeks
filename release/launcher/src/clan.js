'use strict';
// CLAN tab: how the clan is doing (GET /clan.json, read-only; API.md). Modelled on the in-game clan panel (2013 Factions survivors
// panel: WEEK n: DAY d, N SURVIVORS, HEALTHY / HUNGRY / SICK dot circle, NEEDED PER DAY, active mission with its three tiers,
// "+n NEXT ONE-USE BOOSTER").
const Clan = (() => {
  const $ = (id) => document.getElementById(id);
  const esc = (s) => String(s).replace(/[&<>"]/g, (m) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[m]));
  // The in-game clan dial (tools/ext/t2r-ext.c clan_dots + tools/features/clan_widgets.py): a soft grey disc of 340 units, up to 60 small dots
  // (16 units) split healthy / hungry / sick in proportion (a red one is kept while anyone is sick), each born at a random place with a random
  // drift of 7-16 units/s, turning a little now and then (1 in 64 frames at 60 fps) and bouncing off a rim at 148 of the 170 radius.
  const COL = { 0: '#5DC0DC', 1: '#9C9A9A', 2: '#FD6D51' };
  const dials = [];
  let rafOn = false, lastT = 0;
  function loop(now) {
    requestAnimationFrame(loop);
    const dt = lastT && now - lastT < 200 ? (now - lastT) / 1000 : 0.016; lastT = now;
    if (document.hidden || document.body.classList.contains('game-on')) return;   // the game is running: no canvas redraws
    for (const d of dials) d.step(dt);
  }
  class Dial {
    constructor(canvas) { this.cv = canvas; this.ctx = canvas.getContext('2d'); this.dots = []; this.n = [0, 0, 0]; dials.push(this); if (!rafOn) { rafOn = true; requestAnimationFrame(loop); } }
    set(h, u, s) {
      const n = h + u + s, dn = Math.min(n, 60);
      let dh = n ? Math.floor((h * dn + n / 2) / n) : 0, du = n ? Math.floor((u * dn + n / 2) / n) : 0;
      if (dh + du > dn) du = dn - dh;
      if (s && dh + du >= dn && dn) { if (du) du--; else dh--; }
      const want = [];
      for (let k = 0; k < dn; k++) want.push(k < dh ? 0 : k < dh + du ? 1 : 2);
      while (this.dots.length > dn) this.dots.pop();
      want.forEach((st, k) => {
        if (!this.dots[k]) {
          const a = Math.random() * 6.2832, r = (148 / 170) * Math.sqrt(Math.random()), va = Math.random() * 6.2832, sp = (7 + Math.random() * 9) / 170;
          this.dots[k] = { x: r * Math.cos(a), y: r * Math.sin(a), vx: sp * Math.cos(va), vy: sp * Math.sin(va), st };
        } else this.dots[k].st = st;
      });
    }
    step(dt) {
      const cv = this.cv; if (!cv.offsetParent) return;
      const w = cv.width, h = cv.height, R = w / 2, ctx = this.ctx, rim = 148 / 170;
      for (const d of this.dots) {
        if (Math.random() < 1 - Math.exp(-0.9375 * dt)) { const t = (Math.random() - 0.5) * 1.2, cs = Math.cos(t), sn = Math.sin(t), x = d.vx * cs - d.vy * sn; d.vy = d.vx * sn + d.vy * cs; d.vx = x; }
        d.x += d.vx * dt; d.y += d.vy * dt;
        const d2 = d.x * d.x + d.y * d.y;
        if (d2 > rim * rim) { const dd = Math.sqrt(d2), nx = d.x / dd, ny = d.y / dd, dot = d.vx * nx + d.vy * ny; if (dot > 0) { d.vx -= 2 * dot * nx; d.vy -= 2 * dot * ny; } d.x = nx * rim; d.y = ny * rim; }
      }
      ctx.clearRect(0, 0, w, h);
      const g = ctx.createRadialGradient(R, R, R * 0.2, R, R, R); g.addColorStop(0, 'rgba(150,150,150,.30)'); g.addColorStop(1, 'rgba(150,150,150,.22)');
      ctx.fillStyle = g; ctx.beginPath(); ctx.arc(R, R, R, 0, 6.2832); ctx.fill();
      const dr = (8 / 170) * R * (w < 200 ? 1.35 : 1);
      for (const d of this.dots) { ctx.fillStyle = COL[d.st]; ctx.beginPath(); ctx.arc(R + d.x * R, R + d.y * R, dr, 0, 6.2832); ctx.fill(); }
    }
  }
  const MOOD = { hopeful: 'good', cheerful: 'good', calm: '', neutral: '', worried: 'warn', afraid: 'warn', desperate: 'bad', grieving: 'bad', grim: 'bad', despair: 'bad' };
  // ---- chronicle view (update server /clanfeed.json "chron": roster with portraits, match summaries with loot). Optional: absent = the old journal only.
  // Portrait lookup: id "W/f/2/sick" = faction / sex / model / expression -> ../assets/portraits/<model>_<expr>.png; a missing picture shows the
  // placeholder tile (initials on the faction colour), so the screen works before the images land.
  const portraitUrl = (id) => { const a = String(id || '').split('/'); return a.length === 4 ? `../assets/portraits/${a[2]}_${a[3]}.png` : ''; };   // the member's head by pool model id + expression (tools/models/portrait_cast/ship_portraits.py; the daemon fitted the expression to what the head has)
  const ini = (n) => String(n || '?').trim().slice(0, 1).toUpperCase();
  const portrait = (id, name, cls) => `<span class="pt ${cls || ''} f-${esc(String(id || '').split('/')[0])}"><b>${esc(ini(name))}</b><img src="${portraitUrl(id)}" alt="" onerror="this.remove()"></span>`;
  const STYLE = { match_strong: 'good', match_even: '', match_bad: 'bad', match_downs: 'warn', haul_good: 'good', haul_short: 'warn', ration: 'warn', raid_prep: 'info', mourn: 'bad', lastwords: 'bad',
    sick_watch: 'warn', hunger: 'warn', newcomer: 'info', kids: 'good', quarrel: 'warn', quiet: '', use_food: 'good', use_med: 'warn', use_ammo: 'info', use_tool: '' };
  const CAP = { match_strong: 'MATCH', match_even: 'MATCH', match_bad: 'MATCH', match_downs: 'MATCH', haul_good: 'SUPPLIES', haul_short: 'SUPPLIES', ration: 'RATIONS', raid_prep: 'NEXT RAID', mourn: 'LOST',
    lastwords: 'LAST WORDS', sick_watch: 'SICK BAY', hunger: 'HUNGER', newcomer: 'NEWCOMER', kids: 'CAMP', quarrel: 'CAMP', quiet: 'CAMP', use_food: 'STORES', use_med: 'STORES', use_ammo: 'STORES', use_tool: 'STORES' };
  const STATE_CLS = { healthy: 'h', hungry: 'u', sick: 's', depressed: 'd', shock: 'k' };
  // story arcs: rows with m.arc (style arc_<kind>) are steps of a multi-day story. Shown ONLY here in the clan screen (the update server keeps them out of the
  // toasts / ticker); tapping one opens the whole arc so far, inline, as a thread.
  const ARC_TAG = { missing: 'RAID', sick: 'SICK BAY', hung: 'HUNGER', new: 'NEWCOMER', keep: 'KEEPSAKE', birth: 'BIRTH', wish: 'LAST WISH', blame: 'BLAME', betray: 'BETRAYAL', spore: 'SPORES', stranger: 'STRANGER', caravan: 'CARAVAN', pendant: 'FIREFLY', birthday: 'BIRTHDAY' };
  const ARC_RE = /^(?:DAY|DZIEŃ) (\d+): ([^.]*)\.\s*([\s\S]*)$/;
  const arcParts = (m) => { const x = ARC_RE.exec(String(m.text || '')); return x ? { head: x[2], body: x[3] } : { head: '', body: String(m.text || '') }; };
  function arcThreads(rows) {
    const th = {};
    rows.filter((m) => m.arc).forEach((m) => { (th[m.arc] = th[m.arc] || []).push(m); });
    for (const k of Object.keys(th)) th[k].sort((a, b) => (Number(a.step) || 0) - (Number(b.step) || 0) || (Number(a.id) || 0) - (Number(b.id) || 0));
    return th;
  }
  let jTop = -1;   // the newest journal day of this render: on a first visit only those lines are NEW
  const jNewest = (m) => (Number(m.week) || 0) * 7 + (Number(m.day) || 0) >= jTop;
  // ---- icons: the stock TLOU2 HUD icons, shapes tinted by a CSS mask (src/clan-icons.js, generated by tools/rip_clan_ui_icons.py) ----
  const ic = (name, size, cls) => (!/^(skill|g)-/.test(name) && typeof CLAN_ICON_IDS !== 'undefined' && CLAN_ICON_IDS.indexOf(name) < 0 ? (name = 'keepsake') : 0, /^skill-/.test(name) ? `<img class="ic skimg ${cls || ''}" src="../assets/icons/${name}.png" alt="" style="--s:${size || 22}px">` : `<i class="ic i-${name} ${cls || ''}" style="--s:${size || 22}px"></i>`);   // skill icons are round badges (own colours): plain images; the rest tint by mask
  const STYLE_ICON = { match_strong: 'skill-damage-marker', match_even: 'skill-damage-marker', match_bad: 'skill-damage-marker', match_downs: 'skill-reviver', haul_good: 'materials', haul_short: 'materials',
    ration: 'food', raid_prep: 'skill-strategist', mourn: 'skill-second-chance', lastwords: 'skill-second-chance', sick_watch: 'pills', hunger: 'g-bowl', newcomer: 'person', kids: 'skill-lucky',
    quarrel: 'skill-brawler', quiet: 'skill-ears', use_food: 'food', use_med: 'medicine', use_ammo: 'pistolammo', use_tool: 'wrench' };
  const ARC_ICON = { missing: 'skill-hawk', sick: 'pills', hung: 'g-bowl', new: 'person', keep: 'keepsake', birth: 'skill-lucky', wish: 'skill-second-chance', blame: 'skill-brawler', betray: 'skill-lethal', spore: 'gasmask', stranger: 'key', caravan: 'materials', pendant: 'keepsake', birthday: 'skill-lucky' };
  const ROLE_ICON = { scavenger: 'skill-scavenger', medic: 'skill-firstaid', cook: 'skill-collector', guard: 'skill-awareness', mechanic: 'skill-crafter', scout: 'skill-hawk', gardener: 'skill-lucky',
    kid: 'person', elder: 'skill-fortitude', radio: 'skill-ears' };
  const STATE_ICON = { hungry: 'g-bowl', sick: 'pills', depressed: 'g-cloud', shock: 'g-shock' };
  const BOND_ICON = { sweet: 'skill-lucky', mentor: 'skill-strategist', kin: 'person', rival: 'skill-brawler' };
  const MAP_BG = { 'Hospital': 'seattle-flooded', 'Hillside': 'haven-fields', 'School': 'stadium-concourse', 'Overpass': 'seattle-city', 'Bookstore': 'seattle-rain', 'Train Station': 'wlf-docks' };

  // ---- SUPPLIES model (the clan numbers stay clan.py's; r.supplies / chron match .cat carry the daemon's real split per category, tools/clan_supply.py - this model is the fallback for old payloads). A raid's real result is ONE pair: supplies brought (m.sup) vs needed (m.need). The
  // screen splits both over the camp's needs the same way every time (seeded by the raid): need = food 50 % / water 30 % / medicine 20 %, brought spread over
  // those in proportion with a small wobble (the sum is exactly m.sup). Ammo and materials are what the sack held on top (the chronicle's loot). The clan's
  // STOCK is that loot on hand + a rain barrel of water; DAYS = stock / the day's need. (re/clan-ui-motion.md: to be replaced by real numbers from the daemon.)
  const CATS = [{ k: 'food', ic: 'food', n: 'FOOD', need: 1 }, { k: 'water', ic: 'water', n: 'WATER', need: 1 }, { k: 'med', ic: 'medicine', n: 'MEDICINE', need: 1 },
    { k: 'ammo', ic: 'pistolammo', n: 'AMMO' }, { k: 'mat', ic: 'materials', n: 'MATERIALS' }];
  // loot key -> [category, icon, label, units one counts for in the stock, cue]
  const LOOT = { pine: ['food', 'food', 'PINEAPPLE', 1, 'loot'], beans: ['food', 'food', 'BEANS', 1, 'loot'], bandage: ['med', 'medicine', 'BANDAGES', 1, 'lootmed'], abx: ['med', 'pills', 'ANTIBIOTICS', 1, 'lootmed'],
    alcohol: ['med', 'alcohol', 'ALCOHOL', 1, 'lootmed'], r38: ['ammo', 'pistolammo', '.38 ROUNDS', 1, 'lootammo'], box9: ['ammo', 'pistolammo', '9MM BOXES', 12, 'lootammo'],
    filter: ['water', 'water', 'WATER FILTERS', 3, 'loot'], battery: ['mat', 'wrench', 'BATTERIES', 1, 'lootbig'], scissors: ['mat', 'scissors', 'SCISSORS', 1, 'lootbig'], rags: ['mat', 'rag', 'RAGS', 1, 'lootbig'] };
  const KEEPS = { chess: ['CHESSBOARD', 5], guitar: ['GUITAR STRING', 4], comic: ['COMIC BOOK', 4], teddy: ['TEDDY BEAR', 6], seeds: ['SEEDS', 4], cards: ['DECK OF CARDS', 4], harmonica: ['HARMONICA', 5] };
  const hash = (a) => { let h = 2166136261; for (const c of String(a)) { h ^= c.charCodeAt(0); h = Math.imul(h, 16777619); } return (h >>> 0) / 4294967296; };
  function split(total, w) {   // largest-remainder split of an integer by weights
    const sum = w.reduce((a, b) => a + b, 0) || 1, raw = w.map((x) => total * x / sum), out = raw.map(Math.floor);
    let rest = total - out.reduce((a, b) => a + b, 0);
    raw.map((x, i) => [x - Math.floor(x), i]).sort((a, b) => b[0] - a[0]).forEach(([, i]) => { if (rest > 0) { out[i]++; rest--; } });
    return out;
  }
  function raidMix(m) {
    const rc = m.cat;     // the daemon's real split of the day (tools/clan_supply.py); old results fall back to the model below
    if (rc && rc.got && rc.need) {
      const g = (o, k) => (o && o[k]) | 0, got = {}, need = {};
      CATS.forEach((c) => { got[c.k] = g(rc.got, c.k); need[c.k] = g(rc.need, c.k); });
      return { need: { food: need.food, water: need.water, med: need.med, ammo: need.ammo, mat: need.mat }, got, short: m.sup < m.need, delta: m.sup - m.need, real: true };
    }
    const need = Math.max(1, m.need | 0), sup = Math.max(0, m.sup | 0), nm = Math.max(need >= 3 ? 1 : 0, Math.round(need * 0.2)), nw = Math.round(need * 0.3), nf = Math.max(0, need - nm - nw);
    const needs = { food: nf, water: nw, med: nm }, key = (m.d || 0) + (m.map || '');
    const b = split(sup, [nf * (0.7 + 0.6 * hash(key + 'f')), nw * (0.7 + 0.6 * hash(key + 'w')), nm * (0.7 + 0.6 * hash(key + 'm'))]);
    const got = { food: b[0], water: b[1], med: b[2], ammo: 0, mat: 0 };
    (m.loot || []).forEach(([k, n]) => { const e = LOOT[k]; if (e && e[0] === 'ammo') got.ammo += n; else if (e && e[0] === 'mat') got.mat += n; });
    return { need: needs, got, short: sup < need, delta: sup - need };
  }
  function stockOf(matches) {   // the camp's goods on hand (the loot of the last 7 raids)
    const st = { food: 0, water: 6, med: 0, ammo: 0, mat: 0 };
    (matches || []).slice(0, 7).forEach((m) => (m.loot || []).forEach(([k, n]) => { const e = LOOT[k]; if (e) st[e[0]] += n * e[3]; }));
    return st;
  }
  const daysLeft = (st, need) => ({ food: Math.floor(st.food / Math.max(1, need.food)), water: Math.floor(st.water / Math.max(1, need.water)), med: Math.floor(st.med / Math.max(1, need.med)) });
  const NEWTAG = () => `<em class="newtag">${T('NEW')}</em>`;
  const newAttr = (id, fresh) => (fresh ? ` data-new="${esc(id)}"` : '');

  // the clan's SUPPLIES panel under the header: the camp's stock per need with the days it lasts, the last raid's brought-vs-needed, the week's trend, who is short
  function supplyPanel(r, ch) {
    const ms = (ch && ch.matches) || [], people = (ch && ch.roster) || [];
    const lm = ms[0] || (r.lastMatch ? { d: r.lastMatch.day + 1, map: '', sup: r.lastMatch.supplies, need: r.lastMatch.needed, parts: r.lastMatch.parts, loot: [] } : null);
    if (!lm) return '';
    let mix = raidMix(lm), st = stockOf(ms), dl = daysLeft(st, mix.need), real = null;
    const sp = r.supplies;     // the daemon's real numbers (clan payload "supplies"); an old payload has none: the model above stays
    if (sp && sp.need && sp.stock) {
      const g = (o, k) => (o && o[k]) | 0;
      real = sp; st = {}; dl = {};
      CATS.forEach((c) => { st[c.k] = g(sp.stock, c.k); dl[c.k] = g(sp.days, c.k); });
      mix = { need: { food: g(sp.need, 'food'), water: g(sp.need, 'water'), med: g(sp.need, 'med') }, got: Object.assign({}, mix.got), short: mix.short, delta: mix.delta, real: true };
      CATS.forEach((c) => { mix.got[c.k] = g(sp.got, c.k); });
    }
    const worst = Math.min(dl.food, dl.water, dl.med);
    const tile = (c) => {
      const hasNeed = !!c.need, d = hasNeed ? dl[c.k] : 0, low = hasNeed && d <= 1, nn = mix.need[c.k], stock = st[c.k];
      return `<div class="stile${low ? ' low' : ''}${stock ? '' : ' none'}" data-fx="up" data-fxs="slide" title="${hasNeed ? esc(T('{n} / DAY', { n: nn })) : ''}">${ic(c.ic, 40, 'big')}<span class="sk">${T(c.n)}</span>
        ${stock ? `<b class="cu" data-to="${stock}" data-from="0" data-quiet="1">${stock}</b>` : `<b class="zero">${T('NONE')}</b>`}
        <span class="ss">${!stock && !hasNeed ? '&nbsp;' : hasNeed ? `<u class="${low ? 'bad' : ''}">${T(d === 1 ? '1 DAY' : '{n} DAYS', { n: d })}</u>` : T('IN CAMP')}</span>
        </div>`;
    };
    const hung = people.filter((p) => p.state === 'hungry').map((p) => p.name), sick = people.filter((p) => p.state === 'sick').map((p) => p.name);
    const last7 = ms.slice(0, 7).reverse(), top = Math.max(1, ...last7.map((m) => Math.max(m.sup, m.need)));
    const trend = last7.length > 1 ? `<details class="trend supmore"><summary>${T('HOW THIS WEEK WENT')}</summary><div class="tbars" data-fxg="200,70">${last7.map((m) => `<span class="tb ${m.sup >= m.need ? 'ok' : 'sh'}" title="${esc(m.map)}"><i data-fx="grow-y" style="--h:${(m.sup / top * 100).toFixed(0)}%"></i><u style="bottom:${(m.need / top * 100).toFixed(0)}%"></u><small>${m.d}</small></span>`).join('')}</div></details>` : '<div></div>';
    const names = (a) => esc(a.slice(0, 6).join(', ')) + (a.length > 6 ? ' +' + (a.length - 6) : '');
    const dep = people.filter((p) => p.state === 'depressed').map((p) => p.name), shk = people.filter((p) => p.state === 'shock').map((p) => p.name);
    const lines = (dep.length ? `<p class="who d">${ic('g-cloud', 18)}<b>${T('DEPRESSED')}</b><span>${names(dep)}</span></p>` : '') + (shk.length ? `<p class="who k">${ic('g-shock', 18)}<b>${T('IN SHOCK')}</b><span>${names(shk)}</span></p>` : '') + (hung.length ? `<p class="who u">${ic('g-bowl', 18)}<b>${T('HUNGRY')}</b><span>${names(hung)}</span></p>` : '') + (sick.length ? `<p class="who s">${ic('pills', 18)}<b>${T('SICK')}</b><span>${names(sick)}</span></p>` : '');
    return `<section class="sup" data-fxg="120,70"><div class="suph" data-fx="left" data-fxs="slide"><h3>${T('SUPPLIES')}</h3>
        <div class="stock-days${worst <= 1 ? ' low' : ''}"><span>${T('THE STOCK LASTS')}</span><b class="cu" data-to="${worst}" data-from="0" data-quiet="1">${worst}</b><span>${T(worst === 1 ? 'DAY' : 'DAYS')}</span></div></div>
      <div class="stiles">${CATS.map(tile).join('')}</div>
      <div class="supf">${trend}<div class="whos2" data-fx="right">${lines || `<p class="who ok">${ic('skill-fortitude', 18)}<b>${T('NOBODY GOES HUNGRY OR SICK')}</b></p>`}</div></div></section>`;
  }

  // status updates (src/clan-status.js, tools/clan_status.py): only the ones not looked at yet; a first visit ever shows them all (they are at most a handful)
  function statusPanel(feed) {
    const ch = feed && feed.ok && feed.hasClan && feed.chron ? feed.chron : null;
    if (!ch || !ch.status || typeof ClanStatus === 'undefined') return '';
    return ClanStatus.section(ch.status, feed.feed || [], (s) => Fx.fresh('s:' + s.id, true));
  }
  // the launcher shows a capped roster (clans grow over a week): members with a changed status first, then the ones who need care, then the rest; the full camp is in game
  const ROSTER_CAP = 8, JOURNAL_CAP = 6;
  const STATE_ORDER = { dead: 0, sick: 1, hungry: 2, shock: 3, depressed: 4 };
  function roster(ch) {
    // one source of truth: a member with a status update shows that (latest) status here too: its word / icon and its expression
    const stm = {}; ((ch.status) || []).forEach((s) => { stm[s.name] = s; });
    const all = (ch.roster || []).map((p) => { const s = stm[p.name]; if (!s) return p; const im = s.img || {}; return { ...p, state: s.state, portrait: im[s.expr || s.state] || im[s.state] || p.portrait }; }), changed = new Set(Object.keys(stm));
    const rank = (p) => (changed.has(p.name) ? 0 : p.state in STATE_ORDER ? 1 + STATE_ORDER[p.state] / 10 : 2);
    const people = all.map((p, i) => ({ p, i })).sort((x, y) => rank(x.p) - rank(y.p) || x.i - y.i).slice(0, ROSTER_CAP).map((x) => x.p), more = all.length - people.length;
    const list = people.map((p, i) => {
      const fr = Fx.fresh('p:' + p.name, false), CS = typeof ClanStatus !== 'undefined' ? ClanStatus : null, need = STATE_ICON[p.state] || (CS && p.state !== 'healthy' && CS.GLYPH[p.state]), word = CS && CS.WORD[p.state] || String(p.state || '').toUpperCase();
      return `<button class="mem ${STATE_CLS[p.state] || ''}${CS && need && CS.TXT[p.state] ? ' is-st st-' + p.state : ''}${fr ? ' is-new' : ''}"${CS && CS.TXT[p.state] ? ` style="--stc:${CS.TXT[p.state]};--sc:${CS.COLOR[p.state]}"` : ''} data-mem="${i}" data-fx="left" data-fxs="slide"${newAttr('p:' + p.name, fr)}>${portrait(p.portrait, p.name)}<span class="mn"><b>${esc(p.name)}</b>${need ? `<em class="tg">${ic(need, 16, 'cond ' + p.state)}<span>${T(word)}</span></em>` : `<small>${esc(p.roleText || p.role)}</small>`}</span>${fr ? NEWTAG() : ''}</button>`;
    }).join('');
    const tile = more > 0 ? `<div class="mem more" data-fx="left" data-fxs="slide"><b>+ ${T('{n} MORE', { n: more })}</b><small>${T('SEE THE FULL CAMP IN GAME')}</small></div>` : '';
    const det = people.map((p, i) => `<div class="memd" data-memd="${i}" hidden>${portrait(p.portrait, p.name, 'big')}<div><div class="mname">${ic(ROLE_ICON[p.role] || 'person', 26, 'role')} ${esc(p.name)}</div><div class="msub">${esc(p.tagline || '')}</div>
      ${p.bond ? `<div class="msub bond">${ic(BOND_ICON[p.bondType] || 'person', 18)}${T('CLOSE TO')}: <b>${esc(p.bond)}</b>${p.bondLabel ? ' &middot; ' + esc(p.bondLabel) : ''}</div>` : ''}${STATE_ICON[p.state] ? `<div class="msub ${p.state}">${ic(STATE_ICON[p.state], 18)}${T(p.state === 'hungry' ? 'GOES HUNGRY' : p.state === 'sick' ? 'IS SICK' : p.state === 'depressed' ? 'IS DEPRESSED' : 'IS IN SHOCK')}</div>` : ''}</div></div>`).join('');
    return list ? `<div class="roster"><h3>${T('SURVIVORS')}</h3><div class="memlist" data-fxg="700,45">${list}${tile}</div><div class="memdet">${det}</div></div>` : '';
  }
  // one loot card: the real icon, the quantity, the name; keepsakes carry a star and the morale they give
  function lootCard(k, n, day, fr) {
    const e = LOOT[k], kp = KEEPS[k];
    const id = 'l:' + day + ':' + k;
    if (!e && !kp) return '';
    const name = e ? e[2] : kp[0], sub = e ? T(({ food: 'FOOD', med: 'MEDICINE', water: 'WATER', ammo: 'AMMO', mat: 'MATERIALS' })[e[0]]) : `${T('KEEPSAKE')} &middot; ${T('MORALE +{n}', { n: kp[1] })}`;
    return `<div class="lootc${kp ? ' keep' : ''}${fr ? ' is-new' : ''}" data-fx="pop" data-fxs="${e ? e[4] : 'gift'}"${newAttr(id, fr)}>${ic(e ? e[1] : 'keepsake', 44, 'big')}<span class="lq">${e ? '<b>x' + n + '</b>' : '<b>&#9733;</b>'}</span><span class="ln">${T(name)}</span><small>${sub}</small>${fr ? NEWTAG() : ''}</div>`;
  }
  // the rich loot model (tools/clan_loot.py): one card per finding with its own icon, the quantity and a subtle tier edge (scraps .. luxurious); the story lines under it
  const TIER_COL = ['#7d6f5f', '#8f8f8a', '#b9b5a8', '#8fb08a', '#d9b45a'], TIER_NAME = ['SCRAPS', 'POOR', 'NORMAL', 'GOOD', 'LUXURIOUS'];
  const CAT_NAME = { food: 'FOOD', water: 'WATER', med: 'MEDICINE', ammo: 'AMMO', mat: 'MATERIALS' };
  function findCard(f, day, fr, i) {
    const id = 'l:' + day + ':' + f.k + (i ? ':' + i : ''), t = Math.max(0, Math.min(4, f.tier | 0));
    return `<div class="lootc find${f.keep ? ' keep' : ''}${fr ? ' is-new' : ''}" data-tier="${t}" style="--tc:${TIER_COL[t]}" data-fx="pop" data-fxs="${f.keep ? 'gift' : f.cat === 'ammo' ? 'lootammo' : f.cat === 'med' ? 'lootmed' : 'loot'}" title="${esc(f.text || '')}"${newAttr(id, fr)}>${ic(f.icon_want || f.icon, 44, 'big')}<span class="lq">${f.n > 1 ? '<b>x' + f.n + '</b>' : ''}</span><span class="ln">${esc(f.item || f.k)}</span><small>${f.keep ? T('KEEPSAKE') : T(CAT_NAME[f.cat] || String(f.cat || '').toUpperCase())}</small>${fr ? NEWTAG() : ''}</div>`;
  }
  const findLines = (finds) => (finds || []).filter((f) => f.text).map((f) => { const t = Math.max(0, Math.min(4, f.tier | 0)); return `<p class="fline" data-tier="${t}" style="--tc:${TIER_COL[t]}">${ic(f.icon_want || f.icon, 22)}<span>${esc(f.text)}</span></p>`; }).join('');
  // the SUPPLIES panel of one raid: brought vs needed per category with real icons, a stamp for the verdict
  function raidSupplies(m) {
    const mix = raidMix(m), over = Math.max(1, ...CATS.map((c) => Math.max(mix.got[c.k], mix.need[c.k] || 0)));
    const row = (c) => {
      const n = mix.need[c.k], g = mix.got[c.k], short = c.need && g < n, w = (g / over * 100).toFixed(0), nl = n != null ? (n / over * 100).toFixed(0) : null;
      return `<div class="srow${short ? ' sh' : ''}${!c.need && !g ? ' dim' : ''}" data-fx="left" data-fxs="slide">${ic(c.ic, 34)}<span class="sn">${T(c.n)}</span>
        <div class="sbar"><i data-fx="grow" data-fxs="bar" style="--w:${w}%"></i>${nl != null ? `<u style="left:${nl}%"></u>` : ''}</div>
        <b><span class="cu" data-to="${g}" data-from="0" data-quiet="1">${g}</span>${c.need ? ` / ${n}` : ''}</b>${c.need ? `<em class="${short ? 'bad' : 'ok'}">${g >= n ? '+' + (g - n) : '-' + (n - g)}</em>` : '<em></em>'}</div>`;
    };
    const verdict = mix.short ? `<span class="stamp bad" data-fx="stamp">${T('SHORT BY {n}', { n: -mix.delta })}</span>` : `<span class="stamp ok" data-fx="stamp">${T(mix.delta ? 'ENOUGH, +{n} OVER' : 'JUST ENOUGH', { n: mix.delta })}</span>`;
    return `<div class="rsup" data-fxg="0,60"><div class="rsh" data-fx="left"><label>${T('SUPPLIES BROUGHT HOME')}</label><div class="rtot"><b class="cu" data-to="${m.sup}" data-from="0">${m.sup}</b><span>/ ${m.need} ${T('NEEDED')}</span></div>${verdict}</div>${CATS.filter((c) => c.need || mix.got[c.k]).map(row).join('')}</div>`;
  }
  function matches(ch, lm0) {
    const ms = (ch.matches || []).slice(0, 12);
    const rows = ms.map((m, i) => {
      const mix = raidMix(m), fr = Fx.fresh('m:' + m.d + ':' + m.map, i === 0), bg = MAP_BG[m.map] || 'seattle-city';
      const chips = CATS.map((c) => `<span class="mchip${c.need && mix.got[c.k] < mix.need[c.k] ? ' sh' : ''}${!c.need && !mix.got[c.k] ? ' z' : ''}">${ic(c.ic, 18)}<b>${mix.got[c.k]}</b></span>`).join('');
      const loot = (m.loot || []).filter(([k]) => LOOT[k] || KEEPS[k]);
      const finds = (m.finds || []).filter((f) => f && f.k);
      const lootHtml = finds.length ? finds.map((f, j) => findCard(f, m.d, i === 0 ? Fx.fresh('l:' + m.d + ':' + f.k + (j ? ':' + j : ''), true) : Fx.fresh('l:' + m.d + ':' + f.k + (j ? ':' + j : ''), false), j)).join('') : loot.map(([k, n]) => lootCard(k, n, m.d, i === 0 ? Fx.fresh('l:' + m.d + ':' + k, true) : Fx.fresh('l:' + m.d + ':' + k, false))).join('');
      const open = i === 0;
      return `<button class="mat raid${mix.short ? ' sh' : ' ok'}${open ? ' sel' : ''}${fr ? ' is-new' : ''}" data-mat="${i}" data-fx="right" data-fxs="slide" style="--bg:url(../assets/bg/${bg}.jpg)"${newAttr('m:' + m.d + ':' + m.map, fr)}><i class="rbg"></i><small>${T('DAY {d}', { d: m.d })}</small><b>${esc(m.map)}</b>
          <span class="rbar"><i style="width:${Math.min(100, m.sup / Math.max(1, m.need) * 100).toFixed(0)}%"></i></span>${m.win == null ? '' : `<span class="wl ${m.win ? 'w' : 'l'}">${T(m.win ? 'MATCH WON' : 'MATCH LOST')}</span>`}<span class="rv ${mix.short ? 'bad' : 'ok'}">${m.sup} / ${m.need}</span>${fr ? NEWTAG() : ''}</button>
        <div class="matd${open ? ' hero' : ''}" data-matd="${i}"${open ? '' : ' hidden'}>${raidSupplies(m)}
          ${lootHtml ? `<div class="loot2"><label>${T('BROUGHT HOME')}</label><div class="lootg" data-fxg="260,80">${lootHtml}</div></div>` : ''}
          ${finds.some((f) => f.text) ? `<details class="more finds"${open ? ' open' : ''}><summary>${T('WHAT THEY FOUND')}</summary>${findLines(finds)}</details>` : ''}
          <details class="more"><summary>${T('MORE ABOUT THIS RAID')}</summary><div class="rstats" data-fxg="520,55"><div data-fx="up">${ic('killfeed-skull', 22)}<span>${T('Down on their side')}</span><b>${m.kills}</b></div>${m.execs == null ? '' : `<div data-fx="up">${ic('skill-executioner', 22)}<span>${T('Executions')}</span><b>${m.execs}</b></div>`}${m.revives == null ? '' : `<div data-fx="up">${ic('skill-reviver', 22)}<span>${T('Revives')}</span><b>${m.revives}</b></div>`}${m.deaths == null ? '' : `<div data-fx="up" class="${m.deaths ? 'bad' : ''}">${ic('skill-second-chance', 22)}<span>${T('Lost')}</span><b>${m.deaths}</b></div>`}${m.downs == null ? '' : `<div data-fx="up">${ic('skill-damage-marker', 22)}<span>${T('Times downed')}</span><b>${m.downs}</b></div>`}<div data-fx="up">${ic('parts', 22)}<span>${T('PARTS')}</span><b class="cu" data-to="${m.parts}" data-from="0" data-quiet="1">${m.parts}</b></div></div></details>
          ${i === 0 && lm0 ? `<div class="after">${lm0.newHungry ? `<span class="u">${ic('g-bowl', 18)}${T(lm0.newHungry === 1 ? '1 WENT HUNGRY' : '{n} WENT HUNGRY', { n: lm0.newHungry })}</span>` : ''}${lm0.newSick ? `<span class="s">${ic('pills', 18)}${T(lm0.newSick === 1 ? '1 FELL SICK' : '{n} FELL SICK', { n: lm0.newSick })}</span>` : ''}${lm0.healed ? `<span class="h">${ic('medicine', 18)}${T(lm0.healed === 1 ? '1 HEALED' : '{n} HEALED', { n: lm0.healed })}</span>` : ''}</div>` : ''}</div>`;
    }).join('');
    const inv = (ch.inv || []).concat(ch.keep || []);
    return rows ? `<div class="matches"><h3>${T('RAIDS')}</h3><p class="hint">${T('Click a raid to see what it brought home.')}</p><div class="raids" data-fxg="900,70">${rows}</div>${inv.length ? `<div class="loot"><label>${T('IN CAMP NOW')}</label><p>${esc(inv.join(', '))}</p></div>` : ''}</div>` : '';
  }
  let wired = false;
  function selectEntry(je) {
    const lay = je.closest('.jlay'); lay.querySelectorAll('.jrn.sel').forEach((x) => x.classList.remove('sel')); je.classList.add('sel');
    lay.querySelectorAll('.jdet.on').forEach((x) => x.classList.remove('on'));
    const d = lay.querySelector(`.jdet[data-for="${CSS.escape(je.dataset.key)}"]`); if (d) d.classList.add('on');
  }
  function wire() {
    if (wired || !$('clan-body').addEventListener) return; wired = true;
    $('clan-body').addEventListener('click', (e) => {
      const je = e.target.closest('.jlay .jrn'); if (je) selectEntry(je);
      const m = e.target.closest('[data-mem]'), a = e.target.closest('[data-mat]');
      if (m) { const root = m.closest('.roster'); const on = m.classList.contains('sel'); root.querySelectorAll('.mem').forEach((x) => x.classList.remove('sel')); root.querySelectorAll('.memd').forEach((x) => { x.hidden = true; });
        if (!on) { m.classList.add('sel'); const d = root.querySelector(`[data-memd="${m.dataset.mem}"]`); if (d) d.hidden = false; } }
      if (a) { const root = a.closest('.matches'); const d = root.querySelector(`[data-matd="${a.dataset.mat}"]`); const open = d && d.hidden; root.querySelectorAll('.matd').forEach((x) => { x.hidden = true; }); root.querySelectorAll('.mat').forEach((x) => x.classList.remove('sel'));
        if (open) { d.hidden = false; a.classList.add('sel'); Fx.open(d); } else Fx.cue('cl-collapse', 0.4); }
    });
    let lastHov = 0;
    $('clan-body').addEventListener('mouseover', (e) => {
      const el = e.target.closest('.mem,.mat,.lootc,.stile');
      if (!el || el.contains(e.relatedTarget)) return;
      const now = performance.now(); if (now - lastHov < 120) return; lastHov = now; Fx.cue('cl-hover', 0.3);
    });
  }
  // first visit ever: only the newest day / raid / loot are NEW (the rest is marked seen), so nobody is greeted by forty tags
  function seedSeen(rows, ch) {
    if (Fx.has()) return;
    const all = [], newest = new Set();
    rows.forEach((m) => { if (m.id == null) return; const id = 'j:' + m.id; all.push(id); if (jNewest(m)) newest.add(id); });
    ((ch && ch.matches) || []).forEach((m, i) => { const id = 'm:' + m.d + ':' + m.map; all.push(id); if (i === 0) newest.add(id); (m.loot || []).forEach(([k]) => { const l = 'l:' + m.d + ':' + k; all.push(l); if (i === 0) newest.add(l); }); });
    ((ch && ch.roster) || []).forEach((p) => all.push('p:' + p.name));
    Fx.seed(all.filter((i) => !newest.has(i)));
  }
  function camp(f, lm) {   // the CAMP journal: counts, morale and the clan's status lines (no list of individual members)
    const http = f && f.ok && f.hasClan && f.source !== 'daemon' ? f : null;
    const mo = (http && http.morale) || null;
    const rows = http && (http.feed || []).length ? http.feed.map((m, i) => ({ ...m, i })).sort((a, b) => (Number(b.id) || 0) - (Number(a.id) || 0) || (b.ts || 0) - (a.ts || 0) || a.i - b.i)   /* newest first: ids grow, week / day restart with a new clan */.map((m) => ({ ...m, when: `${T('WEEK {w}', { w: m.week })} &middot; ${T('DAY {d}', { d: m.day })}` }))
      : (typeof Feed !== 'undefined' ? [...Feed.history(), ...Feed.sample(Feed.history().length ? 0 : 8)] : []).map((m) => ({ ...m, when: T(m.kind === 'ticker' ? 'CAMP TALK' : 'CAMP') }));
    // morale as a readable scale (tools/clan_story.py bands: grim < 30, strained 30-49, steady 50-69, hopeful >= 70): GRIM at the left end, HOPEFUL at the right, ticks at 30 / 50 / 70, the state word as a marker over the fill end
    const mv = mo ? Math.max(0, Math.min(100, Number(mo.value) || 0)) : 0, mband = mv >= 70 ? 'hopeful' : mv >= 50 ? 'steady' : mv >= 30 ? 'strained' : 'grim';
    const bar = mo ? `<div class="mor m-${mband}"><label>${T('CAMP MORALE')}</label><span class="mlo"${mband === 'grim' ? ' style="visibility:hidden"' : ''}>${T('GRIM')}</span><div class="morbar" style="--v:${mv}%"><i style="width:${Math.max(2, mv)}%"></i><u style="left:30%"></u><u style="left:50%"></u><u style="left:70%"></u><b class="mk" style="left:${Math.min(88, Math.max(12, mv))}%">${T(mband.toUpperCase())}</b></div><span class="mhi"${mband === 'hopeful' ? ' style="visibility:hidden"' : ''}>${T('HOPEFUL')}</span></div>` : '';
    const al = f && f.ok && f.hasClan && f.alife && Array.isArray(f.alife.ongoing) ? f.alife.ongoing : [];   // optional (update server /clanfeed.json "alife"): what is happening in camp now
    const og = al.length ? `<div class="ongo"><label>${T('HAPPENING IN CAMP')}</label>${al.slice(0, 4).map((t) => `<p>${esc(t)}</p>`).join('')}</div>` : '';
    const th = arcThreads(rows);
    jTop = rows.reduce((a, m) => Math.max(a, (Number(m.week) || 0) * 7 + (Number(m.day) || 0)), 0);
    seedSeen(rows, f && f.ok && f.hasClan && f.chron ? f.chron : null);
    // JOURNAL = the in-game clan journal layout (clan_ui_mock.py): LEFT the entry cards (a story arc is ONE card, its newest step; camp-log rows carry the member's portrait), RIGHT the whole thread of the selected
    // entry (big title, who is in it, a vertical timeline). Capped here, the rest is in game.
    const chA = f && f.ok && f.hasClan && f.chron ? f.chron : null;
    const pinned = new Set(((chA && chA.status) || []).map((s) => (typeof ClanStatus !== 'undefined' ? ClanStatus.journalId(s, rows) : null)).filter((x) => x != null).map(String));   // a status card's "read in the journal" target stays listed
    const ppl = (chA && chA.roster) || [], who = (m) => { const t = String(m.text || ''), nm = (m.names && m.names.length ? m.names : ppl.map((p) => p.name).filter((n) => new RegExp('(^|[^\p{L}])' + n + '($|[^\p{L}])', 'u').test(t))); return nm.map((n) => ppl.find((p) => p.name === n)).filter(Boolean); };
    // The journal holds the STORIES only (the daemon marks every row card: true / false, tools/clan_story.py story_rows); the one-liners (camp log, feelings, weather ...)
    // are small dimmed timeline rows UNDER the story they follow, in time order. An old server without the flag shows every row as a card, as before.
    const stories = rows.filter((m) => m.card !== false), oneLiners = rows.filter((m) => m.card === false);
    const seen = new Set(), items = stories.filter((m) => !m.arc || (!seen.has(m.arc) && seen.add(m.arc)));
    const repOf = new Map(items.map((m) => [m.arc ? 'a:' + m.arc : 'j:' + m.id, m])), under = new Map();
    const asc = stories.slice().sort((a, b) => (Number(a.id) || 0) - (Number(b.id) || 0));
    oneLiners.slice().sort((a, b) => (Number(a.id) || 0) - (Number(b.id) || 0)).forEach((l) => {
      let host = asc[0]; asc.forEach((s) => { if ((Number(s.id) || 0) < (Number(l.id) || 0)) host = s; });
      const rep = host && repOf.get(host.arc ? 'a:' + host.arc : 'j:' + host.id); if (!rep) return;
      if (!under.has(rep)) under.set(rep, []); under.get(rep).push(l);
    });
    const lineLi = (l) => `<li data-jid="${esc(l.id)}"><time>${T('DAY {d}', { d: l.day })}</time>${esc(l.line || l.text)}</li>`;
    const tlines = (m) => { const ls = under.get(m) || []; return ls.length ? `<ul class="jlines">${ls.slice(0, 3).map(lineLi).join('')}${ls.length > 3 ? `<details class="jlm"><summary>+${ls.length - 3} ${T('MORE')}</summary>${ls.slice(3).map(lineLi).join('')}</details>` : ''}</ul>` : ''; };
    const shown = items.filter((m, i) => i < JOURNAL_CAP || pinned.has(String(m.id))), more = items.length - shown.length;
    const first = shown.find((m) => m.arc) || shown[0];   // the selected entry: the newest story thread (the mock selects one), else the newest row
    const stepTitle = (m) => arcParts(m).head, key = (m) => (m.arc ? 'a:' + m.arc : 'j:' + m.id);
    const entry = (m) => {
      const fr = m.id != null && Fx.fresh('j:' + m.id, jNewest(m)), kind = String(m.style || '').replace('arc_', ''), ap = arcParts(m), p1 = who(m)[0];
      const cap = m.arc ? `${T('STORY')} &middot; ${T(ARC_TAG[kind] || 'STORY')} &middot; ${T('DAY {d}', { d: m.day })} &middot; ${T('STEP {n}', { n: m.step || 1 })}` : `${T(m.style ? (CAP[m.style] || 'CAMP LOG') : 'CAMP LOG')} &middot; ${T('DAY {d}', { d: m.day })}`;
      const mood = m.arc ? MOOD[m.mood] || '' : m.style ? (STYLE[m.style] || '') : MOOD[m.mood] || '';
      return `<article data-jid="${esc(m.id)}" data-key="${esc(key(m))}" class="jrn${m.arc ? ' arc' : ''} ${mood}${fr ? ' is-new' : ''}${m === first ? ' sel' : ''}"${m.arc ? ` data-arc="${esc(m.arc)}"` : ''} data-fx="right" data-fxs="slide"${newAttr('j:' + m.id, fr)} tabindex="0">
        <div class="jh2">${ic(m.arc ? (ARC_ICON[kind] || 'journal') : (STYLE_ICON[m.style] || 'journal'), 22, 'jic')}<time>${cap}</time>${fr ? NEWTAG() : ''}<button class="jopen" data-snd="click">${T('OPEN THREAD')}</button></div>
        ${ap.head ? `<h4>${esc(ap.head)}</h4>` : ''}<p>${esc(ap.head ? ap.body : m.text)}</p><span class="jrm">${T('READ MORE')} +</span>${!m.arc && p1 ? portrait(p1.portrait, p1.name, 'jpt') : ''}${tlines(m)}</article>`;
    };
    const detail = (m, on) => {
      const kind = String(m.style || '').replace('arc_', ''), steps = m.arc ? th[m.arc] || [m] : [m], names = [], pp = [];
      steps.forEach((x) => who(x).forEach((p) => { if (!names.includes(p.name)) { names.push(p.name); pp.push(p); } }));
      const title = m.arc ? `${T(ARC_TAG[kind] || 'STORY')} - ${T(steps.length === 1 ? '1 STEP' : '{n} STEPS', { n: steps.length })}` : `${T(m.style ? (CAP[m.style] || 'CAMP LOG') : 'CAMP LOG')} - ${T('DAY {d}', { d: m.day })}`;
      const li = steps.map((x) => { const q = arcParts(x); return `<li class="${MOOD[x.mood] || ''}" data-jid="${esc(x.id)}"><time>${T('DAY {d}', { d: x.day })}</time><b>${esc(q.head)}</b><p>${esc(q.body)}</p></li>`; }).join('');
      return `<div class="jdet${on ? ' on' : ''}" data-for="${esc(key(m))}"><div class="thread" hidden><label>${T('THE WHOLE STORY SO FAR')}</label><span class="jtt">${title}</span>${pp.length ? `<span class="jwho">${pp.slice(0, 4).map((p) => `<span class="jw">${portrait(p.portrait, p.name, 'jpt')}<b>${esc(p.name)}</b></span>`).join('')}</span>` : ''}<ol>${li}</ol></div></div>`;
    };
    const dets = [...shown.filter((m) => m.arc), ...shown.filter((m) => !m.arc)].map((m, i) => detail(m, m === first));
    const gameLink = (n) => `<p class="jmore"><b>+ ${T('{n} MORE', { n })} ${T('ENTRIES')}</b><span>${T('READ ALL IN GAME')}</span></p>`;
    const list = shown.length ? `<section class="jsec"><h4 class="jh">${T('JOURNAL')}<small>${T('WHAT HAPPENED, DAY BY DAY')}</small></h4><div class="jlay"><div class="jlist">${shown.map(entry).join('')}${more > 0 ? gameLink(more) : ''}</div><div class="jpane">${dets.join('')}</div></div></section>`
      : `<div class="empty">${T('Nothing in the journal yet. Play a match: your clan will start talking.')}</div>`;
    const ch = f && f.ok && f.hasClan && f.chron ? f.chron : null;                 // optional: roster + match summaries (chronicle switch on)
    if (ch || rows.length) wire();
    return `<div class="camp"><h3>${T('CAMP')}</h3>${bar}${ch ? roster(ch) + matches(ch, lm) : ''}${og}<div class="jrnl" data-fxg="1150,45">${list}</div></div>`;
  }
  let homeDial = null, tabDial = null;
  const circle = () => '<canvas class="circ" id="clan-dial" width="360" height="360"></canvas>';
  let lastArgs = null;
  function render(r, err, feed, animate) {
    lastArgs = [r, err, feed];
    const el = $('clan-body');
    if (err || !r) { el.innerHTML = `<div class="empty">${T('Could not reach the clan service')}${err ? ': ' + esc(T(err)) : ''}.<br><span class="muted">${T('Your clan is stored on the game server; the host\'s update server must run with a database (API.md, /clan.json).')}</span></div>`; return; }
    if (!r.ok) { el.innerHTML = `<div class="empty">${esc(T(r.error || 'Clan service error'))}</div>`; return; }
    if (!r.hasClan) { el.innerHTML = `<div class="empty">${T('Your clan appears after your first match on this install.')}<br><span class="muted">${T('On the very first launch of the game you choose your side (WLF or Seraphites) once for your clan. If you played before, finish one match with this install and it shows up here.')}</span></div>`; return; }
    const fac = r.factionName || '';
    if (r.status !== 'alive') {
      el.innerHTML = `<div class="clan-top"><div><label>${esc(T(fac))}</label><div class="big">${T(r.status === 'won' ? 'YOUR CLAN SURVIVED' : 'YOUR CLAN IS GONE')}</div></div></div>
        <div class="facts"><div><label>${T('CLANS WON')}</label><b>${r.weeksTotal ? '' : ''}${r.runsWon}</b></div><div><label>${T('CLANS LOST')}</label><b>${r.runsLost}</b></div><div><label>${T('LARGEST CLAN')}</label><b>${r.maxSize}</b></div></div>
        <p class="lead">${T('Choose your side again in the game\'s main menu to start a new clan.')}</p>`;
      return;
    }
    const pct = Math.min(100, (r.daysDone / r.daysTotal) * 100);
    const m = r.mission, ev = r.event, lm = r.lastMatch;
    const ticks = Array.from({ length: r.weeks - 1 }, (_, i) => `<i style="left:${((i + 1) * 7 / 84 * 100).toFixed(2)}%"></i>`).join('');
    const mission = m ? `<div class="blk"><label>${T('ACTIVE MISSION')}${m.inEvent ? ' &middot; ' + T('EVENT TRAINING') : ' &middot; ' + T('PRACTICE')}</label><div class="mname">${esc(T(m.name))}</div>
        <div class="mprog"><b>${m.goals[2] > 0 ? Math.min(m.progress, m.goals[2]) : m.progress}</b> <span>/ ${m.goals[2]}</span></div><div class="tiers">${m.goals.map((g, i) => `<span class="${m.progress >= g ? 'done' : ''}"><small>${T('TIER {n}', { n: i + 1 })}</small>${g}</span>`).join('')}</div>
        <div class="msub">${T('Day {d}/3', { d: Math.max(1, 3 - m.daysLeft + 1) })} &middot; ${T(m.daysLeft === 1 ? '1 match left' : '{n} matches left', { n: m.daysLeft })}</div></div>`
      : `<div class="blk"><label>${T('ACTIVE MISSION')}</label><div class="mname dim">${T('None picked')}</div><div class="msub">${T('Pick one from CHOOSE MISSION in the game\'s main menu.')}</div></div>`;
    const event = ev ? `<div class="blk"><label>${T('EVENT')}</label><div class="mname">${ev.kind === 'attack' ? T('ATTACK ON YOUR CAMP') : esc(T(ev.name))}</div><div class="msub">${T('Match {a} of {b}.', { a: ev.dayOfEvent, b: ev.length })} ${T(ev.kind === 'attack' ? 'Casualties follow the mission tier you reach.' : 'New members follow the mission tier you reach.')}</div></div>`
      : r.eventToday ? `<div class="blk"><label>${T('NEXT MATCH')}</label><div class="mname">${r.eventToday.kind === 'attack' ? T('ATTACK ON YOUR CAMP') : esc(T(r.eventToday.name))}</div><div class="msub">${T('An event starts with your next match.')}</div></div>` : '';
    const last = lm ? `<div class="blk"><label>${T('LAST MATCH (DAY {d})', { d: lm.day + 1 })}</label>
        <div class="lrows"><div><span>${T('Parts earned')}</span><b>${lm.parts}</b></div><div><span>${T('Supplies')}</span><b>${lm.supplies} / ${lm.needed}</b></div><div><span>${T('Survivors gained')}</span><b>${lm.gained}</b></div>
        <div><span>${T('Died')}</span><b>${lm.died}</b></div><div><span>${T('New hungry / sick')}</span><b>${lm.newHungry} / ${lm.newSick}</b></div><div><span>${T('Healed')}</span><b>${lm.healed}</b></div></div>
        ${r.lastMission ? `<div class="msub">${esc(T(r.lastMission.name))}: ${r.lastMission.progress}${r.lastMission.tier ? ', ' + T('tier {n}', { n: r.lastMission.tier }) : ''}${r.lastMission.boostersEarned ? ', ' + T('+{n} booster', { n: r.lastMission.boostersEarned }) : ''}</div>` : ''}${lm.leftEarly ? `<div class="msub bad">${T('Match left early: the day passed with no supplies.')}</div>` : ''}</div>` : '';
    const boosters = r.boosters.filter((b) => b.count > 0 || [1, 2, 3, 4, 7].includes(b.id)).map((b) => `<div class="${b.count ? '' : 'dim'}"><span>${esc(T(b.name))}</span><b>x${b.count}</b></div>`).join('');
    el.innerHTML = `<div class="clan-top"><div><label>${esc(T(fac))}</label><div class="big">${T('WEEK {w}: DAY {d}', { w: r.week, d: r.dayOfWeek })}</div></div>
        <div class="journey"><div class="jbar"><i style="width:${pct.toFixed(1)}%"></i>${ticks}</div><small>${T('{a} of {b} days survived', { a: r.daysDone, b: r.daysTotal })} &middot; ${T('week {a} of {b}', { a: r.week, b: r.weeks })}</small></div></div>
      ${statusPanel(feed)}${supplyPanel(r, feed && feed.ok && feed.hasClan && feed.chron ? feed.chron : null)}
      <div class="clan-cols"><div class="clan-surv"><label>${T('{n} SURVIVORS', { n: r.size })}</label>${circle(r)}
        <div class="cnt"><span><i style="background:${COL[0]}"></i>${T('HEALTHY')} <b>${r.healthy}</b></span><span><i style="background:${COL[1]}"></i>${T('HUNGRY')} <b>${r.hungry}</b></span><span><i style="background:${COL[2]}"></i>${T('SICK')} <b>${r.sick}</b></span></div>
        <div class="need">${T('NEEDED PER DAY')} <b>${r.needed}</b></div><div class="msub">${T('+{n} for the next one-use booster', { n: r.boosterNext })}</div></div>
        <div class="clan-right" data-fxg="100,90">${mission}${event}${last}<div class="blk"><label>${T('ONE-USE BOOSTERS')}</label><div class="lrows">${boosters}</div></div>
        <div class="msub">${T('Clans won {a}', { a: r.runsWon })} &middot; ${T('lost {a}', { a: r.runsLost })} &middot; ${T('largest clan {a}', { a: r.maxSize })}</div></div></div>${camp(feed, lm)}`;
    for (let k = dials.length - 1; k >= 0; k--) if (dials[k] !== homeDial && !document.body.contains(dials[k].cv)) dials.splice(k, 1);
    tabDial = new Dial($('clan-dial')); tabDial.set(r.healthy, r.hungry, r.sick);
    L10N.tree($('clan-body'));
    try { tagFx(el); if (animate === false) Fx.still(el); else Fx.play(el); } catch (e) { f2.log('Clan motion failed: ' + (e && e.message || e)); if (el.classList) el.classList.add('fx-static'); }   // motion is a nicety: the screen shows either way
  }
  // the header blocks that the markup does not tag itself: slide / count-up / grow hooks (re/clan-ui-motion.md)
  function tagFx(el) {
    const q = (sel, fn) => el.querySelectorAll(sel).forEach(fn), fx = (e, k, snd) => { e.dataset.fx = k; if (snd) e.dataset.fxs = snd; };
    const top = el.querySelector('.clan-top'); if (top) { top.dataset.fxg = '0,80'; fx(top.firstElementChild, 'down', 'slide'); const j = top.querySelector('.journey'); if (j) fx(j, 'right'); }
    const jb = el.querySelector('.jbar > i:first-child'); if (jb) { jb.parentElement.dataset.fxg = '220,0'; fx(jb, 'grow', 'bar'); }
    const cols = el.querySelector('.clan-cols'); if (cols) { cols.dataset.fxg = '420,0'; fx(cols.querySelector('.clan-surv'), 'left', 'slide'); }
    q('.clan-right > .blk', (b) => fx(b, 'right', 'slide'));
    q('.cnt b, .need b', (b) => { b.classList.add('cu'); b.dataset.to = b.textContent; b.dataset.from = '0'; b.dataset.quiet = '1'; });
    q('.mprog b', (b) => { b.classList.add('cu'); b.dataset.to = b.textContent; b.dataset.from = '0'; b.dataset.quiet = '1'; });
    q('.lrows > div > b', (b) => { if (/^\d+$/.test(b.textContent)) { b.classList.add('cu'); b.dataset.to = b.textContent; b.dataset.from = '0'; b.dataset.quiet = '1'; } });
  }
  let lastSummary = null;
  function summary(r) {   // title block + the bottom-left survivors box of the home screen
    lastSummary = r;
    const ok = r && r.ok && r.hasClan && r.status === 'alive';
    const spm = document.body.classList.contains('sp-mode');   // SP: the plain game's title, no TWELVE WEEKS line
    $('t-fac').textContent = ok ? T(r.factionName) : ''; $('t-fac').hidden = !ok; $('t-sub').hidden = ok || spm;
    $('t-main').textContent = ok ? T('WEEK {w}: DAY {d}', { w: r.week, d: r.dayOfWeek }) : spm ? 'THE LAST OF US PART II' : 'THE LAST OF US';
    if (r && r.ok && r.hasClan && typeof Feed !== 'undefined') Feed.setClan(r);
    $('clanbox').hidden = !ok;
    if (!ok) { document.documentElement.dataset.theme = r && r.ok && r.hasClan && r.factionName ? (r.faction === 2 ? 'sera' : 'wlf') : 'neutral'; Scenes.setFaction(r && r.ok && r.hasClan ? (r.faction === 2 ? 'sera' : 'wlf') : 'neutral'); return; }
    $('cb-n').textContent = T('{n} SURVIVORS', { n: r.size }); $('cb-h').textContent = r.healthy; $('cb-u').textContent = r.hungry; $('cb-s').textContent = r.sick; $('cb-need').textContent = r.needed;
    if (!homeDial) homeDial = new Dial($('cb-dial'));
    homeDial.set(r.healthy, r.hungry, r.sick);
    document.documentElement.dataset.theme = r.faction === 2 ? 'sera' : 'wlf';
    Scenes.setFaction(r.faction === 2 ? 'sera' : 'wlf');
  }
  async function load() {
    try {
      const [r, fr] = await Promise.all([f2.clan(), f2.clanFeed(0)]);
      render(r.ok ? r.clan : null, r.error, fr.ok && fr.feed ? fr.feed : null, true); summary(r.ok ? r.clan : null);
    } catch (e) { f2.log('Clan.load failed: ' + (e && e.stack || e)); $('clan-body').innerHTML = '<div class="empty">' + esc(T('Could not show the clan: {e}', { e: String(e && e.message || e) })) + '</div>'; }
  }
  return { load, summary, rerender() { if (lastArgs) { render(lastArgs[0], lastArgs[1], lastArgs[2], false); } }, last: () => lastSummary };
})();
