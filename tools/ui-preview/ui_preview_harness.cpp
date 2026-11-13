// Linux harness: draws the overlay screens into pictures (layout check only).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <vector>
#include <atomic>
#include <algorithm>
typedef int CRITICAL_SECTION;
static void EnterCriticalSection(CRITICAL_SECTION*) {}
static void LeaveCriticalSection(CRITICAL_SECTION*) {}
static uint32_t g_now = 100000;
static uint32_t GetTickCount() { return g_now; }
static void Log(const char*, ...) {}
#pragma pack(push,1)
struct NHdr { uint32_t magic; uint8_t type, flags; uint16_t epoch; };
#pragma pack(pop)
enum : uint8_t { M_LOBBY = 12, M_START = 13, M_STARTACK = 14 };
enum NetState { NS_OFF, NS_HELLO, NS_ON };
static const uint32_t NET_BUILD = 59;
static struct { int side = 0; uint32_t seed = 1; NetState st = NS_ON; uint32_t rtt = 16; bool saved = false; uint32_t origCfg[24]; } N;
static struct { int delay = 2; } g_ncfg;
static char g_uiName[32] = "DESKTOP";
static NHdr MakeHdr(uint8_t t) { NHdr h{0,t,0,0}; return h; }
static void NetSend(const void*, int) {}
static void NetEnd(const char*) {}
static std::vector<int> g_loopIn; static void LoopQueue(std::vector<int>&, const void*, int) {}
namespace net { struct Friend { uint64_t id; char name[64]; bool inGame; }; }
static CRITICAL_SECTION g_statusLock;
static char g_netStatus[200] = "ONLINE P1 vs 192.168.178.20:7845 | ping 16 ms | delay 2 | loading | waited 0 ms, skipped 0";
static char g_status[256] = "ready";
static uint32_t PadOfPlayer(int) { return 0; }
static uint32_t G_PAD_CFG_DATA[24] = {0x4000,0x1000,0x2000,0x8000,0x100,0x200,0x10000,0x20000,0,0,0x10,0x20};
#define G_PAD_CFG ((uintptr_t)G_PAD_CFG_DATA)
#define UI_HARNESS
static void UiSaveConnectSettings() {}
static void UiSaveController() {}
#include "../../src/mod/lobby.inc"
#include "../../src/mod/overlay.inc"

// --- tiny software rasterizer for ImDrawData ---
static int W = 1920, H = 1080;
static std::vector<float> img;
static unsigned char* atlas; static int aw, ah;
static void raster(ImDrawData* dd) {
  for (int n = 0; n < dd->CmdListsCount; n++) {
    const ImDrawList* cl = dd->CmdLists[n];
    const ImDrawVert* vb = cl->VtxBuffer.Data; const ImDrawIdx* ib = cl->IdxBuffer.Data;
    for (const ImDrawCmd& c : cl->CmdBuffer) {
      int cx0 = std::max(0,(int)c.ClipRect.x), cy0 = std::max(0,(int)c.ClipRect.y), cx1 = std::min(W,(int)c.ClipRect.z), cy1 = std::min(H,(int)c.ClipRect.w);
      for (unsigned e = 0; e < c.ElemCount; e += 3) {
        const ImDrawVert* v[3] = {&vb[c.VtxOffset+ib[c.IdxOffset+e]], &vb[c.VtxOffset+ib[c.IdxOffset+e+1]], &vb[c.VtxOffset+ib[c.IdxOffset+e+2]]};
        float x0=v[0]->pos.x,y0=v[0]->pos.y,x1=v[1]->pos.x,y1=v[1]->pos.y,x2=v[2]->pos.x,y2=v[2]->pos.y;
        float area=(x1-x0)*(y2-y0)-(x2-x0)*(y1-y0); if (fabsf(area)<1e-6f) continue;
        int bx0=std::max(cx0,(int)floorf(std::min({x0,x1,x2}))), bx1=std::min(cx1,(int)ceilf(std::max({x0,x1,x2})));
        int by0=std::max(cy0,(int)floorf(std::min({y0,y1,y2}))), by1=std::min(cy1,(int)ceilf(std::max({y0,y1,y2})));
        for (int y=by0;y<by1;y++) for (int x=bx0;x<bx1;x++) {
          float px=x+0.5f,py=y+0.5f;
          float w0=((x1-px)*(y2-py)-(x2-px)*(y1-py))/area, w1=((x2-px)*(y0-py)-(x0-px)*(y2-py))/area, w2=1-w0-w1;
          if (w0<-1e-4f||w1<-1e-4f||w2<-1e-4f) continue;
          float u=w0*v[0]->uv.x+w1*v[1]->uv.x+w2*v[2]->uv.x, vv=w0*v[0]->uv.y+w1*v[1]->uv.y+w2*v[2]->uv.y;
          int tx=std::min(aw-1,std::max(0,(int)(u*aw))), ty=std::min(ah-1,std::max(0,(int)(vv*ah)));
          const unsigned char* t=&atlas[(ty*aw+tx)*4];
          float col[4];
          for(int k=0;k<4;k++){ float a=((v[0]->col>>(8*k))&255)*w0+((v[1]->col>>(8*k))&255)*w1+((v[2]->col>>(8*k))&255)*w2; col[k]=a/255.0f*t[k]/255.0f; }
          float* d=&img[(y*W+x)*3]; for(int k=0;k<3;k++) d[k]=d[k]*(1-col[3])+col[k]*col[3];
        }
      }
    }
  }
}
static void shot(const char* name, void (*setup)()) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)W,(float)H);
  for (int f = 0; f < 3; f++) {
    io.DeltaTime = 1/60.f; setup();
    ImGui::NewFrame(); ImGui::PushFont(g_fBody);
    if (U.visible) UiMainPanel(io.DisplaySize); else UiStatusBar(io.DisplaySize);
    ImGui::PopFont(); ImGui::Render();
  }
  img.assign(W*H*3, 0);
  for (int y=0;y<H;y++) for(int x=0;x<W;x++){ float* d=&img[(y*W+x)*3]; d[0]=0.25f+0.2f*x/W; d[1]=0.12f; d[2]=0.18f+0.25f*y/H; } // stand-in for the game picture
  raster(ImGui::GetDrawData());
  FILE* f=fopen(name,"wb"); fprintf(f,"P6 %d %d 255\n",W,H);
  for (float c: img) fputc((int)(std::min(1.f,std::max(0.f,c))*255+0.5f), f); fclose(f);
}
int main() {
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO(); io.IniFilename=nullptr;
  float sc = H/1080.f;
  g_fBody = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", theme::BODY_PX*sc*0.92f);
  g_fTitle = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", theme::TITLE_PX*sc*0.92f);
  g_fSmall = io.Fonts->AddFontFromFileTTF("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", theme::SMALL_PX*sc*0.92f);
  io.Fonts->GetTexDataAsRGBA32(&atlas,&aw,&ah);
  theme::Apply(sc);
  U.visible = true; U.mode = 1; snprintf(U.name, 32, "DESKTOP");
  shot("1_connect.ppm", []{ U.screen = US_CONNECT; U.netOn=false; });
  LB.me.seq=1; snprintf(LB.me.name,32,"DESKTOP"); LB.haveThem=true; snprintf(LB.them.name,32,"LAPTOP"); LB.them.ready=true;
  shot("2_lobby.ppm", []{ U.screen = US_SESSION; U.netOn=true; LB.me.phase=LP_LOBBY; });
  shot("3_setup.ppm", []{ LB.me.phase=LP_SETUP; LB.them.phase=LP_SETUP; LB.me.p.chara=3; LB.them.p.chara=LB_RANDOM; LB.them.locked=true; LB.them.p.stage=4; LB.note[0]=0; });
  shot("4_match.ppm", []{ LB.me.phase=LP_MATCH; LB.me.matchNo=1; LB.s.matchNo=1; LB.s.chara[0]=3; LB.s.chara[1]=9; LB.s.stage=4; LB.s.rounds=2; LB.s.delay=2; LB.haveSettings=true; LB.sum=0x46B8C74A; LB.confirmed=1; });
  shot("5_post.ppm", []{ LB.me.phase=LP_POST; LB.me.choice=LCH_REMATCH; LB.me.choiceMatch=1; snprintf(LB.note,160,"match 1 is over"); LB.noteAt=g_now; });
  shot("6_controls.ppm", []{ U.screen = US_CONTROLS; U.padRaw[0]=PB_A|PB_DOWN; U.controller=-1; });
  shot("7_statusbar.ppm", []{ U.visible = false; });
}
