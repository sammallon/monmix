#include "throttle.h"

#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _GNU_SOURCE
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

static bool s_active;

void throttle_apply(void) {
    s_active = true;

#ifdef _WIN32
    HANDLE proc = GetCurrentProcess();
    if (!SetProcessAffinityMask(proc, (DWORD_PTR)1)) {
        fprintf(stderr, "throttle: SetProcessAffinityMask failed (%lu)\n",
                GetLastError());
    }
    if (!SetPriorityClass(proc, BELOW_NORMAL_PRIORITY_CLASS)) {
        fprintf(stderr, "throttle: SetPriorityClass failed (%lu)\n",
                GetLastError());
    }
#else
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(0, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) {
        perror("throttle: sched_setaffinity");
    }
    if (setpriority(PRIO_PROCESS, 0, 5) != 0) {
        perror("throttle: setpriority");
    }
#endif
    fprintf(stdout, "throttle: single-core, below-normal priority, frame cap %u ms\n",
            (unsigned)throttle_frame_ms());
    fflush(stdout);
}

uint32_t throttle_frame_ms(void) {
    return s_active ? 33u : 0u;
}

bool throttle_active(void) {
    return s_active;
}
