# AGENTS.md — HakiOS on Redmi 10 (selene)

Project instructions, layout, and strict operational guidelines for AI coding agents and human engineers working on **HakiOS for Redmi 10 (`selene`)**.
Read this document completely before editing files or executing commands.

---

## 1. Project Overview

* **Target Device:** Xiaomi Redmi 10 (`selene`), MediaTek Helio G88 (MT6769H / MT6769V/CZ, ARMv8.2-A, 8 cores: 2x Cortex-A75 @ 2.0 GHz + 6x Cortex-A55 @ 1.8 GHz), Mali-G52 MC2 GPU, 6 GiB LPDDR4X RAM, 128 GiB eMMC 5.1.
* **Goal:** Boot official Apple iOS 16.5 (build 20F66, iPhone X / A11 `t8015`) userland with SpringBoard (Home Screen) running on Apple's open-source XNU kernel (`xnu-8796.121.2`), brought up by MediaTek Little Kernel (LK) and `mtkloader`.
* **Current Status:**
  * XNU kernel successfully boots on bare metal via `mtkloader` chainboot in `boot_a`.
  * Display console working with hardware memory blit scrolling and Xiaomi boot logo plane eradication.
  * Ramdisk root APFS mounts cleanly; `/private/var` mounts as `ramfs`.
  * `launchd.ios` (Apple iOS 16.5 PID 1) handoff verified with patched `argv[0] == "/sbin/launchd"`.
  * Touch unlock gesture scheduled via `rc.jobs.unlock` writing to `/dev/redwood-touch`.

---

## 2. Target Device & Host Safety Rules (NON-NEGOTIABLE)

> [!CAUTION]
> **STRICT HARDWARE ISOLATION:**
> The host machine is connected to multiple USB devices. Violating these rules will cause immediate host network cutoff or hard-bricking.

1. **NEVER TOUCH THE OPPO DEVICE:**
   * Device ID: `Bus 001 Device 016: ID 22d9:2769 OPPO Electronics Corp. CPH2269` (Serial: `OR9HCQUWZ9SGGQ4P`).
   * **Purpose:** This device provides the host computer's USB internet tethering / hotspot.
   * **Rule:** Never invoke `adb`, `fastboot`, or reset commands against this device or without specifying the target device serial.
2. **TARGET DEVICE ONLY:**
   * Redmi 10 Serial: `65cb03c00702`
   * Fastboot USB ID: `18d1:d00d`
   * Android Debug USB ID: `18d1:4ee7`
   * MTK BROM USB ID: `0e8d:0003` (MediaTek Inc.)
   * Always pass `-s 65cb03c00702` to `fastboot` and `adb`.
3. **PARTITION WRITE BOUNDARIES:**
   * Permitted partitions to write: **`boot_a`** and **`xnusys`** ONLY.
   * **NEVER WRITE:** `boot_b`, `super`, `nvram`, `nvdata`, `nvcfg`, `persist`, `persistbak`, `proinfo`, `seccfg`, `preloader`.
   * Preserving slot B ensures the phone can always recover into Android/ColorOS if needed.

---

## 3. Architecture & Boot Chain

```
[ Power On / Reset ]
         │
         ▼
[ MediaTek Boot ROM (BROM) ]  ── (or DA Stage 2 during flashing)
         │
         ▼
[ Preloader ]
         │
         ▼
[ Little Kernel (LK / LK2) ] (Android Bootloader)
         │
         ▼  (Executes boot_a payload)
[ mtkloader.bin ] (Bare-metal loader, runs before MMU)
   ├─ Inits DRAM / memory maps
   ├─ Disables OVL0 Layers 1..3 (kills Xiaomi boot logo)
   ├─ Sets up Framebuffer Console (FB @ 0x7c400000, 1080x2400)
   ├─ Populates Apple DeviceTree & boot-args
   └─ Jumps to XNU kernel entry point
         │
         ▼
[ XNU Kernel (xnu-8796.121.2) ]
   ├─ Initializes Darwin subsystems
   ├─ Mounts root APFS from RAM disk (rd=md0)
   └─ Starts PID 1: /sbin/init
         │
         ▼
[ /sbin/init ] (Custom HakOS PID 1 Bootstrap)
   ├─ Mounts devfs on /dev
   ├─ Mounts ramfs on /private/var
   ├─ Probes and mounts eMMC xnusys partition on /System
   ├─ Prepares sandbox directories, symlinks, and sockets
   └─ Sets argv[0] = "/sbin/launchd" and execve("/sbin/launchd.ios")
         │
         ▼
[ Apple launchd (iOS 16.5) ]
   ├─ Starts system services & daemons
   ├─ Starts backboardd (Window server / input pipeline)
   └─ Launches SpringBoard (iOS Home Screen)
         │
         ▼
[ rc.jobs.unlock (Second 32) ]
   └─ Injects simulated upward swipe via /dev/redwood-touch -> Unlock Home Screen
```

---

## 4. Repository Layout

| Directory / File | Description |
| :--- | :--- |
| `mtkloader/` | Bare-metal ARM64 loader executed by LK prior to XNU kernel entry. Contains framebuffer console, OVL compositing controls, and DT setup. |
| `mtkloader/mtkloader.c` | Main loader logic, hardware initialization, memory layout setup, and XNU jumping. |
| `mtkloader/mtkdbg.h` | Low-level debug console, font renderer, and hardware memory blit scrolling (`fbcon_newline`). |
| `mtkloader/build_flashed.py` | Assembles `mtkloader.bin` + `mach_kernel` + `build-payload.bin` into `xnu-flashed-slot_a.img`. |
| `userland/` | iOS 16.5 userland bootstrap, ramdisk generator, and APFS root filesystem source. |
| `userland/src/init.c` | HakOS PID 1 supervisor. Mounts filesystems, configures runtime environment, and launches Apple `launchd`. |
| `userland/src/stage0.c` | Early stage bootstrap loader. |
| `userland/system.apfs` | 2.31 GiB APFS image containing Apple iOS 16.5 system binaries, frameworks, and dyld shared cache. |
| `userland/root.apfs` | 4.19 MB APFS ramdisk containing `/sbin/init`, `/stage0`, and bootstrap configurations. |
| `tools/` | Host automation tools, partition scripts, and BROM handlers. |
| `tools/setup_ios_brom.py` | Autonomous MediaTek BROM script that carves `xnusys` (4 GiB) on eMMC and flashes `system.apfs` + `boot_a`. |
| `xnu-flashed-slot_a.img` | Ready-to-flash 48.4 MB chainboot image for `boot_a`. |

---

## 5. Critical Technical Quirks & Implementation Rules

### A. Apple `launchd` PID 1 Requirement
* **Quirk:** Apple's official `launchd` in iOS 16.5 verifies its own invocation process name (`argv[0]`). If `argv[0]` is not strictly `"/sbin/launchd"`, `launchd` exits with an error. When PID 1 exits in Darwin, the kernel halts/powers down with `reason 0 code 0x0`.
* **Rule:** In `userland/src/init.c`, `l_argv[0]` must always be explicitly set to `"/sbin/launchd"` before calling `execve("/sbin/launchd.ios", ...)`. Never pass `/sbin/launchd.ios` as `argv[0]`.

### B. MediaTek Little Kernel OVL0 Compositing (Boot Logo)
* **Quirk:** Little Kernel (LK) uses MediaTek Display Overlay (OVL0) multi-layer compositing. Layer 0 holds the console buffer, but Layers 1..3 continuously scan a separate DRAM buffer containing the Xiaomi / "Redmi" boot logo. Simply wiping Layer 0 leaves the logo persistently showing over the log.
* **Rule:** In `mtkloader.c`, `display_alpha_off()` must explicitly disable Layers 1..3 (`OVL_L1_CON`, `OVL_L2_CON`, `OVL_L3_CON`), keep their DMA addresses pointing to valid `fb_base` (`OVL_L1_ADDR`, `OVL_L2_ADDR`, `OVL_L3_ADDR` — never 0x0 to prevent green YUV tearing), lock `OVL_SRC_CON = 1`, and commit with `DISP_MUTEX_EN` + `DISP_MUTEX_TRIGGER`.

### B2. DISP Mutex Registers
* **Layout:** DISP mutex 0 is the block at `0x14001000`, one instance every `0x20` bytes (Linux `mtk-mutex` layout): `EN 0x20`, frame trigger `0x24`, `RST 0x28`, `MOD 0x2c`, `SOF 0x30`.
* **Rule:** Never write `RST` (`0x28`): it forgets the MOD/SOF that say which modules compose the frame, and nothing in the loader puts them back, so the panel stays dark. Another MediaTek port (merlinx) drives `0x14016020`; that is a different block, not this one on selene.

### C. Framebuffer Console Hardware Scrolling
* **Quirk:** Framebuffer console on Redmi 10 is 1080x2400. Text running past the bottom line previously stalled or caused log truncation.
* **Rule:** In `mtkdbg.h`, `fbcon_newline()` performs real hardware memory blitting (`memmove(dst, src, bytes)`), clears the bottom 4 text lines, and performs clean Data Cache maintenance (`dc cvac`) to flush DRAM to the display controller.

### D. eMMC Partitioning (`xnusys`)
* **Layout:** eMMC total capacity: 244,156,352 sectors (125,008,052,224 bytes).
* **`userdata` victim:** Original end LBA was 244,156,318.
* **`xnusys` location:** LBA `235765760` to `244156351` (8,390,592 sectors = 4,295,983,104 bytes = 4.00 GiB).
* **Rule:** When modifying or flashing `xnusys`, ensure sector boundaries and GPT header/entry CRCs are strictly valid.

---

## 6. How to Build & Flash

### 1. Build Userland Ramdisk
```bash
cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/userland
./build.sh
```
*Outputs: `userland/root.apfs` and `build-payload.bin`.*

### 2. Assemble Chainboot Image (`boot_a`)
```bash
cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene
python3 mtkloader/build_flashed.py
```
*Outputs: `xnu-flashed-slot_a.img` (48.4 MB).*

### 3. Flash `boot_a` via Fastboot
Ensure phone is in Fastboot mode (`fastboot devices` shows `65cb03c00702`):
```bash
fastboot -s 65cb03c00702 flash boot_a /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/xnu-flashed-slot_a.img
fastboot -s 65cb03c00702 reboot
```

### 4. Turnkey BROM Repartition & Flash (`xnusys` + `boot_a`)
If eMMC was repartitioned by stock ROM:
```bash
cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene
/home/naidra/mtkclient/.venv/bin/python tools/setup_ios_brom.py
```
*Hold Volume Up + Volume Down and connect USB to Redmi 10.*

---

## 7. Commit & Version Control Standards
* Always verify changes with `git diff` before committing.
* Write concise, descriptive commit messages describing the hardware effect (e.g. `selene: fix display scrolling, disable boot logo layers`).
* Push changes to remote branch `mtk-selene` on `https://github.com/naidrahiqa/hakios-xnu-kernel.git`.
