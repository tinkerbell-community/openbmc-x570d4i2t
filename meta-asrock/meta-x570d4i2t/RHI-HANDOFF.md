# X570D4I-2T Redfish Host Interface (RHI) — Session Handoff

**Goal:** make the host BIOS bring up the Redfish Host Interface over usb0 (RNDIS,
BMC = 169.254.0.17) so it authenticates to bmcweb and pushes BIOS config. Success
= **rx > 0 on the BMC's `usb0`** during/after POST (host actually transmits).

**STATUS (2026-06-14): root cause found, handler implemented + deployed, BIOS now
gets past the wall but stops at a response-format mismatch.** Details below.

---

## 0. The one-paragraph summary

The BIOS performs DMTF Redfish Host Interface **credential bootstrapping over KCS
IPMI** (NetFn 0x2C group extension) *before* it will bring up usb0. Stock OpenBMC
did not implement those commands and returned `0xC1`, so the BIOS aborted RHI every
boot. We implemented them (`asrock-ipmi-oem/src/redfishhostiface.cpp`) and proved
the BIOS now invokes our handler and gets cc=0 — but it still **stops after cmd 0x01
without proceeding**, meaning it is validating and rejecting our cmd-01 *response
format*. Next: match the stock MegaRAC response exactly (`[0x52][0x01][50-byte
fingerprint]` vs our `[0x00][0x01][32-byte SHA-256]`), and/or confirm whether a
separate EFI-RNDIS-init gate is the real downstream blocker.

---

## 1. What is PROVEN (do not re-derive)

- **The host↔BMC RHI handshake is KCS IPMI `NetFn 0x2C` (Group Extension).** The
  stock MegaRAC handler is `libipmiredfishhostiface.so` (symbols `g_coreHIGroup`,
  `GetManagerCertificateFingerprint`, `GetBootstrapAccountCredentials`,
  `Generate16ByteRandomAlpNumPasswd`). Decoded via ARM reloc table:
  - `NetFn 0x2C / Cmd 0x01` = GetManagerCertificateFingerprint
  - `NetFn 0x2C / Cmd 0x02` = GetBootstrapAccountCredentials
- **EMPIRICAL (live KCS capture): the BIOS sends `NetFn 0x2C, Cmd 0x01, len=1,
  data=[0x00]`** — group/defining-body = **0x00** (NOT the DSP0270-standard 0x52),
  and NO trailing certificate-number byte. It sends this **once, near POST-complete**,
  and it is the **last** KCS command of POST.
- **Pre-fix:** our BMC returned `0xC1` → BIOS aborted (never sent cmd 0x02, rx=0).
- **Post-fix (handler registered under group 0x00):** the BIOS's `0x2C/0x01 [00]`
  now **invokes our handler** (journal: `RHI 0x2C/00/01 GetManagerCertificateFingerprint`)
  and returns **cc=0**. BUT the BIOS then **stops** (no cmd 0x02, no further KCS,
  rx=0). Same "stop after cmd01" shape as the 0xC1 case ⇒ **BIOS is rejecting our
  cmd-01 response format.**
- **All other BIOS POST IPMI is already answered correctly** (replayed all 155
  distinct commands: only `0x2C/01`, `0x06/30`, `0x0A/5C` ever returned 0xC1; the
  latter two are tolerated probes). Get-LAN ch8 returns the correct 169.254.0.17.
- **Everything else on the BMC side is correct and survives power-cycles:** u-boot
  SuperIO/iLPC2AHB gate (`SCU70`=0x7101D246 bit20=0, `HICRB`=0x0000C000 bit6=0),
  GPIOE4 hog (`devmem 0x1E780020` bit4=1), RNDIS gadget bound to vhub p1, Setup
  REDF000 (Redfish enable) = 0x01.
- **A `HostAutoFW` user already exists** on the BMC (stock AMI host-interface trusted
  user). The BIOS may auth to bmcweb as HostAutoFW rather than using cmd-02 creds.

## 2. The response-format mismatch (the current wall)

MegaRAC cmd-01 response (from ARM disasm @0x2100 of the stock lib):
`[CC=0][0x52][0x01][50-byte fingerprint]`, total len 53 (0x35). It **hard-codes
0x52** as the response group byte even though the request group is 0x00. The 50-byte
fingerprint is NOT a plain 32-byte SHA-256 — stock copies 48+2 bytes from a
`GenerateDigestMultiPartFromMem` buffer (exact encoding still TBD; reverse that fn).

Our current response: `[CC=0][0x00][0x01][32-byte SHA-256 of the bmcweb TLS cert]`.
The `0x00` comes from OpenBMC's group framework, which **echoes the request group**
and cannot be told to emit 0x52.

MegaRAC cmd-02 response (DSP0270): `[CC][0x52][Username 16B][Password 16B]`, len 34.
Ours (in-band verified working): `[CC][0x00][user16][pass16]`, and it **creates a
real BMC user** (CreateUser + PAM password) and returns the creds.

## 3. NEXT STEPS (in priority order)

### Step A — cheap diagnostic first (1 cold POST)
Re-run a cold-POST capture with **f_rndis dynamic-debug enabled** to see if cmd-01
success is already advancing the flow:
```sh
for f in rndis.c f_rndis.c u_ether.c; do echo "file $f +p" > /sys/kernel/debug/dynamic_debug/control; done
# then cold POST (see §5) and watch:  dmesg | grep RNDIS_MSG_INIT
```
- If `RNDIS_MSG_INIT` now appears (it NEVER did pre-fix) → cmd-01 is advancing the
  flow; the remaining blocker is the EFI-RNDIS/network path, not the IPMI response.
- If still no `RNDIS_MSG_INIT` → the BIOS is rejecting the cmd-01 response → do Step B.

### Step B — match MegaRAC's cmd-01 response byte-for-byte
1. **Emit response group byte 0x52** (not 0x00). The OpenBMC `registerGroupHandler`
   framework forces echo of the request group, so switch to a **raw `NetFn 0x2C`
   handler**: try `ipmi::registerHandler(prioOemBase, 0x2C, 0x01, priv, h)` with the
   handler taking the full request `std::vector<uint8_t>` (first byte = group 0x00)
   and returning a `std::vector<uint8_t>` starting `[0x52, 0x01, <fingerprint…>]`.
   VERIFY OpenBMC actually routes NetFn 0x2C to a direct handler (the group
   dispatcher may intercept first — check `phosphor-ipmi-host` `executionEntry`/
   group routing; if it intercepts, may need a small bmcweb/ipmid patch or to emit
   0x52 from inside the group handler if the framework allows overriding the body).
2. **Match the 50-byte fingerprint.** Reverse `GenerateDigestMultiPartFromMem`
   (in stock `libipmiredfishhostiface.so` PLT → its defining lib) to learn the exact
   encoding (hex string? base64? which hash?). 50 bytes ≠ 32-byte SHA-256 and ≠
   64-char SHA-256 hex. Until known, experiment: try SHA-256 hex (64 — too long),
   "sha256:"+base64, etc. The hash input is the cert PEM between BEGIN/END markers.
3. Rebuild, redeploy (§4), cold-POST capture (§5), check for cmd 0x02 and rx>0.

### Step C — once the BIOS proceeds to cmd 0x02 / connects
- cmd-02 already creates a BMC user + returns creds; confirm bmcweb accepts it for
  Basic-auth over usb0. Also wire the bmcweb **HostInterface** Redfish resource with
  `CredentialBootstrapping:Enabled` (stock reads/writes redis keys
  `Redfish:Managers:%s:HostInterfaces:%s:CredentialBootstrapping:Enabled`).
- Watch for the BIOS `FirmwareConfigDrv` HTTP client (it POST/PATCHes
  `/redfish/v1/Systems/Self/Bios`, `BiosAttributeRegistry…`, `/ami/static-file`).

### Open questions to resolve
- Does the BIOS validate the response **group byte** (wants 0x52) or the
  **fingerprint length/format** (wants 50 bytes) — or both? (Step A/B narrows this.)
- Is the cmd-01 fingerprint even mandatory, or does the BIOS only need cc=0 + a
  well-formed-enough reply to proceed to network bring-up?
- Separate, possibly still-live: the EFI USB-RNDIS stack enumerates the gadget but
  (pre-fix) never sent `RNDIS_INIT`. Confirm this is downstream of the IPMI gate, not
  independent.

---

## 4. Build + deploy (operational)

```sh
# build
cd /home/appkins/src/tinkerbell-community/openbmc
source ./setup x570d4i2t            # NOT . ./openbmc-env
bitbake -c compile -f asrock-ipmi-oem
# built lib:
#   build/x570d4i2t/tmp/work/arm1176jzs-openbmc-linux-gnueabi/asrock-ipmi-oem/0.1/build/libzasrockoemcmds.so.0.1  (~20 MB)

# hot-deploy to live BMC (root:0penBmc @ 10.0.80.1). /home/root is a 22M cow overlay
# — TOO SMALL for the 20MB .so; stage in /dev/shm (240M tmpfs) instead:
SSHP="sshpass -p 0penBmc ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null root@10.0.80.1"
SCPP="sshpass -p 0penBmc scp -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
$SCPP <built .so> root@10.0.80.1:/dev/shm/lib.new
$SSHP 'cp -f /dev/shm/lib.new /usr/lib/ipmid-providers/libzasrockoemcmds.so.0.1; sync; systemctl restart phosphor-ipmi-host; rm -f /dev/shm/lib.new'
# confirm registration:
$SSHP 'journalctl -u phosphor-ipmi-host --no-pager | grep "Redfish Host Interface" | tail -1'
```

In-band test (no host needed; ch=4):
```sh
# cmd 0x01 with the BIOS's exact request data=[00]:
$SSHP 'busctl call xyz.openbmc_project.Ipmi.Host /xyz/openbmc_project/Ipmi \
  xyz.openbmc_project.Ipmi.Server execute yyyaya{sv} 44 0 1 1 0 0'
# returns: (yyyyay) 45 0 1 <CC> <len> <data...>   ; CC=0 expected, data[0]=group echo
# cmd 0x02:  ...execute yyyaya{sv} 44 0 2 1 0 0
```

## 5. Cold POST + capture (operational) — CRITICAL CAVEATS

- **`ForceRestart` does NOT reliably re-POST this host.** Use a hard **chassis
  off→on** to guarantee a fresh POST:
  ```sh
  CH="busctl set-property xyz.openbmc_project.State.Chassis /xyz/openbmc_project/state/chassis0 xyz.openbmc_project.State.Chassis RequestedPowerTransition s"
  $CH xyz.openbmc_project.State.Chassis.Transition.Off   # wait until CurrentPowerState=Off
  $CH xyz.openbmc_project.State.Chassis.Transition.On
  ```
- The BIOS's `0x2C/01` lands **~105s into POST, right around POST-complete**.
- Watch the BIOS KCS traffic via the OEM provider's request logger:
  `journalctl -u phosphor-ipmi-host | grep "ch=3" | grep "netfn=0x2C"` (ch=3 = host
  KCS3; ch=4 = in-band busctl). The handler logs `RHI 0x2C/00/01 …` when invoked.
- Capture usb0: `tcpdump -i usb0 -n -e -s 256 -w /dev/shm/x.pcap` (BMC MAC
  02:00:16:92:54:17 = BMC-sourced; anything else = host). Watch
  `/sys/class/net/usb0/statistics/rx_packets`.
- **busctl/host-state signals are unreliable on the AST2500 vhub:** `usb0` carrier
  and the UDC `state` stay `configured`/`1` even when the host is powered OFF — do
  NOT use them to infer host enumeration. The reliable host signals are: the KCS
  `0x2C` request appearing, `RNDIS_MSG_INIT` in dmesg, and usb0 rx_packets.
- `POST Complete assert` events: `journalctl | grep "POST Complete assert"` — a NEW
  timestamp confirms a real fresh POST happened.

## 6. Recovery (host/BMC gets stuck) — AC-drain via UniFi PDU

The host can get stuck not-POSTing after repeated/aborted power cycles; only a full
**AC-drain** clears it (chassis off→on does NOT — it keeps AC applied). Outlet 13 is
governed by a UniFi **power-supervisor** that overrides simple outlet toggles.
Script: `/tmp/claude-1000/ac-drain.sh` (or rewrite). Key facts:
- Controller `https://10.0.0.1`, site `default`, API key in `X-API-KEY` header
  (keys ROTATE — ask the user for a current one; revoked key = HTTP 401 everywhere).
- **OFF/ON via power-supervisor** (the authoritative control):
  `PUT /proxy/network/v2/api/site/default/power-supervisors/6a259451d8918b39eb4dbc6c`
  body `{"power_sources":[{"client_psu_index":1,"power_source_index":13,
  "power_source_mac":"d8:b3:70:29:ae:35","power_source_type":"outlet"}],
  "settings":{"heartbeat_interval":60,"power_off_duration":120,
  "silence_threshold":900},"control_mode":"OFF"}` — `control_mode` ∈
  **{GLOBAL, MANUAL, OFF}** (NO "ON"; **GLOBAL** = restore normal power). GET on this
  v2 endpoint = 405 (PUT only).
- Legacy `outlet_overrides` PUT (`/proxy/network/api/s/default/rest/device/
  65fb7f21d4c85a182996e339`, full 20-outlet array, index 13 relay_state false/true)
  turns OFF but the supervisor prevents ON from sticking — prefer the supervisor.
- After ON: BMC returns to Redfish in **~7–8 min**; firmware gate survives the drain.

## 7. Key files & references

- Handler: `meta-asrock/meta-x570d4i2t/recipes-phosphor/ipmi/asrock-ipmi-oem/src/redfishhostiface.cpp`
  (registered under group **0x00**; params are `std::optional<uint8_t>`; uses
  OpenSSL for the cert hash + PAM `pam_chauthtok("passwd",…)` for the user password).
  Wired in `meson.build` (+`openssl`,`pam` deps) and `asrock-ipmi-oem_0.1.bb`
  (SRC_URI + DEPENDS `openssl libpam`). **Compiles clean.**
- Stock MegaRAC firmware (REFERENCE): `/home/appkins/Downloads/X570D4I-2T(01.91.00)BMC/`
  - RHI lib: `rootfs_squash/rootfs/usr/local/lib/libipmiredfishhostiface.so.6.6.1`
  - Disassemble with `arm-none-eabi-objdump -D -j .text` (system objdump lacks ARM).
  - Board IPMI.conf: `rootfs_squash/rootfs/etc/defconfig/BMC1/1U2-X570/2T/IPMI.conf`
    (`SUPPORT_USB_IFC=1`, `SUPPORT_GROUP_EXTN=1`).
  - ASRock OEM cmds: `libasrrcmds.so` (full symbols); core IPMI: `libipmimsghndlr.so`.
- BIOS decompile: `/home/appkins/src/tinkerbell-community/edk2-x570d4i2t/`
  - RHI client = `FirmwareConfigDrv [1c5c6e7e]` (only binary with `169.254.0.17`).
  - BIOS has ONE usb-net driver: `UsbRndisDriverSrc [11e32c34]` (RNDIS only) — its
    `Supported()` binds iface class/subclass/proto `0x02/0x02/0xFF`|`0xEF/0x04/0x01`|
    `0x0A/0x00/0x00` (so Linux f_rndis already matches; gadget identity is fine).
  - Setup vars / IFR dump: `edk2-x570d4i2t/efi-vars.json`.
- DO NOT re-enable BIOS Setup "Network Stack Driver Support" (Setup VarID1 off 8) —
  it was tried and HANGS POST with PXE timeouts; stock has it disabled. (Reverted.)
- Memory notes (load these): `project_x570d4i2t_rhi_bootstrap`,
  `project_x570d4i2t_redfish_host_interface`, `project_x570d4i2t_bmc_power_cycle`,
  `project_x570d4i2t_hotpatch_deploy`.

## 8. Access / creds

- BMC: `10.0.80.1`, `root:0penBmc` (ssh via `sshpass`; bmcweb Redfish `curl -k -u root:0penBmc`).
- Host: ASRock X570D4I-2T, AST2500 BMC, AMD host runs **Talos Linux** (k8s node,
  Maintenance stage). usb0 RHI target = BMC 169.254.0.17/16.
- KVM (web): `https://10.0.80.1/#/operations/kvm` (Selenium MCP works; login
  root/0penBmc). Host serial console on BMC: `/dev/ttyVUART0`,
  `/var/log/obmc-console.log` (shows Talos OS output; BIOS POST is NOT redirected
  there). Drive power via busctl (chassis/host State) as above.
