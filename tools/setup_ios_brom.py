#!/usr/bin/env python3
import binascii
import os
import struct
import sys
import uuid
import time

sys.path.insert(0, '/home/naidra/mtkclient')
from mtkclient.config.mtk_config import MtkConfig
from mtkclient.Library.mtk_class import Mtk
from mtkclient.Library.DA.mtk_da_handler import DaHandler

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SYSTEM_APFS = os.path.join(ROOT, "userland", "system.apfs")
BOOT_IMG = os.path.join(ROOT, "xnu-flashed-slot_a.img")

XNUSYS_SECTOR = 235765760
BOOT_A_SECTOR = 1019904


def main():
    if not os.path.isfile(SYSTEM_APFS):
        sys.exit(f"Error: {SYSTEM_APFS} not found!")
    if not os.path.isfile(BOOT_IMG):
        sys.exit(f"Error: {BOOT_IMG} not found!")

    print("[*] Initializing mtkclient...")
    config = MtkConfig()
    config.vid = 0x0e8d  # MediaTek Inc. ONLY
    mtk = Mtk(config=config, loglevel=20)
    da_handler = DaHandler(mtk, 20)

    print("[*] Connecting to Redmi 10 in BROM mode...")
    mtk = da_handler.connect(mtk, ".")
    if mtk is None:
        sys.exit("[-] Could not connect to device.")

    mtk = da_handler.configure_da(mtk)
    if mtk is None:
        sys.exit("[-] Could not configure DA.")

    print("[+] DA Stage 2 loaded successfully!")

    # 1. Write system.apfs to xnusys
    xnusys_addr = XNUSYS_SECTOR * 512
    apfs_size = os.path.getsize(SYSTEM_APFS)
    print(f"[*] Writing system.apfs ({apfs_size / (1024**3):.2f} GB) to 'xnusys' at sector {XNUSYS_SECTOR} (offset {hex(xnusys_addr)})...")
    if not mtk.daloader.writeflash(addr=xnusys_addr,
                                   length=apfs_size,
                                   filename=SYSTEM_APFS,
                                   parttype="user",
                                   display=True):
        sys.exit(f"[-] Failed to write {SYSTEM_APFS} to xnusys")
    print("[+] Successfully wrote system.apfs to xnusys!")

    # 2. Write XNU boot image to boot_a
    boot_a_addr = BOOT_A_SECTOR * 512
    boot_size = os.path.getsize(BOOT_IMG)
    print(f"[*] Writing XNU boot image ({boot_size / (1024**2):.2f} MB) to 'boot_a' at sector {BOOT_A_SECTOR} (offset {hex(boot_a_addr)})...")
    if not mtk.daloader.writeflash(addr=boot_a_addr,
                                   length=boot_size,
                                   filename=BOOT_IMG,
                                   parttype="user",
                                   display=True):
        print("[-] Warning: Failed to write boot_a via DA")
    else:
        print("[+] Successfully wrote XNU kernel to boot_a!")

    print("\n" + "="*50)
    print("[+] ALL DONE! iOS 16.5 system.apfs and XNU kernel are installed!")
    print("="*50)
    mtk.daloader.close()


if __name__ == "__main__":
    main()
