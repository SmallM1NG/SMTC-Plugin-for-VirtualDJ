#include "vdjPlugin8.h"
#include "BridgeCore.h"
#include "Cover.h"

#include "Logger.h"
#include <shellapi.h>

#include <systemmediatransportcontrolsinterop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <set>

using namespace winrt;
using namespace Windows::Media;
using namespace Windows::Media::Control;
using namespace Windows::Foundation;

namespace {
HMODULE g_module{};
std::atomic<bool> g_owned{false};
// 发布时更新此版本；按钮文字和 Release 地址自动跟随。
constexpr char kVersion[] = "0.1.2";
const std::string kVersionLabel = std::string("Version ") + kVersion;
const std::string kReleaseUrl =
    std::string("https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/tag/v") + kVersion;
constexpr char kSourceLabel[] = "Source Deck";
constexpr char kDeveloperLabel[] = "Developed by 小小小小铭 (aka DJM1NG)";
constexpr char kAuthor[] = "小小小小铭 (aka DJM1NG)";
constexpr char kDescription[] = "Share track information, artwork, playback progress and controls with Windows";

void Log(const char* message, HRESULT hr = S_OK) noexcept {
    if (hr == S_OK) smtc::LogEvent(smtc::LogLevel::Info, "Bridge", "%s", message);
    else smtc::LogEvent(FAILED(hr) ? smtc::LogLevel::Error : smtc::LogLevel::Warning,
        "Bridge", "%s; Windows error code=0x%08lX", message, static_cast<unsigned long>(hr));
}
// 等待 WinRT 操作时监听停止事件；只有成功完成后才允许调用 GetResults。
template<class Operation>
bool WaitForAsync(const Operation& operation, HANDLE stop, const char* description,
    std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto cancel = [&] {
        try { operation.Cancel(); }
        catch (...) { Log("WinRT cancellation request failed", to_hresult()); }
    };
    for (;;) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) {
            cancel();
            return false;
        }
        const auto status = operation.Status();
        if (status == AsyncStatus::Completed) return true;
        if (status == AsyncStatus::Error) throw_hresult(operation.ErrorCode());
        if (status == AsyncStatus::Canceled) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            cancel();
            smtc::LogEvent(smtc::LogLevel::Warning, "Async",
                "%s timed out; operation cancelled", description);
            return false;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const DWORD interval = static_cast<DWORD>((std::min)(std::chrono::milliseconds(25),
            (std::max)(std::chrono::milliseconds(1), remaining)).count());
        if (stop) {
            const DWORD result = WaitForSingleObject(stop, interval);
            if (result == WAIT_FAILED) { cancel(); throw_last_error(); }
        } else Sleep(interval);
    }
}
const char* ActionName(smtc::Action action) noexcept {
    switch (action) {
    case smtc::Action::Play: return "Play";
    case smtc::Action::Pause: return "Pause";
    case smtc::Action::Next: return "Next track";
    case smtc::Action::Previous: return "Previous track";
    case smtc::Action::Seek: return "Seek";
    }
    return "Unknown";
}
const char* SourceName(int deck) noexcept {
    switch (deck) {
    case 1: return "Deck 1"; case 2: return "Deck 2";
    case 3: return "Deck 3"; case 4: return "Deck 4";
    case 5: return "Left"; case 6: return "Right";
    default: return "Master";
    }
}
struct Mailbox {
    // Windows 回调只入队；宿主查询与命令统一由桥接线程执行。
    std::mutex mutex;
    std::deque<smtc::Request> requests;
    smtc::Track displayed;
    // 跳转快照与时间轴一起发布，不能使用尚未更新的 displayed。
    smtc::Track seekTrack;
    std::int64_t seekDuration = 0;
    unsigned seekRevision = 0;
    bool accepting = false;
    bool closed = false;
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Mailbox() { if (stop) CloseHandle(stop); if (wake) CloseHandle(wake); }

    void Push(smtc::Action action, std::string sourceAppId = {}) {
        std::lock_guard lock(mutex);
        if (!closed && accepting && requests.size() < 32) {
            requests.push_back({action, displayed, std::move(sourceAppId)});
            smtc::LogEvent(smtc::LogLevel::Info, "Control",
                "Windows media button: %s; queued for Deck %d; source_at_callback=%s",
                ActionName(action), displayed.deck,
                requests.back().sourceAppId.empty() ? "Unavailable" : requests.back().sourceAppId.c_str());
            SetEvent(wake);
        } else smtc::LogEvent(smtc::LogLevel::Warning, "Control", "Windows media button ignored: %s", closed ? "SMTC is stopping" : !accepting ? "no controllable track is available" : "command queue is full");
    }
    void PushSeek(std::int64_t position, std::string source = {}) {
        std::lock_guard lock(mutex);
        if (closed || !seekTrack.loaded || seekDuration <= 0 || position < 0 ||
            position > seekDuration) return;
        // 连续拖动合并到最新请求，并保留播放、切歌命令的顺序。
        std::erase_if(requests, [](const auto& request) { return request.action == smtc::Action::Seek; });
        if (requests.size() >= 32) return;
        requests.push_back({smtc::Action::Seek, seekTrack, std::move(source),
            static_cast<double>(position) / seekDuration, seekRevision});
        smtc::LogEvent(smtc::LogLevel::Info, "Control",
            "Windows seek requested; deck=%d; position_ticks=%lld; duration_ticks=%lld",
            seekTrack.deck, static_cast<long long>(position), static_cast<long long>(seekDuration));
        SetEvent(wake);
    }
    void Close() {
        std::lock_guard lock(mutex);
        closed = true;
        accepting = false;
        seekDuration = 0;
        requests.clear();
        SetEvent(stop);
    }
};

class Plugin final : public IVdjPlugin8 {
#ifdef SMTC_TESTING
    friend void RecoveryTests();
    std::function<void()> workerFault_;
#endif
public:
    Plugin() { cb = nullptr; hInstance = nullptr; }
    ~Plugin() override { Shutdown(); }

    HRESULT VDJ_API OnGetPluginInfo(TVdjPluginInfo8* info) override {
        if (!info) return E_POINTER;
        info->PluginName = "SMTC";
        info->Author = kAuthor;
        info->Description = kDescription;
        info->Version = kVersion;
        info->Bitmap = nullptr;
        info->Flags = 0;
        return S_OK;
    }

    HRESULT VDJ_API OnLoad() override {
        if (!cb) { Log("Load rejected: host callback is missing", E_POINTER); return E_POINTER; }
        if (owns_) return S_FALSE;
        bool expected = false;
        if (!g_owned.compare_exchange_strong(expected, true)) {
            smtc::LogEvent(smtc::LogLevel::Warning, "Lifecycle", "Duplicate plugin instance rejected");
            return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        }
        owns_ = true;
        try {
            check_hresult(DeclareParameterSwitch(&enabled_, 1, "Enable SMTC", "Enable", true));
            check_hresult(DeclareParameterSwitch(&shareCover_, 14, "Share Cover", "Cover", true));
            check_hresult(DeclareParameterSwitch(&shareInfo_, 15, "Share Track Info", "Track Info", true));
            check_hresult(DeclareParameterSwitch(&shareProgress_, 12, "Share Progress", "Progress", true));
            check_hresult(DeclareParameterSwitch(&allowSeeking_, 13, "Enable Seeking", "Seeking", true));
            check_hresult(DeclareParameterSlider(&timeMode_, 16, "Timeline Mode", "Timeline", 0.f));
            check_hresult(DeclareParameterSlider(&deck_, 5, kSourceLabel, kSourceLabel, 1.f));
            check_hresult(DeclareParameterSlider(&polling_, 10, "Polling Interval", "Polling", smtc::DefaultPollingValue));
            check_hresult(DeclareParameterButton(&developerButton_, 8, kDeveloperLabel, kDeveloperLabel));
            check_hresult(DeclareParameterButton(&versionButton_, 11, kVersionLabel.c_str(), kVersion));
            PublishSettings();
            smtc::LogEvent(smtc::LogLevel::Info, "Bridge", "Plugin loaded v%s; controlled by Enable SMTC", kVersion);
            if (enabled_) check_hresult(StartRuntime());
            return S_OK;
        } catch (...) {
            const HRESULT error = to_hresult();
            Log("OnLoad failed", error);
            Shutdown();
            return error;
        }
    }

    HRESULT StartRuntime() {
        std::lock_guard lock(runtimeMutex_);
        if (!owns_ || !cb) return E_UNEXPECTED;
        if (worker_.joinable()) {
            if (workerRunning_.load()) return S_FALSE;
            Log("Re-enabling SMTC after worker exit; reclaiming stopped worker");
            StopRuntime();
        }
        try {
            failedQueries_.clear();
            missingTrackSince_ = {};
            publishedInfo_ = true;
            timelinePublished_ = false;
            progressLogAt_ = 0;
            progressPolls_ = progressPublications_ = progressStateChanges_ = progressRateChanges_ = 0;
            progressDurationChanges_ = progressDriftCorrections_ = progressIdentityChanges_ = progressForcedSeeks_ = 0;
            progressFailed_ = false;
            PublishSettings();
            mailbox_ = std::make_shared<Mailbox>();
            if (!mailbox_->stop || !mailbox_->wake) throw_last_error();

            // 撤销订阅后仍可能有迟到回调；固定 DLL，回调仅持有共享邮箱和会话管理器，不持有插件对象。
            HMODULE pinned{};
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&g_module), &pinned)) throw_last_error();
            // 创建前置为运行中，避免新线程尚未开始时被误判为已退出。
            workerRunning_.store(true);
            worker_ = std::thread([this] { Run(); });
            Log("Enable SMTC: worker started");
            smtc::LogEvent(smtc::LogLevel::Info, "Lifecycle", "SMTC enabled; source=%s; polling interval=%u ms; platform=64-bit; timestamps=UTC", SourceName(smtc::DeckFromSlider(deck_)), pollingMs_.load());
            return S_OK;
        } catch (...) {
            const HRESULT error = to_hresult();
            Log("SMTC startup failed", error);
            StopRuntime();
            return error;
        }
    }

    ULONG VDJ_API Release() override { delete this; return 0; }

    HRESULT VDJ_API OnParameter(int id) override {
        if (id == 1) {
            enabled_ = enabled_ != 0;
            PublishSettings();
            if (enabled_) return StartRuntime();
            std::lock_guard lock(runtimeMutex_);
            StopRuntime();
            Log("SMTC disabled; polling stopped, artwork tasks cancelled, Windows media session closed");
            return S_OK;
        }
        if (id == 16) timeMode_ = std::isfinite(timeMode_) ? std::clamp(timeMode_, 0.f, 1.f) : 0.f;
        if (id == 14) shareCover_ = shareCover_ != 0;
        if (id == 15) shareInfo_ = shareInfo_ != 0;
        if (id == 13) allowSeeking_ = allowSeeking_ != 0;
        if (id == 12) shareProgress_ = shareProgress_ != 0;
        if (id == 8) {
            if (developerButton_ && !developerHeld_)
                OpenProjectPage("https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ", "Project repository");
            developerHeld_ = developerButton_ != 0;
        }
        if (id == 11) {
            // 按钮在按下和松开时都通知宿主，仅在首次按下时打开页面。
            if (versionButton_ && !versionHeld_)
                OpenProjectPage(kReleaseUrl.c_str(), "Version release");
            versionHeld_ = versionButton_ != 0;
        }
        if (id == 5) {
            // 保留滚轮的微小增量，只对实际来源进行分档。
            deck_ = std::isfinite(deck_) ? std::clamp(deck_, 0.f, 1.f) : 1.f;
        }
        if (id == 10) {
            polling_ = std::isfinite(polling_) ? std::clamp(polling_, 0.f, 1.f) : smtc::DefaultPollingValue;
            smtc::LogEvent(smtc::LogLevel::Info, "Settings", "Polling interval changed; milliseconds=%u", smtc::PollingMilliseconds(polling_));
        }
        if (PublishSettings()) Wake();
        return S_OK;
    }

    HRESULT VDJ_API OnGetParameterString(int id, char* text, int size) override {
        if (!text || size <= 0) return E_INVALIDARG;
        if (id == 1) {
            _snprintf_s(text, size, _TRUNCATE, "%s", !enabled_ ? "Disabled" :
                workerRunning_.load() ? "Enabled" : "Worker stopped; re-enable SMTC");
            return S_OK;
        }
        if (id == 8) {
            _snprintf_s(text, size, _TRUNCATE, "%s", kDeveloperLabel);
            return S_OK;
        }
        if (id == 10) {
            _snprintf_s(text, size, _TRUNCATE, "%u ms", smtc::PollingMilliseconds(polling_));
            return S_OK;
        }
        if (id == 16) {
            _snprintf_s(text, size, _TRUNCATE, "%s", smtc::OriginalTimeFromSlider(timeMode_) ? "Original + Pitch" : "Adjusted");
            return S_OK;
        }
        if (id == 14 || id == 15) {
            _snprintf_s(text, size, _TRUNCATE, "%s", (id == 14 ? shareCover_ : shareInfo_) ? "Enabled" : "Disabled");
            return S_OK;
        }
        if (id == 13) {
            _snprintf_s(text, size, _TRUNCATE, "%s", allowSeeking_ ? "Enabled" : "Disabled");
            return S_OK;
        }
        if (id == 12) {
            _snprintf_s(text, size, _TRUNCATE, "%s", shareProgress_ ? "Enabled" : "Disabled");
            return S_OK;
        }
        if (id == 11) { _snprintf_s(text, size, _TRUNCATE, "%s", kVersionLabel.c_str()); return S_OK; }
        if (id != 5) return E_NOTIMPL;
        const int deck = smtc::DeckFromSlider(deck_);
        _snprintf_s(text, size, _TRUNCATE, "%s", SourceName(deck));
        return S_OK;
    }

private:
    int enabled_ = 1;
    int shareProgress_ = 1, allowSeeking_ = 1;
    int shareCover_ = 1, shareInfo_ = 1;
    std::atomic<unsigned> sharingFlags_{3}; // 位 0 为封面，位 1 为文字信息。
    std::atomic<unsigned> coverRevision_{0};
    bool publishedInfo_ = true; // 仅由桥接线程访问。
    std::atomic<bool> seekingEnabled_{true};
    std::atomic<unsigned> seekRevision_{0};
    std::atomic<bool> progressEnabled_{true};
    bool timelinePublished_ = false; // 仅由桥接线程访问。
    smtc::TimelineAnchor timelineAnchor_;
    smtc::Track timelineTrack_;
    unsigned timelineRevision_ = 0;
    bool forceTimeline_ = false;
    // 进度日志每五秒汇总，避免逐帧写入影响播放。
    double progressLogAt_ = 0;
    unsigned progressPolls_ = 0, progressPublications_ = 0;
    unsigned progressStateChanges_ = 0, progressRateChanges_ = 0;
    unsigned progressDurationChanges_ = 0, progressDriftCorrections_ = 0;
    unsigned progressIdentityChanges_ = 0, progressForcedSeeks_ = 0;

    bool progressFailed_ = false;
    int developerButton_ = 0, versionButton_ = 0;
    bool developerHeld_ = false, versionHeld_ = false;

    float timeMode_ = 0.f;
    std::atomic<bool> originalTime_{true};
    float deck_ = 1.f, polling_ = smtc::DefaultPollingValue;
    std::atomic<unsigned> pollingMs_{100};
    // 原子设置字包含启用位、来源和修订号，避免工作线程读取混合配置。
    std::atomic<unsigned> settings_{1};
    bool owns_ = false;
    std::mutex runtimeMutex_;
    std::thread worker_;
    std::atomic<bool> workerRunning_{false};
    std::shared_ptr<Mailbox> mailbox_;
    std::set<std::string> failedQueries_; // 仅由桥接线程访问。
    std::chrono::steady_clock::time_point missingTrackSince_{};

    void RecordQuery(const std::string& query, HRESULT result) {
        if (result != S_OK) {
            if (result == S_FALSE) {
                if (failedQueries_.insert(query).second)
                    smtc::LogEvent(smtc::LogLevel::Debug, "Host", "VirtualDJ returned no value for %s; repeated messages omitted", query.c_str());
                return;
            }
            if (failedQueries_.insert(query).second)
                smtc::LogEvent(smtc::LogLevel::Warning, "Host", "Could not read VirtualDJ value: %s; error code=0x%08lX; repeated messages omitted until recovery", query.c_str(), static_cast<unsigned long>(result));
        } else if (failedQueries_.erase(query))
            smtc::LogEvent(smtc::LogLevel::Info, "Host", "VirtualDJ value is available again: %s", query.c_str());
    }

    bool PublishSettings() noexcept {
        const unsigned selection = (enabled_ ? 1u : 0u) | (smtc::DeckFromSlider(deck_) << 8);
        const unsigned previous = settings_.load();
        if ((previous & 0xffffu) != selection) {
            settings_.store(((previous + 0x10000u) & 0xffff0000u) | selection);
            smtc::LogEvent(smtc::LogLevel::Info, "Settings", "Settings updated; SMTC=%s; source=%s", enabled_ ? "Enabled" : "Disabled", SourceName(smtc::DeckFromSlider(deck_)));
        }
        const unsigned flags = (shareCover_ ? 1u : 0u) | (shareInfo_ ? 2u : 0u);
        const unsigned oldFlags = sharingFlags_.exchange(flags);
        if ((oldFlags ^ flags) & 1u) coverRevision_.fetch_add(1);
        if (oldFlags != flags) smtc::LogEvent(smtc::LogLevel::Info, "Settings",
            "Sharing changed; artwork=%s; track information=%s",
            shareCover_ ? "Enabled" : "Disabled", shareInfo_ ? "Enabled" : "Disabled");
        const unsigned interval = smtc::PollingMilliseconds(polling_);
        const bool intervalChanged = pollingMs_.exchange(interval) != interval;
        const bool progressChanged = progressEnabled_.exchange(shareProgress_ != 0) != (shareProgress_ != 0);
        if (progressChanged) smtc::LogEvent(smtc::LogLevel::Info, "Settings",
            "Playback progress sharing %s", shareProgress_ ? "enabled" : "disabled");
        const bool seekingChanged = seekingEnabled_.exchange(allowSeeking_ != 0) != (allowSeeking_ != 0);
        const bool original = smtc::OriginalTimeFromSlider(timeMode_);
        const bool modeChanged = originalTime_.exchange(original) != original;
        // 模式切换会改变时间单位语义，丢弃旧模式下尚未执行的位置请求。
        if (progressChanged || seekingChanged || modeChanged) seekRevision_.fetch_add(1);
        if (modeChanged) smtc::LogEvent(smtc::LogLevel::Info, "Settings",
            "Progress time mode changed; mode=%s", original ? "Original + Pitch" : "Adjusted");
        if (seekingChanged) smtc::LogEvent(smtc::LogLevel::Info, "Settings",
            "Playback seeking %s", allowSeeking_ ? "enabled" : "disabled");
        return (previous & 0xffffu) != selection || intervalChanged || progressChanged || seekingChanged || oldFlags != flags || modeChanged;
    }

    void Wake() noexcept { std::lock_guard lock(runtimeMutex_); if (mailbox_) SetEvent(mailbox_->wake); }

    void OpenProjectPage(const char* url, const char* pageName) {
        const auto wideUrl = to_hstring(url);
        SHELLEXECUTEINFOW open{sizeof(open)};
        open.fMask = SEE_MASK_ASYNCOK | SEE_MASK_FLAG_NO_UI;
        open.lpVerb = L"open";
        open.lpFile = wideUrl.c_str();
        open.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&open))
            smtc::LogEvent(smtc::LogLevel::Warning, "UI", "%s page launch failed; Windows error code=0x%08lX", pageName, GetLastError());
        else smtc::LogEvent(smtc::LogLevel::Info, "UI", "%s page launch requested", pageName);
    }

    void StopRuntime() noexcept {
        // 先拒绝回调并唤醒线程，再等待退出，最后释放邮箱和宿主引用。
        if (mailbox_) mailbox_->Close();
        if (worker_.joinable()) worker_.join();
        workerRunning_.store(false);
        mailbox_.reset();
    }

    void Shutdown() noexcept {
        std::lock_guard lock(runtimeMutex_);
        if (owns_) Log("Plugin shutdown requested");
        StopRuntime();
        if (owns_) {
            owns_ = false;
            g_owned.store(false);
            Log("Plugin released; worker stopped");
            smtc::CloseLog();
        }
    }

    bool Number(const std::string& query, double& value) {
        value = 0;
        const HRESULT result = GetInfo(query.c_str(), &value);
        RecordQuery(query, result == S_OK && !std::isfinite(value) ? E_INVALIDARG : result);
        if (result == S_OK && std::isfinite(value)) return true;
        value = 0;
        return false;
    }

    std::string Text(const std::string& query) {
        char value[8192]{};
        const HRESULT result = GetStringInfo(query.c_str(), value, sizeof(value));
        RecordQuery(query, result);
        if (result != S_OK) return {};
        value[sizeof(value) - 1] = 0;
        return value;
    }

    smtc::Track ReadTrack(unsigned settings, const smtc::Track& previous) {
        // 同曲复用文字；切歌或设置修订后刷新，不重复查询标签。
        smtc::Track track;
        track.deck = static_cast<int>((settings >> 8) & 0xff);
        track.revision = settings >> 16;
        if (track.deck == 0 || track.deck == 5 || track.deck == 6) {
            double master{};
            const char* query = track.deck == 5 ? "deck left get_deck" :
                track.deck == 6 ? "deck right get_deck" : "get_activedeck";
            // 将逻辑来源解析成真实 Deck，后续命令绑定此快照，避免左右映射变化时误控。
            if (!Number(query, master) || master < 1 || master > 99 || std::floor(master) != master) {
                track.deck = 0;
                return track;
            }
            track.deck = static_cast<int>(master);
        }
        const auto prefix = "deck " + std::to_string(track.deck) + " ";
        double loaded{}, playing{};
        if (!Number(prefix + "loaded", loaded)) return track;
        if (loaded == 0) { track.empty = true; return track; }
        if (!Number(prefix + "play", playing)) return track;
        track.loaded = true;
        track.playing = playing != 0;
        track.path = Text(prefix + "get_filepath");
        if (track.path.empty()) { track.loaded = false; return track; }
        if (!(sharingFlags_.load() & 2u)) {
            // 关闭信息共享时仍保留曲目路径供播放控制、封面与时间轴使用。
        } else if (track.SameIdentity(previous) && previous.metadataRead) {
            track.metadataRead = true;
            track.title = previous.title; track.artist = previous.artist; track.album = previous.album;
        } else {
            track.metadataRead = true;
            track.title = Text(prefix + "get_title_remix");
            if (track.title.empty()) track.title = Text(prefix + "get_title");
            track.artist = Text(prefix + "get_artist");
            track.album = Text(prefix + "get_album");
            if (track.title.empty()) track.title = Text(prefix + "get_filename");
            if (track.title.empty()) track.title = "VirtualDJ - Deck " + std::to_string(track.deck);
        }

        // 宿主查询不是事务；再次检查路径，丢弃查询期间发生切歌的快照。
        double stillLoaded{};
        if (!Number(prefix + "loaded", stillLoaded) || !stillLoaded ||
            Text(prefix + "get_filepath") != track.path) track.loaded = false;
        return track;
    }

    struct ArtworkState {
        explicit ArtworkState(std::function<void()> ready) : worker(std::move(ready)) {}
        smtc::ArtworkWorker worker;
        std::uint64_t ticket = 0;
        bool pending = false;
        bool active = true;
        unsigned revision = 0;
    };

    void UpdateArtwork(const SystemMediaTransportControls& controls, const smtc::Track& track,
        bool changed, ArtworkState& artwork) {
        const bool enabled = (sharingFlags_.load() & 1u) != 0;
        const unsigned revision = coverRevision_.load();
        if (!enabled) {
            if (artwork.active) {
                artwork.worker.Cancel(); artwork.ticket = 0; artwork.pending = false;
                auto updater = controls.DisplayUpdater();
                updater.Thumbnail(nullptr); updater.Update();
                artwork.active = false;
            }
            return;
        }
        // 每次换歌或重新启用封面只发起一次任务；修订号拦截关闭后的旧结果。
        changed = changed || !artwork.active || artwork.revision != revision;
        artwork.active = true;
        artwork.revision = revision;
        if (changed) {
            const auto home = smtc::IsNetSearchPath(track.path) ? Text("get_vdj_folder") : std::string{};
            artwork.ticket = artwork.worker.Request(track.path, home);
            artwork.pending = true;
        }
        auto result = artwork.worker.Take();
        if (!result || result->ticket != artwork.ticket) {
            if (artwork.pending && !artwork.worker.Running()) {
                artwork.pending = false;
                Log("Artwork worker exited; no retry for this track; next track or re-enable will restart artwork");
            }
            return;
        }
        artwork.pending = false;
        if (result->image.empty()) {
            Log("No artwork available; no automatic retry for this track");
            return;
        }
        using namespace Windows::Storage::Streams;
        InMemoryRandomAccessStream stream;
        const unsigned artworkSettings = settings_.load();
        DataWriter writer(stream);
        writer.WriteBytes(result->image);
        auto store = writer.StoreAsync();
        // 完成回调仅保留流与写入器，不访问插件；取消后的迟到完成仍有有效资源。
        store.Completed([writer, stream](const auto&, AsyncStatus) noexcept {});
        if (!WaitForAsync(store, mailbox_ ? mailbox_->stop : nullptr, "Artwork stream write")) {
            if (mailbox_ && WaitForSingleObject(mailbox_->stop, 0) == WAIT_OBJECT_0) return;
            Log("Artwork stream write did not complete; no automatic retry for this track");
            return;
        }
        store.GetResults();
        if (mailbox_ && WaitForSingleObject(mailbox_->stop, 0) == WAIT_OBJECT_0) return;
        if (settings_.load() != artworkSettings || !(sharingFlags_.load() & 1u) ||
            coverRevision_.load() != revision) return;
        writer.DetachStream();
        stream.Seek(0);
        auto updater = controls.DisplayUpdater();
        updater.Thumbnail(RandomAccessStreamReference::CreateFromStream(stream));
        updater.Update();
        Log("Artwork published");
    }

    bool IsCurrentWindowsSession(const GlobalSystemMediaTransportControlsSessionManager& manager,
        const smtc::Request& request) noexcept {
        // 播放/暂停不按当前会话过滤；Windows 当前会话可能晚于指定卡片命令更新。
        if (request.action == smtc::Action::Play || request.action == smtc::Action::Pause) return true;
        if (!manager) return true;
        try {
            const auto current = manager.GetCurrentSession();
            if (!current) {
                Log("Windows media command ignored: no current media session");
                return false;
            }
            const auto read = current.TryGetMediaPropertiesAsync();
            if (!WaitForAsync(read, mailbox_->stop, "Current media properties read")) return false;
            const auto properties = read.GetResults();
            const auto currentSource = to_string(current.SourceAppUserModelId());
            if (!request.sourceAppId.empty() && request.sourceAppId != currentSource) {
                smtc::LogEvent(smtc::LogLevel::Warning, "Control",
                    "Windows media command ignored: session changed after callback; source_at_callback=%s current_source=%s",
                    request.sourceAppId.c_str(), currentSource.c_str());
                return false;
            }
            const bool matches = smtc::MatchesPublishedMetadata(request.target,
                to_string(properties.Title()), to_string(properties.Artist()), to_string(properties.AlbumTitle()));
            if (!matches) {
                smtc::LogEvent(smtc::LogLevel::Warning, "Control",
                    "Windows media command ignored: current session belongs to another source; source_app_id=%s",
                    currentSource.c_str());
            }
            return matches;
        } catch (...) {
            Log("Current Windows media session could not be verified; command ignored", to_hresult());
            return false;
        }
    }

    void ClearTimeline(const SystemMediaTransportControls& controls) {
        if (mailbox_) { std::lock_guard lock(mailbox_->mutex); mailbox_->seekDuration = 0; }
        if (!timelinePublished_) return;
        SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(TimeSpan{0});
        timeline.EndTime(TimeSpan{0});
        timeline.Position(TimeSpan{0});
        timeline.MinSeekTime(TimeSpan{0});
        timeline.MaxSeekTime(TimeSpan{0});
        controls.UpdateTimelineProperties(timeline);
        controls.PlaybackRate(1.0);
        timelinePublished_ = false;
    }

    void UpdateProgress(const SystemMediaTransportControls& controls, const smtc::Track& track) {
        if (!progressEnabled_.load() || !track.loaded) { ClearTimeline(controls); return; }
        const unsigned settings = settings_.load();
        const unsigned seekRevision = seekRevision_.load();
        const bool seekable = seekingEnabled_.load();
        const bool original = originalTime_.load();
        const auto prefix = "deck " + std::to_string(track.deck) + " ";
        if (track.revision != (settings >> 16)) { ClearTimeline(controls); return; }
        const double sampledAt = std::chrono::duration<double>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        double position{}, duration{}, reverse{}, rate = 1.0;
        std::optional<smtc::Timeline> values;
        // 原始模式保持歌词时间轴并单独发布速度；折算模式沿用宿主折算时间。
        const std::string timeSuffix = original ? " 'absolute'" : "";
        if (Number(prefix + "get_time 'elapsed'" + timeSuffix, position) &&
            Number(prefix + "get_time 'total'" + timeSuffix, duration) &&
            Number(prefix + "reverse", reverse) &&
            (!original || Number(prefix + "get_pitch_value", rate)))
            // get_pitch_value 返回百分数，Windows PlaybackRate 使用倍数。
            values = smtc::MakeTimeline(position, duration, reverse != 0, original ? rate / 100.0 : rate);
        double loaded{};
        if (!values || !Number(prefix + "loaded", loaded) || !loaded ||
            Text(prefix + "get_filepath") != track.path || settings_.load() != settings ||
            !progressEnabled_.load() || seekRevision_.load() != seekRevision) { ClearTimeline(controls); return; }
        if (mailbox_ && WaitForSingleObject(mailbox_->stop, 0) == WAIT_OBJECT_0) return;
        const bool publish = !timelinePublished_ || forceTimeline_ ||
            !timelineTrack_.SameIdentity(track) || timelineRevision_ != seekRevision ||
            timelineAnchor_.NeedsUpdate(*values, track.playing, sampledAt);
        ++progressPolls_;
        if (!progressLogAt_) progressLogAt_ = sampledAt;
        if (publish) {
            ++progressPublications_;
            if (!timelinePublished_ || !timelineTrack_.SameIdentity(track) || timelineRevision_ != seekRevision)
                ++progressIdentityChanges_;
            if (forceTimeline_) ++progressForcedSeeks_;
            if (timelinePublished_) {
                const bool stateChanged = timelineAnchor_.playing != track.playing;
                const bool rateChanged = timelineAnchor_.value.playbackRate != values->playbackRate;
                const bool durationChanged = timelineAnchor_.value.duration != values->duration;
                if (stateChanged) ++progressStateChanges_;
                if (rateChanged) ++progressRateChanges_;
                if (durationChanged) ++progressDurationChanges_;
                if (!stateChanged && !rateChanged && !durationChanged &&
                    timelineAnchor_.NeedsUpdate(*values, track.playing, sampledAt)) ++progressDriftCorrections_;
            }
        }
        if (publish) {
            const auto status = track.playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused;
            if (controls.PlaybackStatus() != status) controls.PlaybackStatus(status);
            SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime(TimeSpan{0});
            timeline.EndTime(TimeSpan{values->duration});
            timeline.Position(TimeSpan{values->position});
            // 只读时收窄范围；两个开关同时开启才开放整首歌曲。
            timeline.MinSeekTime(TimeSpan{seekable ? 0 : values->position});
            timeline.MaxSeekTime(TimeSpan{seekable ? values->duration : values->position});
            if (!timelinePublished_ || timelineAnchor_.value.playbackRate != values->playbackRate)
                controls.PlaybackRate(values->playbackRate);
            controls.UpdateTimelineProperties(timeline);
            timelinePublished_ = true;
            timelineAnchor_ = {*values, sampledAt, track.playing};
            timelineTrack_ = track;
            timelineRevision_ = seekRevision;
            forceTimeline_ = false;
        }
        if (sampledAt - progressLogAt_ >= 5.0) {
            smtc::LogEvent(smtc::LogLevel::Debug, "Progress",
                "Timeline activity over %.2f seconds; deck=%d mode=%s valid_queries=%u publication_attempts=%u identity_or_settings_changes=%u state_changes=%u rate_changes=%u duration_changes=%u drift_corrections=%u accepted_seeks=%u position_ms=%.3f duration_ms=%.3f playback_rate=%.6f state=%s; reason counts may overlap",
                sampledAt - progressLogAt_, track.deck, original ? "Original + Pitch" : "Adjusted",
                progressPolls_, progressPublications_, progressIdentityChanges_, progressStateChanges_,
                progressRateChanges_, progressDurationChanges_, progressDriftCorrections_, progressForcedSeeks_,
                values->position / 10000.0, values->duration / 10000.0, values->playbackRate,
                track.playing ? "Playing" : "Paused");
            progressLogAt_ = sampledAt;
            progressPolls_ = progressPublications_ = progressStateChanges_ = progressRateChanges_ = 0;
            progressDurationChanges_ = progressDriftCorrections_ = progressIdentityChanges_ = progressForcedSeeks_ = 0;
        }
        if (mailbox_) {
            std::lock_guard lock(mailbox_->mutex);
            mailbox_->seekTrack = track;
            if (!(sharingFlags_.load() & 2u)) {
                mailbox_->seekTrack.title.clear(); mailbox_->seekTrack.artist.clear(); mailbox_->seekTrack.album.clear();
            }
            mailbox_->seekRevision = seekRevision;
            mailbox_->seekDuration = !mailbox_->closed && seekable && progressEnabled_.load() &&
                seekingEnabled_.load() && seekRevision_.load() == seekRevision && settings_.load() == settings
                ? values->duration : 0;
        }
    }
    void Poll(const SystemMediaTransportControls& controls,
        const GlobalSystemMediaTransportControlsSessionManager& sessionManager,
        smtc::Track& previous, bool& published, ArtworkState& artwork) {
        const unsigned settings = settings_.load();
        const bool enabled = (settings & 1) != 0;
        auto track = enabled ? ReadTrack(settings, previous) : smtc::Track{};
        std::deque<smtc::Request> requests;
        {
            std::lock_guard lock(mailbox_->mutex);
            requests.swap(mailbox_->requests);
        }
        for (const auto& request : requests) {
            if (WaitForSingleObject(mailbox_->stop, 0) == WAIT_OBJECT_0) return;
            if (settings_.load() != settings) break;
            if (!IsCurrentWindowsSession(sessionManager, request)) continue;
            // 会话验证可能等待异步操作，返回后必须重新检查关闭和配置变化。
            if (WaitForSingleObject(mailbox_->stop, 0) == WAIT_OBJECT_0) return;
            if (settings_.load() != settings) break;
            if (request.action == smtc::Action::Seek) {
                if (!progressEnabled_.load() || !seekingEnabled_.load() ||
                    request.seekRevision != seekRevision_.load()) continue;
                // 查询实际 Deck，防止会话验证等待期间歌曲或左右映射已改变。
                track = ReadTrack(settings, previous);
                double reverse{}, duration{};
                const auto prefix = "deck " + std::to_string(track.deck) + " ";
                if (!Number(prefix + "reverse", reverse) || reverse != 0 ||
                    !Number(prefix + "get_time 'total'" + (originalTime_.load() ? " 'absolute'" : ""), duration) || !std::isfinite(duration) || duration <= 0 ||
                    settings_.load() != settings || !progressEnabled_.load() || !seekingEnabled_.load() ||
                    request.seekRevision != seekRevision_.load() ||
                    WaitForSingleObject(mailbox_->stop, 0) == WAIT_OBJECT_0) continue;
            }
            const auto command = smtc::CommandFor(request, track, enabled, true);
            if (!command.empty()) {
                const HRESULT result = SendCommand(command.c_str());
                if (result == S_OK && request.action == smtc::Action::Seek) forceTimeline_ = true;
                if (result == S_OK)
                    smtc::LogEvent(smtc::LogLevel::Info, "Control", "VirtualDJ accepted %s for Deck %d", ActionName(request.action), track.deck);
                else smtc::LogEvent(smtc::LogLevel::Warning, "Control", "VirtualDJ did not confirm %s for Deck %d; command=%s; result=0x%08lX", ActionName(request.action), track.deck, command.c_str(), static_cast<unsigned long>(result));
                track = ReadTrack(settings, previous);
            } else smtc::LogEvent(smtc::LogLevel::Warning, "Control", "Request discarded: unavailable deck, disabled session, or changed track");
        }

        // 进度查询和发布失败只清理时间轴，不能关闭已有媒体控制。
        try {
            UpdateProgress(controls, track);
            if (progressFailed_) { Log("Playback progress sharing recovered"); progressFailed_ = false; }
        } catch (...) {
            if (!progressFailed_) Log("Playback progress update failed", to_hresult());
            progressFailed_ = true;
            try { ClearTimeline(controls); } catch (...) {}
        }
        // 加载间隙也立即响应关闭共享，不能让旧信息等待宽限期后才消失。
        if (published && !(sharingFlags_.load() & 2u) && publishedInfo_) {
            auto updater = controls.DisplayUpdater();
            auto music = updater.MusicProperties();
            music.Title(L""); music.Artist(L""); music.AlbumTitle(L""); updater.Update();
            publishedInfo_ = false;
        }
        if (!(sharingFlags_.load() & 1u)) {
            try { UpdateArtwork(controls, track, false, artwork); }
            catch (...) { Log("Artwork clearing failed", to_hresult()); }
        }
        const bool available = enabled && track.loaded;
        // 加载间隙保留会话两秒，暂停接收命令，避免 Windows 跳到其他应用。
        if (!available && enabled && published) {
            const auto now = std::chrono::steady_clock::now();
            if (missingTrackSince_ == std::chrono::steady_clock::time_point{}) {
                missingTrackSince_ = now;
                Log("Track temporarily unavailable; keeping media session and waiting up to 2 seconds for loading");
                artwork.worker.Cancel();
                artwork.ticket = 0;
                artwork.pending = false;
            }
            if (now - missingTrackSince_ < std::chrono::seconds(2)) {
                std::lock_guard lock(mailbox_->mutex);
                mailbox_->accepting = false;
                mailbox_->requests.clear();
                return;
            }
        }
        if (available) missingTrackSince_ = {};
        if (available != published || track.empty != previous.empty) {
            // 空 Deck 保留播放能力；所有能力关闭可能让 Windows 隐藏会话。
            controls.IsPlayEnabled(true);
            controls.IsPauseEnabled(available || track.empty);
            controls.IsNextEnabled(available || track.empty);
            controls.IsPreviousEnabled(available || track.empty);
        }
        if (!enabled || !track.loaded) {
            if (published) Log("Track unavailable; showing idle placeholder; browser controls remain available when the deck is confirmed empty");
            if (published) controls.PlaybackStatus(MediaPlaybackStatus::Paused);
            if (published) {
                auto updater = controls.DisplayUpdater();
                updater.ClearAll();
                updater.Type(MediaPlaybackType::Music);
                updater.MusicProperties().Title(L"No track loaded");
                updater.Update();
            }
            published = false;
            if (artwork.ticket) {
                artwork.worker.Cancel(); artwork.ticket = 0;
            }
            artwork.pending = false;
        } else {
            const bool changed = !published || !track.SameIdentity(previous);
            const bool shareInfo = (sharingFlags_.load() & 2u) != 0;
            if (changed || shareInfo != publishedInfo_ || track.metadataRead != previous.metadataRead) {
                smtc::LogEvent(smtc::LogLevel::Info, "Session", "Track changed; deck=%d; publishing metadata and clearing previous artwork", track.deck);
                auto updater = controls.DisplayUpdater();
                if (changed) updater.ClearAll();
                updater.Type(MediaPlaybackType::Music);
                auto music = updater.MusicProperties();
                music.Title(shareInfo ? to_hstring(track.title) : hstring{});
                music.Artist(shareInfo ? to_hstring(track.artist) : hstring{});
                music.AlbumTitle(shareInfo ? to_hstring(track.album) : hstring{});
                updater.Update();
                publishedInfo_ = shareInfo;
            }
            if (!published || track.playing != previous.playing) {
                const auto status = track.playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused;
                if (controls.PlaybackStatus() != status) controls.PlaybackStatus(status);
                smtc::LogEvent(smtc::LogLevel::Info, "Session", "Playback state changed; deck=%d state=%s", track.deck, track.playing ? "Playing" : "Paused");
            }
            // 封面失败只影响图片，不得关闭播放控制。
            try { UpdateArtwork(controls, track, changed, artwork); }
            catch (...) {
                Log("Artwork publication failed", to_hresult());
                artwork.worker.Cancel();
                artwork.pending = false;
            }
            published = true;
        }
        previous = track;
        {
            std::lock_guard lock(mailbox_->mutex);
            mailbox_->displayed = track;
            if (!(sharingFlags_.load() & 2u) && track.loaded) {
                mailbox_->displayed.title.clear(); mailbox_->displayed.artist.clear(); mailbox_->displayed.album.clear();
            }
            mailbox_->accepting = !mailbox_->closed && enabled && (track.loaded || track.empty);
        }
    }

    void Run() noexcept {
        bool apartment = false;
        HWND window{};
        ATOM windowClass{};
        SystemMediaTransportControls controls{nullptr};
        GlobalSystemMediaTransportControlsSessionManager sessionManager{nullptr};
        event_token buttonToken{}, positionToken{};
        bool subscribed = false, positionSubscribed = false;
        try {
#ifdef SMTC_TESTING
            if (workerFault_) workerFault_();
#endif
            init_apartment(apartment_type::multi_threaded);
            apartment = true;
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = g_module;
            wc.lpszClassName = L"VirtualDJ.SMTC.Plugin";
            windowClass = RegisterClassW(&wc);
            if (!windowClass) throw_last_error();
            // SMTC 需要隐藏的顶层窗口；窗口及消息泵全由桥接线程管理。
            window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
                L"VirtualDJ SMTC", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, g_module, nullptr);
            if (!window) throw_last_error();
            auto interop = get_activation_factory<SystemMediaTransportControls, ISystemMediaTransportControlsInterop>();
            check_hresult(interop->GetForWindow(window, guid_of<SystemMediaTransportControls>(), put_abi(controls)));
            controls.IsEnabled(true);
            controls.PlaybackStatus(MediaPlaybackStatus::Paused);
            controls.DisplayUpdater().Type(MediaPlaybackType::Music);
            controls.DisplayUpdater().MusicProperties().Title(L"No track loaded");
            controls.DisplayUpdater().Update();
            controls.IsPlayEnabled(true);
            controls.IsPauseEnabled(false);
            controls.IsNextEnabled(false);
            controls.IsPreviousEnabled(false);
            controls.IsStopEnabled(false);
            controls.IsFastForwardEnabled(false);
            controls.IsRewindEnabled(false);
            try {
                const auto request = GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
                if (!WaitForAsync(request, mailbox_->stop, "Media session manager initialization")) {
                    if (WaitForSingleObject(mailbox_->stop, 0) == WAIT_OBJECT_0) throw_hresult(E_ABORT);
                    throw_hresult(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
                }
                sessionManager = request.GetResults();
                Log("Windows current-session validation enabled");
            } catch (...) {
                Log("Windows current-session validation is unavailable; media commands will use the legacy behavior", to_hresult());
            }
            buttonToken = controls.ButtonPressed([mailbox = mailbox_, sessionManager](auto const&, auto const& args) noexcept {
                try {
                    std::string source;
                    if (sessionManager) {
                        const auto current = sessionManager.GetCurrentSession();
                        if (current) source = to_string(current.SourceAppUserModelId());
                    }
                    if (args.Button() == SystemMediaTransportControlsButton::Play) mailbox->Push(smtc::Action::Play, source);
                    if (args.Button() == SystemMediaTransportControlsButton::Pause) mailbox->Push(smtc::Action::Pause, source);
                    if (args.Button() == SystemMediaTransportControlsButton::Next) mailbox->Push(smtc::Action::Next, source);
                    if (args.Button() == SystemMediaTransportControlsButton::Previous) mailbox->Push(smtc::Action::Previous, source);
                } catch (...) { Log("Media button handler failed", to_hresult()); }
            });
            subscribed = true;
            positionToken = controls.PlaybackPositionChangeRequested(
                [mailbox = mailbox_, sessionManager](auto const&, auto const& args) noexcept {
                    try {
                        std::string source;
                        if (sessionManager) {
                            const auto current = sessionManager.GetCurrentSession();
                            if (current) source = to_string(current.SourceAppUserModelId());
                        }
                        mailbox->PushSeek(args.RequestedPlaybackPosition().count(), std::move(source));
                    } catch (...) { Log("Playback position handler failed", to_hresult()); }
                });
            positionSubscribed = true;
            Log("SMTC initialized");
            smtc::Track previous;
            bool published = false;
            ArtworkState artwork([mailbox = mailbox_] { SetEvent(mailbox->wake); });
            HRESULT lastError = S_OK;
            const HANDLE events[] = {mailbox_->stop, mailbox_->wake};
            while (WaitForSingleObject(mailbox_->stop, 0) != WAIT_OBJECT_0) {
                try {
                    Poll(controls, sessionManager, previous, published, artwork);
                    if (FAILED(lastError)) Log("SMTC recovered");
                    lastError = S_OK;
                } catch (...) {
                    const auto error = to_hresult();
                    if (error != lastError) Log("SMTC update failed", error);
                    lastError = error;
                    published = false;
                    previous = {};
                    artwork.worker.Cancel();
                    artwork.ticket = 0;
                    artwork.pending = false;
                    { std::lock_guard lock(mailbox_->mutex); mailbox_->accepting = false; mailbox_->seekDuration = 0; mailbox_->requests.clear(); }
                    try {
                        controls.IsPlayEnabled(false);
                        controls.IsPauseEnabled(false);
                        controls.IsNextEnabled(false);
                        controls.IsPreviousEnabled(false);
                        controls.PlaybackStatus(MediaPlaybackStatus::Stopped);
                        controls.DisplayUpdater().ClearAll();
                        controls.DisplayUpdater().Update();
                    } catch (...) {}
                }
                const DWORD wait = MsgWaitForMultipleObjects(2, events, FALSE, (settings_.load() & 1) ? pollingMs_.load() : INFINITE, QS_ALLINPUT);
                if (wait == WAIT_OBJECT_0) break;
                if (wait == WAIT_FAILED) throw_last_error();
                MSG message{};
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
            }
        } catch (...) { Log("SMTC worker exited after an error; re-enable SMTC to restart", to_hresult()); }
        mailbox_->Close();
        if (controls) {
            if (subscribed) { try { controls.ButtonPressed(buttonToken); } catch (...) {} }
            if (positionSubscribed) { try { controls.PlaybackPositionChangeRequested(positionToken); } catch (...) {} }
            try {
                controls.PlaybackStatus(MediaPlaybackStatus::Closed);
                controls.IsEnabled(false);
                controls.DisplayUpdater().ClearAll();
                controls.DisplayUpdater().Update();
            } catch (...) {}
            controls = nullptr;
        }
        if (window) DestroyWindow(window);
        if (windowClass) UnregisterClassW(MAKEINTATOM(windowClass), g_module);
        // 在退出 COM apartment 之前释放该线程持有的 WinRT 会话管理器。
        sessionManager = nullptr;
        if (apartment) uninit_apartment();
        // 清理完所有线程资源后才允许宿主回收并重新创建线程。
        workerRunning_.store(false);
    }
};
}

HRESULT VDJ_API DllGetClassObject(const GUID& clsid, const GUID& iid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != CLSID_VdjPlugin8 || iid != IID_IVdjPluginBasic8) return CLASS_E_CLASSNOTAVAILABLE;
    try { *object = static_cast<IVdjPlugin8*>(new Plugin); return S_OK; }
    catch (...) { return E_OUTOFMEMORY; }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) g_module = instance;
    return TRUE;
}
