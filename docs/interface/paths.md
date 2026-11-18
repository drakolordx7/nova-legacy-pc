# File paths, extensions and archives used by libNOVA.so

Everything here comes from strings in `libNOVA.so` `.rodata`/`.data` with the referencing function found by following `adrp`+`add` pairs, plus the Java sources for the paths Java supplies. "Fn" is the demangled function that references the string. Strings that exist only in statically linked third-party code (OpenSSL `/usr/local/ssl/...`, curl, FreeType `.AppleDouble`, Lua `/usr/local/share/lua/5.1/?.lua`) are not game paths and are listed at the end only so they are not mistaken for requirements.

## 1. How the game decides where its data lives

1. Java supplies five roots (`AndroidUtils`, cached natively by `acp_utils::acp_internal::Internal::InitializeCachedVars`/`RefreshCachedVars` into `PackageUtils::Get*FolderPath`):

   | Native getter | Java method | Value on a phone | Meaning for the PC host |
   |---|---|---|---|
   | `PackageUtils::GetDataFolderPath` | `RetrieveDataPath()` | `Context.getExternalFilesDir(null)` = `/storage/emulated/0/Android/data/com.gameloft.android.ANMP.GloftNOHM/files` (falls back to `getFilesDir()` when Java flag `c` / `setIsStorageEmulated(false)`) | the game data root (extracted packs, downloaded content) |
   | `PackageUtils::GetSaveFolderPath` | `RetrieveSavePath()` | `Context.getFilesDir()` = `/data/data/com.gameloft.android.ANMP.GloftNOHM/files` | "home" dir (profile) |
   | `PackageUtils::GetCacheFolderPath` | `RetrieveTempPath()` | `Context.getCacheDir()` | temp dir |
   | (SD card, used for `/sdcard` style paths) | `RetrieveSDCardPath()` | `Environment.getExternalStorageDirectory()` = `/storage/emulated/0` | |
   | (OBB) | `RetrieveObbPath()` | constant string `"NA"` | no OBB is used at all; there is no `.obb` string in libNOVA |
   | `PackageUtils::GetNativeLibraryPath`, `GetLibSoPath`, `GetApkPath` | `RetrieveNativeLibraryPath()`, `GetLibSoPath()`, `GetApkPath()` | `nativeLibraryDir`, `<filesDir>/DecRawsoLib/libNOVA.so`, `ApplicationInfo.sourceDir` (the APK) | used only by the integrity check `ysM::ftC` and `ysM::OpenAPK` (section 6) |

2. `glf::App::Impl::InitPaths()` (first loop iteration of the game thread, `_ZN3glf3App4Impl9InitPathsEv`) does exactly: `glf::Fs::SetDataDir(GetDataFolderPath())`, `glf::Fs::SetHomeDir(GetSaveFolderPath())`, `glf::Fs::SetTempDir(GetCacheFolderPath())` (decoded from the three calls in its disassembly). `glf::fs2::android::{SetPaths,GetDataDirectory,GetSaveDirectory,GetCacheDirectory,GetTempDirectory}` expose the same three. So the PC host controls the whole tree by answering those three Java getters.
3. Opening a file (`glf::FileStreamImpl::Impl::Open`, `_ZN3glf14FileStreamImpl4Impl4OpenEPKcNS_3ios8openflagE`): tries the real file system first (`fopen`); if the file is not there it normalises the path and falls back to the APK asset with the same relative name: `android_fopen()` -> `acp_utils::modules::AssetReader::open` -> `AAssetManager_open` -> wrapped as a `FILE*` with `funopen`, logging `**** FILE %s FOUND IN PACKAGE!`. The `AAssetManager` is obtained once from Java (`AndroidUtils.GetAssetManager()` -> `AAssetManager_fromJava`). A PC host therefore needs (a) a real directory for the data root and (b) an AAssetManager implementation over the APK zip (`assets/` prefix).
4. Packs and mount points: the glitch layer `CCustomFileSystem` (derived from the engine file system) mounts `.gla2`/`.gla` archive files by name: `addGla2PakArchive(name,int)` builds `"%s%s.gla2"` (directory + name; `addGla2PakArchiveInternal` also knows the name `packdlc`), `addCustomPakArchive`, `addCustomPakArchiveMultipart` (suffix format `_p%d%s`), `addScrambledZipFileArchive`. Compressed-texture variants are selected with the suffix returned by `CCustomFileSystem::GetTexturePackSuffix()` (`_ktx`) and the loader tries the extensions `.ktx .dds .TGA .BMP .psd` (`createAndOpenFile`, `existFile`); GPU-specific variants are chosen by matching `GL_RENDERER` against the strings `Mali-T628`, `PowerVR SGX 544`, `Adreno (TM) 330` (that function also special-cases `Character_VS.glsl`, `Character_FS.glsl`, `_sb_low`, `_sb_med`, `GLSL/file.map`, `glsl.config`, `sbs_`, `_gl2.batch`, `_gl2.btsegs`).
5. First-run extraction (`Application::UnpackResources()`, driven from `MainActivity` load phase "DataExtraction"): opens the APK asset `compressed_res.gla2` (26,927,051 bytes in this APK) with `Gla2PakReader`, reads its `.file_list`, and unpacks everything under `GetDataFolderPath()` (including `sbs_textures/` and `substance_texture_info.txt`), writes `<data>/.timestamp`, and sets the preference `MainActivityPrefs` / `HasExtractedData`. Java demands 398,458,880 bytes (380 MiB) free in `getFilesDir()` before it lets this start. A second phase generates procedural ("substance") textures from `substance.gla` / `substance_proc_info.bin` / `texture_references.bin` / `level_order.bin` into `sbs_textures/*.bmp` (`Substantificator`, load phases TextureGeneration 0.35-0.85). After that `Application::InitFromLogo()` mounts the packs listed in section 3. Consequence: the host can either run this extraction itself on first launch (needs `compressed_res.gla2` served from the APK) or pre-extract once and point the data root at the result. What exactly `compressed_res.gla2` contains was not unpacked in this study (the container is Gameloft "gla2"; the file list is inside it).
6. DLC / content updates use the same machinery: `DlcManager` (`dlc.dat`, `.timestampDLC`, `packdlc`, `substance_dlc.gla`), downloaded by native curl code, not by Java.

## 2. Hard-coded absolute paths (Android-specific; a PC VFS must map or rewrite them)

| String | Fn | Notes |
|---|---|---|
| `/data/data/com.gameloft.android.ANMP.GloftNOHM/a1.dat` | `Application::LoadRequestedMap`, `DeleteCampaign`, `SaveCampaignLevelSection`, `GetSavedLevelNameFromCampaign` | campaign save |
| `/data/data/com.gameloft.android.ANMP.GloftNOHM/a2.dat` | `Application::LoadInterfaceData`, `SaveInterfaceData`, `DeleteInterfaceSave` | HUD/interface layout save |
| `/data/data/com.gameloft.android.ANMP.GloftNOHM/a9.dat` | `Application::SaveCheckPoint`, `ExistsCheckPointSave`, `GetSavedCheckpoint` | checkpoint |
| `/data/data/com.gameloft.android.ANMP.GloftNOHM/briefings.dat` | `CMissionsManager::LoadBriefings/SaveBriefings` | |
| `/data/data/com.gameloft.android.ANMP.GloftNOHM/hud.dat` | `Hud::Save/Load` | |
| `/data/data/com.gameloft.android.ANMP.GloftNOHM/dlc.dat` | `DlcManager::Load/Save` | |
| `/data/data/com.gameloft.android.ANMP.GloftNOHM` (+ `%s/NovaScreenShot_%02d%02d%02d_%02d%02d%02d.pvr`) | `Application::ScreenShotRequest(char const*, int)` | screenshots are written as PVR |
| `/sdcard/Android/data/com.gameloft.android.ANMP.GloftNOHM/files/settings.dat` | `CGameSettings::Load/Save/Delete` | options file; same absolute path appears in Java for `qaTestingConfigs.txt` (`LANGUAGE_WITH_VOLUME`) |
| `/sdcard/gameloft/games` | `AndroidDevice::GetLoggingPath` (ads lib) | ads debug log folder (`/glads/glads.log`, `/glads.cfg` appended) |
| `/sdcard/gameloft` | `onDeclineForGfxReboot(gameswf::ASNativeEventState const&)` | UI script callback |
| `../Documents/profileall.json` | `MpPlayer::LoadJsonProfileFromDisk` | relative to the home dir (iOS heritage) |
| `../Documents`, `../Library`, `../Library/Caches`, `../tmp`, `/library/caches`, `/library`, `/documents` | `glf::fs2::apple::*`, `glf::fs2::Path::MakeLower` | Apple code paths, dead on Android |

Rule of thumb for the port: every `/data/data/<pkg>/...` and `/sdcard/Android/data/<pkg>/files/...` path is accessed with plain `fopen`/`open` (not through `glf::Fs`), so the libc layer must rewrite `/data/data/com.gameloft.android.ANMP.GloftNOHM` to the host save dir and `/sdcard` / `/sdcard/Android/data/<pkg>/files` to the host data dir. The strings are baked into the binary; they can also be patched in the mapped image because they live in `.rodata`.

## 3. Archive and data names mounted or loaded relative to the data root / assets

| Name | Where | Notes |
|---|---|---|
| `compressed_res.gla2` | `Application::UnpackResources` | APK asset; first-run source |
| `pack/actors_mp.gla2`, `sbs_textures/sbs_metal_mp.bmp` | static name table | multiplayer actors pack |
| `weapons`, `vehicles`, `vehicles_stream`, `particlesystems`, `characters_stream`, `actors`, `actors_stream`, `actors_sm`, `meshes_modules`, `levels2`, `ui_video`, `ui_HD`, `prerendered`, `lut`, `sprites` ("could not find sprites pack!") | `Application::InitFromLogo` | pack base names, each mounted as `<name>.gla2` through `addGla2PakArchive` (`%s%s.gla2`); level packs `z_0_spaceship`, `z_1_jungle`, `z_2_bunker`, `z_3_alien_city`, `z_mp`, `z_99_main_menu` |
| `_stream.gla`, `_irr.gla`, `config_default.xml`, `locations.bin`, `alpha0.bmp` | `Application::InitFromLogo` | stream / irradiance packs and base config |
| `substance.gla`, `substance_dlc.gla`, `substance_ok`, `substance_proc_info.bin`, `substance_texture_info.txt`, `texture_references.bin`, `level_order.bin`, `sbs_textures/` | `Application::UnpackResources`, `DlcManager::LoadMap`, `Substantificator` | procedural texture generation inputs/outputs |
| `.file_list`, `/.timestamp`, `/.timestampDLC`, `packdlc` | unpack / DLC | |
| `tuning.bin`, `rewards.bin`, `tlemission.bin`, `armory.bin`, `Map_Zones.bin`, `mp_barks.bin`, `gol.bin`, `lv_%s.bin`, `interface_%dx%d.bin`, `soundslist_hash_length.bin`, `dr%d.bin`, `attribution_check.bin`, `properties.bin`, `hestia_local_data.bin`, `hestia_etag_data.bin`, `*_map.bin`, `/timerecord.bin` | `CTuningManager`, `RewardsData`, `Armory`, `CMapZoneGraphManager`, `MpBarksManager`, `Application::GetGolMemStream`, `GS_Loading::Update`, `Application::LoadInterfaceData`, `VoxSoundManager::Init`, `of::adsdailyreports`, `of::Detections`, `glf::PropertyMap`, `CShopManager`, `StringsManager::LoadStrings` | game data tables; `interface_<w>x<h>.bin` and `DPI.xml` (`Application::LoadDPI`) are resolution-keyed UI layout files |
| `config_default.xml`, `material_maps.xml`, `material_technique_maps.xml`, `GPUs.xml`, `GPU_5.xml`, `GPU_SERVER.xml`, `CPU_SERVER.xml`, `MEM_1024.xml`, `FOVs.xml`, `DPI.xml`, `Nova_Strings_Library.xml`, `VOX_SDD_Nova.xml`, `Registry.xml`, `flash.xml`, `CascadedShadowTechniqueMap.xml`, `LiSPShadowTechniqueMap.xml`, `ShadowCubeTechniqueMap.xml` | `DeviceOptions::LoadGpusProfilesTable/LoadProfile/LoadServerProfile/LoadFovsTable`, `CNovaSceneManager::LoadTechniqueMaps`, `CLevel::LoadMaterialsMap`, ... | device profile tables (GPU name -> quality level, FOV per device) and engine maps |
| `GO_android.json` (APK asset), `GameOptions_debug.json`, `GameOptions_saved.json`, `/nova_phd/GO_android_` | `GameOptions2_ReadFile`, `gameoptions::GoDebugger` | device-options JSON; `GO_android.json` is read from the APK assets |
| `*.bdae` (`PostEffect.bdae`, `Decals_01.bdae`, `CustomEffects.bdae`, `materials.bdae`, `DefaultEffects.bdae`, `gameswf_effects.bdae`, `objective_arrow.bdae`, `*_menumats.bdae`, `ShadowVolume.bdae`, `UnlitTexturesPremultiply.bdae`, `DebugCapsule_*.bdae`, `little_endian[_quantized|_not_quantized].bdae`) | glitch collada binary | engine resources |
| `batches.bin`, `segments.bin`, `pvs.bin`, `.col`, `_gl2.batch`, `_gl2.btsegs`, `data.voxels`, `data.voxel_faces`, `little_endian.dat` | level / scene loaders | |
| `.bglsl` (`%s/bshader/%s.bglsl`, `/bshader/`), `GLSL/file.map`, `%s/file.map`, `map.txt`, `glsl.config`, `*.glsl`, `shader_info_cache` | glitch shader loader | binary GLSL ES shaders + shader-info cache directory (created with `mkdir`; failure disables the cache) |
| Textures: `.TGA .tga .BMP .bmp .dds .ktx .KTX .PVR .pvr .psd .jpg .png` (`.pvr3`, `.ktx` header checks in the loader) | `CCustomFileSystem`, `Substantificator`, `GameSwfHostInterface::getTexture` (`_alpha.bmp`) | compressed container formats; case-varying extensions: use a case-insensitive file system or the engine switch `glf::fs::SetIsForcingLowercase` / `glf::fs2::Path::MakeLower` |
| `nova_font_latin.ttf`, `nova_font_japanese.ttf`, `nova_font_korean.ttf`, `nova_font_russian.ttf`, `nova_font_thai.ttf`, `nova_font_arabic.ttf` | font loader | TrueType via FreeType |
| Audio: `*.wav` (`sfx_menu_skip_cutscene.wav`, `sfx_rocket_target_locked.wav`, `%s.wav`), `.ogg`, `.voxdata`, `/tags.packinfo` | `CLevel`, `vox::DescriptorManager::LoadArchivePack/TryFolderLoad` | Vox audio archives; the Vox engine looks for `*.voxdata` archives and a `tags.packinfo` index |
| `.dyn.crc`, key `c|-|3k` | `glf::Fs::LoadObfuscationMap` | optional filename-obfuscation map |
| Social/ads/pop-ups: `snsconfig.json` (APK asset; `sociallib::ClientSNSInterface::loadSnsConfigJson`), `save_sns.dat`, `save_import.dat`, `freemium.json`, `premium.json`, `spaceship.png bunker.png jungle.png aliencity.png spevent.png mpevent.png achievement.png mpmatch.png weapon_%d_%d_%d.png` (share images), `CRMActionData.dat`, `glads/caches/banners`, `glads/caches/fullscreenads`, `glads/ingameads`, `etag.txt`, `__hashTag`, `__info`, `/index.html`, `gl_tracking.js` | `SocialLibManager`, `OnlineManager`, `crm::CrmManager`, `CacheManager`, `PopUpsLib::PopUpsServer` | network-feature caches; harmless to leave empty |
| Logs and debug: `log.txt` (`glf::FileLogger`), `fpsReport.txt`, `log_send.txt`, `log_recv.txt`, `/AutoMake/` (`glf::MakeManager`) | | optional |

## 4. APK assets the native code reads directly (via `AAssetManager`, or Java `GetAssetAsString`)

`compressed_res.gla2` (26.9 MB), `GO_android.json` (195 KB), `snsconfig.json` (466 B), `REVISION.txt` (`ApplicationInfo::GetSvnRevision`; content is the text "Local Windows Build"), `content.txt` (24 bytes of base64 text, used by `ysM::CheckOfflineZoro` with `PackageUtils::GetAssetResource` and key `19GmLfT99`), plus the DecRawso payload `datv8` / `datv7` that only Java/libDecRawso touches. The APK also carries `assets/iaks`, `assets/iaos`, `assets/map_win32.dlce`, `assets/raw/hdv.bin`, `assets/raw/crc.bin` (0 bytes), `GO_ios.json`, layouts and strings for the Java IAP UI; the native code in libNOVA has no reference to these names.

## 5. System probes (virtual file system must answer or fail cleanly)

| Path | Fn | Meaning / safe answer |
|---|---|---|
| `/proc/cpuinfo` | `PackageUtils::GetDeviceArchitecture` | CPU architecture text |
| `/proc/meminfo`, `/proc/self/statm` | `GetCurrentAvailableRamInMegaBytes`, `automation::getCurrentResidentSetSize` | RAM figures |
| `/sys/devices/system/cpu/`, `/sys/devices/system/cpu/present`, `.../cpu%d/cpufreq/{scaling_cur_freq,cpuinfo_max_freq,cpuinfo_min_freq}`, `/sys/kernel/cpufreq_hardlimit`, `/sys/kernel/msm_cpufreq_limit` | `GetNumberOfCpuCores`, `GetCurrentCpuSpeedInHz`, `GetMaxCpuSpeedInHz`, `GetMinCpuSpeedInHz`, `GetFreqChart` | core count and clocks |
| `/sys/class/devfreq/devfreq-vpu/max_freq`, `/sys/class/devfreq/gpufreq/max_freq`, `/sys/class/kgsl/kgsl-3d0/max_gpuclk`, `/sys/devices/platform/dfrgx/devfreq/dfrgx/max_freq`, `/sys/devices/platform/gpusysfs/gpu_max_clock`, `/sys/devices/platform/kgsl-2d0.0/kgsl/kgsl-2d0/max_gpuclk`, `/sys/devices/platform/kgsl-3d0.0/kgsl/kgsl-3d0/max_gpuclk`, `/sys/devices/platform/omap/pvrsrvkm.0/sgxfreq/frequency_limit`, `/sys/kernel/gpu/gpu_max_clock`, `/sys/kernel/tegra_gpu/gpu_cap_rate` | `PackageUtils::GetGpuMaxFreq` | all optional; missing = unknown GPU clock |
| `/sys/class/thermal`, `%s/%s/temp` | `GetDeviceInternalTemperature` | thermal throttling input |
| `/system/build.prop` | `GetDeviceChipset` | `ro.board.platform`-style lookup |
| `/sys/bus/soc/devices/soc0/family` | `CDeviceSpecs_android::GetDeviceSoc` | |
| `/system/app/Superuser.apk`, `/system/bin/su`, `/system/xbin/su` | `PackageUtils::IsDeviceRouted` (root check) | answer "does not exist" |
| `/dev/urandom` | `fopen` in libgenerator and libc++ `random_device`; libNOVA OpenSSL also tries `/dev/random`, `/dev/srandom`, `/dev/egd-pool`, `/etc/egd-pool`, `/etc/entropy`, `/var/run/egd-pool` | serve `/dev/urandom` from the host RNG |
| `/dev/tty` | OpenSSL console | fail |

## 6. Anti-tamper reads the files themselves (important for a port)

`platform::AntiHacking::GetBatman()` calls `ysM::ftC()` (`_ZN3ysM3ftCEv`, which also uses the string `%s/lib%s.so` with `NOVA`, `armeabi-v7a`/`arm64-v8a`): it opens `PackageUtils::GetLibSoPath()` with `fopen`, validates the ELF header (`fyG(__sFILE*, elf64_hdr, unsigned long&, unsigned long&)`), computes SHA-256 of regions of the library (`GenerateSHA256(FILE*, ...)`), then `zip_open(GetApkPath())` and `zip_name_locate` on three entries in the APK (libzip is statically linked), recording sizes. `ysM::CheckOfflineZoro()` (`_ZN3ysM16CheckOfflineZoroEv`) formats `"%d|%d|%d"`, reads `content.txt` from the APK, decrypts with `GetPlainText(.., "19GmLfT99")`, `strcmp`s, and on mismatch calls `PackageUtils::ShowOfflineZoroDialog()` (Java dialog `banning_text`, then `ExitApplication(false)`). `PackageUtils::IsAppEnc()` hashes the APK signing certificates (`SUtils.retrieveBarrels()`, 4 ints). No direct `bl`/`b` caller of `CheckOfflineZoro` was found (it may be reached through a function pointer or a stripped caller); `ftC` is called by `platform::AntiHacking::GetBatman` only. The safe port design is to hook these exported symbols by name (`ysM::CheckOfflineZoro`, `ysM::ftC`, `platform::AntiHacking::GetBatman`, `PackageUtils::IsAppEnc`, `PackageUtils::IsRunOnEmulator`, `IsDeviceRouted`) to return "clean", and, independently, to keep the original APK and `libNOVA.so` bytes at `GetApkPath()` / `GetLibSoPath()` so the checks that remain pass. Whether a mismatch is enforced offline was not tested.

## 7. Server paths (not files, listed because they are also baked-in path strings)

Online portal `/2332/1.2.7/public/NOVAPortal.wsgi` (`OnlineManager::ReplaceUrlIfIsNecessary`), `/gacha/nova-legacy-mobile` (`Application::Browser_OpenDroprate`), and the Gaia service routes (`/accounts/...`, `/datacenters`, `/messages/`, `/transactions/me`, `/matches`, `/quick_launch`, `/automatch/`, ...). Network access is native (curl/asio/sockets). Whether the original servers still answer was not tested; the game must tolerate an offline start for the port to work (not verified at runtime).

## 8. Third-party strings that look like paths but are not game requirements

OpenSSL: `/usr/local/ssl`, `/usr/local/ssl/certs`, `/usr/local/ssl/cert.pem`, `/usr/local/ssl/lib/engines`, `$HOME`-based `RANDFILE`; Lua 5.1 `package.path` defaults `./?.lua;/usr/local/share/lua/5.1/?.lua;...`; FreeType `/..namedfork/rsrc`, `resource.frk/`, `.AppleDouble/`; curl `/etc`, `.netrc`; Windows build-tree strings such as `E:\Work\Project\Nova\trunk\extern\...` (assert/log file names) and `..\..\make\BuildData.py` (`CTexUpdater::Run` -> `system`), which are leftovers of the original Windows build.
