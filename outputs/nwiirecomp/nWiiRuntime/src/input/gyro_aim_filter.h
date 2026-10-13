#pragma once
// Adapted from Dusklight's gyro EMA/deadband approach (CC0-1.0).
// Reference: TwilitRealm/dusklight, 34a144a493b7f57adeb4034d382317c826eb5c8d,
// src/dusk/gyro.cpp. Aurora supplies SDL sensor readings; the game applies this
// filtering. Use sensor time here so USB/Bluetooth and host FPS share the same
// response. Keep body axes and accelerometer data intact for Wii MotionPlus.
#include <algorithm>
#include <cmath>

namespace nwii::runtime::input {
class GyroAimFilter {
    float smooth_[3]{};
    bool ready_ = false;
public:
    void reset() { std::fill(smooth_, smooth_+3, 0.0f); ready_=false; }
    void process(float rate[3], double dt) {
        double magnitude = 0;
        for (int a=0; a<3; ++a) magnitude += double(rate[a])*rate[a];
        if (!std::isfinite(magnitude) || !(dt>0) || dt>0.1) {
            reset(); std::fill(rate,rate+3,0.0f); return;
        }
        // No residual EMA tail after the pad stops. This is a vector deadband,
        // so slow diagonal aiming does not lose each of its small components.
        constexpr double idle = 0.002; // rad/s, about 0.11 degrees/s; preserve very slow pointer motion
        if (magnitude < idle*idle) {
            reset(); std::fill(rate,rate+3,0.0f); return;
        }
        // Sword swings / deliberate fast motion stay responsive; smoothing
        // is confined to small aim corrections, rather than delayed swings.
        // Do not smooth very slow rates: the game's own MotionPlus zero-drift
        // compensation can mistake a flattened slow signal for a bias.
        const double alpha = (!ready_ || magnitude < 0.08*0.08 || magnitude >= 0.25*0.25)
            ? 1.0 : -std::expm1(-dt/0.012);
        for (int a=0; a<3; ++a) {
            smooth_[a] += float(alpha*(rate[a]-smooth_[a]));
            rate[a] = smooth_[a];
        }
        ready_ = true;
    }
};
} // namespace nwii::runtime::input
