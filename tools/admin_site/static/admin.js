"use strict";
// Twelve Weeks admin: single page, polls the local API every 3 s; a view is only re-rendered when its HTML changed (no flicker).
const $ = (s, r = document) => r.querySelector(s);
const esc = s => String(s == null ? "" : s).replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
const TABS = ["overview", "players", "matches", "teams", "clans", "reports", "logs", "notes"];
const TITLES = { overview: "OVERVIEW", players: "PLAYERS", matches: "MATCHES", teams: "TEAMS", clans: "CLANS", reports: "REPORTS", logs: "LOGS", notes: "PATCH NOTES" };
const POLL_MS = 3000;
const S = { route: { tab: "overview", arg: "" }, paused: false, q: "", logQ: "", logLevel: "all", logFile: "0", logQuiet: true, logFollow: true, last: {}, ov: null, timer: null, busy: false, fileView: {} };

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
  if (!r.ok && r.status !== 404) throw new Error("HTTP " + r.status);
  return r.json();
}
async function post(path, body) {
  const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json", "X-Admin-Request": "1" }, body: JSON.stringify(body) });
  return r.json();
}
function setHTML(el, key, html) {
  if (S.last[key] === html) return;
  S.last[key] = html;
  const keep = el.scrollTop;
  el.innerHTML = html;
  el.scrollTop = keep;
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
  h += list.length ? '<div class="tbl" style="--cols:130px 1fr 150px 80px">' + list.map(n => {
    const left = n.kind === "r" && n.endsAt ? "in " + dur(Math.max(0, Math.round(n.endsAt - Date.now() / 1000))) : n.endsAt ? "until " + when(n.endsAt) : "";
    return `<div class="tr"><span class="warn">${esc(NK[n.kind] || n.kind)}</span><span>${esc(n.text || "")}${n.text_pl ? ' <span class="dim">/ PL: ' + esc(n.text_pl) + "</span>" : ""}</span><span class="dim">${esc(left)}</span><span>${n.kind === "t" || n.kind === "r" ? `<button class="btn sm" data-nclear="${esc(n.id)}">${n.kind === "r" ? "CANCEL" : "REMOVE"}</button>` : ""}</span></div>`;
  }).join("") + "</div>" : '<div class="empty">NO ACTIVE NOTICE</div>';
  h += '<div class="toolbar" style="margin-top:10px"><input class="tool-in" id="nt-text" maxlength="140" placeholder="Free text for all players (English / default)" style="flex:1;min-width:240px">' +
    '<input class="tool-in" id="nt-text-pl" maxlength="140" placeholder="Polish version (optional - Polish players see this one)" style="flex:1;min-width:240px">' +
    '<input class="tool-in" id="nt-sec" value="8" style="width:60px" title="seconds on screen (2-600)"><span class="dim" style="align-self:center">s</span><button class="btn" data-nsend="1">SEND NOTICE</button></div>';
  h += '<div class="toolbar" style="margin-top:8px"><input class="tool-in" id="sd-min" value="10" style="width:60px" title="minutes until the restart"><input class="tool-in" id="sd-why" maxlength="100" placeholder="Reason (optional)" style="flex:1;min-width:200px"><input class="tool-in" id="sd-why-pl" maxlength="100" placeholder="Polish reason (optional)" style="flex:1;min-width:200px">' +
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

function viewPlayers(on, all) {
  let h = on.source === "log" ? note("Live feed not active: names come from the log, session data is approximate. Kick and ping need the daemon build with the admin listener (daemon restart).") : "";
  h += `<div class="sec"><h3>ONLINE NOW (${on.players.length})</h3>${onlineTable(on)}</div>`;
  h += `<div class="sec"><h3>ALL PLAYERS (${all.shown}${S.q ? " matching" : ""})${all.hidden ? ` <span class="faint">${all.hidden} never-played installs hidden - search to find them</span>` : ""}</h3>`;
  if (!all.players.length) return h + '<div class="empty">NO PLAYER FOUND</div></div>';
  h += '<div class="tbl" style="--cols:minmax(120px,1.4fr) 120px 1fr 70px 70px 70px 120px 90px"><div class="tr th"><span>NAME</span><span>STEAM ID</span><span>CLAN</span><span>MATCHES</span><span>WINS</span><span>LOSSES</span><span>LAST SEEN</span><span>BUILD</span></div>';
  for (const p of all.players) {
    h += `<div class="tr click" data-go="player/${esc(p.id)}"><span>${p.online ? '<span class="ok">● </span>' : ""}${esc(p.name)}</span><span class="dim">${p.steamid || "-"}</span>` +
      `<span>${p.faction ? `<span class="${facCls(p.faction)}">${esc(p.faction)}</span> <span class="dim">${p.clanStatus === "alive" ? `W${p.week} D${p.day} · ${p.size}` : esc(p.clanStatus)}</span>` : '<span class="faint">no clan</span>'}</span>` +
      `<span>${p.matches}</span><span>${p.wins}</span><span>${p.losses}</span><span class="dim">${p.online ? "now" : ago(p.lastSeen)}</span><span class="dim">${esc(p.version || "-")}</span></div>`;
  }
  return h + "</div></div>";
}

function clanBlock(c, morale) {
  if (!c) return '<div class="empty">NO CLAN YET</div>';
  const pct = c.status === "alive" ? Math.min(100, Math.round(c.day / c.daysTotal * 100)) : 100;
  const tot = Math.max(1, c.size);
  return `<div class="kvs">` + kv("FACTION", facPill(c.factionName)) + kv("STATUS", esc(c.status.toUpperCase()) + (c.risk.length ? ` <span class="bad">AT RISK: ${esc(c.risk.join(", "))}</span>` : "")) +
    kv("PROGRESS", c.status === "alive" ? `WEEK ${c.week} OF ${c.weeks} · DAY ${c.dayOfWeek} <span class="faint">(${c.day} / ${c.daysTotal} days)</span><div class="bar ${facCls(c.factionName)}"><i style="width:${pct}%"></i></div>` : "-") +
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
  h += `<div class="match" style="margin-top:10px"><div class="mh"><b>${esc(p.name)}</b><span>${p.online ? statePill(p.online.state) : '<span class="faint">OFFLINE</span>'} ${p.clan ? facPill(p.clan.factionName) : ""} ${p.canKick ? `<button class="btn sm danger" data-kick="${esc(p.online.pid)}" data-name="${esc(p.name)}">FORCE KICK</button>` : ""}</span></div>`;
  h += '<div class="facts" style="margin:0">' + fact("MATCHES", p.matches) + fact("WINS", p.wins, wr + " win rate", "") + fact("LOSSES", p.losses) + fact("DRAWS", p.draws) +
    (p.avgParts != null ? fact("AVG PARTS", p.avgParts, "per match") : "") + (p.kills || p.deaths ? fact("K / D", p.kills + " / " + p.deaths) : "") + "</div></div>";
  h += '<div class="cols2"><div class="sec"><h3>ACCOUNT</h3><div class="kvs">' +
    kv("STEAM ID", p.steamid || "-") + kv("INSTALLS", p.installs) + kv("LAST SEEN", p.online ? '<span class="ok">online now</span>' : `${esc(when(p.lastSeen))} <span class="faint">${esc(ago(p.lastSeen))}</span>`) +
    kv("CLIENT BUILD", esc(p.version || "-")) + kv("PLATFORM", platform(p.platform)) + kv("ALSO KNOWN AS", p.aliases.length ? esc(p.aliases.join(", ")) : "-") +
    (p.matches === 0 ? "" : "") + "</div>" +
    (p.kills || p.deaths ? "" : '<div class="faint" style="font-size:11.5px;margin-top:8px;line-height:1.5">Kills, deaths and executions are not recorded by the server (the engine keeps them in the match).</div>') +
    '</div><div class="sec"><h3>CLAN</h3>' + clanBlock(p.clan, p.morale) + "</div></div>";
  if (p.clan && p.clan.status) h += `<div class="sec"><h3>CLAN, AS THE PLAYER SEES IT <span class="faint">(the launcher's CLAN tab, read-only; EN / PL button inside)</span></h3><iframe class="clanframe" title="clan" src="/clan.html?id=${encodeURIComponent(p.id)}"></iframe></div>`;
  if (p.feed.length) {
    h += '<div class="sec"><h3>CLAN FEED <span class="faint">(what the launcher journal shows; PL = the Polish text)</span></h3>' + p.feed.map(m => `<div class="feedline"><em>W${m.week} D${m.day}${m.kind === "alife" ? ' <span class="pill ok">A-LIFE</span>' : ""}</em>${esc(m.text)}${m.textPl ? `<div class="faint pl">PL: ${esc(m.textPl)}</div>` : ""}</div>`).join("") + "</div>";
  }
  h += viewAlife(p.alife);
  h += `<div class="sec"><h3>CLAN RESET <span class="faint">(wipes this player's clan to a fresh start; the old state is saved so it can be restored)</span></h3><div class="row" style="gap:8px;display:flex;flex-wrap:wrap"><button class="btn sm danger" data-clanreset="${esc(p.id)}" data-name="${esc(p.name)}">RESET CLAN</button><button class="btn sm" data-clanrestore="${esc(p.id)}" data-name="${esc(p.name)}">RESTORE A BACKUP</button></div></div>`;
  h += '<div class="sec"><h3>MATCH HISTORY <span class="faint">(from the retained daemon logs)</span></h3>';
  if (!p.history.length) h += '<div class="empty">NO MATCHES IN THE RETAINED LOGS' + (p.matches ? ` <span class="faint">(the server counted ${p.matches} match${p.matches === 1 ? "" : "es"} in total; older ones are in logs that were rotated away${p.matchLogFrom ? ", the oldest line kept is " + esc(when(p.matchLogFrom)) : ""})</span>` : "") + "</div>";
  else {
    h += '<div class="tbl" style="--cols:130px 1fr 90px 90px 1fr"><div class="tr th"><span>WHEN</span><span>MAP</span><span>RESULT</span><span>LENGTH</span><span>NOTE</span></div>';
    for (const m of p.history) {
      const r = { W: '<span class="ok">WIN</span>', L: '<span class="bad">LOSS</span>', D: "DRAW" }[m.result] || '<span class="faint">-</span>';
      h += `<div class="tr"><span class="dim">${esc(when(m.when))}</span><span>${esc(m.map || "?")} <span class="faint">${m.players} players</span></span><span>${r}</span><span class="dim">${m.secs ? dur(m.secs) : "-"}</span><span class="warn">${m.left ? "left early / timed out" : m.state !== "ended" ? "no result" : ""}</span></div>`;
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
    h += `<div class="match"><div class="mh"><b>${esc(m.map ? m.map.name : "Map not chosen yet")}</b><span><span class="pill ${m.state === "running" ? "match" : "queue"}">${m.state === "running" ? "RUNNING" : "LOADING"}</span> ${esc(m.mode ? m.mode.toUpperCase() + " · " : "")}${esc(m.net === "steam-p2p" ? "STEAM P2P" : m.net === "server-relay" ? "SERVER RELAY" : "")} · ${dur(m.age_s)} · ID ${esc(m.id)}</span></div>`;
    h += `<div class="team">${teamBlock("TEAM 1", t0, "wlf")}${teamBlock("TEAM 2", t1, "sera")}</div>`;
    h += `<div class="kvs" style="margin:10px 0 0">` + kv("TIME LEFT", left != null ? left : '<span class="faint">no clock report yet</span>') +
      kv("REINFORCEMENTS", m.lives ? `${m.lives.team0} : ${m.lives.team1} <span class="faint">team 1 : team 2</span>` : '<span class="faint">not reported</span>') + "</div></div>";
  }
  h += "</div>" + `<div class="sec"><h3>RECENT MATCHES${d.approx ? "" : ""}</h3>`;
  if (!d.history.length) return h + '<div class="empty">NO MATCH IN THE LOG</div></div>';
  h += '<div class="tbl" style="--cols:120px 1fr 1.6fr 190px 80px 110px"><div class="tr th"><span>WHEN</span><span>MAP</span><span>TEAMS</span><span>RESULT</span><span>LENGTH</span><span>NET</span></div>';
  for (const m of d.history) {
    h += `<div class="tr"><span class="dim">${esc(when(m.started || m.formed))}</span><span>${esc(m.map || "?")}</span>` +
      `<span class="dim" title="${esc(m.team1.join(", ") + " vs " + m.team2.join(", "))}"><span class="wlf">${esc(m.team1.join(", ") || "-")}</span> vs <span class="sera">${esc(m.team2.join(", ") || "-")}</span>${m.left.length ? ` <span class="warn">· ${esc(m.left.join(", "))}</span>` : ""}</span>` +
      `<span class="${m.state === "ended" ? (m.winner === 2 ? "" : "ok") : "warn"}">${esc(m.result)}</span><span class="dim">${m.secs ? dur(m.secs) : "-"}</span><span class="dim">${m.net === "steam-p2p" ? "P2P" : m.net === "server-relay" ? "RELAY" : "-"}${m.ping_ms != null ? " " + m.ping_ms + "ms" : ""}</span></div>`;
  }
  return h + "</div></div>";
}

// TEAMS: the daemon's team rules, applied at every match
// formation (and the searching screen's [T1] / [T2] labels): "apart A B ..." never on one team, "together A B ..." same team.
function teamsLive(d) {
  const live = d.live || [];
  if (!live.length) return '<div class="empty">NO MATCH RUNNING</div>';
  return live.map(m => {
    const col = (n, ps) => `<div class="sec"><h3>${n}</h3>${ps.length ? ps.map(p => `<div class="tr"><span>${esc(p.name)} <span class="faint">slot ${p.slot}</span></span>` +
      (p.slot === 0 && !p.inmatch ? '<span class="faint" title="its game is too old to switch teams in the match; a host cannot be kicked to rejoin">HOST</span>' : `<span>${p.slot === 0 ? '<span class="faint">HOST </span>' : ""}${p.swapped ? '<span class="faint">MOVED </span>' : ""}<button class="tool-in" data-move="${esc(p.pid)}" data-name="${esc(p.name)}" data-inmatch="${p.inmatch ? 1 : 0}">MOVE TO OTHER TEAM</button></span>`) + "</div>").join("") : '<div class="empty">-</div>'}</div>`;
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
    `<b>apart PlayerA PlayerB</b> = never on the same team &nbsp;·&nbsp; <b>together PlayerC PlayerD</b> = same team &nbsp;·&nbsp; # = comment.<br>` +
    `Applies to the next match that forms (a running match keeps its teams). top_players.txt players are also kept apart.</div>` +
    `<textarea id="trules" class="tool-in" rows="8" style="width:100%;font-family:monospace">${esc(d.rules)}</textarea>` +
    `<div style="margin-top:8px"><button class="tool-in" id="tsave">SAVE RULES</button></div></div>` +
    `<div class="sec"><h3>NEXT SPLIT (QUEUE NOW)</h3><div id="tprev">${teamsPreview(d)}</div></div>` +
    `<div class="sec"><h3>RUNNING MATCHES</h3><div class="dim">MOVE switches the player's team inside the running match (same life, new side on every screen). A player on an older game version is sent to the menu instead and rejoins on the other team by itself.</div><div id="tlive">${teamsLive(d)}</div></div>`;
}
function viewClans(d) {
  let h = '<div class="facts">' + fact("CLANS ALIVE", d.alive) + fact("WLF", d.byFaction.WLF, "alive clans") + fact("SERAPHITES", d.byFaction.SERAPHITES, "alive clans") + fact("AT RISK", d.atRisk, "dying or struggling", d.atRisk ? "warn" : "") + "</div>";
  h += '<div class="sec"><h3>ALL CLANS</h3>';
  if (!d.clans.length) return h + '<div class="empty">NO CLANS</div></div>';
  h += '<div class="tbl" style="--cols:minmax(110px,1.2fr) 110px 1.3fr 70px 1.1fr 70px 1.2fr 90px"><div class="tr th"><span>OWNER</span><span>FACTION</span><span>WEEK / DAY</span><span>SIZE</span><span>H / U / S</span><span>WEEKS</span><span>STATE</span><span>A-LIFE</span></div>';
  for (const c of d.clans) {
    const pct = c.status === "alive" ? Math.min(100, Math.round(c.day / c.daysTotal * 100)) : 100;
    h += `<div class="tr click" data-go="player/${esc(c.ownerId)}"><span>${esc(c.owner)}</span><span>${facPill(c.factionName)}</span>` +
      `<span>${c.status === "alive" ? `W${c.week} · D${c.dayOfWeek} <span class="faint">${c.day}/${c.daysTotal}</span><div class="bar ${facCls(c.factionName)}"><i style="width:${pct}%"></i></div>` : '<span class="faint">-</span>'}</span>` +
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
  return `<div class="logbox">${lines || '<div class="empty">NO LINES MATCH</div>'}</div>`;
}

// PATCH NOTES: the launcher's own news page (news.html: launcher news.js + news.css) made editable in place; it loads and saves /api/news itself.
function viewNotes() { return '<iframe id="nwframe" class="nwframe" title="patch notes" src="/news.html"></iframe>'; }
function notesDirty() { const f = $("#nwframe"); try { return !!(f && f.contentWindow.NewsEd && f.contentWindow.NewsEd.dirty()); } catch (e) { return false; } }
window.addEventListener("beforeunload", e => { if (notesDirty()) { e.preventDefault(); e.returnValue = ""; } });

// ---------------------------------------------------------------- routing / polling
function parseRoute() {
  const h = decodeURIComponent(location.hash.replace(/^#\/?/, ""));
  const [tab, ...rest] = h.split("/");
  return { tab: TABS.includes(tab) || tab === "player" || tab === "report" ? tab : "overview", arg: rest.join("/") };
}
function mainTab(r) { return r.tab === "player" ? "players" : r.tab === "report" ? "reports" : r.tab; }
function go(p) { location.hash = "#" + p; }

function renderToolbar() {
  const r = S.route, t = r.tab;
  let h = "";
  if (t === "players") h = `<input class="tool-in" id="q" placeholder="SEARCH NAME OR STEAM ID" value="${esc(S.q)}" autocomplete="off">`;
  if (t === "logs") {
    h = `<input class="tool-in" id="lq" placeholder="FILTER (TEXT)" value="${esc(S.logQ)}" autocomplete="off">` +
      `<select class="tool-in" id="ll">${[["all", "ALL LINES"], ["evt", "EVENTS"], ["warn", "WARNINGS + ERRORS"], ["err", "ERRORS ONLY"]].map(([v, n]) => `<option value="${v}" ${S.logLevel === v ? "selected" : ""}>${n}</option>`).join("")}</select>` +
      `<select class="tool-in" id="lf"></select><span class="tg ${S.logQuiet ? "on" : ""}" id="lquiet"><i></i>HIDE NOISE</span><span class="tg ${S.logFollow ? "on" : ""}" id="lfollow"><i></i>FOLLOW</span>`;
  }
  const tb = $("#toolbar");
  if (S.last.toolbar !== h + t) { S.last.toolbar = h + t; tb.innerHTML = h; }
  document.querySelectorAll(".mi").forEach(b => b.classList.toggle("active", b.dataset.tab === mainTab(r)));
  $("#ph-title").textContent = t === "player" ? "PLAYER" : t === "report" ? "BUG REPORT " + r.arg : TITLES[t];
}

async function refresh(force) {
  if (S.busy) return;
  S.busy = true;
  const r = S.route, body = $("#body");
  try {
    const [ov, on] = await Promise.all([api("/api/overview"), api("/api/online")]);
    const nt = r.tab === "overview" ? await api("/api/notices").catch(() => null) : null;
    S.ov = ov;
    paintTop(ov);
    let html = "";
    if (r.tab === "overview") html = viewOverview(ov, on, nt);
    else if (r.tab === "players") { const all = await api("/api/players?q=" + encodeURIComponent(S.q)); html = viewPlayers(on, all); }
    else if (r.tab === "player") html = viewPlayer(await api("/api/player?id=" + encodeURIComponent(r.arg)));
    else if (r.tab === "matches") html = viewMatches(await api("/api/matches"));
    else if (r.tab === "clans") html = viewClans(await api("/api/clans"));
    else if (r.tab === "teams") {
      const d = await api("/api/teams");
      if ($("#trules") && d.ok) { const pv = $("#tprev"); if (pv) pv.innerHTML = teamsPreview(d); const lv = $("#tlive"); if (lv) lv.innerHTML = teamsLive(d); html = null; }   // (never re-render the box being typed in)
      else html = viewTeams(d);
    }
    else if (r.tab === "reports") html = viewReports(await api("/api/reports"));
    else if (r.tab === "report") html = viewReport(await api("/api/report?code=" + encodeURIComponent(r.arg)), r.arg);
    else if (r.tab === "notes") { if ($("#nwframe")) html = null; else { setHTML(body, "body", viewNotes()); html = null; } }      // (never re-render the editor: unsaved edits)
    else if (r.tab === "logs") {
      const d = await api(`/api/logs?q=${encodeURIComponent(S.logQ)}&level=${S.logLevel}&file=${S.logFile}&quiet=${S.logQuiet ? 1 : 0}&n=400`);
      const sel = $("#lf");
      if (sel && d.ok && S.last.logfiles !== d.files.join()) { S.last.logfiles = d.files.join(); sel.innerHTML = d.files.map(f => `<option value="${f}" ${f === S.logFile ? "selected" : ""}>${f === "0" ? "CURRENT LOG" : "ROTATED LOG " + f}</option>`).join(""); }
      html = viewLogs(d);
      const atEnd = body.scrollHeight - body.scrollTop - body.clientHeight < 60;
      setHTML(body, "body", html);
      if (S.logFollow && (atEnd || force)) body.scrollTop = body.scrollHeight;
      html = null;
    }
    if (html !== null) setHTML(body, "body", html);
    $("#ph-right").textContent = ov.source === "live" ? "LIVE" : ov.source === "snapshot" ? "SNAPSHOT" : "FROM LOG";
    const rp = $("#rep-pip"); rp.hidden = !ov.totals.reportsNew;
  } catch (e) {
    setHTML(body, "body", note("Cannot reach the admin site's API: " + e.message, true));
  } finally { S.busy = false; }
}

function paintTop(ov) {
  const o = $("#online");
  const up = ov.server.up;
  o.className = !up ? "down" : ov.source === "log" ? "warn" : "up";
  $("#on-n").textContent = !up ? "-" : ov.online.total == null ? "?" : ov.online.total;
  $("#on-txt").textContent = !up ? "SERVER DOWN" : ov.source === "log" ? "ONLINE (APPROX.)" : "ONLINE";
  $("#ver-txt").textContent = "SERVER " + (ov.server.version || "-").toUpperCase() + (ov.server.uptime_s != null ? " · UP " + dur(ov.server.uptime_s).toUpperCase() : "");
}

function onRoute() {
  const nxt = parseRoute();
  if (S.route.tab === "notes" && nxt.tab === "notes" && $("#nwframe")) return;
  if (S.route.tab === "notes" && nxt.tab !== "notes" && notesDirty() && !confirm("The patch notes have unsaved changes. Leave and lose them?")) { location.hash = "#notes"; return; }
  S.route = nxt;
  $("#body").classList.toggle("flush", nxt.tab === "notes");
  S.last.body = null;
  $("#body").innerHTML = "";
  $("#body").scrollTop = 0;
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
    const inm = mvb.dataset.inmatch === "1";
    const ok = await confirmBox("MOVE TO OTHER TEAM", `Move <b>${esc(name)}</b> to the other team?<br>` + (inm ? "They stay in the match and keep their current life; their team changes on every screen within a few seconds and they respawn on the new side." : "Their game is an older version: they go back to the menu now and rejoin this match on the other team by themselves."), "MOVE " + name.toUpperCase().slice(0, 18), false);
    if (!ok) return;
    const r = await post("/api/teams-move", { pid: mvb.dataset.move });
    flash(r.ok ? `${r.name} moved - ${r.note || "rejoins on the other team"}` : "Move failed: " + (r.error || "unknown"), !r.ok);
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
  const nsend = t.closest("[data-nsend]");
  if (nsend) {
    const text = $("#nt-text").value.trim(), text_pl = $("#nt-text-pl").value.trim();
    if (!text && !text_pl) return flash("Type the notice text first.", true);
    const r = await post("/api/notice", { text, text_pl, seconds: parseFloat($("#nt-sec").value) || 8 });
    flash(r.ok ? "Notice sent." : "Failed: " + (r.error || "unknown"), !r.ok);
    $("#nt-text").value = ""; $("#nt-text-pl").value = ""; S.last.body = null; return refresh(true);
  }
  const nclear = t.closest("[data-nclear]");
  if (nclear) { const r = await post("/api/notice-clear", { id: parseInt(nclear.dataset.nclear, 10) }); if (!r.ok) flash(r.error || "not found", true); S.last.body = null; return refresh(true); }
  const nrs = t.closest("[data-nrestart]");
  if (nrs) {
    const m = parseFloat($("#sd-min").value);
    if (!(m >= 0)) return flash("Minutes?", true);
    const ok = await confirmBox("SCHEDULE RESTART", `Restart the server in <b>${esc(m)}</b> min? Players get a countdown notice (5 / 2 / 1 min); matches still running then end.`, "SCHEDULE", true);
    if (!ok) return;
    const r = await post("/api/shutdown", { minutes: m, reason: $("#sd-why").value, reason_pl: $("#sd-why-pl").value, restart: true });
    flash(r.ok ? "Restart scheduled." : "Failed: " + (r.error || "unknown"), !r.ok);
    S.last.body = null; return refresh(true);
  }
  const ncan = t.closest("[data-ncancel]");
  if (ncan) { const r = await post("/api/shutdown-cancel", {}); flash(r.ok ? "Restart cancelled." : "Nothing scheduled.", !r.ok); S.last.body = null; return refresh(true); }
  const go_ = t.closest("[data-go]");
  if (go_) return go(go_.dataset.go);
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
  if (e.target.id === "lq") { S.logQ = e.target.value; clearTimeout(S.dq); S.dq = setTimeout(() => refresh(true), 250); }
});
document.addEventListener("change", e => {
  if (e.target.id === "ll") { S.logLevel = e.target.value; refresh(true); }
  if (e.target.id === "lf") { S.logFile = e.target.value; refresh(true); }
});
document.addEventListener("keydown", e => {
  const typing = /INPUT|SELECT|TEXTAREA/.test(document.activeElement.tagName);
  if (e.key === "Escape") { if (typing) document.activeElement.blur(); else if (S.route.tab === "player") go("players"); else if (S.route.tab === "report") go("reports"); return; }
  if (typing || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.key >= "1" && e.key <= "8") go(TABS[+e.key - 1]);
  else if (e.key === "/") { e.preventDefault(); const q = $("#q") || $("#lq"); if (q) q.focus(); }
  else if (e.key.toLowerCase() === "p") { S.paused = !S.paused; flash(S.paused ? "Auto refresh paused" : "Auto refresh on"); }
  else if (e.key.toLowerCase() === "r") refresh(true);
});
window.addEventListener("hashchange", onRoute);
const bgs = ["seattle-falls", "seattle-overgrown", "stadium-stands", "wlf-hq", "haven", "haven-village"];
$("#bg").style.backgroundImage = `url(/bg/${bgs[Math.floor(Date.now() / 3600000) % bgs.length]}.jpg)`;
onRoute();
S.timer = setInterval(() => { if (!S.paused && !document.hidden) refresh(false); }, POLL_MS);
