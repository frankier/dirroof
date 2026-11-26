dirroof(1)

# NAME

dirroof - limit the total size of files in a directory

# SYNOPSIS

*dirroof* *-s* _SIZE_ [*-v*] [*-f*] [*-r*] [*--evict-depth* _DEPTH_ | *--prune-empty*] _DIR_

*dirroof* *--version*

# DESCRIPTION

*dirroof* watches _DIR_ and evicts entries to keep the total size of tracked
regular files at or below _SIZE_. By default it tracks files directly inside
_DIR_ using Linux fanotify. Recursive mode uses inotify to track files at
every depth. Symbolic links are never followed.

Deletion candidates are ordered by open count and then by last access time.
Open files normally protect their entire eviction unit, even if this leaves
the tree above the limit. A directory unit uses the sum of its files' open
counts and its newest file access time. Open counts are approximate and depend
on observed events. Hard links may be counted more than once.

# OPTIONS

*-s*, *--size* _SIZE_
	Set the required, nonzero limit in bytes. Case-insensitive K, M, G and T
	suffixes multiply by powers of 1024; an optional B may follow the suffix.

*-v*, *--verbose*
	Log tracked files, deletions and events.

*-f*, *--delete-open*
	Allow deletion of open files as a last resort. Their space is not freed
	until the last open file descriptor is closed.

*-r*, *--recursive*
	Track regular files at every depth and evict individual files.
	Leave empty directories in place by default.

*--evict-depth* _DEPTH_
	Enable recursive mode and evict directories at _DEPTH_ together with
	all their contents. The watched root is at depth 0 and direct
	subdirectories are at depth 1. Files outside those units can be
	evicted individually. _DEPTH_ must be a positive integer.

*--prune-empty*
	Enable recursive mode and remove directories that become empty after
	file eviction, keeping the watched root. Cannot be combined with
	*--evict-depth*.

*--help*
	Show help.

*--usage*
	Show a short usage message.

*-V*, *--version*
	Show the version.

# NOTES

The default mode requires Linux 5.9 or newer. Unprivileged fanotify use
requires Linux 5.13 or newer. At most 4096 files are tracked in this mode.
Recursive mode needs one inotify watch per tracked directory and access to
_/proc/self/fd_. Filesystem support and permissions apply to both modes.
