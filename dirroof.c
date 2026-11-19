#define _GNU_SOURCE /* Needed to get O_LARGEFILE definition */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#ifdef __GLIBC__
#include <argp.h>
#else
#include <getopt.h>
#endif
#include <string.h>
#include <sys/epoll.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Fil-C's musl syscall dispatcher does not route these calls to the
   runtime. Use the checked runtime entry points directly. */
#if defined(__FILC__) && !defined(__GLIBC__)
#include <pizlonated_syscalls.h>
#define fanotify_init zsys_fanotify_init
#define fanotify_mark zsys_fanotify_mark
#define name_to_handle_at zsys_name_to_handle_at
#endif

/* Linux fanotify ABI values missing from some musl headers. */
#ifndef FAN_EVENT_INFO_TYPE_OLD_DFID_NAME
#define FAN_EVENT_INFO_TYPE_OLD_DFID_NAME 10
#endif
#ifndef FAN_EVENT_INFO_TYPE_NEW_DFID_NAME
#define FAN_EVENT_INFO_TYPE_NEW_DFID_NAME 12
#endif

#define MAXFILES 4096

/* Events watched on the directory itself (fanotify group 1): lifecycle
   of the entries, used to add/remove per-file marks in group 2. */
#define DIR_EVENTS                                                             \
    (FAN_CREATE | FAN_DELETE | FAN_MOVED_FROM | FAN_MOVED_TO                   \
     | FAN_EVENT_ON_CHILD)

/* Events watched on each individual file (fanotify group 2). */
#define FILE_EVENTS                                                            \
    (FAN_OPEN | FAN_ACCESS | FAN_MODIFY | FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE)

struct arguments {
    char *dir;
    unsigned long long roof;
    int verbose;
    int delete_open;
};

/* Return the diagnostic for a malformed size, or set the parsed size. */
static const char *
parse_size(const char *arg, unsigned long long *size)
{
    char *end;
    unsigned long long mult = 1;

    errno = 0;
    unsigned long long value = strtoull(arg, &end, 10);
    if (end == arg || errno != 0) return "invalid size:";

    if (*end != '\0') {
        switch (tolower((unsigned char)*end)) {
        case 'k':
            mult = 1ULL << 10;
            break;
        case 'm':
            mult = 1ULL << 20;
            break;
        case 'g':
            mult = 1ULL << 30;
            break;
        case 't':
            mult = 1ULL << 40;
            break;
        default:
            return "invalid size suffix in";
        }
        end++;
        if (*end == 'b' || *end == 'B') end++;
        if (*end != '\0') return "invalid size:";
    }
    if (value > ULLONG_MAX / mult) return "size too large:";

    *size = value * mult;
    return NULL;
}

#ifdef __GLIBC__
const char *argp_program_version = "dirroof " VERSION;

/* Program documentation. */
static char doc[] = "dirroof keeps a directory's size under a limit (roofline) "
                    "by removing older files";

/* A description of the arguments we accept. */
static char args_doc[] = "DIR";

/* The options we understand. */
static struct argp_option options[]
    = { { "verbose", 'v', 0, 0, "Produce verbose output" },
        { "size", 's', "SIZE", 0,
          "Maximum total size of DIR, e.g. 500M or 2G (required)" },
        { "delete-open", 'f', 0, 0,
          "Also delete files that are currently open (last resort)" },
        { 0 } };

/* Parse a single option. */
static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    /* Get the input argument from argp_parse, which we
       know is a pointer to our arguments structure. */
    struct arguments *arguments = state->input;

    switch (key) {
    case 'v':
        arguments->verbose = 1;
        break;

    case 'f':
        arguments->delete_open = 1;
        break;

    case 's': {
        const char *error = parse_size(arg, &arguments->roof);
        if (error != NULL) argp_error(state, "%s '%s'", error, arg);
        break;
    }

    case ARGP_KEY_ARG:
        if (state->arg_num >= 1) /* Too many arguments. */
            argp_usage(state);

        arguments->dir = arg;

        break;

    case ARGP_KEY_END:
        if (state->arg_num < 1) /* Not enough arguments. */
            argp_usage(state);
        if (arguments->roof == 0) /* No roofline given. */
            argp_error(state, "a maximum size is required, e.g. -s 2G");
        break;

    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static void
parse_arguments(int argc, char **argv, struct arguments *arguments)
{
    static struct argp argp = { options, parse_opt, args_doc, doc };
    argp_parse(&argp, argc, argv, 0, 0, arguments);
}
#else
static void
print_usage(FILE *stream)
{
    fputs("Usage: dirroof -s SIZE [-v] [-f] DIR\n", stream);
}

static void
parse_arguments(int argc, char **argv, struct arguments *arguments)
{
    enum { HELP = 256, USAGE };
    static const struct option options[] = {
        { "verbose", no_argument, NULL, 'v' },
        { "size", required_argument, NULL, 's' },
        { "delete-open", no_argument, NULL, 'f' },
        { "help", no_argument, NULL, HELP },
        { "usage", no_argument, NULL, USAGE },
        { "version", no_argument, NULL, 'V' },
        { 0 }
    };
    int option;

    /* Musl reports unknown long options as '?' with optopt set to zero.
       Its successful short options leave optopt untouched. */
    optopt = 0;
    while ((option = getopt_long(argc, argv, "vs:fV?", options, NULL)) != -1) {
        switch (option) {
        case 'v':
            arguments->verbose = 1;
            break;
        case 'f':
            arguments->delete_open = 1;
            break;
        case 's': {
            const char *error = parse_size(optarg, &arguments->roof);
            if (error != NULL) {
                fprintf(stderr, "dirroof: %s '%s'\n", error, optarg);
                exit(64);
            }
            break;
        }
        case 'V':
            puts("dirroof " VERSION);
            exit(EXIT_SUCCESS);
        case USAGE:
            print_usage(stdout);
            exit(EXIT_SUCCESS);
        case '?':
            if (optopt != 0 || strncmp(argv[optind - 1], "--", 2) == 0) {
                print_usage(stderr);
                exit(64);
            }
            /* fall through */
        case HELP:
            print_usage(stdout);
            puts("dirroof keeps a directory's size under a limit (roofline) "
                 "by removing older files\n\n"
                 "  -s, --size=SIZE    Maximum total size, e.g. 500M or 2G (required)\n"
                 "  -v, --verbose      Produce verbose output\n"
                 "  -f, --delete-open  Also delete files that are currently open\n"
                 "  -?, --help         Show help\n"
                 "      --usage        Show a short usage message\n"
                 "  -V, --version      Show the version");
            exit(EXIT_SUCCESS);
        }
    }

    if (argc - optind != 1) {
        print_usage(stderr);
        exit(64);
    }
    if (arguments->roof == 0) {
        fputs("dirroof: a maximum size is required, e.g. -s 2G\n", stderr);
        exit(64);
    }
    arguments->dir = argv[optind];
}
#endif

/* One tracked file.  The heap array below holds all live records, ordered
   as a min-heap by (open_count, last_access, path): the record at the root
   is the best candidate for deletion. */
struct record {
    char path[PATH_MAX];
    off_t size;
    time_t last_write;  /* last FAN_CLOSE_WRITE/FAN_MODIFY (seed: st_mtime) */
    time_t last_access; /* last FAN_ACCESS (seed: st_atime) */
    int open_count;     /* currently open file descriptors (approximate) */
    size_t heap_idx;    /* position of this record in the heap array */
};

static struct record heap[MAXFILES];
static size_t nheap;

static char watch_dir[PATH_MAX];
static unsigned long long roof;
static unsigned long long total_size;
static int verbose;
static int delete_open;

static int g1fd = -1; /* fanotify group for directory events */
static int g2fd = -1; /* fanotify group for per-file events */

/* File handle of watch_dir, used to recognize events for files directly
   inside watch_dir (events for subdirectories carry another handle). */
static char dir_handle_buf[sizeof(struct file_handle) + 128];
static struct file_handle *dir_handle = (struct file_handle *)dir_handle_buf;
static int dir_handle_valid;

/* ---------------- min-heap ---------------- */

/* Deletion order: fewest open descriptors first, then least recently
   accessed, then path for determinism. */
static int
cmp_record(const struct record *a, const struct record *b)
{
    if (a->open_count != b->open_count)
        return a->open_count < b->open_count ? -1 : 1;
    if (a->last_access != b->last_access)
        return a->last_access < b->last_access ? -1 : 1;
    return strcmp(a->path, b->path);
}

static void
swap_heap(size_t i, size_t j)
{
    struct record tmp = heap[i];
    heap[i] = heap[j];
    heap[j] = tmp;
    heap[i].heap_idx = i;
    heap[j].heap_idx = j;
}

static void
sift_up(size_t i)
{
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (cmp_record(&heap[i], &heap[parent]) >= 0) break;
        swap_heap(i, parent);
        i = parent;
    }
}

static void
sift_down(size_t i)
{
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, m = i;
        if (l < nheap && cmp_record(&heap[l], &heap[m]) < 0) m = l;
        if (r < nheap && cmp_record(&heap[r], &heap[m]) < 0) m = r;
        if (m == i) break;
        swap_heap(i, m);
        i = m;
    }
}

/* The record must already be in the heap at r->heap_idx. */
static void
heap_update(struct record *r)
{
    sift_up(r->heap_idx);
    sift_down(r->heap_idx);
}

static void
heap_remove(struct record *r)
{
    size_t i = r->heap_idx;
    if (i >= nheap) return;
    swap_heap(i, nheap - 1);
    nheap--;
    if (i < nheap) {
        sift_up(i);
        sift_down(i);
    }
}

/* ---------------- record table ---------------- */

static struct record *
find_record(const char *path)
{
    for (size_t i = 0; i < nheap; i++)
        if (strcmp(heap[i].path, path) == 0) return &heap[i];
    return NULL;
}

/* Start tracking 'path' (must be a regular file): add a record and a
   per-file mark in group 2. */
static struct record *
add_record(const char *path)
{
    struct stat st;

    if (nheap >= MAXFILES) {
        fprintf(stderr, "dirroof: more than %d files in %s; ignoring %s\n",
                MAXFILES, watch_dir, path);
        return NULL;
    }
    if (lstat(path, &st) == -1 || !S_ISREG(st.st_mode)) return NULL;

    struct record *r = &heap[nheap];
    memset(r, 0, sizeof(*r));
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->size = st.st_size;
    r->last_write = st.st_mtime;
    r->last_access = st.st_atime;
    r->open_count = 0;
    r->heap_idx = nheap;
    nheap++;
    total_size += (unsigned long long)st.st_size;
    sift_up(nheap - 1);

    if (fanotify_mark(g2fd, FAN_MARK_ADD, FILE_EVENTS, AT_FDCWD, path) == -1)
        perror("fanotify_mark(file)");

    if (verbose)
        printf("tracking %s (%lld bytes)\n", path, (long long)st.st_size);
    return r;
}

static void
remove_record(const char *path)
{
    struct record *r = find_record(path);
    if (r == NULL) return;
    fanotify_mark(g2fd, FAN_MARK_REMOVE, 0, AT_FDCWD, path);
    total_size -= (unsigned long long)r->size;
    if (verbose) printf("untracking %s\n", path);
    heap_remove(r);
}

/* Refresh r->size from disk; adjusts total_size.  Returns 0 if the file
   vanished and the record was removed (r must not be used afterwards). */
static int
refresh_size(struct record *r)
{
    struct stat st;

    if (stat(r->path, &st) == -1) {
        if (errno == ENOENT) remove_record(r->path);
        return errno != ENOENT;
    }
    total_size += (unsigned long long)st.st_size - (unsigned long long)r->size;
    r->size = st.st_size;
    return 1;
}

/* Reconcile the record table with the directory on disk.  This is the
   only place that reads the directory: once at startup to seed the table,
   and again after a fanotify queue overflow. */
static void
resync(void)
{
    struct dirent *de;
    DIR *dir = opendir(watch_dir);

    if (dir == NULL) {
        perror("opendir");
        return;
    }

    while ((de = readdir(dir)) != NULL) {
        char path[PATH_MAX];
        struct record *r;
        struct stat st;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(path, sizeof(path), "%s/%s", watch_dir, de->d_name);

        r = find_record(path);
        if (r != NULL) {
            if (stat(path, &st) == 0) {
                total_size += (unsigned long long)st.st_size
                              - (unsigned long long)r->size;
                r->size = st.st_size;
                if (st.st_mtime > r->last_write) r->last_write = st.st_mtime;
                if (st.st_atime > r->last_access) r->last_access = st.st_atime;
                heap_update(r);
            }
        } else {
            add_record(path);
        }
    }
    closedir(dir);

    /* Drop records whose files have vanished. */
    for (size_t i = 0; i < nheap;) {
        struct stat st;
        if (lstat(heap[i].path, &st) == -1 && errno == ENOENT) {
            fanotify_mark(g2fd, FAN_MARK_REMOVE, 0, AT_FDCWD, heap[i].path);
            total_size -= (unsigned long long)heap[i].size;
            heap_remove(&heap[i]); /* do not advance i */
        } else {
            i++;
        }
    }
}

/* ---------------- enforcement ---------------- */

/* Delete records from the root of the min-heap until the directory is at
   or below the roofline.  Files that are currently open sort last and are
   only deleted with --delete-open. */
static void
enforce(void)
{
    while (total_size > roof && nheap > 0) {
        struct record *r = &heap[0];

        if (r->open_count > 0 && !delete_open) {
            if (verbose)
                fprintf(stderr,
                        "dirroof: %llu bytes over the roof but all remaining "
                        "files are open; not deleting (use -f to override)\n",
                        total_size - roof);
            break;
        }
        if (unlink(r->path) == -1) {
            perror("unlink");
            break; /* avoid an infinite loop; retried on the next event */
        }
        if (verbose)
            printf("deleted %s (%lld bytes, open=%d; %llu of %llu bytes "
                   "used)\n",
                   r->path, (long long)r->size, r->open_count,
                   total_size - (unsigned long long)r->size, roof);
        total_size -= (unsigned long long)r->size;
        heap_remove(r);
    }
}

/* ---------------- fanotify events ---------------- */

/* Extract the path of the event target from the info record of type
   'info_type' (DFID_NAME, OLD_DFID_NAME or NEW_DFID_NAME).  Returns 0 if
   the record is absent or belongs to a different directory (e.g. a
   subdirectory of watch_dir). */
static int
event_path(const struct fanotify_event_metadata *md, unsigned int info_type,
           char *path, size_t pathlen)
{
    const unsigned char *info = (const unsigned char *)md + md->metadata_len;
    const unsigned char *end = (const unsigned char *)md + md->event_len;

    while (info < end) {
        const struct fanotify_event_info_header *h = (const void *)info;

        if (h->len == 0) break;
        if (h->info_type == info_type) {
            const struct fanotify_event_info_fid *fid = (const void *)h;
            const struct file_handle *fh = (const void *)fid->handle;
            const char *name;

            if (dir_handle_valid
                && (fh->handle_bytes != dir_handle->handle_bytes
                    || fh->handle_type != dir_handle->handle_type
                    || memcmp(fh->f_handle, dir_handle->f_handle,
                              fh->handle_bytes)
                           != 0))
                return 0;

            name = (const char *)fh + sizeof(struct file_handle)
                   + fh->handle_bytes;
            return snprintf(path, pathlen, "%s/%s", watch_dir, name)
                   < (int)pathlen;
        }
        info += h->len;
    }
    return 0;
}

static void
handle_event(int fd, const struct fanotify_event_metadata *md)
{
    char path[PATH_MAX];

    if (fd == g1fd) {
        /* Directory lifecycle events. */
        if (md->mask & (FAN_CREATE | FAN_MOVED_TO)) {
            unsigned int t = (md->mask & FAN_MOVED_TO)
                                 ? FAN_EVENT_INFO_TYPE_NEW_DFID_NAME
                                 : FAN_EVENT_INFO_TYPE_DFID_NAME;
            if (event_path(md, t, path, sizeof(path))) {
                if (verbose) printf("FAN_CREATE/MOVED_TO: %s\n", path);
                add_record(path);
            }
        }
        if (md->mask & (FAN_DELETE | FAN_MOVED_FROM)) {
            unsigned int t = (md->mask & FAN_MOVED_FROM)
                                 ? FAN_EVENT_INFO_TYPE_OLD_DFID_NAME
                                 : FAN_EVENT_INFO_TYPE_DFID_NAME;
            if (event_path(md, t, path, sizeof(path))) {
                if (verbose) printf("FAN_DELETE/MOVED_FROM: %s\n", path);
                remove_record(path);
            }
        }
        return;
    }

    /* Group 2: per-file events. */
    if (!(md->mask & FILE_EVENTS)) return;
    if (!event_path(md, FAN_EVENT_INFO_TYPE_DFID_NAME, path, sizeof(path)))
        return;

    struct record *r = find_record(path);
    if (r == NULL) return;

    if (verbose)
        printf("file event mask=%llx: %s\n", (unsigned long long)md->mask,
               path);

    /* Note: fanotify merges same-inode events, so the mask may combine
       several event types; open_count is therefore approximate. */
    if (md->mask & FAN_OPEN) r->open_count++;
    if (md->mask & (FAN_CLOSE_WRITE | FAN_CLOSE_NOWRITE))
        if (r->open_count > 0) r->open_count--;
    if (md->mask & FAN_CLOSE_WRITE) r->last_write = time(NULL);
    if (md->mask & FAN_ACCESS) r->last_access = time(NULL);
    if ((md->mask & (FAN_MODIFY | FAN_CLOSE_WRITE)) && !refresh_size(r))
        return; /* record was removed */

    heap_update(r);
}

/* Read all available fanotify events from the file descriptor 'fd' and
   enforce the roofline after every single event. */
static void
handle_events(int fd)
{
    const struct fanotify_event_metadata *metadata;
    struct fanotify_event_metadata buf[200];
    ssize_t size;

    /* Loop while events can be read from the fanotify file descriptor. */

    for (;;) {

        /* Read some events.  */

        size = read(fd, buf, sizeof(buf));
        if (size == -1 && errno != EAGAIN) {
            perror("read");
            exit(EXIT_FAILURE);
        }

        /* Check if end of available data reached.  */

        if (size <= 0) break;

        /* Point to the first event in the buffer.  */

        metadata = buf;

        /* Loop over all events in the buffer.  */

        while (FAN_EVENT_OK(metadata, size)) {

            /* Check that run-time and compile-time structures match.  */

            if (metadata->vers != FANOTIFY_METADATA_VERSION) {
                fprintf(stderr, "Mismatch of fanotify metadata version.\n");
                exit(EXIT_FAILURE);
            }

            /* With FAN_REPORT_FID, metadata->fd is FAN_NOFD and the event
               data is in the info records.  */

            if (metadata->mask & FAN_Q_OVERFLOW) {
                fprintf(stderr,
                        "dirroof: fanotify queue overflow; resyncing\n");
                resync();
            } else {
                handle_event(fd, metadata);
            }

            enforce(); /* after every event */

            /* Advance to next event.  */

            metadata = FAN_EVENT_NEXT(metadata, size);
        }
    }
}

int
main(int argc, char **argv)
{
    struct arguments arguments;
    struct stat st;
    int mount_id;
    int epfd;
    struct epoll_event ev, events[8];

    /* Parse arguments */
    arguments.verbose = 0;
    arguments.roof = 0;
    arguments.delete_open = 0;
    parse_arguments(argc, argv, &arguments);

    if (stat(arguments.dir, &st) == -1 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "dirroof: %s: not a directory\n", arguments.dir);
        exit(EXIT_FAILURE);
    }

    /* Canonical absolute path, so it can be compared with the paths
       rebuilt from fanotify events.  */
    if (realpath(arguments.dir, watch_dir) == NULL) {
        perror("realpath");
        exit(EXIT_FAILURE);
    }

    roof = arguments.roof;
    verbose = arguments.verbose;
    delete_open = arguments.delete_open;

    /* Line-buffered output, so verbose messages appear promptly even
       when stdout is a pipe or log file.  */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* File handle of watch_dir, to tell events for direct children of
       watch_dir apart from events for files in subdirectories.  */
    dir_handle->handle_bytes = sizeof(dir_handle_buf) - sizeof(*dir_handle);
    dir_handle_valid
        = name_to_handle_at(AT_FDCWD, watch_dir, dir_handle, &mount_id, 0) == 0;

    /* Create the two fanotify groups.  FAN_REPORT_FID with
       FAN_REPORT_DFID_NAME reports events with a directory file handle
       plus the entry name instead of a file descriptor; this also works
       for unprivileged users (Linux 5.13+).  */
    g1fd = fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_NONBLOCK
                             | FAN_REPORT_FID | FAN_REPORT_DFID_NAME,
                         O_RDONLY | O_LARGEFILE);
    if (g1fd == -1) {
        perror("fanotify_init (dir group)");
        exit(EXIT_FAILURE);
    }
    g2fd = fanotify_init(FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_NONBLOCK
                             | FAN_REPORT_FID | FAN_REPORT_DFID_NAME,
                         O_RDONLY | O_LARGEFILE);
    if (g2fd == -1) {
        perror("fanotify_init (file group)");
        exit(EXIT_FAILURE);
    }

    /* Group 1: watch the directory for entry lifecycle events. */
    if (fanotify_mark(g1fd, FAN_MARK_ADD, DIR_EVENTS, AT_FDCWD, watch_dir)
        == -1) {
        perror("fanotify_mark");
        exit(EXIT_FAILURE);
    }

    /* Seed the record table with the files that already exist (the only
       readdir during normal operation), then enforce once.  */
    resync();
    enforce();

    /* Prepare for epoll.  */
    epfd = epoll_create1(0);
    if (epfd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN;
    ev.data.fd = g1fd; /* Fanotify dir-group input */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g1fd, &ev) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }
    ev.data.fd = g2fd; /* Fanotify file-group input */
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g2fd, &ev) == -1) {
        perror("epoll_ctl");
        exit(EXIT_FAILURE);
    }

    for (;;) {
        int n = epoll_wait(epfd, events, 8, -1);
        if (n == -1) {
            if (errno == EINTR) /* Interrupted by a signal */
                continue;       /* Restart epoll_wait() */

            perror("epoll_wait"); /* Unexpected error */
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < n; i++)
            if (events[i].events & EPOLLIN) handle_events(events[i].data.fd);
    }
    exit(EXIT_SUCCESS);
}
