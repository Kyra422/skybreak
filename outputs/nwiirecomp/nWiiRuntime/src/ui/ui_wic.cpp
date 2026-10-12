// UI13: WIC JPEG decode / PNG encode (see ui_wic.h).
#include "ui_wic.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <cstdio>
#include <filesystem>
#include <utility>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace nwii::ui {

namespace {
struct ComInit {
    bool uninit = false;
    ComInit() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        uninit = SUCCEEDED(hr); // S_OK / S_FALSE; RPC_E_CHANGED_MODE = already initialised otherwise
    }
    ~ComInit() {
        if (uninit) CoUninitialize();
    }
};
template <class T> struct Ptr {
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};
bool fail(std::string* err, const char* what, HRESULT hr) {
    if (err) {
        char b[96];
        std::snprintf(b, sizeof b, "%s failed (0x%08lX)", what, (unsigned long)hr);
        *err = b;
    }
    return false;
}
} // namespace

bool DecodeImageWic(const std::vector<uint8_t>& encoded, Image& out, std::string* err) {
    ComInit com;
    Ptr<IWICImagingFactory> f;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.p));
    if (FAILED(hr)) return fail(err, "WIC factory", hr);
    Ptr<IStream> stream;
    stream.p = SHCreateMemStream(encoded.data(), UINT(encoded.size()));
    if (!stream) return fail(err, "SHCreateMemStream", E_OUTOFMEMORY);
    Ptr<IWICBitmapDecoder> dec;
    hr = f->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) return fail(err, "WIC decoder", hr);
    Ptr<IWICBitmapFrameDecode> frame;
    hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) return fail(err, "WIC frame", hr);
    Ptr<IWICFormatConverter> conv;
    hr = f->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return fail(err, "WIC convert", hr);
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    out.w = int(w);
    out.h = int(h);
    out.rgba.resize(size_t(w) * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4, UINT(out.rgba.size()), out.rgba.data());
    if (FAILED(hr)) return fail(err, "WIC copy", hr);
    return true;
}

bool SavePngWic(const std::string& path_utf8, const Image& img, std::string* err) {
    if (img.w <= 0 || img.h <= 0 || img.rgba.size() < size_t(img.w) * img.h * 4) return fail(err, "empty image", E_INVALIDARG);
    ComInit com;
    Ptr<IWICImagingFactory> f;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.p));
    if (FAILED(hr)) return fail(err, "WIC factory", hr);
    const std::wstring wpath = std::filesystem::path(std::u8string(path_utf8.begin(), path_utf8.end())).wstring();
    Ptr<IWICStream> stream;
    hr = f->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(wpath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return fail(err, "WIC file stream", hr);
    Ptr<IWICBitmapEncoder> enc;
    hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream.p, WICBitmapEncoderNoCache);
    if (FAILED(hr)) return fail(err, "WIC PNG encoder", hr);
    Ptr<IWICBitmapFrameEncode> frame;
    hr = enc->CreateNewFrame(&frame, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(img.w), UINT(img.h));
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppRGBA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr)) return fail(err, "WIC PNG frame", hr);
    std::vector<uint8_t> px = img.rgba;
    if (fmt != GUID_WICPixelFormat32bppRGBA) { // encoder picked BGRA
        for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
    }
    hr = frame->WritePixels(UINT(img.h), UINT(img.w) * 4, UINT(px.size()), px.data());
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (FAILED(hr)) return fail(err, "WIC PNG write", hr);
    return true;
}

} // namespace nwii::ui

#else
namespace nwii::ui {
bool DecodeImageWic(const std::vector<uint8_t>&, Image&, std::string* err) {
    if (err) *err = "WIC is Windows-only";
    return false;
}
bool SavePngWic(const std::string&, const Image&, std::string* err) {
    if (err) *err = "WIC is Windows-only";
    return false;
}
} // namespace nwii::ui
#endif
