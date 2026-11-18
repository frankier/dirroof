dirroof(1)

# NAME

dirroof - limit the total size of files in a directory

# SYNOPSIS

*dirroof* *-s* _SIZE_ [*-v*] [*-f*] _DIR_

*dirroof* *--version*

# DESCRIPTION

*dirroof* watches _DIR_ and deletes files to keep the total size of regular
files directly inside it at or below _SIZE_. It initially scans the directory
and then monitors it using Linux fanotify. Subdirectories and symbolic links
are ignored.

Deletion candidates are ordered by open count and then by last access time.
Open files are normally retained, even if this leaves the directory above the
limit. File open counts are approximate because fanotify may merge events.
Hard links may be counted more than once.

# OPTIONS

*-s*, *--size* _SIZE_
	Set the required, nonzero limit in bytes. Case-insensitive K, M, G and T
	suffixes multiply by powers of 1024; an optional B may follow the suffix.

*-v*, *--verbose*
	Log tracked files, deletions and events.

*-f*, *--delete-open*
	Allow deletion of open files as a last resort. Their space is not freed
	until the last open file descriptor is closed.

*--help*
	Show help.

*--usage*
	Show a short usage message.

*-V*, *--version*
	Show the version.

# NOTES

Requires Linux 5.9 or newer; unprivileged fanotify use requires Linux 5.13
or newer. Fanotify and filesystem support and permissions also apply. At most
4096 files are tracked; additional files are ignored.
