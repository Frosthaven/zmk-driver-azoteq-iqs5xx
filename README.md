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
> - **Double-tap-and-drag LOCK.** New `drag-requires-double-tap` (+
>   `double-tap-time`, default 275 ms) properties. A single tap then a touch
>   within the window latches the left button as a drag that stays held across
>   finger lifts (drag-lock); a stationary tap ends it (a 2-/3-finger tap ends it
>   *and* issues its click). A bare press-and-hold just moves the cursor.
> - **Two-finger zoom (pinch/expand) — plumbing only, non-functional in
>   practice.** New `zoom` (+ `zoom-initial-distance`) property. The driver
>   enables the chip's zoom gesture and emits its two directions on
>   `INPUT_REL_MISC` (expand) / `INPUT_REL_DIAL` (pinch) for a Ctrl+scroll
>   mapping (see Usage). **However, the IQS5xx does not appear to raise the zoom
>   gesture** on the modules tested, and no known implementation (QMK, the rwalkr
>   Rust crate, the Linux kernel driver) exercises it either. The code is left in
>   place in case a future module/config fires it; treat pinch-zoom as
>   unsupported on current hardware.
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

## Supported features

- Trackpad movement.
- Single finger tap: left click (synthesized on lift).
- Two finger tap: right click (synthesized on lift by peak finger count).
- Three finger tap: middle click (fork; `three-finger-tap`).
- Press and hold: continuous left click (allows click and drag).
- Double-tap-and-drag lock (fork): with `drag-requires-double-tap`, a tap then a
  touch latches a drag that holds across lifts and ends on a stationary tap.
- Vertical scroll.
- Horizontal scroll.
- Two-finger zoom / pinch+expand (fork): **implemented but non-functional on
  tested IQS5xx hardware** — the chip does not raise the zoom gesture in
  practice. Plumbing (expand on `INPUT_REL_MISC`, pinch on `INPUT_REL_DIAL`,
  mapped to Ctrl+scroll) is left in place; treat as unsupported.

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
        press-and-hold;
        press-and-hold-time = <250>;
        two-finger-tap;

        scroll;
        natural-scroll-y;
        natural-scroll-x;

        /* Fork additions: */
        zoom;                       /* two-finger pinch/expand -> INPUT_REL_MISC */
        drag-requires-double-tap;   /* press-and-hold only drags after a tap */
        double-tap-time = <300>;

        bottom-beta = <5>;
        stationary-threshold = <5>;

        switch-xy;
    };
};
```

### Mapping zoom to Ctrl+scroll on the host

The driver emits the zoom magnitude on `INPUT_REL_MISC`; it does not itself send
Ctrl. On the central (the half that talks to the computer), turn it into a zoom
with a behaviors input-processor:

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
        type = <INPUT_EV_REL>;
        codes = <INPUT_REL_MISC INPUT_REL_DIAL>; /* expand, pinch */
        bindings = <&zoom_in &zoom_out>;
    };
};

/* Attach to the listener that consumes the trackpad (on a split peripheral
 * trackpad this is the input-split listener on the central): */
&tps43_input {
    input-processors = <&zoom_proc>;
};
```

