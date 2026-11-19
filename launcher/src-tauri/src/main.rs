// TimeSplitters 2 Native Recomp - Launcher (Tauri). Setzt auf die vorhandenen Paket-Skripte auf:
//   Einrichtung: app/tools/release/first_setup.sh (Linux) bzw. app/toolchain/python + first_setup.py (Windows)
//   Spielen:     app/tools/ts2_run.sh (Linux) bzw. app/tools/release/TS2-Recomp.ps1 -Play (Windows)
// Einstellungen landen in derselben einstellungen.ini wie beim Spielmenü (ESC/F1).
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use serde::Serialize;
use std::collections::BTreeMap;
use std::io::{BufRead, BufReader};
use std::net::{ToSocketAddrs, UdpSocket};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::Mutex;
use std::time::{Duration, Instant};
use tauri::{AppHandle, Emitter, Manager, State};
use tauri_plugin_dialog::DialogExt;
use tauri_plugin_opener::OpenerExt;

const REPO_RAW: &str = "https://raw.githubusercontent.com/Destinity13/";
const REPO_API: &str = "https://api.github.com/repos/Destinity13/";
const DEFAULT_PORT: &str = "27713";

struct Launcher {
    root: PathBuf,
    host: Mutex<Option<Child>>,
    game: Mutex<bool>,
    setup: Mutex<bool>,
}

// ---------------------------------------------------------------- Pfade

// Running as AppImage (Steam Deck): AppRun sets PYTHONHOME, PYTHONPATH, LD_LIBRARY_PATH, PATH, GTK_* and more to point
// into the AppImage. Children (setup with the system python, the game, the server) must not inherit that: the setup
// failed with "Failed to import encodings module". Entries pointing into $APPDIR are removed, the rest is kept.
fn appimage_env(c: &mut Command) {
    let Some(appdir) = std::env::var_os("APPDIR").map(|v| v.to_string_lossy().to_string()).filter(|v| !v.is_empty()) else { return };
    for k in ["PYTHONHOME", "PYTHONPATH", "PYTHONDONTWRITEBYTECODE", "PERLLIB", "QT_PLUGIN_PATH", "GTK_THEME", "APPDIR", "APPIMAGE", "ARGV0", "OWD"] {
        c.env_remove(k);
    }
    for (k, v) in std::env::vars() {
        if !v.contains(&appdir) { continue; }
        let rest: Vec<&str> = v.split(':').filter(|e| !e.is_empty() && !e.starts_with(&appdir)).collect();
        if rest.is_empty() || !v.contains(':') { c.env_remove(&k); } else { c.env(&k, rest.join(":")); }
    }
}

// the launcher's own file: inside an AppImage current_exe() points into the temporary mount, the real file is $APPIMAGE
fn self_exe() -> Option<PathBuf> {
    #[cfg(target_os = "linux")]
    if let Some(a) = std::env::var_os("APPIMAGE") {
        let p = PathBuf::from(a);
        if p.is_file() { return p.canonicalize().ok().or(Some(p)); }
    }
    std::env::current_exe().ok().and_then(|p| p.canonicalize().ok())
}

fn find_root() -> PathBuf {
    if let Ok(r) = std::env::var("TS2_ROOT") {
        if !r.is_empty() {
            return PathBuf::from(r);
        }
    }
    // Windows: canonicalize() returns \\?\C:\... - the compiler and the scripts cannot use that form (include paths fail)
    let exe = self_exe().map(|p| {
        let t = p.to_string_lossy().to_string();
        match t.strip_prefix(r"\\?\") { Some(r) if !r.starts_with("UNC\\") => PathBuf::from(r), _ => p }
    });
    let mut d = exe.as_ref().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_else(|| PathBuf::from("."));
    // Root = the package's app/ folder (the launcher sits one level above it)
    for _ in 0..3 {
        if d.join("app").join("tools").join("release").is_dir() {
            return d.join("app");
        }
        if d.join("tools").join("release").is_dir() {
            return d;
        }
        match d.parent() {
            Some(p) => d = p.to_path_buf(),
            None => break,
        }
    }
    exe.and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_else(|| PathBuf::from("."))
}

fn cfg_dir() -> PathBuf {
    #[cfg(windows)]
    let base = std::env::var_os("APPDATA").map(PathBuf::from).unwrap_or_else(|| PathBuf::from("."));
    #[cfg(not(windows))]
    let base = match std::env::var_os("XDG_CONFIG_HOME").filter(|v| !v.is_empty()) {
        Some(x) => PathBuf::from(x),
        None => PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".config"),
    };
    let d = base.join("ts2-recomp");
    let _ = std::fs::create_dir_all(&d);
    d
}

// Packages built with the English settings file carry the marker app/SETTINGS_EN (the game then uses settings.ini with
// English keys). The UI keeps the internal (German) key names; they are translated here.
const KEYMAP: &[(&str, &str)] = &[
    ("aufloesung", "resolution"), ("bildformat", "aspect"), ("cas_staerke", "cas_strength"), ("farbe", "color_adjust"),
    ("helligkeit", "brightness"), ("kontrast", "contrast"), ("saettigung", "saturation"), ("texturfilter", "texture_filter"),
    ("anisotrop", "anisotropic"), ("vollbild", "fullscreen"), ("fps_anzeige", "show_fps"), ("vibration_staerke", "vibration_strength"),
    ("startmenue", "start_menu"), ("bildrate", "refresh_rate"), ("lautstaerke", "volume"), ("kamera_unschaerfe", "camera_blur"), ("treffer_toenung", "hit_tint"), ("lebensbogen", "health_arc"), ("weiche_schnitte", "soft_cuts"),
    ("nachzieheffekt", "motion_trail"), ("maus_zielen", "mouse_aim"), ("maus_empfindlichkeit", "mouse_sensitivity"),
    ("maus_invertiert", "mouse_invert"), ("maus_zielmodus_dauer", "aim_mode_hold"), ("sprache", "language"),
    ("eingabeanzeige", "show_input"), ("zoom_empfindlichkeit", "zoom_sensitivity"), ("belegung_version", "bindings_version"),
    ("effekte", "effects"), ("alles_frei", "unlock_all"), ("leicht_plus", "extended_easy"), ("menue_taste", "menu_key"), ("fruehere_stimmen", "earlier_voices"), ("mapmaker_gegner", "mapmaker_spawns"), ("pad_modern", "modern_gamepad_aim"), ("pad_tempo", "gamepad_aim_speed"), ("auto_ziel", "auto_aim"), ("leben_unendlich", "infinite_health"), ("ruestung_unendlich", "infinite_armor"), ("munition_unendlich", "infinite_ammo"), ("fadenkreuz", "crosshair"), ("zielfernrohr", "scope"), ("online_plus_effekte", "impact_effects"), ("schaden_stoss", "damage_kick"), ("treffermarker", "hit_markers"), ("treffer_klang", "hit_sound"), ("blickwinkel_max", "pitch_limit"), ("texturpakete", "texture_packs"), ("online_passwort", "online_password"), ("online_favoriten", "online_favorites"),
];
const FXMAP: &[(&str, &str)] = &[("aus", "off"), ("klar", "clear"), ("filmisch", "cinematic")];

fn english(root: &Path) -> bool {
    root.join("SETTINGS_EN").is_file()
}

fn to_internal(k: &str, v: &str) -> (String, String) {
    let k2 = KEYMAP.iter().find(|(_, e)| *e == k).map(|(d, _)| *d).unwrap_or(k);
    let v2 = if k2 == "effekte" { FXMAP.iter().find(|(_, e)| *e == v).map(|(d, _)| *d).unwrap_or(v) } else { v };
    (k2.to_string(), v2.to_string())
}

fn to_file(k: &str, v: &str, en: bool) -> (String, String) {
    if !en {
        return (k.to_string(), v.to_string());
    }
    let v2 = if k == "effekte" { FXMAP.iter().find(|(d, _)| *d == v).map(|(_, e)| *e).unwrap_or(v) } else { v };
    let k2 = KEYMAP.iter().find(|(d, _)| *d == k).map(|(_, e)| *e).unwrap_or(k);
    (k2.to_string(), v2.to_string())
}

fn ini_path(root: &Path) -> PathBuf {
    cfg_dir().join(if english(root) { "settings.ini" } else { "einstellungen.ini" })
}

// File to read: the current one, or the older German file when the new one does not exist yet
fn ini_read_path(root: &Path) -> PathBuf {
    let p = ini_path(root);
    if p.is_file() { p } else { cfg_dir().join("einstellungen.ini") }
}

fn hide_console(_c: &mut Command) {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        _c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
}

fn exe_name(n: &str) -> String {
    if cfg!(windows) { format!("{n}.exe") } else { n.to_string() }
}

// ---------------------------------------------------------------- Status

#[derive(Serialize)]
struct Status {
    platform: String,
    version: String,
    ready: bool,
    build: String,
    launcher: String,
    stale: bool,
    region: String,
}

// files the game binary is linked from; when one of them is newer than the binary (an update replaced it) the game has
// to be linked again before it is started
fn is_link_input(p: &str) -> bool {
    let p = p.replace('\\', "/");
    ["app/src/", "app/prebuilt/", "app/recomp/", "app/third_party/", "app/ts2_override.cpp"].iter().any(|x| p.starts_with(x))
}
fn newest_mtime(dir: &Path, depth: u32) -> Option<std::time::SystemTime> {
    let md = std::fs::metadata(dir).ok()?;
    if md.is_file() { return md.modified().ok(); }
    if depth == 0 { return None; }
    std::fs::read_dir(dir).ok()?.flatten().filter_map(|e| newest_mtime(&e.path(), depth - 1)).max()
}
fn build_stale(root: &Path) -> bool {
    let Some(n) = current_build(root) else { return false };
    let bin = root.join("build").join("ts2").join(exe_name(&format!("ps2EntryRunner_{n}")));
    let Ok(built) = std::fs::metadata(&bin).and_then(|m| m.modified()) else { return false };
    let src = [root.join("src"), root.join("prebuilt").join("lib"), root.join("ts2_override.cpp")].iter().filter_map(|d| newest_mtime(d, 3)).max();
    // two minutes of slack: files written by the setup itself right before the link step
    src.map(|t| t > built + Duration::from_secs(120)).unwrap_or(false)
}

fn platform() -> String {
    if cfg!(windows) {
        return "windows".into();
    }
    let vend = std::fs::read_to_string("/sys/devices/virtual/dmi/id/board_vendor").unwrap_or_default();
    if vend.starts_with("Valve") || std::env::var("SteamDeck").map(|v| v == "1").unwrap_or(false) {
        "steamdeck".into()
    } else {
        "linux".into()
    }
}

fn game_version(root: &Path) -> String {
    if let Ok(v) = std::fs::read_to_string(root.join("..").join("VERSION")) {
        let v = v.trim();
        if !v.is_empty() {
            return v.trim_start_matches('v').to_string();
        }
    }
    // Fallback: Paketordner "TS2-Recomp-<version>-linux-x86_64"
    let name = root.parent().and_then(|p| p.file_name()).map(|n| n.to_string_lossy().to_string()).unwrap_or_default();
    if let Some(rest) = name.strip_prefix("TS2-Recomp-") {
        if let Some(i) = rest.find("-linux").or_else(|| rest.find("-windows")) {
            return rest[..i].to_string();
        }
    }
    "?".into()
}

fn current_build(root: &Path) -> Option<String> {
    let n = std::fs::read_to_string(root.join("build").join("ts2").join("CURRENT")).ok()?;
    let n = n.trim().to_string();
    let bin = root.join("build").join("ts2").join(exe_name(&format!("ps2EntryRunner_{n}")));
    bin.is_file().then_some(n)
}

// region of the set-up disc ("USA" or "PAL"), written by the setup to build/region.txt; empty before the first setup
fn disc_region(root: &Path) -> String {
    std::fs::read_to_string(root.join("build").join("region.txt")).ok()
        .and_then(|s| s.lines().next().map(|l| l.trim().to_string()))
        .filter(|r| r == "USA" || r == "PAL").unwrap_or_default()
}

#[tauri::command]
fn status(st: State<Launcher>) -> Status {
    let b = current_build(&st.root);
    Status {
        platform: platform(),
        version: game_version(&st.root),
        ready: b.is_some(),
        build: b.unwrap_or_default(),
        launcher: env!("CARGO_PKG_VERSION").into(),
        stale: build_stale(&st.root),
        region: disc_region(&st.root),
    }
}

// ---------------------------------------------------------------- Einstellungen

// Schlüssel, die der Launcher schreiben darf (gleiche Namen wie im Spielmenü, Bridge.cpp Cfg::load/save)
const KEYS: &[&str] = &[
    "aufloesung", "bildformat", "bildformat_v2", "alles_frei", "pad_modern", "pad_tempo", "auto_ziel", "leben_unendlich", "ruestung_unendlich", "munition_unendlich", "fxaa", "cas", "cas_staerke", "dithering", "tv_shader", "farbe", "helligkeit", "kontrast",
    "saettigung", "texturfilter", "anisotrop", "vollbild", "fps_anzeige", "vibration", "vibration_staerke", "vsync",
    "lautstaerke", "kamera_unschaerfe", "treffer_toenung", "lebensbogen", "weiche_schnitte", "nachzieheffekt", "startmenue", "effekte", "trilinear", "hd_text", "maus_zielen",
    "maus_empfindlichkeit", "maus_invertiert", "maus_zielmodus_dauer", "sprache", "eingabeanzeige", "zoom_empfindlichkeit", "fadenkreuz", "blickwinkel_max",
    "zielfernrohr", "online_plus_effekte", "schaden_stoss", "treffermarker", "treffer_klang",
    "texturpakete", "online_server", "online_name", "online_passwort", "online_favoriten",
    // Crystal Cut (same keys as the game menu and the port): restored content, extended easy, voices, MapMaker spawns
    "more_arcade_maps", "planet_x_arcade", "chicago_spawns", "more_characters", "restored_names", "more_modes", "thief_teams",
    "skating", "gothic_tileset", "deagle_texture", "story_enemies", "mapmaker_story_coop", "leicht_plus", "fruehere_stimmen", "mapmaker_gegner", "menue_taste",
];

#[derive(Serialize, serde::Deserialize, Clone)]
struct StartOpts {
    widescreen: bool,
    fullscreen: bool,
    letterbox: bool,
    renderer: String,
    // installed editions: "original", "onlineplus" or "both"; edition = the one the Play button starts
    #[serde(default = "ed_default")]
    editions: String,
    #[serde(default = "ed_default")]
    edition: String,
    // game language for the PAL disc (en, fr, de, es, it); the USA disc is English only. Kept in launcher.conf,
    // the in-game menu rewrites the settings file with its own keys only.
    #[serde(default = "gl_default")]
    game_lang: String,
}
fn ed_default() -> String { "original".into() }
fn gl_default() -> String { "en".into() }
fn gl_ok(v: &str) -> bool { ["en", "fr", "de", "es", "it"].contains(&v) }

#[derive(Serialize)]
struct Settings {
    ini: BTreeMap<String, String>,
    start: StartOpts,
}

fn read_kv(p: &Path) -> BTreeMap<String, String> {
    let mut m = BTreeMap::new();
    if let Ok(t) = std::fs::read_to_string(p) {
        for l in t.lines() {
            let l = l.trim_end_matches('\r');
            if l.starts_with('#') || l.starts_with("taste_") || l.starts_with("key_") {
                continue;
            }
            if let Some((k, v)) = l.split_once('=') {
                let (k, v) = to_internal(k.trim(), v);
                m.insert(k, v);
            }
        }
    }
    m
}

fn read_start(root: &Path) -> StartOpts {
    let kv = read_kv(&cfg_dir().join("launcher.conf"));
    let on = |k: &str, d: bool| kv.get(k).map(|v| v.trim() == "1").unwrap_or(d);
    let renderer = std::fs::read_to_string(root.join("build").join("renderer.txt"))
        .ok()
        .map(|s| s.trim().to_string())
        .filter(|s| s == "3" || s == "14")
        .unwrap_or_else(|| "3".into());
    StartOpts {
        widescreen: on("TS2_WIDESCREEN", true),
        fullscreen: on("PS2XGS_FULLSCREEN", false),
        letterbox: on("TS2_LETTERBOX", false),
        renderer,
        editions: kv.get("TS2_EDITIONEN").map(|v| v.trim().to_string()).filter(|v| v == "original" || v == "onlineplus" || v == "both").unwrap_or_else(ed_default),
        edition: kv.get("TS2_EDITION").map(|v| v.trim().to_string()).filter(|v| v == "original" || v == "onlineplus").unwrap_or_else(ed_default),
        game_lang: kv.get("TS2_GAME_LANGUAGE").map(|v| v.trim().to_string()).filter(|v| gl_ok(v)).unwrap_or_else(gl_default),
    }
}

// Profiles: complete copies of the settings file in <settings dir>/profiles/<name>.ini, the same folder the game menu
// (Crystal Cut tab) uses, so a profile saved in one place shows up in the other.
fn profile_dir() -> PathBuf { let d = cfg_dir().join("profiles"); let _ = std::fs::create_dir_all(&d); d }
fn profile_name(n: &str) -> Result<String, String> {
    let o: String = n.chars().filter(|c| c.is_alphanumeric() || *c == '-' || *c == '_' || *c == ' ').take(40).collect();
    let o = o.trim().to_string();
    if o.is_empty() { Err("invalid profile name".into()) } else { Ok(o) }
}
#[tauri::command]
fn profile_list() -> Vec<String> {
    let mut v: Vec<String> = std::fs::read_dir(profile_dir()).map(|r| r.filter_map(|e| e.ok()).filter_map(|e| {
        let p = e.path(); if p.extension().map(|x| x == "ini").unwrap_or(false) { p.file_stem().map(|s| s.to_string_lossy().to_string()) } else { None }
    }).collect()).unwrap_or_default();
    v.sort(); v
}
#[tauri::command]
fn profile_save(st: State<Launcher>, name: String) -> Result<(), String> {
    let n = profile_name(&name)?;
    std::fs::copy(ini_read_path(&st.root), profile_dir().join(format!("{n}.ini"))).map(|_| ()).map_err(|e| e.to_string())
}
#[tauri::command]
fn profile_load(st: State<Launcher>, name: String) -> Result<(), String> {
    let n = profile_name(&name)?;
    let text = std::fs::read_to_string(profile_dir().join(format!("{n}.ini"))).map_err(|e| e.to_string())?;
    write_atomic(&ini_path(&st.root), &text)
}
#[tauri::command]
fn profile_delete(name: String) -> Result<(), String> {
    let n = profile_name(&name)?;
    std::fs::remove_file(profile_dir().join(format!("{n}.ini"))).map_err(|e| e.to_string())
}

#[tauri::command]
fn settings_load(st: State<Launcher>) -> Settings {
    Settings { ini: read_kv(&ini_read_path(&st.root)), start: read_start(&st.root) }
}

fn write_atomic(p: &Path, text: &str) -> Result<(), String> {
    let tmp = p.with_extension("tmp");
    std::fs::write(&tmp, text).map_err(|e| format!("{}: {e}", p.display()))?;
    std::fs::rename(&tmp, p).map_err(|e| format!("{}: {e}", p.display()))
}

#[tauri::command]
fn settings_save(st: State<Launcher>, ini: BTreeMap<String, String>, start: StartOpts) -> Result<(), String> {
    // Nur bekannte Schlüssel, einzeilig; alles andere in der Datei (Tastenbelegung, Kommentare) bleibt unverändert
    let mut todo: BTreeMap<String, String> = ini
        .into_iter()
        .filter(|(k, _)| KEYS.contains(&k.as_str()))
        .map(|(k, v)| (k, v.replace(['\r', '\n'], " ").chars().take(200).collect()))
        .collect();
    // the game menu moves old 16:9 defaults to "fits any screen" once; a choice saved here counts as made
    todo.insert("bildformat_v2".into(), "1".into());
    let en = english(&st.root);
    let p = ini_path(&st.root);
    let old = std::fs::read_to_string(ini_read_path(&st.root)).unwrap_or_default();
    let mut out = String::new();
    for l in old.lines() {
        if l.starts_with('#') {
            if !(en && l.contains("Einstellungen")) {
                out.push_str(l);
                out.push('\n');
            }
            continue;
        }
        let Some((k, v)) = l.split_once('=') else { continue };
        // key bindings and unknown lines are carried over; only the key name follows the file's language
        if let Some(n) = k.strip_prefix("taste_").or_else(|| k.strip_prefix("key_")) {
            out.push_str(&format!("{}{n}={v}\n", if en { "key_" } else { "taste_" }));
            continue;
        }
        let (ik, iv) = to_internal(k.trim(), v);
        let val = todo.remove(&ik).unwrap_or(iv);
        let (fk, fv) = to_file(&ik, &val, en);
        out.push_str(&format!("{fk}={fv}\n"));
    }
    if old.is_empty() || en && !out.starts_with('#') {
        out.insert_str(0, "# ts2-recomp - settings (launcher and game menu, ESC/F1)\n");
    }
    for (k, v) in todo {
        let (fk, fv) = to_file(&k, &v, en);
        out.push_str(&format!("{fk}={fv}\n"));
    }
    write_atomic(&p, &out)?;

    let b = |x: bool| if x { "1" } else { "0" };
    write_atomic(
        &cfg_dir().join("launcher.conf"),
        &format!(
            "TS2_WIDESCREEN={}\nPS2XGS_FULLSCREEN={}\nTS2_LETTERBOX={}\nTS2_EDITIONEN={}\nTS2_EDITION={}\nTS2_GAME_LANGUAGE={}\n",
            b(start.widescreen),
            b(start.fullscreen),
            b(start.letterbox),
            if ["original", "onlineplus", "both"].contains(&start.editions.as_str()) { start.editions.as_str() } else { "original" },
            if start.edition == "onlineplus" { "onlineplus" } else { "original" },
            if gl_ok(&start.game_lang) { start.game_lang.as_str() } else { "en" }
        ),
    )?;
    if cfg!(windows) && (start.renderer == "3" || start.renderer == "14") {
        let _ = std::fs::create_dir_all(st.root.join("build"));
        write_atomic(&st.root.join("build").join("renderer.txt"), &format!("{}\n", start.renderer))?;
    }
    Ok(())
}

// ---------------------------------------------------------------- Einrichtung

#[tauri::command]
async fn pick_iso(app: AppHandle) -> Option<String> {
    let f = app
        .dialog()
        .file()
        .set_title("TimeSplitters 2 – USA (SLUS-20314) or Europe (SLES-50877)")
        .add_filter("PS2 disc image (*.iso)", &["iso", "ISO"])
        .blocking_pick_file()?;
    f.into_path().ok().map(|p| p.to_string_lossy().to_string())
}

#[derive(Serialize, Clone)]
struct Ev {
    kind: &'static str,
    value: String,
}

#[tauri::command]
fn setup_start(app: AppHandle, st: State<Launcher>, iso: String) -> Result<(), String> {
    {
        let mut s = st.setup.lock().unwrap();
        if *s {
            return Err("setup already running".into());
        }
        *s = true;
    }
    let root = st.root.clone();
    iso_merken(&iso);
    let again = current_build(&root).is_some();
    std::thread::spawn(move || {
        let emit = |kind: &'static str, value: String| {
            let _ = app.emit("setup", Ev { kind, value });
        };
        // an incomplete package (wrong download, partial or permission-less extraction) gave a silent failure before
        let noetig: Vec<PathBuf> = if cfg!(windows) {
            vec![root.join("toolchain").join("python").join("python.exe"), root.join("tools").join("release").join("first_setup.py"), root.join("toolchain").join("zig").join("zig.exe")]
        } else {
            vec![root.join("tools").join("release").join("first_setup.sh"), root.join("toolchain").join("zig").join("zig")]
        };
        if let Some(f) = noetig.iter().find(|f| !f.is_file()) {
            let rel = f.strip_prefix(&root).map(|p| p.display().to_string()).unwrap_or_else(|_| f.display().to_string());
            emit("line", format!("ERROR missing: app/{rel}"));
            emit("error", "The package is incomplete. Download TS2-Recomp-<version>-<system> from the Releases page again (not \"Source code\") and extract it completely into a new folder.".into());
            *app.state::<Launcher>().setup.lock().unwrap() = false;
            return;
        }
        let mut c = if cfg!(windows) {
            let mut c = Command::new(root.join("toolchain").join("python").join("python.exe"));
            appimage_env(&mut c);
            c.arg(root.join("tools").join("release").join("first_setup.py"));
            c
        } else {
            let mut c = Command::new("bash");
            appimage_env(&mut c);
            c.arg(root.join("tools").join("release").join("first_setup.sh"));
            c
        };
        c.arg(&iso).current_dir(&root).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null());
        if again {
            c.env("TS2_FRESH", "1");
        }
        hide_console(&mut c);
        let (mut ok, mut err) = (false, String::new());
        match c.spawn() {
            Ok(mut ch) => {
                if let Some(out) = ch.stdout.take() {
                    for line in BufReader::new(out).lines().map_while(Result::ok) {
                        let l = line.trim_end();
                        if let Some(v) = l.strip_prefix("PROGRESS ") {
                            emit("pct", v.trim().into());
                        } else if let Some(v) = l.strip_prefix("STEP ") {
                            emit("step", v.into());
                        } else if let Some(v) = l.strip_prefix("ERROR ") {
                            err = v.into();
                            emit("line", l.into());
                        } else if l == "DONE" {
                            ok = true;
                        } else if !l.is_empty() {
                            emit("line", l.into());
                        }
                    }
                }
                let code = ch.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
                if !ok && err.is_empty() {
                    err = format!("exit code {code}");
                }
            }
            Err(e) => err = e.to_string(),
        }
        if ok {
            emit("done", String::new());
        } else {
            emit("error", format!("{err} (app/build/setup.log)"));
        }
        *app.state::<Launcher>().setup.lock().unwrap() = false;
    });
    Ok(())
}


// ---------------------------------------------------------------- Automatische Updates
// Each release carries per platform update-<plat>.json (version, flags, every updatable file with SHA-256) and
// update-<plat>.zip (the files, paths relative to the package folder). The launcher only replaces files whose hash
// differs, verifies every file before it is put in place and never touches the player's data:
// savedata/ (memory cards), app/build/ (the game built from the player's disc), app/elf/ and the settings in the
// user config folder. Before installing, savedata/ and the config files are copied to <config>/backups/<time>/
// (the newest three are kept). "rebuild": the game has to be linked again (setup runs with the remembered disc image);
// "critical": a full new setup is required - the launcher asks first.
const REPO_NAME: &str = "Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck";

#[derive(serde::Deserialize, Serialize, Clone)]
struct UpdFile { path: String, sha256: String, #[serde(default)] size: u64, #[serde(default)] mode: u32 }
#[derive(serde::Deserialize, Serialize, Clone)]
struct UpdManifest {
    version: String,
    #[serde(default)] critical: bool,
    #[serde(default)] rebuild: bool,
    #[serde(default)] notes: String,
    #[serde(default)] delta: Vec<String>,
    files: Vec<UpdFile>,
}
#[derive(Serialize, Default)]
struct UpdInfo { available: bool, version: String, critical: bool, rebuild: bool, changed: usize, bytes: u64, download: u64, delta: bool, notes: String, error: String }

struct UpdState { manifest: Option<UpdManifest>, zip_url: String, delta_url: String, changed: Vec<UpdFile> }
static UPD: Mutex<UpdState> = Mutex::new(UpdState { manifest: None, zip_url: String::new(), delta_url: String::new(), changed: Vec::new() });

fn upd_plat() -> &'static str { if cfg!(windows) { "windows" } else { "linux" } }

fn http_get(url: &str, limit: u64) -> Result<Vec<u8>, String> { http_get_cb(url, limit, &|_, _| {}) }

// same as http_get, reports (bytes so far, total bytes or 0) while downloading
fn http_get_cb(url: &str, limit: u64, cb: &dyn Fn(u64, u64)) -> Result<Vec<u8>, String> {
    let ok = url.starts_with(&format!("https://api.github.com/repos/{REPO_NAME}/"))
        || url.starts_with(&format!("https://github.com/{REPO_NAME}/releases/download/"));
    if !ok { return Err("url not allowed".into()); }
    // no overall time limit: a full update package is ~115 MB and a slow line needs longer than two minutes.
    // The connection may take 20 s, every single read 60 s - a dead line still ends the download.
    let agent = ureq::AgentBuilder::new().timeout_connect(Duration::from_secs(20)).timeout_read(Duration::from_secs(60)).build();
    let r = agent.get(url)
        .set("User-Agent", concat!("TS2-Launcher/", env!("CARGO_PKG_VERSION")))
        .set("Accept", if url.contains("api.github.com") { "application/vnd.github+json" } else { "application/octet-stream" })
        .call()
        .map_err(|e| e.to_string())?;
    let total: u64 = r.header("Content-Length").and_then(|x| x.parse().ok()).unwrap_or(0);
    let mut v = Vec::with_capacity(total.min(limit) as usize);
    use std::io::Read;
    let mut rd = r.into_reader().take(limit);
    let mut buf = vec![0u8; 256 * 1024];
    let mut last = std::time::Instant::now();
    cb(0, total);
    loop {
        let n = rd.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 { break; }
        v.extend_from_slice(&buf[..n]);
        if last.elapsed() >= Duration::from_millis(250) { cb(v.len() as u64, total); last = std::time::Instant::now(); }
    }
    cb(v.len() as u64, total);
    Ok(v)
}

fn sha256_hex(data: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    let d = Sha256::digest(data);
    d.iter().map(|b| format!("{b:02x}")).collect()
}

// "1.1.0" > "1.1.0-dev7" > "1.1.0-dev6" > "1.0.1"
fn ver_key(v: &str) -> (Vec<u64>, u64) {
    let v = v.trim().trim_start_matches('v');
    let (basis, vor) = match v.split_once('-') { Some((b, r)) => (b, r.trim_start_matches("dev").trim_start_matches("rc").parse::<u64>().unwrap_or(0)), None => (v, u64::MAX) };
    (basis.split('.').map(|x| x.parse().unwrap_or(0)).collect(), vor)
}
fn ver_newer(a: &str, b: &str) -> bool {
    let ((x, xv), (y, yv)) = (ver_key(a), ver_key(b));
    for i in 0..x.len().max(y.len()) {
        let (p, q) = (x.get(i).copied().unwrap_or(0), y.get(i).copied().unwrap_or(0));
        if p != q { return p > q; }
    }
    xv > yv
}

// a manifest path must stay inside the package and outside the player's data
fn upd_path_ok(p: &str) -> bool {
    let p = p.replace('\\', "/");
    !(p.is_empty() || p.starts_with('/') || p.contains("..") || p.contains(':')
        || p.starts_with("savedata") || p.starts_with("app/build") || p.starts_with("app/elf") || p.starts_with("app/disc"))
}

// files of the manifest that are missing or different in this installation
fn changed_files(pkg: &Path, m: &UpdManifest) -> Vec<UpdFile> {
    m.files.iter().filter(|f| upd_path_ok(&f.path)).filter(|f| {
        std::fs::read(pkg.join(&f.path)).map(|d| sha256_hex(&d) != f.sha256).unwrap_or(true)
    }).cloned().collect()
}

fn pkg_dir(root: &Path) -> PathBuf { root.parent().map(Path::to_path_buf).unwrap_or_else(|| root.to_path_buf()) }

#[tauri::command]
async fn update_check(st: State<'_, Launcher>) -> Result<UpdInfo, String> {
    let root = st.root.clone();
    tauri::async_runtime::spawn_blocking(move || upd_check_core(&root)).await.map_err(|e| e.to_string())
}

fn upd_check_core(root: &Path) -> UpdInfo {
    {
        let root = root.to_path_buf();
        let mut info = UpdInfo::default();
        let rel = match http_get(&format!("https://api.github.com/repos/{REPO_NAME}/releases/latest"), 4_000_000) {
            Ok(v) => v, Err(e) => { info.error = e; return info; }
        };
        let rel: serde_json::Value = match serde_json::from_slice(&rel) { Ok(v) => v, Err(e) => { info.error = e.to_string(); return info; } };
        let (mut man_url, mut zip_url, mut delta_url) = (String::new(), String::new(), String::new());
        let (mut zip_size, mut delta_size) = (0u64, 0u64);
        if let Some(a) = rel["assets"].as_array() {
            for x in a {
                let n = x["name"].as_str().unwrap_or("");
                let u = x["browser_download_url"].as_str().unwrap_or("").to_string();
                let sz = x["size"].as_u64().unwrap_or(0);
                if n == format!("update-{}.json", upd_plat()) { man_url = u; } else if n == format!("update-{}.zip", upd_plat()) { zip_url = u; zip_size = sz; } else if n == format!("update-{}-delta.zip", upd_plat()) { delta_url = u; delta_size = sz; }
            }
        }
        if man_url.is_empty() || zip_url.is_empty() { info.error = "no update package in the latest release".into(); return info; }
        let m: UpdManifest = match http_get(&man_url, 8_000_000).and_then(|v| serde_json::from_slice(&v).map_err(|e| e.to_string())) {
            Ok(m) => m, Err(e) => { info.error = e; return info; }
        };
        let local = game_version(&root);
        let pkg = pkg_dir(&root);
        // compare the files only when the release is newer (hashing the whole package on every start is wasted work)
        let neuer = ver_newer(&m.version, &local);
        let changed: Vec<UpdFile> = if neuer { changed_files(&pkg, &m) } else { Vec::new() };
        info.version = m.version.clone();
        info.critical = m.critical;
        info.rebuild = m.rebuild;
        info.notes = m.notes.clone();
        info.changed = changed.len();
        info.bytes = changed.iter().map(|f| f.size).sum();
        info.available = neuer && !changed.is_empty();
        let mut u = UPD.lock().unwrap();
        // the small delta package is enough when every file this installation needs is in it
        let delta_reicht = !delta_url.is_empty() && changed.iter().all(|f| m.delta.contains(&f.path));
        info.delta = delta_reicht;
        info.download = if delta_reicht { delta_size } else { zip_size };
        u.manifest = Some(m); u.zip_url = if delta_reicht { delta_url } else { zip_url }; u.delta_url = String::new(); u.changed = changed;
        info
    }
}

fn copy_dir(from: &Path, to: &Path) -> std::io::Result<()> {
    std::fs::create_dir_all(to)?;
    for e in std::fs::read_dir(from)? {
        let e = e?;
        let t = to.join(e.file_name());
        if e.file_type()?.is_dir() { copy_dir(&e.path(), &t)?; } else { std::fs::copy(e.path(), t)?; }
    }
    Ok(())
}

// copy of the player's data before anything is replaced; keeps the newest three
fn backup_saves(root: &Path) -> Result<PathBuf, String> {
    let ts = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let base = cfg_dir().join("backups");
    let dst = base.join(format!("{ts}"));
    std::fs::create_dir_all(&dst).map_err(|e| e.to_string())?;
    let pkg = pkg_dir(root);
    for d in ["savedata", "savedata_onlineplus"] {
        if pkg.join(d).is_dir() { copy_dir(&pkg.join(d), &dst.join(d)).map_err(|e| format!("backup {d}: {e}"))?; }
    }
    if let Ok(rd) = std::fs::read_dir(cfg_dir()) {
        for e in rd.flatten() {
            let p = e.path();
            if p.is_file() { let _ = std::fs::copy(&p, dst.join(e.file_name())); }
        }
    }
    let mut alle: Vec<PathBuf> = std::fs::read_dir(&base).map(|r| r.flatten().map(|e| e.path()).filter(|p| p.is_dir()).collect()).unwrap_or_default();
    alle.sort();
    while alle.len() > 3 { let _ = std::fs::remove_dir_all(alle.remove(0)); }
    Ok(dst)
}

#[derive(Serialize, Clone)]
struct UpdEv { kind: &'static str, value: String }

#[tauri::command]
fn update_install(app: AppHandle, st: State<Launcher>) -> Result<(), String> {
    let root = st.root.clone();
    if *st.game.lock().unwrap() { return Err("game is running".into()); }
    if UPD.lock().unwrap().manifest.is_none() { return Err("no update checked".into()); }
    std::thread::spawn(move || {
        let emit = |kind: &'static str, value: String| { let _ = app.emit("update", UpdEv { kind, value }); };
        match upd_install_run(&root, &emit) { Ok(flags) => emit("done", flags), Err(e) => emit("error", e) }
    });
    Ok(())
}

// the whole update after a check: backup, download (with progress), verify, install. Returns the flags for the UI.
// Events: step backup|download|verify|install|finish, pct 0-100, dl "done/total", file "i/n path", info text
fn upd_install_run(root: &Path, emit: &dyn Fn(&'static str, String)) -> Result<String, String> {
    let (m, zip_url, changed) = {
        let u = UPD.lock().unwrap();
        (u.manifest.clone().ok_or("no update checked")?, u.zip_url.clone(), u.changed.clone())
    };
    emit("step", "backup".into());
    emit("pct", "1".into());
    let dst = backup_saves(root)?;
    let n = std::fs::read_dir(&dst).map(|r| r.count()).unwrap_or(0);
    emit("info", format!("backup:{n}"));
    emit("pct", "4".into());
    emit("step", "download".into());
    let name = zip_url.rsplit('/').next().unwrap_or("").to_string();
    emit("info", format!("package:{name}"));
    let data = http_get_cb(&zip_url, 2_000_000_000, &|done, total| {
        emit("dl", format!("{done}/{total}"));
        if total > 0 { emit("pct", format!("{}", 5 + 55 * done.min(total) / total)); }
    })?;
    emit("pct", "60".into());
    let neu = install_core(&pkg_dir(root), &m, data, &changed, emit)?;
    emit("step", "finish".into());
    emit("pct", "100".into());
    // relink only when files the game is linked from were replaced (a launcher-only update starts right away)
    let relink = m.critical || (m.rebuild && changed.iter().any(|f| is_link_input(&f.path)));
    Ok(format!("{}{}", if relink { "rebuild" } else { "" }, if neu { ",restart" } else { "" }))
}

// extract + verify everything into a staging folder first (nothing in place is touched if a single file is wrong),
// then move the files into place; the running launcher is renamed first (Windows allows renaming a running exe)
fn install_core(pkg: &Path, m: &UpdManifest, data: Vec<u8>, changed: &[UpdFile], emit: &dyn Fn(&'static str, String)) -> Result<bool, String> {
    let mut z = zip::ZipArchive::new(std::io::Cursor::new(data)).map_err(|e| e.to_string())?;
    let stage = pkg.join(".update_staging");
    let _ = std::fs::remove_dir_all(&stage);
    emit("step", "verify".into());
    let n = changed.len().max(1);
    for (i, f) in changed.iter().enumerate() {
        if !upd_path_ok(&f.path) { return Err(format!("path not allowed: {}", f.path)); }
        let mut e = z.by_name(&f.path).map_err(|_| format!("missing in package: {}", f.path))?;
        let mut buf = Vec::new();
        use std::io::Read;
        e.read_to_end(&mut buf).map_err(|e| e.to_string())?;
        if sha256_hex(&buf) != f.sha256 { let _ = std::fs::remove_dir_all(&stage); return Err(format!("checksum mismatch: {}", f.path)); }
        let t = stage.join(&f.path);
        if let Some(d) = t.parent() { std::fs::create_dir_all(d).map_err(|e| e.to_string())?; }
        std::fs::write(&t, &buf).map_err(|e| e.to_string())?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = if f.mode != 0 { f.mode } else { e.unix_mode().unwrap_or(0o644) };
            let _ = std::fs::set_permissions(&t, std::fs::Permissions::from_mode(mode & 0o777));
        }
        emit("file", format!("{}/{}/{}", i + 1, changed.len(), f.path));
        emit("pct", format!("{}", 60 + 25 * (i + 1) / n));
    }
    emit("step", "install".into());
    let exe = self_exe();
    let mut launcher_neu = false;
    for (i, f) in changed.iter().enumerate() {
        emit("file", format!("{}/{}/{}", i + 1, changed.len(), f.path));
        emit("pct", format!("{}", 85 + 14 * (i + 1) / n));
        let src = stage.join(&f.path);
        let dst = pkg.join(&f.path);
        if let Some(d) = dst.parent() { std::fs::create_dir_all(d).map_err(|e| e.to_string())?; }
        if exe.is_some() && dst.canonicalize().ok() == exe {
            let old = dst.with_extension("old");
            let _ = std::fs::remove_file(&old);
            std::fs::rename(&dst, &old).map_err(|e| e.to_string())?;
            launcher_neu = true;
        }
        std::fs::rename(&src, &dst).or_else(|_| std::fs::copy(&src, &dst).map(|_| ())).or_else(|_| {
            // in use (Windows: a loaded DLL cannot be overwritten, but it can be renamed): move the old file aside
            let old = PathBuf::from(format!("{}.old", dst.display()));
            let _ = std::fs::remove_file(&old);
            std::fs::rename(&dst, &old).and_then(|_| std::fs::rename(&src, &dst).or_else(|_| std::fs::copy(&src, &dst).map(|_| ())))
        }).map_err(|e| format!("{}: {e}", f.path))?;
    }
    let _ = std::fs::write(pkg.join("VERSION"), format!("{}\n", m.version));
    let _ = std::fs::remove_dir_all(&stage);
    emit("pct", "100".into());
    Ok(launcher_neu)
}

// disc image remembered from the last setup (only on this computer) - used to relink after a "rebuild" update
fn iso_merken(iso: &str) { let _ = std::fs::write(cfg_dir().join("disc_image.txt"), iso); }
#[tauri::command]
fn last_iso() -> String {
    let p = std::fs::read_to_string(cfg_dir().join("disc_image.txt")).unwrap_or_default();
    let p = p.trim().to_string();
    if Path::new(&p).is_file() { p } else { String::new() }
}
#[tauri::command]
fn restart_launcher(app: AppHandle) { app.restart(); }

// ---------------------------------------------------------------- Spielen

#[derive(Serialize, Clone)]
struct GameEv {
    state: &'static str,
    code: i32,
    tail: String,
}

fn log_tail(p: &Path, n: usize) -> String {
    let t = std::fs::read_to_string(p).unwrap_or_default();
    let v: Vec<&str> = t.lines().collect();
    v[v.len().saturating_sub(n)..].join("\n")
}

#[tauri::command]
fn play(app: AppHandle, st: State<Launcher>) -> Result<(), String> {
    {
        let mut g = st.game.lock().unwrap();
        if *g {
            return Err("game already running".into());
        }
        *g = true;
    }
    let root = st.root.clone();
    let start = read_start(&root);
    let b = |x: bool| if x { "1" } else { "0" };
    let mut c = if cfg!(windows) {
        let mut c = Command::new("powershell");
        appimage_env(&mut c);
        c.args(["-NoProfile", "-ExecutionPolicy", "Bypass", "-WindowStyle", "Hidden", "-File"])
            .arg(root.join("tools").join("release").join("TS2-Recomp.ps1"))
            .arg("-Play");
        c
    } else {
        let mut c = Command::new("bash");
        appimage_env(&mut c);
        c.arg(root.join("tools").join("ts2_run.sh"));
        c
    };
    c.current_dir(&root)
        .env("TS2_WIDESCREEN", b(start.widescreen))
        .env("PS2XGS_FULLSCREEN", b(start.fullscreen))
        .env("TS2_LETTERBOX", b(start.letterbox))
        .env("TS2_EDITION", if start.edition == "onlineplus" && start.editions != "original" { "onlineplus" } else { "original" })
        .env("TS2_LAUNCHER", "1")
        .env("TS2_GAME_LANGUAGE", if gl_ok(&start.game_lang) { start.game_lang.as_str() } else { "en" })
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null());
    hide_console(&mut c);
    let mut child = match c.spawn() {
        Ok(ch) => ch,
        Err(e) => {
            *st.game.lock().unwrap() = false;
            return Err(e.to_string());
        }
    };
    let _ = app.emit("game", GameEv { state: "running", code: 0, tail: String::new() });
    if let Some(w) = app.get_webview_window("main") {
        let _ = w.minimize();
    }
    std::thread::spawn(move || {
        let t0 = Instant::now();
        let code = child.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
        // Unter Windows zeigt TS2-Recomp.ps1 einen Sofortabbruch selbst an; unter Linux hier das Protokollende
        let tail = if code != 0 && t0.elapsed() < Duration::from_secs(20) && !cfg!(windows) {
            log_tail(&root.join("build").join("play.log"), 14)
        } else {
            String::new()
        };
        *app.state::<Launcher>().game.lock().unwrap() = false;
        if let Some(w) = app.get_webview_window("main") {
            let _ = w.unminimize();
            let _ = w.show();
            let _ = w.set_focus();
        }
        let _ = app.emit("game", GameEv { state: "exited", code, tail });
    });
    Ok(())
}

// ---------------------------------------------------------------- Server-Abfrage (T2IN, wie Bridge.cpp online::abfragen)

#[derive(Serialize, Default)]
struct SrvInfo {
    addr: String,
    ok: bool,
    proto: u16,
    players: u16,
    max: u16,
    pw: bool,
    name: String,
    motd: String,
    ping: u32,
}

fn split_addr(a: &str) -> (String, String) {
    let a = a.trim();
    if let Some(r) = a.strip_prefix('[') {
        if let Some(e) = r.find(']') {
            let host = r[..e].to_string();
            let port = r[e + 1..].strip_prefix(':').unwrap_or(DEFAULT_PORT).to_string();
            return (host, port);
        }
    }
    if a.matches(':').count() == 1 {
        let (h, p) = a.split_once(':').unwrap();
        return (h.to_string(), p.to_string());
    }
    (a.to_string(), DEFAULT_PORT.to_string())
}

fn query_one(addr: &str, nonce: u32) -> SrvInfo {
    let mut r = SrvInfo { addr: addr.to_string(), ..Default::default() };
    let (host, port) = split_addr(addr);
    let Ok(port) = port.parse::<u16>() else { return r };
    let Ok(mut it) = (host.as_str(), port).to_socket_addrs() else { return r };
    let Some(sa) = it.next() else { return r };
    let bind = if sa.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" };
    let Ok(sock) = UdpSocket::bind(bind) else { return r };
    let _ = sock.set_read_timeout(Some(Duration::from_millis(1500)));
    let mut q = [0u8; 8];
    q[..4].copy_from_slice(b"T2IN");
    q[4..].copy_from_slice(&nonce.to_le_bytes());
    let t0 = Instant::now();
    if sock.send_to(&q, sa).is_err() {
        return r;
    }
    let mut b = [0u8; 512];
    while t0.elapsed() < Duration::from_millis(1500) {
        let Ok((n, from)) = sock.recv_from(&mut b) else { break };
        if from.port() != sa.port() || n < 15 || &b[..4] != b"T2IN" || b[4..8] != nonce.to_le_bytes() {
            continue;
        }
        let u16le = |p: usize| u16::from_le_bytes([b[p], b[p + 1]]);
        r.proto = u16le(8);
        r.players = u16le(10);
        r.max = u16le(12);
        r.pw = b[14] != 0;
        let rest = &b[15..n];
        let mut parts = rest.splitn(2, |c| *c == 0);
        r.name = String::from_utf8_lossy(parts.next().unwrap_or_default()).chars().take(64).collect();
        r.motd = String::from_utf8_lossy(parts.next().unwrap_or_default()).trim_end_matches('\0').chars().take(160).collect();
        r.ping = t0.elapsed().as_millis() as u32;
        r.ok = true;
        break;
    }
    r
}

#[derive(Serialize)]
struct TopEntry {
    name: String,
    xp: u64,
    kills: u64,
}

// Online+ top list of a server (query "T2IP"; servers without Online+ do not answer -> empty list)
#[tauri::command]
async fn query_board(addr: String) -> Vec<TopEntry> {
    tauri::async_runtime::spawn_blocking(move || {
        let mut out = Vec::new();
        let (host, port) = split_addr(&addr);
        let Ok(port) = port.parse::<u16>() else { return out };
        let Ok(mut it) = (host.as_str(), port).to_socket_addrs() else { return out };
        let Some(sa) = it.next() else { return out };
        let Ok(sock) = UdpSocket::bind(if sa.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" }) else { return out };
        let _ = sock.set_read_timeout(Some(Duration::from_millis(1200)));
        let nonce: u32 = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.subsec_nanos()).unwrap_or(11);
        let mut q = [0u8; 8];
        q[..4].copy_from_slice(b"T2IP");
        q[4..].copy_from_slice(&nonce.to_le_bytes());
        if sock.send_to(&q, sa).is_err() { return out; }
        let mut b = [0u8; 1024];
        let t0 = Instant::now();
        while t0.elapsed() < Duration::from_millis(1200) {
            let Ok((n, from)) = sock.recv_from(&mut b) else { break };
            if from.port() != sa.port() || n < 8 || &b[..4] != b"T2IP" || b[4..8] != nonce.to_le_bytes() { continue; }
            for l in String::from_utf8_lossy(&b[8..n]).lines().take(10) {
                let f: Vec<&str> = l.split('\t').collect();
                if f.len() >= 3 {
                    out.push(TopEntry { name: f[0].chars().take(24).collect(), xp: f[1].parse().unwrap_or(0), kills: f[2].parse().unwrap_or(0) });
                }
            }
            break;
        }
        out
    })
    .await
    .unwrap_or_default()
}

#[tauri::command]
async fn query_servers(addrs: Vec<String>) -> Vec<SrvInfo> {
    tauri::async_runtime::spawn_blocking(move || {
        let seed = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.subsec_nanos()).unwrap_or(7);
        std::thread::scope(|s| {
            let hs: Vec<_> = addrs
                .iter()
                .take(64)
                .enumerate()
                .map(|(i, a)| s.spawn(move || query_one(a, seed ^ (i as u32).wrapping_mul(2654435761))))
                .collect();
            hs.into_iter().filter_map(|h| h.join().ok()).collect()
        })
    })
    .await
    .unwrap_or_default()
}

// ---------------------------------------------------------------- News / Serverliste

#[tauri::command]
async fn fetch_text(url: String) -> Result<String, String> {
    if !(url.starts_with(REPO_RAW) || url.starts_with(REPO_API)) {
        return Err("url not allowed".into());
    }
    tauri::async_runtime::spawn_blocking(move || {
        let r = ureq::get(&url)
            .set("User-Agent", concat!("TS2-Launcher/", env!("CARGO_PKG_VERSION")))
            .set("Accept", "application/vnd.github+json")
            .timeout(Duration::from_secs(8))
            .call()
            .map_err(|e| e.to_string())?;
        let mut s = String::new();
        use std::io::Read;
        r.into_reader().take(2_000_000).read_to_string(&mut s).map_err(|e| e.to_string())?;
        Ok(s)
    })
    .await
    .map_err(|e| e.to_string())?
}

// ---------------------------------------------------------------- Eigener Server (ts2-server aus dem Paket)

#[tauri::command]
fn host_start(st: State<Launcher>, name: String, password: String, max: u32, motd: String) -> Result<(), String> {
    let mut h = st.host.lock().unwrap();
    if let Some(c) = h.as_mut() {
        if c.try_wait().ok().flatten().is_none() {
            return Ok(());
        }
    }
    let bin = st.root.join("..").join("server").join(exe_name("ts2-server"));
    if !bin.is_file() {
        return Err(format!("{} not found", bin.display()));
    }
    let clean = |s: &str, n: usize| s.replace(['\r', '\n', '\0'], " ").chars().take(n).collect::<String>();
    let mut c = Command::new(bin);
    appimage_env(&mut c);
    c.arg("--name").arg(clean(&name, 48)).arg("--max").arg(max.clamp(2, 16).to_string());
    if !motd.trim().is_empty() {
        c.arg("--motd").arg(clean(&motd, 120));
    }
    if !password.is_empty() {
        c.arg("--password").arg(clean(&password, 48));
    }
    c.current_dir(&st.root).stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
    hide_console(&mut c);
    *h = Some(c.spawn().map_err(|e| e.to_string())?);
    Ok(())
}

fn stop_host(st: &Launcher) {
    if let Some(mut c) = st.host.lock().unwrap().take() {
        let _ = c.kill();
        let _ = c.wait();
    }
}

#[tauri::command]
fn host_stop(st: State<Launcher>) {
    stop_host(&st);
}

#[tauri::command]
fn host_running(st: State<Launcher>) -> bool {
    let mut h = st.host.lock().unwrap();
    match h.as_mut() {
        Some(c) => c.try_wait().ok().flatten().is_none(),
        None => false,
    }
}

// ---------------------------------------------------------------- Ordner, Links, Verknüpfung

#[tauri::command]
fn open_path(app: AppHandle, st: State<Launcher>, what: String) -> Result<(), String> {
    let p = match what.as_str() {
        "folder" => st.root.parent().map(Path::to_path_buf).unwrap_or_else(|| st.root.clone()),
        "config" => cfg_dir(),
        "log" => {
            let l = st.root.join("build").join("play.log");
            if l.is_file() { l } else { st.root.join("build") }
        }
        _ => return Err("unknown".into()),
    };
    app.opener().open_path(p.to_string_lossy(), None::<&str>).map_err(|e| e.to_string())
}

#[tauri::command]
fn open_url(app: AppHandle, url: String) -> Result<(), String> {
    if !url.starts_with("https://") {
        return Err("only https".into());
    }
    app.opener().open_url(url, None::<&str>).map_err(|e| e.to_string())
}

#[tauri::command]
fn make_shortcut(st: State<Launcher>) -> Result<(), String> {
    let mut c = if cfg!(windows) {
        let top = st.root.parent().map(Path::to_path_buf).unwrap_or_else(|| st.root.clone());
        let exe = top.join("TS2-Launcher.exe");
        let script = format!(
            "$s=(New-Object -ComObject WScript.Shell).CreateShortcut([IO.Path]::Combine([Environment]::GetFolderPath('Desktop'),'TimeSplitters 2 (Native Recomp).lnk'));$s.TargetPath='{}';$s.WorkingDirectory='{}';$s.Save()",
            exe.display().to_string().replace('\'', "''"),
            top.display().to_string().replace('\'', "''")
        );
        let mut c = Command::new("powershell");
        appimage_env(&mut c);
        c.args(["-NoProfile", "-Command", &script]);
        c
    } else {
        let mut c = Command::new("bash");
        appimage_env(&mut c);
        c.arg(st.root.join("..").join("TS2-Recomp.sh")).arg("--shortcut");
        c
    };
    hide_console(&mut c);
    let s = c.status().map_err(|e| e.to_string())?;
    if s.success() { Ok(()) } else { Err(format!("exit {s}")) }
}

// The window's X: end the launcher process completely (closing only the window left it running in the background)
#[tauri::command]
fn quit_launcher(app: tauri::AppHandle) {
    app.exit(0);
}

// ----------------------------------------------------------------

// `TS2-Launcher --setup <iso>` (tests): runs the setup exactly the way the Setup button does (same child process, same
// flags: no console window, stdin and stderr closed, stdout piped) without the window. Lines go to
// app/build/launcher_setup.txt; exit code 0 = DONE seen. Found necessary by issue #4: the Windows test ran the setup
// through the script, never through the launcher.
fn headless_setup(root: &PathBuf, iso: &str) -> i32 {
    use std::io::Write;
    let _ = std::fs::create_dir_all(root.join("build"));
    let mut logf = std::fs::File::create(root.join("build").join("launcher_setup.txt")).ok();
    let mut c = if cfg!(windows) {
        let mut c = Command::new(root.join("toolchain").join("python").join("python.exe"));
        appimage_env(&mut c);
        c.arg(root.join("tools").join("release").join("first_setup.py"));
        c
    } else {
        let mut c = Command::new("bash");
        appimage_env(&mut c);
        c.arg(root.join("tools").join("release").join("first_setup.sh"));
        c
    };
    c.arg(iso).current_dir(root).stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null());
    hide_console(&mut c);
    let mut ok = false;
    match c.spawn() {
        Ok(mut ch) => {
            if let Some(out) = ch.stdout.take() {
                for line in BufReader::new(out).lines().map_while(Result::ok) {
                    if line.trim_end() == "DONE" { ok = true; }
                    if let Some(f) = logf.as_mut() { let _ = writeln!(f, "{}", line.trim_end()); let _ = f.flush(); }
                }
            }
            let code = ch.wait().ok().and_then(|s| s.code()).unwrap_or(-1);
            if let Some(f) = logf.as_mut() { let _ = writeln!(f, "exit code {code}"); }
        }
        Err(e) => { if let Some(f) = logf.as_mut() { let _ = writeln!(f, "spawn failed: {e}"); } }
    }
    if ok { 0 } else { 1 }
}

// `TS2-Launcher --update-from <update-*.json> <update-*.zip>` (tests): installs an update from local files the way the
// Update button does (same file comparison, save backup and install step); result in app/build/launcher_update.txt
fn headless_update(root: &Path, js: &str, zp: &str) -> i32 {
    let pkg = pkg_dir(root);
    let r = (|| -> Result<String, String> {
        let m: UpdManifest = serde_json::from_slice(&std::fs::read(js).map_err(|e| e.to_string())?).map_err(|e| e.to_string())?;
        let changed = changed_files(&pkg, &m);
        if changed.is_empty() { return Ok(format!("nothing to do ({})", m.version)); }
        backup_saves(root)?;
        let neu = install_core(&pkg, &m, std::fs::read(zp).map_err(|e| e.to_string())?, &changed, &|_, _| {})?;
        Ok(format!("installed {} ({} files){}{}", m.version, changed.len(), if m.rebuild || m.critical { ", rebuild" } else { "" }, if neu { ", launcher replaced" } else { "" }))
    })();
    let (code, line) = match r { Ok(t) => (0, t), Err(e) => (1, format!("ERROR {e}")) };
    let exe = self_exe().map(|p| p.display().to_string()).unwrap_or_default();
    let txt = format!("{line}\nlauncher file: {exe}\n");
    print!("{txt}");
    let _ = std::fs::create_dir_all(root.join("build"));
    let _ = std::fs::write(root.join("build").join("launcher_update.txt"), txt);
    code
}

// `TS2-Launcher --update-online` (tests): the real update path of the Update button (GitHub check, backup, download with
// progress, verify, install) without the window; every event goes to stdout and app/build/launcher_update_online.txt
fn headless_update_online(root: &Path) -> i32 {
    use std::io::Write;
    let _ = std::fs::create_dir_all(root.join("build"));
    let logf = std::sync::Mutex::new(std::fs::File::create(root.join("build").join("launcher_update_online.txt")).ok());
    let last_dl = std::sync::Mutex::new(std::time::Instant::now() - Duration::from_secs(5));
    let log = |kind: &str, value: &str| {
        if kind == "dl" || kind == "pct" || kind == "file" {
            let mut l = last_dl.lock().unwrap();
            if l.elapsed() < Duration::from_secs(1) { return; }
            *l = std::time::Instant::now();
        }
        let line = format!("{kind} {value}");
        println!("{line}");
        if let Some(f) = logf.lock().unwrap().as_mut() { let _ = writeln!(f, "{line}"); }
    };
    let info = upd_check_core(root);
    log("check", &format!("available={} version={} changed={} bytes={} download={} delta={} error={}", info.available, info.version, info.changed, info.bytes, info.download, info.delta, info.error));
    if !info.available { return if info.error.is_empty() { 0 } else { 1 }; }
    let r = upd_install_run(root, &|k, v| log(k, &v));
    match r {
        Ok(flags) => { log("done", &flags); let after = upd_check_core(root); log("recheck", &format!("available={} version={}", after.available, after.version)); if after.available { 1 } else { 0 } }
        Err(e) => { log("error", &e); 1 }
    }
}

fn main() {
    let root = find_root();
    {
        let a: Vec<String> = std::env::args().collect();
        if a.len() >= 3 && a[1] == "--setup" { std::process::exit(headless_setup(&root, &a[2])); }
        if a.len() >= 4 && a[1] == "--update-from" { std::process::exit(headless_update(&root, &a[2], &a[3])); }
        if a.len() >= 2 && a[1] == "--update-online" { std::process::exit(headless_update_online(&root)); }
        if a.len() >= 2 && a[1] == "--status" { println!("version={} build={:?} stale={}", game_version(&root), current_build(&root), build_stale(&root)); std::process::exit(0); }
    }
    // leftover of a self-update
    if let Some(e) = self_exe() { let _ = std::fs::remove_file(e.with_extension("old")); }
    let app = tauri::Builder::default()
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_opener::init())
        .manage(Launcher { root, host: Mutex::new(None), game: Mutex::new(false), setup: Mutex::new(false) })
        .invoke_handler(tauri::generate_handler![
            status, settings_load, settings_save, pick_iso, setup_start, play, query_servers, query_board, fetch_text,
            host_start, host_stop, host_running, open_path, open_url, make_shortcut,
            update_check, update_install, last_iso, restart_launcher, quit_launcher,
            profile_list, profile_save, profile_load, profile_delete
        ])
        .build(tauri::generate_context!())
        .expect("TS2 launcher failed to start");
    app.run(|h, ev| {
        if let tauri::RunEvent::Exit = ev {
            stop_host(&h.state::<Launcher>());
        }
    });
}

#[cfg(test)]
mod tests {
    #[test]
    fn update_install_core() {
        use std::io::Write;
        let d = std::env::temp_dir().join(format!("ts2upd{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("app/gs")).unwrap();
        std::fs::create_dir_all(d.join("savedata")).unwrap();
        std::fs::write(d.join("app/gs/lib.so"), b"alt").unwrap();
        std::fs::write(d.join("savedata/Mcd001.ps2"), b"SPIELSTAND").unwrap();
        std::fs::write(d.join("VERSION"), "1.0.0\n").unwrap();
        let mut zb = Vec::new();
        {
            let mut z = zip::ZipWriter::new(std::io::Cursor::new(&mut zb));
            let o = zip::write::SimpleFileOptions::default();
            z.start_file("app/gs/lib.so", o).unwrap(); z.write_all(b"neu").unwrap();
            z.start_file("app/neu/datei.txt", o).unwrap(); z.write_all(b"hallo").unwrap();
            z.finish().unwrap();
        }
        let f = |p: &str, c: &[u8]| super::UpdFile { path: p.into(), sha256: super::sha256_hex(c), size: c.len() as u64, mode: 0o644 };
        let m = super::UpdManifest { version: "1.1.0".into(), critical: false, rebuild: false, notes: String::new(), delta: vec![], files: vec![f("app/gs/lib.so", b"neu"), f("app/neu/datei.txt", b"hallo")] };
        // wrong hash -> nothing changes
        let mut falsch = m.files.clone(); falsch[0].sha256 = "00".into();
        assert!(super::install_core(&d, &m, zb.clone(), &falsch, &|_, _| {}).is_err());
        assert_eq!(std::fs::read(d.join("app/gs/lib.so")).unwrap(), b"alt");
        // forbidden path
        let bad = vec![f("savedata/Mcd001.ps2", b"x")];
        assert!(super::install_core(&d, &m, zb.clone(), &bad, &|_, _| {}).is_err());
        // proper install
        super::install_core(&d, &m, zb, &m.files, &|_, _| {}).unwrap();
        assert_eq!(std::fs::read(d.join("app/gs/lib.so")).unwrap(), b"neu");
        assert_eq!(std::fs::read(d.join("app/neu/datei.txt")).unwrap(), b"hallo");
        assert_eq!(std::fs::read(d.join("savedata/Mcd001.ps2")).unwrap(), b"SPIELSTAND");
        assert_eq!(std::fs::read_to_string(d.join("VERSION")).unwrap().trim(), "1.1.0");
        assert!(super::ver_newer("1.1.0", "1.0.1") && super::ver_newer("1.1.0", "1.1.0-dev7") && super::ver_newer("1.1.0-dev7", "1.1.0-dev6") && !super::ver_newer("1.1.0-dev7", "1.1.0") && !super::ver_newer("1.1.0", "1.1.0"));
        let _ = std::fs::remove_dir_all(&d);
    }

    // End-to-end with real packages: TS2_UPD_PKG = extracted older installation (a copy, it is changed),
    // TS2_UPD_JSON / TS2_UPD_ZIP = update-<platform>.json/.zip of the new release.
    #[test]
    fn update_real_package() {
        let (Ok(pkg), Ok(js), Ok(zp)) = (std::env::var("TS2_UPD_PKG"), std::env::var("TS2_UPD_JSON"), std::env::var("TS2_UPD_ZIP")) else { println!("skipped (no TS2_UPD_PKG)"); return; };
        let pkg = std::path::PathBuf::from(pkg);
        let m: super::UpdManifest = serde_json::from_slice(&std::fs::read(js).unwrap()).unwrap();
        let local = std::fs::read_to_string(pkg.join("VERSION")).unwrap_or_default();
        std::fs::create_dir_all(pkg.join("savedata")).unwrap();
        std::fs::write(pkg.join("savedata/Mcd001.ps2"), b"SAVE").unwrap();
        std::fs::create_dir_all(pkg.join("app/build")).unwrap();
        std::fs::write(pkg.join("app/build/marker.txt"), b"BUILD").unwrap();
        let t0 = std::time::Instant::now();
        let changed = super::changed_files(&pkg, &m);
        println!("installed {} -> {}: {} of {} files differ ({} MB), compare took {:.1} s, newer={}", local.trim(), m.version, changed.len(), m.files.len(),
                 changed.iter().map(|f| f.size).sum::<u64>() / 1_000_000, t0.elapsed().as_secs_f32(), super::ver_newer(&m.version, local.trim()));
        assert!(!changed.is_empty());
        let t1 = std::time::Instant::now();
        super::install_core(&pkg, &m, std::fs::read(zp).unwrap(), &changed, &|_, _| {}).unwrap();
        println!("install took {:.1} s", t1.elapsed().as_secs_f32());
        assert!(super::changed_files(&pkg, &m).is_empty(), "files still differ after the update");
        assert_eq!(std::fs::read_to_string(pkg.join("VERSION")).unwrap().trim(), m.version);
        assert_eq!(std::fs::read(pkg.join("savedata/Mcd001.ps2")).unwrap(), b"SAVE");
        assert_eq!(std::fs::read(pkg.join("app/build/marker.txt")).unwrap(), b"BUILD");
        assert!(!pkg.join(".update_staging").exists());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            for f in &m.files { if f.mode & 0o111 != 0 { assert!(std::fs::metadata(pkg.join(&f.path)).unwrap().permissions().mode() & 0o100 != 0, "not executable: {}", f.path); } }
        }
    }

    #[test]
    fn query_local_server() {
        let r = super::query_one("127.0.0.1:27720", 0x1234_5678);
        assert!(r.ok, "keine Antwort");
        println!("name={} motd={} {}/{} pw={} ping={}", r.name, r.motd, r.players, r.max, r.pw, r.ping);
        assert_eq!(r.max, 12);
        assert!(r.pw);
        let d = super::query_one("127.0.0.1:27799", 1);
        assert!(!d.ok);
        assert_eq!(super::split_addr("[::1]:5"), ("::1".to_string(), "5".to_string()));
        assert_eq!(super::split_addr("host"), ("host".to_string(), "27713".to_string()));
    }
}
