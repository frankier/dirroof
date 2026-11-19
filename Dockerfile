# Build on an old, widely supported glibc baseline. The output has no dynamic
# linker or shared-library dependencies; the application still requires Linux
# fanotify support as described in the manual.
FROM debian:bookworm-slim AS package-tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential scdoc rpm ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY dirroof.1.scd VERSION ./
COPY scripts/ scripts/
RUN mkdir -p /out /root/.scratch && scdoc < dirroof.1.scd > /out/dirroof.1

FROM package-tools AS build
COPY dirroof.c Makefile ./
RUN version=$(cat VERSION) && \
    make CFLAGS=-O2 LDFLAGS=-static && mv dirroof /out/dirroof && \
    scripts/package-deb.sh /out/dirroof /out/dirroof.1 "$version" /out && \
    scripts/package-rpm.sh /out/dirroof /out/dirroof.1 "$version" /out

FROM debian:trixie-slim AS filc
RUN apt-get update && apt-get install -y --no-install-recommends \
    binutils ca-certificates clang curl git linux-libc-dev make patch patchelf ruby xz-utils \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /opt/filc
# Rebuild only the native runtime object that implements the checked
# fanotify calls. Keep the rest of the release toolchain and sysroot.
COPY scripts/filc-musl-fanotify.patch /opt/filc-musl-fanotify.patch
RUN set -eu; \
    version=0.687; \
    case "$(dpkg --print-architecture)" in \
        amd64) arch=x86_64; sha256=aab538401f78a47ab261ee79afed8c6b99e1d4848e263ae159a75b03c1288da1 ;; \
        arm64) arch=aarch64; sha256=f1337799051664f93c7712141deaebe78571ae96ffcf05563d517b89440bcbc0 ;; \
        *) echo "Fil-C supports amd64 and arm64" >&2; exit 1 ;; \
    esac; \
    archive="filc-$version-linux-$arch.tar.xz"; \
    curl --fail --silent --show-error --location --retry 3 -O \
        "https://github.com/pizlonator/fil-c/releases/download/v$version/$archive"; \
    echo "$sha256  $archive" | sha256sum -c -; \
    tar -xf "$archive" --strip-components=1; \
    rm "$archive"; \
    ./setup.sh; \
    git -c advice.detachedHead=false clone --depth 1 --filter=blob:none --sparse \
        --branch "v$version" https://github.com/pizlonator/fil-c.git /opt/filc-source; \
    test "$(git -C /opt/filc-source rev-parse HEAD)" = 2db72c646daa1acad2754bcb0e2f220f732e9789; \
    git -C /opt/filc-source sparse-checkout set libpas projects/yolomusl; \
    patch -d /opt/filc-source -p1 < /opt/filc-musl-fanotify.patch; \
    cd /opt/filc-source/projects/yolomusl; \
    CC=clang ./configure --prefix=/opt/filc-source/yolo; \
    make install-headers; \
    make -C /opt/filc-source/libpas FILC_OUTPUT_ROOT=/opt/filc/pizfix \
        FILC_YOLO_INCLUDE=/opt/filc-source/yolo/include FILC_CLANG=/opt/filc/build/bin/clang \
        build/pas-pizlo-release-filc_runtime.o; \
    ar r /opt/filc/pizfix/lib/libpizlo.a /opt/filc-source/libpas/build/pas-pizlo-release-filc_runtime.o; \
    cd /opt/filc; \
    rm -rf /opt/filc-source
WORKDIR /src
COPY dirroof.c Makefile VERSION ./
RUN version=$(cat VERSION) && \
    make CC=/opt/filc/build/bin/clang CFLAGS=-O2 LDFLAGS="-static -Wl,--build-id" && \
    mv dirroof /dirroof-filc && \
    strip --strip-debug /dirroof-filc && \
    readelf -lW -dW /dirroof-filc > /dirroof.elf && \
    ! grep -Eq '^[[:space:]]*INTERP|\(NEEDED\)' /dirroof.elf && \
    test "$(/dirroof-filc --version)" = "dirroof $version" && \
    /dirroof-filc --help >/dev/null && \
    for notice in MUSL PAS LLVM; do \
        printf '\n%s-LICENSE.txt\n\n' "$notice"; \
        cat "/opt/filc/$notice-LICENSE.txt" || exit 1; \
    done > /dirroof-filc-licenses.txt

FROM package-tools AS filc-packages
COPY --from=filc /dirroof-filc /filc-out/dirroof-filc
COPY --from=filc /dirroof-filc-licenses.txt /filc-out/
RUN version=$(cat VERSION) && \
    scripts/package-deb.sh /filc-out/dirroof-filc /out/dirroof.1 "$version" \
        /filc-out dirroof-filc /filc-out/dirroof-filc-licenses.txt && \
    scripts/package-rpm.sh /filc-out/dirroof-filc /out/dirroof.1 "$version" \
        /filc-out dirroof-filc /filc-out/dirroof-filc-licenses.txt && \
    cp /out/dirroof.1 /filc-out/

FROM scratch AS filc-artifacts
COPY --from=filc-packages /filc-out/ /

FROM scratch AS artifacts
COPY --from=build /out/ /
