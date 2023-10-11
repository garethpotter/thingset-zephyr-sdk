/*
 * Copyright (c) 2023 Brill Power
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "../../conformance/src/main.h"

static int blocking_recv(uint8_t *buf, size_t size, k_timeout_t timeout)
{
    struct can_filter sender = {
        .id = 0,
        .mask = 0,
    };
    return isotp_fast_recv(&ctx, sender, buf, size, timeout);
}

void isotp_fast_recv_handler(struct net_buf *buffer, int rem_len, isotp_fast_msg_id sender_addr,
                             void *arg)
{}

void isotp_fast_recv_error_handler(int8_t error, isotp_fast_msg_id sender_addr, void *arg)
{}