#include "pipewire_mixer.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>

#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/result.h>

namespace {

constexpr const char* kClassStream = "Stream/Output/Audio";
constexpr const char* kClassSink = "Audio/Sink";
constexpr const char* kClassSource = "Audio/Source";

constexpr const char* kKeyDefaultSink = "default.audio.sink";
constexpr const char* kKeyDefaultSource = "default.audio.source";

using Clock = std::chrono::steady_clock;

// Écart minimal (échelle perceptuelle) entre le volume rapporté par un node et celui que
// Miwer lui a appliqué pour parler de changement externe.
constexpr float kExternalTolerance = 0.005f;
// Durée pendant laquelle un réglage appliqué par Miwer peut encore revenir en écho.
constexpr auto kEchoWindow = std::chrono::milliseconds(1000);
constexpr size_t kMaxApplied = 32;
// Après le premier réglage d'un node, un écart est corrigé au lieu d'être adopté (par exemple
// WirePlumber qui restaure le volume mémorisé d'une application juste après son apparition).
constexpr auto kGracePeriod = std::chrono::milliseconds(1000);

std::string percent(float v)
{
    return std::to_string(static_cast<int>(std::lround(v * 100.0f))) + " %";
}

std::string toLower(const char* s)
{
    std::string out = s ? s : "";
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string dictGet(const spa_dict* props, const char* key)
{
    const char* value = props ? spa_dict_lookup(props, key) : nullptr;
    return value ? value : "";
}

// Extrait "xxx" de {"name":"xxx"} (format des valeurs default.audio.* dans les metadata)
std::string parseMetadataName(const char* json)
{
    if (!json)
        return {};
    const std::string s = json;
    auto pos = s.find("\"name\"");
    if (pos == std::string::npos)
        return {};
    pos = s.find(':', pos);
    if (pos == std::string::npos)
        return {};
    const auto begin = s.find('"', pos);
    if (begin == std::string::npos)
        return {};
    const auto end = s.find('"', begin + 1);
    if (end == std::string::npos)
        return {};
    return s.substr(begin + 1, end - begin - 1);
}

}  // namespace

struct PipeWireMixer::Node {
    PipeWireMixer* mixer = nullptr;
    uint32_t id = 0;
    pw_node* proxy = nullptr;
    spa_hook listener{};

    std::string mediaClass;
    std::string nodeName;  // tel quel, comparé aux metadata
    std::string appName;   // en minuscules
    std::string binary;    // en minuscules

    uint32_t channels = 0;  // nombre de canaux audio, connu via le param Props
    int channel = -1;       // canal du mixeur, -1 = non assigné
    bool infoReceived = false;
    bool pendingApply = false;

    // Réglages appliqués par Miwer (ou adoptés), pour reconnaître les échos de nos propres
    // pw_node_set_param. Le dernier élément est l'état que Miwer pense être celui du node.
    struct Applied {
        float volume;  // perceptuel, non borné (un node peut dépasser 100 %)
        bool muted;
        Clock::time_point time;
    };
    std::deque<Applied> applied;
    Clock::time_point graceUntil{};

    // Dernier état rapporté par le node
    bool hasReport = false;
    float reportVolume = 0.0f;
    bool reportHasMute = false;
    bool reportMuted = false;

    void remember(float volume, bool muted)
    {
        const auto now = Clock::now();
        applied.push_back({volume, muted, now});
        while (applied.size() > 1 && (applied.size() > kMaxApplied || now - applied.front().time > kEchoWindow))
            applied.pop_front();
    }

    bool isEcho(float volume, bool hasMute, bool muted) const
    {
        for (const auto& a : applied)
            if (std::fabs(a.volume - volume) <= kExternalTolerance && (!hasMute || a.muted == muted))
                return true;
        return false;
    }
};

struct PipeWireMixer::Metadata {
    uint32_t id = 0;
    pw_metadata* proxy = nullptr;
    spa_hook listener{};
};

PipeWireMixer::PipeWireMixer(pw_loop* loop, const Config& cfg, std::function<void()> onFatal)
    : m_cfg(cfg), m_onFatal(std::move(onFatal))
{
    m_context = pw_context_new(loop, nullptr, 0);
    if (!m_context)
        throw std::runtime_error("pw_context_new a échoué");

    m_core = pw_context_connect(m_context, nullptr, 0);
    if (!m_core) {
        pw_context_destroy(m_context);
        throw std::runtime_error("connexion à PipeWire impossible : " + std::string(std::strerror(errno)));
    }

    static const pw_core_events coreEv = [] {
        pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.error = &PipeWireMixer::onCoreError;
        return e;
    }();
    pw_core_add_listener(m_core, &m_coreListener, &coreEv, this);

    static const pw_registry_events registryEv = [] {
        pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = &PipeWireMixer::onRegistryGlobal;
        e.global_remove = &PipeWireMixer::onRegistryGlobalRemove;
        return e;
    }();
    m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
    pw_registry_add_listener(m_registry, &m_registryListener, &registryEv, this);
}

PipeWireMixer::~PipeWireMixer()
{
    for (auto& [id, node] : m_nodes) {
        spa_hook_remove(&node->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node->proxy));
    }
    m_nodes.clear();

    if (m_metadata) {
        spa_hook_remove(&m_metadata->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_metadata->proxy));
        m_metadata.reset();
    }

    spa_hook_remove(&m_registryListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(m_registry));
    spa_hook_remove(&m_coreListener);
    pw_core_disconnect(m_core);
    pw_context_destroy(m_context);
}

void PipeWireMixer::log(const std::string& msg) const
{
    if (m_cfg.verbose)
        std::cerr << "[pipewire] " << msg << '\n';
}

// ---------------------------------------------------------------- API publique

void PipeWireMixer::setChannelVolume(int channel, float volume)
{
    if (channel < 0 || channel >= kNumChannels)
        return;
    m_volumes[channel] = std::clamp(volume, 0.0f, 1.0f);
    m_volumeKnown[channel] = true;
    applyToChannel(channel, nullptr);
}

void PipeWireMixer::setChannelMute(int channel, bool muted)
{
    if (channel < 0 || channel >= kNumChannels)
        return;
    m_mutes[channel] = muted;
    log("canal " + std::to_string(channel + 1) + (muted ? " muet" : " actif"));
    applyToChannel(channel, nullptr);
}

// ---------------------------------------------------------------- Logique

int PipeWireMixer::findSpecialChannel(const char* target) const
{
    for (int ch = 0; ch < kNumChannels; ++ch)
        for (const auto& t : m_cfg.targets[ch])
            if (t == target)
                return ch;
    return -1;
}

int PipeWireMixer::resolveChannel(const Node& node) const
{
    if (node.mediaClass == kClassSink)
        return (!m_defaultSink.empty() && node.nodeName == m_defaultSink) ? findSpecialChannel(kTargetMaster) : -1;

    if (node.mediaClass == kClassSource)
        return (!m_defaultSource.empty() && node.nodeName == m_defaultSource) ? findSpecialChannel(kTargetMic) : -1;

    // Flux d'application : on compare au nom d'appli, au binaire et au nom du node
    const std::string nodeNameLower = toLower(node.nodeName.c_str());
    for (int ch = 0; ch < kNumChannels; ++ch) {
        for (const auto& t : m_cfg.targets[ch]) {
            if (t == kTargetMaster || t == kTargetMic || t == kTargetOthers)
                continue;
            if (t == node.appName || t == node.binary || t == nodeNameLower)
                return ch;
        }
    }
    return findSpecialChannel(kTargetOthers);
}

void PipeWireMixer::remapAll()
{
    for (auto& [id, node] : m_nodes) {
        if (!node->infoReceived)
            continue;
        const int ch = resolveChannel(*node);
        if (ch != node->channel) {
            node->channel = ch;
            log("node " + std::to_string(id) + " (" + node->nodeName + ") -> canal " +
                (ch < 0 ? std::string("aucun") : std::to_string(ch + 1)));
            onChannelAssigned(*node);
        }
    }
    updateActiveCounts();
}

void PipeWireMixer::applyToChannel(int channel, const Node* except)
{
    for (auto& [id, node] : m_nodes)
        if (node->channel == channel && node.get() != except)
            applyToNode(*node);
}

void PipeWireMixer::updateActiveCounts()
{
    std::array<int, kNumChannels> counts{};
    for (const auto& [id, node] : m_nodes)
        if (node->infoReceived && node->channel >= 0)
            ++counts[node->channel];

    for (int ch = 0; ch < kNumChannels; ++ch) {
        if (counts[ch] == m_activeCounts[ch])
            continue;
        m_activeCounts[ch] = counts[ch];
        if (m_onActiveCountChanged)
            m_onActiveCountChanged(ch, counts[ch]);
    }
}

// Volume / mute rapportés par un node (param Props). Distingue les échos de nos propres
// réglages des changements faits ailleurs.
void PipeWireMixer::handleReportedProps(Node& node, float volume, bool hasMute, bool muted)
{
    const int ch = node.channel;
    if (!m_cfg.followExternal || ch < 0)
        return;

    const bool mute = hasMute ? muted : m_mutes[ch];
    const std::string nodeDesc = "node " + std::to_string(node.id) + " (" + node.nodeName + ")";

    if (!m_volumeKnown[ch]) {
        // Volume du canal encore inconnu : on adopte l'état réel du node, sans le modifier
        m_volumes[ch] = std::clamp(volume, 0.0f, 1.0f);
        m_mutes[ch] = mute;
        m_volumeKnown[ch] = true;
        node.remember(volume, mute);
        log("canal " + std::to_string(ch + 1) + " : volume adopté de " + nodeDesc + " : " + percent(volume) +
            (mute ? " (muet)" : ""));
        applyToChannel(ch, &node);
        if (m_onChannelChanged)
            m_onChannelChanged(ch, m_volumes[ch], m_mutes[ch]);
        return;
    }

    // Rapport initial d'un node qui n'a pas encore reçu le volume du canal : pas un changement
    if (node.applied.empty())
        return;

    if (node.isEcho(volume, hasMute, muted))
        return;

    if (Clock::now() < node.graceUntil) {
        log(nodeDesc + " : volume " + percent(volume) + " juste après son apparition, volume du canal réimposé");
        applyToNode(node);
        return;
    }

    m_volumes[ch] = std::clamp(volume, 0.0f, 1.0f);
    m_mutes[ch] = mute;
    // Les réglages antérieurs ne peuvent plus revenir en écho : le node en est déjà sorti
    node.applied.clear();
    node.remember(volume, mute);
    log("canal " + std::to_string(ch + 1) + " : changement externe sur " + nodeDesc + " : " + percent(volume) +
        (mute ? " (muet)" : ""));
    applyToChannel(ch, &node);
    if (m_onChannelChanged)
        m_onChannelChanged(ch, m_volumes[ch], m_mutes[ch]);
}

void PipeWireMixer::onChannelAssigned(Node& node)
{
    // Volume du canal inconnu et état du node déjà rapporté (Props reçu avant l'assignation) :
    // on l'adopte tout de suite plutôt que d'attendre un prochain rapport
    if (node.channel >= 0 && !m_volumeKnown[node.channel] && node.hasReport)
        handleReportedProps(node, node.reportVolume, node.reportHasMute, node.reportMuted);
    else
        applyToNode(node);
}

void PipeWireMixer::applyToNode(Node& node)
{
    const int ch = node.channel;
    if (ch < 0 || !m_volumeKnown[ch])
        return;

    if (node.channels == 0) {
        // Nombre de canaux pas encore connu : on appliquera à la réception du param Props
        node.pendingApply = true;
        return;
    }
    node.pendingApply = false;

    // PipeWire attend un volume linéaire ; les mixeurs (pavucontrol, wpctl) utilisent une échelle cubique
    const float v = m_volumes[ch];
    float volumes[SPA_AUDIO_MAX_CHANNELS];
    const uint32_t n = std::min<uint32_t>(node.channels, SPA_AUDIO_MAX_CHANNELS);
    std::fill(volumes, volumes + n, v * v * v);

    uint8_t buffer[1024];
    spa_pod_builder b{};
    spa_pod_builder_init(&b, buffer, sizeof(buffer));

    spa_pod_frame f{};
    spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, n, volumes);
    spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
    spa_pod_builder_bool(&b, m_mutes[ch]);
    auto* pod = static_cast<spa_pod*>(spa_pod_builder_pop(&b, &f));

    pw_node_set_param(node.proxy, SPA_PARAM_Props, 0, pod);

    if (node.applied.empty())
        node.graceUntil = Clock::now() + kGracePeriod;
    node.remember(v, m_mutes[ch]);
}

void PipeWireMixer::updateNodeProps(Node& node, const spa_dict* props)
{
    if (auto v = dictGet(props, PW_KEY_MEDIA_CLASS); !v.empty())
        node.mediaClass = v;
    if (auto v = dictGet(props, PW_KEY_NODE_NAME); !v.empty())
        node.nodeName = v;
    if (auto v = dictGet(props, PW_KEY_APP_NAME); !v.empty())
        node.appName = toLower(v.c_str());
    if (auto v = dictGet(props, PW_KEY_APP_PROCESS_BINARY); !v.empty())
        node.binary = toLower(v.c_str());
}

void PipeWireMixer::addNode(uint32_t id, const spa_dict* props)
{
    const std::string mediaClass = dictGet(props, PW_KEY_MEDIA_CLASS);
    if (mediaClass != kClassStream && mediaClass != kClassSink && mediaClass != kClassSource)
        return;

    auto node = std::make_unique<Node>();
    node->mixer = this;
    node->id = id;
    updateNodeProps(*node, props);

    node->proxy = static_cast<pw_node*>(pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!node->proxy)
        return;

    static const pw_node_events nodeEv = [] {
        pw_node_events e{};
        e.version = PW_VERSION_NODE_EVENTS;
        e.info = &PipeWireMixer::onNodeInfo;
        e.param = &PipeWireMixer::onNodeParam;
        return e;
    }();
    pw_node_add_listener(node->proxy, &node->listener, &nodeEv, node.get());

    uint32_t params[] = {SPA_PARAM_Props};
    pw_node_subscribe_params(node->proxy, params, 1);

    m_nodes[id] = std::move(node);
}

void PipeWireMixer::addMetadata(uint32_t id)
{
    if (m_metadata)
        return;

    auto md = std::make_unique<Metadata>();
    md->id = id;
    md->proxy = static_cast<pw_metadata*>(
        pw_registry_bind(m_registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (!md->proxy)
        return;

    static const pw_metadata_events mdEv = [] {
        pw_metadata_events e{};
        e.version = PW_VERSION_METADATA_EVENTS;
        e.property = &PipeWireMixer::onMetadataProperty;
        return e;
    }();
    pw_metadata_add_listener(md->proxy, &md->listener, &mdEv, this);
    m_metadata = std::move(md);
}

// ---------------------------------------------------------------- Callbacks

void PipeWireMixer::onCoreError(void* data, uint32_t id, int /*seq*/, int res, const char* message)
{
    auto* self = static_cast<PipeWireMixer*>(data);
    std::cerr << "[pipewire] erreur : " << (message ? message : "?") << " (" << spa_strerror(res) << ")\n";
    if (id == PW_ID_CORE && res == -EPIPE && self->m_onFatal)
        self->m_onFatal();
}

void PipeWireMixer::onRegistryGlobal(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                                     uint32_t /*version*/, const spa_dict* props)
{
    auto* self = static_cast<PipeWireMixer*>(data);

    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
        self->addNode(id, props);
    } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        if (dictGet(props, PW_KEY_METADATA_NAME) == "default")
            self->addMetadata(id);
    }
}

void PipeWireMixer::onRegistryGlobalRemove(void* data, uint32_t id)
{
    auto* self = static_cast<PipeWireMixer*>(data);

    if (auto it = self->m_nodes.find(id); it != self->m_nodes.end()) {
        self->log("node " + std::to_string(id) + " (" + it->second->nodeName + ") supprimé");
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(it->second->proxy));
        self->m_nodes.erase(it);
        self->updateActiveCounts();
        return;
    }

    if (self->m_metadata && self->m_metadata->id == id) {
        spa_hook_remove(&self->m_metadata->listener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy*>(self->m_metadata->proxy));
        self->m_metadata.reset();
    }
}

void PipeWireMixer::onNodeInfo(void* data, const pw_node_info* info)
{
    auto* node = static_cast<Node*>(data);
    PipeWireMixer* self = node->mixer;

    if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS)
        self->updateNodeProps(*node, info->props);

    const bool first = !node->infoReceived;
    node->infoReceived = true;

    const int ch = self->resolveChannel(*node);
    if (first || ch != node->channel) {
        node->channel = ch;
        self->log("node " + std::to_string(node->id) + " [" + node->mediaClass + "] " + node->nodeName +
                  " app='" + node->appName + "' bin='" + node->binary + "' -> canal " +
                  (ch < 0 ? std::string("aucun") : std::to_string(ch + 1)));
        self->onChannelAssigned(*node);
        self->updateActiveCounts();
    }
}

void PipeWireMixer::onNodeParam(void* data, int /*seq*/, uint32_t id, uint32_t /*index*/, uint32_t /*next*/,
                                const spa_pod* param)
{
    auto* node = static_cast<Node*>(data);
    if (id != SPA_PARAM_Props || !param || !spa_pod_is_object(param))
        return;

    float volumes[SPA_AUDIO_MAX_CHANNELS];
    uint32_t nVolumes = 0;
    bool hasMute = false;
    bool muted = false;

    const auto* obj = reinterpret_cast<const spa_pod_object*>(param);
    const spa_pod_prop* prop;
    SPA_POD_OBJECT_FOREACH(obj, prop)
    {
        if (prop->key == SPA_PROP_channelVolumes && spa_pod_is_array(&prop->value) &&
            SPA_POD_ARRAY_VALUE_TYPE(&prop->value) == SPA_TYPE_Float) {
            nVolumes = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, volumes, SPA_AUDIO_MAX_CHANNELS);
        } else if (prop->key == SPA_PROP_mute) {
            hasMute = spa_pod_get_bool(&prop->value, &muted) == 0;
        }
    }

    if (nVolumes > 0) {
        node->channels = nVolumes;
        // Volumes linéaires -> échelle perceptuelle (cubique), comme pavucontrol : on prend le max
        const float linear = std::max(0.0f, *std::max_element(volumes, volumes + nVolumes));
        node->hasReport = true;
        node->reportVolume = std::cbrt(linear);
        node->reportHasMute = hasMute;
        node->reportMuted = muted;
        node->mixer->handleReportedProps(*node, node->reportVolume, hasMute, muted);
    }

    if (node->pendingApply && node->channels > 0)
        node->mixer->applyToNode(*node);
}

int PipeWireMixer::onMetadataProperty(void* data, uint32_t subject, const char* key, const char* /*type*/,
                                      const char* value)
{
    auto* self = static_cast<PipeWireMixer*>(data);
    if (subject != PW_ID_CORE)
        return 0;

    bool changed = false;
    if (key == nullptr) {
        // Toutes les propriétés ont été effacées
        changed = !self->m_defaultSink.empty() || !self->m_defaultSource.empty();
        self->m_defaultSink.clear();
        self->m_defaultSource.clear();
    } else if (std::strcmp(key, kKeyDefaultSink) == 0) {
        auto name = parseMetadataName(value);
        changed = name != self->m_defaultSink;
        self->m_defaultSink = std::move(name);
        self->log("sortie par défaut : " + self->m_defaultSink);
    } else if (std::strcmp(key, kKeyDefaultSource) == 0) {
        auto name = parseMetadataName(value);
        changed = name != self->m_defaultSource;
        self->m_defaultSource = std::move(name);
        self->log("entrée par défaut : " + self->m_defaultSource);
    }

    if (changed)
        self->remapAll();
    return 0;
}
