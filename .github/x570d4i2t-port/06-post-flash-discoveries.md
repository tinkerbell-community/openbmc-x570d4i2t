# Post-flash discoveries — X570D4I-2T

What the live OpenBMC build revealed after we got past first-boot. Many
assumptions baked into [01-hardware-inventory.md](01-hardware-inventory.md)
turned out to be wrong or incomplete; this doc captures the deltas.

## NCT6779 SuperIO is the real fan/temp chip — not just W83773G

Stock MegaRAC's "X570 Temp", "SYSTIN", "CPUTIN", and similar host-rail sensors
are sourced from a **Nuvoton NCT6779D-family SuperIO at i2c-1 0x2d**. Not the
W83773G. The W83773G at 0x4c only provides MB Temp + Card Side Temp (its
remote-2 diode RDOS2 is open on this board, so the "X570 Temp" we tried to
read off it always shows 0).

NCT6779 hwmon index → label (verified at runtime via `temp*_label` files):

| Index | Label | Notes |
|---|---|---|
| temp1 | SYSTIN | works |
| temp2 | CPUTIN | reads saturated 97.5°C — likely unwired (real CPU temp = SBRMI) |
| temp3 | AUXTIN0 | reads saturated 99°C — unwired |
| temp4 | AUXTIN1 | real ~44°C |
| temp5 | AUXTIN2 | real ~41°C |
| temp6 | AUXTIN3 | reads -60°C — unwired |
| temp7 | PCH_CHIP_CPU_MAX_TEMP | 0 — relay path uninitialized (see below) |
| temp8 | PCH_CHIP_TEMP | **0** — this IS the "X570 Temp" the stock UI showed, but uninitialized |
| temp9 | PCH_CPU_TEMP | 0 — uninitialized |
| temp10 | PCH_MCH_TEMP | 0 — uninitialized |
| fan1, fan2 | tach inputs | **0 always** — nothing on this board wires to them |
| pwm1, pwm2 | only 2 PWMs | even though NCT6779D has up to 5 |

**The PCH_* temps read 0** because the NCT6779 sources them via an internal
SMBus/PECI relay path that BIOS configures over LPC. The BMC-side i2c driver
can't initialize those relays — we get the chip in its uninitialized state.
Stock MegaRAC saw real X570 Temp because MegaRAC read the SuperIO via host-LPC
after BIOS had set it up. We can't replicate that from the BMC i2c side.

## nct6775-i2c driver is intentionally read-only

Upstream commit logs explicitly mark `nct6775-i2c` as the "back door" to the
chip — register-level i2c writes are accepted (`i2cset` verified by readback),
but the driver does not expose PWM control sysfs entries. LPC is the "front
door". For this port, that means **the BMC cannot drive SuperIO PWMs**.

## entity-manager "NCT6779" / "pmbus" silent-drop bug — fix is PowerState

Entity-manager silently drops dbus-sensors stanzas (`pmbus`, `NCT6779`,
`NCT7802`, W83773G clones) **without any log line**. Schema validation passes
in `jsonschema`. The discriminator is the **`PowerState` field**: stanzas
without it get dropped even though the schema marks it optional.

**Always include `"PowerState": "Always"`** (or On/ChassisOn/BiosPost as
appropriate) on every dbus-sensors i2c-instantiated stanza. The ADC and
W83773G stanzas in our config happen to work because of how EM treats those
types differently — don't extrapolate from them.

## Workaround: NCT6779 → ExternalSensor bridge daemon

Since entity-manager won't accept an `NCT6779` stanza, we ship a small shell
daemon at
[meta-asrock/meta-x570d4i2t/recipes-asrock/nct6779-bridge/](../../meta-asrock/meta-x570d4i2t/recipes-asrock/nct6779-bridge/)
that polls `/sys/class/hwmon/hwmonN` and `busctl set-property`-pushes values
into `ExternalSensor` objects entity-manager creates from the JSON. Six temp
+ two fan ExternalSensor stanzas in `x570d4i2t.json` provide the dbus targets.

The bridge also handles the host-power-cycle case — the chip is powered from
a host rail, vanishes in S5, and the device probe `waiting_for_supplier` until
host comes back. The daemon re-binds the i2c client on each poll cycle.

## Supermicro PWS-505P-1H PSU is non-standard PMBus

The PSU at **i2c-2 0x38** responds to address probes but rejects standard
PMBus identification. CAPABILITY returns 0xa0 on first read, 0x08 on second.
PMBUS_REVISION = 0x01 (older than spec). Block reads with auto-incrementing
internal pointer — data shifts by one byte across consecutive transactions.
Linux generic `pmbus` driver probe fails:

```
pmbus 2-0038: Failed to identify chip capabilities
```

Stock MegaRAC could read PSU "Fan 1"/"Fan 2"/Vin/Iin/Pin via proprietary
sequences. We cannot, without writing a custom shell daemon analogous to the
NCT6779 bridge. **The pmbus stanza in x570d4i2t.json is benign but
instantiate-fails** — leaves a "Failed to instantiate 'pmbus' at address '56'"
line in psusensor logs per restart. Decision: leave it in for future
custom-driver work; document the limitation.

## SBRMI driver fails probe — chip ACKs presence but NAKs alert-enable write

DT-instantiated `sbrmi@3c` on i2c-2 (moved from i2c-1 — the host's NCT6779 is
the actual i2c-1:0x3c at S0). After host POST, `i2cdetect -y -r 2` shows 0x3c
respond. Driver probe fails because `sbrmi_enable_alert()` writes to reg 0x40
and gets NAKed. Manual rebind also fails (`-EBUSY`/`-EIO`).

Status: real CPU temp via SBRMI is unavailable without a driver patch.
Options not yet tried: shim driver that skips alert-enable, or shell daemon
reading SBRMI via raw i2c (it ACKs presence ping).

## i2c-6 0x60 = chipset-internal device (likely AMD FCH)

After enabling i2c-6 for diagnostic probing, found a device at 0x60 with a
sparse register map (responses at 0x0–0x8, 0x10, 0x20, 0x40, 0x50, 0x60,
0xb0, 0xd0, 0xe0, 0xf0). Not a standard SMBus sensor. Most likely an AMD FCH
SMBus controller register space exposed by routing. Phantom-ACKs at 0x0c /
0x28 / 0x37 are SMBus protocol artifacts, not real devices. No further
investigation warranted.

## Physical fan control still not working from BMC

User has water cooler + multiple chassis fans physically installed.
Setting AST2500 PWM=255 via `/sys/class/hwmon/hwmon1/pwm*` does not change
fan speed. Hypotheses tested:
- PWM polarity inversion → no change
- All 8 AST2500 PWMs exposed in DTS instead of 3 → no change
- Direct register writes via nct6775-i2c → accepted but no physical effect

**Likely cause**: the X570D4I-2T board routes FAN1/FAN2/FAN3 PWM through
SuperIO (which the host BIOS owns via LPC), not the BMC. The MB manual
mentions a `PWM_CFG1` 3-pin header — investigated, this is an SMBus header
(SMB_DATA_VSB / SMB_CLK_VSB / GND) for "PWM configurations" externally, NOT
a BMC↔SuperIO source-select jumper. So the routing is fixed in hardware:
BMC's PWM lines may not physically reach the fan headers.

Until the routing is understood at the schematic level (we don't have access
to ASRock Rack schematics), do not advertise "BMC fan control" as a working
feature. Document as known limitation. PID/Stepwise zone is still configured
in entity-manager so it would Just Work if the routing were fixed.

## Sensor daemons need restart after live EntityManager restart

When iterating during dev: after `systemctl restart xyz.openbmc_project.EntityManager`,
the Type=dbus sensor daemons (adcsensor, hwmontempsensor, externalsensor,
psusensor) exit on bus disconnect and don't always come back automatically.

```sh
systemctl restart xyz.openbmc_project.{adcsensor,hwmontempsensor,externalsensor,psusensor}.service
```

Not needed at boot — auto-start works fine then.

## Stock MegaRAC "Fan 1" / "Fan 2" sensors = PSU fans

Confirmed via the ASRock BMC manual: the "Fan 1" and "Fan 2" sensors visible
in the stock MegaRAC UI are **PSU fans, sourced via PMBus**, not chassis
fans. Once a custom PSU daemon exists, those should re-appear at parity with
stock. Chassis fans (FAN1/2/3 headers) are the AST2500-side tachs, separate.

## Confirmed via vendor docs (2026-06-01)

- [Motherboard manual (PDF)](https://download.asrock.com/Manual/X570D4I-2T.pdf):
  fan header types, PWM_CFG1 pinout, board layout
- [BMC manual (PDF)](https://download.asrock.com/Manual/BMC/X570D4I-2T.pdf):
  Fan 1/Fan 2 = PSU fans, PSU must be PMBus-capable

## Status snapshot — what's exposed in Redfish

23 sensors in `/redfish/v1/Chassis/ASRockRack_X570D4I_2T_Baseboard/Sensors`:

| Source | Count | Sensors |
|---|---|---|
| AST2500 iio_hwmon ADCs | 13 | 3VSB, 5VSB, VCPU, VSOC, VCCM, APU_VDDP, PM_VDD_CLDO, PM_VDDCR_S5, PM_VDDCR, BAT, 3V, 5V, 12V |
| W83773G hwmon | 2 | MB Temp, Card Side Temp |
| NCT6779 bridge (ExternalSensor) | 8 | SYSTIN, CPUTIN, AUXTIN, X570_Temp, PCH_CPU_Temp, PCH_MCH_Temp, SuperIO_Fan_1, SuperIO_Fan_2 |
| Total | 23 | (CPU_Temp from SBRMI broken; PSU sensors require custom daemon) |
