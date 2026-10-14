from pathlib import Path
import re
import sys
root=Path(__file__).resolve().parents[1]
out=Path(sys.argv[1])
path=root/'work/nwii-build/_deps/sdl2-src/src/joystick/hidapi/SDL_hidapi_wii.c';source=path.read_text()
def function(name,s=source):
 m=re.search(r'static [^\n]+\b'+name+r'\(',s);assert m,name
 start=s.index('{',m.start());depth=1;i=start+1
 while depth:
  depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[m.start():i]+'\n'
report=re.search(r'typedef enum\s*\{[^}]+\}\s*EWiiInputReportIDs;',source).group()
data=re.search(r'typedef struct\s*\{\s*Uint8 rgucBaseButtons[^}]+\}\s*WiiButtonData;',source).group()
common='''
#include <SDL.h>
#include <cstdio>
#include <vector>
#include <array>
#include <cmath>
#include <cstring>
#include "SDL_hidapi_wii_ir.h"
#define WII_MOTIONPLUS_MODE_NONE 0
enum {k_eWiiExtensionControllerType_None,k_eWiiExtensionControllerType_Nunchuk,
      k_eWiiExtensionControllerType_Gamepad,k_eWiiExtensionControllerType_WiiUPro};
typedef int EWiiExtensionControllerType;
enum {k_eWiiOutputReportIDs_IRCameraEnable=0x13,k_eWiiOutputReportIDs_IRCameraEnable2=0x1a};
struct SDL_DriverWii_Context {
 bool m_bReportSensors=true,m_bSkyBreakIR=true,m_bRumbleActive=false;
 unsigned char m_ucMotionPlusMode=0,m_rgucReadBuffer[22]{};
 int m_eExtensionControllerType=k_eWiiExtensionControllerType_Nunchuk;
 unsigned short m_unIRSerial=0;
};
struct _SDL_Joystick {};
static std::array<short,12> axes{};
static unsigned sensor_calls=0;static float accel[3]{};
static void SDL_PrivateJoystickAxis(SDL_Joystick*,int axis,Sint16 v){axes[axis]=v;}
static void SDL_PrivateJoystickSensor(SDL_Joystick*,SDL_SensorType,uint64_t,const float* v,int){++sensor_calls;std::copy(v,v+3,accel);}
static void RequestButtonPacketType(SDL_DriverWii_Context*,int){}
static std::vector<unsigned> writes;
static int fail_write=-1;
static SDL_bool WriteOutput(SDL_DriverWii_Context*,const Uint8* data,int,SDL_bool){writes.push_back(data[0]);return int(writes.size())==fail_write?SDL_FALSE:SDL_TRUE;}
static SDL_bool WriteRegister(SDL_DriverWii_Context*,Uint32 addr,const Uint8*,int,SDL_bool){writes.push_back(addr);return int(writes.size())==fail_write?SDL_FALSE:SDL_TRUE;}
#define SDL_Delay(x) ((void)0)
#define SDL_memcpy std::memcpy
#define SDL_zero(x) std::memset(&(x),0,sizeof(x))
#define SDL_LogDebug(...) ((void)0)
'''+report+'\n'+data+'\n'+'''
static WiiButtonData decoded{};
static void HandleButtonData(SDL_DriverWii_Context*,SDL_Joystick*,WiiButtonData* data){decoded=*data;}
'''
names=['GetButtonPacketType','SkyBreakConfigureIR','SkyBreakPostIR','GetBaseButtons','GetAccelerometer','GetExtensionData','HandleWiiRemoteAccelData','HandleButtonPacket']
common+='\n'.join(function(n) for n in names)
main='''
int main(){
 unsigned checks=0,failures=0;auto check=[&](bool yes,const char* n){++checks;if(!yes){++failures;std::printf("FAIL %s\\n",n);}};
 SDL_DriverWii_Context ctx;SDL_Joystick j;
 WiiButtonData d{};
 HandleWiiRemoteAccelData(&ctx,&j,&d);
 check(sensor_calls==0,"ACK/status without acceleration must not inject fake 5g motion");
 d.hasAccelerometer=SDL_TRUE;d.rgucAccelerometer[0]=128;d.rgucAccelerometer[1]=128;d.rgucAccelerometer[2]=153;
 HandleWiiRemoteAccelData(&ctx,&j,&d);
 check(sensor_calls==1 && std::abs(accel[0])<.001 && std::abs(accel[1]-SDL_STANDARD_GRAVITY)<.001 && std::abs(accel[2])<.001,"level remote reports one real gravity sample");
 ctx.m_bReportSensors=false;HandleWiiRemoteAccelData(&ctx,&j,&d);
 check(sensor_calls==1,"disabled sensors emit no accelerometer updates");ctx.m_bReportSensors=true;
 check(GetButtonPacketType(&ctx)==0x37,"IR plus MotionPlus/Nunchuk selects report 37");
 ctx.m_bSkyBreakIR=false;check(GetButtonPacketType(&ctx)==0x35,"fallback preserves motion/extensions without IR");ctx.m_bSkyBreakIR=true;
 unsigned char pixels[10]={100,20,11,32,248,255,255,255,255,255};
 auto ir=SkyBreakDecodeWiiIR(pixels,10);
 check(ir.valid && ir.count==2 && ir.x[0]==100 && ir.y[0]==20 && ir.x[1]==800 && ir.y[1]==760,"basic IR decodes low and high bits of both dots");
 check(std::abs(ir.centre_x-450.f/1023)<.0001 && std::abs(ir.centre_y-390.f/767)<.0001,"camera coordinates normalize against 1024 by 768 range");
 check(std::abs(ir.horizon_x*ir.horizon_x+ir.horizon_y*ir.horizon_y-1)<.0001 && ir.horizon_y<0,"IR horizon retains bar roll");
 check(!SkyBreakDecodeWiiIR(pixels,9).valid && !SkyBreakDecodeWiiIR(nullptr,10).valid,"truncated IR is rejected");
 unsigned char empty[10];std::memset(empty,255,10);
 check(!SkyBreakDecodeWiiIR(empty,10).valid,"no infrared dots means no pointer");
 unsigned char one[10]={100,20,15,255,255,255,255,255,255,255};
 check(!SkyBreakDecodeWiiIR(one,10).valid,"one dot cannot fabricate a two-point bar");
 ctx.m_rgucReadBuffer[0]=0x37;ctx.m_rgucReadBuffer[1]=0x10;ctx.m_rgucReadBuffer[2]=0x08;
 ctx.m_rgucReadBuffer[3]=128;ctx.m_rgucReadBuffer[4]=128;ctx.m_rgucReadBuffer[5]=153;
 std::memcpy(ctx.m_rgucReadBuffer+6,pixels,10);
 for(int i=0;i<6;++i)ctx.m_rgucReadBuffer[16+i]=Uint8(10+i);
 HandleButtonPacket(&ctx,&j);
 check(decoded.hasBaseButtons && decoded.hasAccelerometer && decoded.rgucAccelerometer[2]==153,"report 37 retains core buttons and remote accelerometer");
 check(decoded.ucNExtensionBytes==6 && decoded.rgucExtension[0]==10 && decoded.rgucExtension[5]==15,"IR must not shift or discard MotionPlus/Nunchuk bytes");
 check(axes[10]==32767 && axes[11]==1,"IR validity and first packet identity are published");
 HandleButtonPacket(&ctx,&j);check(axes[11]==2,"unchanged IR still advances packet identity");
 std::memcpy(ctx.m_rgucReadBuffer+6,empty,10);HandleButtonPacket(&ctx,&j);
 check(axes[10]==0 && axes[11]==3,"lost bar releases validity immediately");
 writes.clear();check(SkyBreakConfigureIR(&ctx)==SDL_TRUE,"IR initialization completes");
 check(writes==std::vector<unsigned>{0x13,0x1a,0xb00030,0xb00000,0xb0001a,0xb00033,0xb00030},"IR pixel clock, logic, sensitivity and basic mode are configured in order");
 for(int stage=1;stage<=7;++stage){writes.clear();fail_write=stage;check(!SkyBreakConfigureIR(&ctx) && int(writes.size())==stage,"failed initialization stops without claiming success");}fail_write=-1;
 std::printf("RESULT %u/%u\\n",checks-failures,checks);return failures?1:0;
}
'''

out.write_text(source[:source.index('#include')]+common+main)
