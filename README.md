# ZMK driver for Azoteq IQS5XX trackpads

> **Fork notice (Frosthaven).** Fork of
> [AYM1607/zmk-driver-azoteq-iqs5xx](https://github.com/AYM1607/zmk-driver-azoteq-iqs5xx)
> with a few extras:
>
> - **Three-finger tap** for middle click.
> - **Tap-then-drag** for drag and drop: tap, then touch and drag — the left
>   button stays held across finger lifts so the drag can span multiple
>   strokes. Tap once to release. Dragging a multi-file selection no longer
>   deselects it.
> - **Cursor tuned to feel like an Apple trackpad** (about half the chip's
>   default speed, with scroll slowed proportionally and slow precision
>   movement preserved).

## Compatibility

This driver should work with any IQS5XX based trackpad. However, it has only been tested and known to work with the following models:

- TPS43 (Reached EOL)
- TPS65 (Reached EOL)

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
| Drag with two fingers | Scroll (vertical / horizontal) |

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
        tap-then-hold;              /* tap, then touch+drag = drag-lock */
        drag-lock;                  /* drag latches across finger lifts; tap releases */
        cursor-scale-percent = <50>;/* feels like an Apple trackpad (100 = chip default) */

        bottom-beta = <5>;
        stationary-threshold = <5>;

        switch-xy;
    };
};
```

