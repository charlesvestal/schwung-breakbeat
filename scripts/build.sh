#!/usr/bin/env bash
# Build breakbeat module for Move (aarch64).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$REPO_ROOT"

IMAGE_NAME="schwung-breakbeat-builder"
MODULE_ID="breakbeat"
DIST_DIR="dist/${MODULE_ID}"
TARBALL="dist/${MODULE_ID}-module.tar.gz"

# ── Version management (host only, not inside Docker) ────────────────────────
if [ ! -f "/.dockerenv" ]; then

    # Auto-increment patch version on every dev build.
    # For a release, set RELEASE=1 to skip the increment and enforce git-tag match.
    if [ "${RELEASE:-0}" != "1" ]; then
        NEW_VERSION=$(python3 - <<'PYEOF'
import json, re

with open('release.json') as f:
    rel = json.load(f)

parts = rel['version'].split('.')
parts[-1] = str(int(parts[-1]) + 1)
new_ver = '.'.join(parts)

rel['version'] = new_ver
rel['download_url'] = re.sub(r'v[\d.]+/', f'v{new_ver}/', rel['download_url'])
with open('release.json', 'w') as f:
    json.dump(rel, f, indent=2)
    f.write('\n')

with open('src/module.json') as f:
    mod = json.load(f)
mod['version'] = new_ver
with open('src/module.json', 'w') as f:
    json.dump(mod, f, indent=2)
    f.write('\n')

print(new_ver)
PYEOF
)
        echo "Dev build: version bumped to $NEW_VERSION"

    else
        # Release mode: version must exactly match the latest git tag.
        MODULE_VERSION=$(python3 -c "import json; print(json.load(open('src/module.json'))['version'])")
        GIT_TAG=$(git describe --tags --abbrev=0 2>/dev/null | sed 's/^v//')
        if [ -n "$GIT_TAG" ] && [ "$MODULE_VERSION" != "$GIT_TAG" ]; then
            echo "ERROR: module.json version ($MODULE_VERSION) does not match latest git tag ($GIT_TAG)."
            echo "       Bump the version in src/module.json and release.json before building a release."
            exit 1
        fi
        echo "Release build: v$MODULE_VERSION"
    fi

fi
# ─────────────────────────────────────────────────────────────────────────────

if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
    echo "=== breakbeat build (via Docker) ==="
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        echo "Building Docker image..."
        docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
    fi
    docker run --rm \
        -v "$REPO_ROOT:/build" \
        -u "$(id -u):$(id -g)" \
        -e "DIST_DIR=$DIST_DIR" \
        -w /build \
        "$IMAGE_NAME" \
        ./scripts/build.sh
    exit 0
fi

CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
CC="${CC:-${CROSS_PREFIX}gcc}"

rm -rf dist build
mkdir -p "$DIST_DIR" build

echo "Compiling DSP..."
"$CC" -Ofast -shared -fPIC \
    -march=armv8-a -mtune=cortex-a72 \
    -fomit-frame-pointer -fno-stack-protector \
    -DNDEBUG \
    -Isrc/dsp \
    src/dsp/breakbeat.c \
    src/dsp/slice_select.c \
    src/dsp/perf.c \
    -o build/dsp.so \
    -lm

echo "Packaging..."
cat build/dsp.so > "$DIST_DIR/dsp.so"
chmod 0755 "$DIST_DIR/dsp.so"

# Copy module.json and inject version into the abbrev field for the device display.
# Source keeps abbrev="BB"; the device sees e.g. "BBv3.4" (minor.patch only).
_VER=$(grep '"version"' src/module.json | head -1 | sed 's/.*"\([0-9.]*\)".*/\1/')
_MINOR=$(echo "$_VER" | cut -d. -f2)
_PATCH=$(echo "$_VER" | cut -d. -f3)
_ABBREV="BBv${_MINOR}.${_PATCH}"
# Inject version into both abbrev (slot label) and name (module menu header)
sed -e "s/\"abbrev\": \"BB\"/\"abbrev\": \"${_ABBREV}\"/" \
    -e "s/\"name\": \"Breakbeat\"/\"name\": \"Breakbeat v${_MINOR}.${_PATCH}\"/" \
    src/module.json > "$DIST_DIR/module.json"
echo "  name/abbrev set to: Breakbeat v${_MINOR}.${_PATCH} / ${_ABBREV}"

cat src/ui.js > "$DIST_DIR/ui.js"

# Bundle samples
mkdir -p "$DIST_DIR/samples"
for f in samples/*.wav; do
    cat "$f" > "$DIST_DIR/samples/$(basename "$f")"
done

# Bundle presets
mkdir -p "$DIST_DIR/presets"
for f in src/presets/*.json; do
    cat "$f" > "$DIST_DIR/presets/$(basename "$f")"
done

(cd dist && tar -czf "${MODULE_ID}-module.tar.gz" "${MODULE_ID}/")

echo "Built: $TARBALL"
ls -lh "$TARBALL"
