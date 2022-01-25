/*
 * ab_rtsp.c
 *
 *  Created on: 2022年1月7日
 *      Author: ljm
 */

#include "ab_rtsp.h"
#include "ab_rtp_def.h"

#include "ab_base/ab_list.h"
#include "ab_base/ab_mem.h"
#include "ab_base/ab_assert.h"

#include "ab_log/ab_logger.h"

#include "ab_net/ab_tcp_server.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#include <sys/select.h>

#include <arpa/inet.h>

#define T ab_rtsp_t

static unsigned short rtp_server_port = 20001;
static unsigned short rtcp_server_port = 20002;
static unsigned short rtsp_port = 554;

typedef struct ab_rtsp_buffer_t {
    void *data;
    int size;
    int used;
} ab_rtsp_buffer_t;

typedef struct ab_select_args_t {
    int max_fd;
    fd_set rfds;
} ab_select_args_t;

struct T {
    int method;

    ab_tcp_server_t srv;

    list_t clients_all;
    list_t clients_already;

    ab_select_args_t slt_args;

    bool quit;
    pthread_mutex_t mutex;
    pthread_t rtsp_thd;

    ab_rtsp_buffer_t buffer;
    ab_rtp_packet_t *rtp_pkt;
    unsigned int rtp_pkt_len;
};

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

static int handle_cmd_options(char *buf, unsigned int buf_size,
                              unsigned int cseq) {
    snprintf(buf, buf_size, "RTSP/1.0 200 OK\r\n"
                            "CSeq: %u\r\n"
                            "Public: OPTIONS, DESCRIBE, SETUP, "
                            "TEARDOWN, PLAY\r\n\r\n",
                            cseq);
    return strlen(buf);
}

static int handle_cmd_describe(char *buf, unsigned int buf_size,
                               const char *url, unsigned int cseq) {
    char sdp[256];
    char local_ip[32];
    sscanf(url, "rtsp://%[^:]:", local_ip);

    snprintf(sdp, sizeof(sdp), "v=0\r\n"
                               "o=- 9%ld 1 IN IP4 %s\r\n"
                               "t=0 0\r\n"
                               "a=control:*\r\n"
                               "m=video 0 RTP/AVP 96\r\n"
                               "a=rtpmap:96 H264/90000\r\n"
                               "a=control:track0\r\n",
                               time(NULL), local_ip);

    snprintf(buf, buf_size, "RTSP/1.0 200 OK\r\n"
                            "CSeq: %u\r\n"
                            "Content-Base: %s\r\n"
                            "Content-type: application/sdp\r\n"
                            "Content-length: %lu\r\n\r\n"
                            "%s",
                            cseq, url, strlen(sdp), sdp);
    return strlen(buf);
}

static int handle_cmd_setup(char *buf, unsigned int buf_size,
                            unsigned int cseq, int rtsp_over,
                            unsigned short rtp, unsigned short rtcp) {
   if (AB_RTSP_OVER_UDP == rtsp_over) {
       snprintf(buf, buf_size, "RTSP/1.0 200 OK\r\n"
                               "CSeq: %u\r\n"
                               "Transport: RTP/AVP;unicast;"
                               "client_port=%u-%u;server_port=%u-%u\r\n"
                               "Session: 66334873\r\n\r\n",
                               cseq, rtp, rtcp,
                               rtp_server_port, rtcp_server_port);
   } else if (AB_RTSP_OVER_TCP == rtsp_over) {
       snprintf(buf, buf_size, "RTSP/1.0 200 OK\r\n"
                               "CSeq: %u\r\n"
                               "Transport: RTP/AVP/TCP;unicast;"
                               "interleaved=%u-%u\r\n"
                               "Session: 66334873\r\n\r\n",
                               cseq, rtp, rtcp);
    } else {
        buf[0] = '\0';
    }

    return strlen(buf);
}

static int handle_cmd_play(char *buf, unsigned int buf_size,
                           unsigned int cseq) {
    snprintf(buf, buf_size, "RTSP/1.0 200 OK\r\n"
                            "CSeq: %u\r\n"
                            "Range: npt=0.000-\r\n"
                            "Session: 66334873; timeout=60\r\n\r\n",
                            cseq);
    return strlen(buf);
}

static void process_rtsp_msg(ab_socket_t *sock, T rtsp) {
    assert(sock && *sock);

    bool already = false;

    char request[4096];
    memset(request, 0, sizeof(request));
    int nread = ab_socket_recv(*sock, request, sizeof(request));
    if (nread < 0)
        AB_LOGGER_ERROR("return %d.\n", nread);
    else if (0 == nread) {
        char sock_info[64];
        memset(sock_info, 0, sizeof(sock_info));
        get_sock_info(*sock, sock_info, sizeof(sock_info));
        AB_LOGGER_DEBUG("Close connection[%s]\n", sock_info);
        ab_socket_free(sock);
    } else {
        AB_LOGGER_DEBUG("request:\n%s\n", request);
        char method[16];
        char url[128];
        char version[16];
        memset(method, 0, sizeof(method));
        memset(url, 0, sizeof(url));
        memset(version, 0, sizeof(version));
        sscanf(request, "%s %s %s\r\n", method, url, version);

        unsigned int cseq = 0;
        const char *line = strstr(request, "CSeq");
        if (NULL == line)
            return;
        else
            sscanf(line, "CSeq: %u\r\n", &cseq);

        char response[1024];
        int len = 0;
        memset(response, 0, sizeof(response));

        if (strcmp(method, "OPTIONS") == 0)
            len = handle_cmd_options(response, sizeof(response), cseq);
        else if (strcmp(method, "DESCRIBE") == 0)
            len = handle_cmd_describe(response, sizeof(response), url, cseq);
        else if (strcmp(method, "SETUP") == 0) {
            unsigned short rtp_port = 0, rtcp_port = 1;
            line = strstr(request, "Transport");
            if (AB_RTSP_OVER_UDP == rtsp->method) {
                sscanf(line, "Transport: RTP/AVP;unicast;client_port=%hu-%hu\r\n",
                             &rtp_port, &rtcp_port);
            } else if (AB_RTSP_OVER_TCP == rtsp->method) {
                sscanf(line, "Transport: RTP/AVP/TCP;unicast;interleaved=%hu-%hu\r\n",
                             &rtp_port, &rtcp_port);
            }

            len = handle_cmd_setup(response, sizeof(response), cseq,
                                    rtsp->method, rtp_port, rtcp_port);
        } else if (strcmp(method, "PLAY") == 0) {
            len = handle_cmd_play(response, sizeof(response), cseq);
            already = true;
        } else {
            snprintf(response, sizeof(response),
                    "RTSP/1.0 501 OK\r\n"
                    "CSeq: %u\r\n\r\n", cseq);
            AB_LOGGER_DEBUG("Not implements method[%s]\n", method);
        }

        AB_LOGGER_DEBUG("response:\n%s\n", response);
        if (len > 0)
            ab_socket_send(*sock, response, len);

        if (already)
            rtsp->clients_already = list_push(rtsp->clients_already, *sock);
    }
}

static void check_rtsp_event(void **x, void *user_data) {
    assert(x && *x);
    assert(user_data);

    ab_socket_t sock = (ab_socket_t) *x;
    T rtsp = (T) user_data;

    int fd = ab_socket_fd(sock);
    if (FD_ISSET(fd, &rtsp->slt_args.rfds))
        process_rtsp_msg((ab_socket_t *) x, rtsp);
}

static void fill_set(void **x, void *user_data) {
    assert(x && *x);
    assert(user_data);

    ab_socket_t sock = (ab_socket_t) *x;
    ab_select_args_t *slt_args = (ab_select_args_t *) user_data;

    int fd = ab_socket_fd((ab_socket_t) *x);
    FD_SET(fd, &slt_args->rfds);
    if (fd > slt_args->max_fd)
        slt_args->max_fd = fd;
}

static list_t clients_clean_up(list_t head) {
    while (head && NULL == head->first) {
        list_t del_node = head;
        head = head->rest;
        FREE(del_node);
    }

    list_t result = head;
    while (head && head->rest) {
        if (NULL == head->rest->first) {
            list_t del_node = head->rest;
            head->rest = head->rest->rest;
            FREE(del_node);
        } else
            head = head->rest;
    }

    return result;
}

static void *rtsp_event_start_routine(void *arg) {
    assert(arg);

    T rtsp = (T) arg;

    struct timeval timeout;
    while (!rtsp->quit) {
        FD_ZERO(&rtsp->slt_args.rfds);
        rtsp->slt_args.max_fd = -1;

        pthread_mutex_lock(&rtsp->mutex);
        list_map(rtsp->clients_all, fill_set, &rtsp->slt_args);
        pthread_mutex_unlock(&rtsp->mutex);

        if (-1 == rtsp->slt_args.max_fd) {
            usleep(50 * 1000);
        } else {
            timeout.tv_sec = 0;
            timeout.tv_usec = 50 * 1000;
            int nums = select(rtsp->slt_args.max_fd + 1,
                    &rtsp->slt_args.rfds, NULL, NULL, &timeout);
            if (nums < 0)
                break;
            else if (0 == nums)
                continue;

            pthread_mutex_lock(&rtsp->mutex);
            list_map(rtsp->clients_all, check_rtsp_event, rtsp);
            rtsp->clients_all = clients_clean_up(rtsp->clients_all);
            rtsp->clients_already = clients_clean_up(rtsp->clients_already);
            pthread_mutex_unlock(&rtsp->mutex);
        }
    }

    return NULL;
}

static void accept_func(ab_socket_t sock, void *user_data) {
    assert(sock);
    assert(user_data);

    char sock_info[64];
    memset(sock_info, 0, sizeof(sock_info));
    get_sock_info(sock, sock_info, sizeof(sock_info));
    AB_LOGGER_DEBUG("New connection[%s]\n", sock_info);

    T rtsp = (T) user_data;

    pthread_mutex_lock(&rtsp->mutex);
    rtsp->clients_all = list_push(rtsp->clients_all, sock);
    pthread_mutex_unlock(&rtsp->mutex);
}

static void print_data(const char *data, unsigned int data_size) {
    printf("\n**************************************");
    for (unsigned int i = 0; i < data_size; i++) {
        if (0 == i % 40)
            printf("\n");
        unsigned char c = data[i];
        printf("%02x ", (unsigned int)c);
    }
    printf("\n**************************************\n");
}

static void rtp_sender_apply(void **x, void *user_data) {
    assert(x && *x);
    assert(user_data);

    ab_socket_t sock = (ab_socket_t) *x;
    ab_rtsp_buffer_t *buf = (ab_rtsp_buffer_t *) user_data;

    // print_data(buf->data, buf->used);

    ab_socket_send(sock, buf->data, buf->used);
}

static void rtp_sender_func(T rtsp, const char *data, unsigned int data_size) {
    assert(rtsp);
    assert(data);
    assert(data_size > 0);

    pthread_mutex_lock(&rtsp->mutex);
    if (list_length(rtsp->clients_already) == 0) {
        pthread_mutex_unlock(&rtsp->mutex);
        return;
    }
    pthread_mutex_unlock(&rtsp->mutex);

    int nalu_type = data[0];
    ab_rtsp_buffer_t buf;
    if (data_size < RTP_MAX_PACKET_SIZE) {
        rtsp->rtp_pkt->header[0]    = 0x24;
        rtsp->rtp_pkt->header[1]    = 0x0;
        rtsp->rtp_pkt->header[2]    =
                        ((data_size + sizeof(ab_rtp_header_t)) & 0xff00) >> 8;
        rtsp->rtp_pkt->header[3]    =
                        (data_size + sizeof(ab_rtp_header_t)) & 0xff;

        rtsp->rtp_pkt->rtp_header.seq       = htons(rtsp->rtp_pkt->rtp_header.seq);
        rtsp->rtp_pkt->rtp_header.timestamp = htonl(rtsp->rtp_pkt->rtp_header.timestamp);
        rtsp->rtp_pkt->rtp_header.ssrc      = htonl(rtsp->rtp_pkt->rtp_header.ssrc);

        memcpy(rtsp->rtp_pkt->payload, data, data_size);
        buf.size = buf.used = 0;
        if (AB_RTSP_OVER_TCP == rtsp->method) {
            buf.data = rtsp->rtp_pkt;
            buf.size = buf.used = data_size + sizeof(ab_rtp_header_t) + 4;
        } else if (AB_RTSP_OVER_UDP == rtsp->method) {
            buf.data = (void *) &rtsp->rtp_pkt->rtp_header;
            buf.size = buf.used = data_size + sizeof(ab_rtp_header_t);
        }

        pthread_mutex_lock(&rtsp->mutex);
        list_map(rtsp->clients_already, rtp_sender_apply, &buf);
        pthread_mutex_unlock(&rtsp->mutex);

        rtsp->rtp_pkt->rtp_header.seq       = ntohs(rtsp->rtp_pkt->rtp_header.seq);
        rtsp->rtp_pkt->rtp_header.timestamp = ntohl(rtsp->rtp_pkt->rtp_header.timestamp);
        rtsp->rtp_pkt->rtp_header.ssrc      = ntohl(rtsp->rtp_pkt->rtp_header.ssrc);

        rtsp->rtp_pkt->rtp_header.seq++;
    } else {
        unsigned send_data_size = data_size - 1;
        unsigned int pos = 1;

        int pkt_num = send_data_size / RTP_MAX_PACKET_SIZE;
        if (send_data_size % RTP_MAX_PACKET_SIZE != 0)
            pkt_num++;

        for (int i = 0; i < pkt_num; i++) {
            unsigned short rtp_pkt_size = 0;
            if (i < pkt_num - 1 || send_data_size % RTP_MAX_PACKET_SIZE == 0)
                rtp_pkt_size = RTP_MAX_PACKET_SIZE + sizeof(ab_rtp_header_t) + 2;
            else
                rtp_pkt_size = (send_data_size % RTP_MAX_PACKET_SIZE) + sizeof(ab_rtp_header_t) + 2;

            rtsp->rtp_pkt->header[0]    = 0x24;
            rtsp->rtp_pkt->header[1]    = 0x0;
            rtsp->rtp_pkt->header[2]    = (rtp_pkt_size & 0xff00) >> 8;
            rtsp->rtp_pkt->header[3]    = rtp_pkt_size & 0xff;

            rtsp->rtp_pkt->rtp_header.seq       = htons(rtsp->rtp_pkt->rtp_header.seq);
            rtsp->rtp_pkt->rtp_header.timestamp = htonl(rtsp->rtp_pkt->rtp_header.timestamp);
            rtsp->rtp_pkt->rtp_header.ssrc      = htonl(rtsp->rtp_pkt->rtp_header.ssrc);

            rtsp->rtp_pkt->payload[0] = (nalu_type & 0x60) | 0x1c;
            rtsp->rtp_pkt->payload[1] = nalu_type & 0x1f;

            if (0 == i)
                rtsp->rtp_pkt->payload[1] |= 0x80;
            else if (i == pkt_num - 1)
                rtsp->rtp_pkt->payload[1] |= 0x40;

            pos += RTP_MAX_PACKET_SIZE;
            if (i < pkt_num - 1 || send_data_size % RTP_MAX_PACKET_SIZE == 0)
                memcpy(rtsp->rtp_pkt->payload + 2, data + pos, RTP_MAX_PACKET_SIZE);
            else
                memcpy(rtsp->rtp_pkt->payload + 2, data + pos, send_data_size % RTP_MAX_PACKET_SIZE);

            buf.size = buf.used = 0;
            if (AB_RTSP_OVER_TCP == rtsp->method) {
                buf.data = rtsp->rtp_pkt;
                buf.size = buf.used = rtp_pkt_size + 4;
            } else if (AB_RTSP_OVER_UDP == rtsp->method) {
                buf.data = &rtsp->rtp_pkt->rtp_header;
                buf.size = buf.used = rtp_pkt_size;
            }

            pthread_mutex_lock(&rtsp->mutex);
            list_map(rtsp->clients_already, rtp_sender_apply, &buf);
            pthread_mutex_unlock(&rtsp->mutex);

            rtsp->rtp_pkt->rtp_header.seq       = ntohs(rtsp->rtp_pkt->rtp_header.seq);
            rtsp->rtp_pkt->rtp_header.timestamp = ntohl(rtsp->rtp_pkt->rtp_header.timestamp);
            rtsp->rtp_pkt->rtp_header.ssrc      = ntohl(rtsp->rtp_pkt->rtp_header.ssrc);

            rtsp->rtp_pkt->rtp_header.seq++;
        }
    }

    if ((nalu_type & 0x1f) != 7 && (nalu_type & 0x1f) != 8)
        rtsp->rtp_pkt->rtp_header.timestamp += 90000 / 25;
}

static bool start_code3(const char *data, unsigned int data_size) {
    if (data_size >= 3 &&
        0x00 == data[0] && 0x00 == data[1] && 0x01 == data[2])
        return true;
    return false;
}

static bool start_code4(const char *data, unsigned int data_size) {
    if (data_size >= 4 &&
        0x00 == data[0] && 0x00 == data[1] &&
        0x00 == data[2] && 0x01 == data[3])
        return true;
    return false;
}

static int find_start_code(const char *data, unsigned int data_size) {
    if (data_size < 4)
        return -1;

    unsigned int i;
    for (i = 0; i < data_size - 4; i++)
        if (start_code3(data + i, 3) || start_code4(data + i, 4))
            break;

    if ((i < data_size - 4) || start_code3(data + i, 3))
        return i;
    return -1;
}

T ab_rtsp_new(int rtsp_over_method) {
    T rtsp;
    NEW(rtsp);
    assert(rtsp);

    rtsp->method = rtsp_over_method;
    rtsp->srv = ab_tcp_server_new(rtsp_port, accept_func, rtsp);

    rtsp->clients_all = NULL;
    rtsp->clients_already = NULL;

    pthread_mutex_init(&rtsp->mutex, NULL);

    rtsp->quit = false;
    pthread_create(&rtsp->rtsp_thd, NULL, rtsp_event_start_routine, rtsp);

    rtsp->buffer.size   = 1024 * 1024;
    rtsp->buffer.used   = 0;
    rtsp->buffer.data   = ALLOC(rtsp->buffer.size);

    rtsp->rtp_pkt =
        (ab_rtp_packet_t *) ALLOC(sizeof(ab_rtp_packet_t) + RTP_MAX_PACKET_SIZE);

    rtsp->rtp_pkt->rtp_header.csrc_len      = 0;
    rtsp->rtp_pkt->rtp_header.extension     = 0;
    rtsp->rtp_pkt->rtp_header.padding       = 0;
    rtsp->rtp_pkt->rtp_header.version       = RTP_VERSION;
    rtsp->rtp_pkt->rtp_header.payload_type  = RTP_PAYLOAD_TYPE_H264;
    rtsp->rtp_pkt->rtp_header.marker        = 0;
    rtsp->rtp_pkt->rtp_header.seq           = 0;
    rtsp->rtp_pkt->rtp_header.timestamp     = 0;
    rtsp->rtp_pkt->rtp_header.ssrc          = 0x88923423;

    rtsp->rtp_pkt_len                       = 0;

    return rtsp;
}

void ab_rtsp_free(T *rtsp) {
    assert(rtsp && *rtsp);

    (*rtsp)->quit = true;

    pthread_join((*rtsp)->rtsp_thd, NULL);
    pthread_mutex_destroy(&(*rtsp)->mutex);

    FREE((*rtsp)->rtp_pkt);
    FREE((*rtsp)->buffer.data);

    while ((*rtsp)->clients_all) {
        ab_socket_t sock = NULL;
        (*rtsp)->clients_all = list_pop((*rtsp)->clients_all, (void **) &sock);
        ab_socket_free(&sock);
    }

    list_free(&(*rtsp)->clients_already);

    ab_tcp_server_free(&(*rtsp)->srv);

    FREE(*rtsp);
}

int ab_rtsp_send(T rtsp, const char *data, unsigned int data_size) {
    if (NULL == data || 0 == data_size) {
        if (rtsp->buffer.used > 0) {
            int first_start_code_pos = find_start_code(rtsp->buffer.data, rtsp->buffer.used);
            if (0 == first_start_code_pos) {
                int start_code = 0;
                if (start_code3(rtsp->buffer.data, rtsp->buffer.used))
                    start_code = 3;
                else if (start_code4(rtsp->buffer.data, rtsp->buffer.used))
                    start_code = 4;

                rtp_sender_func(rtsp, rtsp->buffer.data + start_code, rtsp->buffer.used - start_code);
                rtsp->buffer.used = 0;
            }
        }

        return 0;
    }

    if (rtsp->buffer.size - rtsp->buffer.used >= data_size) {
        memcpy(rtsp->buffer.data + rtsp->buffer.used, data, data_size);
        rtsp->buffer.used += data_size;
    } else {
        AB_LOGGER_WARN("Not enough spaces.\n");
        return 0;
    }

    unsigned int start_pos = 0;
    while (start_pos < rtsp->buffer.used) {
        int first_start_code_pos =
            find_start_code(rtsp->buffer.data + start_pos, rtsp->buffer.used - start_pos);
        if (0 == first_start_code_pos) {
            unsigned int start_code = 0;
            if (start_code3(rtsp->buffer.data + start_pos, rtsp->buffer.used - start_pos))
                start_code = 3;
            else if (start_code4(rtsp->buffer.data + start_pos, rtsp->buffer.used - start_pos))
                start_code = 4;
            else
                continue;

            int next_start_code_pos =
                find_start_code(rtsp->buffer.data + start_pos + start_code,
                                rtsp->buffer.used - start_pos - start_code);
            if (-1 == next_start_code_pos) {
                if (start_pos != 0) {
                    rtsp->buffer.used -= start_pos;
                    memcpy(rtsp->buffer.data, rtsp->buffer.data + start_pos, rtsp->buffer.used);
                }
                break;
            } else {
                // print_data(rtsp->buffer.data + start_pos + start_code, next_start_code_pos);
                rtp_sender_func(rtsp, rtsp->buffer.data + start_pos + start_code, next_start_code_pos);
                start_pos += start_code + next_start_code_pos;
            }
        } else
            break;
    }

    return data_size;
}
