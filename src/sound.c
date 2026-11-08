/* Playing a WAV image: it is written to a temporary file and handed to the system player.
   Windows: PlaySound (winmm.dll);  macOS: afplay;  other Unix: paplay, then aplay.
   PDPY11_PLAYER=<program> replaces the player (it is called with the file name as the only argument).
   -DPDPY11_NO_SOUND turns the module into a stub. */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "comp.h"

#if defined(PDPY11_NO_SOUND)

bool sound_play(const unsigned char *wav, size_t n, char **err) {
    (void)wav; (void)n;
    *err = astrdup("sound is disabled in this build");
    return false;
}

#elif defined(_WIN32)

#include <windows.h>
#include <mmsystem.h>
#ifdef _MSC_VER
#pragma comment(lib, "winmm.lib")
#endif

bool sound_play(const unsigned char *wav, size_t n, char **err) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"p11", 0, path)) {
        *err = astrdup("cannot create a temporary file");
        return false;
    }
    FILE *f = _wfopen(path, L"wb");
    if (!f || fwrite(wav, 1, n, f) != n) {
        if (f) fclose(f);
        DeleteFileW(path);
        *err = astrdup("cannot write the temporary file");
        return false;
    }
    fclose(f);
    BOOL ok = PlaySoundW(path, NULL, SND_FILENAME | SND_SYNC);
    DeleteFileW(path);
    if (!ok) { *err = astrdup("the sound could not be played (no audio device?)"); return false; }
    return true;
}

#elif defined(__unix__) || defined(__APPLE__) || defined(__linux__)

#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

static int run_player(const char *prog, const char *file) {
    char *argv[3] = {(char *)prog, (char *)file, NULL};
    pid_t pid;
    int rc = posix_spawnp(&pid, prog, NULL, NULL, argv, environ);
    if (rc != 0) return -1;                                  /* the program does not exist */
    int status;
    while (waitpid(pid, &status, 0) < 0) if (errno != EINTR) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status) == 127 ? -1 : WEXITSTATUS(status);
    return 1;
}

bool sound_play(const unsigned char *wav, size_t n, char **err) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    char *path = afmt("%s/pdpy11-XXXXXX", tmp);
    int fd = mkstemp(path);
    if (fd < 0) { *err = afmt("cannot create a temporary file in %s", tmp); return false; }
    FILE *f = fdopen(fd, "wb");
    if (!f || fwrite(wav, 1, n, f) != n) {
        if (f) fclose(f); else close(fd);
        unlink(path);
        *err = astrdup("cannot write the temporary file");
        return false;
    }
    fclose(f);
    const char *forced = getenv("PDPY11_PLAYER");
#ifdef __APPLE__
    const char *const players[] = {"afplay", NULL};
#else
    const char *const players[] = {"paplay", "aplay", NULL};
#endif
    int rc = -1;
    if (forced && *forced) rc = run_player(forced, path);
    else for (int i = 0; players[i] && rc < 0; i++) rc = run_player(players[i], path);
    unlink(path);
    if (rc < 0) { *err = astrdup(forced && *forced ? "the player set in PDPY11_PLAYER could not be started"
                                                    : "no audio player found (afplay, paplay or aplay is required)"); return false; }
    if (rc != 0) { *err = astrdup("the audio player reported an error"); return false; }
    return true;
}

#else

bool sound_play(const unsigned char *wav, size_t n, char **err) {
    (void)wav; (void)n;
    *err = astrdup("sound is not supported on this platform");
    return false;
}

#endif
