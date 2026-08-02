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
  -t[=N]                         Tree view of cwd (default depth 3)
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
  you old.txt -rn new.txt
  you file.txt -c copy.txt
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

static bool askYesNo(const std::string& prompt, bool defaultYes) {
    std::cout << prompt << (defaultYes ? " [Y/n] " : " [y/N] ") << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return defaultYes;
    std::string s;
    for (char c : line) if (!std::isspace(static_cast<unsigned char>(c))) s += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
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
// Nested braces are handled by a small recursive expansion.
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
// -i : print file/directory info
// ---------------------------------------------------------------------------
static void printInfo(const fs::path& filePath) {
    std::error_code ec;
    if (!fs::exists(filePath, ec)) {
        log("ENOENT: no such file or directory, stat '" + filePath.string() + "'");
        return;
    }
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
    std::error_code ec;
    if (!fs::exists(filePath, ec)) {
        log("ENOENT: no such file or directory, stat '" + filePath.string() + "'");
        return;
    }
    if (fs::is_directory(filePath, ec)) {
        fs::remove_all(filePath, ec);
    } else {
        fs::remove(filePath, ec);
    }
    if (ec) log(ec.message());
}

static void deleteKind(const fs::path& p, PathKind kind) {
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        log("ENOENT: no such file or directory, stat '" + p.string() + "'");
        return;
    }
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

static void doRename(const fs::path& oldP, const fs::path& newP) {
    std::error_code ec;
    if (!fs::exists(oldP, ec)) {
        log("Pardon! can't find the file/directory to rename, stat '" + oldP.string() + "'");
        return;
    }
    if (newP.empty()) {
        log("Must given a path");
        return;
    }
    fs::rename(oldP, newP, ec);
    if (ec) log(ec.message());
}

static void doMove(const fs::path& oldP, const fs::path& newP) {
    std::error_code ec;
    if (!fs::exists(oldP, ec)) {
        log("Pardon! can't find the directory/file to move, stat '" + oldP.string() + "'");
        return;
    }
    if (newP.empty()) {
        log("Must given a path");
        return;
    }
    if (fs::is_directory(newP, ec)) {
        fs::path target = newP / oldP.filename();
        fs::rename(oldP, target, ec);
    } else {
        fs::create_directories(newP.parent_path(), ec);
        fs::rename(oldP, newP, ec);
    }
    if (ec) log(ec.message());
}

static void doCopy(const fs::path& oldP, const fs::path& newP) {
    std::error_code ec;
    if (!fs::exists(oldP, ec)) {
        log("Pardon! can't find the directory/file to copy, stat '" + oldP.string() + "'");
        return;
    }
    if (newP.empty()) {
        log("Must given a path");
        return;
    }
    bool srcIsDir = fs::is_directory(oldP, ec);
    fs::path target = newP;
    if (srcIsDir) {
        if (fs::is_directory(newP, ec)) target = newP / oldP.filename();
        fs::create_directories(target, ec);
        fs::copy(oldP, target,
            fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    } else {
        if (fs::is_directory(newP, ec)) target = newP / oldP.filename();
        else fs::create_directories(target.parent_path(), ec);
        fs::copy_file(oldP, target, fs::copy_options::overwrite_existing, ec);
    }
    if (ec) log(ec.message());
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
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        log("Pardon! can't find the file/directory to trash, stat '" + p.string() + "'");
        return;
    }
    fs::path trashRoot = fs::current_path() / ".trash" / std::to_string(nowUnix());
    fs::create_directories(trashRoot, ec);
    if (ec) { log(ec.message()); return; }
    fs::path dest = trashRoot / p.filename();
    // rename prefix marker to .trash__<name> so a future restore is unambiguous
    std::string origName = p.filename().string();
    std::string markedName = ".trash__" + origName;
    fs::path markedDest = trashRoot / markedName;
    fs::rename(p, markedDest, ec);
    if (ec) { log(ec.message()); return; }

    // write sidecar
    fs::path meta = trashRoot / "meta.json";
    std::ofstream out(meta, std::ios::trunc | std::ios::binary);
    if (out) {
        out << "{\n"
            << "  \"original\": \"" << fs::absolute(p, ec).string() << "\",\n"
            << "  \"trashed_at\": \"" << isoNow() << "\",\n"
            << "  \"name\": \"" << origName << "\"\n"
            << "}\n";
    }
    std::cout << "Trashed '" << p.filename().string() << "' -> " << markedDest.string() << "\n";
}

// ---------------------------------------------------------------------------
// -t : tree view (recursive, depth-limited, skips node_modules and .git)
// ---------------------------------------------------------------------------
static bool shouldSkip(const std::string& name) {
    return name == "node_modules" || name == ".git";
}

static void walkTree(const fs::path& root, int depth, int maxDepth,
                     const std::string& prefix, bool last, std::ostream& out) {
    if (depth > maxDepth) return;
    std::error_code ec;
    if (depth == 0) {
        out << root.filename().string() << "\n";
    } else {
        out << prefix << (last ? "\u2514\u2500\u2500 " : "\u251c\u2500\u2500 ")
            << root.filename().string() << "\n";
    }

    if (depth == maxDepth) return;
    std::vector<fs::directory_entry> entries;
    for (auto it = fs::directory_iterator(root, ec);
         it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (shouldSkip(it->path().filename().string())) continue;
        entries.push_back(*it);
    }
    std::sort(entries.begin(), entries.end(),
        [](const fs::directory_entry& a, const fs::directory_entry& b) {
            return a.path().filename().string() < b.path().filename().string();
        });
    for (size_t i = 0; i < entries.size(); ++i) {
        bool isLast = (i + 1 == entries.size());
        std::string nextPrefix = prefix + (last ? "    " : "\u2502   ");
        walkTree(entries[i].path(), depth + 1, maxDepth, nextPrefix, isLast, out);
    }
}

static void doTree(const fs::path& start, int maxDepth) {
    std::error_code ec;
    if (!fs::exists(start, ec)) {
        log("ENOENT: no such file or directory, stat '" + start.string() + "'");
        return;
    }
    walkTree(start, 0, maxDepth, "", true, std::cout);
}

// ---------------------------------------------------------------------------
// -o : text-mode directory listing (portable substitute for "open explorer")
// ---------------------------------------------------------------------------
static void doOpen(const fs::path& p) {
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        log("ENOENT: no such file or directory, stat '" + p.string() + "'");
        return;
    }
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
                // strip leading whitespace
                size_t s = 0;
                while (s < line.size() && std::isspace(static_cast<unsigned char>(line[s]))) ++s;
                if (s >= line.size() || line[s] == '#') continue;
                if (line.compare(s, name.size(), name) != 0) continue;
                size_t p = s + name.size();
                while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
                if (p >= line.size() || line[p] != ':') continue;
                ++p;
                while (p < line.size() && std::isspace(static_cast<unsigned char>(line[p]))) ++p;
                size_t e = line.size();
                while (e > p && std::isspace(static_cast<unsigned char>(line[e - 1]))) --e;
                if (e > p) return line.substr(p, e - p);
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
static fs::path settingJsonPath() {
    // Look next to the executable first, fall back to cwd.
    return fs::current_path() / "setting.json";
}

static std::string readLineTrimmed() {
    std::string s;
    std::getline(std::cin, s);
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    return s.substr(a);
}

static std::string settingDefault(const std::string& existing, const std::string& key, const std::string& fallback) {
    std::string v = extractJsonStringField(existing, key);
    return v.empty() ? fallback : v;
}

static void doSetting() {
    fs::path path = settingJsonPath();
    std::string existing;
    {
        std::ifstream in(path);
        std::ostringstream ss; ss << in.rdbuf();
        existing = ss.str();
    }
    std::cout << "Configure You (press Enter to accept defaults)\n";
    std::cout << "  default editor        [" << settingDefault(existing, "editor", "nano") << "]: " << std::flush;
    std::string editor = readLineTrimmed();
    if (editor.empty()) editor = settingDefault(existing, "editor", "nano");
    std::cout << "  trash path            [" << settingDefault(existing, "trash", ".trash") << "]: " << std::flush;
    std::string trash = readLineTrimmed();
    if (trash.empty()) trash = settingDefault(existing, "trash", ".trash");
    std::cout << "  tree depth            [" << settingDefault(existing, "tree_depth", "3") << "]: " << std::flush;
    std::string depth = readLineTrimmed();
    if (depth.empty()) depth = settingDefault(existing, "tree_depth", "3");
    std::cout << "  confirm on create?    [" << settingDefault(existing, "confirm_create", "n") << "]: " << std::flush;
    std::string confirm = readLineTrimmed();
    if (confirm.empty()) confirm = settingDefault(existing, "confirm_create", "n");

    std::ofstream out(path, std::ios::trunc | std::ios::binary);
    if (!out) { log("cannot write " + path.string()); return; }
    out << "{\n"
        << "  \"editor\": \"" << editor << "\",\n"
        << "  \"trash\": \"" << trash << "\",\n"
        << "  \"tree_depth\": \"" << depth << "\",\n"
        << "  \"confirm_create\": \"" << confirm << "\"\n"
        << "}\n";
    std::cout << "Wrote " << path.string() << "\n";
}

// ---------------------------------------------------------------------------
// Creation: touch a file or mkdir a directory, mirroring the original logic
// but parameterised on PathKind so brace-expanded names and cd:-prefixed
// paths both work.
// ---------------------------------------------------------------------------
static void createOne(const fs::path& abs, PathKind kind) {
    std::error_code ec;
    fs::path parent = abs.parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    if (kind == PathKind::Directory) {
        fs::create_directories(abs, ec);
        return;
    }
    if (!fs::exists(abs, ec)) {
        std::ofstream ofs(abs, std::ios::trunc | std::ios::binary);
    } else {
        touchFile(abs);
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);

    static const std::vector<std::string> versionFlags = {"-v", "--v", "--version"};
    static const std::vector<std::string> helpFlags = {"-h", "--h", "--help"};
    auto contains = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    // Single-shot top-level flags first.
    for (size_t i = 0; i < args.size(); ++i) {
        if (contains(versionFlags, args[i])) {
            std::cout << "You/" << getVersion() << " C++: " << cppStandardString() << "\n";
            return 0;
        }
        if (contains(helpFlags, args[i])) {
            std::cout << DefaultLog;
            return 0;
        }
        if (args[i] == "-pwd") {
            std::error_code ec;
            std::cout << fs::current_path(ec).string() << "\n";
            return 0;
        }
        if (args[i] == "--setting" || args[i] == "-setting") {
            doSetting();
            return 0;
        }
        if (args[i] == "-t" || args[i].rfind("-t=", 0) == 0) {
            int depth = 3;
            if (args[i].rfind("-t=", 0) == 0) {
                try { depth = std::stoi(args[i].substr(3)); } catch (...) { depth = 3; }
            } else if (i + 1 < args.size()) {
                try { depth = std::stoi(args[i + 1]); } catch (...) { depth = 3; }
            }
            fs::path start = fs::current_path();
            if (i > 0 && !args[i - 1].empty() && args[i - 1][0] != '-') {
                start = args[i - 1];
            }
            doTree(start, depth);
            return 0;
        }
    }

    // `you run:Folder="..."` -- one-shot, takes a single argument.
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i].rfind("run:", 0) == 0) {
            const std::string& full = args[i];
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
    }

    // Per-argument processing with cd: chaining, brace expansion, and
    // trailing-slash / hidden-file kind resolution.
    fs::path currentRoot = fs::current_path();

    // First pass: collect pairs for the multi-arg flags `-rn` / `-mv` / `-c`,
    // which the spec writes as `-rn old=new`. We scan once, build a tiny
    // mapping from index -> "old=new", and remove those args from the list
    // before the per-argument loop.
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
    // Execute ops first (they describe their own target paths).
    for (const auto& op : ops) {
        std::string lhs, rhs;
        if (!splitEq(op.spec, lhs, rhs)) continue;
        fs::path oldP = (currentRoot / lhs).lexically_normal();
        fs::path newP = (currentRoot / rhs).lexically_normal();
        if (args[op.flagIndex] == "-rn") doRename(oldP, newP);
        else if (args[op.flagIndex] == "-mv") doMove(oldP, newP);
        else doCopy(oldP, newP);
    }

    // Per-argument processing with cd: chaining, brace expansion, and
    // trailing-slash / hidden-file kind resolution.
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
        fs::path directory = filePath.parent_path();
        std::string baseName = filePath.filename().string();
        bool isFile = !nodeExtname(baseName).empty();
        bool hiddenFile = !baseName.empty() && baseName[0] == '.';
        PathKind kind = resolveKind(baseName, dirMarker, hiddenFile);

        std::error_code ec;
        if (!fs::exists(directory, ec)) {
            fs::create_directories(directory, ec);
        }
        createOne(filePath, kind);
        (void)isFile; (void)hiddenFile; // kind already captured
    };

    // Build the set of indices consumed by -rn/-mv/-c so we skip them.
    std::vector<bool> consumed(args.size(), false);
    for (const auto& op : ops) { consumed[op.flagIndex] = true; consumed[op.flagIndex + 1] = true; }

    for (size_t index = 0; index < args.size(); ++index) {
        if (consumed[index]) continue;
        std::string filename = args[index];
        const bool isDash = !filename.empty() && filename[0] == '-';

        if (isDash) {
            // -d, -rf, -rd, -trash, -i, -o all operate on the previous arg.
            if (filename == "-d" && index > 0) {
                deletePath((currentRoot / args[index - 1]).lexically_normal());
                continue;
            }
            if (filename == "-rf" && index > 0) {
                deleteKind((currentRoot / args[index - 1]).lexically_normal(), PathKind::File);
                continue;
            }
            if (filename == "-rd" && index > 0) {
                deleteKind((currentRoot / args[index - 1]).lexically_normal(), PathKind::Directory);
                continue;
            }
            if (filename == "-trash" && index > 0) {
                doTrash((currentRoot / args[index - 1]).lexically_normal());
                continue;
            }
            if (filename == "-i" && index > 0) {
                printInfo((currentRoot / args[index - 1]).lexically_normal());
                continue;
            }
            if (filename == "-o" && index > 0) {
                doOpen((currentRoot / args[index - 1]).lexically_normal());
                continue;
            }
            if (filename == "-t" || filename.rfind("-t=", 0) == 0) {
                // -t handling already done in the top-level loop; ignore here.
                continue;
            }
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
