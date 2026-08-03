# AmpCade 🎸

A super simple GUI for playing [Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore) (NAM) captures — a cartoon pedalboard, drag to reorder, stomp things on and off. Standalone app + VST3 + AU.

Loads `.nam` captures and `.wav`/`.aiff` impulse responses from your own files, or browse and download from [TONE3000](https://www.tone3000.com) right inside the app (unaffiliated — just uses their public API).

AmpCade ships without captures, so bring your own `.nam` files, or connect a free TONE3000 account to browse theirs. **The in-app browser needs that account** — TONE3000's API requires a login for every request, so searching and downloading stay unavailable until you connect under Settings → ACCOUNT. Importing your own files needs no account at all, and the built-in gear (Overdrive, Chorus, Delay, Reverb, EQ, boosts, Compressor) always works.

## Install (macOS)

1. Grab `AmpCade-macOS.zip` from the **[Releases page](https://github.com/IRyzoI/ampcade/releases)**.
2. Unzip and double-click **AmpCade-Installer.pkg** — one run installs the app and both plugins. Builds are signed and notarized by Apple, so nothing gets blocked.
3. Prefer placing files by hand? The zip also carries the raw bundles: `AmpCade.app` → `/Applications`, `AmpCade.vst3` → `~/Library/Audio/Plug-Ins/VST3/`, `AmpCade.component` → `~/Library/Audio/Plug-Ins/Components/`.

Windows: unzip, run `AmpCade.exe`. The Windows build is not code-signed yet, so SmartScreen shows "Windows protected your PC" the first time — choose **More info → Run anyway**. Needs the [WebView2 runtime](https://developer.microsoft.com/microsoft-edge/webview2/), preinstalled on Windows 11 and up-to-date Windows 10; if the window opens blank, install it and reopen.

## Something not working?

Open an issue at **[github.com/IRyzoI/ampcade/issues](https://github.com/IRyzoI/ampcade/issues)**. AmpCade keeps a log — `~/Library/AmpCade/ui.log` on macOS, `%APPDATA%\AmpCade\ui.log` on Windows — and pasting the last few lines makes almost any report actionable.

## For developers

```bash
git clone https://github.com/IRyzoI/ampcade && cd ampcade
./scripts/setup.sh        # fetches pinned JUCE 8.0.15 + NeuralAmpModelerCore
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target AmpCade_Standalone AmpCade_VST3 AmpCade_AU
```

The whole UI is one file: [ui/index.html](ui/index.html) — open it in a normal browser for a clickable mock mode, no build needed. UI ⇄ C++ contract in [docs/BRIDGE.md](docs/BRIDGE.md), dev notes in [NOTES.md](NOTES.md).

## Licenses

AGPL-3.0 (required by JUCE 8). NAM core + AudioDSPTools by Steven Atkinson (MIT).
