/* SPDX-License-Identifier: MIT
 *
 * Harness for tests/test_audio_follow.py: the capture stream of rdx_audio_tx (cap_connect / cap_check
 * of p5/rdx_audio_tx.c, compiled in as it is) without the RDMA part. Nothing is posted (total_q = 0);
 * the data thread only counts frames. Once per 100 ms it prints
 *
 *   frames <captured frames> restarts <stream rebuilds>
 *
 * and ends on SIGTERM / SIGINT.
 *
 *   test_audio_follow default|SINKNAME
 */
#define main rdx_audio_tx_main
#include "../p5/rdx_audio_tx.c"
#undef main

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s default|SINKNAME\n", argv[0]); return 2; }
    T.capture = 1;
    T.rt_prio = 0;
    atomic_store(&T.total_q, 0);                 /* the process callback counts, post_quantum returns at once */
    rdx_stop_signals(on_signal);
    if (pw_start(argv[1])) return 1;
    while (!g_stop) {
        usleep(100000);
        cap_check();
        printf("frames %llu restarts %u\n", (unsigned long long)atomic_load(&T.frames), T.cap_restarts);
        fflush(stdout);
    }
    pw_thread_loop_lock(T.loop);
    if (T.stream) pw_stream_destroy(T.stream);
    pw_thread_loop_unlock(T.loop);
    pw_thread_loop_stop(T.loop);
    return 0;
}
