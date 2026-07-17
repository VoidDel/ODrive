#ifndef __ENCODER_HPP
#define __ENCODER_HPP

class Encoder;

#include <board.h> // needed for arm_math.h
#include <Drivers/STM32/stm32_spi_arbiter.hpp>
#include "utils.hpp"
#include <autogen/interfaces.hpp>
#include "component.hpp"
#include "usart.h" // For UART_HandleTypeDef


class Encoder : public ODriveIntf::EncoderIntf {
public:
    static constexpr uint32_t MODE_FLAG_ABS = 0x100;
    static constexpr uint32_t MODE_UART_ABS_TAMAGAWA = 0x105;
    static constexpr uint8_t TAMAGAWA_DATA_ID_ABS = 0x0;
    static constexpr uint8_t TAMAGAWA_DATA_ID_MULTI_TURN = 0x1;
    static constexpr uint8_t TAMAGAWA_DATA_ID_ENCODER_ID = 0x2;
    static constexpr uint8_t TAMAGAWA_DATA_ID_FULL = 0x3;
    static constexpr uint8_t TAMAGAWA_DATA_ID_EEPROM_WRITE = 0x6;
    static constexpr uint8_t TAMAGAWA_DATA_ID_RESET_ERRORS = 0x7;
    static constexpr uint8_t TAMAGAWA_DATA_ID_RESET_SINGLE_TURN = 0x8;
    static constexpr uint8_t TAMAGAWA_DATA_ID_RESET_MULTI_TURN_ERRORS = 0xC;
    static constexpr uint8_t TAMAGAWA_DATA_ID_EEPROM_READ = 0xD;
    static constexpr uint8_t TAMAGAWA_SINK_CODE = 0x2; // b010, transmitted LSB-first by UART
    static constexpr uint8_t TAMAGAWA_TS5700N8501_ENID = 0x17;
    static constexpr uint8_t TAMAGAWA_EEPROM_PAGE_SELECT_ADDRESS = 127;
    static constexpr uint8_t TAMAGAWA_TEMPERATURE_PAGE = 7;
    static constexpr uint8_t TAMAGAWA_TEMPERATURE_ADDRESS = 5;
    static constexpr size_t TAMAGAWA_MAX_TX_FRAME_SIZE = 4;
    static constexpr size_t TAMAGAWA_RESP_FRAME_SIZE_ABS = 6;
    static constexpr size_t TAMAGAWA_RESP_FRAME_SIZE_FULL = 11;
    static constexpr size_t TAMAGAWA_MAX_RESP_FRAME_SIZE = TAMAGAWA_RESP_FRAME_SIZE_FULL;
    static constexpr uint32_t TAMAGAWA_POLL_INTERVAL_US = 250; // 4kHz ABS-only poll for high-speed Tamagawa phase correction
    static constexpr int32_t TAMAGAWA_REBASE_INTERVAL_TURNS = 4096;
    static constexpr uint8_t TAMAGAWA_RESET_REPEAT_COUNT = 10;
    static constexpr uint32_t TAMAGAWA_RESET_MIN_SPACING_US = 40;
    static constexpr uint32_t TAMAGAWA_EEPROM_WRITE_DELAY_MS = 20;
    static constexpr std::array<float, 6> hall_edge_defaults = 
        {0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};

    struct Config_t {
        Mode mode = MODE_INCREMENTAL;
        float calib_range = 0.02f; // Accuracy required to pass encoder cpr check
        float calib_scan_distance = 16.0f * M_PI; // rad electrical
        float calib_scan_omega = 4.0f * M_PI; // rad/s electrical
        float bandwidth = 1000.0f;
        int32_t phase_offset = 0;        // Offset between encoder count and rotor electrical phase
        float phase_offset_float = 0.0f; // Sub-count phase alignment offset
        int32_t cpr = (2048 * 4);   // Default resolution of CUI-AMT102 encoder,
        float index_offset = 0.0f;
        bool use_index = false;
        bool pre_calibrated = false; // If true, this means the offset stored in
                                    // configuration is valid and does not need
                                    // be determined by run_offset_calibration.
                                    // In this case the encoder will enter ready
                                    // state as soon as the index is found.
        int32_t direction = 0; // direction with respect to motor
        bool use_index_offset = true;
        bool enable_phase_interpolation = true; // Use velocity to interpolate inside the count state
        bool find_idx_on_lockin_only = false; // Only be sensitive during lockin scan constant vel state
        bool ignore_illegal_hall_state = false; // dont error on bad states like 000 or 111
        uint8_t hall_polarity = 0;
        bool hall_polarity_calibrated = false;
        std::array<float, 6> hall_edge_phcnt = hall_edge_defaults;
        uint16_t abs_spi_cs_gpio_pin = 1;
        uint16_t sincos_gpio_pin_sin = 3;
        uint16_t sincos_gpio_pin_cos = 4;
        
        // Tamagawa encoder configuration
        UART_HandleTypeDef* tamagawa_uart = nullptr; // UART handle for Tamagawa communication
        Stm32Gpio tamagawa_de_re_gpio; // GPIO for RS485 DE/RE direction control
        uint8_t tamagawa_data_id = TAMAGAWA_DATA_ID_ABS; // Polling supports ABS-only (0x0) or full-data (0x3)


        // custom setters
        Encoder* parent = nullptr;
        void set_use_index(bool value) { use_index = value; parent->set_idx_subscribe(); }
        void set_find_idx_on_lockin_only(bool value) { find_idx_on_lockin_only = value; parent->set_idx_subscribe(); }
        void set_abs_spi_cs_gpio_pin(uint16_t value) { abs_spi_cs_gpio_pin = value; parent->abs_spi_cs_pin_init(); }
        void set_pre_calibrated(bool value) { pre_calibrated = value; parent->check_pre_calibrated(); }
        void set_bandwidth(float value) { bandwidth = value; parent->update_pll_gains(); }
    };

    Encoder(TIM_HandleTypeDef* timer, Stm32Gpio index_gpio,
            Stm32Gpio hallA_gpio, Stm32Gpio hallB_gpio, Stm32Gpio hallC_gpio,
            Stm32SpiArbiter* spi_arbiter);
    
    bool apply_config(ODriveIntf::MotorIntf::MotorType motor_type);
    void setup();
    void set_error(Error error);
    bool do_checks();

    void enc_index_cb();
    void set_idx_subscribe(bool override_enable = false);
    void update_pll_gains();
    void check_pre_calibrated();
    float get_pos_estimate_counts();
    int64_t get_tamagawa_linear_turn_offset();

    void set_linear_count(int32_t count);
    void set_circular_count(int32_t count, bool update_offset);
    bool calib_enc_offset(float voltage_magnitude);

    bool run_index_search();
    bool run_direction_find();
    bool run_hall_polarity_calibration();
    bool run_hall_phase_calibration();
    bool run_offset_calibration();
    bool tamagawa_clear_encoder_errors();
    bool tamagawa_reset_single_turn();
    bool tamagawa_reset_multi_turn_and_errors();
    std::tuple<bool, uint8_t, bool> tamagawa_read_eeprom(uint8_t page, uint8_t address);
    std::tuple<bool, bool> tamagawa_write_eeprom(uint8_t page, uint8_t address, uint8_t value);
    std::tuple<bool, float> tamagawa_read_temperature();
    void sample_now();
    bool read_sampled_gpio(Stm32Gpio gpio);
    void decode_hall_samples();
    int32_t hall_model(float internal_pos);
    bool update();

    TIM_HandleTypeDef* timer_;
    Stm32Gpio index_gpio_;
    Stm32Gpio hallA_gpio_;
    Stm32Gpio hallB_gpio_;
    Stm32Gpio hallC_gpio_;
    Stm32SpiArbiter* spi_arbiter_;
    Axis* axis_ = nullptr; // set by Axis constructor

    Config_t config_;

    Error error_ = ERROR_NONE;
    bool index_found_ = false;
    bool is_ready_ = false;
    int32_t shadow_count_ = 0;
    int32_t count_in_cpr_ = 0;
    float interpolation_ = 0.0f;
    OutputPort<float> phase_ = 0.0f;     // [rad]
    OutputPort<float> phase_vel_ = 0.0f; // [rad/s]
    float pos_estimate_counts_ = 0.0f;  // [count]
    float pos_cpr_counts_ = 0.0f;  // [count]
    float delta_pos_cpr_counts_ = 0.0f;  // [count] phase detector result for debug
    float vel_estimate_counts_ = 0.0f;  // [count/s]
    float pll_kp_ = 0.0f;   // [count/s / count]
    float pll_ki_ = 0.0f;   // [(count/s^2) / count]
    float calib_scan_response_ = 0.0f; // debug report from offset calib
    int32_t pos_abs_ = 0;
    float spi_error_rate_ = 0.0f;

    OutputPort<float> pos_estimate_ = 0.0f; // [turn]
    OutputPort<float> vel_estimate_ = 0.0f; // [turn/s]
    OutputPort<float> pos_circular_ = 0.0f; // [turn]

    bool pos_estimate_valid_ = false;
    bool vel_estimate_valid_ = false;

    int16_t tim_cnt_sample_ = 0; // 
    static const constexpr GPIO_TypeDef* ports_to_sample[] = { GPIOA, GPIOB, GPIOC };
    uint16_t port_samples_[sizeof(ports_to_sample) / sizeof(ports_to_sample[0])];
    // Updated by low_level pwm_adc_cb
    uint8_t hall_state_ = 0x0; // bit[0] = HallA, .., bit[2] = HallC
    std::optional<uint8_t> last_hall_cnt_ = std::nullopt; // Used to find hall edges for calibration
    bool calibrate_hall_phase_ = false;
    bool sample_hall_states_ = false;
    bool sample_hall_phase_ = false;
    std::array<int, 8> states_seen_count_; // for hall polarity calibration
    std::array<int, 6> hall_phase_calib_seen_count_;

    float sincos_sample_s_ = 0.0f;
    float sincos_sample_c_ = 0.0f;

    bool abs_spi_start_transaction();
    void abs_spi_cb(bool success);
    void abs_spi_cs_pin_init();
    bool abs_spi_pos_updated_ = false;
    Mode mode_ = MODE_INCREMENTAL;
    Stm32Gpio abs_spi_cs_gpio_;
    uint32_t abs_spi_cr1;
    uint32_t abs_spi_cr2;
    uint16_t abs_spi_dma_tx_[1] = {0xFFFF};
    uint16_t abs_spi_dma_rx_[1];
    Stm32SpiArbiter::SpiTask spi_task_;
    
    // Tamagawa encoder functions
    bool tamagawa_init();
    bool tamagawa_send_command(uint8_t data_id);
    bool tamagawa_send_command_dma(uint8_t data_id);
    bool tamagawa_decode_position(uint8_t* data);
    bool tamagawa_execute_reset_sequence(uint8_t data_id);
    bool tamagawa_send_blocking_transaction(uint8_t data_id, uint8_t* response);
    bool tamagawa_send_eeprom_read_transaction(uint8_t address, uint8_t* response);
    bool tamagawa_send_eeprom_write_transaction(uint8_t address, uint8_t value, uint8_t* response);
    bool tamagawa_parse_eeprom_response(uint8_t expected_data_id, uint8_t expected_address, uint8_t* response, uint8_t* value, bool* busy);
    bool tamagawa_select_eeprom_page(uint8_t page);
    bool tamagawa_wait_for_eeprom_write_cycle();
    void tamagawa_flush_rx();
    size_t tamagawa_receive_bytes_blocking(uint8_t* buffer, size_t max_len, uint32_t timeout_us);
    const uint8_t* tamagawa_find_response_frame(const uint8_t* data, size_t capture_len, uint8_t data_id, size_t response_len);
    bool tamagawa_send_blocking_request(uint8_t* request, size_t request_len, uint8_t expected_data_id, uint8_t* response, size_t response_len);
    bool tamagawa_validate_response(uint8_t* data, uint8_t data_id, bool allow_encoder_error_bits);
    bool tamagawa_can_run_manual_command();
    uint8_t tamagawa_calc_crc(uint8_t* data, size_t len);
    uint8_t tamagawa_build_cf(uint8_t data_id);
    size_t tamagawa_get_response_frame_size(uint8_t data_id);
    bool tamagawa_data_id_supported_for_polling(uint8_t data_id);
    bool tamagawa_check_error(uint8_t sts);
    bool tamagawa_check_timeout();
    bool tamagawa_rebase_linear_position(int32_t pending_delta);
    
    // Tamagawa encoder state
    uint8_t tamagawa_rx_buffer_[TAMAGAWA_MAX_RESP_FRAME_SIZE + 1]; // +1 margin
    uint8_t tamagawa_tx_buffer_[TAMAGAWA_MAX_TX_FRAME_SIZE];
    volatile bool tamagawa_rx_complete_ = false;
    volatile bool tamagawa_tx_complete_ = false;
    volatile bool tamagawa_dma_active_ = false; // DMA transfer in progress
    volatile bool tamagawa_waiting_for_rx_ = false; // TX done, waiting to arm RX DMA
    volatile bool tamagawa_manual_transaction_active_ = false;
    uint32_t tamagawa_last_poll_us_ = 0;
    uint32_t tamagawa_last_communication_ = 0;
    uint32_t tamagawa_timeout_ms_ = 2; // Timeout in ms (default 2ms, should be much faster)
    uint8_t tamagawa_last_data_id_ = TAMAGAWA_DATA_ID_FULL;
    uint8_t tamagawa_status_ = 0; // Status field from encoder
    uint8_t tamagawa_almc_ = 0;   // ALMC (alarm code) from encoder
    uint8_t tamagawa_enid_ = 0;
    uint8_t tamagawa_eeprom_page_ = 0;
    uint32_t tamagawa_multi_turn_ = 0;
    uint32_t tamagawa_error_count_ = 0; // Error counter for debugging
    int64_t tamagawa_linear_turn_offset_ = 0; // Whole turns removed from the local 32-bit/float PLL coordinates
    float tamagawa_linear_turn_offset_float_ = 0.0f; // Cached float form for the real-time control path
    uint32_t tamagawa_rebase_count_ = 0;

    constexpr float getCoggingRatio(){
        return 1.0f / 3600.0f;
    }

};

extern "C" void tamagawa_uart_tx_complete_callback(UART_HandleTypeDef *huart);
extern "C" void tamagawa_uart_rx_complete_callback(UART_HandleTypeDef *huart);
extern "C" void tamagawa_uart_error_callback(UART_HandleTypeDef *huart);

#endif // __ENCODER_HPP
