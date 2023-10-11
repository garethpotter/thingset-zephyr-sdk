/*
 * Copyright (c) 2023 Brill Power
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/canbus/isotp.h>

typedef uint8_t isotp_fast_node_id;
typedef uint32_t isotp_fast_msg_id;

typedef void (*isotp_fast_recv_callback_t)(struct net_buf *buffer, int rem_len,
                                           isotp_fast_msg_id sender_addr, void *arg);
typedef void (*isotp_fast_send_callback_t)(int result, void *arg);

struct isotp_fast_opts
{
    uint8_t bs;
    uint8_t stmin;
    uint8_t flags;
};

struct isotp_fast_ctx
{
    const struct device *can_dev;
    int filter_id;
    const struct isotp_fast_opts *opts;
    isotp_fast_recv_callback_t recv_callback;
    void *recv_cb_arg;
    isotp_fast_send_callback_t sent_callback;
    isotp_fast_msg_id my_addr;
};

int isotp_fast_bind(struct isotp_fast_ctx *ctx, const struct device *can_dev,
                    const isotp_fast_msg_id my_addr, const struct isotp_fast_opts *opts,
                    isotp_fast_recv_callback_t recv_callback, void *recv_cb_arg,
                    isotp_fast_send_callback_t sent_callback, k_timeout_t timeout);

int isotp_fast_unbind(struct isotp_fast_ctx *ctx);

int isotp_fast_send(struct isotp_fast_ctx *ctx, const uint8_t *data, size_t len,
                    const isotp_fast_node_id their_id, void *sent_cb_arg);