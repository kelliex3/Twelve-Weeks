import { useEffect, useState, type CSSProperties, type ReactNode } from "react";
import { useNavigate } from "react-router";
import { useApiUrl, useCustom } from "@refinedev/core";
import { Fact, Facts } from "../../components/facts";
import { Note } from "../../components/note";
import { apiUrl, type NoticesData, type OnlineData, type OnlinePlayer, type OverviewData } from "../../providers/data";
import { dur, facCls, noticeKind, platform, stateLabel, when } from "../../lib/format";
import "./overview.css";

const POLL_MS = 3000;

const KV = ({ label, children }: { label: string; children: ReactNode }) => (
  <div>
    <label>{label}</label>
    <b>{children}</b>
  </div>
);

type Flash = { text: string; bad?: boolean };

type ActionResult = { ok?: boolean; error?: string; [key: string]: unknown };

/** Refine's useCustom yields `{}` until the first response arrives; don't treat that as data. */
function isOverview(value: unknown): value is OverviewData {
  if (!value || typeof value !== "object") return false;
  const data = value as Partial<OverviewData>;
  return Boolean(
    data.server && data.online && data.clients && data.totals &&
    Array.isArray(data.notes) && Array.isArray(data.errors),
  );
}

async function post(path: string, body: unknown): Promise<ActionResult> {
  const r = await fetch(`${apiUrl}${path}`, {
    method: "POST",
    headers: { "Content-Type": "application/json", "X-Admin-Request": "1" },
    body: JSON.stringify(body),
  });
  if (!r.ok) throw new Error(`HTTP ${r.status}`);
  return r.json();
}

export const Overview = () => {
  const navigate = useNavigate();
  const baseUrl = useApiUrl("default");

  const { result: ovResult, query: ovQuery } = useCustom<OverviewData>({
    url: `${baseUrl}/overview`,
    method: "get",
    queryOptions: { refetchInterval: POLL_MS, placeholderData: (prev) => prev },
  });
  const { result: onResult, query: onQuery } = useCustom<OnlineData>({
    url: `${baseUrl}/online`,
    method: "get",
    queryOptions: { refetchInterval: POLL_MS, placeholderData: (prev) => prev },
  });
  const { result: ntResult, query: ntQuery } = useCustom<NoticesData>({
    url: `${baseUrl}/notices`,
    method: "get",
    queryOptions: { refetchInterval: POLL_MS, placeholderData: (prev) => prev },
  });

  const [noticeText, setNoticeText] = useState("");
  const [noticeSecs, setNoticeSecs] = useState(8);
  const [restartMin, setRestartMin] = useState(10);
  const [restartWhy, setRestartWhy] = useState("");
  const [flash, setFlash] = useState<Flash | null>(null);

  const ovData = ovResult?.data;
  const ov = isOverview(ovData) ? ovData : undefined;
  const on = onResult?.data;
  const nt = ntResult?.data;

  useEffect(() => {
    if (!ov) return;
    const box = document.getElementById("online");
    box?.classList.remove("up", "down", "warn");
    box?.classList.add(!ov.server.up ? "down" : ov.source === "log" ? "warn" : "up");
    const n = document.getElementById("on-n");
    if (n) n.textContent = !ov.server.up ? "-" : ov.online.total == null ? "?" : String(ov.online.total);
    const t = document.getElementById("on-txt");
    if (t) t.textContent = !ov.server.up ? "SERVER DOWN" : ov.source === "log" ? "ONLINE (APPROX.)" : "ONLINE";
    const v = document.getElementById("ver-txt");
    if (v) v.textContent = `Server ${ov.server.version || "-"}${ov.server.uptime_s != null ? ` · up ${dur(ov.server.uptime_s)}` : ""}`;
    const src = document.getElementById("ph-right");
    if (src) src.textContent = ov.source === "live" ? "LIVE" : ov.source === "snapshot" ? "SNAPSHOT" : "FROM LOG";
    document.title = "Overview - Twelve Weeks admin";
  }, [ov]);

  const run = async (fn: () => Promise<ActionResult>, okMessage: string) => {
    try {
      const r = await fn();
      const bad = r?.ok === false;
      setFlash({ text: bad ? r.error || "Action failed." : okMessage, bad });
      await Promise.all([ovQuery.refetch(), onQuery.refetch(), ntQuery.refetch()]);
    } catch (e) {
      setFlash({ text: e instanceof Error ? e.message : String(e), bad: true });
    }
  };

  if (ovQuery.isLoading && !ov) return <div className="empty">Loading server status...</div>;
  if (!ov || ovQuery.isError) {
    return <Note bad>Cannot reach the admin site&apos;s API: {(ovQuery.error as Error | null)?.message || "no data"}</Note>;
  }

  const s = ov.server;
  const up = s.up;
  const notices = nt?.notices || [];
  const players: OnlinePlayer[] = (on?.players || []).slice(0, 8);
  const canKick = Boolean(on?.canKick);
  const onlineCols = `minmax(120px,1.3fr) 150px 1fr 80px 90px 90px 80px ${canKick ? "70px" : ""}`;

  const kick = (p: OnlinePlayer) => {
    if (!p.pid || !window.confirm(`Kick ${p.name}?`)) return;
    void run(() => post("/kick", { pid: p.pid, reason: "" }), `Kick sent for ${p.name}.`);
  };

  return (
    <div>
      {ov.notes.map((text) => (
        <Note key={text}>{text}</Note>
      ))}

      <Facts>
        <Fact
          label="SERVER"
          value={up ? "UP" : "DOWN"}
          subtitle={up ? `port ${s.probe.addr}${s.probe.ms != null ? ` · ${s.probe.ms} ms` : ""}` : "game port not answering"}
          className={up ? "ok" : "bad"}
        />
        <Fact label="ONLINE NOW" value={ov.online.total == null ? "?" : ov.online.total} subtitle="players with a live connection" />
        <Fact label="IN MATCH" value={ov.online.match == null ? "-" : ov.online.match} subtitle="playing or loading" />
        <Fact label="SEARCHING" value={ov.online.queue == null ? "-" : ov.online.queue} subtitle="in the queue" />
        <Fact label="MATCHES" value={ov.matchesRunning == null ? "?" : ov.matchesRunning} subtitle="running now" />
      </Facts>

      <div className="cols2">
        <div className="sec">
          <h3>SERVER</h3>
          <div className="kvs">
            <KV label="UPTIME">
              {up && s.uptime_s != null ? (
                <>
                  {dur(s.uptime_s)} <span className="faint">since {when(s.started)}</span>
                </>
              ) : (
                "-"
              )}
            </KV>
            <KV label="SERVER BUILD">{s.version || "-"}</KV>
            <KV label="LATEST PUBLISHED">
              {s.latest ? (
                <>
                  {s.latest}{" "}
                  {s.version && s.latest !== s.version ? (
                    <span className="warn">differs from the server</span>
                  ) : (
                    <span className="ok">same</span>
                  )}
                </>
              ) : (
                <span className="faint">no update data (--update-data)</span>
              )}
            </KV>
            <KV label="CLIENT BUILDS ONLINE">
              {Object.keys(ov.clients.versions).length ? (
                <>
                  {Object.entries(ov.clients.versions).map(([v, n]) => `${v} x${n}`).join(", ")}
                  {ov.clients.outdated ? <span className="warn"> {ov.clients.outdated} outdated</span> : null}
                </>
              ) : (
                <span className="faint">-</span>
              )}
            </KV>
            <KV label="DATA SOURCE">
              {ov.source === "live" ? (
                <span className="ok">LIVE (daemon admin listener)</span>
              ) : ov.source === "snapshot" ? (
                "DAEMON SNAPSHOT (10 s)"
              ) : (
                <span className="warn">DERIVED FROM LOG</span>
              )}
            </KV>
            <KV label="LOG">{s.logPath || "none"}</KV>
          </div>
        </div>

        <div className="sec">
          <h3>DATABASE</h3>
          <div className="kvs">
            <KV label="PLAYERS KNOWN">{ov.totals.players}</KV>
            <KV label="CLANS">{ov.totals.clansAlive} alive of {ov.totals.clans}</KV>
            <KV label="MATCHES LOGGED">
              {ov.totals.matchesLogged} <span className="faint">({ov.totals.matches24h} in the last 24 h)</span>
            </KV>
            <KV label="NEW BUG REPORTS">
              {ov.totals.reportsNew ? <span className="warn">{ov.totals.reportsNew}</span> : "0"}
            </KV>
          </div>
        </div>
      </div>

      <div className="sec">
        <h3>
          SERVER NOTICES <span className="faint">(shown in game, in matches and menus, and in the launcher)</span>
        </h3>
        {nt && nt.ok === false && <Note bad>Notices need the daemon admin listener: {nt.error || "not reachable"}</Note>}
        {notices.length ? (
          <div className="tbl" style={{ "--cols": "190px 1fr 150px 90px" } as CSSProperties}>
            <div className="tr th"><span>KIND</span><span>TEXT</span><span>LEFT</span><span /></div>
            {notices.map((n) => {
              const left = n.kind === "r" && n.endsAt
                ? `in ${dur(Math.max(0, Math.round(n.endsAt - Date.now() / 1000)))}`
                : n.endsAt ? `until ${when(n.endsAt)}` : "";
              return (
                <div className="tr" key={n.id}>
                  <span className="warn">{noticeKind(n.kind)}</span>
                  <span>{n.text || ""}</span>
                  <span className="dim">{left}</span>
                  <span>
                    {n.kind === "t" || n.kind === "r" ? (
                      <button
                        type="button"
                        className="btn sm"
                        onClick={() => void run(() => post("/notice-clear", { id: n.id }), "Notice removed.")}
                      >
                        {n.kind === "r" ? "CANCEL" : "REMOVE"}
                      </button>
                    ) : null}
                  </span>
                </div>
              );
            })}
          </div>
        ) : (
          <div className="empty">NO ACTIVE NOTICE</div>
        )}
        <div className="row-in">
          <input
            className="tool-in grow"
            maxLength={140}
            placeholder="Free text for all players"
            value={noticeText}
            onChange={(e) => setNoticeText(e.target.value)}
          />
          <input
            className="tool-in num"
            value={noticeSecs}
            title="seconds on screen (2-600)"
            type="number"
            min={2}
            max={600}
            onChange={(e) => setNoticeSecs(Number(e.target.value))}
          />
          <span className="dim">s</span>
          <button
            type="button"
            className="btn main"
            disabled={!noticeText}
            onClick={() => void run(() => post("/notice", { text: noticeText, seconds: noticeSecs }), "Notice sent.").then(() => setNoticeText(""))}
          >
            SEND NOTICE
          </button>
        </div>
        <div className="row-in">
          <input
            className="tool-in num"
            value={restartMin}
            title="minutes until the restart"
            type="number"
            min={0}
            max={1440}
            onChange={(e) => setRestartMin(Number(e.target.value))}
          />
          <input
            className="tool-in grow"
            maxLength={100}
            placeholder="Reason (optional)"
            value={restartWhy}
            onChange={(e) => setRestartWhy(e.target.value)}
          />
          <button
            type="button"
            className="btn danger"
            onClick={() => void run(() => post("/shutdown", { minutes: restartMin, reason: restartWhy, restart: true }), "Restart scheduled.")}
          >
            SCHEDULE RESTART
          </button>
          <button
            type="button"
            className="btn"
            onClick={() => void run(() => post("/shutdown-cancel", {}), "Restart cancelled.")}
          >
            CANCEL RESTART
          </button>
        </div>
      </div>

      <div className="sec">
        <h3>WHO&apos;S ONLINE</h3>
        {players.length ? (
          <div className="tbl" style={{ "--cols": onlineCols } as CSSProperties}>
            <div className="tr th">
              <span>NAME</span><span>STATE</span><span>CLAN</span><span>PING</span>
              <span>BUILD</span><span>PLATFORM</span><span>ONLINE</span>
              {canKick ? <span /> : null}
            </div>
            {players.map((p) => (
              <div
                className={`tr ${p.playerId ? "click" : ""}`}
                key={p.playerId || p.pid || p.name}
                onClick={() => p.playerId && navigate(`/players/${p.playerId}`)}
              >
                <span>{p.tag ? `[${p.tag}] ` : ""}{p.name}</span>
                <span>
                  <span className={`pill ${p.state}`}>{stateLabel(p.state)}</span>
                  {p.state_s != null && p.state !== "menu" ? <span className="faint"> {dur(p.state_s)}</span> : null}
                </span>
                <span>
                  {p.clan ? (
                    <>
                      <span className={facCls(p.clan.factionName)}>{p.clan.factionName}</span>{" "}
                      <span className="dim">W{p.clan.week} D{p.clan.day} · {p.clan.size}</span>
                    </>
                  ) : (
                    <span className="faint">no clan</span>
                  )}
                </span>
                <span className="dim">{p.ping != null ? `${p.ping} ms` : "-"}</span>
                <span className={p.outdated ? "warn" : "dim"}>{p.version || "-"}</span>
                <span className="dim">{platform(p.platform)}</span>
                <span className="dim">{p.online_s != null ? dur(p.online_s) : "-"}</span>
                {canKick ? (
                  <span>
                    {p.pid ? (
                      <button type="button" className="btn sm danger" onClick={(e) => { e.stopPropagation(); kick(p); }}>
                        KICK
                      </button>
                    ) : null}
                  </span>
                ) : null}
              </div>
            ))}
          </div>
        ) : (
          <div className="empty">NOBODY ONLINE RIGHT NOW</div>
        )}
      </div>

      <div className="sec">
        <h3>RECENT ERRORS</h3>
        {ov.errors.length ? (
          ov.errors.map((e, i) => (
            <div className="ll err logbox" key={`${e.ts}-${i}`}>{when(e.ts)}  {e.line}</div>
          ))
        ) : (
          <div className="empty">NO ERRORS IN THE LOG</div>
        )}
      </div>

      {flash ? <div className={`flash ${flash.bad ? "bad" : ""}`}>{flash.text}</div> : null}
    </div>
  );
};
