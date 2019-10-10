/*
 * Copyright (C) 2015 by Andreas Fuchs <asf@boinkor.net>
 *
 * Includes significant portions of source code from reptyr, Copyright
 * (C) 2011 by Nelson Elhage
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifdef __linux__
#define _XOPEN_SOURCE
#endif

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <termios.h>
#include <signal.h>
#include <sys/socket.h>

#ifdef WITH_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include "deptyr.h"
#include "unix_socket.h"
#include "platform/platform.h"

static int verbose = 0;

void _debug(const char *pfx, const char *msg, va_list ap) {

     if (pfx)
          fprintf(stderr, "%s", pfx);
     vfprintf(stderr, msg, ap);
     fprintf(stderr, "\n");
}

void die(const char *msg, ...) {
     va_list ap;
     va_start(ap, msg);
     _debug("[!] ", msg, ap);
     va_end(ap);

     exit(1);
}

void debug(const char *msg, ...) {

     va_list ap;

     if (!verbose)
          return;

     va_start(ap, msg);
     _debug("[+] ", msg, ap);
     va_end(ap);
}

void error(const char *msg, ...) {
     va_list ap;
     va_start(ap, msg);
     _debug("[-] ", msg, ap);
     va_end(ap);
}

void setup_raw(struct termios *save) {
     struct termios set;
     if (tcgetattr(0, save) < 0) {
          fprintf(stderr, "Unable to read terminal attributes: %m");
          return;
     }
     set = *save;
     cfmakeraw(&set);
     if (tcsetattr(0, TCSANOW, &set) < 0)
          die("Unable to set terminal attributes: %m");
}

void resize_pty(int pty) {
     struct winsize sz;
     if (ioctl(0, TIOCGWINSZ, &sz) < 0) {
          // provide fake size to workaround some problems
          struct winsize defaultsize = {30, 80, 640, 480};
          if (ioctl(pty, TIOCSWINSZ, &defaultsize) < 0) {
               fprintf(stderr, "Cannot set terminal size\n");
          }
          return;
     }
     ioctl(pty, TIOCSWINSZ, &sz);
}

int writeall(int fd, const void *buf, ssize_t count) {
     ssize_t rv;
     while (count > 0) {
          rv = write(fd, buf, count);
          if (rv < 0) {
               if (errno == EINTR)
                    continue;
               return rv;
          }
          count -= rv;
          buf += rv;
     }
     return 0;
}

volatile sig_atomic_t winch_happened = 0;

void do_winch(int signal) {
     winch_happened = 1;
}

void do_proxy(int pty) {
     char buf[4096];
     ssize_t count;
     fd_set set;
     sigset_t mask;
     sigset_t select_mask;
     struct sigaction sa;

     // Block WINCH while we're outside the select, but unblock it
     // while we're inside:
     sigemptyset(&mask);
     sigaddset(&mask, SIGWINCH);
     if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
          fprintf(stderr, "sigprocmask: %m");
          return;
     }
     sa.sa_handler = do_winch;
     sa.sa_flags = 0;
     sigemptyset(&sa.sa_mask);
     sigaction(SIGWINCH, &sa, NULL);
     resize_pty(pty);

     while (1) {
          if (winch_happened) {
               winch_happened = 0;
               resize_pty(pty);
          }
          FD_ZERO(&set);
          FD_SET(0, &set);
          FD_SET(pty, &set);
          sigemptyset(&select_mask);
          if (pselect(pty + 1, &set, NULL, NULL, NULL, &select_mask) < 0) {
               if (errno == EINTR)
                    continue;
               fprintf(stderr, "select: %m");
               return;
          }
          if (FD_ISSET(0, &set)) {
               count = read(0, buf, sizeof buf);
               if (count < 0)
                    return;
               writeall(pty, buf, count);
          }
          if (FD_ISSET(pty, &set)) {
               count = read(pty, buf, sizeof buf);
               if (count <= 0)
                    return;
               writeall(1, buf, count);
          }
     }
}

void usage(char *me) {
     fprintf(stderr, "Usage: %s -s socket CMD\n", me);
     fprintf(stderr, "       %s -S socket\n", me);
     fprintf(stderr, "  -H Act as the head: Proxy input and output to the program\n");
     fprintf(stderr, "  -s Connect to a running proxy and exec the program\n");
     fprintf(stderr, "\n");
}

int main(int argc, char *argv[])
{
     struct termios saved_termios;
     int pty;
     int opt;
     int err;
     int act_as_proxy=0;
     int socket;

     while ((opt = getopt(argc, argv, "hs:H:V")) != -1) {
          switch (opt) {
          case 'h':
               usage(argv[0]);
               return 0;
          case 'V':
               verbose = 1;
               break;
          case 's':
               socket = connect_server(optarg);
               break;
          case 'H':
               socket = create_server(optarg);
               #ifdef WITH_SYSTEMD
               sd_notifyf(0,
                          "STATUS=Listening on socket %s\n"
                          "MAINPID=%lu\n"
                          "READY=1\n",
                          optarg,
                          (unsigned long)getpid());
               #endif
               act_as_proxy = 1;
               break;
          default:
               usage(argv[0]);
               return 1;
          }
     }

     if (!act_as_proxy && optind >= argc) {
          fprintf(stderr, "%s: No command specified\n", argv[0]);
          usage(argv[0]);
          return 1;
     }

     if (act_as_proxy) {
          int connection;

          while (0 <= (connection = accept(socket, NULL, NULL))) {
               pty = recv_file_descriptor(connection);
               if (pty < 0) {
                    die("Oof, didn't get a child FD: %m");
               }
               if (close(connection) < 0) {
                    die("close: %m");
               }

               setup_raw(&saved_termios);
               do_proxy(pty);
               do {
                    errno = 0;
                    if (tcsetattr(0, TCSANOW, &saved_termios) && errno != EINTR)
                         die("Unable to tcsetattr: %m");
               } while (errno == EINTR);
               close(pty);
          }
          die("accept: %m");
     } else {
          if ((pty = get_pt()) < 0)
               die("Unable to allocate a new pseudo-terminal: %m");
          if (unlockpt(pty) < 0)
               die("Unable to unlockpt: %m");
          if (grantpt(pty) < 0)
               die("Unable to grantpt: %m");

          char ptyname[255];
          ptsname_r(pty, ptyname, sizeof(ptyname));
          printf("Opened a new pty: %s\n", ptyname);
          fflush(stdout);

          if (send_file_descriptor(socket, pty) < 0) {
               die("Unable to send the master handle: %m");
          }

          setenv("REPTYR_PTY", ptyname, 1);
          {
               int f;
               setpgid(0, getppid());
               setsid();
               f = open(ptyname, O_RDONLY, 0);
               dup2(f, 0);
               close(f);
               f = open(ptyname, O_WRONLY, 0);
               dup2(f, 1);
               dup2(f, 2);
               close(f);
          }
          close(pty);
          execvp(argv[optind], argv + optind);
          die("execvp failed: %m");
     }
}
