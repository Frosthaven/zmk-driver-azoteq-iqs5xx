/*
 * Copyright (c) 2025 Mariano Uvalle
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT azoteq_iqs5xx

#include <stdlib.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include "iqs5xx.h"

LOG_MODULE_REGISTER(iqs5xx, CONFIG_INPUT_LOG_LEVEL);

static int iqs5xx_read_reg16(const struct device *dev, uint16_t reg, uint16_t *val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[2];
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};
    int ret;

    ret = i2c_write_read_dt(&config->i2c, reg_buf, sizeof(reg_buf), buf, sizeof(buf));
    if (ret < 0) {
        return ret;
    }

    *val = (buf[0] << 8) | buf[1];
    return 0;
}

static int iqs5xx_write_reg16(const struct device *dev, uint16_t reg, uint16_t val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[4] = {reg >> 8, reg & 0xFF, val >> 8, val & 0xFF};

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

static int iqs5xx_read_reg8(const struct device *dev, uint16_t reg, uint8_t *val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t reg_buf[2] = {reg >> 8, reg & 0xFF};

    return i2c_write_read_dt(&config->i2c, reg_buf, sizeof(reg_buf), val, 1);
}

static int iqs5xx_write_reg8(const struct device *dev, uint16_t reg, uint8_t val) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[3] = {reg >> 8, reg & 0xFF, val};

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

static int iqs5xx_end_comm_window(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    uint8_t buf[3] = {IQS5XX_END_COMM_WINDOW >> 8, IQS5XX_END_COMM_WINDOW & 0xFF, 0x00};

    return i2c_write_dt(&config->i2c, buf, sizeof(buf));
}

static void iqs5xx_button_release_work_handler(struct k_work *work) {
    struct k_work_delayable *dwork = k_work_delayable_from_work(work);
    struct iqs5xx_data *data = CONTAINER_OF(dwork, struct iqs5xx_data, button_release_work);

    // TODO: This loop should only deactivate one button.
    // Log a warning when that is not the case.
    for (int i = 0; i < 3; i++) {
        LOG_INF("Releasing synthetic button");
        if (data->buttons_pressed & BIT(i)) {
            input_report_key(data->dev, INPUT_BTN_0 + i, 0, true, K_FOREVER);
            // Turn off the bit.
            // NOTE: This is a potential race.
            data->buttons_pressed &= ~BIT(i);
        }
    }
}

static int iqs5xx_setup_device(const struct device *dev);

// Emit a synthetic mouse-button click: press now, auto-release shortly after.
// If the same button is already pressed (its auto-release hasn't fired yet),
// release it first so the host sees a fresh click rather than one long press
// -- otherwise two fast taps inside the release window collapse into a single
// click and double-tap is lost.
// Auto-release time matches IQS5XX_TAP_HOLD_WINDOW_MS so masking works across
// the full tap-then-hold window: the prior tap's release is still scheduled
// when the second touch lands and pending-setup cancels it.
static void iqs5xx_emit_click(const struct device *dev, struct iqs5xx_data *data, uint16_t code) {
    uint8_t bit = BIT(code - INPUT_BTN_0);
    k_work_cancel_delayable(&data->button_release_work);
    if (data->buttons_pressed & bit) {
        input_report_key(dev, code, 0, true, K_FOREVER);
        data->buttons_pressed &= ~bit;
    }
    // The release we're about to schedule supersedes any deferred-release
    // bookkeeping -- the pending block must not double-release this bit.
    data->tap_release_deferred = false;
    input_report_key(dev, code, 1, true, K_FOREVER);
    data->buttons_pressed |= bit;
    k_work_schedule(&data->button_release_work, K_MSEC(IQS5XX_TAP_HOLD_WINDOW_MS));
}

static void iqs5xx_work_handler(struct k_work *work) {
    struct iqs5xx_data *data = CONTAINER_OF(work, struct iqs5xx_data, work);
    const struct device *dev = data->dev;
    const struct iqs5xx_config *config = dev->config;
    uint8_t sys_info_0, sys_info_1, gesture_events_0, gesture_events_1, num_fingers;
    int ret;

    // Read system info registers.
    ret = iqs5xx_read_reg8(dev, IQS5XX_SYSTEM_INFO_0, &sys_info_0);
    if (ret < 0) {
        LOG_ERR("Failed to read system info 0: %d", ret);
        goto end_comm;
    }

    ret = iqs5xx_read_reg8(dev, IQS5XX_SYSTEM_INFO_1, &sys_info_1);
    if (ret < 0) {
        LOG_ERR("Failed to read system info 1: %d", ret);
        goto end_comm;
    }

    ret = iqs5xx_read_reg8(dev, IQS5XX_GESTURE_EVENTS_0, &gesture_events_0);
    if (ret < 0) {
        LOG_ERR("Failed to read gesture events: %d", ret);
        goto end_comm;
    }

    ret = iqs5xx_read_reg8(dev, IQS5XX_GESTURE_EVENTS_1, &gesture_events_1);
    if (ret < 0) {
        LOG_ERR("Failed to read gesture events 1: %d", ret);
        goto end_comm;
    }

    // Handle reset indication. The chip reloads its FACTORY defaults on reset
    // (e.g. from a power glitch, ESD, or its watchdog), which have a different
    // axis orientation and gesture configuration than we programmed. So
    // re-apply our full configuration here rather than only acknowledging the
    // reset -- otherwise the trackpad silently misbehaves (notably a
    // 90-degree-rotated cursor when switch-xy reverts) until the half reboots.
    if (sys_info_0 & IQS5XX_SHOW_RESET) {
        LOG_WRN("Device reset detected; re-applying configuration");
        iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONTROL_0, IQS5XX_ACK_RESET);
        iqs5xx_setup_device(dev);
        return; // setup_device closes the comm window
    }

    if (iqs5xx_read_reg8(dev, IQS5XX_NUM_FINGERS, &num_fingers) < 0) {
        num_fingers = 0;
    }

#if IS_ENABLED(CONFIG_INPUT_AZOTEQ_IQS5XX_GESTURE_DEBUG)
    if (num_fingers >= 1 || gesture_events_0 || gesture_events_1) {
        int16_t dbg_x0 = 0, dbg_y0 = 0, dbg_x1 = 0, dbg_y1 = 0;
        iqs5xx_read_reg16(dev, IQS5XX_ABS_X, (uint16_t *)&dbg_x0);
        iqs5xx_read_reg16(dev, IQS5XX_ABS_Y, (uint16_t *)&dbg_y0);
        iqs5xx_read_reg16(dev, IQS5XX_ABS_X1, (uint16_t *)&dbg_x1);
        iqs5xx_read_reg16(dev, IQS5XX_ABS_Y1, (uint16_t *)&dbg_y1);
        LOG_INF("ge0=0x%02x ge1=0x%02x nf=%d f0=(%d,%d) f1=(%d,%d)", gesture_events_0,
                gesture_events_1, num_fingers, dbg_x0, dbg_y0, dbg_x1, dbg_y1);
    }
#endif

    bool tp_movement = (sys_info_1 & IQS5XX_TP_MOVEMENT) != 0;
    bool scroll = (gesture_events_1 & IQS5XX_SCROLL) != 0;
    if (!scroll) {
        // Clear accumulators if we're not actively scrolling.
        data->scroll_x_acc = 0;
        data->scroll_y_acc = 0;
    }

    bool hold_gesture = (gesture_events_0 & IQS5XX_PRESS_AND_HOLD) != 0;
    bool hold_became_active = hold_gesture && !data->active_hold;
    bool hold_released = !hold_gesture && data->active_hold;

    int16_t rel_x, rel_y;
    if (tp_movement || scroll) {
        ret = iqs5xx_read_reg16(dev, IQS5XX_REL_X, (uint16_t *)&rel_x);
        if (ret < 0) {
            LOG_ERR("Failed to read relative X: %d", ret);
            goto end_comm;
        }

        ret = iqs5xx_read_reg16(dev, IQS5XX_REL_Y, (uint16_t *)&rel_y);
        if (ret < 0) {
            LOG_ERR("Failed to read relative Y: %d", ret);
            goto end_comm;
        }
    }

    // Track the active touch (start time, peak finger count, total movement) so
    // a "tap" -- quick and low-movement -- can be told from a drag/scroll on
    // lift. Multi-finger taps are classified here by PEAK finger count, so a
    // staggered 2-/3-finger landing still resolves correctly (the chip's native
    // two-finger tap, which would fire early, is left disabled). The chip has no
    // 3-finger gesture at all, so middle-click is synthesized the same way.
    if (num_fingers > 0) {
        if (data->prev_num_fingers == 0) {
            data->touch_start_time = k_uptime_get();
            data->touch_max_fingers = num_fingers;
            data->touch_move_acc = 0;
            data->touch_gestured = false;
            data->tap_then_hold_engaged = false;
            data->tap_then_hold_pending = false;
            data->cursor_rem_x = 0;
            data->cursor_rem_y = 0;
            // Tap-then-hold drag-lock: a clean 1-finger tap recently armed
            // last_tap_lift_time; if this touch lands as a single finger
            // inside the window and a drag isn't already locked, MARK IT
            // PENDING. Engagement is deferred to the first cycle that
            // reports motion -- a clean quick lift instead falls through
            // to the normal tap path and becomes a second click, so a
            // double-tap doesn't get eaten by drag-lock. Multi-finger
            // landings skip this so a scroll intent isn't pending.
            if (config->tap_then_hold && config->drag_lock && num_fingers == 1 &&
                !data->manual_drag && data->last_tap_lift_time > 0 &&
                (k_uptime_get() - data->last_tap_lift_time) <= IQS5XX_TAP_HOLD_WINDOW_MS) {
                data->tap_then_hold_pending = true;
                // Mask the prior tap's click: cancel its pending auto-release
                // so the host's LEFT stays held while we wait for motion vs.
                // a clean lift. If motion engages, we take ownership of the
                // still-held press (no discrete click event ever reaches the
                // host -- the drag absorbs the tap, restoring the old
                // immediate-engage feel that preserved selections). If the
                // touch instead lifts as a tap (double-click), emit_click's
                // release-first handles releasing the masked press cleanly.
                if (data->buttons_pressed & LEFT_BUTTON_BIT) {
                    k_work_cancel_delayable(&data->button_release_work);
                    data->tap_release_deferred = true;
                }
            }
            // Always consume the armed tap on touch-down -- whether engaged
            // or not, the chance closes here.
            data->last_tap_lift_time = 0;
        } else if (num_fingers > data->touch_max_fingers) {
            data->touch_max_fingers = num_fingers;
            // A second finger arriving during a tap-then-hold engagement is
            // the user starting a scroll (staggered landing) -- release the
            // just-latched drag-lock so the gesture works cleanly. A drag-
            // lock engaged on a prior touch (carried across lifts) is NOT
            // released here, only the in-progress one.
            if (data->tap_then_hold_engaged && data->manual_drag) {
                input_report_key(dev, LEFT_BUTTON_CODE, 0, true, K_FOREVER);
                data->manual_drag = false;
                data->tap_then_hold_engaged = false;
            }
        }
        // Count any reported motion (cursor or scroll) so a moving touch --
        // e.g. a quick two-finger scroll flick -- isn't mistaken for a tap.
        if (tp_movement || scroll) {
            data->touch_move_acc += abs(rel_x) + abs(rel_y);
        }
        // A touch that ever scrolled is a gesture, never a tap.
        if (scroll) {
            data->touch_gestured = true;
        }
        // A multi-finger touch that moves at all is a scroll attempt, not a
        // right-/middle-click tap -- mark it gestured before the chip commits to
        // its own SCROLL flag, so a short two-finger scroll the chip hasn't
        // classified yet can't be misread as a two-finger tap (right click) on lift.
        if (data->touch_max_fingers >= 2 && data->touch_move_acc > IQS5XX_MULTI_TAP_MOVE_MAX) {
            data->touch_gestured = true;
        }
    } else if (data->prev_num_fingers > 0) {
        // All fingers lifted -- classify the just-ended touch and act on it.
        bool low_move = data->touch_move_acc <= IQS5XX_TAP_MOVE_MAX;
        bool quick = (k_uptime_get() - data->touch_start_time) <= IQS5XX_TAP_MS_MAX;
        uint8_t peak = data->touch_max_fingers;
        if (data->manual_drag) {
            // Tap to drop the press-and-hold drag-lock: end on a stationary
            // (low-movement) lift; a lift with movement is just a drag stroke and
            // keeps the lock. A multi-finger tap (2 or 3 fingers) ALWAYS ends it
            // -- a guaranteed escape so the lock can never get stuck -- and
            // issues its click when it was a clean tap.
            if (low_move || peak >= 2) {
                input_report_key(dev, LEFT_BUTTON_CODE, 0, true, K_FOREVER);
                data->manual_drag = false;
                data->tap_then_hold_engaged = false;
                data->last_tap_lift_time = 0;
                if (low_move && peak == 2 && config->two_finger_tap) {
                    iqs5xx_emit_click(dev, data, RIGHT_BUTTON_CODE);
                } else if (low_move && peak == 3 && config->three_finger_tap) {
                    iqs5xx_emit_click(dev, data, MIDDLE_BUTTON_CODE);
                }
            }
        } else if (quick && low_move && !data->touch_gestured) {
            // Synthesized tap-click, chosen by PEAK finger count: a staggered
            // multi-finger landing resolves correctly, and a single finger
            // touching down first can't sneak in an early left click. A touch
            // that scrolled is excluded, so a fast flick isn't a tap.
            if (peak == 1 && config->one_finger_tap) {
                iqs5xx_emit_click(dev, data, LEFT_BUTTON_CODE);
                // Arm tap-then-hold: if the next touch lands as a single finger
                // within IQS5XX_TAP_HOLD_WINDOW_MS, drag-lock latches at touch-
                // down. Only armed off a clean 1-finger tap (multi-finger taps
                // are right/middle clicks, not drag intents).
                if (config->tap_then_hold && config->drag_lock) {
                    data->last_tap_lift_time = k_uptime_get();
                }
            } else if (peak == 2 && config->two_finger_tap) {
                iqs5xx_emit_click(dev, data, RIGHT_BUTTON_CODE);
            } else if (peak == 3 && config->three_finger_tap) {
                iqs5xx_emit_click(dev, data, MIDDLE_BUTTON_CODE);
            }
        }
        data->touch_max_fingers = 0;
    }
    data->prev_num_fingers = num_fingers;

    // Resolve a pending tap-then-hold: engage drag-lock only once the chip
    // reports motion, so a quick double-tap (no motion before lift) can pass
    // through to the normal tap path as a second click. Multi-finger or lift
    // before motion clears the pending state without engaging.
    if (data->tap_then_hold_pending) {
        if (num_fingers != 1) {
            // Lift or multi-finger: pending resolves without engagement.
            // If the prior tap's release was deferred (we held LEFT to mask
            // the click), emit it now -- unless the lift handler already
            // ran emit_click for this lift, which would have cleared the
            // flag while taking over the press.
            if (data->tap_release_deferred && (data->buttons_pressed & LEFT_BUTTON_BIT)) {
                input_report_key(dev, LEFT_BUTTON_CODE, 0, true, K_FOREVER);
                data->buttons_pressed &= ~LEFT_BUTTON_BIT;
            }
            data->tap_release_deferred = false;
            data->tap_then_hold_pending = false;
        } else if (tp_movement) {
            // Motion: engage. If LEFT is already held (the masked tap press),
            // take ownership without re-pressing -- the host saw one
            // continuous LEFT down from the tap through the drag, no click
            // event in between. If the bit is clear (tap released before the
            // second touch -- happens when emit_click's auto-release window
            // is shorter than the engage window in a corner case), press
            // fresh.
            if (data->buttons_pressed & LEFT_BUTTON_BIT) {
                data->buttons_pressed &= ~LEFT_BUTTON_BIT;
            } else {
                input_report_key(dev, LEFT_BUTTON_CODE, 1, true, K_FOREVER);
            }
            data->manual_drag = true;
            data->tap_then_hold_engaged = true;
            data->tap_release_deferred = false;
            data->tap_then_hold_pending = false;
        }
        // else: single finger, no motion yet -- keep pending and wait.
    }

    // Handle movement and gestures.
    //
    // Each one of these branches needs to send the last report it makes as
    // sync to ensure that the input subsystem processes things in order.
    if (hold_became_active) {
        LOG_INF("Hold became active");
        data->active_hold = true;
        if (config->drag_lock) {
            // A plain press-and-hold (no preceding tap) also latches the drag-
            // LOCK -- a clean drag with no tap-click first, so it can grab a
            // multi-file selection without the tap deselecting it. Persists
            // across lifts; ended by a tap, like the tap-then-hold drag.
            if (!data->manual_drag) {
                input_report_key(dev, LEFT_BUTTON_CODE, 1, true, K_FOREVER);
                data->manual_drag = true;
            }
        } else {
            input_report_key(dev, LEFT_BUTTON_CODE, 1, true, K_FOREVER);
        }
    } else if (hold_released) {
        LOG_INF("Hold became inactive");
        data->active_hold = false;
        // In drag-lock mode the lock persists across lifts (ended by a tap), so
        // don't release on lift; only the plain momentary hold releases here.
        if (!config->drag_lock) {
            input_report_key(dev, LEFT_BUTTON_CODE, 0, true, K_FOREVER);
        }
    } else if (scroll) {
        // TODO: Expose this base divisor.
        int16_t scroll_div = 32;
        // Apply cursor-scale-percent to scroll, but at HALF the strength of
        // the cursor scaling -- a smaller wheel-divisor change so the scroll
        // doesn't feel as heavily slowed as the cursor. Midpoint formula:
        // scroll_scale = (100 + cursor_scale) / 2. cursor=50 -> scroll_scale
        // =75 -> effective_div=42; cursor=100 -> scroll_scale=100 (no slowdown).
        if (config->cursor_scale_percent > 0 && config->cursor_scale_percent != 100) {
            int16_t scroll_scale = (int16_t)((100 + config->cursor_scale_percent) / 2);
            scroll_div = (int16_t)((int32_t)scroll_div * 100 / scroll_scale);
        }

        // Only one scrolling direction is valid at a time.
        // End the communication right after reporting the movement.
        if (rel_x != 0) {
            // By default the x axis is already "natural".
            if (!config->natural_scroll_x) {
                rel_x *= -1;
            }
            data->scroll_x_acc += rel_x;
            if (abs(data->scroll_x_acc) >= scroll_div) {
                input_report_rel(dev, INPUT_REL_HWHEEL, data->scroll_x_acc / scroll_div, true,
                                K_FOREVER);
                data->scroll_x_acc %= scroll_div;
            }
            goto end_comm;
        }
        if (rel_y != 0) {
            if (config->natural_scroll_y) {
                rel_y *= -1;
            }
            data->scroll_y_acc += rel_y;
            if (abs(data->scroll_y_acc) >= scroll_div) {
                input_report_rel(dev, INPUT_REL_WHEEL, data->scroll_y_acc / scroll_div, true,
                                 K_FOREVER);
                data->scroll_y_acc %= scroll_div;
            }

            goto end_comm;
        }
    } else if (tp_movement) {
        // Only move the cursor for a touch that has only ever been a single
        // finger. After a scroll, lifting one finger before the other leaves
        // one contact whose motion would otherwise drag the cursor.
        if ((rel_x != 0 || rel_y != 0) && data->touch_max_fingers <= 1) {
            // Apply cursor-scale (DT prop; default 100 = no scaling). Lets a
            // user slow the cursor in firmware so the feel matches across hosts
            // without per-OS pointer tweaks.
            int16_t out_x, out_y;
            if (config->cursor_scale_percent != 100) {
                // Fractional accumulator: holding the remainder across cycles
                // preserves sub-unit motion -- without it, any chip delta of 1
                // with scale<100 rounds to 0 and the cursor goes dead at low
                // speeds. The remainder is signed and reset on touch-down.
                int32_t sx = (int32_t)rel_x * config->cursor_scale_percent + data->cursor_rem_x;
                int32_t sy = (int32_t)rel_y * config->cursor_scale_percent + data->cursor_rem_y;
                out_x = (int16_t)(sx / 100);
                out_y = (int16_t)(sy / 100);
                data->cursor_rem_x = (int16_t)(sx - (int32_t)out_x * 100);
                data->cursor_rem_y = (int16_t)(sy - (int32_t)out_y * 100);
            } else {
                out_x = rel_x;
                out_y = rel_y;
            }
            if (out_x != 0 || out_y != 0) {
                input_report_rel(dev, INPUT_REL_X, out_x, false, K_FOREVER);
                input_report_rel(dev, INPUT_REL_Y, out_y, true, K_FOREVER);
            }
        }
    }

end_comm:
    // End communication window.
    iqs5xx_end_comm_window(dev);
}

static void iqs5xx_rdy_handler(const struct device *port, struct gpio_callback *cb,
                               gpio_port_pins_t pins) {
    struct iqs5xx_data *data = CONTAINER_OF(cb, struct iqs5xx_data, rdy_cb);

    k_work_submit(&data->work);
}

static int iqs5xx_setup_device(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    int ret;

    // Enable event mode and trackpad events.
    // TOUCH_EVENT makes the chip signal on finger-count changes (incl. the
    // final lift), which the synthesized three-finger middle-click relies on.
    ret = iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONFIG_1,
                            IQS5XX_EVENT_MODE | IQS5XX_TP_EVENT | IQS5XX_GESTURE_EVENT |
                                IQS5XX_TOUCH_EVENT);
    if (ret < 0) {
        LOG_ERR("Failed to configure event mode: %d", ret);
        return ret;
    }

    ret = iqs5xx_write_reg8(dev, IQS5XX_BOTTOM_BETA, config->bottom_beta);
    if (ret < 0) {
        LOG_ERR("Failed to set bottom beta: %d", ret);
        return ret;
    }

    ret = iqs5xx_write_reg8(dev, IQS5XX_STATIONARY_THRESH, config->stationary_threshold);
    if (ret < 0) {
        LOG_ERR("Failed to set bottom stationary threshold: %d", ret);
        return ret;
    }

    // TODO: Expose these through dts bindings.
    // Set filter settings with:
    // - IIR filter enabled
    // - MAV filter enabled
    // - IIR select disabled (dynamic IIR)
    // - ALP count filter enabled
    ret = iqs5xx_write_reg8(dev, IQS5XX_FILTER_SETTINGS,
                            IQS5XX_IIR_FILTER | IQS5XX_MAV_FILTER | IQS5XX_ALP_COUNT_FILTER);
    if (ret < 0) {
        LOG_ERR("Failed to configure filter settings: %d", ret);
        return ret;
    }

    uint8_t single_finger_gestures = 0;
    single_finger_gestures |= config->one_finger_tap ? IQS5XX_SINGLE_TAP : 0;
    single_finger_gestures |= config->press_and_hold ? IQS5XX_PRESS_AND_HOLD : 0;
    // Configure single finger gestures.
    ret = iqs5xx_write_reg8(dev, IQS5XX_SINGLE_FINGER_GESTURES_CONF, single_finger_gestures);
    if (ret < 0) {
        LOG_ERR("Failed to configure single finger gestures: %d", ret);
        return ret;
    }

    // Configure the hold time for the press and hold gesture.
    ret = iqs5xx_write_reg16(dev, IQS5XX_HOLD_TIME, config->press_and_hold_time);
    if (ret < 0) {
        LOG_ERR("Failed to configure the hold time: %d", ret);
        return ret;
    }

    uint8_t two_finger_gestures = 0;
    // The chip's native two-finger-tap is intentionally NOT enabled: two- and
    // three-finger taps are synthesized from the peak finger count on lift (see
    // the work handler), so a staggered multi-finger landing can't be
    // misclassified as a right-click before the later fingers arrive.
    two_finger_gestures |= config->scroll ? IQS5XX_SCROLL : 0;
    // Configure multi finger gestures.
    ret = iqs5xx_write_reg8(dev, IQS5XX_MULTI_FINGER_GESTURES_CONF, two_finger_gestures);
    if (ret < 0) {
        LOG_ERR("Failed to configure multi finger gestures: %d", ret);
        return ret;
    }

    // Configure axes.
    uint8_t xy_config = 0;
    xy_config |= config->flip_x ? IQS5XX_FLIP_X : 0;
    xy_config |= config->flip_y ? IQS5XX_FLIP_Y : 0;
    xy_config |= config->switch_xy ? IQS5XX_SWITCH_XY_AXIS : 0;
    ret = iqs5xx_write_reg8(dev, IQS5XX_XY_CONFIG_0, xy_config);
    if (ret < 0) {
        LOG_ERR("Failed to configure axes: %d", ret);
        return ret;
    }

    // Configure system settings.
    ret = iqs5xx_write_reg8(dev, IQS5XX_SYSTEM_CONFIG_0, IQS5XX_SETUP_COMPLETE | IQS5XX_WDT);
    if (ret < 0) {
        LOG_ERR("Failed to configure system: %d", ret);
        return ret;
    }

    // End communication window.
    ret = iqs5xx_end_comm_window(dev);
    if (ret < 0) {
        LOG_ERR("Failed to end comm window during initialization: %d", ret);
        return ret;
    }

    return 0;
}

static int iqs5xx_init(const struct device *dev) {
    const struct iqs5xx_config *config = dev->config;
    struct iqs5xx_data *data = dev->data;
    int ret;

    if (!i2c_is_ready_dt(&config->i2c)) {
        LOG_ERR("I2C device not ready");
        return -ENODEV;
    }

    data->dev = dev;
    k_work_init(&data->work, iqs5xx_work_handler);
    k_work_init_delayable(&data->button_release_work, iqs5xx_button_release_work_handler);

    // Configure reset GPIO if available.
    if (config->reset_gpio.port) {
        if (!gpio_is_ready_dt(&config->reset_gpio)) {
            LOG_ERR("Reset GPIO not ready");
            return -ENODEV;
        }

        ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
        if (ret < 0) {
            LOG_ERR("Failed to configure reset GPIO: %d", ret);
            return ret;
        }

        // Reset the device.
        gpio_pin_set_dt(&config->reset_gpio, 1);
        k_msleep(1);
        gpio_pin_set_dt(&config->reset_gpio, 0);
        k_msleep(10);
    }

    // Configure RDY GPIO.
    if (!gpio_is_ready_dt(&config->rdy_gpio)) {
        LOG_ERR("RDY GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&config->rdy_gpio, GPIO_INPUT);
    if (ret < 0) {
        LOG_ERR("Failed to configure RDY GPIO: %d", ret);
        return ret;
    }

    gpio_init_callback(&data->rdy_cb, iqs5xx_rdy_handler, BIT(config->rdy_gpio.pin));
    ret = gpio_add_callback(config->rdy_gpio.port, &data->rdy_cb);
    if (ret < 0) {
        LOG_ERR("Failed to add RDY callback: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_EDGE_RISING);
    if (ret < 0) {
        LOG_ERR("Failed to configure RDY interrupt: %d", ret);
        return ret;
    }

    // Wait for device to be ready.
    k_msleep(100);

    // Setup device configuration.
    ret = iqs5xx_setup_device(dev);
    if (ret < 0) {
        LOG_ERR("Failed to setup device: %d", ret);
        return ret;
    }

    data->initialized = true;
    LOG_INF("IQS5xx trackpad initialized");

    return 0;
}

#ifdef CONFIG_PM_DEVICE
/* Zephyr PM device action: when ZMK_SLEEP wakes the SoC from System OFF, ZMK
 * issues PM_DEVICE_ACTION_RESUME to every PM-aware device. We re-apply the chip
 * configuration (orientation, gestures, scroll, etc.) and re-arm the RDY GPIO
 * interrupt -- System OFF disables GPIO IRQs, and without this re-arm the work
 * handler would never fire again and the cursor stays dead until a power-cycle.
 * The chip itself stays powered through sleep (permanent 3v3 rail), but we
 * re-init defensively in case it was reset by ESD/glitch during the sleep
 * window (same path the driver already uses for runtime reset detection). */
static int iqs5xx_pm_action(const struct device *dev, enum pm_device_action action) {
    const struct iqs5xx_config *config = dev->config;
    int ret;

    switch (action) {
    case PM_DEVICE_ACTION_RESUME:
        ret = iqs5xx_setup_device(dev);
        if (ret < 0) {
            LOG_ERR("PM resume: chip setup failed (%d)", ret);
            return ret;
        }
        ret = gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_EDGE_RISING);
        if (ret < 0) {
            LOG_ERR("PM resume: RDY irq re-arm failed (%d)", ret);
            return ret;
        }
        return 0;
    case PM_DEVICE_ACTION_SUSPEND:
        /* Quiet the RDY edge-detect so spurious wakes don't queue work during
         * suspend; harmless if it fails. */
        (void)gpio_pin_interrupt_configure_dt(&config->rdy_gpio, GPIO_INT_DISABLE);
        return 0;
    default:
        return -ENOTSUP;
    }
}
#endif /* CONFIG_PM_DEVICE */

// Replace CONFIG_INPUT_INIT_PRIORITY with the azoteq specific value.
#define IQS5XX_INIT(n)                                                                             \
    static struct iqs5xx_data iqs5xx_data_##n;                                                     \
    static const struct iqs5xx_config iqs5xx_config_##n = {                                        \
        .i2c = I2C_DT_SPEC_INST_GET(n),                                                            \
        .rdy_gpio = GPIO_DT_SPEC_INST_GET(n, rdy_gpios),                                           \
        .reset_gpio = GPIO_DT_SPEC_INST_GET_OR(n, reset_gpios, {0}),                               \
        .one_finger_tap = DT_INST_PROP(n, one_finger_tap),                                         \
        .press_and_hold = DT_INST_PROP(n, press_and_hold),                                         \
        .two_finger_tap = DT_INST_PROP(n, two_finger_tap),                                         \
        .three_finger_tap = DT_INST_PROP(n, three_finger_tap),                                     \
        .scroll = DT_INST_PROP(n, scroll),                                                         \
        .natural_scroll_x = DT_INST_PROP(n, natural_scroll_x),                                     \
        .natural_scroll_y = DT_INST_PROP(n, natural_scroll_y),                                     \
        .press_and_hold_time = DT_INST_PROP_OR(n, press_and_hold_time, 250),                       \
        .drag_lock = DT_INST_PROP(n, drag_lock),                                                   \
        .tap_then_hold = DT_INST_PROP(n, tap_then_hold),                                           \
        .switch_xy = DT_INST_PROP(n, switch_xy),                                                   \
        .flip_x = DT_INST_PROP(n, flip_x),                                                         \
        .flip_y = DT_INST_PROP(n, flip_y),                                                         \
        .bottom_beta = DT_INST_PROP_OR(n, bottom_beta, 5),                                         \
        .stationary_threshold = DT_INST_PROP_OR(n, stationary_threshold, 5),                       \
        .cursor_scale_percent = DT_INST_PROP_OR(n, cursor_scale_percent, 100),                     \
    };                                                                                             \
    PM_DEVICE_DT_INST_DEFINE(n, iqs5xx_pm_action);                                                 \
    DEVICE_DT_INST_DEFINE(n, iqs5xx_init, PM_DEVICE_DT_INST_GET(n),                                \
                          &iqs5xx_data_##n, &iqs5xx_config_##n, POST_KERNEL,                       \
                          CONFIG_INPUT_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(IQS5XX_INIT)
