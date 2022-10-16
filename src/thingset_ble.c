/*
 * Copyright (c) 2022 Martin Jäger
 */

#include "thingset/ble.h"
#include "thingset/sdk.h"
#include "thingset/storage.h"

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(thingset_ble);

/* ThingSet Custom Service: xxxxyyyy-5a19-4887-9c6a-14ad27bfc06d */
#define BT_UUID_THINGSET_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x00000001, 0x5a19, 0x4887, 0x9c6a, 0x14ad27bfc06d)

#define BT_UUID_THINGSET_REQUEST_VAL \
    BT_UUID_128_ENCODE(0x00000002, 0x5a19, 0x4887, 0x9c6a, 0x14ad27bfc06d)

#define BT_UUID_THINGSET_RESPONSE_VAL \
    BT_UUID_128_ENCODE(0x00000003, 0x5a19, 0x4887, 0x9c6a, 0x14ad27bfc06d)

#define BT_UUID_THINGSET_SERVICE  BT_UUID_DECLARE_128(BT_UUID_THINGSET_SERVICE_VAL)
#define BT_UUID_THINGSET_REQUEST  BT_UUID_DECLARE_128(BT_UUID_THINGSET_REQUEST_VAL)
#define BT_UUID_THINGSET_RESPONSE BT_UUID_DECLARE_128(BT_UUID_THINGSET_RESPONSE_VAL)

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* SLIP protocol (RFC 1055) special characters */
#define SLIP_END     (0xC0)
#define SLIP_ESC     (0xDB)
#define SLIP_ESC_END (0xDC)
#define SLIP_ESC_ESC (0xDD)

static ssize_t thingset_ble_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

static void thingset_ble_disconn(struct bt_conn *conn, uint8_t reason);

static void thingset_ble_conn(struct bt_conn *conn, uint8_t err);

static void thingset_ble_ccc_change(const struct bt_gatt_attr *attr, uint16_t value);

static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_THINGSET_SERVICE_VAL),
};

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = thingset_ble_conn,
    .disconnected = thingset_ble_disconn,
};

/* UART Service Declaration, order of parameters matters! */
BT_GATT_SERVICE_DEFINE(thingset_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_THINGSET_SERVICE),
                       BT_GATT_CHARACTERISTIC(BT_UUID_THINGSET_REQUEST,
                                              BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                                              BT_GATT_PERM_READ | BT_GATT_PERM_WRITE, NULL,
                                              thingset_ble_rx, NULL),
                       BT_GATT_CHARACTERISTIC(BT_UUID_THINGSET_RESPONSE, BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_READ, NULL, NULL, NULL),
                       BT_GATT_CCC(thingset_ble_ccc_change,
                                   BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

/* position of BT_GATT_CCC in array created by BT_GATT_SERVICE_DEFINE */
const struct bt_gatt_attr *attr_ccc_req = &thingset_svc.attrs[3];

static struct bt_conn *ble_conn;

volatile bool notify_resp;

static char tx_buf[CONFIG_THINGSET_SERIAL_TX_BUF_SIZE];
static char rx_buf[CONFIG_THINGSET_SERIAL_RX_BUF_SIZE];

static volatile size_t rx_buf_pos = 0;

static struct k_sem command_flag; // used as an event to signal a received command
static struct k_sem rx_buf_mutex; // binary semaphore used as mutex in ISR context

static void thingset_ble_ccc_change(const struct bt_gatt_attr *attr, uint16_t value)
{
    ARG_UNUSED(attr);
    notify_resp = (value == BT_GATT_CCC_NOTIFY);
    LOG_INF("Notification %s", notify_resp ? "enabled" : "disabled");
}

/*
 * Receives data from BLE interface and decodes it using RFC 1055 SLIP protocol
 */
static ssize_t thingset_ble_rx(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                               const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
    /* store across multiple packages whether we had an escape char */
    static bool escape = false;

    bool finished = true;
    if (buf != NULL && k_sem_take(&rx_buf_mutex, K_NO_WAIT) == 0) {
        for (int i = 0; i < len; i++) {
            uint8_t c = *((uint8_t *)buf + i);
            if (escape) {
                if (c == SLIP_ESC_END) {
                    c = SLIP_END;
                }
                else if (c == SLIP_ESC_ESC) {
                    c = SLIP_ESC;
                }
                /* else: protocol violation, pass character as is */
                escape = false;
            }
            else if (c == SLIP_ESC) {
                escape = true;
                continue;
            }
            else if (c == SLIP_END) {
                if (finished) {
                    /* previous run finished and SLIP_END was used as new start byte */
                    continue;
                }
                else {
                    finished = true;
                    rx_buf[rx_buf_pos] = '\0';
                    // start processing command and keep the rx_buf_mutex locked
                    k_sem_give(&command_flag);
                    return len; // finish up
                }
            }
            else {
                finished = false;
            }
            rx_buf[rx_buf_pos++] = c;
        }
    }
    k_sem_give(&rx_buf_mutex);

    return len;
}

static void thingset_ble_conn(struct bt_conn *conn, uint8_t err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Connected %s", addr);

    ble_conn = bt_conn_ref(conn);
}

static void thingset_ble_disconn(struct bt_conn *conn, uint8_t reason)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_INF("Disconnected %s (reason %u)", addr, reason);

    if (ble_conn) {
        bt_conn_unref(ble_conn);
        ble_conn = NULL;
    }
}

/**
 * Send ThingSet response or statement to BLE client.
 *
 * @param buf Buffer with ThingSet payload (w/o SLIP characters)
 * @param len Length of payload inside the buffer
 */
static void thingset_ble_tx(const uint8_t *buf, size_t len)
{
    if (ble_conn && notify_resp) {
        /* Max. notification: ATT_MTU - 3 */
        const uint16_t max_mtu = bt_gatt_get_mtu(ble_conn) - 3;

        /* even max. possible size of 251 bytes should be OK to allocate on stack */
        uint8_t chunk[max_mtu];
        chunk[0] = SLIP_END;

        int pos_buf = 0;
        int pos_chunk = 1;
        while (pos_buf < len) {
            while (pos_chunk < max_mtu && pos_buf < len) {
                if (buf[pos_buf] == SLIP_END) {
                    chunk[pos_chunk++] = SLIP_ESC;
                    chunk[pos_chunk++] = SLIP_ESC_END;
                }
                else if (buf[pos_buf] == SLIP_ESC) {
                    chunk[pos_chunk++] = SLIP_ESC;
                    chunk[pos_chunk++] = SLIP_ESC_ESC;
                }
                else {
                    chunk[pos_chunk++] = buf[pos_buf];
                }
                pos_buf++;
            }
            if (pos_chunk < max_mtu) {
                chunk[pos_chunk++] = SLIP_END;
            }
            bt_gatt_notify(ble_conn, attr_ccc_req, chunk, pos_chunk);
            pos_chunk = 0;
        }
    }
}

void ble_pub_statement(struct ts_data_object *subset)
{
    if (subset != NULL) {
        int len = ts_txt_statement(&ts, tx_buf, sizeof(tx_buf), subset);
        thingset_ble_tx(tx_buf, len);
    }
}

static void ble_process_command()
{
    // commands must have 2 or more characters
    if (rx_buf_pos > 1) {
        printf("Received Request (%d bytes): %s\n", strlen(rx_buf), rx_buf);

        int len =
            ts_process(&ts, (uint8_t *)rx_buf, strlen(rx_buf), (uint8_t *)tx_buf, sizeof(tx_buf));

        thingset_ble_tx(tx_buf, len);
    }

    // release buffer and start waiting for new commands
    rx_buf_pos = 0;
    k_sem_give(&rx_buf_mutex);
}

static void thingset_ble_thread()
{
    k_sem_init(&command_flag, 0, 1);
    k_sem_init(&rx_buf_mutex, 1, 1);

    int err = bt_enable(NULL);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return;
    }

    err = bt_le_adv_start(BT_LE_ADV_CONN, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return;
    }
    LOG_INF("Waiting for Bluetooth connections...");

    while (true) {
        if (k_sem_take(&command_flag, K_FOREVER) == 0) {
            thingset_ble_process_command();
        }
    }
}

K_THREAD_DEFINE(thingset_ble, 5000, thingset_ble_thread, NULL, NULL, NULL, 6, 0, 1000);
