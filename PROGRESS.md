# Linux-PC98 Progress, Status & Roadmap

## 1. System Overview & Hardware Architecture

### Target Hardware: NEC PC-9821 Ra43
* **CPU**: Intel Celeron (Mendocino, Family 6 Model 6, 433 MHz, no SSE/SSE2).
* **RAM**: 160 MB physical RAM (or 64 MB minimum baseline).
* **SCSI Subsystem**: MELCO IFC-DP (AMD 53C974 / `am53c974`), attached to BlueSCSI.
  * Geometry: INT 1Bh standard PC-98 SCSI ($H=8, S=32$, 256 sectors/cylinder = 128 KiB/cyl).
  * Storage devices: `sda` (2 GB SD card), `sdb` (8 GB), `sdc` (1.3 GB boot drive), `sdd` (19.5 GB `/mnt` data disk).
* **IDE / PATA Subsystem**: Onboard PC-98 built-in IDE controller (`pata_pc9800`, IRQ 9 / INT3).
  * Ports: Shared command block `0x640-0x64e` (2-byte stride), control/altstatus `0x74c`, connection `0x430`, bank select `0x432`, IRQ status `0x433`.
  * **Bank 0 (Primary Channel)**: CF card adapter / fixed disk (`ata1.00`, `sde` 3.6 GB, VFAT partitions).
  * **Bank 1 (Secondary Channel)**: Built-in ATAPI CD-ROM drive (`ata2.00`, `/dev/sr0`).
* **Audio Subsystem**: NEC PC-9801-86 Sound Board (YM2608 / OPNA) at I/O `0x188`, IRQ 12 (INT5).
* **MIDI Subsystem**: Roland MPU-PC98II C-Bus MIDI card at I/O `0xE0D0` (Data) / `0xE0D2` (Status/Command).
* **Video / Framebuffer**: Onboard Trident Cyber9660 / TGUI96xx (`pc98tridentfb`) running at 640×480×8bpp.
* **Input**: Built-in PC-98 keyboard (`pc98_8251` / `pc98kbd`), PC-98 bus mouse (`pc98busmouse`, `0x7fd9`, IRQ 13).
* **Network**: Intel PRO/100 (`e100`, IRQ 3) / LGY-98 NE2000 clone.

---

## 2. Completed Milestones & Architectural Baseline

### 2.1 Bootloader & Storage
- [x] **Bootsimple on SCSI**: Switched `debian13-i486-scsi92` from zedBSD (which lacks SCSI drivers and returned `ENXIO`) to `bootsimple` (uses BIOS `INT 0x1b`).
- [x] **SCSI Geometry (`am53c974.c`)**: Added `bios_param` returning PC-98 $H=8, S=32$ geometry, lazily copied `scsi_esp_template`.
- [x] **NEC-98 Partition Parsing (`nec98.c`)**: Added BIOS geometry fallback retry for non-standard disk formatting.
- [x] **Initramfs-less `/dev/root` Node (`do_mounts.c`)**: Injected relative `dev/root` node into devtmpfs before `pivot_root` for rootfs `/etc/fstab` mounting.
- [x] **ext4 `orphan_file` Bug Fix**: Added `-O ^orphan_file` to prevent `mke2fs -d` corruption (`orphan file too big` / `EUCLEAN`).
- [x] **Image Materialization**: Enforced `cp --sparse=never --reflink=never` across image builders to prevent BlueSCSI `Medium Error` (sense 03/11/00) on sparse holes.
- [x] **Built-in Storage Filesystems**: Enabled `FAT_FS`, `VFAT_FS`, `MSDOS_FS`, `ISO9660_FS`, `JOLIET`, `BLK_DEV_SR` in kernel.

### 2.2 Sound & Music Sequencer (`pc98snd` & `pmdmini`)
- [x] **Kernel Driver (`pc98snd.ko`)**: Raw-hardware character driver at `0x188`, IRQ 12; Timer-A/B interrupt delivery via `poll()`/`read()`; YM2608 silicon IRQ storm fix writing `(shadow & 0xc0) | 0x3f`; safety cleanup on device close (`0x27=0x30, 0x29=0x00`).
- [x] **pmdmini Sequencer Integration**: Vendored PMDWin / ymfm engine with physical hardware OPNA backend; timed purely by chip Timer-B hardware interrupts (no `rdtsc`, no busy polling).
- [x] **Mendocino Size & ISA Safety**: Compiled with static i486-musl toolchain (`zig cc`/`zig c++` `-mcpu=i486 -Os -s -flto -ffunction-sections -fdata-sections -Wl,--gc-sections -fno-exceptions -fno-rtti`) producing a 482 KB binary with zero SSE/xmm instructions.

### 2.3 Kernel Rebase to Linux 7.2
- [x] **Upstream Rebase**: Rebased all project commits on upstream `master` (Linux 7.2 default).
- [x] **Ported Kernel Patches**: Applied `nec98.c`, `am53c974.c`, `do_mounts.c`, and `mpu401.c` patches to `external/kernel/linux-7.2/`.
- [x] **Cleaned Configs**: Disabled `CONFIG_PNPBIOS`/`CONFIG_ISAPNP`, set clean `CONFIG_CMDLINE="console=tty0 earlyprintk=pc9800"`.

### 2.5 Bootsimple & Video Framebuffer Acceleration
- [x] **bootsimple I/O Chunking**: Replaced single-sector `INT 1Bh` loop with track-buffered multi-sector transfers (up to 32 sectors / 16 KB per call), reducing BIOS calls by 97% and cutting kernel boot time in half (~20s).
- [x] **Trident Hardware 2D Acceleration**: Enabled TGUI9660 on-chip 2D BitBLT engine (`fb_copyarea`) and solid fill (`fb_fillrect`) via MMIO `0x2120-0x214C`.
- [x] **Trident Fast String Burst & WC**: Implemented 32-bit `rep movsl` in 16-dword chunks with Write-Combining (`ioremap_wc` + `arch_phys_wc_add`), achieving 117 MB/s PCI bandwidth and 46.9 FPS full-screen blits.
- [x] **Differential L2 Shadow**: Added RAM shadow buffer (`hw_shadow`) to skip unchanged scanlines in CPU L2 cache, eliminating 90%+ of PCI bus traffic.
- [x] **Hardware Page Flipping**: Implemented `pc98tridentfb_pan_display` for zero-copy, 2-microsecond full-screen page flips (`CR0C`, `CR0D`, `CR1E`, `CR27`).
- [x] **Dynamic Multi-Resolution**: Added dynamic PLL frequency synthesis (`set_vclk`) and CRTC tables supporting `640x480`, `800x600`, `1024x768`, and `1280x1024` @ 8bpp.


---

## 3. Active Action Items & Pending Fixes

### 3.1 X11 & Graphics Stack
- [x] **Fix Xorg Screen/Device Identifier Mismatch** (`configs/xorg/20-pc98-coregraph.conf`):
  - In `Section "Screen"`, changed `Device "PC-98 Core-Graph"` to `Device "PC-98 framebuffer"`.
- [x] **Add `xserver-xorg-input-evdev` & `tmux`** (`configs/debian13-i486-packages.txt`):
  - Added `xserver-xorg-input-evdev` and `tmux` to package manifest.
- [x] **(Optimization) Trident Framebuffer Deferred I/O** (`pc98tridentfb.c`):
  - Optimized `pc98tridentfb_deferred_io` to flush individual dirty row spans rather than full-screen `[first, last]` bounding box; trimmed flush width to visible 640 bytes.

### 3.2 Networking, Clock & Services
- [x] **Restore Loopback (`lo`) Bringup** (`scripts/make-boot98-debian-image.sh`):
  - Removed `CONFIGURE_INTERFACES=no` from `/etc/default/networking` so `/etc/init.d/networking` runs `ifup -a` and brings up `auto lo` (`127.0.0.1`).
- [x] **Enable Non-Blocking Boot Time Sync (`pc98-timesync`)**:
  - Implemented `tools/timesync.c` (lightweight static SNTP client), staged into `/usr/sbin/pc98-timesync`, and wired `/etc/init.d/pc98-timesync` across runlevels 2–5.
- [x] **Fix Roland MPU-PC98II Auto-Probing (`pnp=0`)** (`scripts/make-boot98-debian-image.sh`):
  - Updated `/etc/modprobe.d/mpu401.conf` with `pnp=0`.

### 3.3 Kernel Configuration & Drivers
- [x] **Fix `configure-kernel.sh` for Linux 7.2 Input**:
  - Enabled `CONFIG_INPUT_EVDEV=y` and `CONFIG_MOUSE_PC98=y` on Linux 7.2.
- [x] **Enable Japanese Character Support**:
  - Enabled `CONFIG_NLS_CODEPAGE_932=y` (Shift-JIS) and `CONFIG_NLS_UTF8=y` for PC-98 FAT/CD-ROM filenames.
- [x] **Apply `pata_pc9800.c` IDE CD-ROM Patch**:
  - Added `struct mutex host_mutex` to serialize `softreset`, `prereset`, and `postreset` across Bank 0 and Bank 1.
  - Implemented `sff_irq_check` reading port `0x433` bits 0 and 1.
  - Implemented primary channel deselect before secondary bank assertion to ensure ATAPI identification.
  - Hooked `qc_issue` to ensure bank selection prior to command execution.
- [x] **SCSI Geometry Robustness (`am53c974.c` & `nec98.c`)**:
  - Added fallback in `am53c974_bios_param` to PC-98 standard $H=8, S=32$.
  - Set `am53c974_template.module = THIS_MODULE`.
  - Expanded `nec98.c` validation retry loop to test candidate geometries ($8/32$, $8/17$, $16/63$).

### 3.4 Userspace Audio UX
- [x] **Terminal Restoration on SIGINT** (`tools/pc98snd.c`):
  - In `on_fatal_signal()`, restore terminal canonical mode (`stdin_restore()`) if interrupted via Ctrl-C during interactive playback.

---

## 4. Verification & Testing Checklist

- [x] **Network & Clock**: Boot machine with ethernet cable connected -> `eth0` gets DHCP IP (`10.47.1.24x`) automatically, `lo` is UP with `127.0.0.1`, `pc98-timesync` steps date/time automatically, SSH login succeeds with `root`/`pc98`.
- [ ] **Offline Boot**: Boot machine without ethernet cable -> verify boot prompt appears immediately without network timeouts or hangs.
- [x] **X11 Desktop**: `modprobe pc98tridentfb` binds to PCI BAR0 (`0x20000000`), X server runs on `/dev/fb0`, `twm` and `xterm` display cleanly with `evdev` keyboard and bus mouse.
- [x] **CD-ROM Drive**: Verified in `dmesg`: `ata2.00: ATAPI: ASUS DRW-2014L1, 1.00` attaches as `/dev/sr0` (`sr0: Attached scsi CD-ROM sr0`).
- [x] **MIDI**: Verified `/dev/snd/midiC1D0` exists via `snd-mpu401` (`hardware=18 pnp=0`).
- [x] **Sound / PMD Player**: Verified `/usr/sbin/pc98snd detect` identifies PC-9801-86 at `0x188`, IRQ 12.
