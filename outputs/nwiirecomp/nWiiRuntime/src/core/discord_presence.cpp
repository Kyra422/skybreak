#include "runtime/discord_presence.h"
#include "json/json.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <iostream>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace nwii::runtime::discord {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
static constexpr size_t max_payload = 65536;
static uint32_t U32(const uint8_t* b) {
    return uint32_t(b[0]) | uint32_t(b[1])<<8 | uint32_t(b[2])<<16 | uint32_t(b[3])<<24;
}
std::string AreaName(const char (&stage)[9]) {
    const auto end = std::find(stage, stage+8, '\0');
    const std::string s(stage, end);
    // Exact, conservative mappings. Unknown/interior stages never expose internal codes.
    static const std::pair<const char*, const char*> areas[] = {
        {"F000", "Skyloft"}, {"D100", "Skyview Temple"},
        {"F100", "Faron Woods"}, {"F200", "Eldin Volcano"},
        {"F300", "Lanayru Desert"}
    };
    for (const auto& a : areas) if (s == a.first) return a.second;
    return {};
}
Activity MakeActivity(bool launcher, bool port_menu, bool show_area,
                      const progress::Snapshot& scene, int64_t start) {
    Activity a; a.start = start; a.state = "Early development build";
    if (launcher) a.details = "In the launcher";
    else if (!scene.playing) a.details = "Title screen / loading";
    else {
        const std::string area = show_area ? AreaName(scene.stage) : std::string();
        a.details = area.empty() ? "Playing Skyward Sword" : "Exploring " + area;
        if (port_menu) a.state = "In the port menu";
    }
    return a;
}
std::string ActivityJson(const Activity& a, uint32_t pid, uint64_t nonce, const std::string& image) {
    Json activity = {{"type", 0}, {"details", a.details}, {"state", a.state},
                     {"timestamps", {{"start", a.start}}}, {"instance", false}};
    if (!image.empty() && image.size() <= 128 && std::all_of(image.begin(),image.end(),[](unsigned char c){
        return (c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-';
    }))
        activity["assets"] = {{"large_image", image}, {"large_text", "SkyBreak"}};
    activity["buttons"] = Json::array({{{"label", "Community"}, {"url", "https://discord.gg/kXTqgNrd2"}}});
    return Json{{"cmd", "SET_ACTIVITY"}, {"args", {{"pid", pid}, {"activity", activity}}},
                {"nonce", std::to_string(nonce)}}.dump();
}
std::vector<uint8_t> EncodeFrame(uint32_t op, const std::string& json) {
    if (json.size() > max_payload) return {};
    std::vector<uint8_t> b(8+json.size());
    for (int i=0; i<4; ++i) { b[i]=uint8_t(op>>(8*i)); b[4+i]=uint8_t(json.size()>>(8*i)); }
    std::copy(json.begin(), json.end(), b.begin()+8); return b;
}
bool Decoder::Append(const uint8_t* data, size_t size) {
    if (size > 2*(max_payload+8) || bytes_.size()+size > 2*(max_payload+8)) return false;
    bytes_.insert(bytes_.end(), data, data+size);
    for (size_t pos=0; bytes_.size()-pos>=8;) {
        const size_t length=U32(bytes_.data()+pos+4);
        if (U32(bytes_.data()+pos)>4 || length>max_payload) return false;
        if (bytes_.size()-pos<8+length) break;
        pos+=8+length;
    }
    return true;
}
bool Decoder::Pop(uint32_t& op, std::string& payload) {
    if (bytes_.size()<8) return false;
    const size_t n=U32(bytes_.data()+4);
    if (n>max_payload || bytes_.size()<n+8) return false;
    op=U32(bytes_.data());
    payload.assign(reinterpret_cast<const char*>(bytes_.data()+8), n);
    bytes_.erase(bytes_.begin(), bytes_.begin()+8+n); return true;
}

#ifdef _WIN32
namespace {
class Pipe {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
public:
    ~Pipe() { Close(); }
    void Close() { if (handle_!=INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_=INVALID_HANDLE_VALUE; } }
    bool Open() {
        const char* test=std::getenv("NWII_DISCORD_TEST");
        const char* name=std::getenv("NWII_DISCORD_TEST_PIPE");
        if (test && test[0]=='1' && name && std::string(name).starts_with("skybreak-rpc-test-")) {
            const std::string path="\\\\.\\pipe\\"+std::string(name);
            handle_=CreateFileA(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
            return handle_!=INVALID_HANDLE_VALUE;
        }
        for (int i=0; i<10; ++i) {
            const std::wstring name=L"\\\\.\\pipe\\discord-ipc-"+std::to_wstring(i);
            handle_=CreateFileW(name.c_str(), GENERIC_READ|GENERIC_WRITE, 0, nullptr,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle_!=INVALID_HANDLE_VALUE) return true;
        }
        return false;
    }
    bool Write(uint32_t op, const std::string& text) {
        auto bytes=EncodeFrame(op,text); if (bytes.empty()) return false;
        OVERLAPPED ov{}; ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!ov.hEvent) return false;
        DWORD done=0;
        bool ok=WriteFile(handle_,bytes.data(),DWORD(bytes.size()),&done,&ov)!=FALSE;
        if (!ok && GetLastError()==ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent,500)==WAIT_OBJECT_0)
                ok=GetOverlappedResult(handle_,&ov,&done,FALSE)!=FALSE;
            else { CancelIoEx(handle_,&ov); GetOverlappedResult(handle_,&ov,&done,TRUE); }
        }
        CloseHandle(ov.hEvent); return ok && done==bytes.size();
    }
    bool Read(Decoder& decoder) {
        DWORD available=0;
        if (!PeekNamedPipe(handle_,nullptr,0,nullptr,&available,nullptr)) return false;
        if (!available) return true;
        uint8_t bytes[4096]; OVERLAPPED ov{};
        ov.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr); if (!ov.hEvent) return false;
        DWORD done=0;
        bool ok=ReadFile(handle_,bytes,std::min<DWORD>(available,sizeof(bytes)),&done,&ov)!=FALSE;
        if (!ok && GetLastError()==ERROR_IO_PENDING) {
            if (WaitForSingleObject(ov.hEvent,500)==WAIT_OBJECT_0)
                ok=GetOverlappedResult(handle_,&ov,&done,FALSE)!=FALSE;
            else { CancelIoEx(handle_,&ov); GetOverlappedResult(handle_,&ov,&done,TRUE); }
        }
        CloseHandle(ov.hEvent); return ok && done && decoder.Append(bytes,done);
    }
};
struct Request { bool enabled=false; Activity activity; std::string image; };
class Client {
    std::mutex mutex_; std::condition_variable cv_;
    Request latest_; bool stop_=false; std::thread thread_;
    int64_t start_=int64_t(std::time(nullptr));
    void Run() {
        Pipe pipe; Decoder decoder;
        bool open=false, ready=false, pending=false;
        uint64_t nonce=0;
        std::string awaiting, sent;
        auto retry=Clock::now(), connected=retry, next_send=retry, deadline=retry;
        const auto disconnect=[&] {
            pipe.Close(); decoder=Decoder(); open=ready=pending=false; sent.clear();
            retry=Clock::now()+std::chrono::seconds(5);
        };
        for (;;) {
            Request req; bool stop;
            { std::unique_lock lock(mutex_); cv_.wait_for(lock,std::chrono::milliseconds(100)); req=latest_; stop=stop_; }
            if (stop || !req.enabled) {
                if (open && ready) {
                    const auto clear=Json{{"cmd","SET_ACTIVITY"},{"args",{{"pid",GetCurrentProcessId()},{"activity",nullptr}}},
                                          {"nonce",std::to_string(++nonce)}}.dump();
                    pipe.Write(1,clear);
                }
                if (open) disconnect();
                if (stop) break;
                continue;
            }
            auto now=Clock::now();
            if (!open && now>=retry) {
                if (!pipe.Open()) { retry=now+std::chrono::seconds(5); continue; }
                open=true; connected=now;
                if (!pipe.Write(0,Json{{"v",1},{"client_id",application_id}}.dump())) { disconnect(); continue; }
            }
            if (!open) continue;
            if (!pipe.Read(decoder)) { disconnect(); continue; }
            uint32_t op; std::string payload;
            while (decoder.Pop(op,payload)) {
                if (op==2 || op>4) { disconnect(); break; }
                if (op==3) { if (!pipe.Write(4,payload)) { disconnect(); break; } continue; }
                if (op!=1) continue;
                const auto reply=Json::parse(payload,nullptr,false);
                if (reply.is_discarded() || !reply.is_object()) { disconnect(); break; }
                const auto evt=reply.find("evt");
                if (evt!=reply.end() && *evt=="ERROR") {
                    // No READY user object, tokens or personal Discord data are retained/logged.
                    std::cout << "[DiscordPresence] Discord rejected the activity; retrying later.\n";
                    disconnect(); retry=Clock::now()+std::chrono::seconds(30); break;
                }
                if (evt!=reply.end() && *evt=="READY") {
                    ready=true; next_send=now;
                    std::cout << "[DiscordPresence] Connected.\n";
                }
                const auto rn=reply.find("nonce");
                if (pending && rn!=reply.end() && *rn==awaiting) {
                    pending=false;
                    std::cout << "[DiscordPresence] Activity accepted: " << sent.substr(0,sent.find('\n')) << '\n';
                }
            }
            if (!open) continue;
            now=Clock::now();
            if ((!ready && now-connected>std::chrono::seconds(5)) || (pending && now>deadline)) {
                disconnect(); continue;
            }
            // Coalesce scene/menu changes: one outstanding command and max one update per 15s.
            const auto signature=req.activity.details+'\n'+req.activity.state+'\n'+req.image;
            if (ready && !pending && now>=next_send && signature!=sent) {
                awaiting=std::to_string(++nonce);
                if (!pipe.Write(1,ActivityJson(req.activity,GetCurrentProcessId(),nonce,req.image))) { disconnect(); continue; }
                pending=true; deadline=now+std::chrono::seconds(5);
                next_send=now+std::chrono::seconds(15); sent=signature;
            }
        }
    }
public:
    ~Client() { Stop(); }
    void Submit(bool enabled, bool launcher, bool port_menu, bool show_area, const progress::Snapshot& scene, const std::string& image) {
        std::lock_guard lock(mutex_);
        latest_={enabled,MakeActivity(launcher,port_menu,show_area,scene,start_),image};
        if (enabled && !thread_.joinable()) { stop_=false; thread_=std::thread([this]{Run();}); }
        cv_.notify_one();
    }
    void Stop() {
        { std::lock_guard lock(mutex_); stop_=true; cv_.notify_one(); }
        if (thread_.joinable()) thread_.join();
    }
};
Client client;
}
void Update(bool enabled, bool launcher, bool port_menu, bool show_area, const progress::Snapshot& scene, const std::string& image) {
    client.Submit(enabled,launcher,port_menu,show_area,scene,image);
}
void Shutdown() { client.Stop(); }
#else
void Update(bool,bool,bool,bool,const progress::Snapshot&,const std::string&) {}
void Shutdown() {}
#endif
}
