# ympd-webradio

ympd-webradio kombiniert die MPD-Weboberfläche ympd mit Ashuffle 3.14.10.
Ein CMake-Build erzeugt **`ympd` und `ashuffle`**. ympd startet das mitgebaute
Ashuffle-Programm aus demselben Verzeichnis und überwacht es. Ein eigener
Ashuffle-Dienst wird nicht benötigt.

Die Ashuffle-Quellen sowie die festgelegten Versionen von Abseil und yaml-cpp
liegen unter `vendor/ashuffle/`. Herkunft und Lizenzen stehen in
[`vendor/ashuffle/UPSTREAM.md`](vendor/ashuffle/UPSTREAM.md).

## Installation auf Debian 13 (Trixie) mit systemd

Diese Anleitung setzt den **vollständigen Quellbaum dieses Clones** auf dem
Zielrechner voraus. Sie verwendet den lokalen **Systemdienst** `mpd.service`,
MPD auf `127.0.0.1:6600`, Installation nach `/usr/local/bin` und die
Weboberfläche auf `127.0.0.1:6080`. Läuft MPD als Benutzerdienst oder unter
einem anderen Namen, muss die mitgelieferte systemd-Unit angepasst werden.

### Schnellinstallation

Im Wurzelverzeichnis des vollständigen Quellbaums ausführen:

```sh
./trixie_prebuild.sh
```

Das Skript installiert die Pakete, aktiviert und prüft `mpd.service`, baut
beide Programme, installiert die Unit und prüft den HTTP-Zugriff auf Port 6080.
Für einen VPS mit eigener Domain kann der HTTPS-Schritt gleich mitgestartet
werden:

```sh
./trixie_prebuild.sh --https-domain musik.example.org
```

Läuft dort bereits Apache oder nginx, zeigt das Skript die dazu passenden
Schritte an. Andernfalls installiert es Caddy, fragt ein Zugangspasswort ab
und richtet HTTPS automatisch ein. Einzelheiten stehen unter
[HTTPS auf einem VPS](#https-auf-einem-vps).

Es kann zur Aktualisierung erneut ausgeführt werden. Mit
`YMPD_BUILD_JOBS=2 ./trixie_prebuild.sh` lässt sich die Zahl der parallelen
Build-Jobs verringern. `./trixie_prebuild.sh --deps-only` installiert nur die
Pakete. Falls MPD noch nicht auf `127.0.0.1:6600` erreichbar ist, stoppt das
Skript vor Build und Dienstinstallation; MPD muss dann konfiguriert werden.

Die folgenden Schritte beschreiben denselben Ablauf für die manuelle
Installation und Fehlersuche.

### 1. Pakete installieren und MPD prüfen

```sh
sudo apt update
sudo apt install --no-install-recommends \
  build-essential cmake pkgconf libmpdclient-dev libssl-dev \
  mpd mpc curl
sudo systemctl enable --now mpd.service
mpc -h 127.0.0.1 -p 6600 status
```

Der letzte Befehl muss MPD erreichen. MPD benötigt eine konfigurierte
Musikdatenbank beziehungsweise abspielbare Streams und einen Audio-Ausgang.
Die Weboberfläche richtet MPD selbst nicht ein. Falls bereits eine andere
ympd- oder Ashuffle-Instanz dieselbe MPD-Queue steuert, diese vor dem Start
des neuen Dienstes beenden.

### 2. Beide Programme bauen und installieren

Im Wurzelverzeichnis dieses Repositories ausführen:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel 4
sudo cmake --install build
/usr/local/bin/ashuffle --version
```

Der Build muss `build/ympd` und `build/ashuffle` erzeugen. Die Installation
legt beide Dateien nebeneinander unter `/usr/local/bin/` ab. Für den Build
werden weder Meson noch ein separat installiertes Ashuffle benötigt. Auf
Rechnern mit wenig Arbeitsspeicher `--parallel 4` auf `--parallel 2` reduzieren.

### 3. systemd-Unit installieren und starten

```sh
sudo install -m 0644 contrib/ympd-webradio.service \
  /etc/systemd/system/ympd-webradio.service
sudo systemd-analyze verify /etc/systemd/system/ympd-webradio.service
sudo systemctl daemon-reload
sudo systemctl enable --now ympd-webradio.service
```

Die Unit enthält `Requires=mpd.service` und `After=mpd.service`. systemd startet
also zuerst MPD und danach ympd. ympd startet Ashuffle als Unterprozess;
`KillMode=control-group` beendet beim Stoppen beide Prozesse. **Keine zweite
Ashuffle-Unit aktivieren.** Die Unit verwendet einen dynamischen Benutzer und
legt `/var/lib/ympd-webradio/` für dauerhafte Einstellungen an.

### 4. Funktion prüfen

```sh
systemctl is-active mpd.service
systemctl is-active ympd-webradio.service
curl -fsS -o /dev/null -w '%{http_code}\n' http://127.0.0.1:6080/
sudo journalctl -u ympd-webradio.service -b --no-pager -n 50
```

Die beiden `is-active`-Befehle sollen `active`, der HTTP-Test soll `200`
ausgeben. Im Journal sollte eine Zeile `Starting bundled Ashuffle: ...`
mit `/usr/local/bin/ashuffle` erscheinen. Bei einem Startfehler liefert
`systemctl status ympd-webradio.service --no-pager` weitere Details.

Die Unit bindet absichtlich nur an `127.0.0.1`. Der lokale HTTP-Zugriff auf
Port 6080 dient dem Reverse Proxy; im Browser auf einem anderen Rechner wird
die HTTPS-Adresse verwendet. Port 6080 und der MPD-Port 6600 dürfen nicht
öffentlich freigegeben werden.

## HTTPS auf einem VPS

MPD auf Port 6600 spricht kein HTTP. Die Weboberfläche wird deshalb über
einen Reverse Proxy mit TLS und Passwortschutz veröffentlicht; MPD und ympd
bleiben lokal gebunden. Dafür einen eigenen DNS-Namen wie
`musik.example.org` auf den VPS zeigen lassen und eingehende TCP-Ports 80
und 443 in Firewall beziehungsweise VPS-Panel erlauben. Port 80 dient nur
der Zertifikatsausstellung und der Weiterleitung auf HTTPS. Extern wird die
Oberfläche unter `https://musik.example.org/` aufgerufen.

### MPD-Stream im Browser

Der Play-Button oben rechts spielt den Stream **nur in diesem Browser** ab;
MPD und Ashuffle auf dem Server laufen weiter. Dafür muss MPD einen
`httpd`-Audio-Output bereitstellen. Wenn der vorhandene Stream bereits unter
`127.0.0.1:8000` erreichbar ist, ist keine Änderung an MPD nötig.
Andernfalls beispielsweise in `/etc/mpd.conf` ergänzen:

```conf
audio_output {
    type "httpd"
    name "Browser stream"
    encoder "lame"
    port "8000"
    bind_to_address "127.0.0.1"
    bitrate "192"
    format "44100:16:2"
}
```

Anschließend `sudo systemctl restart mpd.service` ausführen und mit
`mpc outputs` prüfen, ob der Output aktiviert ist. Der Reverse Proxy leitet
`/mpd-stream/` geschützt und verschlüsselt an diesen lokalen Port weiter.
In **Settings → MPD Stream URL** steht bei HTTPS standardmäßig
`/mpd-stream/`. Einen früher gespeicherten `http://...:8000/`-Wert ersetzt
die Oberfläche beim nächsten Laden automatisch. Falls dein MPD-Stream auf
einem anderen Port liegt, den Port in der passenden Proxy-Konfiguration
anpassen. Port 8000 selbst nicht öffentlich freigeben.

Nach einer bereits abgeschlossenen Grundinstallation kann die automatische
Erkennung separat gestartet werden:

```sh
./https_setup.sh --domain musik.example.org
```

`--auth-user NAME` ändert den Browser-Benutzernamen (Standard: `ympd`).
Das Skript prüft den lokalen ympd-Dienst. Wenn Apache oder nginx bereits
läuft, gibt es Anweisungen für diesen Server aus und ändert dessen
Konfiguration nicht. Falls keiner von beiden läuft, installiert es das
Debian-Paket `caddy`, fragt das Passwort zweimal verdeckt ab, schreibt
`/etc/caddy/Caddyfile` mit Passwortschutz und startet Caddy. Es überschreibt
keine bereits vorhandene Caddy-Installation. Die Trixie-Version von Caddy
verwendet die Direktive `basicauth`.

### HTTPS mit vorhandenem Apache

Auf einem VPS mit laufendem Apache **Apache weiterverwenden**. Das folgende
Beispiel nimmt eine neue Subdomain an, die noch keinem VirtualHost zugeordnet
ist. `musik.example.org` überall durch den eigenen DNS-Namen ersetzen.

```sh
sudo apt install certbot python3-certbot-apache apache2-utils
sudo a2enmod proxy proxy_http ssl
sudo htpasswd -B -c /etc/apache2/ympd-webradio.htpasswd ympd
```

`-c` nur beim ersten Anlegen der Passwortdatei verwenden; beim Ändern oder
Hinzufügen eines Benutzers `sudo htpasswd -B DATEI BENUTZER` ohne `-c` ausführen.
Zuerst einen reinen HTTP-VirtualHost für die Zertifikatsprüfung in
`/etc/apache2/sites-available/ympd-webradio.conf` anlegen. Er leitet noch
keine Anfragen an ympd weiter:

```apache
<VirtualHost *:80>
    ServerName musik.example.org
    DocumentRoot /var/www/html
</VirtualHost>
```

```sh
sudo a2ensite ympd-webradio.conf
sudo apache2ctl configtest
sudo systemctl reload apache2
sudo certbot certonly --apache -d musik.example.org
```

Nach erfolgreicher Zertifikatsausstellung **dieselbe Datei** durch diese
endgültige Konfiguration ersetzen:

```apache
<VirtualHost *:80>
    ServerName musik.example.org
    Redirect permanent / https://musik.example.org/
</VirtualHost>

<VirtualHost *:443>
    ServerName musik.example.org
    SSLEngine on
    SSLCertificateFile /etc/letsencrypt/live/musik.example.org/fullchain.pem
    SSLCertificateKeyFile /etc/letsencrypt/live/musik.example.org/privkey.pem
    ProxyRequests Off
    ProxyPass "/mpd-stream/" "http://127.0.0.1:8000/"
    ProxyPassReverse "/mpd-stream/" "http://127.0.0.1:8000/"
    ProxyPass "/" "http://127.0.0.1:6080/" upgrade=websocket
    ProxyPassReverse "/" "http://127.0.0.1:6080/"
    <Location "/">
        AuthType Basic
        AuthName "ympd-webradio"
        AuthUserFile /etc/apache2/ympd-webradio.htpasswd
        Require valid-user
    </Location>
</VirtualHost>
```

```sh
sudo apache2ctl configtest
sudo systemctl reload apache2
sudo install -d -m 0755 /etc/letsencrypt/renewal-hooks/deploy
sudo install -m 0755 contrib/certbot-reload-apache.sh \
  /etc/letsencrypt/renewal-hooks/deploy/ympd-reload-apache
sudo certbot renew --dry-run
curl -I https://musik.example.org/
```

Der letzte Befehl soll ohne Zugangsdaten `401` liefern. Bei einem anderen
Zertifikatsnamen den tatsächlichen Pfad unter `/etc/letsencrypt/live/`
verwenden. Wenn die Domain bereits in einem anderen Apache-VirtualHost
benutzt wird, den TLS-Proxy und den `<Location>`-Block dort eintragen,
statt einen zweiten VirtualHost mit demselben Namen anzulegen.

### HTTPS mit vorhandenem nginx

Für eine neue, noch nicht anderweitig konfigurierte Subdomain:

```sh
sudo apt install certbot python3-certbot-nginx apache2-utils
sudo htpasswd -B -c /etc/nginx/ympd-webradio.htpasswd ympd
```

Auch hier `-c` nur beim ersten Anlegen der Passwortdatei verwenden. Zuerst
einen reinen HTTP-Server-Block in
`/etc/nginx/sites-available/ympd-webradio` anlegen:

```nginx
server {
    listen 80;
    server_name musik.example.org;
    root /var/www/html;
}
```

```sh
sudo ln -s /etc/nginx/sites-available/ympd-webradio /etc/nginx/sites-enabled/ympd-webradio
sudo nginx -t
sudo systemctl reload nginx
sudo certbot certonly --nginx -d musik.example.org
```

Danach die Datei durch diese endgültige Konfiguration ersetzen:

```nginx
server {
    listen 80;
    server_name musik.example.org;
    return 301 https://$host$request_uri;
}

server {
    listen 443 ssl;
    server_name musik.example.org;
    ssl_certificate /etc/letsencrypt/live/musik.example.org/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/musik.example.org/privkey.pem;

    auth_basic "ympd-webradio";
    auth_basic_user_file /etc/nginx/ympd-webradio.htpasswd;

    location /mpd-stream/ {
        proxy_pass http://127.0.0.1:8000/;
        proxy_buffering off;
        proxy_read_timeout 3600s;
    }

    location / {
        proxy_pass http://127.0.0.1:6080;
        proxy_http_version 1.1;
        proxy_set_header Host $host;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 3600s;
    }
}
```

```sh
sudo nginx -t
sudo systemctl reload nginx
sudo install -d -m 0755 /etc/letsencrypt/renewal-hooks/deploy
sudo install -m 0755 contrib/certbot-reload-nginx.sh \
  /etc/letsencrypt/renewal-hooks/deploy/ympd-reload-nginx
sudo certbot renew --dry-run
curl -I https://musik.example.org/
```

Die WebSocket-Header sind für die Live-Anzeige der Oberfläche erforderlich.
Der HTTPS-Test soll ohne Zugangsdaten `401` liefern. Der installierte
Deploy-Hook lädt nginx nach einer Zertifikatserneuerung neu. Für Apache gilt
der entsprechende Hook im Abschnitt davor.
Bei einer bereits vorhandenen nginx-Konfiguration für denselben DNS-Namen
den `location /`-Block und den Passwortschutz in deren Server-Block
integrieren. Die Zertifikatsbefehle setzen voraus, dass die Domain öffentlich
auf diesen VPS zeigt und Port 80 erreichbar ist.

### HTTPS mit Caddy

Wenn Apache und nginx nicht laufen und Caddy noch nicht installiert ist,
erledigt `./https_setup.sh --domain musik.example.org` Installation,
Passwortschutz und systemd-Start automatisch. Caddy verwaltet das Zertifikat
und dessen Erneuerung selbst. Der Aufruf
`curl -I https://musik.example.org/` soll ohne Zugangsdaten `401` liefern;
im Browser erfolgt danach die Passwortabfrage. Bei Problemen:

```sh
sudo journalctl -u caddy.service -b --no-pager -n 50
```

Ist Caddy bereits installiert, die vorhandene `/etc/caddy/Caddyfile`
bearbeiten und folgenden Site-Block ergänzen; den Hash interaktiv mit
`caddy hash-password` erzeugen:

```caddyfile
musik.example.org {
    basicauth {
        ympd HIER_DEN_PASSWORT_HASH_EINTRAGEN
    }
    handle_path /mpd-stream/* {
        reverse_proxy 127.0.0.1:8000
    }
    handle {
        reverse_proxy 127.0.0.1:6080
    }
}
```

Danach mit `sudo caddy validate --config /etc/caddy/Caddyfile` prüfen und
`sudo systemctl reload caddy` ausführen. Wenn eine neuere Caddy-Version ab
2.8 installiert ist, `basic_auth` statt `basicauth` verwenden.

## Ashuffle-Einstellungen

Unter **Settings → Ashuffle Radio** lassen sich Einzeltitel, ganze Alben oder
Gruppen nach MPD-Tags auswählen. Weitere Felder steuern den Queue-Puffer,
Inline- und YAML-Ausschlussregeln, eine URI-Datei sowie die erweiterten
Ashuffle-Parameter. `Only add this many songs = 0` bedeutet Dauerbetrieb.
Standardmäßig hält Ashuffle ein Lied voraus in der Queue.
Mit **Clear Queue** wird die Queue geleert; ein laufendes Ashuffle füllt sie
danach mit neuen Titeln. Die Löschmodi **Up**, **Single** und **Down** in der
rechten Leiste bestimmen, ob beim Klick auf das Papierkorb-Symbol einer
Queue-Zeile alle Titel bis zu dieser Zeile, nur dieser Titel oder alle Titel
ab dieser Zeile entfernt werden.

Die Einstellungen liegen beim systemd-Dienst in
`/var/lib/ympd-webradio/ashuffle.conf`. Dateien für URI-Listen oder
YAML-Ausschlussregeln sollten ebenfalls unter `/var/lib/ympd-webradio/`
liegen: Die Unit verwendet `DynamicUser=yes` und `ProtectHome=yes` und kann
Dateien in Home-Verzeichnissen nicht lesen. `--file -` (URI-Liste über stdin)
ist in der Weboberfläche nicht verfügbar; stattdessen eine Datei angeben.

Wenn Ashuffle bei einer sehr großen Bibliothek beim Einlesen die Verbindung
zu MPD verliert, zuerst `journalctl -u ympd-webradio.service` und das MPD-Log
prüfen. Meldet MPD einen vollen Ausgabepuffer, kann dessen
`max_output_buffer_size` in der MPD-Konfiguration erhöht werden; danach MPD
und ympd-webradio neu starten.

## Aktualisierung

Nach dem Aktualisieren des vollständigen Quellbaums im Repository-Verzeichnis:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel 4
sudo systemctl stop ympd-webradio.service
sudo cmake --install build
sudo systemctl start ympd-webradio.service
```

Wurde auch `contrib/ympd-webradio.service` geändert, die Datei erneut nach
`/etc/systemd/system/` kopieren und anschließend `sudo systemctl daemon-reload`
sowie `sudo systemctl restart ympd-webradio.service` ausführen.

## Andere Installationen

### Arch Linux

Im Wurzelverzeichnis des vollständigen Quellbaums:

```sh
./arch_prebuild.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build --parallel 4
sudo cmake --install build
```

`arch_prebuild.sh` installiert mit `pacman -Syu --needed` die Pakete
`base-devel`, `cmake`, `pkgconf`, `libmpdclient`, `openssl`, `mpd`, `mpc` und
`curl`. Dies umfasst ein vollständiges Arch-System-Upgrade. Anschließend
MPD als Systemdienst konfigurieren und starten. Wenn er auf
`127.0.0.1:6600` erreichbar ist, kann die Unit wie oben in Schritt 3
installiert und mit Schritt 4 geprüft werden.

### Ohne systemd

Ohne systemd können `ympd` und `ashuffle` direkt aus demselben Build- oder
Installationsverzeichnis verwendet werden. ympd startet Ashuffle dann selbst.
Der Standard-Webport von ympd ist **6080**; mit `--webport [ip:]PORT` lässt er
sich ändern. `--host` und `--port` wählen die MPD-Verbindung.

Das Dockerfile verwendet ebenfalls Debian Trixie und baut beide Programme.
Der Container-Build wurde in dieser Entwicklungsumgebung nicht ausgeführt.

## Lizenz und Herkunft

ympd basiert auf dem Projekt von Andrew Karpow (GPLv2). Ashuffle stammt von
Josh Kunz (MIT). Die unveränderten Lizenzdateien der eingebundenen
Abhängigkeiten bleiben unter `vendor/ashuffle/` erhalten.
