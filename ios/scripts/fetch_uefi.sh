#!/bin/zsh
# The UEFI firmware Windows machines start from: EDK2's ArmVirtQemu, as QEMU ships it (edk2-aarch64-code.fd;
# BSD-2-Clause-Patent, with the licenses of what it is built with in QEMU's pc-bios/edk2-licenses.txt).
# Homebrew's QEMU has it; otherwise it comes from QEMU's own repository, at the tag below. Gzipped to
# ios/deps/uefi/edk2-aarch64-code.fd.gz (not in git): 64 MB of mostly padding, under 2 MB packed. The app
# carries that file (project.yml, "Bundle the UEFI firmware") and unpacks it on the device.
set -e
OUT="$(cd "$(dirname "$0")/.." && pwd)/deps/uefi"
QEMU_TAG=v11.1.0
mkdir -p "$OUT"
SRC=""
for dir in /opt/homebrew/share/qemu /usr/local/share/qemu; do
    [ -f "$dir/edk2-aarch64-code.fd" ] && SRC="$dir/edk2-aarch64-code.fd" && break
done
if [ -n "$SRC" ]; then
    gzip -9c "$SRC" > "$OUT/edk2-aarch64-code.fd.gz"
else
    WORK=$(mktemp -d)
    trap 'rm -rf "$WORK"' EXIT
    curl -fL --retry 3 -o "$WORK/code.fd.bz2" "https://gitlab.com/qemu-project/qemu/-/raw/$QEMU_TAG/pc-bios/edk2-aarch64-code.fd.bz2"
    bunzip2 -c "$WORK/code.fd.bz2" | gzip -9c > "$OUT/edk2-aarch64-code.fd.gz"
fi
ls -l "$OUT/edk2-aarch64-code.fd.gz"
