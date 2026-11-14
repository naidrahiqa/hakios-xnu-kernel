---
name: hakios-build
description: >-
  Use this skill to compile userland binaries, generate the APFS root ramdisk, compile mtkloader,
  and assemble the final chainboot kernel image (xnu-flashed-slot_a.img) for HakOS on Redmi 10 (selene).
---

# HakOS Build Guide (Redmi 10 / selene)

This skill provides step-by-step instructions to compile the HakOS bootstrap components and assemble the boot image for Redmi 10 (`selene`, MediaTek MT6769H).

---

## Architecture of the Build

The final boot image flashed to `boot_a` is `xnu-flashed-slot_a.img` (approx. 48.4 MB).
It is assembled by concatenating:
1. `mtkloader/mtkloader.bin` (Bare-metal loader with display & DT init)
2. `mach_kernel` (Apple XNU ARM64 kernel)
3. `build-payload.bin` (Consists of DTB + `userland/root.apfs` ramdisk)

---

## 1. Prerequisites & Environment

* Host Architecture: Linux x86_64
* Compilers: Clang / LLVM with AArch64 target support
* Target Root: `/home/naidra/Projects/IOS/hakios-xnu-kernel-selene`

---

## 2. Step 1: Build Userland & Root Ramdisk

The userland contains the early Darwin PID 1 bootstrap (`init`), stage0 loader, and test binaries.

```bash
cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/userland
./build.sh
```

### What `build.sh` does:
1. Compiles `src/init.c` with `-target arm64-apple-ios16.5` and `-Wl,-platform_version,ios,16.5,16.5`.
   * **Crucial check:** Ensure `l_argv[0] = "/sbin/launchd"` in `src/init.c` so Apple `launchd` does not reject PID 1 execution.
2. Compiles `src/stage0.c` and `src/child.c`.
3. Creates a fresh APFS container image: `userland/root.apfs`.
4. Copies compiled binaries, `/etc/fstab`, `/etc/rc.jobs.unlock`, and mock devices into `root.apfs`.
5. Compiles payload builder and generates `build-payload.bin` (19.8 MB).

### Verification:
```bash
ls -lh /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/userland/root.apfs
ls -lh /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/build-payload.bin
```
*Expected: `root.apfs` is ~4.2 MB, `build-payload.bin` is ~19.8 MB.*

---

## 3. Step 2: Build `mtkloader`

`mtkloader` executes from LK at physical DRAM address before MMU activation.

```bash
cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/mtkloader
./build.sh
```

### Key Components Built:
* `mtkloader.c`: Initializes hardware clocks, disables Xiaomi boot logo via OVL0 Layer 1..3 zeroing (`display_alpha_off()`), sets up Apple DT.
* `mtkdbg.h`: Provides framebuffer text console with memory blit scrolling (`fbcon_newline`).

### Verification:
```bash
ls -lh /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/mtkloader/mtkloader.bin
```
*Expected: `mtkloader.bin` exists and is non-empty.*

---

## 4. Step 3: Assemble Chainboot Image (`boot_a`)

Run the assembly script:

```bash
cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene
python3 mtkloader/build_flashed.py
```

### Verification:
```bash
ls -lh /home/naidra/Projects/IOS/hakios-xnu-kernel-selene/xnu-flashed-slot_a.img
```
*Expected output:*
* Total size: ~48.4 MB (under the 64 MB partition boundary of `boot_a`).

---

## 5. Clean / Rebuild All in One Shot

```bash
cd /home/naidra/Projects/IOS/hakios-xnu-kernel-selene && \
  (cd userland && ./build.sh) && \
  (cd mtkloader && ./build.sh) && \
  python3 mtkloader/build_flashed.py
```
