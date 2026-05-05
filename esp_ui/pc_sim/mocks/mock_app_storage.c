// Sim-local stand-in for app_storage. The tablet mounts FATFS on the
// microSD slot at /sdcard; the sim creates pc_sim_state/sdcard/ on disk
// and pretends it's mounted there. fopen/rename/remove for "/sdcard/..."
// paths get redirected by sim_compat.h, so app_prefs.c writes its
// monmix-prefs.json into pc_sim_state/sdcard/ without modification.
#include "app_storage.h"

#include <stdbool.h>
#include <sys/stat.h>
#include <stdio.h>

static bool s_mounted;

static void mkdir_if_needed(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

bool app_storage_init(void) {
    mkdir_if_needed("pc_sim_state");
    mkdir_if_needed("pc_sim_state/sdcard");
    s_mounted = true;
    fprintf(stdout, "[mock_storage] mounted at pc_sim_state/sdcard/ (logical /sdcard)\n");
    return true;
}

bool app_storage_is_mounted(void) {
    if (!s_mounted) (void)app_storage_init();
    return s_mounted;
}

const char *app_storage_mount_point(void) { return "/sdcard"; }
