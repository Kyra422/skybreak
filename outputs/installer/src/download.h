#pragma once
// INSTALL19: HTTPS download (WinHTTP, part of Windows) with progress, resume (Range) and cancel, plus
// verification helpers: SHA-256 pin and Authenticode signature (WinVerifyTrust + signer name).
// Only used after the player's explicit consent on the consent page; never in --sandbox (refused there).
#include <atomic>
#include <functional>
#include <string>

#include "common.h"

namespace sb {

using ProgressFn = std::function<void(double fraction, const std::string& text)>;

bool HttpDownload(const std::string& url, const fs::path& dest, const ProgressFn& progress, std::atomic<bool>* cancel,
                  std::string* err);

// true when the file has a valid Authenticode signature whose signer common name contains `signer`.
bool VerifyAuthenticode(const fs::path& file, const std::string& signer, std::string* err);

} // namespace sb
