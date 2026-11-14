#!/home/naidra/mtkclient/.venv/bin/python
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

TOTAL_SECTORS = 244156352
SECTOR_SIZE = 512
XNUSYS_SECTOR = 235765760
BOOT_A_SECTOR = 1019904
NEW_SECTORS = 8 * 1024 * 1024  # 4 GiB
ALIGN = 2048


def crc32(b):
    return binascii.crc32(b) & 0xffffffff


def name_of(entry):
    return entry[56:128].decode("utf-16-le").rstrip("\0")


def check_and_carve_gpt(mtk):
    print("[*] Checking eMMC GPT for 'xnusys' partition...")
    # Read Primary GPT (sectors 0..33 = 34 sectors = 17408 bytes)
    pri = bytearray(mtk.daloader.readflash(addr=0, length=34 * 512, parttype="user", display=False))
    if len(pri) < 34 * 512:
        sys.exit("[-] Failed to read primary GPT")

    # Read Backup GPT (last 33 sectors = 16896 bytes)
    sec_addr = (TOTAL_SECTORS - 33) * 512
    sec = bytearray(mtk.daloader.readflash(addr=sec_addr, length=33 * 512, parttype="user", display=False))
    if len(sec) < 33 * 512:
        sys.exit("[-] Failed to read backup GPT")

    sig, rev, hsize, hcrc, _, mylba, altlba, first, last = struct.unpack_from("<8sIIIIQQQQ", pri, 512)
    entlba, nent, entsize, entcrc = struct.unpack_from("<QIII", pri, 512 + 72)
    assert sig == b"EFI PART", "Invalid GPT signature"

    ents = pri[entlba * 512: entlba * 512 + nent * entsize]
    entries = [bytearray(ents[i * entsize:(i + 1) * entsize]) for i in range(nent)]
    
    has_xnusys = any(name_of(e) == "xnusys" for e in entries if e[0:16] != b"\0" * 16)
    if has_xnusys:
        print("[+] 'xnusys' partition already exists in GPT!")
        return

    print("[*] 'xnusys' not found. Carving 4 GiB off 'userdata'...")
    victim = next(i for i, e in enumerate(entries) if name_of(e) == "userdata")
    free = next(i for i, e in enumerate(entries) if e[0:16] == b"\0" * 16)
    vstart, vend = struct.unpack_from("<QQ", entries[victim], 32)

    new_start = (vend + 1 - NEW_SECTORS) & ~(ALIGN - 1)
    new_end = vend
    assert new_start == XNUSYS_SECTOR, f"Calculated start {new_start} != expected {XNUSYS_SECTOR}"

    # Shrink userdata
    struct.pack_into("<QQ", entries[victim], 32, vstart, new_start - 1)
    # Create xnusys
    e = entries[free]
    e[0:16] = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", e, 32, new_start, new_end, 0)
    e[56:128] = "xnusys".encode("utf-16-le").ljust(72, b"\0")

    new_ents = b"".join(bytes(x) for x in entries)
    pri[entlba * 512: entlba * 512 + len(new_ents)] = new_ents

    # Recompute Primary Header CRC
    struct.pack_into("<I", pri, 512 + 16, 0)
    struct.pack_into("<I", pri, 512 + 84, crc32(new_ents))
    struct.pack_into("<I", pri, 512 + 16, crc32(bytes(pri[512:512 + hsize])))

    # Recompute Backup Header CRC
    sec_entlba = TOTAL_SECTORS - 1 - (nent * entsize // 512)
    sec[:len(new_ents)] = new_ents
    struct.pack_into("<I", sec, len(sec) - 512 + 16, 0)
    struct.pack_into("<I", sec, len(sec) - 512 + 84, crc32(new_ents))
    struct.pack_into("<I", sec, len(sec) - 512 + 16, crc32(bytes(sec[len(sec) - 512:len(sec) - 512 + hsize])))

    print("[*] Writing updated Primary GPT to eMMC...")
    mtk.daloader.writeflash(addr=0, length=len(pri), filename=None, data=bytes(pri), parttype="user", display=False)

    print("[*] Writing updated Backup GPT to eMMC...")
    mtk.daloader.writeflash(addr=sec_addr, length=len(sec), filename=None, data=bytes(sec), parttype="user", display=False)
    print("[+] Successfully carved 'xnusys' (4 GiB) in Primary and Backup GPT!")


def main():
    os.chdir(ROOT)
    if not os.path.isfile(SYSTEM_APFS):
        sys.exit(f"Error: {SYSTEM_APFS} not found!")
    if not os.path.isfile(BOOT_IMG):
        sys.exit(f"Error: {BOOT_IMG} not found!")

    print("[*] Initializing mtkclient...")
    config = MtkConfig()
    config.vid = 0x0e8d  # MediaTek Inc. ONLY
    config.hwparam_path = ROOT
    mtk = Mtk(config=config, loglevel=20)
    da_handler = DaHandler(mtk, 20)

    print("[*] Waiting for Redmi 10 in BROM mode (Hold Vol Up + Vol Down & connect USB)...")
    mtk = da_handler.connect(mtk, ROOT)
    if mtk is None:
        sys.exit("[-] Could not connect to device.")

    mtk = da_handler.configure_da(mtk)
    if mtk is None:
        sys.exit("[-] Could not configure DA.")

    print("[+] DA Stage 2 loaded successfully!")

    # 1. Ensure xnusys partition exists
    check_and_carve_gpt(mtk)

    # 2. Write system.apfs to xnusys
    xnusys_addr = XNUSYS_SECTOR * 512
    apfs_size = os.path.getsize(SYSTEM_APFS)
    print(f"[*] Writing system.apfs ({apfs_size / (1024**3):.2f} GB) to 'xnusys' at sector {XNUSYS_SECTOR}...")
    if not mtk.daloader.writeflash(addr=xnusys_addr,
                                   length=apfs_size,
                                   filename=SYSTEM_APFS,
                                   parttype="user",
                                   display=True):
        sys.exit(f"[-] Failed to write {SYSTEM_APFS} to xnusys")
    print("[+] Successfully wrote system.apfs to xnusys!")

    # 3. Write XNU boot image to boot_a
    boot_a_addr = BOOT_A_SECTOR * 512
    boot_size = os.path.getsize(BOOT_IMG)
    print(f"[*] Writing XNU boot image ({boot_size / (1024**2):.2f} MB) to 'boot_a' at sector {BOOT_A_SECTOR}...")
    if not mtk.daloader.writeflash(addr=boot_a_addr,
                                   length=boot_size,
                                   filename=BOOT_IMG,
                                   parttype="user",
                                   display=True):
        print("[-] Warning: Failed to write boot_a via DA")
    else:
        print("[+] Successfully wrote XNU kernel to boot_a!")

    print("\n" + "="*50)
    print("[+] ALL DONE! Phone is ready to boot iOS 16.5!")
    print("="*50)
    mtk.daloader.close()


if __name__ == "__main__":
    main()
