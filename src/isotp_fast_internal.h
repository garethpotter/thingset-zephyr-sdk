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

/**
 * Internal send context. Used to manage the transmission of a single
 * message greater than 1 CAN frame in size.
 */
struct isotp_fast_send_ctx
{
    sys_snode_t node;                 /**< linked list node in @ref isotp_send_ctx_list */
    struct isotp_fast_ctx *ctx;       /**< pointer to bound context */
    isotp_fast_msg_id recipient_addr; /**< CAN ID used on sent message frames */
    struct k_work work;
    struct k_timer timer;          /**< handles timeouts */
    struct k_sem sem;              /**< used to ensure CF frames are sent in order */
    const uint8_t *data;           /**< source message buffer */
    uint16_t rem_len : 12;         /**< length of buffer; max len 4095 */
    enum isotp_tx_state state : 8; /**< current state of context */
    int8_t error;
    void *cb_arg;                  /**< supplied to sent_callback */
    uint8_t wft;
    uint8_t bs;
    uint8_t sn : 4; /**< sequence number; overflows at 4 bits per spec */
    uint8_t backlog;
    uint8_t stmin;
};

/**
 * Internal receive context. Used to manage the receipt of a single
 * message.
 */
struct isotp_fast_recv_ctx
{
    sys_snode_t node;              /**< linked list node in @ref isotp_recv_ctx_list */
    struct isotp_fast_ctx *ctx;    /**< pointer to bound context */
    isotp_fast_msg_id sender_addr; /**< CAN ID on received frames */
    struct k_work work;
    struct k_timer timer;          /**< handles timeouts */
    struct net_buf *buffer;        /**< head node of buffer */
    struct net_buf *frag;          /**< current fragment */
#ifdef CONFIG_ISOTP_FAST_PER_FRAME_DISPATCH
    struct k_msgq recv_queue;
    uint8_t recv_queue_pool[sizeof(struct net_buf *) * CONFIG_ISOTP_FAST_RX_MAX_PACKET_COUNT * 2];
#endif
    uint16_t rem_len : 12;         /**< remaining length of incoming message */
    enum isotp_rx_state state : 8; /**< current state of context */
    int8_t error;
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