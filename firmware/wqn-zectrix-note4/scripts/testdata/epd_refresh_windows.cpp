#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=1, ESP_ERR_INVALID_ARG=2, ESP_ERR_INVALID_STATE=3;
#define ESP_RETURN_ON_ERROR(expr,...) do { const auto e=(expr); if(e!=ESP_OK)return e; } while(0)
#define ESP_RETURN_ON_FALSE(expr,e,...) do { if(!(expr))return e; } while(0)
template<class... T> void HostLog(T...) {}
#define ESP_LOGW(...) HostLog(__VA_ARGS__)
#define ESP_LOGI(...) HostLog(__VA_ARGS__)
constexpr char kTag[]="epd-window-host";
constexpr int kEpdWidth=400, kEpdHeight=300, kEpdBytesPerRow=50;
constexpr int kEpdFramebufferSize=kEpdBytesPerRow*kEpdHeight;
constexpr int kPartialCommandBusyTimeoutMs=1500, kPanelStatusProbeTimeoutMs=800;
std::array<uint8_t,kEpdFramebufferSize> current{}, previous{};
uint8_t* g_framebuffer=current.data();
uint8_t* g_previous_framebuffer=previous.data();
bool g_previous_framebuffer_synced=true;
bool g_epd_rail_powered=true, g_epd_powered=true, g_hot_refresh_ok=true;
@@RECT@@

// The controller port deliberately retains the last 0x83 window across 0x10.
// RAM addressing is modeled; optical waveforms and BUSY are not simulated.
struct Window { int x0=0,x1=399,y0=0,y1=299; } window;
std::array<uint8_t,kEpdWidth*kEpdHeight> ram{};
std::vector<uint8_t> commands, parameters;
uint8_t command=0;
size_t position=0, written_bytes=0;
int refreshes=0;
bool fail_window_command=false;
int fail_window_data=-1;

int SendCommand(uint8_t value) {
    commands.push_back(value); command=value; parameters.clear();
    if(value==0x83 && fail_window_command)return ESP_FAIL;
    if(value==0x10)position=0;
    return ESP_OK;
}
int SendData(uint8_t value) {
    if(command==0x83 && static_cast<int>(parameters.size())==fail_window_data)return ESP_FAIL;
    parameters.push_back(value);
    if(command==0x83 && parameters.size()==9) {
        window={parameters[0]*256+parameters[1],parameters[2]*256+parameters[3],
                parameters[4]*256+parameters[5],parameters[6]*256+parameters[7]};
        assert(parameters[8]==1 && window.x0<=window.x1 && window.y0<=window.y1);
    }
    return ESP_OK;
}
int WriteBytes(const uint8_t* bytes,size_t size) {
    assert(command==0x10);
    const int width=window.x1-window.x0+1, height=window.y1-window.y0+1;
    for(size_t i=0;i<size;++i) for(int shift=6;shift>=0;shift-=2) {
        const size_t offset=position++ % (width*height);
        const int x=window.x0+offset%width, y=window.y0+offset/width;
        assert(x>=0 && x<kEpdWidth && y>=0 && y<kEpdHeight);
        ram[y*kEpdWidth+x]=(bytes[i]>>shift)&1;
    }
    written_bytes+=size; return ESP_OK;
}
int EnsurePanelReadyForWrite(bool hot) { if(!hot)window={}; return ESP_OK; }
int PreparePanelForFramebufferWrite() { return ESP_OK; }
int PreparePanelForLocalPartialWrite() { return ESP_OK; }
int WaitPanelStatusReady(int,int) { return ESP_OK; }
int WaitBusyTimeout(int) { return ESP_OK; }
void vTaskDelay(unsigned) {}
#define pdMS_TO_TICKS(value) (value)
void FeedTwdtIfSubscribed() {}
bool ShouldKeepEpdPowered() { return true; }
int TriggerDisplayUpdate(bool,bool) { ++refreshes; return ESP_OK; }
DirtyRect AlignDirtyRect(DirtyRect r) { return r; }
DirtyRect ClampDirtyRect(DirtyRect r) { return r; }

@@PACK@@
@@INTERLEAVE@@
@@SET_WINDOW@@
@@HOT_PRELUDE@@
@@WRITE_FRAME@@
@@WRITE_RECT@@

int checks=0;
#define CHECK(expr) do {++checks;if(!(expr)){ \
    std::fprintf(stderr,"FAIL EPD window line %d: %s\n",__LINE__,#expr);std::exit(1);}}while(0)
bool RamMatchesFrame() {
    for(int y=0;y<kEpdHeight;++y) for(int x=0;x<kEpdWidth;++x)
        if(ram[y*kEpdWidth+x] != ((current[y*kEpdBytesPerRow+x/8]>>(7-x%8))&1))return false;
    return true;
}
void ClearTrace() { commands.clear(); parameters.clear(); written_bytes=0; refreshes=0; }

int main() {
    // Include the exact clock window seen at 532897 in the device log, plus
    // panel edges and values requiring both bytes of the X/Y address.
    for(const DirtyRect rect : {DirtyRect{10,19,43,86,true}, {0,0,0,0,true},
                               {32,49,256,299,true}, {0,49,100,101,true}}) {
        current.fill(0xff); previous=current; ram.fill(1); window={};
        for(int y=rect.y_start;y<=rect.y_end;++y)
            for(int x=rect.x_start_byte;x<=rect.x_end_byte;++x)current[y*50+x]=0xa5;
        CHECK(SendDirtyRectToPanel(rect)==ESP_OK);
        CHECK(RamMatchesFrame());
        previous=current;
        // The next Home selection/navigation changes pixels outside the clock.
        for(int y=106;y<=203;++y) for(int x=2;x<49;++x)current[y*50+x]=0x3c;
        ClearTrace();
        CHECK(SendFramebufferToPanel(true,true)==ESP_OK);
        CHECK(RamMatchesFrame());
        CHECK(window.x0==0 && window.x1==399 && window.y0==0 && window.y1==299);
        CHECK(written_bytes==30000 && refreshes==1);
        CHECK(commands==std::vector<uint8_t>({0x50,0xe0,0x83,0x10}));
        previous=current;
        CHECK(SendFramebufferToPanel(true,true)==ESP_OK && RamMatchesFrame());
        // A complete cold refresh also establishes the full target.
        current.fill(0x5a);
        CHECK(SendFramebufferToPanel(false,false)==ESP_OK && RamMatchesFrame());
    }
    // A failed window command or any failed address byte must stop before the
    // DTM1 write and display trigger, so callers cannot acknowledge a bad frame.
    for(int failure=-1;failure<9;++failure) {
        ClearTrace(); fail_window_command=(failure==-1); fail_window_data=failure;
        CHECK(SendFramebufferToPanel(true,true)==ESP_FAIL);
        CHECK(written_bytes==0 && refreshes==0);
        CHECK(std::find(commands.begin(),commands.end(),0x10)==commands.end());
    }
    fail_window_command=false; fail_window_data=-1;
    ClearTrace(); CHECK(SendFramebufferToPanel(true,false)==ESP_OK && RamMatchesFrame());
    CHECK(written_bytes==30000 && refreshes==1);
    std::printf("EPD refresh windows: %d checks PASS (production SPI paths, NOT HIL)\n",checks);
}
