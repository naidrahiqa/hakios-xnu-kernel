# Sesi 2026-11-22 — xnusys fixed, homescreen iOS 16.5 tembus di selene

Target: Redmi 10 `selene`, serial `65cb03c00702`. OPPO hotspot `22d9:2769` tidak disentuh.
Semua tulis partisi hanya `boot_a` + `xnusys`, slot A terverifikasi sebelum/sesudah setiap flash.

## Ringkasan

Homescreen iOS 16.5 (SpringBoard + ikon + auto-unlock `rc.jobs.unlock`) tercapai di bare-metal
Redmi 10. Loop `/dev/md1` 0.45 s hilang. Root cause utama: `xnusys` di HP berisi `system.apfs`
lama (601698 blocks) yang `/sbin/launchd.ios`-nya berperilaku seperti `stage0`, bukan Apple
launchd 525088 B — mengakibatkan loop `stage0 → init → exec stage0`. Tidak ada perubahan kode
kernel/userland; fix = tulis `userland/system.apfs` baru (604423 blocks) ke `xnusys`.

## Kronologi

1. **Verifikasi QEMU** (`logs/qemu-repro-20261122-1250.log`): QEMU 11.1.3 (pacman pkg, ekstrak
   manual ke `/tmp/opencode/qemu-root`, host tidak punya qemu). Boot penuh: `apfs: mount
   "/dev/md1"` 1x, `exec: /sbin/launchd.ios exists, size 525088`, launchd → containermanagerd[11]
   → backboardd[10] → SpringBoard[7] (Successfully spawned 17.8 s), 26+ `[ ok ]` init self-test.
   Catatan: `!SpringBoard hang` (44 s, 102 s) menunggu `_BKSDisplayGetMainScreenInfo`;
   `MiniIOMobileFramebuffer selector 15/57 not implemented` — GUESS QEMU-only.
2. **xnusys**: `./tools/system-flash.sh` — push 2.4 GB (38.6 MB/s), dd 590+1 records 30.773 s,
   verifikasi `NXSB` offset 32 OK. Image sha256 `1ca19b2b564e18f13ce351ca5afe361161c873bdddcb39e49897c1353d219e64`.
3. **boot_a pertama**: `xnu-selene-boot_a-16KVM-NEWINIT.img` (sha256 `026a9748…`), chainboot,
   payload cmdline `debug=0x8 serial=3 rd=md0 emmc_root=1 -v cpus=1 loader_return_after=20`.
   Slot `a` ✓✓. Run 20 s: pstore `logs/xnusys-fixed-20261122-1412-pmsg.log` — `mount "/dev/md1"`
   **1x**, SpringBoard[7] spawned 17.8 s, tanpa panic. Loop mati — FIX CONFIRMED di hardware.
4. **QEMU CPU4+180 s** tidak dijalankan (langsung hardware, XNU payload identik).
5. **IOS180-CPU4** (`xnu-selene-boot_a-IOS180-CPU4-20261122-1422.img`, sha256 `e8c57eef…`,
   `cpus=4 loader_return_after=180`): homescreen iOS terlihat user (foto), auto-unlock jalan.
   Pstore belum tertangkap (USB hub drop: HP + OPPO hilang dari bus bersamaan; `lsusb` kosong).
6. **Penemuan build pipeline**:
   - `build-payload.bin` repo = stale (Nov 15) — berisi kernel `d9fdf56c` bukan `bbcf6915`.
     Rebuild: `mkpayload.py kernel=xnu-src/.../mach.development.bcm2837 ramdisk=userland/root.apfs`
     → kernel `bbcf6915` (15082000 B, sama dengan NEWINIT/QEMU), ramdisk `6492107d` (root.apfs).
   - `prebuilt/mach.development.bcm2837` (19:28) satu build lebih tua dari
     `xnu-src/.../BUILD/obj/...` (19:49) — payload HARUS dari xnu-src BUILD obj.
   - `mtkloader.bin` repo (Nov 20) dibangun dari source `fd68492` (`serial=3`); HEAD `750bcc5`
     default `serial=0`. `MENU_DEFAULT_XNU` default **1** di kedua versi — image tanpa
     `-DMENU_DEFAULT_XNU=0` boot XNU SETIAP power-on.
   - `backup/boot_a.img` (boot Android rooted) **hilang** → semua build 14:21+ jatuh ke
     `../hakios-selene/fw-selene/boot_a.img` = kernel stock 28204391 B → chain Android
     (`chain_android()`, mtkloader.c:1026) gagal → reset loop → LK/user mendarat di fastboot.
     Inilah sebab "mental fastboot".
7. **Fix chain Android**: kernel rooted Bakasu diekstrak dari NEWINIT blob
   (`img[0x40000:+33978271]`) → `backup/boot_a.img` direkreasi (33980319 B). User memberi
   `/home/naidra/Projects/Kernel/boot-images/vendor-s-4.14/boot_bakasu_lto_20261121.img` —
   kernel gzip multi-member: 33841232 + unused_data 137039 = **33978271 — identik**.
   chainboot.py menangani ini (`dc.unused_data` di-append, tools/chainboot.py:120-124).
8. **NORET-BAKASU** (`xnu-selene-boot_a-NORET-BAKASU-20261122-1502.img`, sha256 `6cb82707…`,
   `MENU_DEFAULT_XNU=0`, `loader_return_after=0`, `mali=1 touchscreen=3 buttons=2 fbshot=20`):
   flashed slot `a` ✓✓ → **Android bootloop logo Redmi**. Image Android identik dengan run
   14:13 yang sukses; mtkloader HEAD (750bcc5, 509 insertions vs fd68492) = tersangka utama
   chain rusak. UNVERIFIED — belum ada log.
9. **Jalur iOS proven**: `xnu-selene-boot_a-XNUONLY-GPU-20261122-1444.img` (sha256
   `4a3a10e5…`, `MENU_DEFAULT_XNU=1`, `loader_return_after=0`, mali+touch+fbshot) — homescreen
   tembus di build ini. Bootloop Android tidak pernah kena di jalur MENU=1 karena setelah XNU
   kembali mtkloader HALT (wfe, mtkloader.c:1583) dan boot berikutnya = XNU lagi.

## Known-good buat dibekukan

- `userland/system.apfs` 604423 blocks — `1ca19b2b…219e64` (di xnusys, NXSB verified)
- `xnu-selene-boot_a-XNUONLY-GPU-20261122-1444.img` — `4a3a10e5…a96bbc` (homescreen)
- payload build-payload.bin: kernel `bbcf6915…` + ramdisk `6492107d…`
- `backup/boot_a.img` — rooted Android Image 33978271 B (= Bakasu; chain Android terbukti 14:13)

## Terbukti di hardware

- 4 core (`cpus=4`) naik tanpa hang sampai SpringBoard (fenomena layar; log pstore UNVERIFIED)
- `mali=1` tidak hang; GPU init lambat pada boot pertama ("jumping to XNU" lama), tetap tembus
- `touchscreen=3` + `buttons=2` jalan: auto-unlock swipe-up sukses
- Display 1080×2400 tanpa DSI kick tambahan; tidak ada teks/logo nyangkut

## Terbuka / lanjutan

- Pstore run homescreen belum tertangkap (butuh Android hidup; bootloop menghalangi).
  Zona ramoops konsol kernel XNU ditulis live — kalau Android hidup, log ada.
- Bootloop chain Android: tersangka mtkloader HEAD chain path — tes dengan mtkloader
  fd68492-era diekstrak dari NEWINIT blob (img[0:0x40000]) + Image Bakasu + payload sekarang.
- Battery (`mtkbatt=1`, `bq2589x=1`), GPU depth (`metal=1`, `fbscale=3`), connsys/scp:
  urutan berikutnya, 1 boot-arg = 1 eksperimen, setelah Android chain kembali hidup.
- QEMU: `!SpringBoard hang` `_BKSDisplayGetMainScreenInfo` — cek apakah juga muncul di
  hardware (MiniIOMobileFramebuffer selector 15/57 not implemented).

## Verbose vs non-verbose (layar & bukti)

Keduanya menulis log yang sama secara internal (`loader_log`, ramoops saat return); bedanya
hanya apakah log digambar ke framebuffer secara live (`-v`, dari `-DXNU_VERBOSE=1`).

| | Non-verbose (`XNUONLY-GPU-1444`, dsb.) | Verbose (`VERBOSE-STOCK-1551`) |
| :--- | :--- | :--- |
| Layar saat boot | Boot animation Apple normal, lalu UI | Console kernel live **menimpa** boot animation |
| Logo Redmi/bootanim | Terlihat | Tertimpa teks sejak mtkloader menggambar |
| Progres boot | Hanya dari mata (hitam → homescreen) | Terlihat per baris (`launchd` → `backboardd` → `SpringBoard`) |
| Diagnosis hang | Butuh pstore (butuh Android hidup) | Foto ekor console = bukti langsung |
| Rumor "GPU gagal/bootanim hilang" | — | Bukan gagal: framebuffer dipakai log; UI tetap menggambar di bawah/di atasnya |

Pengalaman sesi:

- `VERBOSE-LIVE-1548` (chain Bakasu): bootloop logo Redmi — konsisten dengan pola image
  Bakasu, bukan efek verbose.
- `VERBOSE-STOCK-1551` (chain stock): console jalan panjang tanpa panic; warning
  `persona_find_by_type` non-fatal; boot sampai fase SpringBoard. Boot animation memang
  tidak terlihat karena ketimpa — bukan gejala GPU.

Aturan pakai: eksperimen/perbaikan = verbose (kelihatan di mana berhenti); demonstrasi pakai =
non-verbose (boot anim jalan, homescreen bersih).

## Status akhir sesi

- Upaya rescue pstore via Android gagal semua (kernel stock di chain 14:21+, lalu bootloop
  Bakasu di 15:02) — `logs/ios180-cpu4-rescue*/` kosong, dihapus dari tree.
- Keputusan user: jalur iOS-only. `xnu-selene-boot_a-XNUONLY-GPU-20261122-1444.img`
  di-flash ulang ke `boot_a` (slot `a` ✓✓) di akhir sesi — HP berjalan iOS per power-on;
  `xnusys` tidak disentuh ulang.
- Estimasi power-on → homescreen ~2–3 menit (GPU Mali init boot lambat) — GUESS, tidak
  terukur; angka presisi butuh pstore (chain Android hidup).
- Kernel `bbcf6915` + redwood.patch (16 KiB page shift BCM2837) di-commit ke `mtk-selene`.

## File log sesi

- `logs/qemu-repro-20261122-1250.log` — QEMU boot penuh
- `logs/xnusys-fixed-20261122-1412.log` / `-pmsg.log` / `-c2.log` — run 20 s pertama (loop mati)
- `logs/ios180-cpu4-rescue*/` — upaya rescue pstore (semua 0 byte, Android tidak pernah hidup
  via USB sejak 14:21)
