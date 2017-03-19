#include "realpath.h"
#include "fcntl.h" // for AT_FDCWD

static inline char *interpret_path_at(pid_t pid, int fd, const char *path) {
  if (!path) return NULL;
  /* printf("path: %s\n", path); */
  const char *procself = "/proc/self/";
  const int procselflen = strlen(procself);
  if (strlen(path) > procselflen &&
      !memcmp(path, procself, procselflen)) {
    char *out = malloc(PATH_MAX);
    snprintf(out, PATH_MAX, "/proc/%d/%s", pid, path + procselflen);
    /* printf("rawpath: %s\n", out); */
    return out;
  }
  /* printf("path '%s' does not match '%s'\n", path, procself); */
  if (path[0] == '/') {
    /* printf("rawpath: %s\n", path); */
    return strdup(path);
  }

  char *proc_fd = malloc(PATH_MAX);
  if (fd >= 0 && fd != AT_FDCWD) snprintf(proc_fd, PATH_MAX, "/proc/%d/fd/%d", pid, fd);
  else snprintf(proc_fd, PATH_MAX, "/proc/%d/cwd", pid);
  char *cwd = malloc(PATH_MAX);
  int linklen = readlink(proc_fd, cwd, PATH_MAX);
  if (linklen < 0) {
    fprintf(stderr, "unable to determine cwd from %s i.e. fd %d!!! (%s)\n",
            proc_fd, fd, strerror(errno));
    return NULL;
  }
  cwd[linklen] = 0;
  /* printf("cwd(%d): %s (from %s)\n", fd, cwd, proc_fd); */
  int total_len = strlen(cwd) + strlen(path) + 4;
  if (total_len > PATH_MAX) cwd = realloc(cwd, total_len);
  strcat(cwd, "/");
  strcat(cwd, path);
  free(proc_fd);
  /* printf("rawpath: (%d) %s\n", fd, cwd); */
  return cwd;
}

static inline void read_dir_fd(pid_t pid, int dirfd, rw_status *h) {
  char *rawpath = interpret_path_at(pid, dirfd, ".");
  char *abspath = flexible_realpath(rawpath, h, look_for_file_or_directory, false);
  if (!lookup_in_hash(&h->mkdir, abspath)) {
    insert_hashset(&h->readdir, abspath);
  }
  free(rawpath);
  free(abspath);
}

static inline void read_something_at(pid_t pid, int dirfd, const char *path,
                                     rw_status *h, enum last_symlink_handling lh,
                                     bool failure_is_okay) {
  char *rawpath = interpret_path_at(pid, dirfd, path);
  char *abspath = flexible_realpath(rawpath, h, lh, failure_is_okay);
  struct stat st;
  if (!lookup_in_hash(&h->written, abspath) && !stat(abspath, &st) && S_ISREG(st.st_mode)) {
    insert_hashset(&h->read, abspath);
  }
  free(rawpath);
  free(abspath);
}

static inline void write_something_at(pid_t pid, int dirfd, const char *path,
                                      rw_status *h, enum last_symlink_handling lh,
                                      bool failure_is_okay) {
  char *rawpath = interpret_path_at(pid, dirfd, path);
  char *abspath = flexible_realpath(rawpath, h, lh, failure_is_okay);
  insert_hashset(&h->written, abspath);
  delete_from_hashset(&h->read, abspath);
  free(rawpath);
  free(abspath);
}

static inline void read_file_at(pid_t pid, int dirfd, const char *path,
                                rw_status *h) {
  read_something_at(pid, dirfd, path, h, look_for_file_or_directory, false);
}

static inline void maybe_read_file_at(pid_t pid, int dirfd, const char *path,
                                rw_status *h) {
  read_something_at(pid, dirfd, path, h, look_for_file_or_directory, true);
}

static inline void write_file_at(pid_t pid, int dirfd, const char *path,
                                 rw_status *h) {
  write_something_at(pid, dirfd, path, h, look_for_file_or_directory, false);
}

static inline void read_link_at(pid_t pid, int dirfd, const char *path,
                                rw_status *h) {
  read_something_at(pid, dirfd, path, h, look_for_symlink, false);
}

static inline void write_link_at(pid_t pid, int dirfd, const char *path,
                                 rw_status *h) {
  write_something_at(pid, dirfd, path, h, look_for_symlink, false);
}
