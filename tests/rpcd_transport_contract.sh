#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)"
RPCD="$ROOT/package/luci-app-nes-emulator/root/usr/libexec/rpcd/nes-emulator"

eval "$(sed -n '/^load_config()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^select_http_client()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^api_get()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^api_post()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^daemon_is_ready()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^service_instance_state()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^set_preflight_start_error()/,/^}$/p' "$RPCD")"
eval "$(sed -n '/^set_native_start_error()/,/^}$/p' "$RPCD")"

PORT_VALUE=9090
BIND_VALUE=0.0.0.0
uci() {
	case "$*" in
		"-q get nes-emulator.main.port")
			printf '%s\n' "$PORT_VALUE"
			;;
		"-q get nes-emulator.main.rom_dir")
			printf '%s\n' /etc/nes-emulator/roms
			;;
		"-q get nes-emulator.main.bind")
			printf '%s\n' "$BIND_VALUE"
			;;
		*) return 1 ;;
	esac
}

load_config
[ "$RPC_HOST" = "127.0.0.1" ]
[ "$BASE" = "http://127.0.0.1:9090" ]
BIND_VALUE=127.0.0.2
load_config
[ "$RPC_HOST" = "127.0.0.2" ]
[ "$BASE" = "http://127.0.0.2:9090" ]
BIND_VALUE=::
load_config
[ "$RPC_HOST" = "::1" ]
[ "$BASE" = "http://[::1]:9090" ]
BIND_VALUE=192.168.1.1
load_config
[ "$RPC_HOST" = "192.168.1.1" ]
[ "$BASE" = "http://192.168.1.1:9090" ]

PORT_VALUE=invalid
BIND_VALUE=0.0.0.0
load_config
[ "$RPC_HOST" = "127.0.0.1" ]
[ "$BASE" = "http://127.0.0.1:29876" ]

TEMPORARY="$(mktemp -d /tmp/nes-rpc-transport.XXXXXXXX)"
RM_COMMAND="$(command -v rm)"
cleanup() {
	"$RM_COMMAND" -rf -- "$TEMPORARY"
}
trap cleanup 0 1 2 15

NESD_BIN="$TEMPORARY/missing-nesd"
HTTP_CLIENT=stale
! select_http_client
[ -z "$HTTP_CLIENT" ]

ARG_LOG="$TEMPORARY/args"
export ARG_LOG
NESD_BIN="$TEMPORARY/nesd"
cat >"$NESD_BIN" <<'EOF'
#!/usr/bin/env sh
if ( : >&9 ) 2>/dev/null; then
	printf '%s\n' '{"error":"fd 9 leaked into transport"}'
	exit 90
fi
printf '%s\n' "$@" >"$ARG_LOG"
method=
path=
body=
token_file=
while [ "$#" -gt 0 ]; do
	case "$1" in
		--rpc-method) method="$2"; shift 2 ;;
		--rpc-path) path="$2"; shift 2 ;;
		--rpc-body) body="$2"; shift 2 ;;
		--rpc-token-file) token_file="$2"; shift 2 ;;
		--rpc-client) shift ;;
		*) shift 2 ;;
	esac
done
[ "$token_file" = /etc/nes-emulator/auth.token ] || exit 91
case "${STUB_MODE:-transport_error}" in
	status)
		[ "$method:$path" = GET:/api/status ] || exit 92
		printf '%s\n' \
			'{"t":"status","architecture":"router-cpu-thin-client","running":false,"paused":false,"core_loaded":false,"game_loaded":false}'
		;;
	impostor)
		printf '%s\n' \
			'{"running":false,"paused":false,"core_loaded":false,"game_loaded":false}'
		;;
	html)
		printf '%s\n' '<html>another service</html>'
		;;
	http_error)
		printf '%s\n' '{"ok":false,"error":"bad token"}'
		exit 22
		;;
	post_error)
		[ "$method:$path" = POST:/api/load ] || exit 93
		[ "$body" = '{"path":"/roms/smb.nes"}' ] || exit 94
		printf '%s\n' '{"ok":false,"error":"ROM is unreadable"}'
		exit 22
		;;
	transport_error)
		exit 1
		;;
	*)
		exit 95
		;;
esac
EOF
chmod 0755 "$NESD_BIN"
select_http_client
[ "$HTTP_CLIENT" = nesd ]

TOKEN_FILE=/etc/nes-emulator/auth.token
AUTH_TOKEN=do-not-put-this-token-in-argv
exec 9>"$TEMPORARY/upload.lock"

STUB_MODE=status
export STUB_MODE
output="$(api_get /api/status)"
[ "$output" = '{"t":"status","architecture":"router-cpu-thin-client","running":false,"paused":false,"core_loaded":false,"game_loaded":false}' ]
grep -Fx -- '--rpc-token-file' "$ARG_LOG" >/dev/null
grep -Fx -- "$TOKEN_FILE" "$ARG_LOG" >/dev/null
! grep -F -- "$AUTH_TOKEN" "$ARG_LOG" >/dev/null

STUB_MODE=http_error
export STUB_MODE
if output="$(api_get /api/status)"; then
	echo "HTTP error unexpectedly returned success" >&2
	exit 1
else
	status=$?
fi
[ "$status" -eq 22 ]
[ "$output" = '{"ok":false,"error":"bad token"}' ]

STUB_MODE=post_error
export STUB_MODE
if output="$(api_post /api/load '{"path":"/roms/smb.nes"}')"; then
	echo "rejected POST unexpectedly returned success" >&2
	exit 1
else
	status=$?
fi
[ "$status" -eq 22 ]
[ "$output" = '{"ok":false,"error":"ROM is unreadable"}' ]

json_load_without_upload_fd() {
	PARSED_T=
	PARSED_ARCHITECTURE=
	PARSED_RUNNING=
	PARSED_PAUSED=
	PARSED_CORE_LOADED=
	PARSED_GAME_LOADED=
	case "$1" in
		'{"t":"status","architecture":"router-cpu-thin-client","running":false,"paused":false,"core_loaded":false,"game_loaded":false}')
			PARSED_T=status
			PARSED_ARCHITECTURE=router-cpu-thin-client
			PARSED_RUNNING=0
			PARSED_PAUSED=0
			PARSED_CORE_LOADED=0
			PARSED_GAME_LOADED=0
			;;
		'{"running":false,"paused":false,"core_loaded":false,"game_loaded":false}')
			PARSED_RUNNING=0
			PARSED_PAUSED=0
			PARSED_CORE_LOADED=0
			PARSED_GAME_LOADED=0
			;;
		*) return 1 ;;
	esac
}

json_get_var() {
	case "$2" in
		t) eval "$1=\$PARSED_T" ;;
		architecture) eval "$1=\$PARSED_ARCHITECTURE" ;;
		running) eval "$1=\$PARSED_RUNNING" ;;
		paused) eval "$1=\$PARSED_PAUSED" ;;
		core_loaded) eval "$1=\$PARSED_CORE_LOADED" ;;
		game_loaded) eval "$1=\$PARSED_GAME_LOADED" ;;
		*) eval "$1=" ;;
	esac
}

STUB_MODE=status
export STUB_MODE
daemon_is_ready
STUB_MODE=html
export STUB_MODE
! daemon_is_ready
STUB_MODE=impostor
export STUB_MODE
! daemon_is_ready
STUB_MODE=http_error
export STUB_MODE
! daemon_is_ready

exec 9>&-

# The procd response is parsed strictly and only the named `nesd` instance is
# accepted. Raw service-manager output must never become a LuCI error.
STATE_RUNNING=true
STATE_EXIT=67
ubus() {
	printf '%s\n' '{"nes-emulator":{"instances":{"nesd":{}}}}'
}
jsonfilter() {
	case "$*" in
		*'.running') printf '%s\n' "$STATE_RUNNING" ;;
		*'.exit_code') [ -n "$STATE_EXIT" ] && printf '%s\n' "$STATE_EXIT" ;;
		*) return 1 ;;
	esac
}
service_instance_state
[ "$SERVICE_INSTANCE_RUNNING" -eq 1 ]
[ "$SERVICE_INSTANCE_EXIT_CODE" -eq 67 ]
STATE_RUNNING=false
service_instance_state
[ "$SERVICE_INSTANCE_RUNNING" -eq 0 ]
[ "$SERVICE_INSTANCE_EXIT_CODE" -eq 67 ]
STATE_EXIT=invalid
! service_instance_state
STATE_EXIT=
! service_instance_state

for code in 10 11 12 13 14; do
	START_ERROR=
	set_preflight_start_error "$code"
	[ -n "$START_ERROR" ]
done
PORT=29876
for code in 64 65 66 67 68 69 70 71 127 137 139 143; do
	START_ERROR=
	set_native_start_error "$code"
	[ -n "$START_ERROR" ]
done
set_native_start_error 67
case "$START_ERROR" in *29876*already*use*) ;; *) exit 1 ;; esac

# Exercise the exact startup state machine with the init path replaced by a
# test double. This protects the direct preflight and the final t=15 check.
start_source="$(
	sed -n '/^start_daemon()/,/^}$/p' "$RPCD" |
		sed 's#/etc/init.d/nes-emulator#service_cmd#g'
)"
eval "$start_source"
HTTP_CLIENT=nesd
LOCK_RELEASES=0
READY_CALLS=0
SLEEP_CALLS=0
PREFLIGHT_STATUS=14
START_STATUS=0
daemon_is_ready() {
	READY_CALLS=$((READY_CALLS + 1))
	return 1
}
acquire_start_lock() { return 0; }
release_start_lock() { LOCK_RELEASES=$((LOCK_RELEASES + 1)); }
sleep() { SLEEP_CALLS=$((SLEEP_CALLS + 1)); }
service_cmd() {
	case "$1" in
		preflight) return "$PREFLIGHT_STATUS" ;;
		start) return "$START_STATUS" ;;
		running) return 1 ;;
		*) return 1 ;;
	esac
}
service_instance_state() {
	SERVICE_INSTANCE_RUNNING=0
	SERVICE_INSTANCE_EXIT_CODE=67
	return 0
}
! start_daemon
case "$START_ERROR" in *authentication*token*) ;; *) exit 1 ;; esac
[ "$LOCK_RELEASES" -eq 1 ]

PREFLIGHT_STATUS=0
READY_CALLS=0
SLEEP_CALLS=0
! start_daemon
[ "$READY_CALLS" -eq 18 ]
[ "$SLEEP_CALLS" -eq 15 ]
case "$START_ERROR" in *29876*already*use*) ;; *) exit 1 ;; esac

echo "rpcd transport contract: OK"
