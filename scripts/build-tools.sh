#!/usr/bin/env bash
set -euo pipefail

# Build the PC-98 sound support: the userspace player (pc98snd, a static
# i486 musl binary built with zig) and the pc98snd.ko kernel module (built
# against the configured linux-7.2 tree).  When a root staging tree exists,
# both are installed into it so the next `./build.sh image ...` includes them.
#
#   ./build.sh tools                     build + install into the default i486 tree
#   KERNEL_BUILD=... ROOT_STAGE=... ./build.sh tools
#   STAGE=0 ./build.sh tools             build only, do not install

repo="$(cd "$(dirname "$0")/.." && pwd)"
kernel_version="${KERNEL_VERSION:-7.2}"
kernel_source="${KERNEL_SOURCE:-$repo/external/kernel/linux-$kernel_version}"
kernel_build="${KERNEL_BUILD:-$repo/build/kernel-$kernel_version-i486}"
root_stage="${ROOT_STAGE:-$repo/build/boot98/debian13-i486-root}"
stage="${STAGE:-1}"

# The module directory and depmod index must use the kernel's full release
# (e.g. 7.2.0), not the source tree name (7.2); the release is baked into
# include/config/kernel.release by the kernel build.
if [ -f "$kernel_build/include/config/kernel.release" ]; then
	kernel_release="$(cat "$kernel_build/include/config/kernel.release")"
else
	kernel_release="${KERNEL_RELEASE:-$kernel_version.0}"
fi

usage()
{
	cat <<'EOF'
Usage: ./build.sh tools

Build the pc98snd userspace player (zig, static i486 musl) and the
pc98snd.ko kernel module, then stage both into the root filesystem.

Environment:
  KERNEL_BUILD   configured out-of-tree kernel dir
                 (default: build/kernel-7.2-i486)
  ROOT_STAGE     rootfs staging tree to install into
                 (default: build/boot98/debian13-i486-root)
  STAGE=0        build only, do not install into the rootfs
EOF
}

case "${1:-}" in
	-h | --help) usage; exit 0 ;;
esac

command -v zig >/dev/null 2>&1 || {
	echo "zig is required to build pc98snd (zig cc / zig c++)." >&2
	echo "Install zig and re-run." >&2
	exit 2
}

# 1. Userspace player and demos (zig, static i486 musl).
make -C "$repo/tools"
make -C "$repo/tools/rec98"

# 2. Out-of-tree kernel module against the configured kernel tree.
if [ ! -f "$kernel_build/.config" ]; then
	echo "Kernel build tree not configured: $kernel_build" >&2
	echo "Run './build.sh kernel --cpu 486' first." >&2
	exit 2
fi
make -C "$repo/tools/pc98snd-module" \
	KSRC="$kernel_source" KOBJ="$kernel_build" ARCH=i386

# 3. Stage into the root filesystem.
if [ "$stage" != 1 ]; then
	exit 0
fi
if [ ! -d "$root_stage" ]; then
	echo "Rootfs staging tree not found, skipping install: $root_stage" >&2
	exit 0
fi

moddir="$root_stage/lib/modules/$kernel_release/extra"
sudo mkdir -p "$moddir" "$root_stage/usr/sbin"
sudo install -m 0644 \
	"$repo/tools/pc98snd-module/pc98snd.ko" "$moddir/pc98snd.ko"
sudo install -m 0755 "$repo/build/tools/pc98snd" \
	"$root_stage/usr/sbin/pc98snd"
sudo install -m 0755 "$repo/build/tools/pc98-timesync" \
	"$root_stage/usr/sbin/pc98-timesync"
sudo install -m 0755 "$repo/build/tools/pc98-demo-game" \
	"$root_stage/usr/sbin/pc98-demo-game"
sudo install -m 0755 "$repo/build/tools/rec98-demo" \
	"$root_stage/usr/sbin/rec98-demo"
sudo install -m 0755 "$repo/build/tools/th01" \
	"$root_stage/usr/sbin/th01"
sudo install -m 0755 "$repo/build/tools/th02" \
	"$root_stage/usr/sbin/th02"
sudo install -m 0755 "$repo/build/tools/th03" \
	"$root_stage/usr/sbin/th03"
sudo install -m 0755 "$repo/build/tools/th04" \
	"$root_stage/usr/sbin/th04"
sudo install -m 0755 "$repo/build/tools/th05" \
	"$root_stage/usr/sbin/th05"
sudo install -m 0755 "$repo/build/tools/pc98-touhou" \
	"$root_stage/usr/sbin/pc98-touhou"
sudo install -m 0755 "$repo/build/tools/pc98-music-room" \
	"$root_stage/usr/sbin/pc98-music-room"
sudo install -m 0755 "$repo/build/tools/pc98-boss-battle" \
	"$root_stage/usr/sbin/pc98-boss-battle"
sudo install -m 0755 "$repo/build/tools/pc98-benchmark" \
	"$root_stage/usr/sbin/pc98-benchmark"
sudo install -m 0755 "$repo/build/tools/pc98-trident-bench" \
	"$root_stage/usr/sbin/pc98-trident-bench"
sudo install -m 0755 "$repo/build/tools/test_libpc98" \
	"$root_stage/usr/sbin/test_libpc98"
sudo mkdir -p "$root_stage/usr/include"
sudo install -m 0644 "$repo/tools/libpc98.h" \
	"$root_stage/usr/include/libpc98.h"

# The /etc/modules auto-load list is written by make-boot98-debian-image.sh
# (it is image-level config, not a tools artifact).

# Re-index modules so modprobe can resolve pc98snd by name; the kernel's
# modules_install ran depmod before this out-of-tree module was staged.
sudo depmod -b "$root_stage" "$kernel_release"

echo "pc98snd: installed module + player into $root_stage"
