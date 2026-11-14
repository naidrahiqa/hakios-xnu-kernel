---
name: hakios-flash
description: >-
  Use this skill when flashing HakOS onto the Redmi 10 (selene). Covers fastboot flashing of
  xnu-flashed-slot_a.img to boot_a, and autonomous MediaTek BROM partitioning and flashing of
  system.apfs to xnusys, while strictly protecting the host Oppo tethering device.
---

# HakOS Flashing Guide (Redmi 10 / selene)

This skill covers the safe deployment and flashing of HakOS images to the Xiaomi Redmi 10 (`selene`).

---

## 1. Safety Rules & Boundaries (CRITICAL)

> [!CAUTION]
> **DO NOT TOUCH OPPO DEVICE:**
> The host machine uses `Bus 001 Device 016: ID 22d9:2769 OPPO Electronics Corp. CPH2269` (Serial: `OR9HCQUWZ9SGGQ4P`) for host internet connectivity.
> NEVER run `fastboot` or `adb` commands without specifying the target device serial `-s 65cb03c00702`.

* **Target Device Serial:** `65cb03c00702`
* **Target Fastboot VID:PID:** `18d1:d00d`
* **Target BROM VID:PID:** `0e8d:0003`
* **Permitted Partitions to Flash:**
  * `boot_a`: Contains the XNU kernel chainboot image.
  * `xnusys`: Contains `userland/system.apfs` (2.31 GiB).
* **FORBIDDEN Partitions:** NEVER flash `boot_b`, `super`, `nvram`, `nvdata`, `persist`, `preloader`.

---

## 2. Method 1: Fastboot Flash (`boot_a` Iteration)

Use this method when iterating on the XNU kernel, `mtkloader`, or ramdisk (`root.apfs`) where `xnusys` is already intact on the eMMC.

### 1. Verify Target in Fastboot Mode
```bash
fastboot devices
```
*Expected: `65cb03c00702   fastboot`.*

### 2. Flash `boot_a`
```bash
fastboot -s 65cb03c00702 flash boot_a /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/xnu-flashed-slot_a.img
```

### 3. Reboot Device to Boot HakOS
```bash
fastboot -s 65cb03c00702 reboot
```

---

## 3. Method 2: MediaTek BROM Repartition & Full Flash

Use this method when `xnusys` needs to be partitioned or when stock Android/ColorOS was flashed and repartitioning is required.

### What `setup_ios_brom.py` does:
1. Connects to MediaTek BROM (`0e8d:0003`) and uploads DA Stage 2.
2. Checks Primary & Backup GPT:
   * If `xnusys` does not exist: shrinks `userdata` and carves `xnusys` (4.0 GiB) at LBA `235765760`..`244156351`.
   * Updates CRCs and writes both GPTs to eMMC.
3. Flashes `userland/system.apfs` (2.31 GiB) directly to `xnusys`.
4. Flashes `xnu-flashed-slot_a.img` (48.4 MB) to `boot_a`.

### Execution Steps:
1. Power OFF the Redmi 10 completely.
2. Disconnect USB cable from the phone.
3. Run the script on the host:
   ```bash
   cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene
   /home/naidra/mtkclient/.venv/bin/python tools/setup_ios_brom.py
   ```
4. While the script displays `[*] Waiting for Redmi 10 in BROM mode...`:
   * Press and hold **Volume Up (+)** and **Volume Down (-)** simultaneously.
   * Connect USB cable to the phone.
   * Hold the volume buttons for 2–3 seconds until BROM handshake initiates.
5. Once complete, unplug and power on or reboot the device.

---

## 4. Verification Check

To verify partition table integrity from Fastboot:
```bash
fastboot -s 65cb03c00702 getvar partition-size:xnusys
```
*Expected:*
`(bootloader) partition-size:xnusys: 1000f8000` (4.00 GiB)
