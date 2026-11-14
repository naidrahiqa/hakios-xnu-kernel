#!/usr/bin/env python3
"""
Put a bare-metal program in front of Android's kernel in a boot image (header v2).

  chainboot.py <android-boot.img> <program.bin> <out.img> [payload.bin] [--partition-size SIZE]

LK decompresses the kernel to 0x40080000 and enters it there. The new kernel is gzip of:
program.bin, padded to CHAIN_OFFSET, then Android's own (uncompressed) Image. The program runs
first and then moves the Image down to 0x40080000 and starts it (mtkloader/fbdiag.c). An optional
payload (mtkloader's "XNUP") follows the Image, 16-aligned. The program's own arm64 header gets
the Image's length in res2 (0x28), the payload's offset in res3 (0x30) and a covering image_size.
Everything else - header fields, cmdline, ramdisk, dtb - is copied from the Android image.
The boot partition size is taken from the device (fastboot getvar), an option, the source image
dump size, or the default (32 MB for X6826B, 64 MB for selene).
"""
import gzip
import hashlib
import os
import struct
import subprocess
import sys

CHAIN_OFFSET = 0x40000
DEFAULT_BOOT_PARTITION = 32 << 20


def pad(b, n):
    return b + b'\0' * (-len(b) % n)


def parse_size(s):
    s = s.strip()
    if s.endswith(('M', 'm', 'MB', 'mb', 'MiB', 'mib')):
        num = s.rstrip('MmiBb')
        return int(num) << 20
    if s.endswith(('K', 'k', 'KB', 'kb', 'KiB', 'kib')):
        num = s.rstrip('KkiBb')
        return int(num) << 10
    if s.startswith(('0x', '0X')):
        return int(s, 16)
    return int(s)


def detect_boot_partition(src, custom_size=None):
    if custom_size:
        return parse_size(custom_size)

    env_size = os.environ.get('BOOT_PARTITION_SIZE') or os.environ.get('BOOT_PARTITION')
    if env_size:
        return parse_size(env_size)

    # Try fastboot query from device if HAKIOS_SERIAL is set
    serial = os.environ.get('HAKIOS_SERIAL')
    if serial:
        try:
            cmd = ['fastboot', '-s', serial, 'getvar', 'partition-size:boot_a']
            res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=1.5)
            for line in res.stdout.splitlines():
                if 'partition-size:boot_a:' in line:
                    val = line.split('partition-size:boot_a:')[-1].strip()
                    if val:
                        return int(val, 16) if val.startswith('0x') else int(val)
        except Exception:
            pass

    # If the source image is a raw dump of the partition (e.g. 32M or 64M)
    try:
        fsize = os.path.getsize(src)
        if fsize >= (32 << 20) and (fsize % (1024 * 1024) == 0):
            return fsize
    except OSError:
        pass

    # If BOARD is selene or eos
    board = os.environ.get('BOARD', '').lower()
    if board in ('selene', 'eos'):
        return 64 << 20

    return DEFAULT_BOOT_PARTITION


def main():
    custom_size = None
    args = []
    i = 1
    while i < len(sys.argv):
        arg = sys.argv[i]
        if arg in ('--partition-size', '-s') and i + 1 < len(sys.argv):
            custom_size = sys.argv[i + 1]
            i += 2
        elif arg.startswith('--partition-size='):
            custom_size = arg.split('=', 1)[1]
            i += 1
        else:
            args.append(arg)
            i += 1

    if len(args) < 3:
        sys.exit(f'Usage: {sys.argv[0]} <android-boot.img> <program.bin> <out.img> [payload.bin] [--partition-size SIZE]')

    src, prog_path, out = args[0:3]
    payload = open(args[3], 'rb').read() if len(args) > 3 else b''
    d = open(src, 'rb').read()
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    ksize, _, rsize, _, ssize, _, _, page, hver = struct.unpack_from('<9I', d, 8)
    assert hver == 2, f'header v{hver}, expected 2'
    rdo_size = struct.unpack_from('<I', d, 1632)[0]
    dtb_size = struct.unpack_from('<I', d, 1648)[0]

    def pages(n):
        return (n + page - 1) // page * page

    off = page
    kernel = d[off:off + ksize]; off += pages(ksize)
    ramdisk = d[off:off + rsize]; off += pages(rsize)
    second = d[off:off + ssize]; off += pages(ssize)
    rdo = d[off:off + rdo_size]; off += pages(rdo_size)
    dtb = d[off:off + dtb_size]

    if kernel[:2] == b'\x1f\x8b':
        import zlib
        dc = zlib.decompressobj(16 + zlib.MAX_WBITS)
        image = dc.decompress(kernel)
        if dc.unused_data:
            image += dc.unused_data
    else:
        image = kernel
    assert image[0x38:0x3c] == b'ARM\x64', 'Android kernel is not an arm64 Image'
    linux_image_size = struct.unpack_from('<Q', image, 0x10)[0]

    prog = bytearray(open(prog_path, 'rb').read())
    assert prog[0x38:0x3c] == b'ARM\x64', 'program has no arm64 header'
    assert len(prog) <= CHAIN_OFFSET, 'program too large'
    payload_off = CHAIN_OFFSET + len(pad(image, 16)) if payload else 0
    total = max(CHAIN_OFFSET + linux_image_size, payload_off + len(payload))
    struct.pack_into('<Q', prog, 0x10, (total + 0xfffff) & ~0xfffff)
    struct.pack_into('<Q', prog, 0x28, len(image))
    struct.pack_into('<Q', prog, 0x30, payload_off)
    blob = bytes(prog) + b'\0' * (CHAIN_OFFSET - len(prog)) + image
    if payload:
        blob = pad(blob, 16) + payload
    new_kernel = gzip.compress(blob, 9, mtime=0)

    hdr = bytearray(d[:page])
    struct.pack_into('<I', hdr, 8, len(new_kernel))
    sha = hashlib.sha1()
    for part in (new_kernel, ramdisk, second, rdo, dtb):
        sha.update(part)
        sha.update(struct.pack('<I', len(part)))
    hdr[576:608] = pad(sha.digest(), 32)

    img = bytes(hdr) + pad(new_kernel, page) + pad(ramdisk, page) + pad(second, page) + pad(rdo, page) + pad(dtb, page)

    boot_partition = detect_boot_partition(src, custom_size)
    if len(img) > boot_partition:
        raise SystemExit(f'{out}: {len(img)} bytes, more than the {boot_partition} of the boot partition '
                         f'({len(img) - boot_partition} too many): the payload has to shrink')
    open(out, 'wb').write(img)
    print(f'{out}: {len(img)} bytes (program {len(prog)}, Image {len(image)}, payload {len(payload)}, kernel gz {len(new_kernel)}, partition limit {boot_partition // (1024*1024)} MB)')


if __name__ == '__main__':
    main()
