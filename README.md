# Zmk Split Peripheral Input Relay

This module add a input relay to input subsystem for ZMK.

## What it does

This module sideload a new set of GATT Service and Characteratics into existing split bt paired connection. The new characteristics allow to transfer input event from peripherals to central with a relay-channel id. Then, input events would re-emitted from a `zmk,virtual-device` on central. The events from peripherals could be handled by `zmk,input-listener` on central side.

In short, user can read more than one pointing device on central and peripherals sheild simultaneously.

## Installation

Include this project on your ZMK's west manifest in `config/west.yml`:

```yaml
manifest:
  remotes:
    # ...
    - name: badjeff
      url-base: https://github.com/badjeff
  projects:
    # ...
    # START #####
    - name: zmk-split-peripheral-input-relay
      remote: badjeff
      revision: main
    # END #######
    # ...
```

Update `board.keymap`:
```keymap
/ {
	/* assign `input-listener` to all pointing devices */
	/* &pd0 on central, &pd1 on peripheral */

        trackball_central_listener {
                compatible = "zmk,input-listener";
                device = <&pd0>;
	};

        trackball_peripheral_listener {
                compatible = "zmk,input-listener";
                device = <&pd1>;
	};
}/
```

Update split peripheral devicetree file `board_right.overlay`:
```dts
/* enable &pd0 on split peripheral. typical input device for zephyr input subsystem. */
/* NOTE 1: use the same device alias (&pd0) on central and peripheral(s) is alright. */
/* NOTE 2: input event shall be intecepted by module `zmk-split-peripheral-input-relay`. */

/* this is an alias of your actual wired input device sensor in your peripheral shield. */
/* e.g. SPI optical sensor for trackball */
&pd0 {
	status = "okay";
	/* the rest of sensor config should be config here, e.g. gpios */
};

/ {
	/* THIS make keymap binding happy only, nothing happen on peripheral side */
	pd1: virtual_input {
		compatible = "zmk,virtual-input";
	};

	/* for peripheral side, define (input-device)-to-(relay-channel) mapping */
	input_relay_config_102 {
		compatible = "zmk,split-peripheral-input-relay";

		/* peripheral side input device, used to... */
		/*  - be intecepted on peripheral; */
		/*  - and then, be resurrected as `zmk,virtual-device` on central; */
		device = <&pd0>;
		
		/* channel id, used to be be transfered along with all input events. */
		/* NOTE 1: pick any 8bit integer. (1 - 255) */
		/* NOTE 2: should matching relay-channel on central overlay */
		relay-channel = <102>;
	};
};
```

Update split central devicetree file `board_left.overlay`:
```dts
/* enable &pd0 on central. typical input device for zephyr input subsystem. */
/* NOTE 1: use the same device alias (&pd0) on central and peripheral(s) is alright. */

/* this is an alias of your actual wired input device sensor in your central shield. */
/* e.g. SPI optical sensor for trackball */
&pd0 {
	status = "okay";
	/* the rest of sensor config should be config here, e.g. gpios */
};

/ {
	/* define virtual input, will be resurrected for emitting input event */
	/* NOTE: set `device = <&pd1>` in `zmk,input-listener` */
	pd1: virtual_input {
		compatible = "zmk,virtual-input";
	};

	/* for central side, define (relay-channel)-to-(virtual-input) mapping */
	input_relay_config_102 {
		compatible = "zmk,split-peripheral-input-relay";
		
		/* channel id, used to filter incoming input event from split peripheral */
		/* NOTE: should matching relay-channel on peripheral overlay */
		relay-channel = <102>;

		/* virtual input device on central, which used to emit input event as an agent device */
		device = <&pd1>;
	};
};

```

Enable the input config in your all `<shield>_{ left | right }.config`:
```conf
CONFIG_INPUT=y
/* plus, other input device drive config */
```

