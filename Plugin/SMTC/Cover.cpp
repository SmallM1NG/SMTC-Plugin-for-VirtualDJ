#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <wincodec.h>
#include <winrt/base.h>
#include <taglib/fileref.h>
#include <taglib/tfilestream.h>
#include <taglib/tvariant.h>
#include "Cover.h"
#include "Logger.h"
#include <chrono>
#include <array>
#include <filesystem>
#include <memory>

namespace smtc {
namespace {
using Clock = std::chrono::steady_clock;
struct HttpCloser { void operator()(void* value) const { if (value) WinHttpCloseHandle(value); } };
using Http = std::unique_ptr<void, HttpCloser>;

bool IsImage(const ImageBytes& bytes) {
    // 按需读取首帧尺寸，限制压缩数据和像素数；不会主动解码整幅像素。
    if (bytes.empty() || bytes.size() > MaxArtworkBytes) return false;
    winrt::com_ptr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.put())))) return false;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put()))) return false;
    UINT width{}, height{};
    return SUCCEEDED(frame->GetSize(&width, &height)) && width > 0 && height > 0 &&
        width <= 16384 && height <= 16384 && static_cast<std::uint64_t>(width) * height <= 40000000;
}

std::wstring Attribute(IXmlReader* reader, const wchar_t* name) {
    if (reader->MoveToAttributeByName(name, nullptr) != S_OK) return {};
    const wchar_t* value{};
    UINT size{};
    std::wstring result;
    if (reader->GetValue(&value, &size) == S_OK && size <= 32768) result.assign(value, size);
    reader->MoveToElement();
    return result;
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

ImageBytes Resolve(const std::string& path, const std::string& home, const Cancelled& cancelled) {
    // 只有 netsearch:// 走主数据库；本地文件只读标签，不查库或联网。
    const std::wstring widePath(winrt::to_hstring(path));
    const bool online = IsNetSearchPath(widePath);
    if (!online) {
        auto bytes = ReadEmbeddedArtwork(widePath);
        if (cancelled()) return {};
        if (IsImage(bytes)) { LogEvent(LogLevel::Info, "Artwork", "Embedded tag image found; bytes=%zu", bytes.size()); return bytes; }
        LogEvent(LogLevel::Info, "Artwork", "No usable embedded cover in local file; database lookup skipped");
        return {}; // 本地曲目不查询数据库。
    }
    if (cancelled()) return {};
    if (home.empty()) { LogEvent(LogLevel::Warning, "Artwork", "Online cover lookup skipped: VirtualDJ data folder is unavailable"); return {}; }
    const auto database = std::filesystem::path(std::wstring(winrt::to_hstring(home))) / L"database.xml";
    const auto url = FindCoverLink(database.wstring(), widePath, cancelled);
    if (!url.empty() && !cancelled()) {
        LogEvent(LogLevel::Info, "Artwork", "Main database cover link found; downloading image");
        auto image = DownloadArtwork(url, cancelled);
        if (image.empty() && !cancelled()) LogEvent(LogLevel::Warning, "Artwork", "Online cover download failed or image was rejected; no automatic retry for this track");
        return image;
    }
    if (!cancelled()) LogEvent(LogLevel::Info, "Artwork", "No usable online cover link obtained from the main database; no automatic retry for this track");
    return {};
}
}

bool IsNetSearchPath(std::string_view path) noexcept {
    constexpr std::string_view prefix = "netsearch://";
    if (path.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        const char value = path[i];
        const char expected = prefix[i];
        if ((value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value) != expected) return false;
    }
    return true;
}

bool IsNetSearchPath(std::wstring_view path) noexcept {
    constexpr std::wstring_view prefix = L"netsearch://";
    return path.size() >= prefix.size() &&
        CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()), prefix.data(),
            static_cast<int>(prefix.size()), TRUE) == CSTR_EQUAL;
}

ImageBytes ReadEmbeddedArtwork(const std::wstring& path) {
    if (path.empty() || IsNetSearchPath(path)) return {};
    TagLib::FileStream stream(path.c_str(), true);
    if (!stream.isOpen()) return {};
    TagLib::FileRef file(&stream, false);
    if (file.isNull()) return {};
    ImageBytes fallback;
    for (const auto& picture : file.complexProperties("PICTURE")) {
        const auto data = picture.value("data").toByteVector();
        if (data.isEmpty() || data.size() > MaxArtworkBytes) continue;
        ImageBytes bytes(data.begin(), data.end());
        if (picture.value("pictureType").toString() == "Front Cover") return bytes;
        if (fallback.empty()) fallback = std::move(bytes);
    }
    return fallback;
}

std::wstring FindCoverLink(const std::wstring& database, const std::wstring& track,
    const Cancelled& cancelled) {
    // 流式只读 XML，禁止 DTD，限制深度、文件大小和扫描时间。
    winrt::com_ptr<IStream> input;
    if (FAILED(SHCreateStreamOnFileEx(database.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
        FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, input.put()))) return {};
    STATSTG stat{};
    if (FAILED(input->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart > 512ull * 1024 * 1024) return {};
    winrt::com_ptr<IXmlReader> reader;
    if (FAILED(CreateXmlReader(__uuidof(IXmlReader), reader.put_void(), nullptr))) return {};
    if (FAILED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)) ||
        FAILED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64)) ||
        FAILED(reader->SetInput(input.get()))) return {};
    const auto started = Clock::now();
    const std::wstring id = IsNetSearchPath(track) ? track.substr(12) : L"";
    bool inSong = false, selected = false;
    XmlNodeType node{};
    while (!cancelled() && Clock::now() - started < std::chrono::seconds(4) && reader->Read(&node) == S_OK) {
        const wchar_t* name{};
        if (FAILED(reader->GetLocalName(&name, nullptr))) continue;
        if (node == XmlNodeType_Element && wcscmp(name, L"Song") == 0) {
            const bool empty = reader->IsEmptyElement() != FALSE;
            selected = SamePath(Attribute(reader.get(), L"FilePath"), track);
            inSong = !empty;
        } else if (node == XmlNodeType_EndElement && wcscmp(name, L"Song") == 0) {
            inSong = false; selected = false;
        } else if (node == XmlNodeType_Element && inSong && wcscmp(name, L"Link") == 0) {
            const bool matches = selected || (!id.empty() && Attribute(reader.get(), L"NetSearch") == id);
            if (matches) return Attribute(reader.get(), L"Cover");
        }
    }
    return {};
}

ImageBytes DownloadArtwork(const std::wstring& url, const Cancelled& cancelled) {
    if (url.empty() || url.size() > 8192 || cancelled()) return {};
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength =
        parts.dwUserNameLength = parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) ||
        (parts.nScheme != INTERNET_SCHEME_HTTP && parts.nScheme != INTERNET_SCHEME_HTTPS) ||
        parts.dwUserNameLength || parts.dwPasswordLength) return {};
    Http session(WinHttpOpen(L"VirtualDJ-SMTC/0.1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    if (!session) return {};
    WinHttpSetTimeouts(session.get(), 2000, 2000, 2000, 2000);
    DWORD redirects = 4;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS, &redirects, sizeof(redirects));
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    Http connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection || cancelled()) return {};
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";
    const wchar_t* accept[] = {L"image/*", nullptr};
    Http request(WinHttpOpenRequest(connection.get(), L"GET", target.c_str(), nullptr,
        WINHTTP_NO_REFERER, accept, parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0));
    if (!request) return {};
    // 封面请求不继承浏览器登录信息、Cookie 或 Windows 自动认证。
    DWORD disabled = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled));
    DWORD auth = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
    WinHttpSetOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, &auth, sizeof(auth));
    struct AsyncState {
        std::atomic<unsigned> refs{1};
        HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::atomic<DWORD> status{0}, length{0};
        std::array<std::uint8_t, 16384> buffer{};
        void Release() { if (refs.fetch_sub(1) == 1) delete this; }
        ~AsyncState() { if (done) CloseHandle(done); }
        static void CALLBACK Callback(HINTERNET, DWORD_PTR context, DWORD event, void*, DWORD bytes) {
            auto* state = reinterpret_cast<AsyncState*>(context);
            if (!state) return;
            if (event == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { state->Release(); return; }
            if (event == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || event == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE ||
                event == WINHTTP_CALLBACK_STATUS_READ_COMPLETE || event == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) {
                state->length.store(bytes); state->status.store(event); SetEvent(state->done);
            }
        }
    };
    auto* state = new AsyncState;
    struct StateOwner { AsyncState* state; ~StateOwner() { state->Release(); } } owner{state};
    if (!state->done) return {};
    DWORD_PTR context = reinterpret_cast<DWORD_PTR>(state);
    if (!WinHttpSetOption(request.get(), WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return {};
    if (WinHttpSetStatusCallback(request.get(), AsyncState::Callback,
        WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK) return {};
    // 取消请求后仍可能回调，直到 HANDLE_CLOSING 才释放句柄持有的上下文引用。
    state->refs.fetch_add(1);
    Http active(request.release());
    const auto started = Clock::now();
    const auto wait = [&](DWORD expected) {
        // 整个下载共享八秒期限，每 25 ms 检查取消，避免旧请求拖延切歌。
        while (!cancelled() && Clock::now() - started < std::chrono::seconds(8)) {
            const DWORD result = WaitForSingleObject(state->done, 25);
            if (result == WAIT_OBJECT_0) return state->status.load() == expected && !cancelled();
            if (result != WAIT_TIMEOUT) break;
        }
        return false;
    };
    if (!WinHttpSendRequest(active.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, context) || !wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE)) return {};
    if (!WinHttpReceiveResponse(active.get(), nullptr) || !wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE)) return {};
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(active.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200) return {};
    ImageBytes bytes;
    for (;;) {
        if (cancelled() || !WinHttpReadData(active.get(), state->buffer.data(), static_cast<DWORD>(state->buffer.size()), nullptr) ||
            !wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE)) return {};
        const DWORD read = state->length.load();
        if (!read) return IsImage(bytes) ? bytes : ImageBytes{};
        if (bytes.size() + read > MaxArtworkBytes) return {};
        bytes.insert(bytes.end(), state->buffer.begin(), state->buffer.begin() + read);
    }
}

ArtworkWorker::ArtworkWorker(std::function<void()> ready) : ready_(std::move(ready)), thread_([this] { Run(); }) {}
ArtworkWorker::~ArtworkWorker() {
    stopped_.store(true);
    Cancel();
    wake_.notify_one();
    if (thread_.joinable()) thread_.join();
}
std::uint64_t ArtworkWorker::Request(std::string path, std::string home) {
    // 已退出的线程仍有可回收句柄；仅在新任务到来时重建，不循环重试失败曲目。
    std::lock_guard lifecycle(lifecycleMutex_);
    if (!running_.load()) {
        if (thread_.joinable()) thread_.join();
        LogEvent(LogLevel::Info, "Artwork", "New artwork request; restarting exited worker");
        running_.store(true);
        try { thread_ = std::thread([this] { Run(); }); }
        catch (...) { running_.store(false); throw; }
    }
    std::lock_guard lock(mutex_);
    const auto ticket = ++generation_;
    pending_ = Job{ticket, std::move(path), std::move(home)};
    result_.reset();
    wake_.notify_one();
    return ticket;
}
void ArtworkWorker::Cancel() {
    std::lock_guard lock(mutex_);
    ++generation_;
    pending_.reset(); result_.reset();
}
std::optional<ArtworkWorker::Result> ArtworkWorker::Take() {
    std::lock_guard lock(mutex_);
    auto result = std::move(result_);
    result_.reset();
    return result;
}
void ArtworkWorker::Run() noexcept {
    bool apartment = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartment = true;
        while (!stopped_.load()) {
            Job job;
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stopped_.load() || pending_.has_value(); });
                if (stopped_.load()) break;
                job = std::move(*pending_); pending_.reset();
            }
            const auto cancelled = [&] { return stopped_.load() || generation_.load() != job.ticket; };
            LogEvent(LogLevel::Debug, "Artwork", "Artwork lookup started; request=%llu", static_cast<unsigned long long>(job.ticket));
            ImageBytes image;
            try { image = Resolve(job.path, job.home, cancelled); }
            catch (...) { LogEvent(LogLevel::Error, "Artwork", "Lookup exception; HRESULT=0x%08lX", static_cast<unsigned long>(winrt::to_hresult())); }
            bool notify = false;
            {
                std::lock_guard lock(mutex_);
                if (!cancelled()) { result_ = Result{job.ticket, std::move(image)}; notify = true; }
                else LogEvent(LogLevel::Debug, "Artwork", "Artwork result ignored because the track changed or SMTC stopped; request=%llu", static_cast<unsigned long long>(job.ticket));
            }
            if (notify && ready_) ready_();
        }
    } catch (...) { LogEvent(LogLevel::Error, "Artwork", "Worker failed; HRESULT=0x%08lX", static_cast<unsigned long>(winrt::to_hresult())); }
    if (apartment) winrt::uninit_apartment();
    running_.store(false);
}
}
