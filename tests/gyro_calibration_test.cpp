#include "input/gyro_calibration.h"
#include "input/gamepad_motion.h"
#include <cassert>
#include <cstdio>
using namespace nwii::runtime::input;
int main(){
    StillCalibration c;c.begin();float accel[3]{0,9.80665f,0},bias[3]{.004f,-.002f,.001f};
    for(uint64_t t=1000;t<=2101000;t+=4000){c.observe(false,t,accel);c.observe(true,t,bias);}
    assert(c.ready && !c.collecting);for(int i=0;i<3;++i)assert(std::fabs(c.mean[i]-bias[i])<1e-6f);
    c.begin();float moving[3]{0,.5f,0};for(uint64_t t=1000;t<=3001000;t+=4000){c.observe(false,t,accel);c.observe(true,t,moving);}assert(!c.ready && !c.count);
    c.begin();for(int k=0;k<2000;++k){c.observe(false,1000,accel);c.observe(true,1000,bias);}assert(!c.ready && c.count==1);
    c.begin();for(uint64_t t=1000;t<=1001000;t+=4000){c.observe(false,t,accel);c.observe(true,t,bias);}c.observe(false,1101000,accel);c.observe(true,1101000,bias);assert(!c.ready && c.count==0);
    c.begin();float freefall[3]{};for(uint64_t t=1000;t<=3001000;t+=4000){c.observe(false,t,freefall);c.observe(true,t,bias);}assert(!c.ready);
    calibration_device(0,42,true);calibration_begin(0);for(uint64_t t=1000;t<=2101000;t+=4000){calibration_observe(42,false,t,accel);calibration_observe(42,true,t,bias);}assert(calibration_apply(0));
    uint64_t serial=0;float out[3];assert(calibration_bias(42,serial,out));assert(!calibration_bias(42,serial,out));calibration_device(0,43,true);assert(!calibration_bias(43,serial,out));
    GravityAim g;float gravity[3]{0,-1,0},turn[3]{0,1,0};g.map(turn,gravity,.004,out);assert(std::fabs(out[1]-1)<1e-5);
    // A controller rolled 90 degrees: world yaw is now its local X axis.
    float sideways[3]{-1,0,0},side_turn[3]{1,0,0};for(int i=0;i<1000;++i)g.map(side_turn,sideways,.004,out);assert(std::fabs(out[1]-1)<1e-4);
    float vertical[3]{0,0,-1};for(int i=0;i<1000;++i)g.map(side_turn,vertical,.004,out);for(float v:out)assert(std::isfinite(v));
    GyroMotionTracker tracker;tracker.set_bias(bias);assert(tracker.bias_valid());for(int i=0;i<3;++i)assert(tracker.bias()[i]==bias[i]);
    std::puts("PASS: still calibration, movement rejection, duplicate/gap timing, reconnect isolation, gravity aim and manual bias.");
}
