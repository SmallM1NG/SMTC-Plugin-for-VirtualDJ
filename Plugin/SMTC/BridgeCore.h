#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <limits>
#include <optional>
#include <charconv>

namespace smtc {
struct Track {
    // 区分已加载、明确空 Deck 和查询失败，防止失败被误当成可控制的空 Deck。
    // Distinguish loaded, confirmed-empty and failed-query decks so failures cannot enable controls.
    int deck = 1;
    bool loaded = false;
    bool empty = false; // 宿主明确确认空 Deck，不是查询失败。 / The host explicitly confirmed an empty deck; this is not a query failure.
    bool playing = false;
    bool metadataRead = false;
    std::string path, title, artist, album;
    unsigned revision = 0;

    bool SameIdentity(const Track& other) const noexcept {
        return deck == other.deck && loaded == other.loaded && empty == other.empty && path == other.path && revision == other.revision;
    }
};

inline int DeckFromSlider(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    const int selection = static_cast<int>(std::lround(std::clamp(value, 0.f, 1.f) * 6.f));
    return selection == 6 ? 0 : selection + 1; // 0 为 Master，5 为 Left，6 为 Right。 / 0 is Master, 5 is Left and 6 is Right.
}

inline constexpr float DefaultPollingValue = 0.f;
inline unsigned PollingMilliseconds(float value) noexcept {
    // 十档按间隔递增排列，无效输入回退到默认 100 ms。
    // Ten increasing intervals; invalid input falls back to 100 ms.
    constexpr unsigned intervals[] = {100, 200, 300, 400, 500, 600, 700, 800, 900, 1000};
    if (!std::isfinite(value)) return 100;
    const auto index = static_cast<unsigned>(std::lround(std::clamp(value, 0.f, 1.f) * 9.f));
    return intervals[index];
}

inline bool OriginalTimeFromSlider(float value) noexcept {
    return !std::isfinite(value) || value < 0.5f;
}

struct Timeline {
    std::int64_t position = 0, duration = 0; // Windows TimeSpan 使用 100 ns 单位。 / Windows TimeSpan uses 100 ns units.
    double playbackRate = 1.0;
};
inline std::optional<Timeline> MakeTimeline(double positionMs, double durationMs,
    bool reverse, double playbackRate = 1.0) noexcept {
    if (!std::isfinite(positionMs) || !std::isfinite(durationMs) ||
        durationMs <= 0 || reverse || !std::isfinite(playbackRate) || playbackRate <= 0) return {};
    // 两种模式均由宿主返回所需时间；仅转换单位，不重复折算。
    // The host supplies the time base for each mode; convert units without applying speed again.
    const double duration = durationMs * 10000.0;
    const double position = std::clamp(positionMs, 0.0, durationMs) * 10000.0;
    if (!std::isfinite(duration) || !std::isfinite(position) || duration < 1 ||
        duration >= static_cast<double>((std::numeric_limits<std::int64_t>::max)())) return {};
    return Timeline{static_cast<std::int64_t>(position), static_cast<std::int64_t>(duration), playbackRate};
}
// 按上次成功发布的锚点预测位置，避免小幅查询误差反复覆盖接收端进度。
// Predict from the last published anchor so small query errors do not repeatedly reset receiver progress.
struct TimelineAnchor {
    Timeline value;
    double sampledAt = 0;
    bool playing = false;
    bool NeedsUpdate(const Timeline& next, bool nextPlaying, double now) const noexcept {
        if (next.duration != value.duration || next.playbackRate != value.playbackRate ||
            nextPlaying != playing) return true;
        const double predicted = std::clamp(static_cast<double>(value.position) +
            (playing ? std::max(0.0, now - sampledAt) * 10000000.0 * value.playbackRate : 0.0),
            0.0, static_cast<double>(value.duration));
        return std::abs(static_cast<double>(next.position) - predicted) > 1000000.0;
    }
};

enum class Action { Play, Pause, Next, Previous, Seek };
struct Request {
    Action action;
    Track target;
    std::string sourceAppId;
    double seekRatio = 0;
    unsigned seekRevision = 0;
};

inline bool MatchesPublishedMetadata(const Track& track, const std::string& title,
    const std::string& artist, const std::string& album) noexcept {
    if (track.empty) return title == "No track loaded";
    return track.loaded && title == track.title && artist == track.artist && album == track.album;
}

inline std::string CommandFor(const Request& request, const Track& current,
    bool enabled, bool controls) {
    // 请求绑定入队时的歌曲和设置修订；过期请求不得控制新歌曲。
    // Bind requests to track identity and settings revision; stale requests must not control a new track.
    if (!enabled || !controls || (!current.loaded && !current.empty) || current.deck < 1 ||
        !request.target.SameIdentity(current)) return {};
    const auto prefix = "deck " + std::to_string(current.deck);
    switch (request.action) {
    case Action::Play: return prefix + " play";
    case Action::Pause: return prefix + " pause";
    case Action::Next: return prefix + " load_next keepplay";
    case Action::Previous: return prefix + " load_previous keepplay";
    case Action::Seek: {
        if (!current.loaded || !std::isfinite(request.seekRatio) ||
            request.seekRatio < 0 || request.seekRatio > 1) return {};
        // 固定小数格式不受系统语言影响；百分比定位不改变播放/暂停状态。
        // Use locale-independent formatting; percentage seeking preserves the playing or paused state.
        char value[48]{};
        const auto result = std::to_chars(value, value + sizeof(value),
            request.seekRatio * 100.0, std::chars_format::fixed, 9);
        if (result.ec != std::errc{}) return {};
        return prefix + " song_pos " + std::string(value, result.ptr) + "%";
    }
    }
    return {};
}
}
