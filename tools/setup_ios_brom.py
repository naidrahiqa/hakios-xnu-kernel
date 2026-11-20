#!/home/naidra/mtkclient/.venv/bin/python
import binascii
import os
import struct
import sys
import uuid

sys.path.insert(0, '/home/naidra/mtkclient')
from mtkclient.config.mtk_config import MtkConfig
from mtkclient.Library.mtk_class import Mtk
from mtkclient.Library.DA.mtk_da_handler import DaHandler

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SYSTEM_APFS = os.path.join(ROOT, "userland", "system.apfs")
BOOT_IMG = os.path.join(ROOT, "xnu-flashed-slot_a.img")

SECTOR_SIZE = 512
NEW_SECTORS = 8 * 1024 * 1024  # 4 GiB
ALIGN = 2048


def crc32(b):
    return binascii.crc32(b) & 0xffffffff


def name_of(entry):
    return entry[56:128].decode("utf-16-le").rstrip("\0")


def find_partition(entries, name):
    for e in entries:
        if e[0:16] != b"\0" * 16 and name_of(e) == name:
            return struct.unpack_from("<QQ", e, 32)
    return None


def read_gpt(mtk):
    print("[*] Reading the GPT...")
    pri = bytearray(mtk.daloader.readflash(addr=0, length=34 * 512, filename="", parttype="user", display=False))
    if len(pri) < 34 * 512:
        sys.exit("[-] Failed to read the primary GPT")

    sig, rev, hsize, hcrc, _, mylba, altlba, first, last = struct.unpack_from("<8sIIIIQQQQ", pri, 512)
    if sig != b"EFI PART":
        sys.exit("[-] The primary GPT has no EFI PART signature")
    entlba, nent, entsize, entcrc = struct.unpack_from("<QIII", pri, 512 + 72)
    if nent == 0 or entsize < 128 or nent * entsize > 32 * 512:
        sys.exit(f"[-] The GPT's entry array is not one this script understands ({nent} x {entsize})")
    if altlba < 34:
        sys.exit(f"[-] The GPT says the backup header is at LBA {altlba}")
    ents = pri[entlba * 512:entlba * 512 + nent * entsize]
    if len(ents) != nent * entsize:
        sys.exit("[-] The primary GPT's entry array is outside the blocks read")
    entries = [bytearray(ents[i * entsize:(i + 1) * entsize]) for i in range(nent)]

    sec_addr = (altlba - 32) * 512
    sec = bytearray(mtk.daloader.readflash(addr=sec_addr, length=33 * 512, filename="", parttype="user", display=False))
    if len(sec) < 33 * 512:
        sys.exit("[-] Failed to read the backup GPT")
    bak_hdr = sec[len(sec) - 512:]
    if bak_hdr[:8] != b"EFI PART":
        sys.exit("[-] The backup GPT has no EFI PART signature")
    if struct.unpack_from("<Q", bak_hdr, 24)[0] != altlba:
        sys.exit("[-] The backup header is not where the primary header says it is")
    bak_entlba = struct.unpack_from("<Q", bak_hdr, 72)[0]
    bak_off = (bak_entlba - (altlba - 32)) * 512
    if bak_off < 0 or bak_off + nent * entsize > len(sec) - 512:
        sys.exit("[-] The backup GPT's entry array is outside the blocks read")
    if bytes(sec[bak_off:bak_off + nent * entsize]) != bytes(ents):
        sys.exit("[-] The primary and backup GPT entries differ; nothing written")
    return pri, sec, entries, {
        "altlba": altlba,
        "entlba": entlba,
        "nent": nent,
        "entsize": entsize,
        "hsize": hsize,
        "sec_addr": sec_addr,
        "bak_off": bak_off,
    }


def carve_gpt(mtk, pri, sec, entries, gpt):
    if find_partition(entries, "xnusys") is not None:
        print("[+] 'xnusys' already exists; using its GPT entry.")
        return

    print("[*] 'xnusys' not found. Carving 4 GiB off 'userdata'...")
    victim = next((i for i, e in enumerate(entries)
                   if e[0:16] != b"\0" * 16 and name_of(e) == "userdata"), None)
    free = next((i for i, e in enumerate(entries) if e[0:16] == b"\0" * 16), None)
    if victim is None or free is None:
        sys.exit("[-] The GPT has no 'userdata' entry or no free entry")
    vstart, vend = struct.unpack_from("<QQ", entries[victim], 32)
    new_start = (vend + 1 - NEW_SECTORS) & ~(ALIGN - 1)
    new_end = vend
    if new_start <= vstart + ALIGN:
        sys.exit("[-] 'userdata' is too small to give 4 GiB")
    for i, e in enumerate(entries):
        if i == victim or e[0:16] == b"\0" * 16:
            continue
        s, l = struct.unpack_from("<QQ", e, 32)
        if s <= new_end and l >= new_start:
            sys.exit(f"[-] '{name_of(e)}' is in the way of the new partition")

    struct.pack_into("<QQ", entries[victim], 32, vstart, new_start - 1)
    e = entries[free]
    e[0:16] = uuid.UUID("0fc63daf-8483-4772-8e79-3d69d8477de4").bytes_le
    e[16:32] = uuid.uuid4().bytes_le
    struct.pack_into("<QQQ", e, 32, new_start, new_end, 0)
    e[56:128] = "xnusys".encode("utf-16-le").ljust(72, b"\0")

    new_ents = b"".join(bytes(x) for x in entries)
    pri[gpt["entlba"] * 512:gpt["entlba"] * 512 + len(new_ents)] = new_ents
    struct.pack_into("<I", pri, 512 + 16, 0)
    struct.pack_into("<I", pri, 512 + 84, crc32(new_ents))
    struct.pack_into("<I", pri, 512 + 16, crc32(bytes(pri[512:512 + gpt["hsize"]])))

    sec[gpt["bak_off"]:gpt["bak_off"] + len(new_ents)] = new_ents
    struct.pack_into("<I", sec, len(sec) - 512 + 16, 0)
    struct.pack_into("<I", sec, len(sec) - 512 + 84, crc32(new_ents))
    struct.pack_into("<I", sec, len(sec) - 512 + 16,
                     crc32(bytes(sec[len(sec) - 512:len(sec) - 512 + gpt["hsize"]])))

    print("[*] Writing the updated primary GPT...")
    if not mtk.daloader.writeflash(addr=0, length=len(pri), filename="", wdata=bytes(pri),
                                   parttype="user", display=False):
        sys.exit("[-] Failed to write the primary GPT")
    print("[*] Writing the updated backup GPT...")
    if not mtk.daloader.writeflash(addr=gpt["sec_addr"], length=len(sec), filename="", wdata=bytes(sec),
                                   parttype="user", display=False):
        sys.exit("[-] Failed to write the backup GPT")
    print("[+] Carved 'xnusys' (4 GiB) in both GPT copies.")


def write_image(mtk, name, entries, path, unit):
    part = find_partition(entries, name)
    if part is None:
        sys.exit(f"[-] The GPT has no '{name}' partition")
    first, last = part
    size = os.path.getsize(path)
    capacity = (last - first + 1) * SECTOR_SIZE
    if size > capacity:
        sys.exit(f"[-] {path} is {size} bytes, more than the {capacity} of '{name}'")
    amount = size / unit
    print(f"[*] Writing {os.path.basename(path)} ({amount:.2f} {'GB' if unit == 1024**3 else 'MB'}) "
          f"to '{name}' at LBA {first}...")
    if not mtk.daloader.writeflash(addr=first * SECTOR_SIZE, length=size, filename=path,
                                   parttype="user", display=True):
        sys.exit(f"[-] Failed to write {path} to '{name}'")
    print(f"[+] Wrote '{name}'.")


def main():
    os.chdir(ROOT)
    boot_only = "--boot-only" in sys.argv
    if not boot_only and not os.path.isfile(SYSTEM_APFS):
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

    pri, sec, entries, gpt = read_gpt(mtk)

    if not boot_only:
        carve_gpt(mtk, pri, sec, entries, gpt)
        write_image(mtk, "xnusys", entries, SYSTEM_APFS, 1024**3)
    else:
        print("[*] Skipping system.apfs (--boot-only requested)")

    write_image(mtk, "boot_a", entries, BOOT_IMG, 1024**2)

    print("\n" + "="*50)
    print("[+] ALL DONE! Phone is ready to boot iOS 16.5!")
    print("="*50)
    mtk.port.close()


if __name__ == "__main__":
    main()
