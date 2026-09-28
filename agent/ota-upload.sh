#!/usr/bin/env bash
# Build the firmware and install it on the controller over Wi-Fi.
#
#   agent/ota-upload.sh            # build + upload + wait for it to come back
#   agent/ota-upload.sh --build    # build only
#
# Why this is more than `pio run -t upload`: the controller may sit on a
# different Wi-Fi network (e.g. a 2.4 GHz or IoT one) than this PC. Routers
# often let the PC reach the controller but block the controller from
# connecting back, and espota needs exactly that connection -- it then fails
# with "No response from device" after authenticating fine. So when this PC's
# Wi-Fi is on another subnet, the script moves it onto the controller's
# network (WIFI_SSID in .env, using the saved NetworkManager profile) for the
# upload and puts it back afterwards. Wired networking is left alone.
#
# The controller restarts after an upload: a restart clears any per-fan
# "turned off" state and returns to the Normal profile.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

cd "$CTRL"
"$PIO" run -e lolin_s3_mini_ota | tail -3
[ "${1:-}" = "--build" ] && exit 0

IP="$(device_ip)"
[ -n "$IP" ] || { echo "cannot resolve $DGX_HOST" >&2; exit 1; }
subnet="${IP%.*}."

wifi_if="$(nmcli -t -f DEVICE,TYPE dev | awk -F: '$2=="wifi"{print $1; exit}')"
wifi_ip() { ip -4 -o addr show "$wifi_if" 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1; }
restore=""

if [ -n "$wifi_if" ] && [[ "$(wifi_ip)" != "$subnet"* ]]; then
    ssid="$(env_get WIFI_SSID)"
    profile=""
    while IFS=: read -r name type; do
        [ "$type" = 802-11-wireless ] || continue
        [ "$(nmcli -g 802-11-wireless.ssid con show "$name" 2>/dev/null)" = "$ssid" ] && { profile="$name"; break; }
    done < <(nmcli -t -f NAME,TYPE con show)
    [ -n "$profile" ] || { echo "no saved Wi-Fi profile for the controller's network; connect to it once by hand" >&2; exit 1; }
    restore="$(nmcli -t -f NAME,DEVICE con show --active | awk -F: -v d="$wifi_if" '$2==d{print $1; exit}')"
    trap '[ -n "$restore" ] && nmcli con up "$restore" >/dev/null 2>&1 || true' EXIT
    echo "# moving Wi-Fi to the controller's network for the upload"
    nmcli con up "$profile" >/dev/null
    for _ in $(seq 20); do [[ "$(wifi_ip)" == "$subnet"* ]] && break; sleep 1; done
fi

host_ip="$(wifi_ip)"
[[ "$host_ip" == "$subnet"* ]] || host_ip=""   # same subnet by cable: let espota pick

pass="$(env_get WEBPASS)"
ok=0
out="$("$PY" "$HOME/.platformio/packages/framework-arduinoespressif32/tools/espota.py" \
    -r -i "$IP" ${host_ip:+-I "$host_ip"} -a "$pass" \
    -f "$CTRL/.pio/build/lolin_s3_mini_ota/firmware.bin" 2>&1)" || ok=$?
unset pass
printf '%s\n' "$out" | tr '\r' '\n' | grep -v -i -e auth -e Uploading | tail -3 || true
[ "$ok" = 0 ] || { echo "upload failed (espota exit $ok)" >&2; exit 1; }

if [ -n "$restore" ]; then
    nmcli con up "$restore" >/dev/null 2>&1 || true
    restore=""
    echo "# Wi-Fi back on its usual network"
fi

sleep 5                       # let it restart before asking
for _ in $(seq 30); do
    s="$(curl -s -m 3 "http://$IP/api/status" || true)"
    if [ -n "$s" ]; then echo "$s"; exit 0; fi
    sleep 1
done
echo "controller did not come back within 30 s" >&2
exit 1
