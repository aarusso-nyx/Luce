#!/usr/bin/env bash
set -euo pipefail

BROKER_NAME="${LUCE_MQTT_BROKER_NAME:-luce-mqtt-broker}"
BROKER_IMAGE="${LUCE_MQTT_BROKER_IMAGE:-eclipse-mosquitto:2}"
BROKER_PORT="${LUCE_MQTT_BROKER_PORT:-1883}"
BROKER_WS_PORT="${LUCE_MQTT_BROKER_WS_PORT:-9001}"
BROKER_WORKDIR="${LUCE_MQTT_BROKER_WORKDIR:-/tmp/luce-mqtt-broker}"
BROKER_CONF="${BROKER_WORKDIR}/mosquitto.conf"

usage() {
  cat <<USAGE
Usage: scripts/spawn_mqtt_broker.sh <command> [options]

Commands:
  start     Start local MQTT broker container (idempotent)
  stop      Stop broker container if running
  restart   Restart broker container
  status    Print broker status and endpoint info
  logs      Tail broker logs

Options:
  --name <container-name>    Container name (default: ${BROKER_NAME})
  --port <port>              MQTT TCP port on host (default: ${BROKER_PORT})
  --ws-port <port>           MQTT WS port on host (default: ${BROKER_WS_PORT})
  --workdir <path>           Local config dir (default: ${BROKER_WORKDIR})
  -h, --help                 Show help

Environment overrides:
  LUCE_MQTT_BROKER_NAME
  LUCE_MQTT_BROKER_IMAGE
  LUCE_MQTT_BROKER_PORT
  LUCE_MQTT_BROKER_WS_PORT
  LUCE_MQTT_BROKER_WORKDIR

Examples:
  scripts/spawn_mqtt_broker.sh start
  scripts/spawn_mqtt_broker.sh status
  scripts/spawn_mqtt_broker.sh logs
  scripts/spawn_mqtt_broker.sh stop
USAGE
}

require_cmd() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || {
    echo "error: missing command '${cmd}'" >&2
    exit 1
  }
}

container_exists() {
  docker ps -a --format '{{.Names}}' | awk -v n="$BROKER_NAME" '$0==n{found=1} END{exit !found}'
}

container_running() {
  docker ps --format '{{.Names}}' | awk -v n="$BROKER_NAME" '$0==n{found=1} END{exit !found}'
}

write_conf() {
  mkdir -p "$BROKER_WORKDIR"
  cat > "$BROKER_CONF" <<CONF
allow_anonymous true
persistence false
listener 1883
protocol mqtt
listener 9001
protocol websockets
CONF
}

cmd_start() {
  require_cmd docker
  write_conf

  if container_running; then
    echo "broker already running: ${BROKER_NAME}"
    return 0
  fi

  if container_exists; then
    docker start "$BROKER_NAME" >/dev/null
    echo "broker started: ${BROKER_NAME}"
  else
    docker run -d \
      --name "$BROKER_NAME" \
      -p "${BROKER_PORT}:1883" \
      -p "${BROKER_WS_PORT}:9001" \
      -v "${BROKER_CONF}:/mosquitto/config/mosquitto.conf:ro" \
      "$BROKER_IMAGE" >/dev/null
    echo "broker created and started: ${BROKER_NAME}"
  fi

  cmd_status
}

cmd_stop() {
  require_cmd docker
  if container_running; then
    docker stop "$BROKER_NAME" >/dev/null
    echo "broker stopped: ${BROKER_NAME}"
  else
    echo "broker is not running: ${BROKER_NAME}"
  fi
}

cmd_restart() {
  cmd_stop
  cmd_start
}

cmd_status() {
  require_cmd docker
  if container_running; then
    echo "status: RUNNING"
    echo "name: ${BROKER_NAME}"
    echo "image: ${BROKER_IMAGE}"
    echo "mqtt: mqtt://127.0.0.1:${BROKER_PORT}"
    echo "ws:   ws://127.0.0.1:${BROKER_WS_PORT}"
    docker ps --filter "name=^/${BROKER_NAME}$" --format 'container={{.Names}} state={{.Status}} ports={{.Ports}}'
  elif container_exists; then
    echo "status: STOPPED"
    echo "name: ${BROKER_NAME}"
    docker ps -a --filter "name=^/${BROKER_NAME}$" --format 'container={{.Names}} state={{.Status}}'
  else
    echo "status: ABSENT"
    echo "name: ${BROKER_NAME}"
  fi
}

cmd_logs() {
  require_cmd docker
  if ! container_exists; then
    echo "error: broker container does not exist: ${BROKER_NAME}" >&2
    exit 1
  fi
  docker logs -f "$BROKER_NAME"
}

parse_globals() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --name)
        BROKER_NAME="$2"
        shift 2
        ;;
      --port)
        BROKER_PORT="$2"
        shift 2
        ;;
      --ws-port)
        BROKER_WS_PORT="$2"
        shift 2
        ;;
      --workdir)
        BROKER_WORKDIR="$2"
        BROKER_CONF="${BROKER_WORKDIR}/mosquitto.conf"
        shift 2
        ;;
      -h|--help)
        usage
        exit 0
        ;;
      --)
        shift
        break
        ;;
      -*)
        echo "error: unknown option '$1'" >&2
        usage
        exit 1
        ;;
      *)
        break
        ;;
    esac
  done

  if [[ $# -eq 0 ]]; then
    usage
    exit 1
  fi

  COMMAND="$1"
  shift || true

  if [[ $# -gt 0 ]]; then
    echo "error: unexpected arguments: $*" >&2
    usage
    exit 1
  fi
}

COMMAND=""
parse_globals "$@"

case "$COMMAND" in
  start) cmd_start ;;
  stop) cmd_stop ;;
  restart) cmd_restart ;;
  status) cmd_status ;;
  logs) cmd_logs ;;
  *)
    echo "error: unknown command '${COMMAND}'" >&2
    usage
    exit 1
    ;;
esac
