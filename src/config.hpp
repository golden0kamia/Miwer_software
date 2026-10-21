#pragma once

#include <array>
#include <string>
#include <vector>

constexpr int kNumChannels = 5;

// Cibles spéciales utilisables dans channelN = ...
constexpr const char* kTargetMaster = "master";  // sortie audio par défaut
constexpr const char* kTargetMic = "mic";        // entrée audio par défaut
constexpr const char* kTargetOthers = "others";  // toutes les applis non assignées

enum class MuteMode {
    Toggle,  // l'appareil envoie l'état du bouton (1 = appuyé), un appui bascule le mute
    State,   // l'appareil envoie directement l'état du mute (1 = muet)
};

struct Config {
    std::string device = "auto";  // "auto" : /dev/serial/by-id/*Miwer*, sinon /dev/ttyACM0
    int baudrate = 115200;
    int adcMax = 1023;             // valeur brute maximale d'un potentiomètre
    double noiseThreshold = 0.01;  // variation minimale (0..1) pour appliquer un volume
    bool invertSliders = false;
    MuteMode muteMode = MuteMode::Toggle;
    bool followExternal = true;    // les faders suivent les changements de volume faits ailleurs
    bool verbose = false;

    // Pour chaque canal : noms d'applications (en minuscules) ou cibles spéciales
    std::array<std::vector<std::string>, kNumChannels> targets;
    // Libellé affiché par l'appareil pour chaque canal (déduit de targets s'il n'est pas donné)
    std::array<std::string, kNumChannels> labels;
};

// Lance std::runtime_error si le fichier est illisible ou invalide.
Config loadConfig(const std::string& path);

std::string defaultConfigPath();

// Libellé par défaut d'un canal : master -> Master, mic -> Micro, others -> Autres,
// sinon le premier nom d'application avec une majuscule.
std::string defaultLabel(const std::vector<std::string>& targets, int channel);
