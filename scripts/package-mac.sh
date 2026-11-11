#!/usr/bin/env bash
# Packages a macOS Release build (see .github/workflows/macos.yml) into one zip with every plugin:
#   <out>/PureSheen-Complete-<version>-macOS.zip
#     VST3/*.vst3, Components/*.component (Audio Units), CLAP/*.clap, PureSheen Dynamics.app,
#     Claude connector (optional)/PureSheenBridge, How to install.txt, LICENSE.txt, THIRD-PARTY-NOTICES.txt
# Checks that every plugin is universal (arm64 + x86_64) and fails if one is missing.
#
#   scripts/package-mac.sh [buildDir=build-mac] [outDir=dist]
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
build="${1:-build-mac}"
out="${2:-dist}"
case "$build" in /*) ;; *) build="$root/$build" ;; esac
case "$out" in /*) ;; *) out="$root/$out" ;; esac

version="$(sed -n 's/^project(PureSheen VERSION \([0-9.]*\).*/\1/p' "$root/CMakeLists.txt")"
[ -n "$version" ] || { echo "FAILED: no version in CMakeLists.txt"; exit 1; }

stage="$out/PureSheen-Complete-$version-macOS"
rm -rf "$stage"
mkdir -p "$stage/VST3" "$stage/Components" "$stage/CLAP" "$stage/Claude connector (optional)"

universal() {
    local archs
    archs="$(lipo -archs "$1")"
    case "$archs" in
        *arm64*x86_64*|*x86_64*arm64*) ;;
        *) echo "FAILED: $1 is not universal ($archs)"; exit 1 ;;
    esac
}

count=0
while IFS=, read -r id name code rest; do
    id="${id%$'\r'}"
    [ -n "$id" ] || continue
    if [ "$id" = "suite" ]; then target="PureSheen"; else target="PureSheen_$id"; fi
    art="$build/${target}_artefacts/Release"

    ditto "$art/VST3/$name.vst3" "$stage/VST3/$name.vst3"
    ditto "$art/AU/$name.component" "$stage/Components/$name.component"
    ditto "$art/CLAP/$name.clap" "$stage/CLAP/$name.clap"
    universal "$art/VST3/$name.vst3/Contents/MacOS/$name"
    universal "$art/AU/$name.component/Contents/MacOS/$name"
    count=$((count + 1))
done < <(tail -n +2 "$root/installer/products.csv")

suiteArt="$build/PureSheen_artefacts/Release"
ditto "$suiteArt/Standalone/PureSheen Dynamics.app" "$stage/PureSheen Dynamics.app"
bridge="$build/PureSheenBridge_artefacts/Release/PureSheenBridge"
universal "$bridge"
cp "$bridge" "$stage/Claude connector (optional)/PureSheenBridge"

cp "$root/installer/macOS-How-to-install.txt" "$stage/How to install.txt"
cp "$root/LICENSE" "$stage/LICENSE.txt"
cp "$root/installer/THIRD-PARTY-NOTICES.txt" "$stage/THIRD-PARTY-NOTICES.txt"

zip="$out/PureSheen-Complete-$version-macOS.zip"
rm -f "$zip"
ditto -c -k --keepParent "$stage" "$zip"
echo "Packaged $count plugins: $zip"
