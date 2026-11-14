#!/usr/bin/env python3
"""
Pack xnu-flashed-slot_a.img: mtkloader, then Android's kernel, then the XNU payload, in the
rooted Android boot image for Redmi 10 (selene / eos). Booting it runs XNU once, then Android
comes back with XNU's log in /sys/fs/pstore/console-ramoops-0 (see mtkloader/mtkloader.c).

  build_flashed.py [android-boot.img]
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

candidate_boots = [
    sys.argv[1] if len(sys.argv) > 1 else None,
    os.environ.get('ANDROID_BOOT'),
    os.path.join(ROOT, 'backup', 'boot_a.img'),
    os.path.join(ROOT, 'backup', 'boot_a_magisk.img'),
    os.path.join(ROOT, '..', 'hakios-selene', 'fw-selene', 'boot_a.img'),
]

android = None
for c in candidate_boots:
    if c and os.path.isfile(c):
        android = c
        break

if not android:
    sys.exit('build_flashed.py: no boot_a.img found: place your phone\'s rooted boot image in '
             'backup/boot_a.img or set ANDROID_BOOT')

subprocess.check_call([sys.executable, os.path.join(ROOT, 'tools', 'chainboot.py'), android,
                       os.path.join(HERE, 'mtkloader.bin'), os.path.join(ROOT, 'xnu-flashed-slot_a.img'),
                       os.path.join(ROOT, 'build-payload.bin')])
