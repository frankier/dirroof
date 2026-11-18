#!/bin/sh
# Usage: package-deb.sh BINARY MANPAGE VERSION OUTPUT_DIR
set -eu
[ "$#" -eq 4 ] || { echo "Usage: $0 BINARY MANPAGE VERSION OUTPUT_DIR" >&2; exit 2; }
binary=$(realpath "$1")
manpage=$(realpath "$2")
version=$3
out=$(realpath "$4")
case "$version" in ''|*[!0-9A-Za-z.+~]*) echo "Invalid Debian version: $version" >&2; exit 2;; esac
arch=$(dpkg --print-architecture)
work=$(mktemp -d "${TMPDIR:-$HOME/.scratch}/dirroof-deb.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/DEBIAN" "$work/usr/bin" "$work/usr/share/man/man1"
install -m 755 "$binary" "$work/usr/bin/dirroof"
gzip -n -9 -c "$manpage" > "$work/usr/share/man/man1/dirroof.1.gz"
cat > "$work/DEBIAN/control" <<CONTROL
Package: dirroof
Version: $version
Architecture: $arch
Maintainer: dirroof contributors <noreply@github.com>
Homepage: https://github.com/frankier/dirroof
Section: utils
Priority: optional
Description: Limit the total size of files in a directory
 Watches a directory and removes older files when its size exceeds a limit.
CONTROL
dpkg-deb --build --root-owner-group "$work" "$out/dirroof_${version}_${arch}.deb"
