/*
 * Copyright (c) The ThingSet Project Contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>

#include <zephyr/ztest.h>

#include <thingset.h>
#include <thingset/can.h>
#include <thingset/sdk.h>

#define TEST_RECEIVE_TIMEOUT K_MSEC(100)

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));

static struct k_sem request_tx_sem;
static struct k_sem response_rx_sem;
uint8_t *response;
size_t response_len;
int response_code;

static struct k_sem item_rx_sem;
static uint16_t item_data_id;
static uint8_t item_value_buf[8];
static size_t item_value_len;

static void isotp_fast_recv_cb(struct net_buf *buffer, int rem_len, uint32_t rx_can_id, void *arg)
{
    response = malloc(buffer->len);
    memcpy(response, buffer->data, buffer->len);
    response_code = 0;
    response_len = buffer->len;
    k_sem_give(&response_rx_sem);
}

static void isotp_fast_sent_cb(int result, void *arg)
{
    response_code = result;
    k_sem_give(&request_tx_sem);
}

static void item_rx_callback(uint16_t data_id, const uint8_t *value, size_t value_len,
                             uint8_t source_addr)
{
    if (value_len < sizeof(item_value_buf)) {
        item_data_id = data_id;
        memcpy(item_value_buf, value, value_len);
        item_value_len = value_len;
        k_sem_give(&item_rx_sem);
    }
}

ZTEST(thingset_can, test_receive_item_from_node)
{
    struct can_frame rx_frame = {
        .id = 0x1E123402, /* node with address 0x02 */
        .flags = CAN_FRAME_IDE,
        .data = { 0xF6 },
        .dlc = 1,
    };
    int err;

    k_sem_reset(&item_rx_sem);

    err = can_send(can_dev, &rx_frame, K_MSEC(10), NULL, NULL);
    zassert_equal(err, 0, "can_send failed: %d", err);

    err = k_sem_take(&item_rx_sem, TEST_RECEIVE_TIMEOUT);
    zassert_equal(err, 0, "receive timeout");
    zassert_equal(item_data_id, 0x1234, "wrong data object ID");
    zassert_equal(item_value_len, 1, "wrong value len");
    zassert_equal(item_value_buf[0], 0xF6);
}

static void request_rx_cb(const struct device *dev, struct can_frame *frame, void *user_data)
{
    k_sem_give(&request_tx_sem);
}

ZTEST(thingset_can, test_send_request_to_node)
{
    struct can_filter other_node_filter = {
        .id = 0x1800CC00,
        .mask = 0x1F00FF00,
        .flags = CAN_FILTER_DATA | CAN_FILTER_IDE,
    };
    uint8_t req_buf[] = { 0x01, 0x00 }; /* simple single-frame request via ISO-TP */
    int err;

    k_sem_reset(&request_tx_sem);

    err = can_add_rx_filter(can_dev, &request_rx_cb, NULL, &other_node_filter);
    zassert_false(err < 0, "adding rx filter failed: %d", err);

#ifdef CONFIG_ISOTP_FAST
    thingset_can_send(req_buf, sizeof(req_buf), 0xCC, 0x0, NULL, NULL, TEST_RECEIVE_TIMEOUT);
#else
    thingset_can_send(req_buf, sizeof(req_buf), 0xCC, 0x0);
#endif

    err = k_sem_take(&request_tx_sem, TEST_RECEIVE_TIMEOUT);
    zassert_equal(err, 0, "receive timeout");
}

ZTEST(thingset_can, test_request_response)
{
    k_sem_reset(&request_tx_sem);
    k_sem_reset(&response_rx_sem);

    struct isotp_fast_ctx client_ctx;
    struct isotp_fast_opts opts = {
        .bs = 0,
        .stmin = 0,
        .flags = 0,
    };
    int err = isotp_fast_bind(&client_ctx, can_dev, 0x1800cc00, &opts, isotp_fast_recv_cb, NULL,
                              NULL, isotp_fast_sent_cb);
    zassert_equal(err, 0, "bind fail");

    /* GET CAN node address */
    uint8_t msg[] = { 0x01, 0x19, TS_ID_NET_CAN_NODE_ADDR >> 8, TS_ID_NET_CAN_NODE_ADDR & 0xFF };
    err = isotp_fast_send(&client_ctx, msg, sizeof(msg), 0x01, 0x0, NULL);
    zassert_equal(err, 0, "send fail");
    k_sem_take(&request_tx_sem, TEST_RECEIVE_TIMEOUT);

    k_sem_take(&response_rx_sem, TEST_RECEIVE_TIMEOUT);
    zassert_equal(response_code, 0, "receive fail");

    /* expected response is 0x01 for CAN node address */
    uint8_t resp_exp[] = { 0x85, 0xF6, 0x01 };
    zassert_equal(response_len, 3, "unexpected response length %d", response_len);
    zassert_mem_equal(response, resp_exp, sizeof(resp_exp), "unexpected response");
    free(response);
    isotp_fast_unbind(&client_ctx);
}

static void *thingset_can_setup(void)
{
    int err;

    k_sem_init(&item_rx_sem, 0, 1);
    k_sem_init(&request_tx_sem, 0, 1);
    k_sem_init(&response_rx_sem, 0, 1);

    thingset_init_global(&ts);

    zassert_true(device_is_ready(can_dev), "CAN device not ready");

    (void)can_stop(can_dev);

    err = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
    zassert_equal(err, 0, "failed to set loopback mode (err %d)", err);

    err = can_start(can_dev);
    zassert_equal(err, 0, "failed to start CAN controller (err %d)", err);

    /* wait for address claiming to finish */
    k_sleep(K_MSEC(1000));

    thingset_can_set_item_rx_callback(item_rx_callback);

    return NULL;
}

ZTEST_SUITE(thingset_can, NULL, thingset_can_setup, NULL, NULL, NULL);
