/*
 * Copyright (c) The ThingSet Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define MSG_END      (0x0A)
#define MSG_SKIP     (0x0D)
#define MSG_ESC      (0xCE)
#define MSG_ESC_END  (0xCA)
#define MSG_ESC_SKIP (0xCD)
#define MSG_ESC_ESC  (0xCF)

int packetize(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_len, int *src_pos)
{
    int pos_buf = *src_pos;
    int pos_chunk = 0;
    if (pos_buf == 0) {
        dst[pos_chunk++] = MSG_END;
    }

    while (pos_chunk < dst_len && pos_buf < src_len) {
        if (src[pos_buf] == MSG_END) {
            dst[pos_chunk++] = MSG_ESC;
            dst[pos_chunk++] = MSG_ESC_END;
        }
        else if (src[pos_buf] == MSG_SKIP) {
            dst[pos_chunk++] = MSG_ESC;
            dst[pos_chunk++] = MSG_ESC_SKIP;
        }
        else if (src[pos_buf] == MSG_ESC) {
            dst[pos_chunk++] = MSG_ESC;
            dst[pos_chunk++] = MSG_ESC_ESC;
        }
        else {
            dst[pos_chunk++] = src[pos_buf];
        }
        pos_buf++;
    }
    if (pos_chunk < dst_len - 1 && pos_buf == src_len) {
        dst[pos_chunk++] = MSG_END;
        pos_buf++;
    }

    (*src_pos) = pos_buf;
    return pos_chunk;
}
