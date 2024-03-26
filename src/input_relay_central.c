/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_split_peripheral_input_relay

#include <zephyr/types.h>
#include <zephyr/init.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/input/input.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/ble.h>

#include <zmk/split/input-relay/uuid.h>
#include <zmk/split/input-relay/event.h>

enum alt_peripheral_slot_state {
    PERIPHERAL_SLOT_STATE_OPEN,
    PERIPHERAL_SLOT_STATE_CONNECTING,
    PERIPHERAL_SLOT_STATE_CONNECTED,
};

struct alt_peripheral_slot {
    enum alt_peripheral_slot_state state;
    struct bt_conn *conn;
    struct bt_gatt_discover_params discover_params;
    struct bt_gatt_subscribe_params subscribe_params;
    struct bt_gatt_subscribe_params input_subscribe_params;
    struct bt_gatt_discover_params sub_discover_params;
};

static struct alt_peripheral_slot peripherals[ZMK_SPLIT_BLE_PERIPHERAL_COUNT];

static const struct bt_uuid_128 split_alt_service_uuid = BT_UUID_INIT_128(ZMK_SPLIT_BT_ALT_SERVICE_UUID);

int alt_peripheral_slot_index_for_conn(struct bt_conn *conn) {
    for (int i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        if (peripherals[i].conn == conn) {
            return i;
        }
    }
    return -EINVAL;
}

struct alt_peripheral_slot *alt_peripheral_slot_for_conn(struct bt_conn *conn) {
    int idx = alt_peripheral_slot_index_for_conn(conn);
    if (idx < 0) {
        return NULL;
    }
    return &peripherals[idx];
}

int release_alt_peripheral_slot(int index) {
    if (index < 0 || index >= ZMK_SPLIT_BLE_PERIPHERAL_COUNT) {
        return -EINVAL;
    }

    struct alt_peripheral_slot *slot = &peripherals[index];

    if (slot->state == PERIPHERAL_SLOT_STATE_OPEN) {
        return -EINVAL;
    }

    LOG_DBG("Releasing peripheral slot at %d", index);

    if (slot->conn != NULL) {
        slot->conn = NULL;
    }
    slot->state = PERIPHERAL_SLOT_STATE_OPEN;

    // Clean up previously discovered handles;
    slot->subscribe_params.value_handle = 0;

    return 0;
}

int reserve_alt_peripheral_slot_for_conn(struct bt_conn *conn) {
    int i = zmk_ble_put_peripheral_addr(bt_conn_get_dst(conn));
    if (i >= 0) {
        if (peripherals[i].state == PERIPHERAL_SLOT_STATE_OPEN) {
            // Be sure the slot is fully reinitialized.
            release_alt_peripheral_slot(i);
            peripherals[i].conn = conn;
            peripherals[i].state = PERIPHERAL_SLOT_STATE_CONNECTED;
            return i;
        }
    }

    return -ENOMEM;
}

int release_alt_peripheral_slot_for_conn(struct bt_conn *conn) {
    int idx = alt_peripheral_slot_index_for_conn(conn);
    if (idx < 0) {
        return idx;
    }

    return release_alt_peripheral_slot(idx);
}

#if CONFIG_INPUT
K_MSGQ_DEFINE(peripheral_input_event_msgq, sizeof(struct input_event),
              CONFIG_ZMK_SPLIT_BLE_CENTRAL_POSITION_QUEUE_SIZE, 4);

void peripheral_input_event_work_callback(struct k_work *work) {
    struct input_event ev;
    while (k_msgq_get(&peripheral_input_event_msgq, &ev, K_NO_WAIT) == 0) {
        LOG_DBG("Trigger input change for %d/%d/%s", ev.code, ev.value, ev.sync?"s":"ns");
        input_report_rel(ev.dev, ev.code, ev.value, ev.sync, K_NO_WAIT);
    }
}

K_WORK_DEFINE(peripheral_input_event_work, peripheral_input_event_work_callback);

const struct device* device_get_for_relay_channel(uint8_t relay_channel);

static uint8_t split_central_input_notify_func(struct bt_conn *conn,
                                               struct bt_gatt_subscribe_params *params,
                                               const void *data, uint16_t length) {
    if (!data) {
        LOG_DBG("[UNSUBSCRIBED]");
        params->value_handle = 0U;
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[INPUT NOTIFICATION] data %p length %u", data, length);

    struct zmk_split_bt_input_event evt;
    memcpy(&evt, data, MIN(length, sizeof(struct zmk_split_bt_input_event)));

    const struct device *dev = device_get_for_relay_channel(evt.relay_channel);
    if (dev == NULL) {
        LOG_DBG("Unable to retrieve virtual device for channel: %d", evt.relay_channel);
        return BT_GATT_ITER_CONTINUE;
    }

    struct input_event ev = {
        .dev = dev,
        .sync = evt.sync, .type = evt.type,
        .code = evt.code, .value = evt.value};

    k_msgq_put(&peripheral_input_event_msgq, &ev, K_NO_WAIT);
    k_work_submit(&peripheral_input_event_work);

    return BT_GATT_ITER_CONTINUE;
}
#endif /* CONFIG_INPUT */

static int split_central_subscribe(struct bt_conn *conn, struct bt_gatt_subscribe_params *params) {
    int err = bt_gatt_subscribe(conn, params);
    switch (err) {
    case -EALREADY:
        LOG_DBG("[ALREADY SUBSCRIBED]");
        break;
    case 0:
        LOG_DBG("[SUBSCRIBED]");
        break;
    default:
        LOG_ERR("Subscribe failed (err %d)", err);
        break;
    }

    return err;
}

static uint8_t split_central_chrc_discovery_func(struct bt_conn *conn,
                                                 const struct bt_gatt_attr *attr,
                                                 struct bt_gatt_discover_params *params) {
    if (!attr) {
        LOG_DBG("Discover complete");
        return BT_GATT_ITER_STOP;
    }

    if (!attr->user_data) {
        LOG_ERR("Required user data not passed to discovery");
        return BT_GATT_ITER_STOP;
    }

    struct alt_peripheral_slot *slot = alt_peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);
    const struct bt_uuid *chrc_uuid = ((struct bt_gatt_chrc *)attr->user_data)->uuid;

#if CONFIG_INPUT
    if (bt_uuid_cmp(chrc_uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_ALT_CHAR_INPUT_STATE_UUID)) == 0) {
        LOG_DBG("Found input state characteristic");
        slot->discover_params.uuid = NULL;
        slot->discover_params.start_handle = attr->handle + 2;
        slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

        slot->input_subscribe_params.disc_params = &slot->sub_discover_params;
        slot->input_subscribe_params.end_handle = slot->discover_params.end_handle;
        slot->input_subscribe_params.value_handle = bt_gatt_attr_value_handle(attr);
        slot->input_subscribe_params.notify = split_central_input_notify_func;
        slot->input_subscribe_params.value = BT_GATT_CCC_NOTIFY;
        split_central_subscribe(conn, &slot->input_subscribe_params);
    }
#endif /* CONFIG_INPUT */

    bool subscribed = true;

#if CONFIG_INPUT
    subscribed = subscribed && slot->input_subscribe_params.value_handle;
#endif /* CONFIG_INPUT */

    return subscribed ? BT_GATT_ITER_STOP : BT_GATT_ITER_CONTINUE;
}

static uint8_t split_central_service_discovery_func(struct bt_conn *conn,
                                                    const struct bt_gatt_attr *attr,
                                                    struct bt_gatt_discover_params *params) {
    if (!attr) {
        LOG_DBG("Discover complete");
        (void)memset(params, 0, sizeof(*params));
        return BT_GATT_ITER_STOP;
    }

    LOG_DBG("[ATTRIBUTE] handle %u", attr->handle);

    struct alt_peripheral_slot *slot = alt_peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return BT_GATT_ITER_STOP;
    }

    if (bt_uuid_cmp(slot->discover_params.uuid, BT_UUID_DECLARE_128(ZMK_SPLIT_BT_ALT_SERVICE_UUID)) !=
        0) {
        LOG_DBG("Found other service");
        return BT_GATT_ITER_CONTINUE;
    }

    LOG_DBG("Found split service");
    slot->discover_params.uuid = NULL;
    slot->discover_params.func = split_central_chrc_discovery_func;
    slot->discover_params.type = BT_GATT_DISCOVER_CHARACTERISTIC;

    int err = bt_gatt_discover(conn, &slot->discover_params);
    if (err) {
        LOG_ERR("Failed to start discovering split service characteristics (err %d)", err);
    }
    return BT_GATT_ITER_STOP;
}

static void split_central_process_connection(struct bt_conn *conn) {
    int err;

    LOG_DBG("Current security for connection: %d", bt_conn_get_security(conn));

    struct alt_peripheral_slot *slot = alt_peripheral_slot_for_conn(conn);
    if (slot == NULL) {
        LOG_ERR("No peripheral state found for connection");
        return;
    }

    if (!slot->subscribe_params.value_handle) {
        slot->discover_params.uuid = &split_alt_service_uuid.uuid;
        slot->discover_params.func = split_central_service_discovery_func;
        slot->discover_params.start_handle = 0x0001;
        slot->discover_params.end_handle = 0xffff;
        slot->discover_params.type = BT_GATT_DISCOVER_PRIMARY;

        err = bt_gatt_discover(slot->conn, &slot->discover_params);
        if (err) {
            LOG_ERR("Discover failed(err %d)", err);
            return;
        }
    }

    struct bt_conn_info info;

    bt_conn_get_info(conn, &info);

    LOG_DBG("New connection params: Interval: %d, Latency: %d, PHY: %d", info.le.interval,
            info.le.latency, info.le.phy->rx_phy);
}

static void split_central_connected(struct bt_conn *conn, uint8_t conn_err) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    struct bt_conn_info info;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    bt_conn_get_info(conn, &info);

    if (info.role != BT_CONN_ROLE_CENTRAL) {
        LOG_DBG("SKIPPING FOR ROLE %d", info.role);
        return;
    }

    if (conn_err) {
        LOG_ERR("Failed to connect to %s (%u)", addr_str, conn_err);
        release_alt_peripheral_slot_for_conn(conn);
        return;
    }

    LOG_DBG("Connected: %s", addr_str);

    int slot_idx = reserve_alt_peripheral_slot_for_conn(conn);
    if (slot_idx < 0) {
        LOG_ERR("Unable to reserve peripheral slot for connection (err %d)", slot_idx);
        return;
    }

    split_central_process_connection(conn);
}

static void split_central_disconnected(struct bt_conn *conn, uint8_t reason) {
    char addr_str[BT_ADDR_LE_STR_LEN];
    int err;

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr_str, sizeof(addr_str));

    LOG_DBG("Disconnected: %s (reason %d)", addr_str, reason);

    err = release_alt_peripheral_slot_for_conn(conn);

    if (err < 0) {
        return;
    }
}

static struct bt_conn_cb conn_callbacks = {
    .connected = split_central_connected,
    .disconnected = split_central_disconnected,
};

static int zmk_split_bt_central_init(void) {
    bt_conn_cb_register(&conn_callbacks);
    return 0;
}

SYS_INIT(zmk_split_bt_central_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);


#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct split_peripheral_input_relay_config {
    uint8_t relay_channel;
    const struct device *device;
};

#define RELY_CFG_DEFINE(n)                                                                         \
    static const struct split_peripheral_input_relay_config config_##n = {                         \
        .relay_channel = DT_PROP(DT_DRV_INST(0), relay_channel),                                   \
        .device = DEVICE_DT_GET(DT_INST_PHANDLE(n, device)),                                       \
    };

DT_INST_FOREACH_STATUS_OKAY(RELY_CFG_DEFINE)

const struct device* device_get_for_relay_channel(uint8_t relay_channel) {
    #define COND_CMP_RELAY_CHANNEL(n)                               \
        if (relay_channel == config_##n.relay_channel) {            \
            return config_##n.device;                               \
        }
    DT_INST_FOREACH_STATUS_OKAY(COND_CMP_RELAY_CHANNEL)
    return NULL;
}

#else

const struct device* device_get_for_relay_channel(uint8_t relay_channel) {
    return NULL;
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
