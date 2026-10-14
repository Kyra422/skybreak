#include "input/aurora_pad_sensors.h"
#include <cstdio>
int failures=0,checks=0,calls=0;bool capable=true;int result=0;
extern "C" {
SDL_bool SDL_GameControllerHasSensor(SDL_GameController*,SDL_SensorType){return capable?SDL_TRUE:SDL_FALSE;}
int SDL_GameControllerSetSensorEnabled(SDL_GameController*,SDL_SensorType,SDL_bool){++calls;return result;}
int SDL_GameControllerGetSensorData(SDL_GameController*,SDL_SensorType,float* p,int n){if(result==0)for(int i=0;i<n;++i)p[i]=float(i+1);return result;}
}
void check(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",name);}
int main(){using namespace nwii::runtime::input::aurora_sensors;
auto* pad=reinterpret_cast<SDL_GameController*>(1);bool on=false;
check(!has(nullptr,SDL_SENSOR_GYRO),"null controller has no sensor");
result=-1;check(!transition(pad,SDL_SENSOR_GYRO,true,on)&&!on&&calls==1,"failed enable is not recorded as active");
result=0;check(transition(pad,SDL_SENSOR_GYRO,true,on)&&on&&calls==2,"failed enable retries and accepts SDL2 zero success");
check(transition(pad,SDL_SENSOR_GYRO,true,on)&&calls==2,"unchanged state does not reenable hardware");
float v[3]{};check(read(pad,SDL_SENSOR_GYRO,v,3)&&v[0]==1&&v[2]==3,"successful read returns sensor data");
result=-1;check(!read(pad,SDL_SENSOR_GYRO,v,3)&&v[0]==0&&v[2]==0,"failed read clears stale turn data");
check(!transition(pad,SDL_SENSOR_GYRO,false,on)&&on,"failed disable is retried");
result=0;check(transition(pad,SDL_SENSOR_GYRO,false,on)&&!on,"successful disable updates state");
capable=false;check(!enable(pad,SDL_SENSOR_GYRO,true),"unsupported sensor cannot be enabled");
std::printf("RESULT %d/%d\n",checks-failures,checks);return failures?1:0;}
