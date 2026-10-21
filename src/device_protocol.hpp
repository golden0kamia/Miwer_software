#pragma once

// Protocole série Miwer (voir docs/PROTOCOL.md) : analyse des lignes reçues de l'appareil
// et formatage des commandes envoyées par le PC. Fonctions pures, sans état.
//
// Dans ce module les canaux sont des index internes 0..kNumChannels-1 ; sur le fil ils
// sont numérotés 1..kNumChannels.

#include "config.hpp"

#include <string>
#include <variant>
#include <vector>

namespace proto {

constexpr int kMaxValue = 1000;   // valeur haute d'un fader (‰ de la course)
constexpr int kMaxActive = 99;    // valeur maximale de ACTIVE
constexpr size_t kLabelMax = 8;   // longueur maximale d'un libellé NAME

// HELLO MIWER <version> <n>
struct Hello {
    std::string version;
    int channels = 0;
};

// STATE <v1> ... <vn>
struct State {
    std::vector<int> values;  // 0..kMaxValue
};

// V <c> <v> : fader déplacé par l'utilisateur
struct Fader {
    int channel = 0;  // 0..kNumChannels-1
    int value = 0;    // 0..kMaxValue
};

// B <c> <0|1>
struct Button {
    int channel = 0;  // 0..kNumChannels-1
    bool pressed = false;
};

struct Pong {};

// ERR <message>
struct Err {
    std::string message;
};

// Ancien format du firmware Arduino : v1|v2|v3|v4|v5[|m1|m2|m3|m4|m5]
struct LegacyFrame {
    std::vector<int> values;    // valeurs brutes 0..adc_max (au moins kNumChannels)
    std::vector<bool> buttons;  // vide si la trame ne contient pas les boutons
};

// Ligne non reconnue (à ignorer)
struct Unknown {};

using Event = std::variant<Unknown, Hello, State, Fader, Button, Pong, Err, LegacyFrame>;

Event parseLine(const std::string& line);

// Conversions volume (0.0..1.0) <-> valeur du protocole (0..1000)
int toWire(float volume);
float fromWire(int value);

// Libellé en ASCII imprimable : accents courants translittérés, le reste supprimé,
// au plus kLabelMax caractères.
std::string sanitizeLabel(const std::string& utf8);

// Commandes PC -> appareil, sans le '\n' final
std::string formatHello();
std::string formatGet();
std::string formatPing();
std::string formatSet(int channel, int value);
std::string formatMute(int channel, bool muted);
std::string formatName(int channel, const std::string& label);
std::string formatActive(int channel, int count);

}  // namespace proto
