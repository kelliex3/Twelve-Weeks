"use strict";
// Twelve Weeks admin: single page, polls the local API every 3 s; a view is only re-rendered when its HTML changed (no flicker).
const $ = (s, r = document) => r.querySelector(s);
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const TABS = ["overview", "players", "matches", "teams", "clans", "reports", "chat", "logs", "notes"];
const TITLES = { overview: "OVERVIEW", players: "PLAYERS", matches: "MATCHES", teams: "TEAMS", clans: "CLANS", reports: "REPORTS", chat: "CHAT", logs: "LOGS", notes: "PATCH NOTES" };
const POLL_MS = 3000;
const S = { route: { tab: "overview", arg: "" }, paused: false, q: "", logQ: "", logLevel: "all", logFile: "0", logQuiet: true, logFollow: true, chatQ: "", chatKind: "", last: {}, ov: null, timer: null, busy: 0, ovp: null, gen: 0, fileView: {} };

function dur(s) {
  if (s == null) return "-";
  s = Math.max(0, Math.round(s));
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? d + "d " + h + "h" : h ? h + "h " + String(m).padStart(2, "0") + "m" : m ? m + "m " + String(s % 60).padStart(2, "0") + "s" : s + "s";
}
function clock(s) { s = Math.max(0, Math.round(s)); return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0"); }
function when(ts) {
  if (!ts) return "-";
  const d = new Date(ts * 1000), n = new Date(), same = d.toDateString() === n.toDateString();
  const t = d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  return same ? t : d.toLocaleDateString([], { month: "short", day: "numeric" }) + " " + t;
}
function ago(ts) { return ts ? dur(Date.now() / 1000 - ts) + " ago" : "never"; }
function facCls(f) { return f === "WLF" ? "wlf" : f === "SERAPHITES" ? "sera" : ""; }
function facPill(f) { return f ? `<span class="pill ${facCls(f)}">${esc(f)}</span>` : '<span class="faint">-</span>'; }
function statePill(s) { return `<span class="pill ${esc(s)}">${esc({ match: "IN MATCH", queue: "SEARCHING", menu: "MENU" }[s] || s.toUpperCase())}</span>`; }
function platform(p) { return p ? (p === "linux" ? "LINUX" : "WINDOWS") : "-"; }

async function api(path) {
  const r = await fetch(path, { cache: "no-store" });
  if (r.status === 401) { loggedOut(); throw new Error("login required"); }
  if (!r.ok && r.status !== 404) throw new Error("HTTP " + r.status);
  return r.json();
}
async function post(path, body) {
  const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json", "X-Admin-Request": "1" }, body: JSON.stringify(body) });
  if (r.status === 401) loggedOut();
  return r.json();
}
let outShown = false;
function loggedOut() { if (!outShown) { outShown = true; clearInterval(S.timer); location.reload(); } }      // session ended: the reload shows the login page
function setHTML(el, key, html) {
  if (S.last[key] === html) return;
  S.last[key] = html;
  const keep = el.scrollTop;
  el.innerHTML = html;
  el.scrollTop = keep;
  labelCells(el);
}
// phone cards: every cell of a table row gets its column header as data-l (shown by admin.css under 900 px, where the header row is hidden)
function labelCells(root) {
  root.querySelectorAll(".tbl").forEach(t => {
    const th = t.querySelector(".tr.th");
    if (!th) return;
    const names = [...th.children].map(c => c.textContent.trim());
    t.querySelectorAll(".tr:not(.th)").forEach(r => [...r.children].forEach((c, i) => { if (i && names[i]) c.setAttribute("data-l", names[i]); }));
  });
}
function flash(msg, bad) {
  const d = document.createElement("div");
  d.className = "flash" + (bad ? " bad" : "");
  d.textContent = msg;
  document.body.appendChild(d);
  setTimeout(() => d.remove(), 4500);
}
function confirmBox(title, html, okLabel, danger) {
  return new Promise(res => {
    $("#m-title").textContent = title;
    $("#m-body").innerHTML = html;
    $("#m-foot").innerHTML = `<button class="btn" data-m="no">CANCEL</button><button class="btn ${danger ? "danger" : ""}" data-m="yes">${esc(okLabel)}</button>`;
    const m = $("#modal");
    m.hidden = false;
    const done = v => { m.hidden = true; m.onclick = null; document.removeEventListener("keydown", key, true); res(v); };
    const key = e => { if (e.key === "Escape") { e.preventDefault(); e.stopPropagation(); done(false); } };
    document.addEventListener("keydown", key, true);
    m.onclick = e => { const b = e.target.closest("[data-m]"); if (b) done(b.dataset.m === "yes"); else if (e.target === m) done(false); };
    setTimeout(() => $("#m-foot [data-m=no]").focus(), 0);
  });
}

// ---------------------------------------------------------------- views (HTML builders)
function fact(label, val, sub, cls) { return `<div class="fact ${cls || ""}"><label>${esc(label)}</label><b>${esc(val)}</b>${sub ? `<span>${esc(sub)}</span>` : ""}</div>`; }
function kv(l, v) { return `<div><label>${esc(l)}</label><b>${v}</b></div>`; }
function note(t, bad) { return `<div class="note ${bad ? "bad" : ""}">${esc(t)}</div>`; }

function onlineTable(d, limit) {
  const ps = limit ? d.players.slice(0, limit) : d.players;
  if (!ps.length) return '<div class="empty">NOBODY ONLINE RIGHT NOW</div>';
  let h = `<div class="tbl" style="--cols:minmax(120px,1.3fr) 150px 1fr 80px 90px 90px 80px ${d.canKick ? "70px" : ""}"><div class="tr th"><span>NAME</span><span>STATE</span><span>CLAN</span><span>PING</span><span>BUILD</span><span>PLATFORM</span><span>ONLINE</span>${d.canKick ? "<span></span>" : ""}</div>`;
  for (const p of ps) {
    const c = p.clan ? `<span class="${facCls(p.clan.factionName)}">${esc(p.clan.factionName)}</span> <span class="dim">W${p.clan.week} D${p.clan.day} · ${p.clan.size}</span>` : '<span class="faint">no clan</span>';
    const ping = p.ping != null ? p.ping + " ms" : "-";
    h += `<div class="tr ${p.playerId ? "click" : ""}" ${p.playerId ? `data-go="player/${esc(p.playerId)}"` : ""}>` +
      `<span>${esc(p.tag ? "[" + p.tag + "] " : "")}${esc(p.name)}</span><span>${statePill(p.state)}${p.state_s != null && p.state !== "menu" ? ` <span class="faint">${dur(p.state_s)}</span>` : ""}</span><span>${c}</span>` +
      `<span class="dim">${ping}</span><span class="${p.outdated ? "warn" : "dim"}">${esc(p.version || "-")}</span><span class="dim">${platform(p.platform)}</span><span class="dim">${p.online_s != null ? dur(p.online_s) : "-"}</span>` +
      (d.canKick ? `<span>${p.pid ? `<button class="btn sm danger" data-kick="${esc(p.pid)}" data-name="${esc(p.name)}">KICK</button>` : ""}</span>` : "") + "</div>";
  }
  return h + "</div>";
}

const NK = { u: "UPDATE AVAILABLE", r: "PLANNED RESTART", s: "SHUTTING DOWN", t: "NOTICE" };
function noticePanel(nt) {
  const list = (nt && nt.notices) || [];
  let h = '<div class="sec"><h3>SERVER NOTICES <span class="faint">(shown in game, in matches and menus, and in the launcher)</span></h3>';
  if (nt && nt.ok === false) h += note("Notices need the daemon admin listener: " + (nt.error || "not reachable"), true);
  h += list.length ? '<div class="tbl" style="--cols:190px 1fr 150px 90px">' + list.map(n => {
    const left = n.kind === "r" && n.endsAt ? "in " + dur(Math.max(0, Math.round(n.endsAt - Date.now() / 1000))) : n.endsAt ? "until " + when(n.endsAt) : "";
    return `<div class="tr"><span class="warn">${esc(NK[n.kind] || n.kind)}</span><span>${esc(n.text || "")}</span><span class="dim">${esc(left)}</span><span>${n.kind === "t" || n.kind === "r" ? `<button class="btn sm" data-nclear="${esc(n.id)}">${n.kind === "r" ? "CANCEL" : "REMOVE"}</button>` : ""}</span></div>`;
  }).join("") + "</div>" : '<div class="empty">NO ACTIVE NOTICE</div>';
  h += '<div class="row-in"><input class="tool-in grow" id="nt-text" maxlength="140" placeholder="Free text for all players">' +
    '<input class="tool-in num" id="nt-sec" value="8" title="seconds on screen (2-600)"><span class="dim">s</span><button class="btn main" data-nsend="1">SEND NOTICE</button></div>';
  h += '<div class="row-in"><input class="tool-in num" id="sd-min" value="10" title="minutes until the restart"><input class="tool-in grow" id="sd-why" maxlength="100" placeholder="Reason (optional)">' +
    '<button class="btn danger" data-nrestart="1">SCHEDULE RESTART</button><button class="btn" data-ncancel="1">CANCEL RESTART</button></div></div>';
  return h;
}
function viewOverview(ov, on, nt) {
  const s = ov.server;
  let h = "";
  for (const n of ov.notes) h += note(n);
  const up = s.up;
  h += '<div class="facts">' +
    fact("SERVER", up ? "UP" : "DOWN", up ? `port ${s.probe.addr}${s.probe.ms != null ? " · " + s.probe.ms + " ms" : ""}` : "game port not answering", up ? "ok" : "bad") +
    fact("ONLINE NOW", ov.online.total == null ? "?" : ov.online.total, "players with a live connection") +
    fact("IN MATCH", ov.online.match, "playing or loading") +
    fact("SEARCHING", ov.online.queue, "in the queue") +
    fact("MATCHES", ov.matchesRunning == null ? "?" : ov.matchesRunning, "running now") +
    "</div>";
  h += '<div class="cols2"><div class="sec"><h3>SERVER</h3><div class="kvs">' +
    kv("UPTIME", up && s.uptime_s != null ? dur(s.uptime_s) + ` <span class="faint">since ${when(s.started)}</span>` : "-") +
    kv("SERVER BUILD", esc(s.version || "-")) +
    kv("LATEST PUBLISHED", s.latest ? esc(s.latest) + (s.version && s.latest !== s.version ? ' <span class="warn">differs from the server</span>' : ' <span class="ok">same</span>') : '<span class="faint">no update data (--update-data)</span>') +
    kv("CLIENT BUILDS ONLINE", Object.keys(ov.clients.versions).length ? Object.entries(ov.clients.versions).map(([v, n]) => `${esc(v)} x${n}`).join(", ") + (ov.clients.outdated ? ` <span class="warn">${ov.clients.outdated} outdated</span>` : "") : '<span class="faint">-</span>') +
    kv("DATA SOURCE", ov.source === "live" ? '<span class="ok">LIVE (daemon admin listener)</span>' : ov.source === "snapshot" ? "DAEMON SNAPSHOT (10 s)" : '<span class="warn">DERIVED FROM LOG</span>') +
    kv("LOG", esc(s.logPath || "none")) +
    "</div></div><div class=\"sec\"><h3>DATABASE</h3><div class=\"kvs\">" +
    kv("PLAYERS KNOWN", ov.totals.players) + kv("CLANS", `${ov.totals.clansAlive} alive of ${ov.totals.clans}`) +
    kv("MATCHES LOGGED", `${ov.totals.matchesLogged} <span class="faint">(${ov.totals.matches24h} in the last 24 h)</span>`) +
    kv("NEW BUG REPORTS", ov.totals.reportsNew ? `<span class="warn">${ov.totals.reportsNew}</span>` : "0") + "</div></div></div>";
  h += noticePanel(nt);
  h += `<div class="sec"><h3>WHO'S ONLINE</h3>${onlineTable(on, 8)}</div>`;
  h += '<div class="sec"><h3>RECENT ERRORS</h3>';
  h += ov.errors.length ? ov.errors.map(e => `<div class="ll err logbox">${esc(when(e.ts))}  ${esc(e.line)}</div>`).join("") : '<div class="empty">NO ERRORS IN THE LOG</div>';
  return h + "</div>";
}

// ban / unban (a ban button on every player for the mods; the server writes bans.txt, the daemon refuses the player from then on)
function banBtn(id, name, banned) { return `<button class="btn sm ${banned ? "" : "danger"}" data-${banned ? "unban" : "ban"}="${esc(id)}" data-name="${esc(name)}">${banned ? "UNBAN" : "BAN"}</button>`; }
// timed bans, like mutes: the server notes the end on the bans.txt line and drops it when the time is up
const BAN_DUR = [[60, "1 HOUR"], [360, "6 HOURS"], [1440, "1 DAY"], [4320, "3 DAYS"], [10080, "7 DAYS"], [43200, "30 DAYS"], [0, "FOR GOOD"]];
function banLeft(left) { return left === -1 || left == null ? "for good" : dur(left) + " left"; }
const BAN_KIND = { steam: "STEAM ID", name: "NAME", pid: "IDENTITY", hwid: "HARDWARE", token: "INSTALL", ip: "IP" };
// every running ban on the PLAYERS page, newest first, with its UNBAN
function bansTable(bans) {
  const q = S.q.trim().toLowerCase();
  const list = q ? bans.filter(b => [b.entry, b.name, b.note, b.reason].some(x => (x || "").toLowerCase().includes(q))) : bans;
  let h = `<div class="sec"><h3>BANNED (${list.length}${q ? " matching" : ""})</h3>`;
  if (!list.length) return h + `<div class="empty">${q ? "NO BAN MATCHING" : "NOBODY IS BANNED"}</div></div>`;
  h += '<div class="tbl" style="--cols:minmax(130px,1fr) minmax(150px,200px) minmax(180px,1.6fr) 120px minmax(150px,1fr) 80px"><div class="tr th"><span>PLAYER</span><span>BANNED</span><span>REASON</span><span>TIME</span><span>BY</span><span></span></div>';
  for (const b of list) {
    const who = b.name || (b.kind === "name" ? b.entry : b.note.split(/[,-]/)[0].trim()) || b.entry;
    // "<name> - admin site <date> by <admin>" -> "<admin> · <date>"; other notes as written
    const by = (b.note.match(/\bby ([^|,]+)$/) || [])[1], day = (b.note.match(/\d{4}-\d{2}-\d{2}/) || [])[0];
    const byText = by ? by.trim() + (day ? " · " + day : "") : b.note;
    const reason = (b.reason || "").replace(/\s*\(ban ends [^)]*\)$/, "").replace(/^Ban ends .*$/, "");
    h += `<div class="tr${b.pid ? " click" : ""}"${b.pid ? ` data-go="player/${esc(b.pid)}"` : ""}><span>${esc(who)}</span>` +
      `<span class="dim" title="${esc(b.entry)}">${esc(BAN_KIND[b.kind] || b.kind)} <span class="faint">${esc(b.entry.replace(/^(pid|hwid|token|ip):/i, "").slice(0, 20))}</span></span>` +
      `<span class="chatline">${esc(reason || "-")}</span><span class="${b.left === -1 ? "dim" : "warn"}">${esc(banLeft(b.left))}</span>` +
      `<span class="dim" title="${esc(b.note)}">${esc(byText)}</span><span><button class="btn sm" data-unban-entry="${esc(b.entry)}" data-name="${esc(who)}">UNBAN</button></span></div>`;
  }
  return h + "</div></div>";
}

// chat mutes (with a duration; the server writes mutes.txt, the daemon refuses their party / lobby /
// friend lines and the game shows YOU ARE MUTED instead of opening the chat)
const MUTE_DUR = [[15, "15 MIN"], [60, "1 HOUR"], [360, "6 HOURS"], [1440, "1 DAY"], [10080, "7 DAYS"], [43200, "30 DAYS"], [0, "FOR GOOD"]];
function muteLeft(left) { return left === -1 ? "for good" : dur(left) + " left"; }
function muteBtn(id, name, muted) { return id ? `<button class="btn sm ${muted ? "" : "warn"}" data-${muted ? "unmute" : "mute"}="${esc(id)}" data-name="${esc(name)}">${muted ? "UNMUTE" : "MUTE"}</button>` : ""; }
const CHAT_KIND = { match: "MATCH", team: "TEAM", party: "PARTY", lobby: "LOBBY" };
function viewChat(d) {
  let h = "";
  if (d.mutes && d.mutes.length) {
    h += `<div class="sec"><h3>MUTED NOW (${d.mutes.length})</h3><div class="tbl" style="--cols:minmax(140px,1fr) 160px 90px"><div class="tr th"><span>PLAYER</span><span>TIME</span><span></span></div>`;
    for (const m of d.mutes) h += `<div class="tr${m.pid ? " click" : ""}"${m.pid ? ` data-go="player/${esc(m.pid)}"` : ""}><span>${esc(m.name)}</span><span class="dim">${esc(muteLeft(m.left))}</span><span>${muteBtn(m.pid, m.name, true)}</span></div>`;
    h += "</div></div>";
  }
  h += `<div class="sec"><h3>${d.player ? "CHAT OF " + esc(d.player) : "CHAT"} <span class="faint">match, team, party and lobby lines, newest first, kept 14 days. Friend messages are private and never shown.</span></h3>`;
  if (!d.lines.length) return h + '<div class="empty">NO CHAT LINES' + (S.chatQ || S.chatKind ? " MATCHING" : "") + "</div></div>";
  h += '<div class="tbl chat" style="--cols:112px 64px minmax(110px,170px) minmax(200px,1fr) 80px"><div class="tr th"><span>TIME</span><span>CHANNEL</span><span>PLAYER</span><span>LINE</span><span></span></div>';
  for (const l of d.lines) {
    const who = l.admin ? `<b class="admin-name">${esc(l.name)}</b>` : l.pid ? `<a href="#player/${esc(l.pid)}">${esc(l.name)}</a>` : esc(l.name);
    // a MATCH / TEAM line opens its match page
    const mid = (l.kind === "match" || l.kind === "team") && l.room ? l.room : "";
    h += `<div class="tr${mid ? " click" : ""}"${mid ? ` data-go="match/${esc(mid)}" title="Open match ${esc(mid)}"` : ""}><span class="dim" title="${esc(new Date(l.ts * 1000).toLocaleString())}">${esc(when(l.ts))}</span>` +
      `<span>${l.admin ? '<span class="pill ch-admin">ADMIN</span>' : `<span class="pill ch-${esc(l.kind)}" title="${esc(l.room)}">${esc(CHAT_KIND[l.kind] || l.kind)}</span>`}</span>` +
      `<span>${who}${l.mutedNow ? ' <span class="pill bad">MUTED</span>' : ""}</span><span class="chatline${l.sentMuted ? " faint" : ""}">${esc(l.text)}${l.sentMuted ? ' <span class="faint">(sent while muted)</span>' : ""}</span>` +
      `<span>${l.admin ? "" : muteBtn(l.pid, l.name, l.mutedNow)}</span></div>`;
  }
  return h + "</div></div>";
}

function viewPlayers(on, all) {
  let h = on.source === "log" ? note("Live feed not active: names come from the log, session data is approximate. Kick and ping need the daemon build with the admin listener (daemon restart).") : "";
  h += `<div class="sec"><h3>ONLINE NOW (${on.players.length})</h3>${onlineTable(on)}</div>`;
  if (all.bans) h += bansTable(all.bans);
  h += `<div class="sec"><h3>ALL PLAYERS (${all.shown}${S.q ? " matching" : ""})${all.hidden ? ` <span class="faint">${all.hidden} never-played installs hidden - search to find them</span>` : ""}</h3>`;
  if (!all.players.length) return h + '<div class="empty">NO PLAYER FOUND</div></div>';
  h += '<div class="tbl" style="--cols:minmax(110px,1.3fr) 158px minmax(130px,1.2fr) 64px 50px 60px 100px 70px 62px"><div class="tr th"><span>NAME</span><span>STEAM ID</span><span>CLAN</span><span>MATCHES</span><span>WINS</span><span>LOSSES</span><span>LAST SEEN</span><span>BUILD</span><span></span></div>';
  for (const p of all.players) {
    h += `<div class="tr click" data-go="player/${esc(p.id)}"><span>${p.online ? '<span class="ok">● </span>' : ""}${esc(p.name)}${p.banned ? ' <span class="pill bad">BANNED</span>' : ""}${p.muted ? ' <span class="pill bad">MUTED</span>' : ""}</span><span class="dim">${p.steamid || "-"}</span>` +
      `<span>${p.faction ? `<span class="${facCls(p.faction)}">${esc(p.faction)}</span> <span class="dim">${p.clanStatus === "alive" ? `W${p.week} D${p.day} · ${p.size}` : esc(p.clanStatus)}</span>` : '<span class="faint">no clan</span>'}</span>` +
      `<span>${p.matches}</span><span>${p.wins}</span><span>${p.losses}</span><span class="dim">${p.online ? "now" : ago(p.lastSeen)}</span><span class="dim">${esc(p.version || "-")}</span><span>${banBtn(p.id, p.name, p.banned)}</span></div>`;
  }
  return h + "</div></div>";
}

function clanBlock(c, morale) {
  if (!c) return '<div class="empty">NO CLAN YET</div>';
  const pct = c.status === "alive" ? Math.min(100, Math.round(c.day / c.daysTotal * 100)) : 100;
  const tot = Math.max(1, c.size);
  return `<div class="kvs">` + kv("FACTION", facPill(c.factionName)) + kv("STATUS", esc(c.status.toUpperCase()) + (c.risk.length ? ` <span class="bad">AT RISK: ${esc(c.risk.join(", "))}</span>` : "")) +
    kv("PROGRESS", c.status === "alive" ? `WEEK ${c.week} OF ${c.weeks} · DAY ${c.dayOfWeek} <span class="faint">(${c.day} / ${c.daysTotal} days)</span><div class="bar2 ${facCls(c.factionName)}"><i style="width:${pct}%"></i></div>` : "-") +
    kv("SURVIVORS", `${c.size} <span class="faint">healthy ${c.healthy} · hungry ${c.hungry} · sick ${c.sick} · needs ${c.needed}/day</span>`) +
    kv("WEEKS DONE (ALL RUNS)", c.weeksTotal) + kv("CLAN RUNS", `${c.runsWon} won · ${c.runsLost} lost`) +
    (c.lastParts != null ? kv("LAST MATCH", `${c.lastParts} parts · ${c.lastSupplies} supplies${c.lastDied ? ` · <span class="bad">${c.lastDied} died</span>` : ""}`) : "") +
    (morale ? kv("MORALE", `${esc(morale.label.toUpperCase())} <span class="faint">${morale.value}</span>`) : "") + "</div>";
}

function viewAlife(a) {
  if (!a) return "";
  let h = '<div class="sec"><h3>CLAN A-LIFE <span class="faint">(the simulated life around the real clan numbers)</span></h3>';
  h += `<div class="kvs">` + kv("SWITCH", a.enabled ? '<span class="ok">ON</span>' : '<span class="warn">OFF</span> <span class="faint">(tools/clan_alife.txt, default off)</span>') + kv("SIMULATION", a.exists ? "running" : "none yet") + "</div>";
  if (!a.exists) return h + (a.enabled ? '<div class="empty">NO A-LIFE STATE FOR THIS CLAN YET <span class="faint">(the switch is on, but this clan has not played a day since)</span></div></div>'
    : '<div class="empty">CLAN A-LIFE IS SWITCHED OFF ON THIS SERVER <span class="faint">(builds PT84 / PT85 ship with it off, so no simulation is written for anyone: this is expected, not a fault)</span></div></div>');
  const s = a.state, pl = a.pl || {};
  h += '<div class="kvs">' + kv("DAY", s.day) + kv("WEATHER", esc(s.weather) + " · " + esc(s.season)) + kv("ROWS WRITTEN", s.stats.rows + " in " + s.stats.days + " days") +
    kv("TEMPLATES USED", s.stats.templatesUsed + " of " + s.stats.templatesTotal + " (window repeats " + s.stats.windowRepeats + ")") + "</div>";
  h += '<h3 style="margin-top:14px">ONGOING</h3>' + (s.threads.length ? s.threads.map((t, i) => `<div class="feedline"><em>${esc(t.kind.toUpperCase())}</em>${esc(t.text)}${pl.threads && pl.threads[i] ? `<div class="faint pl">PL: ${esc(pl.threads[i])}</div>` : ""}</div>`).join("") : '<div class="empty">NOTHING RUNNING</div>');
  h += '<h3 style="margin-top:14px">SURVIVORS <span class="faint">(the real roster: state and counts come from the clan rules)</span></h3><div class="tbl" style="--cols:1.1fr 1fr 80px 1.5fr 1fr 1.3fr"><div class="tr th"><span>NAME</span><span>ROLE</span><span>STATE</span><span>TRAITS</span><span>BONDS</span><span>PAST</span></div>';
  for (const m of s.people) {
    const bonds = [m.friend ? "friend " + m.friend : "", m.partner ? "partner " + m.partner : "", m.rival ? "rift with " + m.rival : ""].filter(Boolean).join(", ");
    const tr = (pl.traits && pl.traits[m.name]) ? " · " + pl.traits[m.name].join(", ") : "";
    h += `<div class="tr"><span>${esc(m.name)}${m.away ? ' <span class="pill queue">AWAY</span>' : ""}</span><span class="dim">${esc(m.role)}</span><span class="${m.state === "sick" ? "bad" : m.state === "hungry" ? "warn" : "ok"}">${esc(m.state.toUpperCase())}</span>` +
      `<span class="dim">${esc(m.traits.join(", "))}<span class="faint">${esc(tr)}</span></span><span class="dim">${esc(bonds || "-")}</span><span class="dim">${esc(m.past)} <span class="faint">${esc(m.origin)}</span></span></div>`;
  }
  h += "</div>";
  if (s.memorial.length) h += '<h3 style="margin-top:14px">REMEMBERED</h3><div class="faint">' + s.memorial.map(d => esc(d.name) + " (day " + d.day + ", " + esc(d.cause || "?") + ")").join(" · ") + "</div>";
  if (a.menu) h += '<h3 style="margin-top:14px">WHAT THE MENU SHOWS NOW</h3><div class="cols2"><div>' + a.menu.en.map(t => `<div class="feedline">${esc(t)}</div>`).join("") + '</div><div>' + a.menu.pl.map(t => `<div class="feedline">${esc(t)}</div>`).join("") + "</div></div>";
  return h + "</div>";
}

function viewPlayer(p) {
  if (!p.ok) return `<button class="back" data-go="players">&lt; PLAYERS</button><div class="empty">${esc(p.error || "no such player")}</div>`;
  const dec = p.wins + p.losses + p.draws;
  const wr = dec ? Math.round(p.wins / dec * 100) + "%" : "-";
  let h = '<button class="back" data-go="players">&lt; PLAYERS</button>';
  h += `<div class="match" style="margin-top:10px"><div class="mh"><b>${esc(p.name)}</b><span>${p.online ? statePill(p.online.state) : '<span class="faint">OFFLINE</span>'} ${p.clan ? facPill(p.clan.factionName) : ""} ${p.canKick ? `<button class="btn sm danger" data-kick="${esc(p.online.pid)}" data-name="${esc(p.name)}">FORCE KICK</button>` : ""} ${muteBtn(p.id, p.name, !!p.mute)} ${banBtn(p.id, p.name, !!p.ban)} <button class="btn sm" data-go="chat/${esc(p.id)}">CHAT</button></span></div>`;
  if (p.mute) h += note("MUTED " + muteLeft(p.mute.left) + (p.mute.reason ? ": " + p.mute.reason : ""), true);
  if (p.ban) h += note("BANNED " + banLeft(p.ban.left) + (p.ban.reason ? ": " + p.ban.reason : "") + "  (bans.txt: " + p.ban.line + ")", true);
  const nowMid = p.online && p.online.match ? String(p.online.match) : "";   // the daemon's live row names the match the player is in
  if (nowMid) {
    const hm = p.history.find(m => m.id === nowMid);
    h += `<div class="nowmatch click" data-go="match/${esc(nowMid)}" title="Open match ${esc(nowMid)}"><span class="pill match">IN MATCH NOW</span>` +
      `<b>${esc(hm && hm.map ? hm.map : "map not chosen yet")}</b><span class="dim">${hm && hm.players ? hm.players + " players · " : ""}${hm && hm.when ? "started " + esc(ago(hm.when)) + " · " : ""}ID ${esc(nowMid)}</span><span class="go">OPEN MATCH &gt;</span></div>`;
  }
  h += '<div class="facts" style="margin:0">' + fact("MATCHES", p.matches) + fact("WINS", p.wins, wr + " win rate", "") + fact("LOSSES", p.losses) + fact("DRAWS", p.draws) +
    (p.avgParts != null ? fact("AVG PARTS", p.avgParts, "per match") : "") + (p.kills || p.deaths ? fact("K / D", p.kills + " / " + p.deaths) : "") + "</div></div>";
  h += '<div class="cols2"><div class="sec"><h3>ACCOUNT</h3><div class="kvs">' +
    kv("STEAM ID", p.steamid || "-") + kv("INSTALLS", p.installs) + kv("LAST SEEN", p.online ? '<span class="ok">online now</span>' : `${esc(when(p.lastSeen))} <span class="faint">${esc(ago(p.lastSeen))}</span>`) +
    kv("CLIENT BUILD", esc(p.version || "-")) + kv("PLATFORM", platform(p.platform)) + kv("ALSO KNOWN AS", p.aliases.length ? esc(p.aliases.join(", ")) : "-") +
    (p.matches === 0 ? "" : "") + "</div>" +
    (p.kills || p.deaths ? "" : '<div class="faint" style="font-size:11.5px;margin-top:8px;line-height:1.5">Kills, deaths and executions are not recorded by the server (the engine keeps them in the match).</div>') +
    '</div><div class="sec"><h3>CLAN</h3>' + clanBlock(p.clan, p.morale) + "</div></div>";
  if (p.clan && p.clan.status) h += `<div class="sec"><h3>CLAN, AS THE PLAYER SEES IT <span class="faint">(the launcher's CLAN tab, read-only; EN / PL button inside)</span></h3><iframe class="clanframe" title="clan" src="/clan.html?id=${encodeURIComponent(p.id)}"></iframe></div>`;
  if (p.feed.length) {      // English only (both sites get localisation later), the newest few open, the rest behind SHOW ALL
    const line = m => `<div class="feedline"><em>W${m.week} D${m.day}${m.kind === "alife" ? ' <span class="pill ok">A-LIFE</span>' : ""}</em>${esc(m.text)}</div>`;
    const N = 5, more = p.feed.length - N;
    h += `<div class="sec"><h3>CLAN FEED <span class="faint">(what the launcher journal shows, newest first)</span></h3>${p.feed.slice(0, N).map(line).join("")}` +
      (more > 0 ? `<details class="feedmore"><summary><span class="sm-open">SHOW ALL ${p.feed.length}</span><span class="sm-close">SHOW LESS</span></summary>${p.feed.slice(N).map(line).join("")}</details>` : "") + "</div>";
  }
  h += viewAlife(p.alife);
  h += `<div class="sec"><h3>CLAN RESET <span class="faint">(wipes this player's clan to a fresh start; the old state is saved so it can be restored)</span></h3><div class="row" style="gap:8px;display:flex;flex-wrap:wrap"><button class="btn sm danger" data-clanreset="${esc(p.id)}" data-name="${esc(p.name)}">RESET CLAN</button><button class="btn sm" data-clanrestore="${esc(p.id)}" data-name="${esc(p.name)}">RESTORE A BACKUP</button></div></div>`;
  h += '<div class="sec"><h3>MATCH HISTORY <span class="faint">(from the retained daemon logs)</span></h3>';
  if (!p.history.length) h += '<div class="empty">NO MATCHES IN THE RETAINED LOGS' + (p.matches ? ` <span class="faint">(the server counted ${p.matches} match${p.matches === 1 ? "" : "es"} in total; older ones are in logs that were rotated away${p.matchLogFrom ? ", the oldest line kept is " + esc(when(p.matchLogFrom)) : ""})</span>` : "") + "</div>";
  else {
    h += '<div class="tbl" style="--cols:130px 1fr 90px 90px 1fr"><div class="tr th"><span>WHEN</span><span>MAP</span><span>RESULT</span><span>LENGTH</span><span>NOTE</span></div>';
    for (const m of p.history) {
      const r = { W: '<span class="ok">WIN</span>', L: '<span class="bad">LOSS</span>', D: "DRAW" }[m.result] || '<span class="faint">-</span>';
      h += `<div class="tr click" data-go="match/${esc(m.id)}" title="Open match ${esc(m.id)}"><span class="dim">${esc(when(m.when))}</span><span>${esc(m.map || "?")} <span class="faint">${m.players} players</span></span><span>${r}</span><span class="dim">${m.secs ? dur(m.secs) : "-"}</span><span class="warn">${m.left ? "left early / timed out" : m.state !== "ended" ? "no result" : ""}</span></div>`;
    }
    h += "</div>";
  }
  h += "</div>";
  if (p.reports.length) {
    h += '<div class="sec"><h3>BUG REPORTS</h3><div class="tbl" style="--cols:80px 140px 1fr 90px">' + p.reports.map(r => `<div class="tr click" data-go="report/${esc(r.code)}"><span>${esc(r.code)}</span><span class="dim">${esc((r.received || "").replace("T", " ").slice(0, 16))}</span><span class="dim">${esc(r.text)}</span><span>${r.handled ? '<span class="faint">handled</span>' : '<span class="warn">NEW</span>'}</span></div>`).join("") + "</div></div>";
  }
  return h;
}

function teamBlock(name, list, cls) {
  return `<div><h4><span class="${cls}">${name}</span><span>${list.length}</span></h4>` +
    (list.map(p => `<p><span>${esc(p.name)}${p.host ? ' <span class="faint">HOST</span>' : ""}${p.left ? ' <span class="bad">LEFT</span>' : ""}</span><span class="dim">${p.ping_ms != null ? p.ping_ms + " ms" : ""}${d_kick(p)}</span></p>`).join("") || '<p class="faint">-</p>') + "</div>";
}
let KICKABLE = false;
function d_kick(p) { return KICKABLE && p.pid && !p.left ? ` <button class="btn sm danger" data-kick="${esc(p.pid)}" data-name="${esc(p.name)}">KICK</button>` : ""; }

function viewMatches(d) {
  KICKABLE = d.canKick;
  let h = d.approx ? note("Live feed not active: running matches are reconstructed from the log (players, map, host). Scores and time left need the daemon build with the live snapshot (daemon restart).") : "";
  h += `<div class="sec"><h3>LIVE MATCHES (${d.live.length})</h3>`;
  if (!d.live.length) h += '<div class="empty">NO MATCH RUNNING</div>';
  for (const m of d.live) {
    const t0 = m.players.filter(p => p.team === 0), t1 = m.players.filter(p => p.team === 1);
    const left = m.clock ? clock(m.clock.left - m.clock.age_s) : null;
    h += `<div class="match click" data-go="match/${esc(m.id)}"><div class="mh"><b>${esc(m.map ? m.map.name : "Map not chosen yet")}</b><span><span class="pill ${m.state === "running" ? "match" : "queue"}">${m.state === "running" ? "RUNNING" : "LOADING"}</span> ${esc(m.mode ? m.mode.toUpperCase() + " · " : "")}${esc(m.net === "steam-p2p" ? "STEAM P2P" : m.net === "server-relay" ? "SERVER RELAY" : "")} · ${dur(m.age_s)} · ID ${esc(m.id)}</span></div>`;
    h += `<div class="team">${teamBlock("TEAM 1", t0, "wlf")}${teamBlock("TEAM 2", t1, "sera")}</div>`;
    h += `<div class="kvs" style="margin:10px 0 0">` + kv("TIME LEFT", left != null ? left : '<span class="faint">no clock report yet</span>') +
      kv("REINFORCEMENTS", m.lives ? `${m.lives.team0} : ${m.lives.team1} <span class="faint">team 1 : team 2</span>` : '<span class="faint">not reported</span>') + "</div></div>";
  }
  h += "</div>" + `<div class="sec"><h3>RECENT MATCHES${d.approx ? "" : ""}</h3>`;
  if (!d.history.length) return h + '<div class="empty">NO MATCH IN THE LOG</div></div>';
  h += '<div class="tbl" style="--cols:120px 1fr 1.6fr 190px 80px 110px"><div class="tr th"><span>WHEN</span><span>MAP</span><span>TEAMS</span><span>RESULT</span><span>LENGTH</span><span>NET</span></div>';
  for (const m of d.history) {
    h += `<div class="tr click" data-go="match/${esc(m.id)}"><span class="dim">${esc(when(m.started || m.formed))}</span><span>${esc(m.map || "?")}</span>` +
      `<span class="dim" title="${esc(m.team1.join(", ") + " vs " + m.team2.join(", "))}"><span class="wlf">${esc(m.team1.join(", ") || "-")}</span> vs <span class="sera">${esc(m.team2.join(", ") || "-")}</span>${m.left.length ? ` <span class="warn">· ${esc(m.left.join(", "))}</span>` : ""}</span>` +
      `<span class="${m.state === "ended" ? (m.winner === 2 ? "" : "ok") : "warn"}">${esc(m.result)}</span><span class="dim">${m.secs ? dur(m.secs) : "-"}</span><span class="dim">${m.net === "steam-p2p" ? "P2P" : m.net === "server-relay" ? "RELAY" : "-"}${m.ping_ms != null ? " " + m.ping_ms + "ms" : ""}</span></div>`;
  }
  return h + "</div></div>";
}

// one match: its live chat log, players, kick / ban / mute buttons
function viewMatch(d) {
  if (!d.ok) return '<div class="sec"><a href="#matches" class="faint">&lsaquo; MATCHES</a></div>' + note(d.error || "Match not found.", true);
  const m = d.match;
  KICKABLE = d.canKick;
  let h = '<div class="sec"><a href="#matches" class="faint">&lsaquo; MATCHES</a></div>';
  const st = m.live ? `<span class="pill ${m.state === "running" ? "match" : "queue"}">${m.state === "running" ? "RUNNING" : "LOADING"}</span>` : `<span class="pill">${esc(m.result || "ENDED")}</span>`;
  h += `<div class="match" style="margin-top:10px"><div class="mh"><b>${esc(m.map || "?")}</b><span>${st} ${esc(m.mode ? m.mode.toUpperCase() + " · " : "")}${esc(m.net === "steam-p2p" ? "STEAM P2P" : m.net === "server-relay" ? "SERVER RELAY" : "")} · ID ${esc(m.id)}</span></div>`;
  const kvs = [];
  if (m.live) {
    kvs.push(kv("RUNNING FOR", dur(m.age_s)));
    kvs.push(kv("TIME LEFT", m.clock ? clock(m.clock.left - m.clock.age_s) : '<span class="faint">no clock report yet</span>'));
    kvs.push(kv("REINFORCEMENTS", m.lives ? `${m.lives.team0} : ${m.lives.team1} <span class="faint">team 1 : team 2</span>` : '<span class="faint">not reported</span>'));
  } else {
    kvs.push(kv("STARTED", esc(when(m.started || m.formed))));
    kvs.push(kv("LENGTH", m.secs ? dur(m.secs) : "-"));
  }
  if (m.host) kvs.push(kv("HOST", esc(m.host) + (m.ping_ms != null ? ` <span class="faint">worst pair ${m.ping_ms} ms</span>` : "")));
  if (m.left_log && m.left_log.length) kvs.push(kv("LEFT EARLY", `<span class="warn">${esc(m.left_log.join(", "))}</span>`));
  if (m.joined && m.joined.length) kvs.push(kv("JOINED LATE", esc(m.joined.join(", "))));
  h += `<div class="kvs" style="margin:10px 0 0">${kvs.join("")}</div>`;
  const row = p => `<div class="tr"><span>${p.playerId ? `<a href="#player/${esc(p.playerId)}">${esc(p.display || p.name)}</a>` : esc(p.name)}` +
    `${p.host ? ' <span class="faint">HOST</span>' : ""}${p.left ? ' <span class="bad">LEFT</span>' : ""}${p.banned ? ' <span class="pill bad">BANNED</span>' : ""}${p.muted ? ' <span class="pill bad">MUTED</span>' : ""}</span>` +
    `<span class="dim">${p.ping_ms != null ? p.ping_ms + " ms" : ""}</span>` +
    `<span class="acts">${p.kick ? `<button class="btn sm danger" data-kick="${esc(p.pid)}" data-name="${esc(p.name)}">KICK</button> ` : ""}${muteBtn(p.playerId, p.display || p.name, p.muted)} ${p.playerId ? banBtn(p.playerId, p.display || p.name, p.banned) : ""}</span></div>`;
  const team = (n, cls, ps) => `<div class="sec"><h3><span class="${cls}">${n}</span> (${ps.length})</h3><div class="tbl" style="--cols:minmax(0,1fr) 54px max-content">${ps.map(row).join("") || '<div class="empty">-</div>'}</div></div>`;
  h += `<div class="team mteams">${team("TEAM 1", "wlf", d.players.filter(p => p.team === 0))}${team("TEAM 2", "sera", d.players.filter(p => p.team === 1))}</div></div>`;
  h += `<div class="sec"><h3>MATCH CHAT (${d.chat.length}) <span class="faint">match and team lines${m.live ? ", live" : ""}</span></h3>`;
  if (!d.chat.length) h += '<div class="empty">NO CHAT IN THIS MATCH' + (m.live ? " YET" : "") + "</div>";
  else {
    h += '<div class="mchat" id="mchat">';
    for (const l of d.chat) {
      h += `<div class="ml${l.sentMuted ? " faint" : ""}"><span class="dim">${esc(new Date(l.ts * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }))}</span> ` +
        (l.admin ? '<span class="pill ch-admin">ADMIN</span> <b class="admin-name">' + esc(l.name) + "</b>" :
          `<span class="pill ch-${esc(l.kind)}">${esc(CHAT_KIND[l.kind])}</span> <b>${l.pid ? `<a href="#player/${esc(l.pid)}">${esc(l.name)}</a>` : esc(l.name)}</b>`) +
        `${l.mutedNow ? ' <span class="pill bad">MUTED</span>' : ""}: ` +
        `<span class="chatline">${esc(l.text)}</span>${l.sentMuted ? ' <span class="faint">(sent while muted)</span>' : ""} ${l.admin ? "" : muteBtn(l.pid, l.name, l.mutedNow)}</div>`;
    }
    h += "</div>";
  }
  // an admin line typed into the match chat AS: a name: the line goes to every
  // player of this running match (daemon POST /chat/say, clients poll S_ACHAT) and shows on their MATCH tab in the admin colour (from the next client update).
  if (m.live) {
    h += `<div class="msay"><label>AS: <input id="msay-as" type="text" maxlength="24" list="msay-names" autocomplete="off" value="${esc(sayAs())}"></label>` +
      `<datalist id="msay-names">${["Server", "Kellie", "Admin", "Moderator"].map(n => `<option value="${n}">`).join("")}</datalist>` +
      `<input id="msay-text" type="text" maxlength="160" autocomplete="off" placeholder="Message to everyone in this match (Enter sends)">` +
      `<button class="btn sm" data-msay="${esc(m.id)}">SEND</button></div>`;
  }
  return h + "</div>";
}
function sayAs() { try { return localStorage.getItem("t2r-say-as") || "Server"; } catch (e) { return "Server"; } }
async function sendSay(mid) {
  const as = ($("#msay-as") || {}).value || "", text = (($("#msay-text") || {}).value || "").trim();
  if (!as.trim()) return flash("Type a name in AS: first.", true);
  if (!text) return flash("Type the message first.", true);
  try { localStorage.setItem("t2r-say-as", as.trim()); } catch (e) { /* private window */ }
  const r = await post("/api/chat-say", { mid, name: as.trim(), text });
  flash(r.ok ? `Sent to the match as ${as.trim()}.` : "Send failed: " + (r.error || "unknown"), !r.ok);
  if (r.ok) { $("#msay-text").value = ""; S.last.body = null; refresh(true); }
}

// TEAMS (manual team switching): the daemon's team rules, applied at every match
// formation (and the searching screen's [T1] / [T2] labels): "apart A B ..." never on one team, "together A B ..." same team.
function teamsLive(d) {
  const live = d.live || [];
  if (!live.length) return '<div class="empty">NO MATCH RUNNING</div>';
  return live.map(m => {
    const col = (n, ps) => `<div class="sec"><h3>${n}</h3>${ps.length ? ps.map(p => `<div class="tr"><span>${esc(p.name)} <span class="faint">slot ${p.slot}</span></span>` +
      (p.slot === 0 ? '<span class="faint">HOST</span>' : `<span><button class="btn sm" data-move="${esc(p.pid)}" data-name="${esc(p.name)}">MOVE TO OTHER TEAM</button></span>`) + "</div>").join("") : '<div class="empty">-</div>'}</div>`;
    const mv = (m.moving || []).map(x => `${esc(x.name)} -> team ${x.team === 0 ? "A" : "B"}`).join(", ");
    return `<div class="dim">MATCH ${esc(m.mid)}${mv ? " · WAITING TO RE-ENTER: " + mv : ""}</div><div class="facts">${col("TEAM A", m.teams[0])}${col("TEAM B", m.teams[1])}</div>`;
  }).join("");
}
function teamsPreview(d) {
  if (!d.ok) return note(d.error, true);
  const p = d.preview || {}, col = (n, a) => `<div class="sec"><h3>${n}</h3>${a.length ? a.map(x => `<div class="tr"><span>${esc(x)}</span></div>`).join("") : '<div class="empty">-</div>'}</div>`;
  if (!p.t0 || !p.t0.length) return `<div class="empty">QUEUE: ${p.queue && p.queue.length ? esc(p.queue.join(", ")) : "NOBODY SEARCHING"} - THE SPLIT SHOWS WITH 2+ SEARCHING</div>`;
  return `<div class="facts">${col("TEAM A (NEXT MATCH)", p.t0)}${col("TEAM B (NEXT MATCH)", p.t1)}</div>`;
}
function viewTeams(d) {
  if (!d.ok) return note(d.error, true);
  return `<div class="sec"><h3>RULES</h3><div class="dim">One rule per line, names as shown in game (any case) or Steam ids.<br>` +
    `<b>apart ST notqntm</b> = never on the same team &nbsp;·&nbsp; <b>together DeSyncc jaskellie</b> = same team &nbsp;·&nbsp; # = comment.<br>` +
    `Applies to the next match that forms (a running match keeps its teams). top_players.txt players are also kept apart.</div>` +
    `<textarea id="trules" class="tool-in" rows="8" style="margin-top:12px;font-family:Consolas,monospace">${esc(d.rules)}</textarea>` +
    `<div style="margin-top:10px"><button class="btn main" id="tsave">SAVE RULES</button></div></div>` +
    `<div class="sec"><h3>NEXT SPLIT (QUEUE NOW)</h3><div id="tprev">${teamsPreview(d)}</div></div>` +
    `<div class="sec"><h3>RUNNING MATCHES</h3><div class="dim">MOVE sends the player to the menu; their next FIND MATCH puts them back into the same match on the other team (valid 15 min).</div><div id="tlive">${teamsLive(d)}</div></div>`;
}
function viewClans(d) {
  let h = '<div class="facts">' + fact("CLANS ALIVE", d.alive) + fact("WLF", d.byFaction.WLF, "alive clans") + fact("SERAPHITES", d.byFaction.SERAPHITES, "alive clans") + fact("AT RISK", d.atRisk, "dying or struggling", d.atRisk ? "warn" : "") + "</div>";
  h += '<div class="sec"><h3>ALL CLANS</h3>';
  if (!d.clans.length) return h + '<div class="empty">NO CLANS</div></div>';
  h += '<div class="tbl" style="--cols:minmax(110px,1.2fr) 110px 1.3fr 70px 1.1fr 70px 1.2fr 90px"><div class="tr th"><span>OWNER</span><span>FACTION</span><span>WEEK / DAY</span><span>SIZE</span><span>H / U / S</span><span>WEEKS</span><span>STATE</span><span>A-LIFE</span></div>';
  for (const c of d.clans) {
    const pct = c.status === "alive" ? Math.min(100, Math.round(c.day / c.daysTotal * 100)) : 100;
    h += `<div class="tr click" data-go="player/${esc(c.ownerId)}"><span>${esc(c.owner)}</span><span>${facPill(c.factionName)}</span>` +
      `<span>${c.status === "alive" ? `W${c.week} · D${c.dayOfWeek} <span class="faint">${c.day}/${c.daysTotal}</span><div class="bar2 ${facCls(c.factionName)}"><i style="width:${pct}%"></i></div>` : '<span class="faint">-</span>'}</span>` +
      `<span>${c.size}</span><span class="dim"><span class="wlf">${c.healthy}</span> / ${c.hungry} / <span class="bad">${c.sick}</span></span><span class="dim">${c.weeksTotal}</span>` +
      `<span class="${c.risk.length ? "bad" : c.status === "alive" ? "ok" : "dim"}">${esc(c.status.toUpperCase())}${c.risk.length ? " · " + esc(c.risk[0]) : ""}</span>` +
      `<span class="dim">${c.alife && c.alife.threads != null ? c.alife.threads + " running" : "-"}</span></div>`;
  }
  h += "</div></div>";
  h += '<div class="sec"><h3>CLAN FEED</h3>';
  if (!d.feedAvailable) h += '<div class="empty">NO CLAN FEED ON THIS SERVER BUILD</div>';
  else if (!d.feed.length) h += '<div class="empty">NO FEED LINES YET</div>';
  else h += d.feed.map(m => `<div class="feedline"><em>${esc(when(m.ts))} · ${esc(m.owner)} · W${m.week} D${m.day}${m.kind === "alife" ? ' <span class="pill ok">A-LIFE</span>' : ""}</em>${esc(m.text)}</div>`).join("");
  return h + "</div>";
}

function viewReports(d) {
  if (!d.configured) return note("No report store configured. Start the site with --reports <update server data dir>/reports.");
  let h = `<div class="sec"><h3>BUG REPORTS (${d.reports.length}, ${d.new} new)</h3>`;
  if (!d.reports.length) return h + '<div class="empty">NO REPORTS</div></div>';
  h += '<div class="tbl" style="--cols:80px 140px 120px 90px 1fr 80px 60px"><div class="tr th"><span>CODE</span><span>RECEIVED</span><span>PLAYER</span><span>BUILD</span><span>TEXT</span><span>STATE</span><span>FILES</span></div>';
  for (const r of d.reports) {
    h += `<div class="tr click" data-go="report/${esc(r.code)}"><span>${esc(r.code)}</span><span class="dim">${esc((r.received || "").replace("T", " ").slice(0, 16))}</span><span>${esc(r.name)}</span><span class="dim">${esc(r.version)}</span><span class="dim">${esc(r.text)}</span><span>${r.handled ? '<span class="faint">handled</span>' : '<span class="warn">NEW</span>'}</span><span class="dim">${r.files}</span></div>`;
  }
  return h + "</div></div>";
}

function viewReport(r, code) {
  if (!r.ok) return `<button class="back" data-go="reports">&lt; REPORTS</button><div class="empty">${esc(r.error)}</div>`;
  const m = r.meta;
  let h = `<button class="back" data-go="reports">&lt; REPORTS</button><div class="match" style="margin-top:10px"><div class="mh"><b>${esc(m.code)}</b><span>${m.handled ? '<span class="faint">HANDLED</span>' : '<span class="pill warn">NEW</span>'} <button class="btn sm" data-handled="${esc(m.code)}" data-to="${m.handled ? 0 : 1}">${m.handled ? "MARK NEW AGAIN" : "MARK HANDLED"}</button></span></div>`;
  h += '<div class="kvs" style="margin:0">' + kv("RECEIVED", esc((m.received || "").replace("T", " ").replace("Z", " UTC"))) + kv("FROM", esc(m.name || "-") + ` <span class="faint">${esc(m.nameSource || "")}</span>`) +
    kv("STEAM ID", esc(m.steamId || "-")) + kv("BUILD / LAUNCHER", `${esc(m.version || "-")} / ${esc(m.launcherVersion || "-")}`) + kv("WHEN IT HAPPENED", esc([m.when, m.whenText].filter(Boolean).join(" · ") || "-")) +
    kv("MAP / MODE", esc(m.mapMode || "-")) + kv("SYSTEM", esc(m.os || "-")) + kv("GPU", esc(m.gpu || "-")) + kv("DUMPS INCLUDED", m.includeDumps ? "yes" : "no") + "</div></div>";
  h += `<div class="sec"><h3>DESCRIPTION</h3><pre class="txt">${esc(m.text || "")}</pre></div><div class="sec"><h3>FILES (${r.files.length})</h3>`;
  const vids = r.files.filter(f => f.video);
  if (vids.length) h = h.replace(/<div class="sec"><h3>FILES \(/, '<div class="sec"><h3>CLIPS (' + vids.length + ')</h3><div class="clips">' + vids.map(f => { const u = `/api/report-video?code=${encodeURIComponent(m.code)}&name=${encodeURIComponent(f.name)}`; return `<div class="clip"><video controls preload="metadata" src="${u}"></video><span>${esc(f.name)} · ${(f.size / 1048576).toFixed(1)} MB</span></div>`; }).join("") + '</div></div><div class="sec"><h3>FILES (');
  const shots = r.files.filter(f => f.image);
  if (shots.length) h = h.replace(/<div class="sec"><h3>FILES \(/, '<div class="sec"><h3>SCREENSHOTS (' + shots.length + ')</h3><div class="shots">' + shots.map(f => { const u = `/api/report-image?code=${encodeURIComponent(m.code)}&name=${encodeURIComponent(f.name)}`; return `<a class="shot" href="${u}" target="_blank" rel="noopener"><img src="${u}" alt="${esc(f.name)}" loading="lazy"><span>${esc(f.name)} · ${(f.size / 1024).toFixed(0)} KB</span></a>`; }).join("") + '</div></div><div class="sec"><h3>FILES (');
  if (!r.files.length) h += '<div class="empty">NO FILES</div>';
  else {
    h += '<div class="tbl" style="--cols:1fr 100px 90px">';
    for (const f of r.files) h += `<div class="tr"><span>${esc(f.name)}</span><span class="dim">${(f.size / 1024).toFixed(f.size > 102400 ? 0 : 1)} KB</span><span>${f.text ? `<button class="btn sm" data-file="${esc(f.name)}" data-code="${esc(m.code)}">VIEW</button>` : f.video ? `<a class="btn sm" href="/api/report-video?code=${encodeURIComponent(m.code)}&name=${encodeURIComponent(f.name)}" target="_blank" rel="noopener">OPEN</a>` : f.image ? `<a class="btn sm" href="/api/report-image?code=${encodeURIComponent(m.code)}&name=${encodeURIComponent(f.name)}" target="_blank" rel="noopener">OPEN</a>` : '<span class="faint">binary</span>'}</span></div>`;
    h += "</div>";
    const fv = S.fileView[code];
    if (fv) h += `<div class="sec" style="margin-top:12px"><h3>${esc(fv.name)} ${fv.truncated ? "(last 96 KB)" : ""}</h3><pre class="txt" style="max-height:420px">${esc(fv.text)}</pre></div>`;
  }
  return h + "</div>";
}

function hilite(t, q) {
  if (!q) return esc(t);
  const lo = t.toLowerCase(), ql = q.toLowerCase();
  let out = "", i = 0, j;
  while ((j = lo.indexOf(ql, i)) >= 0) { out += esc(t.slice(i, j)) + "<mark>" + esc(t.slice(j, j + q.length)) + "</mark>"; i = j + q.length; }
  return out + esc(t.slice(i));
}
function viewLogs(d) {
  if (!d.ok) return note(d.error, true);
  const lines = d.lines.map(l => `<div class="ll ${l.c}">${hilite(l.t, S.logQ)}</div>`).join("");
  return `<div class="logwrap" id="logwrap"><div class="logbox">${lines || '<div class="empty" style="padding-left:12px">NO LINES MATCH</div>'}</div></div>`;
}

// PATCH NOTES: the launcher's own news page (news.html: launcher news.js + news.css) made editable in place; it loads and saves /api/news itself.
// Steam admins get the website's news editor (rich blocks, stats board, media, EN + PL; same login); token / local users the older sections editor
function notesSrc() { const w = S.who; return w && w.kind === "steam" && w.site ? w.site + "/news.html?embed=1" : "/news.html"; }
function viewNotes() { return `<iframe id="nwframe" class="nwframe" title="patch notes" src="${esc(notesSrc())}"></iframe>`; }
function notesDirty() { const f = $("#nwframe"); try { return !!(f && f.contentWindow.NewsEd && f.contentWindow.NewsEd.dirty()); } catch (e) { return false; } }
window.addEventListener("beforeunload", e => { if (notesDirty()) { e.preventDefault(); e.returnValue = ""; } });

// ---------------------------------------------------------------- routing / polling
function parseRoute() {
  const h = decodeURIComponent(location.hash.replace(/^#\/?/, ""));
  const [tab, ...rest] = h.split("/");
  return { tab: TABS.includes(tab) || tab === "player" || tab === "report" || tab === "match" ? tab : "overview", arg: rest.join("/") };
}
function mainTab(r) { return r.tab === "player" ? "players" : r.tab === "report" ? "reports" : r.tab === "match" ? "matches" : r.tab; }
function go(p) { location.hash = "#" + p; }

function renderToolbar() {
  const r = S.route, t = r.tab;
  let h = "";
  if (t === "players") h = `<input class="tool-in" id="q" placeholder="SEARCH NAME OR STEAM ID" value="${esc(S.q)}" autocomplete="off">`;
  if (t === "chat") {
    h = `<input class="tool-in" id="cq" placeholder="FILTER (TEXT OR NAME)" value="${esc(S.chatQ)}" autocomplete="off">` +
      `<select class="tool-in" id="ck">${[["", "ALL CHANNELS"], ["match", "MATCH"], ["team", "TEAM"], ["party", "PARTY"], ["lobby", "LOBBY"]].map(([v, n]) => `<option value="${v}" ${S.chatKind === v ? "selected" : ""}>${n}</option>`).join("")}</select>` +
      (r.arg ? '<button class="btn sm" data-go="chat">ALL PLAYERS</button>' : "");
  }
  if (t === "logs") {
    h = `<input class="tool-in" id="lq" placeholder="FILTER (TEXT)" value="${esc(S.logQ)}" autocomplete="off">` +
      `<select class="tool-in" id="ll">${[["all", "ALL LINES"], ["evt", "EVENTS"], ["warn", "WARNINGS + ERRORS"], ["err", "ERRORS ONLY"]].map(([v, n]) => `<option value="${v}" ${S.logLevel === v ? "selected" : ""}>${n}</option>`).join("")}</select>` +
      `<select class="tool-in" id="lf"></select><span class="tg ${S.logQuiet ? "on" : ""}" id="lquiet"><i></i>HIDE NOISE</span><span class="tg ${S.logFollow ? "on" : ""}" id="lfollow"><i></i>FOLLOW</span>`;
  }
  const tb = $("#toolbar");
  if (S.last.toolbar !== h + t) { S.last.toolbar = h + t; tb.innerHTML = h; }
  document.querySelectorAll(".mi").forEach(b => b.classList.toggle("active", b.dataset.tab === mainTab(r)));
  $("#ph-title").textContent = t === "player" ? "PLAYER" : t === "report" ? "BUG REPORT " + r.arg : t === "match" ? "MATCH " + r.arg : TITLES[t];
  document.title = (t === "player" ? "Player" : t === "report" ? "Report " + r.arg : t === "match" ? "Match " + r.arg : TITLES[t].charAt(0) + TITLES[t].slice(1).toLowerCase()) + " - Twelve Weeks admin";
}

// The tab's own data (fetched in parallel with the top bar's overview + online, not after them)
function tabUrl(r) {
  switch (r.tab) {
    case "players": return "/api/players?q=" + encodeURIComponent(S.q);
    case "player": return "/api/player?id=" + encodeURIComponent(r.arg);
    case "matches": return "/api/matches";
    case "clans": return "/api/clans";
    case "teams": return "/api/teams";
    case "reports": return "/api/reports";
    case "report": return "/api/report?code=" + encodeURIComponent(r.arg);
    case "match": return "/api/match?id=" + encodeURIComponent(r.arg);
    case "chat": return `/api/chat?q=${encodeURIComponent(S.chatQ)}&kind=${S.chatKind}&id=${encodeURIComponent(r.arg)}&n=300`;
    case "logs": return `/api/logs?q=${encodeURIComponent(S.logQ)}&level=${S.logLevel}&file=${S.logFile}&quiet=${S.logQuiet ? 1 : 0}&n=400`;
    case "overview": return "/api/notices";
  }
  return null;
}
// The top bar's overview + online: one request pair in flight, shared by overlapping refreshes.
function topData() {
  if (!S.ovp) {
    const p = Promise.all([api("/api/overview"), api("/api/online")]);
    S.ovp = p;
    p.then(([ov]) => {
      S.ov = ov;
      paintTop(ov);
      $("#ph-right").textContent = ov.source === "live" ? "LIVE" : ov.source === "snapshot" ? "SNAPSHOT" : "FROM LOG";
      const rp = $("#rep-pip"); rp.hidden = !ov.totals.reportsNew;
    }).catch(() => {}).finally(() => { if (S.ovp === p) S.ovp = null; });
  }
  return S.ovp;
}
async function refresh(force) {
  // A timer poll is skipped while a refresh runs. A forced one (tab switch, filter, action) starts at once and makes every older refresh
  // stale (S.gen): a stale refresh never paints the body. The tab's own data is not held back by the (slow) overview: only the
  // overview and players tabs wait for it. Before, a tab switch during a slow refresh was dropped, the old refresh painted the PREVIOUS
  // tab (chat showed the overview) and the new tab only came with the next poll.
  if (S.busy && !force) return;
  if (force) S.gen++;
  S.busy++;
  const r = S.route, body = $("#body"), gen = S.gen;
  const live = () => gen === S.gen;
  const tu = tabUrl(r), tp = tu ? api(tu) : null;
  if (tp) tp.catch(() => {});
  const top = topData();
  try {
    let ov = null, on = null;
    if (r.tab === "overview" || r.tab === "players") [ov, on] = await top;
    else top.catch(e => { if (live() && !body.textContent.trim()) setHTML(body, "body", note("Cannot reach the admin site's API: " + e.message, true)); });
    const nt = r.tab === "overview" ? await tp.catch(() => null) : null;
    if (!live()) return;
    let html = "";
    if (r.tab === "overview") html = viewOverview(ov, on, nt);
    else if (r.tab === "players") { const all = await tp; html = viewPlayers(on, all); }
    else if (r.tab === "player") html = viewPlayer(await tp);
    else if (r.tab === "matches") html = viewMatches(await tp);
    else if (r.tab === "clans") html = viewClans(await tp);
    else if (r.tab === "teams") {
      const d = await tp;
      if ($("#trules") && d.ok) { const pv = $("#tprev"); if (pv) pv.innerHTML = teamsPreview(d); const lv = $("#tlive"); if (lv) lv.innerHTML = teamsLive(d); html = null; }   // (never re-render the box being typed in)
      else html = viewTeams(d);
    }
    else if (r.tab === "reports") html = viewReports(await tp);
    else if (r.tab === "report") html = viewReport(await tp, r.arg);
    else if (r.tab === "notes") { if ($("#nwframe")) html = null; else { setHTML(body, "body", viewNotes()); html = null; } }      // (never re-render the editor: unsaved edits)
    else if (r.tab === "match") {
      const lw = $("#mchat"), atEnd = !lw || lw.scrollHeight - lw.scrollTop - lw.clientHeight < 40, keep = lw ? lw.scrollTop : 0;
      const md = await tp;
      if (!live()) return;
      // (the say box survives the poll's redraw: its text, AS name, focus and caret are put back)
      const ia = $("#msay-as"), it = $("#msay-text"), fo = document.activeElement && document.activeElement.id;
      const sv = ia && it ? { a: ia.value, t: it.value, s: fo === "msay-text" ? it.selectionStart : fo === "msay-as" ? ia.selectionStart : null } : null;
      setHTML(body, "body", viewMatch(md));
      if (sv && $("#msay-as") && $("#msay-text")) {
        $("#msay-as").value = sv.a; $("#msay-text").value = sv.t;
        const f = fo === "msay-text" || fo === "msay-as" ? $("#" + fo) : null;
        if (f) { f.focus(); if (sv.s != null) f.setSelectionRange(sv.s, sv.s); }
      }
      const nw = $("#mchat");
      if (nw) nw.scrollTop = atEnd || force ? nw.scrollHeight : keep;
      html = null;
    }
    else if (r.tab === "chat") html = viewChat(await tp);
    else if (r.tab === "logs") {
      const d = await tp;
      if (!live()) return;
      const sel = $("#lf");
      if (sel && d.ok && S.last.logfiles !== d.files.join()) { S.last.logfiles = d.files.join(); sel.innerHTML = d.files.map(f => `<option value="${f}" ${f === S.logFile ? "selected" : ""}>${f === "0" ? "CURRENT LOG" : "ROTATED LOG " + f}</option>`).join(""); }
      html = viewLogs(d);
      const lw = $("#logwrap"), atEnd = !lw || lw.scrollHeight - lw.scrollTop - lw.clientHeight < 60, keep = lw ? lw.scrollTop : 0;
      setHTML(body, "body", html);
      const nw = $("#logwrap");
      if (nw) nw.scrollTop = S.logFollow && (atEnd || force) ? nw.scrollHeight : keep;
      html = null;
    }
    if (!live()) return;
    if (html !== null) setHTML(body, "body", html);
  } catch (e) {
    if (live()) setHTML(body, "body", note("Cannot reach the admin site's API: " + e.message, true));
  } finally {
    S.busy--;
  }
}

function paintTop(ov) {
  const o = $("#online");
  const up = ov.server.up;
  o.className = !up ? "down" : ov.source === "log" ? "warn" : "up";
  $("#on-n").textContent = !up ? "-" : ov.online.total == null ? "?" : ov.online.total;
  $("#on-txt").textContent = !up ? "SERVER DOWN" : ov.source === "log" ? "ONLINE (APPROX.)" : "ONLINE";
  $("#ver-txt").textContent = "Server " + (ov.server.version || "-") + (ov.server.uptime_s != null ? " · up " + dur(ov.server.uptime_s) : "");
}

function onRoute() {
  const nxt = parseRoute();
  if (S.route.tab === "notes" && nxt.tab === "notes" && $("#nwframe")) return;
  if (S.route.tab === "notes" && nxt.tab !== "notes" && notesDirty() && !confirm("The patch notes have unsaved changes. Leave and lose them?")) { location.hash = "#notes"; return; }
  S.route = nxt;
  S.gen++;
  $("#body").classList.toggle("flush", nxt.tab === "notes");
  S.last.body = null;
  $("#body").innerHTML = "";
  window.scrollTo(0, 0);
  renderToolbar();
  refresh(true);
}

document.addEventListener("click", async e => {
  const t = e.target;
  const tab = t.closest("[data-tab]");
  if (tab) return go(tab.dataset.tab);
  const mvb = t.closest("[data-move]");
  if (mvb) {
    const name = mvb.dataset.name;
    const ok = await confirmBox("MOVE TO OTHER TEAM", `Move <b>${esc(name)}</b> to the other team?<br>They go back to the menu now; when they press FIND MATCH they rejoin this match on the other team.`, "MOVE " + name.toUpperCase().slice(0, 18), false);
    if (!ok) return;
    const r = await post("/api/teams-move", { pid: mvb.dataset.move });
    flash(r.ok ? `${r.name} moved - tell them to press FIND MATCH to rejoin on the other team` : "Move failed: " + (r.error || "unknown"), !r.ok);
    return refresh(true);
  }
  if (t.closest("#tsave")) {
    const r = await post("/api/teams", { rules: $("#trules").value });
    flash(r.ok ? "Team rules saved - the next match uses them" : "Save failed: " + (r.error || "unknown"), !r.ok);
    if (r.ok && $("#tprev")) $("#tprev").innerHTML = teamsPreview(r);
    return;
  }
  const crs = t.closest("[data-clanreset]");
  if (crs) {
    const name = crs.dataset.name;
    const pr = confirmBox("RESET CLAN", `Wipe the clan of <b>${esc(name)}</b>?<br>Faction, week / day, survivors, boosters, missions, diary and A-Life are removed; the next time they open the game they CHOOSE YOUR SIDE like a new player. The old state is saved to a backup file first. Type the player name to confirm:<br><input id="rc-name" type="text" autocomplete="off" style="margin-top:8px;width:100%"><label style="display:block;margin-top:8px"><input id="rc-names" type="checkbox"> also clear the chosen clan name / tag</label>`, "RESET " + name.toUpperCase().slice(0, 18), true);
    const yes = $("#m-foot [data-m=yes]"), inp = $("#rc-name");
    yes.disabled = true;
    inp.oninput = () => { yes.disabled = inp.value.trim().toLowerCase() !== name.trim().toLowerCase(); };
    setTimeout(() => inp.focus(), 0);
    const names = () => !!($("#rc-names") && $("#rc-names").checked), typed = () => inp.value;
    if (!(await pr)) return;
    const r = await post("/api/clan-reset", { id: crs.dataset.clanreset, confirm: typed(), names: names() });
    flash(r.ok ? `Clan of ${name} reset (backup ${r.backup})` : "Reset failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const crr = t.closest("[data-clanrestore]");
  if (crr) {
    const name = crr.dataset.name;
    const bl = await (await fetch("/api/clan-backups?id=" + encodeURIComponent(crr.dataset.clanrestore))).json();
    const list = (bl.backups || []).slice().reverse();
    if (!list.length) return flash(bl.ok === false ? "Backups unavailable: " + (bl.error || "?") : "No backups for this player.", true);
    const pick = list[0];
    const ok = await confirmBox("RESTORE CLAN BACKUP", `Restore the newest backup of <b>${esc(name)}</b> (${esc(when(pick.ts))}; ${Object.entries(pick.rows).map(([k, v]) => k + " " + v).join(", ")})?<br>What the clan is now is saved to a new backup first. ${list.length > 1 ? list.length + " backups exist; the newest is restored." : ""}`, "RESTORE", false);
    if (!ok) return;
    const r = await post("/api/clan-restore", { id: crr.dataset.clanrestore, file: pick.file });
    flash(r.ok ? `Clan of ${name} restored (${r.restored} rows)` : "Restore failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const ube = t.closest("[data-unban-entry]");
  if (ube) {
    e.stopPropagation();
    const name = ube.dataset.name, entry = ube.dataset.unbanEntry;
    const ok = await confirmBox("UNBAN", `Remove <b>${esc(name)}</b> (${esc(entry)}) from the ban list? They can play again within a few seconds.`, "UNBAN", false);
    if (!ok) return;
    const r = await post("/api/unban-entry", { entry });
    flash(r.ok ? `${name} unbanned` : "Unban failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const ban = t.closest("[data-ban]");
  if (ban) {
    e.stopPropagation();
    const name = ban.dataset.name;
    setTimeout(() => { const i = $("#ban-reason"); if (i) i.focus(); }, 30);
    const ok = await confirmBox("BAN PLAYER", `Ban <b>${esc(name)}</b>?<br>Their Steam id goes on the server's ban list: they cannot play or use the launcher until the ban ends or is lifted. If they are online they are kicked now.` +
      `<label style="display:block;margin-top:8px">For:<select id="ban-dur" style="margin-top:4px;width:100%">${BAN_DUR.map(([m, n]) => `<option value="${m}" ${m === 0 ? "selected" : ""}>${n}</option>`).join("")}</select></label>` +
      `<label style="display:block;margin-top:8px">Reason (shown to them):<input id="ban-reason" type="text" maxlength="160" autocomplete="off" style="margin-top:4px;width:100%"></label>`, "BAN " + name.toUpperCase().slice(0, 18), true);
    if (!ok) return;
    const r = await post("/api/ban", { id: ban.dataset.ban, minutes: parseFloat(($("#ban-dur") || {}).value || "0"), reason: ($("#ban-reason") || {}).value || "" });
    flash(r.ok ? `${r.name || name} banned ${r.until ? "until " + when(r.until) : "for good"}${r.kicked ? " and kicked" : ""}` : "Ban failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const unban = t.closest("[data-unban]");
  if (unban) {
    e.stopPropagation();
    const name = unban.dataset.name;
    const ok = await confirmBox("UNBAN PLAYER", `Remove <b>${esc(name)}</b> from the ban list? They can play again right away.`, "UNBAN", false);
    if (!ok) return;
    const r = await post("/api/unban", { id: unban.dataset.unban });
    flash(r.ok ? `${r.name || name} unbanned` : "Unban failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const mute = t.closest("[data-mute]");
  if (mute) {
    e.stopPropagation();
    const name = mute.dataset.name;
    setTimeout(() => { const i = $("#mute-reason"); if (i) i.focus(); }, 30);
    const ok = await confirmBox("MUTE PLAYER", `Mute <b>${esc(name)}</b> in chat?<br>Their party and lobby lines and friend messages are refused; the game shows them YOU ARE MUTED instead of opening the chat (from the next client update).` +
      `<label style="display:block;margin-top:8px">For:<select id="mute-dur" style="margin-top:4px;width:100%">${MUTE_DUR.map(([m, n]) => `<option value="${m}" ${m === 1440 ? "selected" : ""}>${n}</option>`).join("")}</select></label>` +
      `<label style="display:block;margin-top:8px">Reason (shown to them):<input id="mute-reason" type="text" maxlength="160" autocomplete="off" style="margin-top:4px;width:100%"></label>`, "MUTE " + name.toUpperCase().slice(0, 18), true);
    if (!ok) return;
    const r = await post("/api/mute", { id: mute.dataset.mute, minutes: parseFloat(($("#mute-dur") || {}).value || "1440"), reason: ($("#mute-reason") || {}).value || "" });
    flash(r.ok ? `${r.name || name} muted ${r.until ? "until " + when(r.until) : "for good"}` : "Mute failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const unmute = t.closest("[data-unmute]");
  if (unmute) {
    e.stopPropagation();
    const r = await post("/api/unmute", { id: unmute.dataset.unmute });
    flash(r.ok ? `${r.name || unmute.dataset.name} unmuted` : "Unmute failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const kick = t.closest("[data-kick]");
  if (kick) {
    e.stopPropagation();
    const name = kick.dataset.name;
    setTimeout(() => { const i = $("#kick-reason"); if (i) i.focus(); }, 30);
    const ok = await confirmBox("FORCE KICK", `Kick <b>${esc(name)}</b>?<br>This clears their server-side state (queue, match membership, presence) so they can rejoin, and sends them back to the menu. The other players in their match keep playing.<br><label style="display:block;margin-top:8px">Reason?: (optional)<input id="kick-reason" type="text" maxlength="120" autocomplete="off" style="margin-top:4px;width:100%"></label>`, "KICK " + name.toUpperCase().slice(0, 18), true);
    if (!ok) return;
    const r = await post("/api/kick", { pid: kick.dataset.kick, reason: ($("#kick-reason") || {}).value || "" });
    flash(r.ok ? `${r.name || name} kicked: ${(r.did || []).join(", ") || "nothing to clear"}` : "Kick failed: " + (r.error || "unknown"), !r.ok);
    return refresh(true);
  }
  const msay = t.closest("[data-msay]");
  if (msay) { e.stopPropagation(); return sendSay(msay.dataset.msay); }
  const nsend = t.closest("[data-nsend]");
  if (nsend) {
    const text = $("#nt-text").value.trim();
    if (!text) return flash("Type the notice text first.", true);
    const r = await post("/api/notice", { text, seconds: parseFloat($("#nt-sec").value) || 8 });
    flash(r.ok ? "Notice sent." : "Failed: " + (r.error || "unknown"), !r.ok);
    $("#nt-text").value = ""; S.last.body = null; return refresh(true);
  }
  const nclear = t.closest("[data-nclear]");
  if (nclear) { const r = await post("/api/notice-clear", { id: parseInt(nclear.dataset.nclear, 10) }); if (!r.ok) flash(r.error || "not found", true); S.last.body = null; return refresh(true); }
  const nrs = t.closest("[data-nrestart]");
  if (nrs) {
    const m = parseFloat($("#sd-min").value);
    if (!(m >= 0)) return flash("Minutes?", true);
    const ok = await confirmBox("SCHEDULE RESTART", `Restart the server in <b>${esc(m)}</b> min? Players get a countdown notice (5 / 2 / 1 min); matches still running then end.`, "SCHEDULE", true);
    if (!ok) return;
    const r = await post("/api/shutdown", { minutes: m, reason: $("#sd-why").value, restart: true });
    flash(r.ok ? "Restart scheduled." : "Failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const ncan = t.closest("[data-ncancel]");
  if (ncan) { const r = await post("/api/shutdown-cancel", {}); flash(r.ok ? "Restart cancelled." : "Nothing scheduled.", !r.ok); S.last.body = null; return refresh(true); }
  const go_ = t.closest("[data-go]");
  if (go_ && !t.closest("a, button, input, select")) return go(go_.dataset.go);   // (a link / button inside a clickable row does its own thing)
  const hd = t.closest("[data-handled]");
  if (hd) { const r = await post("/api/report-handled", { code: hd.dataset.handled, handled: hd.dataset.to === "1" }); if (!r.ok) flash(r.error, true); return refresh(true); }
  const fl = t.closest("[data-file]");
  if (fl) { const f = await api(`/api/report-file?code=${encodeURIComponent(fl.dataset.code)}&name=${encodeURIComponent(fl.dataset.file)}`); S.fileView[fl.dataset.code] = f; S.last.body = null; return refresh(true); }
  const tg = t.closest(".tg");
  if (tg) {
    if (tg.id === "lquiet") S.logQuiet = !S.logQuiet; else S.logFollow = !S.logFollow;
    tg.classList.toggle("on"); return refresh(true);
  }
});
document.addEventListener("input", e => {
  if (e.target.id === "q") { S.q = e.target.value; clearTimeout(S.dq); S.dq = setTimeout(() => refresh(true), 200); }
  if (e.target.id === "cq") { S.chatQ = e.target.value; clearTimeout(S.dq); S.dq = setTimeout(() => refresh(true), 250); }
  if (e.target.id === "lq") { S.logQ = e.target.value; clearTimeout(S.dq); S.dq = setTimeout(() => refresh(true), 250); }
});
document.addEventListener("change", e => {
  if (e.target.id === "ck") { S.chatKind = e.target.value; refresh(true); }
  if (e.target.id === "ll") { S.logLevel = e.target.value; refresh(true); }
  if (e.target.id === "lf") { S.logFile = e.target.value; refresh(true); }
});
document.addEventListener("keydown", e => {
  const typing = /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName);
  if (e.key === "Enter" && document.activeElement.id === "msay-text") { e.preventDefault(); const b = $("[data-msay]"); if (b) sendSay(b.dataset.msay); return; }
  if (e.key === "Escape") { if (typing) document.activeElement.blur(); else if (S.route.tab === "player") go("players"); else if (S.route.tab === "report") go("reports"); else if (S.route.tab === "match") go("matches"); return; }
  if (typing || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.key >= "1" && e.key <= "9") go(TABS[+e.key - 1]);
  else if (e.key === "/") { e.preventDefault(); const q = $("#q") || $("#cq") || $("#lq"); if (q) q.focus(); }
  else if (e.key.toLowerCase() === "p") { S.paused = !S.paused; flash(S.paused ? "Auto refresh paused" : "Auto refresh on"); }
  else if (e.key.toLowerCase() === "r") refresh(true);
});
window.addEventListener("hashchange", onRoute);
// who is logged in (Steam admin: name / id + Log out link to the website; token user: name; loopback: nothing to show)
fetch("/api/whoami", { cache: "no-store" }).then(r => r.ok ? r.json() : null).then(w => {
  S.who = w;
  const fr = $("#nwframe");
  if (fr && w && fr.getAttribute("src") !== notesSrc()) fr.setAttribute("src", notesSrc());
  const el = $("#who");
  if (!w || !el || w.kind === "local") return;
  const label = w.kind === "steam" ? `<b>${esc(w.name || "Steam admin")}</b><small>${esc(w.id)}</small>` : `<b>${esc(w.name)}</b>`;
  el.innerHTML = label + (w.logout ? `<a href="${esc(w.logout)}">Log out</a>` : "");
}).catch(() => {});
// LIVE / TEST server switch: the cookie tw_srv=test sends every request (the clan / news frames too) to the test daemon
fetch("/api/server", { cache: "no-store" }).then(r => r.ok ? r.json() : null).then(sv => {
  const b = $("#srvsw");
  if (!sv || !sv.test || !b) return;
  b.hidden = false;
  b.querySelectorAll("span").forEach(x => x.classList.toggle("on", x.dataset.s === sv.server));
  document.body.classList.toggle("srv-test", sv.server === "test");
  const k = $(".phead .kicker");
  if (k && sv.server === "test") k.textContent = "Twelve Weeks admin - TEST SERVER";
  b.addEventListener("click", () => {
    document.cookie = "tw_srv=" + (sv.server === "test" ? "live" : "test") + "; path=/; max-age=31536000; SameSite=Strict" + (location.protocol === "https:" ? "; Secure" : "");
    location.reload();
  });
}).catch(() => {});
onRoute();
S.timer = setInterval(() => { if (!S.paused && !document.hidden) refresh(false); }, POLL_MS);

// phone layout (admin.css max-width 900px): the hamburger opens the sidebar; a tab pick, the shade or Escape closes it
(function () {
  var app = document.getElementById('app'), btn = document.getElementById('navbtn'), shade = document.getElementById('navshade'), menu = document.getElementById('menu');
  if (!app || !btn) return;
  function set(on) { app.classList.toggle('nav-open', on); btn.setAttribute('aria-expanded', on ? 'true' : 'false'); }
  btn.addEventListener('click', function () { set(!app.classList.contains('nav-open')); });
  shade.addEventListener('click', function () { set(false); });
  menu.addEventListener('click', function (e) { if (e.target.closest('.mi')) { set(false); window.scrollTo(0, 0); } });
  document.addEventListener('keydown', function (e) { if (e.key === 'Escape') set(false); });
})();

// the clan iframe (clanframe.js) reports its full height: the frame grows to it, so the admin page is the only scroll
window.addEventListener("message", e => {
  if (e.origin !== location.origin || !e.data || typeof e.data !== "object") return;
  const f = [...document.querySelectorAll(".clanframe")].find(x => x.contentWindow === e.source);
  if (!f) return;
  if (typeof e.data.clanHeight === "number") f.style.height = Math.max(200, Math.min(20000, e.data.clanHeight)) + "px";
  if (e.data.clanTab) { const top = f.getBoundingClientRect().top; if (top < 70) window.scrollBy({ top: top - 80, behavior: "smooth" }); }
});
