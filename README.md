# ZMK driver for Azoteq IQS5XX trackpads

> **Fork notice (Frosthaven).** This is a fork of
> [AYM1607/zmk-driver-azoteq-iqs5xx](https://github.com/AYM1607/zmk-driver-azoteq-iqs5xx)
> with extra gesture support. Differences vs upstream:
>
> - **Two-finger zoom (pinch + expand).** New `zoom` devicetree property. The
>   chip's zoom gesture is enabled and its two directions are emitted on
>   distinct relative codes (`INPUT_REL_MISC` = expand/zoom in,
>   `INPUT_REL_DIAL` = pinch/zoom out) so they never collide with scroll and can
>   be mapped independently. Map each to Ctrl+scroll on the host with a
>   `zmk,input-processor-behaviors` processor (see Usage). Two codes are used
>   because that processor keys on the event code, not the value's sign.
> - **Double-tap-and-drag.** New `drag-requires-double-tap` (+ `double-tap-time`,
>   default 300 ms) properties. When set, a press-and-hold only latches a drag
>   if it follows a single tap within the window (macOS-style); a bare
>   press-and-hold just moves the cursor.
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
- Single finger tap: Reported as a left click.
- Two finger tap: Reported as a right click.
- Press and hold: Reported as a continuos left click (allows click and drag).
- Double-tap-and-drag (fork): with `drag-requires-double-tap`, drag only latches when the hold follows a tap.
- Vertical scroll.
- Horizontal scroll.
- Two-finger zoom / pinch+expand (fork): expand on `INPUT_REL_MISC`, pinch on `INPUT_REL_DIAL`, map both to Ctrl+scroll on the host.

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

