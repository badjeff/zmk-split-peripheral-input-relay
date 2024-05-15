/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_split_peripheral_input_relay

#include <zephyr/drivers/sensor.h>
#include <zephyr/types.h>
#include <zephyr/sys/util.h>
#include <zephyr/init.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/input/input.h>

#include <zmk/split/input-relay/uuid.h>
#include <zmk/split/input-relay/event.h>

#if CONFIG_INPUT

struct zmk_split_bt_input_relay_event last_input_event;

static ssize_t split_svc_input_state(struct bt_conn *conn, const struct bt_gatt_attr *attrs,
                                      void *buf, uint16_t len, uint16_t offset) {
    return bt_gatt_attr_read(conn, attrs, buf, len, offset, &last_input_event,
                             sizeof(last_input_event));
}

static void split_svc_input_state_ccc(const struct bt_gatt_attr *attr, uint16_t value) {
    LOG_DBG("value %d", value);
}
#endif /* CONFIG_INPUT */

BT_GATT_SERVICE_DEFINE(
    ir_split_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_IR_SERVICE_UUID)),
#if CONFIG_INPUT
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_SPLIT_BT_IR_CHAR_INPUT_STATE_UUID),
                           BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ_ENCRYPT,
                           split_svc_input_state, NULL, &last_input_event),
    BT_GATT_CCC(split_svc_input_state_ccc, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT),
#endif /* CONFIG_INPUT */
);

K_THREAD_STACK_DEFINE(service_ir_q_stack, CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_STACK_SIZE);

struct k_work_q service_ir_work_q;

#if CONFIG_INPUT
K_MSGQ_DEFINE(input_state_msgq, sizeof(struct zmk_split_bt_input_relay_event),
              CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_POSITION_QUEUE_SIZE, 4);

void send_input_state_callback(struct k_work *work) {
    while (k_msgq_get(&input_state_msgq, &last_input_event, K_NO_WAIT) == 0) {
        int err = bt_gatt_notify(NULL, &ir_split_svc.attrs[1], &last_input_event, 
                                 sizeof(last_input_event));
        if (err) {
            LOG_WRN("Error notifying %d", err);
        }
    }
};

K_WORK_DEFINE(service_input_notify_work, send_input_state_callback);

int send_input_state(struct zmk_split_bt_input_relay_event ev) {
    int err = k_msgq_put(&input_state_msgq, &ev, K_MSEC(100));
    if (err) {
        // retry...
        switch (err) {
        case -EAGAIN: {
            LOG_WRN("Input state message queue full, popping first message and queueing again");
            struct zmk_split_bt_input_relay_event discarded_state;
            k_msgq_get(&input_state_msgq, &discarded_state, K_NO_WAIT);
            return send_input_state(ev);
        }
        default:
            LOG_WRN("Failed to queue input state to send (%d)", err);
            return err;
        }
    }

    k_work_submit_to_queue(&service_ir_work_q, &service_input_notify_work);
    return 0;
}

void zmk_split_bt_input_ev_triggered(uint8_t relay_channel, struct input_event *evt) {
    struct zmk_split_bt_input_relay_event ev =
        (struct zmk_split_bt_input_relay_event){
            .relay_channel = relay_channel,
            .sync = evt->sync, .type = evt->type,
            .code = evt->code, .value = evt->value};

    LOG_DBG("Send input: rc-%d t-%d c-%d v-%d s-%d",
        ev.relay_channel, ev.type, ev.code, ev.value, ev.sync?1:0);

    send_input_state(ev);
}

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define RELY_INST(n)                                                                               \
    static uint8_t relay_channel_##n = DT_PROP(DT_DRV_INST(n), relay_channel);                     \
    void input_handler_##n(struct input_event *evt) {                                              \
        zmk_split_bt_input_ev_triggered(relay_channel_##n, evt);                                   \
    }                                                                                              \
    INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_INST_PHANDLE(n, device)), input_handler_##n);

DT_INST_FOREACH_STATUS_OKAY(RELY_INST)

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */

#endif /* CONFIG_INPUT */

static int service_init(void) {
    static const struct k_work_queue_config queue_config = {
        .name = "Split Peripheral Alternative Notification Queue"};
    k_work_queue_start(&service_ir_work_q, service_ir_q_stack, 
                       K_THREAD_STACK_SIZEOF(service_ir_q_stack),
                       CONFIG_ZMK_SPLIT_BLE_PERIPHERAL_PRIORITY, &queue_config);

    return 0;
}

SYS_INIT(service_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
