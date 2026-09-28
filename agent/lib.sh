# Shared by the agent scripts. Source it, don't run it.
#
# Reads credentials from controller/.env WITHOUT printing them. Never echo
# $ENV_WEBPASS, $ENV_USER or $ENV_WIFI_* -- pass them straight into commands.

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CTRL="$REPO/controller"
ENV_FILE="$CTRL/.env"
PIO="$HOME/.platformio/penv/bin/pio"
PY="$HOME/.platformio/penv/bin/python"
DGX_HOST="${DGX_HOST:-dgx-fans.local}"

# env_get KEY -> value of KEY in .env, surrounding quotes stripped.
env_get() {
    [ -f "$ENV_FILE" ] || { echo "missing $ENV_FILE (copy .env.example and fill it in)" >&2; return 1; }
    grep -E "^$1=" "$ENV_FILE" | head -1 | cut -d= -f2- | sed -e 's/^["'\'']//' -e 's/["'\'']$//'
}

# device_ip -> IPv4 of the controller (mDNS name or DGX_HOST as given).
device_ip() {
    getent ahostsv4 "$DGX_HOST" | awk 'NR==1 {print $1}'
}

# curl_auth ARGS... -> curl with Basic auth fed on stdin, so the password
# never appears in the process list.
curl_auth() {
    local u p
    u="$(env_get USER)" || return 1
    p="$(env_get WEBPASS)" || return 1
    printf 'user = "%s:%s"\n' "$u" "$p" | curl -s -m 5 -K - "$@"
}
