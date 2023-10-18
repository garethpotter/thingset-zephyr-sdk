/*
 * Copyright (c) 2019 Alexander Wachter
 * Copyright (c) 2023 Enphase Energy
 * Copyright (c) 2023 Brill Power
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "isotp_fast_internal.h"
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(isotp_fast, CONFIG_ISOTP_LOG_LEVEL);

static void receive_work_handler(struct k_work *item);
static void receive_timeout_handler(struct k_timer *timer);
static void receive_state_machine(struct isotp_fast_recv_ctx *rctx);

/* Memory slab to hold send contexts */
K_MEM_SLAB_DEFINE(isotp_send_ctx_slab, sizeof(struct isotp_fast_send_ctx),
                  CONFIG_ISOTP_TX_BUF_COUNT, 4);

/* Memory slab to hold receive contexts */
K_MEM_SLAB_DEFINE(isotp_recv_ctx_slab, sizeof(struct isotp_fast_recv_ctx),
                  CONFIG_ISOTP_RX_BUF_COUNT, 4);

/**
 * Pool of buffers for incoming messages. The current implementation
 * sizes these to match the size of a CAN frame less the 1 header byte
 * that ISO-TP consumes. The important configuration options determining
 * the size of the buffer are therefore ISOTP_RX_BUF_COUNT (i.e. broad
 * number of buffers) and ISOTP_RX_MAX_PACKET_COUNT (i.e. how big a
 * message does one anticipate receiving).
 */
NET_BUF_POOL_DEFINE(isotp_rx_pool, CONFIG_ISOTP_RX_BUF_COUNT *CONFIG_ISOTP_RX_MAX_PACKET_COUNT,
                    CAN_MAX_DLEN - 1, sizeof(struct isotp_fast_recv_ctx *), NULL);

/* list of currently in-flight send contexts */
static sys_slist_t isotp_send_ctx_list;
/* list of currently in-flight receive contexts */
static sys_slist_t isotp_recv_ctx_list;

static int get_send_ctx(struct isotp_fast_ctx *ctx, isotp_fast_msg_id recipient_addr,
                        struct isotp_fast_send_ctx **sctx)
{
    isotp_fast_node_id recipient_id = isotp_fast_get_addr_recipient(recipient_addr);
    struct isotp_fast_send_ctx *context;

    SYS_SLIST_FOR_EACH_CONTAINER(&isotp_send_ctx_list, context, node)
    {
        if (isotp_fast_get_addr_recipient(context->recipient_addr) == recipient_id) {
            LOG_DBG("Found existing send context for recipient %x", recipient_addr);
            *sctx = context;
            return 0;
        }
    }

    int err = k_mem_slab_alloc(&isotp_send_ctx_slab, (void **)&context, K_NO_WAIT);
    if (err != 0) {
        return ISOTP_NO_CTX_LEFT;
    }
    *sctx = context;
    context->ctx = ctx;
    context->recipient_addr = recipient_addr;
    k_work_init(&context->work, receive_work_handler);
    k_timer_init(&context->timer, receive_timeout_handler, NULL);
    sys_slist_append(&isotp_send_ctx_list, &context->node);
    LOG_DBG("Created new send context for recipient %x", recipient_addr);

    return 0;
}

static inline void free_send_ctx(struct isotp_fast_send_ctx **ctx)
{
    LOG_DBG("Freeing send context for recipient %x", (*ctx)->recipient_addr);
    k_timer_stop(&(*ctx)->timer);
    sys_slist_find_and_remove(&isotp_send_ctx_list, &(*ctx)->node);
    k_mem_slab_free(&isotp_send_ctx_slab, (void **)ctx);
}

static inline void free_recv_ctx(struct isotp_fast_recv_ctx **rctx)
{
    LOG_DBG("Freeing receive context %x", (*rctx)->sender_addr);
    k_timer_stop(&(*rctx)->timer);
    sys_slist_find_and_remove(&isotp_recv_ctx_list, &(*rctx)->node);
    net_buf_unref((*rctx)->buffer);
    k_mem_slab_free(&isotp_recv_ctx_slab, (void **)rctx);
}

static int get_recv_ctx(struct isotp_fast_ctx *ctx, isotp_fast_msg_id sender_addr,
                        struct isotp_fast_recv_ctx **rctx)
{
    isotp_fast_node_id sender_id = isotp_fast_get_addr_sender(sender_addr);
    struct isotp_fast_recv_ctx *context;
    struct isotp_fast_recv_ctx **p_ctx;

    SYS_SLIST_FOR_EACH_CONTAINER(&isotp_recv_ctx_list, context, node)
    {
        if (isotp_fast_get_addr_sender(context->sender_addr) == sender_id) {
            LOG_DBG("Found existing receive context %x", sender_addr);
            *rctx = context;
            context->frag = net_buf_alloc(&isotp_rx_pool, K_NO_WAIT);
            if (context->frag == NULL) {
                LOG_ERR("No free buffers");
                free_recv_ctx(rctx);
                return ISOTP_NO_NET_BUF_LEFT;
            }
            p_ctx = net_buf_user_data(context->buffer);
            *p_ctx = context;
            net_buf_frag_add(context->buffer, context->frag);
            return 0;
        }
    }

    int err = k_mem_slab_alloc(&isotp_recv_ctx_slab, (void **)&context, K_NO_WAIT);
    if (err != 0) {
        LOG_ERR("No space for receive context - error %d.", err);
        return ISOTP_NO_CTX_LEFT;
    }
    context->buffer = net_buf_alloc(&isotp_rx_pool, K_NO_WAIT);
    if (!context->buffer) {
        k_mem_slab_free(&isotp_recv_ctx_slab, (void **)&context);
        LOG_ERR("No net bufs.");
        return ISOTP_NO_NET_BUF_LEFT;
    }
    context->frag = context->buffer;
    *rctx = context;
    p_ctx = net_buf_user_data(context->buffer);
    *p_ctx = context;
    context->ctx = ctx;
    context->state = ISOTP_RX_STATE_WAIT_FF_SF;
    context->sender_addr = sender_addr;
    k_work_init(&context->work, receive_work_handler);
    k_timer_init(&context->timer, receive_timeout_handler, NULL);
    sys_slist_append(&isotp_recv_ctx_list, &context->node);
    LOG_DBG("Created new receive context %x", sender_addr);

    return 0;
}

static inline void receive_report_error(struct isotp_fast_recv_ctx *rctx, int err)
{
    rctx->state = ISOTP_RX_STATE_ERR;
}

static void send_report_error(struct isotp_fast_send_ctx *sctx, int16_t err)
{
    sctx->state = ISOTP_TX_ERR;
}

static inline uint32_t receive_get_ff_length(uint8_t *data)
{
    uint32_t len;
    uint8_t pci = data[0];

    len = ((pci & ISOTP_PCI_FF_DL_UPPER_MASK) << 8) | data[1];

    /* Jumbo packet (32 bit length)*/
    /* TODO: this probably isn't supported at the moment, given that max length is 4095 */
    if (!len) {
        len = UNALIGNED_GET((uint32_t *)data);
        len = sys_be32_to_cpu(len);
    }

    return len;
}

static inline uint32_t receive_get_sf_length(uint8_t *data)
{
    uint8_t len = data[0] & ISOTP_PCI_SF_DL_MASK;

    /* Single frames > 16 bytes (CAN-FD only) */
    if (IS_ENABLED(ISOTP_USE_CAN_FD) && !len) {
        len = data[1];
    }

    return len;
}

static void receive_can_tx(const struct device *dev, int error, void *arg)
{
    struct isotp_fast_recv_ctx *rctx = (struct isotp_fast_recv_ctx *)arg;

    ARG_UNUSED(dev);

    if (error != 0) {
        LOG_ERR("Error sending FC frame (%d)", error);
        receive_report_error(rctx, ISOTP_N_ERROR);
        k_work_submit(&rctx->work);
    }
}

static void receive_send_fc(struct isotp_fast_recv_ctx *rctx, uint8_t fs)
{
    struct can_frame frame = { .flags = CAN_FRAME_IDE,
                               .id = (rctx->sender_addr & 0xFFFF0000)
                                     | ((rctx->sender_addr & 0xFF00) >> 8)
                                     | ((rctx->sender_addr & 0xFF) << 8) };
    uint8_t *data = frame.data;
    uint8_t payload_len;
    int ret;

    __ASSERT_NO_MSG(!(fs & ISOTP_PCI_TYPE_MASK));

    *data++ = ISOTP_PCI_TYPE_FC | fs;
    *data++ = rctx->ctx->opts->bs;
    *data++ = rctx->ctx->opts->stmin;
    payload_len = data - frame.data;
    frame.dlc = can_bytes_to_dlc(payload_len);

    ret = can_send(rctx->ctx->can_dev, &frame, K_MSEC(ISOTP_A_TIMEOUT_MS), receive_can_tx, rctx);
    if (ret) {
        LOG_ERR("Can't send FC, (%d)", ret);
        receive_report_error(rctx, ISOTP_N_TIMEOUT_A);
        receive_state_machine(rctx);
    }
}

static void receive_state_machine(struct isotp_fast_recv_ctx *rctx)
{
    switch (rctx->state) {
        case ISOTP_RX_STATE_PROCESS_SF:
            LOG_DBG("SM process SF of length %d", rctx->rem_len);
            rctx->rem_len = 0;
            rctx->state = ISOTP_RX_STATE_RECYCLE;
            receive_state_machine(rctx);
            break;

        case ISOTP_RX_STATE_PROCESS_FF:
            LOG_DBG("SM process FF. Length: %d", rctx->rem_len);
            rctx->rem_len -= rctx->frag->len;
            if (rctx->ctx->opts->bs == 0
                && rctx->rem_len > CONFIG_ISOTP_RX_BUF_COUNT * CONFIG_ISOTP_RX_BUF_SIZE)
            {
                LOG_ERR("Pkt length is %d but buffer has only %d bytes", rctx->rem_len,
                        CONFIG_ISOTP_RX_BUF_COUNT * CONFIG_ISOTP_RX_BUF_SIZE);
                receive_report_error(rctx, ISOTP_N_BUFFER_OVERFLW);
                receive_state_machine(rctx);
                break;
            }

            if (rctx->ctx->opts->bs) {
                rctx->bs = rctx->ctx->opts->bs;
                // ctx->ctx->recv_callback(ctx->buffer, ctx->rem_len, ctx->sender_addr,
                // ctx->ctx->recv_cb_arg); LOG_INF("Dispatched chunk of length %d; remaining %d",
                // ctx->buffer->len, ctx->rem_len);
            }

            rctx->wft = ISOTP_WFT_FIRST;
            rctx->state = ISOTP_RX_STATE_TRY_ALLOC;
            __fallthrough;
        case ISOTP_RX_STATE_TRY_ALLOC:
            LOG_DBG("SM try to allocate");
            k_timer_stop(&rctx->timer);
            // ret = receive_alloc_buffer(ctx);
            // if (ret) {
            //     LOG_DBG("SM allocation failed. Wait for free buffer");
            //     break;
            // }

            rctx->state = ISOTP_RX_STATE_SEND_FC;
            __fallthrough;
        case ISOTP_RX_STATE_SEND_FC:
            LOG_DBG("SM send CTS FC frame");
            receive_send_fc(rctx, ISOTP_PCI_FS_CTS);
            k_timer_start(&rctx->timer, K_MSEC(ISOTP_CR_TIMEOUT_MS), K_NO_WAIT);
            rctx->state = ISOTP_RX_STATE_WAIT_CF;
            break;

        case ISOTP_RX_STATE_SEND_WAIT:
            if (++rctx->wft < CONFIG_ISOTP_WFTMAX) {
                LOG_DBG("Send wait frame number %d", rctx->wft);
                receive_send_fc(rctx, ISOTP_PCI_FS_WAIT);
                k_timer_start(&rctx->timer, K_MSEC(ISOTP_ALLOC_TIMEOUT_MS), K_NO_WAIT);
                rctx->state = ISOTP_RX_STATE_TRY_ALLOC;
                break;
            }

            LOG_ERR("Sent %d wait frames. Giving up to alloc now", rctx->wft);
            receive_report_error(rctx, ISOTP_N_BUFFER_OVERFLW);
            __fallthrough;
        case ISOTP_RX_STATE_ERR:
            // LOG_DBG("SM ERR state. err nr: %d", ctx->error_nr);
            k_timer_stop(&rctx->timer);

            // if (ctx->error_nr == ISOTP_N_BUFFER_OVERFLW) {
            //     receive_send_fc(ctx, ISOTP_PCI_FS_OVFLW);
            // }

            // net_buf_unref(ctx->buffer);
            // ctx->buffer = NULL;
            // ctx->state = ISOTP_RX_STATE_RECYCLE;
            free_recv_ctx(&rctx);
            __fallthrough;
        case ISOTP_RX_STATE_RECYCLE:
            LOG_DBG("Message complete; dispatching");
            rctx->ctx->recv_callback(rctx->buffer, 0, rctx->sender_addr, rctx->ctx->recv_cb_arg);
            rctx->state = ISOTP_RX_STATE_UNBOUND;
            free_recv_ctx(&rctx);
            break;
        case ISOTP_RX_STATE_UNBOUND:
            break;

        default:
            break;
    }
}

static void process_ff_sf(struct isotp_fast_recv_ctx *rctx, struct can_frame *frame)
{
    int index = 0;
    uint8_t payload_len;

    switch (frame->data[index] & ISOTP_PCI_TYPE_MASK) {
        case ISOTP_PCI_TYPE_FF:
            LOG_DBG("Got FF IRQ");
            if (frame->dlc != ISOTP_CAN_DL) {
                LOG_DBG("FF DLC invalid. Ignore");
                return;
            }

            rctx->rem_len = receive_get_ff_length(frame->data);
            rctx->state = ISOTP_RX_STATE_PROCESS_FF;
            rctx->sn_expected = 1;
            index += 2;
            payload_len = CAN_MAX_DLEN - index;
            LOG_DBG("FF total length %d, FF len %d", rctx->rem_len, payload_len);
            break;

        case ISOTP_PCI_TYPE_SF:
            LOG_DBG("Got SF IRQ");
            rctx->rem_len = receive_get_sf_length(frame->data);
            index++;
            payload_len = MIN(rctx->rem_len, CAN_MAX_DLEN - index);
            LOG_DBG("SF length %d", payload_len);
            if (payload_len > frame->dlc) {
                LOG_DBG("SF DL does not fit. Ignore");
                return;
            }

            rctx->state = ISOTP_RX_STATE_PROCESS_SF;
            break;

        default:
            LOG_DBG("Got unexpected frame. Ignore");
            return;
    }

    LOG_DBG("Current buffer size %d; adding %d", rctx->buffer->len, payload_len);
    net_buf_add_mem(rctx->frag, &frame->data[index], payload_len);
}

static void process_cf(struct isotp_fast_recv_ctx *rctx, struct can_frame *frame)
{
    int index = 0;
    uint32_t data_len;

    if ((frame->data[index] & ISOTP_PCI_TYPE_MASK) != ISOTP_PCI_TYPE_CF) {
        LOG_DBG("Waiting for CF but got something else (%d)",
                frame->data[index] >> ISOTP_PCI_TYPE_POS);
        receive_report_error(rctx, ISOTP_N_UNEXP_PDU);
        k_work_submit(&rctx->work);
        return;
    }

    k_timer_start(&rctx->timer, K_MSEC(ISOTP_CR_TIMEOUT_MS), K_NO_WAIT);

    if ((frame->data[index++] & ISOTP_PCI_SN_MASK) != rctx->sn_expected++) {
        LOG_ERR("Sequence number mismatch");
        receive_report_error(rctx, ISOTP_N_WRONG_SN);
        k_work_submit(&rctx->work);
        return;
    }

    LOG_DBG("Got CF irq. Appending data");
    data_len = MIN(rctx->rem_len, frame->dlc - index);
    net_buf_add_mem(rctx->frag, &frame->data[index], data_len);
    rctx->rem_len -= data_len;
    LOG_DBG("Added %d bytes; %d bytes remaining", data_len, rctx->rem_len);

    if (rctx->rem_len == 0) {
        rctx->state = ISOTP_RX_STATE_RECYCLE;
        k_work_submit(&rctx->work); // to dispatch complete message
        return;
    }

    if (rctx->ctx->opts->bs && !--rctx->bs) {
        LOG_DBG("Block is complete. Allocate new buffer");
        rctx->bs = rctx->ctx->opts->bs;
        // rctx->ctx->recv_callback(rctx->buffer, rctx->rem_len, rctx->sender_addr,
        // rctx->ctx->recv_cb_arg);
        rctx->state = ISOTP_RX_STATE_TRY_ALLOC;
    }
}

static void receive_work_handler(struct k_work *item)
{
    struct isotp_fast_recv_ctx *rctx = CONTAINER_OF(item, struct isotp_fast_recv_ctx, work);

    receive_state_machine(rctx);
}

static void receive_timeout_handler(struct k_timer *timer)
{
    struct isotp_fast_recv_ctx *rctx = CONTAINER_OF(timer, struct isotp_fast_recv_ctx, timer);

    switch (rctx->state) {
        case ISOTP_RX_STATE_WAIT_CF:
            LOG_ERR("Timeout while waiting for CF");
            receive_report_error(rctx, ISOTP_N_TIMEOUT_CR);
            break;

        case ISOTP_RX_STATE_TRY_ALLOC:
            rctx->state = ISOTP_RX_STATE_SEND_WAIT;
            break;

        default:
            break;
    }

    k_work_submit(&rctx->work);
}

static void receive_can_rx(struct isotp_fast_recv_ctx *rctx, struct can_frame *frame)
{
    switch (rctx->state) {
        case ISOTP_RX_STATE_WAIT_FF_SF:
            process_ff_sf(rctx, frame);
            break;

        case ISOTP_RX_STATE_WAIT_CF:
            process_cf(rctx, frame);
            /* still waiting for more CF */
            if (rctx->state == ISOTP_RX_STATE_WAIT_CF) {
                return;
            }
            break;

        default:
            LOG_DBG("Got a frame in a state where it is unexpected.");
    }

    k_work_submit(&rctx->work);
}

static inline void prepare_frame(struct can_frame *frame, struct isotp_fast_ctx *ctx,
                                 isotp_fast_msg_id addr)
{
    frame->id = addr;
    frame->flags =
        CAN_FRAME_IDE | 0; //((ctx->opts->flags & ISOTP_MSG_FDF) != 0) ? CAN_FRAME_FDF : 0;
}

static k_timeout_t stmin_to_timeout(uint8_t stmin)
{
    /* According to ISO 15765-2 stmin should be 127ms if value is corrupt */
    if (stmin > ISOTP_STMIN_MAX || (stmin > ISOTP_STMIN_MS_MAX && stmin < ISOTP_STMIN_US_BEGIN)) {
        return K_MSEC(ISOTP_STMIN_MS_MAX);
    }

    if (stmin >= ISOTP_STMIN_US_BEGIN) {
        return K_USEC((stmin + 1 - ISOTP_STMIN_US_BEGIN) * 100U);
    }

    return K_MSEC(stmin);
}

static void send_process_fc(struct isotp_fast_send_ctx *sctx, struct can_frame *frame)
{
    uint8_t *data = frame->data;

    if ((*data & ISOTP_PCI_TYPE_MASK) != ISOTP_PCI_TYPE_FC) {
        LOG_ERR("Got unexpected PDU expected FC");
        send_report_error(sctx, ISOTP_N_UNEXP_PDU);
        return;
    }

    switch (*data++ & ISOTP_PCI_FS_MASK) {
        case ISOTP_PCI_FS_CTS:
            sctx->state = ISOTP_TX_SEND_CF;
            sctx->wft = 0;
            sctx->backlog = 0;
            k_sem_reset(&sctx->sem);
            sctx->bs = *data++;
            sctx->stmin = *data++;
            LOG_DBG("Got CTS. BS: %d, STmin: %d", sctx->bs, sctx->stmin);
            break;

        case ISOTP_PCI_FS_WAIT:
            LOG_DBG("Got WAIT frame");
            k_timer_start(&sctx->timer, K_MSEC(ISOTP_BS_TIMEOUT_MS), K_NO_WAIT);
            if (sctx->wft >= CONFIG_ISOTP_WFTMAX) {
                LOG_WRN("Got too many wait frames");
                send_report_error(sctx, ISOTP_N_WFT_OVRN);
            }

            sctx->wft++;
            break;

        case ISOTP_PCI_FS_OVFLW:
            LOG_ERR("Got overflow FC frame");
            send_report_error(sctx, ISOTP_N_BUFFER_OVERFLW);
            break;

        default:
            send_report_error(sctx, ISOTP_N_INVALID_FS);
    }
}

static void send_can_rx(struct isotp_fast_send_ctx *sctx, struct can_frame *frame)
{
    if (sctx->state == ISOTP_TX_WAIT_FC) {
        k_timer_stop(&sctx->timer);
        send_process_fc(sctx, frame);
    }
    else {
        LOG_ERR("Got unexpected PDU");
        send_report_error(sctx, ISOTP_N_UNEXP_PDU);
    }

    k_work_submit(&sctx->work);
}

static void can_rx_callback(const struct device *dev, struct can_frame *frame, void *arg)
{
    struct isotp_fast_ctx *ctx = arg;
    int index = 0;
    isotp_fast_msg_id sender_id =
        (frame->id & 0xFFFF0000) | ((frame->id & 0xFF00) >> 8) | ((frame->id & 0xFF) << 8);
    if ((frame->data[index++] & ISOTP_PCI_TYPE_MASK) == ISOTP_PCI_TYPE_FC) {
        LOG_DBG("Got flow control frame from %x", frame->id);
        /* inbound flow control for a message we are currently transmitting */
        struct isotp_fast_send_ctx *sctx;
        if (get_send_ctx(ctx, sender_id, &sctx) != 0) {
            LOG_DBG("Ignoring flow control frame from %x", frame->id);
            return;
        }
        send_can_rx(sctx, frame);
    }
    else {
        struct isotp_fast_recv_ctx *rctx;
        if (get_recv_ctx(ctx, frame->id, &rctx) != 0) {
            LOG_ERR("RX buffer full");
            return;
        }
        receive_can_rx(rctx, frame);
    }
}

static void send_can_tx_callback(const struct device *dev, int error, void *arg)
{
    struct isotp_fast_send_ctx *sctx = arg;

    ARG_UNUSED(dev);

    sctx->backlog--;
    k_sem_give(&sctx->sem);

    if (sctx->state == ISOTP_TX_WAIT_BACKLOG) {
        if (sctx->backlog > 0) {
            return;
        }

        sctx->state = ISOTP_TX_WAIT_FIN;
    }

    k_work_submit(&sctx->work);
}

static inline int send_ff(struct isotp_fast_send_ctx *sctx)
{
    struct can_frame frame;
    int index = 0;
    int ret;
    uint16_t len = sctx->rem_len;

    prepare_frame(&frame, sctx->ctx, sctx->recipient_addr);

    if (len > 0xFFF) {
        frame.data[index++] = ISOTP_PCI_TYPE_FF;
        frame.data[index++] = 0;
        frame.data[index++] = (len >> 3 * 8) & 0xFF;
        frame.data[index++] = (len >> 2 * 8) & 0xFF;
        frame.data[index++] = (len >> 8) & 0xFF;
        frame.data[index++] = len & 0xFF;
    }
    else {
        frame.data[index++] = ISOTP_PCI_TYPE_FF | (len >> 8);
        frame.data[index++] = len & 0xFF;
    }

    /* According to ISO FF has sn 0 and is incremented to one
     * although it's not part of the FF frame
     */
    sctx->sn = 1;
    uint16_t size = MIN(CAN_MAX_DLEN, len) - index;
    memcpy(&frame.data[index], sctx->data, size);
    sctx->rem_len -= size;
    sctx->data += size;
    frame.dlc = can_bytes_to_dlc(CAN_MAX_DLEN);

    ret = can_send(sctx->ctx->can_dev, &frame, K_MSEC(ISOTP_A_TIMEOUT_MS), send_can_tx_callback,
                   sctx);
    return ret;
}

static inline int send_cf(struct isotp_fast_send_ctx *sctx)
{
    struct can_frame frame;
    int index = 0;
    int ret;
    uint16_t len;

    prepare_frame(&frame, sctx->ctx, sctx->recipient_addr);

    /*sn wraps around at 0xF automatically because it has a 4 bit size*/
    frame.data[index++] = ISOTP_PCI_TYPE_CF | sctx->sn;

    len = MIN(sctx->rem_len, CAN_MAX_DLEN - index);
    memcpy(&frame.data[index], sctx->data, len);
    sctx->rem_len -= len;
    sctx->data += len;

    frame.dlc = can_bytes_to_dlc(len + index);

    ret = can_send(sctx->ctx->can_dev, &frame, K_MSEC(ISOTP_A_TIMEOUT_MS), send_can_tx_callback,
                   sctx);
    if (ret == 0) {
        sctx->sn++;
        sctx->bs--;
        sctx->backlog++;
    }

    ret = ret ? ret : sctx->rem_len;
    return ret;
}

static void send_state_machine(struct isotp_fast_send_ctx *sctx)
{
    int ret;
    switch (sctx->state) {
        case ISOTP_TX_SEND_FF:
            send_ff(sctx);
            k_timer_start(&sctx->timer, K_MSEC(ISOTP_BS_TIMEOUT_MS), K_NO_WAIT);
            sctx->state = ISOTP_TX_WAIT_FC;
            break;

        case ISOTP_TX_SEND_CF:
            k_timer_stop(&sctx->timer);
            do {
                ret = send_cf(sctx);
                if (!ret) {
                    sctx->state = ISOTP_TX_WAIT_BACKLOG;
                    break;
                }

                if (ret < 0) {
                    LOG_ERR("Failed to send CF");
                    send_report_error(sctx, ret == -EAGAIN ? ISOTP_N_TIMEOUT_A : ISOTP_N_ERROR);
                    break;
                }

                if (sctx->ctx->opts->bs && !sctx->bs) {
                    k_timer_start(&sctx->timer, K_MSEC(ISOTP_BS_TIMEOUT_MS), K_NO_WAIT);
                    sctx->state = ISOTP_TX_WAIT_FC;
                    LOG_DBG("BS reached. Wait for FC again");
                    break;
                }
                else if (sctx->stmin) {
                    sctx->state = ISOTP_TX_WAIT_ST;
                    break;
                }

                /* Ensure FIFO style transmission of CF */
                k_sem_take(&sctx->sem, K_FOREVER);
            } while (ret > 0);
            break;

        case ISOTP_TX_WAIT_ST:
            k_timer_start(&sctx->timer, stmin_to_timeout(sctx->stmin), K_NO_WAIT);
            sctx->state = ISOTP_TX_SEND_CF;
            LOG_DBG("SM wait ST");
            break;

        case ISOTP_TX_ERR:
            LOG_DBG("SM error");
            free_send_ctx(&sctx);
            __fallthrough;

            /*
             * We sent this synchronously in isotp_fast_send.
             * case ISOTP_TX_SEND_SF:
             *   __fallthrough;
             * */

        case ISOTP_TX_WAIT_FIN:
            LOG_DBG("SM finish");
            k_timer_stop(&sctx->timer);

            sctx->ctx->sent_callback(ISOTP_N_OK, sctx->cb_arg);
            sctx->state = ISOTP_TX_STATE_RESET;
            free_send_ctx(&sctx);
            break;

        default:
            break;
    }
}

static void send_work_handler(struct k_work *work)
{
    struct isotp_fast_send_ctx *sctx = CONTAINER_OF(work, struct isotp_fast_send_ctx, work);

    send_state_machine(sctx);
}

static void send_timeout_handler(struct k_timer *timer)
{
    struct isotp_fast_send_ctx *sctx = CONTAINER_OF(timer, struct isotp_fast_send_ctx, timer);

    if (sctx->state != ISOTP_TX_SEND_CF) {
        send_report_error(sctx, ISOTP_N_TIMEOUT_BS);
        LOG_ERR("Timed out waiting for FC frame");
    }

    k_work_submit(&sctx->work);
}

static inline void prepare_filter(struct can_filter *filter, isotp_fast_msg_id my_addr,
                                  const struct isotp_fast_opts *opts)
{
    filter->id = my_addr;
    filter->mask = ISOTP_FIXED_ADDR_RX_MASK;
    filter->flags = CAN_FILTER_DATA | CAN_FILTER_IDE
                    | ((opts->flags & ISOTP_MSG_FDF) != 0 ? CAN_FILTER_FDF : 0);
}

int isotp_fast_bind(struct isotp_fast_ctx *ctx, const struct device *can_dev,
                    const isotp_fast_msg_id my_addr, const struct isotp_fast_opts *opts,
                    isotp_fast_recv_callback_t recv_callback, void *recv_cb_arg,
                    isotp_fast_send_callback_t sent_callback)
{
    sys_slist_init(&isotp_send_ctx_list);
    sys_slist_init(&isotp_recv_ctx_list);

    ctx->can_dev = can_dev;
    ctx->opts = opts;
    ctx->recv_callback = recv_callback;
    ctx->recv_cb_arg = recv_cb_arg;
    ctx->sent_callback = sent_callback;
    ctx->my_addr = my_addr;

    struct can_filter filter;
    prepare_filter(&filter, my_addr, opts);
    ctx->filter_id = can_add_rx_filter(ctx->can_dev, can_rx_callback, ctx, &filter);

    LOG_INF("Successfully bound to %x:%x", filter.id, filter.mask);

    return ISOTP_N_OK;
}

int isotp_fast_unbind(struct isotp_fast_ctx *ctx)
{
    if (ctx->filter_id >= 0 && ctx->can_dev) {
        can_remove_rx_filter(ctx->can_dev, ctx->filter_id);
    }

    // TODO: what if messages are in flight? Need to clean up.

    return ISOTP_N_OK;
}

int isotp_fast_send(struct isotp_fast_ctx *ctx, const uint8_t *data, size_t len,
                    const isotp_fast_node_id their_id, void *cb_arg)
{
    const isotp_fast_msg_id recipient_addr = (ctx->my_addr & 0xFFFF0000)
                                             | (isotp_fast_get_addr_recipient(ctx->my_addr))
                                             | (their_id << ISOTP_FIXED_ADDR_TA_POS);
    if (len < (CAN_MAX_DLEN - 1)) {
        struct can_frame frame = {
            .dlc = can_bytes_to_dlc(len + 1),
            .id = recipient_addr,
            .flags =
                CAN_FRAME_IDE | 0 //((ctx->opts->flags & ISOTP_MSG_FDF) != 0) ? CAN_FRAME_FDF : 0,
        };
        frame.data[0] = (uint8_t)len;
        memcpy(&frame.data[1], data, len);
        int ret = can_send(ctx->can_dev, &frame, K_MSEC(ISOTP_A_TIMEOUT_MS), NULL, NULL);
        ctx->sent_callback(ret, cb_arg);
        return ISOTP_N_OK;
    }
    else {
        struct isotp_fast_send_ctx *context;
        int ret = get_send_ctx(ctx, recipient_addr, &context);
        if (ret) {
            return ISOTP_NO_NET_BUF_LEFT;
        }
        context->ctx = ctx;
        context->recipient_addr = recipient_addr;
        context->data = data;
        context->bs = ctx->opts->bs;
        context->stmin = ctx->opts->stmin;
        context->rem_len = len;
        context->state = ISOTP_TX_SEND_FF;
        context->cb_arg = cb_arg;
        k_sem_init(&context->sem, 0, 1);
        k_work_init(&context->work, send_work_handler);
        k_timer_init(&context->timer, send_timeout_handler, NULL);

        k_work_submit(&context->work);
    }
    return ISOTP_N_OK;
}