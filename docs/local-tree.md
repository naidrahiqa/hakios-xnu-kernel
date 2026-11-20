# This tree on the bench (selene)

How this copy is arranged: the branches, what stays on this machine, and which command writes to
the phone. The port itself is in [from-a-clone.md](from-a-clone.md) and
[mtk-porting.md](mtk-porting.md).

## Branches

`mtk-selene` is the working branch. It is the only one pushed, to `origin/mtk-selene`
(<https://github.com/naidrahiqa/hakios-xnu-kernel.git>). Commits for the Redmi 10 go there.

`main` is the common tree: the XNU source and the generic MediaTek layer, checked out at
`../hakios-xnu-kernel`. It is a worktree of this same repository, not a second clone. The common
layer is changed there and pushed to `origin/main`.

`origin/mtk` is the Infinix Hot 20 (X6826B) branch and `origin/legacy-hackios` is the old tree.
Neither is merged into `mtk-selene`: the phones share the boot chain, not the board code. `emre`
(EmreDemirn/hakios-mt6768) is another project with its own history; it is a reference and is not
merged.

## What is local

`.gitignore` is a whitelist: only what it names is tracked. `ios165/` and `xnu-deps/` keep their
scripts and wrappers in the tree, but the Apple files, the toolchain and the SDK beside them stay
on this machine. The rest of the local weight is build output that a fresh clone makes again:
`xnu-src/`, `userland/obj/`, `userland/system.apfs`, `userland/root.apfs`,
`xnu-flashed-slot_a.img`, `build-payload.bin` and `logs/`. `prebuilt/` is tracked, so a clone
without the cross toolchain can still make the images.

## Writing to the phone

Only `boot_a` and `xnusys` are written. Never `boot_b`, `super`, `nvram`, `persist` or any other
partition: slot B is the way back to Android.

```sh
# the loader and the boot image
mtkloader/build.sh                    # -DCHG_PROBE=1 adds the charger probe
python3 mtkloader/build_flashed.py    # xnu-flashed-slot_a.img

# boot_a (fastboot)
fastboot -s 65cb03c00702 flash boot_a xnu-flashed-slot_a.img

# xnusys, from Android with root: checks the size and dd's status
HAKIOS_SERIAL=65cb03c00702 tools/system-flash.sh

# xnusys and boot_a over BROM, when xnusys does not exist yet: reads the GPT, carves 4 GiB off
# userdata, and checks that both images fit their entries
python3 tools/setup_ios_brom.py
```

The host is tethered through an OPPO phone (`OR9HCQUWZ9SGGQ4P`). Never send `adb`, `fastboot` or a
USB reset to it; every command names the Redmi 10 (`65cb03c00702`).
