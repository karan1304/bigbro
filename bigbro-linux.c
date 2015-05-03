/* bigbro filetracking library
   Copyright (C) 2015 David Roundy

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
   02110-1301 USA */

#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1

#include "bigbro.h"
#include "posixmodel.c"
#include <sys/wait.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h> /* for flags to open(2) */

static const int debug_output = 0;

static inline void debugprintf(const char *format, ...) {
  va_list args;
  va_start(args, format);
  if (debug_output) vfprintf(stdout, format, args);
  va_end(args);
}

#ifdef __linux__

static inline char *debug_realpath(struct inode *i) {
  // WARNING: only use this function as an input to debugprintf!
  if (debug_output) return model_realpath(i);
  return 0;
}

#include <sys/stat.h>

#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <linux/limits.h>
#include <errno.h>

#include <stdint.h>

#include "syscalls/linux.h"

static const void *my_ptrace_options =
  (void *)(PTRACE_O_TRACESYSGOOD |
           PTRACE_O_TRACEFORK |
           PTRACE_O_TRACEVFORK |
           PTRACE_O_TRACEVFORKDONE |
           PTRACE_O_TRACECLONE |
           PTRACE_O_TRACEEXEC);

enum arguments {
  RETURN_VALUE = -1
};

#ifdef __x86_64__
struct i386_user_regs_struct {
  uint32_t ebx;
  uint32_t ecx;
  uint32_t edx;
  uint32_t esi;
  uint32_t edi;
  uint32_t ebp;
  uint32_t eax;
  uint32_t xds;
  uint32_t xes;
  uint32_t xfs;
  uint32_t xgs;
  uint32_t orig_eax;
  uint32_t eip;
  uint32_t xcs;
  uint32_t eflags;
  uint32_t esp;
  uint32_t xss;
};

static long get_syscall_arg_64(const struct user_regs_struct *regs, int which) {
    switch (which) {
    case RETURN_VALUE: return regs->rax;
    case 0: return regs->rdi;
    case 1: return regs->rsi;
    case 2: return regs->rdx;
    case 3: return regs->r10;
    case 4: return regs->r8;
    case 5: return regs->r9;
    default: return -1L;
    }
}

static long get_syscall_arg_32(const struct i386_user_regs_struct *regs, int which) {
#else
static long get_syscall_arg_32(const struct user_regs_struct *regs, int which) {
#endif
    switch (which) {
    case RETURN_VALUE: return regs->eax;
    case 0: return regs->ebx;
    case 1: return regs->ecx;
    case 2: return regs->edx;
    case 3: return regs->esi;
    case 4: return regs->edi;
    case 5: return regs->ebp;
    default: return -1L;
    }
}

static char *read_a_string(pid_t child, unsigned long addr) {
    if (addr == 0) return 0;

    char *val = malloc(4096);
    int allocated = 4096;
    int read = 0;
    unsigned long tmp;
    while (1) {
        if (read + sizeof tmp > allocated) {
            allocated *= 2;
            val = realloc(val, allocated);
        }
        tmp = ptrace(PTRACE_PEEKDATA, child, addr + read);
        if(errno != 0) {
            val[read] = 0;
            break;
        }
        memcpy(val + read, &tmp, sizeof tmp);
        if (memchr(&tmp, 0, sizeof tmp) != NULL)
            break;
        read += sizeof tmp;
    }
    return val;
}

static inline struct inode *assert_cwd(struct posixmodel *m, pid_t pid, const char *msg) {
  struct inode *cwd = model_cwd(m, pid);
  if (!cwd) error(1, 0, "cwd does not exist for pid %d: %s\n", pid, msg);
  return cwd;
}

static pid_t wait_for_syscall(struct posixmodel *m, int firstborn) {
  pid_t child = 0;
  int status = 0;
  while (1) {
    long signal_to_send_back = 0;
    child = waitpid(-firstborn, &status, __WALL);
    if (child == -1) error(1, errno, "trouble waiting");
    if (WIFSTOPPED(status) && WSTOPSIG(status) & 0x80) {
      return child;
    } else if (WIFEXITED(status)) {
      debugprintf("%d: exited -> %d\n", child, -WEXITSTATUS(status));
      if (child == firstborn) return -WEXITSTATUS(status);
      continue; /* no need to do anything more for this guy */
    } else if (WIFSIGNALED(status)) {
      debugprintf("process %d died of a signal!\n", child);
      if (child == firstborn) return -WTERMSIG(status);
      continue; /* no need to do anything more for this guy */
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_FORK << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: forked from %d\n", pid, child);
      model_fork(m, child, pid);
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_VFORK << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: vforked from %d\n", pid, child);
      model_fork(m, child, pid);
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_CLONE << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: cloned from %d\n", pid, child);
      model_newthread(m, child, pid);
    } else if (WIFSTOPPED(status) && (status>>8) == (SIGTRAP | PTRACE_EVENT_EXEC << 8)) {
      unsigned long pid;
      ptrace(PTRACE_GETEVENTMSG, child, 0, &pid);
      debugprintf("%ld: execed from %d\n", pid, child);
      model_chdir(m, assert_cwd(m, child, "on exec"), ".", pid);
    } else if (WIFSTOPPED(status)) {
      // ensure that the signal we interrupted is actually delivered.
      switch (WSTOPSIG(status)) {
      case SIGCHLD: // I don't know why forwarding SIGCHLD along causes trouble.  :(
      case SIGTRAP: // SIGTRAP is what we get from ptrace
      case SIGVTALRM: // for some reason this causes trouble with ghc
        debugprintf("%d: ignoring signal %d\n", child, WSTOPSIG(status));
        break;
      default:
        signal_to_send_back = WSTOPSIG(status);
        debugprintf("%d: sending signal %d\n", child, signal_to_send_back);
      }
    } else {
      debugprintf("%d: unexpected something\n", child);
    }
    // tell the child to keep going!
    if (ptrace(PTRACE_SYSCALL, child, 0, (void *)signal_to_send_back) == -1) {
      /* Assume child died and that we will get a WIFEXITED
         shortly. */
    }
  }
}

static const char *get_registers(pid_t child, void **voidregs,
                                 long (**get_syscall_arg)(void *regs, int which)) {
  struct user_regs_struct *regs = malloc(sizeof(struct user_regs_struct));

  if (ptrace(PTRACE_GETREGS, child, NULL, regs) == -1) {
    debugprintf("error getting registers for %d!\n", child);
    free(regs);
    return 0;
  }
#ifdef __x86_64__
  if (regs->cs == 0x23) {
    struct i386_user_regs_struct *i386_regs = malloc(sizeof(struct i386_user_regs_struct));
    i386_regs->ebx = regs->rbx;
    i386_regs->ecx = regs->rcx;
    i386_regs->edx = regs->rdx;
    i386_regs->esi = regs->rsi;
    i386_regs->edi = regs->rdi;
    i386_regs->ebp = regs->rbp;
    i386_regs->eax = regs->rax;
    i386_regs->orig_eax = regs->orig_rax;
    i386_regs->eip = regs->rip;
    i386_regs->esp = regs->rsp;
    free(regs);
    *voidregs = i386_regs;
    struct i386_user_regs_struct *regs = i386_regs;
#else
    *voidregs = regs;
#endif
    *get_syscall_arg = (long (*)(void *regs, int which))get_syscall_arg_32;
    if ((uint32_t)regs->orig_eax >= sizeof(syscalls_32)/sizeof(syscalls_32[0])) {
      free(regs);
      return 0;
    }
    return syscalls_32[regs->orig_eax];
#ifdef __x86_64__
  } else {
    *voidregs = regs;
    *get_syscall_arg = (long (*)(void *regs, int which))get_syscall_arg_64;
    if ((uint64_t)regs->orig_rax >= sizeof(syscalls_64)/sizeof(syscalls_64[0])) {
      free(regs);
      return 0;
    }
    return syscalls_64[regs->orig_rax];
  }
#endif
}

static long wait_for_return_value(struct posixmodel *m, pid_t child) {
  ptrace(PTRACE_SYSCALL, child, 0, 0); // ignore return value
  wait_for_syscall(m, -child);
  void *regs = 0;
  long (*get_syscall_arg)(void *regs, int which) = 0;
  get_registers(child, &regs, &get_syscall_arg);
  long retval = get_syscall_arg(regs, RETURN_VALUE);
  free(regs);
  return retval;
}

static int save_syscall_access(pid_t child, struct posixmodel *m) {
  void *regs = 0;
  long (*get_syscall_arg)(void *regs, int which) = 0;

  const char *name = get_registers(child, &regs, &get_syscall_arg);
  if (!name) {
    free(regs);
    return -1;
  }

  debugprintf("%d: %s(?)\n", child, name);

  /*  TODO:

      chroot? mkdir? mkdirat? rmdir? rmdirat?
  */

  if (!strcmp(name, "open") || !strcmp(name, "openat")) {
    char *arg;
    struct inode *cwd;
    long flags;
    int fd = wait_for_return_value(m, child);
    if (fd >= 0) {
      if (!strcmp(name, "open")) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
        flags = get_syscall_arg(regs, 1);
        cwd = assert_cwd(m, child, "calling open");
      } else {
        arg = read_a_string(child, get_syscall_arg(regs, 1));
        flags = get_syscall_arg(regs, 2);
        cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
      }
      if (!arg) {
        debugprintf("%d: %s(NULL) -> %d\n", child, name, fd);
      } else if (flags & O_DIRECTORY) {
        if (!strcmp(name, "open")) {
          debugprintf("%d: opendir('%s') -> %d\n", child, arg, fd);
        } else {
          debugprintf("%d: opendirat(%d, '%s') -> %d\n", child, get_syscall_arg(regs, 0), arg, fd);
        }
        model_opendir(m, cwd, arg, child, fd);
      } else if (flags & (O_WRONLY | O_RDWR)) {
        debugprintf("%d: open('%s', 'w') -> %d\n", child, arg, fd);
        if (fd >= 0) {
          struct inode *i = model_creat(m, cwd, arg);
          if (i) debugprintf("%d: has written %s\n", child, debug_realpath(i));
        }
      } else {
        debugprintf("%d: open('%s', 'r') -> %d\n", child, arg, fd);
        struct inode *i = model_stat(m, cwd, arg);
        if (i && i->type == is_file) {
          debugprintf("%d: has read %s\n", child, debug_realpath(i));
          i->is_read = true;
        } else if (i && i->type == is_directory) {
          model_opendir(m, cwd, arg, child, fd);
        }
      }
      free(arg);
    }
  } else if (!strcmp(name, "unlink") || !strcmp(name, "unlinkat")) {
    char *arg;
    struct inode *cwd;
    int retval = wait_for_return_value(m, child);
    if (retval == 0) {
      if (!strcmp(name, "unlink")) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
        cwd = assert_cwd(m, child, "calling unlink");
        if (arg) debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
      } else {
        arg = read_a_string(child, get_syscall_arg(regs, 1));
        cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
        if (arg) debugprintf("%d: %s(%d, '%s') -> %d\n", child, name,
                             (int)get_syscall_arg(regs,0), arg, retval);
      }
      if (arg) {
        model_unlink(m, cwd, arg);
        free(arg);
      }
    }
  } else if (!strcmp(name, "creat") || !strcmp(name, "truncate") ||
             !strcmp(name, "utime") || !strcmp(name, "utimes")) {
    char *arg = read_a_string(child, get_syscall_arg(regs, 0));
    int retval = wait_for_return_value(m, child);
    if (arg) {
      debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
      if (retval >= 0) {
        model_creat(m, assert_cwd(m, child, "calling creat or similar"), arg);
      }
      free(arg);
    }
  } else if (!strcmp(name, "futimesat") || !strcmp(name, "utimensat")) {
    char *arg = read_a_string(child, get_syscall_arg(regs, 1));
    if (arg) {
      int retval = wait_for_return_value(m, child);
      debugprintf("%d: %s(%d, '%s') -> %d\n", child, name,
                  (int)get_syscall_arg(regs,0), arg, retval);
      if (retval >= 0) {
        struct inode *cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
        model_creat(m, cwd, arg);
      }
      free(arg);
    } else {
      int retval = wait_for_return_value(m, child);
      debugprintf("%d: %s(%d, NULL) -> %d\n", child, name,
                  (int)get_syscall_arg(regs,0), retval);
    }
  } else if (!strcmp(name, "lstat") || !strcmp(name, "lstat64") ||
             !strcmp(name, "readlink") || !strcmp(name, "readlinkat")) {
    char *arg;
    struct inode *cwd;
    int retval = wait_for_return_value(m, child);
    if (retval >= 0) {
      if (strcmp(name, "readlinkat")) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
        cwd = assert_cwd(m, child, "calling lstat or readlink");
      } else {
        arg = read_a_string(child, get_syscall_arg(regs, 1));
        cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
      }
      if (arg) {
        debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
        struct inode *i = model_lstat(m, cwd, arg);
        if (i && i->type != is_directory) {
          debugprintf("%d: has read %s\n", child, debug_realpath(i));
          i->is_read = true;
        }
      }
      free(arg);
    }
  } else if (!strcmp(name, "stat") || !strcmp(name, "stat64")) {
    int retval = wait_for_return_value(m, child);
    if (retval == 0) {
      char *arg = read_a_string(child, get_syscall_arg(regs, 0));
      if (arg) {
        debugprintf("%d: %s('%s') -> %d\n", child, name, arg, retval);
        struct inode *i = model_stat(m, assert_cwd(m, child, "calling stat"), arg);
        if (i && i->type != is_directory) {
          debugprintf("%d: has read %s\n", child, debug_realpath(i));
          i->is_read = true;
        }
        free(arg);
      }
    }
  } else if (!strcmp(name, "symlink") || !strcmp(name, "symlinkat")) {
    char *arg, *target;
    struct inode *cwd;
    int retval = wait_for_return_value(m, child);
    if (retval == 0) {
      if (!strcmp(name, "symlink")) {
        arg = read_a_string(child, get_syscall_arg(regs, 0));
        target = read_a_string(child, get_syscall_arg(regs, 1));
        if (arg) cwd = assert_cwd(m, child, "calling symlink");
      } else {
        arg = read_a_string(child, get_syscall_arg(regs, 1));
        target = read_a_string(child, get_syscall_arg(regs, 2));
        if (arg) cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
      }
      if (arg && target) {
        debugprintf("%d: %s('%s', '%s')\n", child, name, arg, target);
        model_symlink(m, cwd, arg, target);
      }
      free(arg);
      free(target);
    }
  } else if (!strcmp(name, "execve") || !strcmp(name, "execveat")) {
    char *arg;
    struct inode *cwd;
    if (!strcmp(name, "execve")) {
      arg = read_a_string(child, get_syscall_arg(regs, 0));
      if (arg) cwd = assert_cwd(m, child, "calling execve");
    } else {
      arg = read_a_string(child, get_syscall_arg(regs, 1));
      if (arg) cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
    }
    if (arg && strlen(arg)) {
      debugprintf("%d: %s('%s')\n", child, name, arg);
      struct inode *i = model_stat(m, cwd, arg);
      if (i) {
        debugprintf("%d: has read %s\n", child, debug_realpath(i));
        i->is_read = true;
      }
    }
    free(arg);
  } else if (!strcmp(name, "rename") || !strcmp(name, "renameat")) {
    char *from, *to;
    struct inode *cwd;
    int retval = wait_for_return_value(m, child);
    if (retval == 0) {
      if (!strcmp(name, "rename")) {
        from = read_a_string(child, get_syscall_arg(regs, 0));
        to = read_a_string(child, get_syscall_arg(regs, 1));
        cwd = assert_cwd(m, child, "calling rename");
      } else {
        from = read_a_string(child, get_syscall_arg(regs, 1));
        to = read_a_string(child, get_syscall_arg(regs, 2));
        if (to && from) cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
      }
      if (to && from) {
        debugprintf("%d: rename('%s', '%s') -> %d\n", child, from, to, retval);
        model_rename(m, cwd, from, to);
      }
      free(from);
      free(to);
    }
  } else if (!strcmp(name, "link") || !strcmp(name, "linkat")) {
    char *from, *to;
    struct inode *cwd;
    int retval = wait_for_return_value(m, child);
    if (retval == 0) {
      if (!strcmp(name, "link")) {
        from = read_a_string(child, get_syscall_arg(regs, 0));
        to = read_a_string(child, get_syscall_arg(regs, 1));
        cwd = assert_cwd(m, child, "calling link");
      } else {
        from = read_a_string(child, get_syscall_arg(regs, 1));
        to = read_a_string(child, get_syscall_arg(regs, 2));
        if (to && from) cwd = lookup_fd(m, child, get_syscall_arg(regs, 0));
      }
      if (to && from) {
        debugprintf("%d: link('%s', '%s') -> %d\n", child, from, to, retval);
        struct inode *f = model_stat(m, cwd, from);
        if (f) f->is_read = true;
        struct inode *t = model_stat(m, cwd, to);
        if (t) t->is_read = true;
      }
      free(from);
      free(to);
    }
  } else if (!strcmp(name, "close")) {
    int fd = get_syscall_arg(regs, 0);
    debugprintf("%d: close(%d)\n", child, fd);
    model_close(m, child, fd);
    wait_for_return_value(m, child);
  } else if (!strcmp(name, "dup2") || !strcmp(name, "dup3")) {
    int fd1 = get_syscall_arg(regs, 0);
    int fd2 = get_syscall_arg(regs, 1);
    int retval = wait_for_return_value(m, child);
    debugprintf("%d: %s(%d, %d) -> %d\n", child, name, fd1, fd2, retval);
    if (retval == fd2) model_dup2(m, child, fd1, fd2);
  } else if (!strcmp(name, "dup")) {
    int fd1 = get_syscall_arg(regs, 0);
    int retval = wait_for_return_value(m, child);
    debugprintf("%d: dup(%d) -> %d\n", child, fd1, retval);
    if (retval >= 0) model_dup2(m, child, fd1, retval);
  } else if (!strcmp(name, "fcntl") || !strcmp(name, "fcntl64")) {
    int fd1 = get_syscall_arg(regs, 0);
    int cmd = get_syscall_arg(regs, 1);
    int retval = wait_for_return_value(m, child);
    if ((cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) && retval >= 0) {
      debugprintf("%d: %s(F_DUPFD, %d) -> %d\n", child, name, fd1, retval);
      if (retval >= 0) model_dup2(m, child, fd1, retval);
    }
  } else if (!strcmp(name, "getdents") || !strcmp(name, "getdents64")) {
    int fd = get_syscall_arg(regs, 0);
    int retval = wait_for_return_value(m, child);
    debugprintf("%d: %s(%d) -> %d\n", child, name, fd, retval);
    if (retval >= 0) {
      struct inode *i = lookup_fd(m, child, fd);
      if (i && i->type == is_directory) {
        i->is_read = true;
        debugprintf("%d: have read '%s'\n", child, debug_realpath(i));
      }
    }
  } else if (!strcmp(name, "chdir")) {
    char *arg = read_a_string(child, get_syscall_arg(regs, 0));
    int retval = wait_for_return_value(m, child);
    if (arg) {
      debugprintf("%d: chdir(%s) -> %d\n", child, arg, retval);
      if (retval == 0) model_chdir(m, model_cwd(m, child), arg, child);
      free(arg);
    } else {
      debugprintf("%d: chdir(NULL) -> %d\n", child, retval);
    }
  }

  free(regs);
  return 0;
}


int bigbro(const char *workingdir, pid_t *child_ptr,
           int stdouterrfd, char **args,
           char ***read_from_directories_out,
           char ***read_from_files_out,
           char ***written_to_files_out) {
  pid_t firstborn = fork();
  if (firstborn == -1) {
    // Not sure what to do in case of fork error...
  }
  setpgid(firstborn, firstborn); // causes grandchildren to be killed along with firstborn

  if (firstborn == 0) {
    if (stdouterrfd > 0) {
      close(0); // close stdin so programs won't wait on input
      close(1);
      close(2);
      dup2(stdouterrfd, 1);
      dup2(stdouterrfd, 2);
      open("/dev/null", O_RDONLY);
    }
    if (workingdir && chdir(workingdir) != 0) return -1;
    ptrace(PTRACE_TRACEME);
    kill(getpid(), SIGSTOP);
    return execvp(args[0], args);
  } else {
    *child_ptr = firstborn;
    waitpid(firstborn, 0, __WALL);
    ptrace(PTRACE_SETOPTIONS, firstborn, 0, my_ptrace_options);
    if (ptrace(PTRACE_SYSCALL, firstborn, 0, 0) == -1) {
      return -1;
    }

    struct posixmodel m;
    init_posixmodel(&m);
    {
      char *cwd = getcwd(0,0);
      model_chdir(&m, 0, cwd, firstborn);
      if (workingdir) model_chdir(&m, model_cwd(&m, firstborn), workingdir, firstborn);
      free(cwd);
    }

    while (1) {
      pid_t child = wait_for_syscall(&m, firstborn);
      if (child <= 0) {
        debugprintf("Returning with exit value %d\n", -child);
        model_output(&m, read_from_directories_out, read_from_files_out,
                     written_to_files_out);
        free_posixmodel(&m);
        return -child;
      }

      if (save_syscall_access(child, &m) == -1) {
        /* We were unable to read the process's registers.  Assume
           that this is bad news, and that we should exit.  I'm not
           sure what else to do here. */
        return -1;
      }

      ptrace(PTRACE_SYSCALL, child, 0, 0); // ignore return value
    }
  }
  return 0;
}

#endif