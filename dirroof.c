#define _GNU_SOURCE /* Needed to get O_LARGEFILE definition */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <argp.h>
#include <sys/fanotify.h>
#include <unistd.h>

const int MAXFILES = 4096;

const char *argp_program_version = "dirroof 0.1";

/* Program documentation. */
static char doc[] = "dirroof keeps a directory's size under a limit (roofline) by removing older files";

/* A description of the arguments we accept. */
static char args_doc[] = "DIR";

/* The options we understand. */
static struct argp_option options[] = {
    { "verbose", 'v', 0, 0, "Produce verbose output" },
    { 0 }
};

/* Used by main to communicate with parse_opt. */
struct arguments {
    char *dir;
    int verbose;
};

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

    case ARGP_KEY_ARG:
        if (state->arg_num >= 1) /* Too many arguments. */
            argp_usage(state);

        arguments->dir = arg;

        break;

    case ARGP_KEY_END:
        if (state->arg_num < 1) /* Not enough arguments. */
            argp_usage(state);
        break;

    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

struct record {
    time_t
}

static struct argp argp = { options, parse_opt, args_doc, doc };

/* Read all available fanotify events from the file descriptor 'fd'.  */

static void
handle_events(int fd)
{
    const struct fanotify_event_metadata *metadata;
    struct fanotify_event_metadata buf[200];
    ssize_t size;
    char path[PATH_MAX];
    ssize_t path_len;
    char procfd_path[PATH_MAX];
    struct fanotify_response response;

    /* Loop while events can be read from fanotify file descriptor.  */

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

            /* metadata->fd contains either FAN_NOFD, indicating a
              queue overflow, or a file descriptor (a nonnegative
              integer).  Here, we simply ignore queue overflow.  */

            if (metadata->fd >= 0) {

                /* Handle closing of writable file event.  */

                if (metadata->mask & FAN_CLOSE_WRITE)
                    printf("FAN_CLOSE_WRITE: ");

                /* Retrieve and print pathname of the accessed file.  */

                snprintf(procfd_path, sizeof(procfd_path), "/proc/self/fd/%d",
                         metadata->fd);
                path_len = readlink(procfd_path, path, sizeof(path) - 1);
                if (path_len == -1) {
                    perror("readlink");
                    exit(EXIT_FAILURE);
                }

                path[path_len] = '\0';
                printf("File %s\n", path);

                /* Close the file descriptor of the event.  */

                close(metadata->fd);
            }

            /* Advance to next event.  */

            metadata = FAN_EVENT_NEXT(metadata, size);
        }
    }
}

int main(int argc, char **argv)
{
    /* Parse arguments */
    struct arguments arguments;
    arguments.verbose = 0;
    argp_parse(&argp, argc, argv, 0, 0, &arguments);

    /* Create the file descriptor for accessing the fanotify API.  */
    int dir_fafd = fanotify_init(
        FAN_CLOEXEC | FAN_CLASS_NOTIF | FAN_NONBLOCK | FAN_REPORT_FID,
        O_RDONLY | O_LARGEFILE
    );
    if (fd == -1) {
        perror("fanotify_init");
        exit(EXIT_FAILURE);
    }

    /* Mark the mount for:
      - permission events before opening files
      - notification events after closing a write-enabled
        file descriptor.  */
    if (fanotify_mark(fd, FAN_MARK_ADD,
                      FAN_OPEN_PERM | FAN_CLOSE_WRITE, AT_FDCWD, arguments.dir)
        == -1) {
        perror("fanotify_mark");
        exit(EXIT_FAILURE);
    }

    /* Prepare for polling.  */
    nfds_t nfds = 1;

    struct pollfd fds[1];
    fds[0].fd = fd; /* Fanotify input */
    fds[0].events = POLLIN;

    for (;;) {
        int poll_num = poll(fds, nfds, -1);
        if (poll_num == -1) {
            if (errno == EINTR) /* Interrupted by a signal */
                continue;       /* Restart poll() */

            perror("poll"); /* Unexpected error */
            exit(EXIT_FAILURE);
        }

        if (poll_num > 0) {
            if (fds[0].revents & POLLIN) {
                handle_events(fd);
            }
        }
    }
    exit(EXIT_SUCCESS);
}
