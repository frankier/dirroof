# Build on an old, widely supported glibc baseline. The output has no dynamic
# linker or shared-library dependencies; the application still requires Linux
# fanotify support as described in the manual.
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential scdoc rpm ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY dirroof.c dirroof.1.scd ./
COPY scripts/ scripts/
ARG VERSION=0.1
RUN case "$VERSION" in ''|*[!0-9A-Za-z.+~_-]*) exit 1;; esac && \
    mkdir -p /out /root/.scratch && \
    cc -O2 -static -DPROJECT_VERSION=\"$VERSION\" -o /out/dirroof dirroof.c && \
    scdoc < dirroof.1.scd > /out/dirroof.1 && \
    scripts/package-deb.sh /out/dirroof /out/dirroof.1 "$VERSION" /out && \
    scripts/package-rpm.sh /out/dirroof /out/dirroof.1 "$VERSION" /out

FROM scratch AS artifacts
COPY --from=build /out/ /
