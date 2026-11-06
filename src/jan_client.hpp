#pragma once
#include <string>
#include <vector>

namespace rpk::jan {

struct Settings {
    std::wstring base_url = L"http://127.0.0.1:1337/v1";
    std::wstring api_key;
    std::wstring model;
};

Settings load_settings();
void save_settings(const Settings& settings);
std::vector<std::wstring> list_models(const Settings& settings);
std::wstring chat_completion(const Settings& settings,
                             const std::wstring& system_prompt,
                             const std::wstring& user_prompt,
                             unsigned int max_tokens = 1200);

} // namespace rpk::jan
