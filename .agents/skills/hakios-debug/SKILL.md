---
name: hakios-debug
description: >-
  Use this skill when diagnosing boot failures, display issues, kernel panics, launchd exits,
  or analyzing ramoops/pstore and framebuffer console output for HakOS on Redmi 10 (selene).
---

# HakOS Debugging Guide (Redmi 10 / selene)

This skill provides diagnostic procedures and troubleshooting playbooks for HakOS bring-up on Redmi 10 (`selene`).

---

## 1. Diagnostics Architecture

HakOS outputs diagnostic information across several channels:
1. **On-Screen Framebuffer Console:** High-speed text console running at 1080x2400 (scaled 1x or 2x) on MediaTek OVL0 Layer 0 (`0x7c400000`).
2. **DRAM Tail Buffer:** 8000-byte tail buffer dumped on return from XNU in `xnu_returned()`.
3. **Pstore / Ramoops:** Persistent console buffer stored at `0x44400000` readable upon reboot into Android (`/sys/fs/pstore/console-ramoops-0`).

---

## 2. Common Failure Modes & Fixes

### A. Kernel Halts at ~13-14 Seconds (`reason 0 code 0x0`)
* **Symptom:** XNU boots, mounts `/System` and `/private/var`, then powers down or halts cleanly with `reason 0 code 0x0` around second 13.8.
* **Root Cause:** `init` called `execve("/sbin/launchd.ios", ...)`. Apple's iOS 16.5 `launchd` strictly checks `argv[0]`. If `argv[0]` is not `"/sbin/launchd"`, `launchd` exits with status code 1. Darwin terminates PID 1, leading to immediate system halt.
* **Solution:**
  In `userland/src/init.c`:
  ```c
  char *l_argv[] = {
      "/sbin/launchd",  /* MUST be /sbin/launchd, never /sbin/launchd.ios */
      "--verbose",
      NULL
  };
  ```
  Recompile userland and update `xnu-flashed-slot_a.img`.

---

### B. Display Text Stalling / Bottom Truncation
* **Symptom:** Display prints lines until line 75, then text stops scrolling or previous lines get overwritten.
* **Root Cause:** Text renderer did not implement frame buffer memory shifting on newline.
* **Solution:**
  Verify `mtkloader/mtkdbg.h` contains hardware memory blit scrolling:
  ```c
  static inline void fbcon_newline(void) {
      if (fbc.cy + fbc.ch > fbc.h) {
          uint32_t shift_h = fbc.ch * 4;
          uint32_t move_bytes = (fbc.h - shift_h) * fbc.stride;
          memmove((void*)fbc.base, (void*)(fbc.base + shift_h * fbc.stride), move_bytes);
          memset((void*)(fbc.base + move_bytes), 0, shift_h * fbc.stride);
          dcache_clean((void*)fbc.base, fbc.h * fbc.stride);
          fbc.cy = fbc.h - shift_h;
      }
  }
  ```

---

### C. Xiaomi Boot Logo Stays Visible Over HakOS Log
* **Symptom:** The "Redmi" or Xiaomi logo remains visible in the background or covers the text log.
* **Root Cause:** Little Kernel (LK) leaves MediaTek OVL0 Layer 1 and Layer 2 active with a separate DMA address in DRAM.
* **Solution:**
  In `mtkloader/mtkloader.c`, ensure `display_alpha_off()` zeroes layers 1..3:
  ```c
  #define OVL_L1_CON  0xf60
  #define OVL_L2_CON  0xf80
  #define OVL_L3_CON  0xfa0
  #define OVL_SRC_CON 0xf04

  /* Disable Layers 1-3 */
  *(volatile uint32_t*)(DISP_OVL0_BASE + OVL_L1_CON) = 0;
  *(volatile uint32_t*)(DISP_OVL0_BASE + OVL_L2_CON) = 0;
  *(volatile uint32_t*)(DISP_OVL0_BASE + OVL_L3_CON) = 0;
  *(volatile uint32_t*)(DISP_OVL0_BASE + OVL_SRC_CON) = 1; /* Layer 0 only */
  ```

---

### D. Missing `/private/var` Mount
* **Symptom:** Daemons fail immediately with sandbox/read-only filesystem errors.
* **Root Cause:** `/private/var` is read-only on the root APFS container.
* **Solution:**
  `init.c` must mount `ramfs` at `/private/var` and populate the runtime directory skeleton (`mobile`, `tmp`, `db`, `preferences`).
  Verify photo log shows: `unmount(1) /private/var`.

---

## 3. Automated Unlock Swipe Diagnostics

SpringBoard boots locked. HakOS uses a background unlock job:
* **Trigger File:** `/etc/rc.jobs.unlock`
* **Mechanism:** At second 32 of boot, runs `rwtouch` writing an upward drag gesture sequence directly to `/dev/redwood-touch`.
* **Verification:**
  1. If SpringBoard remains on Lock Screen after second 35, check if `/dev/redwood-touch` character device is registered by the XNU touch driver.
  2. Confirm touch driver received the touch coordinate buffer in the kernel log.
