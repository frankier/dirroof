#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "recursive.h"

#define ENTRY_EVENTS (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO)
#define FILE_EVENTS                                                            \
    (IN_OPEN | IN_ACCESS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE              \
     | IN_CLOSE_NOWRITE)
#define WATCH_EVENTS                                                           \
    (ENTRY_EVENTS | FILE_EVENTS | IN_DELETE_SELF | IN_MOVE_SELF)

/* A watch follows a directory inode across renames. A move cookie lets us
   reattach the entire subtree, including its open counts, at the new parent. */
struct entry {
    struct entry *parent;
    struct entry *next;
    struct entry *children;
    struct entry *next_watch;
    struct entry *next_move;
    uint32_t cookie;
    dev_t device;
    ino_t inode;
    mode_t type;
    int watch;
    int seen;
    unsigned long long size;
    unsigned long long open_count;
    struct timespec access;
    char *name;
};

struct tree {
    const struct arguments *options;
    struct entry *root;
    struct entry *watches;
    struct entry *moves;
    int root_fd;
    int notify_fd;
    unsigned long long total;
};

static void
fail(const char *operation)
{
    perror(operation);
    exit(EXIT_FAILURE);
}

static struct entry *
find_watch(struct tree *tree, int watch)
{
    for (struct entry *entry = tree->watches; entry; entry = entry->next_watch)
        if (entry->watch == watch) return entry;
    return NULL;
}

static int
same_entry(const struct entry *entry, const struct stat *st)
{
    return entry->device == st->st_dev && entry->inode == st->st_ino
           && entry->type == (st->st_mode & S_IFMT);
}

static struct entry **
entry_link(struct entry *parent, const char *name)
{
    struct entry **link = &parent->children;
    while (*link && strcmp((*link)->name, name) != 0)
        link = &(*link)->next;
    return link;
}

/* A pathname can disappear or be replaced before its queued event arrives. */
static int
open_child_directory(int parent_fd, const struct entry *entry)
{
    int fd = openat(parent_fd, entry->name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == -1 || !same_entry(entry, &st)) {
        close(fd);
        errno = ESTALE;
        return -1;
    }
    return fd;
}

static int
open_directory(const struct tree *tree, const struct entry *entry)
{
    if (entry == tree->root) return dup(tree->root_fd);
    if (!entry->parent) {
        errno = ESTALE;
        return -1;
    }
    int parent_fd = open_directory(tree, entry->parent);
    if (parent_fd < 0) return -1;
    int fd = open_child_directory(parent_fd, entry);
    int error = errno;
    close(parent_fd);
    errno = error;
    return fd;
}

static int
compare_time(struct timespec a, struct timespec b)
{
    if (a.tv_sec != b.tv_sec) return a.tv_sec < b.tv_sec ? -1 : 1;
    return (a.tv_nsec > b.tv_nsec) - (a.tv_nsec < b.tv_nsec);
}

static void
update_file(struct tree *tree, struct entry *entry, const struct stat *st)
{
    tree->total -= entry->size;
    entry->size = (unsigned long long)st->st_size;
    tree->total += entry->size;
    if (compare_time(st->st_atim, entry->access) > 0)
        entry->access = st->st_atim;
}

static void
forget_entry(struct tree *tree, struct entry *entry)
{
    while (entry->children) {
        struct entry *child = entry->children;
        entry->children = child->next;
        forget_entry(tree, child);
    }
    if (entry->watch >= 0) {
        struct entry **link = &tree->watches;
        while (*link != entry)
            link = &(*link)->next_watch;
        *link = entry->next_watch;
        if (tree->notify_fd >= 0)
            inotify_rm_watch(tree->notify_fd, entry->watch);
    }
    tree->total -= entry->size;
    free(entry->name);
    free(entry);
}

static void
forget_link(struct tree *tree, struct entry **link)
{
    struct entry *entry = *link;
    *link = entry->next;
    forget_entry(tree, entry);
}

static struct entry *
new_entry(struct entry *parent, const char *name, const struct stat *st)
{
    struct entry *entry = calloc(1, sizeof(*entry));
    if (!entry) fail("calloc(entry)");
    entry->name = strdup(name);
    if (!entry->name) fail("strdup");
    entry->parent = parent;
    entry->device = st->st_dev;
    entry->inode = st->st_ino;
    entry->type = st->st_mode & S_IFMT;
    entry->watch = -1;
    if (entry->type == S_IFREG) entry->access = st->st_atim;
    return entry;
}

static struct entry *take_move(struct tree *, uint32_t);
static void sync_entry(struct tree *, struct entry *, int, const char *,
                       struct entry *, int);

/* Install the watch before reading the directory so changes during the
   scan are queued. The procfs path refers to the already verified fd. */
static struct entry *
watch_directory(struct tree *tree, struct entry *entry, int fd)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    int watch
        = inotify_add_watch(tree->notify_fd, path, WATCH_EVENTS | IN_ONLYDIR);
    if (watch == -1) fail("inotify_add_watch");
    struct entry *old = find_watch(tree, watch);
    if (old) {
        int old_fd = open_directory(tree, old);
        if (old_fd >= 0) {
            fprintf(stderr,
                    "dirroof: directory %s is reachable by multiple paths\n",
                    entry->name);
            exit(EXIT_FAILURE);
        }
        if (old->parent) {
            struct entry **link = entry_link(old->parent, old->name);
            *link = old->next;
        } else {
            take_move(tree, old->cookie);
        }
        free(old->name);
        old->name = entry->name;
        old->parent = entry->parent;
        free(entry);
        return old;
    }
    entry->watch = watch;
    entry->next_watch = tree->watches;
    tree->watches = entry;
    return entry;
}

static void
scan_directory(struct tree *tree, struct entry *entry, int fd)
{
    for (struct entry *child = entry->children; child; child = child->next)
        child->seen = 0;
    DIR *dir = fdopendir(fd);
    if (!dir) fail("fdopendir");
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) {
            if (errno) fail("readdir");
            break;
        }
        if (strcmp(de->d_name, ".") != 0 && strcmp(de->d_name, "..") != 0)
            sync_entry(tree, entry, fd, de->d_name, NULL, 1);
    }
    closedir(dir);
    struct entry **link = &entry->children;
    while (*link) {
        if ((*link)->seen)
            link = &(*link)->next;
        else
            forget_link(tree, link);
    }
}

static void
sync_entry(struct tree *tree, struct entry *parent, int fd, const char *name,
           struct entry *moved, int rescan)
{
    struct stat st;
    int exists = fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) fail("fstatat");
    struct entry **link = entry_link(parent, name);

    if (moved && (!exists || !same_entry(moved, &st))) {
        /* A subsequent rename may have already happened on disk. Keep
           this inode available for the remaining events in the batch. */
        moved->next_move = tree->moves;
        tree->moves = moved;
        moved = NULL;
    }
    if (exists && rescan == 0 && !moved && S_ISREG(st.st_mode) && !*link) {
        struct entry **pending = &tree->moves;
        while (*pending && !same_entry(*pending, &st))
            pending = &(*pending)->next_move;
        if (*pending) {
            moved = *pending;
            *pending = moved->next_move;
        }
    }
    if (*link && (moved || !exists || !same_entry(*link, &st)))
        forget_link(tree, link);
    if (!exists) return;

    struct entry *entry = *link;
    int attach = moved || !entry || strcmp(entry->name, name) != 0;
    if (attach) {
        entry = moved;
        if (entry) {
            free(entry->name);
            entry->name = strdup(name);
            if (!entry->name) fail("strdup");
            entry->parent = parent;
        } else {
            entry = new_entry(parent, name, &st);
        }
    }
    int child_fd = -1;
    if (entry->type == S_IFDIR && (attach || rescan)) {
        child_fd = open_child_directory(fd, entry);
        if (child_fd < 0) {
            if (errno != ENOENT && errno != ESTALE) fail("openat");
            if (attach) forget_entry(tree, entry);
            return;
        }
        if (entry->watch < 0) entry = watch_directory(tree, entry, child_fd);
    }
    if (attach) {
        entry->next = parent->children;
        parent->children = entry;
    }
    entry->seen = 1;
    if (child_fd >= 0) scan_directory(tree, entry, child_fd);
    if (entry->type == S_IFREG) update_file(tree, entry, &st);
}

static struct entry *
take_move(struct tree *tree, uint32_t cookie)
{
    struct entry **link = &tree->moves;
    while (*link && (*link)->cookie != cookie)
        link = &(*link)->next_move;
    if (!*link) return NULL;
    struct entry *entry = *link;
    *link = entry->next_move;
    return entry;
}

static void
finish_moves(struct tree *tree)
{
    while (tree->moves)
        forget_entry(tree, take_move(tree, tree->moves->cookie));
}

static void
handle_tree_event(struct tree *tree, const struct inotify_event *event)
{
    struct entry *parent = find_watch(tree, event->wd);
    if (!parent) return;
    if (parent == tree->root
        && (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))) {
        fprintf(stderr, "dirroof: the watched root was removed or moved\n");
        exit(EXIT_FAILURE);
    }
    if (!event->len) return;
    if ((event->mask & IN_ISDIR) && !(event->mask & ENTRY_EVENTS)) return;

    struct entry **link = entry_link(parent, event->name);
    struct entry *entry = *link;
    if (event->mask & IN_MOVED_FROM) {
        if (entry) {
            struct stat st;
            int fd = open_directory(tree, parent);
            if (fd >= 0) {
                int still_here
                    = fstatat(fd, entry->name, &st, AT_SYMLINK_NOFOLLOW) == 0
                      && same_entry(entry, &st);
                close(fd);
                if (still_here) return;
            }
            *link = entry->next;
            entry->parent = NULL;
            entry->cookie = event->cookie;
            entry->next_move = tree->moves;
            tree->moves = entry;
        }
        return;
    }

    if (event->mask & (ENTRY_EVENTS | IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE)) {
        int fd = open_directory(tree, parent);
        if (fd < 0) return; /* Its old path may no longer be reachable. */
        struct entry *moved = NULL;
        if (event->mask & IN_MOVED_TO) moved = take_move(tree, event->cookie);
        sync_entry(tree, parent, fd, event->name, moved, 0);
        close(fd);
        entry = *entry_link(parent, event->name);
    }
    if (!entry || entry->type != S_IFREG) return;

    if (event->mask & IN_OPEN) entry->open_count++;
    if ((event->mask & (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE))
        && entry->open_count)
        entry->open_count--;
    if (event->mask & IN_ACCESS) clock_gettime(CLOCK_REALTIME, &entry->access);
}

static char *
entry_path(const struct entry *entry)
{
    char *path;
    char *parent = entry->parent ? entry_path(entry->parent) : NULL;
    if (parent) {
        if (asprintf(&path, "%s%s%s", parent,
                     strcmp(parent, "/") == 0 ? "" : "/", entry->name)
            < 0)
            fail("asprintf");
        free(parent);
    } else {
        path = strdup(entry->name);
        if (!path) fail("strdup");
    }
    return path;
}

struct candidate {
    struct entry *entry;
    char *path;
    unsigned long long size;
    unsigned long long open_count;
    struct timespec access;
    int has_file;
};

struct candidates {
    struct candidate *items;
    size_t count;
    size_t capacity;
};

static void
measure_candidate(struct candidate *candidate, const struct entry *entry)
{
    if (entry->type == S_IFREG) {
        if (!candidate->has_file
            || compare_time(entry->access, candidate->access) > 0)
            candidate->access = entry->access;
        candidate->has_file = 1;
        candidate->size += entry->size;
        candidate->open_count += entry->open_count;
    }
    for (const struct entry *child = entry->children; child;
         child = child->next)
        measure_candidate(candidate, child);
}

static void
collect_candidates(struct candidates *list, struct entry *parent,
                   unsigned int depth, unsigned int eviction_depth)
{
    for (struct entry *entry = parent->children; entry; entry = entry->next) {
        if (entry->type == S_IFDIR && depth != eviction_depth) {
            collect_candidates(list, entry, depth + 1, eviction_depth);
            continue;
        }
        struct candidate candidate = { .entry = entry };
        measure_candidate(&candidate, entry);
        if (candidate.size == 0) continue;

        if (list->count == list->capacity) {
            size_t capacity = list->capacity ? list->capacity * 2 : 32;
            struct candidate *items
                = realloc(list->items, capacity * sizeof(*items));
            if (!items) fail("realloc(candidates)");
            list->items = items;
            list->capacity = capacity;
        }
        candidate.path = entry_path(entry);
        list->items[list->count++] = candidate;
    }
}

static int
compare_candidates(const void *a, const void *b)
{
    const struct candidate *left = a;
    const struct candidate *right = b;
    if (left->open_count != right->open_count)
        return left->open_count < right->open_count ? -1 : 1;
    int order = compare_time(left->access, right->access);
    return order ? order : strcmp(left->path, right->path);
}

/* Work from verified directory descriptors and unlink symlinks as leaves.
   Each successful unlink updates the total, even if a later one fails. */
static int
evict_entry(struct tree *tree, struct entry **link, int parent_fd)
{
    struct entry *entry = *link;
    if (entry->type == S_IFDIR) {
        int fd = open_child_directory(parent_fd, entry);
        if (fd < 0) return 0;
        struct entry **child = &entry->children;
        while (*child) {
            if (!evict_entry(tree, child, fd)) child = &(*child)->next;
        }
        close(fd);
        if (entry->children) return 0;
    }

    struct stat st;
    int flags = entry->type == S_IFDIR ? AT_REMOVEDIR : 0;
    if (fstatat(parent_fd, entry->name, &st, AT_SYMLINK_NOFOLLOW) == -1
        || !same_entry(entry, &st))
        return 0;
    if (unlinkat(parent_fd, entry->name, flags) == 0) {
        if (tree->options->verbose) {
            char *path = entry_path(entry);
            printf("deleted %s (%llu bytes)\n", path, entry->size);
            free(path);
        }
        forget_link(tree, link);
        return 1;
    }
    if (errno != ENOTEMPTY && errno != ENOENT) {
        fprintf(stderr, "dirroof: cannot delete %s: %s\n", entry->name,
                strerror(errno));
    }
    return 0;
}

static void
prune_empty(struct tree *tree, struct entry *dir)
{
    while (dir != tree->root && !dir->children) {
        struct entry *parent = dir->parent;
        int fd = open_directory(tree, parent);
        if (fd < 0) break;
        struct entry **link = entry_link(parent, dir->name);
        int removed = evict_entry(tree, link, fd);
        close(fd);
        if (!removed) break;
        dir = parent;
    }
}

static void
enforce_tree(struct tree *tree)
{
    if (tree->total <= tree->options->roof) return;

    struct candidates list = { 0 };
    collect_candidates(&list, tree->root, 1, tree->options->eviction_depth);
    if (list.count > 1)
        qsort(list.items, list.count, sizeof(*list.items), compare_candidates);

    for (size_t i = 0; i < list.count && tree->total > tree->options->roof;
         i++) {
        struct candidate *candidate = &list.items[i];
        if (candidate->open_count && !tree->options->delete_open) break;
        struct entry *entry = candidate->entry;
        struct entry *parent = entry->parent;
        int fd = open_directory(tree, parent);
        if (fd < 0) continue;
        evict_entry(tree, entry_link(parent, entry->name), fd);
        close(fd);
        if (tree->options->prune_empty) prune_empty(tree, parent);
    }
    for (size_t i = 0; i < list.count; i++)
        free(list.items[i].path);
    free(list.items);
}

static void
rescan_tree(struct tree *tree)
{
    if (tree->notify_fd >= 0) {
        close(tree->notify_fd);
        tree->notify_fd = -1;
        finish_moves(tree);
        /* Retain open counts for entries that survive the rescan. */
        for (struct entry *entry = tree->watches; entry;
             entry = entry->next_watch)
            entry->watch = -1;
        tree->watches = NULL;
    }
    tree->notify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (tree->notify_fd < 0) fail("inotify_init1");

    /* Open a fresh file description so every scan starts at offset zero. */
    int fd = openat(tree->root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) fail("openat(root)");
    struct stat st;
    if (fstat(fd, &st) == -1) fail("fstat(root)");
    struct stat path_st;
    if (lstat(tree->options->dir, &path_st) == -1 || path_st.st_dev != st.st_dev
        || path_st.st_ino != st.st_ino) {
        fprintf(stderr, "dirroof: the watched root was removed or moved\n");
        exit(EXIT_FAILURE);
    }
    if (!tree->root) tree->root = new_entry(NULL, tree->options->dir, &st);
    watch_directory(tree, tree->root, fd);
    scan_directory(tree, tree->root, fd);
}

int
watch_tree(const struct arguments *arguments)
{
    struct tree tree = { .options = arguments, .notify_fd = -1 };
    tree.root_fd
        = open(arguments->dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (tree.root_fd < 0) fail("open(root)");
    rescan_tree(&tree);
    if (arguments->verbose) printf("watching %s recursively\n", arguments->dir);

    for (;;) {
        _Alignas(struct inotify_event) char buffer[16384];
        ssize_t size = read(tree.notify_fd, buffer, sizeof(buffer));
        if (size < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) fail("read(inotify)");
            finish_moves(&tree);
            enforce_tree(&tree);
            struct pollfd ready = { .fd = tree.notify_fd, .events = POLLIN };
            if (poll(&ready, 1, -1) < 0 && errno != EINTR)
                fail("poll(inotify)");
            continue;
        }
        for (ssize_t offset = 0; offset < size;) {
            const struct inotify_event *event = (const void *)(buffer + offset);
            if (event->mask & IN_Q_OVERFLOW) {
                fprintf(stderr,
                        "dirroof: inotify queue overflow; rescanning\n");
                rescan_tree(&tree);
                break;
            }
            handle_tree_event(&tree, event);
            offset += sizeof(*event) + event->len;
        }
    }
}
