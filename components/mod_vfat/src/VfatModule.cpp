/**
 * \file VfatModule.cpp
 * \brief vFAT base module: Tools->Expert explorer + "VFAT" serial shell.
 */

#include "mod_vfat/VfatModule.h"
#include "VfatFs.h"
#include "VfatExplorerView.h"

#include "cdc_core/ModuleRegistry.h"
#include "cdc_core/EventBus.h"
#include "serial_cmd/ICommandRegistry.h"
#include "serial_cmd/SubCommand.h"
#include "serial_cmd/Console.h"
#include "cdc_ui/I18n.h"
#include "cdc_core/Cp437.h"
#include "plugin_manager/PluginSerialCommands.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace cdc::mod_vfat {

namespace {

using cdc::serial::Console;

/// Serial shell working directory, relative to the partition root.
std::string s_cwd;

std::string relOf(const std::string& name)
{
    return s_cwd.empty() ? name : s_cwd + "/" + name;
}

std::string trimmed(const char* s)
{
    if (!s) return {};
    while (*s == ' ' || *s == '\t') ++s;
    std::string r(s);
    while (!r.empty() && (r.back() == ' ' || r.back() == '\t' ||
                          r.back() == '\r' || r.back() == '\n')) {
        r.pop_back();
    }
    // Serial input arrives as CP437 (the RX layer maps UTF-8 -> CP437); store
    // file names and contents as UTF-8 so the partition stays UTF-8.
    return cdc::core::cp437::toUtf8(r.c_str());
}

/// Translate the literal escapes \n, \t and \\ in a PUT payload.
std::string unescape(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '\\' && i + 1 < in.size()) {
            char n = in[i + 1];
            if (n == 'n')  { out.push_back('\n'); ++i; continue; }
            if (n == 't')  { out.push_back('\t'); ++i; continue; }
            if (n == '\\') { out.push_back('\\'); ++i; continue; }
        }
        out.push_back(in[i]);
    }
    return out;
}

void cmdList(const char* /*args*/)
{
    std::vector<FsEntry> es;
    bool truncated = false;
    if (!fs::list(s_cwd, es, truncated)) { Console::printf("ERR: cannot list\r\n"); return; }
    for (const auto& e : es) {
        if (e.is_dir) Console::printf("  [DIR]  %s\r\n", e.name.c_str());
        else          Console::printf("  %6lu  %s\r\n",
                                      static_cast<unsigned long>(e.size), e.name.c_str());
    }
    if (truncated) Console::printf("  ...(truncated)\r\n");
}

void cmdPwd(const char* /*args*/)
{
    Console::printf("/%s\r\n", s_cwd.c_str());
}

void cmdCd(const char* args)
{
    std::string t = trimmed(args);
    if (t.empty() || t == "/") {
        s_cwd.clear();
    } else if (t == "..") {
        size_t p = s_cwd.find_last_of('/');
        if (p == std::string::npos) s_cwd.clear();
        else s_cwd.erase(p);
    } else {
        std::string cand = relOf(t);
        if (!fs::isDir(cand)) { Console::printf("ERR: no such directory\r\n"); return; }
        s_cwd = cand;
    }
    Console::printf("/%s\r\n", s_cwd.c_str());
}

void cmdGet(const char* args)
{
    std::string f = trimmed(args);
    if (f.empty()) { Console::printf("ERR: usage GET <file>\r\n"); return; }
    std::string content;
    if (!fs::readText(relOf(f), content, 8192)) { Console::printf("ERR: not found\r\n"); return; }
    Console::print(content.c_str());
    Console::printf("\r\n");
}

void cmdPut(const char* args)
{
    std::string a = trimmed(args);
    size_t sp = a.find(' ');
    std::string f    = (sp == std::string::npos) ? a : a.substr(0, sp);
    std::string text = (sp == std::string::npos) ? std::string() : unescape(a.substr(sp + 1));
    if (f.empty()) { Console::printf("ERR: usage PUT <file> <text>\r\n"); return; }
    bool existed = fs::exists(relOf(f));
    bool ok = fs::writeText(relOf(f), text.data(), text.size());
    if (!ok)          Console::printf("ERR: write failed\r\n");
    else if (existed) Console::printf("OK: overwriting\r\n");
    else              Console::printf("OK\r\n");
}

void cmdDelete(const char* args)
{
    std::string f = trimmed(args);
    if (f.empty()) { Console::printf("ERR: usage DELETE <file>\r\n"); return; }
    Console::printf(fs::removeFile(relOf(f)) ? "OK\r\n" : "ERR: not found\r\n");
}

void cmdMkdir(const char* args)
{
    std::string d = trimmed(args);
    if (d.empty()) { Console::printf("ERR: usage MKDIR <name>\r\n"); return; }
    if (fs::exists(relOf(d))) { Console::printf("ERR: already exists\r\n"); return; }
    Console::printf(fs::makeDir(relOf(d)) ? "OK\r\n" : "ERR: failed\r\n");
}

void cmdRmdir(const char* args)
{
    std::string d = trimmed(args);
    if (d.empty()) { Console::printf("ERR: usage RMDIR <name>\r\n"); return; }
    if (!fs::isDir(relOf(d))) { Console::printf("ERR: no such directory\r\n"); return; }
    std::vector<FsEntry> es;
    bool tr = false;
    fs::list(relOf(d), es, tr);
    if (!es.empty()) { Console::printf("ERR: not empty\r\n"); return; }
    Console::printf(fs::removeDir(relOf(d)) ? "OK\r\n" : "ERR: failed\r\n");
}

void cmdFree(const char* /*args*/)
{
    uint32_t totalKB = 0, freeKB = 0;
    if (!fs::stats(totalKB, freeKB)) { Console::printf("ERR\r\n"); return; }
    Console::printf("Total: %lu KB, Used: %lu KB, Free: %lu KB\r\n",
                    static_cast<unsigned long>(totalKB),
                    static_cast<unsigned long>(totalKB - freeKB),
                    static_cast<unsigned long>(freeKB));
}

void cmdReceive(const char* args)
{
    // RECEIVE <file> <size> <crc32_hex>: stream a binary file into the current
    // directory. Shares the plugin upload receiver: it replies READY, then
    // expects `size` raw bytes verified against `crc`.
    std::string a = trimmed(args);
    char name[128] = {0};
    unsigned long size = 0;
    unsigned long crc = 0;
    if (std::sscanf(a.c_str(), "%127s %lu %lx", name, &size, &crc) != 3 || size == 0) {
        Console::printf("ERR: usage RECEIVE <file> <size> <crc32_hex>\r\n");
        return;
    }
    std::string abs;
    if (!fs::resolve(relOf(name), abs)) { Console::printf("ERR: bad path\r\n"); return; }
    cdc::plugin_manager::beginFileReceive(abs.c_str(), size, static_cast<uint32_t>(crc));
}

const cdc::serial::SubCommand kSubs[] = {
    {"LIST",    "",                   "List current directory",          cmdList},
    {"PWD",     "",                   "Print working directory",         cmdPwd},
    {"CD",      "<path>",             "Change directory (.. = up)",      cmdCd},
    {"GET",     "<file>",             "Print file contents",             cmdGet},
    {"PUT",     "<file> <text>",      "Write text (\\n -> newline)",     cmdPut},
    {"RECEIVE", "<file> <size> <crc>","Receive a binary file (stream)",  cmdReceive},
    {"DELETE",  "<file>",             "Delete a file",                   cmdDelete},
    {"MKDIR",   "<name>",             "Create a directory",              cmdMkdir},
    {"RMDIR",   "<name>",             "Remove an empty directory",       cmdRmdir},
    {"FREE",    "",                   "Show partition usage",            cmdFree},
    {nullptr, nullptr, nullptr, nullptr},
};

void cmdVfat(const char* args)
{
    cdc::serial::dispatchSubCommand("VFAT", args, kSubs);
}

void onLockEvent(const cdc::core::Event& /*evt*/)
{
    s_cwd.clear();
}

cdc::ui::IView* explorerView()
{
    VfatExplorerView::instance().openRoot();
    return &VfatExplorerView::instance();
}

}  // namespace

VfatModule& VfatModule::instance()
{
    static VfatModule inst;
    return inst;
}

bool VfatModule::init()
{
    cdc::core::ModuleRegistry::instance().registerModule(this);

    cdc::serial::getCommandRegistry().registerCommand(
        {"VFAT",
         "Plugin FAT shell: LIST/CD/PWD/GET/PUT/RECEIVE/DELETE/MKDIR/RMDIR/FREE",
         cmdVfat, "vfat", true, kSubs});

    cdc::core::EventBus::instance().subscribe(
        onLockEvent,
        cdc::core::EventBus::eventMask(cdc::core::EventType::SYSTEM_LOCK));

    state_ = cdc::core::ServiceState::INITIALIZED;
    return true;
}

bool VfatModule::start()
{
    state_ = cdc::core::ServiceState::STARTED;
    return true;
}

void VfatModule::stop()
{
    state_ = cdc::core::ServiceState::STOPPED;
}

uint8_t VfatModule::getMenuItems(cdc::core::ModuleMenuItem* items, uint8_t maxItems)
{
    if (!items || maxItems == 0) return 0;
    items[0] = {
        cdc::ui::tr("core.vfat"),
        90,
        &explorerView,
        nullptr,
        getName(),
        cdc::core::MenuLocation::EXPERT_MENU,
        nullptr,
    };
    return 1;
}

}  // namespace cdc::mod_vfat

extern "C" void mod_vfat_register(void)
{
    cdc::core::ModuleRegistry::instance().registerInitializer([]() {
        auto& m = cdc::mod_vfat::VfatModule::instance();
        if (m.init()) m.start();
    });
}
