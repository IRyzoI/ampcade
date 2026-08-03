# UI ⇄ Native bridge contract

The UI is one file: `ui/index.html`. It runs in two modes:
- **Plugin mode** — inside JUCE's WebView. `window.__JUCE__` exists. The JUCE frontend lib is **inlined into index.html** (do NOT `import('./juce/index.js')` — WKWebView refuses ES-module fetches from the plugin's internal resource scheme, which silently dropped the app into mock mode inside the real plugin).
- **Mock mode** — opened in a normal browser (`window.__JUCE__` undefined). The page must provide a complete fake implementation of the same surface (fake tones, fake latency via setTimeout, fake auth) so the whole UI is clickable and screenshottable.

Wrap everything in an adapter so app code never touches Juce directly:

```js
const bridge = window.__JUCE__ ? await makeJuceBridge() : makeMockBridge();
```

## Parameters (JUCE relays)

Continuous params — `Juce.getSliderState(name)`:
- `.getScaledValue()` → real value; `.setNormalisedValue(n)` with `n=(v-start)/(end-start)` (all ranges linear);
- `.sliderDragStarted()` / `.sliderDragEnded()` around drags (host automation gestures);
- `.valueChangedEvent.addListener(fn)` then read `.getScaledValue()`;
- `.properties = {start, end, interval, name, label}`.

Toggles — `Juce.getToggleState(name)`: `.getValue()`, `.setValue(bool)`, `.valueChangedEvent.addListener(fn)`.

| name | range | default | notes |
|---|---|---|---|
| `in_gain` | −24..+36 dB | 0 | top bar knob |
| `out_gain` | −24..+24 dB | 0 | top bar knob |
| `gate` | −101..−20 dB | −101 | −101 renders "OFF" |
| `drv_on` | bool | **false** | built-in Screamer drive — on the board, ready to kick on |
| `drv_gain` / `drv_tone` | 0..10 | 5 | built-in drive knobs |
| `drv_level` | −24..+24 dB | 0 | built-in drive level |
| `p1_on`..`p16_on` | bool | true | stomp switches (16 capture pedal slots) |
| `p1_drive`..`p16_drive` | −24..+24 dB | 0 | knob "DRIVE" |
| `p1_tone`..`p16_tone` | 0..10 | 5 | knob "TONE" (tilt EQ) |
| `p1_level`..`p16_level` | −24..+24 dB | 0 | knob "LEVEL" |
| `amp_on` | bool | true | amp power/stomp |
| `amp_gain` | −24..+24 dB | 0 | knob "GAIN" |
| `amp_bass` / `amp_mid` / `amp_treble` | 0..10 | 5 | tone stack |
| `amp_vol` | −24..+24 dB | 0 | knob "VOLUME" |
| `cab_on` | bool | true | cab stomp |
| `dly_on` | bool | **false** | built-in delay stomp (trails: audio keeps ringing out) |
| `dly_time` | 60..2000 ms | 380 | knob "TIME", show "380 ms" |
| `dly_fb` | 0..0.9 | 0.35 | knob "FEEDBACK", show % |
| `dly_mix` | 0..1 | 0.25 | knob "MIX", show % |
| `rev_on` | bool | **true** | built-in reverb stomp (trails) — a touch of reverb ships on |
| `rev_size` | 0..10 | 5 | knob "SIZE" |
| `rev_mix` | 0..1 | 0.25 | knob "MIX", show % |

The table lists the core rig; the full authoritative set (cho_*, eq_*, b1-b3 boosts at −24..+24, cmp_*, air_*, tun_*, dly_bpm/dly_sync, loop_vol and loop_imp at −24..+12) lives in `PARAM_DEFS` in ui/index.html, which is checked against the plugin's ranges at boot.

Delay and reverb are **built-in stereo pedals rendered after the cab** (chain: drive → pedals → amp → cab → delay → reverb → out). They are always present: no swap/clear/variant/import/busy — just stomp + knobs.

The rig is plug-and-play: the **amp and cab slots are always on the board**. With nothing loaded they play built-in DSP (a tweed-flavoured clean amp + 1x12 voicing, `BuiltinGear.h`); loading a capture/IR replaces the built-in, clearing the slot falls back to it. The built-in **drv** Screamer drive is its own chain block (movable, id `drv`).

## Native functions

`const fn = Juce.getNativeFunction("name"); await fn(...args)` — args/results are JSON **strings** unless noted. Every result is `{ok:true, ...}` or `{ok:false, error:"human readable"}`.

- `getState()` → full UI state (shape below). Call once on boot and after every `stateChanged` event.
- `t3kSearch(paramsJson)` — `{query, gears:["amp","amp-cab",...], sort, page, page_size}` → `{ok, page, total_pages, total, tones:[{id, title, author, gear, image, downloads, favorites, models_count, description}]}`
- `t3kModels(toneId)` → `{ok, models:[{id, name, size, architecture_version}]}` (the "settings" variants dropdown; for `cab` gear these are IR wavs)
- `t3kLoad(json)` — `{slot, toneId, modelId}` → downloads (cached) + loads; returns `{ok}`; progress/completion arrive via `stateChanged`. `slot` ∈ `p1`..`p16` | `amp` | `cab` | `air`.
- `clearSlot(slot)` → `{ok}`
- `importFile(slot)` → opens native chooser (`.nam` for p*/amp; `.wav/.aif` for cab) → `{ok}` (state event follows; `{ok:false,error:"cancelled"}` on cancel is silent — no toast)
- `t3kConnect()` → opens native OAuth overlay → resolves `{ok, username}` after login (or `{ok:false}`)
- `t3kDisconnect()` → `{ok}`
- `getSettings()` → `{ok, clientId, libraryDir}` — Settings panel
- `openExternal(url)` → `{ok}` system browser
- `listPresets()` → `{ok, presets:[name]}` / `savePresetAs({name, designs?})` / `loadPresetNamed({name})` → `{ok}` — named presets in `<appdata>/AmpCade/presets/*.ampcade`; loadPresetNamed triggers `stateChanged`. `designs` is a JSON string of the rig's style overrides — embedded in the file so shared presets look right; a loaded preset's designs come back via `presetDesigns` in the state (UI merges them into prefs.styles)
- `deletePresetNamed({name})` → `{ok, trashed}` — SOFT delete: the file moves to `presets/.trash/<trashed>`; `restorePreset({trashed, name})` → `{ok, name}` moves it back (renamed if the name was retaken). The UI's Cmd/Ctrl+Z uses this pair.
- `importPresets()` → multi-select native chooser; validated `.ampcade` files are copied into the presets folder (name collisions uniquified unless byte-identical) → `{ok, imported:[name], loaded?:name}` — a single import is also loaded (`stateChanged` follows)
- `revealPresets()` → `{ok}` — opens the presets folder in Finder/Explorer (drag files out to share them)
- `setPedalOrder(json)` — array of the movable block ids (`cmp`, `b1`..`b3`, `p1`..`p16`, `drv`, `cho`, `amp`, `cab`, `eq`) in their new left-to-right order → `{ok}` (state event follows). Older shorter lists are upgraded server-side. Delay/air/reverb are the fixed stereo tail (the EQ and boosts may sit among them).
- `setHidden(json)` — array of block ids parked in the rack → `{ok}`. Everything is hideable: `p1`..`p16`, `drv`, `cho`, `eq`, `b1`..`b3`, `cmp`, `amp`, `cab`, `air`, `dly`, `rev`, `tun`, `loop`. Ids outside that set are dropped silently, so the UI's `HIDEABLE` list must stay in step with `setHiddenSlots` in PluginProcessor.cpp.
- `openAudioSettings()` → `{ok}` — standalone only; opens JUCE's audio device panel.
- `moveIr(dest)` — `"cab"` or `"air"` → `{ok}`. Overrides the automatic cab-vs-space routing by moving the IR out of the other slot.

Scenes — eight snapshots of the whole rig (`kMaxScenes` in PluginProcessor.h). All return `{ok}` and fire `stateChanged`:
- `sceneSave(idx)` / `sceneRecall(idx)` / `sceneClear(idx)` / `sceneDelete(idx)`
- `sceneAdd({name?})` → `{ok, idx}` — refused with `{ok:false}` once eight exist
- `sceneRename({idx, name})` / `sceneMove({from, to})`
- `syncScenesToPreset()` — persists scene edits into the preset file on disk without rewriting the rig the player last saved. Paths are scrubbed on the way out, exactly as `savePresetAs` does.

Looper transport (see also `looperSave`/`looperUndo`/`looperRedo`/`looperConfig`/`looperImport` below):
- `looperTap()` → `{ok}` — the one-button transport: count-in → record → overdub → play.
- `looperStop()` → `{ok}` — stops playback, keeps the loop.
- `looperErase()` → `{ok}` — drops the loop; `looperUnerase()` → `{ok}` brings it back (offered via `loop.unerase` on the `meters` event, and withdrawn when the sample rate changes, because the stored length is in frames).
- `looperFolder()` → `{ok}` — reveals the Loops folder.
- `getPrefs()` → `{ok, prefs}` / `setPrefs(json)` → `{ok}` — small UI preference store (`<appdata>/AmpCade/ui-prefs.json`): `{theme, styles:{<styleKey>:{shape,color,grille,grilleColor,knob,knobColor,labelColor,name}}, keys:{actions,stomps,scenes,presets}, gearScale, numKeys, stackHead, lastPreset, recents, favs, ...}`. Style keys: `tone:<toneId>`, `file:<title>`, `builtin:<slot>`. (Board hiding lives in PLUGIN state as `hiddenSlots`, not here.) Mock mode uses localStorage.
- `debugLog(text)` → `{ok}` — appends to `<appdata>/AmpCade/ui.log`. UI MUST call it once right after bridge init in JUCE mode (`"ui-boot bridge=juce vN"`), and from a window `error` handler with the first JS error message.
- `undo()` / `redo()` → `{ok, did:bool}` — whole-rig snapshot history (knobs, gear, order, rack, scenes). Knob bursts coalesce into one step after ~1 s of quiet; every structural native fn pushes an entry before it mutates. `did:false` = nothing to undo/redo. UI binds Cmd/Ctrl+Z, Shift+Z, Ctrl+Y.
- `newPreset()` → `{ok}` — factory-fresh rig (default knobs, empty slots, fresh scenes); the previous rig stays one undo away. `stateChanged` follows.
- `looperSave()` → `{ok, folder}` — keeps the current loop session: writes `Loop (mix).wav` and moves the whole session folder (mix + layer stems) out of `Loops/Unsaved/` into `Loops/`. No file chooser. Loops are throwaways until this is called — but never silently lost: on erase/quit/crash the most recent unsaved take is promoted to the one rolling `Loops/Last loop (auto-backup)/` folder (overwritten by the next throwaway).
- `looperUndo()` / `looperRedo()` → `{ok}` — lift the last overdub layer out of the loop / put it back (one level, hardware-looper style; a new overdub replaces the layer). Availability rides the `meters` event: `loop.undo` / `loop.redo`.
- `looperConfig(json)` — `{bpm, beats, playAfterRec, countIn, ringOut}` → `{ok}`. `countIn:false` skips the pre-roll: the first tap is beat one and recording starts instantly.
- `looperImport()` → native chooser (wav/aif/flac/mp3/m4a); the file is resampled to the engine rate, capped at the loop maximum, and becomes the current loop (starts playing) → `{ok}` or `{ok:false, error}` (`"cancelled"` is silent). Refused while recording/overdubbing.

## Events (C++ → JS)

`window.__JUCE__.backend.addEventListener(name, payload => ...)`:
- `stateChanged` — payload IS the state object (same shape as `getState`). Re-render board + panel.
- `meters` — 24 Hz: `{in, inPeak, out, outPre, sceneDirty, loop:{st, len, pos, undo, redo, unerase, imp}, tunerHz, audioDead}`. Drives the I/O towers, the looper transport/IMPORT knob, the tuner, the unsaved-scene dot and the audio-down banner.
- `toast` — `{msg, kind:"info"|"error"|"ok"}`.
- `busy` — `{slot, busy:bool, label}` e.g. downloading — show spinner on that slot card.

## State shape

```json
{
  "connected": true, "username": "demo",
  "clientIdSet": true,
  "order": ["p1", "p2", "amp", "cab"],
  "hidden": ["cho", "tun", "eq"],
  "slots": {
    "p1": {"loaded": true, "title": "Screamin Green", "author": "tonelord", "gear": "pedal",
            "modelName": "Drive 5 / Tone 5", "toneId": 123, "modelId": 456, "missing": false},
    "p2": {"loaded": true, "title": "MyCapture", "author": "Imported", "gear": "pedal",
            "modelName": "MyCapture.nam", "missing": false},
    "p3": {"loaded": false},
    "amp": {"loaded": true, "title": "BritStack 800", "author": "capturelab", "gear": "amp",
             "modelName": "Crunch 3-6-9", "toneId": 9, "modelId": 91, "missing": false},
    "cab": {"loaded": true, "title": "4x12 Greenbacks", "author": "irguy", "gear": "cab",
             "modelName": "SM57 cap edge", "toneId": 77, "modelId": 771, "missing": false}
  }
}
```

`toneId`/`modelId` are **present only for TONE3000 gear**. Locally imported gear omits them (see `p2` above), so `ss.toneId != null` is the correct "did this come from TONE3000?" test — anything that branches on it (the tone link, the settings dropdown, favourites, style keys) must tolerate their absence. They used to be emitted unconditionally, defaulting to `0`, which made every imported capture look like tone #0.

`missing:true` = file referenced by a saved session no longer exists → show ⚠ badge + "re-download" affordance (clicking the card opens the browser at that tone if toneId exists). A missing block stays **on the board**, not in the rack — that badge is the only route back to the file.

Gear looks are **pure JS**: the board draws each block full-size with `gearSvg(artKind, style, label)` (parametric chassis/colour/knobs); `autoStyleFor()` keyword-maps the capture name to a style, per-tone overrides live in prefs (`getPrefs`/`setPrefs`). The legacy `spriteFor()` sprite set is still used for TONE3000 browser covers.
