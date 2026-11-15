// Shared header, footer, links and helpers for every page.
// Links in one place: swap DOWNLOAD for the real GitHub release once the repo exists.
const LINKS = {
  download: "https://github.com/kelliex3/Twelve-Weeks/releases/latest",
  discord: "https://discord.gg/wmVgW2wxge",
};

const esc = (s) =>
  String(s == null ? "" : s).replace(
    /[&<>"']/g,
    (c) =>
      ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[
        c
      ],
  );
const api = async (path) => {
  const r = await fetch(path, { credentials: "same-origin" });
  if (!r.ok)
    throw new Error(
      (await r.json().catch(() => ({}))).error || "HTTP " + r.status,
    );
  return r.json();
};

(function shell() {
  const page = document.body.dataset.page || "";
  const items = [
    ["home", "/", "Home"],
    ["news", "/news.html", "News"],
    ["leaderboards", "/leaderboards.html", "Leaderboards"],
    ["players", "/players.html", "Players"],
    ["download", LINKS.download, "Download"],
  ];
  const head = document.createElement("header");
  head.className = "bar" + (page === "home" ? "" : " solid fixed-solid");
  head.id = "bar";
  head.innerHTML =
    `<a class="logo" href="/" aria-label="Twelve Weeks home">12</a>` +
    `<button class="menu" id="menu" aria-label="Menu" aria-expanded="false"><span></span><span></span><span></span></button>` +
    `<nav class="nav" id="nav">` +
    items
      .map(
        ([k, href, label]) =>
          `<a href="${href}"${k === page ? ' class="on"' : ""}${k === "download" ? ' data-link="download"' : ""}>${label}</a>`,
      )
      .join("") +
    `<a href="/auth/steam" class="login" id="login"><svg viewBox="0 0 24 24" aria-hidden="true"><path fill="currentColor" d="M12 2a10 10 0 0 0-9.96 9.2l5.35 2.21a2.8 2.8 0 0 1 1.6-.5h.16l2.38-3.45v-.05a3.77 3.77 0 1 1 3.77 3.77h-.09l-3.4 2.42v.13a2.83 2.83 0 0 1-5.6.57L2.4 15.1A10 10 0 1 0 12 2zM8.28 17.17l-1.22-.5a2.12 2.12 0 1 0 1.16-2.9l1.27.52a1.56 1.56 0 1 1-1.2 2.88zm9.51-7.74a2.51 2.51 0 1 0-2.51 2.51 2.51 2.51 0 0 0 2.51-2.51zm-4.4 0a1.89 1.89 0 1 1 1.89 1.89 1.89 1.89 0 0 1-1.89-1.89z"/></svg><span>Log in with Steam</span></a>` +
    `</nav>`;
  document.body.prepend(head);

  const foot = document.createElement("footer");
  foot.innerHTML = `<span class="logo">12</span><span>Twelve Weeks is a free, non-commercial fan project. It is not made, endorsed or supported by Naughty Dog or Sony Interactive Entertainment.</span>`;
  document.body.append(foot);

  document.querySelectorAll("[data-link]").forEach((a) => {
    a.href = LINKS[a.dataset.link] || "#";
    if (/^https?:/.test(a.href) && !a.href.startsWith(location.origin)) {
      a.target = "_blank";
      a.rel = "noopener";
    }
  });

  const bar = head;
  if (page === "home") {
    const onScroll = () => bar.classList.toggle("solid", scrollY > 40);
    addEventListener("scroll", onScroll, { passive: true });
    onScroll();
  }
  const menu = document.getElementById("menu"),
    nav = document.getElementById("nav");
  menu.addEventListener("click", () =>
    menu.setAttribute("aria-expanded", nav.classList.toggle("open")),
  );
  nav.addEventListener("click", (e) => {
    if (e.target.closest("a")) nav.classList.remove("open");
  });

  // Steam session: show the player's name + log out instead of the login button.
  api("/api/me")
    .then((me) => {
      if (!me.loggedIn) return;
      const l = document.getElementById("login");
      const name = (me.player && me.player.name) || "My profile";
      l.outerHTML = `<a href="/player.html?id=${esc(me.id)}" class="login me${page === "me" ? " on" : ""}">${esc(name)}</a><a href="/auth/logout" class="logout">Log out</a>`;
    })
    .catch(() => {});

  const io = new IntersectionObserver(
    (es) =>
      es.forEach((e) => {
        if (e.isIntersecting) {
          e.target.classList.add("on");
          io.unobserve(e.target);
        }
      }),
    { threshold: 0.12 },
  );
  document.querySelectorAll(".rv").forEach((el) => io.observe(el));
  window.revealNew = () =>
    document.querySelectorAll(".rv:not(.on)").forEach((el) => io.observe(el));
})();

// News items: same sections as the launcher's news tab.
const NEWS_SECS = [
  ["changed", "What changed"],
  ["why", "Why"],
  ["watch", "Watch out for"],
  ["known", "Known issues"],
  ["fixed", "Fixed"],
];
function newsCard(n, open) {
  const secs = n.sections || {};
  let body = NEWS_SECS.filter(([k]) => Array.isArray(secs[k]) && secs[k].length)
    .map(
      ([k, h]) =>
        `<div class="nsec ${k}"><h4>${h}</h4><ul>${secs[k].map((l) => `<li>${esc(l)}</li>`).join("")}</ul></div>`,
    )
    .join("");
  if (!body)
    body = `<div class="nsec"><p>${esc(n.body || n.text || "")}</p></div>`;
  const tag = n.build
    ? `Build ${esc(n.build)}${n.kind === "hotfix" ? " hotfix" : ""}`
    : "";
  return `<details class="ncard${n.kind === "hotfix" ? " hot" : ""}"${open ? " open" : ""}><summary><span class="ntag">${tag}</span><span class="nhead">${esc(n.headline || n.title || "")}</span><time>${esc(n.date || "")}</time></summary><div class="nbody">${body}</div></details>`;
}
