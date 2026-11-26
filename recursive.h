#ifndef DIRROOF_RECURSIVE_H
#define DIRROOF_RECURSIVE_H

struct arguments {
    char *dir;
    unsigned long long roof;
    unsigned int eviction_depth;
    int verbose;
    int delete_open;
    int recursive;
    int prune_empty;
};

int watch_tree(const struct arguments *arguments);

#endif
