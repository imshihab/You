// =============================================================================
// You.cpp - a small, dependency-free C++17 CLI for creating, inspecting,
// deleting, renaming, moving, copying, trashing, tree-viewing, and running
// commands in files and directories.
//
// Build:
//   g++ -std=c++17 -O2 You.cpp -o you
//
// All features use only the C++17 standard library + <filesystem>.
// =============================================================================

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sys/utime.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#endif

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Usage text
//
// NOTE: keep this in sync with the "Usage" / "Flags" sections of README.md.
// ---------------------------------------------------------------------------
static const char* const DefaultLog =
R"(Usage:
  you [fileName / directoryName] [flag]
  you cd:Folder [more files/dirs]
  you store-{a,b}.js
  you run:Folder="cmd1 &&& cmd2"
  you --setting
  you [name]/{a,b} -t[=N]            (tree view)
  you [target] -trash                 (move to ./.trash/<ts>/)

Flags:
  --help, -h, --h                Show this message
  --version, -v, --v             Display the version
  -d                             Delete file or directory
  -rf                            Delete file only (errors on directory)
  -rd                            Delete directory recursively
  -rn old=new                    Rename a file or directory
  -mv old=new                    Move a file or directory
  -c  old=new                    Copy a file or directory tree
  -trash target                  Move target to ./.trash/<timestamp>/
  -i                             Show info for a file or directory
  -o target                      Show a text listing of a directory
  -t[=N]                         Tree view of cwd (default depth 3; -t= = infinite).
                                  Nerd Font icons + colored dirs on a TTY.
  -pwd                           Print current working directory
  --setting                      Interactive prompts; writes setting.json

Prefixes:
  cd:Folder                      Change into Folder for subsequent args
  $(name)                        Resolve via .youconfig lookup
  name/                          Trailing slash => always a directory

Examples:
  you index.js
  you index
  you index.js route.js
  you FolderName/
  you store-{a,b,name}.js
  you cd:src components/Button.tsx
  you -rn old.txt=new.txt
  you -c file.txt=copy.txt
  you secret -trash
  you -t=2
  you run:app="ls -la &&& pwd"
)";

// ---------------------------------------------------------------------------
// log(): writes errors to stderr in the original tool's style
// ---------------------------------------------------------------------------
static void log(const std::string& err) {
    std::cerr << "File or Directory does not exist or sometinge else.... \n" << err << "\n";
}

// ---------------------------------------------------------------------------
// trim(): strip leading/trailing whitespace (incl. \r from Windows line ends)
// ---------------------------------------------------------------------------
static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static bool askYesNo(const std::string& prompt, bool defaultYes) {
    std::cout << prompt << (defaultYes ? " [Y/n] " : " [y/N] ") << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return defaultYes;
    // Original semantics: drop ALL whitespace, then lowercase.
    std::string s;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s.empty()) return defaultYes;
    return s == "y" || s == "yes";
}

// ---------------------------------------------------------------------------
// Mirrors Node's path.extname() closely enough for this tool's needs:
// "" for names with no dot or pure dot-files (".gitignore" -> ""),
// ".ext" otherwise.
// ---------------------------------------------------------------------------
static std::string nodeExtname(const std::string& base) {
    if (base.empty()) return "";
    size_t dot = base.find_last_of('.');
    if (dot == std::string::npos || dot == 0) return "";
    return base.substr(dot);
}

// ---------------------------------------------------------------------------
// Brace expansion: a single name may contain {a,b,c} groups; the result is
// the cartesian product of all groups. e.g. "store-{a,b}-{1,2}.js" ->
//   "store-a-1.js", "store-a-2.js", "store-b-1.js", "store-b-2.js"
// Groups are processed left-to-right and are NOT nested: a '{' group ends at
// the first following '}', so genuinely nested braces like {a,{b,c}} are not
// supported (the outer group sees '{b,c' as one of its parts).
// ---------------------------------------------------------------------------
static std::vector<std::string> expandBraces(const std::string& s) {
    std::vector<std::string> out{""};
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '{') {
            size_t end = s.find('}', i + 1);
            if (end == std::string::npos) {
                for (auto& o : out) o += s.substr(i);
                i = s.size();
                break;
            }
            std::string inside = s.substr(i + 1, end - i - 1);
            std::vector<std::string> parts;
            std::string cur;
            for (char c : inside) {
                if (c == ',') { parts.push_back(cur); cur.clear(); }
                else cur += c;
            }
            parts.push_back(cur);
            std::vector<std::string> next;
            next.reserve(out.size() * parts.size());
            for (const auto& o : out) {
                for (const auto& p : parts) next.push_back(o + p);
            }
            out = std::move(next);
            i = end + 1;
        } else {
            for (auto& o : out) o += s[i];
            ++i;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Path kind: file vs. directory based on the same rules the tool already uses
// (extension present, hidden file -> file, trailing '/' forces directory).
// ---------------------------------------------------------------------------
enum class PathKind { File, Directory };

static PathKind resolveKind(const std::string& base, bool dirMarker, bool hiddenFile) {
    if (dirMarker) return PathKind::Directory;
    if (hiddenFile) return PathKind::File;
    return nodeExtname(base).empty() ? PathKind::Directory : PathKind::File;
}

// ---------------------------------------------------------------------------
// MIME table + lookup (unchanged from the previous version)
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string, std::string>& mimeTable() {
    static const std::unordered_map<std::string, std::string> table = {
        {"txt", "text/plain"}, {"html", "text/html"}, {"htm", "text/html"},
        {"css", "text/css"}, {"js", "text/javascript"}, {"mjs", "text/javascript"},
        {"json", "application/json"}, {"xml", "application/xml"},
        {"csv", "text/csv"}, {"md", "text/markdown"},
        {"yaml", "text/yaml"}, {"yml", "text/yaml"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"},
        {"gif", "image/gif"}, {"svg", "image/svg+xml"}, {"webp", "image/webp"},
        {"ico", "image/vnd.microsoft.icon"}, {"bmp", "image/bmp"}, {"tiff", "image/tiff"},
        {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"}, {"m4a", "audio/mp4"},
        {"mp4", "video/mp4"}, {"avi", "video/x-msvideo"}, {"mov", "video/quicktime"},
        {"webm", "video/webm"}, {"mkv", "video/x-matroska"},
        {"pdf", "application/pdf"}, {"zip", "application/zip"}, {"tar", "application/x-tar"},
        {"gz", "application/gzip"}, {"rar", "application/vnd.rar"}, {"7z", "application/x-7z-compressed"},
        {"doc", "application/msword"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xls", "application/vnd.ms-excel"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"ppt", "application/vnd.ms-powerpoint"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"c", "text/x-c"}, {"h", "text/x-c"}, {"cpp", "text/x-c++"}, {"hpp", "text/x-c++"},
        {"cc", "text/x-c++"}, {"py", "text/x-python"}, {"java", "text/x-java-source"},
        {"ts", "video/mp2t"}, {"sh", "application/x-sh"}, {"bat", "application/x-msdos-program"},
        {"exe", "application/x-msdownload"}, {"bin", "application/octet-stream"},
        {"woff", "font/woff"}, {"woff2", "font/woff2"}, {"ttf", "font/ttf"}, {"otf", "font/otf"},
        {"eot", "application/vnd.ms-fontobject"}, {"wasm", "application/wasm"},
        {"ini", "text/plain"}, {"log", "text/plain"}, {"conf", "text/plain"},
        {"rtf", "application/rtf"}, {"sql", "application/sql"},
        {"apk", "application/vnd.android.package-archive"},
        {"dmg", "application/x-apple-diskimage"}, {"iso", "application/x-iso9660-image"},
        {"psd", "image/vnd.adobe.photoshop"}, {"ai", "application/postscript"},
        {"eps", "application/postscript"}, {"ps", "application/postscript"},
        {"flv", "video/x-flv"}, {"wma", "audio/x-ms-wma"}, {"wmv", "video/x-ms-wmv"},
        {"swf", "application/x-shockwave-flash"},
    };
    return table;
}

static std::optional<std::string> mimeGetType(const std::string& extNoDot) {
    if (extNoDot.empty()) return std::nullopt;
    std::string lower = extNoDot;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    const auto& table = mimeTable();
    auto it = table.find(lower);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

// ---------------------------------------------------------------------------
// Colors for the tree view
// ---------------------------------------------------------------------------
static constexpr const char* kAnsiReset     = "\033[0m";
static constexpr const char* kAnsiFg        = "\033[38;2;248;250;252m";
static constexpr const char* kAnsiYellow    = "\033[38;2;250;204;21m";
static constexpr const char* kAnsiBlue      = "\033[38;2;37;99;235m";
static constexpr const char* kAnsiGreen     = "\033[38;2;34;197;94m";
static constexpr const char* kAnsiRed       = "\033[38;2;220;38;38m";
static constexpr const char* kAnsiDarkBlue  = "\033[38;2;29;78;216m";
static constexpr const char* kAnsiDarkGreen = "\033[38;2;21;128;61m";
static constexpr const char* kAnsiDarkRed   = "\033[38;2;153;27;27m";
static constexpr const char* kAnsiGray      = "\033[38;2;55;65;81m";
static constexpr const char* kAnsiPurple    = "\033[38;2;147;51;234m";
static constexpr const char* kAnsiBg        = "\033[48;2;17;24;39m";

struct IconInfo {
    const char* icon;
    const char* color;
};

// ---------------------------------------------------------------------------
// Nerd Font icons for the tree view (-t).
// ---------------------------------------------------------------------------
static const std::unordered_map<std::string, IconInfo>& iconTable() {
    static const std::unordered_map<std::string, IconInfo> table = {
        // languages
        {"js",    {"\uE74E", kAnsiYellow}},
        {"mjs",   {"\uE74E", kAnsiYellow}},
        {"cjs",   {"\uE74E", kAnsiYellow}},
        {"jsx",   {"\uE7BA", kAnsiBlue}},
        {"ts",    {"\uE628", kAnsiBlue}},
        {"tsx",   {"\uE628", kAnsiBlue}},
        {"py",    {"\uE73C", kAnsiBlue}},
        {"html",  {"\uE736", kAnsiRed}},
        {"htm",   {"\uE736", kAnsiRed}},
        {"css",   {"\uE749", kAnsiBlue}},
        {"scss",  {"\uE749", kAnsiPurple}},
        {"sass",  {"\uE749", kAnsiPurple}},
        {"less",  {"\uE749", kAnsiDarkBlue}},
        {"json",  {"\uE60B", kAnsiGreen}},
        {"md",    {"\uE609", kAnsiFg}},
        {"mdx",   {"\uE609", kAnsiYellow}},
        {"yml",   {"\uE6A8", kAnsiRed}},
        {"yaml",  {"\uE6A8", kAnsiRed}},
        {"go",    {"\uE626", kAnsiBlue}},
        {"rs",    {"\uE7A8", kAnsiRed}},
        {"c",     {"\uE61E", kAnsiBlue}},
        {"h",     {"\uE61A", kAnsiPurple}},
        {"cpp",   {"\uE61D", kAnsiBlue}},
        {"cc",    {"\uE61D", kAnsiBlue}},
        {"cxx",   {"\uE61D", kAnsiBlue}},
        {"hpp",   {"\uE61A", kAnsiPurple}},
        {"hh",    {"\uE61A", kAnsiPurple}},
        {"java",  {"\uE738", kAnsiRed}},
        {"rb",    {"\uE739", kAnsiRed}},
        {"php",   {"\uE73D", kAnsiPurple}},
        {"sh",    {"\uF489", kAnsiGreen}},
        {"bash",  {"\uF489", kAnsiGreen}},
        {"zsh",   {"\uF489", kAnsiGreen}},
        {"toml",  {"\uE6B2", kAnsiYellow}},
        {"sql",   {"\uE706", kAnsiYellow}},
        // images / media
        {"png",   {"\uF1C5", kAnsiPurple}},
        {"jpg",   {"\uF1C5", kAnsiPurple}},
        {"jpeg",  {"\uF1C5", kAnsiPurple}},
        {"gif",   {"\uF1C5", kAnsiPurple}},
        {"webp",  {"\uF1C5", kAnsiPurple}},
        {"svg",   {"\uF1C5", kAnsiYellow}},
        {"ico",   {"\uF1C5", kAnsiYellow}},
        {"bmp",   {"\uF1C5", kAnsiPurple}},
        {"tiff",  {"\uF1C5", kAnsiPurple}},
        {"mp3",   {"\uF001", kAnsiYellow}},
        {"wav",   {"\uF001", kAnsiYellow}},
        {"ogg",   {"\uF001", kAnsiYellow}},
        {"flac",  {"\uF001", kAnsiYellow}},
        {"m4a",   {"\uF001", kAnsiYellow}},
        {"mp4",   {"\uF03D", kAnsiBlue}},
        {"mkv",   {"\uF03D", kAnsiBlue}},
        {"mov",   {"\uF03D", kAnsiBlue}},
        {"avi",   {"\uF03D", kAnsiBlue}},
        {"webm",  {"\uF03D", kAnsiBlue}},
        {"wmv",   {"\uF03D", kAnsiBlue}},
        // docs / archives / misc
        {"pdf",   {"\uF1C1", kAnsiRed}},
        {"doc",   {"\uF1C2", kAnsiBlue}},
        {"docx",  {"\uF1C2", kAnsiBlue}},
        {"xls",   {"\uF1C3", kAnsiGreen}},
        {"xlsx",  {"\uF1C3", kAnsiGreen}},
        {"ppt",   {"\uF1C4", kAnsiRed}},
        {"pptx",  {"\uF1C4", kAnsiRed}},
        {"zip",   {"\uF187", kAnsiRed}},
        {"tar",   {"\uF187", kAnsiRed}},
        {"gz",    {"\uF187", kAnsiRed}},
        {"bz2",   {"\uF187", kAnsiRed}},
        {"xz",    {"\uF187", kAnsiRed}},
        {"7z",    {"\uF187", kAnsiRed}},
        {"rar",   {"\uF187", kAnsiRed}},
        {"lock",  {"\uF023", kAnsiGray}},
        {"log",   {"\uF4D8", kAnsiGray}},
        {"wasm",  {"\uE6A1", kAnsiPurple}},
        {"txt",   {"\uF15C", kAnsiFg}},
        {"csv",   {"\uF15C", kAnsiGreen}},
        {"vue",   {"\uFD42", kAnsiGreen}},
        {"svelte",{"\uE697", kAnsiRed}},
        {"astro", {"\uE207", kAnsiPurple}},
    };
    return table;
}

static std::optional<IconInfo> iconForExactName(const std::string& baseName) {
    static const std::unordered_map<std::string, IconInfo> names = {
        {"dockerfile",   {"\uF308", kAnsiBlue}},
        {"containerfile",{"\uF308", kAnsiBlue}},
        {"makefile",     {"\uE779", kAnsiGray}},
        {"rakefile",     {"\uE779", kAnsiRed}},
        {"license",      {"\uF02D", kAnsiYellow}},
        {"licence",      {"\uF02D", kAnsiYellow}},
        {"copying",      {"\uF02D", kAnsiYellow}},
        // dotfiles
        {".gitignore",    {"\uF1D3", kAnsiRed}},
        {".gitattributes",{"\uF1D3", kAnsiRed}},
        {".gitmodules",   {"\uF1D3", kAnsiRed}},
        {".gitkeep",      {"\uF1D3", kAnsiRed}},
        {".env",          {"\uF462", kAnsiYellow}},
        {".envrc",        {"\uF462", kAnsiYellow}},
        {".dockerignore", {"\uF308", kAnsiGray}},
        {".editorconfig", {"\uE615", kAnsiFg}},
        {".npmrc",        {"\uE71E", kAnsiRed}},
        {".nvmrc",        {"\uE71E", kAnsiRed}},
        {".prettierrc",   {"\uE60B", kAnsiGreen}},
        {".eslintrc",     {"\uE60B", kAnsiGreen}},
        {".babelrc",      {"\uE60B", kAnsiYellow}},
        // Directories:
        {"node_modules",  {"\uE718", kAnsiGreen}},
        {".git",          {"\uE5FB", kAnsiRed}},
        {".github",       {"\uE40A", kAnsiFg}},
        {".vscode",       {"\uE70C", kAnsiBlue}},
    };
    std::string lower = baseName;
    std::transform(lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return std::tolower(c); });
    auto it = names.find(lower);
    if (it != names.end()) return it->second;
    // README
    if (lower.rfind("readme", 0) == 0) return IconInfo{"\uE609", kAnsiYellow};
    return std::nullopt;
}

static IconInfo getIconInfo(const std::string& baseName, bool isDir) {
    auto exact = iconForExactName(baseName);
    if (exact.has_value()) return *exact;
    if (isDir) return {"\uF07B", kAnsiBlue}; // folder

    std::string extWithDot = nodeExtname(baseName);
    if (!extWithDot.empty()) {
        std::string ext = extWithDot.substr(1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
            [](unsigned char c) { return std::tolower(c); });
        const auto& table = iconTable();
        auto it = table.find(ext);
        if (it != table.end()) return it->second;
    }
    return {"\uF15B", kAnsiFg}; // generic file
}

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------
static std::string extractJsonStringField(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) return "";
    pos = json.find('"', pos);
    if (pos == std::string::npos) return "";
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    return json.substr(pos + 1, end - pos - 1);
}

static std::string getVersion() {
    const std::vector<fs::path> candidates = {
        fs::path("package.json"),
        fs::path("..") / "package.json",
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
            std::ifstream f(c, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string v = extractJsonStringField(ss.str(), "version");
            if (!v.empty()) return v;
        }
    }
    return "1.0.0";
}

static std::string cppStandardString() {
    #if __cplusplus >= 202002L
    return "C++20";
    #elif __cplusplus >= 201703L
    return "C++17";
    #elif __cplusplus >= 201402L
    return "C++14";
    #else
    return "C++11";
    #endif
}

// ---------------------------------------------------------------------------
// Cross-platform file time retrieval (birth + modified) and "touch".
// ---------------------------------------------------------------------------
struct FileTimes {
    std::time_t birth = 0;
    std::time_t modified = 0;
};

#ifdef _WIN32
static std::time_t filetimeToTimeT(const FILETIME& ft) {
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    static const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    uint64_t t = (uli.QuadPart - EPOCH_DIFF_100NS) / 10000000ULL;
    return static_cast<std::time_t>(t);
}

static FileTimes getFileTimes(const fs::path& p) {
    FileTimes result;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &data)) {
        result.birth = filetimeToTimeT(data.ftCreationTime);
        result.modified = filetimeToTimeT(data.ftLastWriteTime);
    }
    return result;
}

static void touchFile(const fs::path& p) {
    _wutime(p.wstring().c_str(), nullptr);
}
#else
static FileTimes getFileTimes(const fs::path& p) {
    FileTimes result;
    struct stat st{};
    if (stat(p.c_str(), &st) == 0) {
        result.modified = st.st_mtime;
        #ifdef __APPLE__
        result.birth = st.st_birthtimespec.tv_sec;
        #else
        result.birth = st.st_ctime;
        #endif
    }
    return result;
}

static void touchFile(const fs::path& p) {
    utime(p.c_str(), nullptr);
}
#endif

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------
static std::string formatSizeKib(uintmax_t bytes) {
    double kib = static_cast<double>(bytes) / 1024.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << kib << "Kib";
    return oss.str();
}

static std::string formatDateTime(std::time_t t) {
    std::tm tmv{};
    #ifdef _WIN32
    localtime_s(&tmv, &t);
    #else
    localtime_r(&t, &tmv);
    #endif
    int hour12 = tmv.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = (tmv.tm_hour < 12) ? "AM" : "PM";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d/%d/%d, %d:%02d:%02d %s",
        tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_year + 1900,
        hour12, tmv.tm_min, tmv.tm_sec, ampm);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// console.table() emulation
// ---------------------------------------------------------------------------
using Cell = std::optional<std::string>;

static std::string renderCell(const Cell& v) {
    if (!v.has_value()) return "null";
    return "'" + *v + "'";
}

static void printInfoTable(const std::vector<std::pair<std::string, Cell>>& rows) {
    const std::string headerA = "(index)";
    const std::string headerB = "Values";

    std::vector<std::string> a, b;
    size_t widthA = headerA.size();
    size_t widthB = headerB.size();
    for (const auto& [k, v] : rows) {
        std::string rendered = renderCell(v);
        widthA = std::max(widthA, k.size());
        widthB = std::max(widthB, rendered.size());
        a.push_back(k);
        b.push_back(rendered);
    }

    auto center = [](const std::string& s, size_t width) {
        size_t pad = width - s.size();
        size_t left = pad / 2;
        return std::string(left, ' ') + s + std::string(pad - left, ' ');
    };
    auto rule = [&](const char* l, const char* m, const char* r) {
        std::cout << l;
        for (size_t i = 0; i < widthA + 2; ++i) std::cout << "\u2500";
        std::cout << m;
        for (size_t i = 0; i < widthB + 2; ++i) std::cout << "\u2500";
        std::cout << r << "\n";
    };

    rule("\u250c", "\u252c", "\u2510");
    std::cout << "\u2502 " << center(headerA, widthA) << " \u2502 " << center(headerB, widthB) << " \u2502\n";
    rule("\u251c", "\u253c", "\u2524");
    for (size_t i = 0; i < a.size(); ++i) {
        std::cout << "\u2502 " << center(a[i], widthA) << " \u2502 " << center(b[i], widthB) << " \u2502\n";
    }
    rule("\u2514", "\u2534", "\u2518");
}

// ---------------------------------------------------------------------------
// Shared "does this path exist?" guards
// ---------------------------------------------------------------------------
static bool requirePath(const fs::path& p) {
    std::error_code ec;
    if (fs::exists(p, ec)) return true;
    log("ENOENT: no such file or directory, stat '" + p.string() + "'");
    return false;
}

// "Pardon! can't find the file/directory to <verb>" flavour used by
// rename / move / copy / trash.
static bool checkTarget(const fs::path& p, const char* verb) {
    std::error_code ec;
    if (fs::exists(p, ec)) return true;
    log("Pardon! can't find the file/directory to " + std::string(verb) + ", stat '" + p.string() + "'");
    return false;
}

// ---------------------------------------------------------------------------
// -i : print file/directory info
// ---------------------------------------------------------------------------
static void printInfo(const fs::path& filePath) {
    if (!requirePath(filePath)) return;
    std::error_code ec;
    bool isDirectory = fs::is_directory(filePath, ec);
    FileTimes times = getFileTimes(filePath);
    uintmax_t size = isDirectory ? 0 : fs::file_size(filePath, ec);

    std::string baseName = filePath.filename().string();
    std::string extWithDot = isDirectory ? "" : nodeExtname(baseName);
    std::string ext = extWithDot.empty() ? "" : extWithDot.substr(1);

    Cell extCell = isDirectory ? Cell(std::nullopt) : Cell(ext);
    Cell mimeCell = isDirectory ? Cell(std::string("Directory")) : mimeGetType(ext);
    Cell sizeCell = isDirectory ? Cell(std::nullopt) : Cell(formatSizeKib(size));

    std::vector<std::pair<std::string, Cell>> rows = {
        {"FileName", Cell(baseName)},
        {"Extension", extCell},
        {"MimeType", mimeCell},
        {"Size", sizeCell},
        {"Created", Cell(formatDateTime(times.birth))},
        {"Changed", Cell(formatDateTime(times.modified))},
    };
    printInfoTable(rows);
}

// ---------------------------------------------------------------------------
// Delete / rename / move / copy
// ---------------------------------------------------------------------------
static void deletePath(const fs::path& filePath) {
    if (!requirePath(filePath)) return;
    std::error_code ec;
    if (fs::is_directory(filePath, ec)) {
        fs::remove_all(filePath, ec);
    } else {
        fs::remove(filePath, ec);
    }
    if (ec) log(ec.message());
}

static void deleteKind(const fs::path& p, PathKind kind) {
    if (!requirePath(p)) return;
    std::error_code ec;
    bool isDir = fs::is_directory(p, ec);
    if (kind == PathKind::File && isDir) {
        log("'" + p.string() + "' is a directory; use -rd to remove directories");
        return;
    }
    if (kind == PathKind::Directory && !isDir) {
        log("'" + p.string() + "' is a file; use -rf to remove files");
        return;
    }
    deletePath(p);
}

static bool splitEq(const std::string& s, std::string& left, std::string& right) {
    auto pos = s.find('=');
    if (pos == std::string::npos) return false;
    left = s.substr(0, pos);
    right = s.substr(pos + 1);
    return true;
}

// If `dest` is an existing directory, append the source basename (like
// `cp`/`mv` when the destination is a folder); otherwise use dest as-is.
static fs::path resolveDest(const fs::path& src, const fs::path& dest) {
    std::error_code ec;
    if (fs::is_directory(dest, ec)) return dest / src.filename();
    return dest;
}

static void doRename(const fs::path& oldP, const fs::path& newP) {
    if (!checkTarget(oldP, "rename")) return;
    std::error_code ec;
    fs::rename(oldP, newP, ec);
    if (ec) log(ec.message());
}

static void doMove(const fs::path& oldP, const fs::path& newP) {
    if (!checkTarget(oldP, "move")) return;
    std::error_code ec;
    fs::path target = resolveDest(oldP, newP);
    fs::create_directories(target.parent_path(), ec);
    fs::rename(oldP, target, ec);
    if (ec) log(ec.message());
}

static void doCopy(const fs::path& oldP, const fs::path& newP) {
    if (!checkTarget(oldP, "copy")) return;
    std::error_code ec;
    bool srcIsDir = fs::is_directory(oldP, ec);
    fs::path target = resolveDest(oldP, newP);
    if (srcIsDir) {
        fs::create_directories(target, ec);
        fs::copy(oldP, target,
            fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    } else {
        fs::create_directories(target.parent_path(), ec);
        fs::copy_file(oldP, target, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) log(ec.message());
}

static fs::path getHomeDir() {
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    return home ? fs::path(home) : fs::current_path();
}

static fs::path settingJsonPath() {
    return getHomeDir() / ".you" / "setting.json";
}

struct AppSettings {
    std::string editor = "nano";
    std::string trash = (getHomeDir() / ".you" / ".trash").string();
    int treeDepth = 3;
    bool confirmCreate = false;
    std::vector<std::string> excludeFolders = {"node_modules", ".git"};
};

static AppSettings getGlobalSettings() {
    static AppSettings s = []() {
        AppSettings def;
        fs::path path = settingJsonPath();
        std::error_code ec;
        if (fs::exists(path, ec)) {
            std::ifstream in(path);
            std::ostringstream ss; ss << in.rdbuf();
            std::string existing = ss.str();
            
            std::string ed = extractJsonStringField(existing, "editor");
            if (!ed.empty()) def.editor = ed;
            std::string tr = extractJsonStringField(existing, "trash");
            if (!tr.empty()) def.trash = tr;
            std::string td = extractJsonStringField(existing, "tree_depth");
            if (!td.empty()) { try { def.treeDepth = std::stoi(td); } catch (...) {} }
            std::string cc = extractJsonStringField(existing, "confirm_create");
            if (!cc.empty()) def.confirmCreate = (cc == "y" || cc == "Y" || cc == "yes" || cc == "true");
            std::string ex = extractJsonStringField(existing, "exclude_folders");
            if (!ex.empty()) {
                def.excludeFolders.clear();
                std::stringstream ssEx(ex);
                std::string token;
                while (std::getline(ssEx, token, ',')) {
                    // simple trim for spaces
                    size_t a = 0, b = token.size();
                    while (a < b && std::isspace(static_cast<unsigned char>(token[a]))) ++a;
                    while (b > a && std::isspace(static_cast<unsigned char>(token[b - 1]))) --b;
                    std::string t = token.substr(a, b - a);
                    if (!t.empty()) def.excludeFolders.push_back(t);
                }
            }
        }
        return def;
    }();
    return s;
}

static bool shouldSkip(const std::string& name) {
    const auto& excludes = getGlobalSettings().excludeFolders;
    for (const auto& ex : excludes) {
        if (name == ex) return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// -trash : move target into ./.trash/<unix-timestamp>/<basename> and write a
// small meta.json sidecar with the original absolute path so the user can
// restore it later.
// ---------------------------------------------------------------------------
static long long nowUnix() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

static std::string isoNow() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    #ifdef _WIN32
    gmtime_s(&tmv, &t);
    #else
    gmtime_r(&t, &tmv);
    #endif
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
        tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return std::string(buf);
}

static void doTrash(const fs::path& p) {
    if (!checkTarget(p, "trash")) return;
    // Capture the original absolute path before renaming the target away.
    std::error_code ec;
    std::string origAbsolute = fs::absolute(p, ec).string();
    fs::path trashRoot = fs::path(getGlobalSettings().trash) / std::to_string(nowUnix());
    fs::create_directories(trashRoot, ec);
    if (ec) { log(ec.message()); return; }
    std::string origName = p.filename().string();
    // rename prefix marker to .trash__<name> so a future restore is unambiguous
    std::string markedName = ".trash__" + origName;
    fs::path markedDest = trashRoot / markedName;
    fs::rename(p, markedDest, ec);
    if (ec) { log(ec.message()); return; }

    // write sidecar
    fs::path meta = trashRoot / "meta.json";
    std::ofstream out(meta, std::ios::trunc | std::ios::binary);
    if (out) {
        out << "{\n"
            << "  \"original\": \"" << origAbsolute << "\",\n"
            << "  \"trashed_at\": \"" << isoNow() << "\",\n"
            << "  \"name\": \"" << origName << "\"\n"
            << "}\n";
    }
    std::cout << "Trashed '" << p.filename().string() << "' -> " << markedDest.string() << "\n";
}

// ---------------------------------------------------------------------------
// -t : tree view (recursive, depth-limited, skips node_modules and .git).
//
// When stdout is a TTY that supports ANSI escapes, every entry is preceded
// by a Nerd Font glyph (folder, file-by-extension, .gitignore, Dockerfile,
// LICENSE, README*, ...). Output piped to a file or another command falls
// back to plain ASCII because supportsAnsiColor() returns false in that
// case -- same gate that controls the bold-blue directory color.
// ---------------------------------------------------------------------------


// Sentinel value for "unlimited depth" (e.g. `-t=`).
static constexpr int kTreeInfiniteDepth = -1;

// ---------------------------------------------------------------------------
// ANSI colors for the tree view. Directories are shown in bold blue (the
// same convention as `ls --color`); regular files stay uncolored. Colors are
// only emitted when stdout is a TTY that supports ANSI escapes and the user
// hasn't opted out via NO_COLOR / TERM=dumb.
// ---------------------------------------------------------------------------
// Colors moved up above iconTable()

static bool supportsAnsiColor() {
    static const bool cached = []() -> bool {
        const char* noColor = std::getenv("NO_COLOR");
        if (noColor && *noColor) return false;
        const char* term = std::getenv("TERM");
        if (term && std::string(term) == "dumb") return false;
#ifdef _WIN32
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) return false;
        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        return SetConsoleMode(h, mode) != 0;
#else
        return isatty(STDOUT_FILENO) != 0;
#endif
    }();
    return cached;
}

static void walkTree(const fs::path& root, int depth, int maxDepth,
                     const std::string& prefix, bool last, bool color, std::ostream& out) {
    if (maxDepth != kTreeInfiniteDepth && depth > maxDepth) return;
    std::error_code ec;
    bool isDir = fs::is_directory(root, ec);
    std::string baseName = root.filename().string();
    
    std::string suffix = isDir ? "/" : "";
    std::string labeled = baseName + suffix;

    if (color) {
        IconInfo info = getIconInfo(baseName, isDir);
        std::string nameColor = isDir ? kAnsiBlue : kAnsiFg;
        labeled = std::string(info.color) + info.icon + kAnsiReset + "  " + 
                  nameColor + baseName + suffix + kAnsiReset;
    } else {
        IconInfo info = getIconInfo(baseName, isDir);
        labeled = std::string(info.icon) + "  " + labeled;
    }

    if (depth == 0) {
        out << labeled << "\n";
    } else {
        if (color) out << kAnsiGray;
        out << prefix << (last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ");
        if (color) out << kAnsiReset;
        out << labeled << "\n";
    }

    if (!isDir) return;
    if (maxDepth != kTreeInfiniteDepth && depth == maxDepth) return;
    if (depth > 0 && shouldSkip(baseName)) return; // Show it but don't go inside

    std::vector<fs::directory_entry> entries;
    for (auto it = fs::directory_iterator(root, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        entries.push_back(*it);
    }
    std::sort(entries.begin(), entries.end(),
        [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return a.path().filename().string() < b.path().filename().string();
        });
    for (size_t i = 0; i < entries.size(); ++i) {
        bool isLast = (i + 1 == entries.size());
        std::string nextPrefix = prefix;
        if (depth > 0) {
            nextPrefix += (last ? "    " : "\u2502   ");
        }
        walkTree(entries[i].path(), depth + 1, maxDepth, nextPrefix, isLast, color, out);
    }
}

static void doTree(const fs::path& start, int maxDepth) {
    if (!requirePath(start)) return;
    walkTree(start, 0, maxDepth, "", true, supportsAnsiColor(), std::cout);
}

// ---------------------------------------------------------------------------
// -o : text-mode directory listing (portable substitute for "open explorer")
// ---------------------------------------------------------------------------
static void doOpen(const fs::path& p) {
    if (!requirePath(p)) return;
    std::error_code ec;
    if (!fs::is_directory(p, ec)) {
        // For a file, behave like a tiny `cat` preview header
        std::cout << "--- " << p.string() << " ---\n";
        std::ifstream in(p, std::ios::binary);
        std::cout << in.rdbuf();
        return;
    }
    std::cout << "Listing of " << fs::absolute(p).string() << ":\n";
    for (auto it = fs::directory_iterator(p, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        bool isDir = it->is_directory(ec);
        auto size = isDir ? 0u : it->file_size(ec);
        std::cout << "  " << (isDir ? "[D] " : "    ") << it->path().filename().string()
                  << (isDir ? "/" : "  ") << formatSizeKib(size) << "\n";
    }
}

// ---------------------------------------------------------------------------
// .youconfig support
//
// A `.youconfig` file contains lines like:    name: /some/path
// Arguments beginning with `$(name)/...` are looked up by walking up from
// the current working directory until a `.youconfig` is found. If the name
// isn't present (or no config exists) the user is asked whether they want
// to fall back to creating the path in the current directory.
// ---------------------------------------------------------------------------
static std::optional<std::string> readYouConfigValue(const std::string& name) {
    fs::path dir = fs::current_path();
    while (true) {
        fs::path cfg = dir / ".youconfig";
        std::error_code ec;
        if (fs::exists(cfg, ec) && fs::is_regular_file(cfg, ec)) {
            std::ifstream in(cfg);
            std::string line;
            while (std::getline(in, line)) {
                std::string t = trim(line);
                if (t.empty() || t[0] == '#') continue;
                if (t.compare(0, name.size(), name) != 0) continue;
                size_t p = name.size();
                while (p < t.size() && std::isspace(static_cast<unsigned char>(t[p]))) ++p;
                if (p >= t.size() || t[p] != ':') continue;
                std::string value = trim(t.substr(p + 1));
                if (!value.empty()) return value;
            }
        }
        if (dir == dir.root_path()) break;
        dir = dir.parent_path();
    }
    return std::nullopt;
}

static std::string resolveDollarPrefix(const std::string& arg) {
    if (arg.size() < 4 || arg[0] != '$' || arg[1] != '(') return arg;
    size_t close = arg.find(')');
    if (close == std::string::npos) return arg;
    std::string name = arg.substr(2, close - 2);
    std::string rest = arg.substr(close + 1); // includes leading '/'
    auto v = readYouConfigValue(name);
    if (v.has_value()) {
        std::string s = *v;
        if (!s.empty() && s.back() == '/' && !rest.empty() && rest.front() == '/') s.pop_back();
        return s + rest;
    }
    std::cout << "No .youconfig entry for '$" << name << "'. Create in current directory? ";
    if (askYesNo("", true)) {
        // strip the $(name) prefix entirely. Drop a leading '/' so the path
        // is treated as relative to the current working directory rather
        // than as an absolute path on the root of the filesystem.
        if (!rest.empty() && (rest.front() == '/' || rest.front() == '\\')) rest.erase(0, 1);
        return rest;
    }
    return arg;
}

// ---------------------------------------------------------------------------
// --setting : interactive prompts that build a setting.json next to the
// binary. The defaults are taken from an existing setting.json if present.
// ---------------------------------------------------------------------------
static std::string readLineTrimmed() {
    std::string s;
    std::getline(std::cin, s);
    // trim is defined above
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

static std::string settingDefault(const std::string& existing, const std::string& key, const std::string& fallback) {
    std::string v = extractJsonStringField(existing, key);
    return v.empty() ? fallback : v;
}

static std::string promptSetting(const std::string& existing, const std::string& key,
                                 const std::string& fallback, const std::string& label) {
    std::string current = settingDefault(existing, key, fallback);
    std::cout << "  \033[1;36m" << std::left << std::setw(30) << label << "\033[0m "
              << "[" << current << "]: " << std::flush;
    std::string value = readLineTrimmed();
    return value.empty() ? current : value;
}

static std::string promptExcludeFolders(const std::string& existing) {
    std::string current = settingDefault(existing, "exclude_folders", "node_modules,.git");
    while (true) {
        std::cout << "  \033[1;36m" << std::left << std::setw(30) << "Exclude folders" << "\033[0m "
                  << "[" << current << "]\n";
        std::cout << "    (Use '-a: dir1, dir2' to add, '-r: dir1, dir2' to remove, or Enter to keep): ";
        std::string input = readLineTrimmed();
        if (input.empty()) {
            break;
        }

        std::vector<std::string> currentList;
        std::stringstream ssEx(current);
        std::string token;
        while (std::getline(ssEx, token, ',')) {
            token = trim(token);
            if (!token.empty()) currentList.push_back(token);
        }

        size_t idx = 0;
        int currentMode = 0; // 0 = none, 1 = add, 2 = remove
        std::string buf;
        bool valid = true;

        auto flushBuf = [&]() {
            if (currentMode == 0) {
                if (!trim(buf).empty()) valid = false;
                buf.clear();
                return;
            }
            std::stringstream ss(buf);
            std::string t;
            while (std::getline(ss, t, ',')) {
                t = trim(t);
                if (!t.empty() && t.back() == '.') t.pop_back();
                t = trim(t);
                if (t.empty()) continue;

                if (currentMode == 1) {
                    if (std::find(currentList.begin(), currentList.end(), t) == currentList.end()) {
                        currentList.push_back(t);
                    }
                } else if (currentMode == 2) {
                    auto it = std::remove(currentList.begin(), currentList.end(), t);
                    currentList.erase(it, currentList.end());
                }
            }
            buf.clear();
        };

        while (idx < input.size()) {
            if (input.compare(idx, 3, "-a:") == 0) {
                flushBuf();
                if (!valid) break;
                currentMode = 1;
                idx += 3;
            } else if (input.compare(idx, 3, "-r:") == 0) {
                flushBuf();
                if (!valid) break;
                currentMode = 2;
                idx += 3;
            } else {
                buf += input[idx];
                idx++;
            }
        }
        flushBuf();

        if (!valid) {
            std::cout << "    \033[1;31mInvalid format! Use '-a: ...' or '-r: ...'\033[0m\n";
            continue;
        }

        current.clear();
        for (size_t i = 0; i < currentList.size(); ++i) {
            current += currentList[i];
            if (i + 1 < currentList.size()) current += ",";
        }
    }
    return current;
}

static void doSetting() {
    fs::path path = settingJsonPath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::string existing;
    if (fs::exists(path, ec)) {
        std::ifstream in(path);
        std::ostringstream ss; ss << in.rdbuf();
        existing = ss.str();
    }
    std::cout << "\n\033[1;34m=== You CLI Configuration ===\033[0m\n";
    std::cout << "Saving to: " << path.string() << "\n";
    std::cout << "(Press Enter to accept current defaults)\n\n";

    std::string editor  = promptSetting(existing, "editor", "nano", "Default editor");
    std::string trash   = promptSetting(existing, "trash", (getHomeDir() / ".you" / ".trash").string(), "Trash directory path");
    std::string depth   = promptSetting(existing, "tree_depth", "3", "Default tree depth");
    std::string confirm = promptSetting(existing, "confirm_create", "n", "Confirm on create? (y/n)");
    std::string exclude = promptExcludeFolders(existing);

    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) { std::cerr << "cannot write " << path.string() << "\n"; return; }
    out << "{\n"
        << "  \"editor\": \"" << editor << "\",\n"
        << "  \"trash\": \"" << trash << "\",\n"
        << "  \"tree_depth\": \"" << depth << "\",\n"
        << "  \"confirm_create\": \"" << confirm << "\",\n"
        << "  \"exclude_folders\": \"" << exclude << "\"\n"
        << "}\n";
    std::cout << "\n\033[1;32m\u2714 Settings saved successfully.\033[0m\n\n";
}

// ---------------------------------------------------------------------------
// Creation: touch a file or mkdir a directory, mirroring the original logic
// but parameterised on PathKind so brace-expanded names and cd:-prefixed
// paths both work.
// ---------------------------------------------------------------------------
static void createOne(const fs::path& abs, PathKind kind) {
    std::error_code ec;
    fs::path parent = abs.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) { log(ec.message()); return; }
    }
    if (kind == PathKind::Directory) {
        fs::create_directories(abs, ec);
        if (ec) log(ec.message());
        return;
    }
    if (!fs::exists(abs, ec)) {
        std::ofstream ofs(abs, std::ios::trunc | std::ios::binary);
        if (!ofs) log("could not create file '" + abs.string() + "'");
    } else {
        touchFile(abs);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

// Single-shot flags (version / help / -pwd / --setting / -t): as soon as any
// argument matches, the whole invocation is handled and the call returns true.
static bool handleSingleShot(const std::vector<std::string>& args) {
    static const std::vector<std::string> versionFlags = {"-v", "--v", "--version"};
    static const std::vector<std::string> helpFlags = {"-h", "--h", "--help"};
    auto has = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (has(versionFlags, a)) {
            std::cout << "You/" << getVersion() << " C++: " << cppStandardString() << "\n";
            return true;
        }
        if (has(helpFlags, a)) {
            std::cout << DefaultLog;
            return true;
        }
        if (a == "-pwd") {
            std::error_code ec;
            std::cout << fs::current_path(ec).string() << "\n";
            return true;
        }
        if (a == "--setting" || a == "-setting") {
            doSetting();
            return true;
        }
        if (a == "-t" || a.rfind("-t=", 0) == 0) {
            int depth = 3;
            if (a == "-t=") {
                // `-t=` with no number => walk as deep as it goes.
                depth = kTreeInfiniteDepth;
            } else if (a.rfind("-t=", 0) == 0) {
                try { depth = std::stoi(a.substr(3)); } catch (...) { depth = 3; }
            } else if (i + 1 < args.size()) {
                try { depth = std::stoi(args[i + 1]); } catch (...) { depth = 3; }
            }
            fs::path start = fs::current_path();
            if (i > 0 && !args[i - 1].empty() && args[i - 1][0] != '-') {
                start = args[i - 1];
            }
            doTree(start, depth);
            return true;
        }
    }
    return false;
}

// `you run:Folder="cmd1 &&& cmd2"` -- chdir into the folder (creating it if
// needed) and run each `&&&`-separated command via the system shell.
static int runCommands(const std::string& full) {
    auto eq = full.find('=');
    std::string head = (eq == std::string::npos) ? full : full.substr(0, eq);
    std::string cmd  = (eq == std::string::npos) ? std::string() : full.substr(eq + 1);
    // head is "run:Folder" -- strip prefix
    std::string folder = (head.size() > 4) ? head.substr(4) : std::string();
    if (!folder.empty()) {
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (ec) { log(ec.message()); return 1; }
        fs::current_path(folder, ec);
        if (ec) { log(ec.message()); return 1; }
    }
    // strip outer quotes if present
    if (cmd.size() >= 2 && (cmd.front() == '"' || cmd.front() == '\'')) cmd = cmd.substr(1);
    if (!cmd.empty() && (cmd.back() == '"' || cmd.back() == '\'')) cmd.pop_back();
    // split on "&&&" and run each command
    std::string token;
    int rc = 0;
    for (size_t j = 0; j <= cmd.size(); ++j) {
        if (j == cmd.size() || (j + 2 < cmd.size() && cmd[j] == '&' && cmd[j + 1] == '&' && cmd[j + 2] == '&')) {
            if (!token.empty()) {
                std::cout << "$ " << token << "\n";
                int r = std::system(token.c_str());
                if (r != 0) rc = r;
            }
            token.clear();
            if (j + 2 < cmd.size()) j += 2;
        } else {
            token += cmd[j];
        }
    }
    return rc;
}

// Per-argument flags that operate on the preceding path argument.
// Returns false if `flag` is not one of the known per-argument flags.
static bool handleDashFlag(const std::string& flag, const std::string& prevArg,
                           const fs::path& root) {
    fs::path target = (root / prevArg).lexically_normal();
    if (flag == "-d")     { deletePath(target); return true; }
    if (flag == "-rf")    { deleteKind(target, PathKind::File); return true; }
    if (flag == "-rd")    { deleteKind(target, PathKind::Directory); return true; }
    if (flag == "-trash") { doTrash(target); return true; }
    if (flag == "-i")     { printInfo(target); return true; }
    if (flag == "-o")     { doOpen(target); return true; }
    return false;
}

// Collect and run the `-rn old=new` / `-mv old=new` / `-c old=new` operations
// (which describe their own target paths), then mark the involved argument
// indices as consumed so the per-argument loop skips them.
static void runOps(const std::vector<std::string>& args, const fs::path& currentRoot,
                   std::vector<bool>& consumed) {
    struct PendingOp { size_t flagIndex; std::string spec; };
    std::vector<PendingOp> ops;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& f = args[i];
        if (f == "-rn" || f == "-mv" || f == "-c") {
            if (i + 1 >= args.size() || args[i + 1].find('=') == std::string::npos) {
                log("-rn/-mv/-c expects an 'old=new' argument");
                continue;
            }
            ops.push_back({i, args[i + 1]});
        }
    }
    for (const auto& op : ops) {
        std::string lhs, rhs;
        if (!splitEq(op.spec, lhs, rhs)) continue;
        fs::path oldP = (currentRoot / lhs).lexically_normal();
        fs::path newP = (currentRoot / rhs).lexically_normal();
        if (args[op.flagIndex] == "-rn") doRename(oldP, newP);
        else if (args[op.flagIndex] == "-mv") doMove(oldP, newP);
        else doCopy(oldP, newP);
    }
    for (const auto& op : ops) {
        consumed[op.flagIndex] = true;
        consumed[op.flagIndex + 1] = true;
    }
}

int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    if (handleSingleShot(args)) return 0;

    for (const auto& a : args) {
        if (a.rfind("run:", 0) == 0) return runCommands(a);
    }

    // Per-argument processing with cd: chaining, brace expansion, and
    // trailing-slash / hidden-file kind resolution.
    fs::path currentRoot = fs::current_path();

    // Collect/run -rn/-mv/-c ops and mark their indices as consumed.
    std::vector<bool> consumed(args.size(), false);
    runOps(args, currentRoot, consumed);

    auto processName = [&](const std::string& filename) {
        // .youconfig / $(name) substitution
        std::string resolved = resolveDollarPrefix(filename);

        // trailing-slash directory marker
        bool dirMarker = resolved.size() > 1 && resolved.back() == '/';
        std::string cleanName = resolved;
        if (dirMarker) {
            while (!cleanName.empty() && cleanName.back() == '/') cleanName.pop_back();
            if (cleanName.empty()) cleanName = "/";
        }

        fs::path filePath = (currentRoot / cleanName).lexically_normal();
        std::string baseName = filePath.filename().string();
        bool hiddenFile = !baseName.empty() && baseName[0] == '.';
        PathKind kind = resolveKind(baseName, dirMarker, hiddenFile);

        createOne(filePath, kind);
    };

    for (size_t index = 0; index < args.size(); ++index) {
        if (consumed[index]) continue;
        std::string filename = args[index];
        const bool isDash = !filename.empty() && filename[0] == '-';

        if (isDash) {
            // -d, -rf, -rd, -trash, -i, -o all operate on the previous arg.
            if (index > 0 && handleDashFlag(filename, args[index - 1], currentRoot)) continue;
            // Unknown flag -- preserve original behaviour: print help.
            std::cout << "Pardon! The command does not exist..\n\n" << DefaultLog;
            return 0;
        }

        // non-flag: could be a path or a cd: prefix
        if (filename.rfind("cd:", 0) == 0) {
            std::string folder = filename.substr(3);
            std::error_code ec;
            fs::create_directories(folder, ec);
            if (ec) { log(ec.message()); continue; }
            currentRoot = fs::canonical(folder, ec);
            if (ec) currentRoot = (currentRoot / folder).lexically_normal();
            continue;
        }

        // brace expand
        for (const auto& name : expandBraces(filename)) {
            processName(name);
        }
    }
    return 0;
}

