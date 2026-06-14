// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Redfish Host Interface (RHI) credential-bootstrap IPMI commands.
//
// These reproduce the stock AMI MegaRAC firmware's
// libipmiredfishhostiface.so (symbols g_coreHIGroup / g_coreHIGroup_CmdHndlr,
// gated on CONFIG_SPX_FEATURE_REDFISH_ENABLED). They implement the DMTF
// Redfish Host Interface Specification (DSP0270) credential-bootstrapping
// flow over KCS IPMI:
//
//   NetFn 0x2C (Group Extension), Group Extension ID 0x52 ('R' = Redfish)
//     Cmd 0x01  Get Manager Certificate Fingerprint   (GetManagerCertificateFingerprint)
//     Cmd 0x02  Get Bootstrap Account Credentials      (GetBootstrapAccountCredentials)
//
// WHY THIS MATTERS: during POST the host BIOS (AMI RedfishHi/FirmwareConfigDrv
// driver chain) sends NetFn 0x2C / Cmd 0x01 FIRST. If the BMC answers
// 0xC1 (invalid command) the BIOS ABORTS the bootstrap sequence and never
// requests credentials (Cmd 0x02), so it never authenticates to bmcweb over
// usb0 (169.254.0.17) and the Redfish Host Interface never activates. Stock
// OpenBMC implements neither command; this provider adds them.
//
// Exact response framing taken from arm-none-eabi-objdump of the stock lib:
//   Cmd 0x01: pRes = [CC][0x52][0x01][<fingerprint>], total len 53 (0x35);
//             CC=0xCB if request "certificate number" byte != 1.
//   Cmd 0x02: pRes = [CC][0x52][Username 16B][Password 16B], total len 34.
// The OpenBMC group framework consumes/echoes the group byte (0x52) itself,
// so handlers below return only the bytes AFTER the group byte.

#include <ipmid/api.hpp>
#include <ipmid/message.hpp>
#include <ipmid/types.hpp>
#include <ipmid/utils.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <security/pam_appl.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace asrock
{
namespace rhi
{

using phosphor::logging::entry;
using phosphor::logging::level;
using phosphor::logging::log;

// DMTF Redfish Host Interface group extension.
//
// The BIOS sends NetFn 0x2C, Cmd 0x01, data=[0x00] (group byte 0x00), but
// the stock MegaRAC firmware hard-codes 0x52 ('R' = DMTF Redfish) as the
// response group byte. The BIOS rejects responses with group 0x00.
//
// We register under group 0x00 (so the dispatcher matches the BIOS request)
// but override ctx->group to 0x52 inside the handler. The patched ipmid
// (0001-group-ext-use-ctx-group-for-response-echo.patch) uses ctx->group
// for the response echo byte instead of the raw request byte.
static constexpr ipmi::Group groupRedfish = 0x00;
static constexpr ipmi::Group groupRedfishResponse = 0x52;

static constexpr ipmi::Cmd cmdGetMgrCertFingerprint = 0x01;
static constexpr ipmi::Cmd cmdGetBootstrapCreds = 0x02;

// bmcweb's TLS server certificate (used for the host-interface HTTPS endpoint).
static constexpr const char* kBmcwebCertPath =
    "/etc/ssl/certs/https/server.pem";

// DSP0270 fingerprint hash-type identifiers.
static constexpr uint8_t fingerprintTypeSha256 = 0x01;

// Stock MegaRAC fingerprint length: 50 bytes of base64 cert body.
static constexpr size_t kFingerprintLen = 50;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Read N cryptographically-random bytes from /dev/urandom (matches the stock
// firmware's generate_random_number / Generate16ByteRandomAlpNumPasswd path).
static bool randomBytes(uint8_t* out, size_t n)
{
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.good())
    {
        return false;
    }
    urandom.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(n));
    return static_cast<size_t>(urandom.gcount()) == n;
}

// Generate a 16-character alphanumeric token (A-Z a-z 0-9), matching
// Generate16ByteRandomAlpNumPasswd().
static bool random16Alnum(std::array<uint8_t, 16>& out)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::array<uint8_t, 16> raw{};
    if (!randomBytes(raw.data(), raw.size()))
    {
        return false;
    }
    for (size_t i = 0; i < out.size(); ++i)
    {
        out[i] = static_cast<uint8_t>(alphabet[raw[i] % (sizeof(alphabet) - 1)]);
    }
    return true;
}

// Minimal PAM conversation that injects a fixed password, mirroring bmcweb's
// pamUpdatePassword(). Used to set the bootstrap account's password in the
// shadow database via the "passwd" PAM stack.
static int pamConversation(int numMsg, const struct pam_message** msg,
                           struct pam_response** resp, void* appdataPtr)
{
    if (appdataPtr == nullptr || numMsg <= 0 ||
        numMsg > PAM_MAX_NUM_MSG)
    {
        return PAM_CONV_ERR;
    }
    auto* replies = static_cast<struct pam_response*>(
        calloc(static_cast<size_t>(numMsg), sizeof(struct pam_response)));
    if (replies == nullptr)
    {
        return PAM_BUF_ERR;
    }
    const char* pass = static_cast<const char*>(appdataPtr);
    for (int i = 0; i < numMsg; ++i)
    {
        if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
            msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
        {
            replies[i].resp = strdup(pass);
        }
    }
    *resp = replies;
    return PAM_SUCCESS;
}

static bool pamSetPassword(const std::string& user, const std::string& pass)
{
    // Non-const buffer required by PAM conversation appdata contract.
    std::vector<char> buf(pass.begin(), pass.end());
    buf.push_back('\0');
    const struct pam_conv conv = {pamConversation, buf.data()};
    pam_handle_t* pamh = nullptr;
    int ret = pam_start("passwd", user.c_str(), &conv, &pamh);
    if (ret != PAM_SUCCESS)
    {
        return false;
    }
    ret = pam_chauthtok(pamh, PAM_SILENT);
    pam_end(pamh, ret);
    return ret == PAM_SUCCESS;
}

// Extract the first 50 bytes of the base64 certificate body from the BMC's TLS
// server certificate PEM file. This matches the stock MegaRAC firmware's
// GetManagerCertificateFingerprint, which reads the server PEM, takes the
// certificate's base64 body, and copies the first 50 bytes verbatim.
//
// NOTE: the stock firmware reads a cert-only /conf/certs/server.pem, but
// bmcweb's /etc/ssl/certs/https/server.pem is a COMBINED PEM that holds the
// private key block BEFORE the certificate block. We therefore capture only
// the base64 between the "BEGIN CERTIFICATE"/"END CERTIFICATE" markers — never
// the private-key body — so the fingerprint reflects the cert the BIOS will
// see over TLS at https://169.254.0.17 (and never leaks key material).
static bool certFingerprintBase64Prefix(std::vector<uint8_t>& out)
{
    std::ifstream cert(kBmcwebCertPath);
    if (!cert.good())
    {
        return false;
    }

    std::string base64Body;
    std::string line;
    bool inCert = false;
    while (std::getline(cert, line))
    {
        if (line.find("BEGIN CERTIFICATE") != std::string::npos)
        {
            inCert = true;
            continue;
        }
        if (line.find("END CERTIFICATE") != std::string::npos)
        {
            break;
        }
        if (inCert)
        {
            base64Body += line;
        }
    }

    if (base64Body.size() < kFingerprintLen)
    {
        log<level::ERR>("RHI: cert base64 body too short",
                        entry("LEN=%zu", base64Body.size()));
        return false;
    }

    out.assign(base64Body.begin(),
               base64Body.begin() + static_cast<std::ptrdiff_t>(kFingerprintLen));
    return true;
}

// ---------------------------------------------------------------------------
// NetFn 0x2C / Group 0x52 / Cmd 0x01 — Get Manager Certificate Fingerprint
//
// Request (after group byte): [certificateNumber]  (must be 1)
// Response (after group byte): [fingerprintHashType][fingerprint...]
// Stock returns hashType=0x01 then a 50-byte fingerprint (first 50 bytes of
// the PEM certificate's base64 body, total 53 incl CC+grp). We match this
// format exactly. The host uses this to verify the BMC's TLS cert.
// ---------------------------------------------------------------------------
ipmi::RspType<uint8_t, std::vector<uint8_t>>
    getManagerCertificateFingerprint(
        ipmi::Context::ptr& ctx,
        std::optional<uint8_t> certificateNumber)
{
    // Override the response group byte from 0x00 (what the BIOS sends) to 0x52
    // (what the stock MegaRAC firmware returns). The patched ipmid uses
    // ctx->group for the response echo byte.
    ctx->group = groupRedfishResponse;

    // The BIOS on this board sends no certificate-number byte (request is just
    // the group byte). Treat absent as cert #1. If a number IS supplied it must
    // be 1, else CC 0xCB (matches stock).
    uint8_t certNum = certificateNumber.value_or(1);
    log<level::INFO>(
        "RHI 0x2C/00/01 GetManagerCertificateFingerprint",
        entry("CERT_NUM=%u", certNum));

    if (certNum != 1)
    {
        return ipmi::response(0xCB);
    }

    std::vector<uint8_t> fingerprint;
    if (!certFingerprintBase64Prefix(fingerprint))
    {
        log<level::ERR>(
            "RHI: failed to read bmcweb TLS cert base64 body",
            entry("PATH=%s", kBmcwebCertPath));
        // 0xCE = command response could not be provided.
        return ipmi::response(0xCE);
    }

    return ipmi::responseSuccess(fingerprintTypeSha256, fingerprint);
}

// ---------------------------------------------------------------------------
// NetFn 0x2C / Group 0x52 / Cmd 0x02 — Get Bootstrap Account Credentials
//
// Request (after group byte): [disableBootstrappingControl]
//      0xA5 => disable credential bootstrapping after issuing these creds.
// Response (after group byte): [Username 16B][Password 16B]
//
// Creates a temporary privileged BMC account (the stock firmware stores a
// Redfish "Internal Account"/BootstrapAccount in redis with a random
// user+password and a role). We create a real phosphor-user-manager user so
// the host can authenticate to bmcweb over the usb0 host interface.
// ---------------------------------------------------------------------------
ipmi::RspType<std::array<uint8_t, 16>, std::array<uint8_t, 16>>
    getBootstrapAccountCredentials(
        ipmi::Context::ptr& ctx,
        std::optional<uint8_t> disableBootstrapControlOpt)
{
    // Override response group byte to 0x52 (same as cmd 0x01).
    ctx->group = groupRedfishResponse;
    uint8_t disableBootstrapControl = disableBootstrapControlOpt.value_or(0);
    log<level::INFO>(
        "RHI 0x2C/00/02 GetBootstrapAccountCredentials",
        entry("DISABLE_CTRL=0x%02X", disableBootstrapControl));

    std::array<uint8_t, 16> userArr{};
    std::array<uint8_t, 16> passArr{};
    if (!random16Alnum(userArr) || !random16Alnum(passArr))
    {
        log<level::ERR>("RHI: /dev/urandom unavailable for bootstrap creds");
        return ipmi::response(0xCE);
    }

    // IPMI usernames are limited to 16 chars; prefix to make the bootstrap
    // account recognizable while keeping it within length.
    std::string user(reinterpret_cast<char*>(userArr.data()), userArr.size());
    std::string pass(reinterpret_cast<char*>(passArr.data()), passArr.size());

    try
    {
        auto dbus = getSdBus();

        // Create the user enabled, with the "redfish" group, admin privilege.
        // (phosphor-user-manager: CreateUser(name, groups, privilege, enabled))
        auto create = dbus->new_method_call(
            "xyz.openbmc_project.User.Manager", "/xyz/openbmc_project/user",
            "xyz.openbmc_project.User.Manager", "CreateUser");
        create.append(user, std::vector<std::string>{"redfish"}, "priv-admin",
                      true);
        dbus->call(create);
    }
    catch (const std::exception& e)
    {
        log<level::ERR>("RHI: bootstrap CreateUser failed",
                        entry("ERR=%s", e.what()));
        return ipmi::response(0xCE);
    }

    // Set the account password in the shadow DB via PAM so bmcweb (Basic auth
    // over the usb0 host interface) accepts it.
    if (!pamSetPassword(user, pass))
    {
        log<level::ERR>("RHI: bootstrap pamSetPassword failed",
                        entry("USER=%s", user.c_str()));
        return ipmi::response(0xCE);
    }

    if (disableBootstrapControl == 0xA5)
    {
        log<level::INFO>("RHI: credential bootstrapping disable requested");
        // TODO: persist CredentialBootstrapping:Enabled=false for the host iface.
    }

    return ipmi::responseSuccess(userArr, passArr);
}

} // namespace rhi
} // namespace asrock

// ---------------------------------------------------------------------------
// Registration (constructor, mirrors the other asrock-ipmi-oem providers).
// ---------------------------------------------------------------------------
static void registerRedfishHostInterfaceCommands() __attribute__((constructor));

static void registerRedfishHostInterfaceCommands()
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "ASRock Redfish Host Interface IPMI group registered "
        "(NetFn 0x2C / Group 0x00, cmds 0x01 0x02)");

    ipmi::registerGroupHandler(ipmi::prioOemBase, asrock::rhi::groupRedfish,
                               asrock::rhi::cmdGetMgrCertFingerprint,
                               ipmi::Privilege::User,
                               asrock::rhi::getManagerCertificateFingerprint);

    ipmi::registerGroupHandler(ipmi::prioOemBase, asrock::rhi::groupRedfish,
                               asrock::rhi::cmdGetBootstrapCreds,
                               ipmi::Privilege::User,
                               asrock::rhi::getBootstrapAccountCredentials);
}
