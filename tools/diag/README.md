# Diagnostic tools

Standalone, manually built probes that are not part of the `pc98snd` build
(`tools/Makefile` only builds the player; `build-tools.sh` only builds the
player and the kernel module).

- `mpuprobe.c`  — scan the candidate PC-98 C-bus I/O addresses for a Roland
  MPU-401 (MPU-PC98) MIDI card by resetting it (0xFF) and checking for the
  0xFE ACK.  This is how the card on the Ra43 was located at 0xE0D0.
- `mpuprobe2.c` — dump the raw register map around 0xE0D0 and try the RESET
  and enter-UART-mode commands at several register offsets to determine
  whether the status/command port sits at base+1 or base+2.

Both use `/dev/port` (requires `CONFIG_DEVPORT=y`), not the `pc98snd`
device, and run as root:

    zig cc -target x86-linux-musl -mcpu=i486 -O2 -static -o mpuprobe mpuprobe.c
    ./mpuprobe
