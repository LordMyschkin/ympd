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

Die Unit bindet absichtlich nur an `127.0.0.1`. Auf dem Zielrechner ist die
Oberfläche unter <http://127.0.0.1:6080/> erreichbar. Für den Zugriff von
einem anderen Rechner kann ein SSH-Tunnel verwendet werden:

```sh
ssh -L 6080:127.0.0.1:6080 BENUTZER@SERVER
```

Danach im Browser des eigenen Rechners <http://127.0.0.1:6080/> öffnen.

## Ashuffle-Einstellungen

Unter **Settings → Ashuffle Radio** lassen sich Einzeltitel, ganze Alben oder
Gruppen nach MPD-Tags auswählen. Weitere Felder steuern den Queue-Puffer,
Inline- und YAML-Ausschlussregeln, eine URI-Datei sowie die erweiterten
Ashuffle-Parameter. `Only add this many songs = 0` bedeutet Dauerbetrieb.
Standardmäßig hält Ashuffle ein Lied voraus in der Queue.

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
