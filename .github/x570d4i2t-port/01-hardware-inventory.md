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
