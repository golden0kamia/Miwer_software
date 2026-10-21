#!/usr/bin/env python3
"""Simulateur d'appareil Miwer (protocole v1, voir docs/PROTOCOL.md).

Crée un pseudo-terminal qui se comporte comme l'appareil USB : il répond à HELLO / GET /
PING, applique SET / MUTE / NAME / ACTIVE à son état (5 faders motorisés, 5 mutes) et
affiche tout ce que le PC envoie.

Utilisation :
    tools/miwer_sim.py                 # mode interactif, affiche le chemin du port
    miwer -d /dev/pts/N                # dans un autre terminal

Commandes (clavier ou script) :
    v <c> <val>     mouvement du fader c (1..5) jusqu'à val (0..1000) -> envoie « V c val »
    b <c>           appui puis relâchement du bouton c -> « B c 1 » puis « B c 0 »
    p <c> / r <c>   appui seul / relâchement seul du bouton c
    s               affiche l'état
    raw <texte>     envoie une ligne telle quelle (ex. « raw 512|0|0|0|0|0|0|0|0|0 »)
    sleep <s>       (script) attend en continuant de répondre au PC
    expect <regex>  (script) attend une ligne reçue du PC correspondant à regex (le reste de
                    la ligne), sinon échec (code de sortie 1)
    timeout <s>     (script) délai d'attente de « expect » (5 s par défaut)
    q               quitte

Mode non interactif : --script fichier (une commande par ligne, # pour les commentaires)
ou --cmd "v 1 500; sleep 0.5; s" ; le simulateur quitte à la fin du script.

Bibliothèque standard uniquement.
"""

import argparse
import os
import pty
import re
import select
import sys
import time
import tty

NUM_CHANNELS = 5
MAX_VALUE = 1000
MAX_LINE = 63
VERSION = "1.0"


class Device:
    def __init__(self, faders, link=None, quiet=False):
        self.master, self.slave = pty.openpty()
        # Mode brut côté esclave : pas d'écho ni de traduction de fins de ligne avant que
        # Miwer n'ouvre le port (il refait cfmakeraw de son côté).
        tty.setraw(self.slave)
        self.path = os.ttyname(self.slave)
        self.link = link
        if link:
            try:
                os.unlink(link)
            except FileNotFoundError:
                pass
            os.symlink(self.path, link)
        os.set_blocking(self.master, False)

        self.faders = list(faders)
        self.mutes = [False] * NUM_CHANNELS
        self.names = [""] * NUM_CHANNELS
        self.active = [0] * NUM_CHANNELS
        self.session = False
        self.buffer = b""
        self.discarding = False
        self.received = []  # lignes reçues non encore consommées par « expect »
        self.quiet = quiet
        self.expect_timeout = 5.0

    def close(self):
        if self.link:
            try:
                os.unlink(self.link)
            except FileNotFoundError:
                pass
        os.close(self.master)
        os.close(self.slave)

    # ------------------------------------------------------------ Affichage

    def log(self, msg):
        if not self.quiet:
            print(msg, flush=True)

    def state_text(self):
        parts = []
        for i in range(NUM_CHANNELS):
            parts.append("%d:%s=%d%s(%d)" % (i + 1, self.names[i] or "-", self.faders[i],
                                             "M" if self.mutes[i] else "", self.active[i]))
        return "ETAT " + " ".join(parts)

    # ------------------------------------------------------------ Envoi

    def send(self, line):
        self.log("  sim -> " + line)
        data = (line + "\n").encode("ascii")
        while data:
            try:
                n = os.write(self.master, data)
                data = data[n:]
            except BlockingIOError:
                select.select([], [self.master], [], 1.0)

    def send_state(self):
        self.send("STATE " + " ".join(str(v) for v in self.faders))

    # ------------------------------------------------------------ Réception

    def poll(self, timeout):
        """Lit ce que le PC envoie (attend au plus timeout secondes)."""
        r, _, _ = select.select([self.master], [], [], timeout)
        if not r:
            return
        try:
            data = os.read(self.master, 4096)
        except (BlockingIOError, OSError):
            return
        self.buffer += data
        while b"\n" in self.buffer:
            raw, self.buffer = self.buffer.split(b"\n", 1)
            if self.discarding:
                self.discarding = False
                continue
            if raw.endswith(b"\r"):
                raw = raw[:-1]
            if len(raw) > MAX_LINE:
                self.log("  (ligne trop longue ignorée)")
                continue
            self.handle(raw.decode("ascii", errors="replace"))
        if len(self.buffer) > MAX_LINE:
            self.buffer = b""
            self.discarding = True

    def handle(self, line):
        self.log("  PC  -> " + line)
        self.received.append(line)
        words = line.split()
        if not words:
            return
        kw, args = words[0], words[1:]

        def channel(s):
            c = int(s)
            if not 1 <= c <= NUM_CHANNELS:
                raise ValueError
            return c - 1

        try:
            if kw == "HELLO" and not args:
                self.session = True
                self.send("HELLO MIWER %s %d" % (VERSION, NUM_CHANNELS))
                self.send_state()
            elif kw == "GET" and not args:
                self.send_state()
            elif kw == "PING" and not args:
                self.send("PONG")
            elif kw == "SET" and len(args) == 2:
                c, v = channel(args[0]), int(args[1])
                if not 0 <= v <= MAX_VALUE:
                    raise ValueError
                self.log("  [moteur] fader %d : %d -> %d" % (c + 1, self.faders[c], v))
                self.faders[c] = v
            elif kw == "MUTE" and len(args) == 2 and args[1] in ("0", "1"):
                self.mutes[channel(args[0])] = args[1] == "1"
            elif kw == "NAME" and len(args) >= 2:
                c = channel(args[0])
                self.names[c] = line.split(None, 2)[2][:8]
            elif kw == "ACTIVE" and len(args) == 2:
                c, n = channel(args[0]), int(args[1])
                if not 0 <= n <= 99:
                    raise ValueError
                self.active[c] = n
            else:
                self.send("ERR commande inconnue")
        except ValueError:
            self.send("ERR argument invalide")

    # ------------------------------------------------------------ Commandes utilisateur

    def command(self, text):
        """Exécute une commande ; retourne False pour quitter. Lève RuntimeError si expect échoue."""
        words = text.split()
        if not words:
            return True
        cmd, args = words[0], words[1:]
        if cmd == "q":
            return False
        if cmd == "s":
            print(self.state_text(), flush=True)
        elif cmd == "v" and len(args) == 2:
            c, v = int(args[0]) - 1, max(0, min(MAX_VALUE, int(args[1])))
            if 0 <= c < NUM_CHANNELS:
                self.faders[c] = v
                self.send("V %d %d" % (c + 1, v))
        elif cmd in ("b", "p", "r") and len(args) == 1:
            c = int(args[0])
            if 1 <= c <= NUM_CHANNELS:
                if cmd in ("b", "p"):
                    self.send("B %d 1" % c)
                if cmd == "b":
                    self.sleep(0.05)
                if cmd in ("b", "r"):
                    self.send("B %d 0" % c)
        elif cmd == "raw":
            self.send(text.split(None, 1)[1] if len(words) > 1 else "")
        elif cmd == "sleep" and len(args) == 1:
            self.sleep(float(args[0]))
        elif cmd == "expect" and args:
            self.expect(text.split(None, 1)[1], self.expect_timeout)
        elif cmd == "timeout" and len(args) == 1:
            self.expect_timeout = float(args[0])
        else:
            print("commande inconnue : " + text, flush=True)
        return True

    def sleep(self, seconds):
        end = time.monotonic() + seconds
        while (left := end - time.monotonic()) > 0:
            self.poll(left)

    def expect(self, pattern, timeout):
        regex = re.compile(pattern)
        end = time.monotonic() + timeout
        while True:
            for i, line in enumerate(self.received):
                if regex.search(line):
                    del self.received[: i + 1]
                    self.log("  (attendu : %s)" % line)
                    return
            self.received.clear()
            left = end - time.monotonic()
            if left <= 0:
                raise RuntimeError("expect '%s' : rien reçu en %.1f s" % (pattern, timeout))
            self.poll(left)


def main():
    parser = argparse.ArgumentParser(description="Simulateur d'appareil Miwer (protocole v1)")
    parser.add_argument("--script", help="fichier de commandes à exécuter puis quitter")
    parser.add_argument("--cmd", help="commandes séparées par des « ; » à exécuter puis quitter")
    parser.add_argument("--faders", default="500 500 500 500 500",
                        help="positions initiales des 5 faders (0..1000)")
    parser.add_argument("--link", help="crée un lien symbolique vers le pty (chemin stable pour les scripts)")
    parser.add_argument("--quiet", action="store_true", help="n'affiche pas les échanges")
    opts = parser.parse_args()

    faders = [max(0, min(MAX_VALUE, int(v))) for v in opts.faders.split()]
    faders = (faders + [0] * NUM_CHANNELS)[:NUM_CHANNELS]

    dev = Device(faders, link=opts.link, quiet=opts.quiet)
    print("Port du simulateur : %s%s" % (dev.path, "  (lien : %s)" % opts.link if opts.link else ""), flush=True)
    print("Lancer :  miwer -d %s" % (opts.link or dev.path), flush=True)

    code = 0
    try:
        if opts.script or opts.cmd:
            if opts.script:
                with open(opts.script, encoding="utf-8") as f:
                    commands = f.read().splitlines()
            else:
                commands = opts.cmd.split(";")
            for c in commands:
                c = c.split("#", 1)[0].strip()
                if c and not dev.command(c):
                    break
            print(dev.state_text(), flush=True)
        else:
            print("Commandes : v <c> <val>, b <c>, p <c>, r <c>, s, raw <texte>, q", flush=True)
            running = True
            while running:
                r, _, _ = select.select([dev.master, sys.stdin], [], [])
                if dev.master in r:
                    dev.poll(0)
                if sys.stdin in r:
                    line = sys.stdin.readline()
                    if not line:
                        break
                    try:
                        running = dev.command(line.strip())
                    except (ValueError, RuntimeError) as e:
                        print("erreur : %s" % e, flush=True)
    except RuntimeError as e:
        print("ÉCHEC : %s" % e, flush=True)
        print(dev.state_text(), flush=True)
        code = 1
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()
    return code


if __name__ == "__main__":
    sys.exit(main())
