#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include "ui/navigation_model.h"
#include "ui/ui_fonts.h"
#include "ui/assets/brand_logo.h"
#include "ui/assets/about_logo.h"
#include "ui/provision_qr.h"
#include "font_zectrix.h"

using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = 1, ESP_ERR_NO_MEM = 2;
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
#define ESP_RETURN_ON_FALSE(condition,error,...) do { if(!(condition)){TestLog(__VA_ARGS__);return error;} } while(0)
#define ESP_RETURN_ON_ERROR(expr,...) do { const int e=(expr); if(e!=ESP_OK){TestLog(__VA_ARGS__);return e;} } while(0)
#define WQN_FIRMWARE_VERSION "test-live-version"
template<class... Args> void TestLog(Args...) {}
#define ESP_LOGW(...) TestLog(__VA_ARGS__)
#define ESP_LOGE(...) TestLog(__VA_ARGS__)
#define ESP_LOGI(...) TestLog(__VA_ARGS__)
int checks = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c); std::abort(); } ++checks; } while (0)
const char* esp_err_to_name(int) { return "error"; }
std::array<uint8_t, 400*300> pixels;
struct TextRun { int x,y; const lv_font_t* font; std::string text; };
std::vector<TextRun> drawn;
int reads=0, submits=0, resets=0, powers=0, downloads=0, syncs=0, provisions=0, follows=0;
bool persist_busy=false;
uint32_t submit_id=71;
int allocations=0,releases=0,gray_refreshes=0,gray_result=ESP_OK,nav_result=ESP_OK;
bool allocation_allowed=true,frame_is_gray=false;
std::vector<uint8_t> gray_presented;
void* heap_caps_malloc(size_t size,int caps) {
    CHECK(size==60000&&caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(!allocation_allowed)return nullptr;
    ++allocations;return std::malloc(size);
}
void heap_caps_free(void* data) { if(data)++releases;std::free(data); }

namespace wqn {
constexpr int kEpdWidth=400,kEpdHeight=300;
constexpr size_t kEpdGray4RowBytes=kEpdWidth/2,kEpdGray4PayloadSize=kEpdGray4RowBytes*kEpdHeight;
constexpr uint8_t kOpenCodeDetailDefault=1;
enum class ImageRenderMode { kBlackWhite, kGray16 };
enum class ProvisionState : uint8_t { kIdle,kStarting,kScanning,kConnecting,kConnected,kFailed };
enum class WordObservationCommitState { kIdle,kPersisting };
enum class NoteObservationCommitState { kIdle,kPersisting };
enum class ProblemVerdictCommitState { kIdle,kPersisting };
struct WordDeckInfo { std::string deck_id,title; };
@@MODEL@@
struct WordState {
    struct { WordObservationCommitState commit_state=WordObservationCommitState::kIdle; bool batch_flush_requested=false; } session;
    bool buffered=false;
    bool cloud_sync_failed=false;
    struct { bool has_manifest=true,pack_error=false; } pack_index;
    std::string default_deck_id,default_deck_title;
    std::vector<WordDeckInfo> deck_catalog;
};
struct NoteState { struct { NoteObservationCommitState commit_state=NoteObservationCommitState::kIdle; } session; };
struct ProblemState { ProblemVerdictCommitState commit_state=ProblemVerdictCommitState::kIdle; };
struct HomeSummary { bool wifi_connected=true,charging=false,full=false; int battery_percent=84,wifi_rssi=-52; };
struct UiState {
    UiScreen screen=UiScreen::kSettings;
    SettingsAppState settings;
    NavigationState navigation;
    HomeSummary home;
    WordState word_app;
    NoteState note_app;
    ProblemState problem_app;
};
struct UiFrame {
    UiScreen screen=UiScreen::kSettings;
    NavigationState navigation;
    bool paired=true,prefer_full_refresh=false;
    std::string claim_code,provisioning_ssid;
    ProvisionState provisioning_phase=ProvisionState::kIdle;
    HomeSummary home;
    SettingsAppState settings;
};
ProvisionState phase=ProvisionState::kIdle;
bool provisioning_active=false;
std::string ap;
ProvisionState GetProvisioningState() { return phase; }
std::string GetProvisioningApSsid() { return ap; }
bool IsProvisioningActive() { return provisioning_active; }
int LoadAutoSyncIntervalMinutes(uint32_t* value) { *value=30;return ESP_OK; }
int LoadVolumePercent(int* value) { *value=75;return ESP_OK; }
int LoadImageRenderMode(ImageRenderMode* value) { *value=ImageRenderMode::kGray16;return ESP_OK; }
bool NavigateUiToScreen(UiState* s,UiScreen screen) { s->screen=screen;return true; }
bool HasBufferedWordObservations(const WordState& s) { return s.buffered; }
int FactoryResetNvsAndRestart() { ++resets; return ESP_FAIL; }
void RequestUserPowerOff() { ++powers; }
int InvalidateWordPackManifest() { ++downloads; return ESP_OK; }
void SetDefaultWordDeck(WordState* s,const std::string& id,const std::string& title) { s->default_deck_id=id;s->default_deck_title=title; }
void ResetWordSessionsForScopeChange(WordState*,bool) {}
void SetNoteImageRenderMode(NoteState*,ImageRenderMode) {}
void SetProblemImageRenderMode(ProblemState*,ImageRenderMode) {}
void SetAiAutoFollow(bool) { ++follows; }
@@LABELS@@
namespace services {
@@CONNECTIVITY_TYPES@@
@@WIFI_API@@
@@CONNECTIVITY_SNAPSHOT@@
ConnectivitySnapshot connectivity;
ConnectivitySnapshot GetConnectivitySnapshot() {return connectivity;}
WifiSwitchSnapshot network;
uint32_t switch_id=91;
int switches=0;
WifiNetworkRole last_target=WifiNetworkRole::kPrimary;
uint32_t RequestWifiNetworkSwitch(WifiNetworkRole role) {++switches;last_target=role;network.operation_id=switch_id;network.state=WifiSwitchState::kQueued;return switch_id;}
WifiSwitchSnapshot GetWifiSwitchSnapshot() {return network;}
enum class SyncContentDomain { kWordPacks };
void RequestContentRefresh(SyncContentDomain) {}
void RequestSyncNow() { ++syncs; }
void SetConnectivityProvisioning() { ++provisions; }
void NotifyAutoSyncIntervalChanged(uint32_t) {}
}
namespace runtime {
int memory_dump_requests=0;
void RequestSleepDiagnosticsDump() {}
void RequestMemoryDiagnosticsDump() { ++memory_dump_requests; }
}
void DrawEpdPixel(int x,int y,bool ink) { if(x>=0&&x<400&&y>=0&&y<300) pixels[y*400+x]=ink; }
void ClearEpdFramebuffer(bool white) { pixels.fill(!white); drawn.clear(); }
bool ReadEpdPixel(int x,int y) { assert(x>=0&&x<400&&y>=0&&y<300);return pixels[y*400+x]!=0; }
int RefreshEpdGray16(const uint8_t* data,size_t size) {
    CHECK(size==kEpdGray4PayloadSize);++gray_refreshes;
    gray_presented.assign(data,data+size);frame_is_gray=true;return gray_result;
}
extern "C" const lv_font_t SourceHanSansSC_Regular_slim;
extern "C" bool lv_font_get_glyph_dsc_fmt_txt(const lv_font_t*,lv_font_glyph_dsc_t*,uint32_t,uint32_t) { return false; }
extern "C" const uint8_t* lv_font_get_bitmap_fmt_txt(const lv_font_t*,uint32_t) { return nullptr; }
@@FONT_LOOKUP@@
@@FONT_DRAW@@
@@FONT_TEXT@@
void DrawTextWithFont(int x,int y,const lv_font_t* f,const char* text,bool ink) {
    drawn.push_back({x,y,f,text}); DrawProductionTextWithFont(x,y,f,text,ink);
}
const lv_font_t* PrimaryUiFont() { return &SourceHanSansSC_Regular_slim; }
}

namespace device_ui_internal {
constexpr char kTag[]="settings-host";
enum class RefreshSchedule { kNone,kConfig,kSelection,kCommit,kNavigation };
struct UiRect { int x,y,width,height; const char* name; };
constexpr UiRect kSettingsContentRect={0,0,400,300,"settings-page"};
constexpr size_t kSettingsItemCount=wqn::kSettingsItemCount;
@@LAYOUT@@
@@OPTIONS@@
@@LINES@@
@@FILL@@
@@BATTERY@@
@@ICONS@@
@@OPTIONS_INDEX@@
std::string BytesLabel(size_t value) { return std::to_string(value/1024)+" KiB"; }
int regional=0,complete=0;
int RefreshStableRegion(const UiRect& r,RefreshSchedule) { CHECK(r.x==0&&r.y==0&&r.width==400&&r.height==300);++regional;frame_is_gray=false;return ESP_OK; }
int RefreshFrame(const wqn::UiFrame&,RefreshSchedule) { ++complete;frame_is_gray=false;return ESP_OK; }
int DrawNavigationOverlay(const wqn::UiFrame& frame) {
    if(nav_result!=ESP_OK)return nav_result;
    if(frame.navigation.visible)FillRect(4,214,392,78,true);
    return ESP_OK;
}
esp_err_t RenderProvisioningToEpd(const wqn::UiFrame&,RefreshSchedule);
void UpdateSettingsDiagnostics(wqn::UiState*) { ++reads; }
void RebuildNoteWordDeckRows(wqn::UiState*) {}
void BuildHomeSummary(wqn::UiState*) {}
bool AnyLocalPersistPending(const wqn::UiState&);
bool pack_busy=false;
bool IsWordPackCloudBusy() { return pack_busy; }
}  // namespace device_ui_internal
@@PAGE@@
@@PROVISION_PAGE@@

namespace device_ui_internal {
enum class PersistKind { kSettingsAutoSync,kSettingsVolume,kSettingsImageRender,kSettingsDefaultDeck,kSettingsAiFollow };
bool IsAnyPersistBusy() { return persist_busy; }
bool IsPersistKindBusy(PersistKind) { return persist_busy; }
uint32_t SubmitAutoSyncIntervalSave(uint32_t) { ++submits;return submit_id; }
uint32_t SubmitVolumeSave(int) { ++submits;return submit_id; }
uint32_t SubmitImageRenderModeSave(wqn::ImageRenderMode) { ++submits;return submit_id; }
uint32_t SubmitDefaultDeckChange(const std::string&) { ++submits;return submit_id; }
uint32_t SubmitAiFollowSave(bool) { ++submits;return submit_id; }
bool QueueWordReviewRefresh() { return true; }
bool IsWordCloudBusy() { return false; }
@@INPUT@@
bool CanOpenNavigation(const wqn::UiState&) { return false; }
RefreshSchedule FinishScreenNavigation(wqn::UiState*,wqn::UiScreen) { return RefreshSchedule::kNone; }
@@ROUTE@@
@@DURABLE_RELOAD@@
wqn::UiFrame ProjectProvisioning(const wqn::UiState& state) {
    using wqn::UiScreen;
    wqn::UiFrame frame;
@@PROVISION_PROJECT@@
    return frame;
}
std::string SettingsSignature(const wqn::UiFrame& frame) {
@@SIGNATURE_HEADER@@
@@SIGNATURE@@
    return signature;
}
enum class AppEventKind { kSettingsPersist,kSettingsTick };
struct UiUpdate { RefreshSchedule refresh; bool changed; };
class UiRuntime {
public:
    wqn::UiState state_;
    wqn::ProvisionState settings_provision_phase_=wqn::ProvisionState::kIdle;
    bool settings_provision_active_=false;
    std::string settings_provision_ssid_;
    UiUpdate FinishEvent(AppEventKind,RefreshSchedule r,bool c) { return {r,c}; }
    UiUpdate DispatchSettingsTick(int64_t);
    UiUpdate DispatchAutoSyncSaveResult(int,uint32_t);
    UiUpdate DispatchVolumeSaveResult(int,uint32_t);
    UiUpdate DispatchImageRenderSaveResult(int,uint32_t);
    UiUpdate DispatchDefaultDeckChangeResult(int,uint32_t);
    UiUpdate DispatchAiFollowSaveResult(int,uint32_t);
};
@@TICK@@
@@AUTO_ACK@@
@@OTHER_ACK@@
}

using namespace device_ui_internal;
using Dialog=wqn::SettingsDialog;
using Button=wqn::ButtonId;
using Event=wqn::ButtonEventType;
int64_t clock_ms=1000;
void event(wqn::UiState& state,Button button,Event type,bool repeat=false,int64_t advance=400) {
    clock_ms+=advance;
    wqn::ButtonEvent e{button,type,0,repeat,clock_ms,0};
    ApplySettingsButtonEvent(e,&state);
}
bool has_text(const std::string& value) { return std::any_of(drawn.begin(),drawn.end(),[&](const auto& r){return r.text==value;}); }
void save_frame(const std::string& directory,const std::string& name) {
    if(directory.empty())return;
    if(frame_is_gray) {
        std::ofstream f(directory+"/"+name+".pgm",std::ios::binary);f<<"P5\n400 300\n255\n";
        for(auto byte:gray_presented) { f.put(static_cast<char>((byte>>4)*17));f.put(static_cast<char>((byte&15)*17)); }
        return;
    }
    std::ofstream f(directory+"/"+name+".pbm",std::ios::binary);f<<"P4\n400 300\n";
    for(int y=0;y<300;++y)for(int x=0;x<400;x+=8){unsigned char b=0;for(int k=0;k<8;++k)b|=pixels[y*400+x+k]<<(7-k);f.put(static_cast<char>(b));}
}

int main(int argc,char** argv) {
    const std::string directory=argc>1?argv[1]:"";
    CHECK(wqn_ui_sans_12.line_height==12&&wqn_ui_sans_24.line_height==24);
    CHECK(wqn::MeasureTextWithFont(&wqn_ui_sans_12,"墨水屏")==36);
    CHECK(wqn::MeasureTextWithFont(&wqn_ui_sans_24,"墨水屏")==72);
    CHECK(wqn::MeasureTextWithFont(&wqn_ui_sans_20,"墨水屏")==60);
    CHECK(wqn_ui_sans_12.dsc->bpp==1&&wqn_ui_sans_24.dsc->bitmap_format==0);
    wqn::UiFrame f;auto& d=f.settings.diagnostics;
    d.battery_valid=d.flash_valid=d.nvs_valid=d.psram_valid=true;d.battery_percent=84;
    d.adc_raw=2386;d.adc_mv=1084;d.battery_mv=4021;d.flash_size=16777216;
    d.nvs_used_entries=73;d.nvs_total_entries=256;d.psram_total=8388608;d.psram_used=4299161;
    d.psram_free=d.psram_total-d.psram_used;d.firmware_version="v0.1.0-live";d.idf_target="esp32s3";d.idf_version="v5.5.4";
    d.git_commit="cbf81166";d.build_time="2026-10-17 16:00";d.reset_reason_label="power-on";d.uptime_label="2h18m";
    d.heap_free=172032;d.heap_min_free=143360;d.content_sync_label="W 4/4 N 6/6 P 8/8";
    for(size_t i=0;i<d.sync_diag_lines.size();++i)d.sync_diag_lines[i]="内容域 已同步 重试0";
    f.settings.auto_sync_interval_min=30;f.settings.volume_percent=75;
    std::snprintf(f.settings.wifi_primary_ssid,33,"Study_Room_2G");std::snprintf(f.settings.wifi_backup_ssid,33,"Home_Backup");
    for(size_t i=0;i<wqn::kSettingsItemCount;++i){f.settings.selected=i;CHECK(RenderSettingsToEpd(f,RefreshSchedule::kSelection)==ESP_OK);}
    f.settings.selected=0;RenderSettingsToEpd(f,RefreshSchedule::kCommit);save_frame(directory,"root");
    CHECK(has_text("设置"));CHECK(has_text("WiFi 管理"));CHECK(!has_text("确认进入"));
    CHECK(std::all_of(pixels.begin()+27*400,pixels.begin()+28*400,[](auto p){return p==1;}));
    CHECK(std::any_of(drawn.begin(),drawn.end(),[](const auto& r){return r.text=="设备设置 · 已配对"&&r.x==10&&r.y==6;}));
    CHECK(std::any_of(drawn.begin(),drawn.end(),[](const auto& r){return r.text=="WiFi 管理"&&r.x<=40&&r.font->line_height==20;}));
    CHECK(std::any_of(drawn.begin(),drawn.end(),[](const auto& r){return r.text=="已连接"&&r.font->line_height>=16;}));
#if CONFIG_WQN_DEV_MENU_ENABLE
    f.settings.view=wqn::SettingsView::kDev;
    for(size_t i=0;i<wqn::kDevItemCount;++i){f.settings.dev_selected=i;CHECK(RenderSettingsToEpd(f,RefreshSchedule::kSelection)==ESP_OK);}
    CHECK(has_text("睡眠诊断"));save_frame(directory,"dev-menu");f.settings.view=wqn::SettingsView::kRoot;
#endif
    const std::array<std::pair<Dialog,const char*>,17> pages={{{Dialog::kWifiManage,"wifi"},{Dialog::kAutoSync,"auto"},{Dialog::kVolume,"volume"},{Dialog::kImageRendering,"image"},{Dialog::kAiFollow,"follow"},{Dialog::kDefaultWordDeck,"deck"},{Dialog::kAbout,"about"},{Dialog::kPairing,"pairing"},{Dialog::kBatterySummary,"battery"},{Dialog::kBattery,"battery-raw"},{Dialog::kStorage,"storage"},{Dialog::kDevInfo,"dev"},{Dialog::kDevSync,"sync"},{Dialog::kDevErrors,"errors"},{Dialog::kSleepDiag,"sleep"},{Dialog::kFactoryReset,"reset"},{Dialog::kPowerOff,"power"}}};
    f.settings.word_deck_options={{"","全部词库"},{"a","高中英语必修第一册"},{"b","大学英语四级核心词汇"},{"c","考研英语高频词汇"},{"d","学术阅读与专业词汇长标题示例词库（完整名称超过一行）"},{"e","日常交流词汇"},{"f","阅读中遇到的生词"}};
    for(const auto& entry:pages){f.settings.dialog=entry.first;CHECK(RenderSettingsToEpd(f,RefreshSchedule::kConfig)==ESP_OK);CHECK(std::all_of(pixels.begin()+27*400,pixels.begin()+28*400,[](auto p){return p==1;}));save_frame(directory,entry.second);}
    CHECK(has_text("确认键无法重新开机"));
    f.settings.dialog=Dialog::kDefaultWordDeck;
    for(const auto status:{wqn::SettingsDeckStatus::kEmpty,wqn::SettingsDeckStatus::kLoading,wqn::SettingsDeckStatus::kFailed}){
        f.settings.word_deck_status=status;RenderSettingsToEpd(f,RefreshSchedule::kConfig);save_frame(directory,"deck-state-"+std::to_string(static_cast<int>(status)));
    }
    f.settings.word_deck_status=wqn::SettingsDeckStatus::kReady;
    f.settings.dialog=Dialog::kProvisioningResult;
    f.provisioning_phase=wqn::ProvisionState::kFailed;
    RenderSettingsToEpd(f,RefreshSchedule::kConfig);
    CHECK(has_text("未能连接 WiFi")&&has_text("未连接"));
    save_frame(directory,"provision-failed");
    f.settings.wifi_backup_ssid[0]=0;
    RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(has_text("未配置"));
    std::snprintf(f.settings.wifi_backup_ssid,33,"Home_Backup");
    f.provisioning_phase=wqn::ProvisionState::kConnecting;
    RenderSettingsToEpd(f,RefreshSchedule::kConfig);
    CHECK(has_text("正在连接 WiFi")&&!has_text("WiFi 已连接"));
    CHECK(has_text("Study_Room_2G")&&has_text("Home_Backup"));
    save_frame(directory,"provision-connecting");
    f.provisioning_phase=wqn::ProvisionState::kConnected;
    std::snprintf(f.settings.wifi_active_ssid,33,"Home_Backup");
    RenderSettingsToEpd(f,RefreshSchedule::kConfig);
    CHECK(has_text("WiFi 已连接")&&has_text("Home_Backup")&&!has_text("Study_Room_2G"));
    save_frame(directory,"provision-connected");
    f.provisioning_phase=wqn::ProvisionState::kIdle;
    f.settings.dialog=Dialog::kAbout;RenderSettingsToEpd(f,RefreshSchedule::kConfig);
    CHECK(has_text("rongNoteBook")&&has_text("v0.1.0-live")&&has_text("ESP-IDF v5.5.4"));
    CHECK(std::all_of(pixels.begin()+274*400,pixels.end(),[](auto p){return p==0;}));
    // Real native text is composed into 4bpp; intermediate tones are confined
    // to the original logo bounds, and native ink wins in the overlap.
    const auto gray_level=[](int x,int y) {
        const size_t pixel=static_cast<size_t>(y)*400+x;
        return (pixel&1U)?gray_presented[pixel/2]&15:gray_presented[pixel/2]>>4;
    };
    std::array<bool,16> tones{};
    bool composed_correctly=true;
    for(int y=0;y<300;++y)for(int x=0;x<400;++x) {
        const int actual=gray_level(x,y);
        const bool logo=x>=kSettingsDetailX&&x<kSettingsDetailX+28&&y>=37&&y<65;
        int expected=pixels[y*400+x]?0:15;
        if(logo&&!pixels[y*400+x]) {
            const int source=(y-37)*28+x-kSettingsDetailX;
            expected=(source&1)?kWLogoGray4[source/2]&15:kWLogoGray4[source/2]>>4;
        }
        if(actual!=expected)composed_correctly=false;
        if(logo)tones[actual]=true;
    }
    CHECK(composed_correctly);
    CHECK(std::count(tones.begin()+1,tones.end()-1,true)>=8);
    CHECK(allocations==releases);
    const auto native_about=pixels;
    const auto plain_about=gray_presented;
    const auto about_signature=SettingsSignature(f);
    auto background=f;
    background.settings.selected=1;background.settings.dev_selected=2;
    background.settings.notice="后台状态";background.settings.sync_status="同步中";
    background.settings.diagnostics.uptime_label="2h19m";
    background.settings.diagnostics.heap_free++;
    background.settings.diagnostics.psram_free--;
    background.settings.diagnostics.adc_raw++;
    background.settings.diagnostics.sync_diag_lines[0]="后台同步";
    background.home.wifi_rssi=-53;background.home.battery_percent=83;
    CHECK(SettingsSignature(background)==about_signature);
    CHECK(RenderSettingsToEpd(background,RefreshSchedule::kSelection)==ESP_OK);
    CHECK(gray_presented==plain_about&&pixels==native_about);
    for(const auto change:{0,1,2,3,4,5,6,7}) {
        auto visible=f;
        switch(change) {
            case 0:visible.settings.diagnostics.firmware_version="new-version";break;
            case 1:visible.settings.diagnostics.idf_target="other-chip";break;
            case 2:visible.settings.diagnostics.idf_version="new-idf";break;
            case 3:visible.home.wifi_rssi=-60;break;
            case 4:visible.home.wifi_connected=false;break;
            case 5:visible.home.battery_percent=60;break;
            case 6:visible.home.charging=true;break;
            case 7:visible.navigation.visible=true;break;
        }
        CHECK(SettingsSignature(visible)!=about_signature);
        CHECK(RenderSettingsToEpd(visible,RefreshSchedule::kConfig)==ESP_OK);
        CHECK(gray_presented!=plain_about);
    }
    auto full_battery=f;full_battery.home.full=true;
    CHECK(SettingsSignature(full_battery)==about_signature);
    auto fallback=f;fallback.settings.diagnostics.firmware_version.clear();
    const auto fallback_signature=SettingsSignature(fallback);
    fallback.settings.diagnostics.firmware_version=WQN_FIRMWARE_VERSION;
    CHECK(SettingsSignature(fallback)==fallback_signature);
    f.navigation.visible=true;f.navigation.selected=0;
    const auto nav_signature=SettingsSignature(f);
    f.navigation.selected=1;CHECK(SettingsSignature(f)!=nav_signature);
    const int old_regional=regional,old_complete=complete,old_gray=gray_refreshes;
    for(const auto schedule:{RefreshSchedule::kConfig,RefreshSchedule::kSelection,
                             RefreshSchedule::kNavigation,RefreshSchedule::kCommit}) {
        CHECK(RenderSettingsToEpd(f,schedule)==ESP_OK);
        CHECK(gray_level(10,220)==0);
    }
    CHECK(regional==old_regional&&complete==old_complete&&gray_refreshes==old_gray+4);
    f.navigation.visible=false;f.navigation.selected=0;
    CHECK(RenderSettingsToEpd(f,RefreshSchedule::kNavigation)==ESP_OK);
    CHECK(gray_presented==plain_about);
    const int before_failure=gray_refreshes;
    allocation_allowed=false;
    CHECK(RenderSettingsToEpd(f,RefreshSchedule::kConfig)==ESP_ERR_NO_MEM);
    CHECK(gray_refreshes==before_failure&&allocations==releases);
    allocation_allowed=true;nav_result=ESP_FAIL;
    CHECK(RenderSettingsToEpd(f,RefreshSchedule::kNavigation)==ESP_FAIL);
    CHECK(gray_refreshes==before_failure&&allocations==releases);
    nav_result=ESP_OK;gray_result=ESP_FAIL;
    CHECK(RenderSettingsToEpd(f,RefreshSchedule::kCommit)==ESP_FAIL);
    CHECK(allocations==releases);
    gray_result=ESP_OK;f.settings.image_render_mode=wqn::ImageRenderMode::kBlackWhite;
    CHECK(RenderSettingsToEpd(f,RefreshSchedule::kCommit)==ESP_OK&&frame_is_gray);
    save_frame(directory,"about");
    f.settings.image_render_mode=wqn::ImageRenderMode::kGray16;
    f.settings.dialog=Dialog::kNone;
    CHECK(RenderSettingsToEpd(f,RefreshSchedule::kConfig)==ESP_OK&&!frame_is_gray);
    CHECK(regional==old_regional+1);
    f.settings.dialog=Dialog::kImageRendering;f.settings.image_render_selected=0;
    RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(pixels[178*400+11]==1);CHECK(pixels[116*400+11]==0);
    f.settings.dialog=Dialog::kDefaultWordDeck;f.settings.word_deck_selected=6;RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(has_text("阅读中遇到的生词"));save_frame(directory,"deck-scrolled");
    f.settings.word_deck_options[6].title="第一行\n第二行";
    RenderSettingsToEpd(f,RefreshSchedule::kConfig); CHECK(has_text("第一行 第二行"));
    f.settings.dialog=Dialog::kBatterySummary;d.battery_valid=false;RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(has_text("--")&&has_text("暂未取得电量读数"));save_frame(directory,"battery-unknown");
    d.battery_valid=true;d.full=true;d.external_power_present=false;RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(has_text("未充电，运行正常"));d.external_power_present=true;RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(has_text("电池已充满"));save_frame(directory,"battery-full");d.full=false;d.charging=true;RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(has_text("正在充电"));save_frame(directory,"battery-charging");d.charging=false;d.external_power_present=false;
    f.settings.notice="设置未保存，请重试";RenderSettingsToEpd(f,RefreshSchedule::kConfig);CHECK(has_text(f.settings.notice));save_frame(directory,"feedback");
    f.settings.dialog=Dialog::kDefaultWordDeck;f.settings.word_deck_selected=0;
    auto signature=SettingsSignature(f);
    f.settings.word_deck_options[1].title+="更名";
    CHECK(signature!=SettingsSignature(f));
    signature=SettingsSignature(f);d.nvs_used_entries++;
    CHECK(signature!=SettingsSignature(f));
    signature=SettingsSignature(f);d.battery_valid=!d.battery_valid;
    CHECK(signature!=SettingsSignature(f));
    f.navigation.visible=true;int before=complete;RenderSettingsToEpd(f,RefreshSchedule::kNavigation);CHECK(complete==before+1);f.navigation.visible=false;
    f.settings.notice.clear();f.screen=wqn::UiScreen::kProvisioning;f.provisioning_phase=wqn::ProvisionState::kScanning;f.provisioning_ssid="WQN_N4_LIVE";RenderProvisioningToEpd(f,RefreshSchedule::kCommit);CHECK(has_text("WQN_N4_LIVE")&&has_text("192.168.4.1"));save_frame(directory,"provision");
    CHECK(has_text("rongNoteBook")&&has_text("Note4")&&has_text("等待配置"));
    CHECK(!has_text("设备配网")&&!has_text("设备设置 · 已配对"));
    CHECK(std::all_of(pixels.begin(),pixels.begin()+16*400,[](auto p){return p==0;}));
    CHECK(std::all_of(pixels.begin()+278*400,pixels.end(),[](auto p){return p==0;}));
    CHECK(!std::all_of(pixels.begin()+27*400,pixels.begin()+28*400,[](auto p){return p==1;}));
    for(const auto& sample:std::array<std::pair<const char*,std::string>,3>{{
        {"provision-escaped","Note;4,Lab:\\\"'"},
        {"provision-unicode","设备热点-Note4"},
        {"provision-max",std::string(32,';')}
    }}) {
        f.provisioning_ssid=sample.second;
        RenderProvisioningToEpd(f,RefreshSchedule::kCommit);
        CHECK(!has_text("请手动连接热点"));save_frame(directory,sample.first);
    }
    f.provisioning_ssid.clear();RenderProvisioningToEpd(f,RefreshSchedule::kCommit);
    CHECK(has_text("正在启动设备热点")&&!has_text("192.168.4.1"));
    save_frame(directory,"provision-starting");
    f.provisioning_phase=wqn::ProvisionState::kStarting;f.provisioning_ssid="WQN_PENDING";
    RenderProvisioningToEpd(f,RefreshSchedule::kCommit);
    CHECK(has_text("正在启动设备热点")&&has_text("WQN_PENDING"));
    const auto provision_signature=SettingsSignature(f);
    std::snprintf(f.settings.wifi_active_ssid,33,"NewLink");
    CHECK(SettingsSignature(f)!=provision_signature);

    UiRuntime runtime;auto& state=runtime.state_;auto& s=state.settings;
#if CONFIG_WQN_DEV_MENU_ENABLE
    s.view=wqn::SettingsView::kDev;s.dev_selected=wqn::kDevRowDevInfo;
    const int dump_requests=wqn::runtime::memory_dump_requests;
    event(state,Button::kConfirm,Event::kShortPress);
    CHECK(s.dialog==Dialog::kDevInfo&&wqn::runtime::memory_dump_requests==dump_requests+1);
    event(state,Button::kUp,Event::kShortPress);
    event(state,Button::kConfirm,Event::kShortPress);
    CHECK(s.dialog==Dialog::kNone&&wqn::runtime::memory_dump_requests==dump_requests+1);
    event(state,Button::kConfirm,Event::kShortPress);
    CHECK(wqn::runtime::memory_dump_requests==dump_requests+2);
    state=wqn::UiState{};
#endif
    state.settings.selected=wqn::kSettingsRowAbout;event(state,Button::kConfirm,Event::kShortPress);CHECK(s.dialog==Dialog::kAbout);event(state,Button::kConfirm,Event::kShortPress);CHECK(s.dialog==Dialog::kNone);
    s.selected=wqn::kSettingsRowBattery;event(state,Button::kConfirm,Event::kShortPress);CHECK(s.dialog==Dialog::kBatterySummary);event(state,Button::kConfirm,Event::kLongPress);CHECK(s.dialog==Dialog::kNone);
    s.selected=0;event(state,Button::kUp,Event::kShortPress);CHECK(s.selected==wqn::kSettingsItemCount-1);event(state,Button::kDownPower,Event::kShortPress);event(state,Button::kDownPower,Event::kDoublePress);CHECK(s.selected==1);
    s.selected=wqn::kSettingsRowWifi;persist_busy=true;before=reads;event(state,Button::kConfirm,Event::kShortPress);CHECK(reads==before&&s.dialog==Dialog::kNone);persist_busy=false;
    state.note_app.session.commit_state=wqn::NoteObservationCommitState::kPersisting;event(state,Button::kConfirm,Event::kShortPress);CHECK(reads==before);state.note_app.session.commit_state=wqn::NoteObservationCommitState::kIdle;
    s.dialog=Dialog::kFactoryReset;state.problem_app.commit_state=wqn::ProblemVerdictCommitState::kPersisting;before=resets;event(state,Button::kConfirm,Event::kLongPress);CHECK(resets==before);state.problem_app.commit_state=wqn::ProblemVerdictCommitState::kIdle;
    s.dialog=Dialog::kPowerOff;state.word_app.buffered=true;before=powers;event(state,Button::kConfirm,Event::kLongPress);CHECK(powers==before&&state.word_app.session.batch_flush_requested);state.word_app.buffered=false;event(state,Button::kConfirm,Event::kShortPress);CHECK(s.dialog==Dialog::kNone&&powers==before);
    OpenSettingsDialog(&state,Dialog::kWifiManage);for(int i=0;i<3;++i)event(state,Button::kDownPower,Event::kShortPress);CHECK(s.wifi_action_selected==3);event(state,Button::kConfirm,Event::kShortPress);CHECK(s.dialog==Dialog::kPairing);event(state,Button::kConfirm,Event::kShortPress);CHECK(s.dialog==Dialog::kWifiManage);for(int i=0;i<2;++i)event(state,Button::kDownPower,Event::kShortPress);before=provisions;event(state,Button::kConfirm,Event::kShortPress);CHECK(provisions==before+1);
    persist_busy=true;before=reads;OpenSettingsDialog(&state,Dialog::kPairing);CHECK(reads==before&&s.dialog==Dialog::kNone);persist_busy=false;
    const std::array<Dialog,5> choices={Dialog::kAutoSync,Dialog::kVolume,Dialog::kImageRendering,Dialog::kAiFollow,Dialog::kDefaultWordDeck};
    for(auto dialog:choices){
        state=wqn::UiState{};s.auto_sync_interval_min=30;s.volume_percent=75;state.word_app.deck_catalog={{"live","真实词库"}};
        OpenSettingsDialog(&state,dialog);event(state,Button::kDownPower,Event::kShortPress);const int saved_submits=submits;event(state,Button::kConfirm,Event::kLongPress);CHECK(s.dialog==Dialog::kNone&&submits==saved_submits);
        OpenSettingsDialog(&state,dialog);if(dialog==Dialog::kAutoSync)s.auto_sync_selected=0;if(dialog==Dialog::kVolume)s.volume_selected=0;if(dialog==Dialog::kImageRendering)s.image_render_selected=0;if(dialog==Dialog::kAiFollow)s.ai_follow_selected=1;if(dialog==Dialog::kDefaultWordDeck)s.word_deck_selected=1;
        event(state,Button::kConfirm,Event::kShortPress);CHECK(submits==saved_submits+1);CHECK(s.auto_sync_interval_min==30&&s.volume_percent==75&&s.auto_follow);
        const auto ack=[&](int result,uint32_t id){switch(dialog){case Dialog::kAutoSync:runtime.DispatchAutoSyncSaveResult(result,id);break;case Dialog::kVolume:runtime.DispatchVolumeSaveResult(result,id);break;case Dialog::kImageRendering:runtime.DispatchImageRenderSaveResult(result,id);break;case Dialog::kAiFollow:runtime.DispatchAiFollowSaveResult(result,id);break;default:runtime.DispatchDefaultDeckChangeResult(result,id);}};
        event(state,Button::kConfirm,Event::kShortPress);CHECK(submits==saved_submits+1);
        ack(ESP_OK,submit_id+1);CHECK(s.notice=="正在保存，请稍候");ack(ESP_FAIL,submit_id);CHECK(s.notice.find("保存")!=std::string::npos);
        OpenSettingsDialog(&state,dialog);event(state,Button::kConfirm,Event::kShortPress);ack(ESP_OK,submit_id);
        CHECK(!s.auto_sync_pending_valid&&!s.volume_pending_valid&&!s.image_render_pending_valid&&!s.auto_follow_pending_valid&&!s.word_deck_pending_valid);
        if(dialog==Dialog::kAutoSync) CHECK(s.auto_sync_interval_min==0);
        if(dialog==Dialog::kVolume) CHECK(s.volume_percent==0);
        if(dialog==Dialog::kImageRendering) CHECK(s.image_render_mode==wqn::ImageRenderMode::kBlackWhite);
        if(dialog==Dialog::kAiFollow) CHECK(!s.auto_follow);
        if(dialog==Dialog::kDefaultWordDeck) CHECK(s.default_word_deck_id=="live");
    }
    state=wqn::UiState{};OpenSettingsDialog(&state,Dialog::kAutoSync);submit_id=0;event(state,Button::kConfirm,Event::kShortPress);CHECK(s.auto_sync_pending_valid&&!s.auto_sync_save_op_id);OpenSettingsDialog(&state,Dialog::kAutoSync);event(state,Button::kConfirm,Event::kLongPress);CHECK(!s.auto_sync_pending_valid);submit_id=71;
    state.word_app.pack_index.has_manifest=false;OpenSettingsDialog(&state,Dialog::kDefaultWordDeck);CHECK(s.word_deck_status==wqn::SettingsDeckStatus::kEmpty);pack_busy=true;UpdateSettingsDeckStatus(&state);CHECK(s.word_deck_status==wqn::SettingsDeckStatus::kLoading);pack_busy=false;state.word_app.cloud_sync_failed=true;UpdateSettingsDeckStatus(&state);CHECK(s.word_deck_status==wqn::SettingsDeckStatus::kFailed);
    state.word_app.deck_catalog={{"a","甲"},{"b","乙"}};InstallSettingsDeckOptions(&state,false);s.word_deck_selected=2;state.word_app.deck_catalog={{"b","乙改名"},{"a","甲"}};InstallSettingsDeckOptions(&state,true);CHECK(s.word_deck_selected==1&&s.word_deck_options[1].title=="乙改名");
    OpenSettingsDialog(&state,Dialog::kDevSync);event(state,Button::kConfirm,Event::kShortPress);CHECK(s.sync_confirm_pending&&s.dialog==Dialog::kDevSync);runtime.DispatchSettingsTick(clock_ms+299);CHECK(s.dialog==Dialog::kDevSync);runtime.DispatchSettingsTick(clock_ms+301);CHECK(s.dialog==Dialog::kNone);
    OpenSettingsDialog(&state,Dialog::kDevSync);event(state,Button::kConfirm,Event::kShortPress);before=downloads;event(state,Button::kConfirm,Event::kDoublePress,false,100);CHECK(downloads==before+1&&s.dialog==Dialog::kDevSync);event(state,Button::kConfirm,Event::kLongPress);CHECK(s.dialog==Dialog::kNone);
    OpenSettingsDialog(&state,Dialog::kDevSync);event(state,Button::kConfirm,Event::kShortPress);persist_busy=true;before=downloads;event(state,Button::kConfirm,Event::kDoublePress,false,100);CHECK(downloads==before);persist_busy=false;
    OpenSettingsDialog(&state,Dialog::kVolume);event(state,Button::kConfirm,Event::kLongPress);before=static_cast<int>(s.selected);event(state,Button::kConfirm,Event::kLongPress,true,260);CHECK(state.screen==wqn::UiScreen::kSettings&&s.selected==static_cast<size_t>(before));event(state,Button::kConfirm,Event::kLongRelease);CHECK(state.screen==wqn::UiScreen::kSettings);
    wqn::NavigationState gesture;gesture.guard_confirm_double=true;gesture.confirmed_at_ms=1000;wqn::ButtonEvent tap{Button::kConfirm,Event::kPress,0,false,1100,0};CHECK(wqn::ReduceNavigationInput(&gesture,wqn::UiScreen::kSettings,tap,1100,false)==wqn::NavigationAction::kConsumed);
    state=wqn::UiState{};s.selected=wqn::kSettingsRowAutoSync;
    const auto route=[&](Event type,int64_t when,bool repeat=false){wqn::ButtonEvent e{Button::kConfirm,type,0,repeat,when,0};return ApplyButtonEvent(e,when,&state);};
    route(Event::kPress,2000);route(Event::kShortPress,2100);route(Event::kRelease,2110);CHECK(s.dialog==Dialog::kAutoSync);
    before=submits;route(Event::kPress,2200);route(Event::kShortPress,2250);route(Event::kRelease,2260);CHECK(s.dialog==Dialog::kAutoSync&&submits==before);
    route(Event::kPress,3000);route(Event::kLongPress,3650);CHECK(s.dialog==Dialog::kNone);route(Event::kLongPress,3910,true);route(Event::kLongRelease,4000);route(Event::kRelease,4010);CHECK(state.screen==wqn::UiScreen::kSettings);
    OpenSettingsDialog(&state,Dialog::kDevSync);state.navigation={};route(Event::kPress,5000);route(Event::kShortPress,5100);route(Event::kRelease,5110);route(Event::kPress,5200);before=downloads;route(Event::kDoublePress,5250);route(Event::kRelease,5260);CHECK(downloads==before+1);
    s.dialog=Dialog::kNone;s.selected=wqn::kSettingsRowAbout;wqn::provisioning_active=true;before=reads;route(Event::kPress,6000);route(Event::kShortPress,6100);route(Event::kRelease,6110);CHECK(s.dialog==Dialog::kNone&&reads==before);
    wqn::phase=wqn::ProvisionState::kStarting;CHECK(ProjectProvisioning(state).screen==wqn::UiScreen::kProvisioning);
    std::snprintf(s.wifi_primary_ssid,33,"ProjectedPrimary");
    CHECK(std::string(ProjectProvisioning(state).settings.wifi_primary_ssid)=="ProjectedPrimary");
    wqn::provisioning_active=false;wqn::phase=wqn::ProvisionState::kConnected;CHECK(ProjectProvisioning(state).screen==wqn::UiScreen::kSettings);wqn::phase=wqn::ProvisionState::kIdle;
    s.auto_sync_selected=4;s.volume_selected=1;s.image_render_selected=0;RefreshDurableSelections(&state);CHECK(s.auto_sync_selected==4&&s.volume_selected==1&&s.image_render_selected==0);
    s.dialog=Dialog::kAutoSync;s.auto_sync_selected=0;s.notice="保存失败";
    wqn::ButtonEvent boundary{Button::kUp,Event::kShortPress,0,false,6500,0};
    CHECK(ApplySettingsButtonEvent(boundary,&state)==RefreshSchedule::kConfig&&s.notice.empty());
    wqn::phase=wqn::ProvisionState::kStarting;runtime.DispatchSettingsTick(clock_ms);wqn::phase=wqn::ProvisionState::kScanning;wqn::ap="WQN_N4_REAL";CHECK(runtime.DispatchSettingsTick(clock_ms).refresh==RefreshSchedule::kConfig);wqn::phase=wqn::ProvisionState::kFailed;runtime.DispatchSettingsTick(clock_ms);CHECK(s.dialog==Dialog::kProvisioningResult);event(state,Button::kConfirm,Event::kLongPress);CHECK(s.dialog==Dialog::kWifiManage);wqn::phase=wqn::ProvisionState::kIdle;wqn::ap.clear();
    auto& snapshot=wqn::services::connectivity;
    std::snprintf(snapshot.primary_ssid,33,"RealPrimary");
    std::snprintf(snapshot.backup_ssid,33,"RealBackup");
    snapshot.active_ssid[0]=0;
    const int reads_before=reads,submits_before=submits,provisions_before=provisions;
    wqn::phase=wqn::ProvisionState::kConnecting;
    CHECK(runtime.DispatchSettingsTick(clock_ms).refresh==RefreshSchedule::kConfig);
    CHECK(s.dialog==Dialog::kProvisioningResult);
    CHECK(std::string(s.wifi_primary_ssid)=="RealPrimary"&&std::string(s.wifi_backup_ssid)=="RealBackup");
    CHECK(runtime.DispatchSettingsTick(clock_ms).refresh==RefreshSchedule::kNone);
    // Late current-link identity must repaint even when the phase is unchanged.
    std::snprintf(snapshot.active_ssid,33,"RealBackup");
    CHECK(runtime.DispatchSettingsTick(clock_ms).refresh==RefreshSchedule::kConfig);
    CHECK(std::string(s.wifi_active_ssid)=="RealBackup");
    CHECK(reads==reads_before&&submits==submits_before&&provisions==provisions_before);
    wqn::phase=wqn::ProvisionState::kIdle;
    using Switch=wqn::services::WifiSwitchState;
    OpenSettingsDialog(&state,Dialog::kWifiManage);
    std::snprintf(s.wifi_primary_ssid,33,"Primary");s.wifi_backup_ssid[0]=0;
    event(state,Button::kDownPower,Event::kShortPress);before=wqn::services::switches;
    event(state,Button::kConfirm,Event::kShortPress);CHECK(wqn::services::switches==before&&s.notice=="尚未配置备用 WiFi");
    std::snprintf(s.wifi_backup_ssid,33,"Backup");
    state.word_app.buffered=true;event(state,Button::kConfirm,Event::kShortPress);CHECK(wqn::services::switches==before);state.word_app.buffered=false;
    event(state,Button::kConfirm,Event::kShortPress);CHECK(s.wifi_switch_op_id==91&&wqn::services::last_target==wqn::services::WifiNetworkRole::kBackup);
    before=wqn::services::switches;event(state,Button::kConfirm,Event::kShortPress);CHECK(wqn::services::switches==before);
    wqn::services::network.operation_id=90;wqn::services::network.state=Switch::kConnected;
    runtime.DispatchSettingsTick(clock_ms);CHECK(s.wifi_switch_op_id==91);
    wqn::services::network.operation_id=91;wqn::services::network.state=Switch::kRestored;
    wqn::services::network.online=true;std::snprintf(wqn::services::network.active_ssid,33,"Primary");
    CHECK(runtime.DispatchSettingsTick(clock_ms).refresh==RefreshSchedule::kConfig);
    CHECK(s.wifi_switch_op_id==0&&s.notice=="目标连接失败，已回退到另一网络"&&s.wifi_online&&std::string(s.wifi_active_ssid)=="Primary");
    event(state,Button::kUp,Event::kShortPress);CHECK(s.wifi_action_selected==0);
    event(state,Button::kUp,Event::kShortPress);CHECK(s.wifi_action_selected==3);
    event(state,Button::kDownPower,Event::kShortPress);CHECK(s.wifi_action_selected==0);
    const auto signature_before=SettingsSignature(wqn::UiFrame{});
    wqn::UiFrame network_frame;network_frame.settings.wifi_online=true;std::snprintf(network_frame.settings.wifi_active_ssid,33,"Primary");
    CHECK(SettingsSignature(network_frame)!=signature_before);
    network_frame.settings=s;network_frame.settings.notice.clear();network_frame.settings.dialog=Dialog::kWifiManage;
    RenderSettingsToEpd(network_frame,RefreshSchedule::kConfig);CHECK(has_text("连接主 WiFi")&&has_text("连接备用 WiFi")&&has_text("已连接"));save_frame(directory,"wifi-switch");
    std::printf("Settings dev=%d: %d checks PASS; production input/ACK/signature/native pixels, host ports only\n",CONFIG_WQN_DEV_MENU_ENABLE,checks);
}
