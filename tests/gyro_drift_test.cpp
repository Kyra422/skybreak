#include "input/gamepad_motion.h"
#include "input/game_context.h"
#include <cstdio>
using namespace nwii::runtime::input;
int failures=0,checks=0;
void check(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",name);}
struct Stream {
    GyroMotionTracker tracker;uint64_t t=1000000;int hz=250;
    double run(double seconds,float yaw){double angle=0;for(int i=0;i<int(seconds*hz);++i){
        SensorEvent g;g.t_us=t;g.v[1]=yaw;tracker.add(g);
        SensorEvent a;a.t_us=t;a.gyro=false;a.v[1]=float(kStandardGravity);tracker.add(a);
        MeasuredSubStep out[32];auto n=tracker.produce(1.0/hz,out,32);
        for(size_t j=0;j<n;++j)angle+=out[j].rate[1]*out[j].dt;t+=1000000/hz;
    }return angle;}
};
int main(){for(int hz:{250,1000}){
    Stream s;s.hz=hz;s.run(2,0);
    const double turn=s.run(8,.03f);
    GameContext beetle;beetle.valid=true;beetle.player=true;beetle.item_state=kItemBeetle;beetle.cam_handler=6;
    s.tracker.allow_bias_learning=gyro_bias_learning_allowed(beetle);
    const double stop=s.run(60,0),back=s.run(8,-.03f);
    check(std::fabs(turn-.24)<.0003,"slow turn during ordinary play is not swallowed by calibration");
    check(std::fabs(stop)<.0003,"ordinary play to Beetle: stopped controller remains stopped for a minute");
    check(std::fabs(turn+back)<.0003,"equal opposite rotation returns to neutral after context transition");
    Stream noise;noise.hz=hz;noise.tracker.allow_bias_learning=false;
    check(std::fabs(noise.run(60,.007f))<.0003,"small uncalibrated sensor offset cannot accumulate in full Wii motion");
    Stream calibrated;calibrated.hz=hz;float bias[3]{0,.045f,0};calibrated.tracker.set_bias(bias);
    check(std::fabs(calibrated.run(60,.045f))<.0003,"guided calibration removes larger constant hardware bias");
    const double a=calibrated.run(8,.075f);calibrated.tracker.reset();
    const double b=calibrated.run(8,.015f);
    check(std::fabs(a+b)<.0003,"calibration survives toggles without changing left/right sensitivity");
    Stream lost;lost.hz=hz;lost.run(1,.3f);double phantom=0;
    for(int i=0;i<hz*2;++i){SensorEvent ac;ac.gyro=false;ac.t_us=lost.t;ac.v[1]=float(kStandardGravity);
        lost.tracker.add(ac);MeasuredSubStep out[32];auto n=lost.tracker.produce(1.0/hz,out,32);
        for(size_t j=0;j<n;++j)phantom+=out[j].rate[1]*out[j].dt;lost.t+=1000000/hz;}
    check(std::fabs(phantom)<.0003,"accelerometer-only reports cannot replay a stale gyro turn");
    lost.tracker.deadband=.001f;lost.run(.1,0);
    check(std::fabs(lost.run(1,.003f)-.003)<.0001,"lower idle threshold permits very fine calibrated motion");
}std::printf("RESULT %d/%d\n",checks-failures,checks);return failures?1:0;}
