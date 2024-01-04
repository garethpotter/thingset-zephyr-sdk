/*
 * Copyright (c) 2023 Brill Power
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/canbus/isotp.h>

#ifndef ISOTP_MSG_FDF
#define ISOTP_MSG_FDF BIT(3)
#endif

/**
 * @brief ISO-TP address struct
 *
 * Identifies the source/destination of an ISO-TP message.
 */
struct isotp_fast_addr
{
    /** CAN ID */
    union {
        uint32_t std_id : 11;
        uint32_t ext_id : 29;
    };
    /** ISO-TP extended address */
    uint8_t ext_addr;
};

/**
 * Callback invoked when a message is received.
 *
 * @param buffer Pointer to a @ref net_buf. Call @ref net_buf_frags_len to
 *               obtain the length of the buffer, then @ref net_buf_linearize to copy the
 *               contents of the buffer into a local buffer.
 * @param rem_len At present, this should always be zero. In future, this
 *                callback may be called repeatedly as a message's packets arrive to reduce
 *                the need to buffer an entire message in memory before it is dispatched
 *                to user code.
 * @param can_id The CAN ID of the message that has been received.
 * @param arg The value of @ref recv_cb_arg passed to @ref isotp_fast_bind.
 */
typedef void (*isotp_fast_recv_callback_t)(struct net_buf *buffer, int rem_len,
                                           struct isotp_fast_addr can_id, void *arg);

/**
 * Callback invoked when an error occurs during message reception.
 *
 * @param error The error code.
 * @param can_id The CAN ID of the sender of the message, if available.
 * @param arg The value of @ref recv_cb_arg passed to @ref isotp_fast_bind.
 */
typedef void (*isotp_fast_recv_error_callback_t)(int8_t error, struct isotp_fast_addr can_id,
                                                 void *arg);

/**
 * Callback invoked when a message has been sent.
 *
 * @param result If non-zero, an error has occurred.
 * @param arg The value of @ref sent_cb_arg passed to @ref isotp_fast_send.
 */
typedef void (*isotp_fast_send_callback_t)(int result, void *arg);

#ifdef CONFIG_ISOTP_FAST_CUSTOM_ADDRESSING
/**
 * Callback invoked to determine the reply address for a given inbound message.
 *
 * @param ctx Pointer to the main context.
 * @param target_addr Address.
 */
typedef struct isotp_fast_addr (*isotp_fast_get_tx_can_id_callback_t)(
    struct isotp_fast_ctx *ctx, struct isotp_fast_addr target_addr);
#endif /* CONFIG_ISOTP_FAST_CUSTOM_ADDRESSING */

enum isotp_fast_addressing_mode
{
    /** 11- or 29-bit CAN ID */
    ISOTP_FAST_ADDRESSING_MODE_NORMAL = 0,
    /** 29-bit: 8-bit source and target addresses (SAE J1939) */
    ISOTP_FAST_ADDRESSING_MODE_FIXED = 1,
    /** Additional addressing information in first byte of message */
    ISOTP_FAST_ADDRESSING_MODE_EXTENDED = 2,
#ifdef CONFIG_ISOTP_FAST_ALLOW_CUSTOM_ADDRESSING
    ISOTP_FAST_ADDRESSING_MODE_CUSTOM = 4,
#endif
};

/**
 * Options pertaining to the bound context.
 */
struct isotp_fast_opts
{
    /** Block size. Number of CF PDUs before next CF is sent */
    uint8_t bs;
    /** Minimum separation time. Min time between frames */
    uint8_t stmin;
    uint8_t flags;
    enum isotp_fast_addressing_mode addressing_mode : 4;
};

/**
 * General ISO-TP fast context object.
 */
struct isotp_fast_ctx
{
    /** list of currently in-flight send contexts */
    sys_slist_t isotp_send_ctx_list;
    /** list of currently in-flight receive contexts */
    sys_slist_t isotp_recv_ctx_list;
    /** The CAN device to which the context is bound via @ref isotp_fast_bind */
    const struct device *can_dev;
    /** Identifies the CAN filter which filters incoming messages */
    int filter_id;
    /** Pointer to context options described above */
    const struct isotp_fast_opts *opts;
    /** Callback that is invoked when a message is received */
    isotp_fast_recv_callback_t recv_callback;
    /** Pointer to user-supplied data to be passed to @ref recv_callback */
    void *recv_cb_arg;
    /** Callback that is invoked when a receive error occurs */
    isotp_fast_recv_error_callback_t recv_error_callback;
    /** Callback that is invoked when a message is sent */
    isotp_fast_send_callback_t sent_callback;
    /** CAN ID of this node, used in receipt of messages */
    const struct isotp_fast_addr rx_can_id;
#ifdef CONFIG_ISOTP_FAST_BLOCKING_RECEIVE
    sys_slist_t wait_recv_list;
#endif
#ifdef CONFIG_ISOTP_FAST_CUSTOM_ADDRESSING
    isotp_fast_get_tx_can_id_callback_t get_tx_can_id_callback;
#endif
};

/**
 * Binds the supplied ISO-TP context to the supplied CAN device. Messages
 * addressed to the given address (@ref rx_can_id) will be delivered to user
 * code by invoking the supplied callback, @ref recv_callback.
 *
 * @param ctx A pointer to the general ISO-TP context
 * @param can_dev The CAN device to which the context should be bound
 * @param rx_can_id The address to listen on for incoming messages. When
 *                  transmitting messages, the target address byte will be
 *                  used as the source address byte.
 * @param opts A pointer to an options structure, @ref isotp_fast_opts
 * @param recv_callback A callback that is invoked when a message is received
 * @param recv_cb_arg A pointer to data to be supplied to @ref recv_callback
 * @param recv_error_callback A callback that is invoked when an error occurs.
 * @param sent_callback A callback that is invoked when a message is sent
 *
 * @returns 0 on success, otherwise an error code < 0.
 */
int isotp_fast_bind(struct isotp_fast_ctx *ctx, const struct device *can_dev,
                    const struct isotp_fast_addr rx_can_id, const struct isotp_fast_opts *opts,
                    isotp_fast_recv_callback_t recv_callback, void *recv_cb_arg,
                    isotp_fast_recv_error_callback_t recv_error_callback,
                    isotp_fast_send_callback_t sent_callback);

/**
 * Unbinds the supplied ISO-TP context. Removes the CAN filter if it was
 * successfully set.
 *
 * @param ctx A pointer to the context to unbind
 *
 * @returns 0 on success.
 */
int isotp_fast_unbind(struct isotp_fast_ctx *ctx);

#ifdef CONFIG_ISOTP_FAST_BLOCKING_RECEIVE
int isotp_fast_recv(struct isotp_fast_ctx *ctx, struct can_filter sender, uint8_t *buf, size_t size,
                    k_timeout_t timeout);
#endif

/**
 * Send a message to a given recipient. If the message fits within a
 * CAN frame, it will be sent synchronously. If not, it will be sent
 * asynchronously.
 *
 * @param ctx The bound context on which the message should be sent
 * @param data A pointer to the data containing the message to send
 * @param len The length of the data in @ref data
 * @param target_addr The CAN ID identifying the recipient.
 * @param sent_cb_arg A pointer to data to be supplied to the callback
 *                    that will be invoked when the message is sent.
 *
 * @returns 0 on success.
 */
int isotp_fast_send(struct isotp_fast_ctx *ctx, const uint8_t *data, size_t len,
                    const struct isotp_fast_addr target_addr, void *sent_cb_arg);

struct isotp_fast_addr isotp_fast_get_reply_addr(struct isotp_fast_ctx *ctx,
                                                 const struct isotp_fast_addr addr);