#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
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
// Usage text (kept verbatim from the original tool)
// ---------------------------------------------------------------------------
static const char* const DefaultLog =
R"(Usage:
you [fileName / directoryName] [flag]


Flags:
--help, -h, --h                Show this message
--version, -v, --h             Display the version
-d                             Delete File or Directory
-i                             Get information of File or Directory
Examples:
you index.js               to create a new index.js file
you index                  to create a new index directory
you index.js route.js      to create multiple files
you index route            to create multiple directory
you FolderName/            to explicitly create a directory
you index.js -d            to delete a file
you index -d               to delete a directory
you index.js -i            to get info of a file
you index -i               to get info of a directory
)";

// ---------------------------------------------------------------------------
// log(): mirrors the original's error logger (writes to stderr)
// ---------------------------------------------------------------------------
static void log(const std::string& err) {
    std::cerr << "File or Directory does not exist or sometinge else.... \n" << err << "\n";
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
// A small built-in MIME lookup, standing in for the `mime` npm package.
// Not exhaustive -- extend freely.
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
// Version: the original read pkg.version via require('./../package.json').
// This looks for a package.json next to the executable (cwd, then one dir
// up) and pulls out "version" with a tiny hand-rolled parser; falls back to
// a constant if none is found.
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
// Cross-platform file time retrieval (birth/creation + modified time) and
// "touch" (equivalent of the original's futimesSync(now, now)).
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
    _wutime(p.wstring().c_str(), nullptr); // NULL => set both times to "now"
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
        // Linux's stat() exposes no portable birth time; ctime is the
        // closest approximation available (Node has the same limitation
        // here on Linux).
        result.birth = st.st_ctime;
        #endif
    }
    return result;
}

static void touchFile(const fs::path& p) {
    utime(p.c_str(), nullptr); // NULL => set both atime and mtime to "now"
}
#endif

// ---------------------------------------------------------------------------
// Formatting helpers used by the info table
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
// console.table() emulation. A JS `null` is std::nullopt; a JS string is a
// std::string and gets single-quoted, matching Node's default inspector.
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
// -d : delete a file or directory
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

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    std::vector<std::string> args(argv + 1, argv + argc);
    const fs::path root = fs::current_path();

    static const std::vector<std::string> versionFlags = {"-v", "--v", "--version"};
    static const std::vector<std::string> helpFlags = {"-h", "--h", "--help"};
    auto contains = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    for (size_t index = 0; index < args.size(); ++index) {
        const std::string& filename = args[index];
        const bool isDash = !filename.empty() && filename[0] == '-';
        const bool isVersion = contains(versionFlags, filename);
        const bool isHelp = contains(helpFlags, filename);

        if (isVersion) {
            std::cout << "You/" << getVersion() << " C++: " << cppStandardString() << "\n";
            return 0;
        } else if (isHelp) {
            std::cout << DefaultLog;
            return 0;
        } else if (!isDash) {
            // Detect an explicit trailing "/" -- used only to mark a path as
            // a directory at creation time. The slash itself is stripped so
            // it does not become part of the filesystem path. It has no
            // meaning for -d / -i; those just operate on the resolved path.
            bool dirMarker = filename.size() > 1 && filename.back() == '/';
            std::string cleanName = filename;
            if (dirMarker) {
                while (!cleanName.empty() && cleanName.back() == '/') {
                    cleanName.pop_back();
                }
                if (cleanName.empty()) cleanName = "/";
            }

            fs::path filePath = (root / cleanName).lexically_normal();
            fs::path directory = filePath.parent_path();
            std::string baseName = filePath.filename().string();
            bool isFile = !nodeExtname(baseName).empty();
            bool hiddenFile = !baseName.empty() && baseName[0] == '.';

            bool toDelete = (index + 1 < args.size()) && args[index + 1] == "-d";
            bool toInfo = (index + 1 < args.size()) && args[index + 1] == "-i";
            bool isNextDash = (index + 1 < args.size()) && !args[index + 1].empty() && args[index + 1][0] == '-';
            bool falsehood = !toDelete && !toInfo && !isNextDash;

            std::error_code ec;
            bool dirExists = fs::exists(directory, ec);

            if (!dirExists && falsehood) {
                fs::create_directories(directory, ec);
                if (dirMarker) {
                    fs::create_directories(filePath, ec);
                } else {
                    std::ofstream ofs(filePath, std::ios::trunc | std::ios::binary);
                }
            } else if (dirExists && falsehood) {
                if (dirMarker) {
                    fs::create_directories(filePath, ec);
                } else if (isFile || hiddenFile) {
                    if (!fs::exists(filePath, ec)) {
                        std::ofstream ofs(filePath, std::ios::trunc | std::ios::binary);
                    } else {
                        touchFile(filePath);
                    }
                } else {
                    fs::create_directories(filePath, ec);
                }
            } else if (toDelete) {
                deletePath(filePath);
            } else if (toInfo) {
                printInfo(filePath);
            } else {
                std::cout << "Pardon! The command does not exist..\n\n" << DefaultLog;
                return 0;
            }
        }
    }
    return 0;
}
