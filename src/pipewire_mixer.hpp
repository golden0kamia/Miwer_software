#pragma once

#include "config.hpp"

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <pipewire/pipewire.h>

// Suit les nodes audio de PipeWire et applique volume / mute par canal.
class PipeWireMixer {
public:
    // onFatal est appelé si la connexion à PipeWire est perdue.
    PipeWireMixer(pw_loop* loop, const Config& cfg, std::function<void()> onFatal);
    ~PipeWireMixer();

    PipeWireMixer(const PipeWireMixer&) = delete;
    PipeWireMixer& operator=(const PipeWireMixer&) = delete;

    // volume : 0.0 .. 1.0 (échelle perceptuelle, comme pavucontrol)
    void setChannelVolume(int channel, float volume);
    void setChannelMute(int channel, bool muted);
    bool channelMuted(int channel) const { return m_mutes.at(channel); }

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
    void applyToNode(Node& node);
    void log(const std::string& msg) const;

    const Config& m_cfg;
    std::function<void()> m_onFatal;

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
};
