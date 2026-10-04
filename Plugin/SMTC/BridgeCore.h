#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace smtc {
struct Track {
    // 区分已加载、明确空 Deck 和查询失败，防止失败被误当成可控制的空 Deck。
    int deck = 1;
    bool loaded = false;
    bool empty = false; // 宿主明确确认空 Deck，不是查询失败。
    bool playing = false;
    std::string path, title, artist, album;
    unsigned revision = 0;

    bool SameIdentity(const Track& other) const noexcept {
        return deck == other.deck && loaded == other.loaded && empty == other.empty && path == other.path && revision == other.revision;
    }
};

inline int DeckFromSlider(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    const int selection = static_cast<int>(std::lround(std::clamp(value, 0.f, 1.f) * 6.f));
    return selection == 6 ? 0 : selection + 1; // 0 为 Master，5 为 Left，6 为 Right。
}

inline unsigned PollingMilliseconds(float value) noexcept {
    // 十档 100–1000 ms，无效输入回退到默认 100 ms。
    if (!std::isfinite(value)) return 100;
    return 100u * (1u + static_cast<unsigned>(std::lround(std::clamp(value, 0.f, 1.f) * 9.f)));
}

enum class Action { Play, Pause, Next, Previous };
struct Request {
    Action action;
    Track target;
    std::string sourceAppId;
};

inline bool MatchesPublishedMetadata(const Track& track, const std::string& title,
    const std::string& artist, const std::string& album) noexcept {
    if (track.empty) return title == "No track loaded";
    return track.loaded && title == track.title && artist == track.artist && album == track.album;
}

inline std::string CommandFor(const Request& request, const Track& current,
    bool enabled, bool controls) {
    // 请求绑定入队时的歌曲和设置修订；过期请求不得控制新歌曲。
    if (!enabled || !controls || (!current.loaded && !current.empty) || current.deck < 1 ||
        !request.target.SameIdentity(current)) return {};
    const auto prefix = "deck " + std::to_string(current.deck);
    switch (request.action) {
    case Action::Play: return prefix + " play";
    case Action::Pause: return prefix + " pause";
    case Action::Next: return prefix + " load_next keepplay";
    case Action::Previous: return prefix + " load_previous keepplay";
    }
    return {};
}
}
