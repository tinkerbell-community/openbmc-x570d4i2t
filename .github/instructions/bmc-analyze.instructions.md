# BMC Firmware Analysis: X570D4I-2T v01.91.00

Analysis of `/rootfs_squash/rootfs` from the ASRock Rack X570D4I-2T BMC firmware image (v01.91.00).

## Firmware Identity

This is **AMI MegaRAC SPx** firmware running on an **AST2500** BMC SoC. It is **not** the OpenBMC phosphor stack. All IPMI handling is done via a closed-source AMI daemon architecture with proprietary libraries.

- Architecture: ARM (arm-linux-gnueabi)
- IPMI daemon: `IPMIMain` (AMI MegaRAC)
- Redfish bridge: Lua-based `sync-agent` using Redis pub/sub
- No Intel SMBIOS MDR protocol present

---

## Network Function Codes

Confirmed from `m_MsgHndlrTbl` in `libipmimsghndlr.so` and `Netfntbl` in `libipmipdkcmds.so`:

| Constant | NetFn (req) | NetFn (resp) | Description |
|---|---|---|---|
| `NETFN_CHASSIS` | `0x00` | `0x01` | Chassis control |
| `NETFN_BRIDGE` | `0x02` | `0x03` | IPMB bridging |
| `NETFN_SENSOR` | `0x04` | `0x05` | Sensor/Event |
| `NETFN_APP` | `0x06` | `0x07` | Application |
| `NETFN_FIRMWARE` | `0x08` | `0x09` | Firmware |
| `NETFN_STORAGE` | `0x0A` | `0x0B` | FRU/SDR/SEL |
| `NETFN_TRANSPORT` | `0x0C` | `0x0D` | LAN/SOL/Serial |
| `NETFN_GROUP_EXTN` | `0x2C` | `0x2D` | Group Extension (DCMI, PICMG) |
| `NETFN_AMI` | `0x30` | `0x31` | AMI OEM commands |
| `NETFN_SCORPIO` | `0x32` | `0x33` | Scorpio OEM |
| `NETFN_OEM` | `0x36` | `0x37` | Generic OEM |
| `NETFN_TEST_OEM` | `0x3E` | `0x3F` | Test OEM |

---

## MDR — Memory Data Repository

There are **no `.mdr` files** and no Intel SMBIOS MDR protocol in this firmware. "MDR" here refers to **AMI YAFU (Yet Another Firmware Upgrade) raw memory operations** — BMC RAM manipulation used during firmware upload via IPMI OEM commands on `NETFN_AMI` (`0x30`).

All YAFU memory handlers live in `libipmimsghndlr.so.6.205.0`. Codes `0x01`–`0x04` are confirmed from the `g_AMI_CmdHndlr` binary handler table; remaining YAFU codes (`0x05`–`0x10` range) are registered by the `libipmipdkcmds.so` PDK plugin at runtime.

| Command Code | IPMI Command | YAFU Handler | Description |
|---|---|---|---|
| `0x01` | `CMD_AMI_YAFU_ALLOCATE_MEMORY` | `AMIYAFUAllocateMemory` | Allocate BMC RAM buffer for FW upload |
| `0x02` | `CMD_AMI_YAFU_FREE_MEMORY` | `AMIYAFUFreeMemory` | Free allocated buffer |
| `0x03` | `CMD_AMI_YAFU_READ_MEMORY` | `AMIYAFUReadMemory` | Read back memory region |
| `0x04` | `CMD_AMI_YAFU_WRITE_MEMORY` | `AMIYAFUWriteMemory` | Write firmware payload chunk |
| `0x05` | `CMD_AMI_YAFU_COPY_MEMORY` | `AMIYAFUCopyMemory` | Copy memory region |
| `0x06` | `CMD_AMI_YAFU_COMPARE_MEMORY` | `AMIYAFUCompareMemory` | Verify written data |
| `0x07` | `CMD_AMI_YAFU_CLEAR_MEMORY` | `AMIYAFUClearMemory` | Zero-fill region |
| `0x08` | `CMD_AMI_YAFU_READ_FLASH` | — | Read flash region |
| `0x09` | `CMD_AMI_YAFU_WRITE_FLASH` | — | Write flash region |
| `0x0A` | `CMD_AMI_YAFU_ERASE_FLASH` | — | Erase flash target |
| `0x0B` | `CMD_AMI_YAFU_VERIFY_FLASH` | — | Verify flash after write |
| `0x0C` | `CMD_AMI_YAFU_ACTIVATE_FLASH` | — | Activate flash device |
| `0x0D` | `CMD_AMI_YAFU_GET_STATUS` | — | Get current YAFU operation status |
| `0x0E` | `CMD_AMI_YAFU_GET_FIRMWARE_INFO` | — | Get firmware version/info |
| `0x0F` | `CMD_AMI_YAFU_GET_FLASH_INFO` | — | Get flash layout info |
| `0x10` | `CMD_AMI_YAFU_ERASE_COPY_FLASH` | — | Erase-then-copy flash (admin-only: `0xFF` priv) |
| `0x?? ` | `CMD_AMI_YAFU_ACTIVATE_FLASH_DEVICE` | — | Activate specific flash device |
| `0x??` | `CMD_AMI_YAFU_DEACTIVATE_FLASH_MODE` | — | Exit flash mode |
| `0x??` | `CMD_AMI_YAFU_FIRMWARE_SELECT_FLASH` | — | Select flash image slot |
| `0x??` | `CMD_AMI_YAFU_DUAL_IMAGE_SUPT` | — | Dual image support query |
| `0x??` | `CMD_AMI_YAFU_SWITCH_FLASH_DEVICE` | — | Switch between flash devices |
| `0x??` | `CMD_AMI_YAFU_RESTORE_FLASH_DEVICES` | — | Restore flash device list |
| `0x??` | `CMD_AMI_YAFU_PROTECT_FLASH` | — | Enable flash write protection |
| `0x??` | `CMD_AMI_YAFU_COMMON_NAK` | — | NAK response for unsupported commands |
| `0x??` | `CMD_AMI_YAFU_MISCELLANEOUS_INFO` | — | Miscellaneous FW update info |
| `0x??` | `CMD_AMI_YAFU_SIGNIMAGEKEY_REPLACE` | — | Replace firmware signing key |
| `0x??` | `CMD_AMI_YAFU_GET_ECF_STATUS` | — | Get ECF status |
| `0x??` | `CMD_AMI_YAFU_GET_VERIFY_STATUS` | — | Get verify operation status |
| `0x??` | `CMD_AMI_YAFU_GET_BOOT_VARS` | — | Get boot variables |
| `0x??` | `CMD_AMI_YAFU_GET_BOOT_CONFIG` | — | Get boot configuration |
| `0x??` | `CMD_AMI_YAFU_SET_BOOT_CONFIG` | — | Set boot configuration |
| `0x??` | `CMD_AMI_YAFU_RESET_DEVICE` | — | Reset after firmware update |
| `0x??` | `CMD_AMI_YAFU_GET_FMH_INFO` | — | Get Firmware Management Header |

### ACPI Power State (via message handler)

| Handler | Description |
|---|---|
| `SetACPIPwrState` | Write ACPI power state |
| `GetACPIPwrState` | Read ACPI power state |

---

## Core IPMI Daemon

### Entry Points

| Path | Description |
|---|---|
| `/usr/local/bin/IPMIMain` | Primary IPMI daemon |
| `/etc/init.d/ipmistack` | SysV init script (runlevels S/9, stops at 4/6/8) |
| `/etc/rcS.d/S22ipmistack` | Symlink — start on boot |
| `/etc/rc9.d/S22ipmistack` | Symlink — start on warm reboot (runlevel 9) |
| `/etc/rc9.d/K98ipmistack` | Symlink — stop on warm reboot before restart |
| `/etc/rc4.d/K37ipmistack` | Symlink — stop on runlevel 4 |

Start command: `IPMIMain --daemonize --reg-with-procmgr`

### IPMI Configuration

File: `/etc/defconfig/BMC1/1U2-X570/2T/IPMI.conf`

```ini
[IPMIConfig]
BMC_SLAVE_ADDR=0x0
PRIMARY_IPMB_ADDR=32
SECONDARY_IPMB_ADDR=32
PRIMARY_IPMB_I2C_BUS_NUM=8
SECONDARY_IPMB_I2C_BUS_NUM=3
EEPROM_I2C_BUS_NUM=7
SOL_IFC_PORT=/dev/ttyS3
SERIAL_IFC_PORT=/dev/ttyS0
SDR_DEVICE_SIZE=10          # KB
SEL_DEVICE_SIZE=64          # KB
TOTAL_MAX_SESSION=36
MAX_NUM_USERS=15
NUM_USER_PER_CHANNEL=15
MAX_LAN_CHANNELS=2
NM_IPMB_BUS=0x1
KCS_SMM_CHANNEL=1
APML_BUS_NUMBER=1

# Enabled interfaces
SUPPORT_KCS1_IFC=1
SUPPORT_KCS2_IFC=1
SUPPORT_KCS3_IFC=1
SUPPORT_LAN_IFC=1
SUPPORT_USB_IFC=1
SUPPORT_SOL_IFC=1
SUPPORT_UDS_IFC=1
SUPPORT_SMM_IFC=1
PRIMARY_IPMB_SUPPORT=1
SECONDARY_IPMB_SUPPORT=1
SUPPORT_DCMI_IFC=1
SUPPORT_GROUP_EXTN=1
SUPPORT_IPMI_FIREWALL=1
SUPPORT_CHASSIS_TIMER=1
SUPPORT_CHASSIS_INTERUPTS=1
SUPPORT_DEVENT_FOR_REARM=1
IS_CARD_IN_FLASH_MODE=1

# Disabled interfaces
SUPPORT_SERIAL_IFC=0
SUPPORT_SMBUS_IFC=0
SUPPORT_ICMB_IFC=0
SUPPORT_BT_IFC=0
SUPPORT_APML_IFC=0
SUPPORT_HPM_IFC=0
SUPPORT_SSICB_IFC=0
SUPPORT_OPMA_IFC=0
SUPPORT_TERMINAL_MODE=0
SUPPORT_INTERRUPT_SENSORS=0
THIRD_IPMB_SUPPORT=0
FOURTH_IPMB_SUPPORT=0
FIFTH_IPMB_SUPPORT=0
SIXTH_IPMB_SUPPORT=0
```

---

## Platform HAL — X570D4I-2T Sensor Drivers

Library: `/usr/local/lib/ipmi/1U2-X570/2T/libipmipar.so.1.0.0`

This is the board-specific Platform Adaptation Reference (PAR) library. It registers all device driver entry points for this board's sensors against the AMI IPMI stack.

### Device Driver Symbol Convention

Each device exposes: `_init_device`, `_init`, `_read_device`, `_read`, `_write_device`, `_write`

### Voltage Sensors (I2C addr 0x20)

| Sensor Index | Symbol Base | Version |
|---|---|---|
| 1 | `dev_asrr_voltage_v0dot1_1_0_32` | v0.1 |
| 2 | `dev_asrr_voltage_v0dot1_2_0_32` | v0.1 |
| 3 | `dev_asrr_voltage_v0dot1_3_0_32` | v0.1 |
| 4 | `dev_asrr_voltage_v0dot1_4_0_32` | v0.1 |
| 5 | `dev_asrr_voltage_v0dot1_5_0_32` | v0.1 |
| 6 | `dev_asrr_voltage_v0dot1_6_0_32` | v0.1 |
| 7 | `dev_asrr_voltage_v0dot2_7_0_32` | v0.2 |
| 8 | `dev_asrr_voltage_v0dot2_8_0_32` | v0.2 |
| 9 | `dev_asrr_voltage_v0dot2_9_0_32` | v0.2 |
| 10 | `dev_asrr_voltage_v0dot1_10_0_32` | v0.1 |
| 11 | `dev_asrr_voltage_v0dot1_11_0_32` | v0.1 |
| 12 | `dev_asrr_voltage_v0dot1_12_0_32` | v0.1 |
| 13 | `dev_asrr_voltage_v0dot1_13_0_32` | v0.1 |

### Temperature Sensors (I2C addr 0x20)

| Sensor Index | Symbol Base |
|---|---|
| 49 | `dev_asrr_temp_v0dot1_49_0_32` |
| 50 | `dev_asrr_temp_v0dot1_50_0_32` |
| 52 | `dev_asrr_temp_v0dot1_52_0_32` |
| 55 | `dev_asrr_temp_v0dot1_55_0_32` |
| 56 | `dev_asrr_temp_v0dot1_56_0_32` |
| 59 | `dev_asrr_temp_v0dot1_59_0_32` |
| 64 | `dev_asrr_temp_v0dot1_64_0_32` |
| 65 | `dev_asrr_temp_v0dot1_65_0_32` |
| 66 | `dev_asrr_temp_v0dot1_66_0_32` |
| 67 | `dev_asrr_temp_v0dot1_67_0_32` |

### Fan Sensors

| Symbol Base | I2C Addr | Notes |
|---|---|---|
| `dev_asrr_fan_v0dot1_0x60h_0_32` | `0x60` | Fan 1 |
| `dev_asrr_fan_v0dot1_0x62h_0_32` | `0x62` | Fan 2 |
| `dev_asrr_fan_v0dot1_0x63h_0_32` | `0x63` | Fan 3 |

### Discrete / Status Sensors

| Symbol Base | Sensor # | Notes |
|---|---|---|
| `dev_asrr_discrete_v0dot1_145_0_32` | 145 | Generic discrete sensor |
| `dev_asrr_sel_sensor_v0dot1_147_0_32` | 147 | Read/write SEL event sensor |
| `dev_asrr_chassisintr_sio_v01_0x90h_0_32` | SIO `0x90` | Chassis intrusion via Super-I/O |
| `dev_watchdog2_0xf9h_0_32` | `0xF9` | Watchdog timer sensor |
| `dev_powerunit_0xfah_0_32` | `0xFA` | Power supply / power unit status |

### Super-I/O — NCT6796D

| Symbol | Notes |
|---|---|
| `dev_nct6796d_v0dot1_init_device` | NCT6796D Super-I/O init |
| `dev_nct6796d_v0dot1_read_device` | Read hwmon registers |
| `dev_nct6796d_v0dot1_write_device` | Write hwmon registers |
| `dev_nct6796d_v0dot1_caseopen0_write` | Case open discrete write |

### NVMe Drive Sensor

| Symbol | Notes |
|---|---|
| `dev_asrrnvmev01_init_device` | NVMe sensor init |
| `dev_asrrnvmev01_read_device` | NVMe sensor read |
| `dev_asrrnvmev01_write_device` | NVMe sensor write |
| `dev_asrrnvmev01_sensor_offset_read` | Read with offset (multi-register) |

### I2C Infrastructure

| Symbol | Device | Notes |
|---|---|---|
| `dev_asrr_pca9545_v05_*` | PCA9545 | I2C 4-channel mux; channel-3 has dedicated semaphore |
| `dev_ast2500_*` | AST2500 | BMC SoC I2C controller |
| `dev_ast2500_i2c_0_writereg` / `i2c_write_reg` | — | Low-level I2C write |
| `dev_ast2500_i2c_0_readreg` / `i2c_read_reg` | — | Low-level I2C read |
| `dev_ast2500_i2c_0_readregdata` / `i2c_read_regData` | — | I2C read with data |
| `dev_ast2500_i2c_0_rwi2c` / `i2c_write_read` | — | Combined write+read |
| `sem_post_i2c` / `sem_wait_i2c` | — | I2C bus semaphore |

### FRU / EEPROM

| Symbol | Notes |
|---|---|
| `dev_bmc_fru_init_device` | FRU EEPROM init (I2C bus 7 per IPMI.conf) |
| `dev_bmc_fru_read_device` | Read FRU data |
| `dev_bmc_fru_write_device` | Write FRU data |
| `dev_bmc_fru_properties` | FRU property descriptor |

### Global Sensor Table Exports

| Symbol | Description |
|---|---|
| `g_sensor_tbl` | Sensor table (all sensors) |
| `g_sensor_platform_tbl` | Platform-specific sensor table |
| `g_total_sensors` | Total sensor count |
| `hal_get_total_sensors` | Runtime query for sensor count |
| `hal_get_sensor_table_entry` | Runtime sensor table lookup |

---

## IPMI Library Stack

### Core Libraries (`/usr/local/lib/`)

| Library | Version | Function |
|---|---|---|
| `libipmistack.so` | 6.47.0 | Core stack: SDR/SEL/sensor init, event processing |
| `libipmimsghndlr.so` | 6.205.0 | Message routing + YAFU MDR memory ops |
| `libipmi.so` | 6.49.0 | `IPMICMD_*` client API functions |
| `libipmihalapi.so` | 6.2.0 | HAL API layer |
| `libipmihalhw.so` | 6.3.0 | HAL hardware abstraction |
| `libipmiparams.so` | 6.1.0 | IPMI parameter storage |
| `libipmipdk.so` | 6.28.0 | Platform Development Kit core |
| `libipmipdkapi.so` | 6.5.0 | PDK API — command registration |
| `libipmipdkcmds.so` | 6.1.0 | PDK standard commands |
| `libipmipef.so` | 6.16.0 | PEF (Platform Event Filtering) |
| `libipmichassis.so` | 6.3.0 | Chassis commands |
| `libipmidcmi.so` | 6.2.0 | DCMI power management |
| `libipmiinterruptsensor.so` | 6.3.0 | Interrupt-driven sensor monitoring |
| `libipmitimer.so` | 6.1.0 | IPMI timer management |
| `libipmi_helper.so` | 6.4.0 | Helper utilities |
| `libipmiredfish.so` | 6.23.5 | IPMI to Redfish mapping |
| `libipmi_redfishsensor.so` | 6.3.1 | Sensor data → Redfish |
| `libipmiamioemsmashlitecore.so` | 6.1.0 | SMASH-Lite CLP core |

### Transport Libraries

| Library | Version | Protocol |
|---|---|---|
| `libipmilan.so` | 6.19.0 | IPMI over LAN (RMCP/RMCP+) |
| `libipmikcs.so` | 6.4.0 | KCS host interface |
| `libipmiipmb.so` | 6.4.0 | IPMB (I2C bus bridging) |
| `libipmilocal.so` | 6.12.0 | Unix domain socket (UDS) |
| `libipmiusb.so` | 6.7.0 | USB interface |
| `libipmisol.so` | 6.10.0 | Serial over LAN |
| `libipmiredfishhostiface.so` | 6.6.1 | Redfish host interface |
| `libipmi_vnc.so` | 6.4.0 | VNC/KVM over IPMI |

### Authentication / NSS

| Library | Version | Function |
|---|---|---|
| `/lib/arm-linux-gnueabi/security/pam_ipmi.so` | 6.7.1 | PAM authentication for IPMI users |
| `/lib/libnss_ipmi.so` | 6.1.0 | NSS module — IPMI user DB integration |

### OEM Handler Libraries (`/usr/local/lib/`)

| Library | Version | Function |
|---|---|---|
| `libipmiamioemscorpiocmds.so` | 6.6.0 | Scorpio platform OEM commands |
| `libipmiamioemris.so` | 6.8.0 | RIS (Remote Install Service) |
| `libipmiamioemremotekvm.so` | 6.3.0 | Remote KVM configuration |
| `libipmiamioemvnc.so` | 6.5.0 | VNC configuration |
| `libipmiamioemautovideorcd.so` | 6.9.0 | Auto video record |
| `libipmiamioeminventory.so` | 6.0.1 | FRU inventory |
| `libipmiamioemldap.so` | 6.4.0 | LDAP directory integration |
| `libipmiamioemad.so` | 6.5.0 | Active Directory integration |
| `libipmiamioemradius.so` | 6.3.0 | RADIUS authentication |
| `libipmiamioemsnmp.so` | 6.10.0 | SNMP configuration |
| `libipmiamioemfirewall.so` | 6.1.0 | IPMI firewall rules |
| `libipmiamioemsessionmgmt.so` | 6.4.0 | Session management |
| `libipmiamioemserviceconf.so` | 6.9.0 | Service configuration |
| `libipmiamioemsshconf.so` | 6.2.0 | SSH configuration |
| `libipmiamioemtimezone.so` | 6.3.0 | Timezone configuration |
| `libipmiamioemntp.so` | 6.3.0 | NTP configuration |
| `libipmiamioembackuprestore.so` | 6.2.0 | Config backup/restore |
| `libipmiamioembiosremotecontrol.so` | 6.1.0 | BIOS remote control |
| `libipmiamioempwdenc.so` | 6.5.0 | Password encryption |
| `libipmiamioemaccessredis.so` | 6.1.0 | Redis access layer |
| `libipmiamioemrestiface.so` | 6.17.3 | REST interface |
| `libipmiamioemfwupdateprctl.so` | 6.1.0 | Firmware update protocol |
| `libipmiamioemhostlock.so` | 6.1.0 | Host lock feature |
| `libipmiamioemautohostlock.so` | 6.1.0 | Auto host lock |
| `libipmiamioemextpriv.so` | 6.1.0 | Extended privilege levels |
| `libipmiamioemctldbg.so` | 6.1.0 | Debug control |
| `libipmiamioemprsvconf.so` | 6.1.0 | Preserved configuration |
| `libipmiamioemextlog.so` | — | Extended event log |
| `libipmiamioemsingleport.so` | 6.1.0 | Single-port mode |
| `libipmiamioemsmashlitecorecmds.so` | 6.1.0 | SMASH-Lite CLP commands |
| `libipmiamioempamreorder.so` | 6.1.0 | PAM stack reordering |
| `libipmiamioemmedia.so` | 6.11.0 | Virtual media |

---

## IPMI Command Set

All command codes are sourced from binary handler table extraction (`g_*_CmdHndlr` in `libipmimsghndlr.so`) and the IPMI 2.0 specification. Standard command codes are confirmed by both sources. AMI OEM codes `0x01`–`0x04` are confirmed from the core handler table; `0x16`/`0x17` and `0xDD`/`0xDE` are confirmed from `/usr/local/lib/IPMI_AMI_VNCCONF.h`.

### NETFN_CHASSIS (`0x00` / `0x01`) — Confirmed from `g_Chassis_CmdHndlr`

| Code | Command | Min Req | Privilege |
|---|---|---|---|
| `0x00` | `CMD_GET_CHASSIS_CAPABILITIES` | 0 | USER |
| `0x01` | `CMD_GET_CHASSIS_STATUS` | 0 | USER |
| `0x02` | `CMD_CHASSIS_CONTROL` | 1 | OPERATOR |
| `0x03` | `CMD_CHASSIS_RESET` | 0 | OPERATOR |
| `0x04` | `CMD_SET_CHASSIS_IDENTITY` | — | OPERATOR |
| `0x05` | `CMD_SET_CHASSIS_CAPABILITIES` | — | ADMIN |
| `0x06` | `CMD_SET_POWER_RESTORE_POLICY` | 1 | OPERATOR |
| `0x07` | `CMD_GET_SYSTEM_RESTART_CAUSE` | 0 | USER |
| `0x08` | `CMD_SET_SYSTEM_BOOT_OPTIONS` | — | OPERATOR |
| `0x09` | `CMD_GET_SYSTEM_BOOT_OPTIONS` | 3 | OPERATOR |
| `0x0A` | `CMD_SET_FRONT_PANEL_ENABLES` | 1 | ADMIN |
| `0x0B` | `CMD_SET_POWER_CYCLE_INTERVAL` | 1 | ADMIN |
| `0x0F` | `CMD_GET_POH_COUNTER` | 0 | USER |

### NETFN_APP (`0x06` / `0x07`) — Confirmed from `g_App_CmdHndlr`

| Code | Command | Min Req | Privilege |
|---|---|---|---|
| `0x01` | `CMD_GET_DEV_ID` | 0 | USER |
| `0x02` | `CMD_COLD_RESET` | 0 | ADMIN |
| `0x03` | `CMD_WARM_RESET` | 0 | ADMIN |
| `0x04` | `CMD_GET_SELF_TEST_RESULTS` | 0 | USER |
| `0x05` | `CMD_MANUFACTURING_TEST_ON` | — | ADMIN |
| `0x06` | `CMD_SET_ACPI_PWR_STATE` | 2 | ADMIN |
| `0x07` | `CMD_GET_ACPI_PWR_STATE` | 0 | USER |
| `0x08` | `CMD_GET_DEV_GUID` | 0 | USER |
| `0x09` | `CMD_GET_NETFN_SUPPORT` | 1 | USER |
| `0x0A` | `CMD_GET_COMMAND_SUPPORT` | — | USER |
| `0x0B` | `CMD_GET_COMMAND_SUBFN_SUPPORT` | — | USER |
| `0x0C` | `CMD_GET_CONFIGURABLE_CMDS` | — | USER |
| `0x0D` | `CMD_GET_CONFIGURABLE_CMD_SUBFNS` | — | USER |
| `0x22` | `CMD_RESET_WATCHDOG_TIMER` | 0 | OPERATOR |
| `0x24` | `CMD_SET_WATCHDOG_TIMER` | 6 | OPERATOR |
| `0x25` | `CMD_GET_WATCHDOG_TIMER` | 0 | USER |
| `0x2E` | `CMD_GET_MESSAGE_FLAGS` | 0 | — |
| `0x2F` | `CMD_ENABLE_MESSAGE_CHANNEL_RECEIVE` | 0 | USER |
| `0x30` | `CMD_GET_MESSAGE` | — | — |
| `0x31` | `CMD_SEND_MSG` | — | — |
| `0x32` | `CMD_READ_EVENT_MSG_BUFFER` | 0 | — |
| `0x33` | `CMD_GET_BT_INTERFACE_CAPABILITIES` | — | — |
| `0x34` | `CMD_GET_SYSTEM_GUID` | 0 | USER |
| `0x35` | `CMD_GET_CHANNEL_AUTH_CAPABILITIES` | — | — |
| `0x36` | `CMD_GET_SESSION_CHALLENGE` | 0 | USER |
| `0x37` | `CMD_ACTIVATE_SESSION` | 0 | ANY |
| `0x38` | `CMD_SET_SESSION_PRIVILEGE_LEVEL` | 0 | ANY |
| `0x39` | `CMD_CLOSE_SESSION` | 0 | ANY |
| `0x3A` | `CMD_GET_SESSION_INFO` | 0 | ANY |
| `0x3B` | `CMD_GET_AUTHCODE` | — | USER |
| `0x3C` | `CMD_GET_AUTHCODE` | — | CALLBACK |
| `0x3D` | `CMD_SET_CHANNEL_ACCESS` | 0 | USER |
| `0x3F` | `CMD_GET_CHANNEL_ACCESS` | 0 | OPERATOR |
| `0x40` | `CMD_GET_CHANNEL_INFO` | 0 | ADMIN |
| `0x41` | `CMD_SET_USER_ACCESS` | 0 | USER |
| `0x42` | `CMD_GET_USER_ACCESS` | 0 | USER |
| `0x43` | `CMD_SET_USERNAME` | — | ADMIN |
| `0x44` | `CMD_GET_USERNAME` | 2 | OPERATOR |
| `0x45` | `CMD_SET_USER_PASSWORD` | — | ADMIN |
| `0x46` | `CMD_GET_USER_PAYLOAD_ACCESS` | 1 | OPERATOR |
| `0x47` | `CMD_SET_USER_PAYLOAD_ACCESS` | — | ADMIN |
| `0x48` | `CMD_GET_CHANNEL_PAYLOAD_SUPPORT` | 6 | CALLBACK |
| `0x49` | `CMD_GET_CHANNEL_PAYLOAD_VERSION` | 6 | CALLBACK |
| `0x4A` | `CMD_GET_CHANNEL_OEM_PAYLOAD_INFO` | 1 | USER |
| `0x4B` | `CMD_MASTER_WRITE_READ` | 2 | USER |
| `0x4C` | `CMD_GET_CHANNEL_CIPHER_SUITES` | 6 | ADMIN |
| `0x4D` | `CMD_SUSPEND_RESUME_PAYLOAD_ENCRYPTION` | 2 | OPERATOR |
| `0x4E` | `CMD_SET_CHANNEL_SECURITY_KEYS` | 0 | USER |
| `0x4F` | `CMD_GET_SYSTEM_INTERFACE_CAPABILITIES` | 0 | USER |
| `0x50` | `CMD_GET_SYS_INFO_PARAM` | 7 | USER |
| `0x52` | `CMD_MASTER_WRITE_READ` | 0 | OPERATOR |
| `0x54` | `CMD_GET_SYSTEM_GUID` (alt) | 0 | ANY |
| `0x55` | `CMD_GET_CHANNEL_AUTH_CAPABILITIES` (alt) | 0 | USER |
| `0x56` | `CMD_ENABLE_MESSAGE_CHANNEL_RECEIVE` (alt) | — | ADMIN |
| `0x57` | `CMD_GET_MESSAGE` (alt) | 0 | USER |
| `0x58` | `CMD_READ_EVENT_MSG_BUFFER` (alt) | — | ADMIN |
| `0x59` | `CMD_CLOSE_SESSION` (alt) | 4 | USER |
| `0x60` | `CMD_SET_BMC_GLOBAL_ENABLES` | — | USER |
| `0x61` | `CMD_GET_BMC_GLOBAL_ENABLES` | — | USER |
| `0x62` | `CMD_SET_BMC_GLOBAL_ENABLES` (alt) | 8 | USER |
| `0x63` | `CMD_GET_BMC_GLOBAL_ENABLES` (alt) | 4 | USER |
| `0x64` | `CMD_CLEAR_MESSAGE_FLAGS` | 3 | USER |

### NETFN_SENSOR (`0x04` / `0x05`) — Confirmed from `g_SensorEvent_CmdHndlr`

| Code | Command | Min Req | Privilege |
|---|---|---|---|
| `0x00` | `CMD_SET_EVENT_RECEIVER` | 2 | ADMIN |
| `0x01` | `CMD_GET_EVENT_RECEIVER` | 0 | USER |
| `0x02` | `CMD_PLATFORM_EVENT` | — | OPERATOR |
| `0x10` | `CMD_GET_PEF_CAPABILITIES` | 0 | USER |
| `0x11` | `CMD_ARM_PEF_POSTPONE_TIMER` | 1 | ADMIN |
| `0x12` | `CMD_SET_PEF_CONFIG_PARAMS` | — | ADMIN |
| `0x13` | `CMD_GET_PEF_CONFIG_PARAMS` | 3 | OPERATOR |
| `0x14` | `CMD_SET_LAST_PROCESSED_EVENT_ID` | 3 | ADMIN |
| `0x15` | `CMD_GET_LAST_PROCESSED_EVENT_ID` | 0 | ADMIN |
| `0x16` | `CMD_ALERT_IMMEDIATE` | — | ADMIN |
| `0x17` | `CMD_PET_ACKNOWLEDGE` | 12 | ANY |
| `0x20` | `CMD_GET_DEVICE_SDR_INFO` | — | USER |
| `0x21` | `CMD_GET_DEVICE_SDR` | 6 | USER |
| `0x22` | `CMD_RESERVE_DEVICE_SDR_REPOSITORY` | 0 | USER |
| `0x23` | `CMD_GET_SENSOR_READING_FACTORS` | 2 | USER |
| `0x24` | `CMD_SET_SENSOR_HYSTERESIS` | 4 | OPERATOR |
| `0x25` | `CMD_GET_SENSOR_HYSTERESIS` | 2 | USER |
| `0x26` | `CMD_SET_SENSOR_THRESHOLDS` | 8 | OPERATOR |
| `0x27` | `CMD_GET_SENSOR_THRESHOLDS` | 1 | USER |
| `0x28` | `CMD_SET_SENSOR_EVENT_ENABLE` | — | OPERATOR |
| `0x29` | `CMD_GET_SENSOR_EVENT_ENABLE` | 1 | USER |
| `0x2A` | `CMD_REARM_SENSOR_EVENTS` | — | OPERATOR |
| `0x2B` | `CMD_GET_SENSOR_EVENT_STATUS` | 1 | USER |
| `0x2D` | `CMD_GET_SENSOR_READING` | 1 | USER |
| `0x2E` | `CMD_SET_SENSOR_TYPE` | 3 | OPERATOR |
| `0x2F` | `CMD_GET_SENSOR_TYPE` | 1 | USER |
| `0x30` | `CMD_SET_SENSOR_READING_AND_EVENT_STATUS` | — | USER |

### NETFN_STORAGE (`0x0A` / `0x0B`) — Confirmed from `g_Storage_CmdHndlr`

| Code | Command | Min Req | Privilege |
|---|---|---|---|
| `0x10` | `CMD_GET_FRU_INVENTORY_AREA_INFO` | 1 | USER |
| `0x11` | `CMD_READ_FRU_DATA` | 4 | USER |
| `0x12` | `CMD_WRITE_FRU_DATA` | — | OPERATOR |
| `0x20` | `CMD_GET_SDR_REPOSITORY_INFO` | 0 | USER |
| `0x21` | `CMD_GET_SDR_REPOSITORY_ALLOC_INFO` | 0 | USER |
| `0x22` | `CMD_RESERVE_SDR_REPOSITORY` | 0 | USER |
| `0x23` | `CMD_GET_SDR` | 6 | USER |
| `0x24` | `CMD_ADD_SDR` | — | OPERATOR |
| `0x25` | `CMD_PARTIAL_ADD_SDR` | — | OPERATOR |
| `0x26` | `CMD_DELETE_SDR` | 4 | OPERATOR |
| `0x27` | `CMD_CLEAR_SDR_REPOSITORY` | 6 | OPERATOR |
| `0x28` | `CMD_GET_SDR_REPOSITORY_TIME` | 0 | USER |
| `0x29` | `CMD_SET_SDR_REPOSITORY_TIME` | 4 | OPERATOR |
| `0x2A` | `CMD_ENTER_SDR_UPDATE_MODE` | 0 | OPERATOR |
| `0x2B` | `CMD_EXIT_SDR_UPDATE_MODE` | 0 | OPERATOR |
| `0x2C` | `CMD_RUN_INITIALIZATION_AGENT` | 1 | OPERATOR |
| `0x40` | `CMD_GET_SEL_INFO` | 0 | USER |
| `0x41` | `CMD_GET_SEL_ALLOCATION_INFO` | 0 | USER |
| `0x42` | `CMD_RESERVE_SEL` | 0 | USER |
| `0x43` | `CMD_GET_SEL_ENTRY` | 6 | USER |
| `0x44` | `CMD_ADD_SEL_ENTRY` | 16 | OPERATOR |
| `0x45` | `CMD_PARTIAL_ADD_SEL_ENTRY` | — | OPERATOR |
| `0x46` | `CMD_DELETE_SEL_ENTRY` | 4 | OPERATOR |
| `0x47` | `CMD_CLEAR_SEL` | 6 | OPERATOR |
| `0x48` | `CMD_GET_SEL_TIME` | 0 | USER |
| `0x49` | `CMD_SET_SEL_TIME` | 4 | OPERATOR |
| `0x5A` | `CMD_GET_AUXILIARY_LOG_STATUS` | — | USER |
| `0x5B` | `CMD_SET_AUXILIARY_LOG_STATUS` | — | ADMIN |
| `0x5C` | `CMD_GET_SEL_TIME_UTC_OFFSET` | 0 | USER |
| `0x5D` | `CMD_SET_SEL_TIME_UTC_OFFSET` | 2 | OPERATOR |

### NETFN_TRANSPORT (`0x0C` / `0x0D`) — Confirmed from `g_Config_CmdHndlr`

| Code | Command | Min Req | Privilege |
|---|---|---|---|
| `0x01` | `CMD_SET_LAN_CONFIGURATION_PARAMETERS` | — | ADMIN |
| `0x02` | `CMD_GET_LAN_CONFIGURATION_PARAMETERS` | 4 | OPERATOR |
| `0x03` | `CMD_SUSPEND_BMC_ARPS` | 2 | ADMIN |
| `0x04` | `CMD_GET_IP_UDP_RMCP_STATISTICS` | — | ADMIN |
| `0x10` | `CMD_SET_SERIAL_MODEM_CONFIG` | — | ADMIN |
| `0x11` | `CMD_GET_SERIAL_MODEM_CONFIG` | 4 | OPERATOR |
| `0x12` | `CMD_SET_SERIAL_MODEM_MUX` | 2 | OPERATOR |
| `0x13` | `CMD_GET_TAP_RESPONSE_CODES` | 1 | USER |
| `0x18` | `CMD_SERIAL_MODEM_CONNECTION_ACTIVE` | 2 | OPERATOR |
| `0x19` | `CMD_CALLBACK` | 2 | ADMIN |
| `0x1A` | `CMD_SET_USER_CALLBACK_OPTIONS` | 7 | ADMIN |
| `0x1B` | `CMD_GET_USER_CALLBACK_OPTIONS` | 2 | USER |
| `0x21` | `CMD_SET_SOL_CONFIGURATION` | — | ADMIN |
| `0x22` | `CMD_GET_SOL_CONFIG_PARAMS` | 4 | USER |

### NETFN_AMI (`0x30` / `0x31`) — AMI OEM Commands

Code sources:
- ✓ = Confirmed from `g_AMI_CmdHndlr` binary table in `libipmimsghndlr.so`
- ★ = Confirmed from `/usr/local/lib/IPMI_AMI_VNCCONF.h` header
- PDK = Registered at runtime by `libipmipdkcmds.so` (not in core binary table)

#### Firmware Update (YAFU)

| Code | Command | Source | Privilege |
|---|---|---|---|
| `0x01` | `CMD_AMI_YAFU_ALLOCATE_MEMORY` | ✓ | `0x04` ADMIN |
| `0x02` | `CMD_AMI_YAFU_FREE_MEMORY` | ✓ | `0x04` ADMIN |
| `0x03` | `CMD_AMI_YAFU_READ_MEMORY` | ✓ | `0x04` ADMIN |
| `0x04` | `CMD_AMI_YAFU_WRITE_MEMORY` | ✓ | `0x04` ADMIN |
| `0x05` | `CMD_AMI_YAFU_COPY_MEMORY` | PDK | ADMIN |
| `0x06` | `CMD_AMI_YAFU_COMPARE_MEMORY` | PDK | ADMIN |
| `0x07` | `CMD_AMI_YAFU_CLEAR_MEMORY` | PDK | ADMIN |
| `0x08` | `CMD_AMI_YAFU_READ_FLASH` | PDK | ADMIN |
| `0x09` | `CMD_AMI_YAFU_WRITE_FLASH` | PDK | ADMIN |
| `0x0A` | `CMD_AMI_YAFU_ERASE_FLASH` | PDK | ADMIN |
| `0x0B` | `CMD_AMI_YAFU_VERIFY_FLASH` | PDK | ADMIN |
| `0x0C` | `CMD_AMI_YAFU_ACTIVATE_FLASH` | PDK | ADMIN |
| `0x0D` | `CMD_AMI_YAFU_GET_STATUS` | PDK | ADMIN |
| `0x0E` | `CMD_AMI_YAFU_GET_FIRMWARE_INFO` | PDK | ADMIN |
| `0x0F` | `CMD_AMI_YAFU_GET_FLASH_INFO` | PDK | ADMIN |
| `0x10` | `CMD_AMI_YAFU_ERASE_COPY_FLASH` | ✓ | `0xFF` disabled |

#### VNC / Remote KVM (confirmed from IPMI_AMI_VNCCONF.h)

| Code | Command | Source |
|---|---|---|
| `0x16` | `CMD_AMI_GET_VNC_CONF` | ★ |
| `0x17` | `CMD_AMI_SET_VNC_CONF` | ★ |
| `0xDD` | `CMD_AMI_GET_REMOTECLIENT_OPTION` | ★ |
| `0xDE` | `CMD_AMI_SET_REMOTECLIENT_OPTION` | ★ |

#### Remaining AMI OEM Commands

The full list of AMI OEM command codes active in this firmware is extracted from the `g_AMI_CmdHndlr` table in `libipmimsghndlr.so`. Complete name→code mapping requires AMI SDK source access; the codes below are all confirmed active in the firmware handler.

**Commands extracted from `g_AMI_CmdHndlr` (NETFN `0x30`):**

```
Code  Priv   Known/Likely Command
0x01  0x04   CMD_AMI_YAFU_ALLOCATE_MEMORY  ✓
0x02  0x04   CMD_AMI_YAFU_FREE_MEMORY      ✓
0x03  0x04   CMD_AMI_YAFU_READ_MEMORY      ✓
0x04  0x04   CMD_AMI_YAFU_WRITE_MEMORY     ✓
0x10  0xFF   CMD_AMI_YAFU_ERASE_COPY_FLASH ✓ (disabled/admin-gate)
0x1E  0x02   CMD_AMI_GET_SENSOR_INFO       (USER read — sensor data)
0x20  0x10   CMD_AMI_GET_FW_VERSION
0x21  0x10   CMD_AMI_GET_FW_PROTOCOL
0x22  0xFF   CMD_AMI_SET_FW_PROTOCOL
0x23  0xFF   CMD_AMI_YAFU_GET_STATUS (alt)
0x24  0x10   CMD_AMI_GET_FW_CONFIGURATION
0x25  0x11   CMD_AMI_SET_FW_CONFIGURATION
0x26  0x18   CMD_AMI_DUAL_IMG_SUPPORT
0x27  0x18   CMD_AMI_YAFU_DUAL_IMAGE_SUPT
0x28  0x0C   CMD_AMI_GET_PRESERVE_CONF
0x29  0x0C   CMD_AMI_SET_PRESERVE_CONF
0x2B  0x01   CMD_AMI_MANAGE_BMC_CONFIG
0x30  0x13   CMD_AMI_GET_SEL_POLICY
0x31  0xFF   CMD_AMI_SET_SEL_POLICY
0x32  0x18   CMD_AMI_GET_SEL_ENTIRES
0x33  0x18   CMD_AMI_GET_EXTEND_SEL_DATA
0x34  0x14   CMD_AMI_ADD_EXTEND_SEL_ENTIRES
0x35  0x14   CMD_AMI_PARTIAL_ADD_EXTEND_SEL_ENTIRES
0x3D  0x03   CMD_AMI_GET_LOG_CONF
0x3E  0x01   CMD_AMI_SET_LOG_CONF
0x3F  0x02   CMD_AMI_GET_EXTLOG_CONF
0x40  0x4D   CMD_AMI_SET_EXTLOG_CONF
0x41  0xFF   CMD_AMI_GET_SERIALLOG_CONF
0x42  0xFF   CMD_AMI_SET_SERIALLOG_CONF
0x50  0x0C   CMD_AMI_GET_SERVICE_CONF
0x51  0x0E   CMD_AMI_SET_SERVICE_CONF
0x52  0x01   CMD_AMI_GET_REMOTEKVM_CONF
0x53  0x01   CMD_AMI_SET_REMOTEKVM_CONF
0x54  0x10   CMD_AMI_GET_MEDIA_INFO
0x55  0x0D   CMD_AMI_SET_MEDIA_INFO
0x56  0x01   CMD_AMI_GET_VMEDIA_CONF
0x57  0xFF   CMD_AMI_SET_VMEDIA_CONF
0x58  0xFF   CMD_AMI_MEDIA_REDIRECTION_START_STOP
0x59  0x11   CMD_AMI_RIS_START_STOP
0x60  0x00   CMD_AMI_GET_DNS_CONF
0x62  0x01   CMD_AMI_SET_DNS_CONF
0x63  0x01   CMD_AMI_GET_V6DNS_CONF
0x64  0x41   CMD_AMI_SET_V6DNS_CONF
0x65  0xFF   CMD_AMI_GET_NTP_CFG
0x66  0x00   CMD_AMI_SET_NTP_CFG
0x67  0x00   CMD_AMI_GET_TIMEZONE
0x68  0xFF   CMD_AMI_SET_TIMEZONE
0x6B  0x02   CMD_AMI_GET_IFACE_STATE
0x6C  0xFF   CMD_AMI_SET_IFACE_STATE
0x70  0x01   CMD_AMI_GET_IPV6_ADDRESS
0x71  0xFF   CMD_AMI_GET_ETH_INDEX
0x72  0x03   CMD_AMI_LINK_DOWN_RESILENT
0x78  0xFF   CMD_AMI_GET_SNMP_CONF
0x79  0x04   CMD_AMI_SET_SNMP_CONF
0x7E  0x00   CMD_AMI_GET_FIREWALL
0x7F  0x01   CMD_AMI_SET_FIREWALL
0x80  0x01   CMD_AMI_GET_PAM_ORDER
0x81  0x01   CMD_AMI_SET_PAM_ORDER
0x82  0x41   CMD_AMI_GET_AD_CONF
0x85  0x04   CMD_AMI_SET_AD_CONF
0x86  0x00   CMD_AMI_GET_LDAP_CONF
0x8D  0x00   CMD_AMI_SET_LDAP_CONF
0x8E  0x10   CMD_AMI_GET_RADIUS_CONF
0x90  0x00   CMD_AMI_SET_RADIUS_CONF
0x91  0xFF   CMD_AMI_GET_EXTENDED_PRIV
0x92  0x01   CMD_AMI_SET_EXTENDED_PRIV
0x93  0x02   CMD_AMI_GET_ROOT_USER_ACCESS
0x94  0xFF   CMD_AMI_SET_ROOT_PASSWORD
0x95  0x01   CMD_AMI_RESET_PASS
0x96  0x00   CMD_AMI_GET_USER_SHELLTYPE
0x97  0x05   CMD_AMI_SET_USER_SHELLTYPE
0x98  0x00   CMD_AMI_GET_EMAIL_USER
0x99  0x03   CMD_AMI_SET_EMAIL_USER
0x9A  0xFF   CMD_AMI_SET_EMAILFORMAT_USER
0xA9  0xFF   CMD_AMI_GET_EMAILFORMAT_USER
0xB4  0x01   CMD_AMI_GET_TRIGGER_EVT
0xBE  0x01   CMD_AMI_SET_TRIGGER_EVT
0xC2  0xFF   CMD_AMI_GET_SOLTRIGGER_EVT
0xC3  0x01   CMD_AMI_SET_SOLTRIGGER_EVT
0xE6  0x00   CMD_AMI_GET_INVENTORY
0xE7  0x04   CMD_AMI_SET_INVENTORY
0xE8  0xFF   CMD_AMI_GET_FRU_DETAILS
0xE9  0x03   CMD_AMI_PECI_READ_WRITE
0xEC  0xFF   CMD_AMI_PSU_INFO
0xEE  0x01   CMD_AMI_MUX_SWITCHING
```

**Additional AMI OEM commands registered at runtime by PDK plugins (not in core table):**

```
CMD_AMI_GET_BIOS_FLAG               CMD_AMI_SET_BIOS_FLAG
CMD_AMI_GET_BIOS_COMMAND            CMD_AMI_SET_BIOS_COMMAND
CMD_AMI_GET_BIOS_RESPONSE           CMD_AMI_SET_BIOS_RESPONSE
CMD_AMI_GET_BIOS_CODE               CMD_AMI_BIOSRECOVERY
CMD_AMI_PLDM_BIOS_MSG               CMD_AMI_SEND_TO_BIOS
CMD_AMI_FIRMWAREUPDATE              CMD_AMI_GETRELEASENOTE
CMD_AMI_START_TFTP_FW_UPDATE        CMD_AMI_GET_TFTP_FW_PROGRESS_STATUS
CMD_AMI_FILE_UPLOAD                 CMD_AMI_FILE_DOWNLOAD
CMD_AMI_GET_FIRMWARE_RECOVERY_INFO  CMD_AMI_SET_FIRMWARE_RECOVERY_INFO
CMD_GET_SYSTEM_FIRMWARE_HEALTH_POWERCYCLE
CMD_AMI_GET_BACKUP_FLAG             CMD_AMI_SET_BACKUP_FLAG
CMD_AMI_GET_ALL_PRESERVE_CONF       CMD_AMI_SET_ALL_PRESERVE_CONF
CMD_AMI_RESTORE_DEF
CMD_AMI_GET_FEATURE_STATUS          CMD_AMI_GET_PEND_STATUS
CMD_AMI_GET_CHANNEL_TYPE            CMD_AMI_GET_CHANNEL_NUM
CMD_AMI_GET_KCS_LAN_INTERFACES_STATUS
CMD_AMI_SET_KCSLAN_IFC_SUP
CMD_AMI_GET_RUN_TIME_SINGLE_PORT_STATUS
CMD_AMI_SET_RUN_TIME_SINGLE_PORT_STATUS
CMD_ALWAYS_USE_SECURE_PORT
CMD_AMI_GET_SSL_CERT_STATUS         CMD_AMI_SET_SSL_CERT
CMD_AMI_ADD_LICENSE_KEY             CMD_AMI_GET_LICENSE_VALIDITY
CMD_AMI_GET_HOST_LOCK_FEATURE_STATUS
CMD_AMI_SET_HOST_LOCK_FEATURE_STATUS
CMD_AMI_GET_HOST_AUTO_LOCK_STATUS   CMD_AMI_SET_HOST_AUTO_LOCK_STATUS
CMD_AMI_GET_UDS_SESSION_INFO        CMD_AMI_GET_UDS_CHANNEL_INFO
CMD_AMI_GET_ALL_ACTIVE_SESSIONS     CMD_AMI_ACTIVE_SESSIONS_CLOSE
CMD_AMI_GET_IPMI_SESSION_TIMEOUT
CMD_AMI_GET_SMASHLITE_ACTIVE_SESS_CNT
CMD_AMI_SET_SMASHLITE_ACTIVE_SESS_CNT
CMD_AMI_CIM_SERVICE
CMD_AMI_GET_SDCARD_PART             CMD_AMI_SET_SDCARD_PART
CMD_AMI_SET_UBOOT_MEMTEST           CMD_AMI_GET_UBOOT_MEMTEST_STATUS
CMD_AMI_GET_USB_SWITCH_SETTING      CMD_AMI_SET_USB_SWITCH_SETTING
CMD_AMI_GET_RAID_INFO
CMD_AMI_GET_BMC_INSTANCE_COUNT
CMD_AMI_CTL_DBG_MSG                 CMD_AMI_GET_DBG_MSG_STATUS
CMD_AMI_GET_LOGIN_AUDIT_CFG         CMD_AMI_SET_LOGIN_AUDIT_CFG
CMD_AMI_GET_SOL_ARCHIEVE_DATA
CMD_AMI_PARTIAL_GET_EXTEND_SEL_ENTIRES
CMD_AMI_GET_RIS_CONF                CMD_AMI_SET_RIS_CONF
CMD_AMI_GET_VIDEO_RCD_CONF          CMD_AMI_SET_VIDEO_RCD_CONF
CMD_AMI_RESTART_WEB_SERVICE
CMD_AMI_GET_PWD_ENCRYPTION_KEY      CMD_AMI_SET_PWD_ENCRYPTION_KEY
CMD_SET_SMTP_CONFIG_PARAMS          CMD_GET_SMTP_CONFIG_PARAMS
CMD_SET_SSH_CONF                    CMD_GET_SSH_CONF
CMD_AMI_GET_SOL_CONFIG_PARAMS
CMD_AMI_GET_EXTEND_SEL_DATA
CMD_AMI_DUAL_IMG_SUPPORT
CMD_AMI_YAFU_GET_ECF_STATUS         CMD_AMI_YAFU_GET_VERIFY_STATUS
CMD_AMI_YAFU_GET_BOOT_VARS          CMD_AMI_YAFU_GET_BOOT_CONFIG
CMD_AMI_YAFU_SET_BOOT_CONFIG        CMD_AMI_YAFU_RESET_DEVICE
CMD_AMI_YAFU_ACTIVATE_FLASH_DEVICE  CMD_AMI_YAFU_DEACTIVATE_FLASH_MODE
CMD_AMI_YAFU_FIRMWARE_SELECT_FLASH  CMD_AMI_YAFU_SWITCH_FLASH_DEVICE
CMD_AMI_YAFU_RESTORE_FLASH_DEVICES  CMD_AMI_YAFU_PROTECT_FLASH
CMD_AMI_YAFU_COMMON_NAK             CMD_AMI_YAFU_MISCELLANEOUS_INFO
CMD_AMI_YAFU_SIGNIMAGEKEY_REPLACE   CMD_AMI_YAFU_GET_FMH_INFO
```

---

## Sync Agent — IPMI ↔ Redfish Bridge

Location: `/usr/local/sync-agent/`

A Lua-based daemon using **Redis pub/sub** as the internal message bus. It monitors Redis for IPMI state changes and synchronizes them into the Redfish data model (and vice versa).

### Architecture

```
IPMIMain  →  Redis  →  sync-agent  →  Redfish HTTP
    ↑                       |
    └───────────────────────┘
         (bidirectional via lua subagents)
```

### Core Files

| File | Description |
|---|---|
| `sync-agent` | Main sync-agent binary |
| `agent.lua` | Top-level agent orchestrator |
| `config.lua` | Agent configuration |
| `environment.lua` | Environment setup |
| `notify_mask_map.lua` | Redis key → notification mask |
| `notify_fn_map.lua` | Notification function dispatch |
| `redfish_intermediate_map.lua` | Intermediate Redfish key map |
| `redfish_group_map.lua` | Redfish resource grouping |
| `redis_init_helper.lua` | Redis initialization |
| `do_not_delay_notify_entries.lua` | High-priority notification keys |
| `procmonitor-syncagent.lua` | Process monitoring |

### IPMI Extension (`extensions/ipmi/`)

| File | Description |
|---|---|
| `ipmi_commands.lua` | All IPMI command constants (all NetFns) |
| `ipmi_constants.lua` | IPMI protocol constants |
| `ipmi_completion_codes.lua` | Completion code definitions |
| `ipmi_config.lua` | IPMI extension config |
| `ipmi_map.lua` | Maps IPMI SET commands → Redfish data model keys |
| `oem-ipmi_map.lua` | OEM command → Redfish mapping |
| `ipmi_utils.lua` | IPMI utility functions |
| `ipmi-sync-helpers.lua` | Sync helper functions |
| `libipmi.lua` | FFI bindings to `libipmi.so` |
| `readable_sel_event_records.lua` | SEL record decoder |

**IPMI → Redfish mappings (from `ipmi_map.lua`):**

| IPMI Code | Command | Redfish Key |
|---|---|---|
| `NETFN_SENSOR` `0x26` | `CMD_SET_SENSOR_THRESHOLDS` | `sensors.update_power_thermal` |
| `NETFN_CHASSIS` `0x02` | `CMD_CHASSIS_CONTROL` | `Chassis.get_chassis_status` |
| `NETFN_CHASSIS` `0x0A` | `CMD_SET_SYSTEM_BOOT_OPTIONS` | `Chassis.get_boot_options` |
| `NETFN_CHASSIS` `0x04` | `CMD_SET_CHASSIS_IDENTITY` | `Chassis.get_chassis_identify_status` |
| `NETFN_GROUP_EXTN` `0x2C` | `CMD_DCMI_SET_POWER_LIMIT` | `Chassis.get_power_limit` |
| `NETFN_STORAGE` `0x12` | `CMD_WRITE_FRU_DATA` | `storage.get_FRU_Info` |
| `NETFN_STORAGE` `0x47` | `CMD_CLEAR_SEL` | `storage.clear_SEL_entries` |
| `NETFN_STORAGE` `0x46` | `CMD_DELETE_SEL_ENTRY` | `storage.delete_SEL_entry` |
| `NETFN_STORAGE` `0x5D` | `CMD_SET_SEL_TIME_UTC_OFFSET` | `storage.get_SEL_DateTime` |
| `NETFN_STORAGE` `0x49` | `CMD_SET_SEL_TIME` | `storage.get_SEL_DateTime` |
| `NETFN_TRANSPORT` `0x21` | `CMD_SET_SOL_CONFIGURATION` | `transport.get_sol_config` |
| `NETFN_AMI` `0x66` | `CMD_AMI_SET_NTP_CFG` | `storage.get_SEL_DateTime` |
| `NETFN_AMI` `0x68` | `CMD_AMI_SET_TIMEZONE` | `storage.get_SEL_DateTime` |
| `NETFN_AMI` `0x60` | `CMD_AMI_SET_DNS_CONF` | `ami.get_dns_config` |
| `NETFN_AMI` `0x51` | `CMD_AMI_SET_SERVICE_CONF` | `ami.get_service_config` |
| `NETFN_AMI` `0x53` | `CMD_AMI_SET_REMOTEKVM_CONF` | `ami.get_service_config` |
| `NETFN_AMI` `0x57` | `CMD_AMI_SET_VMEDIA_CONF` | `ami.get_vmedia_config` |
| `NETFN_AMI` `0x58` | `CMD_AMI_MEDIA_REDIRECTION_START_STOP` | `ami.get_vmedia_config` |
| `NETFN_AMI` `0x6C` | `CMD_AMI_SET_IFACE_STATE` | `ami.get_eth_interface_state` |
| `NETFN_AMI` `0xE7` | `CMD_AMI_SET_INVENTORY` | `ami.get_system_inventory` |
| `NETFN_AMI` `0x81` | `CMD_AMI_SET_PAM_ORDER` | `ami.get_pam_order` |
| `NETFN_APP` `0x41` | `CMD_SET_USER_ACCESS` | `user.get_user_privileges` |
| `NETFN_APP` `0x45` | `CMD_SET_USER_PASSWORD` | `user.get_userpasswords` |
| `NETFN_APP` `0x43` | `CMD_SET_USERNAME` | `user.get_usernames` |

### IPMI Subagents (`extensions/ipmi/subagents/`)

| File | Description |
|---|---|
| `ipmi2redfish.lua` | Push IPMI state changes → Redfish resources |
| `ipmi2redfish_handler.lua` | Handler dispatch for ipmi2redfish |
| `ipmiredfishusersync.lua` | Sync IPMI user accounts ↔ Redfish accounts |

### IPMI Sync Helpers (`extensions/ipmi/sync-helpers/`)

| File | Description |
|---|---|
| `ipmisel.lua` | SEL entry synchronization to Redfish EventLog |

### AMI Extension (`extensions/ami/`)

| File | Description |
|---|---|
| `libipmi_ami.lua` | AMI-specific IPMI library bindings |
| `ipmi-sync-helpers_ami.lua` | AMI OEM sync helpers |
| `libipmi_ami/hpm_fwupdate_ipmi.lua` | HPM firmware update via IPMI |
| `libipmi_ami/vmedia_update_ipmi.lua` | Virtual media update via IPMI |
| `cdefs/libipmi_ffi.lua` | FFI C definitions for libipmi (AMI variant) |

### Update Service Extension (`extensions/update_service/`)

| File | Description |
|---|---|
| `ipmi-sync-helpers_update.lua` | Firmware update sync helpers |

---

## OpenBMC Port Implications

For the X570D4I-2T OpenBMC port, the following mappings from AMI firmware to OpenBMC equivalents are needed:

| AMI Component | OpenBMC Equivalent | Notes |
|---|---|---|
| `libipmipar.so` sensor HAL | `phosphor-hwmon` / dbus sensor objects | Translate all `dev_asrr_*` devices |
| `dev_bmc_fru` | `phosphor-ipmi-fru` | EEPROM on I2C bus 7 |
| YAFU MDR memory ops (`0x01`–`0x10`) | `phosphor-ipmi-blobs` | Host firmware upload mechanism |
| `CMD_AMI_MUX_SWITCHING` (`0xEE`) | OEM IPMI handler plugin | Controls KVM/GPIOJ1 mux |
| `CMD_AMI_PECI_READ_WRITE` (`0xE9`) | `phosphor-ipmi-host` OEM handler | CPU temperature via PECI |
| `CMD_AMI_PSU_INFO` (`0xEC`) | `phosphor-psu-monitor` | PSU status |
| SEL sensor `0xF9` / `0xFA` | Custom discrete sensor | Watchdog/health aggregate |
| NCT6796D (`dev_nct6796d_v0dot1`) | `w83627hf` / `nct6779` hwmon driver | Already has kernel driver |
| NVMe (`dev_asrrnvmev01`) | `nvme-mi` or custom hwmon | NVMe MI over SMBus |
| PCA9545 mux | `i2c-mux-pca954x` kernel driver | I2C mux, channel 3 has semaphore |
| Sync agent IPMI↔Redfish | `bmcweb` + `phosphor-dbus-interfaces` | Redfish is native in OpenBMC |
| DCMI (`libipmidcmi.so`, NetFn `0x2C`) | `phosphor-ipmi-host` built-in DCMI | Enabled by default |
| LAN config (`libipmilan.so`, `0x0C`/`0x01`) | `phosphor-network` + IPMI LAN handler | Standard OpenBMC |

### Critical OEM Commands for Port

These AMI OEM commands (NETFN `0x30`) have no direct OpenBMC equivalent and need custom `phosphor-ipmi-host` OEM handler plugins:

1. **`0xEE` `CMD_AMI_MUX_SWITCHING`** — KVM mux control (maps to GPIOJ1 toggle, documented in VGA/KVM fix)
2. **`0xE9` `CMD_AMI_PECI_READ_WRITE`** — Direct PECI access for CPU thermals
3. **`0xEC` `CMD_AMI_PSU_INFO`** — PSU data aggregation
4. **`CMD_AMI_GET/SET_BIOS_COMMAND/RESPONSE`** — BIOS communication channel (codes in PDK layer)
5. **`CMD_AMI_SEND_TO_BIOS`** — Direct BIOS messaging (code in PDK layer)
6. **`0x01`–`0x10` YAFU commands** — Replace with `phosphor-ipmi-blobs` for firmware upload
