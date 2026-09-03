// port/gdx_workshop.cpp — Workshop texture-pack + dump implementation. See gdx_workshop.h.
//
// Everything here is opt-in (CVars default OFF); with the CVars off this file's only runtime cost is
// a single cached CVar read per registered texture load, and no ResourceManager or disk access.

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "gdx_workshop.h"

#include "ship/Context.h"
#include "ship/resource/ResourceManager.h"
#include "ship/resource/File.h"
#include "ship/resource/archive/ArchiveManager.h"
#include "libultraship/bridge/consolevariablebridge.h"
#include "port_log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// stb single-header PNG writer, vendored beside this file (port/gdx_stb_image_write.h). This is the
// one TU that defines STB_IMAGE_WRITE_IMPLEMENTATION, so it never leaks across the torch tree.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO_DEPRECATION
#include "gdx_stb_image_write.h"

// Loaded-asset registry reverse lookup (port/AssetLoader.cpp): decoded RDRAM ptr -> asset key.
extern "C" const char* GDiffuser_LookupLoadedAssetKey(const void* buffer, size_t minSize, int requireUnmodified);
// Interpreter texture-cache full clear (libultraship/src/fast/interpreter.cpp).
extern "C" void gfx_texture_cache_clear(void);

const char* kGdxWorkshopKeySchemeVersion = "1";

namespace {

// ── pack epoch + override-path cache ──────────────────────────────────────────────────────────────
std::mutex gCacheMutex;
uint32_t gPackEpoch = 0;
// key -> full override path ("textures/pack/<key>") when a mounted pack provides it; empty string
// means "checked, no override". Presence in the map means "already checked this epoch".
std::unordered_map<std::string, std::string> gOverrideCache;

// ── dump de-dup (first-seen-wins per session) ─────────────────────────────────────────────────────
std::mutex gDumpMutex;
std::unordered_set<std::string> gDumpSeen;

// ── contact-sheet regen debounce ──────────────────────────────────────────────────────────────────
// Regenerating dump/index.html per dumped texture turns a level-load burst of ~1000 first-seen
// textures into ~1000 synchronous full-HTML rewrites, so the dump path only marks the sheet dirty and
// regenerates once per throttle window. gdx_workshop_dump_count (polled per frame while the dump
// section is open) force-flushes the pending state, so index.html is complete whenever a modder looks
// at it. Its own mutex, so file IO never runs under gDumpMutex.
std::mutex gSheetMutex;
bool gSheetDirty = false;
std::chrono::steady_clock::time_point gSheetLastWrite;  // default-constructed = epoch => first dump writes immediately
constexpr std::chrono::milliseconds kContactSheetThrottle{2000};

std::string toLower(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// crc64 (ECMA polynomial) for the unnamed-texture hash namespace. Table built on first use.
uint64_t crc64(const void* data, size_t len) {
    static uint64_t table[256];
    static bool init = false;
    if (!init) {
        const uint64_t poly = 0xC96C5795D7870F42ull;
        for (uint32_t i = 0; i < 256; i++) {
            uint64_t crc = i;
            for (int k = 0; k < 8; k++) {
                crc = (crc & 1) ? (crc >> 1) ^ poly : (crc >> 1);
            }
            table[i] = crc;
        }
        init = true;
    }
    uint64_t crc = ~0ull;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

// Executable directory: mods/ and dump/ live beside the running binary (like the copied f3d.o2r).
std::filesystem::path exeDir() {
    std::error_code ec;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        return std::filesystem::path(buf).parent_path();
    }
#endif
    return std::filesystem::current_path(ec);
}

std::filesystem::path resolveDir(const char* leaf, bool createIfMissing) {
    std::error_code ec;
    std::filesystem::path dir = exeDir() / leaf;
    if (createIfMissing && !std::filesystem::is_directory(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
    }
    return dir;
}

std::shared_ptr<Ship::ArchiveManager> archiveManager() {
    auto ctx = Ship::Context::GetInstance();
    if (ctx == nullptr) {
        return nullptr;
    }
    auto rm = ctx->GetResourceManager();
    if (rm == nullptr) {
        return nullptr;
    }
    return rm->GetArchiveManager();
}

// Comma-joined basename disable list (gEnhancements.Workshop.DisabledPacks). Case-insensitive.
bool packDisabled(const std::string& basename) {
    const char* raw = CVarGetString("gEnhancements.Workshop.DisabledPacks", "");
    if (raw == nullptr || raw[0] == '\0') {
        return false;
    }
    const std::string target = toLower(basename);
    std::string list = raw;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        size_t end = (comma == std::string::npos) ? list.size() : comma;
        std::string token = list.substr(start, end - start);
        size_t b = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (b != std::string::npos) {
            token = token.substr(b, e - b + 1);
            if (toLower(token) == target) {
                return true;
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return false;
}

const char* n64FormatName(int fmt, int siz) {
    // G_IM_FMT: 0=RGBA 1=YUV 2=CI 3=IA 4=I.  G_IM_SIZ: 0=4b 1=8b 2=16b 3=32b.
    static const char* kNames[5][4] = {
        { "RGBA4", "RGBA8", "RGBA16", "RGBA32" },
        { "YUV4", "YUV8", "YUV16", "YUV32" },
        { "CI4", "CI8", "CI16", "CI32" },
        { "IA4", "IA8", "IA16", "IA32" },
        { "I4", "I8", "I16", "I32" },
    };
    if (fmt >= 0 && fmt < 5 && siz >= 0 && siz < 4) {
        return kNames[fmt][siz];
    }
    return "UNK";
}

// HTML-escape a string for safe embedding in text and double-quoted attributes.
std::string htmlEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c; break;
        }
    }
    return out;
}

// Regenerate dump/index.html — a self-contained contact sheet of every dumped texture (thumbnail of
// the sibling PNG, key, dimensions, N64 format) so a modder can find the texture they saw on screen
// and copy its exact key. Rebuilt from manifest.tsv, which spans sessions, so it always covers the
// full first-seen set. Best-effort: any failure is silently ignored.
void writeContactSheet(const std::filesystem::path& dumpDir) {
    std::filesystem::path tsv = dumpDir / "manifest.tsv";
    FILE* in = std::fopen(tsv.string().c_str(), "rb");
    if (in == nullptr) {
        return;
    }
    // Rows: (key, w, h, fmt). Keep insertion order; manifest.tsv is already first-seen ordered.
    struct Row { std::string key, w, h, fmt; };
    std::vector<Row> rows;
    {
        std::string content;
        char chunk[8192];
        size_t n;
        while ((n = std::fread(chunk, 1, sizeof(chunk), in)) > 0) {
            content.append(chunk, n);
        }
        std::fclose(in);
        size_t start = 0;
        while (start < content.size()) {
            size_t nl = content.find('\n', start);
            size_t end = (nl == std::string::npos) ? content.size() : nl;
            std::string line = content.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            start = (nl == std::string::npos) ? content.size() : nl + 1;
            if (line.empty() || line[0] == '#') {
                continue;
            }
            Row r;
            size_t t0 = line.find('\t');
            if (t0 == std::string::npos) continue;
            r.key = line.substr(0, t0);
            size_t t1 = line.find('\t', t0 + 1);
            if (t1 == std::string::npos) continue;
            r.w = line.substr(t0 + 1, t1 - t0 - 1);
            size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            r.h = line.substr(t1 + 1, t2 - t1 - 1);
            r.fmt = line.substr(t2 + 1);
            rows.push_back(std::move(r));
        }
    }

    std::string html;
    html.reserve(rows.size() * 256 + 2048);
    html += "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += "<title>G-Diffuser texture dump</title>\n<style>\n";
    html += "  body{font:13px/1.4 system-ui,sans-serif;margin:0;padding:16px;background:#1b1d22;color:#e7e9ee}\n";
    html += "  h1{font-size:18px;margin:0 0 4px}\n";
    html += "  p.sub{margin:0 0 16px;color:#9aa0ab}\n";
    html += "  .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}\n";
    html += "  .cell{background:#24272e;border:1px solid #333842;border-radius:8px;padding:8px;overflow:hidden}\n";
    html += "  .thumb{display:flex;align-items:center;justify-content:center;height:120px;margin-bottom:8px;\n";
    html += "    background-image:linear-gradient(45deg,#3a3f48 25%,transparent 25%,transparent 75%,#3a3f48 75%),\n";
    html += "      linear-gradient(45deg,#3a3f48 25%,#2b2f36 25%,#2b2f36 75%,#3a3f48 75%);\n";
    html += "    background-size:16px 16px;background-position:0 0,8px 8px;border-radius:4px}\n";
    html += "  .thumb img{max-width:100%;max-height:120px;image-rendering:pixelated}\n";
    html += "  .key{font-family:ui-monospace,Consolas,monospace;font-size:11px;word-break:break-all;\n";
    html += "    user-select:all;color:#cdd3dd}\n";
    html += "  .meta{color:#9aa0ab;font-size:11px;margin-top:2px}\n";
    html += "  .tag{display:inline-block;padding:0 5px;border-radius:4px;background:#333842;font-size:10px}\n";
    html += "  .hash{color:#c9a24b}\n";
    html += "  input#f{width:100%;box-sizing:border-box;padding:8px;margin-bottom:16px;border-radius:6px;\n";
    html += "    border:1px solid #333842;background:#24272e;color:#e7e9ee;font-size:13px}\n";
    html += "</style></head><body>\n";
    html += "<h1>G-Diffuser texture dump</h1>\n";
    char sub[128];
    std::snprintf(sub, sizeof(sub), "%zu texture(s). Click a key to select it, then copy it into your pack folder as &lt;key&gt;.png.", rows.size());
    html += "<p class=\"sub\">" ; html += sub; html += "</p>\n";
    html += "<input id=\"f\" type=\"text\" placeholder=\"Filter by key or format...\" oninput=\"var q=this.value.toLowerCase();document.querySelectorAll('.cell').forEach(function(c){c.style.display=c.dataset.s.indexOf(q)<0?'none':''})\">\n";
    html += "<div class=\"grid\">\n";
    for (const Row& r : rows) {
        const bool isHash = r.key.rfind("hash/", 0) == 0;
        std::string keyEsc = htmlEscape(r.key);
        std::string search = toLower(r.key + " " + r.fmt);
        html += "  <div class=\"cell\" data-s=\"" + htmlEscape(search) + "\">";
        html += "<div class=\"thumb\"><img loading=\"lazy\" src=\"" + keyEsc + ".png\" alt=\"" + keyEsc + "\"></div>";
        html += "<div class=\"key" ; html += (isHash ? " hash" : ""); html += "\">" + keyEsc + "</div>";
        html += "<div class=\"meta\">" + htmlEscape(r.w) + "&times;" + htmlEscape(r.h) +
                " <span class=\"tag\">" + htmlEscape(r.fmt) + "</span>";
        if (isHash) {
            html += " <span class=\"tag hash\" title=\"Unnamed texture: reference only, not replaceable by a pack yet\">no key</span>";
        }
        html += "</div></div>\n";
    }
    html += "</div>\n</body></html>\n";

    std::filesystem::path indexPath = dumpDir / "index.html";
    FILE* out = std::fopen(indexPath.string().c_str(), "wb");
    if (out != nullptr) {
        std::fwrite(html.data(), 1, html.size(), out);
        std::fclose(out);
    }
}

// force=false from the dump hot path (coalesces bursts); force=true from the menu status path, which
// must not show a stale sheet.
void regenContactSheetIfDue(const std::filesystem::path& dumpDir, bool force) {
    std::lock_guard<std::mutex> lock(gSheetMutex);
    if (!gSheetDirty) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!force && (now - gSheetLastWrite) < kContactSheetThrottle) {
        return;  // within the throttle window: leave dirty, a later dump or menu poll will flush it
    }
    writeContactSheet(dumpDir);
    gSheetDirty = false;
    gSheetLastWrite = now;
}

void markContactSheetDirty() {
    std::lock_guard<std::mutex> lock(gSheetMutex);
    gSheetDirty = true;
}

// Minimal flat-JSON string-field extractor: finds "key" : "value". Good enough for pack manifests
// (name/version/author/game_version/key_scheme_version). Returns empty when absent.
std::string jsonField(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) {
        return "";
    }
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) {
        return "";
    }
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) {
        return "";
    }
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) {
        return "";
    }
    return json.substr(q1 + 1, q2 - q1 - 1);
}

} // namespace

// ── Tier-B override shim ──────────────────────────────────────────────────────────────────────────
extern "C" int gdx_workshop_texture_packs_enabled(void) {
    return CVarGetInteger("gEnhancements.Workshop.TexturePacks", 0) != 0;
}

extern "C" const char* GdxWorkshopLookupOverridePath(const char* key) {
    if (key == nullptr || key[0] == '\0') {
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(gCacheMutex);
    auto it = gOverrideCache.find(key);
    if (it != gOverrideCache.end()) {
        return it->second.empty() ? nullptr : it->second.c_str();
    }
    // First miss for this key this epoch: consult the ResourceManager exactly once.
    std::string overridePath = std::string("textures/pack/") + key;
    bool exists = false;
    auto am = archiveManager();
    if (am != nullptr) {
        exists = am->HasFile(overridePath);
    }
    auto ins = gOverrideCache.emplace(std::string(key), exists ? overridePath : std::string());
    return ins.first->second.empty() ? nullptr : ins.first->second.c_str();
}

// ── Texture dump ──────────────────────────────────────────────────────────────────────────────────
extern "C" int gdx_workshop_texture_dump_enabled(void) {
    return CVarGetInteger("gEnhancements.Workshop.TextureDump", 0) != 0;
}

extern "C" int gdx_workshop_dump_count(void) {
    // The menu polls this every frame the dump section is open, which makes it the flush point for a
    // contact sheet still dirty inside the debounce window. No-op when clean.
    regenContactSheetIfDue(resolveDir("dump", false), /*force=*/true);
    std::lock_guard<std::mutex> lock(gDumpMutex);
    return static_cast<int>(gDumpSeen.size());
}

extern "C" void GdxWorkshopSetPackDisabled(const char* basename, int disabled) {
    if (basename == nullptr || basename[0] == '\0') {
        return;
    }
    const std::string target = basename;
    const std::string targetLower = toLower(target);

    // Parse the current comma-joined list into trimmed tokens (case-insensitive de-dup on target).
    const char* raw = CVarGetString("gEnhancements.Workshop.DisabledPacks", "");
    std::vector<std::string> tokens;
    std::string list = (raw != nullptr) ? raw : "";
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        size_t end = (comma == std::string::npos) ? list.size() : comma;
        std::string token = list.substr(start, end - start);
        size_t b = token.find_first_not_of(" \t");
        size_t e = token.find_last_not_of(" \t");
        if (b != std::string::npos) {
            token = token.substr(b, e - b + 1);
            if (toLower(token) != targetLower) {
                tokens.push_back(token);
            }
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    if (disabled != 0) {
        tokens.push_back(target);
    }

    std::string joined;
    for (size_t i = 0; i < tokens.size(); i++) {
        if (i != 0) {
            joined += ",";
        }
        joined += tokens[i];
    }
    CVarSetString("gEnhancements.Workshop.DisabledPacks", joined.c_str());
    CVarSave();
}

extern "C" void gdx_workshop_dump_texture(const void* origSrcAddr, size_t origSrcLen, const char* resourcePathOrNull,
                                          const uint8_t* rgba32, int width, int height, int n64Fmt, int n64Siz) {
    if (rgba32 == nullptr || width <= 0 || height <= 0) {
        return;
    }

    // Resolve an identity key. Priority: OTR resource path (Tier A/B) -> loaded-asset registry key
    // (Tier B raw copies) -> content hash namespace (unnamed / Tier C).
    std::string key;
    if (resourcePathOrNull != nullptr && resourcePathOrNull[0] != '\0') {
        key = resourcePathOrNull;
        // Strip our own override prefix so a re-dump keeps the canonical key.
        const std::string prefix = "textures/pack/";
        if (key.rfind(prefix, 0) == 0) {
            key = key.substr(prefix.size());
        }
    } else if (origSrcAddr != nullptr) {
        const char* k = GDiffuser_LookupLoadedAssetKey(origSrcAddr, 0, 0);
        if (k != nullptr && k[0] != '\0') {
            key = k;
        }
    }
    if (key.empty()) {
        char hex[32];
        uint64_t h = (origSrcAddr != nullptr && origSrcLen > 0)
                         ? crc64(origSrcAddr, origSrcLen)
                         : crc64(rgba32, static_cast<size_t>(width) * height * 4);
        std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
        key = std::string("hash/") + hex;
    }

    {
        std::lock_guard<std::mutex> lock(gDumpMutex);
        if (!gDumpSeen.insert(key).second) {
            return; // already dumped this session
        }
    }

    std::filesystem::path dumpDir = resolveDir("dump", true);
    if (dumpDir.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::path pngPath = dumpDir / (key + ".png");
    if (std::filesystem::exists(pngPath, ec)) {
        return; // first-seen-wins across sessions too
    }
    std::filesystem::create_directories(pngPath.parent_path(), ec);

    if (stbi_write_png(pngPath.string().c_str(), width, height, 4, rgba32, width * 4) == 0) {
        gdx_port_logf("[workshop] dump FAILED to write %s\n", pngPath.string().c_str());
        return;
    }

    // Append to dump/manifest.tsv: key<TAB>w<TAB>h<TAB>fmt. Write a header comment line the first
    // time the file is created (the packer and any TSV reader skip '#'-prefixed lines).
    std::filesystem::path tsv = dumpDir / "manifest.tsv";
    const bool tsvIsNew = !std::filesystem::exists(tsv, ec);
    FILE* f = std::fopen(tsv.string().c_str(), "ab");
    if (f != nullptr) {
        if (tsvIsNew) {
            std::fprintf(f, "# key\tnative_w\tnative_h\tn64_fmt   (one row per dumped texture)\n");
        }
        std::fprintf(f, "%s\t%d\t%d\t%s\n", key.c_str(), width, height, n64FormatName(n64Fmt, n64Siz));
        std::fclose(f);
        // Debounced regen; a pending dirty state is force-flushed by gdx_workshop_dump_count.
        markContactSheetDirty();
        regenContactSheetIfDue(dumpDir, /*force=*/false);
    }
}

// ── Hot reload ────────────────────────────────────────────────────────────────────────────────────
extern "C" void GdxWorkshopReload(char* outStatus, size_t outStatusLen) {
    auto setStatus = [&](const std::string& s) {
        if (outStatus != nullptr && outStatusLen > 0) {
            std::snprintf(outStatus, outStatusLen, "%s", s.c_str());
        }
    };

    auto ctx = Ship::Context::GetInstance();
    auto rm = (ctx != nullptr) ? ctx->GetResourceManager() : nullptr;
    auto am = (rm != nullptr) ? rm->GetArchiveManager() : nullptr;
    if (rm == nullptr || am == nullptr) {
        setStatus("Reload failed: resource manager unavailable.");
        return;
    }

    rm->DirtyResources("textures/pack/*");

    // Quiesce the resource thread pool before touching the ArchiveManager: DirtyResources queues an
    // async worker that iterates the archive manager's file table, and remounting archives under it
    // is a use-after-free (worker crash in ListFiles). The ArchiveManager carries its own lock now,
    // but the swap still belongs against an idle pool.
    rm->WaitForAsyncTasks();

    std::filesystem::path modsDir = resolveDir("mods", false);
    std::string modsKey = toLower(modsDir.string());
    auto archives = am->GetArchives();
    if (archives != nullptr) {
        // Copy paths first; RemoveArchive mutates the collection.
        std::vector<std::string> toRemove;
        for (const auto& ar : *archives) {
            if (ar == nullptr) {
                continue;
            }
            if (toLower(ar->GetPath()).find(modsKey) != std::string::npos && !modsKey.empty()) {
                toRemove.push_back(ar->GetPath());
            }
        }
        for (const auto& p : toRemove) {
            am->RemoveArchive(p);
        }
    }

    int mounted = 0;
    std::error_code ec;
    if (std::filesystem::is_directory(modsDir, ec)) {
        std::vector<std::string> packs;
        for (const auto& entry : std::filesystem::directory_iterator(modsDir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            if (toLower(entry.path().extension().string()) != ".o2r") {
                continue;
            }
            if (packDisabled(entry.path().filename().string())) {
                continue;
            }
            packs.push_back(std::filesystem::absolute(entry.path(), ec).string());
        }
        std::sort(packs.begin(), packs.end(),
                  [](const std::string& a, const std::string& b) { return toLower(a) < toLower(b); });
        for (const auto& p : packs) {
            if (am->AddArchive(p) != nullptr) {
                mounted++;
            }
        }
    }
    // AddArchive/RemoveArchive rebuild the virtual file system internally, so no explicit
    // ResetVirtualFileSystem call is needed here (and the method is protected anyway).

    gfx_texture_cache_clear();

    // Bump the pack epoch to invalidate the override cache.
    {
        std::lock_guard<std::mutex> lock(gCacheMutex);
        gPackEpoch++;
        gOverrideCache.clear();
    }

    int overrides = GdxWorkshopOverrideCount();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Reloaded: %d pack(s) mounted, %d override(s) available.", mounted, overrides);
    setStatus(buf);
    gdx_port_logf("[workshop] reload: %d pack(s), %d override(s)\n", mounted, overrides);
}

// ── Menu-facing helpers (C++) ─────────────────────────────────────────────────────────────────────
int GdxWorkshopOverrideCount() {
    auto am = archiveManager();
    if (am == nullptr) {
        return 0;
    }
    auto files = am->ListFiles("textures/pack/*");
    return (files != nullptr) ? static_cast<int>(files->size()) : 0;
}

std::string GdxWorkshopModsDir(bool createIfMissing) {
    return resolveDir("mods", createIfMissing).string();
}

std::string GdxWorkshopDumpDir(bool createIfMissing) {
    return resolveDir("dump", createIfMissing).string();
}

std::vector<GdxWorkshopPackInfo> GdxWorkshopListPacks() {
    std::vector<GdxWorkshopPackInfo> out;
    std::error_code ec;
    std::filesystem::path modsDir = resolveDir("mods", false);
    if (!std::filesystem::is_directory(modsDir, ec)) {
        return out;
    }

    // Best-effort manifest from the mounted VFS (highest-priority pack wins on path collision), read
    // once and applied to every row for the mismatch banner. Packs use "workshop.json" because
    // "manifest.json" is libultraship's reserved archive manifest, whose numeric game_version schema
    // makes LUS's parser throw on our string game_version at every mount; the reserved name stays in
    // the probe list only to keep pre-rename packs readable.
    std::string manifestJson;
    if (auto am = archiveManager()) {
        for (const char* manifestName : { "workshop.json", "manifest.json" }) {
            if (am->HasFile(manifestName)) {
                if (auto file = am->LoadFile(manifestName)) {
                    if (file->Buffer != nullptr && !file->Buffer->empty()) {
                        manifestJson.assign(file->Buffer->begin(), file->Buffer->end());
                        break;
                    }
                }
            }
        }
    }
    const std::string mName = jsonField(manifestJson, "name");
    const std::string mVersion = jsonField(manifestJson, "version");
    const std::string mAuthor = jsonField(manifestJson, "author");
    const std::string mGameVersion = jsonField(manifestJson, "game_version");
    const std::string mKeyScheme = jsonField(manifestJson, "key_scheme_version");

    for (const auto& entry : std::filesystem::directory_iterator(modsDir, ec)) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        if (toLower(entry.path().extension().string()) != ".o2r") {
            continue;
        }
        GdxWorkshopPackInfo info;
        info.basename = entry.path().filename().string();
        info.path = std::filesystem::absolute(entry.path(), ec).string();
        info.disabled = packDisabled(info.basename);
        info.manifestPresent = !manifestJson.empty();
        info.name = mName;
        info.version = mVersion;
        info.author = mAuthor;
        info.gameVersion = mGameVersion;
        info.keySchemeVersion = mKeyScheme;
        // Port build version is "us.rev0" (VERSION_US). A manifest declaring a different
        // game_version is flagged; empty is treated as "unspecified" (no warning).
        info.gameVersionMismatch = !mGameVersion.empty() && mGameVersion != "us.rev0" &&
                                    mGameVersion != "us" && mGameVersion != "US";
        info.keySchemeMismatch = !mKeyScheme.empty() && mKeyScheme != kGdxWorkshopKeySchemeVersion;
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(), [](const GdxWorkshopPackInfo& a, const GdxWorkshopPackInfo& b) {
        return toLower(a.basename) < toLower(b.basename);
    });
    return out;
}
