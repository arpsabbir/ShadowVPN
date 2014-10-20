/**
  deamon.c

  Copyright (c) 2014 clowwindy

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.

*/

#include "shadowvpn.h"

#ifdef TARGET_WIN32

/* TODO: support daemonize for Windows */

int daemon_start(const shadowvpn_args_t *args) {
  errf("daemonize currently not supported, skipping");
  return 0;
}

int daemon_stop(const shadowvpn_args_t *args) {
  errf("daemonize currently not supported, skipping");
  return 0;
}

#else

#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>

#define PID_BUF_SIZE 32

static int write_pid_file(const char *filename, pid_t pid);

static void sig_handler_exit(int signo) {
  exit(0);
}

int daemon_start(const shadowvpn_args_t *args) {
  pid_t pid = fork();
  if (pid == -1) {
    err("fork");
    return -1;
  }
  if (pid > 0) {
    // let the child print message to the console first
    signal(SIGINT, sig_handler_exit);
    sleep(5);
    exit(0);
  } 

  pid_t ppid = getppid();
  pid = getpid();
  if (0 != write_pid_file(args->pid_file, pid)) {
    kill(ppid, SIGINT);
    return -1;
  }

  // print on console
  printf("started\n");
  kill(ppid, SIGINT);

  // then rediret stdout & stderr
  fclose(stdin);
  FILE *fp;
  fp = freopen(args->log_file, "a", stdout);
  if (fp == NULL) {
    err("freopen");
    return -1;
  }
  fp = freopen(args->log_file, "a", stderr);
  if (fp == NULL) {
    err("freopen");
    return -1;
  }

  return 0;
}

static int write_pid_file(const char *filename, pid_t pid) {
  char buf[PID_BUF_SIZE];
  int fd = open(filename, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
  if (fd == -1) {
    errf("can not open %s", filename);
    err("open");
    return -1;
  }
  int flags = fcntl(fd, F_GETFD);
  if (flags == -1) {
    err("fcntl");
    return -1;
  }

  flags |= FD_CLOEXEC;
  if (-1 == fcntl(fd, F_SETFD, flags))
    err("fcntl");

  struct flock fl;
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  if (-1 == fcntl(fd, F_SETLK, &fl)) {
    ssize_t n = read(fd, buf, PID_BUF_SIZE - 1);
    if (n > 0) {
      buf[n] = 0;
      errf("already started at pid %ld", atol(buf));
    } else {
      errf("already started");
    }
    close(fd);
    return -1;
  }
  if (-1 == ftruncate(fd, 0)) {
    err("ftruncate");
    return -1;
  }
  snprintf(buf, PID_BUF_SIZE, "%ld\n", (long)getpid());

  if (write(fd, buf, strlen(buf)) != strlen(buf)) {
    err("write");
    return -1;
  }
  return 0;
}

int daemon_stop(const shadowvpn_args_t *args) {
  char buf[PID_BUF_SIZE];
  int status, i, stopped;
  FILE *fp = fopen(args->pid_file, "r");
  if (fp == NULL) {
    printf("not running\n");
    return 0;
  }
  char *line = fgets(buf, PID_BUF_SIZE, fp);
  fclose(fp);
  if (line == NULL) {
    err("fgets");
    return 0;
  }
  pid_t pid = (pid_t)atol(buf);
  if (pid > 0) {
    // make sure pid is not zero or negative
    if (0 != kill(pid, SIGTERM)){
      if (errno == ESRCH) {
        printf("not running\n");
        return 0;
      }
      err("kill");
      return -1;
    }
    stopped = 0;
    // wait for maximum 10s
    for (i = 0; i < 200; i++) {
      if (-1 == kill(pid, 0)) {
        if (errno == ESRCH) {
          stopped = 1;
          break;
        }
      }
      // sleep 0.05s
      usleep(50000);
    }
    if (!stopped) {
      errf("timed out when stopping pid %d", pid);
      return -1;
    }
    printf("stopped\n");
    if (0 != unlink(args->pid_file)) {
      err("unlink");
      return -1;
    }
  } else {
    errf("pid is not positive: %ld", (long)pid);
    return -1;
  }
  return 0;
}

#endif
