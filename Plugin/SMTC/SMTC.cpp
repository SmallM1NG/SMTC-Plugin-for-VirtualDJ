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
constexpr char kVersion[] = "0.1.0";
const std::string kVersionLabel = std::string("Version ") + kVersion;
const std::string kReleaseUrl =
    std::string("https://github.com/SmallM1NG/SMTC-Plugin-for-VirtualDJ/releases/tag/v") + kVersion;
constexpr char kSourceLabel[] = "Source Deck";
constexpr char kDeveloperLabel[] = "Developed by 小小小小铭 (aka DJM1NG)";
constexpr char kAuthor[] = "小小小小铭 (aka DJM1NG)";
constexpr char kDescription[] = "Share track information, artwork and playback controls with Windows";

void Log(const char* message, HRESULT hr = S_OK) noexcept {
    if (hr == S_OK) smtc::LogEvent(smtc::LogLevel::Info, "Bridge", "%s", message);
    else smtc::LogEvent(FAILED(hr) ? smtc::LogLevel::Error : smtc::LogLevel::Warning,
        "Bridge", "%s; Windows error code=0x%08lX", message, static_cast<unsigned long>(hr));
}
const char* ActionName(smtc::Action action) noexcept {
    switch (action) {
    case smtc::Action::Play: return "Play";
    case smtc::Action::Pause: return "Pause";
    case smtc::Action::Next: return "Next track";
    case smtc::Action::Previous: return "Previous track";
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
    void Close() {
        std::lock_guard lock(mutex);
        closed = true;
        accepting = false;
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
            check_hresult(DeclareParameterSlider(&deck_, 5, kSourceLabel, kSourceLabel, 1.f));
            check_hresult(DeclareParameterSlider(&polling_, 10, "Polling Interval", "Polling", 0.f));
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
            PublishSettings();
            mailbox_ = std::make_shared<Mailbox>();
            if (!mailbox_->stop || !mailbox_->wake) throw_last_error();

            // 撤销订阅后仍可能有迟到回调；固定 DLL，并只捕获共享邮箱。
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
            polling_ = std::isfinite(polling_) ? std::clamp(polling_, 0.f, 1.f) : 0.f;
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
        if (id == 11) { _snprintf_s(text, size, _TRUNCATE, "%s", kVersionLabel.c_str()); return S_OK; }
        if (id != 5) return E_NOTIMPL;
        const int deck = smtc::DeckFromSlider(deck_);
        _snprintf_s(text, size, _TRUNCATE, "%s", SourceName(deck));
        return S_OK;
    }

private:
    int enabled_ = 1;
    int developerButton_ = 0, versionButton_ = 0;
    bool developerHeld_ = false, versionHeld_ = false;

    float deck_ = 1.f, polling_ = 0.f;
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
        const unsigned interval = smtc::PollingMilliseconds(polling_);
        const bool intervalChanged = pollingMs_.exchange(interval) != interval;
        return (previous & 0xffffu) != selection || intervalChanged;
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
        if (track.SameIdentity(previous)) {
            track.title = previous.title; track.artist = previous.artist; track.album = previous.album;
        } else {
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
    };

    void UpdateArtwork(const SystemMediaTransportControls& controls, const smtc::Track& track,
        bool changed, ArtworkState& artwork) {
        // 每次换歌只发起一次封面任务，票号阻止旧结果覆盖新曲目。
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
        DataWriter writer(stream);
        writer.WriteBytes(result->image);
        writer.StoreAsync().get();
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
            const auto properties = current.TryGetMediaPropertiesAsync().get();
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
            const auto command = smtc::CommandFor(request, track, enabled, true);
            if (!command.empty()) {
                const HRESULT result = SendCommand(command.c_str());
                if (result == S_OK)
                    smtc::LogEvent(smtc::LogLevel::Info, "Control", "VirtualDJ accepted %s for Deck %d", ActionName(request.action), track.deck);
                else smtc::LogEvent(smtc::LogLevel::Warning, "Control", "VirtualDJ did not confirm %s for Deck %d; command=%s; result=0x%08lX", ActionName(request.action), track.deck, command.c_str(), static_cast<unsigned long>(result));
                track = ReadTrack(settings, previous);
            } else smtc::LogEvent(smtc::LogLevel::Warning, "Control", "Request discarded: unavailable deck, disabled session, or changed track");
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
            if (changed) {
                smtc::LogEvent(smtc::LogLevel::Info, "Session", "Track changed; deck=%d; publishing metadata and clearing previous artwork", track.deck);
                auto updater = controls.DisplayUpdater();
                updater.ClearAll();
                updater.Type(MediaPlaybackType::Music);
                auto music = updater.MusicProperties();
                music.Title(to_hstring(track.title));
                music.Artist(to_hstring(track.artist));
                music.AlbumTitle(to_hstring(track.album));
                updater.Update();
            }
            if (!published || track.playing != previous.playing) {
                controls.PlaybackStatus(track.playing ? MediaPlaybackStatus::Playing : MediaPlaybackStatus::Paused);
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
            mailbox_->accepting = !mailbox_->closed && enabled && (track.loaded || track.empty);
        }
    }

    void Run() noexcept {
        bool apartment = false;
        HWND window{};
        ATOM windowClass{};
        SystemMediaTransportControls controls{nullptr};
        GlobalSystemMediaTransportControlsSessionManager sessionManager{nullptr};
        event_token buttonToken{};
        bool subscribed = false;
        try {
#ifdef SMTC_TESTING
            if (workerFault_) workerFault_();
#endif
            init_apartment(apartment_type::multi_threaded);
            apartment = true;
            WNDCLASSW wc{};
            wc.lpfnWndProc = DefWindowProcW;
            wc.hInstance = g_module;
            wc.lpszClassName = L"VirtualDJ.SMTC.Bridge.0.1";
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
                sessionManager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
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
                    { std::lock_guard lock(mailbox_->mutex); mailbox_->accepting = false; mailbox_->requests.clear(); }
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
