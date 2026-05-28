# OpenBMC port to ASRock Rack X570D4I-2T — overview

Porting playbook for bringing OpenBMC up on the ASRock Rack X570D4I-2T — a
Mini-ITX AM4 server board with an ASPEED AST2500 BMC, AMD X570 chipset, dual
Intel X550-AT2 10GbE, and a dedicated Realtek RTL8211E management PHY.

This directory is the source of truth for the in-flight port. Pick up here when
continuing the work in a new agent session.

## Read order

| Doc | Purpose |
|---|---|
| [01-hardware-inventory.md](01-hardware-inventory.md) | What's physically on the board — sensors, NICs, fans, I/O resources (reference) |
| [02-bmc-discovery.md](02-bmc-discovery.md) | How to query the stock MegaRAC at 10.0.80.1 for more info (playbook) |
| [03-device-tree.md](03-device-tree.md) | Linux DTS implementation task |
| [04-meta-layer.md](04-meta-layer.md) | Yocto `meta-x570d4i2t` customization task |
| [05-flash-and-verify.md](05-flash-and-verify.md) | External SPI flash and first-boot validation task |

## Current status

| Item | State |
|---|---|
| `meta-asrock/meta-x570d4i2t/` layer scaffold | ✅ committed in `08cf84cb46` — verbatim of `meta-x570d4u` with names renamed |
| Live MegaRAC at 10.0.80.1 mapped | ✅ sensors, network, BIOS attrs, firmware all captured (see 01) |
| `bitbake` layer validation | ✅ full parse with `MACHINE=x570d4i2t` — 3108 `.bb` files, 0 errors, all 5 bbappends route to real recipes |
| Linux DTS `aspeed-bmc-asrock-x570d4i2t.dts` | ✅ authored, compiles via `dtc` to a 29 KB DTB; shipped as a kernel patch verified against `openbmc/linux dev-6.18` (the actual target branch — `linux-aspeed` recipe pulls this) |
| `power-config-host0.json` GPIO `LineName` fields | ✅ six required names populated (PostComplete, PowerButton, PowerOk, PowerOut, ResetButton, ResetOut); ID/NMI/SIO entries intentionally empty (no Intel SuperIO on AM4) |
| Sensor IC kernel driver selection | ✅ `CONFIG_SENSORS_W83773G=y` (matches X570D4U; chip is Nuvoton W83773G on i2c1:0x4c) |
| IPMI `dev_id.json` | ✅ populated with the live BMC's actual values (mfg_id 0x00C1D6, prod_id 0x1003, dev_id 0x20) |
| `obmc-console.conf` (SOL) | ✅ wired to COM1 (`lpc-address=0x3f8`, `sirq=4`) per BIOS `SUPERIO001`/`SUPERIO002` |
| `led-group-config.json` + bbappend | ✅ standard `bmc_booted` / `system_fault` groups; DTS uses legacy LED node names (`heartbeat`, `system-fault`) so the config targets them by name |
| AMD APML / SB-RMI support | ✅ DTS adds `sbrmi@3c` on i2c1; kernel `.cfg` enables `CONFIG_SENSORS_SBRMI=m` and `CONFIG_SENSORS_SBTSI=m`. Confirmed via stock AMI firmware's `IPMI.conf` (`APML_BUS_NUMBER=1`) — gives BMC direct CPU thermal/power telemetry |
| NVMe SMBus monitoring noted | ✅ stock SDR has a "NVME HDD" sensor (snum 56) confirming an M.2 slot wires the NVMe MI sideband through the PCA9545 mux on i2c4 channel 1 — comment added to DTS, full NVMe-MI subnode left as runtime add-on |
| `phosphor-pid-control` fan tuning | ⏳ deferred — none of the sibling ASRock OpenBMC layers ship one; dbus-sensors/auto-discovery via DTS hwmon nodes should cover basic fan control |
| `phosphor-power` regulator config | ❌ skipped — confirmed unnecessary. Stock BMC firmware has no VRM/regulator I2C config; voltages are read via the AST2500 internal ADC only |
| `bios-update` in-band hook | ❌ skipped — confirmed unnecessary. Stock BMC firmware has no `BMC_PCH_BIOS_CS_N` SPI-mux GPIO; the X570D4I-2T uses CPU PSP for in-band BIOS flash, not BMC-mediated SPI |
| External SPI flash + first boot | ❌ not yet attempted |

## Provided references

- [meta-asrock/meta-x570d4i2t/](../../meta-asrock/meta-x570d4i2t/) — Yocto layer scaffold
- [meta-asrock/meta-x570d4u/](../../meta-asrock/meta-x570d4u/) — closest upstream-supported sibling board
- [../X570D4I-2T.pdf](../X570D4I-2T.pdf) — board user manual (1.9 MB)
- [../10.0.80.1_Archive [26-05-28 06-16-54].har](../10.0.80.1_Archive%20%5B26-05-28%2006-16-54%5D.har) — stock BIOS Redfish HAR (BIOS Setup page only — limited content)

## Suggested execution order

1. ~~**Discovery** — pull I2C bus / GPIO line / SPI flash layout from the live BMC~~ ✅ done (X570D4U DTS provided all needed values; live MegaRAC SSH dropped into SMASHLITE with no shell escape)
2. ~~**DTS** — copy `aspeed-bmc-asrock-x570d4u.dts` from the upstream Linux tree, apply the X570D4I-2T deltas, drop it in the meta-layer as a kernel patch~~ ✅ done — at `meta-asrock/meta-x570d4i2t/recipes-kernel/linux/linux-aspeed/aspeed-bmc-asrock-x570d4i2t.dts`; patch verified against OpenBMC kernel fork
3. ~~**Layer config** — fill in `power-config-host0.json` LineNames, verify kernel `.cfg`~~ ✅ done. Optional pieces (PID fan tuning, regulator config, BIOS update hook) remain — see status table above
4. **Build** — `bitbake obmc-phosphor-image` against `MACHINE=x570d4i2t` ⏳ next user action
5. **Backup + flash** — external SPI programmer; MACs preserve themselves via the 24c128 EEPROM at i2c7:0x57 (no manual extraction needed thanks to nvmem-cells in the DTS). See [05-flash-and-verify.md](05-flash-and-verify.md).
6. **Validate** — compare sensor readings, POST snooping, NCSI bring-up against the captured baselines.

## Hard constraints already established

- **BMC SoC**: AST2500 (confirmed via Manager.Model `40992141930` + `ServiceEntryPointUUID` matching FRU)
- **BIOS**: AMI P2.50 (signed; OpenBMC web flash is NOT viable — needs external SPI)
- **Management PHY**: dedicated Realtek RTL8211E on BMC channel 1, MAC `9C:6B:00:4E:1C:2A`
- **NC-SI sideband**: BMC channel 8 → one of the Intel X550-AT2 ports, MAC `9C:6B:00:70:57:A4`
- **Both MACs must be preserved** during the stock→OpenBMC flash transition
- **Fan headers wired**: exactly 3 (FAN1, FAN2, FAN3) — declared by `/api/asrr/settings/getsupportfan`
- **DIMM slots**: 4 (DDR4_A1, DDR4_A2, DDR4_B1, DDR4_B2)
- **POST snooping**: AST2500 LPC port-80 with 2-byte (BIOS_CODE_2BYTES) capture — AMD AGESA emits 16-bit extended codes

## Conventions

- All file paths in this playbook are relative to the repo root: `/home/appkins/src/tinkerbell-community/openbmc/`
- BMC credentials throughout: `admin` / `mrYsg79V*7ex!wt2` (stock MegaRAC, user-supplied)
- Captured BMC artifacts during the initial sweep lived in `/tmp/x570d4i2t-bmc/` (ephemeral). [02-bmc-discovery.md](02-bmc-discovery.md) shows how to regenerate them on demand.
