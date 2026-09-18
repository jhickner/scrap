#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "orchdata.h"
#include "orchinstall.h"
#include "text.h"

static char *slurp(const char *path)
{
    return text_slurp(path, 1u << 20, NULL);
}

int main(void)
{
    char root[] = "/tmp/mux-orchinstall-XXXXXX";
    assert(mkdtemp(root));
    assert(setenv("HOME", root, 1) == 0);
    assert(orch_files_n > 0);

    orch_install();

    char path[512];
    snprintf(path, sizeof path, "%s/.claude/skills/orchestrate/SKILL.md", root);
    char *skill = slurp(path);
    assert(skill);
    assert(strstr(skill, "You are the orchestrator"));
    assert(!strstr(skill, "working/apps/mux"));
    free(skill);

    snprintf(path, sizeof path, "%s/.agents/skills/orchestrate/SKILL.md", root);
    char *mirrored = slurp(path);
    assert(mirrored);
    assert(strstr(mirrored, "mux orch dispatch"));
    free(mirrored);

    snprintf(path, sizeof path, "%s/.config/orchestrator/quota.sh", root);
    struct stat st;
    assert(stat(path, &st) == 0);
    assert(st.st_mode & 0111);
    char *quota = slurp(path);
    assert(quota);
    assert(strstr(quota, "agenttabs"));
    free(quota);

    snprintf(path, sizeof path, "%s/.config/orchestrator/routing.json", root);
    char *routing = slurp(path);
    assert(routing);
    assert(strstr(routing, "classes"));
    free(routing);

    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs("keep\n", f) >= 0);
    assert(fclose(f) == 0);
    orch_install();
    routing = slurp(path);
    assert(routing);
    assert(!strcmp(routing, "keep\n"));
    free(routing);

    snprintf(path, sizeof path, "%s/.claude/skills/orchestrate/SKILL.md", root);
    f = fopen(path, "w");
    assert(f);
    assert(fputs("stale\n", f) >= 0);
    assert(fclose(f) == 0);
    orch_install();
    skill = slurp(path);
    assert(skill);
    assert(strstr(skill, "You are the orchestrator"));
    free(skill);

    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    return 0;
}
