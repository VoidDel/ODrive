
#include "odrive_main.h"
#include <Drivers/STM32/stm32_system.h>
#include <bitset>

static volatile uint32_t g_tamagawa_debug_hold = 0;

static inline uint32_t tamagawa_make_debug_word(uint8_t stage, uint8_t info, HAL_StatusTypeDef tx_status, HAL_StatusTypeDef rx_status) {
    return ((uint32_t)stage << 24)
            | ((uint32_t)info << 16)
            | ((uint32_t)(uint8_t)tx_status << 8)
            | ((uint32_t)(uint8_t)rx_status);
}

static inline void tamagawa_clear_debug_hold() {
    g_tamagawa_debug_hold = 0;
}

static inline void tamagawa_hold_debug(uint8_t stage, uint8_t info, HAL_StatusTypeDef tx_status, HAL_StatusTypeDef rx_status) {
    g_tamagawa_debug_hold = tamagawa_make_debug_word(stage, info, tx_status, rx_status);
    odrv.test_property_ = g_tamagawa_debug_hold;
}

static inline void tamagawa_set_debug(uint8_t stage, uint8_t info, HAL_StatusTypeDef tx_status, HAL_StatusTypeDef rx_status) {
    if (g_tamagawa_debug_hold != 0 && stage < 0x80) {
        odrv.test_property_ = g_tamagawa_debug_hold;
        return;
    }

    odrv.test_property_ = tamagawa_make_debug_word(stage, info, tx_status, rx_status);
}

static inline uint8_t tamagawa_get_reset_command_byte(uint8_t data_id) {
    // The currently attached encoder uses a vendor-specific raw 0xAA command
    // for single-turn reset instead of the standard Tamagawa Data ID 0x8 CF.
    return (data_id == Encoder::TAMAGAWA_DATA_ID_RESET_SINGLE_TURN)
            ? 0xAA
            : Encoder::TAMAGAWA_SINK_CODE | ((data_id & 0x0F) << 3)
                    | ((((data_id >> 0) ^ (data_id >> 1) ^ (data_id >> 2) ^ (data_id >> 3)) & 0x1) << 7);
}

Encoder::Encoder(TIM_HandleTypeDef* timer, Stm32Gpio index_gpio,
                 Stm32Gpio hallA_gpio, Stm32Gpio hallB_gpio, Stm32Gpio hallC_gpio,
                 Stm32SpiArbiter* spi_arbiter) :
        timer_(timer), index_gpio_(index_gpio),
        hallA_gpio_(hallA_gpio), hallB_gpio_(hallB_gpio), hallC_gpio_(hallC_gpio),
        spi_arbiter_(spi_arbiter)
{
}

static void enc_index_cb_wrapper(void* ctx) {
    reinterpret_cast<Encoder*>(ctx)->enc_index_cb();
}

bool Encoder::apply_config(ODriveIntf::MotorIntf::MotorType motor_type) {
    config_.parent = this;

    update_pll_gains();

    if (config_.pre_calibrated) {
        if (config_.mode == Encoder::MODE_HALL && config_.hall_polarity_calibrated)
            is_ready_ = true;
        if (config_.mode == Encoder::MODE_SINCOS)
            is_ready_ = true;
        if (config_.mode == Encoder::MODE_UART_ABS_TAMAGAWA)
            is_ready_ = true;
        if (motor_type == Motor::MOTOR_TYPE_ACIM)
            is_ready_ = true;
    }

    return true;
}

void Encoder::setup() {
    HAL_TIM_Encoder_Start(timer_, TIM_CHANNEL_ALL);
    set_idx_subscribe();

    mode_ = config_.mode;

    spi_task_.config = {
        .Mode = SPI_MODE_MASTER,
        .Direction = SPI_DIRECTION_2LINES,
        .DataSize = SPI_DATASIZE_16BIT,
        .CLKPolarity = (mode_ == MODE_SPI_ABS_AEAT || mode_ == MODE_SPI_ABS_MA732) ? SPI_POLARITY_HIGH : SPI_POLARITY_LOW,
        .CLKPhase = SPI_PHASE_2EDGE,
        .NSS = SPI_NSS_SOFT,
        .BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16,
        .FirstBit = SPI_FIRSTBIT_MSB,
        .TIMode = SPI_TIMODE_DISABLE,
        .CRCCalculation = SPI_CRCCALCULATION_DISABLE,
        .CRCPolynomial = 10,
    };

    if (mode_ == MODE_SPI_ABS_MA732) {
        abs_spi_dma_tx_[0] = 0x0000;
    }

    if(mode_ & MODE_FLAG_ABS){
        // Skip SPI CS pin initialization for Tamagawa mode (uses UART, not SPI)
        if (mode_ != MODE_UART_ABS_TAMAGAWA) {
            abs_spi_cs_pin_init();
        }

        if (axis_->controller_.config_.anticogging.pre_calibrated) {
            axis_->controller_.anticogging_valid_ = true;
        }
    }
    
    // Initialize Tamagawa encoder if selected
    if (mode_ == MODE_UART_ABS_TAMAGAWA) {
        if (!tamagawa_init()) {
            set_error(ERROR_ABS_SPI_COM_FAIL); // Reuse SPI error for UART communication failure
        }
    }
}

void Encoder::set_error(Error error) {
    vel_estimate_valid_ = false;
    pos_estimate_valid_ = false;
    error_ |= error;
    axis_->error_ |= Axis::ERROR_ENCODER_FAILED;
}

bool Encoder::do_checks(){
    return error_ == ERROR_NONE;
}

//--------------------
// Hardware Dependent
//--------------------

// Triggered when an encoder passes over the "Index" pin
// TODO: only arm index edge interrupt when we know encoder has powered up
// (maybe by attaching the interrupt on start search, synergistic with following)
void Encoder::enc_index_cb() {
    if (config_.use_index) {
        set_circular_count(0, false);
        if (config_.use_index_offset)
            set_linear_count((int32_t)(config_.index_offset * config_.cpr));
        if (config_.pre_calibrated) {
            is_ready_ = true;
            if(axis_->controller_.config_.anticogging.pre_calibrated){
                axis_->controller_.anticogging_valid_ = true;
            }
        } else {
            // We can't use the update_offset facility in set_circular_count because
            // we also set the linear count before there is a chance to update. Therefore:
            // Invalidate offset calibration that may have happened before idx search
            is_ready_ = false;
        }
        index_found_ = true;
    }

    // Disable interrupt
    index_gpio_.unsubscribe();
}

void Encoder::set_idx_subscribe(bool override_enable) {
    if (config_.use_index && (override_enable || !config_.find_idx_on_lockin_only)) {
        if (!index_gpio_.subscribe(true, false, enc_index_cb_wrapper, this)) {
            odrv.misconfigured_ = true;
        }
    } else if (!config_.use_index || config_.find_idx_on_lockin_only) {
        index_gpio_.unsubscribe();
    }
}

void Encoder::update_pll_gains() {
    pll_kp_ = 2.0f * config_.bandwidth;  // basic conversion to discrete time
    pll_ki_ = 0.25f * (pll_kp_ * pll_kp_); // Critically damped

    // Check that we don't get problems with discrete time approximation
    if (!(current_meas_period * pll_kp_ < 1.0f)) {
        set_error(ERROR_UNSTABLE_GAIN);
    }
}

void Encoder::check_pre_calibrated() {
    // TODO: restoring config from python backup is fragile here (ACIM motor type must be set first)
    if (axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_ACIM) {
        if (!is_ready_)
            config_.pre_calibrated = false;
        if (mode_ == MODE_INCREMENTAL && !index_found_)
            config_.pre_calibrated = false;
    }
}

// Function that sets the current encoder count to a desired 32-bit value.
void Encoder::set_linear_count(int32_t count) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    // Update states
    shadow_count_ = count;
    pos_estimate_counts_ = (float)count;
    tim_cnt_sample_ = count;

    //Write hardware last
    timer_->Instance->CNT = count;

    cpu_exit_critical(prim);
}

// Function that sets the CPR circular tracking encoder count to a desired 32-bit value.
// Note that this will get mod'ed down to [0, cpr)
void Encoder::set_circular_count(int32_t count, bool update_offset) {
    // Disable interrupts to make a critical section to avoid race condition
    uint32_t prim = cpu_enter_critical();

    if (update_offset) {
        config_.phase_offset += count - count_in_cpr_;
        config_.phase_offset = mod(config_.phase_offset, config_.cpr);
    }

    // Update states
    count_in_cpr_ = mod(count, config_.cpr);
    pos_cpr_counts_ = (float)count_in_cpr_;

    cpu_exit_critical(prim);
}

bool Encoder::run_index_search() {
    config_.use_index = true;
    index_found_ = false;
    set_idx_subscribe();

    bool success = axis_->run_lockin_spin(axis_->config_.calibration_lockin, false);
    return success;
}

bool Encoder::run_direction_find() {
    int32_t init_enc_val = shadow_count_;

    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 3.0f; // run for 3 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;
    bool success = axis_->run_lockin_spin(lockin_config, false);

    if (success) {
        // Check response and direction
        if (shadow_count_ > init_enc_val + 8) {
            // motor same dir as encoder
            config_.direction = 1;
        } else if (shadow_count_ < init_enc_val - 8) {
            // motor opposite dir as encoder
            config_.direction = -1;
        } else {
            config_.direction = 0;
        }
    }

    return success;
}


bool Encoder::run_hall_polarity_calibration() {
    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 3.0f; // run for 3 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;

    auto loop_cb = [this](bool const_vel) {
        if (const_vel)
            sample_hall_states_ = true;
        // No need to cancel early
        return true;
    };

    config_.hall_polarity_calibrated = false;
    states_seen_count_.fill(0);
    bool success = axis_->run_lockin_spin(lockin_config, false, loop_cb);
    sample_hall_states_ = false;

    if (success) {
        std::bitset<8> state_seen;
        std::bitset<8> state_confirmed;
        for (int i = 0; i < 8; i++) {
            if (states_seen_count_[i] > 0)
                state_seen[i] = true;
            if (states_seen_count_[i] > 50)
                state_confirmed[i] = true;
        }
        if (!(state_seen == state_confirmed)) {
            set_error(ERROR_ILLEGAL_HALL_STATE);
            return false;
        }

        // Hall effect sensors can be arranged at 60 or 120 electrical degrees.
        // Out of 8 possible states, 120 and 60 deg arrangements each miss 2 states.
        // ODrive assumes 120 deg separation - if a 60 deg setup is used, it can
        // be converted to 120 deg states by flipping the polarity of one sensor.
        uint8_t states = state_seen.to_ulong();
        uint8_t hall_polarity = 0;
        auto flip_detect = [](uint8_t states, unsigned int idx)->bool {
            return (~states & 0xFF) == (1<<(0+idx) | 1<<(7-idx));
        };
        if (flip_detect(states, 0)) {
            hall_polarity = 0b000;
        } else if (flip_detect(states, 1)) {
            hall_polarity = 0b001;
        } else if (flip_detect(states, 2)) {
            hall_polarity = 0b010;
        } else if (flip_detect(states, 3)) {
            hall_polarity = 0b100;
        } else {
            set_error(ERROR_ILLEGAL_HALL_STATE);
            return false;
        }
        config_.hall_polarity = hall_polarity;
        config_.hall_polarity_calibrated = true;
    }

    return success;
}

bool Encoder::run_hall_phase_calibration() {
    Axis::LockinConfig_t lockin_config = axis_->config_.calibration_lockin;
    lockin_config.finish_distance = lockin_config.vel * 30.0f; // run for 30 seconds
    lockin_config.finish_on_distance = true;
    lockin_config.finish_on_enc_idx = false;
    lockin_config.finish_on_vel = false;

    auto loop_cb = [this](bool const_vel) {
        if (const_vel)
            sample_hall_phase_ = true;
        // No need to cancel early
        return true;
    };

    // TODO: There is a race condition here with the execution in Encoder::update.
    // We should evaluate making thread execution synchronous with the control loops
    // at least optionally.
    // Perhaps the new loop_sync feature will give a loose timing guarantee that may be sufficient
    calibrate_hall_phase_ = true;
    config_.hall_edge_phcnt.fill(0.0f);
    hall_phase_calib_seen_count_.fill(0);
    bool success = axis_->run_lockin_spin(lockin_config, false, loop_cb);
    if (error_ & ERROR_ILLEGAL_HALL_STATE)
        success = false;

    if (success) {
        // Check deltas to dicern rotation direction
        float delta_phase = 0.0f;
        for (int i = 0; i < 6; i++) {
            int next_i = (i == 5) ? 0 : i+1;
            delta_phase += wrap_pm_pi(config_.hall_edge_phcnt[next_i] - config_.hall_edge_phcnt[i]);
        }
        // Correct reverse rotation
        if (delta_phase < 0.0f) {
            config_.direction = -1;
            for (int i = 0; i < 6; i++)
                config_.hall_edge_phcnt[i] = wrap_pm_pi(-config_.hall_edge_phcnt[i]);
        } else {
            config_.direction = 1;
        }
        // Normalize edge timing to 1st edge in sequence, and change units to counts
        float offset = config_.hall_edge_phcnt[0];
        for (int i = 0; i < 6; i++) {
            float& phcnt = config_.hall_edge_phcnt[i];
            phcnt = fmodf_pos((6.0f / (2.0f * M_PI)) * (phcnt - offset), 6.0f);
        }
    } else {
        config_.hall_edge_phcnt = hall_edge_defaults;
    }

    calibrate_hall_phase_ = false;
    return success;
}

// @brief Turns the motor in one direction for a bit and then in the other
// direction in order to find the offset between the electrical phase 0
// and the encoder state 0.
bool Encoder::run_offset_calibration() {
    const float start_lock_duration = 1.0f;

    // Require index found if enabled
    if (config_.use_index && !index_found_) {
        set_error(ERROR_INDEX_NOT_FOUND_YET);
        return false;
    }

    if (config_.mode == MODE_HALL && !config_.hall_polarity_calibrated) {
        set_error(ERROR_HALL_NOT_CALIBRATED_YET);
        return false;
    }

    // We use shadow_count_ to do the calibration, but the offset is used by count_in_cpr_
    // Therefore we have to sync them for calibration
    shadow_count_ = count_in_cpr_;

    CRITICAL_SECTION() {
        // Reset state variables
        axis_->open_loop_controller_.Idq_setpoint_ = {0.0f, 0.0f};
        axis_->open_loop_controller_.Vdq_setpoint_ = {0.0f, 0.0f};
        axis_->open_loop_controller_.phase_ = 0.0f;
        axis_->open_loop_controller_.phase_vel_ = 0.0f;

        float max_current_ramp = axis_->motor_.config_.calibration_current / start_lock_duration * 2.0f;
        axis_->open_loop_controller_.max_current_ramp_ = max_current_ramp;
        axis_->open_loop_controller_.max_voltage_ramp_ = max_current_ramp;
        axis_->open_loop_controller_.max_phase_vel_ramp_ = INFINITY;
        axis_->open_loop_controller_.target_current_ = axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL ? axis_->motor_.config_.calibration_current : 0.0f;
        axis_->open_loop_controller_.target_voltage_ = axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL ? 0.0f : axis_->motor_.config_.calibration_current;
        axis_->open_loop_controller_.target_vel_ = 0.0f;
        axis_->open_loop_controller_.total_distance_ = 0.0f;
        axis_->open_loop_controller_.phase_ = axis_->open_loop_controller_.initial_phase_ = wrap_pm_pi(0 - config_.calib_scan_distance / 2.0f);

        axis_->motor_.current_control_.enable_current_control_src_ = (axis_->motor_.config_.motor_type != Motor::MOTOR_TYPE_GIMBAL);
        axis_->motor_.current_control_.Idq_setpoint_src_.connect_to(&axis_->open_loop_controller_.Idq_setpoint_);
        axis_->motor_.current_control_.Vdq_setpoint_src_.connect_to(&axis_->open_loop_controller_.Vdq_setpoint_);
        
        axis_->motor_.current_control_.phase_src_.connect_to(&axis_->open_loop_controller_.phase_);
        axis_->acim_estimator_.rotor_phase_src_.connect_to(&axis_->open_loop_controller_.phase_);

        axis_->motor_.phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
        axis_->motor_.current_control_.phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
        axis_->acim_estimator_.rotor_phase_vel_src_.connect_to(&axis_->open_loop_controller_.phase_vel_);
    }
    axis_->wait_for_control_iteration();

    axis_->motor_.arm(&axis_->motor_.current_control_);

    // go to start position of forward scan for start_lock_duration to get ready to scan
    for (size_t i = 0; i < (size_t)(start_lock_duration * 1000.0f); ++i) {
        if (!axis_->motor_.is_armed_) {
            return false; // TODO: return "disarmed" error code
        }
        if (axis_->requested_state_ != Axis::AXIS_STATE_UNDEFINED) {
            axis_->motor_.disarm();
            return false; // TODO: return "aborted" error code
        }
        osDelay(1);
    }


    int32_t init_enc_val = shadow_count_;
    uint32_t num_steps = 0;
    int64_t encvaluesum = 0;

    CRITICAL_SECTION() {
        axis_->open_loop_controller_.target_vel_ = config_.calib_scan_omega;
        axis_->open_loop_controller_.total_distance_ = 0.0f;
    }

    // scan forward
    while ((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_) {
        bool reached_target_dist = axis_->open_loop_controller_.total_distance_.any().value_or(-INFINITY) >= config_.calib_scan_distance;
        if (reached_target_dist) {
            break;
        }
        encvaluesum += shadow_count_;
        num_steps++;
        osDelay(1);
    }

    // Check response and direction
    if (shadow_count_ > init_enc_val + 8) {
        // motor same dir as encoder
        config_.direction = 1;
    } else if (shadow_count_ < init_enc_val - 8) {
        // motor opposite dir as encoder
        config_.direction = -1;
    } else {
        // Encoder response error
        set_error(ERROR_NO_RESPONSE);
        axis_->motor_.disarm();
        return false;
    }

    // Check CPR
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float expected_encoder_delta = config_.calib_scan_distance / elec_rad_per_enc;
    calib_scan_response_ = std::abs(shadow_count_ - init_enc_val);
    if (std::abs(calib_scan_response_ - expected_encoder_delta) / expected_encoder_delta > config_.calib_range) {
        set_error(ERROR_CPR_POLEPAIRS_MISMATCH);
        axis_->motor_.disarm();
        return false;
    }

    CRITICAL_SECTION() {
        axis_->open_loop_controller_.target_vel_ = -config_.calib_scan_omega;
    }

    // scan backwards
    while ((axis_->requested_state_ == Axis::AXIS_STATE_UNDEFINED) && axis_->motor_.is_armed_) {
        bool reached_target_dist = axis_->open_loop_controller_.total_distance_.any().value_or(INFINITY) <= 0.0f;
        if (reached_target_dist) {
            break;
        }
        encvaluesum += shadow_count_;
        num_steps++;
        osDelay(1);
    }

    // Motor disarmed because of an error
    if (!axis_->motor_.is_armed_) {
        return false;
    }

    axis_->motor_.disarm();

    config_.phase_offset = encvaluesum / num_steps;
    int32_t residual = encvaluesum - ((int64_t)config_.phase_offset * (int64_t)num_steps);
    config_.phase_offset_float = (float)residual / (float)num_steps + 0.5f;  // add 0.5 to center-align state to phase

    is_ready_ = true;
    return true;
}

bool Encoder::tamagawa_clear_encoder_errors() {
    return tamagawa_execute_reset_sequence(TAMAGAWA_DATA_ID_RESET_ERRORS);
}

bool Encoder::tamagawa_reset_single_turn() {
    return tamagawa_execute_reset_sequence(TAMAGAWA_DATA_ID_RESET_SINGLE_TURN);
}

bool Encoder::tamagawa_reset_multi_turn_and_errors() {
    return tamagawa_execute_reset_sequence(TAMAGAWA_DATA_ID_RESET_MULTI_TURN_ERRORS);
}

std::tuple<bool, uint8_t, bool> Encoder::tamagawa_read_eeprom(uint8_t page, uint8_t address) {
    tamagawa_set_debug(0x50, address, HAL_OK, HAL_OK);
    if (!tamagawa_can_run_manual_command()) {
        tamagawa_set_debug(0x51, address, HAL_BUSY, HAL_BUSY);
        return std::make_tuple(false, uint8_t{0}, false);
    }

    uint32_t prim = cpu_enter_critical();
    tamagawa_manual_transaction_active_ = true;
    cpu_exit_critical(prim);

    HAL_UART_AbortTransmit(config_.tamagawa_uart);
    HAL_UART_AbortReceive(config_.tamagawa_uart);
    tamagawa_dma_active_ = false;
    tamagawa_waiting_for_rx_ = false;
    tamagawa_rx_complete_ = false;
    tamagawa_tx_complete_ = false;
    tamagawa_set_debug(0x52, address, HAL_OK, HAL_OK);

    bool success = false;
    uint8_t value = 0;
    bool busy = false;
    uint8_t response[4];

    if (!tamagawa_select_eeprom_page(page)) {
        tamagawa_set_debug(0x53, page, HAL_ERROR, HAL_ERROR);
    } else if (!tamagawa_send_eeprom_read_transaction(address, response)) {
        tamagawa_set_debug(0x54, address, HAL_ERROR, HAL_ERROR);
    } else if (!tamagawa_parse_eeprom_response(TAMAGAWA_DATA_ID_EEPROM_READ, address, response, &value, &busy)) {
        tamagawa_set_debug(0x55, address, HAL_ERROR, HAL_ERROR);
    } else {
        tamagawa_set_debug(0x56, address, HAL_OK, HAL_OK);
        success = true;
    }

    prim = cpu_enter_critical();
    tamagawa_manual_transaction_active_ = false;
    cpu_exit_critical(prim);

    if (!success) {
        tamagawa_error_count_++;
    }

    return std::make_tuple(success, value, busy);
}

std::tuple<bool, bool> Encoder::tamagawa_write_eeprom(uint8_t page, uint8_t address, uint8_t value) {
    if (!tamagawa_can_run_manual_command()) {
        return std::make_tuple(false, false);
    }

    uint32_t prim = cpu_enter_critical();
    tamagawa_manual_transaction_active_ = true;
    cpu_exit_critical(prim);

    HAL_UART_AbortTransmit(config_.tamagawa_uart);
    HAL_UART_AbortReceive(config_.tamagawa_uart);
    tamagawa_dma_active_ = false;
    tamagawa_waiting_for_rx_ = false;
    tamagawa_rx_complete_ = false;
    tamagawa_tx_complete_ = false;

    bool success = false;
    bool busy = false;
    uint8_t response[4];

    if (tamagawa_select_eeprom_page(page)
            && tamagawa_send_eeprom_write_transaction(address, value, response)
            && tamagawa_parse_eeprom_response(TAMAGAWA_DATA_ID_EEPROM_WRITE, address, response, nullptr, &busy)
            && tamagawa_wait_for_eeprom_write_cycle()) {
        success = true;
    }

    prim = cpu_enter_critical();
    tamagawa_manual_transaction_active_ = false;
    cpu_exit_critical(prim);

    if (!success) {
        tamagawa_error_count_++;
    }

    return std::make_tuple(success, busy);
}

std::tuple<bool, float> Encoder::tamagawa_read_temperature() {
    auto result = tamagawa_read_eeprom(TAMAGAWA_TEMPERATURE_PAGE, TAMAGAWA_TEMPERATURE_ADDRESS);
    if (!std::get<0>(result)) {
        return std::make_tuple(false, 0.0f);
    }

    return std::make_tuple(true, static_cast<float>(std::get<1>(result)));
}

static bool decode_hall(uint8_t hall_state, int32_t* hall_cnt) {
    switch (hall_state) {
        case 0b001: *hall_cnt = 0; return true;
        case 0b011: *hall_cnt = 1; return true;
        case 0b010: *hall_cnt = 2; return true;
        case 0b110: *hall_cnt = 3; return true;
        case 0b100: *hall_cnt = 4; return true;
        case 0b101: *hall_cnt = 5; return true;
        default: return false;
    }
}

void Encoder::sample_now() {
    switch (mode_) {
        case MODE_INCREMENTAL: {
            tim_cnt_sample_ = (int16_t)timer_->Instance->CNT;
        } break;

        case MODE_HALL: {
            // do nothing: samples already captured in general GPIO capture
        } break;

        case MODE_SINCOS: {
            sincos_sample_s_ = get_adc_relative_voltage(get_gpio(config_.sincos_gpio_pin_sin)) - 0.5f;
            sincos_sample_c_ = get_adc_relative_voltage(get_gpio(config_.sincos_gpio_pin_cos)) - 0.5f;
        } break;

        case MODE_SPI_ABS_AMS:
        case MODE_SPI_ABS_CUI:
        case MODE_SPI_ABS_AEAT:
        case MODE_SPI_ABS_RLS:
        case MODE_SPI_ABS_MA732:
        {
            abs_spi_start_transaction();
            // Do nothing
        } break;
        
        case MODE_UART_ABS_TAMAGAWA:
        {
            if (tamagawa_manual_transaction_active_) {
                break;
            }
            const uint32_t now_us = micros();
            if ((uint32_t)(now_us - tamagawa_last_poll_us_) >= TAMAGAWA_POLL_INTERVAL_US) {
                tamagawa_last_poll_us_ = now_us;
                // Use DMA for non-blocking communication
                // This allows both axes to communicate in parallel
                tamagawa_send_command_dma(TAMAGAWA_DATA_ID_ABS);
            }
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
        } break;
    }

    // Sample all GPIO digital input data registers, used for HALL sensors for example.
    for (size_t i = 0; i < sizeof(ports_to_sample) / sizeof(ports_to_sample[0]); ++i) {
        port_samples_[i] = ports_to_sample[i]->IDR;
    }
}

bool Encoder::read_sampled_gpio(Stm32Gpio gpio) {
    for (size_t i = 0; i < sizeof(ports_to_sample) / sizeof(ports_to_sample[0]); ++i) {
        if (ports_to_sample[i] == gpio.port_) {
            return port_samples_[i] & gpio.pin_mask_;
        }
    }
    return false;
}

void Encoder::decode_hall_samples() {
    hall_state_ = (read_sampled_gpio(hallA_gpio_) ? 1 : 0)
                | (read_sampled_gpio(hallB_gpio_) ? 2 : 0)
                | (read_sampled_gpio(hallC_gpio_) ? 4 : 0);
}

bool Encoder::abs_spi_start_transaction() {
    if (mode_ & MODE_FLAG_ABS){
        if (Stm32SpiArbiter::acquire_task(&spi_task_)) {
            spi_task_.ncs_gpio = abs_spi_cs_gpio_;
            spi_task_.tx_buf = (uint8_t*)abs_spi_dma_tx_;
            spi_task_.rx_buf = (uint8_t*)abs_spi_dma_rx_;
            spi_task_.length = 1;
            spi_task_.on_complete = [](void* ctx, bool success) { ((Encoder*)ctx)->abs_spi_cb(success); };
            spi_task_.on_complete_ctx = this;
            spi_task_.next = nullptr;
            
            spi_arbiter_->transfer_async(&spi_task_);
        } else {
            return false;
        }
    }
    return true;
}

uint8_t ams_parity(uint16_t v) {
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    v ^= v >> 1;
    return v & 1;
}

uint8_t cui_parity(uint16_t v) {
    v ^= v >> 8;
    v ^= v >> 4;
    v ^= v >> 2;
    return ~v & 3;
}

void Encoder::abs_spi_cb(bool success) {
    uint16_t pos;

    if (!success) {
        goto done;
    }

    switch (mode_) {
        case MODE_SPI_ABS_AMS: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            // check if parity is correct (even) and error flag clear
            if (ams_parity(rawVal) || ((rawVal >> 14) & 1)) {
                goto done;
            }
            pos = rawVal & 0x3fff;
        } break;

        case MODE_SPI_ABS_CUI: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            // check if parity is correct
            if (cui_parity(rawVal)) {
                goto done;
            }
            pos = rawVal & 0x3fff;
        } break;

        case MODE_SPI_ABS_RLS: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            pos = (rawVal >> 2) & 0x3fff;
        } break;

        case MODE_SPI_ABS_MA732: {
            uint16_t rawVal = abs_spi_dma_rx_[0];
            pos = (rawVal >> 2) & 0x3fff;
        } break;

        default: {
           set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
           goto done;
        } break;
    }

    pos_abs_ = pos;
    abs_spi_pos_updated_ = true;
    if (config_.pre_calibrated) {
        is_ready_ = true;
    }

done:
    Stm32SpiArbiter::release_task(&spi_task_);
}

void Encoder::abs_spi_cs_pin_init(){
    // Decode and init cs pin
#if HW_VERSION_MAJOR == 4
    if (mode_ == MODE_SPI_ABS_MA732)
        abs_spi_cs_gpio_ = {GPIOA, GPIO_PIN_15};
    else
#else
    abs_spi_cs_gpio_ = get_gpio(config_.abs_spi_cs_gpio_pin);
#endif
    abs_spi_cs_gpio_.config(GPIO_MODE_OUTPUT_PP, GPIO_PULLUP);

    // Write pin high
    abs_spi_cs_gpio_.write(true);
}

// Note that this may return counts +1 or -1 without any wrapping
int32_t Encoder::hall_model(float internal_pos) {
    int32_t base_cnt = (int32_t)std::floor(internal_pos);

    float pos_in_range = fmodf_pos(internal_pos, 6.0f);
    int pos_idx = (int)pos_in_range;
    if (pos_idx == 6) pos_idx = 5; // in case of rounding error
    int next_i = (pos_idx == 5) ? 0 : pos_idx+1;

    float below_edge = config_.hall_edge_phcnt[pos_idx];
    float above_edge = config_.hall_edge_phcnt[next_i];

    // if we are blow the "below" edge, we are the count under
    if (wrap_pm(pos_in_range - below_edge, 6.0f) < 0.0f)
        return base_cnt - 1;
    // if we are above the "above" edge, we are the count over
    else if (wrap_pm(pos_in_range - above_edge, 6.0f) > 0.0f)
        return base_cnt + 1;
    // otherwise we are in the nominal count (or completely lost)
    return base_cnt;
}

bool Encoder::update() {
    // update internal encoder state.
    int32_t delta_enc = 0;
    int32_t pos_abs_latched = pos_abs_; //LATCH

    switch (mode_) {
        case MODE_INCREMENTAL: {
            //TODO: use count_in_cpr_ instead as shadow_count_ can overflow
            //or use 64 bit
            int16_t delta_enc_16 = (int16_t)tim_cnt_sample_ - (int16_t)shadow_count_;
            delta_enc = (int32_t)delta_enc_16; //sign extend
        } break;

        case MODE_HALL: {
            decode_hall_samples();
            if (sample_hall_states_) {
                states_seen_count_[hall_state_]++;
            }
            if (config_.hall_polarity_calibrated) {
                int32_t hall_cnt;
                if (decode_hall((hall_state_ ^ config_.hall_polarity), &hall_cnt)) {
                    if (calibrate_hall_phase_) {
                        if (sample_hall_phase_ && last_hall_cnt_.has_value()) {
                            int mod_hall_cnt = mod(hall_cnt - last_hall_cnt_.value(), 6);
                            size_t edge_idx;
                            if (mod_hall_cnt == 0) { goto skip; } // no count - do nothing
                            else if (mod_hall_cnt == 1) { // counted up
                                edge_idx = hall_cnt;
                            } else if (mod_hall_cnt == 5) { // counted down
                                edge_idx = last_hall_cnt_.value();
                            } else {
                                set_error(ERROR_ILLEGAL_HALL_STATE);
                                return false;
                            }

                            auto maybe_phase = axis_->open_loop_controller_.phase_.any();
                            if (maybe_phase) {
                                float phase = maybe_phase.value();
                                // Early increment to get the right divisor in recursive average
                                hall_phase_calib_seen_count_[edge_idx]++;
                                float& edge_phase = config_.hall_edge_phcnt[edge_idx];
                                if (hall_phase_calib_seen_count_[edge_idx] == 1)
                                    edge_phase = phase;
                                else {
                                    // circularly wrapped recursive average
                                    edge_phase += (phase - edge_phase) / hall_phase_calib_seen_count_[edge_idx];
                                    edge_phase = wrap_pm_pi(edge_phase);
                                }
                            }
                        }
                    skip:
                        last_hall_cnt_ = hall_cnt;

                        return true; // Skip all velocity and phase estimation
                    }

                    delta_enc = hall_cnt - count_in_cpr_;
                    delta_enc = mod(delta_enc, 6);
                    if (delta_enc > 3)
                        delta_enc -= 6;
                } else {
                    if (!config_.ignore_illegal_hall_state) {
                        set_error(ERROR_ILLEGAL_HALL_STATE);
                        return false;
                    }
                }
            }
        } break;

        case MODE_SINCOS: {
            float phase = fast_atan2(sincos_sample_s_, sincos_sample_c_);
            int fake_count = (int)(1000.0f * phase);
            //CPR = 6283 = 2pi * 1k

            delta_enc = fake_count - count_in_cpr_;
            delta_enc = mod(delta_enc, 6283);
            if (delta_enc > 6283/2)
                delta_enc -= 6283;
        } break;
        
        case MODE_SPI_ABS_RLS:
        case MODE_SPI_ABS_AMS:
        case MODE_SPI_ABS_CUI: 
        case MODE_SPI_ABS_AEAT:
        case MODE_SPI_ABS_MA732: {
            if (abs_spi_pos_updated_ == false) {
                // Low pass filter the error
                spi_error_rate_ += current_meas_period * (1.0f - spi_error_rate_);
                if (spi_error_rate_ > 0.05f) {
                    set_error(ERROR_ABS_SPI_COM_FAIL);
                    return false;
                }
            } else {
                // Low pass filter the error
                spi_error_rate_ += current_meas_period * (0.0f - spi_error_rate_);
            }

            abs_spi_pos_updated_ = false;
            delta_enc = pos_abs_latched - count_in_cpr_; //LATCH
            delta_enc = mod(delta_enc, config_.cpr);
            if (delta_enc > config_.cpr/2) {
                delta_enc -= config_.cpr;
            }

        }break;
        
        case MODE_UART_ABS_TAMAGAWA: {
            if (tamagawa_manual_transaction_active_) {
                delta_enc = 0;
                break;
            }
            // Check for timeout first
            if (tamagawa_check_timeout()) {
                spi_error_rate_ += current_meas_period * (1.0f - spi_error_rate_);
                if (spi_error_rate_ > 0.05f) {
                    set_error(ERROR_ABS_SPI_COM_FAIL);
                    return false;
                }
            }
            // Check if DMA transfer completed
            else if (!tamagawa_rx_complete_) {
                if (tamagawa_dma_active_) {
                    // Fallback: on some paths RxCplt callback may be missed even though
                    // DMA already transferred all requested bytes.
                    if (config_.tamagawa_uart != nullptr
                            && config_.tamagawa_uart->hdmarx != nullptr
                            && config_.tamagawa_uart->hdmarx->Instance != nullptr
                            && config_.tamagawa_uart->RxXferSize > 0) {
                        const uint16_t remaining = config_.tamagawa_uart->hdmarx->Instance->NDTR;
                        const uint16_t received = config_.tamagawa_uart->RxXferSize - std::min<uint16_t>(remaining, config_.tamagawa_uart->RxXferSize);
                        const uint16_t required = (uint16_t)tamagawa_get_response_frame_size(tamagawa_last_data_id_);
                        if (received >= required) {
                            HAL_UART_AbortReceive(config_.tamagawa_uart);
                            tamagawa_rx_complete_ = true;
                            tamagawa_dma_active_ = false;
                            tamagawa_waiting_for_rx_ = false;
                            tamagawa_set_debug(0x73, (uint8_t)axis_->axis_num_, HAL_OK, HAL_OK);
                        }
                    }

                    if (!tamagawa_rx_complete_) {
                        // A transaction is in-flight but has not timed out yet.
                        // Do not count normal UART/DMA latency as a communication error.
                    }
                }

                // No new complete frame this control cycle.
                delta_enc = 0;
            }

            if (tamagawa_rx_complete_) {
                // DMA completed successfully
                tamagawa_dma_active_ = false;
                tamagawa_waiting_for_rx_ = false;

                // Decode position from received data
                if (!tamagawa_decode_position(tamagawa_rx_buffer_)) {
                    // Decode failed
                    tamagawa_error_count_++;
                    spi_error_rate_ += current_meas_period * (1.0f - spi_error_rate_);
                    if (spi_error_rate_ > 0.05f) {
                        set_error(ERROR_ABS_SPI_COM_FAIL);
                        return false;
                    }
                } else {
                    // Successful frame decode means the UART/Tamagawa link is
                    // alive, so clear stale communication faults that may have
                    // latched during earlier bring-up attempts.
                    spi_error_rate_ += current_meas_period * (0.0f - spi_error_rate_);
                    error_ = (Error)(error_ & ~(ERROR_ABS_SPI_COM_FAIL | ERROR_ABS_SPI_TIMEOUT));
                    if (error_ == ERROR_NONE) {
                        axis_->error_ = (Axis::Error)(axis_->error_ & ~Axis::ERROR_ENCODER_FAILED);
                    }
                }
            }
            
            // Calculate position delta (only if we have new data)
            if (tamagawa_rx_complete_) {
                tamagawa_rx_complete_ = false;
                pos_abs_latched = pos_abs_; // use the freshly decoded Tamagawa position
                delta_enc = pos_abs_latched - count_in_cpr_; //LATCH
                delta_enc = mod(delta_enc, config_.cpr);
                if (delta_enc > config_.cpr/2) {
                    delta_enc -= config_.cpr;
                }
            }
        } break;
        
        default: {
            set_error(ERROR_UNSUPPORTED_ENCODER_MODE);
            return false;
        } break;
    }

    shadow_count_ += delta_enc;
    count_in_cpr_ += delta_enc;
    count_in_cpr_ = mod(count_in_cpr_, config_.cpr);

    if(mode_ & MODE_FLAG_ABS)
        count_in_cpr_ = pos_abs_latched;

    // Memory for pos_circular
    float pos_cpr_counts_last = pos_cpr_counts_;

    //// run pll (for now pll is in units of encoder counts)
    // Predict current pos
    pos_estimate_counts_ += current_meas_period * vel_estimate_counts_;
    pos_cpr_counts_      += current_meas_period * vel_estimate_counts_;
    // Encoder model
    auto encoder_model = [this](float internal_pos)->int32_t {
        if (config_.mode == MODE_HALL)
            return hall_model(internal_pos);
        else
            return (int32_t)std::floor(internal_pos);
    };
    // discrete phase detector
    float delta_pos_counts = (float)(shadow_count_ - encoder_model(pos_estimate_counts_));
    float delta_pos_cpr_counts = (float)(count_in_cpr_ - encoder_model(pos_cpr_counts_));
    delta_pos_cpr_counts = wrap_pm(delta_pos_cpr_counts, (float)(config_.cpr));
    delta_pos_cpr_counts_ += 0.1f * (delta_pos_cpr_counts - delta_pos_cpr_counts_); // for debug
    // pll feedback
    pos_estimate_counts_ += current_meas_period * pll_kp_ * delta_pos_counts;
    pos_cpr_counts_ += current_meas_period * pll_kp_ * delta_pos_cpr_counts;
    pos_cpr_counts_ = fmodf_pos(pos_cpr_counts_, (float)(config_.cpr));
    vel_estimate_counts_ += current_meas_period * pll_ki_ * delta_pos_cpr_counts;
    bool snap_to_zero_vel = false;
    if (std::abs(vel_estimate_counts_) < 0.5f * current_meas_period * pll_ki_) {
        vel_estimate_counts_ = 0.0f;  //align delta-sigma on zero to prevent jitter
        snap_to_zero_vel = true;
    }

    // Outputs from Encoder for Controller
    pos_estimate_ = pos_estimate_counts_ / (float)config_.cpr;
    vel_estimate_ = vel_estimate_counts_ / (float)config_.cpr;
    
    // TODO: we should strictly require that this value is from the previous iteration
    // to avoid spinout scenarios. However that requires a proper way to reset
    // the encoder from error states.
    float pos_circular = pos_circular_.any().value_or(0.0f);
    pos_circular +=  wrap_pm((pos_cpr_counts_ - pos_cpr_counts_last) / (float)config_.cpr, 1.0f);
    pos_circular = fmodf_pos(pos_circular, axis_->controller_.config_.circular_setpoint_range);
    pos_circular_ = pos_circular;

    //// run encoder count interpolation
    float phase_pos_counts = 0.0f;
    if (mode_ == MODE_UART_ABS_TAMAGAWA) {
        // Tamagawa absolute samples are sparse compared to the control loop.
        // Use the PLL-predicted CPR position for a continuous FOC phase between ABS frames.
        phase_pos_counts = pos_cpr_counts_ - (float)config_.phase_offset;
    } else {
        int32_t corrected_enc = count_in_cpr_ - config_.phase_offset;
        // if we are stopped, make sure we don't randomly drift
        if (snap_to_zero_vel || !config_.enable_phase_interpolation) {
            interpolation_ = 0.5f;
        // reset interpolation if encoder edge comes
        // TODO: This isn't correct. At high velocities the first phase in this count may very well not be at the edge.
        } else if (delta_enc > 0) {
            interpolation_ = 0.0f;
        } else if (delta_enc < 0) {
            interpolation_ = 1.0f;
        } else {
            // Interpolate (predict) between encoder counts using vel_estimate,
            interpolation_ += current_meas_period * vel_estimate_counts_;
            // don't allow interpolation indicated position outside of [enc, enc+1)
            if (interpolation_ > 1.0f) interpolation_ = 1.0f;
            if (interpolation_ < 0.0f) interpolation_ = 0.0f;
        }
        phase_pos_counts = (float)corrected_enc + interpolation_;
    }

    //// compute electrical phase
    //TODO avoid recomputing elec_rad_per_enc every time
    float elec_rad_per_enc = axis_->motor_.config_.pole_pairs * 2 * M_PI * (1.0f / (float)(config_.cpr));
    float ph = elec_rad_per_enc * (phase_pos_counts - config_.phase_offset_float);
    
    if (is_ready_) {
        phase_ = wrap_pm_pi(ph) * config_.direction;
        phase_vel_ = (2*M_PI) * *vel_estimate_.present() * axis_->motor_.config_.pole_pairs * config_.direction;
    }

    return true;
}

// Tamagawa encoder protocol implementation

bool Encoder::tamagawa_init() {
    if (config_.tamagawa_uart == nullptr) {
        return false;
    }

    if (!tamagawa_data_id_supported_for_polling(config_.tamagawa_data_id)) {
        return false;
    }
    
    // DE/RE control is handled by hardware auto-direction circuit
    // No GPIO initialization needed

    // Tamagawa polling uses bounded response sizes, so RX DMA should complete
    // once per transaction instead of wrapping forever like the UART server.
    if (config_.tamagawa_uart->hdmarx != nullptr
            && config_.tamagawa_uart->hdmarx->Init.Mode != DMA_NORMAL) {
        config_.tamagawa_uart->hdmarx->Init.Mode = DMA_NORMAL;
        if (HAL_DMA_Init(config_.tamagawa_uart->hdmarx) != HAL_OK) {
            return false;
        }
        __HAL_LINKDMA(config_.tamagawa_uart, hdmarx, *config_.tamagawa_uart->hdmarx);
    }
    
    // Clear buffers
    memset(tamagawa_rx_buffer_, 0, sizeof(tamagawa_rx_buffer_));
    memset(tamagawa_tx_buffer_, 0, sizeof(tamagawa_tx_buffer_));
    
    tamagawa_rx_complete_ = false;
    tamagawa_tx_complete_ = false;
    tamagawa_dma_active_ = false;
    tamagawa_waiting_for_rx_ = false;
    tamagawa_last_communication_ = 0;
    tamagawa_error_count_ = 0;
    tamagawa_last_data_id_ = config_.tamagawa_data_id;
    tamagawa_status_ = 0;
    tamagawa_almc_ = 0;
    tamagawa_enid_ = 0;
    tamagawa_eeprom_page_ = 0;
    tamagawa_multi_turn_ = 0;
    
    return true;
}

uint8_t Encoder::tamagawa_calc_crc(uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x01; // Polynomial X^8 + 1
            } else {
                crc = crc << 1;
            }
        }
    }
    return crc;
}

uint8_t Encoder::tamagawa_build_cf(uint8_t data_id) {
    const uint8_t id_parity =
            ((data_id >> 0) ^ (data_id >> 1) ^ (data_id >> 2) ^ (data_id >> 3)) & 0x1;
    return TAMAGAWA_SINK_CODE | ((data_id & 0x0F) << 3) | (id_parity << 7);
}

size_t Encoder::tamagawa_get_response_frame_size(uint8_t data_id) {
    switch (data_id) {
        case TAMAGAWA_DATA_ID_ABS:
        case TAMAGAWA_DATA_ID_MULTI_TURN:
        case TAMAGAWA_DATA_ID_RESET_ERRORS:
        case TAMAGAWA_DATA_ID_RESET_SINGLE_TURN:
        case TAMAGAWA_DATA_ID_RESET_MULTI_TURN_ERRORS:
            return TAMAGAWA_RESP_FRAME_SIZE_ABS;

        case TAMAGAWA_DATA_ID_FULL:
            return TAMAGAWA_RESP_FRAME_SIZE_FULL;

        case TAMAGAWA_DATA_ID_EEPROM_READ:
        case TAMAGAWA_DATA_ID_EEPROM_WRITE:
        case TAMAGAWA_DATA_ID_ENCODER_ID:
            return 4;

        default:
            return TAMAGAWA_RESP_FRAME_SIZE_FULL;
    }
}

bool Encoder::tamagawa_data_id_supported_for_polling(uint8_t data_id) {
    return data_id == TAMAGAWA_DATA_ID_ABS || data_id == TAMAGAWA_DATA_ID_FULL;
}

void Encoder::tamagawa_flush_rx() {
    if (config_.tamagawa_uart == nullptr) {
        return;
    }

    while (true) {
        const uint32_t sr = config_.tamagawa_uart->Instance->SR;
        if ((sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) == 0) {
            break;
        }

        volatile uint32_t discard = config_.tamagawa_uart->Instance->DR;
        (void)discard;
    }
}

size_t Encoder::tamagawa_receive_bytes_blocking(uint8_t* buffer, size_t max_len, uint32_t timeout_us) {
    if (config_.tamagawa_uart == nullptr || buffer == nullptr || max_len == 0) {
        return 0;
    }

    size_t count = 0;
    const uint32_t start_us = micros();
    uint32_t last_rx_us = start_us;

    while ((uint32_t)(micros() - start_us) < timeout_us) {
        const uint32_t sr = config_.tamagawa_uart->Instance->SR;

        if (sr & USART_SR_RXNE) {
            const uint8_t value = (uint8_t)(config_.tamagawa_uart->Instance->DR & 0xFF);
            if (count < max_len) {
                buffer[count++] = value;
            }
            last_rx_us = micros();
            continue;
        }

        if (sr & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
            volatile uint32_t discard = config_.tamagawa_uart->Instance->DR;
            (void)discard;
            last_rx_us = micros();
            continue;
        }

        if (count > 0 && (uint32_t)(micros() - last_rx_us) > 100) {
            break;
        }
    }

    return count;
}

const uint8_t* Encoder::tamagawa_find_response_frame(const uint8_t* data, size_t capture_len, uint8_t data_id, size_t response_len) {
    if (data == nullptr || capture_len < response_len) {
        return nullptr;
    }

    const uint8_t expected_cf = tamagawa_build_cf(data_id);
    for (size_t offset = 0; offset + response_len <= capture_len; ++offset) {
        const uint8_t* candidate = data + offset;
        if (candidate[0] != expected_cf) {
            continue;
        }

        const uint8_t received_crc = candidate[response_len - 1];
        const uint8_t calculated_crc = tamagawa_calc_crc((uint8_t*)candidate, response_len - 1);
        if (received_crc == calculated_crc) {
            return candidate;
        }
    }

    return nullptr;
}

bool Encoder::tamagawa_send_blocking_request(uint8_t* request, size_t request_len, uint8_t expected_data_id, uint8_t* response, size_t response_len) {
    if (config_.tamagawa_uart == nullptr || request == nullptr || response == nullptr) {
        return false;
    }

    uint8_t capture[TAMAGAWA_MAX_TX_FRAME_SIZE + TAMAGAWA_MAX_RESP_FRAME_SIZE] = {};
    const size_t capture_max_len = std::min(sizeof(capture), request_len + response_len);
    const uint32_t timeout_us = std::max<uint32_t>(tamagawa_timeout_ms_ * 1000, 1000);

    tamagawa_flush_rx();
    memset(response, 0, response_len);

    if (HAL_UART_Transmit(config_.tamagawa_uart, request, request_len, 10) != HAL_OK) {
        return false;
    }

    const size_t capture_len = tamagawa_receive_bytes_blocking(capture, capture_max_len, timeout_us);
    const uint8_t* frame = tamagawa_find_response_frame(capture, capture_len, expected_data_id, response_len);
    if (frame == nullptr) {
        return false;
    }

    memcpy(response, frame, response_len);
    tamagawa_last_communication_ = HAL_GetTick();
    return true;
}

// Blocking send command (fallback, not recommended for 8kHz)
bool Encoder::tamagawa_send_command(uint8_t data_id) {
    if (config_.tamagawa_uart == nullptr) {
        return false;
    }

    if (!tamagawa_data_id_supported_for_polling(data_id)) {
        tamagawa_error_count_++;
        return false;
    }
    
    // Regular position polling requests consist of a single Control Field.
    tamagawa_tx_buffer_[0] = tamagawa_build_cf(data_id);
    
    const size_t response_len = tamagawa_get_response_frame_size(data_id);

    // Start receiving response (interrupt mode) BEFORE transmit
    tamagawa_rx_complete_ = false;
    HAL_StatusTypeDef status = HAL_UART_Receive_IT(config_.tamagawa_uart, tamagawa_rx_buffer_, response_len);
    if (status != HAL_OK) {
        return false;
    }

    // Transmit command (blocking)
    status = HAL_UART_Transmit(config_.tamagawa_uart, tamagawa_tx_buffer_, 1, 10);
    if (status != HAL_OK) {
        HAL_UART_AbortReceive(config_.tamagawa_uart);
        return false;
    }
    
    tamagawa_last_data_id_ = data_id;
    tamagawa_last_communication_ = HAL_GetTick();
    return true;
}

// Non-blocking DMA send command (recommended for 8kHz)
bool Encoder::tamagawa_send_command_dma(uint8_t data_id) {
    if (config_.tamagawa_uart == nullptr) {
        return false;
    }

    if (!tamagawa_data_id_supported_for_polling(data_id)) {
        tamagawa_error_count_++;
        tamagawa_set_debug(0x10, data_id, HAL_ERROR, HAL_ERROR);
        return false;
    }
    
    // Check if previous DMA transfer is still active
    if (tamagawa_dma_active_) {
        // Previous transfer not complete, skip this cycle
        return false;
    }

    // Release any stale HAL RX state left behind by a fallback-completed frame.
    if (config_.tamagawa_uart->RxState != HAL_UART_STATE_READY) {
        HAL_UART_AbortReceive(config_.tamagawa_uart);
        __HAL_UART_CLEAR_OREFLAG(config_.tamagawa_uart);
    }
    
    // Regular position polling requests consist of a single Control Field.
    tamagawa_tx_buffer_[0] = tamagawa_build_cf(data_id);
    
    // Reset flags
    memset(tamagawa_rx_buffer_, 0, sizeof(tamagawa_rx_buffer_));
    tamagawa_rx_complete_ = false;
    tamagawa_tx_complete_ = false;
    tamagawa_dma_active_ = true;
    tamagawa_waiting_for_rx_ = true;
    
    const size_t response_len = tamagawa_get_response_frame_size(data_id);
    const size_t rx_len = std::min(response_len, sizeof(tamagawa_rx_buffer_));

    // Arm RX DMA before TX to avoid missing fast responses at 2.5Mbps.
    HAL_StatusTypeDef rx_status = HAL_UART_Receive_DMA(
        config_.tamagawa_uart,
        tamagawa_rx_buffer_,
        rx_len
    );
    if (rx_status != HAL_OK) {
        tamagawa_dma_active_ = false;
        tamagawa_waiting_for_rx_ = false;
        tamagawa_error_count_++;
        tamagawa_set_debug(0x12, data_id, HAL_OK, rx_status);
        return false;
    }

    // Start DMA transmit
    HAL_StatusTypeDef status = HAL_UART_Transmit_DMA(
        config_.tamagawa_uart,
        tamagawa_tx_buffer_,
        1
    );
    
    if (status != HAL_OK) {
        HAL_UART_AbortReceive(config_.tamagawa_uart);
        tamagawa_dma_active_ = false;
        tamagawa_waiting_for_rx_ = false;
        tamagawa_error_count_++;
        tamagawa_set_debug(0x13, data_id, status, HAL_ERROR);
        return false;
    }
    
    tamagawa_last_data_id_ = data_id;
    tamagawa_last_communication_ = HAL_GetTick();
    return true;
}

bool Encoder::tamagawa_can_run_manual_command() {
    if (mode_ != MODE_UART_ABS_TAMAGAWA || config_.tamagawa_uart == nullptr || axis_ == nullptr) {
        tamagawa_set_debug(0x40, mode_, HAL_ERROR, HAL_ERROR);
        return false;
    }

    // Keep reset commands out of the active control loop path. This is stricter
    // than the protocol requirement but avoids colliding with the 8kHz poller.
    if (axis_->motor_.is_armed_) {
        tamagawa_set_debug(0x41, 0, HAL_BUSY, HAL_BUSY);
        return false;
    }

    tamagawa_set_debug(0x42, 0, HAL_OK, HAL_OK);
    return true;
}

bool Encoder::tamagawa_send_eeprom_read_transaction(uint8_t address, uint8_t* response) {
    if (config_.tamagawa_uart == nullptr || response == nullptr) {
        return false;
    }

    uint8_t request[3];
    request[0] = tamagawa_build_cf(TAMAGAWA_DATA_ID_EEPROM_READ);
    request[1] = address;
    request[2] = tamagawa_calc_crc(request, 2);

    if (!tamagawa_send_blocking_request(request, sizeof(request), TAMAGAWA_DATA_ID_EEPROM_READ, response, 4)) {
        tamagawa_set_debug(0x21, address, HAL_OK, HAL_TIMEOUT);
        return false;
    }

    tamagawa_set_debug(0x22, address, HAL_OK, HAL_OK);
    return true;
}

bool Encoder::tamagawa_send_eeprom_write_transaction(uint8_t address, uint8_t value, uint8_t* response) {
    if (config_.tamagawa_uart == nullptr || response == nullptr) {
        return false;
    }

    uint8_t request[4];
    request[0] = tamagawa_build_cf(TAMAGAWA_DATA_ID_EEPROM_WRITE);
    request[1] = address;
    request[2] = value;
    request[3] = tamagawa_calc_crc(request, 3);

    if (!tamagawa_send_blocking_request(request, sizeof(request), TAMAGAWA_DATA_ID_EEPROM_WRITE, response, 4)) {
        tamagawa_set_debug(0x31, address, HAL_OK, HAL_TIMEOUT);
        return false;
    }

    tamagawa_set_debug(0x32, address, HAL_OK, HAL_OK);
    return true;
}

bool Encoder::tamagawa_parse_eeprom_response(uint8_t expected_data_id, uint8_t expected_address, uint8_t* response, uint8_t* value, bool* busy) {
    if (response == nullptr) {
        return false;
    }

    if (response[0] != tamagawa_build_cf(expected_data_id)) {
        return false;
    }

    if (tamagawa_calc_crc(response, 3) != response[3]) {
        return false;
    }

    const uint8_t response_address = response[1] & 0x7F;
    const bool response_busy = (response[1] & 0x80) != 0;
    if (response_address != expected_address) {
        return false;
    }

    if (value != nullptr) {
        *value = response[2];
    }
    if (busy != nullptr) {
        *busy = response_busy;
    }
    return true;
}

bool Encoder::tamagawa_wait_for_eeprom_write_cycle() {
    HAL_Delay(TAMAGAWA_EEPROM_WRITE_DELAY_MS);
    return true;
}

bool Encoder::tamagawa_select_eeprom_page(uint8_t page) {
    if (tamagawa_eeprom_page_ == page) {
        return true;
    }

    uint8_t response[4];
    bool busy = false;
    if (!tamagawa_send_eeprom_write_transaction(TAMAGAWA_EEPROM_PAGE_SELECT_ADDRESS, page, response)
            || !tamagawa_parse_eeprom_response(TAMAGAWA_DATA_ID_EEPROM_WRITE,
                    TAMAGAWA_EEPROM_PAGE_SELECT_ADDRESS, response, nullptr, &busy)) {
        return false;
    }

    if (!tamagawa_wait_for_eeprom_write_cycle()) {
        return false;
    }

    tamagawa_eeprom_page_ = page;
    (void)busy;
    return true;
}

bool Encoder::tamagawa_validate_response(uint8_t* data, uint8_t data_id, bool allow_encoder_error_bits) {
    if (data == nullptr) {
        return false;
    }

    if (data[0] != tamagawa_build_cf(data_id)) {
        return false;
    }

    const size_t response_len = tamagawa_get_response_frame_size(data_id);
    const uint8_t received_crc = data[response_len - 1];
    const uint8_t calculated_crc = tamagawa_calc_crc(data, response_len - 1);
    if (received_crc != calculated_crc) {
        return false;
    }

    const uint8_t status = data[1];
    if (status & 0x0C) { // ca0 / ca1
        return false;
    }

    if (!allow_encoder_error_bits && (status & 0x03)) { // ea0 / ea1
        return false;
    }

    return true;
}

bool Encoder::tamagawa_send_blocking_transaction(uint8_t data_id, uint8_t* response) {
    if (config_.tamagawa_uart == nullptr || response == nullptr) {
        return false;
    }

    const size_t response_len = tamagawa_get_response_frame_size(data_id);
    tamagawa_tx_buffer_[0] = tamagawa_build_cf(data_id);
    if (!tamagawa_send_blocking_request(tamagawa_tx_buffer_, 1, data_id, response, response_len)) {
        return false;
    }

    tamagawa_last_data_id_ = data_id;
    return true;
}

bool Encoder::tamagawa_execute_reset_sequence(uint8_t data_id) {
    if (data_id != TAMAGAWA_DATA_ID_RESET_ERRORS
            && data_id != TAMAGAWA_DATA_ID_RESET_SINGLE_TURN
            && data_id != TAMAGAWA_DATA_ID_RESET_MULTI_TURN_ERRORS) {
        return false;
    }

    if (!tamagawa_can_run_manual_command()) {
        return false;
    }

    uint32_t prim = cpu_enter_critical();
    tamagawa_manual_transaction_active_ = true;
    cpu_exit_critical(prim);

    HAL_UART_AbortTransmit(config_.tamagawa_uart);
    HAL_UART_AbortReceive(config_.tamagawa_uart);
    tamagawa_dma_active_ = false;
    tamagawa_waiting_for_rx_ = false;
    tamagawa_rx_complete_ = false;
    tamagawa_tx_complete_ = false;
    tamagawa_clear_debug_hold();

    bool success = true;

    for (uint8_t i = 0; i < TAMAGAWA_RESET_REPEAT_COUNT; ++i) {
        tamagawa_flush_rx();
        tamagawa_tx_buffer_[0] = tamagawa_get_reset_command_byte(data_id);
        if (HAL_UART_Transmit(config_.tamagawa_uart, tamagawa_tx_buffer_, 1, 10) != HAL_OK) {
            tamagawa_hold_debug(0x81, data_id, (HAL_StatusTypeDef)i, HAL_ERROR);
            success = false;
            break;
        }

        if (i + 1 < TAMAGAWA_RESET_REPEAT_COUNT) {
            delay_us(TAMAGAWA_RESET_MIN_SPACING_US);
        }
    }

    if (success) {
        tamagawa_flush_rx();
        HAL_Delay(20);
        if (data_id == TAMAGAWA_DATA_ID_RESET_SINGLE_TURN) {
            uint8_t abs_response[TAMAGAWA_RESP_FRAME_SIZE_ABS] = {};
            bool reset_observed = false;
            for (uint32_t elapsed_ms = 0; elapsed_ms < 1000; elapsed_ms += 20) {
                if (tamagawa_send_blocking_transaction(TAMAGAWA_DATA_ID_ABS, abs_response)
                        && tamagawa_decode_position(abs_response)
                        && pos_abs_ == 0) {
                    reset_observed = true;
                    break;
                }
                HAL_Delay(20);
            }

            if (!reset_observed) {
                tamagawa_hold_debug(0x82, data_id, (HAL_StatusTypeDef)(pos_abs_ & 0xFF), (HAL_StatusTypeDef)abs_response[1]);
                success = false;
            }
        } else if (data_id == TAMAGAWA_DATA_ID_RESET_MULTI_TURN_ERRORS) {
            uint8_t multi_turn_response[TAMAGAWA_RESP_FRAME_SIZE_ABS] = {};
            if (!tamagawa_send_blocking_transaction(TAMAGAWA_DATA_ID_MULTI_TURN, multi_turn_response)
                    || !tamagawa_validate_response(multi_turn_response, TAMAGAWA_DATA_ID_MULTI_TURN, true)) {
                tamagawa_hold_debug(0x83, data_id, HAL_ERROR, (HAL_StatusTypeDef)multi_turn_response[1]);
                success = false;
            } else {
                const uint32_t multi_turn =
                        (uint32_t)multi_turn_response[2]
                        | ((uint32_t)multi_turn_response[3] << 8)
                        | (((uint32_t)multi_turn_response[4] & 0x01) << 16);
                if (multi_turn_response[4] & 0xFE || multi_turn != 0) {
                    tamagawa_hold_debug(0x84, data_id, (HAL_StatusTypeDef)(multi_turn & 0xFF), (HAL_StatusTypeDef)multi_turn_response[4]);
                    success = false;
                }
            }
        } else {
            uint8_t abs_response[TAMAGAWA_RESP_FRAME_SIZE_ABS] = {};
            if (!tamagawa_send_blocking_transaction(TAMAGAWA_DATA_ID_ABS, abs_response)
                    || !tamagawa_decode_position(abs_response)) {
                tamagawa_hold_debug(0x85, data_id, HAL_ERROR, (HAL_StatusTypeDef)abs_response[1]);
                success = false;
            }
        }
    }

    prim = cpu_enter_critical();
    tamagawa_manual_transaction_active_ = false;
    cpu_exit_critical(prim);

    if (!success) {
        tamagawa_error_count_++;
    } else {
        tamagawa_clear_debug_hold();
    }

    return success;
}

bool Encoder::tamagawa_check_timeout() {
    if (!tamagawa_dma_active_) {
        return false; // No timeout if not active
    }
    
    uint32_t current_time = HAL_GetTick();
    uint32_t elapsed = current_time - tamagawa_last_communication_;
    
    if (elapsed >= tamagawa_timeout_ms_) {
        // Timeout occurred, abort DMA transfers
        HAL_UART_AbortTransmit(config_.tamagawa_uart);
        HAL_UART_AbortReceive(config_.tamagawa_uart);
        tamagawa_dma_active_ = false;
        tamagawa_waiting_for_rx_ = false;
        tamagawa_error_count_++;
        return true;
    }
    
    return false;
}

bool Encoder::tamagawa_decode_position(uint8_t* data) {
    if (data == nullptr) {
        return false;
    }

    const size_t response_len = tamagawa_get_response_frame_size(tamagawa_last_data_id_);
    const size_t capture_len = std::min(response_len + 1, sizeof(tamagawa_rx_buffer_));
    const uint8_t expected_cf = tamagawa_build_cf(tamagawa_last_data_id_);

    const uint8_t* frame = nullptr;
    for (size_t offset = 0; offset + response_len <= capture_len; ++offset) {
        const uint8_t* candidate = data + offset;
        if (candidate[0] != expected_cf) {
            continue;
        }

        const uint8_t received_crc = candidate[response_len - 1];
        const uint8_t calculated_crc = tamagawa_calc_crc((uint8_t*)candidate, response_len - 1);
        if (received_crc == calculated_crc) {
            frame = candidate;
            break;
        }
    }

    if (frame == nullptr) {
        return false;
    }
    
    // Store status field
    tamagawa_status_ = frame[1]; // SF
    
    // Communication alarms indicate that the encoder fell back to an error reply;
    // the host must discard this frame and retry.
    if (tamagawa_check_error(tamagawa_status_)) {
        return false;
    }

    const uint32_t single_turn =
            (uint32_t)frame[2]
            | ((uint32_t)frame[3] << 8)
            | (((uint32_t)frame[4] & 0x01) << 16);

    if (frame[4] & 0xFE) {
        return false;
    }

    uint8_t enid = 0;
    uint32_t multi_turn = 0;
    uint8_t almc = 0;

    switch (tamagawa_last_data_id_) {
        case TAMAGAWA_DATA_ID_ABS:
            break;

        case TAMAGAWA_DATA_ID_FULL:
            enid = frame[5];
            multi_turn =
                    (uint32_t)frame[6]
                    | ((uint32_t)frame[7] << 8)
                    | (((uint32_t)frame[8] & 0x01) << 16);
            almc = frame[9];

            if (frame[8] & 0xFE) {
                return false;
            }

            if (enid != TAMAGAWA_TS5700N8501_ENID) {
                return false;
            }
            break;

        default:
            return false;
    }

    tamagawa_enid_ = enid;
    tamagawa_multi_turn_ = multi_turn;
    tamagawa_almc_ = almc;

    // ODrive control currently consumes the single-turn absolute position; the
    // multi-turn counter and alarm byte are retained for diagnostics.
    pos_abs_ = (int32_t)single_turn;
    
    // For multi-turn support, we could update shadow_count_ as:
    // shadow_count_ = multi_turn * 131072 + single_turn;
    // But this would require 33 bits, so we'll just use single-turn for now
    // and let the PLL handle multi-turn tracking
    
    // Mark that we have new data
    abs_spi_pos_updated_ = true;
    
    // If pre-calibrated, mark as ready
    if (config_.pre_calibrated) {
        is_ready_ = true;
    }
    
    return true;
}

bool Encoder::tamagawa_check_error(uint8_t sts) {
    // Check status field for errors
    // ea0 = 1: counting error
    // ea1 = 1: over-heat, multi-turn error, battery error, or battery alarm
    // ca0 = 1: parity error in request frame
    // ca1 = 1: stop bit error in request frame
    
    if (sts & 0x03) { // ea0 or ea1
        return true;
    }
    
    if (sts & 0x0C) { // ca0 or ca1
        return true;
    }
    
    return false;
}

extern "C" void tamagawa_uart_tx_complete_callback(UART_HandleTypeDef *huart) {
    // Update all encoders that use this UART handle.
    for (int i = 0; i < AXIS_COUNT; i++) {
        if (encoders[i].mode_ == Encoder::MODE_UART_ABS_TAMAGAWA
                && encoders[i].config_.tamagawa_uart == huart) {
            encoders[i].tamagawa_tx_complete_ = true;
            encoders[i].tamagawa_waiting_for_rx_ = false; // RX was already armed before TX
            tamagawa_set_debug(0x70, (uint8_t)i, HAL_OK, HAL_OK);
        }
    }
}

extern "C" void tamagawa_uart_rx_complete_callback(UART_HandleTypeDef *huart) {
    // Update all encoders that use this UART handle.
    for (int i = 0; i < AXIS_COUNT; i++) {
        if (encoders[i].mode_ == Encoder::MODE_UART_ABS_TAMAGAWA
                && encoders[i].config_.tamagawa_uart == huart) {
            encoders[i].tamagawa_rx_complete_ = true;
            encoders[i].tamagawa_dma_active_ = false;
            encoders[i].tamagawa_waiting_for_rx_ = false;
            tamagawa_set_debug(0x71, (uint8_t)i, HAL_OK, HAL_OK);
        }
    }
}

extern "C" void tamagawa_uart_error_callback(UART_HandleTypeDef *huart) {
    bool has_matching_encoder = false;
    for (int i = 0; i < AXIS_COUNT; i++) {
        if (encoders[i].mode_ == Encoder::MODE_UART_ABS_TAMAGAWA
                && encoders[i].config_.tamagawa_uart == huart) {
            has_matching_encoder = true;
            encoders[i].tamagawa_dma_active_ = false;
            encoders[i].tamagawa_waiting_for_rx_ = false;
            encoders[i].tamagawa_error_count_++;
        }
    }

    if (has_matching_encoder) {
        HAL_UART_AbortTransmit(huart);
        HAL_UART_AbortReceive(huart);
        // info: low byte of HAL UART ErrorCode (PE/NE/FE/ORE/DMA)
        tamagawa_set_debug(0x72, (uint8_t)(huart->ErrorCode & 0xFF), HAL_ERROR, HAL_ERROR);
    }
}
