#define NOMINMAX
#include "jan_client.hpp"

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace rpk::jan {
namespace {

std::filesystem::path settings_path() {
    wchar_t local[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local) != S_OK) {
        throw std::runtime_error("Cannot locate the Windows Local AppData folder.");
    }
    const auto directory = std::filesystem::path(local) / L"RPK";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) throw std::runtime_error("Cannot create the RPK settings folder: " + ec.message());
    return directory / L"ai.ini";
}

std::wstring read_profile(const wchar_t* name, const wchar_t* fallback) {
    wchar_t value[32768]{};
    const auto path = settings_path();
    GetPrivateProfileStringW(L"Jan", name, fallback, value,
                             static_cast<DWORD>(std::size(value)), path.c_str());
    return value;
}

void write_profile(const wchar_t* name, const std::wstring& value) {
    const auto path = settings_path();
    if (!WritePrivateProfileStringW(L"Jan", name, value.c_str(), path.c_str())) {
        throw std::runtime_error("Cannot save Jan connection settings.");
    }
}

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("Cannot convert Jan request text to UTF-8.");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) <= 0) {
        throw std::runtime_error("Cannot convert Jan request text to UTF-8.");
    }
    return result;
}

std::wstring from_utf8(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("Jan returned a response that is not valid UTF-8.");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count) <= 0) {
        throw std::runtime_error("Cannot decode Jan's UTF-8 response.");
    }
    return result;
}

std::string json_escape(const std::wstring& value) {
    const std::string utf8 = to_utf8(value);
    std::string escaped;
    escaped.reserve(utf8.size() + 16U);
    for (const unsigned char ch : utf8) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20U) {
                static const char hex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped.push_back(hex[(ch >> 4U) & 0x0fU]);
                escaped.push_back(hex[ch & 0x0fU]);
            } else {
                escaped.push_back(static_cast<char>(ch));
            }
        }
    }
    return escaped;
}

unsigned int hex_digit(char ch) {
    if (ch >= '0' && ch <= '9') return static_cast<unsigned int>(ch - '0');
    if (ch >= 'a' && ch <= 'f') return static_cast<unsigned int>(ch - 'a' + 10);
    if (ch >= 'A' && ch <= 'F') return static_cast<unsigned int>(ch - 'A' + 10);
    return 16U;
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code <= 0x7fU) out.push_back(static_cast<char>(code));
    else if (code <= 0x7ffU) {
        out.push_back(static_cast<char>(0xc0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else if (code <= 0xffffU) {
        out.push_back(static_cast<char>(0xe0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    } else {
        out.push_back(static_cast<char>(0xf0U | (code >> 18U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 12U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3fU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3fU)));
    }
}

std::string parse_json_string(const std::string& json, std::size_t& position) {
    if (position >= json.size() || json[position] != '"') return {};
    ++position;
    std::string value;
    while (position < json.size()) {
        const char ch = json[position++];
        if (ch == '"') return value;
        if (ch != '\\') {
            value.push_back(ch);
            continue;
        }
        if (position >= json.size()) break;
        const char escape = json[position++];
        switch (escape) {
        case '"': value.push_back('"'); break;
        case '\\': value.push_back('\\'); break;
        case '/': value.push_back('/'); break;
        case 'b': value.push_back('\b'); break;
        case 'f': value.push_back('\f'); break;
        case 'n': value.push_back('\n'); break;
        case 'r': value.push_back('\r'); break;
        case 't': value.push_back('\t'); break;
        case 'u': {
            if (position + 4U > json.size()) throw std::runtime_error("Jan returned malformed JSON Unicode.");
            std::uint32_t code = 0;
            for (unsigned int i = 0; i < 4; ++i) {
                const unsigned int digit = hex_digit(json[position++]);
                if (digit > 15U) throw std::runtime_error("Jan returned malformed JSON Unicode.");
                code = (code << 4U) | digit;
            }
            if (code >= 0xd800U && code <= 0xdbffU && position + 6U <= json.size() &&
                json[position] == '\\' && json[position + 1U] == 'u') {
                position += 2U;
                std::uint32_t low = 0;
                for (unsigned int i = 0; i < 4; ++i) {
                    const unsigned int digit = hex_digit(json[position++]);
                    if (digit > 15U) throw std::runtime_error("Jan returned malformed JSON Unicode.");
                    low = (low << 4U) | digit;
                }
                if (low >= 0xdc00U && low <= 0xdfffU) {
                    code = 0x10000U + ((code - 0xd800U) << 10U) + (low - 0xdc00U);
                }
            }
            append_utf8(value, code);
            break;
        }
        default: throw std::runtime_error("Jan returned an unknown JSON escape.");
        }
    }
    throw std::runtime_error("Jan returned an unterminated JSON string.");
}

std::string json_string_value(const std::string& json, const std::string& key, std::size_t from = 0) {
    const std::string marker = "\"" + key + "\"";
    const auto key_pos = json.find(marker, from);
    if (key_pos == std::string::npos) return {};
    const auto colon = json.find(':', key_pos + marker.size());
    if (colon == std::string::npos) return {};
    std::size_t value_pos = colon + 1U;
    while (value_pos < json.size() && (json[value_pos] == ' ' || json[value_pos] == '\r' ||
           json[value_pos] == '\n' || json[value_pos] == '\t')) ++value_pos;
    if (value_pos >= json.size() || json[value_pos] != '"') return {};
    return parse_json_string(json, value_pos);
}

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
    DWORD flags = 0;
};

ParsedUrl parse_local_url(const std::wstring& base_url) {
    if (base_url.empty()) throw std::runtime_error("Enter Jan's Local API Server URL.");
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(base_url.c_str(), 0, 0, &parts)) {
        throw std::runtime_error("Jan URL is invalid. Use http://127.0.0.1:1337/v1.");
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    if (host != L"127.0.0.1" && host != L"localhost" && host != L"::1") {
        throw std::runtime_error("For privacy, RPK only connects to a Jan server on this PC (localhost / 127.0.0.1).");
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTP) {
        throw std::runtime_error("Use Jan's local HTTP address, normally http://127.0.0.1:1337/v1.");
    }
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    while (!path.empty() && path.back() == L'/') path.pop_back();
    ParsedUrl parsed;
    parsed.host = host;
    parsed.port = parts.nPort;
    parsed.path = path;
    return parsed;
}

std::string http_request(const Settings& settings, const wchar_t* method,
                         const std::wstring& endpoint, const std::string& body) {
    const ParsedUrl url = parse_local_url(settings.base_url);
    HINTERNET session = WinHttpOpen(L"RPK-Jan-Connector/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) throw std::runtime_error("Cannot initialize Windows HTTP for Jan.");
    WinHttpSetTimeouts(session, 5000, 5000, 120000, 120000);
    HINTERNET connection = WinHttpConnect(session, url.host.c_str(), url.port, 0);
    if (!connection) {
        WinHttpCloseHandle(session);
        throw std::runtime_error("Cannot connect to Jan. Start Jan > Settings > Local API Server > Start Server.");
    }

    std::wstring request_path = url.path + endpoint;
    HINTERNET request = WinHttpOpenRequest(connection, method, request_path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, url.flags);
    if (!request) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("Cannot open the Jan API request.");
    }

    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &redirect_policy, sizeof(redirect_policy));
    std::wstring headers = L"Accept: application/json\r\n";
    if (method && std::wstring(method) == L"POST") headers += L"Content-Type: application/json\r\n";
    if (!settings.api_key.empty()) headers += L"Authorization: Bearer " + settings.api_key + L"\r\n";

    const BOOL sent = WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        const DWORD error = GetLastError();
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        throw std::runtime_error("Jan did not respond. Confirm the Local API Server is running (Windows error " +
                                 std::to_string(error) + ").");
    }

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    std::string response;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0) break;
        if (response.size() + available > 6U * 1024U * 1024U) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("Jan response exceeded the 6 MiB safety limit.");
        }
        const std::size_t old_size = response.size();
        response.resize(old_size + available);
        DWORD received = 0;
        if (!WinHttpReadData(request, response.data() + old_size, available, &received)) {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connection);
            WinHttpCloseHandle(session);
            throw std::runtime_error("Could not read Jan's response.");
        }
        response.resize(old_size + received);
        if (received == 0) break;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);

    if (status < 200 || status >= 300) {
        std::string detail = response;
        if (detail.size() > 1000U) detail.resize(1000U);
        throw std::runtime_error("Jan API returned HTTP " + std::to_string(status) +
            (status == 401 ? ". Check the API key in Jan." :
             (status == 404 ? ". Check the API prefix and selected model ID." : ". ")) + detail);
    }
    return response;
}

} // namespace

Settings load_settings() {
    Settings settings;
    settings.base_url = read_profile(L"BaseUrl", L"http://127.0.0.1:1337/v1");
    settings.api_key = read_profile(L"ApiKey", L"");
    settings.model = read_profile(L"Model", L"");
    return settings;
}

void save_settings(const Settings& settings) {
    write_profile(L"BaseUrl", settings.base_url);
    write_profile(L"ApiKey", settings.api_key);
    write_profile(L"Model", settings.model);
}

std::vector<std::wstring> list_models(const Settings& settings) {
    const std::string response = http_request(settings, L"GET", L"/models", {});
    std::vector<std::wstring> models;
    std::size_t position = 0;
    while ((position = response.find("\"id\"", position)) != std::string::npos) {
        const auto colon = response.find(':', position + 4U);
        if (colon == std::string::npos) break;
        position = colon + 1U;
        while (position < response.size() && (response[position] == ' ' || response[position] == '\r' ||
               response[position] == '\n' || response[position] == '\t')) ++position;
        if (position < response.size() && response[position] == '"') {
            const std::string id = parse_json_string(response, position);
            if (!id.empty()) models.push_back(from_utf8(id));
        }
    }
    std::sort(models.begin(), models.end());
    models.erase(std::unique(models.begin(), models.end()), models.end());
    if (models.empty()) throw std::runtime_error("Jan is running, but /v1/models returned no model IDs. Load a local model in Jan first.");
    return models;
}

std::wstring chat_completion(const Settings& settings,
                             const std::wstring& system_prompt,
                             const std::wstring& user_prompt,
                             unsigned int max_tokens) {
    if (settings.model.empty()) throw std::runtime_error("Select a model returned by Jan's /v1/models endpoint.");
    if (user_prompt.empty()) throw std::runtime_error("Enter a prompt for Jan.");
    const std::string body =
        "{\"model\":\"" + json_escape(settings.model) +
        "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + json_escape(system_prompt) +
        "\"},{\"role\":\"user\",\"content\":\"" + json_escape(user_prompt) +
        "\"}],\"temperature\":0.1,\"max_tokens\":" + std::to_string(max_tokens) + ",\"stream\":false}";
    const std::string response = http_request(settings, L"POST", L"/chat/completions", body);
    const std::string content = json_string_value(response, "content");
    if (content.empty()) {
        const std::string error = json_string_value(response, "message");
        throw std::runtime_error(error.empty() ? "Jan's response did not contain a chat completion." : error);
    }
    return from_utf8(content);
}

} // namespace rpk::jan
