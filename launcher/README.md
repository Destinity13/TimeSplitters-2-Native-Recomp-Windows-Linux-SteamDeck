<sub>[← back to README](../README.md)</sub>

# Launcher

The small app that starts with `TS2-Recomp.bat` / `TS2-Recomp.sh`: setup from your disc image, settings, news and
automatic updates. Built with [Tauri 2](https://tauri.app) (Rust + a plain HTML/JS front end, no framework).

| | |
|---|---|
| `ui/` | the front end (`index.html`, `app.js`, `api.js`, `style.css`). Opened in a browser it runs with mock data |
| `src-tauri/` | the Rust side: paths, settings file, setup and game start, server browser, updates |
| `news.json` | the news feed the launcher shows, generated from `news/*.md` by `tools/news_build.py` |

## Updates

On start the launcher asks GitHub for the latest release. If it is newer than the installed `VERSION` and carries
`update-<platform>.json` and `update-<platform>.zip`, it compares the file list with the installation, downloads only
what changed (every file is checked by SHA-256), backs up `savedata/` and the settings first and puts the files in
place. Updates marked `rebuild` link the game again with the disc image remembered from the setup; updates marked
`critical` ask before they start. The update files are made by the packaging step from the finished package.

## Build

```
cd launcher/src-tauri
cargo build --release                                   # Linux (needs WebKitGTK 4.1 development files)
cargo build --release --target x86_64-pc-windows-gnu    # Windows, cross-built; ship WebView2Loader.dll next to the exe
```
