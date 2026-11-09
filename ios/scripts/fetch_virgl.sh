#!/bin/zsh
# GPU for Linux guests: virglrenderer, libepoxy, ANGLE (EGL, GLESv2) and the Vulkan loader as frameworks, taken
# unmodified from UTM's official v5.0.8 release (github.com/utmapp/UTM; built by its scripts/build_dependencies.sh
# from utmapp/virglrenderer b6c4d36e and utmapp/libepoxy bf985874; MIT and BSD licensed). Checked against the
# SHA-256 below, then copied to ios/deps/virgl/{ios,macos} (not in git). The app embeds and re-signs the iOS ones
# (project.yml, "Embed GPU frameworks"); for hengevm on a Mac, link build/Frameworks to deps/virgl/macos and set
# HV_VIRGL to it.
set -e
DEPS="$(cd "$(dirname "$0")/.." && pwd)/deps/virgl"
TAG=v5.0.8
IPA_SHA=4875a1ec869278fb173b37467388ea627bfa6d95f1d64501bc41bb86dd7d53b2
DMG_SHA=7b2d7b2e2a02b3de1757ef19a8f36573e6d9cceae8c8cca0abcbcaa4a852019a
FRAMEWORKS=(virglrenderer.1 epoxy.0 EGL GLESv2 vulkan.1)
WORK=$(mktemp -d)
trap 'hdiutil detach -quiet "$WORK/dmg" 2>/dev/null; rm -rf "$WORK"' EXIT

fetch() { # name sha
    curl -fL --retry 3 -o "$WORK/$1" "https://github.com/utmapp/UTM/releases/download/$TAG/$1"
    echo "$2  $WORK/$1" | shasum -a 256 -c -
}

fetch UTM.ipa $IPA_SHA
unzip -q "$WORK/UTM.ipa" -d "$WORK/ipa"
rm -rf "$DEPS/ios" && mkdir -p "$DEPS/ios"
for f in $FRAMEWORKS; do ditto "$WORK/ipa/Payload/UTM.app/Frameworks/$f.framework" "$DEPS/ios/$f.framework"; done

fetch UTM.dmg $DMG_SHA
hdiutil attach -quiet -nobrowse -readonly -mountpoint "$WORK/dmg" "$WORK/UTM.dmg"
rm -rf "$DEPS/macos" && mkdir -p "$DEPS/macos"
for f in $FRAMEWORKS; do ditto "$WORK/dmg/UTM.app/Contents/Frameworks/$f.framework" "$DEPS/macos/$f.framework"; done

echo "GPU frameworks in $DEPS"
