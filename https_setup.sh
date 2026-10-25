#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Usage: ./https_setup.sh --domain DOMAIN [--auth-user BENUTZER]

Richtet auf Debian 13 HTTPS für die lokale ympd-Weboberfläche ein.
Wenn Apache oder nginx läuft, werden passende Anweisungen ausgegeben.
Andernfalls wird Caddy mit TLS und Passwortschutz installiert.

  --domain DOMAIN     DNS-Name der Weboberfläche, z. B. musik.example.org
  --auth-user NAME    Benutzername für den Browser-Zugang (Standard: ympd)
  -h, --help          Diese Hilfe anzeigen
EOF
}

die() {
  printf 'Fehler: %s\n' "$*" >&2
  exit 1
}

domain=''
auth_user='ympd'
while (($#)); do
  case "$1" in
    --domain)
      [[ $# -ge 2 ]] || die '--domain benötigt einen Domainnamen.'
      domain=$2
      shift 2
      ;;
    --auth-user)
      [[ $# -ge 2 ]] || die '--auth-user benötigt einen Benutzernamen.'
      auth_user=$2
      shift 2
      ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
  esac
done

[[ -n "$domain" ]] || die 'Bitte --domain DOMAIN angeben.'
[[ ${#domain} -le 253 && "$domain" == *.* && "$domain" != .* && "$domain" != *. && "$domain" != *..* ]] ||
  die 'Bitte einen vollständigen DNS-Namen ohne Port angeben.'
IFS='.' read -r -a domain_labels <<<"$domain"
for label in "${domain_labels[@]}"; do
  [[ ${#label} -le 63 && "$label" =~ ^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?$ ]] ||
    die 'Ungültiger DNS-Name. Jede Komponente muss aus Buchstaben, Ziffern oder Bindestrichen bestehen.'
done
[[ "$auth_user" =~ ^[A-Za-z0-9_][A-Za-z0-9_.-]*$ ]] ||
  die 'Benutzername darf nur Buchstaben, Ziffern, Punkt, Unterstrich und Bindestrich enthalten.'
[[ -r /etc/os-release ]] || die '/etc/os-release fehlt.'
# shellcheck disable=SC1091
source /etc/os-release
[[ "${ID:-}" == debian && "${VERSION_CODENAME:-}" == trixie ]] ||
  die 'Dieses Skript ist nur für Debian 13 (Trixie) vorgesehen.'
command -v systemctl >/dev/null || die 'systemctl wurde nicht gefunden.'
command -v curl >/dev/null || die 'curl wurde nicht gefunden.'
backend_http_code=$(curl --silent --max-time 5 --output /dev/null --write-out '%{http_code}' \
  http://127.0.0.1:6080/ || true)
[[ "$backend_http_code" == 200 || "$backend_http_code" == 401 ]] ||
  die 'ympd ist auf 127.0.0.1:6080 nicht bereit. Zuerst ./trixie_prebuild.sh ausführen.'

root_cmd=()
if (( EUID != 0 )); then
  command -v sudo >/dev/null || die 'sudo wurde nicht gefunden.'
  root_cmd=(sudo)
fi

apache_running=false
nginx_running=false
if systemctl is-active --quiet apache2.service || pgrep -x apache2 >/dev/null 2>&1; then
  apache_running=true
fi
if systemctl is-active --quiet nginx.service || pgrep -x nginx >/dev/null 2>&1; then
  nginx_running=true
fi

if "$apache_running" || "$nginx_running"; then
  printf '\nEin vorhandener Webserver wurde erkannt. Seine Konfiguration wird nicht überschrieben.\n'
  if "$apache_running"; then
    apache_htpasswd_flag='-c '
    [[ -e /etc/apache2/ympd-webradio.htpasswd ]] && apache_htpasswd_flag=''
    cat <<EOF

Apache läuft. Für ${domain}:
1. sudo apt install certbot python3-certbot-apache apache2-utils
2. sudo a2enmod proxy proxy_http ssl
3. sudo htpasswd -B ${apache_htpasswd_flag}/etc/apache2/ympd-webradio.htpasswd ${auth_user}
4. /etc/apache2/sites-available/ympd-webradio.conf zunächst so anlegen:

<VirtualHost *:80>
    ServerName ${domain}
    DocumentRoot /var/www/html
</VirtualHost>

5. sudo a2ensite ympd-webradio.conf
6. sudo apache2ctl configtest && sudo systemctl reload apache2
7. sudo certbot certonly --apache -d ${domain}
8. Dieselbe Datei durch die HTTPS-Konfiguration im README-Abschnitt
   „HTTPS mit vorhandenem Apache“ ersetzen (Domain und Benutzer anpassen).
9. sudo apache2ctl configtest && sudo systemctl reload apache2
10. curl -I https://${domain}/  # erwartet 401 ohne Zugangsdaten
EOF
  fi
  if "$nginx_running"; then
    nginx_htpasswd_flag='-c '
    [[ -e /etc/nginx/ympd-webradio.htpasswd ]] && nginx_htpasswd_flag=''
    cat <<EOF

nginx läuft. Für ${domain}:
1. sudo apt install certbot python3-certbot-nginx apache2-utils
2. sudo htpasswd -B ${nginx_htpasswd_flag}/etc/nginx/ympd-webradio.htpasswd ${auth_user}
3. /etc/nginx/sites-available/ympd-webradio zunächst so anlegen:

server {
    listen 80;
    server_name ${domain};
    root /var/www/html;
}

4. sudo ln -s /etc/nginx/sites-available/ympd-webradio /etc/nginx/sites-enabled/ympd-webradio
5. sudo nginx -t && sudo systemctl reload nginx
6. sudo certbot certonly --nginx -d ${domain}
7. Dieselbe Datei durch die HTTPS-Konfiguration im README-Abschnitt
   „HTTPS mit vorhandenem nginx“ ersetzen (Domain und Benutzer anpassen).
8. sudo nginx -t && sudo systemctl reload nginx
9. curl -I https://${domain}/  # erwartet 401 ohne Zugangsdaten
EOF
  fi
  printf '\nVoraussetzung: DNS zeigt auf diesen VPS; Port 80 und 443 sind erreichbar.\n'
  exit 0
fi

if systemctl is-active --quiet caddy.service || command -v caddy >/dev/null 2>&1; then
  die 'Caddy ist bereits installiert. Die bestehende Caddyfile wird nicht überschrieben; siehe README.md.'
fi

[[ -t 0 ]] || die 'Für die Caddy-Einrichtung wird ein Terminal zur Passworteingabe benötigt.'
printf 'Kein Apache oder nginx aktiv. Installiere Caddy für https://%s/.\n' "$domain"
printf 'DNS muss auf diesen VPS zeigen; Port 80 und 443 müssen erreichbar sein.\n'

password=''
password_repeat=''
read -r -s -p "Passwort für ${auth_user}: " password
printf '\n'
read -r -s -p 'Passwort wiederholen: ' password_repeat
printf '\n'
[[ -n "$password" && "$password" == "$password_repeat" ]] ||
  die 'Die Passwörter sind leer oder stimmen nicht überein.'
unset password_repeat

"${root_cmd[@]}" apt-get update
"${root_cmd[@]}" apt-get install -y --no-install-recommends caddy
password_hash=$(printf '%s\n' "$password" | caddy hash-password)
unset password

tmp_caddyfile=$(mktemp)
trap 'rm -f -- "$tmp_caddyfile"' EXIT
chmod 0600 "$tmp_caddyfile"
cat >"$tmp_caddyfile" <<EOF
${domain} {
    basicauth {
        ${auth_user} ${password_hash}
    }
    handle_path /mpd-stream/* {
        reverse_proxy 127.0.0.1:8000
    }
    handle {
        reverse_proxy 127.0.0.1:6080
    }
}
EOF
unset password_hash
caddy validate --config "$tmp_caddyfile" --adapter caddyfile
"${root_cmd[@]}" install -o root -g caddy -m 0640 "$tmp_caddyfile" /etc/caddy/Caddyfile
"${root_cmd[@]}" systemctl enable --now caddy.service
"${root_cmd[@]}" systemctl reload caddy.service
systemctl is-active --quiet caddy.service ||
  die 'Caddy läuft nicht. journalctl -u caddy.service -b prüfen.'

printf '\nCaddy eingerichtet: https://%s/\n' "$domain"
printf 'Nach der Zertifikatsausstellung prüfen: curl -I https://%s/ (erwartet: HTTP 401).\n' "$domain"
printf 'Browser-Stream: MPD-httpd-Output auf 127.0.0.1:8000 benötigt; siehe README.md.\n'
printf 'Bei Zertifikatsproblemen: journalctl -u caddy.service -b prüfen.\n'
