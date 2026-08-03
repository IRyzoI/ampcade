#!/usr/bin/env bash
# Fetch pinned third-party dependencies. Run once after cloning.
set -euo pipefail
cd "$(dirname "$0")/.."

JUCE_TAG="8.0.15"
NAM_SHA="3cde95c"

mkdir -p third_party

if [ ! -d third_party/JUCE ]; then
  git clone -q --depth 1 --branch "$JUCE_TAG" https://github.com/juce-framework/JUCE third_party/JUCE
  echo "JUCE $JUCE_TAG ✓"
fi

if [ ! -d third_party/NeuralAmpModelerCore ]; then
  git clone -q --recurse-submodules --shallow-submodules \
    https://github.com/sdatkinson/NeuralAmpModelerCore third_party/NeuralAmpModelerCore
  git -C third_party/NeuralAmpModelerCore checkout -q "$NAM_SHA" || true
  git -C third_party/NeuralAmpModelerCore submodule update --init --recursive --depth 1
  echo "NeuralAmpModelerCore $NAM_SHA ✓"
fi

echo "Dependencies ready."
