/*
 * ab_rtsp_server.c
 *
 *  Created on: 2022年1月7日
 *      Author: ljm
 */

#include "ab_rtsp_server.h"
#include "ab_rtp_def.h"

#include "ab_base/ab_list.h"
#include "ab_base/ab_mem.h"
#include "ab_base/ab_assert.h"

#include "ab_log/ab_logger.h"

#include "ab_net/ab_socket.h"
#include "ab_net/ab_tcp_server.h"
#include "ab_net/ab_udp_client.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>

#include <arpa/inet.h>

#define T ab_rtsp_server_t

enum ab_rtsp_over_method_t {
    AB_RTSP_OVER_NONE = 0,
    AB_RTSP_OVER_TCP,
    AB_RTSP_OVER_UDP
};

enum ab_video_codec_t {
    AB_VIDEO_CODEC_NONE = 0,
    AB_VIDEO_CODEC_H264,
    AB_VIDEO_CODEC_H265
};

typedef struct ab_rtsp_interleaved_frame_t {
    uint8_t         dollar_sign;        // '$' or 0x24
    uint8_t         channel_identifier; // 0x00(Video RTP)、0x01(Video RTCP)、
                                        // 0x02(Audio RTP)、0x03(Audio RTCP)
    uint16_t        data_length;        // RTP length
} ab_rtsp_interleaved_frame_t;

typedef struct ab_buffer_t {
    unsigned char  *data;
    int             size;
    int             used;
} ab_buffer_t;

typedef struct ab_rtsp_client_t {
    ab_socket_t     sock;
    int             video_codec;        // @ab_video_codec_t

    bool            ready;              // 准备就绪为true（收到play)，否则为false
    int             method;

    unsigned short  rtp_chn_port;
    unsigned short  rtcp_chn_port;

} ab_rtsp_client_t;

struct T {
    ab_tcp_server_t rtsp_tcp_srv;
    list_t          clients;

    ab_udp_client_t rtp_udp_srv;
    ab_udp_client_t rtcp_udp_srv;

    int             video_codec;        // @ab_video_codec_t

    pthread_mutex_t mutex;

    bool            quit;
    pthread_t       event_looper_thd;

    uint16_t        sequence;
    uint32_t        timestamp;

    ab_buffer_t     cache;
    ab_buffer_t     rtp_buffer;
};

static bool start_code3(const unsigned char *data, unsigned int data_size);
static bool start_code4(const unsigned char *data, unsigned int data_size);
static int find_start_code(const unsigned char *data, unsigned int data_size);

static void *event_looper_cb(void *arg);

static void accept_func(void *sock, void *user_data);

static void fill_rtsp_interleave_frame(
    ab_rtsp_interleaved_frame_t *interleaved_frame, 
    unsigned short data_len);

static void fill_rtp_header(ab_rtp_header_t *rtp_header, 
    unsigned int seq, unsigned int timestamp);

static void rtp_send_nalu(T rtsp, 
    const unsigned char *nalu, unsigned int nalu_len);

T ab_rtsp_server_new(unsigned short port, int video_codec) {
    const unsigned int data_cache_size          = 1024 * 1024;
    const unsigned int rtp_buffer_size          = 1400;


    T result;
    NEW(result);
    assert(result);

    result->rtsp_tcp_srv    = ab_tcp_server_new(port, accept_func, result);

    result->rtp_udp_srv     = ab_udp_client_new(RTP_SERVER_PORT);
    result->rtcp_udp_srv    = ab_udp_client_new(RTCP_SERVER_PORT);

    result->video_codec     = video_codec;

    result->clients         = NULL;

    pthread_mutex_init(&result->mutex, NULL);

    result->quit            = false;
    pthread_create(&result->event_looper_thd, NULL, event_looper_cb, result);

    result->sequence        = 0;
    result->timestamp       = 0;

    result->cache.size      = data_cache_size;
    result->cache.used      = 0;
    result->cache.data      = ALLOC(result->cache.size);

    result->rtp_buffer.size = rtp_buffer_size;
    result->rtp_buffer.used = 0;
    result->rtp_buffer.data = ALLOC(result->rtp_buffer.size);

    return result;
}

void ab_rtsp_server_free(T *rtsp) {
    assert(rtsp && *rtsp);

    FREE((*rtsp)->rtp_buffer.data);
    FREE((*rtsp)->cache.data);

    (*rtsp)->quit = true;
    pthread_join((*rtsp)->event_looper_thd, NULL);

    pthread_mutex_destroy(&(*rtsp)->mutex);

    while ((*rtsp)->clients) {
        ab_rtsp_client_t *client;
        (*rtsp)->clients = list_pop((*rtsp)->clients, (void **) &client);
        ab_socket_free(&(client->sock));
        FREE(client);
    }

    ab_udp_client_free(&(*rtsp)->rtcp_udp_srv);
    ab_udp_client_free(&(*rtsp)->rtp_udp_srv);
    ab_tcp_server_free(&(*rtsp)->rtsp_tcp_srv);

    FREE(*rtsp);
}

int ab_rtsp_server_send(T rtsp, const char *data, unsigned int data_len) {
    int result = 0;
    if (NULL == data || 0 == data_len) {
        if (rtsp->cache.used > 0) {
            int first_start_code_pos = find_start_code(
                rtsp->cache.data, rtsp->cache.used);
            if (0 == first_start_code_pos) {
                unsigned int start_code = 0;
                if (start_code3(rtsp->cache.data, rtsp->cache.used))
                    start_code = 3;
                else if (start_code4(rtsp->cache.data, rtsp->cache.used))
                    start_code = 4;

                rtp_send_nalu(rtsp, rtsp->cache.data + start_code, 
                    rtsp->cache.used - start_code);
                rtsp->cache.used = 0;
            }
        }

        return result;
    }

    if (rtsp->cache.size - rtsp->cache.used >= data_len) {
        memcpy(rtsp->cache.data + rtsp->cache.used, data, data_len);
        rtsp->cache.used += data_len;
    } else {
        AB_LOGGER_WARN("Not enough spaces(%u < %u).\n", 
            rtsp->cache.size, rtsp->cache.used + data_len);
        return result;
    }

    result = data_len;

    unsigned int start_pos = 0;
    while (start_pos < rtsp->cache.used) {
        int first_start_code_pos = find_start_code(
            rtsp->cache.data + start_pos, rtsp->cache.used - start_pos);
        if (0 == first_start_code_pos) {
            unsigned int start_code = 0;
            if (start_code3(rtsp->cache.data + start_pos, 
                rtsp->cache.used - start_pos)) {
                start_code = 3;
            } else if (start_code4(rtsp->cache.data + start_pos, 
                rtsp->cache.used - start_pos)) {
                start_code = 4;
            } else {
                continue;
            }

            int next_start_code_pos =
                find_start_code(rtsp->cache.data + start_pos + start_code, 
                    rtsp->cache.used - start_pos - start_code);
            if (-1 == next_start_code_pos) {
                if (start_pos != 0) {
                    rtsp->cache.used -= start_pos;
                    memcpy(rtsp->cache.data, 
                        rtsp->cache.data + start_pos, 
                        rtsp->cache.used);
                }
                break;
            } else {
                rtp_send_nalu(rtsp, 
                    rtsp->cache.data + start_pos + start_code, 
                    next_start_code_pos);
                start_pos += start_code + next_start_code_pos;
            }
        } else {
            break;
        }
    }

    return result;
}

static void get_sock_info(ab_socket_t sock,
    char *buf, unsigned int buf_size) {
    char addr_buf[64];
    unsigned short port = 0;

    memset(addr_buf, 0, buf_size);
    if (ab_socket_addr(sock, addr_buf, sizeof(addr_buf)) == 0 &&
        ab_socket_port(sock, &port) == 0) {
        snprintf(buf, buf_size, "%s:%d", addr_buf, port);
    }
}

static void print_sock_info(ab_socket_t sock, const char *msg) {
    char sock_info[64];
    memset(sock_info, 0, sizeof(sock_info));
    get_sock_info(sock, sock_info, sizeof(sock_info));
    AB_LOGGER_DEBUG("[%s], %s\n", sock_info, msg);
}

void fill_rtsp_interleave_frame(
    ab_rtsp_interleaved_frame_t *interleaved_frame, 
    unsigned short data_len) {
    assert(interleaved_frame);

    interleaved_frame->dollar_sign          = 0x24;
    interleaved_frame->channel_identifier   = 0x0;
    interleaved_frame->data_length          = htons(data_len);
}

void fill_rtp_header(ab_rtp_header_t *rtp_header, 
    unsigned int sequence, unsigned int timestamp) {
    if (rtp_header) {
        rtp_header->csrc_len      = 0;
        rtp_header->extension     = 0;
        rtp_header->padding       = 0;
        rtp_header->version       = RTP_VERSION;
        rtp_header->payload_type  = RTP_PAYLOAD_TYPE_H264;
        rtp_header->marker        = 0;
        rtp_header->seq           = htons(sequence);
        rtp_header->timestamp     = htonl(timestamp);
        rtp_header->ssrc          = htonl(0x88923423);
    }
}

void accept_func(void *sock, void *user_data) {
    assert(sock);
    assert(user_data);

    print_sock_info(sock, "new connection.");

    T rtsp = (T) user_data;

    ab_rtsp_client_t *new_client;
    NEW(new_client);

    new_client->sock    = sock;
    new_client->video_codec = rtsp->video_codec;
    new_client->ready   = false;
    new_client->method  = AB_RTSP_OVER_NONE;

    pthread_mutex_lock(&rtsp->mutex);
    rtsp->clients = list_push(rtsp->clients, new_client);
    pthread_mutex_unlock(&rtsp->mutex);
}

bool start_code3(const unsigned char *data, unsigned int data_size) {
    if (data_size >= 3 &&
        0x00 == data[0] && 0x00 == data[1] && 0x01 == data[2])
        return true;
    return false;
}

bool start_code4(const unsigned char *data, unsigned int data_size) {
    if (data_size >= 4 &&
        0x00 == data[0] && 0x00 == data[1] &&
        0x00 == data[2] && 0x01 == data[3]) {
        return true;
    }
    return false;
}

int find_start_code(const unsigned char *data, unsigned int data_size) {
    int result = -1;
    if (data_size >= 4) {
        unsigned int i;
        for (i = 0; i < data_size - 4; ++i) {
            if (start_code3(data + i, 3) || start_code4(data + i, 4)) {
                break;
            }
        }

        if ((i < data_size - 4) || start_code3(data + i, 3)) {
            result = i;
        }
    }

    return result;
}

static void send_rtp_to_client(list_t clients, ab_udp_client_t rtp_udp_srv,
    const unsigned char *data, unsigned int data_len) {
    list_t node = clients;
    while(node) {
        ab_rtsp_client_t *rtsp_client = node->first;
        if (rtsp_client->ready && rtsp_client->sock) {
            if (AB_RTSP_OVER_UDP == rtsp_client->method) {
                char addr_buf[32];
                ab_socket_addr(rtsp_client->sock, 
                    addr_buf, sizeof(addr_buf));
                ab_udp_client_send(rtp_udp_srv, 
                    addr_buf, rtsp_client->rtp_chn_port,
                    data + sizeof(ab_rtsp_interleaved_frame_t), 
                    data_len - sizeof(ab_rtsp_interleaved_frame_t));
            } else if (AB_RTSP_OVER_TCP == rtsp_client->method) {
                int nsend = ab_socket_send(rtsp_client->sock, data, data_len);
                if (nsend != data_len)
                    AB_LOGGER_DEBUG("ab_socket_send error.\n");
            }
        }
        node = node->rest;
    }
}

static void set_h264_slice_header(unsigned char *slice_header, int nalu_type,
    int slice_total, int slice_index) {
    slice_header[0] = (nalu_type & 0x60) | 0x1c;
    slice_header[1] = nalu_type & 0x1f;

    if (0 == slice_index) {
        slice_header[1] |= 0x80;
    } else if (slice_total - 1 == slice_index) {
        slice_header[1] |= 0x40;
    }
}

static void set_h265_slice_header(unsigned char *slice_header, int nalu_type,
    int slice_total, int slice_index) {
    slice_header[0] = 0x62;
    slice_header[1] = 1;

    slice_header[2] = (nalu_type >> 1) & 0x3f;
    if (0 == slice_index) {
        slice_header[2] |= 0x80;
    } else if (slice_total - 1 == slice_index) {
        slice_header[2] |= 0x40;
    }
}

void rtp_send_nalu(T rtsp, 
    const unsigned char *nalu, unsigned int nalu_len) {
    assert(rtsp);
    assert(nalu && nalu_len > 0);

    int nalu_type = nalu[0];
    if (nalu_len < RTP_MAX_SIZE) {
        fill_rtsp_interleave_frame(
            (ab_rtsp_interleaved_frame_t *) rtsp->rtp_buffer.data,
            sizeof(ab_rtp_header_t) + nalu_len);
        rtsp->rtp_buffer.used = sizeof(ab_rtsp_interleaved_frame_t);

        fill_rtp_header(
            (ab_rtp_header_t *) (rtsp->rtp_buffer.data + rtsp->rtp_buffer.used), 
            rtsp->sequence, rtsp->timestamp);
        rtsp->rtp_buffer.used += sizeof(ab_rtp_header_t);

        memcpy(rtsp->rtp_buffer.data + rtsp->rtp_buffer.used, nalu, nalu_len);
        rtsp->rtp_buffer.used += nalu_len;

        pthread_mutex_lock(&rtsp->mutex);
        send_rtp_to_client(rtsp->clients, rtsp->rtp_udp_srv, 
            rtsp->rtp_buffer.data, rtsp->rtp_buffer.used);
        pthread_mutex_unlock(&rtsp->mutex);

        ++rtsp->sequence;
    } else {
        unsigned int header_len = 0, data_offset = 0;
        if (AB_VIDEO_CODEC_H264 == rtsp->video_codec) {
            header_len = 2;
            data_offset = 1;
        } else if (AB_VIDEO_CODEC_H265 == rtsp->video_codec) {
            header_len = 3;
            data_offset = 2;
        }

        int slice_num = (nalu_len - data_offset) / RTP_MAX_SIZE;
        int remain = (nalu_len - data_offset) % RTP_MAX_SIZE;
        if (remain) {
            ++slice_num;
        }

        for (int i = 0; i < slice_num; ++i) {
            unsigned int pkg_data_len = 0;
            if (i < slice_num - 1 || 0 == remain) {
                pkg_data_len = RTP_MAX_SIZE;
            } else {
                pkg_data_len = remain;
            }

            fill_rtsp_interleave_frame(
                (ab_rtsp_interleaved_frame_t *) rtsp->rtp_buffer.data,
                pkg_data_len + sizeof(ab_rtp_header_t) + header_len);
            rtsp->rtp_buffer.used = sizeof(ab_rtsp_interleaved_frame_t);

            fill_rtp_header(
                (ab_rtp_header_t *) (rtsp->rtp_buffer.data + rtsp->rtp_buffer.used), 
                rtsp->sequence, rtsp->timestamp);
            rtsp->rtp_buffer.used += sizeof(ab_rtp_header_t);

            if (AB_VIDEO_CODEC_H264 == rtsp->video_codec) {
                set_h264_slice_header(rtsp->rtp_buffer.data + rtsp->rtp_buffer.used,
                    nalu_type, slice_num, i);
            } else if (AB_VIDEO_CODEC_H265 == rtsp->video_codec) {
                set_h265_slice_header(rtsp->rtp_buffer.data + rtsp->rtp_buffer.used,
                    nalu_type, slice_num, i);
            }

            rtsp->rtp_buffer.used += header_len;

            memcpy(rtsp->rtp_buffer.data + rtsp->rtp_buffer.used, 
                nalu + i * RTP_MAX_SIZE + data_offset, pkg_data_len);
            rtsp->rtp_buffer.used += pkg_data_len;

            pthread_mutex_lock(&rtsp->mutex);
            send_rtp_to_client(rtsp->clients, rtsp->rtp_udp_srv, 
                rtsp->rtp_buffer.data, rtsp->rtp_buffer.used);
            pthread_mutex_unlock(&rtsp->mutex);

            ++rtsp->sequence;
        }
    }

    if ((nalu_type & 0x1f) != 7 && (nalu_type & 0x1f) != 8)
        rtsp->timestamp += 90000 / 25;
}

static list_t update_clients_list(list_t head) {
    while (head) {
        ab_rtsp_client_t *client = head->first;
        if (NULL == client->sock) {
            list_t del_node = head;
            head = head->rest;
            FREE(del_node->first);
            FREE(del_node);
        } else {
            break;
        }
    }

    list_t result = head;
    while (head && head->rest) {
        ab_rtsp_client_t *client = head->rest->first;
        if (NULL == client->sock) {
            list_t del_node = head->rest;
            head->rest = head->rest->rest;
            FREE(del_node->first);
            FREE(del_node);
        } else {
            head = head->rest;
        }
    }

    return result;
}

static int handle_cmd_options(char *buf, unsigned int buf_size,
        unsigned int cseq) {
    snprintf(buf, buf_size, 
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %u\r\n"
        "Public: OPTIONS, DESCRIBE, SETUP, "
        "PLAY, TEARDOWN\r\n\r\n", cseq);
    return strlen(buf);
}

static int handle_cmd_describe(char *buf, unsigned int buf_size,
    const char *url, unsigned int cseq, int video_codec) {
    char sdp[256];
    char local_ip[32];
    sscanf(url, "rtsp://%[^:]:", local_ip);

    if (AB_VIDEO_CODEC_H264 == video_codec) {
        snprintf(sdp, sizeof(sdp), 
            "v=0\r\n"
            "o=- 9%ld 1 IN IP4 %s\r\n"
            "t=0 0\r\n"
            "a=control:*\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=control:track0\r\n", time(NULL), local_ip);
    } else if (AB_VIDEO_CODEC_H265 == video_codec) {
        snprintf(sdp, sizeof(sdp), 
            "v=0\r\n"
            "o=- 9%ld 1 IN IP4 %s\r\n"
            "t=0 0\r\n"
            "a=control:*\r\n"
            "m=video 0 RTP/AVP 96\r\n"
            "a=rtpmap:96 H265/90000\r\n"
            "a=control:track0\r\n", time(NULL), local_ip);
    }

    snprintf(buf, buf_size, 
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %u\r\n"
        "Content-Base: %s\r\n"
        "Content-type: application/sdp\r\n"
        "Content-length: %lu\r\n\r\n"
        "%s", cseq, url, strlen(sdp), sdp);
    return strlen(buf);
}

static int handle_cmd_setup(char *buf, unsigned int buf_size,
    unsigned int cseq, int rtsp_over,
    unsigned short rtp_client_port, unsigned short rtcp_client_port) {
   if (AB_RTSP_OVER_UDP == rtsp_over) {
        snprintf(buf, buf_size, 
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %u\r\n"
            "Transport: RTP/AVP;unicast;"
            "client_port=%u-%u;server_port=%u-%u\r\n"
            "Session: 66334873\r\n\r\n",
            cseq, rtp_client_port, rtcp_client_port, 
            RTP_SERVER_PORT, RTCP_SERVER_PORT);
   } else if (AB_RTSP_OVER_TCP == rtsp_over) {
        snprintf(buf, buf_size, 
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %u\r\n"
            "Transport: RTP/AVP/TCP;unicast;"
            "interleaved=%u-%u\r\n"
            "Session: 66334873\r\n\r\n", 
            cseq, rtp_client_port, rtcp_client_port);
    } else {
        buf[0] = '\0';
    }

    return strlen(buf);
}

static int handle_cmd_play(char *buf, unsigned int buf_size,
    unsigned int cseq) {
    snprintf(buf, buf_size, 
        "RTSP/1.0 200 OK\r\n"
        "CSeq: %u\r\n"
        "Range: npt=0.000-\r\n"
        "Session: 66334873; timeout=60\r\n\r\n", cseq);
    return strlen(buf);
}

static int handle_cmd_teardown(char *buf, unsigned int buf_size, 
    unsigned int cseq) {
    snprintf(buf, buf_size,
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %u\r\n"
            "Session: 66334873\r\n\r\n", cseq);
    return strlen(buf);
}

static int handle_cmd_not_supported(char *buf, unsigned int buf_size,
    unsigned int cseq) {
    snprintf(buf, buf_size,
            "RTSP/1.0 551 Option not supported\r\n"
            "CSeq: %u\r\n"
            "Session: 66334873\r\n\r\n", cseq);
    return strlen(buf);
}

static int process_client_request(ab_rtsp_client_t *client, 
    const char *request, unsigned int request_len, 
    char *response, unsigned int response_size) {
    char method[16];
    char url[128];
    char version[16];
    memset(method, 0, sizeof(method));
    memset(url, 0, sizeof(url));
    memset(version, 0, sizeof(version));

    sscanf(request, "%s %s %s\r\n", method, url, version);

    unsigned int cseq = 0;
    const char *line = strstr(request, "CSeq");
    if (NULL == line) {
        return 0;
    } else {
        sscanf(line, "CSeq: %u\r\n", &cseq);
    }

    int len = 0;
    if (strcmp(method, "OPTIONS") == 0) {
        len = handle_cmd_options(response, response_size, cseq);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        len = handle_cmd_describe(response, response_size, url, cseq, client->video_codec);
    } else if (strcmp(method, "SETUP") == 0) {
        line = strstr(request, "Transport");

        if (strstr(line, "RTP/AVP/TCP") != NULL) {
            client->method = AB_RTSP_OVER_TCP;
            sscanf(line, "Transport: RTP/AVP/TCP;unicast;interleaved=%hu-%hu\r\n", 
                &client->rtp_chn_port, &client->rtcp_chn_port);
        } else if (strstr(line, "RTP/AVP/UDP") != NULL) {
            client->method = AB_RTSP_OVER_UDP;
            sscanf(line, "Transport: RTP/AVP/UDP;unicast;client_port=%hu-%hu\r\n", 
                &client->rtp_chn_port, &client->rtcp_chn_port);
        } else if (strstr(line, "RTP/AVP") != NULL) {
            client->method = AB_RTSP_OVER_UDP;
            sscanf(line, "Transport: RTP/AVP;unicast;client_port=%hu-%hu\r\n", 
                &client->rtp_chn_port, &client->rtcp_chn_port);
        } else {
            return 0;
        }

        len = handle_cmd_setup(response, response_size, cseq, client->method, 
            client->rtp_chn_port, client->rtcp_chn_port);
    } else if (strcmp(method, "PLAY") == 0) {
        len = handle_cmd_play(response, response_size, cseq);
        client->ready = true;
    } else if (strcmp(method, "TEARDOWN") == 0) {
        // IINA测试响应TEARDOWN会收到SIGPIPE信号，导致程序异常退出
        // len = handle_cmd_teardown(response, response_size, cseq);
        len = 0;
        client->ready = false;
    } else {
        len = handle_cmd_not_supported(response, response_size, cseq);
    }

    return len;
}

static void recv_client_msg(ab_rtsp_client_t *client) {
    assert(client);

    char request[4096];
    memset(request, 0, sizeof(request));
    int nread = ab_socket_recv(client->sock, (unsigned char *) request, sizeof(request));
    if (nread < 0) {
        AB_LOGGER_ERROR("return %d, %s.\n", nread, strerror(errno));
    } else if (0 == nread) {
        print_sock_info(client->sock, "close connection.");
        ab_socket_free(&client->sock);
    } else {
        AB_LOGGER_DEBUG("request:\n%s\n", request);
        const unsigned int response_size = 1024;
        char response[response_size];
        memset(response, 0, response_size);
        int len = process_client_request(client, request, nread,
            response, response_size);
        AB_LOGGER_DEBUG("response:\n%s\n", response);
        if (len > 0) {
            int nsend = ab_socket_send(client->sock, (unsigned char *) response, len);
            if (nsend != len) {
                AB_LOGGER_DEBUG("ab_socket_send error.\n");
            }
        }
    }
}

void *event_looper_cb(void *arg) {
    assert(arg);

    T rtsp = (T) arg;

    while (!rtsp->quit) {
        fd_set rfds;
        int max_fd = -1;

        FD_ZERO(&rfds);
        pthread_mutex_lock(&rtsp->mutex);
        list_t client = rtsp->clients;
        while (client) {
            ab_rtsp_client_t *rtsp_client = client->first;
            int fd = ab_socket_fd(rtsp_client->sock);
            FD_SET(fd, &rfds);
            if (fd > max_fd) {
                max_fd = fd;
            }
            client = client->rest;
        }
        pthread_mutex_unlock(&rtsp->mutex);

        if (-1 == max_fd) {
            usleep(50 * 1000);
        } else {
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = 50 * 1000;
            int nums = select(max_fd + 1, &rfds, NULL, NULL, &timeout);
            if (nums < 0) {
                break;
            } else if (0 == nums) {
                continue;
            }

            pthread_mutex_lock(&rtsp->mutex);
            list_t client = rtsp->clients;
            while (client) {
                ab_rtsp_client_t *rtsp_client = client->first;
                int fd = ab_socket_fd(rtsp_client->sock);
                if (FD_ISSET(fd, &rfds)) {
                    recv_client_msg(rtsp_client);
                }

                client = client->rest;
            }
            rtsp->clients = update_clients_list(rtsp->clients);
            pthread_mutex_unlock(&rtsp->mutex);
        }
    }

    return NULL;
}