#!/usr/bin/env bash
# Install the camera program (out/, from scripts/build-app.sh) on a Luckfox
# Pico over SSH and make it start at boot instead of rkipc.
#
#   scripts/deploy.sh --endpoint http://192.168.1.10:18070      install + autostart
#   scripts/deploy.sh --reboot                                  ... and reboot now
#   scripts/deploy.sh --no-autostart                            only copy files
#   scripts/deploy.sh --restore-rkipc                           undo the boot change
#
#   LUCKFOX_HOST=root@192.168.1.100 (default)
#   LUCKFOX_SSH="ssh -i key root@..." replaces the whole ssh command
#
# On the board:
#   /oem/usr/bin/rerinku-luckfox        the program
#   /oem/usr/bin/rerinku-launch.sh      start/restart loop (stop: "... stop")
#   /userdata/rerinku/rerinku.conf      settings (kept across deploys)
#   /oem/usr/bin/RkLunch.sh             boot script: the rkipc line now starts
#                                       rerinku-launch.sh; original kept as
#                                       RkLunch.sh.rkipc (same for RkLunch-stop.sh)
#
# rkipc holds the camera and cannot be stopped and restarted cleanly, so the
# switch takes effect at the next boot (--reboot).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/out"
HOST="${LUCKFOX_HOST:-root@192.168.1.100}"
read -r -a SSH_CMD <<<"${LUCKFOX_SSH:-ssh -o ConnectTimeout=5 $HOST}"

ENDPOINT=
AUTOSTART=1
REBOOT=0
RESTORE=0
while [ $# -gt 0 ]; do
    case "$1" in
    --endpoint) ENDPOINT="$2"; shift ;;
    --no-autostart) AUTOSTART=0 ;;
    --reboot) REBOOT=1 ;;
    --restore-rkipc) RESTORE=1 ;;
    -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

board() { "${SSH_CMD[@]}" "$@"; }
# Copy a local file to the board (no scp/sftp needed), atomically.
push() {
    local src="$1" dst="$2" mode="$3"
    board "cat > '$dst.new' && chmod $mode '$dst.new' && mv -f '$dst.new' '$dst'" <"$src"
}

LUNCH=/oem/usr/bin/RkLunch.sh
LUNCH_STOP=/oem/usr/bin/RkLunch-stop.sh

if [ "$RESTORE" = 1 ]; then
    board "set -e
        [ -f $LUNCH.rkipc ] && mv -f $LUNCH.rkipc $LUNCH
        [ -f $LUNCH_STOP.rkipc ] && mv -f $LUNCH_STOP.rkipc $LUNCH_STOP
        sync"
    echo "rkipc starts at boot again (files under /oem/usr/bin and /userdata/rerinku left in place)"
    [ "$REBOOT" = 1 ] && board reboot
    exit 0
fi

[ -x "$OUT/bin/rerinku-luckfox" ] || { echo "run scripts/build-app.sh first" >&2; exit 1; }

push "$OUT/bin/rerinku-luckfox" /oem/usr/bin/rerinku-luckfox 755
push "$OUT/bin/rerinku-launch.sh" /oem/usr/bin/rerinku-launch.sh 755
board "mkdir -p /userdata/rerinku"
if ! board "test -f /userdata/rerinku/rerinku.conf"; then
    [ -n "$ENDPOINT" ] || { echo "first install: --endpoint <platform URL> is required" >&2; exit 2; }
    push "$OUT/share/rerinku/rerinku.conf.example" /userdata/rerinku/rerinku.conf 644
fi
if [ -n "$ENDPOINT" ]; then
    board "sed -i 's|^endpoint *=.*|endpoint = $ENDPOINT|' /userdata/rerinku/rerinku.conf"
fi
echo "installed; settings: $(board "grep '^endpoint' /userdata/rerinku/rerinku.conf")"

if [ "$AUTOSTART" = 1 ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    # Edit from the pristine copy so repeated deploys don't stack changes.
    board "[ -f $LUNCH.rkipc ] || cp -p $LUNCH $LUNCH.rkipc
           [ -f $LUNCH_STOP.rkipc ] || cp -p $LUNCH_STOP $LUNCH_STOP.rkipc
           cat $LUNCH.rkipc" >"$tmp/lunch"
    board "cat $LUNCH_STOP.rkipc" >"$tmp/stop"

    # Only the program start changes: drivers (insmod_ko.sh), network and the
    # rest of the boot sequence stay as the vendor wrote them.
    sed -E 's@^([[:space:]]*)rkipc( -a [^&]*)? &[[:space:]]*$@\1/oem/usr/bin/rerinku-launch.sh@' \
        "$tmp/lunch" >"$tmp/lunch.new"
    sed -E 's@^([[:space:]]*)killall rkipc[[:space:]]*$@\1/oem/usr/bin/rerinku-launch.sh stop\n\1killall rkipc@' \
        "$tmp/stop" >"$tmp/stop.new"
    if ! grep -q rerinku-launch.sh "$tmp/lunch.new" || grep -qE '^[[:space:]]*rkipc( |&|$)' "$tmp/lunch.new"; then
        echo "unexpected $LUNCH layout; not changed. rkipc start lines:" >&2
        grep -n rkipc "$tmp/lunch" >&2 || true
        exit 1
    fi
    diff -u --label "$LUNCH (vendor)" --label "$LUNCH" "$tmp/lunch" "$tmp/lunch.new" || true
    push "$tmp/lunch.new" "$LUNCH" 755
    push "$tmp/stop.new" "$LUNCH_STOP" 755
    board sync
    echo "boot: rerinku-launch.sh replaces rkipc (undo: scripts/deploy.sh --restore-rkipc)"
fi

if [ "$REBOOT" = 1 ]; then
    echo "rebooting the board"
    board reboot || true
fi
