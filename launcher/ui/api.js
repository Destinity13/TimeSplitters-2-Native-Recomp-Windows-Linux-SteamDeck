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
      profileList: () => inv("profile_list"),
      profileSave: (name) => inv("profile_save", { name }),
      profileLoad: (name) => inv("profile_load", { name }),
      profileDelete: (name) => inv("profile_delete", { name }),
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
    effekte: "aus", vsync: "0", fps_anzeige: "0", hd_text: "1", treffer_toenung: "100", lebensbogen: "100", weiche_schnitte: "0", lautstaerke: "100", maus_zielen: "1", maus_empfindlichkeit: "30",
    maus_invertiert: "0", maus_zielmodus_dauer: "0", zoom_empfindlichkeit: "50", vibration: "1", vibration_staerke: "100", sprache: "-1",
    trilinear: "2", dithering: "0", vollbild: "0", online_name: "", online_server: "", online_favoriten: "" };
  let start = { widescreen: true, fullscreen: false, letterbox: false, renderer: "3", game_lang: "en" };
  const fakeSrv = {
    "ts2.example.net": { name: "Siberia Night Club", players: 7, max: 16, pw: false, motd: "Deathmatch rotation · EU", ping: 24 },
    "eu1.timesplit.example": { name: "Chicago Arcade [EU]", players: 12, max: 16, pw: false, motd: "Bots on, monkeys welcome", ping: 41 },
    "us.timesplit.example": { name: "Neo Tokyo US-East", players: 3, max: 12, pw: false, motd: "Capture the bag Fridays", ping: 112 },
    "clan.example.org:27714": { name: "[TSR] Clan Server", players: 4, max: 8, pw: true, motd: "Members only", ping: 33 },
    "lan.example.com": { name: "Monkey Assistant Party", players: 16, max: 16, pw: false, motd: "Full house!", ping: 58 },
  };
  let hosting = false;
  const profs = { "Steam Deck": {}, "Couch co-op": {} };   // browser preview only
  window.API = {
    real: false, URLS,
    status: async () => ({ platform: /Win/.test(navigator.platform) ? "windows" : "linux", version: "1.2.0", ready: true, iso: "TimeSplitters 2 (USA).iso", build: "game" }),
    settingsLoad: async () => ({ ini: { ...ini }, start: { ...start } }),
    settingsSave: async (i, s) => { ini = { ...ini, ...i }; start = { ...start, ...s }; return true; },
    profileList: async () => Object.keys(profs).sort(),
    profileSave: async (n) => { profs[n] = { ...ini }; return true; },
    profileLoad: async (n) => { ini = { ...(profs[n] || ini) }; return true; },
    profileDelete: async (n) => { delete profs[n]; return true; },
    pickIso: async () => "TimeSplitters 2 (USA).iso",
    setup: async () => {
      const steps = ["Disc prüfen (SLUS-20314 / SLES-50877)", "Dateien entpacken", "ELF rekompilieren", "Laufzeit bauen", "Fertig"];
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
      if (url === URLS.news && window.__MOCK_NEWS) return JSON.stringify(window.__MOCK_NEWS);
      if (url === URLS.releases && window.__MOCK_NEWS) return "[]";
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
    updateCheck: async () => /upd/.test(location.search) ? { available: true, version: "1.2.1", critical: false, rebuild: false, changed: 12, bytes: 9_000_000, download: 9_000_000, delta: true, notes: "* Update pop-up in the launcher\n* Download progress" } : { available: false },
    updateInstall: async () => {   // preview only: fake progress
      const em = (kind, value) => (listeners.update || []).forEach((cb) => cb({ kind, value }));
      em("step", "backup"); em("info", "backup:4"); let d = 0; const tot = 9_000_000;
      setTimeout(() => { em("pct", "5"); em("step", "download");
        const iv = setInterval(() => { d = Math.min(tot, d + 600_000); em("dl", d + "/" + tot); em("pct", String(5 + Math.floor(55 * d / tot)));
          if (d >= tot) { clearInterval(iv); em("step", "verify"); let i = 0; const fv = setInterval(() => { i++; em("file", i + "/12/app/gs/file" + i + ".dll"); em("pct", String(60 + 2 * i));
            if (i >= 12) { clearInterval(fv); em("step", "install"); em("file", "5/12/app/gs/libps2xgs.dll"); em("pct", "92"); setTimeout(() => { em("step", "finish"); em("pct", "100"); em("done", ""); }, 1500); } }, 200); } }, 300); }, 800);
      return true; },
    lastIso: async () => "",
    restart: async () => true,
    on: (ev, cb) => { (listeners[ev] = listeners[ev] || []).push(cb); },
    minimize() {}, maximize() {}, close() {},
  };
})();
