#include "ab_rtsp_server.h"

#include "ab_base/ab_mem.h"

#include "ab_log/ab_logger.h"

#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <sys/signal.h>

static bool g_quit = true;

static void signal_catch(int signal_num) {
    if (SIGINT == signal_num)
        g_quit = true;
}

int main(int argc, char *argv[]) {
    if (argc < 2)
        return -1;

    const char *in_file = argv[1];

    signal(SIGINT, signal_catch);

    ab_logger_init(AB_LOGGER_OUTPUT_TO_STDOUT, ".", "log", 100, 1024 * 1024);
    AB_LOGGER_INFO("startup.\n");

    int video_codec = 0;
    if (strstr(in_file, ".h264"))
        video_codec = 1;
    else if (strstr(in_file, ".h265"))
        video_codec = 2;

    if (video_codec != 0) {
        ab_rtsp_server_t rtsp = ab_rtsp_server_new(554, video_codec);

        AB_LOGGER_INFO("RTSP server startup.\n");

        g_quit = false;
        int nread = 0;
        const unsigned int data_buf_size = 10 * 1024;
        char *data_buf = (char *) ALLOC(data_buf_size);
        FILE *file = fopen(in_file, "rb");
        while (!g_quit) {
            if (file != NULL) {
                nread = fread(data_buf, 1, data_buf_size, file);
                if (nread > 0) {
                    ab_rtsp_server_send(rtsp, data_buf, nread);
                } else {
                    ab_rtsp_server_send(rtsp, NULL, 0);
                    fseek(file, 0, SEEK_SET);
                }
            }
            usleep(40 * 1000);
        }

        FREE(data_buf);
        data_buf = NULL;

        fclose(file);

        AB_LOGGER_INFO("RTSP server quit.\n");

        ab_rtsp_server_free(&rtsp);
    }

    AB_LOGGER_INFO("shutdown.\n");
    ab_logger_deinit();

    return 0;
}
