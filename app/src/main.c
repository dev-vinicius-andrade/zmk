/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/settings/settings.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(zmk, CONFIG_ZMK_LOG_LEVEL);

#if IS_ENABLED(CONFIG_ZMK_DISPLAY)

#include <zmk/display.h>
#include <lvgl.h>

#endif
static int set_static_mac_from_config(void) {
#if defined(CONFIG_ZMK_BLE_CENTRAL_STATIC_MAC) && (sizeof(CONFIG_ZMK_BLE_CENTRAL_STATIC_MAC) > 1)
    const char *mac_str = CONFIG_ZMK_BLE_CENTRAL_STATIC_MAC;
    uint8_t addr_val[6];
    if (sscanf(mac_str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &addr_val[5], &addr_val[4], &addr_val[3],
               &addr_val[2], &addr_val[1], &addr_val[0]) != 6) {
        LOG_ERR("Invalid MAC address format: %s", mac_str);
        return -EINVAL;
    }

    bt_addr_le_t addr = {
        .type = BT_ADDR_LE_RANDOM,
    };
    memcpy(addr.a.val, addr_val, 6);

    int id = bt_id_create(&addr, NULL);
    if (id < 0) {
        LOG_ERR("Failed to set static MAC address (err %d)", id);
        return id;
    }

    LOG_INF("Static MAC set to: %s", mac_str);
#endif
    return 0;
}
int main(void) {
    LOG_INF("Welcome to ZMK!\n");

#if IS_ENABLED(CONFIG_SETTINGS)
    settings_subsys_init();
    settings_load();
#endif
    set_static_mac_from_config();
#ifdef CONFIG_ZMK_DISPLAY
    zmk_display_init();

#if IS_ENABLED(CONFIG_ARCH_POSIX)
    // Workaround for an SDL display issue:
    // https://github.com/zephyrproject-rtos/zephyr/issues/71410
    while (1) {
        lv_task_handler();
        k_sleep(K_MSEC(10));
    }
#endif

#endif /* CONFIG_ZMK_DISPLAY */

    return 0;
}
