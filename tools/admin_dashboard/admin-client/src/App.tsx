import { Refine } from "@refinedev/core"
import routerProvider from "@refinedev/react-router"
import { BrowserRouter, Routes, Route, Link, Navigate } from "react-router-dom";
import { DashboardNav } from "./components/nav";
import { Overview } from "./pages/overview";
import "./App.css"
import { adminDataProvider } from "./providers/data"
import { useState } from "react";


// The main admin dashboard
export default function App() {
  const [isOpen, setIsOpen] = useState(false);
  return (
    <BrowserRouter>
      <Refine
        dataProvider={adminDataProvider}
        routerProvider={routerProvider}
        resources={[
          { name: "overview", list: "/overview", meta: { label: "OVERVIEW" } },
          { name: "players", list: "/players", meta: { label: "PLAYERS" } },
          { name: "matches", list: "/matches", meta: { label: "MATCHES" } },
          { name: "teams", list: "/teams", meta: { label: "TEAMS" } },
          { name: "clans", list: "/clans", meta: { label: "CLANS" } },
          { name: "reports", list: "/reports", meta: { label: "REPORTS" } },
          { name: "logs", list: "/logs", meta: { label: "LOGS" } },
          { name: "notes", list: "/notes", meta: { label: "PATCH NOTES" } },
        ]}>
        <header
          className="fixed inset-x-0 top-0 z-30 flex h-[68px] items-center
          gap-x-[clamp(14px,2vw,28px)] border-b border-line bg-[#070808]/96
          px-[clamp(16px,4vw,44px)] backdrop-blur-sm"
          id="bar"
        >
          <button
            type="button"
            onClick={() => setIsOpen(prev => !prev)}
            className="hidden max-[1080px]:flex shrink-0
            items-center justify-center p-2"
            aria-expanded={isOpen}
            aria-label="Toggle navigation menu"
          >
            {isOpen ? "X" : "☰"}
          </button>
          <Link
            to="/"
            className="order-1 flex flex-none items-center gap-3 max-[1080px]:order-2"
            aria-label="Twelve Weeks admin"
          >
            <span className="font-head font-normal text-[38px] tracking-[0.01em] text-white leading-none">
              12
            </span>
            <span className="font-ui font-semibold text-[12px] tracking-[0.3em] uppercase text-accent border border-[rgba(195,165,102,0.5)] pt-[3px] pr-[8px] pb-[2px] pl-[10px]">
              ADMIN
            </span>
          </Link>

          <div className="order-2 min-w-0 flex-1 max-[1080px]:order-1">
            <DashboardNav isOpen={isOpen} />
          </div>

          <button id="srvsw" hidden title="Which server this panel shows and acts on">
            <span data-s="live">LIVE</span>
            <span data-s="test">TEST</span>
          </button>
          <div id="online"
              className="ml-auto flex shrink-0 items-center gap-2
              font-ui text-xs leading-none font-semibold tracking-[0.16em]
              text-dim uppercase border border-line px-3 py-[5px] order-last"
          >
            <i id="on-dot"></i>
            <b id="on-n">-</b>
            <span id="on-txt">ONLINE</span>
          </div>
        </header>
        <main className="page" id="view">
          <div className="phead">
            <div>
              <div className="font-ui font-semibold text-[12px] 
              tracking-[0.3em] uppercase text-faint mb-1">Twelve Weeks admin</div>
              <h1 className="m-0 font-head font-normal
              text-[clamp(36px,5vw,56px)] leading-none uppercase text-white"
               id="ph-title">Overview</h1>
            </div>
            <span className="src" id="ph-right"></span>
          </div>
          <div id="toolbar"></div>
          <div className="mw-0" id="body">
            <Routes>
              <Route
                path="/"
                element={<Navigate to="/overview" replace />}
              />
              <Route
                path="/overview"
                element={<Overview></Overview>}
              />
              <Route
                path="/players"
                element={<h2>PLAYERS PAGE</h2>}
              />
              <Route
                path="/matches"
                element={<h2>MATCHES PAGE</h2>}
              />
              <Route
                path="/teams"
                element={<h2>TEAMS PAGE</h2>}
              />
              <Route
                path="/clans"
                element={<h2>CLANS PAGE</h2>}
              />
              <Route
                path="/reports"
                element={<h2>REPORTS PAGE</h2>}
              />
              <Route
                path="/logs"
                element={<h2>LOGS PAGE</h2>}
              />
              <Route
                path="/notes"
                element={<h2>PATCH NOTES PAGE</h2>}
              />
            </Routes>
          </div>
        </main>
        <footer>
          <span id="ver">
            <span id="ver-txt"></span>
          </span>
          {// do we need this?
          }
          <div id="hints">
            <span>
              <b>1-8</b> Tabs
            </span>
            <span>
              <b>/</b> Search
            </span>
            <span>
              <b>Esc</b> Back
            </span>
            <span>
              <b>P</b> Pause
            </span>
            <span>
              <b>R</b> Refresh
            </span>
          </div>
        </footer>
        <div id="modal" hidden>
          <div className="mbox">
            <div className="ph">
              <span id="m-title"></span>
            </div>
            <div className="mbody" id="m-body"></div>
            <div className="mfoot" id="m-foot"></div>
          </div>
        </div>
      </Refine>
    </BrowserRouter>
  );
}