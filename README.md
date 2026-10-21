# Miwer

Mixeur de volume par application pour Linux / PipeWire, piloté par un appareil USB
à 5 canaux (un potentiomètre + un bouton mute par canal).

## Fonctionnement

- L'appareil apparaît comme un port série USB (`/dev/ttyACM0`) et envoie une trame par ligne :

  ```
  v1|v2|v3|v4|v5|m1|m2|m3|m4|m5
  ```

  `v` = position du potentiomètre (0..`adc_max`), `m` = bouton (0/1).
- Miwer se connecte à PipeWire, suit les flux audio des applications (`Stream/Output/Audio`),
  la sortie et l'entrée par défaut, et leur applique le volume (`channelVolumes`) et le mute
  du canal auquel ils sont assignés.
- Les applications lancées après coup reçoivent tout de suite le volume de leur canal.
- Si l'appareil est débranché, Miwer réessaie de s'y connecter toutes les 2 secondes.

## Compilation

```sh
# Debian / Ubuntu
sudo apt install build-essential cmake pkg-config libpipewire-0.3-dev
# Fedora : sudo dnf install gcc-c++ cmake pipewire-devel
# Arch   : sudo pacman -S base-devel cmake pipewire

cmake -B build
cmake --build build
sudo cmake --install build   # installe /usr/local/bin/miwer
```

## Configuration

```sh
mkdir -p ~/.config/miwer
cp config.example.ini ~/.config/miwer/config.ini
```

Chaque canal reçoit une liste d'applications (séparées par des virgules) ou une cible spéciale :
`master` (sortie par défaut), `mic` (micro par défaut), `others` (toutes les applis non assignées).

Pour connaître le nom à utiliser pour une application, lancez-la puis :

```sh
miwer -v
```

Chaque flux détecté est affiché avec son `app=` et son `bin=`, ainsi que le canal choisi.

L'utilisateur doit avoir accès au port série (groupe `dialout` sur Debian/Ubuntu, `uucp` sur Arch) :

```sh
sudo usermod -aG dialout $USER   # puis se reconnecter
```

## Démarrage automatique (systemd utilisateur)

`~/.config/systemd/user/miwer.service` :

```ini
[Unit]
Description=Miwer - mixeur de volume USB
After=pipewire.service wireplumber.service
Requires=pipewire.service

[Service]
ExecStart=/usr/local/bin/miwer
Restart=on-failure
RestartSec=3

[Install]
WantedBy=default.target
```

```sh
systemctl --user daemon-reload
systemctl --user enable --now miwer
```

## Firmware

`firmware/miwer_firmware/miwer_firmware.ino` est un firmware de référence Arduino :
potentiomètres sur A0..A4, boutons sur D2..D6 vers GND. Il suffit qu'un autre
microcontrôleur (Pico, ESP32...) envoie le même format de trame. Pour un ADC 12 bits,
réglez `adc_max = 4095`.

Si le firmware gère lui-même l'état du mute (par exemple pour piloter des LED), il doit envoyer
1 = muet et la configuration doit utiliser `mute_mode = state`.
