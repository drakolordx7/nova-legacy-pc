# In-app purchases: free and instant, offline

Every real-money pack in the shop is free and completes at once, offline, with no store UI and no validation. Status: implemented; see "Verification" for what has and has not been run.

## How the original works

Three layers (arm64 `libNOVA.so` 5.8.4a, addresses image-relative):

1. Shop UI (Flash): `onAskForIAPData` (opens the screen), `OnIAPButtonPressed` (buy button; Flash `data.index` and `data.type` = `"hard"` or `"coins"`). For `"hard"` index 0 is the "watch a video for free cash" slot; index n>0 is feed item n-1, found with `IAPPHDManager::FindIAPItem(i, hash)` (`StringHash("Cash")` = 374439154 selects the item vector at `IAPPHDManager+136`, `"Coin"` the one at +160) and bought with `IAPPHDManager::BuyIAPItem(item)`. `"coins"` is the local hard-to-soft conversion (`BuyCoinsItem`), not real money.
2. `IAPPHDManager` (native state machine, `Update` every frame while a menu is up) over the `IAP_*` wrappers. Every `IAP_*` / `IAPLib::*` function is a thin JNI call into the static Java class `com.gameloft.android2d.iap.IAPLib`. It polls `IAP_Update()` for a result code and maps it to its own state: 0 none, 1 request started (waiting popup), 2 sent, 3 error, 7 item purchased (state 8: `OnTransactionSuccess`), 9 feed loading, 10 feed ready (state 1, idle; `ProcessFeed` builds the item vectors from the Java getters).
3. Java `IAPLib` (Gameloft IAP library): pulled the product feed from Gameloft's IAP/Hestia (CRM) servers, sent purchases to Google Play, verified receipts on the server. All dead.

Product list: `ProcessFeed` asks Java for `GetItemNumber("Cash")` and per item `getEntryIdItem`, `GetItemContentID`, `GetItemName`, `GetPrice` (display string), `GetPriceValue`, `getVirtualCurrency` (amount), `GetItemTrackingUID` ... Types in the feed: `Cash` (premium currency packs, the shop list), `Coin`, `bundle_starterpack`, `bundle_chaptertwo`, `bundle_shadow`, `bundle_lpp` and `Bundle` (CRM promo bundles, definitions come from the CRM JSON through `BundleManager`). No catalogue is shipped in the data packs (`hestia_local_data.bin` is only a cache of a server answer).

Grant: `IAPPHDManager::OnTransactionSuccess` (0x1248a3c). It reads the last purchase back from Java (`GetLastVirtualCurrencyPurchased`, `GetPackageTypePurchased`, `GetPackageIDPurchased`, `GetTransactionID`, `GetPurchaseReceipts` ...) and for type `Cash` does `PlayerProfile+6832 += amount` (premium currency, as a `SecuredValue`), logs an anti-hack `ShopOperation` entry, saves the profile and shows the success popup. There is no receipt check in the native code; the only validation lived in the Java library.

Offline gates in native code: `IsConnectionAvailable()` is false (JNI reports no connectivity), so `onAskForIAPData` shows "store unavailable" (error -6) and never attaches the items; `RefreshFeed` needs a non-empty CRM feed from the server, and nothing else ever sets the "load feed" flag `IAPPHDManager+3`.

## What the port does

- `src/jni/classes_iap.cpp`: the Java `IAPLib` is emulated with the same result state machine. `GetFeed` -> 9 -> 10 (feed ready); `SendRequest(index, type, 0)` -> 1 -> 7 (purchased) -> 10. Catalogue (`kPacks`): six `Cash` packs, price string `"Free"`, price value 0, amounts 100 / 260 / 550 / 1200 / 2600 / 6000 premium currency. The original amounts are unrecoverable (server feed), so these are a reconstruction. The tracking uid of each pack carries the slot digit the game reads at character 42, so the list keeps feed order. Last-purchase getters answer from the pack bought; transaction id `NOVA-OFFLINE-n`, receipt `offline-receipt-n` (never checked).
- `src/game/iap.cpp` (three patches by symbol):
  - `IsConnectionAvailable` returns true when its caller (LR symbolised) is IAP code (`IAPPHD*`, `OnIAPButtonPressed`, `onAskForIAPData`); everything else still sees offline.
  - `IAPPHDManager::RefreshFeed(bool)` replaced by its success branch (clear promo flags, `+5 = showPopup`, `+3 = 1`).
  - `IAPPHDManager::Init()` post hook sets `+3 = 1`, so the first feed load happens at the main menu, before the shop is opened.
  - Test aid, opt-in: `NOVA_IAP_TEST_BUY=<slot>` presses buy for that shop slot 3 s after `menu_IAPMenu` is up (`FindIAPItem` + `BuyIAPItem`, the same call the Flash handler makes).

## Not covered (not free yet)

- Promo bundles (`bundle_*`, CRM `Bundle`): their contents and the bundle popups come from the CRM JSON via `BundleManager`; with no server there are no bundle definitions, so none are listed. They would need a synthesised bundle JSON (`BundleManager::ReloadAllBundles`) plus catalogue items of the matching type.
- Subscriptions / VIP (`GetSubscriptionStatus`, `IsLastPurchaseSubscription`): no item in the feed, nothing to buy; the Java answers "none".
- The "Sale" tile and server-driven offers (`CShopManager::ShopOffer` through Hestia) are online features and stay offline.
- `NativeIsPurchasesAllowed` (Flash helper) always returns false in this build; not changed, not known to gate anything.

## Verification

Run once (muted, windowed, `tools\run_dev.ps1`, profile backed up and restored): boot log shows `[iap] 4 native patches installed`, `IAPLib.Init`, the `Init` post hook, `GetFeed: offline catalogue` at the main menu and `IsConnectionAvailable answered true to the IAP code`. The scripted shop visit did not reach the shop: the dev profile is in the first-mission tutorial and its modal popup ("Tap the ICON to select it") swallowed the tap on the top-bar "+". The purchase itself (state 1 -> 7, `OnTransactionSuccess`, currency +amount) is therefore decompilation-verified only, not yet seen running.
