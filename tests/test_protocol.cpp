// Tests unitaires du module protocole (et des libellés par défaut de la configuration).
// Exécutable autonome : retourne 0 si tous les tests passent.

#include "config.hpp"
#include "device_protocol.hpp"

#include <iostream>
#include <string>
#include <variant>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cerr << "ÉCHEC ligne " << line << " : " << what << '\n';
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_EQ(a, b) check((a) == (b), #a " == " #b, __LINE__)

template <typename T>
const T* as(const proto::Event& e)
{
    return std::get_if<T>(&e);
}

void testHello()
{
    const auto e = proto::parseLine("HELLO MIWER 1.0 5");
    const auto* h = as<proto::Hello>(e);
    CHECK(h != nullptr);
    if (h) {
        CHECK_EQ(h->version, "1.0");
        CHECK_EQ(h->channels, 5);
    }
    CHECK(as<proto::Hello>(proto::parseLine("HELLO MIWER 1.0 5\r")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("HELLO")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("HELLO OTHER 1.0 5")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("HELLO MIWER 1.0 x")) != nullptr);
}

void testState()
{
    const auto e = proto::parseLine("STATE 0 250 500 750 1000");
    const auto* s = as<proto::State>(e);
    CHECK(s != nullptr);
    if (s) {
        CHECK_EQ(s->values.size(), 5u);
        CHECK_EQ(s->values[0], 0);
        CHECK_EQ(s->values[4], 1000);
    }
    CHECK(as<proto::Unknown>(proto::parseLine("STATE")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("STATE 0 1001 0 0 0")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("STATE 0 -1 0 0 0")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("STATE 0 a 0 0 0")) != nullptr);
}

void testFader()
{
    const proto::Event fEvent = proto::parseLine("V 3 512");
    const auto* f = as<proto::Fader>(fEvent);
    CHECK(f != nullptr);
    if (f) {
        CHECK_EQ(f->channel, 2);  // index interne
        CHECK_EQ(f->value, 512);
    }
    CHECK(as<proto::Fader>(proto::parseLine("V  1   0 ")) != nullptr);  // espaces multiples tolérés
    CHECK(as<proto::Unknown>(proto::parseLine("V 0 10")) != nullptr);   // canal hors limites
    CHECK(as<proto::Unknown>(proto::parseLine("V 6 10")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("V 1 1001")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("V 1")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("V 1 2 3")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("v 1 2")) != nullptr);    // mots-clés en majuscules
}

void testButton()
{
    const proto::Event bEvent = proto::parseLine("B 5 1");
    const auto* b = as<proto::Button>(bEvent);
    CHECK(b != nullptr);
    if (b) {
        CHECK_EQ(b->channel, 4);
        CHECK(b->pressed);
    }
    const proto::Event rEvent = proto::parseLine("B 1 0");
    const auto* r = as<proto::Button>(rEvent);
    CHECK(r != nullptr && !r->pressed);
    CHECK(as<proto::Unknown>(proto::parseLine("B 1 2")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("B 9 1")) != nullptr);
}

void testMisc()
{
    CHECK(as<proto::Pong>(proto::parseLine("PONG")) != nullptr);
    const proto::Event errEvent = proto::parseLine("ERR commande inconnue");
    const auto* err = as<proto::Err>(errEvent);
    CHECK(err != nullptr && err->message == "commande inconnue");
    CHECK(as<proto::Unknown>(proto::parseLine("")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("   ")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("FOO 1 2")) != nullptr);
}

void testLegacy()
{
    const proto::Event fEvent = proto::parseLine("1023|0|512| 7 |100|1|0|0|1|0");
    const auto* f = as<proto::LegacyFrame>(fEvent);
    CHECK(f != nullptr);
    if (f) {
        CHECK_EQ(f->values.size(), 5u);
        CHECK_EQ(f->values[0], 1023);
        CHECK_EQ(f->values[3], 7);
        CHECK_EQ(f->buttons.size(), 5u);
        CHECK(f->buttons.size() == 5 && f->buttons[0] && !f->buttons[1] && f->buttons[3]);
    }
    const proto::Event noButtonsEvent = proto::parseLine("1|2|3|4|5");
    const auto* noButtons = as<proto::LegacyFrame>(noButtonsEvent);
    CHECK(noButtons != nullptr && noButtons->buttons.empty());
    CHECK(as<proto::Unknown>(proto::parseLine("1|2|3|4")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("1|2|x|4|5")) != nullptr);
    CHECK(as<proto::Unknown>(proto::parseLine("1|2||4|5")) != nullptr);
}

void testConversions()
{
    CHECK_EQ(proto::toWire(0.0f), 0);
    CHECK_EQ(proto::toWire(1.0f), 1000);
    CHECK_EQ(proto::toWire(0.5f), 500);
    CHECK_EQ(proto::toWire(1.5f), 1000);
    CHECK_EQ(proto::toWire(-0.2f), 0);
    CHECK(proto::fromWire(250) == 0.25f);
    for (int v = 0; v <= 1000; ++v)
        if (proto::toWire(proto::fromWire(v)) != v) {
            CHECK(false);
            break;
        }
}

void testLabels()
{
    CHECK_EQ(proto::sanitizeLabel("Firefox"), "Firefox");
    CHECK_EQ(proto::sanitizeLabel("Été à Noël"), "Ete a No");          // 8 caractères
    CHECK_EQ(proto::sanitizeLabel("Façade"), "Facade");
    CHECK_EQ(proto::sanitizeLabel("Œuvre"), "OEuvre");
    CHECK_EQ(proto::sanitizeLabel("Straße"), "Strasse");
    CHECK_EQ(proto::sanitizeLabel("Musique 🎵"), "Musique");           // emoji supprimé, espace final retiré
    CHECK_EQ(proto::sanitizeLabel("日本語"), "");
    CHECK_EQ(proto::sanitizeLabel("  Jeux  "), "Jeux");
    CHECK_EQ(proto::sanitizeLabel("a\tb\x01" "c"), "abc");             // caractères de contrôle supprimés
    CHECK_EQ(proto::sanitizeLabel("\xC3"), "");                        // UTF-8 tronqué
    CHECK_EQ(proto::sanitizeLabel("Discord voice"), "Discord");        // coupé à 8 puis espace retiré
}

void testFormat()
{
    CHECK_EQ(proto::formatHello(), "HELLO");
    CHECK_EQ(proto::formatGet(), "GET");
    CHECK_EQ(proto::formatPing(), "PING");
    CHECK_EQ(proto::formatSet(0, 500), "SET 1 500");
    CHECK_EQ(proto::formatSet(4, 2000), "SET 5 1000");
    CHECK_EQ(proto::formatMute(1, true), "MUTE 2 1");
    CHECK_EQ(proto::formatMute(1, false), "MUTE 2 0");
    CHECK_EQ(proto::formatName(2, "Spotify"), "NAME 3 Spotify");
    CHECK_EQ(proto::formatName(0, "Système"), "NAME 1 Systeme");
    CHECK_EQ(proto::formatName(3, "★"), "NAME 4 Canal 4");
    CHECK_EQ(proto::formatActive(4, 3), "ACTIVE 5 3");
    CHECK_EQ(proto::formatActive(4, 150), "ACTIVE 5 99");
    // Toutes les commandes tiennent dans 63 caractères
    CHECK(proto::formatName(0, std::string(200, 'x')).size() <= 63);
}

void testDefaultLabels()
{
    CHECK_EQ(defaultLabel({"master"}, 0), "Master");
    CHECK_EQ(defaultLabel({"mic"}, 1), "Micro");
    CHECK_EQ(defaultLabel({"others"}, 4), "Autres");
    CHECK_EQ(defaultLabel({"firefox", "chromium"}, 1), "Firefox");
    CHECK_EQ(defaultLabel({}, 2), "Canal 3");
}

}  // namespace

int main()
{
    testHello();
    testState();
    testFader();
    testButton();
    testMisc();
    testLegacy();
    testConversions();
    testLabels();
    testFormat();
    testDefaultLabels();

    std::cout << g_checks - g_failures << '/' << g_checks << " vérifications réussies\n";
    return g_failures == 0 ? 0 : 1;
}
