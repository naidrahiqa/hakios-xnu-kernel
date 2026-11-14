#!/bin/bash
# Build mtkloader.bin. MMU off means device memory: no SIMD, strict alignment, no builtins.
# Extra arguments go to the compiler (e.g. -DXNU_WDT_SECONDS=0 or BOARD=eos / eos).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# an ELF linker: ld.lld of this machine, or zig's (tools/get-zig.sh; macOS has none of its own)
if [ -x "$ROOT/xnu-deps/host/zig/zig" ]; then LLD="$ROOT/xnu-deps/host/zig/zig ld.lld"
elif command -v ld.lld >/dev/null; then LLD=ld.lld
else echo "$(basename "$0"): no ld.lld: run tools/get-zig.sh" >&2; exit 1; fi
CFLAGS="--target=aarch64-linux-gnu -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -mgeneral-regs-only -mstrict-align -Os -Wall -Wextra"

BOARD_FLAG=""
EXTRA_ARGS=()
for arg in "$@"; do
	case "$arg" in
		selene|BOARD=selene|-DBOARD_SELENE) BOARD_FLAG="-DBOARD_SELENE" ;;
		eos|BOARD=eos|-DBOARD_EOS) BOARD_FLAG="-DBOARD_EOS" ;;
		x6826b|BOARD=x6826b|-DBOARD_X6826B) BOARD_FLAG="-DBOARD_X6826B" ;;
		*) EXTRA_ARGS+=("$arg") ;;
	esac
done

if [ -z "$BOARD_FLAG" ]; then
	BOARD="${BOARD:-selene}"
	case "$BOARD" in
		eos) BOARD_FLAG="-DBOARD_EOS" ;;
		x6826b) BOARD_FLAG="-DBOARD_X6826B" ;;
		selene|*) BOARD_FLAG="-DBOARD_SELENE" ;;
	esac
fi

clang $CFLAGS $BOARD_FLAG "${EXTRA_ARGS[@]}" -c "$HERE/start.S" -o "$HERE/start.o"
clang $CFLAGS $BOARD_FLAG "${EXTRA_ARGS[@]}" -c "$HERE/mtkloader.c" -o "$HERE/mtkloader.o"
$LLD -T "$HERE/link.ld" --oformat binary "$HERE/start.o" "$HERE/mtkloader.o" -o "$HERE/mtkloader.bin"
ls -la "$HERE/mtkloader.bin"
