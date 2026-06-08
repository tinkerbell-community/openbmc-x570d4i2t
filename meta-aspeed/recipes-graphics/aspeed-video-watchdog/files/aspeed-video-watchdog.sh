#!/bin/sh
# aspeed-video-watchdog: reset the ASPEED V4L2 video engine when
# obmc-ikvm reports "Link has been severed" (ENOLINK from aspeed-video).
#
# The aspeed-video kernel driver sets ENOLINK when it loses the capture
# link (e.g. host power-cycle, mode change, or hardware glitch).  obmc-ikvm
# exits on ENOLINK, but the driver stays wedged — simply restarting ikvm
# with Restart=always is not enough.  This watchdog catches the sentinel
# log line, tears down the driver binding, then brings ikvm back up.

COOLDOWN=30
STAMP=/run/aspeed-video-watchdog.stamp
DRIVER=/sys/bus/platform/drivers/aspeed-video

log() { logger -t aspeed-video-watchdog "$*"; }

find_video_dev() {
    # Symlink names look like "1e700000.video"; skip the driver control
    # files which are plain alpha names (bind, unbind, uevent, …).
    ls "$DRIVER/" 2>/dev/null | grep -E '^[0-9a-f]+\.' | head -1
}

reset_driver() {
    now=$(date +%s)
    if [ -f "$STAMP" ]; then
        last=$(cat "$STAMP" 2>/dev/null || echo 0)
        elapsed=$((now - last))
        if [ "$elapsed" -lt "$COOLDOWN" ]; then
            log "cooldown active (${elapsed}s < ${COOLDOWN}s), skipping reset"
            return
        fi
    fi
    printf '%s\n' "$now" > "$STAMP"

    log "link severed — stopping obmc-ikvm and resetting aspeed-video"
    systemctl stop obmc-ikvm.service 2>/dev/null || true

    if lsmod 2>/dev/null | grep -q '^aspeed_video '; then
        modprobe -r aspeed_video 2>/dev/null || true
        sleep 1
        modprobe aspeed_video 2>/dev/null || true
    else
        dev=$(find_video_dev)
        if [ -n "$dev" ]; then
            log "unbind $dev"
            echo "$dev" > "$DRIVER/unbind" 2>/dev/null || true
            sleep 1
            log "bind $dev"
            echo "$dev" > "$DRIVER/bind" 2>/dev/null || true
        else
            log "aspeed-video device not found under $DRIVER — cannot reset"
        fi
    fi

    sleep 1
    log "restarting obmc-ikvm"
    systemctl start obmc-ikvm.service 2>/dev/null || true
}

log "started — monitoring obmc-ikvm for video link failures"

# Follow the obmc-ikvm unit journal.  --no-hostname -o cat keeps lines
# clean.  grep --line-buffered ensures we act on each match immediately
# rather than waiting for a full buffer.
journalctl -f -u obmc-ikvm.service --no-hostname -o cat 2>/dev/null | \
    grep --line-buffered "Link has been severed" | \
    while read -r _line; do
        reset_driver
    done
