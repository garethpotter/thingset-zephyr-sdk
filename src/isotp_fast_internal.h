/*
 * Copyright (c) 2023 Brill Power
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "isotp_internal.h"
#include <thingset/isotp_fast.h>
#include <zephyr/sys/slist.h>

#ifndef ISOTP_MSG_FDF
#define ISOTP_MSG_FDF BIT(3)
#endif

struct isotp_fast_send_ctx
{
    sys_snode_t node;
    struct isotp_fast_ctx *ctx;
    isotp_fast_msg_id recipient_addr;
    struct k_work work;
    struct k_timer timer;
    struct k_sem sem;
    /* source message buffer */
    const uint8_t *data;
    uint16_t rem_len : 12;
    enum isotp_tx_state state : 8;
    void *cb_arg;
    uint8_t wft;
    uint8_t bs;
    uint8_t sn : 4;
    uint8_t backlog;
    uint8_t stmin;
};

struct isotp_fast_recv_ctx
{
    sys_snode_t node;
    struct isotp_fast_ctx *ctx;
    isotp_fast_msg_id sender_addr;
    struct k_work work;
    struct k_timer timer;
    struct net_buf *buffer;
    struct net_buf *frag;
    uint16_t rem_len : 12;
    enum isotp_rx_state state : 8;
    uint8_t wft;
    uint8_t bs;
    uint8_t sn_expected : 4;
};

static inline isotp_fast_node_id isotp_fast_get_addr_sender(isotp_fast_msg_id addr)
{
    return (isotp_fast_node_id)(addr & ISOTP_FIXED_ADDR_SA_MASK);
}

static inline isotp_fast_node_id isotp_fast_get_frame_sender(struct can_frame *frame)
{
    return (isotp_fast_node_id)(frame->id & ISOTP_FIXED_ADDR_SA_MASK);
}

static inline isotp_fast_node_id isotp_fast_get_addr_recipient(isotp_fast_msg_id addr)
{
    return (isotp_fast_node_id)((addr & ISOTP_FIXED_ADDR_TA_MASK) >> ISOTP_FIXED_ADDR_TA_POS);
}