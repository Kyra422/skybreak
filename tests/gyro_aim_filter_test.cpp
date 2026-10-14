#include "input/gyro_aim_filter.h"
#include "input/gamepad_motion.h"
#include <cstdio>
#include <limits>
using namespace nwii::runtime::input;
static int passed=0,failed=0;
static void check(bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);(ok?passed:failed)++;}
int main(){
    GyroAimFilter f;
    float noise[3]{.0005f,-.001f,.0005f};f.process(noise,.004);
    check(noise[0]==0&&noise[1]==0&&noise[2]==0,"stationary sensor noise produces no pointer drift");
    float diagonal[3]{.015f,.015f,0};f.process(diagonal,.004);
    check(std::fabs(diagonal[0]-.015)<1e-6&&std::fabs(diagonal[1]-.015)<1e-6,"slow diagonal aim survives the vector deadband");
    f.reset();double rms=0;
    for(int i=0;i<500;++i){float v[3]{0,i%2?.12f:-.12f,0};f.process(v,.004);if(i>=100)rms+=v[1]*v[1];}
    check(std::sqrt(rms/400)<.03,"small alternating gyro jitter is reduced without a large latency filter");
    GyroAimFilter usb,bt;float a[3]{0,.05f,0},b[3]{0,.05f,0};usb.process(a,.001);bt.process(b,.004);
    for(int i=0;i<100;++i){a[0]=a[2]=0;a[1]=.15f;usb.process(a,.001);}
    for(int i=0;i<25;++i){b[0]=b[2]=0;b[1]=.15f;bt.process(b,.004);}
    check(std::fabs(a[1]-b[1])<1e-6,"USB and Bluetooth sensor cadences have the same timed response");
    float fast[3]{-3,2,4};f.process(fast,.004);
    check(fast[0]==-3&&fast[1]==2&&fast[2]==4,"fast intentional motion is delivered immediately");
    float stop[3]{};f.process(stop,.004);
    check(stop[0]==0&&stop[1]==0&&stop[2]==0,"stopping has no smoothing tail");
    float bad[3]{0,std::numeric_limits<float>::quiet_NaN(),0};f.process(bad,.004);
    check(bad[0]==0&&bad[1]==0&&bad[2]==0,"non-finite samples cannot poison the filter");
    float invalid[3]{.1f,.1f,.1f};f.process(invalid,.2);
    check(invalid[0]==0&&invalid[1]==0&&invalid[2]==0,"a stalled sensor interval is rejected");
    GyroMotionTracker tracker;tracker.auto_calibrate=false;tracker.filter_aim=true;
    auto add=[&](uint64_t t,float rate){SensorEvent g;g.t_us=t;g.v[1]=rate;tracker.add(g);SensorEvent ac;ac.t_us=t;ac.gyro=false;ac.v[1]=float(kStandardGravity);tracker.add(ac);};
    MeasuredSubStep out[32];add(1000,.001f);add(5000,.001f);auto n=tracker.produce(.008,out,32);
    check(n>0&&out[n-1].rate[1]==0,"production sensor tracker applies idle suppression during aim");
    check(out[n-1].accel[0]==0&&std::fabs(out[n-1].accel[1]+1)<1e-6&&out[n-1].accel[2]==0,"Wii acceleration and body axes stay physical");
    check(tracker.produce(.001,out,32)==0,"empty host polls do not manufacture angular movement");
    tracker.filter_aim=false;add(9000,.003f);n=tracker.produce(.004,out,32);
    check(n>0&&out[n-1].rate[1]==0,"three-axis puzzles suppress idle noise without changing physical acceleration");
    tracker.filter_aim=true;tracker.set_bias(noise);float bias[3]{0,.03f,0};tracker.set_bias(bias);tracker.auto_calibrate=true;tracker.allow_bias_learning=false;
    add(13000,.03f);n=tracker.produce(.004,out,32);
    check(n>0&&out[n-1].rate[1]==0,"guided calibration bias is removed before idle filtering");
    tracker.discard();add(1000000,.08f);n=tracker.produce(.004,out,32);
    check(n>0&&std::fabs(out[n-1].rate[1]-.05)<1e-6,"discard/reconnect does not reuse the old smoothing state");
    std::printf("RESULT %d passed, %d failed\n",passed,failed);return failed?1:0;
}
