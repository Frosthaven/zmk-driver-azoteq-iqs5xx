# ZMK driver for Azoteq IQS5XX trackpads

> **Fork notice (Frosthaven).** This is a fork of
> [AYM1607/zmk-driver-azoteq-iqs5xx](https://github.com/AYM1607/zmk-driver-azoteq-iqs5xx)
> with extra gesture support. Differences vs upstream:
>
> - **Three-finger tap → middle click.** New `three-finger-tap` property.
>   The chip has no native 3-finger gesture, so it is synthesized from the
>   finger-count register; the driver enables the chip's touch events for it.
> - **All clicks synthesized on lift, by peak finger count.** Left (1) / right
>   (2) / middle (3) clicks are decided when the fingers lift, from the highest
>   finger count seen during the touch. A staggered multi-finger landing resolves
>   correctly (no early/stray right-click before the 3rd finger), and a
>   leftover finger after a scroll can no longer drag the cursor.
> - **Drag-lock.** New `drag-lock` property. The left button latches as a drag
>   that stays held across finger lifts; tap to release (2-/3-finger tap also
>   releases — a guaranteed escape so it can never get stuck). Two ways to
>   engage: `tap-then-hold` (tap, then touch+move within 200 ms — recommended)
>   or `press-and-hold` (hold the finger for the chip's hold time). The driver
>   masks the tap-then-hold prelude click so dragging a multi-file selection
>   doesn't deselect it and doesn't double-click-open files. With `drag-lock`
>   unset, both engagement paths are momentary holds (release on lift).
> - **Cursor speed scale.** New `cursor-scale-percent` property (default 100 =
>   no scaling). Lower values slow the cursor in firmware so the feel is
>   consistent across hosts without per-OS pointer tweaks. The same scale is
>   applied to scroll at half-strength (cursor=50 → scroll needs ~1.3× the
>   motion per tick). A per-axis fractional accumulator preserves sub-unit
>   motion so slow precision movement isn't lost to integer truncation.
> - **Two-finger zoom (pinch/expand) — implemented but off by default.** New
>   `zoom` (+ `zoom-initial-distance`) property. When enabled, the chip's zoom
>   gesture is emitted as KEY clicks on spare button codes (spread →
>   `INPUT_BTN_4`, pinch → `INPUT_BTN_5`), which a central
>   `zmk,input-processor-behaviors` maps to Ctrl+scroll (see Usage). In practice
>   it does **not** trigger reliably on small pads (e.g. the 43 mm TPS43): the
>   chip detects two fingers but the separation barely changes, so it rarely
>   raises a usable zoom — and at sensitive thresholds it steals two-finger
>   scroll. It's left off by default; add `zoom;` to experiment on a larger pad.
>
> Not changed: scroll axis is still chosen by the chip's gesture engine based on
> the two contacts' geometry — the IQS5xx exposes no register to decouple scroll
> direction from finger arrangement, so that behavior is inherent to the hardware.

## Compatibility

This driver should work with any IQS5XX based trackpad. However, it has only been tested and known to work with the following models:

- TPS43 (Reached EOL)
- TPS65 (Reached EOL) per @Mac10goesBRRRT

Feel free to send a pull request if you test with any of the following models:

- TPR40
- TPR48
- TPR54
- TPE60
- TPE48
- TPS48

## Gestures

| Gesture | Action |
|---|---|
| Move one finger | Move cursor |
| Tap with one finger | Left click |
| Tap with two fingers | Right click |
| Tap with three fingers | Middle click |
| Tap, then touch + drag (within 200 ms) | Drag and drop — left button locks, stays held across finger lifts. **Tap once to release.** |
| Press and hold | Drag and drop (alternate) — same lock behavior as above |
| Drag with two fingers | Scroll (vertical / horizontal) |
| Pinch / spread with two fingers | Zoom — **off by default** (emits key clicks; needs a host-side Ctrl+scroll mapping, see below) |

Each click is synthesized on lift from the peak finger count, so a staggered
multi-finger landing resolves correctly (no early/stray right-click before the
3rd finger), and a leftover finger after a scroll can't drag the cursor.

## Usage

- Specify a node with the "azoteq,iqs5xx" compatible inside an i2c node in your keyboard overlay.
- Reference it from an input listener:

```
/ {
    tps43_input: tps43_input {
        compatible = "zmk,input-listener";
        device = <&tps43>;
    };
};

&arduino_i2c {
    status = "okay";
    tps43: iqs5xx@74 {
        status = "okay";
        compatible = "azoteq,iqs5xx";
        reg = <0x74>;

        reset-gpios = <&arduino_header 14 GPIO_ACTIVE_LOW>;
        rdy-gpios = <&arduino_header 15 GPIO_ACTIVE_HIGH>;

        /*
         * Potentially non-exhaustive list of configuration options.
         * See: dts/bindings/input/azoteq,iqs5xx-common.yaml for a full list.
         */
        one-finger-tap;
        two-finger-tap;

        scroll;
        natural-scroll-y;
        natural-scroll-x;

        /* Fork additions: */
        three-finger-tap;           /* three-finger tap -> middle click */
        tap-then-hold;              /* tap, then touch+drag = drag-lock (recommended) */
        drag-lock;                  /* drag latches across finger lifts; tap releases */
        cursor-scale-percent = <50>;/* slow the cursor in firmware (100 = no scaling) */
        zoom;                       /* two-finger pinch/expand -> key clicks (experimental) */
        zoom-initial-distance = <20>;

        /* Press-and-hold drag-lock (alternate engagement; chip waits the
         * hold-time before latching). Combine with drag-lock for the persistent
         * variant. Omit if you only want tap-then-hold. */
        /* press-and-hold; */
        /* press-and-hold-time = <250>; */

        bottom-beta = <5>;
        stationary-threshold = <5>;

        switch-xy;
    };
};
```

### Mapping zoom to Ctrl+scroll on the host

The driver emits the zoom as key clicks (`INPUT_BTN_4` = spread, `INPUT_BTN_5` =
pinch); it does not itself send Ctrl. On the central (the half that talks to the
computer), turn those into a zoom with a behaviors input-processor (it is built
for key events, hence the key-click output):

```
#include <zephyr/dt-bindings/input/input-event-codes.h>

/ {
    behaviors {
        zoom_in: zoom_in {
            compatible = "zmk,behavior-macro";
            #binding-cells = <0>;
            bindings = <&macro_press &kp LCTRL>, <&macro_tap &msc SCRL_UP>,
                       <&macro_release &kp LCTRL>;
        };
        zoom_out: zoom_out {
            compatible = "zmk,behavior-macro";
            #binding-cells = <0>;
            bindings = <&macro_press &kp LCTRL>, <&macro_tap &msc SCRL_DOWN>,
                       <&macro_release &kp LCTRL>;
        };
    };

    zoom_proc: zoom_proc {
        compatible = "zmk,input-processor-behaviors";
        #input-processor-cells = <0>;
        type = <INPUT_EV_KEY>;
        codes = <INPUT_BTN_4 INPUT_BTN_5>; /* spread (zoom in), pinch (zoom out) */
        bindings = <&zoom_in &zoom_out>;
    };
};

/* Attach to the listener that consumes the trackpad (on a split peripheral
 * trackpad this is the input-split listener on the central): */
&tps43_input {
    input-processors = <&zoom_proc>;
};
```

