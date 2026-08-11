/*
 * csp_shell_apm.c - CSH APM: `sh <command...>` -- run one host shell command.
 *
 * Exists so a bench experiment can be ONE .csh file: prep (sentinel files),
 * the bus commands, and the external verdict (cmp/sha256sum/xxd) all in the
 * script that `csh -i` replays. The verdict programs are still external to
 * the mechanism under test -- csh is only the launcher, exactly as a bash
 * wrapper would be; what moves inside is the orchestration, not the judge.
 *
 *   sh cmp /tmp/pre.bin /tmp/sentinel_AA.bin
 *   sh "sha256sum a.bin b.bin"          (quotes optional; args are re-joined)
 *
 * The command line is argv[1..] re-joined with single spaces and handed to
 * system(), so shell operators (&&, |, >) pass through. The exit code is
 * always printed as "sh: rc=N" so a session log can be judged mechanically.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>

#include <apm/apm.h>
#include <slash/slash.h>

#define SH_CMD_MAX 4096

static int csp_shell_cmd(struct slash *slash)
{
    if (slash->argc < 2) {
        printf("  sh <command...>   run one host shell command, print sh: rc=N\n");
        return SLASH_EUSAGE;
    }

    char cmd[SH_CMD_MAX];
    cmd[0] = '\0';
    size_t used = 0;
    for (int i = 1; i < slash->argc; i++) {
        size_t need = strlen(slash->argv[i]) + (i > 1 ? 1 : 0);
        if (used + need >= sizeof(cmd) - 1) {
            printf("sh: command too long (max %d)\n", SH_CMD_MAX);
            return SLASH_EINVAL;
        }
        if (i > 1)
            strcat(cmd, " ");
        strcat(cmd, slash->argv[i]);
        used += need;
    }

    int st = system(cmd);
    if (st == -1) {
        printf("sh: system() failed\n");
        return SLASH_EINVAL;
    }
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    printf("sh: rc=%d\n", rc);
    return rc == 0 ? SLASH_SUCCESS : SLASH_EINVAL;
}

slash_command(sh, csp_shell_cmd, "<command...>",
              "Run one host shell command (for single-file .csh experiments)");

int apm_init(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    return 0;
}
