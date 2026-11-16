// TS2 Native Recomp Launcher - Oberfläche
"use strict";
const $ = (s, r = document) => r.querySelector(s);
const $$ = (s, r = document) => [...r.querySelectorAll(s)];
const esc = (s) => String(s ?? "").replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

// ---------------- Texte ----------------
const L = {
  de: {
    "nav.play": "Start", "play.sub": "Spiel ist aktuell", "setup.sub": "Disc-Abbild wählen", "stat.srv": "Server",
    "pg.building": "Bitte warten, das Spiel wird gebaut …", "pg.step": "Schritt", "pg.total": "Gesamtfortschritt", "pg.elapsed": "Vergangen", "pg.remain": "Verbleibend",
    "pg.ready": "Spiel ist bereit.", "pg.needSetup": "Ersteinrichtung nötig: Der Spielcode wird einmalig aus deiner Disc gebaut.", "pg.verified": "geprüft", "nav.online": "Online", "nav.news": "News", "nav.settings": "Optionen",
    play: "Spielen", running: "Läuft", setupNeeded: "Einrichten",
    "home.mp": "Multiplayer", "home.status": "System", "home.quick": "Schnellstart", "home.live": "Server",
    "st.ver": "Version", "st.plat": "System", "st.disc": "Disc", "st.build": "Build", "st.ok": "bereit", "st.no": "fehlt",
    "q.ws": "16:9 Breitbild", "q.ws.s": "Echtes Widescreen, mehr Sichtfeld", "ed.title": "Was möchtest du installieren?", "ed.orig": "TimeSplitters 2", "ed.orig.s": "Das Original, so nah wie möglich an der PS2. Nur Verbesserungen bei Bild, Steuerung und Leistung.", "ed.op": "TimeSplitters 2 Online+", "ed.op.s": "Eigene Spielversion mit Online+-Menü im Spiel: Serverbrowser, Level und XP, Statistiken, Treffer- und Kill-Marker, Treffer-Sounds und mehr Wucht. Eigener Spielstand.", "ed.both": "Beides", "ed.both.s": "Beide Versionen. Belegt kaum mehr Platz, beide nutzen denselben eingelesenen Spielcode.", "ed.s": "Installierte Versionen", "ed.pick": "Version", "on.top": "Online+ Bestenliste", "on.kills": "Kills",
    "q.fs": "Vollbild", "q.fs.s": "Direkt im Vollbild starten",
    "q.lb": "Kino-Balken", "q.lb.s": "Balken in Zwischensequenzen",
    "q.fps": "FPS-Anzeige", "q.fps.s": "Bildrate oben rechts",
    "mini.none": "Keine Server erreichbar", "mini.all": "Alle Server anzeigen",
    "on.title": "Online", "on.sub": "Server suchen, Favoriten merken, beitreten. Das Spiel verbindet sich automatisch mit dem gewählten Server.",
    "on.search": "Server suchen …", "on.f.free": "Freie Plätze", "on.f.open": "Ohne Passwort", "on.f.fav": "Favoriten", "on.refresh": "Aktualisieren",
    "on.c.name": "Server", "on.c.players": "Spieler", "on.c.ping": "Ping",
    "on.direct": "Direkte Adresse (host oder host:port)", "on.add": "Hinzufügen",
    "on.you": "Dein Spielername", "on.name": "Name (max. 15 Zeichen)", "on.nameNote": "So sehen dich die anderen in der Lobby.",
    "on.pick": "Wähle links einen Server aus.", "on.join": "Beitreten", "on.joinPlay": "Beitreten & Spielen",
    "on.how1": "Spiel startet und verbindet sich mit dem Server", "on.how2": "Im Spiel: Arcade → Network", "on.how3": "Join Game (Beitreten) oder Start Game (selbst hosten)",
    "on.offline": "Nicht erreichbar", "on.full": "Voll", "on.pwTitle": "Server-Passwort",
    "on.host": "Eigenen Server hosten", "on.h.name": "Servername", "on.h.pw": "Passwort (optional)",
    "on.h.start": "Server starten", "on.h.stop": "Server stoppen", "on.h.note": "Port 27713 TCP+UDP im Router freigeben, damit Freunde von außen beitreten können.",
    "on.h.running": "Dein Server läuft auf Port 27713.", "on.loading": "Server werden abgefragt …", "on.empty": "Keine Server gefunden.",
    "news.title": "News", "news.sub": "Updates, Releases und Ankündigungen direkt aus dem Projekt.", "news.offline": "News konnten nicht geladen werden (offline?).",
    "news.open": "Auf GitHub ansehen", "news.update": "Update verfügbar", "news.latest": "Neueste Meldung", "upd.run": "Update wird installiert …", "upd.done": "Update auf {v} installiert. Spielstände wurden vorher gesichert.", "upd.fail": "Update fehlgeschlagen: {e} – nichts wurde verändert.",
    "upd.crit": "Großes Update {v}: Das Spiel muss neu eingerichtet werden (dauert ein paar Minuten). Spielstände bleiben erhalten und werden vorher gesichert.", "upd.now": "Jetzt aktualisieren", "upd.later": "Später",
    "upd.rebuild": "Update installiert – das Spiel wird jetzt neu verknüpft …", "upd.restart": "Launcher wurde aktualisiert und startet neu …",
    "upd.title": "Neues Update verfügbar", "upd.ask": "Version {v} ist da ({mb}). Du kannst jetzt aktualisieren oder erst mit deiner aktuellen Version spielen.", "upd.playOld": "Ohne Update spielen", "upd.install": "Jetzt updaten",
    "upd.btn": "Update", "upd.btnSub": "{v} verfügbar", "upd.head": "Update auf {v} wird installiert …", "upd.s.backup": "Spielstände sichern", "upd.s.download": "Herunterladen", "upd.s.install": "Prüfen und installieren",
    "upd.dl": "Heruntergeladen", "upd.speed": "Tempo", "upd.finTitle": "Fertig!", "upd.fin": "Version {v} ist installiert. Du kannst starten, viel Spaß!", "upd.notes": "Was ist neu",
    "set.title": "Optionen", "set.sub": "Alles, was das Spielmenü auch kann, schon vor dem Start. Änderungen gelten beim nächsten Spielstart.",
    "set.save": "Speichern", "set.reset": "Standard", "set.saved": "Gespeichert ✓", "set.unsaved": "Ungespeicherte Änderungen",
    "tab.gfx": "Grafik", "tab.fx": "Bild", "tab.audio": "Ton & Pad", "tab.input": "Steuerung", "tab.launch": "Start & System",
    "s.res": "Interne Auflösung", "s.asp": "Bildformat", "s.filt": "Texturfilter", "s.aniso": "Anisotrop", "s.tri": "Trilinear",
    "s.vsync": "VSync", "s.vsync.s": "Kein Tearing, minimal mehr Verzögerung", "s.hdtext": "HD-Text", "s.hdtext.s": "Neu gesetzte, scharfe Textseiten",
    "s.cas": "Schärfe (CAS)", "s.cass": "Schärfe-Stärke", "s.fxaa": "FXAA", "s.fxaa.s": "Weiche Kanten (leicht unscharf)",
    "s.tv": "TV-Filter", "s.fx": "Effekte (vkBasalt, Linux)", "s.dith": "Dithering",
    "s.vol": "Lautstärke", "s.rumble": "Vibration", "s.rumble.s": "Rumble am Controller", "s.rumblep": "Vibrationsstärke",
    "s.mouse": "Mauszielen", "s.mouse.s": "Klick ins Fenster fängt die Maus", "s.msens": "Mausempfindlichkeit", "s.minv": "Maus invertiert",
    "s.chp": "Cheat: unendlich Leben", "s.chp.s": "Nur Einzelspieler und Splitscreen. Aus in Online+ und Online-Partien", "s.car": "Cheat: unendlich Rüstung", "s.car.s": "Nur Einzelspieler und Splitscreen", "s.cam": "Cheat: unendlich Munition", "s.cam.s": "Der Vorrat sinkt beim Nachladen nicht",
    "s.aaim": "Zielhilfe (Maus)", "s.aaim.s": "Zieht das Ziel wie auf der PS2 zu Gegnern. Aus = Schüsse gehen genau aufs Fadenkreuz",
    "s.padm": "Gamepad: modernes Zielen", "s.padm.s": "Rechter Stick dreht die Sicht, Fadenkreuz fest in der Mitte, L2 zoomt. Aus = Original-PS2-Steuerung", "s.pads": "Gamepad: Drehgeschwindigkeit", "s.unlock": "Alles freischalten", "s.unlock.s": "Level, Charaktere und Cheats offen, solange dies an ist. Eigener Fortschritt bleibt",
    "s.mmode": "Zielmodus immer halten", "s.mmode.s": "Original-Fadenkreuz, aber langsameres Laufen", "s.zoom": "Zoom-Empfindlichkeit", "s.xhair": "Fadenkreuz (Maus)", "s.pitch": "Blick hoch/runter bis (Original 40°)", "s.scope": "Zielfernrohr", "tab.op": "Online+", "s.imp": "Treffer-Effekte (Online+)", "s.imp.s": "Aus = Spiel exakt wie auf der PS2", "s.mark": "Treffer- und Kill-Marker", "s.snd": "Treffer- und Kill-Sound", "s.kick": "Kamerastoß bei Treffern", "s.opnote": "Level, XP und Abschüsse siehst du im Spielmenü (ESC/F1) im Tab Online+.",
    "s.keys": "Tastenbelegung und Controller stellst du direkt im Spiel ein (ESC / F1 / Guide-Taste).",
    "s.lang": "Sprache (Launcher + Spielmenü)", "s.renderer": "Grafik-API (Windows)",
    "s.setupAgain": "Neu einrichten", "s.setupAgain.s": "Andere Disc oder nach einem Update neu bauen",
    "s.shortcut": "Verknüpfung anlegen", "s.folder": "Spielordner", "s.log": "Protokoll", "s.cfg": "Einstellungsordner",
    "su.title": "Ersteinrichtung", "su.intro": "Dieser Port enthält keine Spieldaten. Der Spielcode wird einmalig aus deiner eigenen Disc gebaut.",
    "su.1": "Disc-Abbild wählen: TimeSplitters 2, USA, SLUS-20314", "su.2": "Prüfen und entpacken (die ISO wird nur gelesen)", "su.3": "Rekompilieren und bauen, ca. 5–20 Minuten",
    "su.pick": "ISO auswählen", "su.run": "Baue", "su.done": "Fertig! Viel Spaß.", "su.fail": "Einrichtung fehlgeschlagen", "su.close": "Schließen",
    "toast.saved": "Einstellungen gespeichert", "toast.exit": "Das Spiel hat sich beendet (Code {c}). Protokoll: app/build/play.log",
    "toast.join": "Server gesetzt: {s}. Spiel startet …", "toast.noName": "Bitte erst einen Spielernamen eingeben.",
    "sb.ready": "Bereit", "sb.setup": "Einrichtung nötig", "sb.running": "Spiel läuft", cancel: "Abbrechen",
    "ticker": "Willkommen zurück, Timesplitter!  ·  Online: Arcade → Network → Join Game  ·  ESC/F1 im Spiel öffnet das Menü  ·  Eigene Server: ts2-server liegt im Paket  ·  ",
  },
  en: {
    "nav.play": "Home", "play.sub": "Game is up to date", "setup.sub": "Choose disc image", "stat.srv": "Servers",
    "pg.building": "Please wait, the game is being built …", "pg.step": "Step", "pg.total": "Total progress", "pg.elapsed": "Elapsed", "pg.remain": "Remaining",
    "pg.ready": "Game is ready.", "pg.needSetup": "First-time setup needed: the game code is built once from your disc.", "pg.verified": "verified", "nav.online": "Online", "nav.news": "News", "nav.settings": "Options",
    play: "Start Game", running: "Running", setupNeeded: "Set up",
    "home.mp": "Multiplayer", "home.status": "System", "home.quick": "Quick start", "home.live": "Servers",
    "st.ver": "Version", "st.plat": "Platform", "st.disc": "Disc", "st.build": "Build", "st.ok": "ready", "st.no": "missing",
    "q.ws": "16:9 widescreen", "q.ws.s": "True widescreen, wider view", "ed.title": "What do you want to install?", "ed.orig": "TimeSplitters 2", "ed.orig.s": "The original, as close to the PS2 as possible. Only picture, controls and performance improvements.", "ed.op": "TimeSplitters 2 Online+", "ed.op.s": "A separate version with an in-game Online+ menu: server browser, levels and XP, stats, hit and kill markers, hit sounds and more impact. Own save data.", "ed.both": "Both", "ed.both.s": "Both versions. Takes almost no extra space, both use the same game code built from your disc.", "ed.s": "Installed versions", "ed.pick": "Version", "on.top": "Online+ leaderboard", "on.kills": "kills",
    "q.fs": "Fullscreen", "q.fs.s": "Start in fullscreen",
    "q.lb": "Cinema bars", "q.lb.s": "Letterbox cutscenes",
    "q.fps": "FPS counter", "q.fps.s": "Frame rate top right",
    "mini.none": "No servers reachable", "mini.all": "Show all servers",
    "on.title": "Online", "on.sub": "Find servers, keep favourites, join. The game connects to the chosen server by itself.",
    "on.search": "Search servers …", "on.f.free": "Free slots", "on.f.open": "No password", "on.f.fav": "Favourites", "on.refresh": "Refresh",
    "on.c.name": "Server", "on.c.players": "Players", "on.c.ping": "Ping",
    "on.direct": "Direct address (host or host:port)", "on.add": "Add",
    "on.you": "Your player name", "on.name": "Name (max. 15 characters)", "on.nameNote": "This is how others see you in the lobby.",
    "on.pick": "Pick a server on the left.", "on.join": "Join", "on.joinPlay": "Join & play",
    "on.how1": "The game starts and connects to the server", "on.how2": "In game: Arcade → Network", "on.how3": "Join Game, or Start Game to host",
    "on.offline": "Unreachable", "on.full": "Full", "on.pwTitle": "Server password",
    "on.host": "Host your own server", "on.h.name": "Server name", "on.h.pw": "Password (optional)",
    "on.h.start": "Start server", "on.h.stop": "Stop server", "on.h.note": "Open port 27713 TCP+UDP on your router so friends can join from outside.",
    "on.h.running": "Your server is running on port 27713.", "on.loading": "Querying servers …", "on.empty": "No servers found.",
    "news.title": "News", "news.sub": "Updates, releases and announcements straight from the project.", "news.offline": "Could not load news (offline?).",
    "news.open": "View on GitHub", "news.update": "Update available", "news.latest": "Latest", "upd.run": "Installing update …", "upd.done": "Updated to {v}. Your saves were backed up first.", "upd.fail": "Update failed: {e} – nothing was changed.",
    "upd.crit": "Major update {v}: the game has to be set up again (takes a few minutes). Saves are kept and backed up first.", "upd.now": "Update now", "upd.later": "Later",
    "upd.rebuild": "Update installed – relinking the game …", "upd.restart": "The launcher was updated and restarts …",
    "upd.title": "New update available", "upd.ask": "Version {v} is here ({mb}). You can update now or play your current version first.", "upd.playOld": "Play without updating", "upd.install": "Update now",
    "upd.btn": "Update", "upd.btnSub": "{v} available", "upd.head": "Installing update {v} …", "upd.s.backup": "Backing up saves", "upd.s.download": "Downloading", "upd.s.install": "Checking and installing",
    "upd.dl": "Downloaded", "upd.speed": "Speed", "upd.finTitle": "Done!", "upd.fin": "Version {v} is installed. You can start now, have fun!", "upd.notes": "What's new",
    "set.title": "Options", "set.sub": "Everything the in-game menu can do, before you launch. Changes apply on the next launch.",
    "set.save": "Save", "set.reset": "Defaults", "set.saved": "Saved ✓", "set.unsaved": "Unsaved changes",
    "tab.gfx": "Graphics", "tab.fx": "Picture", "tab.audio": "Sound & pad", "tab.input": "Controls", "tab.launch": "Launch & system",
    "s.res": "Internal resolution", "s.asp": "Aspect ratio", "s.filt": "Texture filter", "s.aniso": "Anisotropic", "s.tri": "Trilinear",
    "s.vsync": "VSync", "s.vsync.s": "No tearing, slightly more latency", "s.hdtext": "HD text", "s.hdtext.s": "Re-typeset, sharp text pages",
    "s.cas": "Sharpening (CAS)", "s.cass": "Sharpening strength", "s.fxaa": "FXAA", "s.fxaa.s": "Smoother edges (slightly soft)",
    "s.tv": "TV filter", "s.fx": "Effects (vkBasalt, Linux)", "s.dith": "Dithering",
    "s.vol": "Volume", "s.rumble": "Vibration", "s.rumble.s": "Controller rumble", "s.rumblep": "Vibration strength",
    "s.mouse": "Mouse aiming", "s.mouse.s": "Click into the window to capture the mouse", "s.msens": "Mouse sensitivity", "s.minv": "Invert mouse",
    "s.chp": "Cheat: infinite health", "s.chp.s": "Single player and split screen only. Off in Online+ and online matches", "s.car": "Cheat: infinite armor", "s.car.s": "Single player and split screen only", "s.cam": "Cheat: infinite ammo", "s.cam.s": "The reserve does not go down when you reload",
    "s.aaim": "Auto-aim (mouse)", "s.aaim.s": "Pulls the aim to enemies like on the PS2. Off = shots go exactly to the crosshair",
    "s.padm": "Gamepad: modern aiming", "s.padm.s": "Right stick turns the view, crosshair fixed in the centre, L2 zooms. Off = original PS2 controls", "s.pads": "Gamepad: turn speed", "s.unlock": "Unlock everything", "s.unlock.s": "Levels, characters and cheats open while this is on. Your own progress stays",
    "s.mmode": "Always hold aim mode", "s.mmode.s": "Original crosshair, but slower walking", "s.zoom": "Zoom sensitivity", "s.xhair": "Crosshair (mouse)", "s.pitch": "Look up/down limit (original 40°)", "s.scope": "Sniper scope", "tab.op": "Online+", "s.imp": "Impact effects (Online+)", "s.imp.s": "Off = the game exactly as on the PS2", "s.mark": "Hit and kill markers", "s.snd": "Hit and kill sounds", "s.kick": "Camera kick when hit", "s.opnote": "Level, XP and kills are shown in the game menu (ESC/F1), tab Online+.",
    "s.keys": "Key bindings and controllers are set in game (ESC / F1 / Guide button).",
    "s.lang": "Language (launcher + in-game menu)", "s.renderer": "Graphics API (Windows)",
    "s.setupAgain": "Set up again", "s.setupAgain.s": "Rebuild from another disc or after an update",
    "s.shortcut": "Add shortcut", "s.folder": "Game folder", "s.log": "Log", "s.cfg": "Settings folder",
    "su.title": "First-time setup", "su.intro": "This port contains no game data. The game code is built once from your own disc.",
    "su.1": "Select your disc image: TimeSplitters 2, USA, SLUS-20314", "su.2": "Verify and extract (the ISO is only read)", "su.3": "Recompile and build, about 5–20 minutes",
    "su.pick": "Choose ISO", "su.run": "Building", "su.done": "Done! Have fun.", "su.fail": "Setup failed", "su.close": "Close",
    "toast.saved": "Settings saved", "toast.exit": "The game exited (code {c}). Log: app/build/play.log",
    "toast.join": "Server set: {s}. Launching …", "toast.noName": "Please enter a player name first.",
    "sb.ready": "Ready", "sb.setup": "Setup needed", "sb.running": "Game running", cancel: "Cancel",
    "ticker": "Welcome back, Timesplitter!  ·  Online: Arcade → Network → Join Game  ·  ESC/F1 in game opens the menu  ·  Host your own: ts2-server ships in the package  ·  ",
  },
};
let lang = "en";
const t = (k, v = {}) => (L[lang][k] ?? L.en[k] ?? k).replace(/\{(\w+)\}/g, (_, x) => v[x] ?? "");

// ---------------- Zustand ----------------
const S = {
  status: { platform: "linux", version: "?", ready: false },
  ini: {}, start: {}, saved: null,
  servers: [], sel: null, sort: { k: "players", d: -1 }, filt: new Set(), extra: [],
  news: [], running: false, hosting: false, setTab: "gfx",
};
const DEF = { aufloesung: "9", bildformat: "4", fxaa: "0", cas: "1", cas_staerke: "25", texturfilter: "2", anisotrop: "16", tv_shader: "0", effekte: "aus",
  vsync: "0", fps_anzeige: "0", hd_text: "1", lautstaerke: "100", maus_zielen: "1", maus_empfindlichkeit: "30", maus_invertiert: "0", maus_zielmodus_dauer: "0",
  zoom_empfindlichkeit: "50", fadenkreuz: "0", blickwinkel_max: "40", pad_modern: "0", pad_tempo: "100", auto_ziel: "0", leben_unendlich: "0", ruestung_unendlich: "0", munition_unendlich: "0", alles_frei: "0", zielfernrohr: "1", online_plus_effekte: "1", schaden_stoss: "50", treffermarker: "1", treffer_klang: "1", vibration: "1", vibration_staerke: "100", trilinear: "2", dithering: "0" };
const DEF_START = { widescreen: true, fullscreen: false, letterbox: false, renderer: "3", editions: "both", edition: "original" };
const iv = (k) => S.ini[k] ?? DEF[k] ?? "";
const favs = () => (S.ini.online_favoriten || "").split(",").map((s) => s.trim()).filter(Boolean);

function toast(msg, ms = 3200) {
  const el = $("#toast"); el.textContent = msg; el.classList.add("on");
  clearTimeout(toast.h); toast.h = setTimeout(() => el.classList.remove("on"), ms);
}

// ---------------- Sprache ----------------
function applyLang() {
  document.documentElement.lang = lang;
  $$("[data-t]").forEach((e) => (e.textContent = t(e.dataset.t)));
  $$("[data-ph]").forEach((e) => (e.placeholder = t(e.dataset.ph)));
  $("#langBtn").textContent = lang === "de" ? "DE" : "EN";
  renderAll();
}
function detectLang() {
  const s = S.ini.sprache;
  if (s === "1") return "de";
  if (s === "0") return "en";
  return "en" /* default: English (US); German only when chosen */;
}

// ---------------- Navigation ----------------
function go(v) {
  $$("#nav button[data-view]").forEach((b) => b.classList.toggle("on", b.dataset.view === v));
  $$(".view").forEach((s) => s.classList.toggle("on", s.id === "v-" + v));
  if (v === "online" && !S.servers.length) loadServers();
}

// ---------------- Start ----------------
const P = { active: false, pct: 0, step: "", t0: 0, stepT0: 0, stepPct0: 0, failed: "" };
const mmss = (sec) => { sec = Math.max(0, Math.round(sec)); return Math.floor(sec / 60) + ":" + String(sec % 60).padStart(2, "0"); };
function renderHome() {
  const st = S.status;
  const plat = st.platform === "windows" ? "Windows" : st.platform === "steamdeck" ? "Steam Deck" : "Linux";
  const btn = $("#playBtn");
  const updWait = !!(S.updInfo && !U.active && !P.active && !S.running && st.ready);
  btn.classList.toggle("busy", S.running || P.active || U.active);
  btn.classList.toggle("upd", updWait);
  btn.classList.toggle("setup", !st.ready && !P.active);
  $("#playLabel").textContent = U.active ? t("upd.btn") : updWait ? t("upd.btn") : P.active ? t("su.run") : S.running ? t("running") : st.ready ? t("play") : t("setupNeeded");
  $("#playSub").textContent = U.active ? U.pct + " %" : updWait ? t("upd.btnSub", { v: "v" + S.updInfo.version }) : P.active ? P.pct + " %" : S.running ? "" : st.ready ? (S.start.editions === "original" ? t("play.sub") : t(curEd() === "onlineplus" ? "ed.op" : "ed.orig")) : t("setup.sub");
  const sw = $("#edSwitch");
  if (sw) { sw.style.display = st.ready ? "" : "none";   // one build serves both editions: the switch is always there, the classic game stays preselected
    sw.innerHTML = [["original", "Original"], ["onlineplus", "Online+"]].map(([id, k]) => `<button data-edsel="${id}" class="${curEd() === id ? "on" : ""}">${k}</button>`).join(""); }
  $("#sbVer").textContent = `v${st.version} · ${plat}` + (S.update ? ` · ${t("news.update")}: ${S.update}` : "");
  // Serverstatus oben rechts
  const live = S.servers.filter((s) => s.ok), pl = live.reduce((n, s) => n + s.players, 0);
  $("#srvStat").innerHTML = S.servers.length
    ? `<span class="led ${live.length ? "" : "off"}"></span>${t("stat.srv")} <b>${live.length}</b> · ${t("on.c.players")} <b>${pl}</b>`
    : `<span class="led warn"></span>${t("on.loading")}`;
  // Meldungen
  const feed = [];
  if (S.update) feed.push({ tag: "Update", cls: "upd", title: t("news.update") + ": " + S.update, go: S.updInfo ? "upd" : "news" });
  for (const n of S.news.slice(0, S.update ? 2 : 3)) feed.push({ tag: n.src === "rel" ? "Release" : n.tag || "News", cls: n.src === "rel" ? "rel" : "", title: n.title, date: fmtDate(n.date), go: "news" });
  const top = live.slice().sort((a, b) => b.players - a.players)[0];
  if (top && top.players > 0) feed.push({ tag: "Online", cls: "", title: `${top.name} · ${top.players}/${top.max}`, addr: top.addr });
  $("#feed").innerHTML = feed.map((f) => `<div class="n" ${f.addr ? `data-srv="${esc(f.addr)}"` : `data-go="${f.go}"`}><span class="tag ${f.cls}">${esc(f.tag)}</span>
    <span class="ti">${esc(f.title)}${f.date ? `<small>${esc(f.date)}</small>` : ""}</span><span class="ar">›</span></div>`).join("");
  // Fortschritt / Zustand unten
  const head = $("#pgHead"), r1 = $("#pgRow1"), r2 = $("#pgRow2"), bar = $("#pgBar"), sbar = $("#pgStepBar");
  if (U.active) {
    const el = (Date.now() - U.t0) / 1000, rest = U.pct > 5 ? el * (100 - U.pct) / U.pct : null;
    head.textContent = t("upd.head", { v: "v" + U.ver });
    const stepTxt = U.step ? t("upd.s." + U.step) : "…";
    let dl = "";
    if (U.step === "download" && U.dlDone) {
      const sp = U.dlDone / Math.max(0.5, (Date.now() - U.dlT0) / 1000);
      dl = `<span>${t("upd.dl")}: <b>${mb(U.dlDone)}${U.dlTotal ? " / " + mb(U.dlTotal) : ""}</b></span><span>${t("upd.speed")}: <b>${mb(sp)}/s</b></span>`;
    }
    r1.innerHTML = `<span>${t("pg.step")}: <b>${esc(stepTxt)}</b></span>${dl}`;
    r2.innerHTML = `<span>${t("pg.total")}: <b>${U.pct} %</b></span><span>${t("pg.elapsed")}: <b>${mmss(el)}</b></span><span>${t("pg.remain")}: <b>${rest == null ? "…" : "~" + mmss(rest)}</b></span>`;
    sbar.style.display = "none";
    bar.classList.remove("ok"); bar.firstElementChild.style.width = U.pct + "%";
  } else if (P.active) {
    const el = (Date.now() - P.t0) / 1000, rest = P.pct > 3 ? el * (100 - P.pct) / P.pct : null;
    head.textContent = t("pg.building");
    r1.innerHTML = `<span>${t("pg.step")}: <b>${esc(P.step || "…")}</b></span>`;
    r2.innerHTML = `<span>${t("pg.total")}: <b>${P.pct} %</b></span><span>${t("pg.elapsed")}: <b>${mmss(el)}</b></span><span>${t("pg.remain")}: <b>${rest == null ? "…" : "~" + mmss(rest)}</b></span>`;
    sbar.style.display = "";
    bar.classList.remove("ok"); bar.firstElementChild.style.width = P.pct + "%";
  } else {
    sbar.style.display = "none";
    head.innerHTML = P.failed ? `<span class="err">${t("su.fail")}: ${esc(P.failed)}</span>` : S.running ? t("sb.running") : st.ready ? t("pg.ready") : t("pg.needSetup");
    r1.innerHTML = "";
    r2.innerHTML = st.ready
      ? `<span>${t("st.disc")}: <b>${t("pg.verified")}</b></span><span>${t("st.build")}: <b>${esc(st.build)}</b></span><span>${t("pg.total")}: <b>100 %</b></span>`
      : `<span>${t("st.disc")}: <b>${t("st.no")}</b></span><span>${t("pg.total")}: <b>0 %</b></span>`;
    bar.classList.toggle("ok", st.ready); bar.firstElementChild.style.width = st.ready ? "100%" : "0%";
  }
  const q = [["ws", "q.ws", S.start.widescreen], ["fs", "q.fs", S.start.fullscreen], ["fps", "q.fps", iv("fps_anzeige") === "1"]];
  $("#quick").innerHTML = q.map(([id, k, on]) => `<div class="tog ${on ? "on" : ""}" data-q="${id}"><span class="t">${t(k)}</span><span class="sw"></span></div>`).join("");
}

async function quickToggle(id) {
  if (id === "ws") S.start.widescreen = !S.start.widescreen;
  if (id === "fs") S.start.fullscreen = !S.start.fullscreen;
  if (id === "lb") S.start.letterbox = !S.start.letterbox;
  if (id === "fps") S.ini.fps_anzeige = iv("fps_anzeige") === "1" ? "0" : "1";
  if (id === "fs") S.ini.vollbild = S.start.fullscreen ? "1" : "0";
  await persist(false);
  renderHome();
}

const curEd = () => S.start.editions === "onlineplus" ? "onlineplus" : S.start.editions === "original" ? "original" : S.start.edition;
async function play() {
  if (S.running || P.active || U.active) return;
  if (S.updInfo && S.status.ready) return showUpdateDialog(S.updInfo);
  if (!S.status.ready) return openSetup();
  S.start.edition = curEd();
  await persist(false);
  await playNow();
}
async function playNow() {
  S.start.edition = curEd();
  await persist(false);
  try { await API.play(); } catch (e) { toast(String(e)); }
}

// ---------------- Einrichtung ----------------
function openSetup() {
  const edCard = (id, k) => `<div class="edc ${S.start.editions === id ? "on" : ""}" data-ed="${id}"><b>${t(k)}</b><small>${t(k + ".s")}</small></div>`;
  $("#setupBox").innerHTML = `<h3>${t("su.title")}</h3><p class="sub" style="margin:0">${t("su.intro")}</p>
    
    <div class="steps"><div><b>1.</b>${t("su.1")}</div><div><b>2.</b>${t("su.2")}</div><div><b>3.</b>${t("su.3")}</div></div>
    <div style="display:flex;gap:10px;justify-content:flex-end"><button class="btn ghost" id="suCancel">${t("cancel")}</button><button class="btn hot" id="suPick">${t("su.pick")}</button></div>`;
  $("#setup").classList.add("on");
  $("#suCancel").onclick = () => $("#setup").classList.remove("on");
  $$("#setupBox .edc").forEach((el) => el.onclick = () => {
    S.start.editions = el.dataset.ed; S.start.edition = el.dataset.ed === "onlineplus" ? "onlineplus" : S.start.edition === "onlineplus" && el.dataset.ed === "original" ? "original" : S.start.edition;
    $$("#setupBox .edc").forEach((x) => x.classList.toggle("on", x === el)); persist(false); });
  $("#suPick").onclick = async () => {
    const iso = await API.pickIso();
    if (!iso) return;
    $("#setup").classList.remove("on"); go("home");
    Object.assign(P, { active: true, pct: 0, step: "", t0: Date.now(), stepT0: Date.now(), failed: "", log: "" });
    clearInterval(P.timer); P.timer = setInterval(renderHome, 500); renderHome();
    try { await API.setup(iso); } catch (e) { setupEvent({ kind: "error", value: String(e) }); }
  };
}
function setupEvent(e) {
  if (e.kind === "pct") P.pct = Math.max(0, Math.min(100, parseInt(e.value, 10) || 0));
  else if (e.kind === "step") { P.step = e.value; P.stepT0 = Date.now(); }
  else if (e.kind === "line") P.log = ((P.log || "") + e.value + "\n").slice(-4000);
  else if (e.kind === "done" || e.kind === "error") {
    clearInterval(P.timer); P.active = false;
    if (e.kind === "done") { P.failed = ""; refreshStatus().then(() => toast(t("su.done"))); }
    else {
      P.failed = e.value;
      $("#setupBox").innerHTML = `<h3 class="err">${t("su.fail")}</h3><div>${esc(e.value)}</div><div class="logbox" style="max-height:220px">${esc(P.log || "")}</div>
        <div style="display:flex;justify-content:flex-end;margin-top:14px"><button class="btn hot" id="suOk">${t("su.close")}</button></div>`;
      $("#setup").classList.add("on"); $("#suOk").onclick = () => $("#setup").classList.remove("on");
    }
  }
  renderHome();
}

// ---------------- Online ----------------
function pingHtml(p) {
  const g = p == null ? 0 : p < 60 ? 3 : p < 130 ? 2 : 1;
  return `<span class="ms g${g}">${p == null ? "–" : p + " ms"}</span>`;
}
async function loadServers() {
  $("#srvBody").innerHTML = `<tr><td colspan="4" class="empty">${t("on.loading")}</td></tr>`;
  let list = [];
  try { list = (await API.fetchText(API.URLS.servers)).split(/\r?\n/).map((l) => l.replace(/#.*/, "").trim()).filter(Boolean); } catch (_) {}
  const addrs = [...new Set([...favs(), ...list, ...S.extra, ...(S.ini.online_server ? [S.ini.online_server] : [])])];
  let res = [];
  try { res = await API.queryServers(addrs); } catch (e) { toast(String(e)); }
  S.servers = res.map((r) => ({ ...r, name: r.ok ? r.name : r.addr, players: r.ok ? r.players : -1, max: r.max || 0 }));
  if (!S.sel && S.ini.online_server) S.sel = S.ini.online_server;
  renderOnline(); renderHome();
}
function renderOnline() {
  const q = $("#srvSearch").value.trim().toLowerCase(), F = favs();
  let rows = S.servers.filter((s) => (!q || (s.name + " " + s.addr + " " + (s.motd || "")).toLowerCase().includes(q))
    && (!S.filt.has("free") || (s.ok && s.players < s.max)) && (!S.filt.has("open") || (s.ok && !s.pw)) && (!S.filt.has("fav") || F.includes(s.addr)));
  const { k, d } = S.sort;
  rows.sort((a, b) => (b.ok - a.ok) || (k === "name" ? d * a.name.localeCompare(b.name) : k === "ping" ? d * ((a.ping ?? 1e4) - (b.ping ?? 1e4)) : d * (a.players - b.players)));
  $$("table.srv th[data-s]").forEach((th) => th.classList.toggle("sorted", th.dataset.s === k));
  $$("#srvFilt button").forEach((b) => b.classList.toggle("on", S.filt.has(b.dataset.f)));
  $("#srvBody").innerHTML = rows.length ? rows.map((s) => `<tr class="${s.addr === S.sel ? "sel" : ""} ${s.ok ? "" : "dead"}" data-addr="${esc(s.addr)}">
      <td><button class="star ${F.includes(s.addr) ? "on" : ""}" data-fav="${esc(s.addr)}">★</button></td>
      <td class="nm"><b>${s.pw ? '<span class="lock">🔒</span> ' : ""}${esc(s.name)}</b><small>${esc(s.addr)}${s.ok && s.motd ? " · " + esc(s.motd) : ""}</small></td>
      <td class="pl">${s.ok ? `${s.players}/${s.max}<span class="bar"><i style="width:${s.max ? (100 * s.players) / s.max : 0}%"></i></span>` : `<span class="ms">${t("on.offline")}</span>`}</td>
      <td>${s.ok ? pingHtml(s.ping) : ""}</td></tr>`).join("")
    : `<tr><td colspan="4" class="empty">${t("on.empty")}</td></tr>`;
  const s = S.servers.find((x) => x.addr === S.sel);
  $("#srvDet").innerHTML = s ? `<h3>${t("on.c.name")}</h3><div class="big">${esc(s.name)}</div>
      <div style="display:flex;gap:8px;flex-wrap:wrap">${s.ok ? `<span class="chip">${s.players}/${s.max} ${t("on.c.players")}</span><span class="chip">${s.ping} ms</span>` : `<span class="chip o">${t("on.offline")}</span>`}${s.pw ? `<span class="chip">${t("on.pwTitle")}</span>` : ""}${s.ok && s.players >= s.max ? `<span class="chip o">${t("on.full")}</span>` : ""}</div>
      <div class="motd">${esc(s.motd || s.addr)}</div>
      <ol class="how"><li>${t("on.how1")}</li><li>${t("on.how2")}</li><li>${t("on.how3")}</li></ol>
      <div id="opBoard">${S.board && S.board.addr === s.addr ? S.board.html : ""}</div>
      <button class="btn hot" id="joinBtn" style="width:100%;justify-content:center;margin-top:16px" ${s.ok ? "" : "disabled"}>${t("on.joinPlay")}</button>`
    : `<h3>${t("on.c.name")}</h3><div class="note">${t("on.pick")}</div>`;
  const jb = $("#joinBtn"); if (jb) jb.onclick = () => join(s);
  if (s && s.ok && API.queryBoard && renderOnline.boardFor !== s.addr) {
    renderOnline.boardFor = s.addr;
    API.queryBoard(s.addr).then((top) => {
      const html = top && top.length ? `<h3 style="margin-top:14px">${t("on.top")}</h3><ol class="top">${top.map((e) => `<li><b>${esc(e.name)}</b><span>${e.xp} XP · ${e.kills} ${t("on.kills")}</span></li>`).join("")}</ol>` : "";
      S.board = { addr: s.addr, html };
      const el = $("#opBoard"); if (el && S.sel === s.addr) el.innerHTML = html;
    }).catch(() => {});
  }
  $("#hBtn").textContent = S.hosting ? t("on.h.stop") : t("on.h.start");
  $("#hBtn").className = "btn " + (S.hosting ? "" : "hot");
  $("#hNote").textContent = S.hosting ? t("on.h.running") : t("on.h.note");
}
async function join(s, pw) {
  const name = $("#playerName").value.trim();
  if (!name) { toast(t("toast.noName")); $("#playerName").focus(); return; }
  if (s.pw && pw === undefined) { $("#pwInp").value = S.ini.online_server === s.addr ? S.ini.online_passwort || "" : ""; $("#pwDlg").classList.add("on"); $("#pwInp").focus(); join.pending = s; return; }
  S.ini.online_name = name; S.ini.online_server = s.addr; S.ini.online_passwort = s.pw ? pw : "";
  await persist(false);
  toast(t("toast.join", { s: s.name }));
  play();
}
async function toggleFav(addr) {
  const F = favs(); const i = F.indexOf(addr);
  if (i >= 0) F.splice(i, 1); else F.push(addr);
  S.ini.online_favoriten = F.join(",");
  await persist(false); renderOnline();
}
async function toggleHost() {
  try {
    if (S.hosting) { await API.hostStop(); S.hosting = false; }
    else {
      const max = Math.max(2, Math.min(16, parseInt($("#hMax").value, 10) || 16));
      await API.hostStart({ name: $("#hName").value.trim() || "TS2 Server", password: $("#hPw").value, max, motd: "" });
      S.hosting = true;
      if (!S.extra.includes("localhost")) S.extra.push("localhost");
      setTimeout(loadServers, 800);
    }
  } catch (e) { toast(String(e)); }
  renderOnline();
}

// ---------------- News ----------------
function fmtDate(d) { const x = new Date(d); return isNaN(x) ? d : x.toLocaleDateString(lang === "de" ? "de-DE" : "en-GB", { day: "2-digit", month: "short", year: "numeric" }); }
function plain(md) { return String(md || "").replace(/[#*`>_]/g, "").replace(/\[([^\]]+)\]\([^)]+\)/g, "$1").replace(/\s+/g, " ").trim(); }
function md(src) {
  const inline = (s) => esc(s).replace(/`([^`]+)`/g, "<code>$1</code>").replace(/\*\*([^*]+)\*\*/g, "<b>$1</b>")
    .replace(/\[([^\]]+)\]\((https?:\/\/[^)\s]+)\)/g, '<a data-url="$2">$1</a>');
  let out = "", ul = false;
  for (const raw of String(src || "").split(/\r?\n/)) {
    const l = raw.trim();
    if (/^[-*] /.test(l)) { if (!ul) { out += "<ul>"; ul = true; } out += `<li>${inline(l.slice(2))}</li>`; continue; }
    if (ul) { out += "</ul>"; ul = false; }
    if (!l) continue;
    if (/^#{1,6} /.test(l)) out += `<div class="h3" style="margin:12px 0 6px">${inline(l.replace(/^#+ /, ""))}</div>`;
    else if (/^<!--/.test(l) || /^\|/.test(l) || /^>/.test(l)) out += `<p>${inline(l.replace(/^[>|]\s?/, "").replace(/\|/g, " · "))}</p>`;
    else out += `<p>${inline(l)}</p>`;
  }
  return out + (ul ? "</ul>" : "");
}
async function loadNews() {
  const items = [];
  try { for (const n of JSON.parse(await API.fetchText(API.URLS.news))) items.push({ ...n, src: "news" }); } catch (_) {}
  try {
    const rel = JSON.parse(await API.fetchText(API.URLS.releases)).filter((r) => !r.draft);
    for (const r of rel) items.push({ date: r.published_at, title: r.name || r.tag_name, tag: "Release " + r.tag_name, body: r.body, url: r.html_url, src: "rel" });
    const latest = rel.find((r) => !r.prerelease);
    if (latest && cmpVer(latest.tag_name, S.status.version) > 0) S.update = latest.tag_name;
  } catch (_) {}
  items.sort((a, b) => new Date(b.date) - new Date(a.date));
  S.news = items; renderNews(); renderHome();
}
function cmpVer(a, b) { const p = (v) => String(v).replace(/^v/, "").split(/[.-]/).map((x) => parseInt(x, 10) || 0); const A = p(a), B = p(b);
  for (let i = 0; i < 3; i++) if ((A[i] || 0) !== (B[i] || 0)) return (A[i] || 0) - (B[i] || 0); return 0; }
function renderNews() {
  const el = $("#newsList");
  if (!S.news.length) { el.innerHTML = `<div class="panel note">${t("news.offline")}</div>`; return; }
  el.innerHTML = S.news.map((n) => `<article class="panel item"><div class="when">${esc(fmtDate(n.date))} · ${esc(n.tag || "News")}</div>
      <h2>${esc(n.title)}</h2><div class="body">${md(n.body)}</div>
      ${n.url ? `<div class="more"><a class="link" data-url="${esc(n.url)}">${t("news.open")}</a></div>` : ""}</article>`).join("");
}

// ---------------- Einstellungen ----------------
const opt = (de, en) => (lang === "de" ? de : en);
function SETTINGS() {
  const win = S.status.platform === "windows";
  return {
    gfx: [
      { k: "aufloesung", type: "seg", label: "s.res", opts: [["9", opt("Auto (beste)", "Auto (best)")], ["1", "1x"], ["2", "2x 720p"], ["3", "3x 1080p"], ["4", "4x 1440p"], ["6", "6x 4K"], ["8", "8x 5K"]] },
      { k: "bildformat", type: "seg", label: "s.asp", opts: [["4", opt("Auto (passt immer)", "Auto (fits any screen)")], ["0", "16:9"], ["1", opt("Wie im Spiel", "As in game")], ["2", "4:3"], ["3", opt("Gestreckt", "Stretched")]] },
      { k: "texturfilter", type: "seg", label: "s.filt", opts: [["0", opt("Pixelig", "Nearest")], ["1", opt("Bilinear", "Bilinear")], ["2", opt("Wie PS2", "As PS2")], ["3", opt("Ohne Sprites", "No sprites")]] },
      { k: "anisotrop", type: "seg", label: "s.aniso", opts: [["0", opt("Aus", "Off")], ["2", "2x"], ["4", "4x"], ["8", "8x"], ["16", "16x"]] },
      { k: "trilinear", type: "seg", label: "s.tri", opts: [["0", "Auto"], ["1", opt("Wie PS2", "As PS2")], ["2", opt("Erzwungen", "Forced")]] },
      { k: "vsync", type: "tog", label: "s.vsync" }, { k: "hd_text", type: "tog", label: "s.hdtext" },
    ],
    fx: [
      { k: "cas", type: "seg", label: "s.cas", opts: [["0", opt("Aus", "Off")], ["1", opt("Schärfen", "Sharpen")], ["2", opt("Schärfen + Skalieren", "Sharpen + upscale")]] },
      { k: "cas_staerke", type: "range", label: "s.cass", min: 0, max: 100, unit: "%" },
      { k: "fxaa", type: "tog", label: "s.fxaa" },
      { k: "tv_shader", type: "seg", label: "s.tv", opts: [["0", opt("Kein", "None")], ["1", "Scanlines"], ["2", "Diagonal"], ["3", opt("Dreieck", "Triangular")], ["4", opt("Welle", "Wave")], ["5", "CRT"], ["6", "4x RGSS"], ["7", "NxAGSS"]] },
      { k: "zielfernrohr", type: "seg", label: "s.scope", opts: [["0", "Original"], ["1", opt("Klar", "Clear")], ["2", opt("Extra klar", "Extra clear")]] },
      { k: "dithering", type: "seg", label: "s.dith", opts: [["0", opt("Aus", "Off")], ["1", opt("Skaliert", "Scaled")], ["2", "Original"]] },
      ...(win ? [] : [{ k: "effekte", type: "seg", label: "s.fx", opts: [["aus", opt("Aus", "Off")], ["original-plus", "Original+"], ["klar", opt("Klar", "Clear")], ["filmisch", opt("Filmisch", "Cinematic")]] }]),
    ],
    audio: [
      { k: "lautstaerke", type: "range", label: "s.vol", min: 0, max: 200, unit: "%" },
      { k: "vibration", type: "tog", label: "s.rumble" },
      { k: "vibration_staerke", type: "range", label: "s.rumblep", min: 0, max: 100, unit: "%" },
    ],
    input: [
      { k: "maus_zielen", type: "tog", label: "s.mouse" },
      { k: "maus_empfindlichkeit", type: "range", label: "s.msens", min: 1, max: 100, div: 10, unit: "" },
      { k: "maus_invertiert", type: "tog", label: "s.minv", nosub: true },
      { k: "maus_zielmodus_dauer", type: "tog", label: "s.mmode" },
      { k: "fadenkreuz", type: "seg", label: "s.xhair", opts: [["0", opt("Kreuz", "Cross")], ["1", opt("Punkt", "Dot")], ["2", opt("Kreis", "Circle")], ["3", opt("Aus", "Off")]] },
      { k: "blickwinkel_max", type: "range", label: "s.pitch", min: 40, max: 85, unit: "°" },
      { k: "auto_ziel", type: "tog", label: "s.aaim" },
      { k: "pad_modern", type: "tog", label: "s.padm" },
      { k: "pad_tempo", type: "range", label: "s.pads", min: 20, max: 300, unit: " %" },
      { k: "alles_frei", type: "tog", label: "s.unlock" },
      { k: "leben_unendlich", type: "tog", label: "s.chp" },
      { k: "ruestung_unendlich", type: "tog", label: "s.car" },
      { k: "munition_unendlich", type: "tog", label: "s.cam" },
      { k: "zoom_empfindlichkeit", type: "range", label: "s.zoom", min: 5, max: 200, unit: "%" },
      { type: "note", label: "s.keys" },
    ],
    op: [
      { k: "online_plus_effekte", type: "tog", label: "s.imp" },
      { k: "treffermarker", type: "tog", label: "s.mark" },
      { k: "treffer_klang", type: "tog", label: "s.snd" },
      { k: "schaden_stoss", type: "range", label: "s.kick", min: 0, max: 100, unit: "%" },
      { type: "note", label: "s.opnote" },
    ],
    launch: [
      { k: "@widescreen", type: "tog", label: "q.ws" }, { k: "@fullscreen", type: "tog", label: "q.fs" }, { k: "@letterbox", type: "tog", label: "q.lb" },
      { k: "fps_anzeige", type: "tog", label: "q.fps" },
      { k: "sprache", type: "seg", label: "s.lang", opts: [["-1", "Default"], ["0", "English"], ["1", "Deutsch"]] },
      ...(win ? [{ k: "@renderer", type: "seg", label: "s.renderer", opts: [["3", "Direct3D 11"], ["14", "Vulkan"]] }] : []),
      { type: "actions" },
    ],
  };
}
const getV = (k) => (k[0] === "@" ? S.start[k.slice(1)] : iv(k));
const isOn = (k) => { const v = getV(k); return v === true || v === "1" || v === "true"; };
// The in-game menu (ESC/F1) writes the same settings file. After the game exits (and when the window gets the focus
// back) the file is read again, so both always show the same values; settings changed here but not saved yet stay.
async function reloadSettings() {
  if (S.running) return;
  let r; try { r = await API.settingsLoad(); } catch (_) { return; }
  const fresh = r.ini || {};
  for (const k of Object.keys(fresh)) { if (fresh[k] === "true") fresh[k] = "1"; else if (fresh[k] === "false") fresh[k] = "0"; }
  const base = JSON.parse(S.saved || "[{},{}]");
  for (const k of Object.keys(fresh)) if (S.ini[k] === base[0][k] || !(k in S.ini)) S.ini[k] = fresh[k];
  S.saved = JSON.stringify([{ ...base[0], ...fresh }, base[1]]);
  renderSettings(); dirty();
}

function setV(k, v) {
  if (k[0] === "@") { const n = k.slice(1); S.start[n] = typeof DEF_START[n] === "boolean" ? !!v : v; if (n === "fullscreen") S.ini.vollbild = v ? "1" : "0"; }
  else S.ini[k] = typeof v === "boolean" ? (v ? "1" : "0") : String(v);   // toggles: the settings file uses 1/0 ("true" was read as off)
  dirty();
}
function dirty() { $("#saveMsg").textContent = JSON.stringify([S.ini, S.start]) !== S.saved ? t("set.unsaved") : ""; }
function renderSettings() {
  const tabs = [["gfx", "tab.gfx"], ["fx", "tab.fx"], ["audio", "tab.audio"], ["input", "tab.input"], ["op", "tab.op"], ["launch", "tab.launch"]];
  $("#setTabs").innerHTML = tabs.map(([id, k]) => `<button data-tab="${id}" class="${S.setTab === id ? "on" : ""}">${t(k)}</button>`).join("");
  const list = SETTINGS()[S.setTab];
  const half = Math.ceil(list.length / 2);
  const col = (items) => `<div class="panel">${items.map(field).join("")}</div>`;
  $("#setBody").innerHTML = `<div class="cols">${col(list.slice(0, half))}${list.length > 1 ? col(list.slice(half)) : ""}</div>`;
  $$("#setBody input[type=range]").forEach(paintRange);
}
function field(f) {
  if (f.type === "note") return `<p class="note">${t(f.label)}</p>`;
  if (f.type === "actions") return `<div class="field"><div class="lab"><span>${t("s.setupAgain")}</span></div><small class="note">${t("s.setupAgain.s")}</small>
      <div class="seg" style="margin-top:10px"><button data-act="setup">${t("s.setupAgain")}</button>${S.status.platform !== "windows" ? `<button data-act="shortcut">${t("s.shortcut")}</button>` : ""}
      <button data-act="folder">${t("s.folder")}</button><button data-act="config">${t("s.cfg")}</button><button data-act="log">${t("s.log")}</button></div></div>`;
  if (f.type === "tog") { const sub = L[lang][f.label + ".s"] && !f.nosub ? `<small>${t(f.label + ".s")}</small>` : "";
    return `<div class="tog ${isOn(f.k) ? "on" : ""}" data-k="${f.k}"><span class="t">${t(f.label)}${sub}</span><span class="sw"></span></div>`; }
  if (f.type === "seg") { const v = String(getV(f.k));
    return `<div class="field"><div class="lab"><span>${t(f.label)}</span></div><div class="seg" data-k="${f.k}">${f.opts.map(([a, b]) => `<button data-v="${a}" class="${a === v ? "on" : ""}">${esc(b)}</button>`).join("")}</div></div>`; }
  if (f.type === "range") { const v = parseInt(getV(f.k), 10) || 0;
    return `<div class="field"><div class="lab"><span>${t(f.label)}</span></div><div class="range"><input type="range" min="${f.min}" max="${f.max}" value="${v}" data-k="${f.k}" data-div="${f.div || 1}" data-unit="${f.unit}"><output>${fmtR(v, f.div || 1, f.unit)}</output></div></div>`; }
  return "";
}
const fmtR = (v, d, u) => (d > 1 ? (v / d).toFixed(1) : v) + u;
function paintRange(r) { r.style.setProperty("--p", ((r.value - r.min) / (r.max - r.min)) * 100 + "%"); }

async function persist(showToast = true) {
  try {
    await API.settingsSave(S.ini, S.start);
    S.saved = JSON.stringify([S.ini, S.start]); dirty();
    if (showToast) { toast(t("toast.saved")); $("#saveMsg").textContent = t("set.saved"); }
  } catch (e) { toast(String(e)); }
}

// ---------------- Laden / Ereignisse ----------------
async function refreshStatus() { try { S.status = await API.status(); } catch (e) { toast(String(e)); } renderHome(); }
function renderAll() { renderHome(); renderOnline(); renderNews(); renderSettings(); }

function wire() {
  document.addEventListener("click", (e) => {
    const g = e.target.closest("[data-go]"); if (g) { if (g.dataset.go === "upd" && S.updInfo) showUpdateDialog(S.updInfo); else go(g.dataset.go); return; }
    const u = e.target.closest("[data-url]"); if (u) { API.openUrl(u.dataset.url); return; }
    const nb = e.target.closest("#nav button[data-view]"); if (nb) { go(nb.dataset.view); return; }
    const q = e.target.closest("#quick .tog"); if (q) { quickToggle(q.dataset.q); return; }
    const ed = e.target.closest("#edSwitch [data-edsel]"); if (ed) { S.start.edition = ed.dataset.edsel; persist(false); renderHome(); return; }
    const ms = e.target.closest("[data-srv]"); if (ms) { S.sel = ms.dataset.srv; go("online"); renderOnline(); return; }
    const fav = e.target.closest("[data-fav]"); if (fav) { e.stopPropagation(); toggleFav(fav.dataset.fav); return; }
    const row = e.target.closest("#srvBody tr[data-addr]"); if (row) { S.sel = row.dataset.addr; renderOnline(); return; }
    const th = e.target.closest("th[data-s]"); if (th) { S.sort = { k: th.dataset.s, d: S.sort.k === th.dataset.s ? -S.sort.d : th.dataset.s === "ping" || th.dataset.s === "name" ? 1 : -1 }; renderOnline(); return; }
    const fb = e.target.closest("#srvFilt button"); if (fb) { const f = fb.dataset.f; S.filt.has(f) ? S.filt.delete(f) : S.filt.add(f); renderOnline(); return; }
    const tb = e.target.closest("#setTabs button"); if (tb) { S.setTab = tb.dataset.tab; renderSettings(); return; }
    const tg = e.target.closest("#setBody .tog"); if (tg) { setV(tg.dataset.k, !isOn(tg.dataset.k)); renderSettings(); return; }
    const sg = e.target.closest("#setBody .seg[data-k] button"); if (sg) { const k = sg.parentElement.dataset.k; setV(k, sg.dataset.v); if (k === "sprache") { lang = detectLang(); applyLang(); } else renderSettings(); return; }
    const ac = e.target.closest("[data-act]"); if (ac) { const a = ac.dataset.act; if (a === "setup") openSetup(); else if (a === "shortcut") API.shortcut().then(() => toast("✓")); else API.open(a); }
  });
  document.addEventListener("input", (e) => {
    const r = e.target.closest("#setBody input[type=range]");
    if (r) { paintRange(r); r.nextElementSibling.textContent = fmtR(+r.value, +r.dataset.div, r.dataset.unit); setV(r.dataset.k, r.value); }
    if (e.target.id === "srvSearch") renderOnline();
  });
  $("#playBtn").onclick = play;
  $("#srvRefresh").onclick = loadServers;
  $("#srvAdd").onclick = () => { const a = $("#srvDirect").value.trim(); if (!a) return; if (!S.extra.includes(a)) S.extra.push(a); S.sel = a; $("#srvDirect").value = ""; loadServers(); };
  $("#srvDirect").addEventListener("keydown", (e) => e.key === "Enter" && $("#srvAdd").click());
  $("#playerName").addEventListener("change", (e) => { S.ini.online_name = e.target.value.trim(); persist(false); });
  $("#hBtn").onclick = toggleHost;
  $("#pwOk").onclick = () => { $("#pwDlg").classList.remove("on"); join(join.pending, $("#pwInp").value); };
  $("#pwCancel").onclick = () => $("#pwDlg").classList.remove("on");
  $("#pwInp").addEventListener("keydown", (e) => e.key === "Enter" && $("#pwOk").click());
  $("#setSave").onclick = () => persist(true);
  $("#setReset").onclick = () => { const keep = ["online_name", "online_server", "online_passwort", "online_favoriten", "sprache", "texturpakete"];
    for (const k of Object.keys(DEF)) if (!keep.includes(k)) S.ini[k] = DEF[k]; S.start = { ...DEF_START, renderer: S.start.renderer, editions: S.start.editions, edition: S.start.edition }; renderSettings(); dirty(); };
  $("#langBtn").onclick = () => { lang = lang === "de" ? "en" : "de"; S.ini.sprache = lang === "de" ? "1" : "0"; persist(false); applyLang(); };
  $("#wMin").onclick = () => API.minimize(); $("#wClose").onclick = () => API.close();
  document.addEventListener("keydown", (e) => {
    if (e.target.closest("input")) return;
    if (e.key === "Enter" && $("#v-home").classList.contains("on") && !$(".overlay.on")) play();
    if (e.key === "Escape") $$(".overlay.on").forEach((o) => o.id !== "setup" && o.classList.remove("on"));
  });
  API.on("setup", setupEvent);
  API.on("game", (g) => {
    S.running = g.state === "running"; renderHome();
    if (g.state === "exited") reloadSettings();
    if (g.state === "exited" && g.code && g.tail) {
      $("#setupBox").innerHTML = `<h3 class="err">${t("toast.exit", { c: g.code })}</h3><div class="logbox" style="max-height:260px">${esc(g.tail)}</div>
        <div style="display:flex;gap:10px;justify-content:flex-end;margin-top:14px"><button class="btn" data-act="log">${t("s.log")}</button><button class="btn hot" id="exOk">OK</button></div>`;
      $("#setup").classList.add("on"); $("#exOk").onclick = () => $("#setup").classList.remove("on");
    } else if (g.state === "exited" && g.code) toast(t("toast.exit", { c: g.code }), 6000);
  });
}

// ---------------- Automatische Updates ----------------
// Normal updates install by themselves on start (only changed files, checked by hash, saves backed up first).
// Updates that need a new setup ("critical") ask first. After a "rebuild" update the game is linked again with the
// disc image remembered from the last setup.
const U = { active: false, pct: 0, step: "", t0: 0, ver: "", dlDone: 0, dlTotal: 0, dlT0: 0, timer: 0 };
const mb = (n) => (n / 1048576).toFixed(n >= 104857600 ? 0 : 1) + " MB";
let updListener = false;
async function autoUpdate() {
  let u; try { u = await API.updateCheck(); } catch (_) { return; }
  if (!u || !u.available || S.running || P.active || U.active) return;
  S.update = "v" + u.version; S.updInfo = u; renderHome();
  showUpdateDialog(u);
}
// pop-up: update now or play the current version first (a critical update can still be put off, but not skipped when playing)
function showUpdateDialog(u) {
  const txt = u.critical ? t("upd.crit", { v: u.version }) : t("upd.ask", { v: u.version, mb: mb(u.bytes || 0) });
  const canOld = S.status.ready;
  $("#setupBox").innerHTML = `<h3>${t("upd.title")}: v${esc(u.version)}</h3><p>${esc(txt)}</p>${u.notes ? `<div class="sub" style="margin-top:8px"><b>${t("upd.notes")}</b></div><div class="body" style="max-height:240px;overflow:auto">${md(u.notes)}</div>` : ""}
    <div style="display:flex;gap:10px;justify-content:flex-end;margin-top:14px"><button class="btn ghost" id="updLater">${t("upd.later")}</button>${canOld ? `<button class="btn" id="updOld">${t("upd.playOld")}</button>` : ""}<button class="btn hot" id="updNow">${t("upd.install")}</button></div>`;
  $("#setup").classList.add("on");
  const close = () => $("#setup").classList.remove("on");
  $("#updLater").onclick = close;
  if (canOld) $("#updOld").onclick = () => { close(); playNow(); };
  $("#updNow").onclick = () => { close(); installUpdate(u); };
}
function showUpdateDone(v) {
  $("#setupBox").innerHTML = `<h3>${t("upd.finTitle")}</h3><p>${esc(t("upd.fin", { v: "v" + v }))}</p>
    <div style="display:flex;gap:10px;justify-content:flex-end;margin-top:14px"><button class="btn ghost" id="finClose">${t("su.close")}</button><button class="btn hot" id="finPlay">${t("play")}</button></div>`;
  $("#setup").classList.add("on");
  $("#finClose").onclick = () => $("#setup").classList.remove("on");
  $("#finPlay").onclick = () => { $("#setup").classList.remove("on"); play(); };
}
function installUpdate(u) {
  if (U.active) return;
  go("home");
  Object.assign(U, { active: true, pct: 0, step: "backup", t0: Date.now(), ver: u.version, dlDone: 0, dlTotal: 0, dlT0: 0 });
  clearInterval(U.timer); U.timer = setInterval(renderHome, 500); renderHome();
  const stop = () => { clearInterval(U.timer); U.active = false; renderHome(); };
  if (!updListener) { updListener = true; API.on("update", (e) => updateEvent(e)); }
  U.u = u;
  API.updateInstall().catch((er) => { stop(); toast(t("upd.fail", { e: String(er) }), 9000); });
}
async function updateEvent(e) {
  const u = U.u || {};
  if (e.kind === "step") { U.step = e.value; if (e.value === "download") U.dlT0 = Date.now(); }
  else if (e.kind === "pct") U.pct = Math.max(U.pct, Math.min(100, parseInt(e.value, 10) || 0));
  else if (e.kind === "dl") { const [d, tot] = String(e.value).split("/").map(Number); U.dlDone = d || 0; U.dlTotal = tot || 0; }
  else if (e.kind === "done") {
    clearInterval(U.timer); U.active = false; U.pct = 100;
    S.update = ""; S.updInfo = null; await refreshStatus(); renderHome();
    const flags = String(e.value || "");
    if (flags.includes("rebuild")) {
      const iso = await API.lastIso();
      if (iso) { toast(t("upd.rebuild"), 8000); go("home");
        Object.assign(P, { active: true, pct: 0, step: "", t0: Date.now(), stepT0: Date.now(), failed: "", log: "" });
        clearInterval(P.timer); P.timer = setInterval(renderHome, 500); renderHome();
        try { await API.setup(iso); } catch (er) { setupEvent({ kind: "error", value: String(er) }); }
      } else openSetup();
      return;
    }
    if (flags.includes("restart")) {
      try { localStorage.setItem("ts2UpdDone", u.version || ""); } catch (_) {}
      toast(t("upd.restart"), 3000); setTimeout(() => API.restart(), 2500); return;
    }
    showUpdateDone(u.version || "");
  } else if (e.kind === "error") {
    clearInterval(U.timer); U.active = false; renderHome();
    toast(t("upd.fail", { e: e.value }), 9000);
  }
  if (U.active) renderHome();
}

(async function init() {
  wire();
  window.addEventListener("focus", () => { reloadSettings(); });
  try { const r = await API.settingsLoad(); S.ini = r.ini || {}; for (const k of Object.keys(S.ini)) { if (S.ini[k] === "true") S.ini[k] = "1"; else if (S.ini[k] === "false") S.ini[k] = "0"; } S.start = { ...DEF_START, ...(r.start || {}) }; S.start.editions = "both"; } catch (e) { S.start = { ...DEF_START }; }
  S.start.editions = "both";
  S.saved = JSON.stringify([S.ini, S.start]);
  $("#playerName").value = S.ini.online_name || "";
  lang = detectLang();
  try { S.hosting = await API.hostRunning(); } catch (_) {}
  await refreshStatus();
  applyLang();
  loadNews();
  loadServers();
  let fertig = ""; try { fertig = localStorage.getItem("ts2UpdDone") || ""; localStorage.removeItem("ts2UpdDone"); } catch (_) {}
  if (fertig) showUpdateDone(fertig); else setTimeout(autoUpdate, 1500);
})();
