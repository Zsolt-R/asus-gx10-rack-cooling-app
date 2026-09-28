#!/usr/bin/env bash
# Talk to the running controller over Wi-Fi.
#
#   agent/fans.sh status                 # one JSON status line
#   agent/fans.sh watch [seconds]        # status every 2 s (default 20 s)
#   agent/fans.sh profile normal|quiet|max
#   agent/fans.sh identify 1|2|stop
#   agent/fans.sh fan 1|2 on|off
#
# Anything that changes state needs the web password from controller/.env.
# NOTE: these act on real fans cooling real machines -- say what you are
# about to do, and put things back the way the user had them afterwards.
set -euo pipefail
source "$(dirname "$0")/lib.sh"

IP="$(device_ip)"
[ -n "$IP" ] || { echo "cannot resolve $DGX_HOST" >&2; exit 1; }
B="http://$IP"

pretty() { python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin)))'; }

case "${1:-status}" in
    status)   curl -s -m 5 "$B/api/status" | pretty ;;
    watch)    end=$(( $(date +%s) + ${2:-20} ))
              while [ "$(date +%s)" -lt "$end" ]; do
                  curl -s -m 5 "$B/api/status" | pretty; sleep 2
              done ;;
    profile)  curl_auth -d "name=${2:?normal|quiet|max}" "$B/api/profile"; echo ;;
    identify) n="${2:?1|2|stop}"; [ "$n" = stop ] && n=0
              curl_auth -d "fan=$n" "$B/api/identify"; echo ;;
    fan)      case "${3:?on|off}" in on) on=1 ;; off) on=0 ;; *) exit 2 ;; esac
              curl_auth -d "fan=${2:?1|2}&on=$on" "$B/api/fan"; echo ;;
    *)        sed -n '2,9p' "$0"; exit 2 ;;
esac
