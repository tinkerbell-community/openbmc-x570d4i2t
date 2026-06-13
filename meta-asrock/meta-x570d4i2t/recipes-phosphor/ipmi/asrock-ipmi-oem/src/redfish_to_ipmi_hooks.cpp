// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ASRock-Rack Inc.
//
// Redfish → IPMI translation hooks for the in-band usb0 Redfish Host Interface.
//
// This is the mirror image of intel-ipmi-oem's src/ipmi_to_redfish_hooks.cpp.
// Intel's file takes IPMI SEL records and emits Redfish *events*; this file goes
// the other way: it accepts inbound *Redfish (HTTP) requests* that the host BIOS
// /OS sends over the in-band usb0 link and translates each one into an IPMI
// command, executed against the BMC's own IPMI stack via the
// xyz.openbmc_project.Ipmi.Server D-Bus interface (the same entry point ipmid
// and the KCS bridge use). The IPMI completion code + data are then folded back
// into a small Redfish-shaped JSON response.
//
// Structure deliberately resembles the Intel reference:
//   * a RedfishReq parse struct          (cf. their SELData)
//   * a startRedfishHook() dispatcher     (cf. their startRedfishHook)
//     that switches on the Redfish resource + HTTP method
//   * one <resource>Hook() per category   (cf. biosMessageHook / meMessageHook)
//     each building an IPMI request and returning the translated response
//   * a defaultHook() fallthrough         (cf. defaultMessageHook)
//
// Only the BMC-meaningful resources are wired (power state / reset / SEL); the
// table is intentionally easy to extend — add a branch in startRedfishHook and
// a <resource>Hook() that calls ipmiExecute().
//
// The listener is bound to usb0 via SO_BINDTODEVICE so it only ever serves the
// host-interface link, never the management LAN.

#include <string.h>     // strerror / strlen
#include <sys/socket.h> // SOL_SOCKET / SO_BINDTODEVICE

#include <boost/asio.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <nlohmann/json.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace asrock::redfish_to_ipmi
{

namespace beast = boost::beast;
namespace http = beast::http;
using tcp = boost::asio::ip::tcp;
using json = nlohmann::json;

// -----------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------

static constexpr const char* ipmiSvc  = "xyz.openbmc_project.Ipmi.Host";
static constexpr const char* ipmiObj  = "/xyz/openbmc_project/Ipmi";
static constexpr const char* ipmiIntf = "xyz.openbmc_project.Ipmi.Server";

static constexpr const char* hostIface = "usb0"; // RHI link only
static constexpr unsigned short listenPort = 8080;

// IPMI NetFns / commands we translate into.
static constexpr uint8_t netFnChassis = 0x00;
static constexpr uint8_t netFnApp     = 0x06;
static constexpr uint8_t netFnStorage = 0x0A;
static constexpr uint8_t cmdGetChassisStatus = 0x01;
static constexpr uint8_t cmdChassisControl   = 0x02;
static constexpr uint8_t cmdGetDeviceId      = 0x01;
static constexpr uint8_t cmdGetSelInfo       = 0x40;
static constexpr uint8_t cmdReserveSel       = 0x42;
static constexpr uint8_t cmdClearSel         = 0x47;

// -----------------------------------------------------------------------
// IPMI execution helper — calls xyz.openbmc_project.Ipmi.Server.execute,
// the in-process D-Bus entry that runs the full ipmid filter+handler chain.
// Returns {completionCode, responseData}; cc=0xFF on transport failure.
// -----------------------------------------------------------------------

struct IpmiResult
{
    uint8_t cc = 0xFF;
    std::vector<uint8_t> data;
};

static IpmiResult ipmiExecute(uint8_t netFn, uint8_t cmd,
                              const std::vector<uint8_t>& data)
{
    IpmiResult out;
    try
    {
        auto bus = sdbusplus::bus::new_default();
        auto m = bus.new_method_call(ipmiSvc, ipmiObj, ipmiIntf, "execute");
        // signature: yyyay a{sv}  →  netFn, lun, cmd, data, options
        std::map<std::string, std::variant<int>> options;
        m.append(netFn, static_cast<uint8_t>(0), cmd, data, options);

        auto reply = bus.call(m);
        // returns (yyyyay): netFn, lun, cmd, cc, data
        uint8_t rNetFn = 0, rLun = 0, rCmd = 0;
        reply.read(rNetFn, rLun, rCmd, out.cc, out.data);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "redfish_to_ipmi: ipmiExecute failed",
            phosphor::logging::entry("NETFN=0x%02X", netFn),
            phosphor::logging::entry("CMD=0x%02X", cmd),
            phosphor::logging::entry("ERROR=%s", e.what()));
    }
    return out;
}

// -----------------------------------------------------------------------
// Parsed inbound request — cf. Intel's SELData.
// -----------------------------------------------------------------------

struct RedfishReq
{
    http::verb method;
    std::string target; // request path, e.g. /redfish/v1/Systems/system
    json body;          // parsed JSON body (object{} if none / unparseable)
};

// A translated response: an HTTP status + a Redfish-shaped JSON payload.
struct RedfishRsp
{
    http::status status = http::status::not_implemented;
    json body = json::object();
};

namespace redfish_hooks
{

// Helper: an error body carrying the raw IPMI CC, so the host can see exactly
// what the underlying IPMI command returned.
static json ipmiError(const std::string& resource, uint8_t cc)
{
    char ccHex[8];
    std::snprintf(ccHex, sizeof(ccHex), "0x%02X", cc);
    return json{{"error",
                 {{"code", "Base.1.0.GeneralError"},
                  {"message", "IPMI translation of " + resource +
                                  " failed, completion code " + ccHex}}}};
}

// ── GET /redfish/v1/Systems/system → Get Chassis Status (+ Get Device ID) ──
// Translates the host's "read my power state" into IPMI 0x00/0x01 and maps the
// current-power-state bit into Redfish PowerState. (cf. biosMessageHook)
static RedfishRsp systemHook(const RedfishReq& req)
{
    RedfishRsp rsp;
    if (req.method != http::verb::get)
    {
        rsp.status = http::status::method_not_allowed;
        return rsp;
    }

    auto chassis = ipmiExecute(netFnChassis, cmdGetChassisStatus, {});
    if (chassis.cc != 0x00 || chassis.data.empty())
    {
        rsp.status = http::status::service_unavailable;
        rsp.body = ipmiError("Systems/system", chassis.cc);
        return rsp;
    }
    bool powerOn = (chassis.data[0] & 0x01) != 0; // current power state bit0

    json sys = {
        {"@odata.id", "/redfish/v1/Systems/system"},
        {"@odata.type", "#ComputerSystem.v1_0_0.ComputerSystem"},
        {"Id", "system"},
        {"PowerState", powerOn ? "On" : "Off"},
    };

    // Best-effort enrich with Get Device ID (firmware revision).
    auto devId = ipmiExecute(netFnApp, cmdGetDeviceId, {});
    if (devId.cc == 0x00 && devId.data.size() >= 5)
    {
        char fw[16];
        std::snprintf(fw, sizeof(fw), "%u.%02x", devId.data[2], devId.data[3]);
        sys["FirmwareVersion"] = fw;
    }

    rsp.status = http::status::ok;
    rsp.body = sys;
    return rsp;
}

// ── POST /redfish/v1/Systems/system/Actions/ComputerSystem.Reset ──────────
// Map ResetType → IPMI Chassis Control (0x00/0x02) control byte. (cf. meHook)
static RedfishRsp resetHook(const RedfishReq& req)
{
    RedfishRsp rsp;
    if (req.method != http::verb::post)
    {
        rsp.status = http::status::method_not_allowed;
        return rsp;
    }

    std::string resetType =
        req.body.value("ResetType", std::string("ForceRestart"));

    // IPMI Chassis Control byte (IPMI spec §28.3):
    //   0=power down, 1=power up, 2=power cycle, 3=hard reset, 5=soft-shutdown
    uint8_t control;
    if (resetType == "On")
        control = 0x01;
    else if (resetType == "ForceOff")
        control = 0x00;
    else if (resetType == "ForceRestart")
        control = 0x03;
    else if (resetType == "PowerCycle" || resetType == "ForcePowerCycle")
        control = 0x02;
    else if (resetType == "GracefulShutdown" || resetType == "GracefulRestart")
        control = 0x05; // soft-shutdown
    else
    {
        rsp.status = http::status::bad_request;
        rsp.body = json{{"error",
                         {{"code", "Base.1.0.ActionParameterValueError"},
                          {"message", "Unsupported ResetType: " + resetType}}}};
        return rsp;
    }

    auto result = ipmiExecute(netFnChassis, cmdChassisControl, {control});
    if (result.cc != 0x00)
    {
        rsp.status = http::status::service_unavailable;
        rsp.body = ipmiError("ComputerSystem.Reset", result.cc);
        return rsp;
    }
    rsp.status = http::status::no_content; // 204, like bmcweb's Reset action
    return rsp;
}

// ── /redfish/v1/Systems/system/LogServices/EventLog/Entries ───────────────
// GET    → Get SEL Info (0x0A/0x40): report entry count.
// DELETE → Clear SEL    (0x0A/0x47): reserve, then the "CLR" initiate-erase.
static RedfishRsp eventLogHook(const RedfishReq& req)
{
    RedfishRsp rsp;
    if (req.method == http::verb::get)
    {
        auto info = ipmiExecute(netFnStorage, cmdGetSelInfo, {});
        if (info.cc != 0x00 || info.data.size() < 3)
        {
            rsp.status = http::status::service_unavailable;
            rsp.body = ipmiError("EventLog", info.cc);
            return rsp;
        }
        uint16_t count =
            static_cast<uint16_t>(info.data[1] | (info.data[2] << 8));
        rsp.status = http::status::ok;
        rsp.body = json{
            {"@odata.id",
             "/redfish/v1/Systems/system/LogServices/EventLog/Entries"},
            {"@odata.type", "#LogEntryCollection.LogEntryCollection"},
            {"Members@odata.count", count}};
        return rsp;
    }
    if (req.method == http::verb::delete_)
    {
        // Clear SEL: reservationId(2), "CLR", 0xAA = initiate erase.
        auto resv = ipmiExecute(netFnStorage, cmdReserveSel, {});
        uint8_t r0 = 0, r1 = 0;
        if (resv.cc == 0x00 && resv.data.size() >= 2)
        {
            r0 = resv.data[0];
            r1 = resv.data[1];
        }
        auto clr = ipmiExecute(netFnStorage, cmdClearSel,
                               {r0, r1, 'C', 'L', 'R', 0xAA});
        rsp.status = (clr.cc == 0x00) ? http::status::no_content
                                      : http::status::service_unavailable;
        if (clr.cc != 0x00)
            rsp.body = ipmiError("EventLog.Clear", clr.cc);
        return rsp;
    }
    rsp.status = http::status::method_not_allowed;
    return rsp;
}

// ── Fallthrough — resource not translated. (cf. defaultMessageHook) ────────
static RedfishRsp defaultHook(const RedfishReq& req)
{
    phosphor::logging::log<phosphor::logging::level::INFO>(
        "redfish_to_ipmi: no IPMI translation for resource",
        phosphor::logging::entry("TARGET=%s", req.target.c_str()));
    RedfishRsp rsp;
    rsp.status = http::status::not_found;
    rsp.body = json{{"error",
                     {{"code", "Base.1.0.ResourceMissingAtURI"},
                      {"message", "No IPMI translation for " + req.target}}}};
    return rsp;
}

// ── Top-level dispatcher — cf. Intel's startRedfishHook switch. ───────────
// Matches on the Redfish resource (path suffix) and routes to a hook.
static RedfishRsp startRedfishHook(const RedfishReq& req)
{
    const std::string& t = req.target;
    auto ends = [&](const char* s) {
        std::string suf(s);
        return t.size() >= suf.size() &&
               t.compare(t.size() - suf.size(), suf.size(), suf) == 0;
    };

    if (t.find("/Actions/ComputerSystem.Reset") != std::string::npos)
        return resetHook(req);
    if (t.find("/LogServices/EventLog/Entries") != std::string::npos)
        return eventLogHook(req);
    if (ends("/Systems/system") || ends("/Systems/system/"))
        return systemHook(req);

    return defaultHook(req);
}

} // namespace redfish_hooks

// -----------------------------------------------------------------------
// Public entry — parse an HTTP request into RedfishReq and dispatch.
// cf. Intel's checkRedfishHooks(...) marshalling wrapper.
// -----------------------------------------------------------------------

static http::response<http::string_body>
    translate(const http::request<http::string_body>& httpReq)
{
    RedfishReq req;
    req.method = httpReq.method();
    req.target = std::string(httpReq.target());
    req.body = json::object();
    if (!httpReq.body().empty())
    {
        json parsed = json::parse(httpReq.body(), nullptr,
                                  /*allow_exceptions*/ false);
        if (!parsed.is_discarded())
            req.body = std::move(parsed);
    }

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "redfish_to_ipmi: request",
        phosphor::logging::entry("METHOD=%s",
                                 std::string(httpReq.method_string()).c_str()),
        phosphor::logging::entry("TARGET=%s", req.target.c_str()));

    RedfishRsp out = redfish_hooks::startRedfishHook(req);

    http::response<http::string_body> httpRsp{out.status, httpReq.version()};
    httpRsp.set(http::field::server, "asrock-redfish-to-ipmi");
    httpRsp.set(http::field::content_type, "application/json");
    httpRsp.keep_alive(httpReq.keep_alive());
    if (out.status != http::status::no_content)
        httpRsp.body() = out.body.dump();
    httpRsp.prepare_payload();
    return httpRsp;
}

// -----------------------------------------------------------------------
// usb0-bound HTTP listener.
// -----------------------------------------------------------------------

static void serveConnection(tcp::socket socket)
{
    try
    {
        beast::flat_buffer buffer;
        http::request<http::string_body> httpReq;
        http::read(socket, buffer, httpReq);

        http::response<http::string_body> httpRsp = translate(httpReq);
        http::write(socket, httpRsp);

        beast::error_code ec;
        socket.shutdown(tcp::socket::shutdown_send, ec);
    }
    catch (const std::exception& e)
    {
        phosphor::logging::log<phosphor::logging::level::WARNING>(
            "redfish_to_ipmi: connection error",
            phosphor::logging::entry("ERROR=%s", e.what()));
    }
}

static int run()
{
    boost::asio::io_context io;
    tcp::acceptor acceptor(io);
    acceptor.open(tcp::v4());
    acceptor.set_option(tcp::acceptor::reuse_address(true));

    // Bind the listener to usb0 only (the RHI link) before binding the port.
    int fd = acceptor.native_handle();
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, hostIface,
                   std::strlen(hostIface)) < 0)
    {
        phosphor::logging::log<phosphor::logging::level::ERR>(
            "redfish_to_ipmi: SO_BINDTODEVICE(usb0) failed",
            phosphor::logging::entry("ERROR=%s", strerror(errno)));
        return 1;
    }

    acceptor.bind(tcp::endpoint(boost::asio::ip::address_v4::any(), listenPort));
    acceptor.listen();

    phosphor::logging::log<phosphor::logging::level::INFO>(
        "redfish_to_ipmi: listening on usb0",
        phosphor::logging::entry("PORT=%u", listenPort));

    for (;;)
    {
        tcp::socket socket(io);
        boost::system::error_code ec;
        acceptor.accept(socket, ec);
        if (ec)
        {
            phosphor::logging::log<phosphor::logging::level::WARNING>(
                "redfish_to_ipmi: accept failed",
                phosphor::logging::entry("ERROR=%s", ec.message().c_str()));
            continue;
        }
        serveConnection(std::move(socket));
    }
    return 0;
}

} // namespace asrock::redfish_to_ipmi

int main()
{
    return asrock::redfish_to_ipmi::run();
}
