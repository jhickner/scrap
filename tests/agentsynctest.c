#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agentsync.h"
#include "text.h"

static char root[] = "/tmp/scrap-agentsync-XXXXXX";

static void mk(const char *rel)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/%s'", root, rel);
    assert(system(cmd) == 0);
}

static void spit(const char *rel, const char *text)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static char *slurp(const char *rel)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    return text_slurp(path, 1u << 20, NULL);
}

static int exists(const char *rel)
{
    char path[1024];
    struct stat st;
    snprintf(path, sizeof path, "%s/%s", root, rel);
    return lstat(path, &st) == 0;
}

int main(void)
{
    assert(mkdtemp(root));

    mk(".claude/skills/a");
    spit(".claude/skills/a/SKILL.md", "canonical a\n");
    mk(".codex/skills/a");
    spit(".codex/skills/a/SKILL.md", "stale copy\n");
    mk(".codex/skills/own");
    spit(".codex/skills/own/SKILL.md", "codex only\n");
    mk(".codex/skills/.system");
    mk(".pi/agent/skills");
    mk(".grok/skills");
    char cmd[2048];
    snprintf(cmd, sizeof cmd, "ln -s '%s/.codex/skills/a' '%s/.pi/agent/skills/a'", root, root);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "ln -s '%s/.claude/skills/a' '%s/.grok/skills/a'", root, root);
    assert(system(cmd) == 0);
    snprintf(cmd, sizeof cmd, "ln -s /nonexistent/elsewhere '%s/.grok/skills/foreign'", root);
    assert(system(cmd) == 0);

    mk(".claude");
    spit(".claude/settings.json",
         "{\"model\":\"opus\",\"hooks\":{"
         "\"SessionStart\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"hook.py\"}]}],"
         "\"Notification\":[{\"matcher\":\"\",\"hooks\":[{\"type\":\"command\",\"command\":\"hook.py\"}]}]"
         "}}\n");
    mk(".codex");
    spit(".codex/hooks.json",
         "{\"hooks\":{\"SessionStart\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"codex-hook.py\"}]}],"
         "\"PostToolUse\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"codex-hook.py\"}]}]}}\n");

    FILE *log = fopen("/dev/null", "w");
    assert(log);
    assert(agentsync_run(root, 0, log));

    char path[1024], target[1024];
    snprintf(path, sizeof path, "%s/.agents/skills", root);
    ssize_t n = readlink(path, target, sizeof target - 1);
    assert(n > 0);
    target[n] = '\0';
    assert(strstr(target, "/.claude/skills"));
    assert(!exists(".pi/agent/skills/a"));
    assert(!exists(".grok/skills/a"));
    assert(exists(".grok/skills/foreign"));
    assert(exists(".codex/skills/a/SKILL.md"));
    assert(exists(".codex/skills/own/SKILL.md"));
    assert(exists(".codex/skills/.system"));

    char *src = slurp(".config/scrap/hooks.json");
    assert(src);
    assert(strstr(src, "\"codex\":\t\"codex-hook.py\""));
    assert(strstr(src, "\"Notification\""));
    free(src);

    char *claude = slurp(".claude/settings.json");
    assert(claude);
    assert(strstr(claude, "\"model\":\t\"opus\""));
    assert(strstr(claude, "\"Notification\""));
    assert(!strstr(claude, "codex-hook.py"));
    assert(!strstr(claude, "\"PostToolUse\""));
    free(claude);

    char *codex = slurp(".codex/hooks.json");
    assert(codex);
    assert(strstr(codex, "codex-hook.py"));
    assert(!strstr(codex, "\"Notification\""));
    assert(!strstr(codex, "\"codex\""));
    assert(strstr(codex, "\"PostToolUse\""));
    free(codex);

    char *ext = slurp(".pi/agent/extensions/scrap-hooks.ts");
    assert(ext);
    assert(strstr(ext, "session_start"));
    free(ext);

    spit(".config/scrap/hooks.json",
         "{\"hooks\":{\"Stop\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"all.sh\","
         "\"claude\":\"claude.sh\",\"pi\":false}]}],"
         "\"SessionEnd\":[{\"hooks\":[{\"type\":\"command\",\"command\":\"end.sh\",\"codex\":false}]}]}}\n");
    assert(agentsync_run(root, 1, log));
    claude = slurp(".claude/settings.json");
    assert(strstr(claude, "claude.sh") && !strstr(claude, "all.sh") && strstr(claude, "end.sh"));
    assert(!strstr(claude, "\"pi\""));
    free(claude);
    codex = slurp(".codex/hooks.json");
    assert(strstr(codex, "all.sh") && !strstr(codex, "SessionEnd"));
    free(codex);

    assert(!exists(".codex/skills/a"));
    assert(exists(".codex/skills/own/SKILL.md"));

    fclose(log);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    assert(system(cmd) == 0);
    puts("agentsynctest ok");
    return 0;
}
