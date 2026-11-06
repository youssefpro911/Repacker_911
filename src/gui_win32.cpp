#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include "archive.hpp"
#include "scanner.hpp"
#include "sfx.hpp"

#include <atomic>
#include <cwchar>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kWindowClass[] = L"RPKBuilderWindow";
constexpr UINT WM_RPK_LOG = WM_APP + 10;
constexpr UINT WM_RPK_DONE = WM_APP + 11;

enum ControlId : int {
    IDC_SOURCE = 100,
    IDC_SOURCE_BROWSE,
    IDC_ARCHIVE,
    IDC_ARCHIVE_BROWSE,
    IDC_ARCHIVE_OPEN,
    IDC_DESTINATION,
    IDC_DESTINATION_BROWSE,
    IDC_PRESET,
    IDC_DICT,
    IDC_BLOCK,
    IDC_CODEC,
    IDC_THREADS,
    IDC_MEMORY,
    IDC_VOLUME,
    IDC_DEDUP,
    IDC_PRECOMP,
    IDC_OVERWRITE,
    IDC_SCAN,
    IDC_PACK,
    IDC_VERIFY,
    IDC_EXTRACT,
    IDC_LOG,
    IDC_PROGRESS,
    IDC_STATUS,
    IDC_SFX,
    IDC_AI
};

struct UiMessage {
    std::wstring text;
};

struct DoneMessage {
    bool succeeded = false;
    std::wstring text;
};

struct AppState {
    HWND window = nullptr;
    HWND source = nullptr;
    HWND archive = nullptr;
    HWND destination = nullptr;
    HWND preset = nullptr;
    HWND dictionary = nullptr;
    HWND block_size = nullptr;
    HWND codec = nullptr;
    HWND threads = nullptr;
    HWND memory_limit = nullptr;
    HWND volume_size = nullptr;
    HWND dedup = nullptr;
    HWND precomp = nullptr;
    HWND overwrite = nullptr;
    HWND scan = nullptr;
    HWND pack = nullptr;
    HWND verify = nullptr;
    HWND extract = nullptr;
    HWND log = nullptr;
    HWND progress = nullptr;
    HWND status = nullptr;
    HWND ai_button = nullptr;
    HFONT font = nullptr;
    HFONT title_font = nullptr;
    HFONT heading_font = nullptr;
    HFONT small_font = nullptr;
    HBRUSH background_brush = nullptr;
    HBRUSH edit_brush = nullptr;
    std::thread worker;
    std::atomic<bool> busy{false};
    bool close_after_work = false;
};

AppState* g_app = nullptr;

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                             static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return L"(message conversion failed)";
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                            result.data(), required) <= 0) return L"(message conversion failed)";
    return result;
}

std::wstring get_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1U, L'\0');
    if (length > 0) GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::filesystem::path get_path(HWND control) {
    const std::wstring value = get_text(control);
    if (value.empty()) throw std::runtime_error("A required path is empty.");
    return std::filesystem::path(value);
}

void append_log(const std::wstring& message) {
    if (g_app == nullptr || g_app->log == nullptr) return;
    const std::wstring line = message + L"\r\n";
    SendMessageW(g_app->log, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(g_app->log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_app->log, EM_SCROLLCARET, 0, 0);
}

void post_log(HWND window, const std::string& message) {
    auto* event = new UiMessage{utf8_to_wide(message)};
    if (!PostMessageW(window, WM_RPK_LOG, 0, reinterpret_cast<LPARAM>(event))) delete event;
}

void post_done(HWND window, bool succeeded, const std::string& message) {
    auto* event = new DoneMessage{succeeded, utf8_to_wide(message)};
    if (!PostMessageW(window, WM_RPK_DONE, 0, reinterpret_cast<LPARAM>(event))) delete event;
}

HWND add_control(HWND parent, const wchar_t* class_name, const wchar_t* text,
                 DWORD style, int x, int y, int width, int height, int id,
                 HFONT font, DWORD extended_style = 0) {
    HWND control = CreateWindowExW(extended_style, class_name, text, WS_CHILD | WS_VISIBLE | style,
                                   x, y, width, height, parent,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr && font != nullptr) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    return control;
}

void set_busy(bool value) {
    g_app->busy.store(value);
    if (value) {
        EnableWindow(g_app->scan, FALSE);
        EnableWindow(g_app->pack, FALSE);
        EnableWindow(g_app->verify, FALSE);
        EnableWindow(g_app->extract, FALSE);
        SendMessageW(g_app->progress, PBM_SETMARQUEE, TRUE, 35);
        SetWindowTextW(g_app->status, L"Working...");
    } else {
        EnableWindow(g_app->scan, TRUE);
        EnableWindow(g_app->pack, TRUE);
        EnableWindow(g_app->verify, TRUE);
        EnableWindow(g_app->extract, TRUE);
        SendMessageW(g_app->progress, PBM_SETMARQUEE, FALSE, 0);
        SetWindowTextW(g_app->status, L"Ready");
        SendMessageW(g_app->progress, PBM_SETPOS, 0, 0);
    }
}

void start_worker(const std::wstring& operation,
                  const std::function<void(HWND)>& task) {
    if (g_app->busy.load()) return;
    if (g_app->worker.joinable()) g_app->worker.join();
    const HWND window = g_app->window;
    set_busy(true);
    append_log(L"--- " + operation + L" ---");
    try {
        g_app->worker = std::thread([window, task] {
            try {
                task(window);
                post_done(window, true, "Operation completed successfully.");
            } catch (const std::exception& error) {
                post_done(window, false, error.what());
            } catch (...) {
                post_done(window, false, "Unknown error; the operation was stopped.");
            }
        });
    } catch (...) {
        set_busy(false);
        throw;
    }
}

void browse_folder(HWND target) {
    BROWSEINFOW info{};
    info.hwndOwner = g_app->window;
    info.lpszTitle = L"Select a folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX;
    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&info);
    if (selected == nullptr) return;
    wchar_t path[MAX_PATH]{};
    const BOOL ok = SHGetPathFromIDListW(selected, path);
    CoTaskMemFree(selected);
    if (ok) SetWindowTextW(target, path);
}

void browse_open_archive(HWND target) {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"RPK archives (*.rpk)\0*.rpk\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) SetWindowTextW(target, path);
}

void browse_save_sfx() {
    wchar_t path[MAX_PATH] = L"RPK-Setup.exe";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"Windows setup (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"exe";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&dialog)) {
        const auto archive = get_path(g_app->archive);
        wchar_t module[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr,module,MAX_PATH);
        if(length==0||length>=MAX_PATH) throw std::runtime_error("Cannot locate the RPK setup stub.");
        const auto stub=std::filesystem::path(module).parent_path()/L"rpk_setup.exe";
        const auto output=std::filesystem::path(path);
        const bool overwrite=SendMessageW(g_app->overwrite,BM_GETCHECK,0,0)==BST_CHECKED;
        start_worker(L"Self-extracting setup creation",[archive,stub,output,overwrite](HWND window){
            rpk::create_sfx_installer(archive,stub,output,overwrite,
                [window](const std::string& message){post_log(window,message);});
        });
    }
}

void launch_ai_assistant(HWND owner) {
    wchar_t module[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,module,MAX_PATH);
    if(length==0||length>=MAX_PATH) throw std::runtime_error("Cannot locate the RPK application folder.");
    const auto assistant=std::filesystem::path(module).parent_path()/L"rpk_ai.exe";
    if(!std::filesystem::is_regular_file(assistant))
        throw std::runtime_error("RPK AI Advisor is missing. Extract the complete RPK Windows ZIP.");
    const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",assistant.c_str(),nullptr,
                                                               assistant.parent_path().c_str(),SW_SHOWNORMAL));
    if(result<=32) throw std::runtime_error("Windows could not start RPK AI Advisor.");
}

void browse_save_archive(HWND target) {
    wchar_t path[MAX_PATH] = L"archive.rpk";
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_app->window;
    dialog.lpstrFilter = L"RPK archives (*.rpk)\0*.rpk\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"rpk";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
    if (GetSaveFileNameW(&dialog)) {
        SetWindowTextW(target, path);
        if (std::filesystem::exists(std::filesystem::path(path))) {
            SendMessageW(g_app->overwrite, BM_SETCHECK, BST_CHECKED, 0);
        }
    }
}

std::uint32_t read_unsigned_control(HWND control,
                                    std::uint32_t minimum, std::uint32_t maximum) {
    const std::wstring value = get_text(control);
    if (value.empty()) throw std::runtime_error("Enter a value for the compression setting.");
    wchar_t* end = nullptr;
    const unsigned long parsed = std::wcstoul(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != L'\0' || parsed < minimum || parsed > maximum) {
        throw std::runtime_error("Compression setting must be between " + std::to_string(minimum) +
                                 " and " + std::to_string(maximum) + ".");
    }
    return static_cast<std::uint32_t>(parsed);
}

void create_controls(HWND window) {
    constexpr COLORREF kBackground = RGB(243, 246, 251);
    constexpr COLORREF kInput = RGB(255, 255, 255);

    g_app->font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_app->title_font = CreateFontW(-30, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_app->heading_font = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_app->small_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_app->background_brush = CreateSolidBrush(kBackground);
    g_app->edit_brush = CreateSolidBrush(kInput);

    const HFONT font = g_app->font;
    auto label = [&](const wchar_t* text, int x, int y, int width, int height = 22) {
        add_control(window, L"STATIC", text, SS_LEFT, x, y, width, height, -1, font);
    };
    auto edit = [&](const wchar_t* text, int id, int x, int y, int width) {
        HWND control = add_control(window, L"EDIT", text,
                                   ES_AUTOHSCROLL | WS_TABSTOP,
                                   x, y, width, 32, id, font, WS_EX_CLIENTEDGE);
        if (control) SendMessageW(control, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                                  MAKELONG(8, 8));
        return control;
    };
    auto button = [&](const wchar_t* text, int id, int x, int y, int width, int height = 34) {
        return add_control(window, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP,
                           x, y, width, height, id, font);
    };

    g_app->ai_button = button(L"AI Assistant", IDC_AI, 840, 28, 190, 36);
    g_app->source = edit(L"", IDC_SOURCE, 164, 160, 760);
    g_app->archive = edit(L"", IDC_ARCHIVE, 164, 198, 605);
    g_app->destination = edit(L"", IDC_DESTINATION, 164, 568, 760);

    label(L"Source folder", 50, 165, 110);
    button(L"Browse", IDC_SOURCE_BROWSE, 940, 159, 92);
    label(L"Archive file", 50, 203, 110);
    button(L"Open", IDC_ARCHIVE_OPEN, 780, 197, 90);
    button(L"Save as", IDC_ARCHIVE_BROWSE, 880, 197, 152);

    label(L"Preset", 50, 315, 62);
    g_app->preset = add_control(window, L"COMBOBOX", L"",
                                CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                120, 309, 195, 180, IDC_PRESET, font, WS_EX_CLIENTEDGE);
    SendMessageW(g_app->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Fast"));
    SendMessageW(g_app->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced (recommended)"));
    SendMessageW(g_app->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Maximum"));
    SendMessageW(g_app->preset, CB_SETCURSEL, 1, 0);

    label(L"Balanced is a good choice for most folders.", 332, 315, 470);
    label(L"Dictionary", 50, 353, 85);
    g_app->dictionary = edit(L"64", IDC_DICT, 140, 347, 62);
    label(L"Block", 220, 353, 58);
    g_app->block_size = edit(L"64", IDC_BLOCK, 285, 347, 62);
    label(L"Codec", 370, 353, 45);
    g_app->codec = add_control(window, L"COMBOBOX", L"",
                               CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                               420, 347, 155, 150, IDC_CODEC, font, WS_EX_CLIENTEDGE);
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic"));
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"LZMA2"));
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Zstandard long"));
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Store"));
    SendMessageW(g_app->codec, CB_SETCURSEL, 0, 0);
    label(L"Threads", 600, 353, 105);
    g_app->threads = edit(L"0", IDC_THREADS, 708, 347, 58);
    label(L"Memory MiB", 790, 353, 78);
    g_app->memory_limit = edit(L"1024", IDC_MEMORY, 875, 347, 76);
    label(L"Volume", 960, 353, 48);
    g_app->volume_size = edit(L"0", IDC_VOLUME, 1008, 347, 60);

    g_app->dedup = add_control(window, L"BUTTON", L"Remove duplicate chunks",
                               BS_AUTOCHECKBOX | WS_TABSTOP, 50, 385, 220, 24,
                               IDC_DEDUP, font);
    SendMessageW(g_app->dedup, BM_SETCHECK, BST_CHECKED, 0);
    g_app->precomp = add_control(window, L"BUTTON", L"Exact-match zlib precompression",
                                 BS_AUTOCHECKBOX | WS_TABSTOP, 290, 385, 300, 24,
                                 IDC_PRECOMP, font);
    SendMessageW(g_app->precomp, BM_SETCHECK, BST_CHECKED, 0);
    g_app->overwrite = add_control(window, L"BUTTON", L"Replace existing files",
                                   BS_AUTOCHECKBOX | WS_TABSTOP, 630, 385, 230, 24,
                                   IDC_OVERWRITE, font);

    button(L"Analyze source", IDC_SCAN, 50, 467, 122, 36);
    button(L"Build archive", IDC_PACK, 185, 467, 138, 36);
    button(L"Verify archive", IDC_VERIFY, 336, 467, 128, 36);
    button(L"Extract archive", IDC_EXTRACT, 477, 467, 128, 36);
    button(L"Create setup", IDC_SFX, 618, 467, 128, 36);

    g_app->status = add_control(window, L"STATIC", L"Ready",
                                SS_LEFT, 710, 456, 310, 20, IDC_STATUS, font);
    g_app->progress = add_control(window, PROGRESS_CLASSW, L"",
                                  PBS_MARQUEE | PBS_SMOOTH, 710, 480, 310, 12,
                                  IDC_PROGRESS, font);
    SendMessageW(g_app->progress, PBM_SETMARQUEE, FALSE, 0);
    SendMessageW(g_app->progress, PBM_SETBKCOLOR, 0, RGB(231, 237, 246));
    SendMessageW(g_app->progress, PBM_SETBARCOLOR, 0, RGB(52, 120, 246));

    label(L"Extract to", 50, 573, 105);
    button(L"Browse", IDC_DESTINATION_BROWSE, 940, 567, 92);

    g_app->log = add_control(window, L"EDIT",
                             L"Ready. Choose a source folder, then build an archive or analyze it first.\r\n",
                             ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_TABSTOP,
                             50, 664, 980, 104, IDC_LOG, font, WS_EX_CLIENTEDGE);
    SendMessageW(g_app->log, EM_SETLIMITTEXT, 4U * 1024U * 1024U, 0);
    SendMessageW(g_app->log, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(8, 8));

    InvalidateRect(window, nullptr, TRUE);
}
void on_scan() {
    const auto source = get_path(g_app->source);
    start_worker(L"Source analysis", [source](HWND window) {
        post_log(window, "Scanning file types and estimating entropy from samples...");
        const auto report = rpk::scan_directory(source, [window](const std::string& message) { post_log(window, message); });
        std::ostringstream summary;
        summary << "Scan summary: " << report.total_files << " files, " << report.total_directories
                << " directories, " << report.total_bytes << " bytes total.\n";
        for (const auto& item : report.files_by_type) {
            summary << "  " << item.first << ": " << item.second << " files, "
                    << report.bytes_by_type.at(item.first) << " bytes\n";
        }
        for (const auto& file : report.files) {
            summary << "  " << std::fixed << std::setprecision(2) << file.entropy_bits_per_byte
                    << " bits/byte | " << file.classification << " | " << file.path << "\n";
        }
        post_log(window, summary.str());
    });
}

void on_pack() {
    const auto source = get_path(g_app->source);
    const auto archive = get_path(g_app->archive);
    rpk::PackOptions options;
    const LRESULT preset_index = SendMessageW(g_app->preset, CB_GETCURSEL, 0, 0);
    options.preset = preset_index == 0 ? 1U : (preset_index == 2 ? 9U : 6U);
    options.dictionary_mib = read_unsigned_control(g_app->dictionary, 1, 1024);
    const auto block_mib = read_unsigned_control(g_app->block_size, 1, 1024);
    options.block_size = static_cast<std::uint64_t>(block_mib) * 1024ULL * 1024ULL;
    const LRESULT codec_index = SendMessageW(g_app->codec, CB_GETCURSEL, 0, 0);
    options.codec = codec_index == 1 ? rpk::CodecMode::Lzma2 : (codec_index == 2 ? rpk::CodecMode::ZstdLong : (codec_index == 3 ? rpk::CodecMode::Store : rpk::CodecMode::Auto));
    options.threads = read_unsigned_control(g_app->threads, 0, 64);
    options.memory_limit_mib = read_unsigned_control(g_app->memory_limit, 128, 65536);
    options.volume_size_mib = read_unsigned_control(g_app->volume_size, 0, 4096);
    options.deduplicate = SendMessageW(g_app->dedup, BM_GETCHECK, 0, 0) == BST_CHECKED;
    options.precompress = SendMessageW(g_app->precomp, BM_GETCHECK, 0, 0) == BST_CHECKED;
    options.overwrite = SendMessageW(g_app->overwrite, BM_GETCHECK, 0, 0) == BST_CHECKED;
    start_worker(L"Archive build", [source, archive, options](HWND window) {
        rpk::pack_directory(source, archive, options, [window](const std::string& message) { post_log(window, message); });
    });
}

void on_verify() {
    const auto archive = get_path(g_app->archive);
    start_worker(L"Archive verification", [archive](HWND window) {
        rpk::verify_archive(archive, [window](const std::string& message) { post_log(window, message); });
        post_log(window, "All solid-block and restored-file SHA-256 checks passed.");
    });
}

void on_extract() {
    const auto archive = get_path(g_app->archive);
    const auto destination = get_path(g_app->destination);
    const bool overwrite = SendMessageW(g_app->overwrite, BM_GETCHECK, 0, 0) == BST_CHECKED;
    start_worker(L"Archive extraction", [archive, destination, overwrite](HWND window) {
        rpk::extract_archive(archive, destination, overwrite,
                             [window](const std::string& message) { post_log(window, message); });
        post_log(window, "Extraction completed. Restored files passed SHA-256 checks.");
    });
}


constexpr COLORREF kUiBackground = RGB(243, 246, 251);
constexpr COLORREF kUiCard = RGB(255, 255, 255);
constexpr COLORREF kUiBorder = RGB(226, 232, 241);
constexpr COLORREF kUiText = RGB(29, 41, 57);
constexpr COLORREF kUiMuted = RGB(100, 116, 139);
constexpr COLORREF kUiNavy = RGB(22, 36, 58);
constexpr COLORREF kUiBlue = RGB(52, 120, 246);

void draw_round_panel(HDC dc, int left, int top, int right, int bottom) {
    HBRUSH brush = CreateSolidBrush(kUiCard);
    HPEN pen = CreatePen(PS_SOLID, 1, kUiBorder);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, left, top, right, bottom, 16, 16);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void draw_section_title(HDC dc, const wchar_t* title, const wchar_t* subtitle,
                        int x, int y) {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kUiText);
    SelectObject(dc, g_app->heading_font);
    TextOutW(dc, x, y, title, static_cast<int>(wcslen(title)));
    if (subtitle && *subtitle) {
        SetTextColor(dc, kUiMuted);
        SelectObject(dc, g_app->small_font);
        TextOutW(dc, x + 150, y + 1, subtitle, static_cast<int>(wcslen(subtitle)));
    }
}

void paint_workspace(HWND window, HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    HBRUSH background = CreateSolidBrush(kUiBackground);
    FillRect(dc, &client, background);
    DeleteObject(background);

    RECT header{0, 0, client.right, 96};
    HBRUSH header_brush = CreateSolidBrush(kUiNavy);
    FillRect(dc, &header, header_brush);
    DeleteObject(header_brush);
    RECT accent{0, 93, client.right, 96};
    HBRUSH accent_brush = CreateSolidBrush(kUiBlue);
    FillRect(dc, &accent, accent_brush);
    DeleteObject(accent_brush);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    SelectObject(dc, g_app->title_font);
    TextOutW(dc, 30, 18, L"RPK Builder", 11);
    SelectObject(dc, g_app->small_font);
    SetTextColor(dc, RGB(205, 216, 231));
    const wchar_t* tagline = L"Create, verify, and restore compact archives";
    TextOutW(dc, 33, 62, tagline, static_cast<int>(wcslen(tagline)));

    draw_round_panel(dc, 24, 110, client.right - 24, 245);
    draw_round_panel(dc, 24, 260, client.right - 24, 420);
    draw_round_panel(dc, 24, 435, client.right - 24, 510);
    draw_round_panel(dc, 24, 526, client.right - 24, 610);
    draw_round_panel(dc, 24, 626, client.right - 24, client.bottom - 10);

    draw_section_title(dc, L"CREATE ARCHIVE", L"Choose what to compress and where to save it", 50, 126);
    draw_section_title(dc, L"COMPRESSION", L"Balanced settings are recommended; advanced tuning is available below", 50, 276);
    draw_section_title(dc, L"ACTIONS", L"Run a task using the paths above", 50, 449);
    draw_section_title(dc, L"RESTORE", L"Choose a folder for extracted files", 50, 539);
    draw_section_title(dc, L"ACTIVITY", L"Messages and verification results", 50, 639);
}

void draw_owner_button(const DRAWITEMSTRUCT& item) {
    const int id = static_cast<int>(item.CtlID);
    const bool primary = id == IDC_PACK;
    const bool header_action = id == IDC_AI;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    COLORREF fill = primary ? kUiBlue : (header_action ? RGB(39,58,88) : RGB(255,255,255));
    COLORREF border = primary ? kUiBlue : (header_action ? RGB(67,91,126) : kUiBorder);
    COLORREF text = (primary || header_action) ? RGB(255,255,255) : kUiText;

    if (disabled) {
        fill = RGB(236, 240, 246);
        border = RGB(228, 233, 241);
        text = RGB(151, 163, 180);
    } else if (pressed) {
        fill = primary ? RGB(35, 96, 205) : RGB(239, 244, 251);
        border = primary ? RGB(35, 96, 205) : RGB(203, 214, 229);
    }

    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_brush = SelectObject(item.hDC, brush);
    HGDIOBJ old_pen = SelectObject(item.hDC, pen);
    RECT rect = item.rcItem;
    InflateRect(&rect, -1, -1);
    RoundRect(item.hDC, rect.left, rect.top, rect.right, rect.bottom, 10, 10);
    SelectObject(item.hDC, old_brush);
    SelectObject(item.hDC, old_pen);
    DeleteObject(brush);
    DeleteObject(pen);

    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, text);
    SelectObject(item.hDC, g_app->font);
    wchar_t caption[128]{};
    GetWindowTextW(item.hwndItem, caption, static_cast<int>(std::size(caption)));
    DrawTextW(item.hDC, caption, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    if ((item.itemState & ODS_FOCUS) != 0 && !disabled) {
        RECT focus = rect;
        InflateRect(&focus, -5, -5);
        DrawFocusRect(item.hDC, &focus);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_app->window = window;
        create_controls(window);
        return 0;
    case WM_ERASEBKGND: {
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, g_app->background_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        paint_workspace(window, dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kUiMuted);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, kUiText);
        return reinterpret_cast<LRESULT>(g_app->edit_brush);
    }
    case WM_CTLCOLORBTN: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, kUiText);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, kUiText);
        return reinterpret_cast<LRESULT>(g_app->edit_brush);
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item && item->CtlType == ODT_BUTTON) {
            draw_owner_button(*item);
            return TRUE;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_COMMAND:
        try {
            switch (LOWORD(wparam)) {
            case IDC_SOURCE_BROWSE: browse_folder(g_app->source); return 0;
            case IDC_ARCHIVE_BROWSE: browse_save_archive(g_app->archive); return 0;
            case IDC_ARCHIVE_OPEN: browse_open_archive(g_app->archive); return 0;
            case IDC_DESTINATION_BROWSE: browse_folder(g_app->destination); return 0;
            case IDC_SCAN: on_scan(); return 0;
            case IDC_PACK: on_pack(); return 0;
            case IDC_VERIFY: on_verify(); return 0;
            case IDC_EXTRACT: on_extract(); return 0;
            case IDC_SFX: browse_save_sfx(); return 0;
            case IDC_AI: launch_ai_assistant(window); return 0;
            default: break;
            }
        } catch (const std::exception& error) {
            MessageBoxW(window, utf8_to_wide(error.what()).c_str(), L"RPK Builder", MB_OK | MB_ICONERROR);
        }
        return 0;
    case WM_RPK_LOG: {
        std::unique_ptr<UiMessage> event(reinterpret_cast<UiMessage*>(lparam));
        if (event) append_log(event->text);
        return 0;
    }
    case WM_RPK_DONE: {
        std::unique_ptr<DoneMessage> event(reinterpret_cast<DoneMessage*>(lparam));
        if (g_app->worker.joinable()) g_app->worker.join();
        set_busy(false);
        if (event) {
            append_log((event->succeeded ? L"SUCCESS: " : L"ERROR: ") + event->text);
            SetWindowTextW(g_app->status, event->succeeded ? L"Completed successfully" : L"Operation failed — see details");
            if (!event->succeeded) MessageBoxW(window, event->text.c_str(), L"RPK Builder", MB_OK | MB_ICONERROR);
        }
        if (g_app->close_after_work) PostMessageW(window, WM_CLOSE, 0, 0);
        return 0;
    }
    case WM_CLOSE:
        if (g_app->busy.load()) {
            const int answer = MessageBoxW(window,
                L"An archive operation is still running. RPK cannot safely interrupt this operation yet.\n\nWait for it to finish before closing?",
                L"Operation in progress", MB_YESNO | MB_ICONWARNING);
            if (answer == IDYES) g_app->close_after_work = true;
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if (g_app->worker.joinable()) g_app->worker.join();
        if (g_app->font) DeleteObject(g_app->font);
        if (g_app->title_font) DeleteObject(g_app->title_font);
        if (g_app->heading_font) DeleteObject(g_app->heading_font);
        if (g_app->small_font) DeleteObject(g_app->small_font);
        if (g_app->background_brush) DeleteObject(g_app->background_brush);
        if (g_app->edit_brush) DeleteObject(g_app->edit_brush);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
    INITCOMMONCONTROLSEX controls{};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&controls);
    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_initialized = SUCCEEDED(com_result);

    AppState app;
    g_app = &app;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        if (com_initialized) CoUninitialize();
        MessageBoxW(nullptr, L"Unable to register the RPK Builder window class.", L"RPK Builder", MB_OK | MB_ICONERROR);
        return 1;
    }

    const DWORD window_style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT window_rect{0, 0, 1080, 790};
    AdjustWindowRectEx(&window_rect, window_style, FALSE, 0);
    HWND window = CreateWindowExW(0, kWindowClass, L"RPK — Compression Workspace",
                                  window_style, CW_USEDEFAULT, CW_USEDEFAULT,
                                  window_rect.right - window_rect.left,
                                  window_rect.bottom - window_rect.top,
                                  nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        if (com_initialized) CoUninitialize();
        MessageBoxW(nullptr, L"Unable to create the RPK Builder window.", L"RPK Builder", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(window, show_command);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (com_initialized) CoUninitialize();
    g_app = nullptr;
    return static_cast<int>(message.wParam);
}
