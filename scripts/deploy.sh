#!/bin/bash
set -e
PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROCK5B_HOST="${ROCK5B_HOST:-rock5b.local}"
ROCK5B_USER="${ROCK5B_USER:-bredos}"

echo "📦 Deploy su ${ROCK5B_USER}@${ROCK5B_HOST}..."
scp "$PROJ_ROOT/src/linux/arch/arm64/boot/Image" "${ROCK5B_USER}@${ROCK5B_HOST}:/tmp/Image"
scp "$PROJ_ROOT/src/linux/arch/arm64/boot/dts/rockchip/rk3588-rock-5b-plus.dtb" "${ROCK5B_USER}@${ROCK5B_HOST}:/tmp/"

ssh "${ROCK5B_USER}@${ROCK5B_HOST}" << 'REMOTE'
    sudo cp /tmp/Image /boot/
    sudo cp /tmp/rk3588-rock-5b-plus.dtb /boot/dtbs/rockchip/
    sudo grub-mkconfig -o /boot/grub/grub.cfg
    echo "✅ Kernel installato. Riavviare con: sudo reboot"
REMOTE
echo "✅ Deploy completato."
