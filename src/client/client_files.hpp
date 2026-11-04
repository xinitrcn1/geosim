#pragma once
// The client's own files (docs/08-ui.md#settings-and-saves):
//  - settings: $XDG_CONFIG_HOME/geosim/settings.txt (default ~/.config/geosim),
//    "key = value" lines (music_volume, music, clouds, fullscreen); read before
//    the command line, which overrides it; written when the Settings window
//    changes a value;
//  - saves: $XDG_DATA_HOME/geosim/saves (default ~/.local/share/geosim/saves),
//    "<scenario>-<YYYYMMDD-HHMMSS>.gsave" (sim::save_game; Ctrl+S in a session),
//    listed newest first by the main menu's Load session window.
// Header-only; std::filesystem through its error_code overloads (no exceptions).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace geosim::client {

struct client_settings {
    float music_volume = 0.6f;
    bool music = true;
    bool clouds = true;
    bool fullscreen = false;
};

inline std::filesystem::path xdg_dir(char const* var, char const* fallback) {
    if(char const* v = std::getenv(var); v && *v) return std::filesystem::path(v) / "geosim";
    if(char const* h = std::getenv("HOME"); h && *h) return std::filesystem::path(h) / fallback / "geosim";
    return std::filesystem::path(".geosim");
}

inline std::filesystem::path settings_path() { return xdg_dir("XDG_CONFIG_HOME", ".config") / "settings.txt"; }
inline std::filesystem::path saves_dir() { return xdg_dir("XDG_DATA_HOME", ".local/share") / "saves"; }

inline client_settings load_settings() {
    client_settings s;
    std::FILE* f = std::fopen(settings_path().c_str(), "r");
    if(!f) return s;
    char line[256];
    while(std::fgets(line, sizeof(line), f)) {
        char key[64] = {}, value[64] = {};
        if(std::sscanf(line, " %63[a-z_] = %63s", key, value) != 2) continue;
        const std::string k = key, v = value;
        if(k == "music_volume") s.music_volume = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.0f, 1.0f);
        else if(k == "music") s.music = v != "0";
        else if(k == "clouds") s.clouds = v != "0";
        else if(k == "fullscreen") s.fullscreen = v != "0";
    }
    std::fclose(f);
    return s;
}

inline bool save_settings(client_settings const& s) {
    std::error_code ec;
    std::filesystem::create_directories(settings_path().parent_path(), ec);
    std::FILE* f = std::fopen(settings_path().c_str(), "w");
    if(!f) return false;
    std::fprintf(f, "# geosim client settings (written by the Settings window)\nmusic_volume = %.2f\nmusic = %d\nclouds = %d\nfullscreen = %d\n",
                 static_cast<double>(s.music_volume), s.music ? 1 : 0, s.clouds ? 1 : 0, s.fullscreen ? 1 : 0);
    std::fclose(f);
    return true;
}

struct save_entry {
    std::string path, name; // name: the file name without extension
    std::filesystem::file_time_type time;
};

// Saves, newest first.
inline std::vector<save_entry> list_saves() {
    std::vector<save_entry> out;
    std::error_code ec;
    for(std::filesystem::directory_iterator it(saves_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if(it->path().extension() != ".gsave") continue;
        std::error_code tec;
        out.push_back({it->path().string(), it->path().stem().string(), std::filesystem::last_write_time(it->path(), tec)});
    }
    std::stable_sort(out.begin(), out.end(), [](save_entry const& a, save_entry const& b) { return a.time > b.time; });
    return out;
}

// A new save path for `scenario` (creates the directory).
inline std::string new_save_path(std::string const& scenario) {
    std::error_code ec;
    std::filesystem::create_directories(saves_dir(), ec);
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    return (saves_dir() / (scenario + "-" + stamp + ".gsave")).string();
}

// The scenario a save was made from: its file name up to the last two '-' fields.
inline std::string save_scenario(std::string const& name) {
    std::size_t cut = name.rfind('-');
    if(cut != std::string::npos && cut > 0) cut = name.rfind('-', cut - 1);
    return cut == std::string::npos || cut == 0 ? std::string("world2020") : name.substr(0, cut);
}

} // namespace geosim::client
