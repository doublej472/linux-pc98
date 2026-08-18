#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
stage="${ROOT_STAGE:-$repo/build/debian-i386-root}"
suite="${DEBIAN_SUITE:-trixie}"
mirror="${DEBIAN_MIRROR:-https://deb.debian.org/debian}"
include="${DEBIAN_INCLUDE:-sysvinit-core,e2fsprogs,kmod,udev,ifupdown,iproute2,dhcpcd-base,ca-certificates,dropbear,iputils-ping,procps,tmux,vim-tiny,dialog,chrony,rsync}"
root_password="${ROOT_PASSWORD:-pc98}"
slim_rootfs="${SLIM_ROOTFS:-1}"
console_mode="${CONSOLE_MODE:-video}"
debootstrap="${DEBOOTSTRAP:-$(command -v debootstrap 2>/dev/null || true)}"
if [ -z "$debootstrap" ] && [ -x /usr/sbin/debootstrap ]; then
	debootstrap=/usr/sbin/debootstrap
fi

if [ -z "$debootstrap" ] || [ ! -x "$debootstrap" ]; then
	echo "debootstrap is required." >&2
	echo "On Debian, install it with: sudo apt-get install debootstrap" >&2
	exit 1
fi

if [ -e "$stage" ]; then
	echo "rootfs staging directory already exists: $stage" >&2
	echo "Set ROOT_STAGE to a new path or remove the old tree explicitly." >&2
	exit 1
fi

mkdir -p "$(dirname "$stage")"
sudo "$debootstrap" \
	--arch=i386 \
	--variant=minbase \
	--include="$include" \
	"$suite" "$stage" "$mirror"

printf 'pc98\n' | sudo tee "$stage/etc/hostname" >/dev/null
printf '%s\n' \
	'/dev/root / ext4 defaults 0 0' \
	'proc /proc proc defaults 0 0' \
	'sysfs /sys sysfs defaults 0 0' \
	| sudo tee "$stage/etc/fstab" >/dev/null

# Keep Debian's package-provided tty1 login on the PC-98 GDC console. A
# serial getty is opt-in and is used only for private diagnostic images.
case "$console_mode" in
video)
	;;
dual)
	printf '%s\n' \
		'' \
		'T0:23:respawn:/sbin/agetty -L ttyPC0 9600 vt100' \
		| sudo tee -a "$stage/etc/inittab" >/dev/null
	;;
*)
	echo "Unsupported CONSOLE_MODE: $console_mode" >&2
	exit 1
	;;
esac

printf 'auto lo\niface lo inet loopback\n\nallow-hotplug eth0\niface eth0 inet dhcp\n' \
	| sudo tee "$stage/etc/network/interfaces" >/dev/null
printf 'root:%s\n' "$root_password" | sudo chroot "$stage" chpasswd

sudo chroot "$stage" /bin/sh -c '
	command -v ip >/dev/null ||
		{ echo "iproute2 installation did not provide ip" >&2; exit 1; }
	command -v dhcpcd >/dev/null ||
		{ echo "dhcpcd-base installation did not provide dhcpcd" >&2; exit 1; }
	command -v dropbear >/dev/null ||
		{ echo "dropbear installation did not provide dropbear" >&2; exit 1; }
	command -v ping >/dev/null ||
		{ echo "iputils-ping installation did not provide ping" >&2; exit 1; }
	command -v ps >/dev/null ||
		{ echo "procps installation did not provide ps" >&2; exit 1; }
	command -v tmux >/dev/null ||
		{ echo "tmux installation did not provide tmux" >&2; exit 1; }
	command -v vi >/dev/null ||
		{ echo "vi installation did not provide vi" >&2; exit 1; }
	command -v dialog >/dev/null ||
		{ echo "dialog installation did not provide dialog" >&2; exit 1; }
	command -v chronyd >/dev/null ||
		{ echo "chrony installation did not provide chronyd" >&2; exit 1; }
	command -v rsync >/dev/null ||
		{ echo "rsync installation did not provide rsync" >&2; exit 1; }
'

# dhcpcd-base ships only the daemon; provide the sysvinit service so the
# onboard NIC is configured before dropbear starts in runlevel 2.
sudo tee "$stage/etc/init.d/dhcpcd" >/dev/null <<'EOF'
#!/bin/sh
### BEGIN INIT INFO
# Provides:          dhcpcd
# Required-Start:    $network $local_fs $remote_fs
# Required-Stop:     $network $local_fs $remote_fs
# Should-Start:      udev
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: DHCP IPv4/IPv6 client
### END INIT INFO

DAEMON=/usr/sbin/dhcpcd
NAME=dhcpcd
DESC="DHCP client"

case "$1" in
  start)
	echo "Starting $DESC: $NAME"
	for dev in /sys/class/net/*; do
		name="$(basename "$dev")"
		if [ "$name" != "lo" ] && [ -d "$dev" ]; then
			ip link set "$name" up 2>/dev/null || true
		fi
	done
	start-stop-daemon --start --quiet --oknodo --background \
		--exec $DAEMON -- -q -b
	;;
  stop)
	echo "Stopping $DESC: $NAME"
	start-stop-daemon --stop --quiet --oknodo --exec $DAEMON
	;;
  restart|force-reload)
	$0 stop
	$0 start
	;;
  *)
	echo "Usage: $0 {start|stop|restart|force-reload}" >&2
	exit 1
	;;
esac

exit 0
EOF
sudo chmod 0755 "$stage/etc/init.d/dhcpcd"
sudo chroot "$stage" /bin/sh -c 'PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin /usr/sbin/update-rc.d dhcpcd defaults'
sudo mkdir -p "$stage/etc/ssh/sshd_config.d"
printf '%s\n' 'PermitRootLogin yes' | \
	sudo tee "$stage/etc/ssh/sshd_config.d/permit-root-login.conf" >/dev/null


if [ "$slim_rootfs" = 1 ]; then
	# Keep dpkg/apt and package copyright files usable, while omitting data
	# that can be downloaded again.  This lets the PC-98 live system fit in
	# a 500 MiB ext4 partition without turning it into an initramfs.
	sudo chroot "$stage" apt-get clean
	sudo find "$stage/var/lib/apt/lists" -mindepth 1 -delete
	sudo find "$stage/usr/share/man" -mindepth 1 -delete
	sudo find "$stage/usr/share/info" -mindepth 1 -delete
	sudo find "$stage/usr/share/locale" -mindepth 1 -delete
fi

echo "Debian $suite i386 rootfs staging tree: $stage"
