# JNI interface of libNOVA.so (and libgenerator.so)

Sources: the 4,350-class decompile of `N.O.V.A._Legacy-5.8.4a.apk` (jadx 1.5.6), the `.dynsym` of both libraries, and a combination of a linear scan of libNOVA `.text` (JNI table-slot calls with string arguments) and an emulation of the library's own JNI-using functions in Unicorn (stubbed JNIEnv that records `FindClass`/`GetMethodID`/`GetStaticMethodID`/`GetFieldID`/`GetStaticFieldID` arguments). The emulation recovers names that are built at run time (`PackageUtils::GetClass` takes a std::string, `PopUpsBridgeClass` signatures come from `MakeSig`, the `adslib` names are XOR-obfuscated in .rodata).

App identity: package `com.gameloft.android.ANMP.GloftNOHM`, versionCode 58423 / versionName 5.8.4a, minSdk 15, targetSdk 30, application class `FrameworkApplication`, launcher activity `com.gameloft.android.ANMP.GloftNOHM.MainActivity` (`sensorLandscape`, `singleTop`, `resizeableActivity=false`), OpenGL ES 2.0 required (`glEsVersion 0x20000`). Obfuscated Java names that appear below: `PackageUtils.a.a` = InputDispatcher, `PackageUtils.a.b` = SignalDispatcher, `PluginSystem.b` = PluginEventDispatcher (the decompiler kept the original file name in a comment).

## (a) Native methods declared in Java

116 native methods are declared in the decompiled Java (ignoring androidx and android.support): 82 are implemented by libNOVA.so (exported `Java_*` symbols; libNOVA also exports `JNI_OnLoad`), 2 by libgenerator.so (`com.gameloft.android.hdidfv.HDIDFV`), 3 by libDecRawso.so (`DecRawso.Decode/IsArmMode/SetFilter`, not needed on PC), and 29 have no implementation in any shipped library (dead code or never called: they would throw UnsatisfiedLinkError if called; listed with "no export"). `RegisterNatives` is never used (no JNI function-table slot 215 call), so the host only has to resolve names of the form `Java_<mangled class>_<mangled method>`.

libNOVA exports 86 `Java_*` functions: 82 match a Java declaration; 4 have no Java declaration in this APK: `NativeBridgeHIDControllers.NativeControllerConnected/NativeControllerDisconnected/NativeHandleInputEvents` (the Java class `com.gameloft.android.ANMP.GloftNOHM.GLUtils.controller.NativeBridgeHIDControllers` is absent from classes.dex, so no gamepad event ever reaches the native side in this build) and `FacebookAndroidGLSocialLib.nativeOnFBDialogSetCancel`.

### `com.android.billingclient.api.zzah` - Google Play Billing library internals (9 natives, no implementation)

All 9 declared methods (`nativeOnAcknowledgePurchaseResponse`, `nativeOnBillingServiceDisconnected`, `nativeOnBillingSetupFinished`, `nativeOnConsumePurchaseResponse`, `nativeOnPriceChangeConfirmationResult`, `nativeOnPurchaseHistoryResponse`, `nativeOnPurchasesUpdated`, `nativeOnQueryPurchasesResponse`, `nativeOnSkuDetailsResponse`) are called from the billing client itself and exist in no shipped library: dead code for this port.

### `com.gameloft.adsmanager.AdMob`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void AdMobNotifyEvent(int i, int i2, int i3, String str)` | `(IIILjava/lang/String;)V` | no export | `adsmanager.AdMob.Configure`, `adsmanager.AdMob.NotifyEvent` | - |
| `void ReportInternalError(int i, int i2)` | `(II)V` | no export | `adsmanager.a.onAdFailedToLoad`, `adsmanager.d.onAdFailedToLoad` | - |

### `com.gameloft.adsmanager.FAN`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void FanNotifyEvent(int i, int i2, int i3, String str)` | `(IIILjava/lang/String;)V` | no export | `adsmanager.FAN.if`, `adsmanager.FAN.NotifyEvent` | - |
| `void ReportInternalError(int i, int i2)` | `(II)V` | no export | `adsmanager.b.if`, `adsmanager.e.if` | - |

### `com.gameloft.adsmanager.IronSourceAds`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void IronSourceNotifyEvent(int i, String str, int i2, int i3, int i4, String str2, String str3)` | `(ILjava/lang/String;IIILjava/lang/String;Ljava/lang/String;)V` | `Java_..._IronSourceNotifyEvent` | `adsmanager.IronSourceAds.a_`, `adsmanager.IronSourceAds.NotifyEvent` | calls `adslib::IronSourceManager::GetInstance`, `operator new`, `operator delete` |
| `void ReportInternalError(int i, int i2)` | `(II)V` | `Java_..._ReportInternalError` | `adsmanager.IncentivizedIronSource.run`, `adsmanager.IncentivizedIronSource.if` | calls `adslib::AdTypeInRange`, `adslib::IronSourceManager::GetInstance`, `operator new` |

### `com.gameloft.adsmanager.JavaUtils`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void AdsManagerLog(String str, String str2, String str3)` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | `Java_..._AdsManagerLog` | `adsmanager.a.onAdFailedToLoad`, `adsmanager.b.if` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |
| `void AdsManagerLogError(String str, String str2, String str3)` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | `Java_..._AdsManagerLogError` | `adsmanager.a.onAdFailedToLoad`, `adsmanager.b.if` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |
| `void AdsManagerLogInfo(String str, String str2, String str3)` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | `Java_..._AdsManagerLogInfo` | `adsmanager.a.onAdLoaded`, `adsmanager.a.onAdOpened` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |
| `void AdsManagerLogWarning(String str, String str2, String str3)` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | `Java_..._AdsManagerLogWarning` | `adsmanager.IncentivizedIronSource.if`, `adsmanager.OfferwallIronSource.if` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |

### `com.gameloft.adsmanager.UnityAdsManager`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void ReportInternalError(int i, int i2)` | `(II)V` | `Java_..._ReportInternalError` | `adsmanager.UnityAdsManager.if` | calls `adslib::AdTypeInRange`, `adslib::UnityAdsManager::GetInstance`, `operator new` |
| `void UnityAdsNotifyEvent(int i, int i2, int i3, int i4, String str, String str2)` | `(IIIILjava/lang/String;Ljava/lang/String;)V` | `Java_..._UnityAdsNotifyEvent` | `adsmanager.UnityAdsManager.if`, `adsmanager.UnityAdsManager.NotifyEvent` | calls `adslib::UnityAdsManager::GetInstance`, `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str` |

### `com.gameloft.android.ANMP.GloftNOHM.GLUtils.NetworkStateReceiver`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void setConnectionType(int i)` | `(I)V` | no export | `GLUtils.NetworkStateReceiver.if` | - |

### `com.gameloft.android.ANMP.GloftNOHM.GLUtils.SUtils`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void onVideoFinished()` | `()V` | no export | (no Java caller found) | - |

### `com.gameloft.android.ANMP.GloftNOHM.PackageUtils.EmulatorDetector`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `boolean NativeReadFileContent(String str, String str2)` | `(Ljava/lang/String;Ljava/lang/String;)Z` | `Java_..._NativeReadFileContent` | `PackageUtils.EmulatorDetector.if` | calls `fopen` |

### `com.gameloft.android.ANMP.GloftNOHM.PackageUtils.JNIBridge`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void NativeAudioFocusChanged(boolean z)` | `(Z)V` | `Java_..._NativeAudioFocusChanged` | `PackageUtils.a.if`, `PackageUtils.a.switch` | calls `NativeOnAudioFocusChanged` |
| `void NativeInit()` | `()V` | `Java_..._NativeInit` | `MainActivity.o` | calls `Internal::Init`, `OnGameInit` |
| `void NativeKeyAction(int i, boolean z)` | `(IZ)V` | `Java_..._NativeKeyAction` | `InputDispatcher.if` | calls `NativeOnKeyAction` |
| `void NativeOnIGPClosed()` | `()V` | no export | (no Java caller found) | - |
| `void NativeOnPause()` | `()V` | `Java_..._NativeOnPause` | `PackageUtils.AndroidUtils.onPause` | calls `Internal::PreGamePause`, `OnGamePause`, `Internal::PostGamePause` |
| `void NativeOnResume()` | `()V` | `Java_..._NativeOnResume` | `PackageUtils.AndroidUtils.onResume` | calls `Internal::PreGameResume`, `OnGameResume`, `Internal::PostGameResume` |
| `void NativeOnTouch(int i, float f, float f2, int i2)` | `(IFFI)V` | `Java_..._NativeOnTouch` | `InputDispatcher.a`, `InputDispatcher.while` | calls `OnGameTouchEvent` |
| `void NativeSendKeyboardData(String str)` | `(Ljava/lang/String;)V` | `Java_..._NativeSendKeyboardData` | `GLUtils.VirtualKeyboard.for` | calls `operator new`, `operator delete`, `acp_utils::GetVM` |
| `void NativeSurfaceChanged(Surface surface, int i, int i2)` | `(Landroid/view/Surface;II)V` | `Java_..._NativeSurfaceChanged` | `MainActivity.if` | calls `ANativeWindow_fromSurface`, `Internal::SetWindow`, `PackageUtils::GetNativeWindow` |
| `void NativeUserMusicIsPlaying(boolean z)` | `(Z)V` | `Java_..._NativeUserMusicIsPlaying` | `PackageUtils.AudioListenerPlugin.onPreNativeResume` | calls `NativeUserMusicIsPlaying` |
| `void NativeVolumeAction(boolean z)` | `(Z)V` | `Java_..._NativeVolumeAction` | `InputDispatcher.if` | calls `NativeVolumeAction` |
| `void NotifyLowBattery()` | `()V` | `Java_..._NotifyLowBattery` | `PackageUtils.UtilsBatteryStateReceiver.onReceive` | calls `NativeLowBattery` |
| `void NotifyTrimMemory(int i)` | `(I)V` | `Java_..._NotifyTrimMemory` | (no Java caller found) | calls `NativeTrimMemory` |
| `void SetBatteryInfo(boolean z, boolean z2, boolean z3, int i)` | `(ZZZI)V` | `Java_..._SetBatteryInfo` | `PackageUtils.UtilsBatteryStateReceiver.SetInitialBatteryState`, `PackageUtils.UtilsBatteryStateReceiver.onReceive` | calls `Internal::SetBatteryInfo` |
| `void SetConnectionType(int i)` | `(I)V` | `Java_..._SetConnectionType` | `PackageUtils.UtilsNetworkStateReceiver.onReceive` | calls `Internal::SetConnection` |
| `void SetUserLocation(int i, double d, double d2, float f, String str)` | `(IDDFLjava/lang/String;)V` | `Java_..._SetUserLocation` | `PackageUtils.LocationPlugin.EnableUserLocation`, `PackageUtils.LocationPlugin.DisableUserLocation` | calls `Internal::SetUserLocation`, `operator delete`, `acp_utils::GetVM` |
| `void SetWifiScannerResult(int i, String str)` | `(ILjava/lang/String;)V` | no export | `PackageUtils.WifiInfoPlugin.if`, `PackageUtils.WifiInfoPlugin.run` | - |

### `com.gameloft.android.ANMP.GloftNOHM.PushNotification.SimplifiedAndroidUtils`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void nativeInit()` | `()V` | no export | (no Java caller found) | - |
| `void nativeResetPNDataTracking()` | `()V` | `Java_..._nativeResetPNDataTracking` | `PushNotification.PushNotificationPlugin.if` | calls `acp_utils::modules::SimplifiedPN::ResetPNDataTracking` |
| `void nativeSendPNData(String str)` | `(Ljava/lang/String;)V` | `Java_..._nativeSendPNData` | `PushNotification.SimplifiedAndroidUtils.onReceive` | calls `OnPushNotificationResponseCB`, `operator delete`, `acp_utils::GetVM` |
| `void nativeSendPNDataToTracking(int i2, String str)` | `(ILjava/lang/String;)V` | `Java_..._nativeSendPNDataToTracking` | `PushNotification.PushNotificationPlugin.if` | calls `acp_utils::modules::SimplifiedPN::SendPNDataToTrackingCallba`, `operator delete`, `acp_utils::GetVM` |
| `void nativeSendRegistrationData(String str)` | `(Ljava/lang/String;)V` | `Java_..._nativeSendRegistrationData` | `PushNotification.SimplifiedAndroidUtils.GetDeviceToken` | calls `acp_utils::modules::SimplifiedPN::SendPnCallback`, `operator delete`, `acp_utils::GetVM` |

### `com.gameloft.android.hdidfv.HDIDFV`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `String getNHDIDFV()` | `()Ljava/lang/String;` | libgenerator `Java_com_gameloft_android_hdidfv_HDIDFV_getNHDIDFV` | `android.hdidfv.HDIDFV.getHDIDFV` | - |
| `String getNHDIDFVVersion()` | `()Ljava/lang/String;` | libgenerator `Java_com_gameloft_android_hdidfv_HDIDFV_getNHDIDFVVersion` | `android.hdidfv.HDIDFV.getHDIDFVVersion` | - |

### `com.gameloft.android2d.iap.IAP_CallBack`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void NIAP_GetLimitsValidationCB(boolean z)` | `(Z)V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1GetLimitsValidationCB` | `android2d.iap.IAP_CallBack.IAP_GetLimitsValidationCB` | calls `IAP_GetLimitsValidationCB` |
| `void NIAP_GetListInfoRestoreCB(String[] strArr)` | `([Ljava/lang/String;)V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1GetListInfoRestoreCB` | `android2d.iap.IAP_CallBack.IAP_GetListInfoRestoreCB` | trivial stub (4 bytes) |
| `void NIAP_GetListItemsRestoreCB(String[] strArr)` | `([Ljava/lang/String;)V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1GetListItemsRestoreCB` | `android2d.iap.IAP_CallBack.IAP_GetListItemsRestoreCB` | calls `operator new`, `operator delete` |
| `void NIAP_GetSubscriptionStatusCB(String str)` | `(Ljava/lang/String;)V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1GetSubscriptionStatusCB` | `android2d.iap.IAP_CallBack.IAP_GetSubscriptionStatusCB` | calls `IAP_GetSubscriptionStatusCB` |
| `void NIAP_OnLimitRequestCallback(String str)` | `(Ljava/lang/String;)V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1OnLimitRequestCallback` | `android2d.iap.IAP_CallBack.IAP_OnLimitRequestCallback` | calls `IAP_OnLimitRequestCallback` |
| `void NIAP_OnPurchaseRequestCancelCallback()` | `()V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1OnPurchaseRequestCancelCallback` | `android2d.iap.IAP_CallBack.IAP_OnPurchaseRequestCancelCallback` | calls `IAP_OnPurchaseRequestCancelCallback` |
| `void NIAP_OnPurchaseRequestSentCallback()` | `()V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1OnPurchaseRequestSentCallback` | `android2d.iap.IAP_CallBack.IAP_OnPurchaseRequestSentCallback` | calls `IAP_OnPurchaseRequestSentCallback` |
| `void NIAP_OnReLoadFeedCallback()` | `()V` | `Java_com_gameloft_android2d_iap_IAP_1CallBack_NIAP_1OnReLoadFeedCallback` | `android2d.iap.IAP_CallBack.IAP_OnReLoadFeedCallback` | calls `IAP_OnReLoadFeedCallback` |

### `com.gameloft.android2d.iap.Links`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `boolean NIAP_FederationIsInitialized()` | `()Z` | `Java_..._NIAP_1FederationIsInitialized` | `android2d.iap.Links.IAP_FederationIsInitialized` | calls `IAP_FederationIsInitialized` |
| `String NIAP_GetAccessToken()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetAccessToken` | `android2d.iap.Links.IAP_GetAccessToken` | calls `IAP_GetAccessToken` |
| `String NIAP_GetAnonymousCredential()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetAnonymousCredential` | `android2d.iap.Links.IAP_GetAnonymousCredential` | calls `IAP_GetAnonymousCredential` |
| `String NIAP_GetCRMFeed()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetCRMFeed` | `android2d.iap.Links.IAP_GetCRMFeed` | calls `IAP_GetCRMFeed` |
| `String NIAP_GetCRMUrl()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetCRMUrl` | `android2d.iap.Links.IAP_GetCRMUrl` | calls `IAP_GetCRMUrl` |
| `String NIAP_GetClientId()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetClientId` | `android2d.iap.Links.IAP_GetClientId` | calls `IAP_GetClientId` |
| `String NIAP_GetDatacenter()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetDatacenter` | `android2d.iap.Links.IAP_GetDatacenter` | calls `IAP_GetDatacenter` |
| `String NIAP_GetDeviceId()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetDeviceId` | `android2d.iap.Links.IAP_GetDeviceId` | calls `IAP_GetDeviceId` |
| `String NIAP_GetLastResponseTimestamp()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetLastResponseTimestamp` | `android2d.iap.Links.IAP_GetLastResponseTimestamp` | calls `IAP_GetLastResponseTimestamp` |
| `String NIAP_GetUserCredential()` | `()Ljava/lang/String;` | `Java_..._NIAP_1GetUserCredential` | `android2d.iap.Links.IAP_GetUserCredential` | calls `IAP_GetUserCredential` |
| `boolean NIAP_IsFederationOffline()` | `()Z` | `Java_..._NIAP_1IsFederationOffline` | `android2d.iap.Links.IAP_IsFederationOffline` | calls `IAP_IsFederationOffline` |

### `com.gameloft.android2d.iap.wrapper.DataSharing`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void nativeInit()` | `()V` | no export | `android2d.iap.wrapper.DataSharing.doNativeInit` | - |

### `com.gameloft.android2d.iap.wrapper.HDIDFV`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `String getNHDIDFV()` | `()Ljava/lang/String;` | no export | `android2d.iap.wrapper.HDIDFV.getHDIDFV` | - |
| `String getNHDIDFVVersion()` | `()Ljava/lang/String;` | no export | `android2d.iap.wrapper.HDIDFV.getHDIDFVVersion` | - |

### `com.gameloft.android2d.iap.wrapper.NativeUtils`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `boolean CheckRoot()` | `()Z` | no export | `android2d.iap.wrapper.Utils.isRooted` | - |
| `float getClock()` | `()F` | no export | `android2d.iap.wrapper.Utils.getClock`, `android2d.iap.wrapper.Utils.if` | - |
| `long getClockMillis()` | `()J` | no export | `android2d.iap.wrapper.Utils.getClockMillis`, `android2d.iap.wrapper.Utils.if` | - |
| `long getClockNano()` | `()J` | no export | `android2d.iap.wrapper.Utils.getClockNano`, `android2d.iap.wrapper.Utils.if` | - |

### `com.gameloft.glads.AndroidAds`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void NativeOnVolumeChanged(long j)` | `(J)V` | `Java_..._NativeOnVolumeChanged` | `glads.AndroidAds.OnVolumeChanged` | calls `GLAds::OnVolumeChanged` |

### `com.gameloft.glads.AndroidDevice`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void nativeModalWebViewResponse(String str, String str2)` | `(Ljava/lang/String;Ljava/lang/String;)V` | `Java_..._nativeModalWebViewResponse` | `glads.AndroidDevice.a` | calls `operator new`, `operator delete` |

### `com.gameloft.glads.AndroidWebView`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void NativeButtonClicked(long j, int i)` | `(JI)V` | `Java_..._NativeButtonClicked` | `glads.AndroidWebView.onClick` | converts string/array args then updates native state (28 bytes) |
| `long NativeGetTime()` | `()J` | `Java_..._NativeGetTime` | `glads.AndroidWebView.GetTime` | converts string/array args then updates native state (48 bytes) |
| `void NativeOnCreateWindow(long j, long j2, String str)` | `(JJLjava/lang/String;)V` | `Java_..._NativeOnCreateWindow` | `glads.AndroidWebView.if` | calls `operator new`, `AndroidWebView::OnCreateWindow`, `operator delete` |
| `void NativeOnFailedLoad(long j, String str)` | `(JLjava/lang/String;)V` | `Java_..._NativeOnFailedLoad` | `glads.AndroidWebView.if` | calls `operator new`, `GLWebView::OnFailedLoad`, `operator delete` |
| `void NativeOnFinishLoad(long j)` | `(J)V` | `Java_..._NativeOnFinishLoad` | `glads.AndroidWebView.if` | calls `GLWebView::OnFinishLoad` |
| `void NativeOnJavaScriptEvaluated(long j, String str, int i)` | `(JLjava/lang/String;I)V` | `Java_..._NativeOnJavaScriptEvaluated` | `glads.AndroidWebView.if` | calls `operator new`, `AndroidWebView::OnJavaScriptEvaluated`, `operator delete` |
| `void NativeOnTouch(long j)` | `(J)V` | `Java_..._NativeOnTouch` | `glads.AndroidWebView.if` | calls `AndroidWebView::OnTouch` |
| `boolean NativeShouldOpenURL(long j, String str)` | `(JLjava/lang/String;)Z` | `Java_..._NativeShouldOpenURL` | `glads.AndroidWebView.if` | calls `operator new`, `AndroidWebView::ShouldOpenURL`, `operator delete` |

### `com.gameloft.glads.OmSDK`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `String NativeGetLibraryVersion()` | `()Ljava/lang/String;` | `Java_..._NativeGetLibraryVersion` | `glads.OmSDK.if` | converts string/array args then updates native state (20 bytes) |
| `void NativeOnSDKActivated(boolean z)` | `(Z)V` | `Java_..._NativeOnSDKActivated` | `glads.OmSDK.if` | converts string/array args then updates native state (20 bytes) |

### `com.gameloft.glads.Utils`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `boolean nativeIsUnityMode()` | `()Z` | `Java_..._nativeIsUnityMode` | `glads.Utils.IsUnityMode` | trivial stub (12 bytes) |
| `void nativeRestoreOrientation()` | `()V` | `Java_..._nativeRestoreOrientation` | `glads.Utils.RestoreOrientation` | converts string/array args then updates native state (20 bytes) |
| `void nativeSetOrientation(int i)` | `(I)V` | `Java_..._nativeSetOrientation` | `glads.Utils.SetOrientation` | converts string/array args then updates native state (24 bytes) |

### `com.gameloft.GLSocialLib.facebook.FacebookAndroidGLSocialLib`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void nativeOnFBDataLoad(String str)` | `(Ljava/lang/String;)V` | `Java_..._nativeOnFBDataLoad` | `GLSocialLib.facebook.FacebookAndroidGLSocialLib.Init`, `GLSocialLib.facebook.FacebookAndroidGLSocialLib.run` | calls `operator new`, `appGLSocialLib_OnFBDataLoad`, `operator delete` |
| `void nativeOnFBDialogDidComplete()` | `()V` | `Java_..._nativeOnFBDialogDidComplete` | `GLSocialLib.facebook.FacebookAndroidGLSocialLib.onSuccess` | calls `appGLSocialLib_OnFBDialogDidComplete` |
| `void nativeOnFBDialogDidNotComplete()` | `()V` | `Java_..._nativeOnFBDialogDidNotComplete` | `GLSocialLib.facebook.FacebookAndroidGLSocialLib.onCancel` | calls `appGLSocialLib_OnFBDialogDidNotComplete` |
| `void nativeOnFBFailWithError(String str)` | `(Ljava/lang/String;)V` | `Java_..._nativeOnFBFailWithError` | `GLSocialLib.facebook.FacebookAndroidGLSocialLib.RequestAppId`, `GLSocialLib.facebook.FacebookAndroidGLSocialLib.RequestAppDetails` | calls `operator new`, `appGLSocialLib_OnFBFailWithError`, `operator delete` |

### `com.gameloft.GLSocialLib.GameAPI.GameAPIAndroidGLSocialLib`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void nativeGameAPIComplete()` | `()V` | `Java_..._nativeGameAPIComplete` | `GLSocialLib.GameAPI.AchievementManager.switch`, `GLSocialLib.GameAPI.GameAPIAndroidGLSocialLib.run` | calls `sociallib::CSingleton<sociallib::ClientSNSInterface>::getIns`, `sociallib::ClientSNSInterface::getCurrentActiveRequestState` |
| `void nativeGameAPICompleteWithData(String str, boolean z, byte[] bArr)` | `(Ljava/lang/String;Z[B)V` | `Java_..._nativeGameAPICompleteWithData` | `GLSocialLib.GameAPI.GameAPIAndroidGLSocialLib.if`, `GLSocialLib.GameAPI.GameAPIAndroidGLSocialLib.GetPlayerAvatar` | calls `operator new[]`, `GLSocialLib_OnGameAPIDataLoad`, `acp_utils::GetVM` |
| `void nativeGameAPIDidNotComplete(String str)` | `(Ljava/lang/String;)V` | `Java_..._nativeGameAPIDidNotComplete` | `GLSocialLib.GameAPI.AchievementManager.onResult`, `GLSocialLib.GameAPI.AchievementManager.showAchievements` | calls `sociallib::CSingleton<sociallib::ClientSNSInterface>::getIns`, `sociallib::ClientSNSInterface::getCurrentActiveRequestState`, `XP_API_STRLEN` |
| `void nativeGameAPINotifyAuthChanges(boolean z, String str)` | `(ZLjava/lang/String;)V` | `Java_..._nativeGameAPINotifyAuthChanges` | `GLSocialLib.GameAPI.GameAPIAndroidGLSocialLib.DisconnectGamesClient`, `GLSocialLib.GameAPI.GameAPIAndroidGLSocialLib.if` | calls `operator new`, `_ZN9sociallib15SNSRequestStateC1ENS_13ClientSNSEnumENSt6__nd`, `XP_API_STRLEN` |
| `void nativeGameAPISetCanceled()` | `()V` | `Java_..._nativeGameAPISetCanceled` | `GLSocialLib.GameAPI.GameAPIAndroidGLSocialLib.if` | calls `sociallib::CSingleton<sociallib::ClientSNSInterface>::getIns`, `sociallib::ClientSNSInterface::getCurrentActiveRequestState` |
| `void nativeInit()` | `()V` | no export | (no Java caller found) | - |

### `com.gameloft.GLSocialLib.PlatformAndroid`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void nativeInit()` | `()V` | no export | (no Java caller found) | - |

### `com.gameloft.igp.IGPFreemiumActivity`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void IGPAutomation(String str, String str2)` | `(Ljava/lang/String;Ljava/lang/String;)V` | `Java_..._IGPAutomation` | `igp.IGPFreemiumActivity.if` | converts string/array args then updates native state (176 bytes) |

### `com.gameloft.ingamebrowser.InGameBrowser`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void InGameBrowserCheckRewardCallback(String str, boolean z)` | `(Ljava/lang/String;Z)V` | `Java_..._InGameBrowserCheckRewardCallback` | `ingamebrowser.InGameBrowser.CallNativeCheckRewardCallback` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |
| `void InGameBrowserExitCallback()` | `()V` | `Java_..._InGameBrowserExitCallback` | `ingamebrowser.InGameBrowser.CallNativeExitCallback` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |
| `void InGameBrowserExternalPageFailedToOpenCallback()` | `()V` | `Java_..._InGameBrowserExternalPageFailedToOpenCallback` | `ingamebrowser.InGameBrowser.CallNativePageFailedToOpenCallback` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |
| `void InGameBrowserStartCallback()` | `()V` | `Java_..._InGameBrowserStartCallback` | `ingamebrowser.InGameBrowser.CallNativeStartCallback` | calls `operator new`, `_ZN7olutils7logging3LogC1ENS0_5LevelERKNSt6__ndk112basic_str`, `olutils::logging::AddLog` |
| `void nativeUnreadNewsChangedCallback(int i)` | `(I)V` | no export | (no Java caller found) | - |

### `com.gameloft.popupslib.PopUpsBridgeClass`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `void nativeLog(int i, String str, String str2, int i2)` | `(ILjava/lang/String;Ljava/lang/String;I)V` | `Java_..._nativeLog` | `popupslib.PopUpsBridgeClass.Log` | calls `operator new`, `PopUpsLib::PopUpsControl::Log`, `operator delete` |
| `void nativeOnAssetTracking(String str)` | `(Ljava/lang/String;)V` | `Java_..._nativeOnAssetTracking` | `popupslib.PopUpsBridgeClass.OnAssetTracking` | calls `operator new`, `PopUpsLib::PopUpsControl::OnAssetTracking`, `operator delete` |
| `void nativeOnDownloadState(int i)` | `(I)V` | no export | `popupslib.PopUpsBridgeClass.OnDownloadState` | - |
| `void nativeOnErrorMessage(int i)` | `(I)V` | `Java_..._nativeOnErrorMessage` | `popupslib.PopUpsBridgeClass.OnErrorMessage` | calls `PopUpsLib::PopUpsControl::OnErrorMessage` |
| `void nativeOnJavaScriptEvaluation(String str, long j)` | `(Ljava/lang/String;J)V` | `Java_..._nativeOnJavaScriptEvaluation` | `popupslib.PopUpsBridgeClass.OnJavaScriptEvaluation` | calls `operator new`, `PopUpsLib::PopUpsControl::OnJavaScriptEvaluation`, `operator delete` |
| `void nativeOnScreenSizeChanged(int i)` | `(I)V` | `Java_..._nativeOnScreenSizeChanged` | `popupslib.PopUpsManager.b`, `popupslib.PopUpsViewComponent.a` | calls `PopUpsLib::PopUpsControl::OnScreenSizeChanged` |
| `void nativeOnViewState(int i)` | `(I)V` | `Java_..._nativeOnViewState` | `popupslib.PopUpsBridgeClass.OnViewState` | calls `PopUpsLib::PopUpsControl::OnViewState` |

### `com.library.decrawso.DecRawso`

| Method (Java signature) | JNI descriptor | Implemented as | Called by | What the native side does |
|---|---|---|---|---|
| `int Decode(AssetManager assetManager, String str, String str2, String str3)` | `(L?AssetManager;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I` | libDecRawso.so | (no Java caller found) | - |
| `boolean IsArmMode()` | `()Z` | libDecRawso.so | (no Java caller found) | - |
| `void SetFilter(String str, String str2)` | `(Ljava/lang/String;Ljava/lang/String;)V` | libDecRawso.so | `com.library.decrawso.DecRawso.if` | - |

## (b) Java classes, methods and fields that the native code looks up

### Class table preloaded at `JNI_OnLoad`

`JNI_OnLoad` (0x121f928) stores the `JavaVM*` (`acp_utils::acp_internal::Internal::SetVm`), calls `GetEnv(JNI_VERSION_1_6 = 0x10006)`, then `Internal::LoadClasses(JNIEnv*)` runs `FindClass` for the classes below and caches them (global refs) in a `std::map<std::string, jclass>` keyed by the relative name (the part after `com/gameloft/android/ANMP/GloftNOHM`, e.g. `/PackageUtils/AndroidUtils`). Later `PackageUtils::GetClass(std::string)` is a map lookup, so every later `GetStaticMethodID` uses a cached class. `FindClass` runs on the thread that loads the library (the Java UI thread inside `System.load`), which is why app classes resolve. A second, direct `FindClass("android/os/Process")` exists in `glf::Thread::Impl::SetPriority` and `FindClass("java/lang/String")` in another JNI helper.

| # | Class | Present in this APK? |
|---|---|---|
| 1 | `java/lang/Class` | framework |
| 2 | `android/os/Build` | framework |
| 3 | `android/content/Intent` | framework |
| 4 | `android/os/Bundle` | framework |
| 5 | `android/media/AudioTrack` | framework |
| 6 | `java/security/SecureRandom` | framework |
| 7 | `java/util/HashSet` | framework |
| 8 | `java/lang/Long` | framework |
| 9 | `java/security/spec/X509EncodedKeySpec` | framework |
| 10 | `java/security/KeyFactory` | framework |
| 11 | `java/security/NoSuchAlgorithmException` | framework |
| 12 | `java/security/spec/InvalidKeySpecException` | framework |
| 13 | `com/gameloft/igp/IGPFreemiumActivity` | yes |
| 14 | `com/gameloft/igp/InGamePromotionPlugin` | yes |
| 15 | `com/gameloft/ingamebrowser/InGameBrowser` | yes |
| 16 | `com/gameloft/GLSocialLib/PlatformAndroid` | yes |
| 17 | `com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib` | yes |
| 18 | `com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib` | yes |
| 19 | `com/gameloft/glads/AndroidDevice` | yes |
| 20 | `com/gameloft/glads/AndroidWebView` | yes |
| 21 | `com/gameloft/glads/MRAIDHtmlProcessor` | yes |
| 22 | `com/gameloft/glads/MoatSDK` | yes |
| 23 | `com/gameloft/glads/AvidSDK` | yes |
| 24 | `com/gameloft/glads/AndroidAds` | yes |
| 25 | `com/gameloft/adsmanager/AdMob` | yes |
| 26 | `com/gameloft/adsmanager/FAN` | yes |
| 27 | `com/gameloft/adsmanager/InterstitialFAN` | yes |
| 28 | `com/gameloft/adsmanager/InterstitialFANListener` | NO (lookup returns null/exception) |
| 29 | `com/gameloft/adsmanager/AdsManager` | yes |
| 30 | `com/gameloft/adsmanager/BannerFAN` | yes |
| 31 | `com/gameloft/adsmanager/BannerFANListener` | NO (lookup returns null/exception) |
| 32 | `com/gameloft/adsmanager/BannerAdMob` | yes |
| 33 | `com/gameloft/adsmanager/BannerAdMobListener` | NO (lookup returns null/exception) |
| 34 | `com/gameloft/adsmanager/InterstitialAdMob` | yes |
| 35 | `com/gameloft/adsmanager/InterstitialAdMobListener` | NO (lookup returns null/exception) |
| 36 | `com/gameloft/adsmanager/Constants` | yes |
| 37 | `com/gameloft/adsmanager/UnityAdsManager` | yes |
| 38 | `com/gameloft/adsmanager/JavaUtils` | yes |
| 39 | `com/gameloft/adsmanager/NativeFAN` | yes |
| 40 | `com/gameloft/adsmanager/NativeFANListener` | NO (lookup returns null/exception) |
| 41 | `com/gameloft/adsmanager/NativeAdMob` | yes |
| 42 | `com/gameloft/adsmanager/NativeAdMobListener` | NO (lookup returns null/exception) |
| 43 | `com/gameloft/adsmanager/IncentivizedFAN` | NO (lookup returns null/exception) |
| 44 | `com/gameloft/adsmanager/IronSourceAds` | yes |
| 45 | `com/gameloft/adsmanager/InterstitialIronSource` | yes |
| 46 | `com/gameloft/adsmanager/IncentivizedIronSource` | yes |
| 47 | `com/gameloft/adsmanager/BannerIronSource` | yes |
| 48 | `com/gameloft/android2d/iap/IAPLib` | yes |
| 49 | `com/gameloft/popupslib/PopUpsBridgeClass` | yes |
| 50 | `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/AndroidUtils` | yes |
| 51 | `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/EmulatorDetector` | yes |
| 52 | `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/LogoViewPlugin` | yes |
| 53 | `com/gameloft/android/ANMP/GloftNOHM/DataSharing` | yes |
| 54 | `com/gameloft/android/ANMP/GloftNOHM/GLUtils/Device` | yes |
| 55 | `com/gameloft/android/ANMP/GloftNOHM/GLUtils/SUtils` | yes |
| 56 | `com/gameloft/android/ANMP/GloftNOHM/PushNotification/SimplifiedAndroidUtils` | yes |
| 57 | `com/gameloft/android/ANMP/GloftNOHM/AppDetection` | yes |
| 58 | `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/ClipboardPlugin` | yes |
| 59 | `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/PermissionPlugin` | yes |
| 60 | `com/gameloft/android/ANMP/GloftNOHM/GoogleFirebase/GoogleFirebase` | yes |
| 61 | `com/gameloft/android/ANMP/GloftNOHM/GLUtils/CutoutHelper` | yes |

Classes looked up lazily by relative name through `GetClass` after startup (not in the table above; `PackageUtils::GetClass` falls back to `FindClass`-style resolution): `/PackageUtils/AdServerPlugin`, `/PackageUtils/GoogleInvitePlugin`, `/GLUtils/controller/NativeBridgeHIDControllers` (missing from the APK, 3 lookups in `HidController`), `/DRM/Gloft/GloftDrmImpl`, `com/gameloft/glads/OmSDK`. The classes `AdMob`, `FAN` and `*FAN*` are preloaded but their members are never looked up by this build (their `*NotifyEvent` natives are not exported either).

### Lookups grouped by Java class

Legend: `S` = `GetStaticMethodID`, `I` = `GetMethodID`, `F` = `GetFieldID`, `SF` = `GetStaticFieldID`. "Native function" = the libNOVA function that performs the lookup (first two). "Java side" = what the decompiled Java method does (one level of delegation is followed). Class resolution: "tag" = known from the native call, "sig" = identified by matching name and descriptor against the decompiled Java (the cached `jclass` global could not be tracked statically; where several Java classes define the same method the one used by `PackageUtils` is shown first). The signature strings are exactly the ones passed to JNI.

#### `com/gameloft/android2d/iap/IAPLib` (125)

Port stance: In-app purchase (Gameloft IAP). Stub: report purchase unavailable / no items.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `AllowLoadFeedAgain` | `()V` | IAPLib::Init | 2 stmts; eb = false; |
| S | `CheckAvailable` | `()Z` | IAPLib::Init | returns eY.fa && !RMS.isDemoBuild() |
| S | `CheckItemIsHidden` | `(ILjava/lang/String;)Z` | IAPLib::Init | 7 stmts; try { |
| S | `CheckRedeem` | `()V` | IAPLib::Init | 12 stmts; uses background thread |
| S | `EShowRegionOperatorSelection` | `()V` | IAPLib::Init | no-op (empty body) |
| S | `GetBillingType` | `(I)Ljava/lang/String;` | IAPLib::Init | returns GetBillingType(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetBillingType` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetBundleItemInfo` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetConfigStoreName` | `()Ljava/lang/String;` | IAPLib::Init | returns eY.eZ.a() != null ? eY.eZ.a().E() : "" |
| S | `GetCountryISO` | `()Ljava/lang/String;` | IAPLib::Init | 2 stmts; String simCountry = Utils.getSimCountry(); |
| S | `GetFeed` | `()V` | IAPLib::Init | 48 stmts; uses background thread |
| S | `GetHiddenItemNumber` | `(Ljava/lang/String;)I` | IAPLib::Init | 10 stmts; try { |
| S | `GetItemCategoryTag` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetItemContentID` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetItemName` | `(I)Ljava/lang/String;` | IAPLib::Init | returns GetItemName(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetItemName` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetItemNumber` | `()I` | IAPLib::Init | returns GetItemNumber(SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetItemNumber` | `(Ljava/lang/String;)I` | IAPLib::Init | 10 stmts; try { |
| S | `GetItemTrackingUID` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetLastBillingType` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastBillingType() -> returns rms_LoadInfo(20) |
| S | `GetLastContentIdPurchased` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastContentID() -> returns rms_LoadInfo(23) |
| S | `GetLastFormattedPricePurchased` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastFormattedPrice() -> 2 stmts; String strRmsLoad = rmsLoad(ay[49]); |
| S | `GetLastItemJsonPurchased` | `()Ljava/lang/String;` | IAPLib::Init | RMS.rms_LoadInfo() -> returns rmsLoad(ay[i]) |
| S | `GetLastItemTrackingUIDPurchased` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastItemTrackingUID() -> 2 stmts; String strRmsLoad = rmsLoad(ay[50]); |
| S | `GetLastOfflineItemsPurchased` | `()[Ljava/lang/String;` | IAPLib::Init | RMS.getLastOfflineItems() -> 4 stmts; String strRmsLoad = rmsLoad(ay[19]); |
| S | `GetLastPriceValuePurchased` | `()F` | IAPLib::Init | delegates to SUtils.parseFloatNumber() (obfuscated/not decompiled) |
| S | `GetLastPurchasedBundleInfo` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastPurchasedBundleInfo() -> 4 stmts; String strRmsLoad = rmsLoad(ay[56]); |
| S | `GetLastPurchasedSubscriptionItemInfo` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastPurchasedSubscriptionItemInfo() -> 4 stmts; String strRmsLoad = rmsLoad(ay[65]); |
| S | `GetLastPurchasedSubscriptionRewardItemInfo` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastPurchasedSubscriptionRewardItemInfo() -> 4 stmts; String strRmsLoad = rmsLoad(ay[66]); |
| S | `GetLastVirtualCurrencyPurchased` | `()J` | IAPLib::Init | RMS.getLastVirtualCurrency() -> 7 stmts; String strRmsLoad = rmsLoad(ay[18]); |
| S | `GetLimitationNextTransactionTime` | `()Ljava/lang/String;` | IAPLib::Init | returns ew |
| S | `GetLimitationSecondsBeforeNextTransaction` | `()Ljava/lang/String;` | IAPLib::Init | returns ex |
| S | `GetLimitsValidation` | `()V` | IAPLib::Init | 35 stmts; if (!Device.hasConnectivity()) { |
| S | `GetListItemsRestore` | `()V` | IAPLib::Init | 14 stmts; uses posts to UI thread, background thread |
| S | `GetOfflineItems` | `(I)[Ljava/lang/String;` | IAPLib::Init | returns GetOfflineItems(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetOfflineItems` | `(ILjava/lang/String;)[Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetOldContentId` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetPackageIDPurchased` | `()I` | IAPLib::Init | 4 stmts; IAPLib iAPLib = eY; |
| S | `GetPackageIDPurchasedNTier` | `()I` | IAPLib::Init | returns getIndexOfPricepoint(GetPackageIDPurchased(), GetPackageTypePurchased()) |
| S | `GetPackageTypePurchased` | `()Ljava/lang/String;` | IAPLib::Init | 3 stmts; if (eY.fg.equals("")) { |
| S | `GetPrice` | `(I)Ljava/lang/String;` | IAPLib::Init | returns GetPrice(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetPrice` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 8 stmts; try { |
| S | `GetPricePoint` | `(I)I` | IAPLib::Init | returns GetPricePoint(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetPricePoint` | `(ILjava/lang/String;)I` | IAPLib::Init | 14 stmts; try { |
| S | `GetPriceValue` | `(ILjava/lang/String;)F` | IAPLib::Init | 7 stmts; try { |
| S | `GetProfilesStatus` | `()Ljava/lang/String;` | IAPLib::Init | 2 stmts; String strRms_LoadInfo = RMS.rms_LoadInfo(44); |
| S | `GetPromoDescription` | `()Ljava/lang/String;` | IAPLib::Init | returns eY.eZ.a().s() |
| S | `GetPromoEffectiveDiscount` | `(ILjava/lang/String;)I` | IAPLib::Init | 7 stmts; try { |
| S | `GetPromoEffectiveDiscountAmount` | `(ILjava/lang/String;)I` | IAPLib::Init | 7 stmts; try { |
| S | `GetPromoEffectiveDiscountPrice` | `(ILjava/lang/String;)I` | IAPLib::Init | 9 stmts; try { |
| S | `GetPromoEndTime` | `()Ljava/lang/String;` | IAPLib::Init | 3 stmts; if (eD) { |
| S | `GetPromoOldPrice` | `(I)Ljava/lang/String;` | IAPLib::Init | returns GetPromoOldPrice(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetPromoOldPrice` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetPromoOldPriceValue` | `(ILjava/lang/String;)F` | IAPLib::Init | 8 stmts; String strJ; |
| S | `GetPurchaseReceipts` | `()Ljava/lang/String;` | IAPLib::Init | RMS.rms_LoadInfo() -> returns rmsLoad(ay[i]) |
| S | `GetRestoreCertificate` | `()Ljava/lang/String;` | IAPLib::Init | RMS.rms_LoadInfo() -> returns rmsLoad(ay[i]) |
| S | `GetRestoreTransactionID` | `()Ljava/lang/String;` | IAPLib::Init | RMS.rms_LoadInfo() -> returns rmsLoad(ay[i]) |
| S | `GetServerTime` | `()Ljava/lang/String;` | IAPLib::Init | 4 stmts; String strU = eY.eZ.a().u(); |
| S | `GetSpecialFlow` | `()I` | IAPLib::Init | 7 stmts; try { |
| S | `GetSpecialPackageRequirement` | `(I)I` | IAPLib::Init | returns GetSpecialPackageRequirement(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetSpecialPackageRequirement` | `(ILjava/lang/String;)I` | IAPLib::Init | returns 0 |
| S | `GetSubscriptionStatus` | `(Ljava/lang/String;)V` | IAPLib::Init | 11 stmts; uses posts to UI thread, background thread |
| S | `GetTermsAndConditions` | `()Ljava/lang/String;` | IAPLib::Init | 14 stmts; f fVarNextElement; |
| S | `GetTermsAndConditions` | `(I)Ljava/lang/String;` | IAPLib::Init | returns GetTermsAndConditions(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `GetTermsAndConditions` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `GetTransactionID` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getTransactionID() -> 4 stmts; String strRms_LoadInfo = rms_LoadInfo(39); |
| S | `GetUpdateProfileInfo` | `()V` | IAPLib::Init | 7 stmts; eY.eZ.b(); |
| S | `Init` | `()V` | IAPLib::Init | 2 stmts; Utils.setContext(com.gameloft.android.ANMP.GloftNOHM.GLUtils.SUtils.ge |
| S | `IsLastPurchaseRedeem` | `()Z` | IAPLib::Init | 2 stmts; String strRms_LoadInfo = RMS.rms_LoadInfo(59); |
| S | `IsLastPurchaseRestore` | `()Z` | IAPLib::Init | 2 stmts; String strRms_LoadInfo = RMS.rms_LoadInfo(62); |
| S | `IsLastPurchaseSubscription` | `()Z` | IAPLib::Init | 2 stmts; String strRms_LoadInfo = RMS.rms_LoadInfo(63); |
| S | `NewVerifyRequest` | `(I)Z` | IAPLib::Init | returns VerifyProcess(verifyRequest(i)) |
| S | `NewVerifyRequest` | `(Ljava/lang/String;)Z` | IAPLib::Init | returns VerifyProcess(verifyRequest(i)) |
| S | `SendRequest` | `(I)V` | IAPLib::Init | calls SendRequest(i, SUtils.getStringRS(R.string.k_ItemCash), 0L) |
| S | `SendRequest` | `(ILjava/lang/String;)V` | IAPLib::Init | 5 stmts; try { |
| S | `SendRequest` | `(ILjava/lang/String;J)V` | IAPLib::Init | 243 stmts; uses posts to UI thread, Android system service, Play Billing, background thread |
| S | `SendRequest` | `(Ljava/lang/String;J)V` | IAPLib::Init | 5 stmts; try { |
| S | `Update` | `()I` | IAPLib::Init | 18 stmts; IAPLib iAPLib = eY; |
| S | `UpdatePromoCountDownTime` | `()J` | IAPLib::Init | 5 stmts; long jElapsedRealtime = (fu - (SystemClock.elapsedRealtime() - fv)) +  |
| S | `checkEntryIdItem` | `(Ljava/lang/String;)Z` | IAPLib::Init | 4 stmts; try { |
| S | `checkMessageSent` | `()Z` | IAPLib::Init | 4 stmts; eY.fK = RMS.isSMSSent(); |
| S | `getCustomerURL` | `(Ljava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 13 stmts; if (str == null \|\| str.equals("")) { |
| S | `getEntryIdItem` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 4 stmts; try { |
| S | `getError` | `()I` | IAPLib::Init | returns eY.fi |
| S | `getFlowText` | `(ILjava/lang/String;I)Ljava/lang/String;` | IAPLib::Init | returns null |
| S | `getIAPLimitationHelp` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 19 stmts; uses DisplayMetrics |
| S | `getIAPLimitationWarning` | `(Ljava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 8 stmts; uses DisplayMetrics |
| S | `getItemCid` | `(ILjava/lang/String;)I` | IAPLib::Init | 4 stmts; try { |
| S | `getItemCountDownTime` | `(ILjava/lang/String;)J` | IAPLib::Init | 10 stmts; try { |
| S | `getItemCurrencyISOCode` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `getItemGroup` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 4 stmts; try { |
| S | `getItemJson` | `(Ljava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 13 stmts; uses Play Billing |
| S | `getItemPromoEndtime` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 5 stmts; try { |
| S | `getItemRemaining` | `(ILjava/lang/String;)I` | IAPLib::Init | 7 stmts; String strB; |
| S | `getItemSku` | `(ILjava/lang/String;)I` | IAPLib::Init | 4 stmts; try { |
| S | `getLanguage` | `()Ljava/lang/String;` | IAPLib::Init | returns eY.fq |
| S | `getLastCurrencyISOCode` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getLastCurrency() -> 2 stmts; String strRmsLoad = rmsLoad(ay[13]); |
| S | `getLastPackOrderPurchased` | `()Ljava/lang/String;` | IAPLib::Init | 2 stmts; String lastPackOrderPurchased = RMS.getLastPackOrderPurchased(); |
| S | `getMoneySpentInfo` | `()Ljava/lang/String;` | IAPLib::Init | RMS.getMoneySpent() -> 5 stmts; int iIndexOf; |
| S | `getOldVirtualCurrency` | `(I)J` | IAPLib::Init | returns getOldVirtualCurrency(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `getOldVirtualCurrency` | `(ILjava/lang/String;)J` | IAPLib::Init | 7 stmts; try { |
| S | `getSupportNumber` | `(I)Ljava/lang/String;` | IAPLib::Init | returns getSupportNumber(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `getSupportNumber` | `(ILjava/lang/String;)Ljava/lang/String;` | IAPLib::Init | 7 stmts; try { |
| S | `getVirtualCurrency` | `(I)J` | IAPLib::Init | returns getVirtualCurrency(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `getVirtualCurrency` | `(ILjava/lang/String;)J` | IAPLib::Init | 7 stmts; try { |
| S | `handlePauseEvent` | `()V` | IAPLib::Init | no-op (empty body) |
| S | `handleResumeEvent` | `()V` | IAPLib::Init | 10 stmts; uses background thread |
| S | `isManagedItem` | `(ILjava/lang/String;)Z` | IAPLib::Init | 6 stmts; String strO; |
| S | `isNeedPreFormation` | `(I)Z` | IAPLib::Init | returns isNeedPreFormation(i, SUtils.getStringRS(R.string.k_ItemCash)) |
| S | `isNeedPreFormation` | `(ILjava/lang/String;)Z` | IAPLib::Init | 10 stmts; try { |
| S | `itemUnlockedSilent` | `()Z` | IAPLib::Init | returns false |
| S | `redeemCode` | `()V` | IAPLib::Init | 83 stmts; uses posts to UI thread, Android system service, Play Billing, background thread |
| S | `reset` | `()V` | IAPLib::Init | 9 stmts; RMS.rmsSave(ay[2], ""); |
| S | `setAgeUser` | `(I)V` | IAPLib::Init | calls fF = i |
| S | `setAnonymousCredential` | `(Ljava/lang/String;)V` | IAPLib::Init | 3 stmts; fs = str; |
| S | `setClientID` | `(Ljava/lang/String;)V` | IAPLib::Init | calls eU = str |
| S | `setDIAPConfirmationDisabledOnShop` | `(Z)V` | IAPLib::Init | calls ek = z |
| S | `setDIAPItemInShop` | `(Z)V` | IAPLib::Init | calls el = z |
| S | `setDatacenter` | `(Ljava/lang/String;)V` | IAPLib::Init | calls eW = str |
| S | `setGameType` | `(Ljava/lang/String;)V` | IAPLib::Init | calls fG = str |
| S | `setLanguage` | `(Ljava/lang/String;)V` | IAPLib::Init | 5 stmts; String str2 = eY.fq; |
| S | `setResult` | `(I)V` | IAPLib::Init | 11 stmts; IAPLib iAPLib = eY; |
| S | `setUserCredential` | `(Ljava/lang/String;)V` | IAPLib::Init | calls eV = str |
| S | `setUserName` | `(Ljava/lang/String;)V` | IAPLib::Init | calls eT = str |
| S | `showIAPLimitationInforming` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | IAPLib::Init | 21 stmts; uses posts to UI thread, AlertDialog, background thread |

#### `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/AndroidUtils` (81)

Port stance: REQUIRED. Host implements: paths, device identity, preferences, asset reading, dialogs/toasts (log), orientation (ignore), keep-screen-on (ignore), exit.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `AreNotificationsEnabled` (sig) | `()Z` | PackageUtils::AreNotificationsEnabled | returns NotificationManagerCompat.from(SUtils.getApplicationContext()).b() |
| S | `DeleteFile` (sig) | `(Ljava/lang/String;)V` | PackageUtils::DeleteFile | File.delete() on the path |
| S | `DisableUserLocation` (sig) (also in LocationPlugin) | `()V` | PackageUtils::DisableUserLocation | LocationPlugin: stops location updates |
| S | `EnableUserLocation` (sig) (also in LocationPlugin) | `()V` | PackageUtils::EnableUserLocation | LocationPlugin: starts LocationManager updates on the UI thread; results via JNIBridge.SetUserLocation |
| S | `ExitApplication` (sig) | `(Z)V` | PackageUtils::ExitApplication | MainActivity.a(restart): runOnUiThread(finish()); onPause then kills the process (optionally schedules a restart alarm) |
| S | `GenericUnzipArchive` (sig) | `(Ljava/lang/String;Ljava/lang/String;)Z` | PackageUtils::GenericUnzipArchive | unzips zip(arg1) into directory(arg2) with ZipInputStream; returns success |
| S | `GetAndroidID` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Settings.Secure.ANDROID_ID; returns "" if null, the known-bad value 9774d56d682e549c, or shorter than 15 chars |
| S | `GetApkPath` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | PackageManager.getApplicationInfo(pkg).sourceDir (the installed APK path) |
| S | `GetAssetAsString` | `(Ljava/lang/String;)[B` | Internal::InitializeCachedVars | opens the APK asset (leading "." stripped, ACCESS_BUFFER), reads it fully, returns byte[]; null on IOException |
| S | `GetAssetManager` | `()Landroid/content/res/AssetManager;` | modules::AssetReader::InitAssetManager | returns the Activity AssetManager (source object for AAssetManager_fromJava) |
| S | `GetBrowserBookmarks` (sig) (also in SUtils) | `(Ljava/lang/String;II)Ljava/lang/String;` | PackageUtils::GetBrowserBookmarks | SUtils.GetBrowserBookmarks() -> returns getBrowserInfo(str, i, i2, true) |
| S | `GetBrowserHistory` (sig) (also in SUtils) | `(Ljava/lang/String;II)Ljava/lang/String;` | PackageUtils::GetBrowserHistory | SUtils.GetBrowserHistory() -> returns getBrowserInfo(str, i, i2, false) |
| S | `GetBuildBoard` (sig) | `()Ljava/lang/String;` | PackageUtils::GetBuildBoard | returns Build.BOARD |
| S | `GetCPUAbi` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | API 21+: up to 3 Build.SUPPORTED_ABIS joined with ";"; older: Build.CPU_ABI/CPU_ABI2 |
| S | `GetCPUSerial` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | first line containing "serial" in /proc/cpuinfo, else null |
| S | `GetCarrierAgent` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | TelephonyManager.getSimOperator() or "" |
| S | `GetCountry` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | SIM country ISO (validated) with locale fallback, upper-case |
| S | `GetDefaultIGP` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | returns the static IGP code string Device.k |
| S | `GetDeviceIMEI` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | TelephonyManager.getDeviceId() only if READ_PHONE_STATE is granted, else "" |
| S | `GetDeviceLanguage` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | Configuration.locale.getLanguage(), falls back to Locale.getDefault() |
| S | `GetDeviceManufacturer` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Device.getPhoneManufacturer() -> returns ValidateStringforURL(Build.MANUFACTURER) |
| S | `GetDeviceModel` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Device.getPhoneModel() -> returns ValidateStringforURL(Build.MODEL) |
| S | `GetDeviceSettingsCountryCode` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | locale.getCountry().toUpperCase() or "" |
| S | `GetDiskFreeSpace` (sig) | `(Ljava/lang/String;)J` | PackageUtils::GetDiskFreeSpace | StatFs free bytes of the volume containing the path (creates the directory if missing) |
| S | `GetDiskTotalSpace` (sig) | `()I` | PackageUtils::GetDiskTotalSpace | StatFs total size of the data partition, in MiB (int) |
| S | `GetElapsedRealtime` (sig) | `()J` | PackageUtils::GetElapsedRealtime | returns SystemClock.elapsedRealtime() |
| S | `GetFirmware` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Device.getDeviceFirmware() -> returns Build.VERSION.RELEASE |
| S | `GetGameName` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | package name minus the "com.gameloft.android." prefix (= "ANMP.GloftNOHM") |
| S | `GetGoogleAdId` (sig) | `()Ljava/lang/String;` | PackageUtils::GetGoogleAdId | Device.getGoogleAdId() -> returns TextUtils.isEmpty(F) ? "" : F |
| S | `GetGoogleAdIdStatus` (sig) | `()I` | PackageUtils::GetGoogleAdIdStatus | Device.getGoogleAdIdStatus() -> returns G |
| S | `GetHDIDFV` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | persisted HDIDFV UUID from DataSharing (decrypted); else System.load(libgenerator) + HDIDFV.getHDIDFV() and stores it; zero UUID if invalid |
| S | `GetInjectedIGP` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | IGP code injected by the installer (saved pref decoded with DefReader); "" if none |
| S | `GetInjectedSerialKey` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | SUtils.getInjectedSerialKey() -> returns "" |
| S | `GetLibSoPath` (sig) | `()Ljava/lang/String;` | PackageUtils::GetLibSoPath | returns DecRawso.GetInstance().a("NOVA") |
| S | `GetMacAddress` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | WifiManager.getConnectionInfo().getMacAddress() or "" |
| S | `GetMetaDataValue` (sig) | `(Ljava/lang/String;)Ljava/lang/String;` | PackageUtils::GetMetaDataValue | ApplicationInfo.metaData.get(key).toString() or "" |
| S | `GetPhoneDevice` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Device.getPhoneDevice() -> returns ValidateStringforURL(Build.DEVICE) |
| S | `GetPhoneProduct` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Device.getPhoneProduct() -> returns ValidateStringforURL(Build.PRODUCT) |
| S | `GetPreferenceBool` (sig) | `(Ljava/lang/String;Ljava/lang/String;Z)Z` | PackageUtils::ReadSharedPreferenceBool | SharedPreferences(file=arg2).getBoolean(key=arg1, default) |
| S | `GetPreferenceInt` (sig) | `(Ljava/lang/String;Ljava/lang/String;I)I` | PackageUtils::ReadSharedPreferenceInt | SharedPreferences(file=arg2).getInt(key=arg1, default) |
| S | `GetPreferenceLong` (sig) | `(Ljava/lang/String;Ljava/lang/String;J)J` | PackageUtils::ReadSharedPreferenceLong | SharedPreferences(file=arg2).getLong(key=arg1, default) |
| S | `GetPreferenceString` (sig) | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` | PackageUtils::ReadSharedPreferenceString | SharedPreferences(file=arg2).getString(key=arg1, default) |
| S | `GetSerial` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Device.getSerial() -> returns "" |
| S | `GetSimIsoCountryCode` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | TelephonyManager.getSimCountryIso().toUpperCase() or "" |
| S | `GetUserAgent` | `()Ljava/lang/String;` | Internal::RefreshCachedVars | Device.getUserAgent() -> returns s |
| S | `GetVKeyboardText` | `()Ljava/lang/String;` | modules::VirtualKeyboard::GetKeyboardText | VirtualKeyboard.GetVirtualKeyboardText() -> returns getInstance() != null ? getInstance().getText().toString() : "" |
| S | `GetXDpi` | `()F` | Internal::SetWindow | DisplayMetrics.xdpi of the default display |
| S | `GetYDpi` | `()F` | Internal::SetWindow | DisplayMetrics.ydpi of the default display |
| S | `HideKeyboard` | `()V` | modules::VirtualKeyboard::HideKeyboard | VirtualKeyboard.HideKeyboard() -> calls getInstance().a() |
| S | `HttpExecuteAsync` (sig) | `(Ljava/lang/String;)V` | PackageUtils::HttpExecuteAsync | AsyncTask: DefaultHttpClient().execute(new HttpGet(url)); result ignored |
| S | `IsKeyboardVisible` | `()Z` | modules::VirtualKeyboard::IsKeyboardVisible | VirtualKeyboard.isKeyboardVisible() -> returns c.findFocus() == getInstance() |
| S | `IsMusicActive` (sig) | `()Z` | PackageUtils::IsMusicActive | returns ((AudioManager) SUtils.getApplicationContext().getSystemService("audio")).isMusicActive() |
| S | `IsSimReady` (sig) | `()Z` | PackageUtils::IsSimReady | Device.isSimReady() -> 3 stmts; uses TelephonyManager |
| S | `IsVideoCompleted` (sig) | `()I` | PackageUtils::IsVideoCompleted | returns 1 |
| S | `LaunchBrowser` (sig) | `(Ljava/lang/String;)Z` | PackageUtils::LaunchBrowser | startActivity(ACTION_VIEW, Uri.parse(url)); returns false on empty url or exception |
| S | `LaunchVideoPlayer` (sig) | `(Ljava/lang/String;)Z` | PackageUtils::LaunchVideoPlayer | returns false |
| S | `LaunchWifiSettings` (sig) | `()Z` | PackageUtils::LaunchWifiSettings | startActivity(android.settings.WIFI_SETTINGS); returns success |
| S | `MinimizeApplication` (sig) | `()V` | PackageUtils::MinimizeApplication | MainActivity.m(): runOnUiThread(moveTaskToBack(true)) |
| S | `RemoveDirectoryRecursively` (sig) | `(Ljava/lang/String;)Z` | PackageUtils::RemoveDirectoryRecursively | recursive delete, returns success |
| S | `RemovePreference` (sig) | `(Ljava/lang/String;Ljava/lang/String;)V` | PackageUtils::RemoveSharedPreference | removes key from the named SharedPreferences file |
| S | `RetrieveDataPath` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | getExternalFilesDir(null) path when external storage is flagged usable (setIsStorageEmulated), else getFilesDir() |
| S | `RetrieveNativeLibraryPath` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | ApplicationInfo.nativeLibraryDir |
| S | `RetrieveObbPath` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | returns "NA" |
| S | `RetrieveSDCardPath` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | returns Environment.getExternalStorageDirectory().getAbsolutePath() |
| S | `RetrieveSavePath` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Context.getFilesDir().getAbsolutePath() |
| S | `RetrieveTempPath` | `()Ljava/lang/String;` | Internal::InitializeCachedVars | Context.getCacheDir().getAbsolutePath() |
| S | `SavePreferenceBool` (sig) | `(Ljava/lang/String;Ljava/lang/String;Z)V` | PackageUtils::SavePreferenceBool | SharedPreferences(file=arg2).edit().putBoolean(key=arg1,value).commit() |
| S | `SavePreferenceInt` (sig) | `(Ljava/lang/String;Ljava/lang/String;I)V` | PackageUtils::SavePreferenceInt | SharedPreferences(file=arg2).edit().putInt(key=arg1,value).commit() |
| S | `SavePreferenceLong` (sig) | `(Ljava/lang/String;Ljava/lang/String;J)V` | PackageUtils::SavePreferenceLong | SharedPreferences(file=arg2).edit().putLong(key=arg1,value).commit() |
| S | `SavePreferenceString` (sig) | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | PackageUtils::SavePreferenceString | SharedPreferences(file=arg2).edit().putString(key=arg1,value).commit() |
| S | `SetKeepScreenOn` (sig) | `(Z)V` | PackageUtils::SetKeepScreenOn | MainActivity.c(boolean): SurfaceView.setKeepScreenOn on the UI thread |
| S | `SetOrientation` (sig) | `(Z)V` | PackageUtils::SetOrientationState | MainActivity.b(boolean): setRequestedOrientation(landscape sensor variants) |
| S | `SetVKeyboardText` | `(Ljava/lang/String;)V` | modules::VirtualKeyboard::SetKeyboardText | VirtualKeyboard.SetKeyboardText() -> 10 stmts; uses posts to UI thread, background thread |
| S | `ShowCannotGoBack` (sig) | `()V` | PackageUtils::ShowCannotGoBack | shows a Toast ("cannot go back", 300 ms) from a helper thread on the UI thread |
| S | `ShowGameDialog` (sig) | `(Ljava/lang/String;Ljava/lang/String;Z)Z` | PackageUtils::ShowDialog | AlertDialog on the UI thread (title, message, 1 or 2 buttons); blocks the caller until the user answers; returns the choice |
| S | `ShowKeyboard` | `(Ljava/lang/String;IIII)V` | modules::VirtualKeyboard::ShowKeyboard | VirtualKeyboard: shows an EditText overlay with initial text/position; result goes to JNIBridge.NativeSendKeyboardData |
| S | `ShowToastMessage` (sig) (also in SUtils) | `(Ljava/lang/String;I)V` | PackageUtils::ShowToastMessage | Toast.makeText(text, duration) on the UI thread |
| S | `getVersionCodes` (sig) | `()I` | PackageUtils::GetVersionCodes | returns Build.VERSION.SDK_INT |
| S | `initBatteryInfo` | `()V` | Internal::RefreshCachedVars | UtilsBatteryStateReceiver.SetInitialBatteryState(activity) -> calls JNIBridge.SetBatteryInfo |
| S | `initCheckConnectionType` | `()I` | Internal::RefreshCachedVars | UtilsNetworkStateReceiver.CheckConnectionType(ctx): returns connection class code (0 = none) |
| S | `retrieveBarrels` (sig) (also in SUtils) | `()[I` | PackageUtils::IsAppEnc | returns int[4] = hashCode() of up to 4 APK signing signatures (anti-tamper input of PackageUtils::IsAppEnc); zeros if unavailable |

#### `com/gameloft/GLSocialLib/GameAPI/GameAPIAndroidGLSocialLib` (22)

Port stance: Facebook / Google Play Games / social. Stub: report "not logged in", never call the completion callbacks except failure.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `ConnectToService` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 15 stmts; uses posts to UI thread, android.os.Build, background thread |
| S | `DisconnectFromService` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 14 stmts; uses posts to UI thread, background thread |
| S | `GetFriends` (sig) | `(Ljava/lang/String;)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 49 stmts; ConsoleAndroidGLSocialLib.Log_Debug("GetFriends - pageToken = " + str) |
| S | `GetFriendsData` (sig) | `(ZZII)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `GetGamerId` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; uses Google Play Games |
| S | `GetPlayerAvatar` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 25 stmts; uses Google Play Games |
| S | `GetPlayerName` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 20 stmts; uses Google Play Games |
| S | `GetUid` (sig) (also in FacebookAndroidGLSocialLib) | `()Ljava/lang/String;` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial, facebookAndroidGLSocialLib_init | 8 stmts; uses Google Play Games |
| S | `GetUserData` (sig) | `(Ljava/lang/String;)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 45 stmts; try { |
| S | `IncrementAchievement` (sig) | `(Ljava/lang/String;I)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `InitGameAPI` | `(ZZ)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 15 stmts; uses posts to UI thread, Google Play Games, background thread |
| S | `IsLoggedIn` (sig) (also in FacebookAndroidGLSocialLib) | `()Z` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial, facebookAndroidGLSocialLib_init | 2 stmts; GameHelper gameHelper = mHelper; |
| S | `PostPhotoToWall` (sig) | `(Ljava/lang/String;Ljava/lang/String;)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 18 stmts; uses starts an Activity/Intent, posts to UI thread, background thread |
| S | `PostToWall` (sig) | `(Ljava/lang/String;Ljava/lang/String;)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 17 stmts; uses starts an Activity/Intent, posts to UI thread, background thread |
| S | `ResetAchievements` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `ShowAchievements` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `ShowAllLeadearboards` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `ShowLeadearboardWithId` (sig) | `(Ljava/lang/String;)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `SubmitScore` (sig) | `(Ljava/lang/String;I)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `TryAutoConnectToService` (sig) | `()V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 8 stmts; uses posts to UI thread, android.os.Build, background thread |
| S | `UnlockAchievement` (sig) | `(Ljava/lang/String;)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 4 stmts; try { |
| S | `sendGameRequestToFriends` (sig) | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | Java_com_gameloft_GLSocialLib_GameAPI_GameAPIAndroidGLSocial | 30 stmts; uses starts an Activity/Intent, posts to UI thread, background thread |

#### `com/gameloft/glads/AndroidWebView` (21)

Port stance: Ads. Stub (no ads).

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| I | `<init>` | `(J)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | no method body found in decompiled source |
| I | `AddButton` | `([BIIIII)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 18 stmts; uses WebView, background thread |
| I | `BringToFront` | `()V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `ClearCacheAndCookies` | `()V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 17 stmts; uses WebView, android.os.Build, background thread |
| I | `DismissFullScreen` | `()V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `EnableModalWebView` | `()V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | calls this.openWithModalWebview = true |
| I | `EvaluateJavaScript` | `(Ljava/lang/String;I)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `LoadHTMLFile` | `(Ljava/lang/String;)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `LoadHTMLString` | `(Ljava/lang/String;Ljava/lang/String;)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `LoadURL` | `(Ljava/lang/String;)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `PresentFullScreen` | `(I)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 11 stmts; uses WebView, android.os.Build, background thread |
| I | `Release` | `()V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 17 stmts; uses WebView, background thread |
| I | `RemoveButton` | `(I)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `RunJavaScript` | `(Ljava/lang/String;)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `SetBounds` | `(IIII)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `SetButtonBounds` | `(IIIII)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `SetInitialScale` | `(I)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `SetOrientation` | `(I)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 4 stmts; if (AndroidDevice.GetOrientation() != i) { |
| I | `SetUseWideViewPort` | `(Z)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `SetUserAgent` | `(Ljava/lang/String;)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |
| I | `SetVisible` | `(Z)V` | AndroidWebView::AndroidWebView() via JAdapter::LoadMethod (reached from GLAd::RenderWebPage) | 5 stmts; uses WebView, background thread |

#### `com/gameloft/GLSocialLib/facebook/FacebookAndroidGLSocialLib` (20)

Port stance: Facebook / Google Play Games / social. Stub: report "not logged in", never call the completion callbacks except failure.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `CanPresentShareDialog` (sig) | `()Z` | facebookAndroidGLSocialLib_init | 2 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: CanPr |
| S | `Init` | `()V` | facebookAndroidGLSocialLib_init | 4 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Init" |
| S | `Login` (sig) | `(Ljava/lang/String;Z)V` | facebookAndroidGLSocialLib_init | 2 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Login |
| S | `Logout` (sig) | `()V` | facebookAndroidGLSocialLib_init | 7 stmts; uses posts to UI thread, Facebook LoginManager, background thread |
| S | `PostOpenGraphAction` (sig) | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | facebookAndroidGLSocialLib_init | 5 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: PostO |
| S | `PostPhotoToWallWithoutDialog` (sig) | `([BLjava/lang/String;)V` | facebookAndroidGLSocialLib_init | 8 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: PostP |
| S | `PostToWall` (sig) | `(Ljava/lang/String;Z)V` | facebookAndroidGLSocialLib_init | 3 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: PostT |
| S | `PostToWallWithoutDialog` (sig) | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | facebookAndroidGLSocialLib_init | 18 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: PostT |
| S | `RequestAppDetails` (sig) | `()V` | facebookAndroidGLSocialLib_init | 5 stmts; uses Facebook SDK |
| S | `RequestAppId` (sig) | `()V` | facebookAndroidGLSocialLib_init | 5 stmts; uses Facebook SDK |
| S | `RequestCoverImage` (sig) | `()V` | facebookAndroidGLSocialLib_init | 4 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `RequestFriendLikes` (sig) | `(Ljava/lang/String;)V` | facebookAndroidGLSocialLib_init | 5 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `RequestFriendsData` (sig) | `(III)V` | facebookAndroidGLSocialLib_init | 7 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `RequestFriendsData` (sig) | `(Ljava/lang/String;I)V` | facebookAndroidGLSocialLib_init | 6 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `RequestFriendsIDs` (sig) | `(I)V` | facebookAndroidGLSocialLib_init | 5 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `RequestFriendsName` (sig) | `(Ljava/lang/String;)V` | facebookAndroidGLSocialLib_init | 5 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `RequestName` (sig) | `()V` | facebookAndroidGLSocialLib_init | 4 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `RequestPicture` (sig) | `(Ljava/lang/String;)V` | facebookAndroidGLSocialLib_init | 4 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Reque |
| S | `SendGameRequest` (sig) | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | facebookAndroidGLSocialLib_init | 7 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: SendG |
| S | `UploadVideo` (sig) | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | facebookAndroidGLSocialLib_init | 21 stmts; ConsoleAndroidGLSocialLib.Log_Debug("FacebookAndroidGLSocialLib: Uploa |

#### `android/os/Bundle` (13)

Port stance: Android framework.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| I | `<init>` | `()V` | ABundle::SetJniVars | Android/JDK framework call |
| I | `clear` | `()V` | ABundle::SetJniVars | Android/JDK framework call |
| I | `containsKey` | `(Ljava/lang/String;)Z` | ABundle::SetJniVars | Android/JDK framework call |
| I | `getBoolean` | `(Ljava/lang/String;)Z` | ABundle::SetJniVars | Android/JDK framework call |
| I | `getByteArray` | `(Ljava/lang/String;)[B` | ABundle::SetJniVars | Android/JDK framework call |
| I | `getInt` | `(Ljava/lang/String;)I` | ABundle::SetJniVars | Android/JDK framework call |
| I | `getLong` | `(Ljava/lang/String;)J` | ABundle::SetJniVars | Android/JDK framework call |
| I | `getString` | `(Ljava/lang/String;)Ljava/lang/String;` | ABundle::SetJniVars | Android/JDK framework call |
| I | `putBoolean` | `(Ljava/lang/String;Z)V` | ABundle::SetJniVars | Android/JDK framework call |
| I | `putByteArray` | `(Ljava/lang/String;[B)V` | ABundle::SetJniVars | Android/JDK framework call |
| I | `putInt` | `(Ljava/lang/String;I)V` | ABundle::SetJniVars | Android/JDK framework call |
| I | `putLong` | `(Ljava/lang/String;J)V` | ABundle::SetJniVars | Android/JDK framework call |
| I | `putString` | `(Ljava/lang/String;Ljava/lang/String;)V` | ABundle::SetJniVars | Android/JDK framework call |

#### `com/gameloft/adsmanager/IronSourceAds` (11)

Port stance: Ads. Stub (no ads).

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `ChangeBanner` | `(III)V` | adslib::IronSourcePlatformManager::Configure | BannerIronSource.ChangeBanner() -> 8 stmts; uses IronSource SDK |
| S | `CheckIncentivizedAdAvailable` | `(Ljava/lang/String;)Z` | adslib::IronSourcePlatformManager::Configure | IncentivizedIronSource.CheckIncentivizedAdAvailable() -> 4 stmts; uses IronSource SDK |
| S | `CheckOfferwallAdAvailable` | `(Ljava/lang/String;)Z` | adslib::IronSourcePlatformManager::Configure | OfferwallIronSource.CheckOfferwallAdAvailable() -> 4 stmts; uses IronSource SDK |
| S | `Configure` | `(Ljava/lang/String;Ljava/lang/String;ZZ)V` | adslib::IronSourcePlatformManager::Configure | 23 stmts; uses IronSource SDK |
| S | `HideBanner` | `()V` | adslib::IronSourcePlatformManager::Configure | BannerIronSource.HideBanner() -> 13 stmts; uses IronSource SDK |
| S | `LoadBanner` | `(Ljava/lang/String;Ljava/lang/String;)V` | adslib::IronSourcePlatformManager::Configure | BannerIronSource.LoadBanner() -> 26 stmts; uses IronSource SDK |
| S | `LoadInterstitial` | `(Ljava/lang/String;Ljava/lang/String;)V` | adslib::IronSourcePlatformManager::Configure | InterstitialIronSource.LoadInterstitial() -> 5 stmts; uses IronSource SDK |
| S | `ShowBanner` | `(Ljava/lang/String;Ljava/lang/String;III)V` | adslib::IronSourcePlatformManager::Configure | 2 stmts; uses IronSource SDK |
| S | `ShowIncentivized` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | adslib::IronSourcePlatformManager::Configure | IncentivizedIronSource.ShowIncentivized() -> 19 stmts; uses IronSource SDK |
| S | `ShowInterstitial` | `(Ljava/lang/String;Ljava/lang/String;)V` | adslib::IronSourcePlatformManager::Configure | InterstitialIronSource.ShowInterstitial() -> 5 stmts; uses IronSource SDK |
| S | `ShowOfferwall` | `(Ljava/lang/String;Ljava/lang/String;)V` | adslib::IronSourcePlatformManager::Configure | OfferwallIronSource.ShowOfferwall() -> 13 stmts; uses IronSource SDK |

#### `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/LogoViewPlugin` (10)

Port stance: Optional UI: loading-progress callbacks (extraction, texture generation). Host may draw a progress bar or ignore; `IsCacheWarningVisible` should return false.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `IsCacheWarningVisible` (sig) | `()Z` | PackageUtils::IsCacheWarningVisible | delegates to d.g() (obfuscated/not decompiled) |
| S | `RequestCacheWarningAfterLogo` (sig) | `()V` | PackageUtils::RequestCacheWarningAfterLogo | delegates to d.c() (obfuscated/not decompiled) |
| S | `RequestHeadphoneWarningAfterLogo` (sig) | `()V` | PackageUtils::RequestHeadphoneWarningAfterLogo | delegates to d.d() (obfuscated/not decompiled) |
| S | `RequestHideAllLogoDisplays` (sig) | `()V` | PackageUtils::RequestHideAllLogoDisplays | delegates to d.e() (obfuscated/not decompiled) |
| S | `RequestHideAllLogoDisplaysExceptCacheWarning` (sig) | `()V` | PackageUtils::RequestHideAllLogoDisplaysExceptCacheWarning | delegates to d.f() (obfuscated/not decompiled) |
| S | `SetWarningLanguage` (sig) | `(Ljava/lang/String;)V` | PackageUtils::SetWarningLanguage | 5 stmts; uses posts to UI thread, background thread |
| S | `UpdateExtractionProgress` (sig) | `(II)V` | PackageUtils::UpdateExtractionProgress | 5 stmts; uses posts to UI thread, background thread |
| S | `UpdateLoadingProgressAfterTexGen` (sig) | `(II)V` | PackageUtils::UpdateLoadingProgressAfterTexGen | 5 stmts; uses posts to UI thread, background thread |
| S | `UpdateLoadingProgressBeforeTexGen` (sig) | `(II)V` | PackageUtils::UpdateLoadingProgressBeforeTexGen | 5 stmts; uses posts to UI thread, background thread |
| S | `UpdateTexGenProgress` (sig) | `(II)V` | PackageUtils::UpdateTexGenProgress | 5 stmts; uses posts to UI thread, background thread |

#### `com/gameloft/popupslib/PopUpsBridgeClass` (10)

Port stance: Gameloft pop-ups web view. Stub.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `CallJavascript` | `(Ljava/lang/String;)V` | PopUpsLib::PopUpsBridgeClass::SetJniVars | PopUpsManager.GetInstance() -> 3 stmts; if (e == null) { |
| S | `DestroyPopUps` | `()V` | PopUpsLib::PopUpsBridgeClass::SetJniVars | 5 stmts; uses posts to UI thread, background thread |
| S | `EvaluateJavascript` | `(Ljava/lang/String;J)V` | PopUpsLib::PopUpsBridgeClass::SetJniVars | PopUpsManager.GetInstance() -> 3 stmts; if (e == null) { |
| S | `HidePopUpsView` | `()V` | PopUpsLib::PopUpsBridgeClass::SetJniVars | 7 stmts; uses posts to UI thread, background thread |
| S | `InitPopUps` | `()Z` | PopUpsLib::PopUpsBridgeClass::SetJniVars | PopUpsManager.GetInstance() -> 3 stmts; if (e == null) { |
| S | `OpenBrowser` | `(Ljava/lang/String;)V` | PopUpsLib::PopUpsBridgeClass::SetJniVars | 2 stmts; Log(1, "PopUpsAndroid", "E:/Work/Project/Nova/trunk/extern/popupslib/s |
| S | `PopUpsViewState` | `()I` | PopUpsLib::PopUpsBridgeClass::SetJniVars | PopUpsManager.GetInstance() -> 3 stmts; if (e == null) { |
| S | `SetBackgroundColor` | `(IIII)V` | PopUpsLib::PopUpsBridgeClass::SetJniVars | PopUpsManager.GetInstance() -> 3 stmts; if (e == null) { |
| S | `SetPopUpsViewSize` | `(IIII)V` | PopUpsLib::PopUpsBridgeClass::SetJniVars | PopUpsManager.GetInstance() -> 3 stmts; if (e == null) { |
| S | `ShowPopUpsView` | `(Ljava/lang/String;)I` | PopUpsLib::PopUpsBridgeClass::SetJniVars | 14 stmts; uses posts to UI thread, background thread |

#### `com/gameloft/android/ANMP/GloftNOHM/PushNotification/SimplifiedAndroidUtils` (7)

Port stance: Push notifications / Firebase. Stub.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `DeleteMessageGroup` | `(I)I` | modules::SimplifiedPN::SetJniVars | 28 stmts; List<String> listGetPNMessageIdForType; |
| S | `GetDeviceToken` | `(I)I` | modules::SimplifiedPN::SetJniVars | 8 stmts; if (isEmptyOrNull(GetRegistrationID())) { |
| S | `IsAppLaunchedFromPN` | `()Ljava/lang/String;` | modules::SimplifiedPN::SetJniVars | returns getMessagePayload(w) |
| S | `IsEnable` | `()Z` | modules::SimplifiedPN::SetJniVars | Prefs.isEnabled() -> returns get(context).getBoolean(a, true) |
| S | `SendMessage` | `(Landroid/os/Bundle;Ljava/lang/String;I)I` | modules::SimplifiedPN::SetJniVars | 22 stmts; String strSetAlarm; |
| S | `SetEnable` | `(Z)V` | modules::SimplifiedPN::SetJniVars | calls SetEnable(SUtils.getApplicationContext(), z2) |
| S | `ShowAppDetailsSettings` | `()V` | modules::SimplifiedPN::SetJniVars | 7 stmts; uses starts an Activity/Intent, android.os.Build |

#### `com/gameloft/ingamebrowser/InGameBrowser` (7)

Port stance: In-game browser / IGP promotion. Stub (or open system browser for OpenExternalBrowser).

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `QuitIGB` | `()V` | InGameBrowser::InGameBrowser::QuitIGB | 16 stmts; uses background thread |
| S | `SetGameLanguage` | `(Ljava/lang/String;)V` | InGameBrowser::InGameBrowser::SetGameLanguage | 7 stmts; if (str.equalsIgnoreCase("ES-LT")) { |
| S | `getLastNewsIndex` | `()Ljava/lang/String;` | InGameBrowser::InGameBrowser::getLastNewsIndex | 4 stmts; uses SharedPreferences |
| S | `openBrowser` | `(Ljava/lang/String;)V` | InGameBrowser::InGameBrowser::OpenExternalBrowser | calls openIntent(str) |
| S | `postData` | `(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` | InGameBrowser::InGameBrowser::postData | 21 stmts; uses HTTP request |
| S | `saveLastNewsIndex` | `(Ljava/lang/String;)V` | InGameBrowser::InGameBrowser::saveLastNewsIndex | 5 stmts; uses SharedPreferences |
| S | `showInGameBrowserWithParams` | `(Ljava/lang/String;Ljava/lang/String;ZI)V` | InGameBrowser::InGameBrowser::OpenWithParams | 10 stmts; uses starts an Activity/Intent |

#### `(class not resolved)` (6)

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| I | `<init>` | `(J)V` | IGPLib::JAdapter::CreateInstance, JAdapter::CreateInstance | - |
| I | `getMessage` | `()Ljava/lang/String;` | PackageUtils::Jni_CheckForExceptions | - |
| I | `getName` | `()Ljava/lang/String;` | PackageUtils::Jni_CheckForExceptions | - |
| S | `IsMemoryWarningActive` | `()I` | PackageUtils::GetLastMemoryWarning | - |
| S | `getPreference` | `(Landroid/os/Bundle;)Landroid/os/Bundle;` | PackageUtils::ReadSharedPreference | - |
| S | `setThreadPriority` | `(I)V` | glf::Thread::Impl::SetPriority | - |

#### `com/gameloft/adsmanager/UnityAdsManager` (4)

Port stance: Ads. Stub (no ads).

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `CheckAdAvailable` | `(Ljava/lang/String;I)Z` | adslib::UnityAdsPlatformManager::UnityAdsPlatformManager | 4 stmts; uses Unity Ads SDK |
| S | `Configure` | `(Ljava/lang/String;Ljava/lang/String;ZZZ)V` | adslib::UnityAdsPlatformManager::UnityAdsPlatformManager | 16 stmts; uses Unity Ads SDK |
| S | `ShowIncentivized` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | adslib::UnityAdsPlatformManager::UnityAdsPlatformManager | 26 stmts; uses Unity Ads SDK |
| S | `ShowInterstitial` | `(Ljava/lang/String;Ljava/lang/String;)V` | adslib::UnityAdsPlatformManager::UnityAdsPlatformManager | 21 stmts; uses Unity Ads SDK |

#### `com/gameloft/android/ANMP/GloftNOHM/DataSharing` (4)

Port stance: Cross-game shared key/value store (glshare permission). Implement as a local JSON/INI file.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `deleteSharedValue` | `(Ljava/lang/String;)V` | modules::DataSharing::SetJniVars | 16 stmts; uses background thread |
| S | `getSharedValue` | `(Ljava/lang/String;)Ljava/lang/String;` | modules::DataSharing::SetJniVars | 17 stmts; uses background thread |
| S | `isSharedValue` | `(Ljava/lang/String;)Z` | modules::DataSharing::SetJniVars | 3 stmts; LazyInit(); |
| S | `setSharedValue` | `(Ljava/lang/String;Ljava/lang/String;)V` | modules::DataSharing::SetJniVars | 15 stmts; uses background thread |

#### `com/gameloft/android/ANMP/GloftNOHM/GLUtils/SUtils` (4)

Port stance: Utility class behind AndroidUtils.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `GenerateUUID` (sig) | `()Ljava/lang/String;` | PackageUtils::GenerateUUID | UUID.randomUUID().toString() |
| S | `GetPlainText` (sig) | `(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;` | PackageUtils::GetPlainText | DefReader.getInstance().a(a,b): decodes an obfuscated string with a key |
| S | `ShowOfflineZoroDialog` (sig) | `()V` | PackageUtils::ShowOfflineZoroDialog | 13 stmts; uses posts to UI thread, AlertDialog, background thread |
| S | `getGLUID` (sig) | `(Ljava/lang/String;)[I` | PackageUtils::getGLUID | MD5(arg + package prefix) split into 4 ints |

#### `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/PermissionPlugin` (4)

Port stance: Return true for every permission, ignore requests.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `isCameraPermissionEnabled` (sig) | `()Z` | modules::PermissionManager::IsEnabled | AndroidUtils.isPermissionEnabled() -> returns ContextCompat.checkSelfPermission(SUtils.getApplicationContext(), str) == 0 |
| S | `requestCameraPermission` | `(Z)I` | AndroidDevice::RequestCameraPermission | returns requestPermission("android.permission.CAMERA", R.string.PERMISSION_NOTIFICATION_TITLE, R.string.PERMIS |
| S | `requestCameraPermission` (sig) | `(Z)I` | modules::PermissionManager::Request | returns requestPermission("android.permission.CAMERA", R.string.PERMISSION_NOTIFICATION_TITLE, R.string.PERMIS |
| S | `requestStoragePermission` | `(Z)I` | AndroidDevice::RequestStoragePermission | returns requestPermission("android.permission.WRITE_EXTERNAL_STORAGE", R.string.PERMISSION_NOTIFICATION_TITLE, |

#### `com/gameloft/adsmanager/JavaUtils` (3)

Port stance: Ads. Stub (no ads).

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `DipToPx` | `(I)I` | adslib::javautils::InitJavaMethods | returns (i2 * r) / s |
| S | `GetBannerHeight` | `()I` | adslib::javautils::InitJavaMethods | 3 stmts; if (f == 0) { |
| S | `GetBannerWidth` | `()I` | adslib::javautils::InitJavaMethods | 3 stmts; if (e == 0) { |

#### `com/gameloft/android/ANMP/GloftNOHM/GoogleFirebase/GoogleFirebase` (3)

Port stance: Push notifications / Firebase. Stub.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `imAliveFirebase` | `()V` | modules::GoogleFirebase::SetJniVars | calls FirebaseAnalytics firebaseAnalytics = c |
| S | `resetAnalyticsDataFirebase` | `()V` | modules::GoogleFirebase::SetJniVars | 4 stmts; uses Firebase |
| S | `sendDummyEventFirebase` | `()V` | modules::GoogleFirebase::SetJniVars | 7 stmts; if (c == null) { |

#### `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/GoogleInvitePlugin` (3)

Port stance: Google App Invites. Stub.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `convertInvitation` | `(Ljava/lang/String;)V` | modules::AppInvite::ConvertInvitation | class not in decompiled sources (framework/library class) |
| S | `launchInvitationIntent` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | modules::AppInvite::Send | class not in decompiled sources (framework/library class) |
| S | `launchInvitationIntentHTML` | `(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V` | modules::AppInvite::Send | class not in decompiled sources (framework/library class) |

#### `com/gameloft/android/ANMP/GloftNOHM/AppDetection` (2)

Port stance: Installed-app queries: return empty / false.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `getInstalledPackages` | `()[Ljava/lang/String;` | AppDetection::getInstalledPackages | 4 stmts; if (b == null) { |
| S | `isAppInstalled` | `(Ljava/lang/String;)Z` | AppDetection::isAppInstalled | 3 stmts; if (b == null) { |

#### `com/gameloft/android/ANMP/GloftNOHM/GLUtils/CutoutHelper` (2)

Port stance: Return no cutout (DeviceHasCutout=false, insets 0).

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `CutoutSafeInset` (sig) | `(I)I` | PackageUtils::GetCutoutSafeInset | 16 stmts; MainActivity mainActivity = MainActivity.l; |
| S | `DeviceHasCutout` (sig) | `()Z` | PackageUtils::DeviceHasCutout | 3 stmts; if (MainActivity.l.c != null) { |

#### `com/gameloft/android/ANMP/GloftNOHM/GLUtils/controller/NativeBridgeHIDControllers` (2)

Port stance: Class missing in this APK; lookup fails, native code must tolerate null.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `NativeListenerRegistered` | `(I)V` | modules::HidController::SetJniVars | class not in decompiled sources (framework/library class) |
| S | `NativeListenerUnRegistered` | `()V` | modules::HidController::SetJniVars | class not in decompiled sources (framework/library class) |

#### `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/ClipboardPlugin` (2)

Port stance: Clipboard: map to the Windows clipboard.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `CopyToClipboard` | `(Ljava/lang/String;)V` | modules::Clipboard::CopyToClipboard | 8 stmts; uses posts to UI thread, Android system service, Clipboard, background thread |
| S | `PasteFromClipboard` | `()Ljava/lang/String;` | modules::Clipboard::PasteFromClipboard | 4 stmts; uses Android system service, Clipboard |

#### `com/gameloft/GLSocialLib/PlatformAndroid` (1)

Port stance: Facebook / Google Play Games / social. Stub: report "not logged in", never call the completion callbacks except failure.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `GetDataFromURL` | `(Ljava/lang/String;)[B` | sociallib::urlToImageByteBuffer | 55 stmts; uses background thread, HTTP request |

#### `com/gameloft/android/ANMP/GloftNOHM/PackageUtils/EmulatorDetector` (1)

Port stance: Return false.

| K | Method / field | Descriptor | Native function | Java side |
|---|---|---|---|---|
| S | `IsGameRunOnEmulator` (sig) | `()Z` | PackageUtils::IsRunOnEmulator | 6 stmts; Activity activity = SUtils.getActivity(); |

#### String-keyed calls through `JAdapter` (class `com/gameloft/glads/*`, `com/gameloft/igp/*`)

The Gameloft ads library (`GLAds`, `gladsv2`, `IGPLib`) does not call `GetMethodID` with literal strings at each site. It wraps Java in a reflection-like helper: `JAdapter::SetBaseClass(name)`, `JAdapter::CreateInstance(ptr)`, then `JAdapter::Call/CallStatic/Bool_Call/String_CallStatic/...(methodName, args...)`; the descriptor is derived from the argument types at run time and cached (`LoadMethod`/`LoadStaticMethod`). `AndroidWebView` is the exception: its 21 members (including the constructor `<init>(J)V`) were recovered with exact descriptors by running its constructor, see the `com/gameloft/glads/AndroidWebView` table above. For `AndroidDevice`, `AvidSDK` and `InGamePromotionPlugin` the method names below are the literal strings referenced by the native wrapper classes, matched against methods of the Java class they wrap; descriptors are those of the Java methods. All of this is ad/web-view infrastructure and can be stubbed wholesale.

AndroidDevice (native class `AndroidDevice`, static calls) - `com/gameloft/glads/AndroidDevice`

| Method | Descriptor | Java side |
|---|---|---|
| `AddCalendarEvent` | `(Ljava/lang/String;)Z` | 12 stmts; uses starts an Activity/Intent, Firebase |
| `CreateDirectory` | `(Ljava/lang/String;)V` | 4 stmts; File file = new File(str); |
| `DeleteDirectory` | `(Ljava/lang/String;)V` | calls RemoveDirectoryRecursive(new File(str)) |
| `GetCachePath` | `()Ljava/lang/String;` | delegates to Utils.GetContext() (obfuscated/not decompiled) |
| `GetChildDirectories` | `(Ljava/lang/String;)[Ljava/lang/String;` | 8 stmts; String[] list = new File(str).list(); |
| `GetConnectionType` | `()Ljava/lang/String;` | 3 stmts; if (HasInternetConnection()) { |
| `GetDIPScaleFactor` | `()F` | returns 160.0f / GetDisplayMetrics().densityDpi |
| `GetGoogleAdID` | `()Ljava/lang/String;` | Device.getGoogleAdIdStatus() -> returns G |
| `GetGoogleOptOut` | `()Ljava/lang/String;` | returns "" + Device.getGoogleAdIdStatus() |
| `GetLogicalDensity` | `()F` | 4 stmts; uses android.os.Build, DisplayMetrics |
| `GetNetworkCountryIso` | `()Ljava/lang/String;` | 6 stmts; if (!AndroidUtils.IsSimReady()) { |
| `GetOperatorMCC` | `()Ljava/lang/String;` | 6 stmts; if (!AndroidUtils.IsSimReady()) { |
| `GetOperatorMNC` | `()Ljava/lang/String;` | 6 stmts; if (!AndroidUtils.IsSimReady()) { |
| `GetOrientation` | `()I` | 35 stmts; uses Android system service, DisplayMetrics |
| `GetScreenXDPI` | `()F` | returns GetDisplayMetrics().xdpi |
| `GetScreenYDPI` | `()F` | returns GetDisplayMetrics().ydpi |
| `GetSoundVolume` | `()I` | 5 stmts; uses Android system service, AudioManager |
| `GetSystemVersion` | `()Ljava/lang/String;` | returns Build.VERSION.RELEASE |
| `GetTotalMemory` | `()F` | 5 stmts; uses Android system service, android.os.Build |
| `GetUsedMemory` | `()F` | 5 stmts; uses Android system service, android.os.Build |
| `GetUserAgent` | `()Ljava/lang/String;` | 12 stmts; uses WebView, background thread |
| `HasInternetConnection` | `()Z` | 5 stmts; uses Android system service, ConnectivityManager |
| `IsDeviceRooted` | `()Z` | 7 stmts; uses android.os.Build |
| `LaunchBrowser` | `(Ljava/lang/String;)V` | 3 stmts; uses starts an Activity/Intent |
| `LaunchModalWebView` | `(Ljava/lang/String;)V` | calls LaunchModalWebView(str, null) |
| `LaunchModalWebView` | `(Ljava/lang/String;Landroid/os/Message;)V` | 7 stmts; uses WebView |
| `PauseUserMusic` | `()V` | 3 stmts; Intent intent = new Intent("com.android.music.musicservicecommand"); |
| `RemoveFile` | `(Ljava/lang/String;)V` | 8 stmts; uses deletes files |
| `SaveScreenshot` | `(Ljava/lang/String;)V` | 11 stmts; uses Environment |

AvidSDK - `com/gameloft/glads/AvidSDK`

| Method | Descriptor | Java side |
|---|---|---|
| `EndSession` | `()V` | 6 stmts; uses background thread |
| `OnCreateWebView` | `(Ljava/lang/Object;)V` | 7 stmts; uses WebView, background thread |
| `StartSession` | `(Ljava/lang/String;Z)V` | 9 stmts; uses background thread |

InGamePromotionPlugin - `com/gameloft/igp/InGamePromotionPlugin`

| Method | Descriptor | Java side |
|---|---|---|
| `launchIGP` | `(IZ)Z` | 9 stmts; uses starts an Activity/Intent |

Also created through JAdapter with no literal member names recovered (names are short or built from tables): `com/gameloft/glads/AndroidAds` (5 Java methods), `OmSDK` (6), `MoatSDK` (5), `AndroidMRAIDView`.

#### Framework lookups without a Java source

- `java/lang/Throwable.getMessage()Ljava/lang/String;` and `java/lang/Class.getName()Ljava/lang/String;` from `PackageUtils::Jni_CheckForExceptions` after every call (exception text is logged).
- `android/os/Bundle`: 13 methods looked up in `ABundle::SetJniVars` (`containsKey`, `clear`, `putInt/putLong/putString/putBoolean/getInt/...`), used to pass shared-preference containers to `AndroidUtils.getPreference(Bundle)`. Host: implement a map-backed Bundle.
- `android/os/Process.setThreadPriority(I)V` in `glf::Thread::Impl::SetPriority` (class resolved with a direct `FindClass`).
- `gameoptions::StringField("android/os/Build","MODEL")` (via `CDeviceSpecs_android::GetDeviceModel`) is the one reflective static field read: `GetStaticFieldID(Build, "MODEL", "Ljava/lang/String;")` + `GetStaticObjectField`. `gameoptions::IntFunction(class, method)` (`GetStaticMethodID(.., "()I")` + `CallStaticIntMethod`) is present but has no caller. Everything else in `CDeviceSpecs_android` (platform, cores, GPU, memory, SoC) is computed natively from libc/sysfs/GL queries.
- No `GetFieldID` of instance fields exists anywhere in the live code (the only field IDs are the one above).

### JNI function-table slots used by the native code

Index = slot in `JNINativeInterface` (offset = index x 8). Counts are call sites inside functions that obtain a `JNIEnv`; a host `JNIEnv` must implement at least these. All other slots may point at an abort stub.

| Slot | Function | Call sites |
|---|---|---|
| 6 | FindClass | 17 |
| 15 | ExceptionOccurred | 1 |
| 17 | ExceptionClear | 1 |
| 21 | NewGlobalRef | 9 |
| 22 | DeleteGlobalRef | 7 |
| 23 | DeleteLocalRef | 308 |
| 25 | NewLocalRef | 1 |
| 28 | NewObject (varargs; via exported _JNIEnv::NewObject) | 2 |
| 29 | NewObjectV | 1 |
| 31 | GetObjectClass | 1 |
| 33 | GetMethodID | 21 |
| 35 | CallObjectMethodV | 1 |
| 38 | CallBooleanMethodV | 1 |
| 50 | CallIntMethodV | 1 |
| 53 | CallLongMethodV | 1 |
| 62 | CallVoidMethodV | 1 |
| 113 | GetStaticMethodID | 230 |
| 115 | CallStaticObjectMethodV | 1 |
| 118 | CallStaticBooleanMethodV | 1 |
| 130 | CallStaticIntMethodV | 1 |
| 133 | CallStaticLongMethodV | 1 |
| 136 | CallStaticFloatMethodV | 1 |
| 139 | CallStaticDoubleMethodV | 1 |
| 142 | CallStaticVoidMethodV | 1 |
| 144 | GetStaticFieldID | 1 |
| 145 | GetStaticObjectField | 1 |
| 167 | NewStringUTF | 131 |
| 169 | GetStringUTFChars | 158 |
| 170 | ReleaseStringUTFChars | 152 |
| 171 | GetArrayLength | 11 |
| 172 | NewObjectArray | 1 |
| 173 | GetObjectArrayElement | 9 |
| 174 | SetObjectArrayElement | 1 |
| 175 | NewBooleanArray | 2 |
| 176 | NewByteArray | 5 |
| 179 | NewIntArray | 2 |
| 200 | GetByteArrayRegion | 3 |
| 207 | SetBooleanArrayRegion | 2 |
| 208 | SetByteArrayRegion | 5 |
| 211 | SetIntArrayRegion | 2 |

JavaVM table: slot 4 `AttachCurrentThread`, slot 5 `DetachCurrentThread` (in every `ScopeGetEnv` destructor, 365 sites), slot 6 `GetEnv` (`ScopeGetEnv`, JNI_OnLoad). Other native threads (game thread, glwebtools, asio) attach through `ScopeGetEnv`, so `AttachCurrentThread` must hand out a per-thread JNIEnv. The exported C++ wrappers `_JNIEnv::CallStaticVoidMethod/CallStaticIntMethod/CallStaticBooleanMethod/CallStaticLongMethod/CallStaticFloatMethod/CallStaticDoubleMethod/CallStaticObjectMethod/CallVoidMethod/CallIntMethod/CallLongMethod/CallBooleanMethod/CallObjectMethod/NewObject` (varargs; they build a `va_list` and call the `...V` slots) are in libNOVA, so the host receives AAPCS64 `va_list` structs for the V slots (see imports.md, vsnprintf note).

## (c) Start-up sequence

Java side, in order (all from the decompiled sources):

1. Process start: `FrameworkApplication.attachBaseContext` (MultiDex install), `onCreate` stores the application `Context` (`FrameworkApplication.getContext()`).
2. `MainActivity.onCreate`: finishes immediately if it is not the task root and was started with `ACTION_MAIN`; stores `MainActivity.l`; reads display metrics; `b(true)` sets requested orientation (landscape sensor); on API 28+ sets `layoutInDisplayCutoutMode=1`; then `q()`.
3. `MainActivity.q()` builds a `RelativeLayout` with a full-screen `SurfaceView` (registers itself as `SurfaceHolder.Callback`) and the Gameloft logo `ImageView`; `TopLayer.SetContainer`; `PopUpsBridgeClass.InitBridgeLibrary(activity, layout)`; checks `Environment.getExternalStorageState()=="mounted"` (then `AndroidUtils.setIsStorageEmulated(true)`) and that `getFilesDir().getFreeSpace() > 398,458,880` bytes (380 MiB) unless the pref `MainActivityPrefs/HasExtractedData` is already true (low space shows a "not enough space" dialog and stops).
4. DecRawso unpack: `DecRawso.NewInstance(context, null, false)`. If `<filesDir>/DecRawsoLib/decdone_<versionCode>_<lastUpdateTime>` does not exist it creates `<filesDir>/DecRawsoLib/`, `System.loadLibrary("DecRawso")` (libDecRawso.so from the APK, arm64-v8a build = 67,312 bytes) and starts a thread that calls the native `Decode(AssetManager, null, "<filesDir>/DecRawsoLib/", abi)`. `Decode` reads the APK asset `datv8` (arm64) or `datv7` (arm32/other) (8,311,568 / 5,669,511 bytes, obfuscated 7-zip data; libDecRawso contains the 7z SDK: LZMA, LZMA2, PPMd7, BCJ2) and writes the libraries that Java later asks for (`libNOVA.so`, `libgenerator.so`; inferred from `DecRawso.a("NOVA")` / `a("generator")`) into that directory, then the Java thread creates the `decdone_*` marker (plus `armmode` if `IsArmMode()`). x86 devices are detected through `getprop ro.product.cpu.abi` and fall back to the ARM payload via a `_FORCEARM_.tmp` marker. `DecRawso.GetInstance().c()` then waits for the marker. The two ELF files in `game/lib/` are exactly that payload, so the host skips this step.
5. `System.load(DecRawso.GetInstance().a("NOVA"))` = `System.load("<filesDir>/DecRawsoLib/libNOVA.so")` -> dynamic loader -> constructors (`DT_INIT_ARRAY`, 371 entries) -> `JNI_OnLoad` (`SetVm`, `GetEnv`, `LoadClasses`). If this throws, `MainActivity.P=false` and the activity stays inert.
6. `MainActivity.p()` (only when the space check passed): `n()` creates `InputDispatcher` and `SignalDispatcher`; `SignalDispatcher.a(activity, layout)` calls `AndroidUtils.SetActivityRef` (which runs `Device.init()`) and the `PluginEventDispatcher`, which instantiates by reflection and calls `onPluginStart(activity, viewGroup)` on 13 internal plugins (PushNotificationPlugin, VirtualKeyboardPlugin, GoogleAnalyticsTrackerPlugin, LocationPlugin, LogoViewPlugin, TrackingPlugin, AppDetectionPlugin, PermissionPlugin, ClipboardPlugin, AudioListenerPlugin, QALogsPlugin, GoogleAdIdPlugin, AdBlockerPlugin) and 6 external ones (SocialPlugin, PopupslibPlugin, GLAdsV2Plugin, AdsManagerPlugin, InGamePromotionPlugin, ShaderPlugin). Then `o()` -> `JNIBridge.NativeInit()`, then `LowProfileListener.ActivateImmersiveMode`. A 3-second `Handler` timer (`M`) later removes the logo / headphone / cache-warning overlays (`MainActivity.c()` state machine, driven by `LogoViewPlugin` and the `UpdateExtractionProgress` family).
7. `NativeInit()` = `Internal::Init()` then `OnGameInit()`, which (if `glf::gApp` is null) does `pthread_create(&thread, NULL, AndroidMain, NULL)`. `AndroidMain` (the game thread): `glf::InitGlobals(argc, argv)`, `NewApp(argc, argv)` (creates `Application`/`glf::App`), `glf::App::Impl::Run()` (the main loop), then `pthread_exit`. There is no Java-driven frame call: the game renders on its own thread and presents with `eglSwapBuffers` (`glf::App::Impl::Run`/`Present`).
8. Surface: `SurfaceHolder.surfaceCreated` is empty. `surfaceChanged(holder, format, w, h)` -> `JNIBridge.NativeSurfaceChanged(Surface, w, h)` (only if `x`, i.e. NativeInit ran) -> `ANativeWindow_fromSurface`, `Internal::SetWindow(window,w,h)`, `PackageUtils::GetNativeWindow`, `OnWindowStateChange(ANativeWindow*)` -> `glf::App::Impl::NotifyWindowStateChanged(window)` (mutex-protected hand-off to the game thread, which then calls `NativeSurface::CreateGLEnvironment` / `RefreshGLEnvironment`: `eglGetDisplay`, `eglInitialize`, `eglChooseConfig`, `eglCreateWindowSurface`, `eglCreateContext`, `eglMakeCurrent`). `surfaceDestroyed` -> `NativeSurfaceChanged(null, 0, 0)` (releases the window, then `OnWindowStateChange(NULL)`).
9. Resume/pause: `MainActivity.onResume` (when focus is held) -> `SignalDispatcher.b()` = `AndroidUtils.onPreNativeResume`, plugins `onPreNativeResume`, `AndroidUtils.onResume` -> `JNIBridge.NativeOnResume()` (= `Internal::PreGameResume`, `OnGameResume()`, `Internal::PostGameResume`), plugins `onPostNativeResume`; also registers the connectivity and battery receivers. `onPause` -> `SignalDispatcher.a()` -> `NativeOnPause()` (= `PreGamePause`, `OnGamePause()`, `PostGamePause`); if `isFinishing()` the process is killed (`Process.killProcess`), optionally after scheduling a restart alarm.
10. Input: `Activity.onTouchEvent` -> `InputDispatcher.a(MotionEvent)` -> `NativeOnTouch(int action, float x, float y, int pointerId)`: ACTION_DOWN/POINTER_DOWN -> 0, MOVE -> 1 (one call per pointer), UP/POINTER_UP -> 2, CANCEL -> 3 (all pointers); coordinates are raw window pixels. Native: `OnGameTouchEvent(action,x,y,id)` -> `glf::GetInputMgr()->GetGamepad(0)->RaiseTouchEvent(0, type 0xd8 down / 0xd9 move / 0xda up, id, x, y, 2)`; action 3 is ignored. `onKeyDown/onKeyUp` -> `NativeKeyAction(keyCode, isDown)` only for KEYCODE_BACK (4) and KEYCODE_MENU (82) -> `NativeOnKeyAction` -> `glf::GetInputMgr()->GetKeyboard(0)->RaiseButtonEvent(key, down, ..)`. Volume keys (24/25) call `NativeVolumeAction(bool up)` only when the QA config file `/sdcard/Android/data/<pkg>/files/qaTestingConfigs.txt` sets `LANGUAGE_WITH_VOLUME=1`. `onGenericMotionEvent` (gamepad sticks, mouse wheel) is not forwarded in this build, and the HID controller Java class is absent. Back button during the logo/headphone/cache screens only calls `moveTaskToBack`; afterwards Back arrives as `NativeKeyAction(4, down/up)`.
11. Other callbacks into native at runtime (all optional for a PC host): `SetConnectionType(int)` (UtilsNetworkStateReceiver; 0 = no connectivity, 1 = Wi-Fi, other values encode other network classes), `SetBatteryInfo(boolean charging, boolean usb, boolean ac, int level)` and `NotifyLowBattery()` (battery receiver; also called once by `initBatteryInfo`), `NativeAudioFocusChanged(boolean)` and `NativeUserMusicIsPlaying(boolean)` (AudioListenerPlugin), `SetUserLocation(...)` (LocationPlugin), `NativeSendKeyboardData(String)` (VirtualKeyboard text result), IAP/CRM, social and ad callbacks listed in (a).

What a host must do to replicate this without Java (minimum): (1) load `game/lib/libNOVA.so`, run its init array, call `JNI_OnLoad(javaVM, NULL)` with a JavaVM whose `GetEnv` returns a JNIEnv that implements the slots above and whose `FindClass` knows the preloaded class table; (2) answer the `PackageUtils::Retrieve*`/device-info/preference calls listed below; (3) call `Java_..._JNIBridge_NativeInit`, then `NativeSurfaceChanged(surfaceObj, width, height)` with a fake Surface that `ANativeWindow_fromSurface` understands; (4) call `NativeOnResume`; (5) from the PC input loop call `NativeOnTouch(0/1/2, x, y, 0)` for mouse-as-touch and `NativeKeyAction(4, down)` for Esc; engine-level injection points (mouse, keyboard, gamepad) are listed in engine.md. `NativeOnPause` on focus loss. No per-frame call is needed.

## (d) What the native side asks Java for

Everything below is reached through the lookups above; the method rows give the descriptors and Java behaviour. Grouped by purpose.

File system and paths (`AndroidUtils`; cached into `PackageUtils::Get*FolderPath` and read at `glf::App::Impl::InitPaths`): `RetrieveDataPath()` -> `Context.getExternalFilesDir(null)` (= `/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftNOHM/files`) when external storage was flagged usable, else `getFilesDir()`; `RetrieveSavePath()` -> `getFilesDir()` (`/data/data/<pkg>/files`); `RetrieveTempPath()` -> `getCacheDir()`; `RetrieveSDCardPath()` -> `Environment.getExternalStorageDirectory()` (`/storage/emulated/0`); `RetrieveObbPath()` -> the constant string `"NA"` (no OBB is used); `RetrieveNativeLibraryPath()` -> `ApplicationInfo.nativeLibraryDir`; `GetLibSoPath()` -> `<filesDir>/DecRawsoLib/libNOVA.so`; `GetApkPath()` -> `ApplicationInfo.sourceDir` (the APK file; opened with libzip by `ysM::OpenAPK`); `GetAssetAsString(String)` -> bytes of an APK asset (leading `.` stripped); `GetDiskFreeSpace(String)`, `GetDiskTotalSpace()`; `DeleteFile`, `RemoveDirectoryRecursively`, `GenericUnzipArchive(zip, dir)`. See paths.md.

Device information (all strings, via `Device`/`Build`): `GetAndroidID`, `GetSerial`, `GetCPUSerial`, `GetDeviceManufacturer`, `GetDeviceModel` (`Build.MODEL`, also read natively), `GetPhoneProduct`, `GetPhoneDevice`, `GetFirmware`, `GetCPUAbi`, `GetBuildBoard`, `GetMacAddress`, `GetDeviceIMEI` (empty without permission), `GetHDIDFV` (persisted UUID, see imports.md), `GetGoogleAdId`/`GetGoogleAdIdStatus`, `IsSimReady`, carrier/country/language getters in `AndroidUtils`/`Device` (`GetCarrierAgent`, `GetCountry`, `GetSimIsoCountryCode`, `GetDeviceSettingsCountryCode`, `GetDeviceLanguage`, `GetUserAgent`), `getVersionCodes()` (= `Build.VERSION.SDK_INT`), `IsGameRunOnEmulator`, `GetLastMemoryWarning`/`IsMemoryWarningActive`, `GetElapsedRealtime`, `GetMetaDataValue(String)` (manifest meta-data). Hardware facts such as CPU frequency, core count, RAM, temperature, GPU clock are NOT asked from Java: libNOVA reads `/proc` and `/sys` itself (see paths.md) and GL strings.

Language and locale: `AndroidUtils.GetDeviceLanguage()` -> `Device.retrieveDeviceLanguage()` (ISO code from `Locale`); `PackageUtils::SetWarningLanguage(code)` -> `LogoViewPlugin.SetWarningLanguage` and the `MainActivityPrefs/WarningLanguage` preference (`MainActivity.a(String)` rewrites the resource `Configuration.locale`; br/pt -> pt_BR, la -> es_US). The game picks its own UI language natively (`Application::InitDefaultLanguage`, `GetLanguageString`).

Screen size, DPI, density, cutout: there is no Java call for width/height of the game surface: the size comes from `NativeSurfaceChanged(surface, w, h)` and `ANativeWindow_getWidth/getHeight`. `PackageUtils::GetWidth/GetHeight/GetDpi/GetDisplaySizeInInch` read cached values filled by `InitializeCachedVars`/`SetWindow`; DPI comes from `AndroidUtils.GetXDpi/GetYDpi` (`DisplayMetrics.xdpi/ydpi`) and, for the ad web views, `AndroidDevice.GetScreenXDPI/GetScreenYDPI/GetLogicalDensity/GetDIPScaleFactor`. Cutout: `CutoutHelper.DeviceHasCutout()` and `CutoutSafeInset(int dir)`. Orientation: `SetOrientation(boolean)` (the game is landscape-only), `SetKeepScreenOn(boolean)`.

Preferences and key/value storage (`SharedPreferences`): `SavePreference{Int,Long,Bool,String}(key, file, value)`, `GetPreference{Int,Long,Bool,String}(key, file, default)`, `RemovePreference(key, file)`, `getPreference(Bundle)`; and `DataSharing` (`getSharedValue/setSharedValue`, cross-app store used for HDIDFV and shared Gameloft IDs). Preference file names seen in Java: `MainActivityPrefs` (`HasExtractedData`, `WarningLanguage`).

Network and HTTP: the game does NOT use Java networking for its own traffic (libcurl 7.59.0-DEV + statically linked OpenSSL + asio, plus raw sockets for LAN play). Java is asked only for connectivity state (`AndroidDevice.HasInternetConnection/GetConnectionType`, `UtilsNetworkStateReceiver` -> `SetConnectionType`), `AndroidUtils.HttpExecuteAsync(url)` (fire-and-forget HTTP GET with `DefaultHttpClient`), `LaunchBrowser(url)` (`ACTION_VIEW`), `LaunchWifiSettings()`, and the web views (ads, pop-ups, in-game browser, IAP web pages). A PC host needs real sockets and a TLS-capable network but no Java HTTP.

Downloads: there is no Java download manager in the call set. Content download/CRM goes through native curl (`gaia::`, `of::`, `PopUpsLib::PopUpsServer::DoGetAssetJob`, `glwebtools`). `PopUpsBridgeClass.nativeOnDownloadState` is declared but not implemented natively.

Ads: `com/gameloft/adsmanager/IronSourceAds` (9 members), `UnityAdsManager`, `JavaUtils`, `glads/AndroidWebView`, `AndroidAds`, MRAID/VAST/Moat/Avid/OmSDK views, `PopUpsBridgeClass`; callbacks `IronSourceNotifyEvent`, `UnityAdsNotifyEvent`, `*ReportInternalError`, `AdsManagerLog*`. Stub everything; the game then runs with "no ads available".

In-app purchases / CRM / federation: `com/gameloft/android2d/iap/IAPLib` (124 static methods: price/feed/limit/restore/subscription calls) plus `Links.NIAP_*` natives (`NIAP_GetClientId`, `NIAP_GetDeviceId`, `NIAP_GetAccessToken`, `NIAP_GetUserCredential`, `NIAP_GetDatacenter`, `NIAP_GetCRMUrl/Feed` ... these are Java-to-native getters of Gaia/federation state) and `IAP_CallBack.NIAP_*` result callbacks. Stub: `CheckAvailable()Z` false, price queries empty; never invoke purchase callbacks.

Social login: Facebook (`FacebookAndroidGLSocialLib`: Login/Logout/IsLoggedIn/Request*/Post*/SendGameRequest, callbacks `nativeOnFBDataLoad`, `nativeOnFBFailWithError`, `nativeOnFBDialogDidComplete/DidNotComplete`), Google Play Games (`GameAPIAndroidGLSocialLib`: ConnectToService, achievements, leaderboards, friends; callbacks `nativeGameAPI*`), VK/Renren/Weibo SDK classes exist in the APK but are not looked up. Stub: `IsLoggedIn` false, report failure via the callback, or never answer.

Push notifications, Firebase, location, clipboard, keyboard, permissions, app detection: `SimplifiedAndroidUtils` (7 lookups), `GoogleFirebase` (3), `LocationPlugin` via `AndroidUtils.EnableUserLocation/DisableUserLocation/getUserLocation*`, `ClipboardPlugin` (copy/paste), `VirtualKeyboard` (`ShowKeyboard(String,int,int,int,int)`, `HideKeyboard`, `IsKeyboardVisible`, `GetVKeyboardText`, `SetVKeyboardText`; result returned through `NativeSendKeyboardData`; on PC use native text input), `PermissionPlugin` (`isCameraPermissionEnabled` etc.), `AppDetection` (`isAppInstalled`, `getInstalledPackages`), `AppInvite` (`GoogleInvitePlugin`, 3 lookups).

Anti-tamper / identity checks that touch Java: `AndroidUtils.GetLibSoPath()`, `GetApkPath()`, `GetNativeLibraryPath` and `IsAppEnc()`/`retrieveBarrels()` (returns `int[]` signature digests via `SUtils.retrieveBarrels`, used by `PackageUtils::IsAppEnc`), `getGLUID(String)` (`[I`), `GenerateUUID()`, `GetPlainText(String,String)` (decrypts a string with a key, used by `ysM::CheckOfflineZoro`), `ShowOfflineZoroDialog()` (shows a ban message and exits). See "risks" in the report and paths.md for the native half (`platform::AntiHacking::GetBatman` -> `ysM::ftC`).

