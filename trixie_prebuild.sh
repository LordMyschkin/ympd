#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: ./trixie_prebuild.sh [--deps-only]

Installiert auf Debian 13 (Trixie) die benötigten Pakete, baut ympd und
Ashuffle und richtet den systemd-Dienst auf Port 6080 ein.

  --deps-only  Nur Pakete installieren; keinen Build und keinen Dienst starten
  -h, --help   Diese Hilfe anzeigen

Build-Jobs lassen sich mit YMPD_BUILD_JOBS=N festlegen (Standard: 4).
Voraussetzung: MPD als Systemdienst auf 127.0.0.1:6600.
EOF
}

die() {
  printf 'Fehler: %s\n' "$*" >&2
  exit 1
}

deps_only=false
case "${1:-}" in
  '') ;;
  --deps-only) deps_only=true ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { usage >&2; exit 2; }

[[ -r /etc/os-release ]] || die '/etc/os-release fehlt.'
# shellcheck disable=SC1091
source /etc/os-release
[[ "${ID:-}" == debian && "${VERSION_CODENAME:-}" == trixie ]] ||
  die 'Dieses Skript ist nur für Debian 13 (Trixie) vorgesehen.'
command -v apt-get >/dev/null || die 'apt-get wurde nicht gefunden.'

root_cmd=()
if (( EUID != 0 )); then
  command -v sudo >/dev/null || die 'sudo wurde nicht gefunden. Bitte sudo installieren oder das Skript als root ausführen.'
  root_cmd=(sudo)
fi

repo_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
[[ -f "$repo_dir/CMakeLists.txt" && -f "$repo_dir/contrib/ympd-webradio.service" ]] ||
  die 'Der vollständige Quellbaum von ympd-webradio fehlt.'

printf 'Installiere Pakete für Debian 13 (Trixie).\n'
"${root_cmd[@]}" apt-get update
"${root_cmd[@]}" apt-get install -y --no-install-recommends \
  build-essential cmake pkgconf libmpdclient-dev libssl-dev mpd mpc curl

if "$deps_only"; then
  printf 'Abhängigkeiten installiert. Build und Dienst wurden nicht gestartet.\n'
  exit 0
fi

command -v systemctl >/dev/null || die 'systemctl wurde nicht gefunden.'
command -v systemd-analyze >/dev/null || die 'systemd-analyze wurde nicht gefunden.'
build_jobs=${YMPD_BUILD_JOBS:-4}
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || die 'YMPD_BUILD_JOBS muss eine positive Ganzzahl sein.'

printf 'Starte MPD und prüfe die Verbindung auf 127.0.0.1:6600.\n'
"${root_cmd[@]}" systemctl enable --now mpd.service
mpc -h 127.0.0.1 -p 6600 status >/dev/null ||
  die 'MPD ist auf 127.0.0.1:6600 nicht erreichbar. MPD-Konfiguration und journalctl -u mpd.service prüfen.'

printf 'Baue ympd und Ashuffle.\n'
cmake -S "$repo_dir" -B "$repo_dir/build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build "$repo_dir/build" --parallel "$build_jobs"

if systemctl is-active --quiet ympd-webradio.service; then
  "${root_cmd[@]}" systemctl stop ympd-webradio.service
fi
"${root_cmd[@]}" cmake --install "$repo_dir/build"
"${root_cmd[@]}" install -m 0644 "$repo_dir/contrib/ympd-webradio.service" \
  /etc/systemd/system/ympd-webradio.service
"${root_cmd[@]}" systemd-analyze verify /etc/systemd/system/ympd-webradio.service
"${root_cmd[@]}" systemctl daemon-reload
"${root_cmd[@]}" systemctl enable --now ympd-webradio.service

systemctl is-active --quiet ympd-webradio.service ||
  die 'ympd-webradio.service läuft nicht. journalctl -u ympd-webradio.service -b prüfen.'

http_code=''
for attempt in 1 2 3 4 5 6 7 8 9 10; do
  http_code=$(curl --silent --output /dev/null --write-out '%{http_code}' \
    http://127.0.0.1:6080/ || true)
  [[ "$http_code" == 200 ]] && break
  sleep 1
done
[[ "$http_code" == 200 ]] ||
  die 'Die Weboberfläche antwortet nicht mit HTTP 200 auf 127.0.0.1:6080. journalctl -u ympd-webradio.service -b prüfen.'

printf 'Installation abgeschlossen: http://127.0.0.1:6080/\n'
