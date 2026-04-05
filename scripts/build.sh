#!/bin/bash
set -e
PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE_NAME="rock5b-kernel-builder"

# ── Funzione: verifica che Docker sia in esecuzione ──
check_docker() {
    if ! command -v docker &>/dev/null; then
        echo "❌ Docker non installato."
        echo "   Installa: brew install --cask docker"
        exit 1
    fi
    if ! docker info &>/dev/null 2>&1; then
        echo "❌ Docker non è in esecuzione."
        echo ""
        echo "   Su macOS, avvia Docker Desktop:"
        echo "     open -a Docker"
        echo ""
        echo "   Poi attendi ~15 secondi e riprova."
        echo "   (Puoi verificare con: docker info)"
        exit 1
    fi
}

# ── Funzione: costruisci immagine Docker ──
build_image() {
    echo "🔨 Costruzione immagine Docker '${IMAGE_NAME}'..."
    echo "   (prima volta: ~2-3 minuti, poi cachata)"
    docker buildx build --platform linux/arm64 \
        -t "$IMAGE_NAME" \
        --load \
        -f "$PROJ_ROOT/docker/Dockerfile.build" \
        "$PROJ_ROOT/docker/"
    echo "✅ Immagine Docker pronta."
}

# ── Verifica Docker ──
check_docker

# ── Sottoc omando: setup (solo costruisce l'immagine) ──
if [ "${1:-}" = "setup" ]; then
    build_image
    echo ""
    echo "Prossimo passo: clona i sorgenti kernel in src/linux"
    echo "  git clone --depth=1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git $PROJ_ROOT/src/linux"
    exit 0
fi

# ── Verifica immagine Docker esiste ──
if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "⚠️  Immagine Docker non trovata. La costruisco ora..."
    build_image
fi

# ── Verifica sorgenti kernel ──
if [ ! -d "$PROJ_ROOT/src/linux" ]; then
    echo "❌ Sorgenti kernel non trovati in src/linux/"
    echo ""
    echo "   Clona prima i sorgenti:"
    echo "   cd $PROJ_ROOT"
    echo "   git clone --depth=1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git src/linux"
    exit 1
fi

# ── Build kernel ──
TARGET="${1:-Image}"
JOBS="${2:-$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 12)}"
LOGFILE="build_$(date +%Y%m%d_%H%M).log"

# RK3588: Cortex-A76 (ARMv8.2-A) + Cortex-A55 (ARMv8.2-A)
# Enables: LSE atomics (faster locks), FP16, dot-product, crypto extensions
KCFLAGS='-march=armv8.2-a+crypto+fp16+dotprod -mtune=cortex-a76'

echo "🔨 Building kernel (target: $TARGET, jobs: $JOBS)..."
echo "   KCFLAGS: $KCFLAGS"
echo "   Log: logs/build/$LOGFILE"

docker run --rm --platform linux/arm64 \
    -v "$PROJ_ROOT/src/linux:/work/linux" \
    -v "$PROJ_ROOT/patches:/work/patches" \
    -v "$PROJ_ROOT/configs:/work/configs" \
    -v "$PROJ_ROOT/logs/build:/work/logs" \
    "$IMAGE_NAME" \
    bash -c "cd /work/linux && make ARCH=arm64 KCFLAGS='$KCFLAGS' $TARGET -j$JOBS 2>&1 | tee /work/logs/$LOGFILE"

echo "✅ Build completata."
