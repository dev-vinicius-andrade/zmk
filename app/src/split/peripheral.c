#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/settings/settings.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#ifndef CONFIG_ZMK_MAX_KNOWN_CENTRALS
#define CONFIG_ZMK_MAX_KNOWN_CENTRALS 3
#endif

#define MAX_KNOWN_CENTRALS CONFIG_ZMK_MAX_KNOWN_CENTRALS

static bt_addr_le_t known_centrals[MAX_KNOWN_CENTRALS];
static int known_central_count = 0;

static bool is_connected = false;
static bool is_bonded = false;

static int save_known_centrals() {
    return settings_save_one("ble_peripheral/known_centrals", known_centrals,
                             sizeof(known_centrals));
}

static void forget_oldest_central() {
    if (known_central_count == 0)
        return;
    for (int i = 1; i < known_central_count; i++) {
        known_centrals[i - 1] = known_centrals[i];
    }
    known_central_count--;
}

static void add_known_central(const bt_addr_le_t *addr) {
    for (int i = 0; i < known_central_count; i++) {
        if (!bt_addr_le_cmp(&known_centrals[i], addr))
            return; // Already known
    }
    if (known_central_count >= MAX_KNOWN_CENTRALS) {
        forget_oldest_central();
    }
    bt_addr_le_copy(&known_centrals[known_central_count++], addr);
    save_known_centrals();
}

static int start_advertising(bool low_duty) {
    for (int i = 0; i < known_central_count; ++i) {
        struct bt_le_adv_param adv_param = low_duty
                                               ? *BT_LE_ADV_CONN_DIR_LOW_DUTY(&known_centrals[i])
                                               : *BT_LE_ADV_CONN_DIR(&known_centrals[i]);

        int err = bt_le_adv_start(&adv_param, NULL, 0, NULL, 0);
        if (err == 0) {
            LOG_INF("Advertising to known central %d", i);
            return 0; // Started directed advertising
        }
    }
    LOG_INF("Fallback to open advertising");
    return bt_le_adv_start(BT_LE_ADV_CONN, NULL, 0, NULL, 0); // Fallback
}

static void connected(struct bt_conn *conn, uint8_t err) {
    is_connected = (err == 0);
    if (!err) {
        add_known_central(bt_conn_get_dst(conn));
    }
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    is_connected = false;
    start_advertising(false);
}

static void auth_pairing_complete(struct bt_conn *conn, bool bonded) {
    is_bonded = bonded;
    if (bonded) {
        add_known_central(bt_conn_get_dst(conn));
    }
}

static int peripheral_ble_handle_set(const char *name, size_t len, settings_read_cb read_cb,
                                     void *cb_arg) {
    if (strcmp(name, "known_centrals") == 0) {
        ssize_t read = read_cb(cb_arg, known_centrals, sizeof(known_centrals));
        if (read > 0) {
            known_central_count = read / sizeof(bt_addr_le_t);
        }
    }
    return 0;
}

static struct settings_handler ble_peripheral_settings_handler = {
    .name = "ble_peripheral",
    .h_set = peripheral_ble_handle_set,
};

static int zmk_peripheral_ble_init(void) {
    int err = bt_enable(NULL);
    if (err)
        return err;

    bt_conn_cb_register(&conn_callbacks);
    bt_conn_auth_cb_register(&auth_cb_display);

    settings_register(&ble_peripheral_settings_handler);
    settings_load(); // Load known centrals

    start_advertising(false);
    return 0;
}

SYS_INIT(zmk_peripheral_ble_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);