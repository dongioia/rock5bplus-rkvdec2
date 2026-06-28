#!/bin/bash
# deploy-kernel-tagged.sh — version-isolated kernel deploy for Rock 5B+.
#
# Installs a built kernel into its OWN slot without overwriting any existing
# kernel: dedicated /boot/vmlinuz-linux-<tag>, per-version /usr/lib/modules/<kver>/,
# dedicated /boot/initramfs-linux-<tag>.img, then a one-shot GRUB boot into it so
# the previous default stays the fallback if the new kernel fails to boot.
#
# Consumes the tarball produced by `docker/run.sh build-kernel <config> <tag>`
# (contains ./boot/vmlinuz-linux-<tag> and ./lib/modules/<kver>/).
#
# Usage:
#   scripts/deploy-kernel-tagged.sh <tag> [tarball] [board-host]
#   TAG=beryllium scripts/deploy-kernel-tagged.sh beryllium deploy/kernel-latest.tar.gz rock5b
set -eu  # no pipefail: `tar|head`/`awk|grep -q` close pipes early → SIGPIPE would abort under pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TAG="${1:?usage: deploy-kernel-tagged.sh <tag> [tarball] [host]}"
TARBALL="${2:-$PROJ_ROOT/deploy/kernel-latest.tar.gz}"
HOST="${3:-${ROCK5B_HOST:-rock5b}}"
DEV_CONTAINER="${DEV_CONTAINER:-dev-server}"
DTB_REL="arch/arm64/boot/dts/rockchip/rk3588-rock-5b-plus.dtb"

[ -f "$TARBALL" ] || { echo "ERROR: tarball not found: $TARBALL"; exit 1; }

# Detect the module dir (kernel version string) inside the tarball.
KVER="$(tar tzf "$TARBALL" | sed -n 's#^\./lib/modules/\([^/]*\)/.*#\1#p' | head -1)"
[ -n "$KVER" ] || { echo "ERROR: no ./lib/modules/<kver>/ in tarball"; exit 1; }
IMG_MEMBER="./boot/vmlinuz-linux-${TAG}"
tar tzf "$TARBALL" | grep -qx "$IMG_MEMBER" || {
    echo "ERROR: tarball has no $IMG_MEMBER (wrong KIMG_TAG?). Members:";
    tar tzf "$TARBALL" | grep '^\./boot/' ; exit 1; }

echo "Deploying kernel: tag=$TAG  kver=$KVER  ->  $HOST (fallback preserved)"

# --- stage on board ---
scp "$TARBALL" "${HOST}:/tmp/k-${TAG}.tar.gz"

# DTB from the build volume (build tarball does not carry it)
if docker exec "$DEV_CONTAINER" test -f "/k/$DTB_REL" 2>/dev/null; then
    docker exec "$DEV_CONTAINER" cat "/k/$DTB_REL" | ssh "$HOST" "cat > /tmp/rk3588-rock-5b-plus-${TAG}.dtb"
    HAVE_DTB=1
else
    echo "WARN: DTB not found in $DEV_CONTAINER:/k/$DTB_REL — skipping DTB update (kernel will use existing DTB)"
    HAVE_DTB=0
fi

# --- install into isolated slot on the board ---
ssh "$HOST" "TAG='$TAG' KVER='$KVER' HAVE_DTB='$HAVE_DTB' bash -s" <<'REMOTE'
set -eu  # no pipefail: `tar|head`/`awk|grep -q` close pipes early → SIGPIPE would abort under pipefail
echo " - extracting image + modules (no overwrite of other kernels)"
# Extract ONLY this tag's image and this kver's modules; never touch vmlinuz-linux-custom etc.
sudo tar xzf "/tmp/k-${TAG}.tar.gz" -C / "./boot/vmlinuz-linux-${TAG}"
sudo rm -rf "/usr/lib/modules/${KVER}"          # clean re-install of THIS version only
sudo mkdir -p "/usr/lib/modules/${KVER}"
sudo tar xzf "/tmp/k-${TAG}.tar.gz" -C / "./lib/modules/${KVER}"

if [ "$HAVE_DTB" = "1" ]; then
    sudo install -D "/tmp/rk3588-rock-5b-plus-${TAG}.dtb" /boot/dtbs/rockchip/rk3588-rock-5b-plus.dtb
fi

echo " - depmod $KVER"
sudo depmod "$KVER"
echo " - initramfs -> /boot/initramfs-linux-${TAG}.img"
sudo mkinitcpio -k "$KVER" -g "/boot/initramfs-linux-${TAG}.img"
echo " - regenerating grub.cfg"
sudo grub-mkconfig -o /boot/grub/grub.cfg

# One-shot boot into the new kernel; default (fallback) is unchanged until validated.
ENTRY="$(awk -F\" '/menuentry /{print $2}' /boot/grub/grub.cfg | grep -m1 -- "-${TAG}" || true)"
if [ -n "$ENTRY" ]; then
    sudo grub-reboot "$ENTRY" || sudo grub-reboot "$(grep -c menuentry /boot/grub/grub.cfg)" || true
    echo " - grub-reboot set ONE-SHOT into: $ENTRY"
else
    echo " - WARN: could not auto-find '-$TAG' menuentry; set boot manually"
fi
rm -f "/tmp/k-${TAG}.tar.gz" "/tmp/rk3588-rock-5b-plus-${TAG}.dtb" 2>/dev/null || true
echo "Installed $KVER as tag '$TAG'. Reboot to test (one-shot); power-cycle returns to fallback if it fails."
REMOTE

echo "Done. After validating the new kernel, make it permanent with:"
echo "  ssh $HOST \"sudo sed -i 's/^GRUB_DEFAULT=.*/GRUB_DEFAULT=<entry>/' /etc/default/grub && sudo grub-mkconfig -o /boot/grub/grub.cfg\""
