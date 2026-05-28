# X570D4I-2T BIOS P2.50 internals (mined from `X574I2T2.50` image)

The 32 MB host BIOS image at
`/home/appkins/Downloads/X570D4I-2T(2.50)ROM/X574I2T2.50` is a standard AMI
Aptio V BIOS in dual-image layout (two 16 MB mirrors). When parsed with
`uefi-firmware-parser`, it yields **10 firmware volumes** and **~4113 FFS
files**. This document captures everything that's useful for the OpenBMC port
or for understanding the platform from the BMC side.

## High-level identity

| Item | Value |
|---|---|
| Vendor | American Megatrends (Aptio V) |
| Setup vendor code | `1AWQE` |
| Build date | **2022-08-08, 16:11:44** |
| BIOS image size | 32 MB |
| Layout | Dual 16 MB image (primary + backup mirror) |
| AGESA | **AGESA!V9 ComboAM4v2PI 1.2.0.7** — supports Vermeer/Matisse/Cezanne/Renoir CPUs |
| Setup Attribute Registry | `BiosAttributeRegistry1AWQE.2.50.0` (matches what the BMC's Redfish exposes) |

## Firmware-volume map

| FV start | Length | Role (inferred) |
|---|---|---|
| `0x00037000` | `0x00020000` | NVRAM defaults volume (NVAR records) |
| `0x0069f000` | `0x00808000` | Main DXE volume — contains AGESA + AMITSE Setup |
| `0x006a00c0` | `0x00030000` | Inner FV (nested) |
| `0x00ea7000` | `0x00159000` | Secondary DXE volume |
| `0x01037000`+ | (mirror) | Backup copies of the above |

GUID for all 10 volumes: `8c8ce578-8a3d-4f1c-9935-896185c32dd3` (=
`EFI_FIRMWARE_FILE_SYSTEM2_GUID`).

## BIOS Setup variable inventory

The AMITSE Setup driver carries a JSON-format attribute registry at:
```
volume-28053504/.../file-110dc5d3-ed94-49c1-9f2d-13e129ba22f4/section0.raw
```
(~205 KB plaintext JSON). It declares:

- **15 NVRAM variables** (UEFI variables that persist across reboots)
- **9 BS Questions** (boot-services only — change requires reboot)
- **429 RT Questions** (runtime-settable Setup questions)
- ~316 unique questions after dedup, each carrying:
  `{ QuestionID, Prompt, Help, QuestionType, VarID, VarOffset, VarWidth }`

### NVRAM variable list

| VarID | Name | GUID | Notes |
|---|---|---|---|
| 0 | `PlatformLang` | `8BE4DF61-93CA-11D2-AA0D-00E098032B8C` | Default = `en-US` |
| 1 | **`Setup`** | `EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9` | **507 bytes** — the main Setup defaults blob |
| 2 | `PCI_COMMON` | `ACA9F304-21E2-4852-9875-7FF4881D67A5` | 8 bytes |
| 3 | `PNP0501_0_NV` | `560BF58A-1E0D-4D7E-953F-2980A261E031` | 3 bytes — COM1 settings |
| 4 | `PNP0501_1_NV` | `560BF58A-1E0D-4D7E-953F-2980A261E031` | 3 bytes — COM2 settings |
| 5 | `SioSetupData` | `6B0CC1BC-910F-411E-B6CB-0E314D0BB8C1` | 1 byte |
| 6 | `UsbSupport` | `EC87D643-EBA4-4BB5-A1E5-3F3E36B20DA9` | 49 bytes |
| 7 | `NetworkStackVar` | `D1405D16-7AFC-4695-BB12-41459D3695A2` | 8 bytes — network stack defaults |
| 8 | `AMITSESetup` | `C811FA38-42C8-4579-A9BB-60E94EDDFB34` | 65 bytes — Setup UI defaults |
| 9 | `SecureBootSetup` | `7B59104A-C00D-4158-87FF-F04D6396A915` | 7 bytes |
| 10 | `Timeout` | `8BE4DF61-93CA-11D2-AA0D-00E098032B8C` | 2 bytes — boot menu timeout |
| 11 | `BootOrder` | `8BE4DF61-93CA-11D2-AA0D-00E098032B8C` | Variable length |
| 12 | `RefreshAttribRegistry` | `8E31482A-72EA-4E08-AE30-232472BE3DD9` | 1 byte |
| 13 | `Setup` | `80E1202E-2697-4264-9CC9-80762C3E5863` | Alternative Setup |
| 14 | **`ServerSetup`** | `01239999-FC0E-4B6E-9E79-D54D5DB6CD20` | **740 bytes** — BMC/IPMI config |

### Setup defaults extraction

The NVAR records at BIOS offset `0x37090` carry default values for every
Setup variable. The parsed table for VarID=1 (`Setup`) and VarID=14
(`ServerSetup`) is preserved at `/tmp/bios/setup-defaults.txt` (316 lines).
Notable defaults that influence BMC behavior:

| QuestionID | Setup display name | Default | Notes |
|---|---|---|---|
| `CHIPSET003` | Restore AC Power Loss | `No change` | matches `/api/asrr/GetPowerRestore=1` |
| `CHIPSET005` | Onboard X550 LAN1 | `Enabled` | |
| `CHIPSET006` | Onboard X550 LAN2 | `Enabled` | |
| `CHIPSET007` | OCU1 Mode Selection | `PCIE` | OCuLink 1 in NVMe mode by default |
| `CHIPSET008` | OCU2 Mode Selection | `PCIE` | OCuLink 2 in NVMe mode by default |
| `CHIPSET009` | SATA Mode | `AHCI` | |
| `BFOL000`/`BFOL001` | Boot From Onboard LAN(X550) | `Disabled` | |
| `HWMONITOR000` | Watch Dog Timer | `Auto` | |
| `PSP001` | SPI/LPC/fTPM TPM switch | `LPC TPM` | |
| `PSP002` | Erase fTPM NV for factory reset | `Enabled` | |
| `IPMI600` | BMC Support | `Enabled` | |
| `IPMI601` | Wait For BMC | `Enabled` | |
| `IPMI510` | IPV6 Support | `Enabled` | |
| `IPMI523` | IPMI LAN configuration address source | `DHCP` | matches what we saw on the live BMC |
| `IPMI520` | BMC Out of band Access | `No Change` | |
| `IPMI522` | Manual setting IPMI LAN | `No` | |
| `TCG001` | TPM State | `Enabled` | |
| `TCG010` | TPM 2.0 UEFI Spec Version | `TCG_2` | TCG 2.0 |
| `TCG014` | TPM 2.0 InterfaceType | `CRB` | Command-Response Buffer |
| `GNB004` | PSPP Policy (PCIe Speed Power Policy) | `Auto` | |

## PCIe topology (extracted device-path templates)

The BIOS hardcodes several PCIe device paths for slot/device enumeration. The
following are the actual paths embedded in the BIOS firmware:

### CPU-direct PCIe roots (Ryzen has 24 PCIe Gen4 lanes)

```
PciRoot(0x0)/Pci(0x1,0x1)/Pci(0x0,0x0)/NVMe(0x1,...)        ← CPU root 1 → M.2 / PCIe slot
PciRoot(0x0)/Pci(0x1,0x6)/Pci(0x0,0x0)/NVMe(0x1,...)        ← CPU root 6 → second M.2 / PCIe slot
PciRoot(0x0)/Pci(0x2,0x1)/Pci(0x0,0x0)/NVMe(0x1,...)        ← CPU root from second group → OCuLink or M.2
```

### Chipset uplink and downstream

`Pci(0x1,0x2)` is the **CPU→X570 chipset uplink** (matches the
`UefiDevicePath` we saw for the BMC's USB-NCM endpoint earlier:
`PciRoot(0x0)/Pci(0x1,0x2)/Pci(0x0,0x0)/Pci(0x8,0x0)/Pci(0x0,0x3)/USB(0x4,0x0)/USB(0x2,0x1)`).

Downstream of the chipset:

```
.../Pci(0x8,0x0)/Pci(0x0,0x3)/USB(0x0..0x3,0x0)             ← USB ports (xHCI #1, 4 root ports)
.../Pci(0x9,0x0)/Pci(0x0,0x0)/Sata(0x0..0x7,0xFFFF,0x0)     ← FCH AHCI controller, 8 SATA ports
.../Pci(0xA,0x0)/Pci(0x0,0x0)                                ← additional chipset device
.../Pci(0x1,0x0)/Pci(0x0,0x0)/NVMe(0x1,...)                  ← NVMe through chipset (one of M.2/OCU)
```

### CPU/FCH internal

```
PciRoot(0x0)/Pci(0x7,0x1)/Pci(0x0,0x3)        ← FCH internal device
PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x2)        ← FCH GPP audio
PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x3)        ← FCH GPP USB
PciRoot(0x0)/Pci(0x8,0x1)/Pci(0x0,0x4)        ← FCH GPP additional
PciRoot(0x0)/Pci(0x8,0x2)/Pci(0x0,0x0)/Sata(0x1,...)        ← second SATA controller
PciRoot(0x0)/Pci(0x8,0x3)/Pci(0x0,0x0)/Sata(0x5,...)        ← third SATA controller (OCuLink-SATA)
PciRoot(0x0)/Pci(0x11,0x0)                    ← FCH PCIe root
PciRoot(0x0)/Pci(0x14,0x1)                    ← FCH SMBus / I2C controller
```

The combination of multiple SATA controllers (FCH primary AHCI + OCuLink-mode
controllers) is what allows the "8 SATA ports" total seen in BIOS Setup.

## Slot designators (silkscreen reference)

From cross-referencing BIOS strings with the PCIe path templates:

| BIOS designator | Silkscreen | Connector | Lanes | Notes |
|---|---|---|---|---|
| `PCIE7` | physical x16 slot | PCIe x16 mechanical | x16 / 4×4 bifurcation from CPU | has a PCIe redriver at `J3600` with components `U3601`/`U3602`/`U3603`/`U3608` |
| `M2_1` | M.2 Socket | M-Key (NVMe + SATA) | x4 from CPU | also called "Slot 2" — NVMe/SATA switch chip `J3705` |
| `M2_2` | M.2 Socket | M-Key (NVMe + SATA) | x4 from CPU | also called "Slot 3" — NVMe/SATA switch chip `J3706` |
| `OCU1` | OCuLink | NVMe (PCIe x4) or 4× SATA | x4 from FCH | mode-switchable |
| `OCU2` | OCuLink | NVMe (PCIe x4) or 4× SATA | x4 from FCH | mode-switchable |

The `X550_3_4` asset tag on the Intel X550-AT2 NIC (already seen via Redfish)
refers to **chipset GPP lanes 3-4** that the X550 uses.

## SATA controller layout

The X570D4I-2T has **three logical SATA controllers** that fan out to **8
SATA ports**:

1. FCH primary AHCI (`Pci(0x9,0x0)/Pci(0x0,0x0)`) — 8 ports (0-7), though
   physical wiring depends on OCuLink mode
2. FCH secondary AHCI (`Pci(0x8,0x2)/Pci(0x0,0x0)`) — used for OCU1 in SATA mode
3. FCH tertiary AHCI (`Pci(0x8,0x3)/Pci(0x0,0x0)`) — used for OCU2 in SATA mode

Switching `CHIPSET007/CHIPSET008` from `PCIE` to `SATA` re-routes the OCuLink
SerDes lanes from the chipset PCIe controller to its AHCI controller.

## Boot device-path catalogue

The AMI Boot Manager constructs `Boot####` UEFI variable entries from these
device path templates at runtime. Key boot-relevant templates the BIOS knows
about:

- `NVMe(0x%x,%02x-%02x-%02x-%02x-%02x-%02x-%02x-%02x)` — NVMe SSD (NSID + EUI64)
- `Sata(0x%x,0xFFFF,0x0)` — SATA drive (port number)
- `USB(0x%x,0x0)` — USB device (port number)
- Network boot via UEFI HTTP / PXE on the X550 NICs (LAN1 / LAN2) — exposed
  by the OpROMs configured via `BFOL000` / `BFOL001`

## Items NOT present in the BIOS (relevant to OpenBMC port)

The BIOS image **does not** expose:

- `BMC_PCH_BIOS_CS_N` GPIO line — confirms our prior conclusion that the
  X570D4I-2T does not have a BMC-controlled SPI mux. BIOS update is via
  the AMD PSP, not via the BMC.
- Any I2C-attached VRM regulator config — the platform's VRM is controlled
  by the AMD SVI2 bus from the CPU, not via I2C. BMC voltage monitoring
  uses the AST2500 ADC.
- Fan tach/PWM tables — fan control is the BMC's job (handled by AMI's
  `pwmtach.ko` and `libasrrcmds.so`); the BIOS has no fan options.

## Reproducing the extraction

If the BIOS image moves or you need to redo this:

```bash
# Install tooling
python3 -m venv /tmp/biostools
/tmp/biostools/bin/pip install uefi_firmware pikepdf

# Decompose UEFI volumes
/tmp/biostools/bin/uefi-firmware-parser -b -e -O -o /tmp/bios/extracted X574I2T2.50

# Setup JSON lives at (volume / file path varies by image):
find /tmp/bios -name 'section*.raw' -size +100k -exec grep -l '"Variables"' {} \;
```

The defaults blob is the NVAR record at BIOS offset `0x370a7` (name `Setup`,
size 524 bytes including header). Apply each Question's `VarOffset` + `VarWidth`
to extract its default value.
