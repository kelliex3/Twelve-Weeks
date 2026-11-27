/** Formatting helpers ported verbatim from tools/admin_site/static/admin.js. */

export function dur(s: number | null | undefined): string {
  if (s == null) return "-";
  const v = Math.max(0, Math.round(s));
  const d = Math.floor(v / 86400);
  const h = Math.floor((v % 86400) / 3600);
  const m = Math.floor((v % 3600) / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${String(m).padStart(2, "0")}m` : m ? `${m}m ${String(v % 60).padStart(2, "0")}s` : `${v}s`;
}

export function when(ts: number | null | undefined): string {
  if (!ts) return "-";
  const d = new Date(ts * 1000);
  const same = d.toDateString() === new Date().toDateString();
  const t = d.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
  return same ? t : `${d.toLocaleDateString([], { month: "short", day: "numeric" })} ${t}`;
}

export function facCls(faction?: string): string {
  return faction === "WLF" ? "wlf" : faction === "SERAPHITES" ? "sera" : "";
}

export const STATE_LABELS: Record<string, string> = {
  match: "IN MATCH",
  queue: "SEARCHING",
  menu: "MENU",
};

export function stateLabel(state: string): string {
  return STATE_LABELS[state] || state.toUpperCase();
}

export function platform(p: string): string {
  return p ? (p === "linux" ? "LINUX" : "WINDOWS") : "-";
}

export const NOTICE_KINDS: Record<string, string> = {
  u: "UPDATE AVAILABLE",
  r: "PLANNED RESTART",
  s: "SHUTTING DOWN",
  t: "NOTICE",
};

export function noticeKind(kind: string): string {
  return NOTICE_KINDS[kind] || kind;
}
