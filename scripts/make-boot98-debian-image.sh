#!/usr/bin/env bash
set -euo pipefail

PATH="/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

repo="$(cd "$(dirname "$0")/.." && pwd)"
bootloader_dir="$repo/external/zedBSD/build/pc98"
rootfs="${1:?usage: $0 ROOTFS OUTPUT [VMLINUX]}"
output="${2:?usage: $0 ROOTFS OUTPUT [VMLINUX]}"
kernel="${3:-$repo/build/kernel-7.2-i486/vmlinux.boot}"

heads="${DISK_HEADS:-8}"
sectors="${DISK_SECTORS:-17}"
swap_mb="${SWAP_MB:-128}"
root_password="${ROOT_PASSWORD:-pc98}"
bootloader="${BOOTLOADER:-bootsimple}"
bootsimple_profile="${BOOTSIMPLE_PROFILE:-}"
bootsimple_cmdline="${BOOTSIMPLE_CMDLINE:-}"

case "$bootloader" in
	bootsimple)
		test -n "$bootsimple_profile" || {
			echo "BOOTSIMPLE_PROFILE is required" >&2; exit 2; }
		test -n "$bootsimple_cmdline" || {
			echo "BOOTSIMPLE_CMDLINE is required" >&2; exit 2; }
		;;
	zedbsd) . "$repo/scripts/zedbsd-env.sh" ;;
	*) echo "Unsupported Debian bootloader: $bootloader" >&2; exit 2 ;;
esac

test -d "$rootfs" || {
	echo "Root filesystem not found: $rootfs" >&2
	exit 1
}
test -f "$kernel" || {
	echo "Kernel not found: $kernel" >&2
	echo "Build it with: ./build.sh kernel --cpu 486" >&2
	exit 1
}
if test -e "$output"; then
	echo "Refusing to overwrite existing image: $output" >&2
	exit 1
fi

for command in mformat mcopy mkfs.ext4 mkswap mountpoint python3 sudo; do
	command -v "$command" >/dev/null || {
		echo "$command is required" >&2
		exit 1
	}
done

case "$heads:$sectors:$swap_mb" in
	*[!0-9:]* | 0:* | *:0:* | *:*:0)
		echo "Heads, sectors, and swap size must be positive integers" >&2
		exit 1
		;;
esac

# BOOT98 uses BIOS logical CHS for the PC-98 partition table.  Cylinder zero
# is the NEC system area and the FAT16 BOOT volume begins at cylinder one.
# The FAT16 PBR occupies one 1024-byte reserved sector and IO.SYS is the first
# ordinary FAT file, so DOS and the partition IPL use the same start CHS.
# Keep the root byte capacity, use a 128 MiB FAT16 BOOT partition, and round
# each one up to whole cylinders for the selected geometry.  Fixed ending
# cylinder numbers would make an H=8/S=32 SCSI image about 1.88 times larger
# than the IDE image even though both contain the same files.  Only the
# SCSI-92 profile (H=8/S=32) gets a larger 1 GiB root; every other profile
# keeps the default capacity.
cylinder_sectors=$((heads * sectors))
cylinder_bytes=$((cylinder_sectors * 512))
baseline_cylinder_bytes=$((8 * 17 * 512))
boot_target_bytes=$((128 * 1024 * 1024))
if [ "$heads:$sectors" = "8:32" ]; then
	root_target_bytes=$((1024 * 1024 * 1024))
else
	root_target_bytes=$((13371 * baseline_cylinder_bytes))
fi

boot_start_cylinder=1
boot_cylinders=$(((boot_target_bytes + cylinder_bytes - 1) / cylinder_bytes))
boot_last_cylinder=$((boot_start_cylinder + boot_cylinders - 1))
root_start_cylinder=$((boot_last_cylinder + 1))
root_cylinders=$(((root_target_bytes + cylinder_bytes - 1) / cylinder_bytes))
root_last_cylinder=$((root_start_cylinder + root_cylinders - 1))
swap_start_cylinder=$((root_last_cylinder + 1))
swap_cylinders=$(((swap_mb * 1024 * 1024 + cylinder_bytes - 1) /
	cylinder_bytes))
swap_last_cylinder=$((swap_start_cylinder + swap_cylinders - 1))
total_cylinders=$((swap_last_cylinder + 1))

root_start=$((root_start_cylinder * cylinder_sectors))
boot_sectors=$((boot_cylinders * cylinder_sectors))
root_sectors=$((root_cylinders * cylinder_sectors))
swap_start=$((swap_start_cylinder * cylinder_sectors))
swap_sectors=$((swap_cylinders * cylinder_sectors))
total_bytes=$((total_cylinders * cylinder_bytes))
root_bytes=$((root_sectors * 512))
swap_bytes=$((swap_sectors * 512))

work="$(mktemp -d "${TMPDIR:-/tmp}/boot98-debian.XXXXXX")"
root_image="$work/root.ext4"
swap_image="$work/swap.img"
mount_dir="$work/root"
cfg="$work/BOOT.CFG"
kernel_extra_args="${KERNEL_EXTRA_ARGS:-}"
mounted=0

cleanup()
{
	if test "$mounted" -eq 1 && mountpoint -q "$mount_dir"; then
		sudo umount "$mount_dir"
	fi
	rm -f -- "$root_image" "$swap_image" "$cfg"
	rmdir "$mount_dir" "$work" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if test "$bootloader" = zedbsd; then
	"$repo/external/zedBSD/build.sh" all pc98
fi
mkdir -p "$(dirname "$output")" "$mount_dir"
truncate -s "$total_bytes" "$output"
if test "$bootloader" = zedbsd; then
	dd if="$bootloader_dir/ipl-lba0.bin" of="$output" bs=512 count=1 \
		conv=notrunc status=none
	dd if="$bootloader_dir/ipl-lba2.bin" of="$output" bs=512 seek=2 count=14 \
		conv=notrunc status=none
fi
python3 - "$output" "$boot_last_cylinder" "$root_start_cylinder" \
	"$root_last_cylinder" "$swap_start_cylinder" \
	"$swap_last_cylinder" "$heads" "$sectors" <<'PY'
import struct
import sys

image = sys.argv[1]
boot_last = int(sys.argv[2])
root_start = int(sys.argv[3])
root_last = int(sys.argv[4])
swap_start = int(sys.argv[5])
swap_last = int(sys.argv[6])
last_head = int(sys.argv[7]) - 1
last_sector = int(sys.argv[8]) - 1


def chs(cylinder, head=0, sector=0):
    return bytes((sector, head)) + struct.pack("<H", cylinder)


def entry(flags, kind, first_ipl, first_data, last, name):
    value = bytearray(32)
    value[0] = flags
    value[1] = kind
    value[4:8] = chs(first_ipl)
    value[8:12] = chs(first_data)
    value[12:16] = chs(last, last_head, last_sector)
    value[16:32] = name.encode("ascii").ljust(16, b" ")
    return value


table = bytearray(512)
table[0:32] = entry(0xA1, 0x91, 1, 1, boot_last, "BOOT")
table[32:64] = entry(0x21, 0x83, root_start, root_start, root_last,
                     "DEBIAN13")
table[64:96] = entry(0x21, 0x82, swap_start, swap_start, swap_last,
                     "LINUXSWAP")
with open(image, "r+b") as stream:
    stream.seek(512)
    stream.write(table)
PY

if test "$bootloader" = zedbsd; then
	printf '%s\n' \
		'echo Booting Debian 13 i486...' \
		'kernel VMLINUX' \
		"arg root=PARTLABEL=DEBIAN13 rootfstype=ext4 rw rootwait pnpbios=off${kernel_extra_args:+ $kernel_extra_args}" \
		'boot' >"$cfg"
fi

truncate -s "$root_bytes" "$root_image"
mkfs.ext4 -q -F -L DEBIAN13 "$root_image"
sudo mount -o loop "$root_image" "$mount_dir"
mounted=1
sudo cp -a "$rootfs"/. "$mount_dir"/

# The normal Debian getty starts /bin/login and its PAM stack.  On i486 PC-98
# systems that path can take long enough to time out, and at 64 MiB it may be
# killed by memory pressure before a prompt appears.  Retain getty only for
# terminal ownership/setup, then enter a root shell directly through a tiny
# wrapper which deliberately ignores any arguments supplied by getty.
sudo install -d -m 0755 "$mount_dir/usr/local/sbin"
sudo tee "$mount_dir/usr/local/sbin/pc98-direct-shell" >/dev/null <<'EOF'
#!/bin/sh
HOME=/root
USER=root
LOGNAME=root
SHELL=/bin/bash
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME USER LOGNAME SHELL PATH
cd /root || cd /
exec /bin/bash -l
EOF
sudo chmod 0755 "$mount_dir/usr/local/sbin/pc98-direct-shell"
sudo sed -i '/^[1-6]:[0-9]*:respawn:\/sbin\/getty /d' \
	"$mount_dir/etc/inittab"
printf '%s\n' \
	'1:2345:respawn:/sbin/getty -8 -L --noclear --skip-login --login-program /usr/local/sbin/pc98-direct-shell 38400 tty1 linux' | \
	sudo tee -a "$mount_dir/etc/inittab" >/dev/null

# The kernel mounts devtmpfs.  Keep udev installed for later manual use, but
# avoid its full early-device trigger on this memory-constrained live image.
sudo rm -f "$mount_dir/etc/rcS.d/S02udev"
printf '%s\n' \
	'/dev/root / ext4 defaults,noatime 0 0' \
	'LABEL=PC98SWAP none swap sw 0 0' \
	'proc /proc proc defaults 0 0' \
	'sysfs /sys sysfs defaults 0 0' | \
	sudo tee "$mount_dir/etc/fstab" >/dev/null

# sysvinit auto-loads the modules listed in /etc/modules (read by kmod's
# init script).  pc98snd drives the PC-9801-86 sound card; pc98busmouse is
# the built-in bus mouse (0x7fd9, IRQ 13); snd-mpu401 is the Roland
# MPU-PC98II MIDI card.  A missing module is skipped by modprobe with only
# a warning, so an unstaged entry cannot break boot.
printf '%s\n' \
	'# pc98tridentfb framebuffer (for X11) is loaded manually before startx:' \
	'# its shadow/verify path is CPU-heavy on the single-core Celeron.' \
	'pc98busmouse' \
	'pc98snd' \
	'snd-mpu401' | \
	sudo tee "$mount_dir/etc/modules" >/dev/null

# Xorg configuration for PC-98 framebuffer / evdev input
sudo install -D -m 0644 "$repo/configs/xorg/20-pc98-coregraph.conf" \
	"$mount_dir/etc/X11/xorg.conf.d/20-pc98-coregraph.conf"

# SSH configuration: allow root login with password
sudo mkdir -p "$mount_dir/etc/ssh/sshd_config.d"
printf '%s\n' 'PermitRootLogin yes' | \
	sudo tee "$mount_dir/etc/ssh/sshd_config.d/permit-root-login.conf" >/dev/null

# Roland MPU-PC98II MIDI card (data 0xE0D0, status/command 0xE0D2).
# TODO(irq): reassign the card's DIP switch to INT1/IRQ5 and change
# irq=-1 to irq=5.  Polling (irq=-1) is used for now because the factory
# IRQ 6 is held by the am53c974 SCSI card.
# Set pnp=0 so the driver does not skip manual C-Bus probing on CONFIG_PNP kernels.
sudo mkdir -p "$mount_dir/etc/modprobe.d"
printf '%s\n' \
	'options snd-mpu401 port=0xe0d0 irq=-1 hardware=18 pnp=0' | \
	sudo tee "$mount_dir/etc/modprobe.d/mpu401.conf" >/dev/null
printf '%s\n' 'debian-pc98' | \
	sudo tee "$mount_dir/etc/hostname" >/dev/null
printf '%s\n' \
	'127.0.0.1 localhost' \
	'127.0.1.1 debian-pc98' | \
	sudo tee "$mount_dir/etc/hosts" >/dev/null
printf '%s\n' \
	'auto lo' \
	'iface lo inet loopback' \
	'' \
	'allow-hotplug eth0' \
	'iface eth0 inet dhcp' | \
	sudo tee "$mount_dir/etc/network/interfaces" >/dev/null

# DHCP client for any present NIC (ifupdown above only manages lo).  Do not
# assume a network card or a reachable DHCP server exists: dhcpcd is started
# in the background and simply idles until an interface appears, so a
# machine with no NIC (or no internet access) still boots promptly and never
# blocks on the network.
sudo tee "$mount_dir/etc/init.d/dhcpcd" >/dev/null <<'EOF'
#!/bin/sh
### BEGIN INIT INFO
# Provides:          dhcpcd
# Required-Start:    $network $local_fs $remote_fs
# Required-Stop:     $network $local_fs $remote_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: DHCP client for all interfaces
### END INIT INFO

DAEMON=/usr/sbin/dhcpcd

test -x "$DAEMON" || exit 0

case "$1" in
  start)
	# Administratively bring up ethernet interfaces so PHYs power on and detect carrier
	for dev in /sys/class/net/*; do
		name="$(basename "$dev")"
		if [ "$name" != "lo" ] && [ -d "$dev" ]; then
			ip link set "$name" up 2>/dev/null || true
		fi
	done
	start-stop-daemon --start --quiet --oknodo --background \
		--exec "$DAEMON" -- -q -b
	;;
  stop)
	start-stop-daemon --stop --quiet --oknodo --exec "$DAEMON"
	;;
  restart|force-reload)
	"$0" stop
	"$0" start
	;;
  status)
	if pgrep -x dhcpcd >/dev/null 2>&1 || pgrep -f "dhcpcd: \[manager\]" >/dev/null 2>&1; then
		echo "dhcpcd is running"
		exit 0
	else
		echo "dhcpcd is not running"
		exit 3
	fi
	;;
  *)
	echo "Usage: $0 {start|stop|restart|force-reload|status}" >&2
	exit 1
	;;
esac

exit 0
EOF
sudo chmod 0755 "$mount_dir/etc/init.d/dhcpcd"
sudo chroot "$mount_dir" /bin/sh -c 'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin /usr/sbin/update-rc.d dhcpcd defaults'

# Time synchronization service: runs in the background and syncs the clock
# as soon as networking becomes available, without blocking offline boot.
sudo tee "$mount_dir/etc/init.d/pc98-timesync" >/dev/null <<'EOF'
#!/bin/sh
### BEGIN INIT INFO
# Provides:          pc98-timesync
# Required-Start:    $network $local_fs $remote_fs
# Required-Stop:     $network $local_fs $remote_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Non-blocking NTP time synchronization
### END INIT INFO

DAEMON=/usr/sbin/pc98-timesync

test -x "$DAEMON" || exit 0

case "$1" in
  start)
	start-stop-daemon --start --quiet --oknodo --background \
		--exec "$DAEMON" -- --daemon
	;;
  stop)
	start-stop-daemon --stop --quiet --oknodo --exec "$DAEMON"
	;;
  restart|force-reload)
	"$0" stop
	"$0" start
	;;
  status)
	if pgrep -x pc98-timesync >/dev/null 2>&1; then
		echo "pc98-timesync is running"
		exit 0
	else
		echo "pc98-timesync is not running"
		exit 3
	fi
	;;
  *)
	echo "Usage: $0 {start|stop|restart|force-reload|status}" >&2
	exit 1
	;;
esac

exit 0
EOF
sudo chmod 0755 "$mount_dir/etc/init.d/pc98-timesync"
sudo chroot "$mount_dir" /bin/sh -c 'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin /usr/sbin/update-rc.d pc98-timesync defaults'
printf '%s\n' \
	'deb [trusted=yes arch=i386] https://noctvm.io/debian-i486/packages trixie main pc98' \
	'deb [trusted=yes arch=i386] http://deb.debian.org/debian trixie main' | \
	sudo tee "$mount_dir/etc/apt/sources.list" >/dev/null
printf '%s\n' \
	'Debian GNU/Linux 13 (trixie) i486 PC-98 BOOT98 image' \
	"Login: root  Password: $root_password" | \
	sudo tee "$mount_dir/etc/motd" >/dev/null
printf 'root:%s\n' "$root_password" | sudo chroot "$mount_dir" /usr/sbin/chpasswd
sudo chroot "$mount_dir" /usr/sbin/usermod -s /bin/bash root 2>/dev/null || true

# Install kernel image and kexec fast-reboot tool
sudo mkdir -p "$mount_dir/boot"
sudo install -m 0644 "$kernel" "$mount_dir/boot/vmlinux.boot"
sudo ln -sf vmlinux.boot "$mount_dir/boot/vmlinux"

sudo tee "$mount_dir/usr/sbin/pc98-kexec" >/dev/null <<'EOF'
#!/bin/sh
set -e
KERNEL="${1:-/boot/vmlinux.boot}"
CMDLINE="${2:-$(cat /proc/cmdline)}"

if [ ! -f "$KERNEL" ]; then
	echo "Kernel image not found: $KERNEL" >&2
	exit 1
fi

echo "Loading kernel for kexec: $KERNEL"
echo "Kernel command line: $CMDLINE"
kexec -l "$KERNEL" --command-line="$CMDLINE"

echo "Executing kexec fast reboot..."
kexec -e
EOF
sudo chmod 0755 "$mount_dir/usr/sbin/pc98-kexec"

sudo chroot "$mount_dir" apt-get clean
sudo sync
sudo umount "$mount_dir"
mounted=0

dd if="$root_image" of="$output" bs=512 seek="$root_start" \
	conv=notrunc,sparse,fdatasync status=progress
truncate -s "$swap_bytes" "$swap_image"
mkswap --quiet --label PC98SWAP "$swap_image"
dd if="$swap_image" of="$output" bs=512 seek="$swap_start" \
	conv=notrunc,sparse,fdatasync status=progress

case "$bootloader" in
	bootsimple)
		"$repo/bootsimple/install-image.sh" \
			--profile "$bootsimple_profile" --partition 1 \
			--heads "$heads" --sectors "$sectors" \
			--cmdline "$bootsimple_cmdline" \
			"$output" "$kernel"
		;;
	zedbsd)
		DISK_HEADS="$heads" DISK_SECTORS="$sectors" \
			"$repo/external/zedBSD/scripts/install-image.sh" \
			--install-disk-stubs "$output" "$kernel" "$cfg"
		printf 'Installing graphical menu and Remacs overlay...\n'
		DISK_HEADS="$heads" DISK_SECTORS="$sectors" \
			"$repo/bootloader/install-fs.sh" --partition 1 "$output"
		;;
esac

# The BlueSCSI reports a SCSI Medium Error when it reads a sparse hole
# in the SD-card image (the dd steps use conv=sparse and truncate leaves
# holes), so materialize the file: copy without reflink and without sparse
# output, writing zeros to every hole.
materialized="$output.full"
cp --sparse=never --reflink=never "$output" "$materialized"
mv "$materialized" "$output"

sha256sum "$output"
printf 'BOOT98 Debian 13 i486 image: %s\n' "$output"
printf 'Geometry: C=%d H=%d S=%d, root=PARTLABEL=DEBIAN13, swap=LABEL=PC98SWAP (%d MiB)\n' \
	"$total_cylinders" "$heads" "$sectors" "$swap_mb"
printf 'Partition bytes: BOOT=%d, root=%d, swap=%d\n' \
	"$((boot_sectors * 512))" "$root_bytes" "$swap_bytes"
printf 'Login: root / %s\n' "$root_password"
printf 'Bootloader: %s\n' "$bootloader"
