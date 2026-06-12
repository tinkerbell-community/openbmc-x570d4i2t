#!/bin/sh
# Provision the "HostAutoFW" account the AMI host BIOS uses to authenticate to
# bmcweb over the in-band Redfish Host Interface (USB-NCM, 169.254.0.17).
#
# Background (from decompiled AMI firmware v01.91.00):
#   redfish/config.lua          DEFAULT_HOST_FW = "HostAutoFW"
#   redfish/system/bios.lua:83  grants BIOS access when the request arrives on
#                               the host interface AND username == HostAutoFW.
# The stock MegaRAC *trusts the host-interface channel* and does not verify the
# HostAutoFW password.  bmcweb has no equivalent "trust the host interface"
# mode, so the account must exist as a real Redfish user with a password that
# matches whatever the BIOS presents in its HTTP Basic Authorization header.
#
# IMPORTANT: HOSTFW_PASS below must equal the credential the BIOS sends.  If the
# BIOS uses Redfish credential bootstrapping (DSP0270, SMBIOS Type 42) instead
# of a fixed password, that path must be wired separately; capture the BIOS's
# Authorization header once to confirm which it is.

set -eu

USER_NAME="HostAutoFW"
# Override at build time via the recipe (HOSTFW_PASS) if the captured BIOS
# credential differs.  Must satisfy the BMC pwquality policy (>= 8 chars, mixed
# classes, and must NOT contain the user name — pam rejects e.g. "HostAutoFW0").
HOSTFW_PASS="${HOSTFW_PASS:-Rh1Bmc#2026}"

UMGR_SVC="xyz.openbmc_project.User.Manager"
UMGR_OBJ="/xyz/openbmc_project/user"
UMGR_IFACE="xyz.openbmc_project.User.Manager"

log() { logger -t x570-hostfw-user "$*" 2>/dev/null || true; }

# Wait for phosphor-user-manager to own its bus name (bounded).
i=0
while [ "$i" -lt 30 ]; do
    if busctl call "$UMGR_SVC" "$UMGR_OBJ" "$UMGR_IFACE" \
            GetUserInfo s "root" >/dev/null 2>&1; then
        break
    fi
    i=$((i + 1))
    sleep 1
done

# Already provisioned?  GetUserInfo succeeds for an existing user.
if busctl call "$UMGR_SVC" "$UMGR_OBJ" "$UMGR_IFACE" \
        GetUserInfo s "$USER_NAME" >/dev/null 2>&1; then
    log "$USER_NAME already exists; ensuring password is set"
else
    # CreateUser(name, groups[], privilege, enabled) -> signature sassb
    # (string + array-of-string + string + bool). NOTE: it is "sassb", not
    # "sasb" — dropping the privilege 's' makes busctl bind the privilege onto
    # the bool arg ("Failed to parse 'priv-admin' as boolean").
    if busctl call "$UMGR_SVC" "$UMGR_OBJ" "$UMGR_IFACE" \
            CreateUser sassb "$USER_NAME" 1 "redfish" "priv-admin" true \
            >/dev/null 2>&1; then
        log "created Redfish user $USER_NAME (priv-admin)"
    else
        log "CreateUser $USER_NAME failed (may already exist) — continuing"
    fi
fi

# Set / refresh the password (PAM).  chpasswd is provided by busybox.
if printf '%s:%s\n' "$USER_NAME" "$HOSTFW_PASS" | chpasswd 2>/dev/null; then
    log "password set for $USER_NAME"
else
    log "WARNING: failed to set password for $USER_NAME"
fi

exit 0
