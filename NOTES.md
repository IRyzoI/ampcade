# AmpCade — dev notes

NAM-powered guitar rig player (Standalone + VST3 + AU) with a cartoon pedalboard UI and a built-in TONE3000 browser.

## Stack
- JUCE **8.0.15** (pinned tag; repo master is 9.0.0 — too fresh). AGPLv3 → this repo is AGPL-3.0.
- NeuralAmpModelerCore @ 3cde95c (2026-07-08), MIT. `nam::get_dsp(path)` → `unique_ptr<nam::DSP>`; `process(NAM_SAMPLE** in, NAM_SAMPLE** out, int frames)` (channel-pointer API); supports .nam file versions 0.5.0–0.7.0. Compiled with `NAM_SAMPLE_FLOAT`.
- AudioDSPTools (bundled dep of NAM core): `dsp/ResamplingContainer/ResamplingContainer.h` (Lanczos, template<T,NCHANS,A>) used via a port of the official plugin's `ResamplingNAM` wrapper (MIT, credit Steven Atkinson).
- IR convolution: `juce::dsp::Convolution` (zero-latency, normalise, trim).
- Gate: `juce::dsp::NoiseGate<float>`; tone stack: 3× `juce::dsp::IIR` (low shelf 120 Hz, peak 650 Hz, high shelf 3.2 kHz, ±12 dB mapped from 0..10 knobs).
- Editor = JUCE 8 WebView (`WebBrowserComponent` + resource provider + relays). UI = single `ui/index.html`. JUCE frontend JS lib served at `/juce/index.js` straight from the JUCE tree via BinaryData.

## TONE3000 API (verified 2026-07-26)
- Base `https://www.tone3000.com/api/v1`. **Everything requires OAuth Bearer** (anonymous GET → `{"error":"Missing or invalid Authorization header"}`).
- OAuth 2.0 + PKCE: `GET /oauth/authorize?client_id&redirect_uri&response_type=code&code_challenge&code_challenge_method=S256&state`; `POST /oauth/token` `{grant_type:authorization_code, code, code_verifier, redirect_uri, client_id}`; refresh `{grant_type:refresh_token, refresh_token, client_id}`. Localhost redirect URIs allowed without registration (dev). We use `http://127.0.0.1:53682/callback` intercepted in `pageAboutToLoad` of an in-app auth webview — no socket listener needed.
- `client_id` = **publishable key** from tone3000.com → Settings (safe to embed). The app owner must create their own and either paste it in app Settings or bake it into `src/Version.h` (`kDefaultClientId`) before sharing builds.
- Endpoints used: `GET /tones/search?query&page&page_size&sort&gears` (gears = **underscore-joined** values that themselves contain hyphens, e.g. `amp_amp-cab_pedal`), `GET /models?tone_id={id}&page&page_size`, `GET /tones/{id}`, `GET /user`.
- Gear enum: `amp`, `amp-cab`, `pedal`, `outboard`, `cab`, `space`, `experimental` (deprecated: `full-rig`, `ir`). Sizes: standard/lite/feather/nano/custom. Sorts: best-match/newest/oldest/trending/downloads-all-time. Formats: nam/ir/aida-x/aa-snapshot/proteus — we only load `nam` (models) + `ir` (cab wavs).
- Model objects: `{id, name, size, architecture_version, model_url, tone_id}`. Download = GET `model_url` with Bearer; **follow redirects manually and drop the Authorization header on cross-host redirect** (presigned CDN URLs reject foreign auth headers).
- Rate limit 100 req/min. Free tier = open-source/non-commercial. Reference client: github.com/tone-3000/api.

## Signal chain (v0.3, cab mix added in v0.6)
in (hotter input channel, **not** a sum) → input gain → gate → **six mono blocks in any user order** (`chainOrder`, one block id per byte: pedals = drive → NAM → unity makeup → level → tilt TONE; amp = gain → NAM → loudness-norm to −18 → tone stack → volume; cab = IR conv blended by `cab_mix`) → DC blocker (10 Hz HP) → **stereo fan-out** → built-in delay (wide: R tap = 1.15×, damped feedback, trails when off) → built-in reverb (wet-only juce::Reverb blend, trails) → output gain → soft ceiling (tanh knee above −6 dBFS) → NaN scrub → outs (mono host = 0.5(L+R)). Slots hot-swap via shared_ptr + graveyard purged on message-thread timer (never free NAM on audio thread). Latency = sum of active Lanczos resampler latencies. Tail reported 4 s.

## v0.1 → v0.2 lesson
`await import('./juce/index.js')` inside the plugin webview fails on WKWebView (ES-module fetch from the internal resource scheme) → the UI silently fell back to MOCK inside the real plugin (dry signal, fake search results). Fix: the JUCE frontend lib is inlined into ui/index.html; never lazy-import modules from the resource provider. `debugLog` native fn appends to `~/Library/AmpCade/ui.log` (JUCE quirk: userApplicationDataDirectory on macOS is ~/Library, not Application Support); the UI logs `ui-boot bridge=juce` on startup — check that file first when "buttons do nothing".

## v0.2.1 → v0.2.2 lessons (both invisible to every test we had)
- **Architectures were stripped from every plugin build since v0.1.** `nam::get_dsp` threw "No config parser registered for architecture: WaveNet" for *every* capture. The architectures self-register from static initializers in their own TUs (`static nam::ConfigParserHelper _register_WaveNet(...)` at the bottom of `wavenet/model.cpp`, likewise LSTM/ConvNet/Linear); those objects reach each plugin format through **`AmpCade_SharedCode.a`**, and a linker only pulls archive members that resolve an undefined symbol. `nam_core` being an OBJECT library does *not* save you — `juce_add_plugin`'s shared-code target is itself a static library. `keepNamArchitecturesLinked()` in `NamSlot.cpp` stores one address per TU into a `volatile` sink (optimiser may not elide it) and `NamSlot::load` calls it. `scripts/check-nam-linked.sh` greps the built binary POST_BUILD for all four registrars and fails the build otherwise. **Why the smoke test lied: it is a console app, so it links the nam_core objects directly, never through the archive.** Quick manual check: `nm <binary> | grep -c wavenet` must be > 0.
- **Never read a range from `WebSliderState.properties` when building a control.** JUCE constructs the state with placeholder `{start:0,end:1,…}` and only pushes the real range asynchronously (measured: still placeholder at t0, correct at t+1.5 s). A `typeof p.start === 'number'` guard passes on the placeholder, so every knob got range 0..1; drags clamped to 0..1, the plugin rescaled that into the real range and pinned the param to its minimum (−24 dB / gate OFF), and from then on `startV` was the minimum so no drag could move it. `PARAM_DEFS` is the range authority; a `param-range-mismatch` line hits ui.log if it ever drifts from the APVTS ranges.
- Both were only catchable by driving the **real standalone** and reading `~/Library/AmpCade/ui.log`. When touching the bridge or the DSP loader, boot the built app and confirm: ranges, a `set`→`get` round-trip, and an actual capture reaching `loaded=true`. Mock mode and the console app can both be green while the product is dead.

## v0.3.0 lessons
- **Gain staging was the "weak input" problem, and it was three compounding faults.** Measured with `ampcade-smoke <models> --levels` (keep using it — do not tune levels by ear alone): (1) mono-summing `0.5*(L+R)` cost 6 dB whenever the guitar was on one input of a 2-in interface, and 6 dB into a nonlinear capture reads as thin+clean, not merely quiet; (2) pedal slots ran with no makeup at all and each capture measured **15–22 dB below its own input**, because captures are trained at a hotter reference level than a typical interface delivers — two pedals buried the rig; (3) ±12 dB on GAIN/DRIVE is inaudible on a compressed capture, which reads as "the gain knob does nothing". Fixes: follow the input channel that actually carries signal (smoothed RMS + 3 dB hysteresis), `NamSlot::Makeup::unity` on pedals (measured **pre**-drive so DRIVE adds dirt, not volume), loudness normalization kept on the amp only (swapping amps stays an A/B), wider ranges (in −24..+36, drive/gain/level ±24), and a soft ceiling above −6 dBFS so the wider ranges cannot produce raw digital clipping.
- **`if (auto* arr = parseArg (args).getArray())` is a use-after-free.** The temporary `juce::var` dies at the end of the condition, before the loop body — the array pointer dangles, so reorders failed with "couldn't apply that chain order" while the logic was correct. Bind the var to a named local first. Only `.getArray()`/`.getDynamicObject()` are affected; `.toString()` copies.
- **A custom-scheme resource must declare its charset.** WKWebView decodes `"text/html"` with no charset as Latin-1 and ignores the document's own `<meta charset>`, so every em dash rendered as `â€"`. Serve `"text/html; charset=utf-8"`. Verify headlessly: `document.characterSet` must be `UTF-8` and the DOM must contain a real `—`.
- **Classify TONE3000 tones on `format`, never on `gear`.** A "space" tone (reverb/delay, e.g. a Hall of Fame) exists as both an IR and a NAM capture. Keying the slot check off gear sent every space tone to the Cab slot, whose search filter was `gears=cab` — so it could never be found there. Cab · IR now searches `cab`+`space`, pedal slots search `pedal`+`space`+`outboard`.
- Chain order covers the **six mono blocks** (p1-p4, amp, cab) in any arrangement, packed one id per byte in `chainOrder`. Delay and reverb are the stereo tail and deliberately stay last — everything upstream is mono, and making a NAM run after a stereo reverb would mean either summing away the width or paying 2× for every capture. Four-name orders from ≤0.2.x are upgraded by appending amp+cab.

## v0.4.0 UI direction (minimalist pass)
Owner's brief: keep the grille + cartoon board (he likes the look), strip the first
screen to as close to nothing as possible, **hide rather than remove**. Decided with him:
- Top bar carries a **slim signal readout only** (`#sigbar`: lamp = input present/clipping,
  one bar = output). Input/Output/Gate knobs live in the `#lv-pop` popover behind the
  sliders button. The lamp exists because "is my guitar arriving?" must stay answerable
  at a glance — do not remove it when stripping further.
- Board = the chain and nothing else. The old 16-segment meters and dB scales are gone.
- Browser = one search box + results. No gear chips, no sort control; a slot searches
  exactly the gears it can load (`gearsForSearch()` → `br.allowed`), best-match order.
- Delay + Reverb stay as visible board cards (his call) even though they are not movable.
Anything stripped further should follow the same rule: move it behind the sliders button
or the gear button, don't delete capability. Verify UI changes by DOM assertion in the
real standalone (count `#lv-pop .knob`, `#board-row .slotwrap`, etc.) — see the diag
pattern in the v0.2.2 lessons.

## v0.6.0 lessons
- **`juce::dsp::Convolution` only installs a queued IR from inside `process()`.** Look at
  `Impl::processSamples`: it calls `engineQueue->postPendingCommand()` and
  `installPendingEngine()` itself, and `Mixer::processSamples` skips the wet path entirely
  once fully bypassed. So "removing" an IR by flipping a bypass flag left the **old IR in
  the engine** — with a reverb IR in the cab slot the wash never went away, not on clear
  and not on loading a preset, and only another IR could displace it (which is exactly what
  got reported). `clearCabIr()` queues an inert 1-sample IR *and* keeps the convolution
  running with its output discarded (`cabFlushBlocks`) until `getCurrentIRSize() <= 1`.
  Locked down by the `cab clear:` line in the smoke test — it prints `stale-size=24000`
  for the not-pumped case, i.e. the trap itself is asserted, not just the fix.
- **`Normalise::yes` is a cab setting, not an IR setting.** It divides by total energy
  (`0.125/sqrt(sum of squares)` in `normaliseImpulseResponse`), which holds cabs at a
  consistent level but buries a multi-second reverb IR tens of dB and leaves nothing
  audible but tail — that is what "sounds 1000% wet" was, on top of a convolution having
  no dry path at all. `src/IrLoader.h` splits the two animals by length (>0.4 s = space):
  space IRs are read by hand, summed to mono, **peak**-normalised to 0.9, and truncated to
  3 s (faded, or the cut clicks) because this is a zero-latency convolution that pays for
  every tap. New `cab_mix` param supplies the dry path; a space IR auto-lands at 30% wet
  and a cab IR forces 100%, but **only on a user-initiated load** — doing it on state
  restore would overwrite the MIX the player saved.
- **Board shows only what is in the rig.** Empty slots are no longer rendered; one `+` card
  (`addBlockEl`, kind menu → first free slot) replaces the four "Add pedal" ghosts, and the
  ✕/Delete/panel button removes a block. Anything that walks `state.order` now has to skip
  slots that are not `onBoard()` — `nudgePedal` swaps with the nearest *visible* neighbour,
  otherwise the arrow silently swaps with an invisible slot and looks broken.
- **A meter that starts at -60 dB shows a guitar as nothing.** The input tower now runs
  `MT_FLOOR = -84` with a `^0.75` law (so -60 dB sits ~39% up, not on the floor), draws the
  **RMS** the gate's own detector sees (`juce::dsp::NoiseGate` gates on an RMS ballistics
  filter, so a peak bar and the threshold line disagreed by ~12 dB), and floats a true peak
  that the processor *holds* until the editor reads it (`meterIn.exchange(0)`), because a
  block peak sampled at 15 Hz missed almost everything. Gate drag maps through `fracDb` so
  the whole -84..-20 range is reachable — it used to jump from OFF straight to ~-55 dB,
  which ate note tails — and the gate release is 280 ms for the same reason.
- Verifying UI in the real standalone: a temporary `bridge.call('debugLog', 'diag ' + JSON…)`
  at the end of `boot()` is the whole trick (`wraps`, gate `bottom`, `dbFrac` samples,
  relay ranges), then read `~/Library/AmpCade/ui.log`. **Rebuilding changes the ad-hoc
  signature, so macOS re-asks for microphone access**; until that dialog is answered
  `AudioDeviceManager::initialise` blocks in CoreAudio and the app never opens a window
  (`sample <pid>` shows it parked in `HALC_ProxyIOContext::_TellServerAboutStreamUsage`).
  That is not a plugin bug — answer the prompt, or open the app from Finder once.

## v0.8.0 (scenes · looper · rack · chorus · order fix)
- **"Pedal order doesn't change the tone" was the unity servo.** Makeup::unity used
  to be a runtime RMS servo re-matching every pedal's output to its input level each
  block — so a cranked DRIVE (or a boosted pedal upstream) was normalized away before
  the next block heard it, and Klon→TS vs TS→Klon collapsed to a subtle waveshape
  difference. It is now a FIXED per-capture correction (`ResamplingNam::unityDb`)
  measured once in `NamSlot::load` on the loader thread: 0.5 s of 220 Hz at 0.1 peak
  through the fresh net (first third discarded as warmup), ratio clamped −12..+24 dB,
  then `reset()` to flush the measurement out of the net. DRIVE and LEVEL now truly
  change the level hitting the next block. Smoke test asserts order matters
  (A(drv24)→B vs B→A(drv24) differ) and that a driven pedal never comes out quieter;
  the old `<6 dB` drive-is-not-volume assertion is gone on purpose.
- **Chorus is a built-in because it cannot be anything else**: NAM captures are
  static waveshapers, so TONE3000 will never carry modulation. `cho` is chain block
  id 7 (`kBlockChorus`), wet-only `juce::dsp::Chorus` blended by hand (MIX .5 =
  classic, 1.0 = vibrato), default order p1-p4 drv cho amp cab, packed
  `0x0504070603020100`. Order upgrade: <8 names → insert missing drv/cho before amp.
- **Backup rack**: `hiddenSlots` (StringArray, message thread, default `{"cho"}`)
  lives in PLUGIN STATE, not prefs — presets restore the whole board. UI migrates
  legacy `prefs.hidden` once at boot. Hiding stomps the toggle off; unhiding turns it
  on. Only p1-4/drv/cho/dly/rev are hideable. The audio thread never reads
  hiddenSlots — bypass is entirely the toggles.
- **Scenes** (4, default Clean/Rhythm/Solo/Lead): full param snapshots via
  `RangedAudioParameter::convertFrom0to1(getValue())` over `getParameters()`,
  recalled with begin/endChangeGesture + `setValueNotifyingHost`. Gear/slots are NOT
  in a scene. Serialized as a SCENES tree in session state and .ampcade presets.
  UI: chips in the topbar; click = recall (or save if empty), right-click menu =
  save/rename/clear, keys 1-4 recall. Scene values ride the existing relays, so
  knobs/stomps update themselves — no re-render needed.
- **Looper** (`src/Looper.h`): end of chain, post out_gain, pre soft-ceiling (the
  ceiling protects live+loop sum). Audio thread owns the state machine; message
  thread posts one-shot commands via an atomic, status (state/pos/len) rides the
  24 Hz `meters` event as `loop`. Tap: empty→count-in(4 clicks, 120 BPM)→record→
  overdub→play→overdub…; stop keeps the loop, erase clears, save = FileChooser →
  24-bit wav (UI hides save during overdub so the buffer copy is stable). Spacebar
  taps unless typing or an overlay is open. Max 60 s, loop survives prepareToPlay
  at the same sample rate, dies on SR change.
- **History + favorites** live in ui-prefs.json (`recents` ≤24, `favs`) — right for
  "stuff I used before" since it spans sessions/instances. Browser overlay shows
  FAVORITES / RECENTLY USED chip rows (filtered by the slot's format+gears) while
  the query is empty; ★ on result cards. Entries carry toneId/modelId/gear/format
  so one click re-loads via t3kLoad.
- **Declutter**: bottom panel collapses to a 46 px hint bar when nothing is
  selected; save/load/theme/settings all live behind the one gear menu (topbar is
  brand · scenes · connect · menu); chevrons dimmed. Board layout otherwise intact.
- **Feedback round (same release):** looper is a selectable pseudo-slot (`sel==='loop'`,
  guard every `SLOT_META[sel]` access) with a full transport in the bottom panel,
  live-updated by the same 24 Hz tick; EVERYTHING is hideable now incl. amp/cab
  (bypass = their own toggles, board can be empty); rack items are draggable onto
  the chain (drop between blocks to place — `rackDrag` beside `dragSlot` in
  wireDrag) and board blocks can be dropped on the rack tab to hide; IN/OUT meter
  towers toggle from the gear menu (`prefs.hideMeters` — gate/trims unreachable
  while hidden, that's the player's call); third theme `plain` (flat, no tweed, no
  screws) — theme menu item CYCLES light→dark→plain. Trap fixed: `.loop-minis` set
  `left:50%` while base `.minis` kept `right:6px` → both edges pinned → the three
  buttons squished. `right:auto` when overriding an absolute row's anchor.
## v0.8.1 (looper v2 · tuner · dynamic scenes)
- **The "looper panel buttons don't work" bug was a WKWebView click-swallow**:
  updateLooperUI rewrote the buttons' textContent/disabled 24×/s (even to identical
  values); a DOM mutation on the pressed element between mousedown and mouseup
  cancels the click in WKWebView. ALL periodic UI writes are now change-guarded
  (`wr(last,key,val,apply)` helper); only the status line — its own element, never
  pressed — streams freely. Apply this rule to anything updated from the meters tick.
- **Looper config**: count-in is one bar at `bpm` (40-240) × `beats` (2-7), and
  `playAfterRec` decides whether closing the first recording lands in Overdub or
  Play. Atomics on Looper, set via `looperConfig` native fn, persisted as root
  properties (looperBpm/looperBeats/looperPlayAfterRec) in state + presets, echoed
  to the UI through getUiState.looper. Looper + tuner are hideable pseudo-slots
  ("loop"/"tun" in hiddenSlots); 'loop' has no SLOT_META — every rack/board filter
  needs the special case, and re-adding 'tun' must NOT auto-engage it (that mutes).
- **Tuner** (`src/Tuner.h`): YIN on a 4× averaging-decimated input tap (post
  in_gain, PRE gate — the gate eats the tail you're tuning), window 512/lags to
  ~36 Hz, level-gated at -60 dBFS, ~45 ms hop, freq atomic → meters event
  (`tunerHz`). Params tun_on + tun_mute (mute default ON); mute is a smoothed gain
  on the FINAL output (after looper — silences live+loop like a real tuner).
  Big note + cents needle render in the panel; stomp or Engage toggles.
- **Scenes are a dynamic vector** (max 8): sceneAdd snapshots current settings,
  sceneDelete reindexes activeScene, old fixed-4 SCENES trees load by child order.
  Topbar shows ONE "SCENES" chip (active scene name when collapsed); expanding
  reveals chips + "+ New". prefs.scenesOpen remembers the drawer. Keys 1-8.
- Old states migrate: pre-0.8.1 versions get "tun" appended to hiddenSlots
  (versionAtLeast() compare in both setStateInformation and loadPresetFrom).
- Loop layers are recorded POST-everything (chain, delay, reverb, out gain), so
  changing the rig after recording never alters existing layers — by construction.
- **"The looper isn't working" was the AUDIO DEVICE, not the looper.** `sample <pid>`
  showed caulk messenger threads but NO `com.apple.audio.IOThread` — the standalone's
  device never started IO, so processBlock never ran and every looper command sat
  unconsumed while the UI looked alive. Compounding trap: JUCE standalones save
  `shouldMuteInput=1` (feedback protection) in `~/Library/Application Support/
  AmpCade.settings`, which silences the guitar even when audio runs. Fixes:
  (1) `lastBlockMs` heartbeat stamped in processBlock → editor sends `audioDead`
  with the meters → UI shows an unmissable amber banner after ~1.2 s of silence,
  click = `openAudioSettings` native fn → `StandalonePluginHolder::getInstance()->
  showAudioSettingsDialog()` (null in hosts → toast pointing at the DAW);
  (2) editor constructor unmutes the standalone input at launch. The smoke test
  now also drives the Looper state machine directly (tap→count→record→close→
  dub/play, click energy, playAfterRec), so "looper broken" can be split into
  engine-vs-transport in seconds.
- Looper tempo: BPM is now a typeable field + TAP tempo button (last 4 tap
  intervals averaged; taps >2 s apart reset). tap state lives at module level —
  the panel re-renders without losing it. Inputs inside the app must
  `stopPropagation()` on keydown or spacebar fires the looper.
- Scene rename input was crushed to a sliver by `.menu button{width:100%}`
  bleeding onto the Rename button inside the menu — scope-override buttons you
  place inside `.menu` (`.menu .scn-menu-rename .btn{width:auto}`).
- CAB INSIDE badge removed (amp-cab captures just look like amps now).
- **X means GONE, everywhere.** Removing a loaded amp/cab no longer falls back to
  the built-in on the board: clearSlot + hideBlock, so the slot lands in the rack
  as its built-in identity ("Tweed Clean"/"Tweed 1x12"). An emptied board can
  always be rebuilt from ADD alone.
- **ADD is a visual sheet now** (the plain text menu "was so ugly" — owner): level 1
  = four category tiles drawn with the board's own parametric gear art (Pedals /
  Amps / Cabs & IRs / Effects & Tools), level 2 = INCLUDED gear + rack captures
  first, then a dashed "Browse TONE3000" hero card. On-board items render disabled
  with "On the board". `asItemEl('loop')` — remember 'loop' has no SLOT_META, so
  never call onBoard()/blockIdentity() on it (this trap has now bitten twice).
  ADD button is always visible; Esc steps back a level before closing.
- ADD sheet polish round: no tile subheadings, art bottom-aligned in a fixed
  `.as-cat-art` box (same floor whatever the aspect), "Built-in Tools" naming;
  level 2 is "INCLUDED & FAVORITES" — starred TONE3000 gear renders as gear-art
  cards (autoStyleFor on the title) and loads on click (pedals pick the next
  free slot). **ensureOnBoard(slot)** runs after EVERY successful load
  (browser, quick chips, favorites, imports) — loading into a slot that was
  parked in the rack must land on the board, not silently in the rack. Scene
  chips are pastel-coded per index (`--scn-h` hue custom prop, light+dark
  variants in CSS). Send-to-rack buttons use the shelf glyph (#i-rack) — the
  old down-arrow-into-tray read as a download button.
- **jsdom harness** (scratchpad pattern): strip `type="module"` and the whole UI
  runs headless in Node for DOM assertions/interaction tests — caught a duplicate
  `const` the plain browser preview would have caught only at runtime. The real
  check is still the standalone + `~/Library/AmpCade/ui.log` (`diag scenes=… loop=…`
  line at boot).

## v0.9.0 (undo · scenes-own-the-board · looper save-or-toss · native title bar)
- **Mojibake root cause was C++, not the webview charset.** `juce::String(const
  char*)` reads bytes as Latin-1, so every literal with a real `…`/`—` (busy
  labels "Fetching…", IR-routing toasts) rendered as `â€¦` even after the
  v0.3 charset fix. All non-ASCII literals now route through a `u8()` helper
  (`CharPointer_UTF8`); a grep guard: `grep -n '"[^"]*[^\x00-\x7F][^"]*"' src/*`
  must only hit lines wrapped in `u8(`/`CharPointer_UTF8`.
- **Undo/redo** (Cmd/Ctrl+Z · Shift+Z · Ctrl+Y): whole-rig ValueTree snapshots,
  NOT juce::UndoManager (param attachments bypass the ValueTree). Knob bursts
  mark `paramDirty` via an APVTS listener (atomics only — can fire on the audio
  thread) and coalesce into one step after ~900 ms of quiet (timerCallback);
  every structural native fn calls `beginUserAction()` first. `notifyState()`
  refreshes the baseline snapshot. Restores go through `applyRigTree` with
  `restoringState` suppressing dirt. Snapshots MUST be deep copies —
  `replaceState(params.createCopy())`, or live edits mutate the history.
- **rigToTree()/applyRigTree()** now the single source for session state,
  presets, "New preset" (resetRigToDefault) and undo. `slotsFromTree` records
  `prevEnginePaths` so `restoreSlotsFromState` skips reloading a file the
  engine already runs — scene flips and undo between same-gear states move
  knobs only, no re-download/reload, no convolution drain.
- **Scenes own the board now**: sceneSave also captures chain order,
  hiddenSlots and the SLOTS tree (`hasBoard` flag); recall restores them, so a
  Klon racked/removed in scene 1 comes back when scene 3 is recalled. Pre-0.9
  scenes have no board snapshot and recall knobs only. Scenes still live inside
  presets, so each preset has its own set.
- **New pedals start at noon**: a *different* capture landing in a pedal slot
  (user-initiated t3kLoad/import only — never state restore, never a setting
  variant of the same tone) resets drive/tone/level to defaults and stomps on.
- **Looper is save-or-toss**: stems stream into `Loops/Unsaved/<session>`;
  SAVE LOOP (`Looper::saveSession`, no chooser) writes `Loop (mix).wav` and
  moves the folder into `Loops/`. Erase enqueues a discard marker **through the
  audio-thread fifo** (code 2) so it stays ordered with layer opens/closes —
  a message-thread delete raced the writer and could kill the NEXT session.
  Unsaved leftovers >6 h old are swept at startup (age guard: a second
  instance's live session must survive). After save, the writer follows the
  moved folder via `writerDirMoved` under `sessionDirLock`. Count-in is now
  optional (`countIn` in looperConfig/state; off = tap is beat one).
- **Tuner stopped blinking out**: level gate lowered -60 → -75 dBFS and the
  last confident pitch is held for ~24 detect hops (~1.1 s) across dropouts.
- **Standalone gets the native OS title bar** (`setUsingNativeTitleBar(true)` +
  `allButtons` in configureStandaloneWindow — the shell ships minimise+close
  only, which is why there was no zoom button). The switch RECREATES the peer:
  the macOS fullscreen collectionBehavior tweak must re-fetch getPeer() after.
- UI: board/panel/rack text is never truncated (clamps removed, gear-text
  stack 44→62 px, meters margin 48→66); Esc or clicking empty board space
  deselects to the clean view; digit keys 1-8 and Esc always preventDefault
  (unhandled keys reach the native view → macOS "funk" beep — that was the
  scene-switch sound); preset menu gained "✳ New preset (fresh rig)".
- Verified: SmokeTest (extended: save/discard/count-in-off assertions) + full
  build green; UI behaviors driven headlessly in the browser mock. Standalone
  boot check blocked on the re-sign → mic-permission dialog (known trap above).
- **Feedback round (same release):** webview context menu suppressed
  (document-level `contextmenu` preventDefault, text fields exempt — the
  "Reload" popup meant nothing in a plugin); gear is renameable from the
  Design sidebar (stored as `name` in the same prefs.styles override,
  `blockIdentity` applies it — key computed INLINE there because styleKeyFor
  calls blockIdentity and would recurse); the tiny paint icon became a
  labeled "Design" pill at the bottom of the panel's identity column; grille
  rows hidden for head-shaped amps (heads draw no cloth, the controls did
  nothing); preset menu scrolls (`max-height` + sticky save row) and "New
  preset" lost the "(fresh rig)" suffix; default scenes reordered
  Clean→Rhythm→Lead→Solo; browser quick-row chips wrap full names; the
  bottom panel is content-sized (`min-height:170px; max-height:46vh` — the
  looper's wrapped config row used to clip at the fixed 170px; `.panel.min`
  must override min-height too); tweed-washed panels force dark-brown
  `--pn-*` inks in light themes (theme orange was ~2.8:1 on the weave).

## v0.9.0 feedback round 2 (glow · shared rack · looper undo/backup · ship prep)
- Selection glow + name chip take the gear's OWN colour: `--sel-c/--sel-glow/
  --sel-ink` set per card in `applySelColor` (tweed → weave brown, looper →
  its red); CSS falls back to the theme accent. Topbar logo redrawn to match
  the dock icon (tweed rounded square + knurled knob; the knurl is a
  stroke-dasharray ring, `#pat-tweed` reused cross-spritesheet).
- **Pedal captures are preset-wide, not per-scene.** `recallSceneBoard` MERGES:
  a live loaded pedal the scene's snapshot doesn't place goes into the scene's
  rack (hidden + kept loaded) instead of being deleted — add an overdrive in
  Rhythm and it waits in Clean's rack. Amp/cab/air stay scene-exact on purpose
  (an empty amp slot in a scene MEANS the built-in amp sound).
- **Looper layer undo/redo** (one level, hardware style): the overdub pass
  shadows its added samples into `undoBuf` (first-touch per index for lap one,
  `+=` after), pass region tracked as (dubStart, dubCount). Undo/redo are cmd
  codes 4/5 — the add/subtract walk runs on the audio thread (≤ aLen samples;
  instant for musical loops, worst case one audible block on a 5-min loop).
  `undoBuf` is allocated WITHOUT clearing so its ~115 MB stays lazy pages.
  A new overdub pass discards the undo layer; erase resets everything.
- **Rolling auto-backup**: an unsaved session is never deleted — erase, quit
  and the startup sweep all promote it to `Loops/Last loop (auto-backup)`
  (previous backup overwritten; startup promotes only the NEWEST >6h-old
  leftover and only if newer than the current backup).
- Ship prep: first-name/mock-username references scrubbed from tracked files;
  commit history rewritten to a noreply author (the old commits carried a real
  email + machine hostname). The TONE3000 key in Version.h is the PUBLISHABLE
  key — safe to ship by design; the secret (t3k_cs_…) must never land here.

## v0.9.0 pre-upload round (scenes drag · chorus v2 · tuner stability · looper keys)
- Scene chips drag-to-rearrange (`sceneMove` from/to; activeScene follows its
  scene through the splice). Keys 1-8 track the new order automatically.
- **Chorus rebuilt as a dual-voice unit** (the "warbly" complaint): one
  modulated tap IS pitch wobble; two taps on opposite LFO phases cancel it
  into CE-1-style shimmer. Base delays 13/21 ms, excursion ≤2.6 ms, voice 2 at
  1.13× rate so the swirl never phase-locks, default rate 1.2 → 0.6 Hz.
- **Tuner A-string flapping**: the YIN early-out ("first dip under threshold")
  alternated between the true period and the half-period as a decaying pluck's
  2nd harmonic came and went. Now the full lag range is computed, a clearly
  deeper dip at a longer lag overrules a shallow early one, and a >6% jump
  must repeat on two consecutive detections before the display follows
  (`candFreq`). Small drifts (actually tuning) pass straight through.
- **Looper keys**: L selects (unracks if needed); SPACE tap fires on RELEASE
  so a ~0.6 s hold can mean undo/redo of the last overdub layer (hold toggles);
  double-tap SPACE or ENTER = stop. Enter never steals from a focused
  button/knob (closest-widget check). Trade-off accepted knowingly: tap-on-
  keyup adds the key-release latency (~50-100 ms) to loop-close timing.
- ADD sheet: "Browse TONE3000 IRs" → "Browse amp cab IRs" (read as ONLY IRs
  next to the reverb/delay row).

## v0.9.1 (first field reports after the public release)
- **Racked means DISENGAGED, on scene recall too.** The scene-merge (a pedal
  added in another scene parks in this scene's rack) left the pedal's `_on`
  param wherever the scene's snapshot had it — an invisible engaged overdrive
  in the chain. recallSceneBoard now stomps every hidden slot's `_on` off
  after setHiddenSlots, matching the invariant hideBlock keeps in the UI.
- **Scene switches render ONCE.** Recall mutates order → rack → slots in
  sequence and each notifyState pushed an intermediate board — blocks visibly
  jumped mid-switch. beginStateBatch/endStateBatch coalesce the pushes (also
  wrapped: applyRigTree, resetRigToDefault). Message-thread counter, not a
  flag — the paths nest.
- Scene strip: slim hover-tinted scrollbar + plain vertical wheel scrolls it
  horizontally (only trackpads/tilt wheels could reach off-screen scenes).
- CI lesson from the v0.9.0 release: Actions artifacts are login-gated — a
  public download REQUIRES the tag-triggered Release job. And the first real
  Windows build needed NOMINMAX on the plugin targets (std::min/max in
  NamSlot.h vs windows.h), NEEDS_WEBVIEW2 TRUE, and the WebView2 SDK from
  nuget in the workflow. Xcode 15.4 folds the zero-size anonymous-namespace
  NAM registrars into unnamed init code → parsers are now registered
  EXPLICITLY in keepNamArchitecturesLinked (has()-guarded) and the check
  script greps the external create_config symbols instead.

## v0.9.2
- **Every reverb/delay IR was rejected** ("That's an IR — load it into the
  Cab · IR slot"): requestT3kLoad's format triage predates the AIR slot and
  only counted `slot == "cab"` as an IR destination. `wantIr` now covers both
  convolution slots. Field-reported within hours of release — the in-app
  browse path for the air slot had never been exercised against real TONE3000.
- Keys: S toggles the scenes drawer; P toggles the preset menu and, while it
  is open, digits 1-9 load presets by row (the keyboard-opened menu skips the
  name-input autofocus so digits act instead of typing); closing hands the
  digits back to scenes.

## v0.9.3
- **Scene saves survive preset switches now.** Scenes live inside preset
  files, so a scene save without a follow-up preset save evaporated on the
  next preset load. Every scene mutation (save/add/rename/delete/move) now
  quietly rewrites ONLY the SCENES block of the active preset file
  (`syncScenesToPresetFile` — XML surgery, the preset's own saved rig stays
  untouched, so unsaved knob tweaks are NOT committed as a side effect).
- Loading a preset lands on scene 1 (recalled if used, batched into the same
  single render). Preset menu rows show their 1-9 digit badges.

## v0.9.4
- **Scene switches pumped the loop's volume.** The -6 dBFS soft ceiling sat
  AFTER the looper, compressing the live+loop SUM — so the current scene's
  out_gain modulated how hard the recorded loop was being squashed. The
  ceiling now runs on the live rig BEFORE the looper (the loop records and
  replays that exact signal at that exact level forever); after the loop sum
  only a near-transparent overload guard remains (knee -1 dBFS). meterOutPre
  is now live-only; meterOut stays post-everything.
- Smoke-test race fixed: "Unsaved is empty" is true BEFORE the writer thread
  drains the first marker — wait for the backup folder (the discard's real
  postcondition) instead.

## v0.9.5
- The OUT tower's "SOFT CLIP −x" readout wrapped inside the ~32 px tower and
  RESIZED it, reflowing the whole board row on every clip — the "stretching"
  and the "lines popping out beside the amp" (the tube-glow halo smearing
  through the reflows) were the same event. Both removed: no clip readout
  (the dB number's hot/warn coloring still flags a hot output), no
  `.slotwrap::before` glow. Rule: NOTHING in the board row may change size
  from the meters path.

## v0.1.0 — the public renumber (dev 0.9.x history retired)
- **The "fake Clean scene" was the silent tap-to-save**: tapping an UNUSED
  scene chip captured the current board into it (v0.8 design), and the
  v0.9.3 auto-sync then baked that into the preset file. Tapping an empty
  scene now just points at right-click; saving is always explicit (SAVE
  button / right-click / + New). Presets poisoned before this fix need one
  manual re-save of the scene.
- Preset menu: digits 1-9 work however it was opened; the name field is
  never auto-focused (click it to type).
- Board shows names only — no "built-in"/setting subheadings (panel carries
  those); the looper keeps its live status line.
- Renumbered 0.9.5 → 0.1.0 for the first public release. The version-gated
  tuner migration was DELETED with it: "saved before 0.8.1" is meaningless
  once new states say 0.1.0 (it would have re-racked the tuner on every
  load). Old v0.9.x releases/tags deleted from GitHub.

## v0.1.0 re-upload (pre-share fix round)
- **The vertical light bands beside gear while playing were WKWebView paint
  artifacts, not DOM.** Meter fill/peak animated `height`/`bottom` at 60 fps,
  repainting shared background tiles every frame; visible mini rows also
  carried a `backdrop-filter`. Fixes: towers get their own compositing layer
  (`transform:translateZ(0)`), fill/peak move by `transform` only
  (change-guarded, so an idle meter costs zero), minis lost the
  backdrop-filter (88%-opaque chip — the blur bought nothing). Extends the
  v0.9.5 rule: the meters path may not repaint ANYTHING outside its own layer.
- **The colour picker's "square on a circle" was a WKWebView conic-gradient
  bug** — a conic background running under a dashed rounded border
  double-paints its content box as a bright square. `background:conic-…
  padding-box` (and no overflow:hidden) renders clean. Chromium never shows
  it: verify WebKit-specific paint with the headless snapshot harness
  (scratchpad `wksnap.swift`: WKWebView → takeSnapshot → png, no window, no
  mic prompt) — this is the tool that caught the first fix attempt not working.
- **Scene switches animate now (FLIP)**: renderBoard records each keyed
  child's left edge before the rebuild, then Web-Animations survivors from
  their old spot and fades newcomers in. Keys: `data-slot` (towers `_in`/`_out`,
  looper `loop`). WAAPI, not CSS classes — it can't fight `.card`'s hover
  transition, and prefers-reduced-motion is honoured explicitly (the CSS
  kill-switch doesn't reach WAAPI).
- **Scene pills drag from anywhere now**: hit-testing used to land on the
  chip's child spans, so a drag only started from the bare padding at the
  pill's bottom edge — children are `pointer-events:none` (save pill opts
  back in, and preventDefaults its own dragstart). SAVE pill is hidden while
  the scene matches (max-width/opacity collapse, −6px margin swallows the
  flex gap), slides open on the sceneDirty flip via class toggle on the live
  node — a re-render would swap the node and skip the transition.

- **Rebranded the plugin identity to plain "AmpCade"** (the old owner name in
  Logic's manufacturer column was confusing): COMPANY_NAME AmpCade, bundle id
  com.ampcade.ampcade, AU manufacturer code `Ampc` (was RyRt). This changes
  the AU triple (aufx/Acad/Ampc) and the VST3 ids — DAW sessions saved
  against pre-rebrand builds will not find the new plugin. Done in the
  re-upload window precisely because nobody has such sessions yet; never
  touch these three fields again post-release.

## v0.1.0 re-cut (pre-public audit round)
- **One home per action.** The bottom panel's Browse/To-rack/Remove buttons were
  exact duplicates of the card's own minis (visible whenever selected via
  `.card.sel .minis`); the panel's actions column now holds ONLY local-file
  Import — the one action with no home on the card. Same cut in the looper and
  tuner panels. The missing-file re-download path moved onto the card: bSwap
  prefills the search with `ss.title` when `ss.missing`.
- **Gear menu is audio/meters/settings only.** Save/Load preset deleted (the
  chooser-based Load also left `prefs.lastPreset` stale, so scene auto-sync
  rewrote the WRONG preset file on disk — removing the path removed the bug);
  the theme cycler deleted (Settings has the full picker). Preset dropdown
  gained "Import preset(s)…" (`importPresets` native fn: multi-select chooser →
  validate AMPCADE root → copy into presetsDir, uniquify on name collision
  unless byte-identical, auto-load single imports) and "Show presets folder"
  (`revealPresets`) — presets are self-contained XML, so Finder drag-out IS
  export. savePreset/loadPreset native fns are gone.
- **The "white box around a pedal" was `:focus-visible` promotion**: click
  focuses the card (tabIndex=0), the NEXT keypress promotes it to
  :focus-visible and paints the accent outline. Fix = pointer-modality class:
  `pointerdown` (capture) sets `html.ptr` which kills all :focus-visible
  outlines; Shift+Tab removes it. Never suppress :focus itself — keyboard
  users keep rings.
- **The macOS beep + dead shortcuts had TWO causes.** (1) JUCE's
  becomeKeyWindow → grabFocus makes the peer NSView first responder on EVERY
  activation, stealing keys from the WKWebView. **NEVER fix this with
  setWantsKeyboardFocus + grabKeyboardFocus** — that shipped for one build and
  made EVERY key beep: Component::grabKeyboardFocus always re-firsts the PEER
  view, the WKWebView resigns (its hook poisons lastFocusChange to "unknown"),
  focusGainedWithDirection then gives focus away, and the 24 Hz retry loops
  the fight forever. The real fix is AppKit-level, no JUCE focus at all: from
  the timer, when `[window firstResponder] == peer NSView` (exactly the
  post-steal dead state), find the WKWebView descendant and
  `makeFirstResponder:` it — the programmatic version of "click the middle of
  the app". Skips: auth overlay up, window not key, or a page text field
  focused (FR is then a WKContentView, not the peer view). (2) Any keydown
  the page didn't claim bubbled into the native view (body can't scroll = no
  default action) — a final catch-all document listener preventDefaults
  everything unclaimed except text fields, modifier combos, Tab, and
  Enter/Space aimed at real `button`/`a` elements, and scroll keys whose
  target sits inside an actually-scrollable ancestor (Settings/browser/ADD/
  rack are inner scrollers — WebKit consumes those keys by scrolling, so
  swallowing them broke keyboard scrolling in every overlay). Both the
  catch-all and the Tab branch are gated on `state.standalone` (getUiState
  sends `wrapperType == Standalone`) — in a DAW, unclaimed keys belong to
  the host. Rule: every new key branch can skip preventDefault only if the
  catch-all will see it.
- **First-launch focus weirdness was the native-title-bar flip**: it ran from
  the first timer tick, ~40 ms AFTER the shell showed and keyed the window —
  destroying and re-keying a brand-new NSWindow mid app-activation. It now runs
  from `parentHierarchyChanged()` gated on `! dw->isShowing()` — NOT on
  "no peer": DocumentWindow's ctor adds itself to the desktop, so the window
  is ALWAYS peered by the time the editor exists (a getPeer()==nullptr guard
  is dead code — review caught exactly that). Recreating a still-hidden
  window is harmless; the timer path stays as an idempotent fallback.
- **Tab toggles the rack, D (rebindable) toggles Design.** Tab preventDefaults
  unconditionally (empty rack → toast); Shift+Tab keeps reverse focus-nav as
  the keyboard escape hatch — forward Tab-walking of cards/knobs is gone by
  owner's choice. New KEY_ACTIONS entries auto-appear in Settings and are
  auto-protected from stomp assignment; boot migration frees a pre-existing
  'd' stomp binding with a toast.
- **Gear size slider** (Settings → APPEARANCE, prefs.gearScale 1.0–1.5):
  a `--gs` CSS var multiplied via calc() into the FOUR width sources (.card
  116px, .sprite 86×96, SHAPE_W inline width, looper 118px). Layout-based
  scaling, NOT transform:scale — transforms don't affect flex flow, so scaled
  cards would overlap and the scroller wouldn't grow (getBoundingClientRect
  was never the problem; rects are post-transform). The APPLIED scale is
  capped to the board's headroom (`(clientHeight − 148) / 166`, re-run via a
  ResizeObserver on .board-scroll) because the card minis are now the ONLY
  Swap/Rack/Remove path and at 1.5× in a minimum-size window they clipped
  above the board's overflow, unreachable. prefs keeps the requested value;
  a taller window gets the full size back.
- **Grey grille got the tweed-brown weave** because pattern choice was a pure
  luminance threshold — light+low-saturation cloths (grey/silver/white) now
  route to `pat-latticegrey` (#42464e strokes); warm light cloths keep
  `pat-lattice`. Threshold: max−min channel spread ≤ 28 (grey 17, cream 29,
  wheat 91). Heads take GRILLE COLOUR now (front rect fill + weave + ink that
  flips for light cloths); Cloth/See-through stays combo/cab-only. New
  `cab212v` vertical 2×12 (portrait 150×240 viewBox, SHAPE_W 122 — sized so
  its 12" speakers render the same size as the horizontal 2×12's, r40/150 vs
  r39/240; the gear-scale headroom cap constant tracks its ~195px height);
  AUTO_RULES
  fixed so "2x12" names get cab212 art (they used to collapse to 1x12).
- Descriptions pass: every "does X, never Y" / bug-defense / design-history
  string rewritten to plain function ("what it does", new-user voice) — panel
  descriptions, IR notes, ADD subs, IR-routing toasts (PluginProcessor), stale
  "filter chip" hint, connect hero. Rule going forward: describe the thing,
  not the conversation that shaped it.
- **Owner feedback round on the above:**
  - Scene strip clipped the active chip's 2px ring: `.tb-center` is an
    overflow-x scroller (so it clips vertically too) and only had
    padding-BOTTOM. Symmetric `padding:3px 0` — any chip decoration bigger
    than 3px needs the padding bumped with it.
  - Gear-size slider is 0-based (`min=0 max=50` = percent above 100):
    WebKit's accent-color range draws a phantom slidable-looking dead zone
    left of the fill when `min > 0`.
  - On-gear labels: `st.labelColor` (Design → LABEL COLOUR, amps + pedals)
    overrides every ink; head auto-ink is plain dark/light by grille
    luminance — the tweed-brown ink was illegible on the wheat cloth.
  - Design sidebar is board-scoped now (appended to #board, absolute
    top/bottom) exactly like the rack: sidebars stop at the panel. NEVER
    margin-push .panel — its flex layout collapses into overlapping knobs;
    only .board-scroll slides.
  - Dock icon: solid rect measured 832px @ (96,96) r≈186 CIRCULAR corners;
    macOS 26's mask is the 824-grid CONTINUOUS-corner shape whose corners
    bulge past a same-radius circle → corner notches in the dock. Fixed by
    compositing a rounded-rect backdrop (840 span, r185, per-corner sampled
    tweed fill) UNDER the art — overfills the mask so macOS cuts the exact
    shape. The icns is generated at CMake CONFIGURE time (deleting it breaks
    the build target) — re-run `cmake -S . -B <dir>` after changing
    assets/icon_1024.png.
  - **Head-on-cab stack** (`prefs.stackHead`): drag the amp over the middle
    50% of the cab card (`drop-stack` ring) to sit it on top — renders as a
    `.stackwrap` column at the first of the pair's chain positions with one
    shared label (the head's .gear-text is display:none). Dragging the amp
    anywhere else takes it down (movePedal clears the pref). Stacking also
    moves amp to just before cab in the chain; drops from other gear onto
    the stack map left→before the head, right→after the cab, so the chain
    always matches the board. Render falls back to side-by-side whenever
    amp/cab is racked or the amp isn't head-shaped — the pref survives.
  - **Flush stack art** (owner mockup): stacked variants ride a style flag
    (`st.stack = 'head'|'cab'` injected by slotWrapEl's second arg, board
    cards only). Head loses its feet; cab loses its handle; both square off
    at the seam by CROPPING via viewBox + a translate group — interior
    coordinates never change, so the variants can't drift from the normal
    art. Width match is BODY-to-body, not card-to-card: each art insets its
    body differently in the viewBox, so headCardW = cabCardW ×
    CAB_BODY_FRAC[shape] / (220/240) (fractions in renderBoard). The stacked
    head also loses its hover-lift so it never floats off the cab.
  - cab412 redrawn to the family construction (handle + rx13 body + rx7
    grille, no slant top, no corner protectors) — it read as a different
    species next to the others.
  - The Design sidebar's background is in the board's click-to-deselect
    exception list — deselecting closes the sidebar, so a stray click inside
    it was collapsing the panel. Any future full-height board child needs
    the same exemption in the #board click handler.
  - Long IR names: .pn-cab clamps the title to 2 lines and holds
    min-width:170px so a tiny window can't crush it to one word per line;
    the AIR note is one line again.
  - Stack polish: the stacked cab's minis rise above the head
    (`--stack-lift` = head card height, set in renderBoard, × --gs in the
    CSS); the stacked head's outline strokes are multiplied by stackK
    (cab px-per-viewBox-unit ÷ head px-per-unit, third slotWrapEl arg) so
    the shrunk head keeps the cab's line weight — remember cab212v's
    viewBox is 150 wide, not 240, when touching that math.
  - Cabs lost the KNOBS/KNOB COLOUR design rows (no knobs in the art).
    Knob styles grew 'skirt' (Marshall skirted), 'knurl' (MXR ridged cap),
    'point' (Davies/tweed-era nose pointer) and 'dome' (gold dome) in
    knobArt/KNOB_CHOICES. The BOTTOM PANEL's knobs follow the selected
    gear's style too: each style is a hidden pointer-variant element inside
    makeKnob's kface, shown by a panel class from KN_CLASS in
    applyPanelStyle (this also fixed 'flute' never reaching the panel — a
    new knob style needs BOTH the knobArt branch and a kface variant).
    Panel pointers stay SIMPLE LINES: the arrow/nose shapes read as blobs
    at panel size (owner call) — skirt/dome show the k-cap circle + line,
    'point' is just the line. The gear-size slider is custom-drawn
    (-webkit-appearance:none + --gs-fill gradient): the native macOS range
    pads the thumb in from the track ends even at min.
  - Stacked head keeps its FULL bottom edge (viewBox 240x132, nothing
    cropped) — cropping the bottom outline read as "cut off"; only the feet
    go. Stack label is two lines: the head's .gear-name is cloned into the
    cab's .gear-text (the head's own gear-text stays display:none).

## v0.1.0 re-cut, round 2 (testing feedback + sharing features)
- **Boosts swing negative** (b1-b3 gain −24..+24, def +6): a "boost" is now a
  volume pedal anywhere in the chain. Old 0..24 saves land unchanged inside
  the wider range — APVTS stores plain values, not normalised.
- **Key combos everywhere.** `comboOf(e)` canonicalises a keydown to
  'q' / 'f13' / 'ctrl+alt+q' / 'meta+shift+3' (modifier order fixed:
  meta,ctrl,alt,shift). Plain single keys keep `e.key` (layout-correct AND
  back-compatible with every stored binding); with Alt/Shift held the
  PHYSICAL key comes from `e.code` — mac Alt-characters ('ø') and shifted
  digits ('!') must never be stored. `comboLabel()` renders ⌘⌃⌥⇧ chips.
  Actions, stomps, scenes and presets all match through one branch in the
  main keydown handler; unbound modifier combos fall through untouched so
  system shortcuts keep working. RESERVED_KEYS only blocks PLAIN keys.
- **Per-scene / per-preset keys**: scenes bind by INDEX (prefs.keys.scenes —
  sceneDelete closes the gap by reindexing), presets by NAME
  (prefs.keys.presets — deletePresetNamed clears the binding). Assigned from
  the scene chip's right-click menu ("Set shortcut key…") and right-click on
  a preset row; both listed + rebindable in Settings. captureCombo() is the
  menu-driven capture (no steal flow — taken combo = pick another).
- **Number keys are switchable** (Settings → prefs.numKeys): 'scenes'
  (default) or 'presets' — digits then load by list position via
  listPresets. While the preset menu is open digits pick rows regardless.
- **Presets carry designs**: savePresetAs sends rigDesigns() (every style
  override the current gear carries, keyed like prefs.styles) as a JSON
  string; savePresetTo stores it as a "designs" property; loadPresetFrom
  stashes it in `presetDesigns` which rides getUiState; applyState merges it
  into prefs.styles ONCE per load (appliedPresetDesigns guard). The engine
  never parses it.
- **DOWNLOAD MISSING GEAR banner** (#dl-warn): restoreSlotsFromState already
  auto-fetches fetchable slots when CONNECTED at load time — the banner
  covers "connected later" and failed fetches: shown when any slot is
  missing with a toneId+modelId while connected; click = sequential t3kLoad
  per slot. Sits under #audio-warn when both are up.
- **Loop import** (`looperImport` → Looper::importLoopFile): message thread
  reads + Lagrange-resamples the file into a FULL-SIZE staged buffer
  (kMaxSeconds — a smaller buffer would shrink the max length of the NEXT
  recording after the swap), then cmd 7: the audio thread std::swap()s the
  buffers (pointer swap, zero alloc), resets pass/dub/session flags like
  erase does, lands in Play. Handshake: importPending/importAdopted atomics;
  if audio never consumes the command it is CAS'd back out and the fn errors
  "Audio isn't running". Old loop storage returns via importBuf and is freed
  on the NEXT import's move-assign — never on the audio thread. Looper.h has
  no u8() helper: keep its user-facing strings pure ASCII.

## v0.1.0 re-cut, round 3 (owner test feedback)
- Stacked cab art shifts by (bodyTop − strokeHalf) — e.g. −9.5 for cab112,
  −11.5 for the rest — so the TOP EDGE of the outline lands exactly at
  viewBox 0: full stroke, fully rounded top corners, zero gap. The earlier
  handle-band shift cropped 2 units of corner and read as "cut flat".
- Shortcut removal is right-click everywhere: bound preset rows remove on
  right-click (unbound rows capture), scene menu grows a "Remove shortcut
  key" item. Scene chips show the BOUND KEY in their number badge
  (.scn-num is min-width now, stretches for ⌘⇧5-style labels).
- Preset menu order: rows → save row → New preset → Import preset(s) →
  Show presets folder. Digits map to rows only, so moving the utility rows
  down changed nothing for the number keys.
- Arrows: ←/→ walk the USED scenes (wrap), ↑/↓ walk presets via
  listPresets + prefs.lastPreset. Guarded on e.defaultPrevented +
  [role=slider] so focused knobs keep their arrows; inside overlays the
  early-return leaves arrows to the scroll-key exemption.
- Imported loops are level-matched to the PLAYER, not to a fixed target.
  The first cut (fixed −18 dBFS integrated RMS) still buried the guitar:
  mastered material is loud CONTINUOUSLY while sustained single-note guitar
  RMS sits ~−25..−30 even when the peak meter reads −6..−2, and integrated
  RMS lets silence in the file push the loud parts hotter. Now: processBlock
  keeps `liveLoudRms` (slow EMA of the pre-looper signal, active blocks
  only) and importLoopFile normalises the file's ACTIVE-stretch RMS (4096
  blocks above −50 dBFS) to that, clamped −32..−14 dB, default −22 when the
  player hasn't played yet, boost still capped at +12 dB. Matching the live
  level also keeps OVERDUBS balanced — cutting loop_vol instead attenuated
  the player's own dub layers along with the import (the real complaint).
- **Imports live on their OWN layer with their own knob.** cmd 7 now swaps
  TWO staged buffers: the file into `baseBuf` (the imported bed) and a
  fresh zeroed buffer (only aLen frames cleared — page-commit rule) into
  `buffer`, so overdubs land on a clean slate. Playback sums
  buffer×loop_vol + baseBuf×loop_imp; `loop_imp` (param, relay, PARAM_DEFS
  IMPORT) is a live control excluded from scenes exactly like loop_vol.
  The bed dies on a from-Idle re-record, survives erase/unerase
  (eraseHadBase), dies on SR change, and writeWav bakes it at its knob
  gain. `hasImportedBase()` rides the meters payload as `loop.imp`; the
  panel's IMPORT knob (.lc-impwrap) slides in beside VOLUME only while a
  bed exists. Auto level-matching still runs at import — the knob is the
  manual override on top.
- **Export = the heard mix.** writeWav bakes the bed at baseGain/playGain
  (the audible bed-vs-dubs ratio) with dubs normalised to unity — loop_vol
  is monitoring, not mix level — then peak-guards at 0.98 (the ratio can
  exceed full scale). saveSession also copies the imported source file into
  the session folder as "Imported - <name>" (importSrcFile, message
  thread), so a saved folder is the complete take: mix + stems + source.
- The topbar gear button IS Settings now (no intermediate menu): ACCOUNT
  (username link + quiet Disconnect, or Connect) → AUDIO (opens the shell
  dialog) → LIBRARY FOLDER → KEYBOARD SHORTCUTS (+ number-keys seg) →
  APPEARANCE (theme, background, IN/OUT meters seg, gear size). tb-menu and
  every mi-* id are GONE — renderTopbar calls renderAcct() to keep the
  account row fresh, the M key also refreshes the meters seg.
- Looper help button reads "Looper instructions".
- **UI polish round:** (1) `.panel` fixed-height was tried and REVERTED
  (owner: scrolling a tall panel is worse than the few-px jump) — it is
  content-sized again, min-height 170 / max 46vh.
  (2) The selection glow moved off the `<svg>` onto a plain `.artwrap` div:
  WebKit clips a CSS drop-shadow on an svg ROOT to a rectangular filter
  region — the occasional "sharp box around the glow". Never put big-blur
  drop-shadows directly on svg elements. (3) Fixed-position menus built from
  the base `.menu` class MUST set `right:'auto'` before setting `left` —
  .menu carries `right:0`, and left+right together stretch the box to the
  screen edge (the giant scene menu). Scene menu now centers on its chip.

## v0.1.0 re-cut, round 4 (cross-domain undo)
- **Cmd/Ctrl+Z now covers the non-engine domains** via a small UI-side stack
  (uiUndo/uiRedo in index.html): preset deletion (soft-delete —
  deletePresetNamed moves the file to presets/.trash and returns `trashed`;
  restorePreset moves it back), shortcut-key changes (ALL four setters
  record prev→next when user-initiated; system paths — removeBlock, stale
  prunes, the 'd' migration — pass record=false), loop erase (undo =
  looperUnerase while loopSt.unerase holds; stale entries skip silently),
  and scene deletion's key-map shift (a PAIRED entry: one Ctrl+Z does the
  engine undo AND restores prefs.keys.scenes; a scene-count guard keeps a
  stale pairing from hijacking an unrelated engine undo). Dispatch:
  doUndo() pops local entries first (each announces itself with a toast),
  then falls through to the engine; doRedo() replays the undo journal in
  order, skipping stale entries. A fresh local action clears the redo
  journal, mirroring the engine's own invalidation.
- Rule for new features: anything destructive OUTSIDE the rig tree either
  gets a soft-delete + a uiUndo entry, or it doesn't ship.
- Topbar is a GRID (`1fr minmax(0,auto) 1fr`), not flex: the scenes/preset
  strip now sits at the true window centre, lined up with the board's ADD
  button — flex centred it in the leftover gap, which drifted right because
  the brand and the right-side buttons aren't the same width. Right-side
  controls live in .tb-right; the strip still scrolls when crowded.

## v0.1.0 final review round (pre-tag audit fixes)
- **state.standalone was never copied into applyState's rebuilt state** — the
  DAW gate on key-swallowing silently never engaged; a hosted plugin stole
  Tab/arrows/keys from the host. applyState now carries it
  (`s.standalone !== false`). Lesson: every new getUiState field needs an
  explicit line in applyState's literal, or it doesn't exist.
- restorePreset hardened: `trashed` must be a bare filename inside .trash
  (`getFileName()` equality + `isAChildOf`) — getChildFile resolves ".." and
  absolute paths, which made it an arbitrary-file-move primitive.
- applyRigTree holds loop_imp steady alongside loop_vol while a loop plays
  (presets/undo used to snap the imported bed's volume).
- saveSession creates the session folder itself when an imported loop has
  zero overdubs (sessionDiscard on import meant Save said "No loop to save"
  while the loop played); writerDirMoved makes later stems land there too.
- importLoopFile: bails when sr<=0 (prepare never ran); re-checks
  importAdopted once after the timeout CAS (adoption in the last 10 ms was
  reported as failure); prepare() cancels a pending import on SR change so
  wrong-rate buffers can never be adopted.
- Scene-key undo is a plain UNPAIRED map entry now ('scene-keymap'): the
  clever pairing with the engine undo could hijack an unrelated knob undo
  and corrupt the map (the engine commits pending knob dirt as its top
  entry inside undo() — never assume the top engine entry is yours).
  sceneMove also remaps bound keys through the splice.
- The head/cab stack renders ONLY while amp sits directly before cab among
  on-board slots — the drawing must never contradict the signal path.
- captureCombo: single instance, click-anywhere cancels; both captures set
  spacePressHandled on Space so its keyup can't tap the looper; ⌘Z-family
  combos are RESERVED_COMBOS (bindable-but-shadowed otherwise). Preset menu
  rebuilds after in-menu delete (digit badges), prunes bindings for
  externally-deleted presets, and a preset (re)load always re-applies its
  designs (appliedPresetDesigns reset in uiLoadPreset).

## Post-0.1.0 — chain order did not survive a standalone restart
- **The launch landing was eating the board.** A custom signal-chain order
  never survived quitting and reopening the standalone (shipped 0.1.0 and
  main both). Nothing was wrong with saving or restoring it: `applyRigTree`
  put `pedalOrder` back correctly, and then the standalone-only "land on
  scene 1" step queued in `setStateInformation` called `sceneRecall(0)` —
  which recalls the scene's **board** (`recallSceneBoard`: order, rack,
  SLOTS) straight over the top of it. Every launch overwrote the order and
  the rack with whatever scene 1 was last saved holding, so the top-level
  `pedalOrder`/`hiddenSlots` were written at quit and thrown away at boot.
  The landing is now TONE ONLY (`sceneRecall(idx, withBoard=false)`); the
  scene chips and preset loads still recall the full board, which is the
  documented v0.9.0 behaviour.
- **The decoy: `hiddenSlots` looked like it survived.** It did not — it was
  being replaced by scene 1's rack, which happened to contain the same
  blocks. Two things reset by one cause can look like one working and one
  broken. Decode the settings blob and diff the live values against the
  SCENE they came from before trusting "but this half persists".
- **Tone-only recall has to re-assert racked-means-disengaged.** Keeping the
  live board means the scene's own `_on` values can reach a block the player
  has parked in the rack (scene 1 saved with the delay out front, live board
  has it racked → an invisible delay running in the chain). `sceneRecall`
  stomps `_on` off for the live `hiddenSlots` when it skips the board, the
  same invariant `recallSceneBoard` keeps for the scene's own rack.
- `test/StateTest.cpp` (`ampcade-state`) covers it: quit → relaunch through
  the real `get/setStateInformation`, standalone AND DAW. It pins the fix in
  both directions — the order and rack persist, *and* the launch still lands
  on scene 1's tone, so deleting the landing fails the suite too. It is a
  separate target from the smoke test because it drags in
  juce_audio_processors; `AMPCADE_SMOKE` strips the editor out of
  PluginProcessor.cpp so the processor links headless.

## Presets are a sharing format — treat every one of them as hostile
Found in the pre-share audit. All three are about the same realisation: a
`.ampcade` file is something strangers send each other, so it is untrusted
input in one direction and a privacy leak in the other.
- **A shared preset used to ship the sender's home directory.** `slotsToTree`
  wrote `filePath` verbatim, so every preset a player saved carried
  `/Users/<name>/Library/AmpCade/models/…`. Gear paths are now stored RELATIVE
  to `appDataDir` (`Library::packGearPath`/`unpackGearPath`, separators
  normalised to `/` so a Windows rig opens on a Mac), and `savePresetTo` blanks
  anything still absolute — a blanked slot is not a dead slot, it re-fetches by
  toneId exactly like a factory preset. Only what reaches DISK is
  de-personalised; paths stay absolute in memory, so nothing else in the engine
  changed.
- **The scene copies are the ones you forget.** Every scene keeps its own SLOTS
  snapshot, so sanitising the top-level SLOTS is not enough: a preset whose
  SLOTS block looked spotless still carried 26 absolute paths inside SCENES.
  Both the save and the load path walk EVERY `SLOT` in the tree (`forEachSlot`).
  Any future field that can hold a path needs the same treatment.
- **`juce::XmlDocument::parse(File)` is an arbitrary-file-read primitive.** It
  installs a `FileInputSource`, so a `<!DOCTYPE AMPCADE SYSTEM "…">` in a preset
  someone sends you makes the parser read a file of THEIR choosing off YOUR disk
  (`getSiblingFile` resolves `..` and absolute paths), and `<!ENTITY a "&a;">`
  recurses until the stack gives out. `Library::parsePresetXml` refuses any
  DOCTYPE/ENTITY and parses from a STRING so no input source exists at all. Use
  it for every `.ampcade` read — never `XmlDocument::parse(File)`.
- **A preset must not aim a slot at the rest of the disk.** Relative paths are
  clamped inside `appDataDir` (`getChildFile` happily resolves `../../..`), and
  on load an absolute path is kept only when it is already inside the app folder
  — that keeps a player's own pre-relativisation presets working while dropping
  anything pointing elsewhere.
- **Locally imported gear is copied in now** (`adoptGearFile`): picking a `.nam`
  off the Desktop copies it to `models/imported/` first. That is what makes the
  relative-path scheme total, and it stops a rig from breaking when the player
  moves or deletes the original.
- Audited and found already sound, so don't "fix" them: the UI's two `innerHTML`
  sinks (labels go through `esc()`, every colour through
  `/^#?([0-9a-f]{6})$/i`, knob/shape values are compared not interpolated),
  `sanitizeFileName` (strips `/` and `\`), and the trash-restore path
  (`getFileName() != trashed || ! isAChildOf(trashDir)`).
- `test/StateTest.cpp` covers all of it, and every check was confirmed to FAIL
  against the unhardened code first.

## v0.1.0 launch audit — what the hardening round still missed

The "treat every preset as hostile" pass above closed the file-read and
path-escape holes. A full pre-launch audit against the *published* artifacts
found three more ways a shared `.ampcade` could hurt the person who opened it,
and two ways the engine could silence itself. All fixed; the pattern in every
case is the same — **a guard that sits downstream of the thing it is guarding.**

- **`juce::XmlDocument` has no nesting limit and parses recursively.**
  `readNextElement` → `readChildElements` → `readNextElement`, one stack frame
  per level, no depth counter anywhere in the file. Refusing DOCTYPE/ENTITY does
  nothing about it: ~18k nested elements in a 126 KB file overflows the stack,
  and in a plugin that takes the host DAW down with the player's unsaved work.
  `Library::nestingWithinLimit` scans the text before the parser ever sees it
  (`kMaxPresetNesting = 100`; real presets nest 4). The scan is deliberately
  crude — it does not skip comments or attribute values, so it over-counts — and
  that is fine at 25× headroom. Over-counting only ever rejects.
- **`jlimit` passes NaN through, so JUCE's parameter clamping is not a filter.**
  `NormalisableRange::clampTo0To1` is a `jlimit`, and `jlimit` is
  `v < lo ? lo : (hi < v ? hi : v)` — both comparisons are false for NaN, so it
  returns NaN unchanged. Inf clamps correctly; **only NaN gets through**, which
  is why this was invisible. It is reachable from a hand-written preset because
  `CharacterFunctions::readDoubleValue` explicitly parses the spellings `"nan"`
  and `"inf"` into real non-finite doubles. `scrubNonFiniteParams` runs on the
  copy inside `applyRigTree` — the one point preset load, host state restore and
  undo all pass through — and substitutes the parameter's own default.
- **The NaN scrub cleaned the output and let the engine keep the NaN.** The only
  non-finite guard was in the output loop, downstream of the DC blocker, the
  delay feedback, the reverb tail and the IIR tone stack. So one bad sample from
  an upstream plugin — the ordinary "a plugin blew up" event everything else
  shrugs off — silenced AmpCade for the rest of the session, curable only by
  re-inserting it. There are now two scrubs *upstream*: one on the incoming mono
  block, one at the top of `goWide()` (the last point before the stereo tail,
  and downstream of every NAM slot and pedal). The output scrub stays as a
  backstop. **Scrub what a stateful element is about to eat, not what it emits.**
- **`juce::jmax` hides a NaN exactly the way `jlimit` does**, and that turned a
  broken IR into a silent one. `readSpace` measured energy with
  `maxEnergy = jmax (maxEnergy, e)`; `0.0 < NaN` is false, so a NaN energy left
  `maxEnergy` at 0, the `<= 1e-10` guard fired, and the caller took the "our
  reader can't open this" fallback — handing the raw all-NaN file straight to
  the convolution. `ir::Info::valid` now says so explicitly and both
  `loadIrIntoCab`/`loadIrIntoAir` refuse the file with a real toast.
- **A substring test is not a host check.** `download()` gated the OAuth bearer
  token on `getDomain().containsIgnoreCase ("tone3000.com")`, so a redirect to
  `tone3000.com.evil.example` was handed the player's live access token, and
  with no scheme check a plain-http hop sent it in clear text. Note also that
  `juce::URL::getDomain()` does **not** strip userinfo — it returns
  `tone3000.com@evil.example` for `https://tone3000.com@evil.example/`.
  `isTone3000Host` requires https plus an exact or `.`-anchored suffix match,
  which fails closed on all three. This was preset-reachable: a shared preset's
  blanked gear paths make the rig auto-fetch by the `toneId` its author chose.
- **`toneId = 0` is not "no tone".** Every UI branch asks `ss.toneId != null`,
  which `0` passes, so locally imported gear rendered a tone link to
  `/tones/0` (a 404) and a settings dropdown that could only fail — on the
  no-account import path a first-time user is most likely to take. `getUiState`
  now omits `toneId`/`modelId` entirely unless they are `> 0`.
- **A hidden list and a hideable list have to be the same list.** `air` was in
  the UI's slot set but in neither `HIDEABLE` nor `setHiddenSlots`'s whitelist,
  so its TO RACK button rendered, `hideBlock` optimistically stomped the toggle
  off, the processor rejected the id, and the block came back on the board with
  its IR silently out of the signal path.
- **`missing` has to count as on-board.** `onBoard()` required `loaded`, so a
  preset-referenced capture with no file was neither on the board (not loaded)
  nor in the rack (no longer hidden) the moment you pulled it out of the rack —
  it vanished, and the missing badge is the only route back to the file.

Process notes from the same round:
- **`scripts/check-nam-linked.sh` skips silently without `nm`**, which is every
  Windows runner — so the build most likely to hit the linker bug was the one
  guard-free. The Windows job now greps the binaries for the architecture names
  itself. (The shipped v0.1.0 Windows binaries were checked by hand and are
  fine.) A lenient guard that reports "skipped" in a log nobody reads is not a
  guard.
- Verify signing against the **published** asset, not `dist/`. `scripts/release.sh`
  ad-hoc signs; CI signs with the real Developer ID. The two disagree by design,
  and only one of them is what users download.

## Pathless gear, and gear shared across a preset's scenes

Reported as "my Clean preset's amp and cab suddenly became the stock tweed rig".
Nothing was lost — both bugs are about *resolving* gear a preset only names.

- **`fetchable` tested `sm.loaded`, which `slotsFromTree` has just set to false
  on every slot** ("becomes true once the file actually loads"). So the whole
  fetch-by-toneId branch in `restoreSlotsFromState` was unreachable: a slot with
  no path fell to `filePath.isEmpty() && ! fetchable` and was simply CLEARED.
  Every factory preset and every shared preset ships pathless by design, so
  loading one has always dropped its gear — the rig survived only because the
  standalone's session state stores real (relative) paths and is what restores
  at launch. Reload the preset itself and the gear went. `SlotMeta::wanted` now
  carries "the state asked for gear here", which is a different question from
  "the engine holds gear here" and the only one answerable before the file
  exists. **When a flag's comment says when it becomes true, check nobody is
  reading it earlier than that.**
- **The download cache is content-addressed and nothing was consulting it.**
  `requestT3kLoad` parks gear at `<models|irs>/tone_<toneId>/<sanitised
  name>_<modelId>.<nam|wav>` — the exact ids a pathless preset carries. But the
  decision was "no path → needs the network", so an expired token turned a rig
  whose files were already on disk into the built-in amp and cab. `cachedGearFile`
  resolves it locally first (exact name, then `*_<modelId>.<ext>` in case the
  tone was renamed upstream), which also means gear a friend's preset names
  loads instantly if you already own it, with no download at all.
- **Gear is shared across a preset's scenes, so the merge cannot read the live
  rig alone.** `recallSceneBoard` already merged stray pedals into the rack, but
  it sourced them from `meta` — i.e. only what is playing right now. Land
  straight on a scene that does not use the TS808 (a preset load lands on scene
  1, so this is the *common* case, not an edge one) and there was nothing to
  merge: the pedal was on no board and in no rack, and the preset had silently
  lost it. It now merges the union of every scene's own snapshot, live rig
  first so the copy you are playing wins when two scenes put different captures
  in the same slot. Note the test for this only bites if the pedal is absent
  from scene 1 — write it the other way round and the live-rig path covers it
  and the test passes against the unfixed code.
- `usable()` in that merge also had to stop requiring a non-empty `filePath`:
  with pathless presets every scene slot looked empty, so each scene's own gear
  was being deleted and replaced by whatever was live.

## Params (APVTS id == WebView relay name)
`in_gain` −24..+36; `out_gain` −24..+24; `gate` −101..−20 (−101 = off; the UI can drag it down to −84, below that is off); per pedal i∈1..`kNumPedals` (16): `p{i}_on`, `p{i}_drive` (±24), `p{i}_tone` 0..10 (5 = flat tilt), `p{i}_level` (±24); `amp_on`, `amp_gain`/`amp_vol` (±24), `amp_bass/mid/treble` 0..10; `cab_on`, `cab_mix` 0..1 (1 = fully wet, which is what a speaker cab wants). All linear ranges (JS normalizes scaled↔normalised without skew). Adding a param means three places: `createLayout`, `sliderNames()`/`toggleNames()` in PluginEditor.cpp (no relay = a knob that silently does nothing), and `PARAM_DEFS`/`TOGGLE_NAMES` in ui/index.html.

- **Raising `kNumPedals` is a five-place change, and four of them fail silently.** The count itself lives in `PluginProcessor.h`; `NUM_PEDALS` in ui/index.html must match it. Everything else now *derives* from those two — `slotNames()`, `defaultChainOrderNames()`, the hideable list, `sliderNames()`/`toggleNames()`, `SLOTS`/`MOVABLE_SLOTS`/`HIDEABLE`/`SLOT_META`/`PARAM_DEFS`/`TOGGLE_NAMES` — precisely because the hardcoded versions each broke differently: a missing entry in `TOGGLE_NAMES` took the whole UI down (the mock bridge has no value to `push` a listener onto, and the throw happens inside `boot`, after `renderBoard` has already cleared the board — so the symptom is an EMPTY BOARD with no console error, not a dead knob). Do not re-hardcode any of them.
- **The chain order is a seqlock over a byte array, not a packed integer.** It was one block id per nibble of a `uint64`, which is exactly 16 blocks and was exactly full — that, not CPU, is what capped the rig. Empty pedal slots cost a branch (`NamSlot::process` returns false) and are invisible on the board (`onBoard`), so the ceiling is only ever about *declared parameters*: host parameter lists are fixed at plugin load, so every slot's four params must exist up front whether or not anything is in it. Block ids are positional (`kBlockAmp = kNumPedals + 0`, …) and NEVER persisted — presets and scenes store names — so renumbering is invisible to saved rigs.
- **`setChainOrderFromNames` pads, it does not reject.** An order saved with fewer pedal slots gains the new ones right behind its last pedal, so the classic run stays contiguous and the migrated rig sounds identical (the new slots are empty). Watch the legacy branch: the pre-0.3 "four names = pedals only" test is on CONTENT (`size()==4 && !contains("amp")`), not on `size() == kNumPedals` — once `kNumPedals` passed 4 the size test started misreading current-shape orders as ancient ones.

## Layout of repo
`src/` C++ · `ui/index.html` whole UI (works in a plain browser with built-in mock when `window.__JUCE__` is absent — that's how we screenshot/dev it) · `test/SmokeTest.cpp` offline render check using NAM core's `example_models` · `test/StateTest.cpp` quit→relaunch session-state check, no UI and no models needed (`cmake --build <dir> --target AmpCadeState && <dir>/AmpCadeState_artefacts/<cfg>/AmpCadeState`) · `scripts/setup.sh` re-fetches pinned deps (third_party/ is gitignored) · `.github/workflows/build.yml` mac universal + windows.

## Gotchas
- macOS sharing: CI builds are signed with a **Developer ID** and notarised, and
  the ticket is stapled (so they open offline) — verified on the published zip
  with `codesign -dvvv`, `spctl -a -t exec` and `xcrun stapler validate`. That
  needs the signing secrets to be present; with none configured the workflow
  falls back to ad-hoc signing, which is what `scripts/release.sh` does locally
  and what followers would then need right-click-Open or `xattr -cr` for.
- Windows builds are **not** code-signed — SmartScreen shows "Windows protected
  your PC" on first run. Both the README and the shipped `READ ME FIRST.txt`
  say so and tell people what to click.
- Windows: WebView2 runtime assumed (ships with Win 11 and up-to-date Win 10),
  `JUCE_USE_WIN_WEBVIEW2_WITH_STATIC_LINKING=1`.
- Keep terminal output capped: build logs → file, tail only (a 32 GB scrollback crash once).
