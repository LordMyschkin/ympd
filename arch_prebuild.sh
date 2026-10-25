#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: ./arch_prebuild.sh

Installiert auf Arch Linux die Build- und Laufzeitpakete für ympd-webradio
und das mitgebaute Ashuffle. pacman führt dabei ein vollständiges
System-Upgrade durch. Build und systemd-Einrichtung erfolgen anschließend
wie in README.md beschrieben.

  -h, --help  Diese Hilfe anzeigen
EOF
}

case "${1:-}" in
  '') ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { usage >&2; exit 2; }

[[ -r /etc/os-release ]] || { printf 'Fehler: /etc/os-release fehlt.\n' >&2; exit 1; }
# shellcheck disable=SC1091
source /etc/os-release
[[ "${ID:-}" == arch ]] || {
  printf 'Fehler: Dieses Skript ist nur für Arch Linux vorgesehen.\n' >&2
  exit 1
}
command -v pacman >/dev/null || { printf 'Fehler: pacman wurde nicht gefunden.\n' >&2; exit 1; }

root_cmd=()
if (( EUID != 0 )); then
  command -v sudo >/dev/null || {
    printf 'Fehler: sudo fehlt. Bitte sudo installieren oder das Skript als root ausführen.\n' >&2
    exit 1
  }
  root_cmd=(sudo)
fi

printf 'Installiere Arch-Pakete mit vollständigem System-Upgrade.\n'
"${root_cmd[@]}" pacman -Syu --needed \
  base-devel cmake pkgconf libmpdclient openssl mpd mpc curl
printf 'Abhängigkeiten installiert. Build-Anleitung: README.md\n'
