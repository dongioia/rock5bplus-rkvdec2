#!/bin/bash
# benchmark.sh — Rock 5B+ benchmark suite
# Esegue tutti i test e salva i risultati per confronto tra kernel diversi.
#
# Uso:
#   ./scripts/benchmark.sh                    # esegue localmente sulla Rock 5B+
#   ./scripts/benchmark.sh --remote           # esegue via SSH (da Mac)
#   ./scripts/benchmark.sh --remote --host rock5b
#   ./scripts/benchmark.sh --compare file1.txt file2.txt

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG_DIR="$PROJECT_ROOT/logs/benchmark"
SSH_HOST="rock5b"
REMOTE=false
COMPARE=false

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  --remote          Run benchmark via SSH (from Mac dev machine)"
    echo "  --host HOST       SSH host (default: rock5b)"
    echo "  --compare A B     Compare two benchmark result files"
    echo "  -h, --help        Show this help"
    exit 0
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --remote) REMOTE=true; shift ;;
        --host) SSH_HOST="$2"; shift 2 ;;
        --compare) COMPARE=true; FILE_A="$2"; FILE_B="$3"; shift 3 ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# =============================================
# Compare mode
# =============================================
if $COMPARE; then
    if [[ ! -f "$FILE_A" || ! -f "$FILE_B" ]]; then
        echo "Error: both files must exist"
        exit 1
    fi

    LABEL_A=$(grep "^KERNEL=" "$FILE_A" | cut -d= -f2-)
    LABEL_B=$(grep "^KERNEL=" "$FILE_B" | cut -d= -f2-)

    echo "============================================="
    echo " BENCHMARK COMPARISON"
    echo "============================================="
    echo ""
    printf "%-30s  %-22s  %-22s  %s\n" "Test" "$LABEL_A" "$LABEL_B" "Delta"
    echo "------------------------------  ----------------------  ----------------------  ----------"

    while IFS='=' read -r key val_a; do
        [[ "$key" =~ ^(KERNEL|DATE|BUILD|GOVERNOR|FREQ_) ]] && continue
        [[ -z "$key" || "$key" =~ ^# || -z "$val_a" ]] && continue
        val_b=$(grep "^${key}=" "$FILE_B" 2>/dev/null | cut -d= -f2-)
        [[ -z "$val_b" ]] && val_b="n/a"

        num_a=$(echo "$val_a" | grep -oE '[0-9]+\.?[0-9]*' | head -1)
        num_b=$(echo "$val_b" | grep -oE '[0-9]+\.?[0-9]*' | head -1)

        delta=""
        if [[ -n "$num_a" && -n "$num_b" && "$num_a" != "0" ]]; then
            delta=$(awk "BEGIN { d = ($num_b - $num_a) / $num_a * 100; printf \"%+.1f%%\", d }")
        fi

        printf "%-30s  %-22s  %-22s  %s\n" "$key" "$val_a" "$val_b" "$delta"
    done < "$FILE_A"

    echo ""
    echo "+delta = B faster/higher | -delta = A faster/higher"
    exit 0
fi

# =============================================
# Benchmark payload (runs on target machine)
# =============================================
BENCH_PAYLOAD='
#!/bin/bash
set -uo pipefail

info() { echo "  $*" >&2; }

KERNEL=$(uname -r)
BUILD=$(uname -v)
GOV=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_governor 2>/dev/null || echo "unknown")
FA55=$(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq 2>/dev/null || echo "0")
FA76=$(cat /sys/devices/system/cpu/cpufreq/policy4/scaling_cur_freq 2>/dev/null || echo "0")

echo "KERNEL=$KERNEL"
echo "DATE=$(date +%Y-%m-%d\ %H:%M:%S)"
echo "BUILD=$BUILD"
echo "GOVERNOR=$GOV"
echo "FREQ_A55=${FA55}kHz"
echo "FREQ_A76=${FA76}kHz"

# 1. CPU multi-thread
info "[1/7] CPU multi-thread (openssl, 8 threads)..."
ssl_mt=$(openssl speed -elapsed -multi 8 aes-256-cbc sha256 2>&1)
echo "CPU_SHA256_MT=$(echo "$ssl_mt" | grep "^sha256" | awk "{print \$NF}")"
echo "CPU_AES256_MT=$(echo "$ssl_mt" | grep "^aes-256-cbc" | awk "{print \$NF}")"

# 2. CPU single-thread
info "[2/7] CPU single-thread (openssl, 1 thread)..."
ssl_st=$(openssl speed -elapsed aes-256-cbc sha256 2>&1)
echo "CPU_SHA256_ST=$(echo "$ssl_st" | grep "^sha256" | awk "{print \$NF}")"
echo "CPU_AES256_ST=$(echo "$ssl_st" | grep "^aes-256-cbc" | awk "{print \$NF}")"

# 3. GPU
info "[3/7] GPU (glmark2-es2 offscreen 1080p)..."
if command -v glmark2-es2-wayland &>/dev/null; then
    gscore=$(glmark2-es2-wayland --off-screen -s 1920x1080 2>&1 | grep "glmark2 Score" | grep -oE "Score: [0-9]+" | grep -oE "[0-9]+")
    echo "GPU_GLMARK2=${gscore:-n/a}"
else
    echo "GPU_GLMARK2=n/a"
    info "  glmark2-es2-wayland not found"
fi

# 4. Memory bandwidth
info "[4/7] Memory bandwidth..."
mem=$(dd if=/dev/zero of=/dev/null bs=1M count=4096 2>&1 | grep -oE "[0-9.]+ [GM]B/s" | tail -1)
echo "MEM_BANDWIDTH=$mem"

# 5. Storage write
info "[5/7] Storage write (512MB)..."
sw=$(dd if=/dev/zero of=/tmp/_bench_w bs=1M count=512 conv=fdatasync 2>&1 | grep -oE "[0-9.]+ [GM]B/s" | tail -1)
rm -f /tmp/_bench_w
echo "STORAGE_WRITE=$sw"

# 6. Storage read
info "[6/7] Storage read (256MB)..."
dd if=/dev/zero of=/tmp/_bench_r bs=1M count=256 conv=fdatasync 2>/dev/null
sync
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1 || true
sr=$(dd if=/tmp/_bench_r of=/dev/null bs=1M 2>&1 | grep -oE "[0-9.]+ [GM]B/s" | tail -1)
rm -f /tmp/_bench_r
echo "STORAGE_READ=$sr"

# 7. Thermal
info "[7/7] Thermal..."
for z in /sys/class/thermal/thermal_zone*/; do
    tn=$(cat "$z/type" 2>/dev/null)
    tv=$(cat "$z/temp" 2>/dev/null)
    [[ -n "$tn" && -n "$tv" ]] && echo "THERMAL_${tn}=$(awk "BEGIN{printf \"%.1f\",$tv/1000}")C"
done

# VPU/modules
vpuc=$(v4l2-ctl --list-devices 2>/dev/null | grep -c "platform:" || echo "0")
echo "VPU_DEVICES=$vpuc"
echo "MOD_RKVDEC=$(lsmod 2>/dev/null | awk "/^rockchip_vdec /{found=1} END{print found+0}")"
echo "MOD_HANTRO=$(lsmod 2>/dev/null | awk "/^hantro_vpu /{found=1} END{print found+0}")"
echo "MOD_PANTHOR=$(lsmod 2>/dev/null | awk "/^panthor /{found=1} END{print found+0}")"

info ""
info "Done."
'

# =============================================
# Run
# =============================================
mkdir -p "$LOG_DIR"

echo "============================================="
echo " Rock 5B+ Benchmark Suite"
echo "============================================="

if $REMOTE; then
    echo "Target: $SSH_HOST (via SSH)"
    echo ""
    # Send payload via stdin to remote bash
    OUTPUT=$(echo "$BENCH_PAYLOAD" | ssh "$SSH_HOST" "bash -s" 2>&1)
else
    echo "Target: localhost"
    echo ""
    OUTPUT=$(echo "$BENCH_PAYLOAD" | bash -s 2>&1)
fi

# Separate results (key=value lines) from progress messages
RESULTS=$(echo "$OUTPUT" | grep -E '^[A-Z_]+=' || true)
PROGRESS=$(echo "$OUTPUT" | grep -vE '^[A-Z_]+=' || true)

# Show progress
if [[ -n "$PROGRESS" ]]; then
    echo "$PROGRESS"
fi

if [[ -z "$RESULTS" ]]; then
    echo "Error: no benchmark results collected"
    exit 1
fi

# Save results
KERN=$(echo "$RESULTS" | grep "^KERNEL=" | cut -d= -f2- | tr '/' '_' | tr ' ' '_')
OUTFILE="$LOG_DIR/bench-${KERN}-$(date +%Y%m%d-%H%M%S).txt"

echo "$RESULTS" > "$OUTFILE"

echo ""
echo "============================================="
echo " Results saved: $OUTFILE"
echo "============================================="
echo ""
echo "$RESULTS" | grep -E "^(KERNEL|CPU_|GPU_|MEM_|STORAGE_|VPU_|MOD_)"
echo ""
echo "Compare with: $0 --compare $OUTFILE <other_result.txt>"
