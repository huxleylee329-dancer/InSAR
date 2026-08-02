#pragma once
#include "time.h"
struct rusage { struct timeval ru_utime; struct timeval ru_stime; };
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1
static int getrusage(int who, struct rusage* usage) { (void)who; if (usage) { usage->ru_utime.tv_sec = usage->ru_utime.tv_usec = usage->ru_stime.tv_sec = usage->ru_stime.tv_usec = 0; } return 0; }
