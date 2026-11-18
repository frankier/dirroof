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
Linux 5.9+ and glibc (unprivileged use requires Linux 5.13+).

## Usage

```
dirroof -s SIZE [-v] [-f] DIR
```

- `-s/--size SIZE` (required): the roofline — maximum total size of the
  regular files directly inside `DIR`. Suffixes K, M, G, T are accepted
  (e.g. `-s 2G`).
- `-v/--verbose`: log tracked/deleted files and events.
- `-f/--delete-open`: also delete files that are currently open (last
  resort). By default open files are never unlinked.

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

Build the static Linux executable, generated manual, Debian package and RPM
with Docker (BuildKit):

```sh
docker build --build-arg VERSION=0.1 --output type=local,dest=dist .
```

The image builds for the Docker host architecture. The resulting executable
has no shared-library dependencies but still needs the kernel, filesystem and
fanotify permissions described above. The packaging scripts also accept an
existing binary and generated man page:

```sh
scdoc < dirroof.1.scd > dirroof.1
mkdir -p dist
scripts/package-deb.sh ./dirroof ./dirroof.1 0.1 dist
scripts/package-rpm.sh ./dirroof ./dirroof.1 0.1 dist
```

They require `dpkg-deb` or `rpmbuild` respectively and use the build host's
architecture; supply a binary built for that architecture. Tagging a commit
`v` followed by a version publishes the x86-64 static binary and both packages
as GitHub release assets. The workflow can also be run for an existing tag.
