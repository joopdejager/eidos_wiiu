/* Included first by every game source on a big-endian host (tools/dsaddr.py
 * --sso, cmake/dsaddr.cmake EIDOS_SSO):
 * the system headers with their own (native) layouts, then every struct and
 * union the game defines after this point stored in the DS order, so its
 * bitfield and union views of hardware words read as they do on the DS. */
#ifndef BW_SSO_PRE_H
#define BW_SSO_PRE_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <wchar.h>
#pragma scalar_storage_order little-endian
/* A game struct the hardware and the files never see, and whose fields the
 * game takes the address of, opts out (native order). */
#define BW_NATIVE_ORDER __attribute__((scalar_storage_order("big-endian")))
#endif
