#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>

#include "archive.hpp"
#include "scanner.hpp"
#include "sfx.hpp"
#include "jan_client.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iterator>
#include <memory>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"RPKBuilderDarkWindow";
constexpr UINT WM_RPK_LOG = WM_APP + 10;
constexpr UINT WM_RPK_DONE = WM_APP + 11;
constexpr UINT WM_RPK_OPTIMIZED = WM_APP + 12;

enum ControlId : int {
    IDC_SOURCE = 100, IDC_SOURCE_BROWSE, IDC_ARCHIVE, IDC_ARCHIVE_BROWSE,
    IDC_ARCHIVE_OPEN, IDC_DESTINATION, IDC_DESTINATION_BROWSE,
    IDC_PRESET, IDC_DICT, IDC_BLOCK, IDC_CODEC, IDC_THREADS, IDC_MEMORY,
    IDC_VOLUME, IDC_DEDUP, IDC_PRECOMP, IDC_OVERWRITE,
    IDC_SCAN, IDC_OPTIMIZE, IDC_PACK, IDC_VERIFY, IDC_EXTRACT, IDC_LOG,
    IDC_PROGRESS, IDC_STATUS, IDC_SFX, IDC_AI,
    IDC_LABEL_SOURCE=300, IDC_LABEL_ARCHIVE, IDC_LABEL_PRESET,
    IDC_LABEL_PRESET_NOTE, IDC_LABEL_DICT, IDC_LABEL_BLOCK, IDC_LABEL_CODEC,
    IDC_LABEL_THREADS, IDC_LABEL_MEMORY, IDC_LABEL_VOLUME, IDC_LABEL_DEST
};

struct UiMessage { std::wstring text; };
struct DoneMessage { bool succeeded=false; std::wstring text; };
struct OptimizeMessage {
    int preset_index=1;
    int codec_index=0;
    std::wstring dictionary=L"64";
    std::wstring block=L"64";
    std::wstring threads=L"0";
    std::wstring memory=L"1024";
    bool deduplicate=true;
    bool precompress=true;
    std::wstring explanation;
};

struct AppState {
    HWND window=nullptr;
    HWND source=nullptr, archive=nullptr, destination=nullptr;
    HWND preset=nullptr, dictionary=nullptr, block_size=nullptr, codec=nullptr;
    HWND threads=nullptr, memory_limit=nullptr, volume_size=nullptr;
    HWND dedup=nullptr, precomp=nullptr, overwrite=nullptr;
    HWND scan=nullptr, optimize=nullptr, pack=nullptr, verify=nullptr, extract=nullptr, sfx=nullptr;
    HWND log=nullptr, progress=nullptr, status=nullptr, ai_button=nullptr;
    HWND source_label=nullptr, archive_label=nullptr, preset_label=nullptr, preset_note=nullptr;
    HWND dictionary_label=nullptr, block_label=nullptr, codec_label=nullptr;
    HWND threads_label=nullptr, memory_label=nullptr, volume_label=nullptr, destination_label=nullptr;
    HFONT font=nullptr, title_font=nullptr, heading_font=nullptr, small_font=nullptr;
    HBRUSH background_brush=nullptr, card_brush=nullptr, edit_brush=nullptr;
    std::thread worker;
    std::atomic<bool> busy{false};
    bool close_after_work=false;
};
AppState* g_app=nullptr;

constexpr COLORREF kBackground=RGB(15,18,26);
constexpr COLORREF kCard=RGB(24,30,42);
constexpr COLORREF kCardBorder=RGB(42,51,68);
constexpr COLORREF kInput=RGB(18,23,33);
constexpr COLORREF kText=RGB(232,238,248);
constexpr COLORREF kMuted=RGB(157,171,192);
constexpr COLORREF kNavy=RGB(20,29,45);
constexpr COLORREF kBlue=RGB(63,126,255);
constexpr COLORREF kButton=RGB(34,44,61);

std::wstring utf8_to_wide(const std::string& value) {
    if(value.empty()) return {};
    const int required=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),nullptr,0);
    if(required<=0) return L"(message conversion failed)";
    std::wstring result(static_cast<std::size_t>(required),L'\0');
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),result.data(),required)<=0) return L"(message conversion failed)";
    return result;
}
std::string wide_to_utf8(const std::wstring& value) {
    if(value.empty()) return {};
    const int required=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(required<=0) throw std::runtime_error("Could not encode the Jan recommendation.");
    std::string result(static_cast<std::size_t>(required),'\0');
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),
        static_cast<int>(value.size()),result.data(),required,nullptr,nullptr)<=0)
        throw std::runtime_error("Could not encode the Jan recommendation.");
    return result;
}
std::wstring get_text(HWND control) {
    const int length=GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length)+1U,L'\0');
    if(length>0) GetWindowTextW(control,value.data(),length+1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}
std::filesystem::path get_path(HWND control) {
    const auto value=get_text(control);
    if(value.empty()) throw std::runtime_error("A required path is empty.");
    return std::filesystem::path(value);
}
void append_log(const std::wstring& message) {
    if(!g_app||!g_app->log) return;
    const std::wstring line=message+L"\r\n";
    SendMessageW(g_app->log,EM_SETSEL,static_cast<WPARAM>(-1),static_cast<LPARAM>(-1));
    SendMessageW(g_app->log,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g_app->log,EM_SCROLLCARET,0,0);
}
void post_log(HWND window,const std::string& message) {
    auto* event=new UiMessage{utf8_to_wide(message)};
    if(!PostMessageW(window,WM_RPK_LOG,0,reinterpret_cast<LPARAM>(event))) delete event;
}
void post_done(HWND window,bool succeeded,const std::string& message) {
    auto* event=new DoneMessage{succeeded,utf8_to_wide(message)};
    if(!PostMessageW(window,WM_RPK_DONE,0,reinterpret_cast<LPARAM>(event))) delete event;
}
HWND add_control(HWND parent,const wchar_t* klass,const wchar_t* text,DWORD style,
                 int x,int y,int width,int height,int id,HFONT font,DWORD ex=0) {
    HWND control=CreateWindowExW(ex,klass,text,WS_CHILD|WS_VISIBLE|style,
        x,y,width,height,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr),nullptr);
    if(control&&font) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    return control;
}
void set_dark_title(HWND window) {
    const BOOL enabled=TRUE;
    DwmSetWindowAttribute(window,20,&enabled,sizeof(enabled));
    DwmSetWindowAttribute(window,19,&enabled,sizeof(enabled));
}
void set_busy(bool value) {
    g_app->busy.store(value);
    for(HWND button:{g_app->scan,g_app->optimize,g_app->pack,g_app->verify,g_app->extract,g_app->sfx}) {
        if(button) EnableWindow(button,!value);
    }
    if(g_app->progress) SendMessageW(g_app->progress,PBM_SETMARQUEE,value?TRUE:FALSE,35);
    if(g_app->status) SetWindowTextW(g_app->status,value?L"Working…":L"Ready");
}
void start_worker(const std::wstring& operation,const std::function<void(HWND)>& task) {
    if(g_app->busy.load()) return;
    if(g_app->worker.joinable()) g_app->worker.join();
    const HWND window=g_app->window;
    set_busy(true);
    append_log(L"");
    append_log(L"— "+operation+L" —");
    try {
        g_app->worker=std::thread([window,task] {
            try { task(window); post_done(window,true,"Operation completed successfully."); }
            catch(const std::exception& error) { post_done(window,false,error.what()); }
            catch(...) { post_done(window,false,"Unknown error; the operation stopped."); }
        });
    } catch(...) { set_busy(false); throw; }
}
void browse_folder(HWND target) {
    BROWSEINFOW info{};info.hwndOwner=g_app->window;info.lpszTitle=L"Select a folder";
    info.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE|BIF_EDITBOX;
    PIDLIST_ABSOLUTE selected=SHBrowseForFolderW(&info);
    if(!selected) return;
    wchar_t path[MAX_PATH]{};
    const BOOL ok=SHGetPathFromIDListW(selected,path);
    CoTaskMemFree(selected);
    if(ok) SetWindowTextW(target,path);
}
void browse_open_archive(HWND target) {
    wchar_t path[32768]{};
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=g_app->window;
    dialog.lpstrFilter=L"RPK archive set (*.rpk;*.001) *.rpk;*.001 RPK archives (*.rpk) *.rpk All files (*.*) *.*  ";
    dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));
    dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    if(GetOpenFileNameW(&dialog)) SetWindowTextW(target,path);
}
void browse_save_archive(HWND target) {
    wchar_t path[32768]=L"archive.rpk";
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=g_app->window;
    dialog.lpstrFilter=L"RPK archives (*.rpk) *.rpk All files (*.*) *.*  ";
    dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt=L"rpk";dialog.Flags=OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
    if(GetSaveFileNameW(&dialog)) {
        SetWindowTextW(target,path);
        if(std::filesystem::exists(std::filesystem::path(path)))
            SendMessageW(g_app->overwrite,BM_SETCHECK,BST_CHECKED,0);
    }
}
void browse_save_sfx() {
    wchar_t path[32768]=L"RPK-Setup.exe";
    OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=g_app->window;
    dialog.lpstrFilter=L"Windows setup (*.exe) *.exe All files (*.*) *.*  ";
    dialog.lpstrFile=path;dialog.nMaxFile=static_cast<DWORD>(std::size(path));
    dialog.lpstrDefExt=L"exe";dialog.Flags=OFN_PATHMUSTEXIST|OFN_OVERWRITEPROMPT;
    if(!GetSaveFileNameW(&dialog)) return;
    const auto archive=get_path(g_app->archive);
    wchar_t module[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,module,MAX_PATH);
    if(length==0||length>=MAX_PATH) throw std::runtime_error("Cannot locate the RPK setup stub.");
    const auto stub=std::filesystem::path(module).parent_path()/L"rpk_setup.exe";
    const auto output=std::filesystem::path(path);
    const bool overwrite=SendMessageW(g_app->overwrite,BM_GETCHECK,0,0)==BST_CHECKED;
    start_worker(L"Create self-extracting setup",[archive,stub,output,overwrite](HWND window) {
        rpk::create_sfx_installer(archive,stub,output,overwrite,
            [window](const std::string& message){post_log(window,message);});
    });
}
void launch_ai_assistant(HWND owner) {
    wchar_t module[MAX_PATH]{};
    const DWORD length=GetModuleFileNameW(nullptr,module,MAX_PATH);
    if(length==0||length>=MAX_PATH) throw std::runtime_error("Cannot locate the RPK application folder.");
    const auto assistant=std::filesystem::path(module).parent_path()/L"rpk_ai.exe";
    if(!std::filesystem::is_regular_file(assistant))
        throw std::runtime_error("RPK AI Assistant is missing. Extract the complete RPK Windows ZIP.");
    const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",assistant.c_str(),nullptr,
        assistant.parent_path().c_str(),SW_SHOWNORMAL));
    if(result<=32) throw std::runtime_error("Windows could not start RPK AI Assistant.");
}
std::uint32_t read_unsigned_control(HWND control,std::uint32_t minimum,std::uint32_t maximum) {
    const auto value=get_text(control);
    if(value.empty()) throw std::runtime_error("Enter a value for the compression setting.");
    wchar_t* end=nullptr;const unsigned long parsed=std::wcstoul(value.c_str(),&end,10);
    if(end==value.c_str()||*end!=L'\0'||parsed<minimum||parsed>maximum)
        throw std::runtime_error("Compression setting must be between "+std::to_string(minimum)+" and "+std::to_string(maximum)+".");
    return static_cast<std::uint32_t>(parsed);
}
HWND make_label(HWND window,const wchar_t* caption,int id) {
    return add_control(window,L"STATIC",caption,SS_LEFT,0,0,10,10,id,g_app->font);
}
HWND make_edit(HWND window,const wchar_t* caption,int id) {
    HWND control=add_control(window,L"EDIT",caption,ES_AUTOHSCROLL|WS_TABSTOP,0,0,10,32,id,g_app->font,WS_EX_CLIENTEDGE);
    SendMessageW(control,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELONG(8,8));
    return control;
}
HWND make_button(HWND window,const wchar_t* caption,int id) {
    return add_control(window,L"BUTTON",caption,BS_OWNERDRAW|WS_TABSTOP,0,0,10,36,id,g_app->font);
}
void create_controls(HWND window) {
    g_app->font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    g_app->title_font=CreateFontW(-31,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    g_app->heading_font=CreateFontW(-14,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    g_app->small_font=CreateFontW(-13,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    g_app->background_brush=CreateSolidBrush(kBackground);
    g_app->card_brush=CreateSolidBrush(kCard);
    g_app->edit_brush=CreateSolidBrush(kInput);

    g_app->ai_button=make_button(window,L"AI Assistant — Jan",IDC_AI);
    g_app->source_label=make_label(window,L"Source folder",IDC_LABEL_SOURCE);
    g_app->source=make_edit(window,L"",IDC_SOURCE);
    g_app->archive_label=make_label(window,L"Archive file",IDC_LABEL_ARCHIVE);
    g_app->archive=make_edit(window,L"",IDC_ARCHIVE);
    g_app->preset_label=make_label(window,L"Preset",IDC_LABEL_PRESET);
    g_app->preset=add_control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,0,0,10,180,IDC_PRESET,g_app->font,WS_EX_CLIENTEDGE);
    SendMessageW(g_app->preset,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Fast — preset 1"));
    SendMessageW(g_app->preset,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Balanced — preset 6"));
    SendMessageW(g_app->preset,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Maximum — preset 9"));
    SendMessageW(g_app->preset,CB_SETCURSEL,1,0);
    g_app->preset_note=make_label(window,L"Auto mode measures representative bytes from each file group before selecting a codec.",IDC_LABEL_PRESET_NOTE);

    g_app->dictionary_label=make_label(window,L"Dictionary MiB",IDC_LABEL_DICT);
    g_app->dictionary=make_edit(window,L"64",IDC_DICT);
    g_app->block_label=make_label(window,L"Block MiB",IDC_LABEL_BLOCK);
    g_app->block_size=make_edit(window,L"64",IDC_BLOCK);
    g_app->codec_label=make_label(window,L"Codec",IDC_LABEL_CODEC);
    g_app->codec=add_control(window,L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_TABSTOP|WS_VSCROLL,0,0,10,140,IDC_CODEC,g_app->font,WS_EX_CLIENTEDGE);
    SendMessageW(g_app->codec,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Adaptive (recommended)"));
    SendMessageW(g_app->codec,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"LZMA2"));
    SendMessageW(g_app->codec,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Zstandard long"));
    SendMessageW(g_app->codec,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(L"Store"));
    SendMessageW(g_app->codec,CB_SETCURSEL,0,0);
    g_app->threads_label=make_label(window,L"Threads (0 = auto)",IDC_LABEL_THREADS);
    g_app->threads=make_edit(window,L"0",IDC_THREADS);
    g_app->memory_label=make_label(window,L"Memory MiB",IDC_LABEL_MEMORY);
    g_app->memory_limit=make_edit(window,L"1024",IDC_MEMORY);
    g_app->volume_label=make_label(window,L"Volume MiB (0 = off)",IDC_LABEL_VOLUME);
    g_app->volume_size=make_edit(window,L"0",IDC_VOLUME);

    g_app->dedup=add_control(window,L"BUTTON",L"Content deduplication",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,26,IDC_DEDUP,g_app->font);
    g_app->precomp=add_control(window,L"BUTTON",L"Exact-match zlib precompression",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,26,IDC_PRECOMP,g_app->font);
    g_app->overwrite=add_control(window,L"BUTTON",L"Replace existing files",BS_AUTOCHECKBOX|WS_TABSTOP,0,0,10,26,IDC_OVERWRITE,g_app->font);
    SendMessageW(g_app->dedup,BM_SETCHECK,BST_CHECKED,0);
    SendMessageW(g_app->precomp,BM_SETCHECK,BST_CHECKED,0);

    g_app->scan=make_button(window,L"Analyze source",IDC_SCAN);
    g_app->optimize=make_button(window,L"Optimize with Jan",IDC_OPTIMIZE);
    g_app->pack=make_button(window,L"Build archive",IDC_PACK);
    g_app->verify=make_button(window,L"Verify archive",IDC_VERIFY);
    g_app->extract=make_button(window,L"Extract archive",IDC_EXTRACT);
    g_app->sfx=make_button(window,L"Create setup",IDC_SFX);
    g_app->status=add_control(window,L"STATIC",L"Ready",SS_LEFT,0,0,10,22,IDC_STATUS,g_app->font);
    g_app->progress=add_control(window,PROGRESS_CLASSW,L"",PBS_MARQUEE|PBS_SMOOTH,0,0,10,12,IDC_PROGRESS,g_app->font);
    SendMessageW(g_app->progress,PBM_SETMARQUEE,FALSE,0);

    g_app->destination_label=make_label(window,L"Extract to",IDC_LABEL_DEST);
    g_app->destination=make_edit(window,L"",IDC_DESTINATION);
    add_control(window,L"BUTTON",L"Browse",BS_OWNERDRAW|WS_TABSTOP,0,0,10,36,IDC_DESTINATION_BROWSE,g_app->font);
    g_app->log=add_control(window,L"EDIT",
        L"Ready. Choose a source folder. Analyze it to see file types and entropy, or use Optimize with Jan for local model recommendations.\r\n",
        ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL|WS_TABSTOP,0,0,10,100,IDC_LOG,g_app->font,WS_EX_CLIENTEDGE);
    SendMessageW(g_app->log,EM_SETLIMITTEXT,4U*1024U*1024U,0);
    SendMessageW(g_app->log,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELONG(9,9));
    // Hidden until needed: browse and open actions are created as controls with their command IDs.
    HWND source_browse=add_control(window,L"BUTTON",L"Browse",BS_OWNERDRAW|WS_TABSTOP,0,0,10,36,IDC_SOURCE_BROWSE,g_app->font);
    (void)source_browse;
    HWND archive_open=add_control(window,L"BUTTON",L"Open",BS_OWNERDRAW|WS_TABSTOP,0,0,10,36,IDC_ARCHIVE_OPEN,g_app->font);
    (void)archive_open;
    HWND archive_save=add_control(window,L"BUTTON",L"Save as",BS_OWNERDRAW|WS_TABSTOP,0,0,10,36,IDC_ARCHIVE_BROWSE,g_app->font);
    (void)archive_save;
    layout_controls(window);
}
void layout_controls(HWND window) {
    if(!g_app||!g_app->source) return;
    RECT rc{};GetClientRect(window,&rc);
    const int w=rc.right,h=rc.bottom;
    const int m=28, right=w-m;
    MoveWindow(g_app->ai_button,std::max(m,w-245),23,210,40,TRUE);

    MoveWindow(g_app->source_label,52,164,110,24,TRUE);
    MoveWindow(g_app->source,170,155,std::max(280,w-170-m-120),34,TRUE);
    if(HWND b=GetDlgItem(window,IDC_SOURCE_BROWSE)) MoveWindow(b,right-105,155,105,34,TRUE);
    MoveWindow(g_app->archive_label,52,207,110,24,TRUE);
    MoveWindow(g_app->archive,170,198,std::max(250,w-170-m-260),34,TRUE);
    if(HWND b=GetDlgItem(window,IDC_ARCHIVE_OPEN)) MoveWindow(b,right-245,198,100,34,TRUE);
    if(HWND b=GetDlgItem(window,IDC_ARCHIVE_BROWSE)) MoveWindow(b,right-135,198,135,34,TRUE);

    MoveWindow(g_app->preset_label,52,327,70,24,TRUE);
    MoveWindow(g_app->preset,128,319,220,34,TRUE);
    MoveWindow(g_app->preset_note,366,327,std::max(300,w-405),24,TRUE);

    MoveWindow(g_app->dictionary_label,52,371,100,22,TRUE);
    MoveWindow(g_app->dictionary,52,392,95,32,TRUE);
    MoveWindow(g_app->block_label,174,371,95,22,TRUE);
    MoveWindow(g_app->block_size,174,392,95,32,TRUE);
    MoveWindow(g_app->codec_label,296,371,75,22,TRUE);
    MoveWindow(g_app->codec,296,392,192,34,TRUE);
    MoveWindow(g_app->threads_label,515,371,145,22,TRUE);
    MoveWindow(g_app->threads,515,392,100,32,TRUE);
    MoveWindow(g_app->memory_label,642,371,100,22,TRUE);
    MoveWindow(g_app->memory_limit,642,392,100,32,TRUE);
    MoveWindow(g_app->volume_label,770,371,150,22,TRUE);
    MoveWindow(g_app->volume_size,770,392,100,32,TRUE);

    MoveWindow(g_app->dedup,52,447,225,26,TRUE);
    MoveWindow(g_app->precomp,300,447,300,26,TRUE);
    MoveWindow(g_app->overwrite,630,447,250,26,TRUE);

    const int gap=10, count=6, available=w-2*m-gap*(count-1);
    const int button_width=std::max(120,available/count);
    const int y=550;
    const HWND buttons[]={g_app->scan,g_app->optimize,g_app->pack,g_app->verify,g_app->extract,g_app->sfx};
    for(int i=0;i<count;++i) MoveWindow(buttons[i],m+i*(button_width+gap),y,button_width,37,TRUE);
    MoveWindow(g_app->status,m,592,220,22,TRUE);
    MoveWindow(g_app->progress,m+232,596,std::max(180,w-2*m-232),12,TRUE);

    MoveWindow(g_app->destination_label,52,672,105,24,TRUE);
    MoveWindow(g_app->destination,170,662,std::max(280,w-170-m-120),34,TRUE);
    if(HWND b=GetDlgItem(window,IDC_DESTINATION_BROWSE)) MoveWindow(b,right-105,662,105,34,TRUE);

    const int activity_top=740;
    MoveWindow(g_app->log,52,780,w-104,std::max(70,h-800),TRUE);
    InvalidateRect(window,nullptr,TRUE);
}
void draw_round_panel(HDC dc,int left,int top,int right,int bottom) {
    HBRUSH brush=CreateSolidBrush(kCard);HPEN pen=CreatePen(PS_SOLID,1,kCardBorder);
    HGDIOBJ ob=SelectObject(dc,brush),op=SelectObject(dc,pen);
    RoundRect(dc,left,top,right,bottom,16,16);
    SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(brush);DeleteObject(pen);
}
void draw_section_title(HDC dc,const wchar_t* title,const wchar_t* subtitle,int x,int y) {
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,kText);SelectObject(dc,g_app->heading_font);
    TextOutW(dc,x,y,title,static_cast<int>(wcslen(title)));
    if(subtitle&&*subtitle){SetTextColor(dc,kMuted);SelectObject(dc,g_app->small_font);TextOutW(dc,x+170,y+1,subtitle,static_cast<int>(wcslen(subtitle)));}
}
void paint_workspace(HWND window,HDC dc) {
    RECT client{};GetClientRect(window,&client);
    FillRect(dc,&client,g_app->background_brush);
    RECT header{0,0,client.right,100};HBRUSH hb=CreateSolidBrush(kNavy);FillRect(dc,&header,hb);DeleteObject(hb);
    RECT accent{0,97,client.right,101};HBRUSH ab=CreateSolidBrush(kBlue);FillRect(dc,&accent,ab);DeleteObject(ab);
    SetBkMode(dc,TRANSPARENT);SetTextColor(dc,kText);SelectObject(dc,g_app->title_font);
    TextOutW(dc,30,17,L"RPK Builder",11);
    SetTextColor(dc,kMuted);SelectObject(dc,g_app->small_font);
    const wchar_t* subtitle=L"Lossless compression • adaptive codecs • verified restore";
    TextOutW(dc,33,62,subtitle,static_cast<int>(wcslen(subtitle)));

    const int w=client.right,h=client.bottom;
    draw_round_panel(dc,20,114,w-20,262);
    draw_round_panel(dc,20,278,w-20,494);
    draw_round_panel(dc,20,510,w-20,626);
    draw_round_panel(dc,20,638,w-20,724);
    draw_round_panel(dc,20,738,w-20,h-12);
    draw_section_title(dc,L"CREATE ARCHIVE",L"Choose your source folder and archive destination",50,130);
    draw_section_title(dc,L"COMPRESSION",L"Automatic mode tests representative data from each file type",50,294);
    draw_section_title(dc,L"ACTIONS",L"Analyze, optimize, build, verify, or restore",50,526);
    draw_section_title(dc,L"RESTORE",L"Choose where extracted files should be written",50,654);
    draw_section_title(dc,L"ACTIVITY",L"Live operation messages and verification results",50,754);
}
void draw_button(const DRAWITEMSTRUCT& item) {
    const int id=static_cast<int>(item.CtlID);
    const bool primary=id==IDC_PACK, assistant=id==IDC_AI, optimize=id==IDC_OPTIMIZE;
    const bool disabled=(item.itemState&ODS_DISABLED)!=0,pressed=(item.itemState&ODS_SELECTED)!=0;
    COLORREF fill=primary?kBlue:(assistant?RGB(39,58,88):(optimize?RGB(35,67,111):kButton));
    COLORREF border=primary?kBlue:(assistant?RGB(67,91,126):(optimize?RGB(64,105,168):kCardBorder));
    COLORREF fg=kText;
    if(disabled){fill=RGB(39,46,60);border=RGB(47,55,70);fg=RGB(112,124,143);}
    else if(pressed) fill=primary?RGB(45,105,230):RGB(44,56,76);
    HBRUSH brush=CreateSolidBrush(fill);HPEN pen=CreatePen(PS_SOLID,1,border);
    HGDIOBJ ob=SelectObject(item.hDC,brush),op=SelectObject(item.hDC,pen);
    RECT rc=item.rcItem;InflateRect(&rc,-1,-1);RoundRect(item.hDC,rc.left,rc.top,rc.right,rc.bottom,9,9);
    SelectObject(item.hDC,ob);SelectObject(item.hDC,op);DeleteObject(brush);DeleteObject(pen);
    wchar_t caption[128]{};GetWindowTextW(item.hwndItem,caption,static_cast<int>(std::size(caption)));
    SetBkMode(item.hDC,TRANSPARENT);SetTextColor(item.hDC,fg);SelectObject(item.hDC,g_app->font);
    DrawTextW(item.hDC,caption,-1,&rc,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
    if((item.itemState&ODS_FOCUS)&&!disabled){InflateRect(&rc,-5,-5);DrawFocusRect(item.hDC,&rc);}
}

std::optional<std::string> json_string_field(const std::string& json,const std::string& key) {
    const std::regex pattern("\\\""+key+"\\\"\\s*:\\s*\\\"([A-Za-z0-9_-]+)\\\"",
                             std::regex_constants::icase);
    std::smatch match;if(!std::regex_search(json,match,pattern))return std::nullopt;
    return match[1].str();
}
std::optional<std::uint32_t> json_number_field(const std::string& json,const std::string& key) {
    const std::regex pattern("\\\""+key+"\\\"\\s*:\\s*([0-9]+)");
    std::smatch match;if(!std::regex_search(json,match,pattern))return std::nullopt;
    try {const auto number=std::stoul(match[1].str());if(number>UINT32_MAX)return std::nullopt;return static_cast<std::uint32_t>(number);}
    catch(...){return std::nullopt;}
}
std::optional<bool> json_bool_field(const std::string& json,const std::string& key) {
    const std::regex pattern("\\\""+key+"\\\"\\s*:\\s*(true|false)",std::regex_constants::icase);
    std::smatch match;if(!std::regex_search(json,match,pattern))return std::nullopt;
    return match[1].str()=="true"||match[1].str()=="TRUE";
}
std::string codec_name(std::size_t index) {
    if(index==1)return "lzma2";if(index==2)return "zstd-long";if(index==3)return "store";return "auto";
}
OptimizeMessage parse_optimization(const std::wstring& response) {
    const std::string json=wide_to_utf8(response);
    const auto preset=json_number_field(json,"preset");
    const auto dict=json_number_field(json,"dictionary_mib");
    const auto block=json_number_field(json,"block_size_mib");
    const auto threads=json_number_field(json,"threads");
    const auto memory=json_number_field(json,"memory_mib");
    const auto codec=json_string_field(json,"codec");
    const auto dedup=json_bool_field(json,"deduplicate");
    const auto precomp=json_bool_field(json,"precompress");
    if(!preset||!dict||!block||!threads||!memory||!codec||!dedup||!precomp)
        throw std::runtime_error("Jan's answer did not include all required settings. Try Optimize with Jan again.");
    if(*preset!=1U&&*preset!=6U&&*preset!=9U)throw std::runtime_error("Jan suggested an unsupported LZMA preset; no settings were applied.");
    if(*dict<1U||*dict>1024U||*block<1U||*block>1024U||*threads>64U||*memory<128U||*memory>65536U)
        throw std::runtime_error("Jan suggested settings outside RPK's allowed limits; no settings were applied.");
    if(*memory<=*block+*dict+128U)throw std::runtime_error("Jan suggested a memory limit too small for its block and dictionary sizes; no settings were applied.");
    OptimizeMessage result;
    result.preset_index=*preset==1U?0:(*preset==9U?2:1);
    result.dictionary=std::to_wstring(*dict);
    result.block=std::to_wstring(*block);
    result.threads=std::to_wstring(*threads);
    result.memory=std::to_wstring(*memory);
    result.deduplicate=*dedup;result.precompress=*precomp;
    if(*codec=="lzma2")result.codec_index=1;
    else if(*codec=="zstd-long")result.codec_index=2;
    else if(*codec=="store")result.codec_index=3;
    else if(*codec=="auto")result.codec_index=0;
    else throw std::runtime_error("Jan suggested an unknown codec; no settings were applied.");
    result.explanation=response;
    return result;
}

void on_scan() {
    const auto source=get_path(g_app->source);
    start_worker(L"Analyze source", [source](HWND window) {
        post_log(window,"Scanning file types and sampling entropy. File contents are not sent to Jan by this scan.");
        const auto report=rpk::scan_directory(source,[window](const std::string& message){post_log(window,message);});
        std::ostringstream summary;
        summary<<"Scan: "<<report.total_files<<" files, "<<report.total_directories
            <<" folders, "<<report.total_bytes<<" bytes total.\n";
        for(const auto& item:report.files_by_type) {
            summary<<"  "<<item.first<<": "<<item.second<<" files, "
                <<report.bytes_by_type.at(item.first)<<" bytes\n";
        }
        for(const auto& file:report.files) {
            summary<<std::fixed<<std::setprecision(2)<<file.entropy_bits_per_byte
                <<" bits/byte | "<<file.classification<<" | "<<file.path<<"\n";
        }
        post_log(window,summary.str());
    });
}
void on_optimize() {
    const auto source=get_path(g_app->source);
    const auto current_preset=SendMessageW(g_app->preset,CB_GETCURSEL,0,0);
    const auto current_codec=SendMessageW(g_app->codec,CB_GETCURSEL,0,0);
    const auto current_dict=get_text(g_app->dictionary);
    const auto current_block=get_text(g_app->block_size);
    const auto current_threads=get_text(g_app->threads);
    const auto current_memory=get_text(g_app->memory_limit);
    const bool current_dedup=SendMessageW(g_app->dedup,BM_GETCHECK,0,0)==BST_CHECKED;
    const bool current_precomp=SendMessageW(g_app->precomp,BM_GETCHECK,0,0)==BST_CHECKED;
    start_worker(L"Local Jan compression optimization",[=](HWND window) {
        post_log(window,"Scanning the selected folder and building an aggregate profile for Jan...");
        const auto report=rpk::scan_directory(source,[window](const std::string& message){post_log(window,message);});
        if(report.total_files==0)throw std::runtime_error("The source folder contains no files to optimize.");
        auto settings=rpk::jan::load_settings();
        if(settings.model.empty()) {
            const auto models=rpk::jan::list_models(settings);
            settings.model=models.front();
            rpk::jan::save_settings(settings);
        }
        std::ostringstream prompt;
        prompt<<"RPK source profile (aggregate metadata only; no file contents or file names are included):\n"
            <<"Files: "<<report.total_files<<"\nDirectories: "<<report.total_directories
            <<"\nTotal source bytes: "<<report.total_bytes<<"\nType distribution:\n";
        std::unordered_map<std::string,std::pair<double,std::uint64_t>> entropy;
        for(const auto& file:report.files) {
            auto& item=entropy[file.classification];
            item.first+=file.entropy_bits_per_byte;++item.second;
        }
        for(const auto& item:report.files_by_type) {
            const auto ent=entropy[item.first];
            const double average=ent.second?ent.first/static_cast<double>(ent.second):0.0;
            prompt<<"- "<<item.first<<": "<<item.second<<" files, "<<report.bytes_by_type.at(item.first)
                <<" bytes, average sampled entropy "<<std::fixed<<std::setprecision(2)<<average<<" bits/byte\n";
        }
        prompt<<"Current RPK settings: preset="<<(current_preset==0?1:(current_preset==2?9:6))
            <<", dictionary_mib="<<current_dict<<", block_size_mib="<<current_block
            <<", codec="<<codec_name(static_cast<std::size_t>(std::max<LRESULT>(0,current_codec)))
            <<", threads="<<current_threads<<", memory_mib="<<current_memory
            <<", deduplicate="<<(current_dedup?"true":"false")
            <<", precompress="<<(current_precomp?"true":"false")<<"\n\n";
        prompt<<"Recommend settings intended to improve compression ratio while preserving byte-identical restoration. "
            <<"Prefer codec=auto because RPK now trial-compresses representative bytes from each group with LZMA2 and Zstandard and stores the group if neither beats Store. "
            <<"Do not recommend changing source files, disabling checksums, or lossy transforms. "
            <<"Select conservative settings compatible with a typical 16-32 GiB Windows PC. "
            <<"Return ONLY one JSON object with these exact keys: "
            <<"{\"preset\":1|6|9,\"dictionary_mib\":integer 1..256,\"block_size_mib\":integer 16..256,"
            <<"\"codec\":\"auto\"| \"lzma2\"| \"zstd-long\"| \"store\",\"threads\":0..16,"
            <<"\"memory_mib\":512..8192,\"deduplicate\":true|false,\"precompress\":true|false}. "
            <<"Memory must exceed block_size_mib + dictionary_mib + 128. Do not include markdown fences.";
        const std::wstring system=
            L"You are RPK's local compression optimizer. You recommend only validated settings for a lossless archive application. "
            L"Use the supplied aggregate file profile and current settings; do not invent benchmark results. "
            L"Do not ask for or reproduce file contents. Follow the exact JSON schema requested and include no other text.";
        post_log(window,"Sending aggregate file statistics to the selected Jan local model. No files are uploaded or sent to a cloud service.");
        const std::wstring answer=rpk::jan::chat_completion(settings,system,utf8_to_wide(prompt.str()),1400);
        const auto recommendation=parse_optimization(answer);
        auto* result=new OptimizeMessage(recommendation);
        if(!PostMessageW(window,WM_RPK_OPTIMIZED,0,reinterpret_cast<LPARAM>(result)))delete result;
        post_log(window,"Jan recommendation received and validated. The selected compression settings were updated.");
        post_log(window,wide_to_utf8(L"Model recommendation: "+answer));
    });
}
void on_pack() {
    const auto source=get_path(g_app->source),archive=get_path(g_app->archive);
    rpk::PackOptions options;
    const LRESULT preset=SendMessageW(g_app->preset,CB_GETCURSEL,0,0);
    options.preset=preset==0?1U:(preset==2?9U:6U);
    options.dictionary_mib=read_unsigned_control(g_app->dictionary,1,1024);
    options.block_size=static_cast<std::uint64_t>(read_unsigned_control(g_app->block_size,1,1024))*1024ULL*1024ULL;
    const LRESULT codec=SendMessageW(g_app->codec,CB_GETCURSEL,0,0);
    options.codec=codec==1?rpk::CodecMode::Lzma2:(codec==2?rpk::CodecMode::ZstdLong:(codec==3?rpk::CodecMode::Store:rpk::CodecMode::Auto));
    options.threads=read_unsigned_control(g_app->threads,0,64);
    options.memory_limit_mib=read_unsigned_control(g_app->memory_limit,128,65536);
    options.volume_size_mib=read_unsigned_control(g_app->volume_size,0,4096);
    options.deduplicate=SendMessageW(g_app->dedup,BM_GETCHECK,0,0)==BST_CHECKED;
    options.precompress=SendMessageW(g_app->precomp,BM_GETCHECK,0,0)==BST_CHECKED;
    options.overwrite=SendMessageW(g_app->overwrite,BM_GETCHECK,0,0)==BST_CHECKED;
    start_worker(L"Build lossless archive",[source,archive,options](HWND window) {
        rpk::pack_directory(source,archive,options,[window](const std::string& message){post_log(window,message);});
    });
}
void on_verify() {
    const auto archive=get_path(g_app->archive);
    start_worker(L"Verify archive",[archive](HWND window) {
        rpk::verify_archive(archive,[window](const std::string& message){post_log(window,message);});
        post_log(window,"All solid-block and restored-file SHA-256 checks passed.");
    });
}
void on_extract() {
    const auto archive=get_path(g_app->archive),destination=get_path(g_app->destination);
    const bool overwrite=SendMessageW(g_app->overwrite,BM_GETCHECK,0,0)==BST_CHECKED;
    start_worker(L"Extract archive",[archive,destination,overwrite](HWND window) {
        rpk::extract_archive(archive,destination,overwrite,[window](const std::string& message){post_log(window,message);});
        post_log(window,"Extraction finished. Restored files passed SHA-256 checks.");
    });
}

LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    switch(message) {
    case WM_CREATE:
        g_app->window=window;create_controls(window);set_dark_title(window);return 0;
    case WM_SIZE:
        layout_controls(window);InvalidateRect(window,nullptr,TRUE);return 0;
    case WM_GETMINMAXINFO:{
        auto* info=reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize.x=1120;info->ptMinTrackSize.y=850;return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{
        PAINTSTRUCT ps{};HDC dc=BeginPaint(window,&ps);paint_workspace(window,dc);EndPaint(window,&ps);return 0;}
    case WM_CTLCOLORSTATIC:{
        HDC dc=reinterpret_cast<HDC>(wp);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,kMuted);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));}
    case WM_CTLCOLOREDIT:{
        HDC dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,kInput);SetTextColor(dc,kText);
        return reinterpret_cast<LRESULT>(g_app->edit_brush);}
    case WM_CTLCOLORBTN:{
        HDC dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,kCard);SetTextColor(dc,kText);
        return reinterpret_cast<LRESULT>(g_app->card_brush);}
    case WM_CTLCOLORLISTBOX:{
        HDC dc=reinterpret_cast<HDC>(wp);SetBkColor(dc,kInput);SetTextColor(dc,kText);
        return reinterpret_cast<LRESULT>(g_app->edit_brush);}
    case WM_DRAWITEM:{
        const auto* item=reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if(item&&item->CtlType==ODT_BUTTON){draw_button(*item);return TRUE;}
        return DefWindowProcW(window,message,wp,lp);}
    case WM_COMMAND:
        try {
            switch(LOWORD(wp)) {
            case IDC_SOURCE_BROWSE:browse_folder(g_app->source);return 0;
            case IDC_ARCHIVE_BROWSE:browse_save_archive(g_app->archive);return 0;
            case IDC_ARCHIVE_OPEN:browse_open_archive(g_app->archive);return 0;
            case IDC_DESTINATION_BROWSE:browse_folder(g_app->destination);return 0;
            case IDC_SCAN:on_scan();return 0;
            case IDC_OPTIMIZE:on_optimize();return 0;
            case IDC_PACK:on_pack();return 0;
            case IDC_VERIFY:on_verify();return 0;
            case IDC_EXTRACT:on_extract();return 0;
            case IDC_SFX:browse_save_sfx();return 0;
            case IDC_AI:launch_ai_assistant(window);return 0;
            default:break;
            }
        } catch(const std::exception& error) {
            MessageBoxW(window,utf8_to_wide(error.what()).c_str(),L"RPK Builder",MB_OK|MB_ICONERROR);
        }
        return 0;
    case WM_RPK_LOG:{
        std::unique_ptr<UiMessage> event(reinterpret_cast<UiMessage*>(lp));
        if(event)append_log(event->text);return 0;}
    case WM_RPK_OPTIMIZED:{
        std::unique_ptr<OptimizeMessage> event(reinterpret_cast<OptimizeMessage*>(lp));
        if(event) {
            SendMessageW(g_app->preset,CB_SETCURSEL,event->preset_index,0);
            SetWindowTextW(g_app->dictionary,event->dictionary.c_str());
            SetWindowTextW(g_app->block_size,event->block.c_str());
            SendMessageW(g_app->codec,CB_SETCURSEL,event->codec_index,0);
            SetWindowTextW(g_app->threads,event->threads.c_str());
            SetWindowTextW(g_app->memory_limit,event->memory.c_str());
            SendMessageW(g_app->dedup,BM_SETCHECK,event->deduplicate?BST_CHECKED:BST_UNCHECKED,0);
            SendMessageW(g_app->precomp,BM_SETCHECK,event->precompress?BST_CHECKED:BST_UNCHECKED,0);
            append_log(L"Validated Jan compression profile has been applied to the controls.");
            append_log(L"Review the suggested settings before building the archive.");
        }
        return 0;}
    case WM_RPK_DONE:{
        std::unique_ptr<DoneMessage> event(reinterpret_cast<DoneMessage*>(lp));
        if(g_app->worker.joinable())g_app->worker.join();
        set_busy(false);
        if(event) {
            append_log((event->succeeded?L"SUCCESS: ":L"ERROR: ")+event->text);
            SetWindowTextW(g_app->status,event->succeeded?L"Completed successfully":L"Operation failed — see activity log");
            if(!event->succeeded)MessageBoxW(window,event->text.c_str(),L"RPK Builder",MB_OK|MB_ICONERROR);
        }
        if(g_app->close_after_work)PostMessageW(window,WM_CLOSE,0,0);
        return 0;}
    case WM_CLOSE:
        if(g_app->busy.load()) {
            const int answer=MessageBoxW(window,L"An operation is still running. Close RPK after it finishes?",
                L"Operation in progress",MB_YESNO|MB_ICONWARNING);
            if(answer==IDYES)g_app->close_after_work=true;return 0;
        }
        DestroyWindow(window);return 0;
    case WM_DESTROY:
        if(g_app->worker.joinable())g_app->worker.join();
        for(HFONT font:{g_app->font,g_app->title_font,g_app->heading_font,g_app->small_font})if(font)DeleteObject(font);
        for(HBRUSH brush:{g_app->background_brush,g_app->card_brush,g_app->edit_brush})if(brush)DeleteObject(brush);
        PostQuitMessage(0);return 0;
    default:return DefWindowProcW(window,message,wp,lp);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS|ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const bool com_initialized=SUCCEEDED(com);

    AppState app;g_app=&app;
    WNDCLASSEXW cls{};cls.cbSize=sizeof(cls);cls.lpfnWndProc=window_proc;cls.hInstance=instance;
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
    cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);cls.lpszClassName=kWindowClass;
    if(!RegisterClassExW(&cls)) {
        if(com_initialized)CoUninitialize();
        MessageBoxW(nullptr,L"Unable to register the RPK Builder window class.",L"RPK Builder",MB_OK|MB_ICONERROR);
        return 1;
    }
    const DWORD style=WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN;
    RECT rect{0,0,1380,970};AdjustWindowRectEx(&rect,style,FALSE,0);
    HWND window=CreateWindowExW(0,kWindowClass,L"RPK - Compression Workspace",style,
        CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,
        nullptr,nullptr,instance,nullptr);
    if(!window) {
        if(com_initialized)CoUninitialize();
        MessageBoxW(nullptr,L"Unable to create the RPK Builder window.",L"RPK Builder",MB_OK|MB_ICONERROR);
        return 1;
    }
    ShowWindow(window,show);UpdateWindow(window);
    MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);}
    if(com_initialized)CoUninitialize();
    g_app=nullptr;
    return static_cast<int>(msg.wParam);
}
