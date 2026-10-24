/* Stand-in for p1/rdx_mirror_src and p4/rdx_input_rx in tests/test_session_update.py:
 * answers --list-json with two outputs; as input_rx prints its "listening" line; as
 * mirror_src logs every runtime control signal (SIGRTMIN cursor, SIGRTMIN+1 output) with
 * its value to the file named by $RDX_DUMMY_LOG, until SIGTERM. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    sigset_t ss;                     /* first thing, like the handlers of the real mirror_src */
    sigemptyset(&ss);
    sigaddset(&ss, SIGRTMIN);
    sigaddset(&ss, SIGRTMIN + 1);
    sigaddset(&ss, SIGTERM);
    sigprocmask(SIG_BLOCK, &ss, NULL);
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--list-json")) {
            printf("{\"outputs\":[{\"name\":\"DP-1\",\"w\":2560,\"h\":1440,\"hz\":144,\"x\":2560,\"y\":0},"
                   "{\"name\":\"DP-2\",\"w\":2560,\"h\":1440,\"hz\":60,\"x\":0,\"y\":0}]}\n");
            return 0;
        }
    printf(strstr(argv[0], "input_rx") ? "[rx] listening on 127.0.0.1:0\n" : "[dummy] mirror running\n");
    fflush(stdout);
    FILE *log = fopen(getenv("RDX_DUMMY_LOG") ? getenv("RDX_DUMMY_LOG") : "/dev/null", "a");
    for (;;) {
        siginfo_t si;
        int s = sigwaitinfo(&ss, &si);
        if (s == SIGTERM) return 0;
        if (log) { fprintf(log, "%s %d\n", s == SIGRTMIN ? "cursor" : "output", si.si_value.sival_int); fflush(log); }
    }
}
