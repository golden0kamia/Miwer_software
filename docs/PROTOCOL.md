# Protocole série Miwer — version 1

Ce document est la référence commune entre le firmware (`Miwer_firmware`, STM32C071)
et le logiciel PC (`Miwer_software`, PipeWire).

## Transport

- USB CDC-ACM (port série virtuel, `/dev/ttyACMx` sous Linux). Le débit configuré
  par le PC est accepté mais ignoré.
- Identifiants USB : VID `0x0483`, PID `0x5740`, fabricant `Miwer`, produit `Miwer Mixer`,
  numéro de série = UID du STM32 en hexadécimal.
  Lien stable sous Linux : `/dev/serial/by-id/usb-Miwer_Miwer_Mixer_<série>-if00`.
- L'appareil considère le port « ouvert » tant que le PC maintient le signal DTR
  (Linux l'active à l'ouverture du port et le retire à la fermeture).

## Format

- Texte ASCII, une commande par ligne terminée par `\n` (un `\r` final est toléré et ignoré).
- Mots-clés en majuscules, champs séparés par un espace.
- 63 caractères maximum par ligne (hors `\n`). Une ligne plus longue est ignorée
  jusqu'au `\n` suivant.
- Canaux numérotés de `1` à `5`.
- Valeurs de volume de `0` à `1000` (‰ de la course du fader : 0 = bas, 1000 = haut).
  C'est l'échelle « perceptuelle » : le PC applique lui-même la courbe cubique de PipeWire.
- Toute ligne non reconnue est ignorée (l'appareil peut répondre `ERR ...`, le PC ignore
  ce qu'il ne comprend pas).

## PC → appareil

| Commande | Effet |
|---|---|
| `HELLO` | Ouvre la session. Réponse : `HELLO MIWER <version> <nb_canaux>` puis `STATE ...`. |
| `GET` | Réponse : `STATE ...`. |
| `SET <c> <v>` | Volume du canal `c` décidé par le PC (synchro initiale ou changement fait ailleurs). Le fader motorisé rejoint `v` et l'écran affiche `v`. Si l'utilisateur manipule ce fader (dernier mouvement < 500 ms), le moteur n'est pas actionné. |
| `MUTE <c> <0\|1>` | État muet du canal, pour l'affichage. |
| `NAME <c> <texte>` | Libellé du canal (reste de la ligne, ASCII imprimable). L'écran en affiche au plus 8 caractères. |
| `ACTIVE <c> <n>` | Nombre de flux audio actuellement associés au canal (0..99), pour l'affichage. |
| `PING` | Réponse : `PONG`. |

## Appareil → PC

| Message | Sens |
|---|---|
| `HELLO MIWER <version> <n>` | Réponse à `HELLO`. |
| `STATE <v1> <v2> <v3> <v4> <v5>` | Positions actuelles des faders (réponse à `HELLO` / `GET`). |
| `V <c> <v>` | L'utilisateur a déplacé le fader `c`. Jamais émis pour un mouvement motorisé. Pendant un mouvement : au plus un message toutes les 20 ms par canal, puis toujours la valeur finale. |
| `B <c> <0\|1>` | Bouton du canal `c` appuyé (`1`) ou relâché (`0`), anti-rebond déjà fait. |
| `PONG` | Réponse à `PING`. |
| `ERR <message>` | Commande invalide (aide au débogage). |

Les messages `V` et `B` sont émis dès que le port est ouvert (DTR), même sans `HELLO`,
ce qui permet de tester l'appareil avec un simple terminal série.

## Déroulement d'une session

1. Le PC ouvre le port et envoie `HELLO`.
2. L'appareil répond `HELLO MIWER 1.0 5` puis `STATE v1 v2 v3 v4 v5`, et affiche « connecté ».
3. Pour chaque canal, le PC envoie `NAME`, `MUTE`, `ACTIVE`, puis :
   - `SET` s'il connaît déjà le volume du canal (le fader se déplace) ;
   - sinon il adopte la position reçue dans `STATE` comme volume du canal.
4. Ensuite :
   - `V` → le PC applique le volume aux applications du canal ;
   - `B c 1` → le PC bascule le mute du canal et renvoie `MUTE c 0|1` ;
   - volume ou mute modifié ailleurs (touches multimédia, pavucontrol…) → le PC envoie
     `SET` / `MUTE`, le fader motorisé suit ;
   - arrivée ou départ d'une application → `ACTIVE`.
5. À la fermeture du port (DTR retombe), l'appareil affiche « en attente du PC ».

## Règles anti-boucle

- Le PC n'envoie jamais `SET` en réaction à un `V` : ce volume vient déjà de l'appareil.
- L'appareil n'émet jamais `V` pour un déplacement motorisé ; après un `SET`, seul un écart
  dû à l'utilisateur produit à nouveau des `V`. Exception : si le mouvement motorisé est
  abandonné loin de sa cible (fader retenu à la main ou bloqué, timeout de 1,5 s), l'appareil
  envoie un `V` avec la position réelle pour que le PC reste synchronisé.
- Le PC limite les `SET` à environ 20 par seconde et par canal (seule la dernière valeur compte).

## Compatibilité

Le logiciel PC accepte toujours l'ancien format de trame du firmware Arduino de référence :
`v1|v2|v3|v4|v5|m1|m2|m3|m4|m5` (valeurs brutes 0..`adc_max`).
