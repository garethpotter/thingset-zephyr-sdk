/*
 * Copyright (c) 2019 Alexander Wachter
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <thingset/isotp_fast.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>
#include <strings.h>
#include "random_data.h"

#define PCI_TYPE_POS      4
#define DATA_SIZE_SF      7
#define DATA_SIZE_CF      7
#define DATA_SIZE_SF_EXT  6
#define DATA_SIZE_FF      6
#define CAN_DL            8
#define DATA_SEND_LENGTH  272
#define SF_PCI_TYPE       0
#define SF_PCI_BYTE_1     ((SF_PCI_TYPE << PCI_TYPE_POS) | DATA_SIZE_SF)
#define SF_PCI_BYTE_2_EXT ((SF_PCI_TYPE << PCI_TYPE_POS) | DATA_SIZE_SF_EXT)
#define SF_PCI_BYTE_LEN_8 ((SF_PCI_TYPE << PCI_TYPE_POS) | (DATA_SIZE_SF + 1))
#define EXT_ADDR           5
#define FF_PCI_TYPE        1
#define FF_PCI_BYTE_1(dl)      ((FF_PCI_TYPE << PCI_TYPE_POS) | ((dl) >> 8))
#define FF_PCI_BYTE_2(dl)      ((dl) & 0xFF)
#define FC_PCI_TYPE        3
#define FC_PCI_CTS         0
#define FC_PCI_WAIT        1
#define FC_PCI_OVFLW       2
#define FC_PCI_BYTE_1(FS)  ((FC_PCI_TYPE << PCI_TYPE_POS) | (FS))
#define FC_PCI_BYTE_2(BS)  (BS)
#define FC_PCI_BYTE_3(ST_MIN) (ST_MIN)
#define CF_PCI_TYPE        2
#define CF_PCI_BYTE_1      (CF_PCI_TYPE << PCI_TYPE_POS)
#define STMIN_VAL_1        5
#define STMIN_VAL_2        50
#define STMIN_UPPER_TOLERANCE 5

#if defined(CONFIG_ISOTP_ENABLE_TX_PADDING) || \
				defined(CONFIG_ISOTP_ENABLE_TX_PADDING)
#define DATA_SIZE_FC       CAN_DL
#else
#define DATA_SIZE_FC       3
#endif

#define BS_TIMEOUT_UPPER_MS   1100
#define BS_TIMEOUT_LOWER_MS   1000

/*
 * @addtogroup t_can
 * @{
 * @defgroup t_can_isotp test_can_isotp
 * @brief TestPurpose: verify correctness of the iso tp implementation
 * @details
 * - Test Steps
 *   -#
 * - Expected Results
 *   -#
 * @}
 */

struct frame_desired {
	uint8_t data[8];
	uint8_t length;
};

struct frame_desired des_frames[DIV_ROUND_UP((DATA_SEND_LENGTH - DATA_SIZE_FF),
					     DATA_SIZE_CF)];


const struct isotp_fast_opts fc_opts = {
	.bs = 8,
	.stmin = 0,
	.flags = 0
};
const struct isotp_fast_opts fc_opts_single = {
	.bs = 0,
	.stmin = 0,
	.flags = 0
};

const isotp_fast_msg_id rx_addr = 0x18DA0201;
const isotp_fast_msg_id tx_addr = 0x18DA0102;

const isotp_fast_node_id rx_node_id = 0x01;
const isotp_fast_node_id tx_node_id = 0x02;

struct recv_msg
{
	uint8_t data[8];
	int16_t len;
	int rem_len;
};

const struct device *const can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
struct isotp_fast_ctx ctx;
uint8_t data_buf[128];
CAN_MSGQ_DEFINE(frame_msgq, 10);
K_MSGQ_DEFINE(recv_msgq, sizeof(struct recv_msg), DIV_ROUND_UP(DATA_SEND_LENGTH, DATA_SIZE_CF), 2);
struct k_sem send_compl_sem;

static void print_hex(const uint8_t *ptr, size_t len)
{
	while (len--) {
		printk("%02x ", *ptr++);
	}
}

static int blocking_recv(uint8_t *buf, size_t size, k_timeout_t timeout)
{
	int ret;
	struct recv_msg msg;
	int rx_len = 0;
	while ((ret = k_msgq_get(&recv_msgq, &msg, timeout)) == 0)
	{
		if (msg.len < 0) {
			/* an error has occurred */
			printk("Error %d occurred", msg.len);
			return msg.len;
		}
		int cp_len = MIN(msg.len, size - rx_len);
		memcpy(buf, &msg.data, cp_len);
		printk("RECV: ");
		print_hex(&msg.data[0], msg.len);
		printk("\n");
		rx_len += cp_len;
		buf += cp_len;
		if (msg.rem_len > (size - rx_len)) {
			/* recv buffer will probably overflow on next call; hand back to user code */
			break;
		}
		if (msg.rem_len == 0) {
			/* msg is complete */
			break;
		}
	}
	if (ret == -EAGAIN) {
		return ISOTP_RECV_TIMEOUT;
	}
	return rx_len;
}

void isotp_fast_recv_handler(struct net_buf *buffer, int rem_len,
                             isotp_fast_msg_id sender_addr, void *arg)
{
	struct recv_msg msg = {
		.len = buffer->len,
		.rem_len = rem_len,
	};
	memcpy(&msg.data, buffer->data, MIN(sizeof(msg.data), buffer->len));
	printk("%d bytes received; remaining: %d\n", msg.len, rem_len);
	k_msgq_put(&recv_msgq, &msg, K_NO_WAIT);
}

void isotp_fast_recv_error_handler(int8_t error, isotp_fast_msg_id sender_addr, void *arg)
{
	struct recv_msg msg = {
		.len = error,
	};
	printk("Error %d received\n", error);
	k_msgq_put(&recv_msgq, &msg, K_NO_WAIT);
}

void isotp_fast_sent_handler(int result, void *arg)
{
	int expected_err_nr = POINTER_TO_INT(arg);

	zassert_equal(result, expected_err_nr,
		      "Unexpected error nr. expect: %d, got %d",
		      expected_err_nr, result);
	k_sem_give(&send_compl_sem);
}

static int check_data(const uint8_t *frame, const uint8_t *desired, size_t length)
{
	int ret;

	ret = memcmp(frame, desired, length);
	if (ret) {
		printk("desired bytes:\n");
		print_hex(desired, length);
		printk("\nreceived (%zu bytes):\n", length);
		print_hex(frame, length);
		printk("\n");
	}

	return ret;
}

static void send_sf(void)
{
	int ret;

	ret = isotp_fast_send(&ctx, random_data, DATA_SIZE_SF, tx_node_id, NULL);
	zassert_equal(ret, 0, "Send returned %d", ret);
}


static void get_sf(struct isotp_fast_ctx *recv_ctx, size_t data_size)
{
	int ret;

	memset(data_buf, 0, sizeof(data_buf));
	ret = blocking_recv(data_buf, sizeof(data_buf), K_MSEC(1000));
	zassert_equal(ret, data_size, "recv returned %d", ret);

	ret = check_data(data_buf, random_data, data_size);
	zassert_equal(ret, 0, "Data differ");
}

static void get_sf_ignore(struct isotp_fast_ctx *recv_ctx)
{
	int ret;

	ret = blocking_recv(data_buf, sizeof(data_buf), K_MSEC(200));
	zassert_equal(ret, ISOTP_RECV_TIMEOUT, "recv returned %d", ret);
}

static void send_test_data(const uint8_t *data, size_t len)
{
	int ret;

	ret = isotp_fast_send(&ctx, data, len, tx_node_id,
					 	  INT_TO_POINTER(ISOTP_N_OK));
	zassert_equal(ret, 0, "Send returned %d", ret);
}

static void receive_test_data(struct isotp_fast_ctx *recv_ctx,
			      const uint8_t *data, size_t len, int32_t delay)
{
	size_t remaining_len = len;
	int ret, recv_len;
	const uint8_t *data_ptr = data;

	do {
		memset(data_buf, 0, sizeof(data_buf));
		recv_len = blocking_recv(data_buf, sizeof(data_buf), K_MSEC(1000));
		zassert_true(recv_len >= 0, "recv error: %d", recv_len);

		zassert_true(remaining_len >= recv_len,
			     "More data than expected");
		ret = check_data(data_buf, data_ptr, recv_len);
		zassert_equal(ret, 0, "Data differ");
		data_ptr += recv_len;
		remaining_len -= recv_len;

		if (delay) {
			k_msleep(delay);
		}
	} while (remaining_len);

	ret = blocking_recv(data_buf, sizeof(data_buf), K_MSEC(50));
	zassert_equal(ret, ISOTP_RECV_TIMEOUT,
		      "Expected timeout but got %d", ret);
}

static void send_frame_series(struct frame_desired *frames, size_t length,
			      uint32_t id)
{
	int i, ret;
	struct can_frame frame = {
		.flags = (id > 0x7FF) ? CAN_FRAME_IDE :	0,
		.id = id
	};
	struct frame_desired *desired = frames;

	for (i = 0; i < length; i++) {
		frame.dlc = desired->length;
		memcpy(frame.data, desired->data, desired->length);
		ret = can_send(can_dev, &frame, K_MSEC(500), NULL, NULL);
		printk("SENT: ");
		print_hex(frame.data, desired->length);
		printk("\n");
		zassert_equal(ret, 0, "Sending msg %d failed.", i);
		desired++;
	}
}

static void check_frame_series(struct frame_desired *frames, size_t length,
			       struct k_msgq *msgq)
{
	int i, ret;
	struct can_frame frame;
	struct frame_desired *desired = frames;

	for (i = 0; i < length; i++) {
		ret = k_msgq_get(msgq, &frame, K_MSEC(500));
		zassert_equal(ret, 0, "Timeout waiting for msg nr %d. ret: %d",
			      i, ret);

		zassert_equal(frame.dlc, desired->length,
			      "DLC of frame nr %d differ. Desired: %d, Got: %d",
			      i, desired->length, frame.dlc);

		ret = check_data(frame.data, desired->data, desired->length);
		zassert_equal(ret, 0, "Data differ");

		desired++;
	}
	ret = k_msgq_get(msgq, &frame, K_MSEC(200));
	zassert_equal(ret, -EAGAIN, "Expected timeout, but received %d", ret);
}

static int add_rx_msgq(uint32_t id, uint32_t mask)
{
	int filter_id;
	struct can_filter filter = {
		.flags = CAN_FILTER_DATA | ((id > 0x7FF) ? CAN_FILTER_IDE : 0),
		.id = id,
		.mask = mask
	};

	filter_id = can_add_rx_filter_msgq(can_dev, &frame_msgq, &filter);
	zassert_not_equal(filter_id, -ENOSPC, "Filter full");
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	return filter_id;
}

static void prepare_cf_frames(struct frame_desired *frames, size_t frames_cnt,
			      const uint8_t *data, size_t data_len)
{
	int i;
	const uint8_t *data_ptr = data;
	size_t remaining_length = data_len;

	for (i = 0; i < frames_cnt && remaining_length; i++) {
		frames[i].data[0] = CF_PCI_BYTE_1 | ((i+1) & 0x0F);
		frames[i].length = CAN_DL;
		memcpy(&des_frames[i].data[1], data_ptr, DATA_SIZE_CF);

		if (remaining_length < DATA_SIZE_CF) {
			frames[i].length = remaining_length + 1;
			remaining_length = 0;
		}

		remaining_length -= DATA_SIZE_CF;
		data_ptr += DATA_SIZE_CF;
	}
}

ZTEST(isotp_fast_conformance, test_send_sf)
{
	int filter_id;
	struct frame_desired des_frame;

	des_frame.data[0] = SF_PCI_BYTE_1;
	memcpy(&des_frame.data[1], random_data, DATA_SIZE_SF);
	des_frame.length = DATA_SIZE_SF + 1;

	filter_id = add_rx_msgq(rx_addr, CAN_EXT_ID_MASK);
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	send_sf();

	check_frame_series(&des_frame, 1, &frame_msgq);

	can_remove_rx_filter(can_dev, filter_id);
}

ZTEST(isotp_fast_conformance, test_receive_sf)
{
	struct frame_desired single_frame;

	single_frame.data[0] = SF_PCI_BYTE_1;
	memcpy(&single_frame.data[1], random_data, DATA_SIZE_SF);
	single_frame.length  = DATA_SIZE_SF + 1;

	send_frame_series(&single_frame, 1, rx_addr);

	get_sf(&ctx, DATA_SIZE_SF);

	single_frame.data[0] = SF_PCI_BYTE_LEN_8;
	send_frame_series(&single_frame, 1, rx_addr);
}

ZTEST(isotp_fast_conformance, test_send_sf_fixed)
{
	int filter_id, ret;
	struct frame_desired des_frame;

	des_frame.data[0] = SF_PCI_BYTE_1;
	memcpy(&des_frame.data[1], random_data, DATA_SIZE_SF);
	des_frame.length = DATA_SIZE_SF + 1;

	/* mask to allow any priority and source address (SA) */
	filter_id = add_rx_msgq(rx_addr, 0x03FFFF00);
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	ret = isotp_fast_send(&ctx, random_data, DATA_SIZE_SF,
			 tx_node_id,
			 INT_TO_POINTER(ISOTP_N_OK));
	zassert_equal(ret, 0, "Send returned %d", ret);

	check_frame_series(&des_frame, 1, &frame_msgq);

	can_remove_rx_filter(can_dev, filter_id);
}

ZTEST(isotp_fast_conformance, test_receive_sf_fixed)
{
	struct frame_desired single_frame;

	single_frame.data[0] = SF_PCI_BYTE_1;
	memcpy(&single_frame.data[1], random_data, DATA_SIZE_SF);
	single_frame.length  = DATA_SIZE_SF + 1;

	/* default source address */
	send_frame_series(&single_frame, 1, rx_addr);
	get_sf(&ctx, DATA_SIZE_SF);

	/* different source address */
	send_frame_series(&single_frame, 1, rx_addr | 0xFF);
	get_sf(&ctx, DATA_SIZE_SF);

	/* different priority */
	send_frame_series(&single_frame, 1, rx_addr | (7U << 26));
	get_sf(&ctx, DATA_SIZE_SF);

	/* different target address (should fail) */
	send_frame_series(&single_frame, 1, rx_addr | 0xFF00);
	get_sf_ignore(&ctx);
}

ZTEST(isotp_fast_conformance, test_send_data)
{
	struct frame_desired fc_frame, ff_frame;
	const uint8_t *data_ptr = random_data;
	size_t remaining_length = DATA_SEND_LENGTH;
	int filter_id;

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SEND_LENGTH);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SEND_LENGTH);
	memcpy(&ff_frame.data[2], data_ptr, DATA_SIZE_FF);
	ff_frame.length = CAN_DL;
	data_ptr += DATA_SIZE_FF;
	remaining_length -= DATA_SIZE_FF;

	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	fc_frame.data[1] = FC_PCI_BYTE_2(0);
	fc_frame.data[2] = FC_PCI_BYTE_3(0);
	fc_frame.length = DATA_SIZE_FC;

	prepare_cf_frames(des_frames, ARRAY_SIZE(des_frames), data_ptr,
			  remaining_length);

	filter_id = add_rx_msgq(rx_addr, CAN_STD_ID_MASK);
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	send_test_data(random_data, DATA_SEND_LENGTH);

	check_frame_series(&ff_frame, 1, &frame_msgq);

	send_frame_series(&fc_frame, 1, tx_addr);

	check_frame_series(des_frames, ARRAY_SIZE(des_frames), &frame_msgq);

	can_remove_rx_filter(can_dev, filter_id);
}

ZTEST(isotp_fast_conformance, test_send_data_blocks)
{
	const uint8_t *data_ptr = random_data;
	size_t remaining_length = DATA_SEND_LENGTH;
	struct frame_desired *data_frame_ptr = des_frames;
	int filter_id, ret;
	struct can_frame dummy_frame;
	struct frame_desired fc_frame, ff_frame;

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SEND_LENGTH);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SEND_LENGTH);
	memcpy(&ff_frame.data[2], data_ptr, DATA_SIZE_FF);
	ff_frame.length = DATA_SIZE_FF + 2;
	data_ptr += DATA_SIZE_FF;
	remaining_length -= DATA_SIZE_FF;

	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	fc_frame.data[1] = FC_PCI_BYTE_2(fc_opts.bs);
	fc_frame.data[2] = FC_PCI_BYTE_3(0);
	fc_frame.length = DATA_SIZE_FC;

	prepare_cf_frames(des_frames, ARRAY_SIZE(des_frames), data_ptr,
			  remaining_length);

	remaining_length = DATA_SEND_LENGTH;

	filter_id = add_rx_msgq(rx_addr, CAN_STD_ID_MASK);
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	send_test_data(random_data, DATA_SEND_LENGTH);

	check_frame_series(&ff_frame, 1, &frame_msgq);
	remaining_length -= DATA_SIZE_FF;

	send_frame_series(&fc_frame, 1, tx_addr);

	check_frame_series(data_frame_ptr, fc_opts.bs, &frame_msgq);
	data_frame_ptr += fc_opts.bs;
	remaining_length -= fc_opts.bs * DATA_SIZE_CF;
	ret = k_msgq_get(&frame_msgq, &dummy_frame, K_MSEC(50));
	zassert_equal(ret, -EAGAIN, "Expected timeout but got %d", ret);

	fc_frame.data[1] = FC_PCI_BYTE_2(2);
	send_frame_series(&fc_frame, 1, tx_addr);

	/* dynamic bs */
	check_frame_series(data_frame_ptr, 2, &frame_msgq);
	data_frame_ptr += 2;
	remaining_length -= 2  * DATA_SIZE_CF;
	ret = k_msgq_get(&frame_msgq, &dummy_frame, K_MSEC(50));
	zassert_equal(ret, -EAGAIN, "Expected timeout but got %d", ret);

	/* get the rest */
	fc_frame.data[1] = FC_PCI_BYTE_2(0);
	send_frame_series(&fc_frame, 1, tx_addr);

	check_frame_series(data_frame_ptr,
			   DIV_ROUND_UP(remaining_length, DATA_SIZE_CF),
			   &frame_msgq);
	ret = k_msgq_get(&frame_msgq, &dummy_frame, K_MSEC(50));
	zassert_equal(ret, -EAGAIN, "Expected timeout but got %d", ret);

	can_remove_rx_filter(can_dev, filter_id);
}

ZTEST(isotp_fast_conformance, test_receive_data)
{
	const uint8_t *data_ptr = random_data;
	size_t remaining_length = DATA_SEND_LENGTH;
	int filter_id;
	struct frame_desired fc_frame, ff_frame;

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SEND_LENGTH);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SEND_LENGTH);
	memcpy(&ff_frame.data[2], data_ptr, DATA_SIZE_FF);
	ff_frame.length = CAN_DL;
	data_ptr += DATA_SIZE_FF;
	remaining_length -= DATA_SIZE_FF;

	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	fc_frame.data[1] = FC_PCI_BYTE_2(fc_opts.bs);
	fc_frame.data[2] = FC_PCI_BYTE_3(fc_opts.stmin);
	fc_frame.length = DATA_SIZE_FC;

	prepare_cf_frames(des_frames, ARRAY_SIZE(des_frames), data_ptr,
			  remaining_length);

	filter_id = add_rx_msgq(tx_addr, CAN_STD_ID_MASK);

	send_frame_series(&ff_frame, 1, rx_addr);

	check_frame_series(&fc_frame, 1, &frame_msgq);

	send_frame_series(des_frames, ARRAY_SIZE(des_frames), rx_addr);

	receive_test_data(&ctx, random_data, DATA_SEND_LENGTH, 0);

	can_remove_rx_filter(can_dev, filter_id);
}

ZTEST(isotp_fast_conformance, test_receive_data_blocks)
{
	const uint8_t *data_ptr = random_data;
	size_t remaining_length = DATA_SEND_LENGTH;
	struct frame_desired *data_frame_ptr = des_frames;
	int filter_id, ret;
	size_t remaining_frames;
	struct frame_desired fc_frame, ff_frame;

	struct can_frame dummy_frame;

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SEND_LENGTH);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SEND_LENGTH);
	memcpy(&ff_frame.data[2], data_ptr, DATA_SIZE_FF);
	ff_frame.length = DATA_SIZE_FF + 2;
	data_ptr += DATA_SIZE_FF;
	remaining_length -= DATA_SIZE_FF;

	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	fc_frame.data[1] = FC_PCI_BYTE_2(fc_opts.bs);
	fc_frame.data[2] = FC_PCI_BYTE_3(fc_opts.stmin);
	fc_frame.length = DATA_SIZE_FC;

	prepare_cf_frames(des_frames, ARRAY_SIZE(des_frames), data_ptr,
			  remaining_length);

	remaining_frames = DIV_ROUND_UP(remaining_length, DATA_SIZE_CF);

	filter_id = add_rx_msgq(tx_addr, CAN_STD_ID_MASK);
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	send_frame_series(&ff_frame, 1, rx_addr);

	while (remaining_frames) {
		check_frame_series(&fc_frame, 1, &frame_msgq);

		if (remaining_frames >= fc_opts.bs) {
			send_frame_series(data_frame_ptr, fc_opts.bs,
					  rx_addr);
			data_frame_ptr += fc_opts.bs;
			remaining_frames -= fc_opts.bs;
		} else {
			send_frame_series(data_frame_ptr, remaining_frames,
					  rx_addr);
			data_frame_ptr += remaining_frames;
			remaining_frames = 0;
		}
	}

	ret = k_msgq_get(&frame_msgq, &dummy_frame, K_MSEC(50));
	zassert_equal(ret, -EAGAIN, "Expected timeout but got %d", ret);

	receive_test_data(&ctx, random_data, DATA_SEND_LENGTH, 0);

	can_remove_rx_filter(can_dev, filter_id);
}

ZTEST(isotp_fast_conformance, test_send_timeouts)
{
	int ret;
	uint32_t start_time, time_diff;
	struct frame_desired fc_cts_frame;

	fc_cts_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	fc_cts_frame.data[1] = FC_PCI_BYTE_2(fc_opts.bs);
	fc_cts_frame.data[2] = FC_PCI_BYTE_3(0);
	fc_cts_frame.length = DATA_SIZE_FC;

	/* Test timeout for first FC*/
	start_time = k_uptime_get_32();
	ret = isotp_fast_send(&ctx, random_data, sizeof(random_data),
			 tx_node_id, NULL);
	time_diff = k_uptime_get_32() - start_time;
	zassert_equal(ret, ISOTP_N_TIMEOUT_BS, "Expected timeout but got %d",
		      ret);
	zassert_true(time_diff <= BS_TIMEOUT_UPPER_MS,
		     "Timeout too late (%dms)",  time_diff);
	zassert_true(time_diff >= BS_TIMEOUT_LOWER_MS,
		     "Timeout too early (%dms)", time_diff);

	/* Test timeout for consecutive FC frames */
	k_sem_reset(&send_compl_sem);
	ret = isotp_fast_send(&ctx, random_data, sizeof(random_data),
			 tx_node_id,
			 INT_TO_POINTER(ISOTP_N_TIMEOUT_BS));
	zassert_equal(ret, ISOTP_N_OK, "Send returned %d", ret);

	send_frame_series(&fc_cts_frame, 1, rx_addr);

	start_time = k_uptime_get_32();
	ret = k_sem_take(&send_compl_sem, K_MSEC(BS_TIMEOUT_UPPER_MS));
	zassert_equal(ret, 0, "Timeout too late");

	time_diff = k_uptime_get_32() - start_time;
	zassert_true(time_diff >= BS_TIMEOUT_LOWER_MS,
		     "Timeout too early (%dms)", time_diff);

	/* Test timeout reset with WAIT frame */
	k_sem_reset(&send_compl_sem);
	ret = isotp_fast_send(&ctx, random_data, sizeof(random_data),
			 tx_node_id,
			 INT_TO_POINTER(ISOTP_N_TIMEOUT_BS));
	zassert_equal(ret, ISOTP_N_OK, "Send returned %d", ret);

	ret = k_sem_take(&send_compl_sem, K_MSEC(800));
	zassert_equal(ret, -EAGAIN, "Timeout too early");

	fc_cts_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	send_frame_series(&fc_cts_frame, 1, rx_addr);

	start_time = k_uptime_get_32();
	ret = k_sem_take(&send_compl_sem, K_MSEC(BS_TIMEOUT_UPPER_MS));
	zassert_equal(ret, 0, "Timeout too late");
	time_diff = k_uptime_get_32() - start_time;
	zassert_true(time_diff >= BS_TIMEOUT_LOWER_MS,
		     "Timeout too early (%dms)", time_diff);
}

ZTEST(isotp_fast_conformance, test_receive_timeouts)
{
	int ret;
	uint32_t start_time, time_diff;
	struct frame_desired ff_frame;

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SEND_LENGTH);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SEND_LENGTH);
	memcpy(&ff_frame.data[2], random_data, DATA_SIZE_FF);
	ff_frame.length = DATA_SIZE_FF + 2;

	send_frame_series(&ff_frame, 1, rx_addr);
	start_time = k_uptime_get_32();

	ret = blocking_recv(data_buf, sizeof(data_buf), K_FOREVER);
	zassert_equal(ret, DATA_SIZE_FF,
		      "Expected FF data length but got %d", ret);
	ret = blocking_recv(data_buf, sizeof(data_buf), K_FOREVER);
	zassert_equal(ret, ISOTP_N_TIMEOUT_CR,
		      "Expected timeout but got %d", ret);

	time_diff = k_uptime_get_32() - start_time;
	zassert_true(time_diff >= BS_TIMEOUT_LOWER_MS,
		     "Timeout too early (%dms)", time_diff);
	zassert_true(time_diff <= BS_TIMEOUT_UPPER_MS,
		     "Timeout too slow (%dms)", time_diff);
}

ZTEST(isotp_fast_conformance, test_stmin)
{
	int filter_id, ret;
	struct frame_desired fc_frame, ff_frame;
	struct can_frame raw_frame;
	uint32_t start_time, time_diff;

	if (CONFIG_SYS_CLOCK_TICKS_PER_SEC < 1000) {
		/* This test requires millisecond tick resolution */
		ztest_test_skip();
	}

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SIZE_FF + DATA_SIZE_CF * 4);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SIZE_FF + DATA_SIZE_CF * 4);
	memcpy(&ff_frame.data[2], random_data, DATA_SIZE_FF);
	ff_frame.length = DATA_SIZE_FF + 2;

	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	fc_frame.data[1] = FC_PCI_BYTE_2(2);
	fc_frame.data[2] = FC_PCI_BYTE_3(STMIN_VAL_1);
	fc_frame.length = DATA_SIZE_FC;

	filter_id = add_rx_msgq(rx_addr, CAN_STD_ID_MASK);
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	send_test_data(random_data, DATA_SIZE_FF + DATA_SIZE_CF * 4);

	check_frame_series(&ff_frame, 1, &frame_msgq);

	send_frame_series(&fc_frame, 1, tx_addr);

	ret = k_msgq_get(&frame_msgq, &raw_frame, K_MSEC(100));
	zassert_equal(ret, 0, "Expected to get a message. [%d]", ret);

	start_time = k_uptime_get_32();
	ret = k_msgq_get(&frame_msgq, &raw_frame,
			 K_MSEC(STMIN_VAL_1 + STMIN_UPPER_TOLERANCE));
	time_diff = k_uptime_get_32() - start_time;
	zassert_equal(ret, 0, "Expected to get a message within %dms. [%d]",
		      STMIN_VAL_1 + STMIN_UPPER_TOLERANCE, ret);
	zassert_true(time_diff >= STMIN_VAL_1, "STmin too short (%dms)",
		     time_diff);

	fc_frame.data[2] = FC_PCI_BYTE_3(STMIN_VAL_2);
	send_frame_series(&fc_frame, 1, tx_addr);

	ret = k_msgq_get(&frame_msgq, &raw_frame, K_MSEC(100));
	zassert_equal(ret, 0, "Expected to get a message. [%d]", ret);

	start_time = k_uptime_get_32();
	ret = k_msgq_get(&frame_msgq, &raw_frame,
			 K_MSEC(STMIN_VAL_2 + STMIN_UPPER_TOLERANCE));
	time_diff = k_uptime_get_32() - start_time;
	zassert_equal(ret, 0, "Expected to get a message within %dms. [%d]",
		      STMIN_VAL_2 + STMIN_UPPER_TOLERANCE, ret);
	zassert_true(time_diff >= STMIN_VAL_2, "STmin too short (%dms)",
		     time_diff);

	can_remove_rx_filter(can_dev, filter_id);
}

ZTEST(isotp_fast_conformance, test_receiver_fc_errors)
{
	int ret, filter_id;
	struct frame_desired ff_frame, fc_frame;

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SEND_LENGTH);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SEND_LENGTH);
	memcpy(&ff_frame.data[2], random_data, DATA_SIZE_FF);
	ff_frame.length = DATA_SIZE_FF + 2;

	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_CTS);
	fc_frame.data[1] = FC_PCI_BYTE_2(fc_opts.bs);
	fc_frame.data[2] = FC_PCI_BYTE_3(fc_opts.stmin);
	fc_frame.length = DATA_SIZE_FC;

	filter_id = add_rx_msgq(tx_addr, CAN_STD_ID_MASK);
	zassert_true((filter_id >= 0), "Negative filter number [%d]",
		     filter_id);

	/* wrong sequence number */
	send_frame_series(&ff_frame, 1, rx_addr);
	check_frame_series(&fc_frame, 1, &frame_msgq);
	prepare_cf_frames(des_frames, ARRAY_SIZE(des_frames),
			  random_data + DATA_SIZE_FF,
			  sizeof(random_data) - DATA_SIZE_FF);
	/* SN should be 2 but is set to 3 for this test */
	des_frames[1].data[0] = CF_PCI_BYTE_1 | (3 & 0x0F);
	send_frame_series(des_frames, fc_opts.bs, rx_addr);

	ret = blocking_recv(data_buf, sizeof(data_buf), K_MSEC(200));
	zassert_equal(ret, DATA_SIZE_FF,
		      "Expected FF data length but got %d", ret);
	ret = blocking_recv(data_buf, sizeof(data_buf), K_MSEC(200));
	zassert_equal(ret, ISOTP_N_WRONG_SN,
		      "Expected wrong SN but got %d", ret);

	can_remove_rx_filter(can_dev, filter_id);
	k_msgq_cleanup(&frame_msgq);
}

ZTEST(isotp_fast_conformance, test_sender_fc_errors)
{
	int ret, filter_id, i;
	struct frame_desired ff_frame, fc_frame;

	ff_frame.data[0] = FF_PCI_BYTE_1(DATA_SEND_LENGTH);
	ff_frame.data[1] = FF_PCI_BYTE_2(DATA_SEND_LENGTH);
	memcpy(&ff_frame.data[2], random_data, DATA_SIZE_FF);
	ff_frame.length = DATA_SIZE_FF + 2;

	filter_id = add_rx_msgq(tx_addr, CAN_STD_ID_MASK);

	/* invalid flow status */
	fc_frame.data[0] = FC_PCI_BYTE_1(3);
	fc_frame.data[1] = FC_PCI_BYTE_2(fc_opts.bs);
	fc_frame.data[2] = FC_PCI_BYTE_3(fc_opts.stmin);
	fc_frame.length = DATA_SIZE_FC;

	k_sem_reset(&send_compl_sem);
	ret = isotp_fast_send(&ctx, random_data, DATA_SEND_LENGTH,
			 tx_node_id,
			 INT_TO_POINTER(ISOTP_N_INVALID_FS));
	zassert_equal(ret, ISOTP_N_OK, "Send returned %d", ret);

	check_frame_series(&ff_frame, 1, &frame_msgq);
	send_frame_series(&fc_frame, 1, rx_addr);
	ret = k_sem_take(&send_compl_sem, K_MSEC(200));
	zassert_equal(ret, 0, "Send complete callback not called");

	/* buffer overflow */
	can_remove_rx_filter(can_dev, filter_id);

	ret = isotp_fast_send(&ctx, random_data, 5*1024,
			 tx_node_id, NULL);
	zassert_equal(ret, ISOTP_N_BUFFER_OVERFLW,
		      "Expected overflow but got %d", ret);
	filter_id = add_rx_msgq(tx_addr, CAN_STD_ID_MASK);

	k_sem_reset(&send_compl_sem);
	ret = isotp_fast_send(&ctx, random_data, DATA_SEND_LENGTH,
			 tx_node_id,
			 INT_TO_POINTER(ISOTP_N_BUFFER_OVERFLW));

	check_frame_series(&ff_frame, 1, &frame_msgq);
	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_OVFLW);
	send_frame_series(&fc_frame, 1, rx_addr);
	ret = k_sem_take(&send_compl_sem, K_MSEC(200));
	zassert_equal(ret, 0, "Send complete callback not called");

	/* wft overrun */
	k_sem_reset(&send_compl_sem);
	ret = isotp_fast_send(&ctx, random_data, DATA_SEND_LENGTH,
			 tx_node_id,
			 INT_TO_POINTER(ISOTP_N_WFT_OVRN));

	check_frame_series(&ff_frame, 1, &frame_msgq);
	fc_frame.data[0] = FC_PCI_BYTE_1(FC_PCI_WAIT);
	for (i = 0; i < CONFIG_ISOTP_WFTMAX + 1; i++) {
		send_frame_series(&fc_frame, 1, rx_addr);
	}

	ret = k_sem_take(&send_compl_sem, K_MSEC(200));
	zassert_equal(ret, 0, "Send complete callback not called");
	k_msgq_cleanup(&frame_msgq);
	can_remove_rx_filter(can_dev, filter_id);
}

void *isotp_fast_conformance_setup(void)
{
	int ret;

	zassert_true(sizeof(random_data) >= sizeof(data_buf) * 2 + 10,
		     "Test data size too small");

	zassert_true(device_is_ready(can_dev), "CAN device not ready");

	ret = can_set_mode(can_dev, CAN_MODE_LOOPBACK);
	zassert_equal(ret, 0, "Failed to set loopback mode [%d]", ret);

	ret = can_start(can_dev);
	zassert_equal(ret, 0, "Failed to start CAN controller [%d]", ret);

	k_sem_init(&send_compl_sem, 0, 1);

	return NULL;
}

void isotp_fast_conformance_before(void *)
{
	isotp_fast_bind(&ctx, can_dev, rx_addr, &fc_opts,
				isotp_fast_recv_handler, NULL,
				isotp_fast_recv_error_handler,
				isotp_fast_sent_handler);
}

void isotp_fast_conformance_after(void *)
{
	isotp_fast_unbind(&ctx);

	k_msgq_purge(&recv_msgq);
	k_msgq_purge(&frame_msgq);
}

ZTEST_SUITE(isotp_fast_conformance, NULL, isotp_fast_conformance_setup,
			isotp_fast_conformance_before, isotp_fast_conformance_after, NULL);
