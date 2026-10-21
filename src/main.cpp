// Miwer : mixeur de volume par application pour PipeWire, piloté par un appareil USB
// (5 potentiomètres + 5 boutons mute).
//
// Protocole série (une ligne par trame, terminée par '\n') :
//   v1|v2|v3|v4|v5|m1|m2|m3|m4|m5
// v = valeur brute du potentiomètre (0..adc_max), m = 0/1 (bouton ou état mute).

#include "config.hpp"
#include "pipewire_mixer.hpp"
#include "serial_device.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>

namespace {

constexpr int kReconnectSeconds = 2;

struct App {
    Config cfg;
    pw_main_loop* mainLoop = nullptr;
    pw_loop* loop = nullptr;
    std::unique_ptr<PipeWireMixer> mixer;

    SerialDevice serial;
    spa_source* ioSource = nullptr;
    spa_source* reconnectTimer = nullptr;
    bool waitingLogged = false;

    bool haveFrame = false;
    std::array<float, kNumChannels> lastVolumes{};
    std::array<bool, kNumChannels> lastButtons{};

    int exitCode = 0;
};

void armReconnectTimer(App& app, bool enable)
{
    timespec value{enable ? kReconnectSeconds : 0, 0};
    timespec interval{enable ? kReconnectSeconds : 0, 0};
    pw_loop_update_timer(app.loop, app.reconnectTimer, &value, &interval, false);
}

std::vector<int> parseFrame(const std::string& line)
{
    std::vector<int> values;
    const char* p = line.data();
    const char* end = p + line.size();

    while (p <= end) {
        const char* sep = std::find(p, end, '|');
        int v = 0;
        // Espaces tolérés autour des valeurs
        const char* b = p;
        const char* e = sep;
        while (b < e && *b == ' ') ++b;
        while (e > b && e[-1] == ' ') --e;
        const auto [ptr, ec] = std::from_chars(b, e, v);
        if (ec != std::errc() || ptr != e || b == e)
            return {};
        values.push_back(v);
        p = sep + 1;
    }
    return values;
}

void handleFrame(App& app, const std::string& line)
{
    const std::vector<int> values = parseFrame(line);
    if (values.size() < static_cast<size_t>(kNumChannels)) {
        if (app.cfg.verbose)
            std::cerr << "[serie] trame ignorée : '" << line << "'\n";
        return;
    }

    for (int i = 0; i < kNumChannels; ++i) {
        float v = std::clamp(static_cast<float>(values[i]) / app.cfg.adcMax, 0.0f, 1.0f);
        if (app.cfg.invertSliders)
            v = 1.0f - v;

        const float last = app.lastVolumes[i];
        const bool reachedEdge = (v == 0.0f && last != 0.0f) || (v == 1.0f && last != 1.0f);
        if (!app.haveFrame || reachedEdge || std::fabs(v - last) >= app.cfg.noiseThreshold) {
            app.lastVolumes[i] = v;
            app.mixer->setChannelVolume(i, v);
        }
    }

    if (values.size() >= static_cast<size_t>(2 * kNumChannels)) {
        for (int i = 0; i < kNumChannels; ++i) {
            const bool b = values[kNumChannels + i] != 0;
            if (app.cfg.muteMode == MuteMode::Toggle) {
                // Front montant = appui ; la première trame sert seulement de référence
                if (app.haveFrame && b && !app.lastButtons[i])
                    app.mixer->setChannelMute(i, !app.mixer->channelMuted(i));
            } else if (!app.haveFrame || b != app.lastButtons[i]) {
                app.mixer->setChannelMute(i, b);
            }
            app.lastButtons[i] = b;
        }
    }

    app.haveFrame = true;
}

void disconnectDevice(App& app)
{
    if (app.ioSource) {
        pw_loop_destroy_source(app.loop, app.ioSource);
        app.ioSource = nullptr;
    }
    app.serial.close();
    app.haveFrame = false;
    std::cerr << "Appareil déconnecté, nouvelle tentative toutes les " << kReconnectSeconds << " s\n";
    app.waitingLogged = true;
    armReconnectTimer(app, true);
}

void onSerialIo(void* data, int /*fd*/, uint32_t mask)
{
    auto& app = *static_cast<App*>(data);

    bool disconnected = false;
    if (mask & SPA_IO_IN) {
        for (const auto& line : app.serial.readLines(disconnected))
            handleFrame(app, line);
    }
    if (disconnected || (mask & (SPA_IO_ERR | SPA_IO_HUP)))
        disconnectDevice(app);
}

bool tryConnect(App& app)
{
    std::string error;
    if (!app.serial.open(app.cfg.device, app.cfg.baudrate, error)) {
        if (!app.waitingLogged) {
            std::cerr << "En attente de l'appareil (" << error << ")\n";
            app.waitingLogged = true;
        }
        return false;
    }

    app.ioSource = pw_loop_add_io(app.loop, app.serial.fd(), SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false,
                                  &onSerialIo, &app);
    app.waitingLogged = false;
    std::cerr << "Appareil connecté : " << app.cfg.device << '\n';
    return true;
}

void onReconnectTimer(void* data, uint64_t /*expirations*/)
{
    auto& app = *static_cast<App*>(data);
    if (tryConnect(app))
        armReconnectTimer(app, false);
}

void onSignal(void* data, int /*signal*/)
{
    auto& app = *static_cast<App*>(data);
    pw_main_loop_quit(app.mainLoop);
}

void printUsage(const char* argv0)
{
    std::cout << "Usage : " << argv0 << " [-c fichier.ini] [-d /dev/ttyACM0] [-v]\n"
              << "  -c  fichier de configuration (défaut : " << defaultConfigPath() << ")\n"
              << "  -d  port série de l'appareil (prioritaire sur la configuration)\n"
              << "  -v  mode verbeux (affiche les flux détectés et leur canal)\n";
}

}  // namespace

int main(int argc, char* argv[])
{
    std::string configPath = defaultConfigPath();
    std::string deviceOverride;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "-c" || arg == "-d") && i + 1 < argc) {
            (arg == "-c" ? configPath : deviceOverride) = argv[++i];
        } else if (arg == "-v") {
            verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        } else {
            printUsage(argv[0]);
            return 1;
        }
    }

    App app;
    try {
        app.cfg = loadConfig(configPath);
    } catch (const std::exception& e) {
        std::cerr << "Configuration (" << configPath << ") : " << e.what() << '\n';
        return 1;
    }
    if (!deviceOverride.empty())
        app.cfg.device = deviceOverride;
    if (verbose)
        app.cfg.verbose = true;

    pw_init(&argc, &argv);

    app.mainLoop = pw_main_loop_new(nullptr);
    if (!app.mainLoop) {
        std::cerr << "Impossible de créer la boucle PipeWire\n";
        return 1;
    }
    app.loop = pw_main_loop_get_loop(app.mainLoop);

    pw_loop_add_signal(app.loop, SIGINT, &onSignal, &app);
    pw_loop_add_signal(app.loop, SIGTERM, &onSignal, &app);

    try {
        app.mixer = std::make_unique<PipeWireMixer>(app.loop, app.cfg, [&app] {
            std::cerr << "Connexion à PipeWire perdue\n";
            app.exitCode = 1;
            pw_main_loop_quit(app.mainLoop);
        });
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        pw_main_loop_destroy(app.mainLoop);
        pw_deinit();
        return 1;
    }

    app.reconnectTimer = pw_loop_add_timer(app.loop, &onReconnectTimer, &app);
    if (!tryConnect(app))
        armReconnectTimer(app, true);

    pw_main_loop_run(app.mainLoop);

    if (app.ioSource)
        pw_loop_destroy_source(app.loop, app.ioSource);
    pw_loop_destroy_source(app.loop, app.reconnectTimer);
    app.serial.close();
    app.mixer.reset();
    pw_main_loop_destroy(app.mainLoop);
    pw_deinit();

    return app.exitCode;
}
