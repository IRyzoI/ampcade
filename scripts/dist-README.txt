AmpCade — NAM guitar rig player
===============================
Standalone app + VST3 + AU (Logic / GarageBand / Reaper / Ableton).
Universal build: works on Apple Silicon and Intel Macs, macOS 11+.

EASY INSTALL
------------
Double-click "AmpCade-Installer.pkg" (right here in this folder). One
run installs the app and both plugins system-wide. Everything is signed
and notarized by Apple — nothing gets blocked.

MANUAL INSTALL
--------------
1. Drag AmpCade.app to /Applications
2. Copy AmpCade.vst3      -> ~/Library/Audio/Plug-Ins/VST3/
   Copy AmpCade.component -> ~/Library/Audio/Plug-Ins/Components/
3. Logic/GarageBand pick up the AU after a restart of the host.

FIRST TONE IN 60 SECONDS
------------------------
1. Open AmpCade. macOS will ask for microphone/input permission --
   that's your guitar input, say yes. Pick your audio interface (input =
   your guitar, output = speakers or headphones) under the gear menu,
   top right -> "Audio & MIDI settings". The built-in tweed rig plays
   immediately -- no downloads or accounts needed to make sound.
   The IN meter on the left of the board shows your guitar arriving; the
   OUT meter on the right shows what leaves. Each has its own trim dial
   underneath. The white line across the IN meter is the noise gate --
   drag it up to where the hiss sits, drag it to the bottom (or
   double-click) to switch it off.
2. For the real fun, click "Connect TONE3000" (top right).
   TONE3000 logs you in by emailing you a one-time code, so watch your
   inbox. A free account is all you need.
3. Click the amp -> SWAP (or the + ADD button up top) and search
   anything: "plexi", "tweed", "recto", "jcm800". Click a tone card,
   pick a capture, hit Load.
4. Do the same for pedals (search "screamer", "klon", "fuzz") and the
   CAB slot if your amp capture is amp-only.
5. Stomp switches turn each block on/off. Drag any card to reorder --
   pedals can go before or after the amp and cab. The built-in Delay and
   Reverb at the end run last (no download needed).
6. Each pedal has DRIVE (how hard it is pushed), TONE (dark to bright)
   and LEVEL (volume). Pedals no longer change your volume on their own,
   so stacking a few will not bury your sound.

TROUBLESHOOTING
---------------
- No sound: check the audio device settings first (gear button, top
  right -> "Audio & MIDI settings"), then that the amp slot actually
  has a capture loaded.
- Weak sound: watch the IN meter while you play. If it barely moves, turn
  up your interface's input gain first, then the dial under the IN meter
  (it goes to +36 dB).
- Looking for a reverb or delay capture? Each slot only searches what it
  can actually load: pedal and amp slots list NAM captures, the Cab / IR
  slot lists IRs. So a reverb IR shows up in the Cab / IR slot even when
  its uploader filed it under "pedal".
- Buttons do nothing / search is fake: open ~/Library/AmpCade/ui.log --
  the first line must say "ui-boot bridge=juce". If the version in the
  top bar says "mock", something is wrong with the build, not with you.
- Plugin missing in Logic: Logic caches plugin scans; quit Logic, then
  reopen.
- Amp+cab "full rig" captures: leave the cab slot empty or stomp it off.

FOUND A BUG?
------------
Please report it: https://github.com/IRyzoI/ampcade/issues
Say what you were doing and paste the last few lines of
~/Library/AmpCade/ui.log -- that log is usually enough to find it.

Captures come from the TONE3000 community -- each one belongs to its
creator under the license shown on its TONE3000 page. AmpCade is an
independent open-source project (AGPL-3.0), not affiliated with TONE3000.
Source, and your rights to it under the AGPL, are at
https://github.com/IRyzoI/ampcade
