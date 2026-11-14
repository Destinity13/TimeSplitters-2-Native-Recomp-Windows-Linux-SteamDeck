// Brücke zum Rust-Teil (Tauri). Ohne Tauri (Browser-Vorschau) laufen Attrappen, damit die Oberfläche bedienbar bleibt.
(() => {
  const T = window.__TAURI__;
  const REPO = "Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck";
  const URLS = {
    servers: `https://raw.githubusercontent.com/${REPO}/main/server/servers.txt`,
    news: `https://raw.githubusercontent.com/${REPO}/main/launcher/news.json`,
    releases: `https://api.github.com/repos/${REPO}/releases?per_page=8`,
    repo: `https://github.com/${REPO}`,
  };

  if (T && T.core) {
    const inv = T.core.invoke;
    const win = () => T.window.getCurrentWindow();
    window.API = {
      real: true, URLS,
      status: () => inv("status"),
      settingsLoad: () => inv("settings_load"),
      settingsSave: (ini, start) => inv("settings_save", { ini, start }),
      pickIso: () => inv("pick_iso"),
      setup: (iso) => inv("setup_start", { iso }),
      play: () => inv("play"),
      queryServers: (addrs) => inv("query_servers", { addrs }),
      queryBoard: (addr) => inv("query_board", { addr }),
      fetchText: (url) => inv("fetch_text", { url }),
      hostStart: (o) => inv("host_start", o),
      hostStop: () => inv("host_stop"),
      hostRunning: () => inv("host_running"),
      open: (what) => inv("open_path", { what }),
      openUrl: (url) => inv("open_url", { url }),
      shortcut: () => inv("make_shortcut"),
      updateCheck: () => inv("update_check"),
      updateInstall: () => inv("update_install"),
      lastIso: () => inv("last_iso"),
      restart: () => inv("restart_launcher"),
      on: (ev, cb) => T.event.listen(ev, (e) => cb(e.payload)),
      minimize: () => win().minimize(),
      maximize: () => win().toggleMaximize(),
      close: () => inv("quit_launcher").catch(() => win().close()),
    };
    return;
  }

  // ---------- Vorschau-Attrappen ----------
  const listeners = {};
  const emit = (ev, p) => (listeners[ev] || []).forEach((cb) => cb(p));
  let ini = { aufloesung: "4", bildformat: "4", fxaa: "0", cas: "1", cas_staerke: "25", texturfilter: "2", anisotrop: "16", tv_shader: "0",
    effekte: "aus", vsync: "0", fps_anzeige: "0", hd_text: "1", lautstaerke: "100", maus_zielen: "1", maus_empfindlichkeit: "30",
    maus_invertiert: "0", maus_zielmodus_dauer: "0", zoom_empfindlichkeit: "50", vibration: "1", vibration_staerke: "100", sprache: "-1",
    trilinear: "2", dithering: "0", vollbild: "0", online_name: "", online_server: "", online_favoriten: "" };
  let start = { widescreen: true, fullscreen: false, letterbox: false, renderer: "3" };
  const fakeSrv = {
    "ts2.example.net": { name: "Siberia Night Club", players: 7, max: 16, pw: false, motd: "Deathmatch rotation · EU", ping: 24 },
    "eu1.timesplit.example": { name: "Chicago Arcade [EU]", players: 12, max: 16, pw: false, motd: "Bots on, monkeys welcome", ping: 41 },
    "us.timesplit.example": { name: "Neo Tokyo US-East", players: 3, max: 12, pw: false, motd: "Capture the bag Fridays", ping: 112 },
    "clan.example.org:27714": { name: "[TSR] Clan Server", players: 4, max: 8, pw: true, motd: "Members only", ping: 33 },
    "lan.example.com": { name: "Monkey Assistant Party", players: 16, max: 16, pw: false, motd: "Full house!", ping: 58 },
  };
  let hosting = false;
  window.API = {
    real: false, URLS,
    status: async () => ({ platform: /Win/.test(navigator.platform) ? "windows" : "linux", version: "1.0.1", ready: true, iso: "TimeSplitters 2 (USA).iso", build: "pc034" }),
    settingsLoad: async () => ({ ini: { ...ini }, start: { ...start } }),
    settingsSave: async (i, s) => { ini = { ...ini, ...i }; start = { ...start, ...s }; return true; },
    pickIso: async () => "TimeSplitters 2 (USA).iso",
    setup: async () => {
      const steps = ["Disc prüfen (SLUS-203.14)", "Dateien entpacken", "ELF rekompilieren", "Laufzeit bauen", "Fertig"];
      let p = 0;
      const t = setInterval(() => {
        p += 4; emit("setup", { kind: "pct", value: String(Math.min(p, 100)) });
        if (p % 20 === 4) emit("setup", { kind: "step", value: steps[Math.min(4, Math.floor(p / 20))] });
        emit("setup", { kind: "line", value: `[bau] Einheit ${p}/100` });
        if (p >= 100) { clearInterval(t); emit("setup", { kind: "done", value: "" }); }
      }, 160);
      return true;
    },
    play: async () => { emit("game", { state: "running" }); setTimeout(() => emit("game", { state: "exited", code: 0 }), 4000); return true; },
    queryServers: async (addrs) => { await new Promise((r) => setTimeout(r, 600));
      return addrs.map((a) => fakeSrv[a] ? { addr: a, ok: true, proto: 1, ...fakeSrv[a], ping: fakeSrv[a].ping + Math.round(Math.random() * 8) } : { addr: a, ok: false }); },
    fetchText: async (url) => {
      if (url === URLS.servers) return Object.keys(fakeSrv).join("\n") + "\nold.example.net\n";
      if (url === URLS.news) return JSON.stringify([{ date: "2026-11-12", title: "Neuer Launcher", tag: "Launcher", body: "Komplett neuer Start-Launcher mit Serverbrowser, News und allen Einstellungen an einem Ort." }]);
      if (url === URLS.releases) return JSON.stringify([
        { tag_name: "v1.0.1", name: "1.0.1 – Windows is here", published_at: "2026-11-11T10:00:00Z", html_url: URLS.repo + "/releases/tag/v1.0.1",
          body: "## What's new\n- **Windows 10/11** build, D3D11 by default\n- HUD fixed in 16:9, sniper zoom fixed\n- Fog and snow slider `TS2_DUNST`\n- Online server browser in the settings menu" },
        { tag_name: "v1.0.0", name: "1.0.0 – Online", published_at: "2026-11-08T10:00:00Z", html_url: URLS.repo + "/releases/tag/v1.0.0",
          body: "First stable release.\n- LAN mode brought online through `ts2-server`\n- Steam Deck profile\n- Texture packs" }]);
      throw new Error("offline");
    },
    hostStart: async () => { hosting = true; return true; },
    hostStop: async () => { hosting = false; return true; },
    hostRunning: async () => hosting,
    open: async () => true,
    openUrl: async (u) => window.open(u, "_blank"),
    shortcut: async () => true,
    updateCheck: async () => ({ available: false }),
    updateInstall: async () => true,
    lastIso: async () => "",
    restart: async () => true,
    on: (ev, cb) => { (listeners[ev] = listeners[ev] || []).push(cb); },
    minimize() {}, maximize() {}, close() {},
  };
})();
