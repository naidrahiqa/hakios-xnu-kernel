#!/usr/bin/env python3
"""
Pack mtkloader, XNUP payload, and stock DTB into Android boot image v2
for MediaTek phones (Infinix Hot 20 / X6826B, Redmi 10 / selene, eos) for `fastboot boot`.
"""
import argparse
import gzip
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PREBUILT = os.path.join(HERE, 'prebuilt', 'uefi')

BOARDS = {
    'x6826b': {
        'name': 'x6826b',
        'desc': 'Infinix Hot 20 (X6826B / MT6768)',
        'dram_base': 0x40078000,
        'kernel_offset': 0x00008000,
        'ramdisk_offset': 0x07C08000,
        'tags_offset': 0x0BC08000,
        'dtb_file': 'x6826b.dtb',
        'default_output': 'xnu-x6826b.img',
    },
    'selene': {
        'name': 'selene',
        'desc': 'Redmi 10 / 10 Prime / 10 2022 / Note 11 4G (selene / MT6769H)',
        'dram_base': 0x40000000,
        'kernel_offset': 0x00080000,
        'ramdisk_offset': 0x07C80000,
        'tags_offset': 0x0BC80000,
        'dtb_file': 'selene.dtb',
        'default_output': 'xnu-selene.img',
    },
    'eos': {
        'name': 'eos',
        'desc': 'Redmi Note 11 4G China (eos / MT6769H)',
        'dram_base': 0x40000000,
        'kernel_offset': 0x00080000,
        'ramdisk_offset': 0x07C80000,
        'tags_offset': 0x0BC80000,
        'dtb_file': 'selene.dtb',
        'default_output': 'xnu-eos.img',
    },
}

PAGE_SIZE = 2048
HEADER_VERSION = 2


def get_board(name):
    name = (name or os.environ.get('BOARD') or 'selene').lower()
    if name not in BOARDS:
        sys.exit(f'Unknown board: {name}. Choose from: {", ".join(BOARDS.keys())}')
    return BOARDS[name]


def main(default_board=None):
    if default_board is None:
        default_board = os.environ.get('BOARD') or 'selene'
    ap = argparse.ArgumentParser(description='Pack mtkloader, payload, and DTB into Android boot image v2')
    ap.add_argument('-b', '--board', default=default_board, choices=list(BOARDS.keys()), help='Target board (default: %(default)s)')
    ap.add_argument('-p', '--payload', required=True, help='Path to XNUP payload container')
    ap.add_argument('-o', '--output', help='Output boot image path (default: xnu-<board>.img)')
    ap.add_argument('--dtb', help='Path to device DTB (default: prebuilt/uefi/<board>.dtb)')
    args = ap.parse_args()

    binfo = get_board(args.board)
    dtb_path = args.dtb or os.path.join(PREBUILT, binfo['dtb_file'])
    out_path = args.output or os.path.join(HERE, binfo['default_output'])

    if not os.path.isfile(dtb_path):
        sys.exit(f'Error: no device tree for {binfo["name"]} ({dtb_path}): give one with --dtb')

    loader_bin = os.path.join(HERE, 'mtkloader', 'mtkloader.bin')
    if not os.path.isfile(loader_bin):
        sys.exit(f'Error: {loader_bin} not found. Run mtkloader/build.sh first.')

    loader_data = open(loader_bin, 'rb').read()
    payload_data = open(args.payload, 'rb').read()
    assert payload_data[:4] == b'XNUP', 'Invalid payload magic'

    blob = loader_data + b'\0' * (-len(loader_data) % 16) + payload_data
    blob += b'\0' * (-len(blob) % 16)
    print(f'Kernel blob [{binfo["name"]}]: loader {len(loader_data)} bytes, payload {len(payload_data)} bytes (total {len(blob)})')

    kernel_temp = out_path + '.kernel~'
    with open(kernel_temp, 'wb') as f:
        f.write(gzip.compress(blob, 6))

    mkbootimg = os.path.join(PREBUILT, 'mkbootimg.py')
    ramdisk = os.path.join(PREBUILT, 'ramdisk')

    cmd = [
        sys.executable, mkbootimg,
        '--kernel', kernel_temp,
        '--ramdisk', ramdisk,
        '--dtb', dtb_path,
        '--base', hex(binfo['dram_base']),
        '--kernel_offset', hex(binfo['kernel_offset']),
        '--ramdisk_offset', hex(binfo['ramdisk_offset']),
        '--tags_offset', hex(binfo['tags_offset']),
        '--pagesize', str(PAGE_SIZE),
        '--header_version', str(HEADER_VERSION),
        '-o', out_path
    ]

    try:
        subprocess.check_call(cmd)
    finally:
        try:
            os.unlink(kernel_temp)
        except OSError:
            pass

    out_size = os.path.getsize(out_path)
    print(f'Created {out_path} ({binfo["name"]}): {out_size} bytes')


if __name__ == '__main__':
    main()
