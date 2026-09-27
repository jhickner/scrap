#include <assert.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

static uid_t test_uid(void) { return (uid_t)strtoul(getenv("SCRAP_TEST_UID"), NULL, 10); }
#define getuid test_uid
#define MACOS_VOICE_IMPLEMENTATION
#include "vendor/macos_voice.h"
#undef getuid

static int ready, final;
static void event(void *ud, const char *kind, const char *text)
{
    (void)ud;
    if (!strcmp(kind, "ready")) ready = 1;
    if (!strcmp(kind, "final")) {
        assert(!strcmp(text, "hello"));
        final = 1;
    }
}

int main(int argc, char **argv)
{
    alarm(10);
    macos_voice_opts opts = {.helper_path = "/unused/VoiceHelper.app"};
    if (argc > 1) {
        int fd = mv_handoff_fd();
        assert(fd >= 0);
        assert(!(fcntl(fd, F_GETFD) & FD_CLOEXEC));
        macos_voice_protect_handoff();
        assert(fcntl(fd, F_GETFD) & FD_CLOEXEC);
        macos_voice *v = macos_voice_start(&opts, event, NULL);
        assert(v && macos_voice_resumed(v) && ready);
        assert(v->fd == fd && v->len == 5);
        assert(!getenv(MV_HANDOFF_FD) && !getenv(MV_HANDOFF_BUF));
        assert(write(fd, "RESUMED\n", 8) == 8);
        while (!final) assert(macos_voice_poll(v, 1000) >= 0);
        macos_voice_stop(v);
        return 0;
    }

    char uid[32];
    snprintf(uid, sizeof uid, "%u", (unsigned)getpid() + 1000000);
    assert(setenv("SCRAP_TEST_UID", uid, 1) == 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "/tmp/macos-voice-%u.sock", test_uid());
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    assert(bind(listener, (struct sockaddr *)&addr, sizeof addr) == 0);
    assert(listen(listener, 1) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(listener);
        macos_voice *v = macos_voice_start(&opts, event, NULL);
        assert(v && !macos_voice_resumed(v));
        while (!ready || v->len != 5) assert(macos_voice_poll(v, 1000) >= 0);
        assert(macos_voice_handoff(v) == 0);
        execl(argv[0], argv[0], "--resume", (char *)NULL);
        _exit(1);
    }
    int peer = accept(listener, NULL, NULL);
    assert(peer >= 0);
    assert(write(peer, "READY\nT hel", 11) == 11);
    FILE *stream = fdopen(peer, "r");
    assert(stream);
    char line[128];

    assert(fgets(line, sizeof line, stream) && !strcmp(line, "RESUMED\n"));
    assert(write(peer, "lo\n", 3) == 3);
    assert(fgets(line, sizeof line, stream) && !strcmp(line, "QUIT\n"));
    int status;
    assert(waitpid(child, &status, 0) == child);
    fclose(stream);
    close(listener);
    unlink(addr.sun_path);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("voicehandofftest: ok");
    return 0;
}
