#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include "archive.hpp"
#include "scanner.hpp"

#include <atomic>
#include <cwchar>
#include <cstdint>
#include <filesystem>
#include <iomanip>
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
    IDC_DEDUP,
    IDC_PRECOMP,
    IDC_OVERWRITE,
    IDC_SCAN,
    IDC_PACK,
    IDC_VERIFY,
    IDC_EXTRACT,
    IDC_LOG,
    IDC_PROGRESS,
    IDC_STATUS
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
    HFONT font = nullptr;
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
    g_app->font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    const HFONT font = g_app->font;

    add_control(window, L"STATIC", L"RPK Builder", SS_LEFT, 18, 12, 300, 28, -1, font);
    add_control(window, L"STATIC", L"Milestone 4: type-aware LZMA2 / Zstandard long-range / Store with parallel block compression", SS_LEFT,
                18, 40, 940, 22, -1, font);

    add_control(window, L"STATIC", L"Source folder", SS_LEFT, 18, 78, 110, 22, -1, font);
    g_app->source = add_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 132, 74, 710, 26, IDC_SOURCE, font);
    add_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, 852, 74, 100, 26, IDC_SOURCE_BROWSE, font);

    add_control(window, L"STATIC", L"Archive file", SS_LEFT, 18, 114, 110, 22, -1, font);
    g_app->archive = add_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 132, 110, 590, 26, IDC_ARCHIVE, font);
    add_control(window, L"BUTTON", L"Open...", BS_PUSHBUTTON | WS_TABSTOP, 732, 110, 100, 26, IDC_ARCHIVE_OPEN, font);
    add_control(window, L"BUTTON", L"Save as...", BS_PUSHBUTTON | WS_TABSTOP, 842, 110, 110, 26, IDC_ARCHIVE_BROWSE, font);

    add_control(window, L"STATIC", L"Preset", SS_LEFT, 18, 154, 60, 22, -1, font);
    g_app->preset = add_control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                                80, 150, 150, 180, IDC_PRESET, font);
    SendMessageW(g_app->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Fast"));
    SendMessageW(g_app->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Balanced"));
    SendMessageW(g_app->preset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Maximum"));
    SendMessageW(g_app->preset, CB_SETCURSEL, 1, 0);

    add_control(window, L"STATIC", L"Dictionary MiB", SS_LEFT, 250, 154, 105, 22, -1, font);
    g_app->dictionary = add_control(window, L"EDIT", L"64", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER,
                                    360, 150, 68, 26, IDC_DICT, font);
    add_control(window, L"STATIC", L"Block MiB", SS_LEFT, 450, 154, 76, 22, -1, font);
    g_app->block_size = add_control(window, L"EDIT", L"64", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER,
                                    530, 150, 68, 26, IDC_BLOCK, font);
    add_control(window, L"STATIC", L"Codec", SS_LEFT, 610, 154, 42, 22, -1, font);
    g_app->codec = add_control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL,
                               655, 150, 150, 150, IDC_CODEC, font);
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto by file type"));
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"LZMA2"));
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Zstandard long"));
    SendMessageW(g_app->codec, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Store"));
    SendMessageW(g_app->codec, CB_SETCURSEL, 0, 0);

    add_control(window, L"STATIC", L"Threads (0 = auto)", SS_LEFT, 18, 185, 115, 22, -1, font);
    g_app->threads = add_control(window, L"EDIT", L"0", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER,
                                 136, 181, 58, 26, IDC_THREADS, font);
    add_control(window, L"STATIC", L"Memory MiB", SS_LEFT, 215, 185, 82, 22, -1, font);
    g_app->memory_limit = add_control(window, L"EDIT", L"1024", ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER,
                                      302, 181, 72, 26, IDC_MEMORY, font);
    g_app->dedup = add_control(window, L"BUTTON", L"Content-defined deduplication",
                               BS_AUTOCHECKBOX | WS_TABSTOP, 410, 181, 230, 24, IDC_DEDUP, font);
    SendMessageW(g_app->dedup, BM_SETCHECK, BST_CHECKED, 0);
    g_app->precomp = add_control(window, L"BUTTON", L"Exact-match embedded zlib precompression", BS_AUTOCHECKBOX | WS_TABSTOP,
                                 18, 214, 430, 24, IDC_PRECOMP, font);
    SendMessageW(g_app->precomp, BM_SETCHECK, BST_CHECKED, 0);
    g_app->overwrite = add_control(window, L"BUTTON", L"Overwrite existing archive/files", BS_AUTOCHECKBOX | WS_TABSTOP,
                                   620, 214, 330, 24, IDC_OVERWRITE, font);

    g_app->scan = add_control(window, L"BUTTON", L"Analyze source", BS_PUSHBUTTON | WS_TABSTOP,
                              18, 247, 145, 34, IDC_SCAN, font);
    g_app->pack = add_control(window, L"BUTTON", L"Build archive", BS_DEFPUSHBUTTON | WS_TABSTOP,
                              175, 247, 145, 34, IDC_PACK, font);
    g_app->verify = add_control(window, L"BUTTON", L"Verify archive", BS_PUSHBUTTON | WS_TABSTOP,
                                332, 247, 145, 34, IDC_VERIFY, font);

    add_control(window, L"STATIC", L"Extract destination", SS_LEFT, 18, 297, 110, 22, -1, font);
    g_app->destination = add_control(window, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER,
                                     132, 293, 710, 26, IDC_DESTINATION, font);
    add_control(window, L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP,
                852, 293, 100, 26, IDC_DESTINATION_BROWSE, font);
    g_app->extract = add_control(window, L"BUTTON", L"Extract archive", BS_PUSHBUTTON | WS_TABSTOP,
                                 18, 331, 145, 34, IDC_EXTRACT, font);

    g_app->status = add_control(window, L"STATIC", L"Ready", SS_LEFT, 180, 338, 300, 20, IDC_STATUS, font);
    g_app->progress = add_control(window, PROGRESS_CLASSW, L"", PBS_MARQUEE, 18, 377, 934, 18, IDC_PROGRESS, font);
    SendMessageW(g_app->progress, PBM_SETMARQUEE, FALSE, 0);

    add_control(window, L"STATIC", L"Activity log", SS_LEFT, 18, 408, 200, 22, -1, font);
    g_app->log = add_control(window, L"EDIT", L"Ready. Select a source folder to scan or build an archive.\r\n",
                             ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL | WS_BORDER,
                             18, 432, 934, 300, IDC_LOG, font);
    SendMessageW(g_app->log, EM_SETLIMITTEXT, 4U * 1024U * 1024U, 0);
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

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        g_app->window = window;
        create_controls(window);
        return 0;
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
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        if (com_initialized) CoUninitialize();
        MessageBoxW(nullptr, L"Unable to register the RPK Builder window class.", L"RPK Builder", MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND window = CreateWindowExW(0, kWindowClass, L"RPK Builder — LZMA2 + Zstandard + Deduplication",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 990, 810,
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
