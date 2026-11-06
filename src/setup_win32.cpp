#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>

#include "archive.hpp"
#include "sfx.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr wchar_t kWindowClass[] = L"RPKSfxSetupWindow";
constexpr UINT WM_SETUP_LOG = WM_APP + 30;
constexpr UINT WM_SETUP_DONE = WM_APP + 31;
enum ControlId { IDC_PATH = 700, IDC_BROWSE, IDC_INSTALL, IDC_OPEN, IDC_OVERWRITE, IDC_PROGRESS, IDC_STATUS, IDC_LOG };

struct LogEvent { std::wstring text; };
struct DoneEvent { bool success; std::wstring text; };

struct App {
    HWND window=nullptr, path=nullptr, browse=nullptr, install=nullptr, open=nullptr;
    HWND overwrite=nullptr, progress=nullptr, status=nullptr, log=nullptr;
    HFONT font=nullptr, title=nullptr, heading=nullptr;
    HBRUSH input=nullptr;
    std::filesystem::path embedded_archive, installed_folder;
    std::thread worker;
    bool busy=false, close_after=false;
};
App* g=nullptr;

std::wstring from_utf8(const std::string& value) {
    if(value.empty()) return {};
    const int length=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(length<=0) return L"Could not convert the status message.";
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length);
    return result;
}
std::wstring get_text(HWND control) {
    const int length=GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(length)+1U,L'\0');
    if(length>0) GetWindowTextW(control,result.data(),length+1);
    result.resize(static_cast<std::size_t>(length));
    return result;
}
std::uint64_t read_u64(std::istream& input) {
    std::array<std::uint8_t,8> bytes{};
    input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    if(!input) throw std::runtime_error("The SFX footer is incomplete.");
    std::uint64_t value=0;
    for(unsigned int i=0;i<8;++i) value|=static_cast<std::uint64_t>(bytes[i])<<(i*8U);
    return value;
}
std::filesystem::path extract_embedded_archive(const std::filesystem::path& executable) {
    std::error_code ec;
    const auto file_size=std::filesystem::file_size(executable,ec);
    if(ec||file_size<rpk::kSfxFooterSize) throw std::runtime_error("This executable has no RPK archive payload.");

    std::ifstream input(executable,std::ios::binary);
    if(!input) throw std::runtime_error("Cannot open the setup executable.");
    input.seekg(static_cast<std::streamoff>(file_size-rpk::kSfxFooterSize),std::ios::beg);
    std::array<char,8> magic{};
    input.read(magic.data(),static_cast<std::streamsize>(magic.size()));
    if(!input||magic!=rpk::kSfxFooterMagic) throw std::runtime_error("This is not a valid RPK self-extracting setup file.");

    const std::uint64_t stub_size=read_u64(input);
    const std::uint64_t archive_size=read_u64(input);
    const std::uint64_t footer_offset=file_size-rpk::kSfxFooterSize;
    if(stub_size==0||archive_size==0||stub_size>footer_offset||archive_size!=footer_offset-stub_size)
        throw std::runtime_error("The embedded archive boundaries are invalid.");

    auto directory=std::filesystem::temp_directory_path();
    std::filesystem::path temporary;
    for(unsigned int attempt=0;attempt<100;++attempt) {
        temporary=directory/(L"rpk-sfx-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(attempt)+L".rpk");
        if(!std::filesystem::exists(temporary,ec)&&!ec) break;
        ec.clear();
    }
    if(temporary.empty()||std::filesystem::exists(temporary,ec)) throw std::runtime_error("Cannot choose a temporary archive path.");

    try {
        std::ofstream output(temporary,std::ios::binary|std::ios::trunc);
        if(!output) throw std::runtime_error("Cannot create a temporary archive file.");
        input.clear();
        input.seekg(static_cast<std::streamoff>(stub_size),std::ios::beg);
        std::vector<char> buffer(1024U*1024U);
        std::uint64_t remaining=archive_size;
        while(remaining>0) {
            const auto request=static_cast<std::streamsize>(std::min<std::uint64_t>(remaining,buffer.size()));
            input.read(buffer.data(),request);
            if(input.gcount()!=request) throw std::runtime_error("The embedded archive is truncated.");
            output.write(buffer.data(),request);
            if(!output) throw std::runtime_error("Cannot write the temporary archive file.");
            remaining-=static_cast<std::uint64_t>(request);
        }
        output.flush();
        if(!output) throw std::runtime_error("Cannot finish preparing the embedded archive.");
        output.close();
        (void)rpk::read_archive(temporary);
    } catch(...) {
        std::filesystem::remove(temporary,ec);
        throw;
    }
    return temporary;
}
void append_log(const std::wstring& message) {
    if(!g||!g->log) return;
    const auto line=message+L"\r\n";
    SendMessageW(g->log,EM_SETSEL,static_cast<WPARAM>(-1),static_cast<LPARAM>(-1));
    SendMessageW(g->log,EM_REPLACESEL,FALSE,reinterpret_cast<LPARAM>(line.c_str()));
    SendMessageW(g->log,EM_SCROLLCARET,0,0);
}
void post_log(HWND window,const std::string& message) {
    auto* event=new LogEvent{from_utf8(message)};
    if(!PostMessageW(window,WM_SETUP_LOG,0,reinterpret_cast<LPARAM>(event))) delete event;
}
void post_done(HWND window,bool success,const std::string& message) {
    auto* event=new DoneEvent{success,from_utf8(message)};
    if(!PostMessageW(window,WM_SETUP_DONE,0,reinterpret_cast<LPARAM>(event))) delete event;
}
HWND add_control(HWND parent,const wchar_t* klass,const wchar_t* caption,DWORD style,
                 int x,int y,int width,int height,int id,HFONT font,DWORD ex=0) {
    HWND control=CreateWindowExW(ex,klass,caption,WS_CHILD|WS_VISIBLE|style,x,y,width,height,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    if(control&&font) SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);
    return control;
}
void browse_folder() {
    BROWSEINFOW info{};
    info.hwndOwner=g->window;
    info.lpszTitle=L"Choose the folder where the archive will be extracted";
    info.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE|BIF_EDITBOX;
    PIDLIST_ABSOLUTE selected=SHBrowseForFolderW(&info);
    if(!selected) return;
    wchar_t path[MAX_PATH]{};
    const BOOL ok=SHGetPathFromIDListW(selected,path);
    CoTaskMemFree(selected);
    if(ok) SetWindowTextW(g->path,path);
}
void start_extract() {
    const auto folder=get_text(g->path);
    if(folder.empty()) throw std::runtime_error("Choose a destination folder first.");
    if(g->busy) return;
    if(g->worker.joinable()) g->worker.join();

    const auto archive=g->embedded_archive;
    const std::filesystem::path destination(folder);
    const bool overwrite=SendMessageW(g->overwrite,BM_GETCHECK,0,0)==BST_CHECKED;
    const HWND window=g->window;

    g->busy=true;
    EnableWindow(g->install,FALSE);
    EnableWindow(g->browse,FALSE);
    EnableWindow(g->path,FALSE);
    EnableWindow(g->overwrite,FALSE);
    EnableWindow(g->open,FALSE);
    SendMessageW(g->progress,PBM_SETMARQUEE,TRUE,35);
    SetWindowTextW(g->status,L"Verifying archive and extracting files...");

    g->worker=std::thread([archive,destination,overwrite,window] {
        try {
            rpk::extract_archive(archive,destination,overwrite,
                [window](const std::string& message){post_log(window,message);});
            post_done(window,true,"Extraction completed. All restored files passed SHA-256 verification.");
        } catch(const std::exception& error) {
            post_done(window,false,error.what());
        }
    });
}
void create_controls(HWND window) {
    g->font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    g->title=CreateFontW(-29,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    g->heading=CreateFontW(-15,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    g->input=CreateSolidBrush(RGB(255,255,255));

    g->path=add_control(window,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,34,190,570,34,IDC_PATH,g->font,WS_EX_CLIENTEDGE);
    g->browse=add_control(window,L"BUTTON",L"Browse...",BS_PUSHBUTTON|WS_TABSTOP,616,190,112,34,IDC_BROWSE,g->font);
    g->install=add_control(window,L"BUTTON",L"Extract and verify",BS_DEFPUSHBUTTON|WS_TABSTOP,34,252,180,42,IDC_INSTALL,g->font);
    g->open=add_control(window,L"BUTTON",L"Open destination",BS_PUSHBUTTON|WS_TABSTOP,230,252,160,42,IDC_OPEN,g->font);
    EnableWindow(g->open,FALSE);
    g->overwrite=add_control(window,L"BUTTON",L"Replace existing files",BS_AUTOCHECKBOX|WS_TABSTOP,414,260,230,24,IDC_OVERWRITE,g->font);
    g->status=add_control(window,L"STATIC",L"Ready",SS_LEFT,34,320,694,22,IDC_STATUS,g->font);
    g->progress=add_control(window,PROGRESS_CLASSW,L"",PBS_MARQUEE|PBS_SMOOTH,34,348,694,12,IDC_PROGRESS,g->font);
    SendMessageW(g->progress,PBM_SETMARQUEE,FALSE,0);
    g->log=add_control(window,L"EDIT",L"Ready. Select a destination folder and click Extract and verify.\r\n",
        ES_MULTILINE|ES_AUTOVSCROLL|ES_READONLY|WS_VSCROLL|WS_TABSTOP,34,402,694,112,IDC_LOG,g->font,WS_EX_CLIENTEDGE);
    SendMessageW(g->log,EM_SETLIMITTEXT,1024U*1024U,0);

    wchar_t local[MAX_PATH]{};
    if(SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,local)==S_OK)
        SetWindowTextW(g->path,(std::wstring(local)+L"\\Programs\\RPK-Extracted").c_str());
}
LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    switch(message) {
    case WM_CREATE:
        g->window=window;
        create_controls(window);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc=BeginPaint(window,&paint);
        RECT client{}; GetClientRect(window,&client);
        HBRUSH background=CreateSolidBrush(RGB(244,247,251)); FillRect(dc,&client,background); DeleteObject(background);
        RECT header{0,0,client.right,142}; HBRUSH navy=CreateSolidBrush(RGB(23,38,60)); FillRect(dc,&header,navy); DeleteObject(navy);
        RECT accent{0,139,client.right,142}; HBRUSH blue=CreateSolidBrush(RGB(52,120,246)); FillRect(dc,&accent,blue); DeleteObject(blue);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(255,255,255)); SelectObject(dc,g->title);
        TextOutW(dc,34,22,L"RPK Setup",9);
        SelectObject(dc,g->font); SetTextColor(dc,RGB(205,218,235));
        const wchar_t* subtitle=L"Self-extracting archive installer";
        TextOutW(dc,37,76,subtitle,static_cast<int>(wcslen(subtitle)));
        SelectObject(dc,g->heading); SetTextColor(dc,RGB(29,41,57));
        TextOutW(dc,34,158,L"Extract location",15);
        SelectObject(dc,g->font); SetTextColor(dc,RGB(100,116,139));
        TextOutW(dc,34,382,L"Activity",8);
        EndPaint(window,&paint);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc=reinterpret_cast<HDC>(wparam); SetBkColor(dc,RGB(255,255,255)); SetTextColor(dc,RGB(29,41,57));
        return reinterpret_cast<LRESULT>(g->input);
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc=reinterpret_cast<HDC>(wparam); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(100,116,139));
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_COMMAND:
        try {
            if(LOWORD(wparam)==IDC_BROWSE){browse_folder();return 0;}
            if(LOWORD(wparam)==IDC_INSTALL){start_extract();return 0;}
            if(LOWORD(wparam)==IDC_OPEN&&!g->installed_folder.empty())
                ShellExecuteW(window,L"open",g->installed_folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        } catch(const std::exception& error) {
            MessageBoxW(window,from_utf8(error.what()).c_str(),L"RPK Setup",MB_OK|MB_ICONERROR);
        }
        return 0;
    case WM_SETUP_LOG: {
        std::unique_ptr<LogEvent> event(reinterpret_cast<LogEvent*>(lparam));
        if(event) append_log(event->text);
        return 0;
    }
    case WM_SETUP_DONE: {
        std::unique_ptr<DoneEvent> event(reinterpret_cast<DoneEvent*>(lparam));
        if(g->worker.joinable()) g->worker.join();
        g->busy=false;
        EnableWindow(g->install,TRUE); EnableWindow(g->browse,TRUE); EnableWindow(g->path,TRUE); EnableWindow(g->overwrite,TRUE);
        SendMessageW(g->progress,PBM_SETMARQUEE,FALSE,0);
        if(event) {
            SetWindowTextW(g->status,event->success?L"Extraction completed":L"Extraction failed");
            append_log(event->text);
            if(event->success) {
                g->installed_folder=get_text(g->path);
                EnableWindow(g->open,TRUE);
                MessageBoxW(window,L"Files were extracted and verified successfully.",L"RPK Setup",MB_OK|MB_ICONINFORMATION);
            } else MessageBoxW(window,event->text.c_str(),L"RPK Setup",MB_OK|MB_ICONERROR);
        }
        if(g->close_after) PostMessageW(window,WM_CLOSE,0,0);
        return 0;
    }
    case WM_CLOSE:
        if(g->busy) {
            if(MessageBoxW(window,L"Extraction is still running. Close this window after it finishes?",L"RPK Setup",MB_YESNO|MB_ICONWARNING)==IDYES)
                g->close_after=true;
            return 0;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        if(g->worker.joinable()) g->worker.join();
        if(g->font) DeleteObject(g->font);
        if(g->title) DeleteObject(g->title);
        if(g->heading) DeleteObject(g->heading);
        if(g->input) DeleteObject(g->input);
        if(!g->embedded_archive.empty()){std::error_code ec;std::filesystem::remove(g->embedded_archive,ec);}
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window,message,wparam,lparam);
    }
}
}

int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int show_command) {
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    App app; g=&app;
    try {
        wchar_t self[32768]{};
        const DWORD length=GetModuleFileNameW(nullptr,self,static_cast<DWORD>(std::size(self)));
        if(length==0||length>=std::size(self)) throw std::runtime_error("Cannot determine setup executable path.");
        app.embedded_archive=extract_embedded_archive(self);
        const auto info=rpk::read_archive(app.embedded_archive);

        WNDCLASSEXW window_class{};
        window_class.cbSize=sizeof(window_class); window_class.lpfnWndProc=window_proc;
        window_class.hInstance=instance; window_class.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        window_class.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        window_class.lpszClassName=kWindowClass; window_class.hIcon=LoadIconW(nullptr,IDI_APPLICATION);
        if(!RegisterClassExW(&window_class)) throw std::runtime_error("Cannot register RPK Setup window.");

        HWND window=CreateWindowExW(0,kWindowClass,L"RPK Setup",
            WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,
            CW_USEDEFAULT,CW_USEDEFAULT,770,590,nullptr,nullptr,instance,nullptr);
        if(!window) throw std::runtime_error("Cannot create RPK Setup window.");
        ShowWindow(window,show_command); UpdateWindow(window);
        append_log(L"Package ready: "+std::to_wstring(info.files.size())+L" files and "+std::to_wstring(info.directories.size())+L" folders.");
        MSG message{};
        while(GetMessageW(&message,nullptr,0,0)>0){TranslateMessage(&message);DispatchMessageW(&message);}
        return static_cast<int>(message.wParam);
    } catch(const std::exception& error) {
        MessageBoxW(nullptr,from_utf8(error.what()).c_str(),L"RPK Setup",MB_OK|MB_ICONERROR);
        if(!app.embedded_archive.empty()){std::error_code ec;std::filesystem::remove(app.embedded_archive,ec);}
        return 1;
    }
}
