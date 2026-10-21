#include "device_protocol.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace proto {

namespace {

std::vector<std::string> splitWords(const std::string& line)
{
    std::vector<std::string> words;
    size_t pos = 0;
    while (pos < line.size()) {
        const size_t begin = line.find_first_not_of(" \t", pos);
        if (begin == std::string::npos)
            break;
        const size_t end = line.find_first_of(" \t", begin);
        words.push_back(line.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        pos = end == std::string::npos ? line.size() : end;
    }
    return words;
}

// Entier décimal strict (pas de signe, pas de caractère en trop)
bool parseInt(const char* b, const char* e, int& out)
{
    if (b == e)
        return false;
    const auto [ptr, ec] = std::from_chars(b, e, out);
    return ec == std::errc() && ptr == e;
}

bool parseInt(const std::string& s, int& out)
{
    return parseInt(s.data(), s.data() + s.size(), out);
}

bool parseChannel(const std::string& s, int& channel)
{
    int c = 0;
    if (!parseInt(s, c) || c < 1 || c > kNumChannels)
        return false;
    channel = c - 1;
    return true;
}

bool parseValue(const std::string& s, int& value)
{
    return parseInt(s, value) && value >= 0 && value <= kMaxValue;
}

Event parseLegacy(const std::string& line)
{
    std::vector<int> values;
    const char* p = line.data();
    const char* end = p + line.size();

    while (p <= end) {
        const char* sep = std::find(p, end, '|');
        // Espaces tolérés autour des valeurs
        const char* b = p;
        const char* e = sep;
        while (b < e && *b == ' ') ++b;
        while (e > b && e[-1] == ' ') --e;
        int v = 0;
        if (!parseInt(b, e, v))
            return Unknown{};
        values.push_back(v);
        p = sep + 1;
    }

    if (values.size() < static_cast<size_t>(kNumChannels))
        return Unknown{};

    LegacyFrame frame;
    if (values.size() >= static_cast<size_t>(2 * kNumChannels)) {
        for (int i = 0; i < kNumChannels; ++i)
            frame.buttons.push_back(values[kNumChannels + i] != 0);
    }
    values.resize(kNumChannels);
    frame.values = std::move(values);
    return frame;
}

// Décode un caractère UTF-8 ; retourne 0xFFFD si la séquence est invalide.
uint32_t nextCodepoint(const std::string& s, size_t& i)
{
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80)
        return c;

    int extra = 0;
    uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        cp = c & 0x07;
    } else {
        return 0xFFFD;
    }

    for (int k = 0; k < extra; ++k) {
        if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    }
    return cp;
}

// Translittération des caractères latins courants ; chaîne vide = caractère supprimé
const char* transliterate(uint32_t cp)
{
    if (cp >= 0x20 && cp < 0x7F)
        return nullptr;  // ASCII imprimable : traité par l'appelant

    switch (cp) {
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: return "A";
    case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE4: case 0xE5: return "a";
    case 0xC6: return "AE";
    case 0xE6: return "ae";
    case 0xC7: return "C";
    case 0xE7: return "c";
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "E";
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return "e";
    case 0xCC: case 0xCD: case 0xCE: case 0xCF: return "I";
    case 0xEC: case 0xED: case 0xEE: case 0xEF: return "i";
    case 0xD1: return "N";
    case 0xF1: return "n";
    case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD8: return "O";
    case 0xF2: case 0xF3: case 0xF4: case 0xF5: case 0xF6: case 0xF8: return "o";
    case 0xD9: case 0xDA: case 0xDB: case 0xDC: return "U";
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return "u";
    case 0xDD: case 0x178: return "Y";
    case 0xFD: case 0xFF: return "y";
    case 0xDF: return "ss";
    case 0x152: return "OE";
    case 0x153: return "oe";
    case 0xA0: return " ";                     // espace insécable
    case 0x2018: case 0x2019: return "'";      // apostrophes typographiques
    case 0x201C: case 0x201D: case 0xAB: case 0xBB: return "\"";
    case 0x2013: case 0x2014: return "-";      // tirets
    default: return "";
    }
}

std::string trimSpaces(const std::string& s)
{
    const auto begin = s.find_first_not_of(' ');
    if (begin == std::string::npos)
        return {};
    const auto end = s.find_last_not_of(' ');
    return s.substr(begin, end - begin + 1);
}

std::string channelWord(int channel)
{
    return std::to_string(channel + 1);
}

}  // namespace

Event parseLine(const std::string& rawLine)
{
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.pop_back();

    if (line.find('|') != std::string::npos)
        return parseLegacy(line);

    const std::vector<std::string> w = splitWords(line);
    if (w.empty())
        return Unknown{};
    const std::string& kw = w[0];

    if (kw == "V" && w.size() == 3) {
        Fader f;
        if (parseChannel(w[1], f.channel) && parseValue(w[2], f.value))
            return f;
    } else if (kw == "B" && w.size() == 3) {
        Button b;
        if (parseChannel(w[1], b.channel) && (w[2] == "0" || w[2] == "1")) {
            b.pressed = w[2] == "1";
            return b;
        }
    } else if (kw == "STATE" && w.size() >= 2) {
        State s;
        for (size_t i = 1; i < w.size(); ++i) {
            int v = 0;
            if (!parseValue(w[i], v))
                return Unknown{};
            s.values.push_back(v);
        }
        return s;
    } else if (kw == "HELLO" && w.size() == 4 && w[1] == "MIWER") {
        Hello h;
        h.version = w[2];
        if (parseInt(w[3], h.channels) && h.channels > 0)
            return h;
    } else if (kw == "PONG" && w.size() == 1) {
        return Pong{};
    } else if (kw == "ERR") {
        const auto pos = line.find("ERR");
        return Err{trimSpaces(line.substr(pos + 3))};
    }
    return Unknown{};
}

int toWire(float volume)
{
    const float v = std::clamp(volume, 0.0f, 1.0f);
    return static_cast<int>(std::lround(v * kMaxValue));
}

float fromWire(int value)
{
    return std::clamp(static_cast<float>(value) / kMaxValue, 0.0f, 1.0f);
}

std::string sanitizeLabel(const std::string& utf8)
{
    std::string out;
    size_t i = 0;
    while (i < utf8.size()) {
        const uint32_t cp = nextCodepoint(utf8, i);
        if (const char* t = transliterate(cp))
            out += t;
        else
            out += static_cast<char>(cp);
    }
    out = trimSpaces(out);
    if (out.size() > kLabelMax)
        out = trimSpaces(out.substr(0, kLabelMax));
    return out;
}

std::string formatHello() { return "HELLO"; }
std::string formatGet() { return "GET"; }
std::string formatPing() { return "PING"; }

std::string formatSet(int channel, int value)
{
    return "SET " + channelWord(channel) + ' ' + std::to_string(std::clamp(value, 0, kMaxValue));
}

std::string formatMute(int channel, bool muted)
{
    return "MUTE " + channelWord(channel) + (muted ? " 1" : " 0");
}

std::string formatName(int channel, const std::string& label)
{
    std::string text = sanitizeLabel(label);
    if (text.empty())
        text = "Canal " + channelWord(channel);
    return "NAME " + channelWord(channel) + ' ' + text;
}

std::string formatActive(int channel, int count)
{
    return "ACTIVE " + channelWord(channel) + ' ' + std::to_string(std::clamp(count, 0, kMaxActive));
}

}  // namespace proto
