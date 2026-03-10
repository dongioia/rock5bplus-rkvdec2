#!/bin/bash
set -e
PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROCK5B_HOST="${ROCK5B_HOST:-rock5b.local}"
ROCK5B_USER="${ROCK5B_USER:-sav}"
STAGING="${PROJ_ROOT}/staging"

# Detect kernel version from modules directory
KVER=$(ls "$STAGING/lib/modules/" 2>/dev/null | head -1)
if [ -z "$KVER" ]; then
    echo "ERROR: No modules found in $STAGING/lib/modules/"
    echo "Run 'make modules_install INSTALL_MOD_PATH=$STAGING' first."
    exit 1
fi

echo "Deploying kernel $KVER to ${ROCK5B_USER}@${ROCK5B_HOST}..."

# Copy kernel image and DTB
scp "$PROJ_ROOT/src/linux/arch/arm64/boot/Image" "${ROCK5B_USER}@${ROCK5B_HOST}:/tmp/Image"
scp "$PROJ_ROOT/src/linux/arch/arm64/boot/dts/rockchip/rk3588-rock-5b-plus.dtb" "${ROCK5B_USER}@${ROCK5B_HOST}:/tmp/"

# Copy modules to /tmp first, then move to /usr/lib/modules/
# IMPORTANT: Never extract to /lib/ directly — on Arch Linux, /lib is a symlink
# to usr/lib. Overwriting it with a real directory breaks the dynamic linker
# and makes the system unbootable.
rsync -a "$STAGING/lib/modules/$KVER/" "${ROCK5B_USER}@${ROCK5B_HOST}:/tmp/modules-$KVER/"

ssh "${ROCK5B_USER}@${ROCK5B_HOST}" << REMOTE
    sudo cp /tmp/Image /boot/vmlinuz-linux-custom
    sudo cp /tmp/rk3588-rock-5b-plus.dtb /boot/dtbs/rockchip/
    sudo rsync -a /tmp/modules-$KVER/ /usr/lib/modules/$KVER/
    sudo depmod $KVER
    sudo mkinitcpio -k $KVER -g /boot/initramfs-linux-custom.img
    sudo grub-mkconfig -o /boot/grub/grub.cfg
    rm -rf /tmp/modules-$KVER /tmp/Image /tmp/rk3588-rock-5b-plus.dtb
    echo "Kernel $KVER installed. Reboot with: sudo reboot"
REMOTE
echo "Deploy completed."
