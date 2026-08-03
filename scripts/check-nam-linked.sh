#!/usr/bin/env bash
# Guards the bug that shipped in v0.1–v0.2.1: NAM architectures self-register
# from static initializers in their own translation units, those TUs reach the
# plugin through AmpCade_SharedCode.a, and a linker drops archive members that
# nothing references — so every plugin build silently contained no architectures
# at all and any capture failed with "No config parser registered".
#
# src/NamSlot.cpp keeps them alive on purpose. This check makes sure that keeps
# working. Runs POST_BUILD on each plugin format.
#
# Deliberately lenient: if the toolchain can't be inspected we warn and pass,
# so a missing nm never breaks someone's build. Only a readable binary that is
# genuinely missing the registrars is a failure.
set -u
BIN="${1:-}"

[ -n "$BIN" ] && [ -f "$BIN" ] || { echo "check-nam-linked: no binary at '$BIN' — skipped"; exit 0; }
command -v nm > /dev/null 2>&1 || { echo "check-nam-linked: nm unavailable — skipped"; exit 0; }

SYMS=$(nm "$BIN" 2>/dev/null) || { echo "check-nam-linked: nm failed on $BIN — skipped"; exit 0; }
[ -n "$SYMS" ] || { echo "check-nam-linked: no symbols read from $BIN — skipped"; exit 0; }

# Grep for the EXTERNAL create_config symbols (one per architecture TU), not
# the static registrar objects: those are zero-size locals in anonymous
# namespaces and Xcode 15.4 folds them into unnamed init code, which made this
# check fail on a perfectly good binary. The create_config functions have
# external linkage, live in the same TUs, and cannot lose their names.
missing=""
check_frag() { printf '%s\n' "$SYMS" | grep -q "$2" || missing="$missing $1"; }
check_frag WaveNet "3nam7wavenet13create_config"
check_frag LSTM    "3nam4lstm13create_config"
check_frag ConvNet "3nam7convnet13create_config"
check_frag Linear  "3nam6linear13create_config"

if [ -n "$missing" ]; then
  echo "" >&2
  echo "ERROR: $(basename "$BIN") is missing NAM architectures:$missing" >&2
  echo "       The linker stripped their self-registering translation units." >&2
  echo "       Captures using them would fail at load with" >&2
  echo "       'No config parser registered for architecture'." >&2
  echo "       See keepNamArchitecturesLinked() in src/NamSlot.cpp." >&2
  echo "" >&2
  exit 1
fi

echo "check-nam-linked: $(basename "$BIN") OK (WaveNet LSTM ConvNet Linear)"
