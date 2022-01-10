/*
 * ab_rtp_def.h
 *
 *  Created on: 2022年1月7日
 *      Author: ljm
 */

#ifndef AB_RTP_DEF_H_
#define AB_RTP_DEF_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define RTP_VERSION             2
#define RTP_PAYLOAD_TYPE_H264   96
#define RTP_PAYLOAD_TYPE_AAC    97
#define RTP_MAX_PACKET_SIZE     1360

typedef struct ab_rtp_header_t {
    uint8_t csrc_len:4;
    uint8_t extension:1;
    uint8_t padding:1;
    uint8_t version:2;

    uint8_t payload_type:7;
    uint8_t marker:1;

    uint16_t seq;

    uint32_t timestamp;
    uint32_t ssrc;
} ab_rtp_header_t;

typedef struct ab_rtp_packet_t {
    uint8_t header[4];
    ab_rtp_header_t rtp_header;
    uint8_t payload[0];
} ab_rtp_packet_t;

#ifdef __cplusplus
}
#endif

#endif // AB_RTP_DEF_H_