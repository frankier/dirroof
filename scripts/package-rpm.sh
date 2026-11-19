#!/bin/sh
# Usage: package-rpm.sh BINARY MANPAGE VERSION OUTPUT_DIR [PACKAGE [LICENSES]]
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
case "$version" in ''|*[!0-9A-Za-z.+~]*) echo "Invalid RPM version: $version" >&2; exit 2;; esac
work=$(mktemp -d "${TMPDIR:-$HOME/.scratch}/dirroof-rpm.XXXXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/BUILD" "$work/BUILDROOT" "$work/RPMS" "$work/SOURCES" "$work/SPECS" "$work/SRPMS"
cp "$binary" "$work/SOURCES/dirroof"
cp "$manpage" "$work/SOURCES/dirroof.1"
if [ -n "$licenses" ]; then
    cp "$licenses" "$work/SOURCES/LICENSES"
fi
cat > "$work/SPECS/dirroof.spec" <<'SPEC'
Name: %{pkgname}
Version: %{pkgversion}
Release: 1
Summary: Limit the total size of files in a directory
License: Unspecified
URL: https://github.com/frankier/dirroof
AutoReqProv: no
%if "%{pkgname}" == "dirroof-filc"
Conflicts: dirroof
%endif
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
%if %{withlicenses}
install -D -m 644 %{_sourcedir}/LICENSES %{buildroot}%{_licensedir}/%{name}/LICENSES
%endif
%files
%{_bindir}/dirroof
%{_mandir}/man1/dirroof.1*
%if %{withlicenses}
%license %{_licensedir}/%{name}/LICENSES
%endif
SPEC
withlicenses=0
if [ -n "$licenses" ]; then withlicenses=1; fi
rpmbuild -bb --define "_topdir $work" --define "pkgversion $version" \
    --define "pkgname $package" --define "withlicenses $withlicenses" "$work/SPECS/dirroof.spec"
find "$work/RPMS" -name '*.rpm' -exec cp -t "$out" {} +
