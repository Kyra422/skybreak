#pragma once
// CTRL18-CAM: the camera-stick / rumble channel lives in include/runtime/camera_stick.h (reachable from every build
// and test through the runtime include directory); this forwarding header keeps "input/camera_stick.h" working.
#include "runtime/camera_stick.h"
