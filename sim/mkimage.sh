#!/bin/sh
# mkimage.sh -- a disk image for the simulator from a folder: what the device's internal storage
# holds (the user volume, FAT32) with the folder's contents copied onto it.
#
#   sim/mkimage.sh MUSIC_DIR internal.img [SIZE_MB]
#
# SIZE_MB defaults to the folder's size plus 64 MB (room for the library index, playlists, logs).
# The volume starts 1 MiB in (LBA 2048), where emmc_layout() puts the user volume in the simulator.
# The same image works with --sd as a microSD card.
# Needs mtools and dosfstools (sudo apt install mtools dosfstools).
set -eu

[ $# -ge 2 ] || { echo "usage: $0 MUSIC_DIR internal.img [SIZE_MB]" >&2; exit 2; }
src=$1
img=$2
[ -d "$src" ] || { echo "$src: not a folder" >&2; exit 1; }
command -v mkfs.fat >/dev/null || { echo "mkfs.fat not found: sudo apt install dosfstools" >&2; exit 1; }
command -v mcopy >/dev/null || { echo "mcopy not found: sudo apt install mtools" >&2; exit 1; }

used_mb=$(du -sm "$src" | cut -f1)
size_mb=${3:-$((used_mb + 64))}
[ "$size_mb" -ge 64 ] || size_mb=64

case $img in /*) ;; *) img=$(pwd)/$img ;; esac
rm -f "$img"
truncate -s "$((size_mb + 1))M" "$img"
# A partition table too, as on a real card: then the same image works as the internal storage
# (the firmware reads the volume at LBA 2048 directly) or as a microSD card (--sd: found via the MBR).
le32() {  # a number as 4 little-endian bytes (printf octal escapes: POSIX sh)
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($1 & 255)) $(($1 >> 8 & 255)) $(($1 >> 16 & 255)) $(($1 >> 24 & 255)))"
}
sectors=$((size_mb * 2048))
{ printf '\000\000\002\000\014\376\377\377'; le32 2048; le32 "$sectors"; } |
    dd of="$img" bs=1 seek=446 conv=notrunc status=none
printf '\125\252' | dd of="$img" bs=1 seek=510 conv=notrunc status=none
mkfs.fat -F 32 -n ECHOMINI --offset 2048 "$img" >/dev/null
# Copy the folder's contents (not the folder itself) to the volume's root.
for f in "$src"/* "$src"/.[!.]*; do
    [ -e "$f" ] || continue
    mcopy -s -Q -i "$img@@1M" "$f" ::/
done
echo "$img: ${size_mb} MB volume, $(find "$src" -type f | wc -l) files copied"
