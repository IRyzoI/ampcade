AmpCade — NAM guitar rig player
===============================
Standalone app + VST3 (Reaper / Ableton / FL / Cubase / Studio One).
64-bit Windows 10 or 11.

INSTALL
-------
1. Run AmpCade.exe right out of this folder — the standalone needs no
   installer.
2. For the plugin, copy the AmpCade.vst3 FOLDER (all of it, not just the
   file inside) to:
      C:\Program Files\Common Files\VST3\
   Then rescan plugins in your DAW.

"WINDOWS PROTECTED YOUR PC"
--------------------------
The Windows build is not code-signed yet, so SmartScreen warns the first
time you run it. Click "More info", then "Run anyway". (The macOS build
IS signed and notarized by Apple — the Windows certificate is simply not
in place yet.) If you would rather not take that on trust, the whole
source is at https://github.com/IRyzoI/ampcade and builds itself.

BLANK WINDOW ON LAUNCH?
-----------------------
AmpCade draws its interface with WebView2, which is preinstalled on
Windows 11 and on up-to-date Windows 10. If the window opens empty,
install the Evergreen runtime and reopen:
   https://developer.microsoft.com/microsoft-edge/webview2/

FIRST TONE IN 60 SECONDS
------------------------
1. Open AmpCade and pick your audio interface (input = your guitar,
   output = speakers or headphones) under the gear menu, top right ->
   "Audio & MIDI settings". The built-in tweed rig plays immediately --
   no downloads or accounts needed to make sound.
   The IN meter on the left of the board shows your guitar arriving; the
   OUT meter on the right shows what leaves. Each has its own trim dial
   underneath. The white line across the IN meter is the noise gate --
   drag it up to where the hiss sits, drag it to the bottom (or
   double-click) to switch it off.
2. Already have .nam captures? Click "+ ADD", pick a slot, and import
   them straight off your drive. No account needed.
3. For the TONE3000 library, click "Connect TONE3000" (top right).
   TONE3000 logs you in by emailing you a one-time code, so watch your
   inbox. A free account is all you need -- their API asks for a login on
   every request, so browsing inside AmpCade does not work until you
   connect.
4. Click the amp -> SWAP (or the + ADD button up top) and search
   anything: "plexi", "tweed", "recto", "jcm800". Click a tone card,
   pick a capture, hit Load.
5. Stomp switches turn each block on/off. Drag any card to reorder --
   pedals can go before or after the amp and cab. The built-in Delay and
   Reverb at the end run last (no download needed).

TROUBLESHOOTING
---------------
- No sound: check the audio device settings first (gear button, top
  right -> "Audio & MIDI settings"), then that the amp slot actually
  has a capture loaded. On Windows, prefer an ASIO driver if your
  interface ships one -- it is the difference between playable and
  unplayable latency.
- Weak sound: watch the IN meter while you play. If it barely moves, turn
  up your interface's input gain first, then the dial under the IN meter
  (it goes to +36 dB).
- Looking for a reverb or delay capture? Each slot only searches what it
  can actually load: pedal and amp slots list NAM captures, the Cab / IR
  slot lists IRs. So a reverb IR shows up in the Cab / IR slot even when
  its uploader filed it under "pedal".
- Buttons do nothing / search is fake: open
  %APPDATA%\AmpCade\ui.log -- the first line must say
  "ui-boot bridge=juce". If the version in the top bar says "mock",
  something is wrong with the build, not with you.
- Plugin missing in your DAW: confirm you copied the whole AmpCade.vst3
  folder into C:\Program Files\Common Files\VST3\, then rescan.
- Amp+cab "full rig" captures: leave the cab slot empty or stomp it off.

FOUND A BUG?
------------
Please report it: https://github.com/IRyzoI/ampcade/issues
Say what you were doing and paste the last few lines of
%APPDATA%\AmpCade\ui.log -- that log is usually enough to find it.

Captures come from the TONE3000 community -- each one belongs to its
creator under the license shown on its TONE3000 page. AmpCade is an
independent open-source project (AGPL-3.0), not affiliated with TONE3000.
Source, and your rights to it under the AGPL, are at
https://github.com/IRyzoI/ampcade
