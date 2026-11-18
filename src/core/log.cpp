#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace nova::log {
namespace {
std::mutex g_mu;
std::FILE* g_file = nullptr;
std::atomic<int> g_debug{-1};

const char* Name(Level l) {
  switch (l) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO ";
    case Level::Warn: return "WARN ";
    default: return "ERROR";
  }
}
}  // namespace

void SetLogFile(const std::string& path) {
  std::lock_guard lk(g_mu);
  if (g_file) {
    std::fclose(g_file);
    g_file = nullptr;
  }
  if (!path.empty()) g_file = std::fopen(path.c_str(), "w");
}

void SetDebug(bool on) { g_debug.store(on ? 1 : 0); }

void Write(Level lvl, std::string_view msg) {
  if (lvl == Level::Debug) {
    int d = g_debug.load();
    if (d < 0) {
      const char* e = std::getenv("NOVA_LOG_DEBUG");
      d = (e && *e && *e != '0') ? 1 : 0;
      g_debug.store(d);
    }
    if (!d) return;
  }
  std::lock_guard lk(g_mu);
  std::fprintf(stderr, "[%s] %.*s\n", Name(lvl), static_cast<int>(msg.size()), msg.data());
  std::fflush(stderr);
  if (g_file) {
    std::fprintf(g_file, "[%s] %.*s\n", Name(lvl), static_cast<int>(msg.size()), msg.data());
    std::fflush(g_file);
  }
}
}  // namespace nova::log
