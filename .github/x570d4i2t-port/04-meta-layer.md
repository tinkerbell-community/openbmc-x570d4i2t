# Task: customize `meta-asrock/meta-x570d4i2t`

## Goal

Bring the auto-generated `meta-x570d4i2t` layer (commit `08cf84cb46`) from
"verbatim copy of x570d4u" to "produces a working OpenBMC image for the
X570D4I-2T".

The layer is at
[meta-asrock/meta-x570d4i2t/](../../meta-asrock/meta-x570d4i2t/). It currently
contains:

```
conf/
  layer.conf                        # collection name x570d4i2t-layer
  machine/x570d4i2t.conf            # KERNEL_DEVICETREE, FLASH_SIZE=32MB
  templates/default/
    bblayers.conf.sample
    conf-notes.txt
    local.conf.sample               # MACHINE ??= "x570d4i2t"
recipes-asrock/
  packagegroups/packagegroup-asrock-apps.bb
recipes-kernel/linux/
  linux-aspeed_%.bbappend           # appends x570d4i2t.cfg
  linux-aspeed/x570d4i2t.cfg        # enables NCT6775_I2C
recipes-x86/chassis/
  x86-power-control_%.bbappend
  x86-power-control/power-config-host0.json
```

## Edits required

### 1. `power-config-host0.json` — fill in GPIO `LineName` strings ⚠️ critical

[power-config-host0.json](../../meta-asrock/meta-x570d4i2t/recipes-x86/chassis/x86-power-control/power-config-host0.json)
has 12 entries; only `PostComplete`, `PowerButton`, `PowerOk`, `PowerOut`,
`ResetButton`, `ResetOut` have non-empty `LineName` fields (and only some of
those). Fill in the remaining ones to match the DTS work in
[03-device-tree.md](03-device-tree.md) §5.

Required exact mapping (these strings must appear *both* in the DTS
`gpio-line-names` and in this JSON):

```json
{
  "gpio_configs": [
    { "Name": "PostComplete", "LineName": "input-bios-post-cmplt-n", "Polarity": "ActiveLow", "Type": "GPIO" },
    { "Name": "PowerButton",  "LineName": "button-power-n",          "Polarity": "ActiveLow", "Type": "GPIO" },
    { "Name": "PowerOk",      "LineName": "input-power-good",        "Polarity": "ActiveHigh","Type": "GPIO" },
    { "Name": "PowerOut",     "LineName": "control-power-n",         "Polarity": "ActiveLow", "Type": "GPIO" },
    { "Name": "ResetButton",  "LineName": "button-reset-n",          "Polarity": "ActiveLow", "Type": "GPIO" },
    { "Name": "ResetOut",     "LineName": "control-reset-n",         "Polarity": "ActiveLow", "Type": "GPIO" }
  ]
}
```

Decide whether `IdButton`, `NMIButton`, `NMIOut`, `SioOnControl`,
`SioPowerGood`, `SIOS5` should remain present-but-unmapped (which is what
x570d4u ships) or be deleted. The X570D4I-2T Mini-ITX form factor likely has
no front-panel ID button — verify against the user manual
[../X570D4I-2T.pdf](../X570D4I-2T.pdf) front-panel header section before
removing.

Timing in this file can stay as-is — the X570D4U values
(`PowerPulseMs:200`, `ForceOffPulseMs:15000`, `GracefulPowerOffS:300`) are
generic and correct for AMD platforms.

### 2. `x570d4i2t.cfg` — verify sensor driver matches actual silicon

[x570d4i2t.cfg](../../meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed/x570d4i2t.cfg)
currently:

```
CONFIG_SENSORS_NCT6775_CORE=y
CONFIG_SENSORS_NCT6775_I2C=y
CONFIG_SENSORS_NCT6775=n
```

This covers NCT6779D / NCT6796D / NCT6798D / NCT6776 / NCT6791. If the chip
turns out to be NCT7802Y instead (Nuvoton's purpose-built server hwmon), change
to:

```
CONFIG_SENSORS_NCT7802=y
```

Discovery procedure: see [02-bmc-discovery.md](02-bmc-discovery.md) option A —
`cat /sys/bus/i2c/devices/*/name` on the live BMC.

Also evaluate adding:

```
# If host SBRMI is wired up for APML (AMD telemetry)
CONFIG_SENSORS_SBRMI=m
# Always-useful debug
CONFIG_DEBUG_FS=y
```

### 3. `linux-aspeed_%.bbappend` — pull in the new DTS patch

After [03-device-tree.md](03-device-tree.md) produces a patch:

```bitbake
FILESEXTRAPATHS:prepend:x570d4i2t := "${THISDIR}/linux-aspeed:"

SRC_URI:append:x570d4i2t = " \
    file://x570d4i2t.cfg \
    file://0001-aspeed-add-x570d4i2t-dts.patch \
    "
```

### 4. `packagegroup-asrock-apps.bb` — usually fine as-is

The current contents pull in:
- `phosphor-ipmi-flash` (for in-band firmware updates)
- `phosphor-host-postd` (POST code daemon)
- `phosphor-post-code-manager`
- `phosphor-power-regulators`

For the X570D4I-2T add **phosphor-pid-control** (active fan control) by
appending to `RDEPENDS:${PN}-system`:

```bitbake
RDEPENDS:${PN}-system = " \
        phosphor-host-postd \
        phosphor-post-code-manager \
        phosphor-power-regulators \
        phosphor-pid-control \
        "
```

### 5. NEW: phosphor-pid-control configuration

Create `recipes-phosphor/fans/phosphor-pid-control/phosphor-pid-config-native_%.bbappend`
that ships a `phosphor-pid-config.json` matching the discovered fan behavior:

- 3 fan zones (or 1 zone with 3 fan outputs)
- Temperature inputs: `CPU Temp` (sensor 52), `X570 Temp` (sensor 59),
  `MB Temp` (sensor 49)
- Curve from `/api/asrr/settings/getfanopenloopcontroltable`:
  30°C→20%, 60→30%, 70→40%, 80→50%, 90→60%, 100→100%
- PID coefficients: start with values from a similar AMD board
  (e.g. `meta-romed8hm3` or `meta-spc621d8hm3`) and tune

### 6. NEW (optional): entity-manager configuration

Create `recipes-phosphor/configuration/entity-manager/entity-manager_%.bbappend`
that drops an `x570d4i2t.json` describing the board so entity-manager-aware
services (dbus-sensors, etc.) can discover the layout automatically. Schema in
upstream `phosphor-entity-manager/configurations/`. Use the ROMED8HM3 JSON as
a starting template — also AMD AM4-class.

### 7. NEW (optional): inventory JSON

`recipes-phosphor/inventory/phosphor-inventory-manager_%.bbappend` to add 4
DIMM-slot inventory items (D1=DDR4_A1, D2=DDR4_A2, D3=DDR4_B1, D4=DDR4_B2) so
that `phosphor-ipmi-inventory` reports them in `ipmitool fru list`.

## Files to create

```
meta-asrock/meta-x570d4i2t/
├── recipes-kernel/linux/linux-aspeed/
│   ├── aspeed-bmc-asrock-x570d4i2t.dts             (NEW — from task 03)
│   └── 0001-aspeed-add-x570d4i2t-dts.patch         (NEW — wraps the dts)
├── recipes-phosphor/
│   ├── fans/phosphor-pid-control/
│   │   ├── phosphor-pid-config-native_%.bbappend   (NEW)
│   │   └── phosphor-pid-config-native/phosphor-pid-config.json (NEW)
│   └── configuration/entity-manager/               (NEW, optional)
│       ├── entity-manager_%.bbappend
│       └── entity-manager/x570d4i2t.json
└── recipes-x86/chassis/x86-power-control/
    └── power-config-host0.json                     (EDIT — fill LineNames)
```

## Build

```bash
cd /home/appkins/src/tinkerbell-community/openbmc
. setup x570d4i2t
bitbake obmc-phosphor-image
```

Output artifacts land at
`tmp/deploy/images/x570d4i2t/obmc-phosphor-image-x570d4i2t.{static,mtd,ubi}`.

## Verification

```bash
bitbake-layers show-layers | grep x570d4i2t
bitbake -e | grep '^MACHINE='        # should print MACHINE="x570d4i2t"
bitbake -e | grep '^KERNEL_DEVICETREE='
# Confirm the DTB is in the produced image:
ls tmp/deploy/images/x570d4i2t/aspeed-bmc-asrock-x570d4i2t.dtb
```

## Definition of done

- [ ] `bitbake obmc-phosphor-image` succeeds with no failures
- [ ] `power-config-host0.json` LineNames match the DTS gpio-line-names exactly
- [ ] Kernel `.cfg` driver selection matches the chip on the actual I2C bus
- [ ] Image artifact `obmc-phosphor-image-x570d4i2t.static.mtd` exists and is
      ≤ the configured `FLASH_SIZE` of 32 MB
- [ ] Image deploys to the `x570d4i2t` machine dir, not a fallback
