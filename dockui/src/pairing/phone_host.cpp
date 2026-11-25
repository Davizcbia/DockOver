#include "phone_host.h"

#include <objbase.h>
#include <ocidl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <WebView2.h>
#include <wrl.h>

#include <string>
#include <utility>

namespace docky::pairing {
namespace {
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr wchar_t kHost[] = L"https://dockdrop.invalid/";
constexpr wchar_t kFilter[] = L"https://dockdrop.invalid/*";

struct Resource {
    const wchar_t* path;
    int id;
    const wchar_t* headers;
};
constexpr Resource kResources[] = {
    {L"", 201, L"Content-Type: text/html; charset=utf-8\r\nCache-Control: no-store"},
    {L"pair.js", 202, L"Content-Type: text/javascript; charset=utf-8"},
    {L"config.js", 203, L"Content-Type: text/javascript; charset=utf-8"},
    {L"bubble-code.js", 204, L"Content-Type: text/javascript; charset=utf-8"},
    {L"qr.js", 205, L"Content-Type: text/javascript; charset=utf-8"},
    {L"transfer.js", 206, L"Content-Type: text/javascript; charset=utf-8"},
};

std::wstring TakeString(LPWSTR value) {
    std::wstring result = value ? value : L"";
    CoTaskMemFree(value);
    return result;
}
}  // namespace

class PhoneHost : public std::enable_shared_from_this<PhoneHost> {
public:
    explicit PhoneHost(PhoneMessage receive) : receive_(std::move(receive)) {}
    ~PhoneHost() { if (controller_) controller_->Close(); }

    void Reply(const wchar_t* message) {
        if (web_) web_->PostWebMessageAsString(message);
    }

    void Start(HWND parent) {
        LPWSTR localAppData = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
            receive_(L"error");
            return;
        }
#ifdef DOCKDROP_STANDALONE
        const auto data = TakeString(localAppData) + L"\\DockDrop\\WebView";
#else
        const auto data = TakeString(localAppData) + L"\\DockOver\\PhonePairing";
#endif
        const auto weak = weak_from_this();
        auto ready = Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [weak, parent](HRESULT result, ICoreWebView2Environment* environment) -> HRESULT {
                if (auto self = weak.lock()) {
                    if (FAILED(result) || !environment) self->receive_(L"runtime");
                    else self->CreateController(parent, environment);
                }
                return S_OK;
            });
        if (FAILED(CreateCoreWebView2EnvironmentWithOptions(nullptr, data.c_str(), nullptr, ready.Get()))) {
            receive_(L"runtime");
        }
    }

private:
    void CreateController(HWND parent, ICoreWebView2Environment* environment) {
        environment_ = environment;
        const auto weak = weak_from_this();
        auto ready = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [weak](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                if (auto self = weak.lock()) {
                    if (FAILED(result) || !controller || !self->Configure(controller)) self->receive_(L"error");
                } else if (controller) {
                    controller->Close();
                }
                return S_OK;
            });
        if (FAILED(environment->CreateCoreWebView2Controller(parent, ready.Get()))) receive_(L"error");
    }

    bool Configure(ICoreWebView2Controller* controller) {
        controller_ = controller;
        controller->put_IsVisible(FALSE);
        ComPtr<ICoreWebView2> web;
        if (FAILED(controller->get_CoreWebView2(&web))) return false;
        web_ = web;
        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(web->get_Settings(&settings))) {
            settings->put_AreDevToolsEnabled(FALSE);
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreHostObjectsAllowed(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
        }

        EventRegistrationToken token{};
        if (FAILED(web->AddWebResourceRequestedFilter(kFilter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL))) return false;
        const auto weak = weak_from_this();
        auto resources = Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                if (auto self = weak.lock()) self->Respond(args);
                return S_OK;
            });
        if (FAILED(web->add_WebResourceRequested(resources.Get(), &token))) return false;
        auto navigation = Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (SUCCEEDED(args->get_Uri(&raw)) && TakeString(raw) == kHost) return S_OK;
                return args->put_Cancel(TRUE);
            });
        if (FAILED(web->add_NavigationStarting(navigation.Get(), &token))) return false;
        auto messages = Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR source = nullptr;
                if (FAILED(args->get_Source(&source)) || TakeString(source) != kHost) return S_OK;
                if (auto self = weak.lock()) {
                    LPWSTR raw = nullptr;
                    if (SUCCEEDED(args->TryGetWebMessageAsString(&raw))) self->receive_(TakeString(raw));
                }
                return S_OK;
            });
        if (FAILED(web->add_WebMessageReceived(messages.Get(), &token))) return false;
        auto failed = Callback<ICoreWebView2ProcessFailedEventHandler>(
            [weak](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*) -> HRESULT {
                if (auto self = weak.lock()) self->receive_(L"error");
                return S_OK;
            });
        if (FAILED(web->add_ProcessFailed(failed.Get(), &token))) return false;
        return SUCCEEDED(web->Navigate(kHost));
    }

    void Respond(ICoreWebView2WebResourceRequestedEventArgs* args) {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        LPWSTR raw = nullptr;
        if (FAILED(args->get_Request(&request)) || FAILED(request->get_Uri(&raw))) return;
        const auto uri = TakeString(raw);
        ComPtr<ICoreWebView2WebResourceResponse> response;
        for (const auto& entry : kResources) {
            if (uri != std::wstring(kHost) + entry.path) continue;
            const HMODULE module = GetModuleHandleW(nullptr);
            const HRSRC handle = FindResourceW(module, MAKEINTRESOURCEW(entry.id), RT_RCDATA);
            if (!handle) break;
            const HGLOBAL resource = LoadResource(module, handle);
            const auto* data = static_cast<const BYTE*>(LockResource(resource));
            if (!data) break;
            ComPtr<IStream> stream;
            stream.Attach(SHCreateMemStream(data, SizeofResource(module, handle)));
            if (stream) environment_->CreateWebResourceResponse(stream.Get(), 200, L"OK", entry.headers, &response);
            break;
        }
        if (!response) environment_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
        if (response) args->put_Response(response.Get());
    }

    PhoneMessage receive_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> web_;
};

std::shared_ptr<PhoneHost> StartPhoneHost(HWND parent, PhoneMessage receive) {
    auto host = std::make_shared<PhoneHost>(std::move(receive));
    host->Start(parent);
    return host;
}
void ReplyToPhoneHost(const std::shared_ptr<PhoneHost>& host, const wchar_t* reply) {
    if (host) host->Reply(reply);
}
}  // namespace docky::pairing
