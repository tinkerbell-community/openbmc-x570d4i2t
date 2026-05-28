# BMC discovery playbook

When you need a fact about the hardware that isn't already in
[01-hardware-inventory.md](01-hardware-inventory.md), use this playbook to ask
the live MegaRAC BMC directly. The BMC is at **`https://10.0.80.1`** on this
network.

> Credentials: **`admin`** / **`mrYsg79V*7ex!wt2`** (user-supplied).

## Two parallel auth schemes

The MegaRAC exposes two parallel APIs with **different authentication**:

| Surface | Auth | When to use |
|---|---|---|
| Redfish (`/redfish/v1/...`) | `X-Auth-Token` header from a `Session` POST | Standardised inventory, sensors, FRU, BIOS attrs |
| AMI Web API (`/api/...`) | `QSESSIONID` cookie + `X-CSRFTOKEN` header from a session POST | ASRock-specific extensions (fan tables, POST code, NCSI mode, etc.) |

You will usually need both. Sessions expire silently (~5 min idle on Redfish);
re-auth on `Security.1.0.AccessDenied`.

## Bootstrap

```bash
mkdir -p /tmp/x570d4i2t-bmc && cd /tmp/x570d4i2t-bmc

# --- Redfish session ---
curl -ks -X POST https://10.0.80.1/redfish/v1/SessionService/Sessions \
  -H 'Content-Type: application/json' \
  -d '{"UserName":"admin","Password":"mrYsg79V*7ex!wt2"}' \
  -D rf_headers.txt -o rf_session.json
RF_TOKEN=$(grep -i '^x-auth-token' rf_headers.txt | awk '{print $2}' | tr -d '\r\n')
RF_SESSION=$(grep -i '^location' rf_headers.txt | awk '{print $2}' | tr -d '\r\n')

# --- Web API session ---
curl -ks -c webcookies.txt -o web_session.json \
  -X POST \
  --data-urlencode "username=admin" \
  --data-urlencode "password=mrYsg79V*7ex!wt2" \
  https://10.0.80.1/api/session
CSRF=$(jq -r .CSRFToken web_session.json)
COOKIES="QSESSIONID=$(awk '$6=="QSESSIONID"{print $7}' webcookies.txt)"
```

Helpers used in the rest of this doc:

```bash
rf()  { curl -ks --max-time 8 -H "X-Auth-Token: $RF_TOKEN" "https://10.0.80.1$1"; }
web() { curl -ks --max-time 8 -H "Cookie: $COOKIES" -H "X-CSRFTOKEN: $CSRF" "https://10.0.80.1$1"; }
```

Cleanup when done (always do this):

```bash
curl -ks -X DELETE -H "X-Auth-Token: $RF_TOKEN" "https://10.0.80.1$RF_SESSION"
curl -ks -X DELETE -H "Cookie: $COOKIES" -H "X-CSRFTOKEN: $CSRF" \
  https://10.0.80.1/api/session
```

## High-value endpoints by question

### "What sensors exist?"

```bash
web /api/sensors  | jq .            # 45 entries with thresholds, units, sensor numbers
web /api/sdr      | jq .            # bare SDR (lighter)
rf  /redfish/v1/Chassis/Self/Thermal | jq .
rf  /redfish/v1/Chassis/Self/Power   | jq .
```

### "What's the live POST code? What did the last boot trace look like?"

```bash
web /api/asrr/getbioscode | jq .                    # one value
web /api/logs/postcode    | jq '.[].postcodedata1'   # array of frames
```

Codes ≥ 0x4000 (decimal ≥ 16384) are AMD AGESA 16-bit extended codes. Bytes
above 0xA000 (decimal ≥ 40960) are post-DXE / handoff codes.

### "What fan headers / control tables are in use?"

```bash
web /api/asrr/settings/getsupportfan        | jq .   # which fan_N positions are wired
web /api/asrr/settings/getsupportfanname    | jq .   # display names
web /api/asrr/settings/getfancontrolmode    | jq .   # 0=open-loop, 1=closed-loop per fan
web /api/asrr/settings/getfanopenloopcontroltable  | jq .  # 24-pt temp→duty curve
web /api/asrr/settings/getfancloseloopcontroltable | jq .
web /api/asrr/settings/getfantempsensorandcorrespondfantable | jq .
```

### "What NIC topology is configured?"

```bash
web /api/settings/network        | jq .   # eth0/eth1 IP + MAC + channel
web /api/settings/network-link   | jq .   # link speed / duplex / NCSI status per iface
web /api/settings/ncsi/mode      | jq .
web /api/settings/channels       | jq .   # IPMI channel activity bitmap
web /api/settings/services       | jq .   # which iface each service binds to
rf  /redfish/v1/Managers/Self/EthernetInterfaces?\$expand=* | jq .
rf  /redfish/v1/Chassis/Self/NetworkAdapters?\$expand=*     | jq .
```

### "What BIOS attributes / what does each one mean?"

```bash
rf  /redfish/v1/Systems/Self/Bios | jq '.Attributes | keys'   # 399 attrs
# Decode an attribute against the registry:
rf  /redfish/v1/Registries/BiosAttributeRegistry1AWQE.2.50.0.json \
  | jq --arg k IPMI523 '.RegistryEntries.Attributes[]
                        | select(.AttributeName==$k)
                        | {DisplayName, HelpText, Type, DefaultValue, Value}'
```

> The registry endpoint is gzip-compressed — pass `--compressed` to curl or it
> arrives as binary.

### "What firmware versions are loaded?"

```bash
web /api/firmware-info       | jq .   # BMC IPMI Get Device ID
web /api/asrr/fw-info        | jq .   # BMC / BIOS / PSP / microcode / CPLD
rf  /redfish/v1/UpdateService/FirmwareInventory/BMC  | jq .
rf  /redfish/v1/UpdateService/FirmwareInventory/BIOS | jq .
```

### "What's in the SEL / event log / audit log?"

```bash
rf  /redfish/v1/Managers/Self/LogServices/SEL/Entries      | jq .
rf  /redfish/v1/Managers/Self/LogServices/EventLog/Entries | jq .
rf  /redfish/v1/Managers/Self/LogServices/AuditLog/Entries | jq .
```

### "What's the FRU data?"

```bash
web /api/fru                 | jq .   # full FRU dump including board mfg date
rf  /redfish/v1/Systems/Self/FruInfo | jq .
```

### "What features did ASRock compile into this MegaRAC?"

```bash
web /api/configuration/project | jq -r '.[].feature' | sort -u
```

Useful to know what code paths exist (`ASRR_FAN_TABLE`, `NCSI_SUPPORT`,
`BIOS_CODE_2BYTES`, `REDFISH_HOSTINTERFACE`, etc.).

### "Show me the full WebUI bundle so I can find more endpoints"

```bash
curl -ks --compressed -H "Cookie: $COOKIES" https://10.0.80.1/source.min.js \
  -o /tmp/source.min.js
grep -oE '"/api/[a-zA-Z0-9/_\-]*"' /tmp/source.min.js | sort -u
```

309 endpoints total at fw 1.91.00. The captured lists were saved as
`asrr_api.txt` and `non_asrr_api.txt` during the initial sweep.

## When the BMC API isn't enough — go to the OS

Some facts can only come from running on the BMC itself (I2C bus → device
mapping, GPIO line names, MTD layout). Two options:

### Option A — SSH into the stock MegaRAC

```bash
ssh admin@10.0.80.1   # password as above
```

MegaRAC SSH typically drops into the **SMASH CLI** (`->`). To escape to a
shell, try:

- `shell` (some builds)
- `escape` followed by typing commands
- Fall back to IPMI raw commands if SMASH is locked down

> If you get a real shell, the most useful commands are:
> ```bash
> cat /proc/mtd                 # SPI partition layout
> ls -la /sys/bus/i2c/devices/  # i2c-N entries
> cat /sys/bus/i2c/devices/*/name
> cat /sys/kernel/debug/gpio    # GPIO line names (may need root)
> cat /proc/cpuinfo             # ASPEED rev
> cat /proc/device-tree/compatible
> dmesg | grep -iE 'aspeed|i2c|ftgmac|nct|pmbus'
> ```

### Option B — IPMI over the network from your workstation

```bash
ipmitool -I lanplus -H 10.0.80.1 -U admin -P 'mrYsg79V*7ex!wt2' sdr
ipmitool -I lanplus -H 10.0.80.1 -U admin -P 'mrYsg79V*7ex!wt2' fru
ipmitool -I lanplus -H 10.0.80.1 -U admin -P 'mrYsg79V*7ex!wt2' sel list
ipmitool -I lanplus -H 10.0.80.1 -U admin -P 'mrYsg79V*7ex!wt2' mc info
# Raw command — read SDR record N for full bytes:
ipmitool -I lanplus -H 10.0.80.1 -U admin -P 'mrYsg79V*7ex!wt2' \
  raw 0x0a 0x23 0x00 0x00 0xff 0x00 0x00 0xff
```

`sdr` output is the cleanest cross-reference for sensor → IPMI number → SDR
type → I2C address (column "Owner ID" in IPMI is the BMC's view of the I2C
slave, and `0x20` = BMC itself, `0x82..0x9C` = external I2C devices).

### Option C — read the SPI flash dump

Once an external SPI backup of the stock firmware exists
(see [05-flash-and-verify.md](05-flash-and-verify.md)):

```bash
# Find the MAC addresses
xxd stock-x570d4i2t-bmc.bin | grep -i "9c 6b 00"

# Look for a packaged DTB (sometimes embedded as a separate partition)
binwalk stock-x570d4i2t-bmc.bin
binwalk -e stock-x570d4i2t-bmc.bin

# strings the kernel image for sensor/GPIO names
strings stock-x570d4i2t-bmc.bin | grep -iE 'gpio|nct|ftgmac|i2c-'
```

Of these, `binwalk -e` is the most likely to recover a usable DTB + initramfs
that lets you read the running kernel's device tree directly.

## What's still unknown (block list)

Items that this discovery surface does NOT directly answer — use Option A/B/C:

1. **Which I2C bus** the on-board sensor IC sits on (likely `i2c-1` or `i2c-6`,
   common on AST2500 boards, but unverified)
2. **Which sensor IC** is actually fitted — `CONFIG_SENSORS_NCT6775_I2C=y` covers a
   family; need `cat /sys/bus/i2c/devices/*/name` to confirm e.g. `nct6796`
3. **GPIO line numbers** for: PWR_BTN_N, RESET_N, PG, POSTCMPLT_N, ID button,
   NMI, SIO power-good, S5 indicator. Stock MegaRAC `dts` will have these named;
   recover via Option A or C.
4. **eSPI vs LPC** — AGESA P2.50 likely defaults to eSPI on Ryzen 5000. Verify
   with `cat /sys/firmware/devicetree/base/.../compatible` for the host-bridge
   node, or check `dmesg | grep -i espi`.
5. **SPI flash partition layout** — `cat /proc/mtd` while the stock BMC is up,
   or `binwalk` of the dump.
6. **Exact PWM controller channels** that route to FAN1/2/3 — usually 0,1,2 but
   the stock DTS will document the actual mapping.

These are the inputs you need before completing the DTS in
[03-device-tree.md](03-device-tree.md). Block on them.
