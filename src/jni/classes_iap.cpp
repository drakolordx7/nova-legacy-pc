// com.gameloft.android2d.iap.IAPLib: Gameloft's in-app purchase library, answered offline.
//
// The game's native IAP layer (IAPPHDManager, IAP_* wrappers) talks to this Java class only through static calls.
// The original class pulled a product feed from Gameloft's servers and sent purchases to Google Play; both servers
// are gone, so the feed is a built-in table (kPacks) and a purchase completes instantly and free. The class keeps the
// same result state machine the native side polls through Update() (docs/iap.md), so the game's own code path runs:
//   GetFeed()      -> 9 (feed loading) -> 10 (feed ready)            -> native ProcessFeed builds the shop items
//   SendRequest()  -> 1 (request started) -> 7 (item purchased) -> 10 -> native OnTransactionSuccess grants the pack
// Everything the library reports about the last purchase (amount, pack index, type, transaction id) is answered from
// the pack that was bought.
#include <algorithm>
#include <mutex>
#include <string>

#include "core/log.h"
#include "jni/internal.h"

namespace nova::jni {
namespace {

// IAP result codes the native IAPPHDManager::UpdateIAPModule understands.
enum Result : int {
  kNone = 0,
  kRequestStarted = 1,
  kPurchased = 7,
  kFeedLoading = 9,
  kFeedReady = 10,
  kError = 3,
};

struct Pack {
  const char* type;     // feed section ("Cash" = premium currency packs the shop lists)
  const char* name;     // display name
  const char* entry;    // entry id
  const char* content;  // content id (hashed by the game)
  std::int64_t amount;  // premium currency granted
};

// The premium currency packs of the shop, in shop order. The original feed is unrecoverable (server only); the
// amounts are a reconstruction (see docs/iap.md).
constexpr Pack kPacks[] = {
    {"cash", "Handful Pack", "nova_offline_pack_1", "NOVA_OFFLINE_PACK_1", 100},
    {"cash", "Small Pack", "nova_offline_pack_2", "NOVA_OFFLINE_PACK_2", 260},
    {"cash", "Medium Pack", "nova_offline_pack_3", "NOVA_OFFLINE_PACK_3", 550},
    {"cash", "Large Pack", "nova_offline_pack_4", "NOVA_OFFLINE_PACK_4", 1200},
    {"cash", "Huge Pack", "nova_offline_pack_5", "NOVA_OFFLINE_PACK_5", 2600},
    {"cash", "Mega Pack", "nova_offline_pack_6", "NOVA_OFFLINE_PACK_6", 6000},
};

Impl Noop() {
  return [](Call&) { return Value(); };
}
Impl RetInt(std::int64_t v) {
  return [v](Call&) { return Value::Int(v); };
}
Impl RetFloat(double v) {
  return [v](Call&) { return Value::Float(v); };
}

std::string Lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

struct IapState {
  std::mutex mu;
  int result = kNone;
  int error = 0;
  bool feed_ready = false;  // GetFeed completed once
  std::string language = "en";
  // Last purchase.
  int last_index = -1;
  std::string last_type = "Cash";
  const Pack* last = nullptr;
  int transactions = 0;
};
IapState g;

// The index-th pack of a feed section, or null.
const Pack* FindPack(int index, const std::string& type) {
  const std::string t = Lower(type);
  int n = 0;
  for (const Pack& p : kPacks) {
    if (t != p.type) continue;
    if (n++ == index) return &p;
  }
  return nullptr;
}
int CountPacks(const std::string& type) {
  const std::string t = Lower(type);
  int n = 0;
  for (const Pack& p : kPacks) n += t == p.type;
  return n;
}

// Tracking uid: the game reads character 42 as the shop slot digit (IAPPHDItem::ProcessFromFeed: digit d gives
// order d-1 for d < 5, d-2 otherwise), so the list stays in feed order with slot i at position i.
std::string TrackingUid(int index) {
  static const char kDigit[] = {'1', '2', '3', '4', '6', '7', '8', '9'};
  std::string uid = "offline-iap-";
  uid.resize(42, '0');
  uid += kDigit[std::clamp(index, 0, 7)];
  return uid;
}

}  // namespace

void DefineIapLib() {
  auto Str = [](const char* s) -> Impl { return [s](Call&) { return Value::Str(s); }; };

  DefineClass("com/gameloft/android2d/iap/IAPLib")
      .NoopRest()
      .Static("Init", "()V", [](Call&) {
        log::Info("[iap] IAPLib.Init: offline catalogue of {} packs, purchases are free", CountPacks("cash"));
        return Value();
      })
      .Static("CheckAvailable", "()Z", [](Call&) { return Value::Bool(true); })
      .Static("setLanguage", "(Ljava/lang/String;)V", [](Call& c) {
        std::lock_guard lk(g.mu);
        g.language = c.S(0);
        return Value();
      })
      .Static("getLanguage", "()Ljava/lang/String;", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Str(g.language);
      })
      // ---- feed ----
      .Static("GetFeed", "()V", [](Call&) {
        std::lock_guard lk(g.mu);
        g.result = kFeedLoading;
        g.error = 0;
        log::Info("[iap] GetFeed: offline catalogue");
        return Value();
      })
      .Static("AllowLoadFeedAgain", "()V", Noop())
      .Static("reset", "()V", [](Call&) {
        std::lock_guard lk(g.mu);
        g.result = g.feed_ready ? kFeedReady : kNone;
        return Value();
      })
      .Static("setResult", "(I)V", [](Call& c) {
        std::lock_guard lk(g.mu);
        g.result = c.I(0);
        return Value();
      })
      // The native side polls this once per frame; the value is delivered, then the state moves on.
      .Static("Update", "()I", [](Call&) {
        std::lock_guard lk(g.mu);
        const int r = g.result;
        switch (r) {
          case kFeedLoading:
            g.feed_ready = true;
            g.result = kFeedReady;
            break;
          case kRequestStarted:
            g.result = kPurchased;
            break;
          case kPurchased:
            g.result = kFeedReady;  // the original returns to 0 and reloads the feed; stay ready so the next buy works
            break;
          case kError:
            g.result = g.feed_ready ? kFeedReady : kNone;
            break;
          default:
            break;
        }
        return Value::Int(r);
      })
      .Static("getError", "()I", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Int(g.error);
      })
      // ---- catalogue ----
      .Static("GetItemNumber", "()I", [](Call&) { return Value::Int(CountPacks("cash")); })
      .Static("GetItemNumber", "(Ljava/lang/String;)I", [](Call& c) { return Value::Int(CountPacks(c.S(0))); })
      .Static("GetHiddenItemNumber", "(Ljava/lang/String;)I", [](Call&) { return Value::Int(0); })
      .Static("getEntryIdItem", "(ILjava/lang/String;)Ljava/lang/String;",
              [](Call& c) {
                const Pack* p = FindPack(c.I(0), c.S(1));
                return Value::Str(p ? p->entry : "");
              })
      .Static("checkEntryIdItem", "(Ljava/lang/String;)Z", [](Call& c) {
        const std::string e = c.S(0);
        for (const Pack& p : kPacks)
          if (e == p.entry) return Value::Bool(true);
        return Value::Bool(false);
      })
      .Static("GetItemContentID", "(ILjava/lang/String;)Ljava/lang/String;",
              [](Call& c) {
                const Pack* p = FindPack(c.I(0), c.S(1));
                return Value::Str(p ? p->content : "");
              })
      .Static("GetOldContentId", "(ILjava/lang/String;)Ljava/lang/String;",
              [](Call& c) {
                const Pack* p = FindPack(c.I(0), c.S(1));
                return Value::Str(p ? p->content : "");
              })
      .Static("GetItemName", "(I)Ljava/lang/String;",
              [](Call& c) {
                const Pack* p = FindPack(c.I(0), "cash");
                return Value::Str(p ? p->name : "");
              })
      .Static("GetItemName", "(ILjava/lang/String;)Ljava/lang/String;",
              [](Call& c) {
                const Pack* p = FindPack(c.I(0), c.S(1));
                return Value::Str(p ? p->name : "");
              })
      .Static("GetItemTrackingUID", "(ILjava/lang/String;)Ljava/lang/String;",
              [](Call& c) { return Value::Str(FindPack(c.I(0), c.S(1)) ? TrackingUid(c.I(0)) : std::string()); })
      .Static("getItemJson", "(Ljava/lang/String;)Ljava/lang/String;", Str("{}"))
      .Static("GetPrice", "(I)Ljava/lang/String;", Str("Free"))
      .Static("GetPrice", "(ILjava/lang/String;)Ljava/lang/String;", Str("Free"))
      .Static("GetPriceValue", "(ILjava/lang/String;)F", RetFloat(0.0))
      .Static("GetPromoOldPriceValue", "(ILjava/lang/String;)F", RetFloat(0.0))
      .Static("GetPromoOldPrice", "(I)Ljava/lang/String;", Str(""))
      .Static("GetPromoOldPrice", "(ILjava/lang/String;)Ljava/lang/String;", Str(""))
      .Static("getItemCurrencyISOCode", "(ILjava/lang/String;)Ljava/lang/String;", Str("USD"))
      .Static("getVirtualCurrency", "(I)J",
              [](Call& c) {
                const Pack* p = FindPack(c.I(0), "cash");
                return Value::Int(p ? p->amount : 0);
              })
      .Static("getVirtualCurrency", "(ILjava/lang/String;)J",
              [](Call& c) {
                const Pack* p = FindPack(c.I(0), c.S(1));
                return Value::Int(p ? p->amount : 0);
              })
      .Static("getOldVirtualCurrency", "(I)J", RetInt(0))
      .Static("getOldVirtualCurrency", "(ILjava/lang/String;)J", RetInt(0))
      .Static("GetCountryISO", "()Ljava/lang/String;", Str("US"))
      .Static("getLastCurrencyISOCode", "()Ljava/lang/String;", Str("USD"))
      .Static("GetConfigStoreName", "()Ljava/lang/String;", Str("offline"))
      .Static("UpdatePromoCountDownTime", "()J", RetInt(0))
      .Static("GetPromoDescription", "()Ljava/lang/String;", Str(""))
      // ---- purchase ----
      .Static("SendRequest", "(ILjava/lang/String;J)V",
              [](Call& c) {
                std::lock_guard lk(g.mu);
                const int index = c.I(0);
                const std::string type = c.S(1);
                const Pack* p = FindPack(index, type);
                if (!p) {
                  log::Warn("[iap] SendRequest({}, {}): no such pack", index, type);
                  g.error = -1;
                  g.result = kError;
                  return Value();
                }
                g.last = p;
                g.last_index = index;
                g.last_type = type;
                g.result = kRequestStarted;
                g.error = 0;
                ++g.transactions;
                log::Info("[iap] purchase {} ({} x{}): free, instant offline transaction #{}", p->name, type, p->amount, g.transactions);
                return Value();
              })
      .Static("SendRequest", "(I)V",
              [](Call& c) {
                std::lock_guard lk(g.mu);
                const Pack* p = FindPack(c.I(0), "cash");
                if (!p) return Value();
                g.last = p;
                g.last_index = c.I(0);
                g.last_type = "Cash";
                g.result = kRequestStarted;
                ++g.transactions;
                return Value();
              })
      .Static("GetLastVirtualCurrencyPurchased", "()J", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Int(g.last ? g.last->amount : 0);
      })
      .Static("GetPackageIDPurchased", "()I", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Int(g.last_index);
      })
      .Static("GetPackageTypePurchased", "()Ljava/lang/String;", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Str(g.last_type);
      })
      .Static("GetLastItemTrackingUIDPurchased", "()Ljava/lang/String;", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Str(g.last ? TrackingUid(g.last_index) : std::string());
      })
      .Static("GetLastContentIdPurchased", "()Ljava/lang/String;", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Str(g.last ? g.last->content : "");
      })
      .Static("GetTransactionID", "()Ljava/lang/String;", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Str("NOVA-OFFLINE-" + std::to_string(g.transactions));
      })
      .Static("GetPurchaseReceipts", "()Ljava/lang/String;", [](Call&) {
        std::lock_guard lk(g.mu);
        return Value::Str("offline-receipt-" + std::to_string(g.transactions));
      })
      .Static("GetLastItemJsonPurchased", "()Ljava/lang/String;", Str("{}"))
      .Static("GetLastBillingType", "()Ljava/lang/String;", Str("offline"))
      .Static("GetLastFormattedPricePurchased", "()Ljava/lang/String;", Str("Free"))
      .Static("GetLastPriceValuePurchased", "()F", RetFloat(0.0));
}

}  // namespace nova::jni
