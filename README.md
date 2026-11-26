# dirroof

```
   ^
  o o
 r   f
/     \
| dir |
```

`dirroof` lets you set a size limit for a directory and evicts the least
recently accessed files or subdirectories to stay within it.
It is intended to be run as a daemon to give a maximum size to a directory.

The default mode uses `fanotify` (`FAN_REPORT_FID | FAN_REPORT_DFID_NAME`)
and requires Linux 5.9+ (unprivileged use requires Linux 5.13+).
Recursive mode uses `inotify`.

## Usage

```
dirroof -s SIZE [-v] [-f] [-r] [--evict-depth DEPTH | --prune-empty] DIR
dirroof --version
```

- `-s/--size SIZE` (required): the maximum total size of the tracked
  regular files. Suffixes K, M, G, T are accepted
  (e.g. `-s 2G`).
- `-v/--verbose`: log tracked/deleted files and events.
- `-f/--delete-open`: also delete files that are currently open (last
  resort). By default a file that dirroof observes as open protects its
  entire eviction unit.
- `-r/--recursive`: count regular files at every depth and evict them
  individually. Empty directories are retained by default.
- `--evict-depth DEPTH`: enable recursive mode and evict each directory
  at that depth as a unit. The watched root is at depth 0 and its direct
  subdirectories are at depth 1. Files outside those units remain
  individually evictable.
- `--prune-empty`: enable recursive mode and remove empty ancestors
  after evicting a file. The watched root is kept. This option is
  mutually exclusive with `--evict-depth`.
- `-V/--version`: print the program version and exit.

For workloads with one directory per job, the following command groups
each job's outputs for eviction and limits the entire tree to 2 GiB.

```sh
dirroof -s 2G --evict-depth 1 "$RESULTS_DIR"
```

## How it works

In recursive mode, dirroof scans the tree and installs an inotify watch
on each directory. It uses events to update file sizes, add new entries,
and unregister subtrees that leave the tree. It rescans after an event
queue overflow. This mode needs access to `/proc/self/fd` and is subject to
the system's inotify watch limit.

Eviction units sort by open count and then by last access time. For a
directory unit, dirroof sums the open counts and uses the newest access
time of any regular file in the unit. Only regular files count toward
the size limit. Directory eviction also unlinks symlinks and other
entries in the unit, without following symlinks.

Without recursive mode, dirroof tracks up to 4096 regular files directly
inside `DIR`.

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

Both modes track open counts from filesystem events, so counts are
approximate. For example, files already open when tracking begins may
be counted as closed. Records are per path, and hard links can be
counted more than once.

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
