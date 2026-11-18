// Minimal logger. Adapted from Zombiewood's core/log (MIT, (c) 2026 ChrisDutary); messages are English.
#pragma once
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace nova::log {
enum class Level { Debug, Info, Warn, Error };

// Mirror every line to a file as well (in addition to stderr). Empty path closes it.
void SetLogFile(const std::string& path);
// Debug lines are dropped unless enabled (NOVA_LOG_DEBUG=1 also enables them).
void SetDebug(bool on);
void Write(Level lvl, std::string_view msg);

template <class... A>
void Debug(std::format_string<A...> f, A&&... a) { Write(Level::Debug, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void Info(std::format_string<A...> f, A&&... a) { Write(Level::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void Warn(std::format_string<A...> f, A&&... a) { Write(Level::Warn, std::format(f, std::forward<A>(a)...)); }
template <class... A>
void Error(std::format_string<A...> f, A&&... a) { Write(Level::Error, std::format(f, std::forward<A>(a)...)); }
}  // namespace nova::log
