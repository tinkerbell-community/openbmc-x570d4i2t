# X570D4I-2T hardware inventory (discovered from live BMC)

Reference doc — every fact below was pulled from the live MegaRAC BMC at
`10.0.80.1` (admin / `mrYsg79V*7ex!wt2`) on 2026-05-28. Reproduce any of it with
the commands in [02-bmc-discovery.md](02-bmc-discovery.md).

## Identity

| Item | Value | Source |
|---|---|---|
| Board model | X570D4I-2T | FRU `Board.ProductName` |
| Board mfg | ASRockRack | FRU `Board.Manufacturer` |
| Board serial | `BR80H6008900252` | FRU `Board.SerialNumber` |
| Board mfg date | 2024-12-18 18:49 UTC | FRU `Board.date` |
| System UUID | `7533E379-96C4-49D9-F452-9C6B0070598A` | `Systems/Self.UUID` |
| BMC/Manager UUID | `02ab11b21dd22608dd1e8e00fd861f45` | `Manager.UUID` (== SMBIOS GUID / FRU product UUID extra) |

## Firmware versions on the running system

| Component | Version | Notes |
|---|---|---|
| BMC (MegaRAC) | **1.91.00** | Built 2025-05-02 10:11 CST — being replaced |
| BIOS (AMI) | **P2.50** | Signed; web flash blocked by signature check |
| AMD PSP | 0.14.0.36 | Platform Security Processor |
| CPU microcode | `0a20120a` | Zen 3 Vermeer |
| ME | N/A | Not present on AMD |
| CPLD | N/A | Not present on this board |

## SoC and BMC interfaces

- **AST2500** (ASPEED, ARM11 BMC SoC)
- **32 MB SPI flash** (FLASH_SIZE = 65536 sectors × 512 B in `x570d4i2t.conf`)
- **IPMI manufacturer ID**: `0x00C1D6` (`mfg_id_0..2 = 214, 193, 0`) — ASRock Rack IANA Enterprise Number
- **IPMI product ID**: `0x1003`
- **IPMI device ID**: `0x20`, revision `0x81`
- **U-Boot config**: `evb-ast2500_defconfig` (per machine `.conf`)

## CPU / memory / chipset

- **Socket**: AMD AM4 (Ryzen 5000-series capable per BIOS P2.50)
- **Currently installed CPU**: AMD Ryzen 9 5900X — 12C/24T, max 4950 MHz (`Systems/Self/Processors/DevType1_CPU0`)
- **Chipset**: AMD X570 (sensor `X570 Temp` exposes the SB die)
- **DIMM slots**: 4 × DDR4 (ECC UDIMM)
  | Slot Id | DT-style name | DeviceLocator (DMI) |
  |---|---|---|
  | `DevType2_DIMM0` | `DDR4_A2` | DIMM 0 |
  | `DevType2_DIMM1` | `DDR4_A1` | DIMM 1 |
  | `DevType2_DIMM2` | `DDR4_B2` | DIMM 0 (channel B) |
  | `DevType2_DIMM3` | `DDR4_B1` | DIMM 1 (channel B) |
- **Memory currently installed**: 4 × 32 GB Teamgroup `SD4-3200` running at 2400 MT/s (sub-rated, likely JEDEC fallback) — 128 GB total

## Network topology

This is the largest single source of confusion vs. the X570D4U (which has a
different MAC fan-out). All values verified from `/api/settings/network` and
`/api/settings/network-link`.

### BMC-side interfaces

| iface | Channel | MAC | Role | Status |
|---|---|---|---|---|
| `eth0` | 1 | `9C:6B:00:4E:1C:2A` | Dedicated RJ45 mgmt port (Realtek RTL8211E, 1 GbE PHY) | Active, DHCP, link 1000/FULL |
| `eth1` | 8 | `9C:6B:00:70:57:A4` | NC-SI sideband through Intel X550-AT2 | Disabled by default, link 10/HALF when idle |
| `bond0` | — | — | NIC-teaming overlay (eth0+eth1) | Supported, disabled |
| `usb0` | — | `9A:01:77:6F:BA:40` | Host-BMC USB-NCM tunnel | `169.254.0.17/16` |

### Host-side NICs

- 2 × Intel **X550-AT2** 10GbE (PCIe `00:24.0`, vendor `8086:1563`, firmware `1.1927.0`)
- Asset-tag string on the chip: `X550_3_4`
- BIOS attrs that control them:
  | Attribute | Display name | Default |
  |---|---|---|
  | `CHIPSET005` | Onboard X550 LAN1 | Enabled |
  | `BFOL000` / `BFOL001` | Boot From Onboard LAN(X550) | Disabled |

### Host USB-NCM endpoint

- Host MAC: `02:DC:A4:B5:22:55` (locally administered)
- Host IP: `169.254.0.18/16`
- Visible in host SMBIOS as a virtual NIC at `PciRoot(0x0)/Pci(0x1,0x2)/Pci(0x0,0x0)/Pci(0x8,0x0)/Pci(0x0,0x3)/USB(0x4,0x0)/USB(0x2,0x1)`

### Implication for OpenBMC port

- DTS must mux MAC1 (AST2500) to **RGMII → Realtek RTL8211E** (dedicated PHY)
- DTS must mux MAC2 (AST2500) to **NC-SI** mode for the X550 sideband
- `phosphor-network` channel config: channel 1 = eth0 (dedicated), channel 8 = eth1 (NC-SI)
- **MAC addresses are persistent identifiers** — must be preserved across the stock-to-OpenBMC flash (see [05-flash-and-verify.md](05-flash-and-verify.md))

## Sensor inventory (45 IPMI SDR entries)

Pulled from `GET /api/sensors`. Sensor numbers are stable IPMI SDR indices.

### Voltages (V)

| # | Name | Reading | LCrit | UCrit |
|---|---|---|---|---|
| 1 | 3VSB | 3.36 | 3.04 | 3.70 |
| 2 | 5VSB | 5.07 | 4.50 | 5.49 |
| 3 | VCPU | 0.97 | — | 1.65 |
| 4 | VSOC | 0.97 | 0.36 | 1.54 |
| 5 | VCCM | 1.20 | 1.08 | 1.32 |
| 6 | APU_VDDP | 0.94 | 0.81 | 1.16 |
| 7 | PM_VDD_CLDO | 1.20 | 1.08 | 1.32 |
| 8 | PM_VDDCR_S5 | 1.03 | 0.89 | 1.16 |
| 9 | PM_VDDCR | 1.00 | 0.89 | 1.16 |
| 10 | BAT | 2.90 | 2.70 | 3.40 |
| 11 | 3V | 3.36 | 2.98 | 3.62 |
| 12 | 5V | 5.10 | 4.50 | 5.49 |
| 13 | 12V | 12.00 | 10.80 | 13.20 |
| 24 | PSU1 VIN | 0 | — | — |
| 25 | PSU2 VIN | 0 | — | — |

### Currents / Power (A / W)

| # | Name | Type |
|---|---|---|
| 32 | PSU1 IOUT | current |
| 33 | PSU2 IOUT | current |
| 112 | PSU1 PIN | power |
| 113 | PSU2 PIN | power |
| 116 | PSU1 POUT | power |
| 117 | PSU2 POUT | power |

### Temperatures (°C)

| # | Name | Idle reading | UCrit | UNonCrit |
|---|---|---|---|---|
| 49 | MB Temp | 43 | — | 65 |
| 50 | Card Side Temp | 38 | — | 68 |
| 52 | CPU Temp | 51 | 95 | 94 |
| 55 | Sys In Temp | 41 | — | 65 |
| 59 | X570 Temp | 63 | 95 | 94 |
| 60 | TR1 | — (absent) | — | 55 |
| 64 | DDR4_A2_Temp | — (no DIMM TS) | 85 | 84 |
| 65 | DDR4_A1_Temp | — | 85 | 84 |
| 66 | DDR4_B2_Temp | — | 85 | 84 |
| 67 | DDR4_B1_Temp | — | 85 | 84 |
| 92 | PSU1 Temp | — | — | — |
| 93 | PSU2 Temp | — | — | — |

### Fans (RPM)

| # | Name | Wired |
|---|---|---|
| 96 | FAN1 | yes |
| 98 | FAN2 | yes |
| 99 | FAN3 | yes |

`/api/asrr/settings/getsupportfan` confirms `tscft_support_fan1..3 = 1`, all
others = 0. No physical FAN4-FAN16.

### Discrete

| # | Name | Type |
|---|---|---|
| 144 | ChassisIntr | Physical Security |
| 145 | CPU_PROCHOT | Processor |
| 147 | CPU_THERMTRIP | Processor |
| 160 | PSU1 Status | Power Supply |
| 161 | PSU1 AC lost | Power Supply |
| 168 | PSU2 Status | Power Supply |
| 169 | PSU2 AC lost | Power Supply |
| 249 | WATCHDOG2 | Watchdog 2 |
| 250 | PowerUnit | Power Unit |

### Temperature (additional NVMe slot)

| # | Name | Source |
|---|---|---|
| 56 | NVME HDD | NVMe MI sideband over SMBus, via PCA9545 mux on i2c4 (M.2 slot) |

## PCIe topology (from BIOS device-path templates)

Extracted from the AMI BIOS P2.50 image — these are the actual device-path
templates the BIOS hardcodes for boot enumeration. See
[07-bios-internals.md](07-bios-internals.md) for the full BIOS-internals
mining writeup.

### CPU PCIe roots (24 Gen4 lanes from Ryzen)

- `Pci(0x1,0x1)` → NVMe target → likely **M2_1** (first M.2 socket, "Slot 2" in BIOS strings)
- `Pci(0x1,0x6)` → NVMe target → likely **M2_2** (second M.2 socket, "Slot 3" in BIOS strings)
- `Pci(0x2,0x1)` → NVMe target → an additional slot (PCIE7 or one of the OCuLinks in PCIe mode)
- `Pci(0x1,0x2)` → **CPU↔X570 chipset uplink** (also where the BMC's USB-NCM endpoint shows up in SMBIOS via `.../Pci(0x8,0x0)/Pci(0x0,0x3)/USB(0x4,0x0)/USB(0x2,0x1)`)

### Through the X570 chipset (downstream of `Pci(0x1,0x2)`)

- `Pci(0x9,0x0)/Pci(0x0,0x0)` — FCH primary AHCI: SATA ports 0..7
- `Pci(0x8,0x2)/Pci(0x0,0x0)` — FCH secondary AHCI: OCU1 in SATA mode
- `Pci(0x8,0x3)/Pci(0x0,0x0)` — FCH tertiary AHCI: OCU2 in SATA mode
- `Pci(0x8,0x0)/Pci(0x0,0x3)` — chipset xHCI controller (4 USB root ports)
- `Pci(0x14,0x1)` — FCH SMBus host (host-side, not BMC-visible)

### BIOS-controlled slot routing

| BIOS attr | Setting | Default | Effect |
|---|---|---|---|
| `CHIPSET007` | OCU1 Mode | PCIE | OCuLink #1 = NVMe (PCIe x4) |
| `CHIPSET008` | OCU2 Mode | PCIE | OCuLink #2 = NVMe (PCIe x4) |
| `CHIPSET005`/`006` | X550 LAN1/2 | Enabled | dual 10GbE active |
| `BFOL000`/`001` | Boot from X550 | Disabled | UEFI PXE off by default |

## Slot and connector inventory (from BIOS Setup IFR)

Extracted from the AMI BIOS P2.50 firmware's UEFI HII Setup JSON
(`file-110dc5d3-ed94-49c1-9f2d-13e129ba22f4/section0.raw`, ~205 KB, plaintext
JSON inside the AMI inner FFS volume). All slot designations below are the
*actual silkscreen / SMBIOS labels* used on the board.

### PCIe / M.2 / OCuLink

| Designation | Type | Lanes | Source | BIOS bifurcation control |
|---|---|---|---|---|
| `PCIE7` | PCIe x16 slot | x16 / x8 / x4×4 | CPU (Ryzen) | `PCIE7_1`..`PCIE7_4` — 4×4 split for AIC-NVMe |
| `M2_1` | M.2 socket | x4 PCIe **or** SATA | CPU/FCH (mode switch) | `M2_1 Slot OpROM` |
| `M2_2` | M.2 socket | x4 PCIe **or** SATA | CPU/FCH (mode switch) | `M2_2 Slot OpROM` |
| `OCU1` | OCuLink connector | x4 PCIe **or** 4× SATA | FCH | `OCU1 Mode Selection` (PCIE / SATA) |
| `OCU2` | OCuLink connector | x4 PCIe **or** 4× SATA | FCH | `OCU2 Mode Selection` (PCIE / SATA) |

> The two OCuLink connectors are a server-board feature — each can deliver
> either one NVMe drive (PCIe ×4) or up to four SATA drives. Combined with
> the FCH SATA controller, this yields the **8 SATA ports** (Port 0..7) seen
> in BIOS Setup. The `ESATA Port On Port N` options indicate every SATA port
> can be hot-plug enabled.

### Onboard networking

| Designation | Chip | BIOS attribute | Notes |
|---|---|---|---|
| `LAN1` | Intel X550-AT2 port 1 | `Onboard X550 LAN1` | 10GbE; PCIe asset tag `X550_3_4` (chipset lanes 3-4) |
| `LAN2` | Intel X550-AT2 port 2 | `Onboard X550 LAN2` | 10GbE; shares the X550-AT2 chip with LAN1 |
| `MGMT` | Realtek RTL8211E | n/a (BMC only) | dedicated 1GbE management PHY on BMC's mac0 (RGMII) |

The X550-AT2 is one physical chip with two SerDes interfaces. BMC NC-SI
sideband terminates at one of these X550 ports — selecting via
`/api/settings/ncsi/mode` chooses which.

### TPM

The TPM source is configurable via BIOS attribute `PSP001` (Setup display
"SPI/LPC/fTPM TPM switch"):

| Value | Mode | Use case |
|---|---|---|
| 0 | AMD CPU fTPM | firmware TPM via AMD PSP |
| 1 (default) | LPC TPM | discrete TPM module on LPC header |
| 2 | SPI TPM | discrete TPM module on SPI header |

The board therefore has both an **LPC TPM header** and an **SPI TPM header**.

### DIMM slots

`DDR4_A1`, `DDR4_A2`, `DDR4_B1`, `DDR4_B2` (2 DPC dual-channel UDIMM ECC).
Naming matches what the live BMC's Redfish memory inventory reported.

### Fan headers

The BIOS exposes **no** fan-related options — fan control is exclusively
BMC-managed via `pwmtach.ko`. The three populated headers are
`FAN1`, `FAN2`, `FAN3` (confirmed by `/api/asrr/settings/getsupportfan`).

### Serial / console

- COM1: `3F8h IRQ4` (SOL primary; BMC's `/dev/ttyS3` connects here via LPC VUART)
- COM2: `2F8h IRQ3`
- Debug Port 80h: snooped to BMC via LPC port 80

### Storage controller summary

- **AMD FCH AHCI**: up to 8 SATA ports, exposed when OCuLink connectors are in SATA mode
- **NVMe**: via M.2 slots (M2_1, M2_2) and OCuLink connectors (OCU1, OCU2) in PCIe mode
- **NVMe MI sideband**: routed to BMC via the PCA9545 mux on i2c4 → channel 1 (M.2 slot 1). The stock BMC reports it as SDR sensor #56 "NVME HDD".

### Stock BMC firmware mining (added 2026-05-28)

Extracting the stock BMC image (`X570D4I-2T_1.90.00.ima`, 64 MB) yielded:

- **`/etc/defconfig/BMC1/1U2-X570/2T/SDR.dat`** (2 KB) — 36 IPMI sensor records confirming every sensor name/number above, plus an additional `NVME HDD` temperature sensor (#56) that wasn't visible in the Redfish API
- **`/etc/defconfig/BMC1/1U2-X570/2T/IPMI.conf`** documents the I2C bus topology:
  - `PRIMARY_IPMB_I2C_BUS_NUM=8` (i2c-8)
  - `SECONDARY_IPMB_I2C_BUS_NUM=3` (i2c-3)
  - `SMBUS_BUS_NUM=6` (i2c-6 — general SMBus, host-shared)
  - `EEPROM_I2C_BUS_NUM=7` (i2c-7 — FRU + MAC EEPROM, matches X570D4U DTS)
  - `APML_BUS_NUMBER=1`, `APML2_BUS_NUMBER=1` (i2c-1 — AMD CPU SB-RMI/SB-TSI, shares bus with W83773G)
  - `SOL_IFC_PORT=/dev/ttyS3` (BMC's serial3 wired to host COM1 via LPC/VUART)
- **`/info/X570D4I-2T_K5.PRJ`** — AMI MegaRAC SPX 4.0 build manifest:
  - Kernel: 5.4.99-ami (uImage at IMA offset `0x1740040`, load `0x80008000`)
  - Platform identifier: `1U2-X570/2T`
  - Build date: Jun 9 2022
- **Stock DTB** decompiles to a generic AST2500EVB device tree with NO board-specific I2C clients, GPIO line names, or LED nodes. AMI keeps all board specifics in **userspace** (libasrrcmds.so), not in the kernel DTS. This validates the approach of authoring a fresh OpenBMC-style DTS rather than trying to re-use the stock DTB.
- **No `BMC_PCH_BIOS_CS_N` GPIO** is named anywhere in the firmware → in-band BIOS flash is done through the AMD PSP, not via BMC-mediated SPI mux. The `bios-update` hook used by ROMED8HM3 is **not applicable** to this board.
- **No I2C-based VRM regulator** is configured → all voltages are read via the AST2500's internal ADC. The `phosphor-power/config.json` regulator config used by ROMED8HM3 is **not applicable**.

## Fan control defaults (closed-loop = "Auto" / mode 1)

Captured from `/api/asrr/settings/getfanopenloopcontroltable`:

| Temp (°C) | Duty (%) |
|---|---|
| 30 | 20 |
| 60 | 30 |
| 70 | 40 |
| 80 | 50 |
| 90 | 60 |
| 100 | 100 |

Default mode bits (`/api/asrr/settings/getfancontrolmode`): mode1=1, mode2=1,
mode3=0 → fan1 and fan2 closed-loop, fan3 open-loop.

## I/O resource map (from BIOS attribute registry)

| Resource | Address / IRQ | BIOS attribute |
|---|---|---|
| COM1 (SOL) | `3F8h` / IRQ 4 | `SUPERIO001=3F8hIRQ4`, `SUPERIO002=true` |
| COM2 | `2F8h` / IRQ 3 | `SUPERIO004=2F8hIRQ3` |
| TPM mode | LPC TPM | `PSP001=LPCTPM` |
| Watchdog Timer | Auto | `HWMONITOR000=Auto` |
| Restore AC power loss | PowerOn | `CHIPSET003`, also `/api/asrr/GetPowerRestore` → `1` |
| BMC out-of-band addr source | DHCP | `IPMI523=DHCP` |
| IPv6 support | Enabled | `IPMI510` |
| Wait For BMC | Enabled | `IPMI601` |
| FRB-2 timer | Disabled | `IPMI100` |
| BMC KCS control via BIOS | No change | `BMCTOOL000` |

## POST snooping

- Mechanism: AST2500 LPC port-80 snoop with **2-byte capture** enabled
  (feature flag `BIOS_CODE_2BYTES` in `/api/configuration/project`)
- Current code endpoint: `GET /api/asrr/getbioscode` → `{poststatus:1, postdata:42240}`
- History endpoint: `GET /api/logs/postcode` → 803-entry circular buffer per boot
- AMD AGESA emits 16-bit extended codes:
  - High byte `0xE0`–`0xE4` = PEI phase events
  - High byte `0xA8`–`0xAA` = PEI→DXE transitions
  - Low byte (8-bit) values = standard AMI PEI / DXE checkpoints

## Power-on hours

- `/api/status/uptime` → `poh_counter_reading = 5471` × `minutes_per_count = 60`
- ≈ 228 days powered-on since manufacture

## Web/API capabilities exposed by stock firmware

309 API endpoints discovered in `source.min.js`. Notable namespaces:

| Prefix | Purpose |
|---|---|
| `/api/sensors` | SDR + readings |
| `/api/sdr` | bare SDR enumeration |
| `/api/asrr/getbioscode` | live POST code |
| `/api/asrr/settings/getfan*` | fan tables, modes, support map |
| `/api/asrr/maintenance/{BIOS,BP,CPLD,FPGA,PSU,RETIMER}/*` | flash slots |
| `/api/asrr/fw-info` | BMC/BIOS/PSP/microcode/CPLD versions |
| `/api/settings/network*` | LAN cfg, link, bonding, NCSI mode |
| `/api/logs/postcode` | full POST trace |
| `/api/configuration/project` | compile-time feature flags |
| `/api/maintenance/firmware*` | BMC firmware upload |

Full list in `/tmp/x570d4i2t-bmc/non_asrr_api.txt` and `asrr_api.txt` (ephemeral
— regenerate with `02-bmc-discovery.md` if needed).

## Sibling-board comparison (X570D4U vs X570D4I-2T)

Both boards share BMC SoC, chipset, and family. Known structural deltas:

| Item | X570D4U (Micro-ATX) | X570D4I-2T (Mini-ITX) |
|---|---|---|
| Form factor | uATX | Mini-ITX |
| Fan headers | typically 4–5 | **3** |
| Onboard 10G NICs | optional X550 | dual X550-AT2, standard |
| OCP / SFP+ | none | none |
| DIMM slots | 4 | 4 (same names) |
| BMC mgmt PHY | dedicated | dedicated (RTL8211E) |
| BIOS | P2.x | P2.50 |
| Sensor IC | likely NCT6779/NCT6796 family | unverified — assume same family until I2C probe confirms |

The `meta-x570d4u` layer is the closest already-working OpenBMC reference. The
upstream Linux DTS `aspeed-bmc-asrock-x570d4u.dts` is the starting point for the
X570D4I-2T DTS (see [03-device-tree.md](03-device-tree.md)).
