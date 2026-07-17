#include <doctest.h>

#include "MotorControl/tamagawa_position.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>

namespace {

constexpr int32_t kCpr = 131072;
constexpr int32_t kRebaseIntervalTurns = 4096;

float global_position_counts(int64_t turn_offset, float local_position_counts) {
    return (float)turn_offset * (float)kCpr + local_position_counts;
}

} // namespace

TEST_SUITE("Tamagawa linear position rebase") {
    TEST_CASE("positive and negative rebases preserve the global position") {
        for (int direction : {-1, 1}) {
            int32_t shadow_count = direction * kRebaseIntervalTurns * kCpr;
            float pos_estimate_counts = (float)shadow_count + direction * 256.0f;
            int64_t turn_offset = 0;
            const float before = global_position_counts(turn_offset, pos_estimate_counts);

            const TamagawaRebaseResult result = rebase_tamagawa_linear_position(
                    kCpr,
                    kRebaseIntervalTurns,
                    direction * 100,
                    shadow_count,
                    pos_estimate_counts,
                    turn_offset);

            CHECK(result == TamagawaRebaseResult::REBASED);
            CHECK(shadow_count == 0);
            CHECK(turn_offset == direction * kRebaseIntervalTurns);
            CHECK(global_position_counts(turn_offset, pos_estimate_counts) == before);
        }
    }

    TEST_CASE("repeated rebases cross 16384 turns without overflowing local state") {
        int32_t shadow_count = 0;
        float pos_estimate_counts = 0.0f;
        int64_t turn_offset = 0;

        for (int i = 1; i <= 4; ++i) {
            shadow_count = kRebaseIntervalTurns * kCpr;
            pos_estimate_counts = (float)shadow_count;

            const TamagawaRebaseResult result = rebase_tamagawa_linear_position(
                    kCpr,
                    kRebaseIntervalTurns,
                    1,
                    shadow_count,
                    pos_estimate_counts,
                    turn_offset);

            CHECK(result == TamagawaRebaseResult::REBASED);
            CHECK(shadow_count == 0);
            CHECK(pos_estimate_counts == 0.0f);
            CHECK(turn_offset == (int64_t)i * kRebaseIntervalTurns);
        }

        CHECK(turn_offset == 16384);
        CHECK(global_position_counts(turn_offset, pos_estimate_counts)
                == (float)std::numeric_limits<int32_t>::max() + 1.0f);
    }

    TEST_CASE("continuous motion remains monotonic beyond the old overflow boundary") {
        constexpr int32_t kStepCounts = kCpr / 8;
        constexpr int32_t kTargetTurns = 20000;
        int32_t shadow_count = 0;
        float pos_estimate_counts = 0.0f;
        int64_t turn_offset = 0;
        float previous_global_counts = 0.0f;
        bool stayed_monotonic = true;
        bool stayed_in_local_range = true;
        bool stayed_valid = true;

        for (int32_t i = 0; i < kTargetTurns * 8; ++i) {
            const TamagawaRebaseResult result = rebase_tamagawa_linear_position(
                    kCpr,
                    kRebaseIntervalTurns,
                    kStepCounts,
                    shadow_count,
                    pos_estimate_counts,
                    turn_offset);
            if (result == TamagawaRebaseResult::INVALID_STATE) {
                stayed_valid = false;
                break;
            }

            shadow_count = (int32_t)((int64_t)shadow_count + kStepCounts);
            pos_estimate_counts += (float)kStepCounts;
            const float global_counts = global_position_counts(turn_offset, pos_estimate_counts);
            stayed_monotonic = stayed_monotonic && global_counts >= previous_global_counts;
            stayed_in_local_range = stayed_in_local_range
                    && shadow_count <= kRebaseIntervalTurns * kCpr + kStepCounts;
            previous_global_counts = global_counts;
        }

        CHECK(stayed_valid);
        CHECK(stayed_monotonic);
        CHECK(stayed_in_local_range);
        CHECK(turn_offset == 16384);
        CHECK(previous_global_counts == (float)kTargetTurns * (float)kCpr);
    }

    TEST_CASE("an imminent int32 overflow forces an early safe rebase") {
        int32_t shadow_count = std::numeric_limits<int32_t>::max() - 10;
        float pos_estimate_counts = (float)shadow_count;
        int64_t turn_offset = 0;

        const TamagawaRebaseResult result = rebase_tamagawa_linear_position(
                kCpr,
                std::numeric_limits<int32_t>::max(),
                100,
                shadow_count,
                pos_estimate_counts,
                turn_offset);

        CHECK(result == TamagawaRebaseResult::REBASED);
        CHECK((int64_t)shadow_count + 100 <= std::numeric_limits<int32_t>::max());
        CHECK(turn_offset == 16383);
    }

    TEST_CASE("invalid CPR is rejected without changing state") {
        int32_t shadow_count = 123;
        float pos_estimate_counts = 123.0f;
        int64_t turn_offset = 456;

        const TamagawaRebaseResult result = rebase_tamagawa_linear_position(
                0,
                kRebaseIntervalTurns,
                1,
                shadow_count,
                pos_estimate_counts,
                turn_offset);

        CHECK(result == TamagawaRebaseResult::INVALID_STATE);
        CHECK(shadow_count == 123);
        CHECK(pos_estimate_counts == 123.0f);
        CHECK(turn_offset == 456);
    }
}
