# Post-flash discoveries — X570D4I-2T

What the live OpenBMC build revealed after we got past first-boot. Many
assumptions baked into [01-hardware-inventory.md](01-hardware-inventory.md)
turned out to be wrong or incomplete; this doc captures the deltas.

## NCT6779 SuperIO is the real fan/temp chip — not just W83773G

Stock MegaRAC's "X570 Temp", "SYSTIN", "CPUTIN", and similar host-rail sensors
are sourced from a **Nuvoton NCT6779D-family SuperIO at i2c-1 0x2d**. Not the
W83773G. The W83773G at 0x4c only provides CPU Temp + Card Side Temp (its
remote-2 diode RDOS2 is open on this board, so the "X570 Temp" we tried to
read off it always shows 0).

NCT6779 hwmon index → label (verified at runtime via `temp*_label` files):

| Index | Label | Notes |
|---|---|---|
| temp1 | SYSTIN | works |
| temp2 | CPUTIN | reads saturated 97.5°C — likely unwired (real CPU temp = W83773G @ 0x4C; SBRMI is dead, see below) |
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

## SBRMI / APML — DEAD END, REMOVED (2026-06)

DT-instantiated `sbrmi@3c` on i2c-2. After host POST, `i2cdetect -y -r 2` shows
0x3c respond and reg 0x20 reads 0x17, but the driver probe (`drivers/misc/amd-sbi`)
fails `-EIO`. Re-investigated with the host fully powered on:

+ The SB-RMI `CTRL` (0x01) and `STATUS` (0x02) registers **NAK reads**, and
  **all writes NAK** — so `sbrmi_enable_alert()` and the MP1 power mailbox
  (`READ_PKG_PWR_CONSUMPTION` / `WRITE_PKG_PWR_LIMIT`) cannot run. A raw i2c
  replay of the mailbox sequence NAKs at the first inbound-message write.
+ The **stock OEM firmware ships `SUPPORT_APML_IFC=0`**
  (`.../defconfig/BMC1/1U2-X570/2T/IPMI.conf`) — the OEM disabled APML, and the
  stock SDR has no CPU-power sensor either.
+ Root cause: full SB-RMI power telemetry/cap is an EPYC SP3/SP5 feature; the
  AM4/X570 SMU does not expose it to the BMC, and no BIOS module enables it
  (zero `apml`/`sbrmi` strings across the whole AMI firmware).

Decision: **removed** the `sbrmi@3c` DTS node, `CONFIG_AMD_SBRMI_*`, and the
(wrong-bus) SBRMI master-write-read whitelist entry. CPU Temp comes from the
W83773G @ i2c1 0x4C, not SB-RMI. There is **no CPU-power / power-cap source** on
this board (PSU is non-standard PMBus the OEM never read; ADCs are voltage-only).

## i2c-6 0x60 = chipset-internal device (likely AMD FCH)

After enabling i2c-6 for diagnostic probing, found a device at 0x60 with a
sparse register map (responses at 0x0–0x8, 0x10, 0x20, 0x40, 0x50, 0x60,
0xb0, 0xd0, 0xe0, 0xf0). Not a standard SMBus sensor. Most likely an AMD FCH
SMBus controller register space exposed by routing. Phantom-ACKs at 0x0c /
0x28 / 0x37 are SMBus protocol artifacts, not real devices. No further
investigation warranted.

## BMC fan control — phosphor-pid-control, TWO zones (CPU + Chassis)

User asked specifically for BMC-side fan control via phosphor-fan-control.
OpenBMC has two distinct stacks; we picked **`phosphor-pid-control`** (swampd,
entity-manager-driven) over `phosphor-fan-presence` (older YAML-config
IBM-Witherspoon stack) — simpler, single daemon, and the upstream X570D4U
sibling uses the same.

### Board-specific fan topology (user-confirmed)

- **FAN1** → AIO water-cooler radiator fan, **cools the CPU**. Controlled by `CPU Temp` (W83773G local diode, tracks CPU-area board temp on this board).
- **FAN2** → chassis fan splitter (multiple case fans, one PWM signal), **cools the chassis / X570 chipset**. Controlled by `X570 Temp` (W83773G remote-1 diode).
- **FAN3** → unplugged on this build. Left in entity-manager inventory but not driven by any zone; PWM 3 untouched.

### Two zones, two curves

Stepwise is **step-floor** (no interpolation): at temp T, output = O[ max i where R[i] ≤ T ]. So a 75/85 step pair leaves a 10°C "dead zone" at the same PWM. Curves use finer steps to avoid that.

**Zone 0 "CPU Zone"** — `CPU Temp` → `Pwm_1` (AIO radiator):

| Reading °C | 30 | 40 | 50 | 60 | 70 | 80 | 85 | 90 |
|---|---|---|---|---|---|---|---|---|
| Output % | 30 | 40 | 50 | 65 | 80 | 95 | 100 | 100 |

**Zone 1 "Chassis Zone"** — `X570 Temp` → `Pwm_2` (chassis fans):

| Reading °C | 30 | 40 | 50 | 60 | 65 | 70 | 75 | 80 |
|---|---|---|---|---|---|---|---|---|
| Output % | 30 | 40 | 55 | 70 | 80 | 90 | 100 | 100 |

NegativeHysteresis = 2°C on both (PWM steps down only when temp drops 2°C below the threshold — avoids fan-speed thrashing).

The chassis curve is more aggressive because the X570 chipset runs hot and the W83773G's remote diode is what we have visibility into. The CPU curve allows lower noise at idle since the AIO has thermal mass.

### Config artifacts

- `phosphor-pid-control` added to `packagegroup-asrock-apps` RDEPENDS so swampd ships in the image
- Two `"Type": "Pid.Zone"` entries (CPU Zone idx 0, Chassis Zone idx 1)
- Two `"Type": "Stepwise"` curves
- Two `"Type": "Pid"` Class:"fan" entries (CPU Fans → Pwm_1, Chassis Fans → Pwm_2), FFGain 1.0 / all other coeffs 0 (pass-through)
- 3 `"Type": "AspeedFan"` (FAN1/2/3) — Connector.Pwm 0/1/2

### Verified end-to-end (2026-06-01)

With X570 Temp = 78°C and CPU Temp = 63°C:

- swampd: `Zone 0 fans, returning to normal mode, output pwm: 65` (CPU)
- swampd: `Zone 1 fans, returning to normal mode, output pwm: 100` (Chassis)
- `/sys/class/hwmon/hwmon1/pwm1 = 165` (= 65% × 255)
- `/sys/class/hwmon/hwmon1/pwm2 = 255` (= 100% × 255)
- `/sys/class/hwmon/hwmon1/pwm3 = 255` (stale, not driven by any zone)
- Over 3 min, chassis dropped 0.75°C while CPU held steady at 62°C

### swampd "fan" Pid hard-requires tach feedback — fake-tach workaround

phosphor-pid-control's Class:"fan" controller refuses to leave failsafe
(PWM=100%) unless every Input tach sensor publishes a valid reading. The
`MissingIsAcceptable` and `InputUnavailableAsFailed` options are **silently
ignored** for "fan" class (dispatcher checks `pidClass != "fan"` before
applying them — `dbus/dbusconfiguration.cpp:786`).

This board's chassis fan headers (FAN1/2/3) don't seem to wire tach back to
AST2500 — `cat /sys/class/hwmon/hwmon1/fan1_input` returns "Connection timed
out". So the natural fan-tach Inputs all read NaN and swampd stays in
failsafe at PWM=255 forever.

**Workaround**: three `ExternalSensor` stanzas (`FanTach1`, `FanTach2`,
`FanTach3`, Units RPMS, PowerState Always) + extend the nct6779-bridge
daemon to `busctl set-property` a constant `1500.0` to each every 5 seconds.
The Pid "fan" Inputs point at these synthetic tachs. Setpoint is purely
feed-forward (FFGain=1, no I/D/P) so the constant value doesn't perturb the
math — it just satisfies swampd's "must have readings" gate.

```sh
publish_fake_tachs() {
    for n in 1 2 3; do
        busctl set-property xyz.openbmc_project.ExternalSensor \
            "/xyz/openbmc_project/sensors/fan_tach/FanTach$n" \
            xyz.openbmc_project.Sensor.Value Value d 1500.0
    done
}
```

The `FanTach1..3` objects show up in Redfish as fan sensors at 1500 RPM,
which is misleading. The clean fix is a swampd source patch that respects
MissingIsAcceptable on "fan" class — until then, the workaround is in
place.

## bmcweb HTTP body limit — 30 MB default chokes our 67 MB BMC tarball

The Redfish HttpPushUri (`/redfish/v1/UpdateService/update`) and
MultipartHttpPushUri (`/redfish/v1/UpdateService/update-multipart`) flows
both go through bmcweb, which has a meson `http-body-limit` option defaulting
to **30 MB**. Our BMC image .all.tar is 67 MB, so the upload is silently
truncated. phosphor-software-manager pipes the body to `tar -x -f -` and
sees "invalid tar magic" because the body is incomplete.

Fix: `meta-asrock/meta-x570d4i2t/recipes-phosphor/interfaces/bmcweb_%.bbappend`
sets `-Dhttp-body-limit=512` (max upstream value). Chicken-and-egg: applying
the fix requires a successful flash, but the broken HttpPushUri prevents that.

**Dev-flash shortcut**: `scp` the .all.tar to `/tmp/update.tar` on the BMC,
extract it (`tar x` to /tmp/extract/), then `flashcp -v
/tmp/extract/image-bmc /dev/mtd0`. flashcp will report a verification
mismatch ~58 MB into the 64 MB image — that's inside the jffs2 rwfs region
(mtd5), which is being actively written by the running OS. The mismatch is
harmless: u-boot / u-boot-env / kernel / rofs (mtd1-4, the first 42 MB) are
written and verified correctly. Reboot picks up the new firmware.

After bmcweb itself is updated with the larger limit, Redfish UpdateService
becomes usable again.

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
| W83773G hwmon | 2 | CPU Temp, X570 Temp |
| NCT6779 bridge (ExternalSensor) | 8 | SYSTIN, CPUTIN, AUXTIN, X570_Temp, PCH_CPU_Temp, PCH_MCH_Temp, SuperIO_Fan_1, SuperIO_Fan_2 |
| Total | 23 | (CPU_Temp is the W83773G, not SBRMI — SB-RMI removed as unusable; no PSU power source) |
