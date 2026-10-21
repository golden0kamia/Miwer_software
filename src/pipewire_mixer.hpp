#pragma once

#include "config.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <pipewire/pipewire.h>

// Suit les nodes audio de PipeWire et applique volume / mute par canal.
//
// Avec follow_external, les changements faits ailleurs (touches multimédia, pavucontrol,
// application qui change son propre volume) sont repris comme volume / mute du canal et
// signalés par onChannelChanged, pour que les faders motorisés suivent.
class PipeWireMixer {
public:
    // Volume ou mute d'un canal modifié hors de Miwer (ou adopté au démarrage).
    using ChannelChangedFn = std::function<void(int channel, float volume, bool muted)>;
    // Nombre de nodes assignés à un canal modifié.
    using ActiveCountFn = std::function<void(int channel, int count)>;

    // onFatal est appelé si la connexion à PipeWire est perdue.
    PipeWireMixer(pw_loop* loop, const Config& cfg, std::function<void()> onFatal);
    ~PipeWireMixer();

    PipeWireMixer(const PipeWireMixer&) = delete;
    PipeWireMixer& operator=(const PipeWireMixer&) = delete;

    // volume : 0.0 .. 1.0 (échelle perceptuelle, comme pavucontrol)
    void setChannelVolume(int channel, float volume);
    void setChannelMute(int channel, bool muted);
    bool channelMuted(int channel) const { return m_mutes.at(channel); }
    float channelVolume(int channel) const { return m_volumes.at(channel); }
    bool volumeKnown(int channel) const { return m_volumeKnown.at(channel); }
    int activeCount(int channel) const { return m_activeCounts.at(channel); }

    void setOnChannelChanged(ChannelChangedFn fn) { m_onChannelChanged = std::move(fn); }
    void setOnActiveCountChanged(ActiveCountFn fn) { m_onActiveCountChanged = std::move(fn); }

private:
    struct Node;
    struct Metadata;

    static void onCoreError(void* data, uint32_t id, int seq, int res, const char* message);
    static void onRegistryGlobal(void* data, uint32_t id, uint32_t permissions, const char* type,
                                 uint32_t version, const spa_dict* props);
    static void onRegistryGlobalRemove(void* data, uint32_t id);
    static void onNodeInfo(void* data, const pw_node_info* info);
    static void onNodeParam(void* data, int seq, uint32_t id, uint32_t index, uint32_t next,
                            const spa_pod* param);
    static int onMetadataProperty(void* data, uint32_t subject, const char* key, const char* type,
                                  const char* value);

    void addNode(uint32_t id, const spa_dict* props);
    void addMetadata(uint32_t id);
    void updateNodeProps(Node& node, const spa_dict* props);
    int resolveChannel(const Node& node) const;
    int findSpecialChannel(const char* target) const;
    void remapAll();
    void onChannelAssigned(Node& node);
    void applyToNode(Node& node);
    void applyToChannel(int channel, const Node* except);
    void handleReportedProps(Node& node, float volume, bool hasMute, bool muted);
    void updateActiveCounts();
    void log(const std::string& msg) const;

    const Config& m_cfg;
    std::function<void()> m_onFatal;
    ChannelChangedFn m_onChannelChanged;
    ActiveCountFn m_onActiveCountChanged;

    pw_context* m_context = nullptr;
    pw_core* m_core = nullptr;
    pw_registry* m_registry = nullptr;
    spa_hook m_coreListener{};
    spa_hook m_registryListener{};

    std::map<uint32_t, std::unique_ptr<Node>> m_nodes;
    std::unique_ptr<Metadata> m_metadata;
    std::string m_defaultSink;
    std::string m_defaultSource;

    std::array<float, kNumChannels> m_volumes{};
    std::array<bool, kNumChannels> m_volumeKnown{};
    std::array<bool, kNumChannels> m_mutes{};
    std::array<int, kNumChannels> m_activeCounts{};
};
