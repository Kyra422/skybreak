// INSTALL19: SkyBreak-Setup wizard pages (see app.h).
#include "app.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>

#include "imgui.h"
#include "setup_theme.h"

namespace sb {

namespace {

const char* kStepsNl[] = {"Welkom", "Schijf", "Mappen", "Benodigdheden", "Overzicht", "Bouwen", "Klaar"};
const char* kStepsEn[] = {"Welcome", "Disc", "Folders", "Prerequisites", "Summary", "Build", "Done"};

int StepIndex(App::Page p) {
    switch (p) {
    case App::kWelcome: return 0;
    case App::kDisc: return 1;
    case App::kFolders: return 2;
    case App::kPrereqs: return 3;
    case App::kSummary: return 4;
    case App::kBuild: case App::kError: return 5;
    case App::kDone: return 6;
    default: return -1;
    }
}

std::string Gb(uint64_t b) { return Format("%.1f GB", double(b) / 1073741824.0); }

std::string Clock(double s) {
    if (s < 0) return "--:--";
    int t = int(s + 0.5);
    return t >= 3600 ? Format("%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60) : Format("%d:%02d", t / 60, t % 60);
}

std::string HintFor(const std::string& kind, const std::deque<std::string>& tail, bool en) {
    std::string all;
    for (auto& l : tail) all += l + "\n";
    std::string low = Lower(all);
    if (low.find("failed recompaction") != std::string::npos)
        return en ? "Ninja could not update its build files. Another build may be using this folder. Wait for it to finish, then click Try again."
                  : "Ninja kon de bouwbestanden niet bijwerken. Mogelijk gebruikt een andere build deze map. Wacht tot die klaar is en probeer opnieuw.";
    if (kind == "ruimte" || low.find("not enough space") != std::string::npos || low.find("disk is full") != std::string::npos)
        return en ? "The disk is full. Free some space and click Try again." : "De schijf is vol. Maak ruimte vrij en klik op Opnieuw proberen.";
    if (low.find("c1060") != std::string::npos || low.find("out of heap") != std::string::npos || low.find("c1076") != std::string::npos)
        return en ? "The compiler ran out of memory. Close other programs and click Try again: Setup then uses half as many parallel jobs."
                  : "De compiler had te weinig geheugen. Sluit andere programma's en klik op Opnieuw proberen: Setup gebruikt dan de helft van de taken.";
    if (low.find("cl.exe") != std::string::npos && low.find("niet beschikbaar") != std::string::npos)
        return en ? "The C++ compiler was not found. Go back to Prerequisites." : "De C++-compiler is niet gevonden. Ga terug naar Benodigdheden.";
    if (low.find("0xc0000135") != std::string::npos || low.find("-1073741515") != std::string::npos)
        return en ? "DolphinTool needs the Visual C++ runtime (it comes with Visual Studio)." : "DolphinTool heeft de Visual C++-runtime nodig (die komt mee met Visual Studio).";
    if (low.find("executionpolicy") != std::string::npos || low.find("execution policy") != std::string::npos)
        return en ? "Your organisation blocks PowerShell scripts (ExecutionPolicy). Ask the administrator, or use another PC."
                  : "Je organisatie blokkeert PowerShell-scripts (ExecutionPolicy). Vraag de beheerder, of gebruik een andere pc.";
    if (kind == "schijf") return en ? "The disc did not pass the check. Choose your own USA 1.00 disc on the Disc page." : "De schijf kwam niet door de controle. Kies je eigen VS-schijf 1.00 op de pagina Schijf.";
    if (kind == "hulpmiddel") return en ? "A required program is missing or does not work. Go back to Prerequisites." : "Een benodigd programma ontbreekt of werkt niet. Ga terug naar Benodigdheden.";
    if (kind == "geannuleerd") return en ? "Cancelled. Click Try again to continue where it stopped." : "Geannuleerd. Klik op Opnieuw proberen om verder te gaan waar het stopte.";
    if (kind == "pakket") return en ? "The package is incomplete. Extract the whole zip again." : "Het pakket is onvolledig. Pak de hele zip opnieuw uit.";
    if (kind == "draait") // REL22: the build script waited for SkyBreak to be closed and gave up
        return en ? "SkyBreak was still running. Close it (game and launcher) and click Try again: the build continues where it stopped."
                  : "SkyBreak draaide nog. Sluit het (spel en launcher) en klik op Opnieuw proberen: het bouwen gaat verder waar het stopte.";
    return en ? "Open the log for details. Try again continues where it stopped." : "Open het logboek voor details. Opnieuw proberen gaat verder waar het stopte.";
}

void Spinner(float r, ImU32 col) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float t = float(ImGui::GetTime());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < 8; ++i) {
        float a = t * 6.0f + i * 0.785f;
        float alpha = 0.25f + 0.75f * float(i) / 8.0f;
        dl->AddCircleFilled(ImVec2(p.x + r + std::cos(a) * r * 0.7f, p.y + r + std::sin(a) * r * 0.7f), r * 0.18f,
                            (col & 0x00FFFFFF) | (ImU32(alpha * 255) << 24));
    }
    ImGui::Dummy(ImVec2(r * 2, r * 2));
}

void CheckMark(ImDrawList* dl, ImVec2 c, float s, ImU32 col) {
    ImVec2 pts[3] = {ImVec2(c.x - s * 0.5f, c.y), ImVec2(c.x - s * 0.15f, c.y + s * 0.35f), ImVec2(c.x + s * 0.55f, c.y - s * 0.4f)};
    dl->AddPolyline(pts, 3, col, 0, s * 0.22f);
}

} // namespace

const char* App::PageId(Page p) {
    switch (p) {
    case kWelcome: return "1-welkom";
    case kDisc: return "2-schijf";
    case kFolders: return "3-mappen";
    case kPrereqs: return "4-benodigdheden";
    case kSummary: return "5-overzicht";
    case kBuild: return "6-bouwen";
    case kDone: return "7-klaar";
    case kError: return "8-fout";
    case kModify: return "9-wijzigen";
    case kUninstall: return "10-verwijderen";
    case kUninstallDone: return "11-verwijderd";
    default: return "?";
    }
}

App::App(Engine& e, UiFonts fonts, void* logo_tex, int logo_w, int logo_h)
    : e_(e), f_(fonts), logo_(logo_tex), logo_w_(logo_w), logo_h_(logo_h) {
    c_.lang = e_.Lang();
    c_.install_dir = e_.DefaultInstallDir();
    c_.data_dir = e_.DefaultDataDir();
    install_dir_text_ = U(c_.install_dir);
    data_dir_text_ = U(c_.data_dir);
    e_.DetectExisting();
}

void App::StartUninstallMode() {
    e_.DetectExisting();
    page_ = kUninstall;
}
void App::StartModifyMode() {
    e_.DetectExisting();
    const Json& ex = e_.Existing();
    c_.startmenu = ex.boolean("startmenu", true);
    c_.desktop = ex.boolean("desktop", true);
    page_ = kModify;
}

void App::Go(Page p) {
    page_ = p;
    desc_.clear();
    focus_next_ = true;
    if (p == kPrereqs && !rep_valid_ && !detect_job_.valid()) BeginDetect();
}

void App::OnDrop(const fs::path& p) {
    if (page_ != kDisc) return;
    disc_text_ = U(p);
    BeginProbe(p);
}

// ------------------------------------------------------------------------------------------------ workers
void App::BeginProbe(const fs::path& p) {
    probe_valid_ = false;
    rep_valid_ = false; // need for DolphinTool depends on the disc kind
    Engine* e = &e_;
    probe_job_ = std::async(std::launch::async, [e, p] { return e->Probe(p); });
}

void App::BeginDetect() {
    rep_valid_ = false;
    Engine* e = &e_;
    Choices c = c_;
    DiscProbe d = probe_;
    detect_job_ = std::async(std::launch::async, [e, c, d] { return e->Detect(c, d); });
}

void App::BeginInstallPrereq(const std::string& id) {
    install_progress_ = 0;
    install_msg_.clear();
    Engine* e = &e_;
    DetectContext ctx;
    ctx.install_dir = c_.install_dir;
    ctx.need_dolphintool = probe_.kind != DiscKind::Folder;
    std::atomic<double>* prog = &install_progress_;
    install_job_ = std::async(std::launch::async, [e, id, ctx, prog] {
        return e->prereqs().Install(id, ctx, [prog](double f, const std::string&) { prog->store(f); }, nullptr);
    });
}

void App::PollWorkers() {
    using namespace std::chrono_literals;
    if (probe_job_.valid() && probe_job_.wait_for(0ms) == std::future_status::ready) {
        probe_ = probe_job_.get();
        probe_valid_ = true;
    }
    if (detect_job_.valid() && detect_job_.wait_for(0ms) == std::future_status::ready) {
        rep_ = detect_job_.get();
        rep_valid_ = true;
    }
    if (install_job_.valid() && install_job_.wait_for(0ms) == std::future_status::ready) {
        InstallOutcome o = install_job_.get();
        install_msg_ = e_.Lang() == "en" ? o.msg_en : o.msg_nl;
        install_log_.push_back(consent_id_ + ": " + install_msg_);
        if (o.restart_needed) {
            e_.state().set("phase", "herstart");
            e_.SaveState(c_.install_dir);
        }
        BeginDetect();
    }
}

bool App::PickFile(fs::path& out, bool folder) {
    if (snapshot_) return false;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    DWORD flags = 0;
    dlg->GetOptions(&flags);
    dlg->SetOptions(flags | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    if (!folder) {
        COMDLG_FILTERSPEC spec[] = {{L"Wii disc (*.iso, *.wbfs, *.rvz, *.wia, *.gcz, *.ciso)", L"*.iso;*.wbfs;*.rvz;*.wia;*.gcz;*.ciso"},
                                    {L"All files", L"*.*"}};
        dlg->SetFileTypes(2, spec);
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(HWND(hwnd_)))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(dlg->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                out = p;
                ok = true;
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ------------------------------------------------------------------------------------------------ chrome
void App::Describe(const std::string& text) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) desc_ = text;
}

void App::StatusCard(bool ok, bool warn, const std::string& title, const std::string& body) {
    ImU32 col = ok ? theme::kGreen : (warn ? theme::kAmber : theme::kRed);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.095f, 0.06f, 0.85f));
    ImGui::BeginChild(ImGui::GetID(title.c_str()), ImVec2(w, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::Indent(10);
    ImGui::PushFont(f_.bold);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
    ImGui::TextWrapped("%s", title.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    if (!body.empty()) ImGui::TextWrapped("%s", body.c_str());
    ImGui::Unindent(10);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 4, p.y + ImGui::GetItemRectSize().y), col, 2);
}

void App::DrawBackground(float w, float h) {
    ImDrawList* bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h), IM_COL32(26, 20, 13, 255), IM_COL32(18, 14, 9, 255),
                                IM_COL32(10, 8, 5, 255), IM_COL32(16, 12, 8, 255));
    // faint sky arc behind the logo
    bg->AddCircleFilled(ImVec2(w * 0.08f, -h * 0.35f), h * 0.75f, IM_COL32(120, 30, 24, 28), 96);
    bg->AddCircle(ImVec2(w * 0.08f, -h * 0.35f), h * 0.78f, IM_COL32(236, 200, 120, 30), 96, 2.0f);
}

void App::DrawHeader(float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetWindowPos();
    float s = theme::Scale();
    float logo = 76 * s;
    if (logo_) dl->AddImage(ImTextureID(logo_), ImVec2(o.x + 24 * s, o.y + 14 * s), ImVec2(o.x + 24 * s + logo, o.y + 14 * s + logo));
    float x = o.x + 24 * s + (logo_ ? logo + 16 * s : 0);
    dl->AddText(f_.title, f_.title->FontSize, ImVec2(x, o.y + 18 * s), theme::kGold, "SkyBreak");
    // INSTALL19 10 Oct: the exact SkyBreak brand line
    std::string sub = T("pc-port van The Legend of Zelda: Skyward Sword, gebouwd van je eigen schijf — geen officieel Nintendo-product",
                        "PC port of The Legend of Zelda: Skyward Sword, built from your own disc — not an official Nintendo product");
    dl->AddText(f_.body, f_.body->FontSize, ImVec2(x, o.y + 18 * s + f_.title->FontSize + 2 * s), theme::kParchment, sub.c_str());
    std::string what = page_ >= kModify ? T("Installatie beheren", "Manage installation")
                                        : T("Installeren", "Install") + std::string("  -  ") + T("versie ", "version ") + e_.PackageVersion();
    ImVec2 ts = f_.tiny->CalcTextSizeA(f_.tiny->FontSize, FLT_MAX, 0, what.c_str());
    dl->AddText(f_.tiny, f_.tiny->FontSize, ImVec2(o.x + w - ts.x - 24 * s, o.y + 22 * s), theme::kDim, what.c_str());
    if (e_.plat().IsSandbox()) {
        std::string sb = "ZANDBAK: " + U(e_.plat().Root());
        // INSTALL19 10 Oct: just above the separator line, so it never overlaps the (longer) brand line
        dl->AddText(f_.tiny, f_.tiny->FontSize, ImVec2(o.x + w - 24 * s - f_.tiny->CalcTextSizeA(f_.tiny->FontSize, FLT_MAX, 0, sb.c_str()).x,
                                                         o.y + 100 * s - f_.tiny->FontSize), theme::kAmber, sb.c_str());
    }
    dl->AddLine(ImVec2(o.x + 24 * s, o.y + 104 * s), ImVec2(o.x + w - 24 * s, o.y + 104 * s), IM_COL32(236, 200, 120, 70), 1.0f);
}

void App::DrawSteps(float x, float y, float w, float h) {
    float s = theme::Scale();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int cur = StepIndex(page_);
    if (cur < 0) {
        const char* label = page_ == kModify ? "Wijzigen" : "Verwijderen";
        std::string l = page_ == kModify ? T("Wijzigen", "Modify") : T("Verwijderen", "Uninstall");
        (void)label;
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + 3 * s, y + 34 * s), theme::kGold);
        dl->AddText(f_.bold, f_.bold->FontSize, ImVec2(x + 18 * s, y + 6 * s), theme::kGoldBright, l.c_str());
        return;
    }
    for (int i = 0; i < 7; ++i) {
        float yy = y + i * 46 * s;
        bool active = i == cur, done = i < cur;
        ImU32 col = active ? theme::kGoldBright : (done ? theme::kParchment : theme::kDim);
        if (active) {
            dl->AddRectFilled(ImVec2(x, yy), ImVec2(x + w, yy + 36 * s), IM_COL32(236, 200, 120, 40), 6 * s);
            dl->AddRectFilled(ImVec2(x, yy + 4 * s), ImVec2(x + 3 * s, yy + 32 * s), theme::kGold);
        }
        ImVec2 c(x + 22 * s, yy + 18 * s);
        if (done) CheckMark(dl, c, 14 * s, theme::kGold);
        else {
            float d = 6 * s;
            if (active) dl->AddQuadFilled(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), theme::kGoldBright);
            else dl->AddQuad(ImVec2(c.x, c.y - d), ImVec2(c.x + d, c.y), ImVec2(c.x, c.y + d), ImVec2(c.x - d, c.y), theme::kDim, 1.5f);
        }
        const char* label = e_.Lang() == "en" ? kStepsEn[i] : kStepsNl[i];
        dl->AddText(active ? f_.bold : f_.body, (active ? f_.bold : f_.body)->FontSize, ImVec2(x + 42 * s, yy + 7 * s), col, label);
    }
    (void)h;
}

void App::DrawFooter(float w, float h) {
    float s = theme::Scale();
    std::string t = T("The Legend of Zelda: Skyward Sword is een handelsmerk van Nintendo. SkyBreak is een fanproject en geen officieel Nintendo-product.",
                      "The Legend of Zelda: Skyward Sword is a trademark of Nintendo. SkyBreak is a fan project and not an official Nintendo product.");
    ImGui::GetWindowDrawList()->AddText(f_.tiny, f_.tiny->FontSize, ImVec2(ImGui::GetWindowPos().x + 24 * s, ImGui::GetWindowPos().y + h - 26 * s),
                                        theme::kDim, t.c_str());
    (void)w;
}

bool App::NavButtons(bool can_back, bool can_next, const std::string& next_label) {
    float s = theme::Scale();
    bool next = false;
    ImGui::Separator();
    std::string hint = T("Enter / A = verder    Esc / B = terug", "Enter / A = next    Esc / B = back");
    ImGui::PushFont(f_.tiny);
    ImGui::TextDisabled("%s", hint.c_str());
    ImGui::PopFont();
    float bw = 150 * s;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 2 - 12 * s);
    ImGui::BeginDisabled(!can_back);
    if (ImGui::Button(T("Terug", "Back").c_str(), ImVec2(bw, 0)) && can_back) {
        int i = StepIndex(page_);
        if (i > 0) Go(Page(i - 1));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!can_next);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.40f, 0.17f, 1.0f));
    if (ImGui::Button(next_label.c_str(), ImVec2(bw, 0))) next = true;
    if (focus_next_ && can_next) { ImGui::SetItemDefaultFocus(); focus_next_ = false; }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    // keyboard / controller: Esc or B = back (not while a popup is open)
    if (can_back && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) && !ImGui::GetIO().WantTextInput &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        int i = StepIndex(page_);
        if (i > 0) Go(Page(i - 1));
    }
    return next && can_next;
}

// ------------------------------------------------------------------------------------------------ frame
void App::Frame(float width, float height) {
    PollWorkers();
    float s = theme::Scale();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus);
    DrawBackground(width, height);
    DrawHeader(width);
    float top = 118 * s, left = 24 * s, steps_w = 230 * s, bottom = 40 * s;
    DrawSteps(left, ImGui::GetWindowPos().y + top + 8 * s, steps_w, height - top - bottom);
    DrawFooter(width, height);

    ImGui::SetCursorPos(ImVec2(left + steps_w + 24 * s, top));
    float cw = width - (left + steps_w + 24 * s) - 24 * s, ch = height - top - bottom - 8 * s;
    float desc_h = 92 * s;
    ImGui::BeginChild("##content", ImVec2(cw, ch - desc_h - 12 * s), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    desc_.clear();
    switch (page_) {
    case kWelcome: PageWelcome(); break;
    case kDisc: PageDisc(); break;
    case kFolders: PageFolders(); break;
    case kPrereqs: PagePrereqs(); break;
    case kSummary: PageSummary(); break;
    case kBuild: PageBuild(); break;
    case kDone: PageDone(); break;
    case kError: PageError(); break;
    case kModify: PageModify(); break;
    case kUninstall: PageUninstall(); break;
    case kUninstallDone: PageUninstallDone(); break;
    default: break;
    }
    RunningPopup(); // REL22
    ImGui::EndChild();
    // description pane (right, under the content): explains the focused or hovered item
    ImGui::SetCursorPos(ImVec2(left + steps_w + 24 * s, top + ch - desc_h));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.08f, 0.05f, 0.85f));
    ImGui::BeginChild("##desc", ImVec2(cw, desc_h), ImGuiChildFlags_Borders);
    ImGui::PushFont(f_.tiny);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kGold));
    ImGui::TextUnformatted(T("UITLEG", "DESCRIPTION").c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::TextWrapped("%s", (desc_.empty() ? page_desc_ : desc_).c_str());
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

// ------------------------------------------------------------------------------------------------ pages
void App::PageWelcome() {
    page_desc_ = T("SkyBreak bouwt de pc-versie op jouw pc, uit jouw eigen schijf. Er wordt geen spelcode verspreid of gedownload.",
                   "SkyBreak builds the PC version on your PC, from your own disc. No game code is distributed or downloaded.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("Welkom", "Welcome").c_str());
    ImGui::PopFont();
    ImGui::TextWrapped("%s", T("Dit programma installeert SkyBreak: het pakt je eigen Skyward Sword-schijf uit, vertaalt de spelcode naar C++ en "
                               "bouwt daar met Visual Studio een Windows-programma van. Dat duurt ongeveer 20 tot 40 minuten.",
                               "This program installs SkyBreak: it extracts your own Skyward Sword disc, translates the game code to C++ and "
                               "builds a Windows program from it with Visual Studio. That takes about 20 to 40 minutes.").c_str());
    ImGui::Spacing();
    ImGui::BeginChild("##legal", ImVec2(0, 120 * theme::Scale()), ImGuiChildFlags_Borders);
    ImGui::TextWrapped("%s", T("- Je hebt je eigen schijf nodig: The Legend of Zelda: Skyward Sword, Amerikaanse versie 1.00 (SOUE01). "
                               "Als bestand (.iso, .wbfs, .rvz, .wia, .gcz) of als uitgepakte map.\n"
                               "- De map die Setup bouwt bevat code en gegevens uit jouw schijf. Alleen voor eigen gebruik: niet delen of uploaden.\n"
                               "- SkyBreak is een fanproject en geen officieel Nintendo-product. Niet-commercieel.\n"
                               "- Gebaseerd op NWiiRecomp door Volodymyr Vovchok (BlackLine Interactive); licentie in LICENSE.txt. "
                               "Andere onderdelen: zie THIRD-PARTY-NOTICES.txt.",
                               "- You need your own disc: The Legend of Zelda: Skyward Sword, American version 1.00 (SOUE01). "
                               "As a file (.iso, .wbfs, .rvz, .wia, .gcz) or as an extracted folder.\n"
                               "- The folder Setup builds contains code and data from your disc. For your own use only: do not share or upload it.\n"
                               "- SkyBreak is a fan project and not an official Nintendo product. Non-commercial.\n"
                               "- Based on NWiiRecomp by Volodymyr Vovchok (BlackLine Interactive); licence in LICENSE.txt. "
                               "Other components: see THIRD-PARTY-NOTICES.txt.").c_str());
    ImGui::EndChild();
    Describe(T("De voorwaarden in het kort. De volledige teksten staan in LICENSE.txt en THIRD-PARTY-NOTICES.txt naast Setup.",
               "The terms in short. The full texts are in LICENSE.txt and THIRD-PARTY-NOTICES.txt next to Setup."));
    ImGui::Spacing();
    ImGui::Checkbox(T("Ik gebruik een dump van mijn eigen exemplaar (VS, versie 1.00)", "I use a dump of my own copy (USA, version 1.00)").c_str(), &c_.accepted);
    Describe(T("Verplicht. Setup bouwt alleen uit een schijf die je zelf bezit.", "Required. Setup only builds from a disc you own."));
    ImGui::TextUnformatted(T("Taal:", "Language:").c_str());
    ImGui::SameLine();
    bool nl = e_.Lang() != "en";
    if (ImGui::RadioButton("Dutch", nl)) { e_.SetLang("nl"); c_.lang = "nl"; }
    Describe("Setup language.");
    ImGui::SameLine();
    if (ImGui::RadioButton("English", !nl)) { e_.SetLang("en"); c_.lang = "en"; }
    Describe("Setup language.");

    const Json& ex = e_.Existing();
    if (ex.is_object()) {
        ImGui::Spacing();
        std::string where = U(e_.ExistingDir());
        Mode m = e_.mode();
        std::string title = m == Mode::Resume ? T("Een installatie is niet afgemaakt", "An installation was not finished")
                                              : Format(T("SkyBreak %s staat al op deze pc", "SkyBreak %s is already on this PC").c_str(), ex.str("version").c_str());
        std::string body = where + (m == Mode::Resume ? Format(T(" (stap %d, %d/%d)", " (step %d, %d/%d)").c_str(), ex.integer("step"), ex.integer("a"), ex.integer("b")) : "");
        StatusCard(true, false, title, body);
        // REL22: early, live notice (checked once a second); the real check with Retry/Cancel comes when Setup starts writing
        if (!snapshot_ && GetTickCount64() - running_poll_ > 1000) {
            running_poll_ = GetTickCount64();
            if (pending_ == Pending::None) running_ = e_.Running(e_.ExistingDir());
        }
        if (!snapshot_ && pending_ == Pending::None && !running_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAmber));
            ImGui::TextWrapped("%s", T("SkyBreak draait nog. Sluit het spel (en de launcher) voordat je bijwerkt, herstelt of verwijdert; "
                                       "Setup sluit het niet zelf af.",
                                       "SkyBreak is still running. Close the game (and the launcher) before you update, repair or uninstall; "
                                       "Setup does not close it for you.").c_str());
            ImGui::PopStyleColor();
        }
        auto prefill = [&] {
            c_.install_dir = e_.ExistingDir();
            install_dir_text_ = U(c_.install_dir);
            if (!ex.str("data_dir").empty()) { c_.data_dir = P(ex.str("data_dir")); data_dir_text_ = U(c_.data_dir); }
            c_.copy_folder = !ex.boolean("pointer_mode", false);
            c_.startmenu = ex.boolean("startmenu", true);
            c_.desktop = ex.boolean("desktop", true);
            c_.keep_build = ex.boolean("keep_build", true);
            disc_text_ = ex.str("disc");
            if (!disc_text_.empty()) BeginProbe(P(disc_text_));
            e_.state() = ex;
        };
        ImGui::BeginDisabled(!c_.accepted);
        if (m == Mode::Update && ImGui::Button(Format(T("Bijwerken naar %s", "Update to %s").c_str(), e_.PackageVersion().c_str()).c_str())) { prefill(); Go(kDisc); }
        if (m == Mode::Update) { Describe(T("Alleen gewijzigde bronbestanden worden vervangen; daarna een snelle, incrementele bouw. Saves en instellingen blijven.",
                                            "Only changed source files are replaced; then a fast, incremental build. Saves and settings stay.")); ImGui::SameLine(); }
        if (m == Mode::Resume && ImGui::Button(T("Doorgaan waar je was", "Continue where you were").c_str())) { prefill(); Go(kDisc); }
        if (m == Mode::Resume) { Describe(T("Wat al gebouwd is blijft staan; de bouw gaat verder.", "What is already built stays; the build continues.")); ImGui::SameLine(); }
        if (m == Mode::Repair && ImGui::Button(T("Herstellen", "Repair").c_str())) { prefill(); Go(kDisc); }
        if (m == Mode::Repair) { Describe(T("Controleert de schijf opnieuw, bouwt wat ontbreekt en zet snelkoppelingen en de Apps-ingang terug.",
                                            "Checks the disc again, builds what is missing and restores shortcuts and the Apps entry.")); ImGui::SameLine(); }
        ImGui::EndDisabled();
        if (ImGui::Button(T("Wijzigen", "Modify").c_str())) StartModifyMode();
        Describe(T("Snelkoppelingen aan/uit, bouwbestanden opruimen.", "Shortcuts on/off, clean up build files."));
        ImGui::SameLine();
        if (ImGui::Button(T("Verwijderen", "Uninstall").c_str())) StartUninstallMode();
        Describe(T("Verwijdert SkyBreak. Je saves blijven bewaard, tenzij je dat apart kiest.", "Removes SkyBreak. Your saves are kept unless you choose otherwise."));
    }
    ImGui::Dummy(ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f));
    if (NavButtons(false, c_.accepted, T("Volgende", "Next"))) Go(kDisc);
}

void App::PageDisc() {
    page_desc_ = T("Kies je schijfbestand of een uitgepakte map. Alleen de Amerikaanse versie 1.00 (SOUE01, revisie 0) werkt. "
                   "Je kunt het bestand ook op dit venster slepen.",
                   "Choose your disc image or an extracted folder. Only the American version 1.00 (SOUE01, revision 0) works. "
                   "You can also drag the file onto this window.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("Schijf kiezen", "Choose your disc").c_str());
    ImGui::PopFont();
    ImGui::SetNextItemWidth(-1);
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", disc_text_.c_str());
    if (ImGui::InputTextWithHint("##disc", T("pad naar .iso / .wbfs / .rvz / .wia / .gcz of een map", "path to .iso / .wbfs / .rvz / .wia / .gcz or a folder").c_str(),
                                 buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        disc_text_ = buf;
        BeginProbe(P(disc_text_));
    } else {
        disc_text_ = buf;
    }
    Describe(T("Typ of plak een pad en druk op Enter, of gebruik de knoppen hieronder.", "Type or paste a path and press Enter, or use the buttons below."));
    fs::path picked;
    if (ImGui::Button(T("Bestand kiezen...", "Choose file...").c_str()) && PickFile(picked, false)) { disc_text_ = U(picked); BeginProbe(picked); }
    Describe(T("Een schijfbestand: .iso, .wbfs, .rvz, .wia of .gcz. Voor deze formaten is DolphinTool nodig (zie Benodigdheden).",
               "A disc image: .iso, .wbfs, .rvz, .wia or .gcz. These formats need DolphinTool (see Prerequisites)."));
    ImGui::SameLine();
    if (ImGui::Button(T("Map kiezen...", "Choose folder...").c_str()) && PickFile(picked, true)) { disc_text_ = U(picked); BeginProbe(picked); }
    Describe(T("Een uitgepakte DATA-partitie (map met sys\\ en files\\), bijvoorbeeld uit Dolphin > Eigenschappen > Bestandssysteem > Hele schijf uitpakken.",
               "An extracted DATA partition (folder with sys\\ and files\\), e.g. from Dolphin > Properties > Filesystem > Extract Entire Disc."));
    ImGui::Spacing();
    if (probe_job_.valid()) {
        Spinner(10 * theme::Scale(), theme::kGold);
        ImGui::SameLine();
        ImGui::TextUnformatted(T("Schijf controleren...", "Checking the disc...").c_str());
    } else if (probe_valid_) {
        bool ok = probe_.ok(), tool = probe_.verdict == DiscVerdict::NeedsTool;
        StatusCard(ok, tool, ok ? T("Schijf in orde", "Disc OK") : (tool ? T("Wordt later gecontroleerd", "Checked later") : T("Deze schijf werkt niet", "This disc does not work")),
                   (e_.Lang() == "en" ? probe_.msg_en : probe_.msg_nl) + (probe_.detail.empty() ? "" : "\n" + probe_.detail));
        if (ok && probe_.kind == DiscKind::Folder) {
            ImGui::Spacing();
            if (ImGui::RadioButton(T("Kopiëren naar de SkyBreak-map (aanbevolen)", "Copy into the SkyBreak folder (recommended)").c_str(), c_.copy_folder)) c_.copy_folder = true;
            Describe(T("Setup kopieert de uitgepakte map (ca. 3,8 GB). Daarna mag je de originele map verplaatsen of verwijderen.",
                       "Setup copies the extracted folder (about 3.8 GB). Afterwards you may move or delete the original folder."));
            if (ImGui::RadioButton(T("Verwijzen naar deze map (bespaart 3,8 GB)", "Point to this folder (saves 3.8 GB)").c_str(), !c_.copy_folder)) c_.copy_folder = false;
            Describe(T("SkyBreak gebruikt de map waar hij nu staat. Verplaats of verwijder je hem later, dan start het spel niet meer.",
                       "SkyBreak uses the folder where it is now. If you move or delete it later, the game no longer starts."));
        }
    }
    bool can = probe_valid_ && !probe_job_.valid() && (probe_.ok() || probe_.verdict == DiscVerdict::NeedsTool);
    if (probe_valid_ && !probe_job_.valid()) {
        for (auto& i : e_.CheckDiscChoice(c_, probe_)) { // REVINSTALL19 (continuation): pointer mode + non-ASCII folder
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kRed));
            ImGui::TextWrapped("x %s", (e_.Lang() == "en" ? i.en : i.nl).c_str());
            ImGui::PopStyleColor();
            can = can && !i.error;
        }
    }
    ImGui::Dummy(ImVec2(0, std::max(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f)));
    if (NavButtons(true, can, T("Volgende", "Next"))) {
        c_.disc = probe_.path;
        Go(kFolders);
    }
}

void App::PageFolders() {
    page_desc_ = T("Waar SkyBreak komt en waar je saves komen. Er zijn geen beheerdersrechten nodig.",
                   "Where SkyBreak goes and where your saves go. No administrator rights are needed.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("Mappen", "Folders").c_str());
    ImGui::PopFont();
    char buf[1024];
    float bw = 130 * theme::Scale();
    ImGui::TextUnformatted(T("Installatiemap", "Installation folder").c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - 10);
    snprintf(buf, sizeof buf, "%s", install_dir_text_.c_str());
    if (ImGui::InputText("##inst", buf, sizeof buf)) install_dir_text_ = buf;
    Describe(T("Standaard %LOCALAPPDATA%\\Programs\\SkyBreak (alleen voor jou, geen beheerder nodig). Ongeveer 2,4 GB, plus 3,8 GB als de schijf gekopieerd wordt.",
               "Default %LOCALAPPDATA%\\Programs\\SkyBreak (just for you, no administrator needed). About 2.4 GB, plus 3.8 GB when the disc is copied."));
    ImGui::SameLine();
    fs::path picked;
    if (ImGui::Button((T("Wijzigen...", "Change...") + "##i").c_str(), ImVec2(bw, 0)) && PickFile(picked, true)) install_dir_text_ = U(picked / L"SkyBreak");
    ImGui::TextUnformatted(T("Datamap (saves)", "Data folder (saves)").c_str());
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - bw - 10);
    snprintf(buf, sizeof buf, "%s", data_dir_text_.c_str());
    if (ImGui::InputText("##data", buf, sizeof buf)) data_dir_text_ = buf;
    // INSTALL19 10 Oct (user decision): default = the existing save folder %APPDATA%\SkywardSwordPC (saves in its nand subfolder)
    Describe(T("Hier komen je opgeslagen spellen (de virtuele Wii-NAND, submap nand). Standaard %APPDATA%\\SkywardSwordPC: dezelfde map als "
               "eerder, er wordt niets verplaatst. Verwijderen van SkyBreak laat deze map staan.",
               "Your saved games (the virtual Wii NAND, subfolder nand) go here. Default %APPDATA%\\SkywardSwordPC: the same folder as "
               "before, nothing is moved. Uninstalling SkyBreak keeps this folder."));
    ImGui::SameLine();
    if (ImGui::Button((T("Wijzigen...", "Change...") + "##d").c_str(), ImVec2(bw, 0)) && PickFile(picked, true)) data_dir_text_ = U(picked);
    c_.install_dir = Normal(P(install_dir_text_));
    c_.data_dir = Normal(P(data_dir_text_));

    bool blocked = false;
    for (auto& i : e_.CheckInstallDir(c_.install_dir)) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(i.error ? theme::kRed : theme::kAmber));
        ImGui::TextWrapped("%s %s", i.error ? "x" : "!", (e_.Lang() == "en" ? i.en : i.nl).c_str());
        ImGui::PopStyleColor();
        blocked |= i.error;
    }
    ImGui::Spacing();
    ImGui::Checkbox(T("Bouwbestanden bewaren voor snelle updates (1,4 GB)", "Keep build files for fast updates (1.4 GB)").c_str(), &c_.keep_build);
    Describe(T("Met de bouwbestanden duurt een update ongeveer 10 minuten in plaats van 25.", "With the build files an update takes about 10 minutes instead of 25."));
    ImGui::Checkbox(T("Snelkoppeling in het Startmenu", "Start menu shortcut").c_str(), &c_.startmenu);
    Describe(T("Eén snelkoppeling 'SkyBreak' in het Startmenu.", "One 'SkyBreak' shortcut in the Start menu."));
    ImGui::SameLine();
    ImGui::Checkbox(T("Snelkoppeling op het bureaublad", "Desktop shortcut").c_str(), &c_.desktop);
    Describe(T("Een snelkoppeling 'SkyBreak' op je bureaublad.", "A 'SkyBreak' shortcut on your desktop."));
    if (e_.OldSavesElsewhere(c_)) { // INSTALL19 10 Oct: only when another data folder than the old save folder is chosen
        ImGui::Spacing();
        ImGui::TextWrapped("%s", (T("Oude saves gevonden in ", "Old saves found in ") + U(e_.OldSavesDir())).c_str());
        if (ImGui::RadioButton(T("Kopiëren naar de nieuwe datamap (aanbevolen; de oude map blijft als reservekopie)", "Copy to the new data folder (recommended; the old folder stays as a backup)").c_str(), c_.old_saves == "copy")) c_.old_saves = "copy";
        if (ImGui::RadioButton(T("Oude map blijven gebruiken", "Keep using the old folder").c_str(), c_.old_saves == "use_old")) c_.old_saves = "use_old";
        if (ImGui::RadioButton(T("Nieuw beginnen", "Start fresh").c_str(), c_.old_saves == "fresh")) c_.old_saves = "fresh";
    }
    ImGui::Spacing();
    bool red = false;
    if (ImGui::BeginTable("##space", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Station", "Drive").c_str());
        ImGui::TableSetupColumn(T("Nodig", "Needed").c_str());
        ImGui::TableSetupColumn(T("Vrij", "Free").c_str());
        ImGui::TableHeadersRow();
        PrereqReport r = rep_valid_ ? rep_ : PrereqReport{};
        for (auto& row : e_.Space(c_, probe_, r)) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.drive.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Gb(row.need).c_str());
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(row.red() ? theme::kRed : row.amber() ? theme::kAmber : theme::kGreen));
            ImGui::TextUnformatted(Gb(row.free).c_str());
            ImGui::PopStyleColor();
            red |= row.red();
        }
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, std::max(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f)));
    if (NavButtons(true, !blocked && !red, T("Volgende", "Next"))) {
        rep_valid_ = false;
        Go(kPrereqs);
    }
}

void App::ConsentPopup() {
    float s = theme::Scale();
    if (open_consent_) {
        ImGui::OpenPopup("##consent");
        open_consent_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640 * s, 0));
    if (ImGui::BeginPopupModal("##consent", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ConsentInfo ci = e_.prereqs().Consent(consent_id_);
        ImGui::PushFont(f_.bold);
        ImGui::TextWrapped("%s", (T("Installeren: ", "Install: ") + ci.what).c_str());
        ImGui::PopFont();
        auto row = [&](const std::string& k, const std::string& v) {
            if (v.empty()) return;
            ImGui::TextDisabled("%s", k.c_str());
            ImGui::SameLine(160 * s);
            ImGui::TextWrapped("%s", v.c_str());
        };
        row(T("Van", "Publisher"), ci.publisher);
        row(T("Bron", "Source"), ci.url.empty() ? T("(officiële website; Setup opent die)", "(official website; Setup opens it)") : ci.url);
        row(T("Grootte", "Size"), ci.size);
        row(T("Licentie", "Licence"), ci.license + (ci.license_url.empty() ? "" : "  " + ci.license_url));
        row(T("Beheerder", "Admin"), ci.admin ? T("ja (Windows vraagt toestemming)", "yes (Windows asks for permission)") : T("nee", "no"));
        row(T("Wat verandert", "What changes"), e_.Lang() == "en" ? ci.changes_en : ci.changes_nl);
        ImGui::Spacing();
        bool busy = install_job_.valid();
        ImGui::BeginDisabled(busy);
        ImGui::Checkbox(T("Ik ga akkoord met de licentie van dit onderdeel", "I accept the licence of this component").c_str(), &consent_ticked_);
        ImGui::EndDisabled();
        if (busy) {
            ImGui::ProgressBar(float(install_progress_.load()), ImVec2(-1, 0));
            ImGui::TextDisabled("%s", T("Bezig... Een eventueel installatievenster van Windows kan om toestemming vragen.", "Working... A Windows installer window may ask for permission.").c_str());
        } else if (!install_msg_.empty()) {
            ImGui::TextWrapped("%s", install_msg_.c_str());
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!consent_ticked_ || busy);
        if (ImGui::Button(T("Installeren", "Install").c_str(), ImVec2(150 * s, 0))) BeginInstallPrereq(consent_id_);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy && consent_id_ == "vs"); // the VS installer has its own window and cannot be stopped here
        if (ImGui::Button(T("Sluiten", "Close").c_str(), ImVec2(150 * s, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

void App::PagePrereqs() {
    page_desc_ = T("Wat er nodig is om SkyBreak te bouwen. Wat er al staat, wordt gebruikt. Wat ontbreekt, wordt alleen met jouw toestemming geïnstalleerd.",
                   "What is needed to build SkyBreak. What is already there is used. What is missing is only installed with your permission.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("Benodigdheden", "Prerequisites").c_str());
    ImGui::PopFont();
    if (!rep_valid_) {
        if (!detect_job_.valid()) BeginDetect();
        Spinner(10 * theme::Scale(), theme::kGold);
        ImGui::SameLine();
        ImGui::TextUnformatted(T("Zoeken naar Visual Studio, Python en DolphinTool...", "Looking for Visual Studio, Python and DolphinTool...").c_str());
    } else {
        for (auto& it : rep_.items) {
            ImGui::PushID(it.id.c_str());
            bool ok = it.present, needed = it.required;
            std::string title = it.name + "  -  " + (ok ? T("gevonden", "found") : (needed ? T("ontbreekt", "missing") : T("niet nodig", "not needed")));
            std::string body = ok ? it.path + (it.version.empty() ? "" : "  (" + it.version + ")") : (e_.Lang() == "en" ? it.note_en : it.note_nl);
            if (ok && !it.note_nl.empty()) body += "\n" + (e_.Lang() == "en" ? it.note_en : it.note_nl);
            StatusCard(ok || !needed, !needed, title, body);
            if (!ok && needed) {
                if (ImGui::Button(T("Installeren...", "Install...").c_str())) {
                    consent_id_ = it.id;
                    consent_ticked_ = false;
                    install_msg_.clear();
                    open_consent_ = true; // opened in ConsentPopup(), outside this row's PushID scope
                }
                Describe(T("Opent een toestemmingsscherm met wat het is, van wie, hoe groot en welke licentie. Zonder jouw akkoord gebeurt er niets.",
                           "Opens a consent page with what it is, who publishes it, its size and licence. Nothing happens without your approval."));
                ImGui::SameLine();
                if (ImGui::Button(T("Zelf doen (uitleg)", "Do it myself (help)").c_str())) {
                    if (it.id == "vs") e_.plat().OpenUrl("https://visualstudio.microsoft.com/visual-cpp-build-tools/");
                    else if (it.id == "python") e_.plat().OpenUrl("https://www.python.org/downloads/windows/");
                    else e_.plat().OpenUrl("https://dolphin-emu.org/download/");
                }
                Describe(it.id == "vs" ? T("Installeer 'Visual Studio 2022 Build Tools' met 'Desktopontwikkeling met C++' (inclusief Windows SDK en C++ CMake tools).",
                                           "Install 'Visual Studio 2022 Build Tools' with 'Desktop development with C++' (including the Windows SDK and C++ CMake tools).")
                         : it.id == "python" ? T("Installeer Python 3.12 van python.org (alleen voor jou is genoeg).", "Install Python 3.12 from python.org (just for you is enough).")
                                             : T("Download Dolphin, pak het uit en kies DolphinTool.exe met Bladeren. Of kies op de pagina Schijf een uitgepakte map.",
                                                 "Download Dolphin, extract it and pick DolphinTool.exe with Browse. Or choose an extracted folder on the Disc page."));
                if (it.id == "dolphintool") {
                    ImGui::SameLine();
                    fs::path picked;
                    if (ImGui::Button(T("Bladeren...", "Browse...").c_str()) && PickFile(picked, false)) {
                        SetEnvironmentVariableW(L"DOLPHINTOOL", picked.c_str());
                        BeginDetect();
                    }
                }
                ImGui::SameLine();
            }
            if (!ok && needed && ImGui::Button(T("Opnieuw zoeken", "Search again").c_str())) BeginDetect();
            ImGui::PopID();
        }
        ConsentPopup();
        ImGui::Spacing();
        const SystemInfo& si = rep_.sys;
        ImGui::TextDisabled("%s", Format("%s  -  %d %s  -  %.0f GB RAM  -  winget %s  -  tar+lzma %s", si.windows.c_str(), si.cores,
                                         T("kernen", "cores").c_str(), si.ram_gb, si.winget ? "ja" : "nee", si.tar_lzma ? "ja" : "nee").c_str());
        if (si.smart_app_control == 1) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAmber));
            ImGui::TextWrapped("%s", T("Slim app-beheer (Smart App Control) staat aan. Dat kan het zelf gebouwde, niet-ondertekende spel blokkeren. "
                                       "Uitzetten is jouw keuze (Windows-beveiliging > App- en browserbeheer).",
                                       "Smart App Control is on. It may block the self-built, unsigned game. Turning it off is your choice "
                                       "(Windows Security > App & browser control).").c_str());
            ImGui::PopStyleColor();
        }
        for (auto& l : install_log_) ImGui::TextDisabled("%s", l.c_str());
    }
    bool can = rep_valid_ && rep_.MissingRequired().empty();
    ImGui::Dummy(ImVec2(0, std::max(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f)));
    if (NavButtons(true, can, T("Volgende", "Next"))) Go(kSummary);
}

void App::PageSummary() {
    page_desc_ = T("Controleer je keuzes. Installeren start het uitpakken en bouwen; je kunt het altijd annuleren en later verdergaan.",
                   "Check your choices. Install starts extracting and building; you can always cancel and continue later.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("Overzicht", "Summary").c_str());
    ImGui::PopFont();
    if (ImGui::BeginTable("##sum", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        auto row = [&](const std::string& k, const std::string& v) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k.c_str());
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", v.c_str());
        };
        bool pointer = probe_.kind == DiscKind::Folder && !c_.copy_folder;
        row(T("Schijf", "Disc"), U(probe_.path) + (probe_.detail.empty() ? "" : "  (" + probe_.detail + ")"));
        row(T("Spelbestanden", "Game files"), pointer ? T("verwijzen naar de map (niet gekopieerd)", "point to the folder (not copied)")
                                                      : (probe_.kind == DiscKind::Folder ? T("kopiëren naar ", "copy to ") : T("uitpakken naar ", "extract to ")) + U(c_.install_dir / L"game"));
        row(T("Installatiemap", "Installation folder"), U(c_.install_dir));
        row(T("Saves", "Saves"), U(c_.data_dir / L"nand"));
        row(T("Snelkoppelingen", "Shortcuts"), std::string(c_.startmenu ? T("Startmenu ", "Start menu ") : "") + (c_.desktop ? T("Bureaublad", "Desktop") : "") +
                                                   (!c_.startmenu && !c_.desktop ? T("geen", "none") : ""));
        row(T("Bouwbestanden", "Build files"), c_.keep_build ? T("bewaren (snelle updates)", "keep (fast updates)") : T("opruimen na de bouw", "clean up after the build"));
        row(T("Geschatte tijd", "Estimated time"), e_.TimeEstimate(rep_, c_, probe_, e_.Lang() == "en"));
        std::string space;
        for (auto& r : e_.Space(c_, probe_, rep_)) space += r.drive + " " + Gb(r.need) + T(" nodig, ", " needed, ") + Gb(r.free) + T(" vrij   ", " free   ");
        row(T("Ruimte", "Space"), space);
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(true);
    bool hd = false;
    ImGui::Checkbox(T("HD-texturen (komt in een volgende versie)", "HD textures (coming in a later version)").c_str(), &hd);
    ImGui::EndDisabled();
    Describe(T("HD-texturen worden ook uit je eigen schijf gemaakt. Dat komt later via Wijzigen > HD-texturen maken.",
               "HD textures are also made from your own disc. That comes later via Modify > Make HD textures."));
    ImGui::Dummy(ImVec2(0, std::max(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f)));
    if (NavButtons(true, true, T("Installeren", "Install"))) BeginBuild();
}

void App::BeginBuild() {
    if (!GuardRunning(Pending::Build, c_.install_dir)) return; // REL22: update/repair/resume replace the running exe
    std::string err;
    if (Normal(e_.ExistingDir()) != Normal(c_.install_dir)) e_.DetectExisting(c_.install_dir); // INSTALL19 10 Oct: resume in a chosen folder
    e_.state() = e_.Existing().is_object() && e_.ExistingDir() == c_.install_dir ? e_.Existing() : Json::Object();
    e_.state().set("phase", "bezig");
    e_.state().set("install_dir", U(c_.install_dir));
    e_.state().set("data_dir", U(c_.data_dir));
    e_.state().set("disc", U(probe_.path));
    e_.SaveState(c_.install_dir);
    int a = 0, b = 0, r = 0;
    if (!e_.SyncSources(c_.install_dir, &err, &a, &b, &r)) {
        error_title_ = T("Bronbestanden kopiëren mislukt", "Copying the source files failed");
        error_body_ = err;
        error_hint_ = HintFor("pakket", {}, e_.Lang() == "en");
        Go(kError);
        return;
    }
    std::string saves_msg;
    if (!e_.MigrateOldSaves(c_, &saves_msg)) {
        error_title_ = T("Oude saves kopiëren mislukt", "Copying the old saves failed");
        error_body_ = saves_msg;
        error_hint_.clear();
        Go(kError);
        return;
    }
    if (!e_.StartBuild(c_, probe_, rep_, &err)) {
        error_title_ = T("Bouwen kon niet starten", "The build could not start");
        error_body_ = err;
        error_hint_ = HintFor("proces", {}, e_.Lang() == "en");
        Go(kError);
        return;
    }
    build_started_ = true;
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    if (!taskbar_ && hwnd_) {
        ITaskbarList3* tb = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&tb)))) { tb->HrInit(); taskbar_ = tb; }
    }
    Go(kBuild);
}

void App::FinishBuild() {
    SetThreadExecutionState(ES_CONTINUOUS);
    const PipelineState& ps = e_.pipeline().State();
    if (taskbar_) static_cast<ITaskbarList3*>(taskbar_)->SetProgressState(HWND(hwnd_), ps.failed ? TBPF_ERROR : TBPF_NOPROGRESS);
    if (ps.failed || !ps.finished) {
        e_.SetPhase(c_.install_dir, ps.fail_kind == "geannuleerd" ? "geannuleerd" : "fout");
        error_title_ = ps.fail_kind == "geannuleerd" ? T("Geannuleerd", "Cancelled") : T("Het bouwen is gestopt", "The build stopped");
        error_body_ = ps.fail_msg;
        error_hint_ = HintFor(ps.fail_kind, ps.log_tail, e_.Lang() == "en");
        for (auto& l : ps.log_tail) // out of memory: the retry uses half the parallel jobs
            if (l.find("C1060") != std::string::npos || l.find("out of heap") != std::string::npos) {
                c_.jobs = std::max(1, e_.pipeline().Options().jobs / 2);
                break;
            }
        e_.runner().CloseJob();
        Go(kError);
        return;
    }
    e_.runner().CloseJob();
    CompleteFinish();
}

void App::CompleteFinish() {
    if (!GuardRunning(Pending::Finish, c_.install_dir)) return; // REL22: SkyBreak.exe is replaced by Finish
    std::string err;
    if (!e_.Finish(c_, probe_, &err)) {
        error_title_ = T("Afronden mislukt", "Finishing failed");
        error_body_ = err;
        error_hint_.clear();
        Go(kError);
        return;
    }
    Go(kDone);
}

void App::PageBuild() {
    page_desc_ = T("Setup pakt de schijf uit, vertaalt de spelcode en compileert het spel. Je pc blijft bruikbaar (lagere prioriteit). "
                   "Annuleren mag: wat klaar is, blijft bewaard.",
                   "Setup extracts the disc, translates the game code and compiles the game. Your PC stays usable (lower priority). "
                   "Cancelling is fine: what is done is kept.");
    uint64_t now = GetTickCount64();
    if (!snapshot_ && build_started_ && now - last_poll_ > 250) {
        last_poll_ = now;
        if (!e_.pipeline().Poll(e_.runner())) {
            build_started_ = false;
            FinishBuild();
            return;
        }
    }
    const PipelineState& ps = e_.pipeline().State();
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("Bouwen", "Building").c_str());
    ImGui::PopFont();
    ImGui::ProgressBar(float(ps.overall), ImVec2(-1, 26 * theme::Scale()), Format("%d%%", int(ps.overall * 100)).c_str());
    ImGui::TextUnformatted((T("Verstreken ", "Elapsed ") + Clock(ps.elapsed_s) + "   -   " +
                            (ps.eta_s >= 0 ? T("nog ca. ", "about ") + Format("%d min", std::max(1, int(ps.eta_s / 60 + 0.5))) + T("", " left") : std::string("..."))).c_str());
    if (taskbar_) static_cast<ITaskbarList3*>(taskbar_)->SetProgressValue(HWND(hwnd_), ULONGLONG(ps.overall * 1000), 1000);
    ImGui::Spacing();
    for (auto& st : e_.pipeline().Plan()) {
        bool done = std::find(ps.done_steps.begin(), ps.done_steps.end(), st.step) != ps.done_steps.end();
        bool cur = st.step == ps.step && !done;
        ImVec2 p = ImGui::GetCursorScreenPos();
        float h = ImGui::GetTextLineHeight();
        if (done) CheckMark(ImGui::GetWindowDrawList(), ImVec2(p.x + 8, p.y + h * 0.5f), h * 0.7f, theme::kGold);
        else if (cur) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 8, p.y + h * 0.5f), h * 0.22f, theme::kGoldBright);
        ImGui::Dummy(ImVec2(22, h));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(done ? theme::kParchment : cur ? theme::kGoldBright : theme::kDim));
        std::string label = e_.Lang() == "en" ? st.name_en : st.name_nl;
        if (cur && ps.b > 0) label += Format("   %d/%d", ps.a, ps.b);
        ImGui::TextUnformatted(label.c_str());
        ImGui::PopStyleColor();
        if (cur && (st.step == 7 || st.step == 3) && ps.step_fraction > 0) {
            ImGui::SameLine(320 * theme::Scale());
            ImGui::ProgressBar(float(ps.step_fraction), ImVec2(-1, h * 0.8f), "");
        }
    }
    if (ps.waiting) { // REL22: Bouw-SkywardSword.ps1 waits before step 8 replaces the game exe; nothing is closed for the player
        ImGui::Spacing();
        StatusCard(false, true, T("SkyBreak is nog open", "SkyBreak is still running"),
                   T("Sluit SkyBreak (het spel en de launcher); het bouwen gaat daarna vanzelf verder. Setup sluit het niet zelf af.\n",
                     "Close SkyBreak (the game and the launcher); the build then continues by itself. Setup does not close it.\n") +
                       ps.wait_msg);
    }
    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("Logboek", "Log").c_str())) {
        ImGui::BeginChild("##log", ImVec2(0, 140 * theme::Scale()), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushFont(f_.tiny);
        for (auto& l : ps.log_tail) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
        ImGui::EndChild();
        if (ImGui::Button(T("Bouwlog openen", "Open build log").c_str())) e_.plat().OpenFolder(e_.pipeline().DiagnosticLog());
        ImGui::SameLine();
        if (ImGui::Button(T("Kopieer log", "Copy log").c_str())) {
            std::string all;
            for (auto& l : ps.log_tail) all += l + "\n";
            ImGui::SetClipboardText(all.c_str());
        }
    }
    ImGui::Dummy(ImVec2(0, std::max(0.0f, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.6f)));
    ImGui::Separator();
    if (ImGui::Button(T("Annuleren", "Cancel").c_str(), ImVec2(150 * theme::Scale(), 0))) ImGui::OpenPopup("##cancel");
    Describe(T("Stopt het bouwen. Je kunt later verder; wat klaar is, blijft bewaard.", "Stops the build. You can continue later; what is done is kept."));
    if (ImGui::BeginPopupModal("##cancel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T("Stoppen? Je kunt later verder; wat klaar is blijft bewaard.", "Stop? You can continue later; what is done is kept.").c_str());
        if (ImGui::Button(T("Ja, stoppen", "Yes, stop").c_str())) {
            e_.pipeline().Cancel(e_.runner());
            build_started_ = false;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            FinishBuild();
            return;
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Doorgaan", "Continue").c_str())) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::PageDone() {
    page_desc_ = T("Klaar. Start SkyBreak via de snelkoppeling of de knop hieronder.", "Done. Start SkyBreak with the shortcut or the button below.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("SkyBreak staat klaar", "SkyBreak is ready").c_str());
    ImGui::PopFont();
    StatusCard(true, false, T("Installatie voltooid", "Installation complete"),
               T("Je saves: ", "Your saves: ") + U(c_.data_dir / L"nand") + "\n" +
                   T("Deel de map ", "Do not share the folder ") + U(c_.install_dir) +
                   T(" niet: die bevat code uit je schijf.", ": it contains code from your disc."));
    ImGui::Spacing();
    StatusCard(true, false, T("Eerste keer spelen", "First play session"),
               T("1. Kies je controllerindeling onder Instellingen > Besturing.\n"
                 "2. Met een DualSense: open Gyro kalibreren, leg de controller stil en pas de correctie toe.\n"
                 "3. Test eerst zonder mods. Schakel daarna één Link-model tegelijk in en herstart.\n"
                 "Bij een fout: F8 maakt een lokaal rapport met beeld en scène. Bekijk het rapport voordat je het deelt.",
                 "1. Choose your controller layout in Settings > Controls.\n"
                 "2. With a DualSense: open Calibrate gyro, put the controller down and apply the correction.\n"
                 "3. Test without mods first. Then enable one Link model at a time and restart.\n"
                 "For a bug: F8 creates a local report with an image and scene. Review it before sharing."));
    ImGui::Spacing();
    if (ImGui::Button(T("SkyBreak starten", "Start SkyBreak").c_str())) {
        if (!e_.plat().IsSandbox()) ShellExecuteW(nullptr, L"open", (c_.install_dir / L"SkyBreak.exe").c_str(), nullptr, c_.install_dir.c_str(), SW_SHOWNORMAL);
        else e_.plat().Audit("start(logged only)", U(c_.install_dir / L"SkyBreak.exe"));
        quit_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Map openen", "Open folder").c_str())) e_.plat().OpenFolder(c_.install_dir);
    ImGui::SameLine();
    if (ImGui::Button(T("Sluiten", "Close").c_str())) quit_ = true;
}

void App::PageError() {
    page_desc_ = T("Opnieuw proberen gaat verder waar het stopte. Wat al klaar was, blijft bewaard.", "Try again continues where it stopped. What was done is kept.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(error_title_.c_str());
    ImGui::PopFont();
    StatusCard(false, error_title_ == T("Geannuleerd", "Cancelled"), error_body_, error_hint_);
    ImGui::Spacing();
    if (ImGui::Button(T("Opnieuw proberen", "Try again").c_str())) BeginBuild();
    ImGui::SameLine();
    if (ImGui::Button(T("Bouwlog openen", "Open build log").c_str())) {
        fs::path log = e_.pipeline().DiagnosticLog();
        e_.plat().OpenFolder(log.empty() ? c_.install_dir / L"setup" / L"logs" : log);
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Terug naar Benodigdheden", "Back to Prerequisites").c_str())) { rep_valid_ = false; Go(kPrereqs); }
    ImGui::SameLine();
    if (ImGui::Button(T("Sluiten", "Close").c_str())) quit_ = true;
}

void App::ApplyModify() {
    fs::path dir = e_.ExistingDir();
    const Json& ex = e_.Existing();
    Choices c = c_;
    c.install_dir = dir;
    c.data_dir = P(ex.str("data_dir"));
    c.copy_folder = !ex.boolean("pointer_mode", false);
    DiscProbe d;
    d.path = P(ex.str("disc"));
    d.kind = ex.str("disc_kind") == "folder" ? DiscKind::Folder : DiscKind::Rvz;
    d.game_root = P(ex.str("game_dir"));
    e_.state() = ex;
    std::string err;
    modify_msg_ = e_.Finish(c, d, &err) ? T("Toegepast.", "Applied.") : err;
}

void App::PageModify() {
    page_desc_ = T("Pas de installatie aan. Je saves en instellingen blijven staan.", "Change the installation. Your saves and settings stay.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("Installatie wijzigen", "Modify installation").c_str());
    ImGui::PopFont();
    fs::path dir = e_.ExistingDir();
    if (dir.empty()) { ImGui::TextUnformatted(T("Geen installatie gevonden.", "No installation found.").c_str()); return; }
    ImGui::TextDisabled("%s", U(dir).c_str());
    ImGui::Checkbox(T("Snelkoppeling in het Startmenu", "Start menu shortcut").c_str(), &c_.startmenu);
    ImGui::Checkbox(T("Snelkoppeling op het bureaublad", "Desktop shortcut").c_str(), &c_.desktop);
    if (ImGui::Button(T("Toepassen", "Apply").c_str()) && GuardRunning(Pending::Modify, dir)) ApplyModify(); // REL22
    ImGui::Spacing();
    if (ImGui::Button(T("Bouwbestanden verwijderen (ca. 1,4 GB; updates duren daarna langer)", "Remove build files (about 1.4 GB; updates take longer afterwards)").c_str())) {
        std::error_code ec;
        fs::remove_all(dir / L"bouw", ec);
        modify_msg_ = ec ? ec.message() : T("Bouwbestanden verwijderd.", "Build files removed.");
    }
    ImGui::BeginDisabled(true);
    ImGui::Button(T("HD-texturen maken (komt later)", "Make HD textures (coming later)").c_str());
    ImGui::EndDisabled();
    if (!modify_msg_.empty()) ImGui::TextWrapped("%s", modify_msg_.c_str());
    ImGui::Spacing();
    if (ImGui::Button(T("Sluiten", "Close").c_str())) quit_ = true;
}

void App::PageUninstall() {
    page_desc_ = T("SkyBreak wordt verwijderd. Je saves blijven bewaard, tenzij je dat hieronder apart kiest. Visual Studio, Python en Dolphin blijven staan.",
                   "SkyBreak is removed. Your saves are kept unless you choose otherwise below. Visual Studio, Python and Dolphin stay.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("SkyBreak verwijderen", "Uninstall SkyBreak").c_str());
    ImGui::PopFont();
    fs::path dir = e_.ExistingDir();
    if (dir.empty()) { ImGui::TextUnformatted(T("Geen installatie gevonden.", "No installation found.").c_str()); if (ImGui::Button(T("Sluiten", "Close").c_str())) quit_ = true; return; }
    std::string nand = e_.Existing().str("nand_dir");
    ImGui::TextWrapped("%s", (T("Map: ", "Folder: ") + U(dir)).c_str());
    ImGui::Checkbox((T("Ook mijn saves verwijderen (", "Also delete my saves (") + nand + ")").c_str(), &del_saves_);
    Describe(T("Standaard uit. Je opgeslagen spellen blijven dan staan voor een latere installatie.", "Off by default. Your saved games then stay for a later installation."));
    ImGui::Checkbox(T("Ook screenshots verwijderen", "Also delete screenshots").c_str(), &del_shots_);
    ImGui::Spacing();
    if (ImGui::Button(T("Verwijderen", "Uninstall").c_str())) {
        if (del_saves_) ImGui::OpenPopup("##saves");
        else if (GuardRunning(Pending::UninstallKeep, dir)) RunPending(Pending::UninstallKeep); // REL22
    }
    if (ImGui::BeginPopupModal("##saves", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s", (T("Weet je het zeker? Deze map met je saves wordt definitief verwijderd:\n", "Are you sure? This folder with your saves is deleted for good:\n") + nand).c_str());
        if (ImGui::Button(T("Ja, saves ook verwijderen", "Yes, delete saves too").c_str())) {
            ImGui::CloseCurrentPopup();
            if (GuardRunning(Pending::UninstallDelete, dir)) RunPending(Pending::UninstallDelete); // REL22
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Nee, saves bewaren", "No, keep saves").c_str())) { del_saves_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Annuleren", "Cancel").c_str())) quit_ = true;
}

void App::PageUninstallDone() {
    page_desc_ = T("Visual Studio, Python en Dolphin kun je zelf verwijderen via Windows-instellingen > Apps, als je ze niet meer nodig hebt.",
                   "You can remove Visual Studio, Python and Dolphin yourself via Windows Settings > Apps if you no longer need them.");
    ImGui::PushFont(f_.title);
    ImGui::TextUnformatted(T("SkyBreak is verwijderd", "SkyBreak was removed").c_str());
    ImGui::PopFont();
    StatusCard(uninstall_res_.ok, !uninstall_res_.ok, uninstall_res_.saves_removed ? T("Saves verwijderd", "Saves deleted") : T("Je saves zijn bewaard", "Your saves were kept"),
               uninstall_res_.kept_unknown.empty() ? std::string()
                                                    : T("Niet van SkyBreak en daarom blijven staan: ", "Not from SkyBreak and therefore kept: ") + [&] {
                                                          std::string s;
                                                          for (auto& k : uninstall_res_.kept_unknown) s += k + "  ";
                                                          return s;
                                                      }());
    for (auto& e : uninstall_res_.errors) ImGui::TextWrapped("%s", e.c_str());
    if (ImGui::Button(T("Sluiten", "Close").c_str())) quit_ = true;
}

// ------------------------------------------------------------------------------------------------ snapshots
void App::SnapshotRunning() {
    SnapshotState(kSummary);
    pending_ = Pending::Build;
    running_ = {{4242, P("C:\\Users\\Speler\\AppData\\Local\\Programs\\SkyBreak\\SkyBreak.exe")},
                {4243, P("C:\\Users\\Speler\\AppData\\Local\\Programs\\SkyBreak\\bin\\SkyBreak-game.exe")}};
    open_running_ = true;
}

void App::SnapshotState(Page p) {
    snapshot_ = true;
    page_ = p;
    desc_.clear();
    c_.accepted = p != kWelcome;
    disc_text_ = "D:\\Spellen\\Skyward Sword (USA).rvz";
    probe_ = DiscProbe{};
    probe_.path = P(disc_text_);
    probe_.kind = DiscKind::Rvz;
    probe_.game_id = "SOUE01";
    probe_.revision = 0;
    probe_.internal_name = "The Legend of Zelda Skyward Sword";
    ClassifyDisc(probe_);
    probe_valid_ = true;
    rep_ = PrereqReport{};
    rep_.sys.cores = 16;
    rep_.sys.ram_gb = 32;
    rep_.sys.windows = "Windows 11 (build 22631)";
    rep_.sys.winget = true;
    rep_.sys.tar_lzma = true;
    rep_.items = {{"vs", "Visual Studio 2022 Build Tools (C++ x64, Windows SDK, CMake)", true, true, "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community", "17.14", "", ""},
                  {"python", "Python 3.8+", p != kPrereqs, true, p != kPrereqs ? "C:\\Program Files\\Python312\\python.exe" : "", p != kPrereqs ? "3.12.10" : "",
                   "Python 3 is niet gevonden.", "Python 3 was not found."},
                  {"dolphintool", "DolphinTool (Dolphin-emulator)", true, true, "C:\\Dolphin-x64\\DolphinTool.exe", "", "", ""}};
    rep_valid_ = true;
    if (p == kBuild || p == kError) {
        BuildOptions o;
        o.disc = probe_.path;
        o.jobs = 14;
        e_.pipeline().InitModel(o, false);
        PipelineState s;
        s.step = 7;
        s.step_name = "7. SkywardSwordPC.exe bouwen";
        s.done_steps = {1, 2, 3, 4, 5, 6};
        s.a = 412;
        s.b = 690;
        s.step_fraction = 412.0 / 690.0;
        s.elapsed_s = 751;
        s.eta_s = 560;
        s.overall = Pipeline::Overall(e_.pipeline().Plan(), s.done_steps, 7, s.step_fraction);
        s.log_tail = {"[409/690] Building CXX object CMakeFiles/SkywardSwordPC.dir/output_212.cpp.obj",
                      "[410/690] Building CXX object CMakeFiles/SkywardSwordPC.dir/output_213.cpp.obj",
                      "[412/690] Building CXX object CMakeFiles/SkywardSwordPC.dir/rels/d_a_obj_212.cpp.obj"};
        e_.pipeline().SetStateForSnapshot(s);
    }
    if (p == kError) {
        error_title_ = T("Het bouwen is gestopt", "The build stopped");
        error_body_ = T("Bouwen van het spel mislukt (fatal error C1060: compiler is out of heap space).", "Building the game failed (fatal error C1060: compiler is out of heap space).");
        error_hint_ = HintFor("bouw", {"fatal error C1060: compiler is out of heap space"}, e_.Lang() == "en");
    }
    if (p == kUninstallDone) {
        uninstall_res_ = Engine::UninstallResult{};
        uninstall_res_.ok = true;
        uninstall_res_.removed = {"bin", "bron", "bouw", "game", "setup"};
    }
    if (p == kModify || p == kUninstall) {
        // shown without an installation; the page then says so
    }
}

// ------------------------------------------------------------------------------------------------ REL22: SkyBreak still running
bool App::GuardRunning(Pending what, const fs::path& dir) {
    if (snapshot_) return true;
    running_ = e_.Running(dir);
    if (running_.empty()) return true;
    Log("[running] %zu SkyBreak program(s) of %s still run: asking the player (pending %d)", running_.size(), U(dir).c_str(), int(what));
    pending_ = what;
    pending_dir_ = dir;
    open_running_ = true;
    running_poll_ = GetTickCount64();
    return false;
}

void App::RunPending(Pending what) {
    switch (what) {
    case Pending::Build: BeginBuild(); break;
    case Pending::Finish: CompleteFinish(); break;
    case Pending::Modify: ApplyModify(); break;
    case Pending::UninstallKeep:
    case Pending::UninstallDelete: {
        fs::path dir = e_.ExistingDir();
        uninstall_res_ = e_.Uninstall(dir, what == Pending::UninstallDelete, del_shots_);
        if (uninstall_res_.running) { GuardRunning(what, dir); break; } // started again in the meantime: ask again, nothing removed
        page_ = kUninstallDone;
        break;
    }
    case Pending::None: break;
    }
}

void App::RunningPopup() {
    float s = theme::Scale();
    if (open_running_) {
        ImGui::OpenPopup("##running");
        open_running_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(640 * s, 0));
    if (!ImGui::BeginPopupModal("##running", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) return;
    uint64_t now = GetTickCount64();
    if (!snapshot_ && now - running_poll_ > 1000) { // live status: the player sees it turn "closed" (Setup never closes it)
        running_poll_ = now;
        running_ = e_.Running(pending_dir_);
    }
    ImGui::PushFont(f_.bold);
    ImGui::TextWrapped("%s", T("SkyBreak is nog open", "SkyBreak is still running").c_str());
    ImGui::PopFont();
    bool un = pending_ == Pending::UninstallKeep || pending_ == Pending::UninstallDelete;
    const char* what_nl = un ? "verwijderen" : pending_ == Pending::Modify ? "de wijziging toepassen" : "bijwerken";
    const char* what_en = un ? "uninstall" : pending_ == Pending::Modify ? "apply the change" : "update";
    ImGui::TextWrapped("%s", (e_.Lang() == "en"
                                  ? Format("Close SkyBreak (the game and the launcher) so Setup can %s. Save your game first. Setup "
                                           "does not close it for you, so nothing is lost. Then choose Retry.", what_en)
                                  : Format("Sluit SkyBreak (het spel en de launcher), dan kan Setup %s. Sla je spel eerst op. Setup "
                                           "sluit het niet zelf af, zodat je niets kwijtraakt. Kies daarna Opnieuw proberen.", what_nl))
                                 .c_str());
    ImGui::Spacing();
    if (running_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kGold));
        ImGui::TextWrapped("%s", T("SkyBreak is nu gesloten.", "SkyBreak is closed now.").c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(theme::kAmber));
        ImGui::TextUnformatted(T("Nog open:", "Still open:").c_str());
        ImGui::PopStyleColor();
        ImGui::PushFont(f_.tiny);
        for (auto& r : running_) ImGui::TextWrapped("%s", (U(r.image) + Format("  (pid %u)", unsigned(r.pid))).c_str());
        ImGui::PopFont();
    }
    ImGui::Spacing();
    if (ImGui::Button(T("Opnieuw proberen", "Retry").c_str(), ImVec2(180 * s, 0))) {
        running_ = e_.Running(pending_dir_);
        if (running_.empty()) {
            Pending p = pending_;
            pending_ = Pending::None;
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            RunPending(p);
            return;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Annuleren", "Cancel").c_str(), ImVec2(150 * s, 0))) {
        Pending p = pending_;
        pending_ = Pending::None;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        if (p == Pending::Finish) { // the game is built; only the last step waits -> Try again (incremental, short) finishes it
            e_.SetPhase(c_.install_dir, "fout");
            error_title_ = T("Afronden uitgesteld", "Finishing postponed");
            error_body_ = T("Het spel is gebouwd, maar SkyBreak draaide nog. Sluit SkyBreak en kies Opnieuw proberen: dat duurt maar even.",
                            "The game is built, but SkyBreak was still running. Close SkyBreak and choose Try again: that only takes a moment.");
            error_hint_.clear();
            Go(kError);
        }
        return;
    }
    ImGui::EndPopup();
}

} // namespace sb
