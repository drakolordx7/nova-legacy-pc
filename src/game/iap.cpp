// Native half of the offline shop (docs/iap.md). Three patches on libNOVA.so:
//  - IsConnectionAvailable(): the IAP screens refuse to list or sell packs without a network. It answers true to the
//    IAP code only (caller found by LR), so every other online feature still sees "offline".
//  - IAPPHDManager::RefreshFeed(bool): it needs a Hestia CRM feed from the server; replaced by what its success branch
//    does (flag the feed for loading), which the Java IAPLib answers from the built-in catalogue.
//  - IAPPHDManager::Init() (post): flags the first feed load, so the packs exist before the shop is opened.
// Test aid (opt-in): with NOVA_IAP_TEST_BUY=<slot> set, IAPPHDManager::Update (post) presses the buy button of that shop
// slot 3 s after the shop screen is up, through IAPPHDManager::BuyIAPItem, which is all the Flash button handler does.
// Used by the scripted verification run, which cannot know the Flash layout of the shop.
#include "game/iap.h"

#include <chrono>
#include <cstdlib>
#include <string>

#include "core/log.h"
#include "cpu/cpu.h"
#include "cpu/hle.h"
#include "pc/hook.h"

namespace nova::game {
namespace {

const loader::Module* g_module = nullptr;
gaddr g_init_tramp = 0, g_update_tramp = 0;
gaddr g_find_item = 0, g_buy_item = 0, g_menu_name = 0, g_menu_ptr = 0;
int g_test_buy = -1;  // NOVA_IAP_TEST_BUY

// IAPPHDManager field offsets (decompiled): byte flags.
constexpr gaddr kNeedFeed = 3, kFeedProcessed = 4, kShowWaiting = 5, kHasPromo = 6, kCoinPromo = 11, kCashPromo = 12;

bool IsIapCaller(gaddr lr) {
  std::string where;
  if (!g_module || !g_module->Symbolize(lr, where)) return false;
  return where.find("IAPPHD") != std::string::npos || where.find("IAPButtonPressed") != std::string::npos ||
         where.find("AskForIAPData") != std::string::npos;
}

// bool IsConnectionAvailable()
void H_IsConnectionAvailable(Cpu& cpu) {
  const bool iap = IsIapCaller(cpu.x(30));
  static bool logged = false;
  if (iap && !logged) {
    logged = true;
    log::Info("[iap] IsConnectionAvailable answered true to the IAP code (first call)");
  }
  cpu.set_x(0, iap ? 1 : 0);
}

// void IAPPHDManager::RefreshFeed(bool showPopup)
void H_RefreshFeed(Cpu& cpu) {
  const gaddr self = cpu.x(0);
  mem::W<std::uint8_t>(self + kFeedProcessed, 0);
  mem::W<std::uint8_t>(self + kHasPromo, 0);
  mem::W<std::uint8_t>(self + kCoinPromo, 0);
  mem::W<std::uint8_t>(self + kCashPromo, 0);
  mem::W<std::uint8_t>(self + kShowWaiting, static_cast<std::uint8_t>(cpu.x(1) & 1));
  mem::W<std::uint8_t>(self + kNeedFeed, 1);
  cpu.set_x(0, 0);
}

// void IAPPHDManager::Init()
void H_Init(Cpu& cpu) {
  const gaddr self = cpu.x(0);  // x0 is garbage once the original has run
  pc::hook::CallOriginal(cpu, g_init_tramp);
  mem::W<std::uint8_t>(self + kNeedFeed, 1);
  log::Info("[iap] IAPPHDManager::Init done: first feed load requested");
}

// std::string at `p` (libc++ NDK layout) as a C string.
std::string GuestStdString(gaddr p) {
  const std::uint8_t flag = mem::R<std::uint8_t>(p);
  return mem::CString((flag & 1) ? mem::R64(p + 16) : p + 1, 128);
}

// void IAPPHDManager::Update()
void H_Update(Cpu& cpu) {
  const gaddr self = cpu.x(0);
  pc::hook::CallOriginal(cpu, g_update_tramp);
  if (g_test_buy < 0 || !g_find_item || !g_buy_item || !g_menu_name || !g_menu_ptr) return;
  using Clock = std::chrono::steady_clock;
  static Clock::time_point shown{};
  static bool done = false;
  if (done) return;
  const bool ready = mem::R32(self + 112) == 1 && mem::R<std::uint8_t>(self + kFeedProcessed) != 0;
  const gaddr menu = mem::R64(g_menu_ptr);
  if (!ready || !menu || GuestStdString(call_guest<gaddr>(g_menu_name, menu)) != "menu_IAPMenu") {
    shown = {};
    return;
  }
  if (shown == Clock::time_point{}) {
    shown = Clock::now();
    log::Info("[iap] test: shop screen is up, buying slot {} in 3 s", g_test_buy);
    return;
  }
  if (Clock::now() - shown < std::chrono::seconds(3)) return;
  done = true;
  constexpr std::uint32_t kCashHash = 374439154;  // StringHash("Cash"), the shop's premium currency list
  const gaddr item = call_guest<gaddr>(g_find_item, self, g_test_buy, kCashHash);
  log::Info("[iap] test: buy slot {} -> item 0x{:x}", g_test_buy, item);
  if (item) call_guest<void>(g_buy_item, self, item);
}

}  // namespace

int ApplyIapPatches(const loader::Module& module) {
  g_module = &module;
  int applied = 0;
  if (const gaddr a = module.Lookup("_Z21IsConnectionAvailablev")) {
    if (pc::hook::Replace(a, "iap::IsConnectionAvailable", H_IsConnectionAvailable)) ++applied;
  } else log::Warn("[iap] _Z21IsConnectionAvailablev not found");
  if (const gaddr a = module.Lookup("_ZN13IAPPHDManager11RefreshFeedEb")) {
    if (pc::hook::Replace(a, "iap::RefreshFeed", H_RefreshFeed)) ++applied;
  } else log::Warn("[iap] IAPPHDManager::RefreshFeed not found");
  if (const gaddr a = module.Lookup("_ZN13IAPPHDManager4InitEv")) {
    g_init_tramp = pc::hook::Wrap(a, "iap::Init", H_Init);
    if (g_init_tramp) ++applied;
  } else log::Warn("[iap] IAPPHDManager::Init not found");
  if (const char* e = std::getenv("NOVA_IAP_TEST_BUY"); e && *e) {
    g_test_buy = std::atoi(e);
    g_find_item = module.Lookup("_ZNK13IAPPHDManager11FindIAPItemEi10StringHash");
    g_buy_item = module.Lookup("_ZN13IAPPHDManager10BuyIAPItemEPK10IAPPHDItem");
    g_menu_name = module.Lookup("_ZN11MenuManager17GetActiveMenuNameEv");
    g_menu_ptr = module.Lookup("_ZN11MenuManager10s_instanceE");
    if (const gaddr a = module.Lookup("_ZN13IAPPHDManager6UpdateEv")) g_update_tramp = pc::hook::Wrap(a, "iap::Update", H_Update);
    if (g_update_tramp) ++applied;
    log::Info("[iap] test buy of slot {} armed", g_test_buy);
  }
  log::Info("[iap] {} native patches installed", applied);
  return applied;
}

}  // namespace nova::game
