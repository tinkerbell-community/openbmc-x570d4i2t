# Task: flash and verify

## Goal

Replace the signed AMI MegaRAC firmware on the X570D4I-2T's 32 MB SPI flash
with the OpenBMC image built in [04-meta-layer.md](04-meta-layer.md), then
validate that all critical functions still work — comparing against the
baseline captured in [01-hardware-inventory.md](01-hardware-inventory.md).

## ⚠️ Why external SPI flashing is required

- The stock AMI BIOS P2.50 and MegaRAC fw 1.91.00 are **signature-verified**.
  The stock web UI `/api/maintenance/firmware/upgrade` checks the image
  signature and will reject any unsigned OpenBMC build.
- There is no documented bypass on the X570D4I-2T (no `/api/maintenance/signed-image-key`
  override surface, no jumper documented in the user manual).
- An external SPI programmer is the only reliable path.

## Hardware needed

| Item | Notes |
|---|---|
| External SPI programmer | CH341A USB stick ($5), Bus Pirate, or Raspberry Pi + `flashrom` |
| SOIC8 clip or test-hook leads | The BMC flash chip is a standard SOIC8 SPI part |
| `flashrom` ≥ 1.2 | Most distros package it. Arch: `pacman -S flashrom` |

The BMC SPI flash chip location on the board is documented in
[../X570D4I-2T.pdf](../X570D4I-2T.pdf) — look for "BMC SPI flash" or a U-label
near the AST2500 chip. Typical labels: `U10` or `U24`.

## Phase 1 — backup the stock flash (do this FIRST)

System should be **fully powered off** with AC removed before clipping the
SOIC8 chip. The chip is shared with the live BMC, and the BMC can corrupt the
read if it writes during your read.

```bash
# CH341A example. For other programmers see `flashrom -L`.
flashrom -p ch341a_spi -r stock-x570d4i2t-bmc.bin

# Always read twice and compare:
flashrom -p ch341a_spi -r stock-x570d4i2t-bmc-2.bin
cmp stock-x570d4i2t-bmc.bin stock-x570d4i2t-bmc-2.bin && echo OK
```

**Keep `stock-x570d4i2t-bmc.bin` safe and offline.** It is your only path back
to a working factory BMC if something goes wrong.

## Phase 2 — extract preserved values from the stock dump

You captured the live MAC addresses in
[01-hardware-inventory.md](01-hardware-inventory.md):
- eth0 (dedicated mgmt): `9C:6B:00:4E:1C:2A`
- eth1 (NC-SI sideband): `9C:6B:00:70:57:A4`

These are stored *somewhere* in the stock dump and must be re-injected into
the OpenBMC image (or, more commonly, into the u-boot environment that
OpenBMC reads at boot).

```bash
# Find raw MAC bytes:
xxd stock-x570d4i2t-bmc.bin | grep -i "9c 6b 00"

# Often they live in the u-boot env partition. Find the env:
binwalk stock-x570d4i2t-bmc.bin
binwalk -e stock-x570d4i2t-bmc.bin
# Look in extracted dir for files containing `ethaddr=` or `eth1addr=`.

# Or extract a packed DTB/u-boot env:
strings stock-x570d4i2t-bmc.bin | grep -E 'ethaddr|serial#|baudrate' | head
```

Other values worth recovering:
- Board serial `BR80H6008900252` (already known)
- Board UUID `02ab11b21dd22608dd1e8e00fd861f45`
- Any vendor-burned PSP-related cryptographic blobs (usually NOT in BMC flash —
  these live in the AMI BIOS SPI on a separate chip)

## Phase 3 — inject MACs into the OpenBMC image

Two common approaches:

### Option A — patch u-boot env in the OpenBMC image

```bash
# Locate the env offset in the produced image (it's documented in
# openbmc-flash-layout.dtsi)
fw_printenv -c /dev/null --config=fw_env.config \
            tmp/deploy/images/x570d4i2t/obmc-phosphor-image-x570d4i2t.static.mtd \
            ethaddr eth1addr
# Set:
fw_setenv --config=fw_env.config \
          ethaddr 9C:6B:00:4E:1C:2A
fw_setenv --config=fw_env.config \
          eth1addr 9C:6B:00:70:57:A4
```

### Option B — set systemd-networkd `MACAddress=`

Drop a `.link` file in the meta-layer at:
```
meta-asrock/meta-x570d4i2t/recipes-network/systemd-networkd/systemd-networkd_%.bbappend
meta-asrock/meta-x570d4i2t/recipes-network/systemd-networkd/files/00-eth0-mac.link
```

Contents of `00-eth0-mac.link`:
```ini
[Match]
OriginalName=eth0
[Link]
MACAddress=9C:6B:00:4E:1C:2A
```

> Option A is more robust (survives a `factory-reset`), but couples the build
> to per-unit data. Option B is the OpenBMC norm.

## Phase 4 — flash OpenBMC

```bash
flashrom -p ch341a_spi -w tmp/deploy/images/x570d4i2t/obmc-phosphor-image-x570d4i2t.static.mtd
```

`flashrom` will read-back-verify automatically.

## Phase 5 — first boot

1. Reattach AC. The host stays powered off; BMC powers up on standby.
2. Open a serial console to the BMC debug UART (header pinout in the user
   manual). Watch for u-boot → kernel → systemd handoff.
3. After ~30 s, the dedicated mgmt PHY should DHCP an address.

If serial isn't accessible, watch your DHCP server's log for a new lease on
MAC `9C:6B:00:4E:1C:2A`.

## Phase 6 — verification checklist

SSH into the running OpenBMC (`ssh root@<bmc-ip>`, default OpenBMC root pwd
`0penBmc` — change immediately).

### Identity

```bash
cat /etc/os-release | grep '^VERSION'      # confirms OpenBMC build
cat /sys/firmware/devicetree/base/model    # ASRockRack X570D4I-2T BMC
ip addr show eth0                          # MAC should be 9C:6B:00:4E:1C:2A
```

### GPIO line names exposed (must match power-config-host0.json)

```bash
gpioinfo | grep -E 'button-power-n|input-power-good|control-power-n|control-reset-n|input-bios-post-cmplt-n'
```

All six labels should appear, each on a single bank/line. Missing line ⇒ DTS
incomplete.

### I2C sensor IC

```bash
ls /sys/class/hwmon/
for d in /sys/class/hwmon/hwmon*; do echo "== $d ==" ; cat $d/name; done
sensors                          # if lm-sensors is packaged
```

Should show the nct67xx (or nct7802) with temp/in/fan channels.

### Sensor readings — cross-check against baseline

```bash
busctl tree xyz.openbmc_project.Sensor.Hwmon
# Or via IPMI from a workstation:
ipmitool -I lanplus -H <bmc-ip> -U root -P '<pw>' sdr
```

Expected at idle (from [01-hardware-inventory.md](01-hardware-inventory.md) §Sensor inventory):

| Sensor | Expected idle | Tolerance |
|---|---|---|
| 3VSB | 3.36 V | ±5 % |
| 5VSB | 5.07 V | ±5 % |
| 12V | 12.00 V | ±5 % |
| VCPU | 0.97 V | ±10 % (Pstate-dependent) |
| BAT | 2.90 V | depletes over years |
| CPU Temp | ~50 °C | ±10 °C idle |
| X570 Temp | ~60 °C | always warm |
| MB Temp | ~43 °C | airflow-dependent |

Major deviations (>20%) suggest wrong scale factor in the DTS or wrong I2C
device class — fix in the DTS, not by adjusting thresholds.

### POST snooping

```bash
journalctl -u phosphor-host-postd -f
# In another terminal, power on the host:
busctl call xyz.openbmc_project.State.Host xyz/openbmc_project/state/host0 \
       xyz.openbmc_project.State.Host RequestedHostTransition s On
```

The journal should stream the same byte/16-bit codes captured at
`/api/logs/postcode` in the stock baseline.

### Fan control

```bash
busctl tree xyz.openbmc_project.Hwmon.external
# Set a manual override to test:
busctl call xyz.openbmc_project.Settings /xyz/openbmc_project/control/host0/fan_mode \
       org.freedesktop.DBus.Properties Set ss xyz.openbmc_project.Control.FanMode \
       v string "Manual"
```

All three fans (FAN1-FAN3) should respond to PWM duty changes. FAN4+ should
not appear.

### NC-SI sideband

```bash
# Switch BMC active LAN from dedicated to NC-SI:
busctl call xyz.openbmc_project.Network /xyz/openbmc_project/network/config \
       xyz.openbmc_project.Network.SystemConfiguration ...
# Or via Redfish:
curl -ks -k -X PATCH https://<bmc>/redfish/v1/Managers/bmc/EthernetInterfaces/eth1 \
     -u root:<pw> -H 'Content-Type: application/json' \
     -d '{"InterfaceEnabled":true}'
```

When eth1 is enabled with NC-SI, traffic should egress through one of the
X550-AT2 ports on the host (with the host system powered on so the X550 is
up).

## Rollback

If anything is broken in a way you can't fix over SSH (e.g. wrong MAC means
you can't see the BMC on the network):

1. Power off host, remove AC.
2. Clip the SPI flash again.
3. `flashrom -p ch341a_spi -w stock-x570d4i2t-bmc.bin` — fully restores the
   factory MegaRAC.
4. Reattach AC. Stock BMC returns at 10.0.80.1.

OpenBMC supports **dual-image** flashing on AST2500 boards (two firmware slots
inside the 32 MB SPI). Once a first OpenBMC boot succeeds, future updates can
go over the network via `phosphor-bmc-code-mgmt` and the dual-slot scheme
provides a safe fallback without re-flashing externally.

## Definition of done

- [ ] Stock flash dumped twice, byte-identical
- [ ] Stock MACs identified and inserted into OpenBMC config or u-boot env
- [ ] OpenBMC image flashed and read-back verified
- [ ] BMC pulls DHCP on the dedicated PHY with the correct MAC
- [ ] All six required GPIO line names are present and `gpioinfo`-visible
- [ ] At least the 11 voltages, 5 working temps, and 3 fans report values
      within ±10 % of the stock baseline
- [ ] Powering the host on streams POST codes through `phosphor-host-postd`
- [ ] (Optional) NC-SI sideband on eth1 carries traffic when active
