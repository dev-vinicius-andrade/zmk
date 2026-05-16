/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Dongle-select behavior: switches the split peripheral to a different bonded
 * central (dongle) slot.  When running on the central (dongle) the behavior
 * forwards itself to all connected peripheral halves via the split transport.
 * When running on a peripheral half (invoked via ZMK_SPLIT_RUN) it calls
 * zmk_split_bt_peripheral_select_dongle() directly.
 *
 * Keymap usage:
 *   &dongle_select 0   — reconnect to slot 0 (e.g., home dongle)
 *   &dongle_select 1   — reconnect to slot 1 (e.g., work / travel dongle)
 */

#define DT_DRV_COMPAT zmk_behavior_dongle_select

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* Central side: forward the behavior invocation to all peripheral halves. */
#include <zmk/split/central.h>

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    for (uint8_t i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        int err = zmk_split_central_invoke_behavior(i, binding, event, true);
        if (err < 0) {
            LOG_WRN("Failed to invoke dongle_select on peripheral %u (%d)", i, err);
        }
    }
    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#elif IS_ENABLED(CONFIG_ZMK_SPLIT) /* peripheral role */

/* Peripheral side: actually change the active dongle slot. */
#include <zmk/split/bluetooth/peripheral.h>

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return zmk_split_bt_peripheral_select_dongle((uint8_t)binding->param1);
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#else /* non-split build — no-op */

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    return 0;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#endif /* role selection */

static const struct behavior_driver_api behavior_dongle_select_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_dongle_select_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY */
