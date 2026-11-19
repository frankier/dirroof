# dirroof

```
   ^
  o o
 r   f
/     \
| dir |
```

`dirroof` allows you to specify a maximum size of a directory and deletes the oldest files in order to keep within this size.
It is intended to be run as a daemon to give a maximum size to a directory.

It uses `fanotify` (`FAN_REPORT_FID | FAN_REPORT_DFID_NAME`) and requires
Linux 5.9+ (unprivileged use requires Linux 5.13+).

## Usage

```
dirroof -s SIZE [-v] [-f] DIR
dirroof --version
```

- `-s/--size SIZE` (required): the roofline — maximum total size of the
  regular files directly inside `DIR`. Suffixes K, M, G, T are accepted
  (e.g. `-s 2G`).
- `-v/--verbose`: log tracked/deleted files and events.
- `-f/--delete-open`: also delete files that are currently open (last
  resort). By default open files are never unlinked.
- `-V/--version`: print the program version and exit.

## How it works

- Two fanotify groups are watched with epoll: one on `DIR` itself
  (`FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO`), and one with
  a mark on each file (`FAN_OPEN | FAN_ACCESS | FAN_MODIFY |
  FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE`), added when the file appears.
- Each file is kept in a record holding its size, last-write time, last
  access time and current open count. All records live in a min-heap
  ordered by `(open_count, last_access)`, so the best deletion candidate is
  always at the root.
- After every fanotify event the roofline is enforced: records are popped
  from the heap and unlinked until the total size is at or below the limit.
  Files that are currently open sort last and are only deleted with `-f`
  (unlinking an open file frees no space until the last close anyway).
- The directory is enumerated with `readdir` only once at startup (to seed
  the records) and again after a fanotify queue overflow; during normal
  operation everything is event-driven and sizes/times are refreshed from
  the per-file events plus `stat()`.

Caveats: fanotify merges same-inode events, so open counts are approximate;
records are per path, so hard links may be mis-accounted; subdirectories
and symlinks are ignored.

## Static builds and packages

Run `make` to compile a local executable with the system C compiler.

Build the static Linux executable, generated manual, Debian package and RPM
with Docker (BuildKit):

```sh
docker build --output type=local,dest=dist .
```

Build the Fil-C variant with musl on an x86-64 or ARM64 Docker host:

```sh
docker build --target filc-artifacts --output type=local,dest=dist-filc .
```

The Fil-C target uses a pinned [Fil-C toolchain](https://fil-c.org/install_pizfix)
with a small runtime patch that enables fanotify on musl. It exports
`dirroof-filc` with its license notices, manual, Debian package and RPM.
Both executables are statically linked and require the kernel,
filesystem and fanotify permissions described above. The `dirroof` and
`dirroof-filc` packages both install `/usr/bin/dirroof` and are mutually
exclusive.

The packaging scripts also accept an existing binary and generated man page:

```sh
scdoc < dirroof.1.scd > dirroof.1
mkdir -p dist
scripts/package-deb.sh ./dirroof ./dirroof.1 "$(cat VERSION)" dist
scripts/package-rpm.sh ./dirroof ./dirroof.1 "$(cat VERSION)" dist
```

They require `dpkg-deb` or `rpmbuild` respectively and use the build host's
architecture. Supply a binary built for that architecture.

To release, update and commit `VERSION`, then create and push the matching
tag with `git tag "v$(cat VERSION)"`. GitHub Actions checks that the tag
matches the version in the tagged commit before publishing the x86-64 static
binaries and packages for both variants. You can also run the workflow
manually for an existing tag whose commit contains the matching `VERSION`.
