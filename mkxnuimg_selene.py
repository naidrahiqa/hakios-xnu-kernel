#!/usr/bin/env python3
"""
Pack mtkloader, XNUP payload, and stock DTB into Android boot image v2
for Redmi 10 family (selene / selenes / eos / MT6769H) for `fastboot boot`.
"""
import mkxnuimg_mtk

if __name__ == '__main__':
    mkxnuimg_mtk.main(default_board='selene')
