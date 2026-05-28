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
| Linux DTS `aspeed-bmc-asrock-x570d4i2t.dts` | ❌ does not exist — `x570d4i2t.conf` references the DTB name but no DTS upstream |
| `power-config-host0.json` GPIO `LineName` fields | ⚠️ mostly empty (carryover from `meta-x570d4u`) — must be filled to match DTS |
| Sensor IC kernel driver selection | ⚠️ `x570d4i2t.cfg` enables `CONFIG_SENSORS_NCT6775_I2C=y` — needs verification against actual chip |
| `phosphor-pid-control` fan tuning | ❌ not defined — three fan headers, default curve captured |
| External SPI flash + first boot | ❌ not yet attempted |

## Provided references

- [meta-asrock/meta-x570d4i2t/](../../meta-asrock/meta-x570d4i2t/) — Yocto layer scaffold
- [meta-asrock/meta-x570d4u/](../../meta-asrock/meta-x570d4u/) — closest upstream-supported sibling board
- [../X570D4I-2T.pdf](../X570D4I-2T.pdf) — board user manual (1.9 MB)
- [../10.0.80.1_Archive [26-05-28 06-16-54].har](../10.0.80.1_Archive%20%5B26-05-28%2006-16-54%5D.har) — stock BIOS Redfish HAR (BIOS Setup page only — limited content)

## Suggested execution order

1. **Discovery** — pull I2C bus / GPIO line / SPI flash layout from the live BMC, since neither the meta-layer nor the upstream `aspeed-bmc-asrock-x570d4u.dts` documents every value. See [02-bmc-discovery.md](02-bmc-discovery.md).
2. **DTS** — copy `aspeed-bmc-asrock-x570d4u.dts` from the upstream Linux tree, apply the X570D4I-2T deltas, drop it in the meta-layer as a kernel patch. See [03-device-tree.md](03-device-tree.md).
3. **Layer config** — fill in `power-config-host0.json` LineNames, verify kernel `.cfg`, define fan PID config. See [04-meta-layer.md](04-meta-layer.md).
4. **Build** — `bitbake obmc-phosphor-image` against `MACHINE=x570d4i2t`.
5. **Backup + flash** — external SPI programmer; extract stock MACs from the dump before writing OpenBMC. See [05-flash-and-verify.md](05-flash-and-verify.md).
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
