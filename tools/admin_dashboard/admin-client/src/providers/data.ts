import dataProvider from "@refinedev/simple-rest";

/**
 * JSON contracts mirrored from tools/admin_site (store.Admin.overview / players_online /
 * notices). Keeping the exact shapes means the Refine Overview renders the same data as the
 * classic `viewOverview(ov, on, nt)` with no translation layer.
 */

export interface ServerProbe {
  open: boolean;
  ms: number | null;
  addr: string;
}

export interface OverviewError {
  ts: number;
  line: string;
}

export interface OverviewData {
  now: number;
  source: "live" | "snapshot" | "snapshot-counts" | "log" | string;
  snapshotAge: number | null;
  notes: string[];
  server: {
    up: boolean;
    probe: ServerProbe;
    version: string;
    latest: string;
    uptime_s: number | null;
    started: number | null;
    logFresh: boolean;
    logPath: string;
  };
  online: {
    total: number | null;
    menu: number | null;
    queue: number | null;
    match: number | null;
  };
  matchesRunning: number | null;
  clients: {
    versions: Record<string, number>;
    outdated: number;
  };
  totals: {
    players: number;
    clans: number;
    clansAlive: number;
    matchesLogged: number;
    matches24h: number;
    reportsNew: number;
  };
  errors: OverviewError[];
  canKick: boolean;
}

/** Kept as an alias for the older scaffold import. */
export type ServerStatus = OverviewData;

export interface OnlinePlayer {
  pid: string | null;
  name: string;
  steamid: number;
  state: "match" | "queue" | "menu" | string;
  match: string | null;
  idle_s: number | null;
  queued_s: number | null;
  version: string;
  platform: string;
  steam_helper: boolean | null;
  online_s?: number;
  state_s?: number;
  ping?: number | null;
  playerId?: string;
  tag?: string;
  clan?: {
    factionName: string;
    week: number;
    day: number;
    size: number;
  } | null;
  outdated?: boolean;
}

export interface OnlineData {
  source: string;
  players: OnlinePlayer[];
  serverVersion: string;
  canKick: boolean;
}

export interface NoticeItem {
  id: number | string;
  kind: "u" | "r" | "s" | "t" | string;
  text: string;
  endsAt?: number;
  [key: string]: unknown;
}

export interface NoticesData {
  ok: boolean;
  error?: string;
  notices: NoticeItem[];
  target_version?: string;
  now?: number;
}

export const apiUrl = import.meta.env.VITE_API_URL || "/api";

export const adminDataProvider = dataProvider(apiUrl);
