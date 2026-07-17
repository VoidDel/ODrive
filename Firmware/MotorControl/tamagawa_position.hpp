#ifndef __TAMAGAWA_POSITION_HPP
#define __TAMAGAWA_POSITION_HPP

#include <cmath>
#include <cstdint>
#include <limits>

enum class TamagawaRebaseResult {
    UNCHANGED,
    REBASED,
    INVALID_STATE,
};

inline TamagawaRebaseResult rebase_tamagawa_linear_position(
        int32_t cpr,
        int32_t interval_turns,
        int32_t pending_delta,
        int32_t& shadow_count,
        float& pos_estimate_counts,
        int64_t& linear_turn_offset) {
    if (cpr <= 0 || interval_turns <= 0 || !std::isfinite(pos_estimate_counts)) {
        return TamagawaRebaseResult::INVALID_STATE;
    }

    const int64_t next_shadow_count = (int64_t)shadow_count + (int64_t)pending_delta;
    const int64_t rebase_threshold = (int64_t)cpr * (int64_t)interval_turns;
    const int64_t abs_shadow_count = shadow_count >= 0
            ? (int64_t)shadow_count
            : -(int64_t)shadow_count;
    const bool threshold_reached = abs_shadow_count >= rebase_threshold;
    const bool next_count_overflows =
            next_shadow_count > std::numeric_limits<int32_t>::max()
            || next_shadow_count < std::numeric_limits<int32_t>::min();

    if (!threshold_reached && !next_count_overflows) {
        return TamagawaRebaseResult::UNCHANGED;
    }

    // Only move whole turns so count_in_cpr_ and the FOC phase are unaffected.
    const int64_t turns_to_rebase = (int64_t)shadow_count / (int64_t)cpr;
    const int64_t counts_to_rebase = turns_to_rebase * (int64_t)cpr;
    const int64_t rebased_shadow_count = (int64_t)shadow_count - counts_to_rebase;
    const int64_t rebased_next_count = rebased_shadow_count + (int64_t)pending_delta;

    if (turns_to_rebase == 0
            || rebased_shadow_count > std::numeric_limits<int32_t>::max()
            || rebased_shadow_count < std::numeric_limits<int32_t>::min()
            || rebased_next_count > std::numeric_limits<int32_t>::max()
            || rebased_next_count < std::numeric_limits<int32_t>::min()
            || (turns_to_rebase > 0
                    && linear_turn_offset > std::numeric_limits<int64_t>::max() - turns_to_rebase)
            || (turns_to_rebase < 0
                    && linear_turn_offset < std::numeric_limits<int64_t>::min() - turns_to_rebase)) {
        return TamagawaRebaseResult::INVALID_STATE;
    }

    const float rebased_pos_estimate = pos_estimate_counts - (float)counts_to_rebase;
    if (!std::isfinite(rebased_pos_estimate)) {
        return TamagawaRebaseResult::INVALID_STATE;
    }

    shadow_count = (int32_t)rebased_shadow_count;
    pos_estimate_counts = rebased_pos_estimate;
    linear_turn_offset += turns_to_rebase;
    return TamagawaRebaseResult::REBASED;
}

#endif // __TAMAGAWA_POSITION_HPP
