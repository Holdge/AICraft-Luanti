#!/usr/bin/env bash
# Added for AICraft on 2026-07-27; see AICRAFT_CHANGES.md.
set -euo pipefail
umask 077

engine_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
control_project_dir="${AICRAFT_CONTROL_PROJECT_DIR:-$(cd "$engine_dir/../AICraft" && pwd)}"
server_binary="${AICRAFT_LUANTI_SERVER_BIN:-$engine_dir/dist/aicraft/AICraft.app/Contents/MacOS/aicraft}"
agent_binary="${AICRAFT_AGENT_CLIENT_BIN:-$engine_dir/bin/aicraft-agent-client}"
runtime_parent="$engine_dir/.aicraft-tests"
mkdir -p "$runtime_parent"
runtime_dir="$(mktemp -d "$runtime_parent/mineclonia-agent.XXXXXX")"
world_dir="$runtime_dir/world"
user_dir="$runtime_dir/user"
log_dir="$runtime_dir/logs"
socket_path="$runtime_dir/control.sock"
done_file="$runtime_dir/control.done"
password_file="$runtime_dir/player-password"
session_secret_file="$runtime_dir/agent-session-secret"
roster_file="$world_dir/aicraft-runtime-roster.json"
server_config="$runtime_dir/server.conf"
session_id="00000000-0000-4000-8000-000000000023"
server_pid=""
control_pid=""
agent_pid=""

terminate_process() {
	local process_id="$1"
	if [[ -z "$process_id" ]]; then
		return
	fi
	if kill -0 "$process_id" >/dev/null 2>&1; then
		kill -TERM "$process_id" >/dev/null 2>&1 || true
		for _ in {1..100}; do
			kill -0 "$process_id" >/dev/null 2>&1 || break
			sleep 0.05
		done
		if kill -0 "$process_id" >/dev/null 2>&1; then
			kill -KILL "$process_id" >/dev/null 2>&1 || true
		fi
	fi
	wait "$process_id" >/dev/null 2>&1 || true
}

cleanup() {
	local exit_code=$?
	trap - EXIT INT TERM
	# Keep the authenticated socket alive while the Agent handles SIGTERM.
	terminate_process "$agent_pid"
	terminate_process "$control_pid"
	terminate_process "$server_pid"
	if (( exit_code != 0 )); then
		for log_file in "$log_dir"/*.log; do
			[[ -f "$log_file" ]] || continue
			echo "Mineclonia smoke failure: $log_file" >&2
			tail -n 100 "$log_file" | cut -c1-1600 >&2 || true
		done
	elif [[ "${AICRAFT_KEEP_SMOKE_ARTIFACTS:-0}" == "1" ]]; then
		echo "Mineclonia smoke artifacts retained at $runtime_dir"
	else
		rm -rf "$runtime_dir"
		rmdir "$runtime_parent" >/dev/null 2>&1 || true
	fi
	exit "$exit_code"
}
trap cleanup EXIT INT TERM

for executable in "$server_binary" "$agent_binary"; do
	if [[ ! -x "$executable" ]]; then
		echo "Missing executable: $executable" >&2
		exit 1
	fi
done
for command_name in python3 openssl; do
	command -v "$command_name" >/dev/null 2>&1 || {
		echo "Missing required command: $command_name" >&2
		exit 1
	}
done

mkdir -p "$runtime_parent" "$world_dir/worldmods" "$user_dir" "$log_dir"
"$control_project_dir/scripts/stage-local-world.sh" "$world_dir" >/dev/null
ln -s "$engine_dir/util/aicraft/mineclonia_smoke_mod" \
	"$world_dir/worldmods/aicraft_gate2_smoke"
seed_world="${AICRAFT_MINECLONIA_SMOKE_SEED_WORLD:-$control_project_dir/.aicraft/runtime/worlds/free-explore}"
if [[ -f "$seed_world/map_meta.txt" && -f "$seed_world/mod_storage.sqlite" ]]; then
	# Reuse only Mineclonia's deterministic map metadata/mod-storage cache.
	# This avoids regenerating the global stronghold index while keeping map,
	# auth, players, and all account data isolated in the temporary world.
	cp "$seed_world/map_meta.txt" "$world_dir/map_meta.txt"
	cp "$seed_world/mod_storage.sqlite" "$world_dir/mod_storage.sqlite"
fi
release_dir="$("$control_project_dir/scripts/bootstrap-mineclonia.sh")"
game_search_path="$release_dir"

openssl rand -base64 32 >"$password_file"
openssl rand -base64 32 >"$session_secret_file"
chmod 600 "$password_file" "$session_secret_file"
cp "$engine_dir/util/aicraft/mineclonia-smoke.conf" "$server_config"
sed -i.bak \
	-e 's/^aicraft_require_roster = false$/aicraft_require_roster = true/' \
	-e 's/^aicraft_local_auto_assign = true$/aicraft_local_auto_assign = false/' \
	"$server_config"
rm -f "$server_config.bak"
printf 'aicraft_roster_file = %s\n' "$roster_file" >>"$server_config"
python3 - "$password_file" "$roster_file" "$session_id" <<'PY'
import base64
import hashlib
import json
import os
import sys

password_path, roster_path, session_id = sys.argv[1:]
password = open(password_path, encoding="utf-8").read().strip()
password_hash = base64.b64encode(
    hashlib.sha1(("agent_gate2" + password).encode()).digest()
).decode()
temporary = roster_path + ".tmp"
with open(temporary, "x", encoding="utf-8") as handle:
    json.dump({
        "version": 1,
        "match_id": "00000000-0000-4000-8000-000000000021",
        "revision": 1,
        "seats": [{
            "seat": 0,
            "player_name": "agent_gate2",
            "control_type": "EXTERNAL_AGENT",
            "session_id": session_id,
            "password_hash": password_hash,
        }],
        "observers": [],
    }, handle, separators=(",", ":"))
    handle.write("\n")
os.chmod(temporary, 0o600)
os.replace(temporary, roster_path)
PY
chmod 600 "$server_config" "$roster_file"

port="$(python3 - <<'PY'
import socket
sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.bind(("127.0.0.1", 0))
print(sock.getsockname()[1])
sock.close()
PY
)"

LUANTI_GAME_PATH="$game_search_path" \
LUANTI_USER_PATH="$user_dir" \
"$server_binary" \
	--server \
	--gameid mineclonia \
	--world "$world_dir" \
	--config "$server_config" \
	--port "$port" >"$log_dir/server.log" 2>&1 &
server_pid="$!"

for _ in {1..1200}; do
	if grep -q 'Server for gameid=.*listening on' "$log_dir/server.log" 2>/dev/null; then
		break
	fi
	if ! kill -0 "$server_pid" >/dev/null 2>&1; then
		echo "Mineclonia server exited before readiness." >&2
		exit 1
	fi
	sleep 0.1
done
grep -q 'Server for gameid=.*listening on' "$log_dir/server.log"
grep -q '"type":"roster.updated"' "$log_dir/server.log"

python3 "$engine_dir/util/aicraft/gate2_control_smoke.py" \
	"$socket_path" "$session_id" "$session_secret_file" "$done_file" \
	>"$log_dir/control.log" 2>&1 &
control_pid="$!"
for _ in {1..120}; do
	[[ -S "$socket_path" ]] && break
	if ! kill -0 "$control_pid" >/dev/null 2>&1; then
		echo "Control smoke exited before creating its socket." >&2
		exit 1
	fi
	sleep 0.1
done
[[ -S "$socket_path" ]]

LUANTI_USER_PATH="$user_dir" \
"$agent_binary" \
	--address 127.0.0.1 \
	--port "$port" \
	--name agent_gate2 \
	--password-file "$password_file" \
	--agent-control-socket "$socket_path" \
	--agent-session "$session_id" \
	--agent-session-secret-file "$session_secret_file" \
	--go >"$log_dir/agent.log" 2>&1 &
agent_pid="$!"

for _ in {1..600}; do
	if [[ -f "$done_file" ]]; then
		break
	fi
	if ! kill -0 "$control_pid" >/dev/null 2>&1; then
		echo "Control smoke exited before completing the action sequence." >&2
		exit 1
	fi
	if ! kill -0 "$agent_pid" >/dev/null 2>&1; then
		echo "Agent client exited before the action smoke completed." >&2
		exit 1
	fi
	sleep 0.1
done
if [[ ! -f "$done_file" ]]; then
	echo "Mineclonia action smoke exceeded its deadline." >&2
	exit 1
fi

grep -q '\[aicraft_gate2_smoke\] fixture.ready player=agent_gate2' \
	"$log_dir/server.log"
grep -q '"type":"observation.sampled"' "$log_dir/server.log"
if grep -Eq 'ModError|AsyncErr|LuaError|stack traceback|ERROR\[[^]]*\].*[Ll]ua' \
	"$log_dir/server.log" "$log_dir/agent.log"; then
	echo "Mineclonia smoke emitted an engine or Lua error." >&2
	exit 1
fi

cat "$log_dir/control.log"
echo "Real Mineclonia Agent smoke passed without Lua errors; server observation.sampled remained active."
