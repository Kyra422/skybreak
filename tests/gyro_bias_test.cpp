#include "input/game_context.h"
#include "input/gamepad_motion.h"
#include <cstdio>
using namespace nwii::runtime::input;
static int checks=0,failures=0;
static void check(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("%s %s\n",ok?"PASS":"FAIL",name);}
struct Stream {
    GyroMotionTracker tracker;
    uint64_t time=1000;
    double run(double seconds,float yaw,int hz=250) {
        double angle=0;
        for(int i=0;i<int(seconds*hz);++i){
            SensorEvent g;g.t_us=time;g.v[1]=yaw;tracker.add(g);
            SensorEvent a;a.t_us=time;a.gyro=false;a.v[1]=float(kStandardGravity);tracker.add(a);
            MeasuredSubStep out[32];const auto n=tracker.produce(1.0/hz,out,32);
            for(size_t k=0;k<n;++k)angle+=double(out[k].rate[1])*out[k].dt;
            time+=1000000/hz;
        }
        return angle;
    }
};
int main(){
    for(int hz:{250,1000}) {
        GameContext cases[11];for(auto& c:cases){c.valid=true;c.player=true;c.item_state=kItemNone;}
        cases[0].item_state=kItemBeetle;cases[0].cam_handler=6;
        cases[1].riding=2;cases[2].action_flags=kActInWater;cases[3].action_flags=kActTightRope;
        cases[4].action=0x12;cases[5].sword_states=kSwordBWheelOpen;cases[6].carried_profile=0x1AA;
        cases[7].aim_flags=0x40;cases[8].prompt_slot[2]=130;cases[9].event_state=1;cases[10].action=0x13;
        for(auto& c:cases){
            Stream s;s.run(2,0,hz);const auto updates=s.tracker.bias_updates;
            s.tracker.allow_bias_learning=gyro_bias_learning_allowed(c);
            const double turn=s.run(4,.03f,hz),held=s.run(2,0,hz),back=s.run(4,-.03f,hz);
            check(std::fabs(turn-.12)<.0002&&std::fabs(held)<.0002&&std::fabs(turn+back)<.0002&&
                  s.tracker.bias_updates==updates,"active reference controls preserve slow turns, stops and return to neutral");
        }
        Stream manual;float bias[3]{0,.006f,0};manual.tracker.set_bias(bias);
        const double turn=manual.run(4,.036f,hz),held=manual.run(2,.006f,hz);
        check(std::fabs(turn-.12)<.0002&&std::fabs(held)<.0002&&manual.tracker.bias()[1]==bias[1],
              "background calibration cannot overwrite a guided calibration during ordinary play");
        manual.tracker.reset();manual.tracker.auto_calibrate=false;
        check(std::fabs(manual.run(2,.006f,hz))<.0002,"guided bias survives mode toggles and applies with automatic learning disabled");
        manual.tracker.reset_bias();manual.tracker.auto_calibrate=true;
        manual.run(3,.02f,hz);
        check(manual.tracker.bias_valid()&&std::fabs(manual.tracker.bias()[1]-.02)<.0001,
              "reconnecting clears guided bias and allows stationary automatic calibration again");
        Stream resume;resume.tracker.allow_bias_learning=false;resume.run(3,.03f,hz);
        resume.tracker.allow_bias_learning=gyro_bias_learning_allowed(GameContext{});resume.run(3,.02f,hz);
        check(resume.tracker.bias_valid()&&std::fabs(resume.tracker.bias()[1]-.02)<.0001,
              "still calibration resumes outside active reference controls with a fresh window");
    }
    std::printf("RESULT %d/%d checks passed\n",checks-failures,checks);return failures?1:0;
}
