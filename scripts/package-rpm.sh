#!/bin/sh
# Usage: package-rpm.sh BINARY MANPAGE VERSION OUTPUT_DIR
set -eu
[ "$#" -eq 4 ] || { echo "Usage: $0 BINARY MANPAGE VERSION OUTPUT_DIR" >&2; exit 2; }
binary=$(realpath "$1")
manpage=$(realpath "$2")
version=$3
out=$(realpath "$4")
case "$version" in ''|*[!0-9A-Za-z.+~]*) echo "Invalid RPM version: $version" >&2; exit 2;; esac
work=$(mktemp -d "${TMPDIR:-$HOME/.scratch}/dirroof-rpm.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/BUILD" "$work/BUILDROOT" "$work/RPMS" "$work/SOURCES" "$work/SPECS" "$work/SRPMS"
cp "$binary" "$work/SOURCES/dirroof"
cp "$manpage" "$work/SOURCES/dirroof.1"
cat > "$work/SPECS/dirroof.spec" <<'SPEC'
Name: dirroof
Version: %{pkgversion}
Release: 1
Summary: Limit the total size of files in a directory
License: Unspecified
URL: https://github.com/frankier/dirroof
AutoReqProv: no
%global __brp_compress %{nil}
%global __brp_strip %{nil}
%global __brp_strip_static_archive %{nil}
%global __brp_strip_comment_note %{nil}
%description
Watches a directory and removes older files when its size exceeds a limit.
%install
install -D -m 755 %{_sourcedir}/dirroof %{buildroot}%{_bindir}/dirroof
mkdir -p %{buildroot}%{_mandir}/man1
gzip -n -9 -c %{_sourcedir}/dirroof.1 > %{buildroot}%{_mandir}/man1/dirroof.1.gz
%files
%{_bindir}/dirroof
%{_mandir}/man1/dirroof.1*
SPEC
rpmbuild -bb --define "_topdir $work" --define "pkgversion $version" "$work/SPECS/dirroof.spec"
find "$work/RPMS" -name '*.rpm' -exec cp -t "$out" {} +
