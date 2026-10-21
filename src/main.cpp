// Miwer : mixeur de volume par application pour PipeWire, piloté par un appareil USB
// (5 faders + 5 boutons mute).
//
// Protocole série : voir docs/PROTOCOL.md (HELLO / STATE / V / B côté appareil,
// SET / MUTE / NAME / ACTIVE côté PC). L'ancien format du firmware Arduino de référence
// (v1|v2|v3|v4|v5|m1|m2|m3|m4|m5, valeurs brutes 0..adc_max) est toujours accepté.

#include "config.hpp"
#include "device_protocol.hpp"
#include "pipewire_mixer.hpp"
#include "serial_device.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <pipewire/pipewire.h>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kReconnectSeconds = 2;
// HELLO renvoyé si l'appareil ne répond pas (il a pu manquer le premier)
constexpr int kHelloRetrySeconds = 1;
constexpr int kHelloMaxAttempts = 3;
// Au plus un message SET / MUTE / ACTIVE toutes les 50 ms par canal (~20/s)
constexpr auto kSyncInterval = std::chrono::milliseconds(50);

// Informations synchronisées vers l'appareil (index dans les tableaux par canal)
enum SyncKind { kSyncVolume, kSyncMute, kSyncActive, kSyncKinds };

struct App {
    Config cfg;
    pw_main_loop* mainLoop = nullptr;
    pw_loop* loop = nullptr;
    std::unique_ptr<PipeWireMixer> mixer;

    SerialDevice serial;
    spa_source* ioSource = nullptr;
    bool ioWatchOut = false;  // SPA_IO_OUT surveillé (tampon de sortie non vide)
    spa_source* reconnectTimer = nullptr;
    bool waitingLogged = false;

    // Ancien format de trame
    bool haveFrame = false;
    std::array<float, kNumChannels> lastVolumes{};
    std::array<bool, kNumChannels> lastButtons{};

    // Protocole v1
    bool deviceV1 = false;    // l'appareil a répondu HELLO MIWER
    bool legacySeen = false;  // l'appareil parle l'ancien format
    int helloAttempts = 0;
    spa_source* helloTimer = nullptr;
    // Dernier état connu de l'appareil (valeurs du fil, -1 = inconnu)
    std::array<std::array<int, kSyncKinds>, kNumChannels> deviceState{};
    std::array<std::array<bool, kSyncKinds>, kNumChannels> pending{};
    std::array<std::array<Clock::time_point, kSyncKinds>, kNumChannels> lastSent{};
    spa_source* syncTimer = nullptr;

    int exitCode = 0;
};

void armTimer(App& app, spa_source* timer, std::chrono::nanoseconds delay, std::chrono::nanoseconds period)
{
    const auto toTs = [](std::chrono::nanoseconds d) {
        const auto s = std::chrono::duration_cast<std::chrono::seconds>(d);
        return timespec{static_cast<time_t>(s.count()), static_cast<long>((d - s).count())};
    };
    timespec value = toTs(delay);
    timespec interval = toTs(period);
    pw_loop_update_timer(app.loop, timer, &value, &interval, false);
}

void disarmTimer(App& app, spa_source* timer)
{
    armTimer(app, timer, std::chrono::nanoseconds(0), std::chrono::nanoseconds(0));
}

void armReconnectTimer(App& app, bool enable)
{
    const auto d = enable ? std::chrono::seconds(kReconnectSeconds) : std::chrono::seconds(0);
    armTimer(app, app.reconnectTimer, d, d);
}

// Volume du canal <-> valeur du fader (invert_sliders s'applique dans les deux sens)
int volumeToWire(const App& app, float volume)
{
    const int v = proto::toWire(volume);
    return app.cfg.invertSliders ? proto::kMaxValue - v : v;
}

float wireToVolume(const App& app, int value)
{
    const float v = proto::fromWire(value);
    return app.cfg.invertSliders ? 1.0f - v : v;
}

void disconnectDevice(App& app);

// Surveille SPA_IO_OUT seulement tant que le tampon de sortie n'est pas vide
void updateIoMask(App& app)
{
    const bool wantOut = app.serial.hasPendingOutput();
    if (!app.ioSource || wantOut == app.ioWatchOut)
        return;
    app.ioWatchOut = wantOut;
    uint32_t mask = SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP;
    if (wantOut)
        mask |= SPA_IO_OUT;
    pw_loop_update_io(app.loop, app.ioSource, mask);
}

void sendLine(App& app, const std::string& line)
{
    if (!app.serial.isOpen())
        return;
    if (app.cfg.verbose)
        std::cerr << "[serie] -> " << line << '\n';
    if (!app.serial.writeLine(line)) {
        disconnectDevice(app);
        return;
    }
    updateIoMask(app);
}

// ---------------------------------------------------------------- Synchronisation vers l'appareil

// Valeur actuelle côté PC d'une information synchronisée
int currentValue(const App& app, int ch, int kind)
{
    switch (kind) {
    case kSyncVolume: return volumeToWire(app, app.mixer->channelVolume(ch));
    case kSyncMute: return app.mixer->channelMuted(ch) ? 1 : 0;
    default: return std::clamp(app.mixer->activeCount(ch), 0, proto::kMaxActive);
    }
}

void sendSync(App& app, int ch, int kind)
{
    const int value = currentValue(app, ch, kind);
    app.pending[ch][kind] = false;
    app.lastSent[ch][kind] = Clock::now();
    app.deviceState[ch][kind] = value;
    switch (kind) {
    case kSyncVolume: sendLine(app, proto::formatSet(ch, value)); break;
    case kSyncMute: sendLine(app, proto::formatMute(ch, value != 0)); break;
    default: sendLine(app, proto::formatActive(ch, value)); break;
    }
}

// Envoie ce qui est en attente et dont le délai minimal est écoulé, sinon réarme le timer.
// Seule la dernière valeur compte : elle est lue au moment de l'envoi.
void flushSync(App& app)
{
    const auto now = Clock::now();
    auto nextDue = Clock::time_point::max();

    for (int ch = 0; ch < kNumChannels && app.deviceV1; ++ch) {
        for (int kind = 0; kind < kSyncKinds && app.deviceV1; ++kind) {
            if (!app.pending[ch][kind])
                continue;
            if (currentValue(app, ch, kind) == app.deviceState[ch][kind]) {
                app.pending[ch][kind] = false;  // l'appareil est déjà à jour
                continue;
            }
            const auto due = app.lastSent[ch][kind] + kSyncInterval;
            if (due <= now)
                sendSync(app, ch, kind);
            else
                nextDue = std::min(nextDue, due);
        }
    }

    if (app.deviceV1 && nextDue != Clock::time_point::max())
        armTimer(app, app.syncTimer,
                 std::max<std::chrono::nanoseconds>(nextDue - now, std::chrono::milliseconds(1)),
                 std::chrono::nanoseconds(0));
}

void queueSync(App& app, int ch, int kind)
{
    if (!app.deviceV1 || ch < 0 || ch >= kNumChannels)
        return;
    app.pending[ch][kind] = true;
    flushSync(app);
}

void onSyncTimer(void* data, uint64_t /*expirations*/)
{
    flushSync(*static_cast<App*>(data));
}

void resetDeviceState(App& app)
{
    app.deviceV1 = false;
    app.legacySeen = false;
    app.helloAttempts = 0;
    app.haveFrame = false;
    for (auto& row : app.deviceState)
        row.fill(-1);
    for (auto& row : app.pending)
        row.fill(false);
    for (auto& row : app.lastSent)
        row.fill(Clock::time_point{});
    disarmTimer(app, app.helloTimer);
    disarmTimer(app, app.syncTimer);
}

// ---------------------------------------------------------------- Messages de l'appareil

// Ancien format : comportement historique (seuil de bruit, adc_max, mute_mode)
void handleLegacyFrame(App& app, const proto::LegacyFrame& frame)
{
    if (!app.legacySeen) {
        app.legacySeen = true;
        disarmTimer(app, app.helloTimer);
        std::cerr << "Appareil à l'ancien format de trame (v1|...|m5)\n";
    }

    for (int i = 0; i < kNumChannels; ++i) {
        float v = std::clamp(static_cast<float>(frame.values[i]) / app.cfg.adcMax, 0.0f, 1.0f);
        if (app.cfg.invertSliders)
            v = 1.0f - v;

        const float last = app.lastVolumes[i];
        const bool reachedEdge = (v == 0.0f && last != 0.0f) || (v == 1.0f && last != 1.0f);
        if (!app.haveFrame || reachedEdge || std::fabs(v - last) >= app.cfg.noiseThreshold) {
            app.lastVolumes[i] = v;
            app.mixer->setChannelVolume(i, v);
        }
    }

    if (!frame.buttons.empty()) {
        for (int i = 0; i < kNumChannels; ++i) {
            const bool b = frame.buttons[i];
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

void handleHello(App& app, const proto::Hello& hello)
{
    if (!app.deviceV1) {
        std::cerr << "Appareil Miwer " << hello.version << " (" << hello.channels << " canaux)\n";
        if (hello.channels != kNumChannels)
            std::cerr << "Attention : l'appareil annonce " << hello.channels << " canaux, Miwer en gère "
                      << kNumChannels << '\n';
    }
    app.deviceV1 = true;
    disarmTimer(app, app.helloTimer);
}

// Synchronisation initiale : STATE donne la position actuelle des faders
void handleState(App& app, const proto::State& state)
{
    if (!app.deviceV1)
        return;  // STATE sans HELLO préalable : on attend la réponse à HELLO

    const int n = static_cast<int>(state.values.size());
    for (int ch = 0; ch < kNumChannels && app.deviceV1; ++ch) {
        if (ch < n)
            app.deviceState[ch][kSyncVolume] = state.values[ch];

        if (app.mixer->volumeKnown(ch))
            sendSync(app, ch, kSyncVolume);  // le fader motorisé rejoint le volume du PC
        else if (ch < n)
            app.mixer->setChannelVolume(ch, wireToVolume(app, state.values[ch]));  // on adopte le fader
    }

    for (int ch = 0; ch < kNumChannels && app.deviceV1; ++ch) {
        sendLine(app, proto::formatName(ch, app.cfg.labels[ch]));
        if (!app.deviceV1)
            break;
        sendSync(app, ch, kSyncMute);
        if (!app.deviceV1)
            break;
        sendSync(app, ch, kSyncActive);
    }
}

void handleFader(App& app, const proto::Fader& fader)
{
    // Ce volume vient de l'appareil : jamais de SET en retour (règle anti-boucle).
    // Un SET en attente (changement externe) est abandonné : l'utilisateur a la main.
    app.deviceState[fader.channel][kSyncVolume] = fader.value;
    app.pending[fader.channel][kSyncVolume] = false;
    app.mixer->setChannelVolume(fader.channel, wireToVolume(app, fader.value));
}

void handleButton(App& app, const proto::Button& button)
{
    const int ch = button.channel;
    if (app.cfg.muteMode == MuteMode::Toggle) {
        if (!button.pressed)
            return;  // seul l'appui compte
        app.mixer->setChannelMute(ch, !app.mixer->channelMuted(ch));
    } else {
        app.mixer->setChannelMute(ch, button.pressed);
    }
    queueSync(app, ch, kSyncMute);
}

void handleLine(App& app, const std::string& line)
{
    if (app.cfg.verbose)
        std::cerr << "[serie] <- " << line << '\n';

    const proto::Event event = proto::parseLine(line);
    std::visit(
        [&app, &line](const auto& e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, proto::LegacyFrame>) {
                handleLegacyFrame(app, e);
            } else if constexpr (std::is_same_v<T, proto::Hello>) {
                handleHello(app, e);
            } else if constexpr (std::is_same_v<T, proto::State>) {
                handleState(app, e);
            } else if constexpr (std::is_same_v<T, proto::Fader>) {
                handleFader(app, e);
            } else if constexpr (std::is_same_v<T, proto::Button>) {
                handleButton(app, e);
            } else if constexpr (std::is_same_v<T, proto::Err>) {
                std::cerr << "[serie] erreur signalée par l'appareil : " << e.message << '\n';
            } else if constexpr (std::is_same_v<T, proto::Unknown>) {
                if (app.cfg.verbose)
                    std::cerr << "[serie] ligne ignorée : '" << line << "'\n";
            }
            // Pong : rien à faire
        },
        event);
}

// ---------------------------------------------------------------- Connexion

void disconnectDevice(App& app)
{
    if (app.ioSource) {
        pw_loop_destroy_source(app.loop, app.ioSource);
        app.ioSource = nullptr;
    }
    app.ioWatchOut = false;
    app.serial.close();
    resetDeviceState(app);
    std::cerr << "Appareil déconnecté, nouvelle tentative toutes les " << kReconnectSeconds << " s\n";
    app.waitingLogged = true;
    armReconnectTimer(app, true);
}

void onSerialIo(void* data, int /*fd*/, uint32_t mask)
{
    auto& app = *static_cast<App*>(data);

    bool disconnected = false;
    if (mask & SPA_IO_IN) {
        for (const auto& line : app.serial.readLines(disconnected)) {
            handleLine(app, line);
            if (!app.serial.isOpen())
                return;  // déconnecté pendant le traitement (écriture impossible)
        }
    }
    if (!disconnected && (mask & SPA_IO_OUT)) {
        disconnected = !app.serial.flush();
        if (!disconnected)
            updateIoMask(app);
    }
    if (disconnected || (mask & (SPA_IO_ERR | SPA_IO_HUP)))
        disconnectDevice(app);
}

void onHelloTimer(void* data, uint64_t /*expirations*/)
{
    auto& app = *static_cast<App*>(data);
    if (app.deviceV1 || app.legacySeen || !app.serial.isOpen() || app.helloAttempts >= kHelloMaxAttempts) {
        disarmTimer(app, app.helloTimer);
        return;
    }
    ++app.helloAttempts;
    sendLine(app, proto::formatHello());
}

bool tryConnect(App& app)
{
    // En mode auto, la détection est refaite à chaque tentative
    const std::string path = app.cfg.device == SerialDevice::kAuto ? SerialDevice::autoDetect() : app.cfg.device;

    std::string error;
    if (!app.serial.open(path, app.cfg.baudrate, error)) {
        if (!app.waitingLogged) {
            std::cerr << "En attente de l'appareil (" << error << ")\n";
            app.waitingLogged = true;
        }
        return false;
    }

    app.ioWatchOut = false;
    app.ioSource = pw_loop_add_io(app.loop, app.serial.fd(), SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP, false,
                                  &onSerialIo, &app);
    app.waitingLogged = false;
    std::cerr << "Appareil connecté : " << path << '\n';

    resetDeviceState(app);
    app.helloAttempts = 1;
    sendLine(app, proto::formatHello());
    if (!app.serial.isOpen())
        return false;  // écriture impossible : disconnectDevice a réarmé la reconnexion
    const auto retry = std::chrono::seconds(kHelloRetrySeconds);
    armTimer(app, app.helloTimer, retry, retry);
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
    std::cout << "Usage : " << argv0 << " [-c fichier.ini] [-d /dev/ttyACM0|auto] [-v]\n"
              << "  -c  fichier de configuration (défaut : " << defaultConfigPath() << ")\n"
              << "  -d  port série de l'appareil (prioritaire sur la configuration)\n"
              << "  -v  mode verbeux (flux détectés, canal choisi, échanges série)\n";
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

    // Volume / mute changé ailleurs (ou adopté) : le fader motorisé et l'écran suivent
    app.mixer->setOnChannelChanged([&app](int ch, float /*volume*/, bool /*muted*/) {
        queueSync(app, ch, kSyncVolume);
        queueSync(app, ch, kSyncMute);
    });
    app.mixer->setOnActiveCountChanged([&app](int ch, int /*count*/) { queueSync(app, ch, kSyncActive); });

    app.reconnectTimer = pw_loop_add_timer(app.loop, &onReconnectTimer, &app);
    app.helloTimer = pw_loop_add_timer(app.loop, &onHelloTimer, &app);
    app.syncTimer = pw_loop_add_timer(app.loop, &onSyncTimer, &app);
    resetDeviceState(app);
    if (!tryConnect(app))
        armReconnectTimer(app, true);

    pw_main_loop_run(app.mainLoop);

    if (app.ioSource)
        pw_loop_destroy_source(app.loop, app.ioSource);
    pw_loop_destroy_source(app.loop, app.reconnectTimer);
    pw_loop_destroy_source(app.loop, app.helloTimer);
    pw_loop_destroy_source(app.loop, app.syncTimer);
    app.serial.close();
    app.mixer.reset();
    pw_main_loop_destroy(app.mainLoop);
    pw_deinit();

    return app.exitCode;
}
