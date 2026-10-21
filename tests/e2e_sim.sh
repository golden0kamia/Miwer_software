#!/bin/sh
# Test de bout en bout : miwer <-> simulateur d'appareil (tools/miwer_sim.py).
#
#   tests/e2e_sim.sh chemin/vers/miwer
#
# Nécessite une session PipeWire (miwer s'y connecte) mais ne modifie AUCUN volume réel :
# la configuration de test ne vise que des applications inexistantes (pas de master, mic
# ni others).
set -eu

MIWER=${1:?usage : $0 chemin/vers/miwer}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SIM="$ROOT/tools/miwer_sim.py"
TMP=$(mktemp -d)
MIWER_PID=""

cleanup() {
    [ -n "$MIWER_PID" ] && kill "$MIWER_PID" 2>/dev/null || true
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

cat > "$TMP/test.ini" <<'EOF'
channel1 = miwer-e2e-a
channel2 = miwer-e2e-b
channel3 = miwer-e2e-c
label3 = Défilé ça
channel4 = miwer-e2e-d
channel5 = miwer-e2e-e
EOF

# Session 1 : synchronisation initiale (volumes inconnus -> le PC adopte les faders),
# fader déplacé (pas de SET en retour), boutons mute.
cat > "$TMP/session1.txt" <<'EOF'
expect ^HELLO$
expect ^NAME 1 Miwer-e2
expect ^NAME 3 Defile c$
expect ^ACTIVE 5 0$
v 1 300
b 2
expect ^MUTE 2 1$
b 2
expect ^MUTE 2 0$
sleep 0.5
EOF

# Session 2 (reconnexion) : le PC connaît les volumes -> SET pour remettre les faders
cat > "$TMP/session2.txt" <<'EOF'
timeout 6
expect ^HELLO$
expect ^SET 1 300$
expect ^SET 2 200$
expect ^SET 5 500$
expect ^ACTIVE 5 0$
sleep 0.3
EOF

fail() {
    echo "ÉCHEC : $1"
    echo "--- journal miwer"
    cat "$TMP/miwer.log"
    exit 1
}

python3 "$SIM" --link "$TMP/pty" --faders "100 200 300 400 500" --script "$TMP/session1.txt" \
    > "$TMP/sim1.log" 2>&1 &
SIM_PID=$!
sleep 0.5
"$MIWER" -c "$TMP/test.ini" -d "$TMP/pty" -v > "$TMP/miwer.log" 2>&1 &
MIWER_PID=$!

wait "$SIM_PID" || { cat "$TMP/sim1.log"; fail "session 1"; }
grep -q '^  PC  -> SET' "$TMP/sim1.log" && { cat "$TMP/sim1.log"; fail "SET inattendu en session 1"; }
echo "session 1 : OK"

# Le simulateur a fermé le pty : miwer doit se reconnecter au nouveau (toutes les 2 s)
python3 "$SIM" --link "$TMP/pty" --faders "900 900 900 900 900" --script "$TMP/session2.txt" \
    > "$TMP/sim2.log" 2>&1 || { cat "$TMP/sim2.log"; fail "session 2"; }
echo "session 2 (reconnexion) : OK"

kill "$MIWER_PID"
wait "$MIWER_PID" 2>/dev/null || true
MIWER_PID=""
echo "Test de bout en bout réussi"
