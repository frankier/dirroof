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

It uses `fanotify` and requires Linux 5.1+ and glibc.
