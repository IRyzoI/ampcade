#!/usr/bin/env bash
# Local universal (arm64+x86_64) release build -> dist/AmpCade-macOS.zip
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -B build-release -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64" -DAMPCADE_COPY_PLUGIN=OFF > /dev/null
cmake --build build-release --target AmpCade_Standalone AmpCade_VST3 AmpCade_AU --parallel 3 2>&1 | tail -3

# Ad-hoc sign so the bundles at least have valid signatures (still unsigned
# for Gatekeeper — followers use right-click Open or xattr -cr).
for b in build-release/AmpCade_artefacts/Release/Standalone/AmpCade.app \
         build-release/AmpCade_artefacts/Release/VST3/AmpCade.vst3 \
         build-release/AmpCade_artefacts/Release/AU/AmpCade.component; do
  codesign --force --deep -s - "$b" 2> /dev/null || true
done

rm -rf dist && mkdir -p dist/AmpCade
cp -R build-release/AmpCade_artefacts/Release/Standalone/AmpCade.app dist/AmpCade/
cp -R build-release/AmpCade_artefacts/Release/VST3/AmpCade.vst3 dist/AmpCade/
cp -R build-release/AmpCade_artefacts/Release/AU/AmpCade.component dist/AmpCade/
cp scripts/dist-README.txt dist/AmpCade/"READ ME FIRST.txt"
(cd dist && zip -qry AmpCade-macOS.zip AmpCade)
echo "dist/AmpCade-macOS.zip ready:"
du -sh dist/AmpCade-macOS.zip
