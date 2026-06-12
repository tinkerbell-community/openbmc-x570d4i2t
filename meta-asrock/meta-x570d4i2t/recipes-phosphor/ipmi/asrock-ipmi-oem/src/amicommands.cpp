// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Named handlers for the AMI management command set (NETFN_AMI = 0x32),
// mirrored from the stock firmware's sync-agent ipmi_commands.lua AMI_CMD table.
//
// On the stock BMC these are the proprietary AMI web / sync-agent management
// commands (LDAP, KVM, virtual media, users, firmware update, SOL, SNMP, …).
// Under OpenBMC the AMI sync-agent is NOT present — bmcweb/Redfish owns all of
// that functionality — so there is no real work to do here. These are success
// STUBS: each command is registered with its name so every request is captured
// by name in the journal and ACKed, which keeps any AMI/IPMI client from
// CC-erroring and retrying. Register at prioOpenBmcBase so the real MDR SMBIOS
// handlers on NetFn 0x32 (registered at prioOemBase in oemcommands.cpp —
// 0x31/0x3D/0x51/0x52/0x53/0x5D/0x71/0x72/0xA0/0xA1/0xB2/0xB5/0xF3) always win.

#include <amicommands.hpp>

#include <ipmid/api.hpp>
#include <phosphor-logging/log.hpp>

#include <vector>

namespace asrock
{

using phosphor::logging::entry;
using phosphor::logging::level;
using phosphor::logging::log;

namespace
{

// NETFN_AMI = 50 (0x32) per ipmi_commands.lua.
constexpr ipmi::NetFn netFnAmi = 0x32;

struct AmiCmd
{
    uint8_t     code;
    const char* name;
};

// Generated from sync-agent ipmi_commands.lua AMI_CMD (NETFN_AMI). Do not edit
// by hand — regenerate from the lua if the command table changes.
constexpr AmiCmd kAmiCmds[] = {
    {0x01, "YAFU_GET_FLASH_INFO"},
    {0x02, "YAFU_GET_FIRMWARE_INFO"},
    {0x03, "YAFU_GET_FMH_INFO"},
    {0x04, "YAFU_GET_STATUS"},
    {0x10, "YAFU_ACTIVATE_FLASH"},
    {0x11, "GET_SSH_CONF"},
    {0x12, "SET_SSH_CONF"},
    {0x20, "YAFU_ALLOCATE_MEMORY"},
    {0x21, "YAFU_FREE_MEMORY"},
    {0x22, "YAFU_READ_FLASH"},
    {0x23, "YAFU_WRITE_FLASH"},
    {0x24, "YAFU_ERASE_FLASH"},
    {0x25, "YAFU_PROTECT_FLASH"},
    {0x26, "YAFU_ERASE_COPY_FLASH"},
    {0x27, "YAFU_VERIFY_FLASH"},
    {0x28, "YAFU_GET_ECF_STATUS"},
    {0x29, "YAFU_GET_VERIFY_STATUS"},
    {0x30, "YAFU_READ_MEMORY"},
    {0x31, "YAFU_WRITE_MEMORY"},
    {0x32, "YAFU_COPY_MEMORY"},
    {0x33, "YAFU_COMPARE_MEMORY"},
    {0x34, "YAFU_CLEAR_MEMORY"},
    {0x3F, "SET_KCSLAN_IFC_SUP"},
    {0x40, "YAFU_GET_BOOT_CONFIG"},
    {0x41, "YAFU_SET_BOOT_CONFIG"},
    {0x42, "YAFU_GET_BOOT_VARS"},
    {0x50, "YAFU_DEACTIVATE_FLASH_MODE"},
    {0x51, "YAFU_RESET_DEVICE"},
    {0x52, "YAFU_SWITCH_FLASH_DEVICE"},
    {0x53, "YAFU_RESTORE_FLASH_DEVICE"},
    {0x54, "YAFU_DUAL_IMAGE_SUP"},
    {0x55, "YAFU_FIRMWARE_SELECT_FLASH"},
    {0x56, "YAFU_ACTIVATE_FLASH_DEVICE"},
    {0x57, "FILE_UPLOAD"},
    {0x58, "FILE_DOWNLOAD"},
    {0x59, "YAFU_MISCELLANEOUS_INFO"},
    {0x5A, "SET_INVENTORY"},
    {0x5B, "GET_INVENTORY"},
    {0x60, "GET_CHANNEL_NUM"},
    {0x62, "GET_ETH_INDEX"},
    {0x63, "GET_EMAIL_USER"},
    {0x64, "SET_EMAIL_USER"},
    {0x65, "RESET_PASS"},
    {0x66, "RESTORE_DEF"},
    {0x67, "GET_LOG_CONF"},
    {0x68, "SET_LOG_CONF"},
    {0x69, "GET_SERVICE_CONF"},
    {0x6A, "SET_SERVICE_CONF"},
    {0x6B, "GET_DNS_CONF"},
    {0x6C, "SET_DNS_CONF"},
    {0x6D, "ALWAYS_USE_SECURE_PORT"},
    {0x70, "LINK_DOWN_RESILENT"},
    {0x71, "SET_IFACE_STATE"},
    {0x72, "GET_IFACE_STATE"},
    {0x73, "GET_BIOS_CODE"},
    {0x74, "GET_V6DNS_CONF"},
    {0x75, "SET_V6DNS_CONF"},
    {0x76, "SET_FIREWALL"},
    {0x77, "GET_FIREWALL"},
    {0x78, "SET_SMTP_CONFIG_PARAMS"},
    {0x79, "GET_SMTP_CONFIG_PARAMS"},
    {0x7A, "SET_PAM_ORDER"},
    {0x7B, "GET_PAM_ORDER"},
    {0x7C, "GET_SNMP_CONF"},
    {0x7D, "SET_SNMP_CONF"},
    {0x7E, "GET_SEL_POLICY"},
    {0x7F, "SET_SEL_POLICY"},
    {0x80, "GET_FRU_DETAILS"},
    {0x81, "GET_EMAILFORMAT_USER"},
    {0x82, "SET_EMAILFORMAT_USER"},
    {0x83, "SET_PRESERVE_CONF"},
    {0x84, "GET_PRESERVE_CONF"},
    {0x85, "GET_SEL_ENTIRES"},
    {0x86, "GET_SENSOR_INFO"},
    {0x87, "START_TFTP_FW_UPDATE"},
    {0x88, "GET_TFTP_FW_PROGRESS_STATUS"},
    {0x89, "SET_FW_CONFIGURATION"},
    {0x8A, "GET_FW_CONFIGURATION"},
    {0x8B, "SET_FW_PROTOCOL"},
    {0x8C, "GET_FW_PROTOCOL"},
    {0x8D, "GET_IPMI_SESSION_TIMEOUT"},
    {0x8E, "GET_UDS_CHANNEL_INFO"},
    {0x8F, "DUAL_IMG_SUPPORT"},
    {0x90, "GET_ROOT_USER_ACCESS"},
    {0x91, "SET_ROOT_PASSWORD"},
    {0x92, "GET_USER_SHELLTYPE"},
    {0x93, "SET_USER_SHELLTYPE"},
    {0x94, "SET_TRIGGER_EVT"},
    {0x95, "GET_TRIGGER_EVT"},
    {0x96, "GET_SOL_CONFIG_PARAMS"},
    {0x97, "SET_LOGIN_AUDIT_CFG"},
    {0x98, "GET_LOGIN_AUDIT_CFG"},
    {0x99, "GET_IPV6_ADDRESS"},
    {0x9A, "GET_UDS_SESSION_INFO"},
    {0x9B, "SET_PWD_ENCRYPTION_KEY"},
    {0x9C, "SET_UBOOT_MEMTEST"},
    {0x9D, "GET_UBOOT_MEMTEST_STATUS"},
    {0x9E, "GET_RIS_CONF"},
    {0x9F, "SET_RIS_CONF"},
    {0xA0, "RIS_START_STOP"},
    {0xA1, "CTL_DBG_MSG"},
    {0xA2, "GET_DBG_MSG_STATUS"},
    {0xA3, "SET_EXTENDED_PRIV"},
    {0xA4, "GET_EXTENDED_PRIV"},
    {0xA5, "SET_TIMEZONE"},
    {0xA6, "GET_TIMEZONE"},
    {0xA7, "GET_NTP_CFG"},
    {0xA8, "SET_NTP_CFG"},
    {0xA9, "YAFU_SIGNIMAGEKEY_REPLACE"},
    {0xAA, "VIRTUAL_DEVICE_SET_STATUS"},
    {0xAB, "VIRTUAL_DEVICE_GET_STATUS"},
    {0xAC, "ADD_LICENSE_KEY"},
    {0xAD, "GET_LICENSE_VALIDITY"},
    {0xAE, "GET_HOST_LOCK_FEATURE_STATUS"},
    {0xAF, "SET_HOST_LOCK_FEATURE_STATUS"},
    {0xB0, "GET_ALL_ACTIVE_SESSIONS"},
    {0xB1, "ACTIVE_SESSIONS_CLOSE"},
    {0xB4, "GET_FW_VERSION"},
    {0xB5, "GET_VIDEO_RCD_CONF"},
    {0xB6, "SET_VIDEO_RCD_CONF"},
    {0xB7, "GET_RUN_TIME_SINGLE_PORT_STATUS"},
    {0xB8, "SET_RUN_TIME_SINGLE_PORT_STATUS"},
    {0xBA, "SET_ALL_PRESERVE_CONF"},
    {0xBB, "GET_ALL_PRESERVE_CONF"},
    {0xBC, "GET_HOST_AUTO_LOCK_STATUS"},
    {0xBD, "SET_HOST_AUTO_LOCK_STATUS"},
    {0xBE, "GET_CHANNEL_TYPE"},
    {0xBF, "PECI_READ_WRITE"},
    {0xC0, "GET_REMOTEKVM_CONF"},
    {0xC1, "SET_REMOTEKVM_CONF"},
    {0xC2, "GET_FEATURE_STATUS"},
    {0xC3, "GET_SSL_CERT_STATUS"},
    {0xC4, "GET_AD_CONF"},
    {0xC5, "SET_AD_CONF"},
    {0xC6, "GET_RADIUS_CONF"},
    {0xC7, "SET_RADIUS_CONF"},
    {0xC8, "GET_LDAP_CONF"},
    {0xC9, "SET_LDAP_CONF"},
    {0xCA, "GET_VMEDIA_CONF"},
    {0xCB, "SET_VMEDIA_CONF"},
    {0xCC, "ADD_EXTEND_SEL_ENTIRES"},
    {0xCD, "GET_EXTEND_SEL_DATA"},
    {0xCE, "SEND_TO_BIOS"},
    {0xCF, "GET_BIOS_COMMAND"},
    {0xD1, "SET_BIOS_RESPONSE"},
    {0xD2, "GET_BIOS_RESPONSE"},
    {0xD3, "SET_BIOS_FLAG"},
    {0xD4, "GET_BIOS_FLAG"},
    {0xD5, "PLDM_BIOS_MSG"},
    {0xD7, "MEDIA_REDIRECTION_START_STOP"},
    {0xD8, "GET_MEDIA_INFO"},
    {0xD9, "SET_MEDIA_INFO"},
    {0xDA, "GET_SDCARD_PART"},
    {0xDB, "SET_SDCARD_PART"},
    {0xE1, "SET_EXTLOG_CONF"},
    {0xE2, "GET_EXTLOG_CONF"},
    {0xE3, "SET_BACKUP_FLAG"},
    {0xE4, "GET_BACKUP_FLAG"},
    {0xE5, "MANAGE_BMC_CONFIG"},
    {0xE6, "RESTART_WEB_SERVICE"},
    {0xE7, "GET_PEND_STATUS"},
    {0xE8, "FIRMWAREUPDATE"},
    {0xE9, "GETRELEASENOTE"},
    {0xEA, "BIOSRECOVERY"},
    {0xEB, "GET_BMC_INSTANCE_COUNT"},
    {0xEC, "SET_SSL_CERT"},
    {0xED, "SET_USB_SWITCH_SETTING"},
    {0xEE, "MUX_SWITCHING"},
    {0xEF, "GET_RAID_INFO"},
    {0xF0, "PARTIAL_ADD_EXTEND_SEL_ENTIRES"},
    {0xF1, "PARTIAL_GET_EXTEND_SEL_ENTIRES"},
    {0xF2, "GET_USB_SWITCH_SETTING"},
    {0xF3, "GET_FIRMWARE_RECOVERY_INFO"},
    {0xF4, "SET_FIRMWARE_RECOVERY_INFO"},
    {0xF5, "SET_SERIALLOG_CONF"},
    {0xF6, "GET_SERIALLOG_CONF"},
    {0xF7, "SET_SOLTRIGGER_EVT"},
    {0xF8, "GET_SOLTRIGGER_EVT"},
    {0xF9, "PSU_INFO"},
    {0xFA, "CIM_SERVICE"},
    {0xFB, "GET_SMASHLITE_ACTIVE_SESS_CNT"},
    {0xFC, "SET_SMASHLITE_ACTIVE_SESS_CNT"},
    {0xFD, "GET_SYSTEM_FIRMWARE_HEALTH_POWERCYCLE"},
    {0xFE, "GET_SOL_ARCHIEVE_DATA"},
    {0xFF, "YAFU_COMMON_NAK"},
};

// Generic AMI stub: log the named command + payload, ACK with a 1-byte status.
ipmi::RspType<std::vector<uint8_t>>
    ipmiAmiNamedStub(ipmi::Context::ptr ctx, std::vector<uint8_t> data)
{
    log<level::INFO>("AMI cmd (NetFn 0x32)",
                     entry("NAME=%s", amiCommandName(ctx->cmd)),
                     entry("CMD=0x%02X", ctx->cmd),
                     entry("LEN=%zu", data.size()));
    return ipmi::responseSuccess(std::vector<uint8_t>{0x00});
}

// Runs before main() via the constructor attribute (same pattern as
// registerOEMFunctions in oemcommands.cpp).
void registerAmiCommands() __attribute__((constructor));
void registerAmiCommands()
{
    // Cover the entire NetFn 0x32 command space so no AMI command CC-errors;
    // amiCommandName() supplies the name for known codes. MDR handlers on 0x32
    // sit at prioOemBase and override the matching codes here.
    for (unsigned c = 0x01; c <= 0xFE; ++c)
    {
        ipmi::registerHandler(ipmi::prioOpenBmcBase, netFnAmi,
                              static_cast<ipmi::Cmd>(c),
                              ipmi::Privilege::Admin, ipmiAmiNamedStub);
    }
    log<level::INFO>(
        "ASRock AMI NetFn 0x32 named stub handlers registered (prioOpenBmcBase)");
}

} // namespace

const char* amiCommandName(uint8_t cmd)
{
    for (const auto& e : kAmiCmds)
    {
        if (e.code == cmd)
        {
            return e.name;
        }
    }
    return "AMI_UNKNOWN";
}

} // namespace asrock
