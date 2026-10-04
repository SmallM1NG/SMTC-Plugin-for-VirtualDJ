#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace smtc {
using ImageBytes = std::vector<std::uint8_t>;
using Cancelled = std::function<bool()>;
constexpr std::size_t MaxArtworkBytes = 8 * 1024 * 1024;
// 8 MiB 是插件自设保护值，并非 SMTC 接口的强制上限。
bool IsNetSearchPath(std::string_view path) noexcept;
bool IsNetSearchPath(std::wstring_view path) noexcept;

// 只读文件标签并返回内嵌图片，不修改文件，不使用文件图标。
ImageBytes ReadEmbeddedArtwork(const std::wstring& path);
std::wstring FindCoverLink(const std::wstring& database, const std::wstring& track,
    const Cancelled& cancelled = [] { return false; });
ImageBytes DownloadArtwork(const std::wstring& url, const Cancelled& cancelled);

class ArtworkWorker {
    // 只保留最新任务与结果；generation 变化即取消旧任务，无历史缓存。
public:
    struct Result { std::uint64_t ticket; ImageBytes image; };
    explicit ArtworkWorker(std::function<void()> ready = {});
    ~ArtworkWorker();
    std::uint64_t Request(std::string path, std::string home);
    void Cancel();
    std::optional<Result> Take();
    bool Running() const noexcept { return running_.load(); }
private:
    struct Job { std::uint64_t ticket; std::string path, home; };
    void Run() noexcept;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::atomic<bool> stopped_{false};
    std::atomic<bool> running_{true};
    std::atomic<std::uint64_t> generation_{0};
    std::optional<Job> pending_;
    std::optional<Result> result_;
    std::function<void()> ready_;
    std::mutex lifecycleMutex_;
    std::thread thread_;
};
}
