/*
 * Copyright (c) 2022 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci_types.h>
#include <zephyr/sys/util.h>

#if IS_ENABLED(CONFIG_SETTINGS)

#include <zephyr/settings/settings.h>

#endif

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/ble.h>
#include <zmk/split/bluetooth/uuid.h>
#include <zmk/split/bluetooth/peripheral.h>

/* -----------------------------------------------------------------------
 * Dual-dongle slot support
 * Up to CONFIG_ZMK_SPLIT_PERIPHERAL_DONGLE_PROFILES centrals can be bonded.
 * The active slot determines which dongle the peripheral directs advertising
 * toward.  Use zmk_split_bt_peripheral_select_dongle() to switch slots.
 * ----------------------------------------------------------------------- */

#define DONGLE_MAX_PROFILES CONFIG_ZMK_SPLIT_PERIPHERAL_DONGLE_PROFILES

static const struct bt_data zmk_ble_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID16_SOME, 0x0f, 0x18 /* Battery Service */
                  ),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, ZMK_SPLIT_BT_SERVICE_UUID)};

static bool is_connected = false;
static bool is_bonded = false;

/* Active connection reference (NULL when not connected). */
static struct bt_conn *current_conn = NULL;

/* Ordered list of bonded dongle (central) addresses.
 * Slot 0 = first paired dongle (e.g., home), slot 1 = second (e.g., work). */
static bt_addr_le_t dongle_addrs[DONGLE_MAX_PROFILES];
static uint8_t dongle_count = 0; /* how many slots have a stored address */
static uint8_t active_dongle_slot = 0;

struct bond_scan_ctx {
    bt_addr_le_t *addrs;
    uint8_t max;
    uint8_t count;
};

static void each_bond(const struct bt_bond_info *info, void *user_data) {
    struct bond_scan_ctx *ctx = (struct bond_scan_ctx *)user_data;

    if (ctx->count >= ctx->max) {
        return;
    }

    if (bt_addr_le_cmp(&info->addr, BT_ADDR_LE_NONE) != 0) {
        bt_addr_le_copy(&ctx->addrs[ctx->count], &info->addr);
        ctx->count++;
    }
}

static void refresh_dongle_slots_from_bonds(void) {
    struct bond_scan_ctx ctx = {
        .addrs = dongle_addrs,
        .max = DONGLE_MAX_PROFILES,
        .count = 0,
    };

    memset(dongle_addrs, 0, sizeof(dongle_addrs));
    bt_foreach_bond(BT_ID_DEFAULT, each_bond, &ctx);
    dongle_count = ctx.count;

    if (dongle_count == 0) {
        active_dongle_slot = 0;
    } else if (active_dongle_slot >= dongle_count) {
        active_dongle_slot = 0;
    }
}

/* -----------------------------------------------------------------------
 * Settings helpers
 * ----------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_SETTINGS)

static void save_dongle_config(void) {
    settings_save_one("ble_peripheral/dslot", &active_dongle_slot, sizeof(active_dongle_slot));
    settings_save_one("ble_peripheral/dcount", &dongle_count, sizeof(dongle_count));
    settings_save_one("ble_peripheral/daddrs", dongle_addrs, sizeof(dongle_addrs));
}

#else
static void save_dongle_config(void) {}
#endif /* CONFIG_SETTINGS */

/* -----------------------------------------------------------------------
 * Advertising
 * ----------------------------------------------------------------------- */

static bool low_duty_advertising = false;

static int start_advertising(bool low_duty) {
    /* Backward-compatible behavior: for devices with existing bonds and no
     * stored slot table yet, seed from the BLE bond list. */
    if (dongle_count == 0) {
        refresh_dongle_slots_from_bonds();
    }

    /* Use the stored address for the active slot if available. */
    if (active_dongle_slot < dongle_count &&
        bt_addr_le_cmp(&dongle_addrs[active_dongle_slot], BT_ADDR_LE_NONE) != 0) {
        is_bonded = true;
        const bt_addr_le_t *target = &dongle_addrs[active_dongle_slot];

        if (DONGLE_MAX_PROFILES > 1) {
            char addr_str[BT_ADDR_LE_STR_LEN];
            bt_addr_le_to_str(target, addr_str, sizeof(addr_str));
            LOG_DBG("Directed advertising to dongle slot %u: %s", active_dongle_slot, addr_str);
        }

        struct bt_le_adv_param adv_param =
            low_duty ? *BT_LE_ADV_CONN_DIR_LOW_DUTY(target) : *BT_LE_ADV_CONN_DIR(target);
        return bt_le_adv_start(&adv_param, NULL, 0, NULL, 0);
    }

    /* No stored address for the active slot — undirected (pairing mode). */
    is_bonded = (dongle_count > 0);
    if (DONGLE_MAX_PROFILES > 1) {
        LOG_DBG("Undirected advertising (slot %u not yet paired)", active_dongle_slot);
    }
    return bt_le_adv_start(BT_LE_ADV_CONN, zmk_ble_ad, ARRAY_SIZE(zmk_ble_ad), NULL, 0);
}

static void advertising_cb(struct k_work *work) {
    const int err = start_advertising(low_duty_advertising);
    if (err < 0) {
        LOG_ERR("Failed to start advertising (%d)", err);
    }
}

K_WORK_DEFINE(advertising_work, advertising_cb);

/* -----------------------------------------------------------------------
 * Connection callbacks
 * ----------------------------------------------------------------------- */

static void connected(struct bt_conn *conn, uint8_t err) {
    if (err == 0) {
        is_connected = true;
        current_conn = bt_conn_ref(conn);
    } else {
        is_connected = false;
    }

    raise_zmk_split_peripheral_status_changed(
        (struct zmk_split_peripheral_status_changed){.connected = is_connected});

    if (err == BT_HCI_ERR_ADV_TIMEOUT) {
        low_duty_advertising = true;
        k_work_submit(&advertising_work);
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_DBG("Disconnected from %s (reason 0x%02x)", addr, reason);

    is_connected = false;

    if (current_conn) {
        bt_conn_unref(current_conn);
        current_conn = NULL;
    }

    raise_zmk_split_peripheral_status_changed(
        (struct zmk_split_peripheral_status_changed){.connected = is_connected});

    low_duty_advertising = false;
    k_work_submit(&advertising_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));

    if (!err) {
        LOG_DBG("Security changed: %s level %u", addr, level);
    } else {
        LOG_ERR("Security failed: %s level %u err %d", addr, level, err);
    }
}

static void le_param_updated(struct bt_conn *conn, uint16_t interval, uint16_t latency,
                             uint16_t timeout) {
    char addr[BT_ADDR_LE_STR_LEN];
    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    LOG_DBG("%s: interval %d latency %d timeout %d", addr, interval, latency, timeout);
}

static struct bt_conn_cb conn_callbacks = {
    .connected = connected,
    .disconnected = disconnected,
    .security_changed = security_changed,
    .le_param_updated = le_param_updated,
};

/* -----------------------------------------------------------------------
 * Pairing callback — register new dongle address in the next free slot.
 * ----------------------------------------------------------------------- */

static void auth_pairing_complete(struct bt_conn *conn, bool bonded) {
    is_bonded = bonded;
    if (!bonded) {
        return;
    }

    const bt_addr_le_t *peer = bt_conn_get_dst(conn);

    /* Check if this address is already stored. */
    for (uint8_t i = 0; i < dongle_count; i++) {
        if (bt_addr_le_cmp(&dongle_addrs[i], peer) == 0) {
            LOG_DBG("Dongle already registered at slot %u", i);
            return;
        }
    }

    /* Store in next available slot. */
    if (dongle_count < DONGLE_MAX_PROFILES) {
        char addr_str[BT_ADDR_LE_STR_LEN];
        bt_addr_le_to_str(peer, addr_str, sizeof(addr_str));
        bt_addr_le_copy(&dongle_addrs[dongle_count], peer);
        if (DONGLE_MAX_PROFILES > 1) {
            LOG_INF("Registered dongle at slot %u: %s", dongle_count, addr_str);
        }
        dongle_count++;
        save_dongle_config();
    } else {
        LOG_WRN("All %d dongle slots are full; new dongle not registered", DONGLE_MAX_PROFILES);
    }
}

static struct bt_conn_auth_info_cb zmk_peripheral_ble_auth_info_cb = {
    .pairing_complete = auth_pairing_complete,
};

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

bool zmk_split_bt_peripheral_is_connected(void) { return is_connected; }

bool zmk_split_bt_peripheral_is_bonded(void) { return is_bonded; }

int zmk_split_bt_peripheral_select_dongle(uint8_t slot) {
    if (slot >= DONGLE_MAX_PROFILES) {
        LOG_ERR("Dongle slot %u out of range (max %d)", slot, DONGLE_MAX_PROFILES);
        return -EINVAL;
    }

    LOG_INF("Switching to dongle slot %u", slot);
    active_dongle_slot = slot;
    save_dongle_config();

    if (is_connected && current_conn) {
        /* Disconnect; the disconnected callback will restart advertising
         * toward the newly selected slot. */
        bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    } else {
        /* Not connected — restart advertising immediately. */
        bt_le_adv_stop();
        low_duty_advertising = false;
        k_work_submit(&advertising_work);
    }

    return 0;
}

/* -----------------------------------------------------------------------
 * Startup
 * ----------------------------------------------------------------------- */

static int zmk_peripheral_ble_complete_startup(void) {
#if IS_ENABLED(CONFIG_ZMK_BLE_CLEAR_BONDS_ON_START)
    LOG_WRN("Clearing all existing BLE bond information from the keyboard");
    bt_unpair(BT_ID_DEFAULT, NULL);
    /* Also wipe our slot table so we re-pair from scratch. */
    memset(dongle_addrs, 0, sizeof(dongle_addrs));
    dongle_count = 0;
    active_dongle_slot = 0;
    save_dongle_config();
#else
    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_info_cb_register(&zmk_peripheral_ble_auth_info_cb);

    /* Preserve previous startup behavior for existing paired devices. */
    if (dongle_count == 0) {
        refresh_dongle_slots_from_bonds();
    }

    low_duty_advertising = false;
    k_work_submit(&advertising_work);
#endif

    return 0;
}

/* -----------------------------------------------------------------------
 * Settings
 * ----------------------------------------------------------------------- */

#if IS_ENABLED(CONFIG_SETTINGS)

static int peripheral_ble_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    if (strcmp(name, "dslot") == 0) {
        if (len != sizeof(active_dongle_slot)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &active_dongle_slot, sizeof(active_dongle_slot));
    } else if (strcmp(name, "dcount") == 0) {
        if (len != sizeof(dongle_count)) {
            return -EINVAL;
        }
        return read_cb(cb_arg, &dongle_count, sizeof(dongle_count));
    } else if (strcmp(name, "daddrs") == 0) {
        /* Accept stored blobs up to our current array size (handles profile
         * count changes gracefully by loading as many addresses as fit). */
        size_t load_len = MIN(len, sizeof(dongle_addrs));
        return read_cb(cb_arg, dongle_addrs, load_len);
    }

    return 0;
}

static struct settings_handler ble_peripheral_settings_handler = {
    .name = "ble_peripheral",
    .h_set = peripheral_ble_handle_set,
    .h_commit = zmk_peripheral_ble_complete_startup,
};

#endif /* IS_ENABLED(CONFIG_SETTINGS) */

static int zmk_peripheral_ble_init(void) {
    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("BLUETOOTH FAILED (%d)", err);
        return err;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_register(&ble_peripheral_settings_handler);
#else
    zmk_peripheral_ble_complete_startup();
#endif

    return 0;
}

SYS_INIT(zmk_peripheral_ble_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);
