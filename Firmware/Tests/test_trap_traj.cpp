
#include <doctest.h>
#include <limits.h>
#include <cmath>
#include <iostream>
#include <initializer_list>
#include <random>

#include "MotorControl/utils.hpp"

// TODO: This is currently a copy-paste of the real code due to non-trivial
// include dependencies. Should include real code.

class TrapezoidalTrajectory {
public:
    struct Step_t {
        float Y;
        float Yd;
        float Ydd;
    };

    explicit TrapezoidalTrajectory();
    bool planTrapezoidal(float Xf, float Xi, float Vi,
                         float Vmax, float Amax, float Dmax);
    Step_t eval(double t);

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

    double t_;
};



// A sign function where input 0 has positive sign (not 0)
float sign_hard(float val) {
    return (std::signbit(val)) ? -1.0f : 1.0f;
}

// Symbol                     Description
// Ta, Tv and Td              Duration of the stages of the AL profile
// Xi and Vi                  Adapted initial conditions for the AL profile
// Xf                         Position set-point
// s                          Direction (sign) of the trajectory
// Vmax, Amax, Dmax and jmax  Kinematic bounds
// Ar, Dr and Vr              Reached values of acceleration and velocity

TrapezoidalTrajectory::TrapezoidalTrajectory() {}

bool TrapezoidalTrajectory::planTrapezoidal(float Xf, float Xi, float Vi,
                                            float Vmax, float Amax, float Dmax) {
    float dX = Xf - Xi;  // Distance to travel
    float stop_dist = (Vi * Vi) / (2.0f * Dmax); // Minimum stopping distance
    float dXstop = std::copysign(stop_dist, Vi); // Minimum stopping displacement
    float s = sign_hard(dX - dXstop); // Sign of coast velocity (if any)
    Ar_ = s * Amax;  // Maximum Acceleration (signed)
    Dr_ = -s * Dmax; // Maximum Deceleration (signed)
    Vr_ = s * Vmax;  // Maximum Velocity (signed)

    // If we start with a speed faster than cruising, then we need to decel instead of accel
    // aka "double deceleration move" in the paper
    if ((s * Vi) > (s * Vr_)) {
        Ar_ = -s * Amax;
    }

    // Time to accel/decel to/from Vr (cruise speed)
    Ta_ = (Vr_ - Vi) / Ar_;
    Td_ = -Vr_ / Dr_;

    // Integral of velocity ramps over the full accel and decel times to get
    // minimum displacement required to reach cuising speed
    float dXmin = 0.5f*Ta_*(Vr_ + Vi) + 0.5f*Td_*Vr_;

    // Are we displacing enough to reach cruising speed?
    if (s*dX < s*dXmin) {
        // Short move (triangle profile)
        Vr_ = s * std::sqrt(std::max((Dr_*SQ(Vi) + 2*Ar_*Dr_*dX) / (Dr_ - Ar_), 0.0f));
        //Vr_ = s * std::sqrt((Dr_*SQ(Vi) + 2*Ar_*Dr_*dX) / (Dr_ - Ar_));
        Ta_ = std::max(0.0f, (Vr_ - Vi) / Ar_);
        Td_ = std::max(0.0f, -Vr_ / Dr_);
        Tv_ = 0.0f;
    } else {
        // Long move (trapezoidal profile)
        Tv_ = (dX - dXmin) / Vr_;
    }

    // Fill in the rest of the values used at evaluation-time
    Tf_ = Ta_ + Tv_ + Td_;
    Xi_ = Xi;
    Xf_ = Xf;
    Vi_ = Vi;
    yAccel_ = Xi + Vi*Ta_ + 0.5f*Ar_*SQ(Ta_); // pos at end of accel phase

    return true;
}

TrapezoidalTrajectory::Step_t TrapezoidalTrajectory::eval(double t) {
    Step_t trajStep;
    const double Ta = static_cast<double>(Ta_);
    const double Tv = static_cast<double>(Tv_);
    const double Tf = static_cast<double>(Tf_);

    if (t < 0.0) {  // Initial Condition
        trajStep.Y   = Xi_;
        trajStep.Yd  = Vi_;
        trajStep.Ydd = 0.0f;
    } else if (t < Ta) {  // Accelerating
        const float accel_time = static_cast<float>(t);
        trajStep.Y   = Xi_ + Vi_*accel_time + 0.5f*Ar_*SQ(accel_time);
        trajStep.Yd  = Vi_ + Ar_*accel_time;
        trajStep.Ydd = Ar_;
    } else if (t < Ta + Tv) {  // Coasting
        const float coast_time = static_cast<float>(t - Ta);
        trajStep.Y   = yAccel_ + Vr_*coast_time;
        trajStep.Yd  = Vr_;
        trajStep.Ydd = 0.0f;
    } else if (t < Tf) {  // Deceleration
        const float td = static_cast<float>(t - Tf);
        trajStep.Y   = Xf_ + 0.5f*Dr_*SQ(td);
        trajStep.Yd  = Dr_*td;
        trajStep.Ydd = Dr_;
    } else if (t >= Tf) {  // Final Condition
        trajStep.Y   = Xf_;
        trajStep.Yd  = 0.0f;
        trajStep.Ydd = 0.0f;
    } else {
        // TODO: report error here
    }

    return trajStep;
}

static_assert(sizeof(float) * CHAR_BIT == 32);


void run_trajectory_test(float goal, float position, float velocity, float Vmax, float Amax, float Dmax) {
    float dt = 0.000125f;
    int replan_interval = 10; // must be > 2 (see note below)
    double t = 0.0;
    float Vmax_test = std::max(Vmax, std::abs(velocity));

    TrapezoidalTrajectory traj{};

    int replan_counter = 0;

    do {
        if (replan_counter <= 0) {
            CHECK(traj.planTrapezoidal(goal, position, velocity, Vmax, Amax, Dmax));
            t = 0.0;
            replan_counter = replan_interval;
        } else {
            replan_counter--;
        }

        TrapezoidalTrajectory::Step_t step = traj.eval(t);
        t += static_cast<double>(dt);

        //std::cerr << "vel: " << step.Yd << ", pos: " << step.Y << "\n";
        
        // Check if acceleration within bounds
        if (velocity >= 0.0f) {
            CHECK(step.Ydd <= Amax);
            CHECK(step.Ydd >= -Dmax);
            CHECK((step.Yd - velocity) / dt <= Amax * 1.002f);
            CHECK((step.Yd - velocity) / dt >= -Dmax * 1.002f);
        } else {
            CHECK(step.Ydd <= Dmax);
            CHECK(step.Ydd >= -Amax);
            CHECK((step.Yd - velocity) / dt <= Dmax * 1.002f);
            CHECK((step.Yd - velocity) / dt >= -Amax * 1.002f);
        }

        // Check if velocity within bounds
        CHECK(step.Yd >= -Vmax_test);
        CHECK(step.Yd <= Vmax_test);
        CHECK((step.Y - position) / dt >= -Vmax_test * 1.002f);
        CHECK((step.Y - position) / dt <= Vmax_test * 1.002f);
        velocity = step.Yd;

        // Check if position is making progress
        // TODO: the trajectory planner currently needs three "warm-up" iterations
        // until its position makes progress. This should probably be revisited.
        // TODO: this is disabled currently because there are legitimate trajectories
        // where the position first moves in the wrong direction.
        //if ((replan_counter < replan_interval - 2) && (t <= traj.Tf_)) {
        //    CHECK(std::abs(step.Y - goal) < std::abs(position - goal));
        //}
        position = step.Y;

    } while (t <= static_cast<double>(traj.Tf_));

    CHECK(position >= goal - 1.0f);
    CHECK(position <= goal + 1.0f);
    CHECK(velocity >= -Dmax * dt);
    CHECK(velocity <= Dmax * dt);
}


TEST_SUITE("Trajectory Planner") {
    TEST_CASE("long trajectory time remains accurate across 2048 seconds") {
        constexpr float dt = 0.000125f;
        constexpr float velocity = 30.0f;
        TrapezoidalTrajectory traj{};

        CHECK(traj.planTrapezoidal(1000000.0f, 0.0f, velocity,
                velocity, 1000.0f, 1000.0f));

        for (double base_time : {2048.0, 4096.0, 8192.0}) {
            traj.t_ = base_time;
            const float start_position = traj.eval(traj.t_).Y;
            for (int i = 0; i < 8000; ++i) {
                traj.t_ += static_cast<double>(dt);
            }
            const float end_position = traj.eval(traj.t_).Y;

            CHECK(traj.t_ - base_time == doctest::Approx(1.0).epsilon(1e-6));
            CHECK(static_cast<double>(end_position - start_position)
                    == doctest::Approx(static_cast<double>(velocity)).epsilon(1e-4));
        }
    }

    // these form a triangle trajectory because 2*v^2/(2*a) = 2 * 27712^2 / (2*22288) = 34456 > 16384
    TEST_CASE("neg-dir-triangle") {
        run_trajectory_test(-8192.0f, 8192.0f, 0.0f, 27712.0f, 22288.0f, 22288.0f);
    }
    TEST_CASE("pos-dir-triangle") {
        run_trajectory_test(8192.0f, -8192.0f, 0.0f, 27712.0f, 22288.0f, 22288.0f);
    }

    // these form a trapezoid trajectory because 2*v^2/(2*a) = 2 * 27712^2 / (2*22288) = 34456 < 16384
    TEST_CASE("neg-dir-trapezoid") {
        run_trajectory_test(-25000.0f, 25000.0f, 0.0f, 27712.0f, 22288.0f, 22288.0f);
    }
    TEST_CASE("pos-dir-trapezoid") {
        run_trajectory_test(25000.0f, -25000.0f, 0.0f, 27712.0f, 22288.0f, 22288.0f);
    }

    // for the following tests note that v^2/(2*a) = 27712^2 / (2*22288) = 17227 > 16384
    TEST_CASE("neg-dir-not-enough-braking-distance") {
        run_trajectory_test(-8192.0f, 8192.0f, -27712.0f, 27712.0f, 22288.0f, 22288.0f);
    }
    TEST_CASE("pos-dir-not-enough-braking-distance") {
        run_trajectory_test(8192.0f, -8192.0f, 27712.0f, 27712.0f, 22288.0f, 22288.0f);
    }

    TEST_CASE("neg-dir-over-speed") {
        run_trajectory_test(-8192.0f, 8192.0f, -40000.0f, 27712.0f, 22288.0f, 22288.0f);
    }
    TEST_CASE("pos-dir-over-speed") {
        run_trajectory_test(8192.0f, -8192.0f, 40000.0f, 27712.0f, 22288.0f, 22288.0f);
    }
}
