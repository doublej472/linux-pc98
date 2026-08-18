# PC-9821 IDE / CD-ROM Subsystem Documentation

## 1. Hardware Overview (PC-9821 Ra43)

The built-in IDE/PATA interface on PC-9821 systems (such as the PC-9821 Ra43) multiplexes two ATA channels ("banks") onto a single set of legacy I/O ports.

### I/O Port Map
| Port | Direction | Description |
|---|---|---|
| `0x0430` | Read/Write | Connection / configuration register (`0x40` indicates slave capable; bits 0/6 report device presence). |
| `0x0432` | Read/Write | **Bank select register**. Writing `0x00` selects Bank 0 (Primary channel); writing `0x01` selects Bank 1 (Secondary channel). Bit 7 is a no-op guard. Bit 3 enables 32-bit DWORD I/O. Bit 6 reports slave capable. |
| `0x0433` | Read | **IRQ status register**. Bit 0 = Primary channel IRQ asserted; Bit 1 = Secondary channel IRQ asserted. |
| `0x0640` - `0x064e` | Read/Write | ATA taskfile command block with a 2-byte stride (e.g. `0x640`=data, `0x642`=error/features, `0x644`=nsect, `0x646`=lbal, `0x648`=lbam, `0x64a`=lbah, `0x64c`=device/head, `0x64e`=status/command). |
| `0x074c` | Read/Write | Device control register (write) / Alternate status register (read). |
| `0x074e` | Read | Drive address register. |
| **IRQ 9** | Interrupt | Shared IDE interrupt line (INT3) for both channels. |

### Channel / Device Assignment
* **Bank 0 (Primary Channel)**: Internal IDE Hard Disk or CompactFlash card adapter (`ata1.00` Master, `ata1.01` Slave).
* **Bank 1 (Secondary Channel)**: Built-in ATAPI CD-ROM drive (`ata2.00` Master, `ata2.01` Slave).

---

## 2. Hardware Validation on Real Ra43 (`10.47.5.5`)

Direct hardware port probing was performed on the real machine after replacing the CD-ROM drive:

### Diagnostics Performed
1. **Device Presence Check (`ata_devchk`)**:
   * Bank 0 Dev 0: `nsect = 0x55`, `lbal = 0xaa` (CF card detected).
   * Bank 1 Dev 0: `nsect = 0x55`, `lbal = 0xaa` (**CD-ROM drive detected**).
2. **Taskfile Signature Read**:
   * Selected Bank 1 (`0x432 = 0x01`) and Device 0 (`0x64c = 0xa0`).
   * Read Cylinder registers: **`lbam (0x648) = 0x14`**, **`lbah (0x64a) = 0xeb`**.
   * Signature `0xEB14` is the standard ATA/ATAPI specification signature for ATAPI packet devices (CD-ROM).
   * Drive status returned **`0x58`** (`DRDY | DSC | DRQ`), confirming the unit is powered, ready, and receptive to ATAPI PACKET commands.

**Conclusion**: The replacement CD-ROM drive is 100% physically functional, correctly jumpered, and responsive on Bank 1 Master.

---

## 3. Root Cause Analysis of Driver Failure in `pata_pc9800.c`

The upstream / current `pata_pc9800.c` driver failed to attach the CD-ROM drive due to the following architectural mismatches with modern `libata`:

### 1. Asynchronous Port Probing without Bank Locking
* In modern Linux kernels, `libata` runs device discovery and reset asynchronously on separate kernel worker threads for `ata1` (Bank 0) and `ata2` (Bank 1).
* Because both ports share the exact same hardware I/O addresses (`0x640-0x64e`) switched solely by `0x432`, concurrent probing causes `ata1` and `ata2` to interleave register writes.
* When `ata2` resets and queries its signature, `ata1` frequently sets `0x432` back to `0x00`, resulting in `ata2` reading `0x00 0x00` (empty) from Bank 0 instead of `0x14 0xeb` from Bank 1.

### 2. Missing Host-Wide Mutex on Reset / Discovery
* `ata_sff_softreset`, `prereset`, and `postreset` must be guarded by a host-level mutex (`hpriv->host_mutex`) so that Bank 0 and Bank 1 perform their reset cycles sequentially.

### 3. Missing IRQ Disambiguation (`sff_irq_check`)
* Because both channels share IRQ 9, `ata_sff_interrupt` needs an `sff_irq_check` hook reading port `0x433` (bit 0 for Bank 0, bit 1 for Bank 1) to determine which port triggered the interrupt.

### 4. `sff_drain_fifo` NULL Pointer Dereference
* In `ata_sff_error_handler`, `sff_drain_fifo` is called with `qc = NULL` to clear lingering DRQ on error. A custom wrapper that dereferences `qc->ap` without checking for NULL triggers a kernel panic. (Using standard `ata_sff_drain_fifo` directly avoids this, as `sff_check_status` already selects the bank).

---

## 4. Fix Roadmap for `pata_pc9800.c`

When ready to apply the kernel driver patch, the following changes should be integrated into both `external/kernel/linux-7.2/drivers/ata/pata_pc9800.c` and `linux-7.1`:

1. **Add `struct mutex host_mutex`** in `struct pc98_pata_host` and initialize it in `pc98_pata_probe`.
2. **Wrap `reset.prereset`, `reset.softreset`, and `reset.postreset`** with `mutex_lock(&hpriv->host_mutex)` / `mutex_unlock(&hpriv->host_mutex)` and explicit `pc98_pata_select_bank(ap)`.
3. **Implement `sff_irq_check`**:
   ```c
   static bool pc98_pata_sff_irq_check(struct ata_port *ap)
   {
       u8 irq_stat = inb(0x0433);
       return (irq_stat & (1 << (ap->port_no & 1))) != 0;
   }
   ```
4. **Hook `qc_issue`**:
   ```c
   static unsigned int pc98_pata_qc_issue(struct ata_queued_cmd *qc)
   {
       pc98_pata_select_bank(qc->ap);
       return ata_sff_qc_issue(qc);
   }
   ```
5. **Set `.cable_detect = ata_cable_40wire`** in `pc98_pata_ops`.
6. Ensure built-in `ISO9660_FS`, `JOLIET`, and `BLK_DEV_SR` remain enabled in kernel configs so `/dev/sr0` auto-attaches as a standard CD-ROM block device.
