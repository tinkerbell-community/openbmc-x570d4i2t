---
description: "Use when implementing, extending, or debugging IPMI OEM command handlers for the X570D4I-2T OpenBMC layer. Covers all Megarac/AMI BIOS IPMI methods, the BIOS OOB payload protocol, MDR2 SMBIOS transfer, KCS channels, EFI variable access, D-Bus interfaces, command codes, completion codes, and wire-format structs."
applyTo:
  - "meta-asrock/meta-x570d4i2t/recipes-phosphor/ipmi/asrock-ipmi-oem/**"
  - "meta-asrock/meta-x570d4i2t/recipes-phosphor/smbios/**"
  - "meta-asrock/meta-x570d4i2t/recipes-asrock/**"
---

# X570D4I-2T Megarac/BIOS IPMI Methods — Implementation Reference

## Platform Context

- **BMC SoC**: ASPEED AST2500 (`arm1176jzs`)
- **Original firmware**: AMI Megarac SPX, Linux 5.4.99-ami
- **OpenBMC target**: `phosphor-host-ipmid` + `smbios-mdr` + `bmcweb`
- **IPMI provider plugin**: `asrock-ipmi-oem` → `libzasrockoemcmds.so`
- **Plugin location**: `meta-asrock/meta-x570d4i2t/recipes-phosphor/ipmi/asrock-ipmi-oem/`

---

## 1. KCS Channel Architecture

The AST2500 exposes three KCS (Keyboard Controller Style) channels used for in-band IPMI between the host CPU and BMC.

| Channel | Device     | Purpose                               | Megarac Config Key             |
|---------|------------|---------------------------------------|--------------------------------|
| KCS1    | `/dev/kcs0` | **SMM BIOS interface** (highest prio) | `KCS_SMM_CHANNEL=1`, `SUPPORT_SMM_IFC=1` |
| KCS2    | `/dev/kcs1` | Standard OS IPMI                      | `SUPPORT_KCS2_IFC=1`          |
| KCS3    | `/dev/kcs2` | Overflow / alt channel                | `SUPPORT_KCS3_IFC=1`          |

- KCS1 (SMM channel) is the exclusive path for BIOS→BMC communication during POST. It has elevated priority and is active before the OS IPMI stack loads.
- The eSPI interface (`espi.ko` + `espi_hw.ko`) is loaded at S45 for BIOS communication on newer platforms; KCS is the primary path on the X570D4I-2T.
- In OpenBMC, `phosphor-ipmi-host` handles all three channels automatically via the kernel IPMI character devices.

---

## 2. BIOS OOB Configuration Protocol

### 2.1 Overview

The AMI BIOS populates the BMC's BIOS setup database by pushing a compressed XML payload via IPMI OEM commands. The BMC decompresses it and exposes the data via the `xyz.openbmc_project.BIOSConfig.Manager` D-Bus interface, which bmcweb serves as Redfish `/redfish/v1/Systems/system/Bios`.

Protocol phases (sequential, during POST via KCS1/SMM channel):
```
BIOS → SetBIOSCap  →  BMC records capability flags
BIOS → SetPayload(StartTransfer)  →  BMC opens slot, returns reservationID
BIOS → SetPayload(InProgress) × N  →  BMC accumulates chunks with CRC-32 check
BIOS → SetPayload(EndTransfer)  →  BMC decompresses → /var/oob/bios.xml → D-Bus
BIOS → GetPayload(GetPayloadData)  →  BMC returns pending attribute changes (type 1)
```

### 2.2 Command Codes

All BIOS OOB commands use **NetFn 0x30** (OEM General).

| Command       | Code  | Direction     | Privilege | Description |
|---------------|-------|---------------|-----------|-------------|
| `SetBIOSCap`  | `0x7F` | BIOS → BMC   | Admin     | Declare BIOS OOB capability flags. Must be called before POST completes (OS not at Standby). |
| `GetBIOSCap`  | `0x7E` | Host → BMC   | User      | Read back the capability byte stored by the previous `SetBIOSCap`. |
| `SetPayload`  | `0x73` | BIOS → BMC   | Admin     | Multi-state chunked transfer (paramSel selects state machine stage). |
| `GetPayload`  | `0x72` | Host → BMC   | User      | Read payload metadata, raw data, or status (paramSel selects mode). |
| `GetBoardInfo`| `0x50` | Host → BMC   | User      | Return board product name from D-Bus inventory. ASRock-specific. |

**Completion codes** (from `include/oemcommands.hpp`):

| Symbol                   | Value | Meaning |
|--------------------------|-------|---------|
| `cc::payloadPacketMissed` | `0x80` | lzcat decompression of the finalised payload failed |
| `cc::payloadChecksumFail` | `0x81` | Per-chunk CRC-32 mismatch in `InProgress` |
| `cc::notSupportedInState` | `0x82` | Command rejected because host OS is at Standby (POST finished) |
| `cc::payloadIncomplete`   | `0x83` | `EndTransfer` received but total written ≠ declared total size |
| `cc::biosCapNotInit`      | `0x85` | `SetBIOSCap` has not been called yet |

### 2.3 SetBIOSCap Request Wire Format

```
[Byte 0] BIOSCapabilityByte  — bit 1 set = OOB BIOS config supported
[Byte 1] reserved (must be 0)
[Byte 2] reserved (must be 0)
[Byte 3] reserved (must be 0)
```

Response: completion code only.

Implementation guard: reject with `cc::notSupportedInState` if host OS D-Bus property `xyz.openbmc_project.State.OperatingSystem.Status.OperatingSystemState == Standby`.

### 2.4 SetPayload Request Wire Format

```
[Byte 0] paramSel   — PTState enum (0=StartTransfer, 1=InProgress, 2=EndTransfer, 3=UserAbort)
[Byte 1] payloadType — 0=BIOSXMLType0 (lzcat XML), 1=BIOSXMLType1 (pending attrs), 5=OTAPayload
[Bytes 2..N] payload-data (paramSel-dependent, see structs below)
```

#### StartTransfer body (`PayloadStartTransfer`, 16 bytes, little-endian)
```
uint32_t payloadTotalChecksum  — CRC-32 of the complete payload (verified at EndTransfer)
uint32_t payloadTotalSize      — total byte count of the complete payload
uint32_t payloadVersion        — version field (pass-through, stored in NVOOBdata)
uint32_t payloadflag           — flags (pass-through, stored in NVOOBdata)
```
Response: `uint32_t reservationID` (random, caller must echo in subsequent calls).

#### InProgress body (`PayloadInProgress`, 16-byte header + chunk data)
```
uint32_t payloadReservationID    — must match StartTransfer response
uint32_t payloadCurrentSize      — byte count of the following chunk data
uint32_t payloadOffset           — byte offset of this chunk within the full payload
uint32_t payloadCurrentChecksum  — CRC-32 of the chunk data bytes only (not the header)
[chunk data bytes follow immediately after the 16-byte header]
```
Response: `uint32_t bytesWritten` (= payloadCurrentSize of this chunk).

CRC-32 is computed with `boost::crc_32_type` over bytes at `payload.data() + 16` through `payload.data() + payload.size()`.

#### EndTransfer body (`PayloadEndTransfer`, 4 bytes)
```
uint32_t payloadReservationID   — must match StartTransfer response
```
Response: `uint32_t totalBytesWritten`.

On success: temp file at `/var/oob/temp<N>` is renamed to `/var/oob/Payload<N>`.
For type 0: BMC runs `lzcat -d /var/oob/Payload0` → `/var/oob/bios.xml`, then posts a D-Bus async task to call `publishBIOSPayload()`.

#### UserAbort body (`PayloadEndTransfer`, same layout)
Clears reservationID, deletes the temp file, flushes NVOOBdata. Returns success with 0.

### 2.5 GetPayload Request Wire Format

```
[Byte 0] paramSel    — GetPayloadParameter (0=GetPayloadInfo, 1=GetPayloadData, 2=GetPayloadStatus)
[Byte 1] payloadType — same type encoding as SetPayload
[Bytes 2..N] additional params (only for paramSel=1)
```

#### GetPayloadInfo (paramSel=0) response
```
uint8_t  payloadVersion
uint8_t  payloadType
uint32_t payloadTotalSize
uint32_t payloadTotalChecksum
uint8_t  payloadflag
uint8_t  payloadStatus    — 0=Unknown, 1=Valid, 2=Corrupted
uint32_t payloadTimeStamp — st_mtime of the finalised file
```

#### GetPayloadData (paramSel=1) request params
```
uint32_t offset   — byte offset within the payload file
uint32_t length   — byte count to return (max 4096)
```
Response:
```
uint8_t  payloadType
uint32_t readCount   — actual bytes returned
uint32_t checksum    — CRC-32 of the returned data block
uint8_t  data[readCount]
```

#### GetPayloadStatus (paramSel=2) response
```
uint8_t payloadStatus   — 0=Unknown, 1=Valid, 2=Corrupted
```

### 2.6 NV Persistence (NVOOBdata)

Stored at `/var/oob/nvoobdata.dat` as a raw binary blob (`sizeof(NVOOBdata)`).
Loaded at plugin constructor time by `initNVOOBdata()`. Written by `flushNVOOBdata()` after every state change.

```cpp
struct NVOOBdata {
    BIOSCapabilities mBIOSCapabilities;   // OOBCapability byte
    bool             mIsBIOSCapInitDone;
    PayloadInfo      payloadInfo[3];      // slots 0, 1, 5 (max index = maxPayloadSupported)
};
```

The `/var/oob/` directory is created at boot via systemd-tmpfiles (`/etc/tmpfiles.d/asrock-ipmi-oem.conf`).

### 2.7 D-Bus Integration

After decompressing the BIOS XML (payload type 0), the plugin should interact with `biosconfig-manager`:

```
Service:    xyz.openbmc_project.BIOSConfig.Manager   (dynamically resolved via getService())
Object:     /xyz/openbmc_project/bios_config/manager
Interface:  xyz.openbmc_project.BIOSConfig.Manager
Property:   BaseBIOSTable   — map<string, BIOSAttributeTuple>
Property:   PendingAttributes — map<string, tuple<string, DbusVariant>>
Property:   ResetBIOSSettings — enum string
```

Use `boost::asio::post(*io, lambda)` to push the D-Bus call asynchronously so the IPMI response is returned promptly. The `getIoContext()` and `getSdBus()` helpers are provided by `ipmid/api.hpp`.

For `GetPayload` payload type 1 (pending attributes), read `PendingAttributes` from D-Bus and serialise as `key=value\n` plain text written to `/var/oob/Payload1`.

---

## 3. MDR2 SMBIOS Transfer Protocol

### 3.1 How SMBIOS Reaches the BMC

The AMI BIOS sends SMBIOS tables to the BMC via IPMI OEM commands on **NetFn 0x3E** (`netFnOemEight`). The OpenBMC `smbios-mdr` package (`smbiosmdrv2app`) provides the D-Bus backend that parses and publishes SMBIOS, but does **not** register its own IPMI handlers. The `asrock-ipmi-oem` plugin bridges the IPMI layer to the D-Bus backend via `src/smbiosmdrv2handler.cpp`.

- `smbios-mdr_%.bbappend` removes Intel-specific CPU inventory providers (`cpuinfo`, `cpuinfo-peci`) and enables `smbios-ipmi-blob` (blob interface for direct SMBIOS file upload).
- `smbiosmdrv2app` exposes `xyz.openbmc_project.Smbios.MDR_V2` and parses `/var/lib/smbios/smbios2` when `AgentSynchronizeData` is called.
- Board-specific DIMM socket mapping is provided by `memoryLocationTable.json`.

### 3.2 MDR2 Command Set (NetFn **0x3E** / `netFnOemEight`, implemented in `asrock-ipmi-oem`)

`smbiosmdrv2app` provides the D-Bus backend but does **not** register IPMI handlers.
The IPMI→D-Bus bridge is in `src/smbiosmdrv2handler.cpp`.

| Constant                       | Code   | Handler function         | Description |
|--------------------------------|--------|--------------------------|-------------|
| `mdr::cmdMdrIIAgentStatus`     | `0x30` | `mdr2AgentStatus`        | BIOS queries BMC MDR2 agent / directory version |
| `mdr::cmdMdrIIGetDir`          | `0x31` | `mdr2GetDir`             | BMC returns directory entries (D-Bus `GetDirectoryInformation`) |
| `mdr::cmdMdrIIGetDataInfo`     | `0x32` | `mdr2GetDataInfo`        | BMC returns data-set metadata (D-Bus `FindIdIndex` → `GetDataInformation`) |
| `mdr::cmdMdrIILockData`        | `0x33` | `mdr2LockData`           | BIOS acquires exclusive write lock (D-Bus `SynchronizeDirectoryCommonData`) |
| `mdr::cmdMdrIIUnlockData`      | `0x34` | `mdr2UnlockData`         | BIOS releases lock |
| `mdr::cmdMdrIIGetDataBlock`    | `0x35` | `mdr2GetDataBlock`       | BMC returns a block from the session accumulation buffer |
| `mdr::cmdMdrIISendDir`         | `0x38` | `mdr2SendDir`            | BIOS sends directory metadata (D-Bus `SendDirectoryInformation`) |
| `mdr::cmdMdrIISendDataInfoOffer` | `0x39` | `mdr2DataInfoOffer`    | BMC offers a data-set ID slot (D-Bus `GetDataOffer`) |
| `mdr::cmdMdrIISendDataInfo`    | `0x3A` | `mdr2SendDataInfo`       | BIOS declares size/version/timestamp (D-Bus `SendDataInformation`) |
| `mdr::cmdMdrIIDataStart`       | `0x3B` | `cmd_mdr2_data_start`    | BIOS opens a write session; BMC allocates accumulation buffer |
| `mdr::cmdMdrIIDataDone`        | `0x3C` | `cmd_mdr2_data_done`     | BIOS closes session → D-Bus `AgentSynchronizeData` → SMBIOS parsed |
| `mdr::cmdMdrIISendDataBlock`   | `0x3D` | `mdr2SendDataBlock`      | BIOS writes one SMBIOS chunk; additive CRC-32 verified |

**GetDataInfo detail** (`mdr2GetDataInfo`, 0x32):
1. Receives `agentId` (must be `0x0101`) + `dataInfo[16]` (data-set identifier).
2. Calls D-Bus `FindIdIndex(dataInfo)` → `idIndex`.
3. Calls D-Bus `GetDataInformation(idIndex)` → packed byte vector.
4. Returns the vector verbatim as the IPMI response body.

Response layout from `smbios-mdr` `getDataInformation()`:
```
[0]     mdrVersion (= 2)
[1-16]  dataInfo echo
[17]    validFlag   (0=invalid, 1=valid, 2=locked)
[18-21] dataSetSize (uint32_t, reversed bytes)
[22]    dataVersion
[23-26] timestamp   (uint32_t, reversed bytes)
```

Session buffer: `SendDataBlock` accumulates chunks into `g_sessionBuffer` (keyed on `lockHandle`). `DataDone` calls `AgentSynchronizeData` and clears the buffer.

Transfer path: IPMI `SendDataBlock` → `g_sessionBuffer` → `AgentSynchronizeData` D-Bus call → `smbiosmdrv2app` re-reads `/var/lib/smbios/smbios2` → SMBIOS records published to D-Bus inventory.

### 3.3 DIMM Location Table

`memoryLocationTable.json` maps SMBIOS Type 17 `DeviceLocator` strings to socket topology:
```json
{
  "CPU1_DIMM_A1": { "Socket": 0, "MemoryController": 0, "Channel": 0, "Slot": 0 },
  "CPU1_DIMM_B1": { "Socket": 0, "MemoryController": 0, "Channel": 1, "Slot": 0 }
}
```
Installed at `/usr/share/smbios-mdr/memoryLocationTable.json` by the bbappend.

---

## 4. AMI-Specific Commands (Megarac Reference Only)

The following AMI Megarac `libipmiamioembiosremotecontrol.so.6.1.0` commands were discovered by reverse engineering. They are documented here as a reference for understanding the protocol origin; the OpenBMC equivalents are the OOB payload commands in Section 2.

| AMI Function              | Megarac NetFn | Direction     | OpenBMC Equivalent |
|---------------------------|---------------|---------------|--------------------|
| `AMISendToBios`           | `0x30`/`0x??` | BMC → BIOS    | `SetPayload(EndTransfer)` triggers decompression + D-Bus |
| `AMIGetBiosCommand`       | `0x30`/`0x??` | BMC → BIOS    | `GetPayload(GetPayloadData)` reads pending attrs |
| `AMISetBiosResponse`      | `0x30`/`0x??` | BMC → BIOS    | Write to `BIOSConfig.Manager.PendingAttributes` |
| `AMIGetBiosResponse`      | `0x30`/`0x??` | BIOS → BMC    | Read `BIOSConfig.Manager.BaseBIOSTable` |
| `AMISetBiosFlag`          | `0x30`/`0x??` | Host → BMC    | Set `BIOSConfig.Manager.ResetBIOSSettings` |
| `AMIGetBiosFlag`          | `0x30`/`0x??` | Host → BMC    | Get `BIOSConfig.Manager.ResetBIOSSettings` |

Megarac persistence: `/conf/BMC{N}/BIOS_FLAG.ini` (INI file per BMC instance).
OpenBMC persistence: `biosconfig-manager` stores to `/var/lib/bios-settings-manager/`.

Redfish BIOS on Megarac was served by `bios.lua` via a Redfish HI (host interface, no auth) route at `/bios/`. The BIOS connected to it directly during POST to push `SetupData.xml`. OpenBMC uses `bmcweb` with the same Redfish BIOS routes.

---

## 5. EFI Variable / NVAR Access

### 5.1 Offline Access (host power OFF — existing implementation)

The `host-efivars` recipe provides offline EFI variable reading:
1. Set GPIO J1 (line 73) LOW to mux the BIOS SPI flash to the BMC's SPI1 controller.
2. Bind `1e630000.spi` to expose the flash as `/dev/mtdNro` (the `pnor` MTD).
3. Run `nvar-dump` (AMI NVAR parser) to extract UEFI variables.

AMI NVAR store layout:
```
Magic:     "NVAR"  (4 bytes)
Each entry: NVAR_GUID + length + attributes + name\0 + data
Store terminator: 0xFFFFFFFF
```

### 5.2 Live Access (host running — via KCS, NOT yet implemented)

Implement as additional IPMI OEM commands in `asrock-ipmi-oem`:
- The BIOS exposes EFI variables via an IPMI-based NVAR bridge during POST.
- Implement `cmdGetEFIVar` / `cmdSetEFIVar` in a new `src/efivarcommands.cpp`.
- Register on NetFn `0x30` with unused command codes in the `0x60`–`0x6F` range.
- Use the same chunked-transfer pattern (StartTransfer/InProgress/EndTransfer) for large variables.

---

## 6. File Layout — asrock-ipmi-oem Plugin

```
recipes-phosphor/ipmi/
├── asrock-ipmi-oem_0.1.bb          # BitBake recipe (inherit meson obmc-phosphor-ipmiprovider-symlink)
└── asrock-ipmi-oem/
    ├── meson.build                  # Builds libzasrockoemcmds.so → ${libdir}/ipmid-providers/
    ├── meson.options                # Feature flags (tests disabled by default)
    ├── include/
    │   ├── biosconfig.hpp           # Wire-format structs, PTState/PStatus/PType enums, NVOOBdata
    │   └── oemcommands.hpp          # NetFn/command codes (general + mdr namespaces), CC constants
    └── src/
        ├── biosconfig.cpp           # SetBIOSCap, GetBIOSCap, SetPayload, GetPayload handlers
        ├── oemcommands.cpp          # GetBoardInfo handler + future OEM command handlers
        └── smbiosmdrv2handler.cpp   # All 12 MDR2 SMBIOS IPMI handlers (NetFn 0x3E)
```

### Adding a New Command

1. Add the command code constant to `include/oemcommands.hpp` under `namespace asrock::general`.
2. Add any new wire-format structs to `include/biosconfig.hpp` (or a new header).
3. Implement the handler function in `src/oemcommands.cpp` (or a new `src/*.cpp` file).
4. Register with `ipmi::registerHandler()` in the file's `__attribute__((constructor))` function.
5. Add any new `.cpp` file to the `asrockoemcmds_src` list in `meson.build`.

---

## 7. Build System Integration

### Recipe Inheritance

```bitbake
inherit meson pkgconfig obmc-phosphor-ipmiprovider-symlink

LIBRARY_NAMES        = "libzasrockoemcmds.so"
HOSTIPMI_PROVIDER_LIBRARY += "${LIBRARY_NAMES}"
NETIPMI_PROVIDER_LIBRARY  += "${LIBRARY_NAMES}"
```

The `obmc-phosphor-ipmiprovider-symlink` bbclass creates symlinks in `${libdir}/host-ipmid/` and `${libdir}/net-ipmid/` so `phosphor-ipmi-host` discovers and loads the shared library at startup.

### Dependencies

```bitbake
DEPENDS = "boost nlohmann-json phosphor-dbus-interfaces phosphor-ipmi-host phosphor-logging sdbusplus"
```

The `phosphor-ipmi-host` dependency provides the `libipmid` pkg-config entry and the `ipmid/api.hpp`, `ipmid/utils.hpp`, `ipmid/message.hpp` headers.

### Meson Flags Required

```meson
'-DBOOST_NO_RTTI'               # phosphor ipmid ABI requirement
'-DBOOST_NO_TYPEID'
'-DBOOST_ASIO_DISABLE_THREADS'
override_options: ['b_lundef=false']   # allow unresolved ipmid symbols at link time
```

---

## 8. D-Bus Service Reference

| Service                                | Object Path                                          | Interface |
|----------------------------------------|------------------------------------------------------|-----------|
| `xyz.openbmc_project.BIOSConfig.Manager` | `/xyz/openbmc_project/bios_config/manager`         | `xyz.openbmc_project.BIOSConfig.Manager` |
| `xyz.openbmc_project.State.Host0`      | `/xyz/openbmc_project/state/host0`                   | `xyz.openbmc_project.State.OperatingSystem.Status` |
| `xyz.openbmc_project.Smbios.MDR_V2`   | `/xyz/openbmc_project/Smbios/MDR_V2`                 | `xyz.openbmc_project.Smbios.MDR_V2` |
| *(inventory, resolved dynamically)*    | `/xyz/openbmc_project/inventory/system/board`        | `xyz.openbmc_project.Inventory.Item.Board` |

### POST-Complete Detection

```cpp
// Returns true when host OS is at Standby (POST done).
// On D-Bus failure, conservatively returns true to reject config commands.
static bool getPostCompleted() {
    Value v = getDbusProperty(*dbus,
        "xyz.openbmc_project.State.Host0",
        "/xyz/openbmc_project/state/host0",
        "xyz.openbmc_project.State.OperatingSystem.Status",
        "OperatingSystemState");
    const auto& s = std::get<std::string>(v);
    return (s == "Standby") ||
           (s == "xyz.openbmc_project.State.OperatingSystem.Status.OSStatus.Standby");
}
```

---

## 9. Security Considerations

- **`SetBIOSCap` and `SetPayload`** require `Privilege::Admin` — enforce IPMI user privilege.
- Reject `SetBIOSCap` and `SetPayload(type 0)` when `getPostCompleted()` returns true to prevent post-boot BIOS XML injection.
- Validate `payloadReservationID` on every `InProgress`, `EndTransfer`, and `UserAbort` call — return `ipmi::responseInvalidReservationId()` on mismatch.
- Validate per-chunk CRC-32 and return `cc::payloadChecksumFail` on mismatch — prevents corrupted data from reaching `/var/oob/`.
- Validate `payloadType < maxPayloadSupported` before indexing `payloadInfo[]`.
- Clamp `GetPayload` block length to `maxGetPayloadDataSize` (4096 bytes) to prevent oversized response buffers.
- The BIOS XML decompression runs `lzcat` as a subprocess — ensure the path `/usr/bin/lzcat` is absolute and not controllable by external input.
