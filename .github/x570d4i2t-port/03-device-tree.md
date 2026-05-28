# Task: Linux device tree for the X570D4I-2T

## Goal

Create `arch/arm/boot/dts/aspeed/aspeed-bmc-asrock-x570d4i2t.dts` so that the
already-configured machine `.conf` produces a matching DTB.

The machine `.conf` already declares:
```
KERNEL_DEVICETREE = "aspeed/aspeed-bmc-asrock-${MACHINE}.dtb"
```
where `MACHINE=x570d4i2t` — so the DTB filename must be exactly
`aspeed-bmc-asrock-x570d4i2t.dtb`, sourced from a DTS of the same stem.

## Delivery options

### Path A — patch the kernel inside the meta-layer (recommended for in-flight porting)

Put the DTS in `meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed/`
and have the bbappend wire it in as a kernel source override.

Steps:

1. Place the new DTS at:
   ```
   meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed/aspeed-bmc-asrock-x570d4i2t.dts
   ```
2. Add a small Makefile fragment so the kernel build picks it up — easiest
   approach is to ship a *.patch that touches both
   `arch/arm/boot/dts/aspeed/Makefile` (adds the .dtb target) and adds the
   new .dts file:
   ```
   meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed/0001-aspeed-add-x570d4i2t-dts.patch
   ```
3. Edit the existing bbappend at
   [meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed_%.bbappend](../../meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed_%25.bbappend):
   ```
   FILESEXTRAPATHS:prepend:x570d4i2t := "${THISDIR}/linux-aspeed:"

   SRC_URI:append:x570d4i2t = " \
       file://x570d4i2t.cfg \
       file://0001-aspeed-add-x570d4i2t-dts.patch \
       "
   ```

### Path B — upstream

Submit `aspeed-bmc-asrock-x570d4i2t.dts` to linux-aspeed
(`linux-aspeed@lists.ozlabs.org`) and bump the kernel SRCREV in
`meta-aspeed` once merged. Use Path A first; switch to Path B after the
port is validated.

## Starting point

The closest upstream-supported sibling is `aspeed-bmc-asrock-x570d4u.dts`. Fetch
it from the kernel tree being used by this build (either via the active build
work-dir or directly from kernel.org):

```bash
# After at least one build:
find . -name 'aspeed-bmc-asrock-x570d4u.dts' \
     -path '*tmp/work-shared*' 2>/dev/null

# Or pull the current upstream copy:
curl -sLO https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/arch/arm/boot/dts/aspeed/aspeed-bmc-asrock-x570d4u.dts
```

Inspect it for the conventions ASRock uses on AST2500: I2C bus assignments,
PWM/tach channels, GPIO line names, MAC/PHY mux, SPI flash partitioning.

## Required deltas vs `aspeed-bmc-asrock-x570d4u.dts`

### 1. Identity

```dts
/dts-v1/;

#include "aspeed-g5.dtsi"
#include <dt-bindings/gpio/aspeed-gpio.h>
#include <dt-bindings/i2c/i2c.h>

/ {
    model = "ASRockRack X570D4I-2T BMC";
    compatible = "asrock,x570d4i2t-bmc", "aspeed,ast2500";
    /* … */
};
```

### 2. MAC fan-out (critical — different mode for each interface)

| MAC | AST2500 controller | Mode | PHY/target |
|---|---|---|---|
| eth0 | MAC1 | **RGMII / RMII** to dedicated PHY | Realtek RTL8211E (1 GbE PHY) |
| eth1 | MAC2 | **NC-SI** | Sideband into Intel X550-AT2 |

Pattern:

```dts
&mac0 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_rgmii1_default &pinctrl_mdio1_default>;
    phy-mode = "rgmii";
    phy-handle = <&phy0>;

    mdio0 {
        #address-cells = <1>;
        #size-cells = <0>;
        phy0: ethernet-phy@0 {
            reg = <0>; /* TODO verify PHY address (commonly 0 or 1 for RTL8211E) */
        };
    };
};

&mac1 {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_rmii2_default>;
    use-ncsi;
};
```

> The PHY address for the RTL8211E and the exact pinctrl group names need to
> be verified against the X570D4U DTS and the AST2500 binding docs in
> `Documentation/devicetree/bindings/net/aspeed,ast2500-mdio.yaml`.

### 3. Fan / PWM table — three channels only

```dts
&pwm_tacho {
    status = "okay";
    pinctrl-names = "default";
    pinctrl-0 = <&pinctrl_pwm0_default
                 &pinctrl_pwm1_default
                 &pinctrl_pwm2_default>;

    fan@0 { reg = <0x00>; aspeed,fan-tach-ch = /bits/ 8 <0x00>; };
    fan@1 { reg = <0x01>; aspeed,fan-tach-ch = /bits/ 8 <0x01>; };
    fan@2 { reg = <0x02>; aspeed,fan-tach-ch = /bits/ 8 <0x02>; };
};
```

> **Verify** the PWM channel → physical fan header mapping against the X570D4U
> DTS. If the X570D4U routes via `pwm0/pwm4/pwm5` etc., copy whichever channels
> drive headers FAN1-FAN3 on the X570D4I-2T (board layout in
> [../X570D4I-2T.pdf](../X570D4I-2T.pdf)).

### 4. Sensor IC nodes

The kernel `.cfg` enables `CONFIG_SENSORS_NCT6775_I2C=y`. This covers the
NCT6779D, NCT6796D, NCT6798D family on i2c. Add a node on the I2C bus that
hosts the chip:

```dts
&i2c_NN_TBD {
    status = "okay";

    /* one of nct6779/nct6796/nct6798 depending on what i2cdetect/dmesg reports */
    nct6779: hwmon@2d {
        compatible = "nuvoton,nct6779";
        reg = <0x2d>;
    };
};
```

> **Discover via** [02-bmc-discovery.md §"When the BMC API isn't enough"](02-bmc-discovery.md):
> the live MegaRAC `cat /sys/bus/i2c/devices/*/name` (or `dmesg | grep nct`)
> answers both *which I2C bus* and *which chip variant*. If the chip is actually
> NCT7802Y (Nuvoton's lower-pin server hwmon part), switch the kernel `.cfg`
> from `CONFIG_SENSORS_NCT6775_I2C=y` to `CONFIG_SENSORS_NCT7802=y` and use
> `compatible = "nuvoton,nct7802"`.

### 5. GPIO line names — must match `power-config-host0.json`

`x86-power-control` references these line names. The DTS must declare them
so `gpiod` can find them. Pattern:

```dts
&gpio {
    gpio-line-names =
        /*  0..7  */ "", "", "", "", "", "", "", "",
        /*  8..15 */ "", "", "", "", "", "", "", "",
        /* …                                          */
        /* AA..AH */ "button-power-n", "input-power-good", ...,
        /* …                                          */
        ;
};
```

The exact bit positions need to match the AST2500 GPIO numbering (banks A..AC,
8 pins per bank). The currently-empty `LineName` fields in
[power-config-host0.json](../../meta-asrock/meta-x570d4i2t/recipes-x86/chassis/x86-power-control/power-config-host0.json#L23)
indicate the four core signals (and they must end up with these exact strings):

| `Name` in JSON | Required `LineName` in DTS |
|---|---|
| PostComplete | `input-bios-post-cmplt-n` |
| PowerButton | `button-power-n` |
| PowerOk | `input-power-good` |
| PowerOut | `control-power-n` |
| ResetButton | `button-reset-n` |
| ResetOut | `control-reset-n` |

> ASRock AST2500 boards almost universally route:
> - PowerButton out from BMC: GPIO E0 or E1
> - PowerGood input: GPIO E2 or D5
> - Reset out: GPIO B5 / B6
> - POSTCMPLT_N: GPIO H4 or H6
>
> But verify against the X570D4U DTS or the stock MegaRAC's `/sys/class/gpio`
> before committing — wrong polarity or wrong bank will brick power control.

### 6. SPI flash layout

The MegaRAC partitioned the 32 MB SPI flash into UBoot / Image / Kernel /
Rootfs / SOLPersist / etc. OpenBMC's stock layout differs (UBoot env, FIT
image, RWFS). The DTS should declare partitions that match what
`obmc-phosphor-image-x570d4i2t.static.mtd` lays down — usually inherited from
`obmc-bsp-common.inc` automatically via `&fmc { ... }`.

If you need to override:

```dts
&fmc {
    status = "okay";
    flash@0 {
        status = "okay";
        m25p,fast-read;
        label = "bmc";
        spi-max-frequency = <50000000>;
#include "openbmc-flash-layout-64.dtsi"
    };
};
```

`openbmc-flash-layout-64.dtsi` is the standard 64 MB layout; for 32 MB use
`openbmc-flash-layout.dtsi`. Confirm against `FLASH_SIZE = "65536"` (32 MB)
declared in
[x570d4i2t.conf](../../meta-asrock/meta-x570d4i2t/conf/machine/x570d4i2t.conf#L7).

### 7. UARTs and SOL

Match `SUPERIO001=3F8h/IRQ4` (COM1 → SOL):

```dts
&uart5 {
    status = "okay";   /* SoC debug UART */
};

&vuart {
    status = "okay";   /* virtual UART for SOL — bridges KCS to /dev/ttyVUART0 */
};
```

The host-visible COM1/COM2 are physically driven by the AMI SuperIO, not the
BMC SoC — the BMC only snoops via VUART for SOL.

### 8. Host-BMC USB-NCM

```dts
&vhub {
    status = "okay";
};
```

OpenBMC's USB gadget config typically synthesizes the BMC-side iface
(`9A:01:77:6F:BA:40` was observed but is just a build-time default — pick any
locally-administered MAC). The host-side MAC `02:DC:A4:B5:22:55` is what the
host enumerates as.

## Verification

```bash
. setup x570d4i2t
bitbake virtual/kernel -c compile
fdtdump tmp/work/x570d4i2t-openbmc-linux-gnueabi/linux-aspeed/*/build/arch/arm/boot/dts/aspeed/aspeed-bmc-asrock-x570d4i2t.dtb \
  | head -200
# Confirm the model and key nodes:
fdtdump …/aspeed-bmc-asrock-x570d4i2t.dtb | grep -E 'model|compatible|gpio-line-names|use-ncsi'
```

Sanity checks before flashing:

- DTB compiles without warnings
- `gpio-line-names` contains all six required strings
- One `mac0` with PHY child, one `mac1` with `use-ncsi`
- Exactly three fan@N nodes under `pwm_tacho`
- One hwmon node on a populated I2C bus

## Definition of done

- [ ] DTS file placed under `meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed/`
- [ ] Kernel patch applied cleanly via bbappend
- [ ] `bitbake virtual/kernel` builds the matching DTB
- [ ] `fdtdump` output matches the verification list above
- [ ] [04-meta-layer.md](04-meta-layer.md) `power-config-host0.json` updated to match new line names
