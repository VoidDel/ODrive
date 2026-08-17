#ifndef _TRAP_TRAJ_H
#define _TRAP_TRAJ_H

class TrapezoidalTrajectory {
public:
    struct Config_t {
        float vel_limit = 2.0f;   // [turn/s]
        float accel_limit = 0.5f; // [turn/s^2]
        float decel_limit = 0.5f; // [turn/s^2]
    };
    
    struct Step_t {
        float Y;
        float Yd;
        float Ydd;
    };

    bool planTrapezoidal(float Xf, float Xi, float Vi,
                         float Vmax, float Amax, float Dmax);
    Step_t eval(double t);

    Axis* axis_ = nullptr;  // set by Axis constructor
    Config_t config_;

    float Xi_;
    float Xf_;
    float Vi_;

    float Ar_;
    float Vr_;
    float Dr_;

    float Ta_;
    float Tv_;
    float Td_;
    float Tf_;

    float yAccel_;

    // Keep elapsed trajectory time in double precision. At the 8kHz control
    // rate a float accumulator starts advancing almost twice as fast after
    // roughly 2048 seconds, which corrupts long-running trajectory setpoints.
    double t_;
};

#endif
