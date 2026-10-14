#pragma once
// GYRO28: explicit still calibration. Sensor events retain hardware time; UI
// never calls a joystick driver. Bias is scoped to the connected SDL instance.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace nwii::runtime::input {
struct StillCalibration {
    bool collecting=false, ready=false;
    uint64_t first=0,last=0,accel_time=0;
    unsigned count=0;
    double sum[3]{},square[3]{};
    float mean[3]{},noise=0,seconds=0,accel[3]{},latest[3]{};
    void begin() { *this=StillCalibration{};collecting=true; }
    void clear_window() { first=last=0;count=0;seconds=0;noise=0;for(int i=0;i<3;++i)sum[i]=square[i]=0; }
    void observe(bool gyro,uint64_t time,const float v[3]) {
        if(!time) return; // no invented sensor duration
        for(int i=0;i<3;++i)if(!std::isfinite(v[i]))return;
        if(!gyro){ if(time>=accel_time){accel_time=time;std::copy(v,v+3,accel);}return; }
        std::copy(v,v+3,latest);
        if(!collecting || (last && time<=last))return;
        double magnitude=0,a=0;for(int i=0;i<3;++i){magnitude+=v[i]*v[i];a+=accel[i]*accel[i];}
        // Explicitly lay the pad down. Reject movement, stale acceleration and gaps.
        if(std::sqrt(magnitude)>0.10 || !accel_time ||
           (time>accel_time && time-accel_time>100000) ||
           std::fabs(std::sqrt(a)-9.80665)>1.0 || (last && time-last>50000)) {
            clear_window();return;
        }
        if(!first)first=time;last=time;++count;
        for(int i=0;i<3;++i){sum[i]+=v[i];square[i]+=double(v[i])*v[i];mean[i]=float(sum[i]/count);}
        double variance=0;for(int i=0;i<3;++i)variance+=std::max(0.,square[i]/count-double(mean[i])*mean[i]);
        noise=float(std::sqrt(variance));seconds=float(double(last-first)*1e-6);
        if(count>=100 && seconds>=2.0){
            if(noise<=0.008f){ready=true;collecting=false;}else clear_window();
        }
    }
};
struct CalibrationStatus {
    int32_t id=-1;bool capable=false,applied=false;
    uint64_t serial=0,heartbeat=0,last_event_ms=0;
    float bias[3]{};
    StillCalibration sample;
};
struct CalibrationStore { std::mutex mutex;std::array<CalibrationStatus,4> slots; };
inline CalibrationStore& calibration_store(){static auto* s=new CalibrationStore;return *s;}
inline uint64_t calibration_now(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
inline void calibration_device(int slot,int32_t id,bool capable){
    auto& s=calibration_store();std::lock_guard lock(s.mutex);auto& p=s.slots.at(slot);
    if(p.id!=id){p=CalibrationStatus{};p.id=id;}p.capable=capable;
}
inline CalibrationStatus calibration_status(int slot,bool request=false){
    auto& s=calibration_store();std::lock_guard lock(s.mutex);auto& p=s.slots.at(slot);
    if(request)p.heartbeat=calibration_now();return p;
}
inline bool calibration_requested(int slot){
    auto p=calibration_status(slot);return p.capable && p.heartbeat && calibration_now()-p.heartbeat<1000;
}
inline void calibration_begin(int slot){auto& s=calibration_store();std::lock_guard lock(s.mutex);auto& p=s.slots.at(slot);if(p.capable)p.sample.begin();}
inline void calibration_cancel(int slot){auto& s=calibration_store();std::lock_guard lock(s.mutex);s.slots.at(slot).sample.collecting=false;}
inline bool calibration_apply(int slot){
    auto& s=calibration_store();std::lock_guard lock(s.mutex);auto& p=s.slots.at(slot);
    if(!p.capable || !p.sample.ready || calibration_now()-p.last_event_ms>1000)return false;
    std::copy(p.sample.mean,p.sample.mean+3,p.bias);p.applied=true;++p.serial;return true;
}
inline bool calibration_bias(int32_t id,uint64_t& serial,float bias[3]){
    auto& s=calibration_store();std::lock_guard lock(s.mutex);
    for(auto& p:s.slots)if(p.id==id && p.applied && serial!=p.serial){serial=p.serial;std::copy(p.bias,p.bias+3,bias);return true;}return false;
}
inline void calibration_observe(int32_t id,bool gyro,uint64_t time,const float v[3]){
    auto& s=calibration_store();std::lock_guard lock(s.mutex);
    for(auto& p:s.slots)if(p.id==id && p.capable){p.sample.observe(gyro,time,v);p.last_event_ms=calibration_now();break;}
}

// Gravity-relative two-axis aiming. Full Wii motion and three-axis puzzles keep
// physical rates. Near vertical, retain the last horizontal axis to avoid flips.
class GravityAim {
    double up_[3]{0,1,0},right_[3]{1,0,0};
public:
    void reset(){up_[0]=up_[2]=right_[1]=right_[2]=0;up_[1]=right_[0]=1;}
    void map(const float rate[3],const float accel[3],double dt,float out[3]){
        double n=0;for(int i=0;i<3;++i)n+=double(accel[i])*accel[i];n=std::sqrt(n);
        if(n>0.85 && n<1.15 && dt>0 && dt<=0.1){
            const double k=1-std::exp(-dt*20);double norm=0;
            for(int i=0;i<3;++i){up_[i]+=k*(-accel[i]/n-up_[i]);norm+=up_[i]*up_[i];}
            if(norm>0.1){norm=std::sqrt(norm);for(double& v:up_)v/=norm;}
            const double r=std::hypot(up_[0],up_[1]);
            if(r>0.15){right_[0]=up_[1]/r;right_[1]=-up_[0]/r;right_[2]=0;}
        }
        double pitch=0,yaw=0;for(int i=0;i<3;++i){pitch+=rate[i]*right_[i];yaw+=rate[i]*up_[i];}
        out[0]=float(pitch);out[1]=float(yaw);out[2]=0;
    }
};
}
