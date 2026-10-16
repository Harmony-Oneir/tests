#!/bin/bash
set -e
T=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$T")
OUT=out/tests
GN=prebuilts/build-tools/linux-x86/bin/gn
NINJA=prebuilts/build-tools/linux-x86/bin/ninja
cd "$ROOT"

# build_configs генерирует loader; делаем это один раз (или с --reload)
if [ ! -d out/sdk/build_configs ] || [ "$1" = "--reload" ]; then
  ./build.sh --product-name oneir --no-prebuilt-sdk --skip-partlist-check \
             --load-test-config false --build-only-load
fi

mkdir -p "$OUT"
cp "$T/args.gn" "$OUT/args.gn"
ln -sfn ../sdk/build_configs "$OUT/build_configs"

$GN gen "$OUT" --root-target=//tests:all
$NINJA -w dupbuild=warn -C "$OUT" tests:all
