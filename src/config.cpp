#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {

std::string trim(const std::string& s)
{
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool parseBool(const std::string& value, int lineNo)
{
    const std::string v = toLower(value);
    if (v == "true" || v == "yes" || v == "1" || v == "on")
        return true;
    if (v == "false" || v == "no" || v == "0" || v == "off")
        return false;
    throw std::runtime_error("ligne " + std::to_string(lineNo) + " : booléen invalide '" + value + "'");
}

// Reconnaît « prefixN » avec N dans 1..kNumChannels ; retourne l'index du canal ou -1
int channelKey(const std::string& key, const std::string& prefix)
{
    if (key.size() != prefix.size() + 1 || key.compare(0, prefix.size(), prefix) != 0)
        return -1;
    const char c = key.back();
    return (c >= '1' && c < '1' + kNumChannels) ? c - '1' : -1;
}

}  // namespace

std::string defaultLabel(const std::vector<std::string>& targets, int channel)
{
    if (targets.empty())
        return "Canal " + std::to_string(channel + 1);
    const std::string& t = targets.front();
    if (t == kTargetMaster)
        return "Master";
    if (t == kTargetMic)
        return "Micro";
    if (t == kTargetOthers)
        return "Autres";
    std::string label = t;
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return label;
}

std::string defaultConfigPath()
{
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/miwer/config.ini";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/miwer/config.ini";
    return "config.ini";
}

Config loadConfig(const std::string& path)
{
    std::ifstream file(path);
    if (!file)
        throw std::runtime_error("impossible d'ouvrir " + path);

    Config cfg;
    std::string line;
    int lineNo = 0;

    while (std::getline(file, line)) {
        ++lineNo;
        if (const auto pos = line.find_first_of("#;"); pos != std::string::npos)
            line.erase(pos);
        line = trim(line);
        if (line.empty() || line.front() == '[')
            continue;

        const auto eq = line.find('=');
        if (eq == std::string::npos)
            throw std::runtime_error("ligne " + std::to_string(lineNo) + " : '=' attendu");

        const std::string key = toLower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));

        try {
            if (key == "device") {
                cfg.device = toLower(value) == "auto" ? "auto" : value;
            } else if (key == "baudrate") {
                cfg.baudrate = std::stoi(value);
            } else if (key == "adc_max") {
                cfg.adcMax = std::max(1, std::stoi(value));
            } else if (key == "noise_threshold") {
                cfg.noiseThreshold = std::clamp(std::stod(value), 0.0, 1.0);
            } else if (key == "invert_sliders") {
                cfg.invertSliders = parseBool(value, lineNo);
            } else if (key == "follow_external") {
                cfg.followExternal = parseBool(value, lineNo);
            } else if (key == "verbose") {
                cfg.verbose = parseBool(value, lineNo);
            } else if (key == "mute_mode") {
                const std::string v = toLower(value);
                if (v == "toggle")
                    cfg.muteMode = MuteMode::Toggle;
                else if (v == "state")
                    cfg.muteMode = MuteMode::State;
                else
                    throw std::runtime_error("ligne " + std::to_string(lineNo) +
                                             " : mute_mode doit valoir 'toggle' ou 'state'");
            } else if (const int ch = channelKey(key, "label"); ch >= 0) {
                cfg.labels[ch] = value;
            } else if (const int ch = channelKey(key, "channel"); ch >= 0) {
                auto& targets = cfg.targets[ch];
                targets.clear();
                std::stringstream ss(value);
                std::string item;
                while (std::getline(ss, item, ',')) {
                    item = toLower(trim(item));
                    if (!item.empty())
                        targets.push_back(item);
                }
            } else {
                throw std::runtime_error("ligne " + std::to_string(lineNo) + " : clé inconnue '" + key + "'");
            }
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("ligne " + std::to_string(lineNo) + " : valeur invalide '" + value + "'");
        } catch (const std::out_of_range&) {
            throw std::runtime_error("ligne " + std::to_string(lineNo) + " : valeur hors limites '" + value + "'");
        }
    }

    for (int ch = 0; ch < kNumChannels; ++ch)
        if (cfg.labels[ch].empty())
            cfg.labels[ch] = defaultLabel(cfg.targets[ch], ch);

    return cfg;
}
