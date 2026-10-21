# Miwer

Mixeur de volume par application pour Linux / PipeWire, piloté par un appareil USB
à 5 canaux : un fader motorisé et un bouton mute par canal, et un écran qui affiche l'état
de chaque canal.

## Matériel

L'appareil de référence est construit autour d'un **STM32C071** :

- 5 potentiomètres linéaires **motorisés** (faders) ;
- 5 boutons mute ;
- un écran TFT 320x240 qui affiche pour chaque canal son libellé, le volume, l'état muet
  et le nombre de flux audio actifs ;
- un port USB CDC-ACM (port série virtuel, aucun pilote nécessaire sous Linux).

Le firmware STM32 est développé dans le dépôt **Miwer_firmware** (projet STM32CubeMX).
Le protocole entre le PC et l'appareil est décrit dans [docs/PROTOCOL.md](docs/PROTOCOL.md)
(copie de la référence commune du dépôt firmware).

## Fonctionnement

- Miwer se connecte à PipeWire, suit les flux audio des applications (`Stream/Output/Audio`),
  la sortie et l'entrée par défaut, et leur applique le volume (`channelVolumes`) et le mute
  du canal auquel ils sont assignés. Les applications lancées après coup reçoivent tout de
  suite le volume de leur canal.
- À la connexion, Miwer envoie `HELLO` ; l'appareil répond `HELLO MIWER 1.0 5` puis
  `STATE` avec la position de ses faders. Pour chaque canal :
  - si Miwer connaît déjà le volume du canal, il envoie `SET` et le **fader motorisé rejoint
    ce volume** ;
  - sinon il adopte la position du fader ;
  - puis il envoie le libellé (`NAME`), l'état muet (`MUTE`) et le nombre de flux (`ACTIVE`).
- Fader déplacé à la main (`V c v`) → le volume est appliqué aux applications du canal.
  Miwer ne renvoie jamais de `SET` en réponse (règle anti-boucle).
- Bouton appuyé (`B c 1`) → le mute du canal est basculé et `MUTE` est renvoyé pour l'écran.
- Arrivée ou départ d'une application → `ACTIVE`.
- Si l'appareil est débranché, Miwer réessaie de s'y connecter toutes les 2 secondes.

### Faders motorisés : suivi des changements externes

Avec `follow_external = true` (par défaut), les faders suivent aussi ce qui se passe ailleurs :
touches multimédia, pavucontrol, `wpctl set-volume`, application qui change son propre volume…

- Quand un flux assigné à un canal change de volume ou de mute hors de Miwer, ce nouvel état
  devient celui du canal : il est appliqué aux autres flux du canal et envoyé à l'appareil
  (`SET` / `MUTE`), dont le fader motorisé se déplace. Les envois sont limités à environ
  20 par seconde et par canal (seule la dernière valeur compte).
- Au démarrage, un canal dont le volume est encore inconnu prend le volume réel de ses
  applications (par exemple le volume système pour `master`) : les faders se placent sur
  l'état réel du PC.
- Les échos des réglages faits par Miwer lui-même ne sont pas pris pour des changements
  externes, ni le volume initial (souvent 100 %) d'une application qui vient d'apparaître.
- Si l'utilisateur tient le fader, l'appareil n'actionne pas le moteur (voir le protocole).

Avec `follow_external = false`, Miwer impose ses volumes et ignore les changements externes
(comportement des versions précédentes) ; les faders sont seulement repositionnés à la
connexion de l'appareil.

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

### Tests

```sh
cmake -B build -DMIWER_BUILD_TESTS=ON
cmake --build build
cd build && ctest            # « ctest -LE e2e » pour ne lancer que les tests unitaires
```

- `protocol` : tests unitaires du module protocole (`src/device_protocol.*`).
- `e2e_sim` : test de bout en bout avec le simulateur (`tests/e2e_sim.sh`). Il nécessite
  `python3` et une session PipeWire, mais ne vise que des applications inexistantes :
  aucun volume réel n'est modifié.

## Configuration

```sh
mkdir -p ~/.config/miwer
cp config.example.ini ~/.config/miwer/config.ini
```

Chaque canal reçoit une liste d'applications (séparées par des virgules) ou une cible spéciale :
`master` (sortie par défaut), `mic` (micro par défaut), `others` (toutes les applis non assignées).

| Clé | Défaut | Rôle |
|---|---|---|
| `device` | `auto` | Port série, ou `auto` (voir ci-dessous). |
| `follow_external` | `true` | Les faders suivent les changements faits ailleurs. |
| `channel1`..`channel5` | | Applications ou cible spéciale de chaque canal. |
| `label1`..`label5` | déduit | Libellé affiché par l'écran (8 caractères ASCII au plus). Par défaut : `Master`, `Micro`, `Autres`, ou la première application du canal avec une majuscule. Les accents courants sont translittérés (é → e), les autres caractères non ASCII supprimés. |
| `invert_sliders` | `false` | Inverse le sens des faders (positions reçues et `SET` envoyés). |
| `mute_mode` | `toggle` | `toggle` : un appui bascule le mute. `state` : l'appareil envoie l'état du mute (1 = muet). |
| `baudrate` | `115200` | Ignoré par l'appareil STM32 (USB natif). |
| `adc_max`, `noise_threshold` | `1023`, `0.01` | Ancien format de trame uniquement. |
| `verbose` | `false` | Affiche les flux détectés, leur canal et les échanges série. |

Pour connaître le nom à utiliser pour une application, lancez-la puis :

```sh
miwer -v
```

Chaque flux détecté est affiché avec son `app=` et son `bin=`, ainsi que le canal choisi.

### Détection du port

Avec `device = auto`, Miwer cherche dans `/dev/serial/by-id/` une entrée dont le nom contient
« Miwer » (insensible à la casse), par exemple
`/dev/serial/by-id/usb-Miwer_Miwer_Mixer_<série>-if00`, et prend `/dev/ttyACM0` à défaut.
La détection est refaite à chaque tentative de reconnexion. L'option `-d` est prioritaire
sur la configuration (`miwer -d /dev/ttyACM1`).

L'utilisateur doit avoir accès au port série (groupe `dialout` sur Debian/Ubuntu, `uucp` sur Arch) :

```sh
sudo usermod -aG dialout $USER   # puis se reconnecter
```

## Simulateur d'appareil

`tools/miwer_sim.py` (Python 3, bibliothèque standard uniquement) crée un pseudo-terminal qui
se comporte comme l'appareil (protocole v1, 5 faders, 5 mutes) et affiche tout ce que le PC
envoie :

```sh
tools/miwer_sim.py                  # affiche « Port du simulateur : /dev/pts/N »
miwer -d /dev/pts/N -v              # dans un autre terminal
```

Commandes clavier :

| Commande | Effet |
|---|---|
| `v 1 500` | Fader 1 déplacé à 500 (envoie `V 1 500`). |
| `b 2` | Appui puis relâchement du bouton 2 (`B 2 1`, `B 2 0`). |
| `s` | Affiche l'état (libellé, position, mute, nombre de flux). |
| `raw <texte>` | Envoie une ligne telle quelle (ex. une trame de l'ancien format). |
| `q` | Quitte. |

Mode non interactif : `--script fichier` ou `--cmd "v 1 500; sleep 0.5; s"`, avec en plus
`sleep <s>`, `expect <regex>` (attend une ligne du PC, échec sinon) et `timeout <s>`.
`--link chemin` crée un lien symbolique stable vers le pty, `--faders "0 250 500 750 1000"`
fixe les positions initiales.

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

## Firmware Arduino de référence (ancien format)

`firmware/miwer_firmware/miwer_firmware.ino` est l'ancien firmware de référence Arduino,
sans moteurs ni écran : potentiomètres sur A0..A4, boutons sur D2..D6 vers GND. Il envoie
toutes les 20 ms une trame :

```
v1|v2|v3|v4|v5|m1|m2|m3|m4|m5
```

`v` = position du potentiomètre (0..`adc_max`), `m` = bouton (0/1). Miwer accepte toujours ce
format (le `HELLO` envoyé à la connexion est simplement ignoré par ce firmware). Pour un ADC
12 bits, réglez `adc_max = 4095`. Si le firmware gère lui-même l'état du mute (par exemple
pour piloter des LED), il doit envoyer 1 = muet et la configuration doit utiliser
`mute_mode = state`.
