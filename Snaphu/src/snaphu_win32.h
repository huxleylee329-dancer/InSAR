#pragma once

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include <direct.h>

struct stat { unsigned short st_mode; };

FILE* snaphu_fopen_utf8(const char* path, const char* mode);
int snaphu_remove_utf8(const char* path);
int snaphu_stat_utf8(const char* path, struct stat* result);
int snaphu_mkdir_utf8(const char* path, int mode);
int snaphu_rmdir_utf8(const char* path);
int snaphu_unlink_utf8(const char* path);

static int snaphu_gethostname(char* name, int namelen)
{
  DWORD length;

  if(name==NULL || namelen<=0){
    return(-1);
  }
  length=(DWORD)namelen;
  return(GetComputerNameA(name,&length) ? 0 : -1);
}

#define fopen(path, mode) snaphu_fopen_utf8((path), (mode))
#define remove(path) snaphu_remove_utf8((path))
#define unlink(path) snaphu_unlink_utf8((path))
#define mkdir(path, mode) snaphu_mkdir_utf8((path), (mode))
#define rmdir(path) snaphu_rmdir_utf8((path))
#define stat(path, result) snaphu_stat_utf8((path), (result))
#define gethostname(name, namelen) snaphu_gethostname((name), (namelen))
#define getpid _getpid
#define sleep(seconds) Sleep((DWORD)(seconds) * 1000)

/* Tile-mode process fan-out is intentionally unsupported on Windows until a
 * CreateProcessW implementation is supplied.  Single-tile execution remains
 * available and is what the Unwrap caller uses. */
typedef int pid_t;
#define fork() (-1)
#define waitpid(pid, status, options) (-1)
#define wait(status) (-1)
#define kill(pid, signal) (-1)
#define WIFEXITED(status) 0
#define WEXITSTATUS(status) (-1)
#ifndef SIGBUS
#define SIGBUS SIGSEGV
#endif
#endif
