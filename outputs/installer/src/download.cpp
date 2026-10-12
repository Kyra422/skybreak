// INSTALL19: WinHTTP download + Authenticode check (see download.h).
#include "download.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <winhttp.h>

#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace sb {

bool HttpDownload(const std::string& url, const fs::path& dest, const ProgressFn& progress, std::atomic<bool>* cancel,
                  std::string* err) {
    auto fail = [&](const std::string& m) { if (err) *err = m; Log("[download] %s: %s", url.c_str(), m.c_str()); return false; };
    std::wstring wurl = W(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof uc;
    wchar_t host[256] = {}, path[2048] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return fail("ongeldige URL");
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) return fail("alleen https is toegestaan");

    fs::path part = dest;
    part += L".deel";
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    uint64_t have = fs::exists(part, ec) ? fs::file_size(part, ec) : 0;

    HINTERNET s = WinHttpOpen(L"SkyBreak-Setup/0.19", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return fail("WinHttpOpen mislukt");
    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(s, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof redir);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    bool ok = false;
    if (r) {
        std::wstring hdr;
        if (have) hdr = L"Range: bytes=" + std::to_wstring(have) + L"-\r\n";
        if (WinHttpSendRequest(r, hdr.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdr.c_str(), DWORD(-1L), WINHTTP_NO_REQUEST_DATA,
                               0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, len = sizeof status;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
            if (status == 200) have = 0; // server ignored the range: start over
            if (status == 200 || status == 206) {
                wchar_t clen[64] = {};
                DWORD cl = sizeof clen;
                uint64_t total = 0;
                if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, clen, &cl, nullptr)) total = _wtoi64(clen) + have;
                std::ofstream f(part, std::ios::binary | (have ? std::ios::app : std::ios::trunc));
                std::vector<char> buf(1 << 16);
                uint64_t done = have;
                ok = bool(f);
                for (;;) {
                    if (cancel && cancel->load()) { ok = false; if (err) *err = "geannuleerd"; break; }
                    DWORD got = 0;
                    if (!WinHttpReadData(r, buf.data(), DWORD(buf.size()), &got)) { ok = false; if (err) *err = "verbinding verbroken"; break; }
                    if (!got) break;
                    f.write(buf.data(), got);
                    done += got;
                    if (progress) progress(total ? double(done) / double(total) : 0.0, Format("%.1f MB", done / 1048576.0));
                }
            } else if (err) {
                *err = Format("HTTP %lu", status);
            }
        } else if (err) {
            *err = Format("verbinding mislukt (%lu)", GetLastError());
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    if (!ok) return false;
    fs::remove(dest, ec);
    fs::rename(part, dest, ec);
    if (ec) return fail("hernoemen mislukt");
    return true;
}

bool VerifyAuthenticode(const fs::path& file, const std::string& signer, std::string* err) {
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = file.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG st = WinVerifyTrust(nullptr, &action, &wd);
    bool ok = st == ERROR_SUCCESS;
    std::string name;
    if (ok) {
        CRYPT_PROVIDER_DATA* pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR* sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        if (pc && pc->pCert) {
            wchar_t cn[256] = {};
            CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, cn, 256);
            name = U(std::wstring(cn));
        }
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    if (!ok) { if (err) *err = Format("geen geldige digitale handtekening (0x%08lX)", (unsigned long)st); return false; }
    if (name.find(signer) == std::string::npos) {
        if (err) *err = "ondertekend door '" + name + "', verwacht '" + signer + "'";
        return false;
    }
    return true;
}

} // namespace sb
