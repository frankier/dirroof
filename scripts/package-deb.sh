#!/bin/sh
# Usage: package-deb.sh BINARY MANPAGE VERSION OUTPUT_DIR [PACKAGE [LICENSES]]
set -eu
[ "$#" -ge 4 ] && [ "$#" -le 6 ] || {
    echo "Usage: $0 BINARY MANPAGE VERSION OUTPUT_DIR [PACKAGE [LICENSES]]" >&2
    exit 2
}
binary=$(realpath "$1")
manpage=$(realpath "$2")
version=$3
out=$(realpath "$4")
package=${5:-dirroof}
licenses=${6:-}
case "$package" in
    dirroof) ;;
    dirroof-filc)
        [ -n "$licenses" ] || { echo "Fil-C requires license notices" >&2; exit 2; }
        ;;
    *) echo "Unknown package: $package" >&2; exit 2 ;;
esac
case "$version" in ''|*[!0-9A-Za-z.+~]*) echo "Invalid Debian version: $version" >&2; exit 2;; esac
arch=$(dpkg --print-architecture)
work=$(mktemp -d "${TMPDIR:-$HOME/.scratch}/dirroof-deb.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/DEBIAN" "$work/usr/bin" "$work/usr/share/man/man1"
install -m 755 "$binary" "$work/usr/bin/dirroof"
gzip -n -9 -c "$manpage" > "$work/usr/share/man/man1/dirroof.1.gz"
cat > "$work/DEBIAN/control" <<CONTROL
Package: $package
Version: $version
Architecture: $arch
Maintainer: dirroof contributors <noreply@github.com>
Homepage: https://github.com/frankier/dirroof
Section: utils
Priority: optional
Description: Limit the total size of files in a directory
 Watches a directory and removes older files when its size exceeds a limit.
CONTROL
if [ "$package" = dirroof-filc ]; then
    echo 'Conflicts: dirroof' >> "$work/DEBIAN/control"
fi
if [ -n "$licenses" ]; then
    install -D -m 644 "$licenses" "$work/usr/share/doc/$package/copyright"
fi
dpkg-deb --build --root-owner-group "$work" "$out/${package}_${version}_${arch}.deb"
