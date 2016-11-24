//****************************************************************************
// This program is open source software: you can redistribute it and/or
// modify it under the terms of the GNU General Public License as published
// by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// for more details.
//****************************************************************************

#include "qp_port.h"
#include "bsp.h"
#include "Txr.h"
#include "Arduino.h" // if I start having problems with RF24, consider removing this
#include "radio.h"
#include "serial_api.h"
#include "console.h"
#include "settings.h"

Q_DEFINE_THIS_FILE

// These are percentages
#define MAX_SPEED  100
#define MID_SPEED  50
#define MIN_SPEED  1

#define MAX_SPEED_ENCODER_FACTOR  64
#define ENCODER_COUNTS_PER_SPEED_PERCENT  4
#define SPEED_PERCENT_SLOP 2

// various timeouts in ticks
enum TxrTimeouts {
    // how often to send encoder position
    SEND_ENCODER_TOUT  = BSP_TICKS_PER_SEC / 100,
    // how quick to flash LED
    FLASH_RATE_TOUT = BSP_TICKS_PER_SEC / 16,
    // how long to flash LED for
    FLASH_DURATION_TOUT = BSP_TICKS_PER_SEC / 4,
    // how long to hold calibration button before reentering calibration
    ENTER_CALIBRATION_TOUT = BSP_TICKS_PER_SEC / 2,
    // how often to check that transmitter is still powered (in case of low battery)
    ALIVE_DURATION_TOUT = BSP_TICKS_PER_SEC * 5,

    SEND_SPEED_AND_ACCEL_TOUT = BSP_TICKS_PER_SEC / 4,

    FLUSH_SETTINGS_TOUT = BSP_TICKS_PER_SEC * 4,
};

// todo: move this somewhere else or do differently
long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

long clamp(long x, long min, long max)
{
	return (x < min) ? min : ((x > max) ? max : x);
}

long distance(long a, long b)
{
	return abs(a - b);
}


class Txr : public QP::QActive {
private:
    QTimeEvt alive_timeout_;
    QTimeEvt flash_timeout_;
    QTimeEvt send_timeout_;
    QTimeEvt speed_and_accel_timeout_;
    QTimeEvt flush_settings_timeout_;
    QTimeEvt calibration_timeout_;
    long playback_target_pos_;
    float cur_pos_;    // float to save partial moves needed by encoder resolution division
    long prev_pos_1_;   // used to prevent jitter
    long prev_pos_2_;   // used to prevent jitter
    long prev_encoder_count_;
    long calibration_pos_1_;
    long calibration_pos_2_;
    char enc_pushes_;
    char calibration_multiplier_;
    long saved_positions_[NUM_POSITION_BUTTONS];
    unsigned char prev_position_button_pressed_;
    char z_mode_saved_speed_;
    char z_mode_saved_acceleration_;
    long encoder_base_;
    int previous_speed_percent_;

public:
    Txr() :
        QActive((QStateHandler) & Txr::initial),
        flash_timeout_(FLASH_RATE_SIG), send_timeout_(SEND_TIMEOUT_SIG),
        flush_settings_timeout_(FLUSH_SETTINGS_TIMEOUT_SIG),
        speed_and_accel_timeout_(SPEED_AND_ACCEL_TIMEOUT_SIG),
        calibration_timeout_(CALIBRATION_SIG), alive_timeout_(ALIVE_SIG),
        previous_speed_percent_(-1), playback_target_pos_(0)
    {
    }

protected:
    static QP::QState initial(Txr *const me, QP::QEvt const *const e);
    static QP::QState on(Txr *const me, QP::QEvt const *const e);
    static QP::QState uncalibrated(Txr *const me, QP::QEvt const *const e);
    static QP::QState calibrated(Txr *const me, QP::QEvt const *const e);
    static QP::QState flashing(Txr *const me, QP::QEvt const *const e);
    static QP::QState free_run_mode(Txr *const me, QP::QEvt const *const e);
    static QP::QState play_back_mode(Txr *const me, QP::QEvt const *const e);
    static QP::QState z_mode(Txr *const me, QP::QEvt const *const e);

    void update_position();
    void update_position_freerun();
    void update_position_calibration();
    void update_position_z_mode();
    void update_position_playback();
    void update_max_speed_using_encoder();
    void update_calibration_multiplier(int setting);
    void log_value(char key, long value);
    void set_LED_status(int led, int status);
    void init_speed_percent(int start_percentage);
    int get_speed_percent_if_changed();
    void update_speed_LEDs(int speed_percent);
    void set_speed_LEDs_off();
};


static Txr l_Txr;                   // the single instance of Txr active object (local)
QActive *const AO_Txr = &l_Txr;     // the global opaque pointer

enum {
    LED_OFF,
    LED_ON,
    LED_TOGGLE
};


// NOTE(doug): this exists to make it easier in the future to shoot out events
// for these LEDs to the API, to make the UI prettier if we want to.
void Txr::set_LED_status(int led, int status)
{
    if (status == LED_ON) {
        switch (led) {
        case SPEED_LED1: {
            RED_LED_ON();
        } break;
        case SPEED_LED2: {
            AMBER_LED_ON();
        } break;
        case SPEED_LED4: {
            AMBER2_LED_ON();
        } break;
        case SPEED_LED5: {
            GREEN_LED_ON();
        } break;
        case GREEN_LED: {
            GREEN2_LED_ON();
        } break;
        case ENC_RED_LED: {
            ENC_RED_LED_ON();
        } break;
        case ENC_GREEN_LED: {
            ENC_GREEN_LED_ON();
        } break;
        }
    } else if (status == LED_OFF) {
        switch (led) {
        case SPEED_LED1: {
            RED_LED_OFF();
        } break;
        case SPEED_LED2: {
            AMBER_LED_OFF();
        } break;
        case SPEED_LED4: {
            AMBER2_LED_OFF();
        } break;
        case SPEED_LED5: {
            GREEN_LED_OFF();
        } break;
        case GREEN_LED: {
            GREEN2_LED_OFF();
        } break;
        case ENC_RED_LED: {
            ENC_RED_LED_OFF();
        } break;
        case ENC_GREEN_LED: {
            ENC_GREEN_LED_OFF();
        } break;
        }
    } else if (status == LED_TOGGLE) {
        switch (led) {
        case SPEED_LED1: {
            RED_LED_TOGGLE();
        } break;
        case SPEED_LED2: {
            AMBER_LED_TOGGLE();
        } break;
        case SPEED_LED4: {
            AMBER2_LED_TOGGLE();
        } break;
        case SPEED_LED5: {
            GREEN_LED_TOGGLE();
        } break;
        case GREEN_LED: {
            GREEN2_LED_TOGGLE();
        } break;
        case ENC_RED_LED: {
            ENC_RED_LED_TOGGLE();
        } break;
        case ENC_GREEN_LED: {
            ENC_GREEN_LED_TOGGLE();
        } break;
        }
    }
}

void Txr::init_speed_percent(int start_percentage)
{
	long encoder_pos = BSP_get_encoder();
	encoder_base_ = encoder_pos -
		(start_percentage * ENCODER_COUNTS_PER_SPEED_PERCENT);
}

int Txr::get_speed_percent_if_changed()
{
	long encoder_pos = BSP_get_encoder();
	long speed_percent = (encoder_pos - encoder_base_) /
		ENCODER_COUNTS_PER_SPEED_PERCENT;

	if (speed_percent < -SPEED_PERCENT_SLOP) {
		encoder_base_ = speed_percent * ENCODER_COUNTS_PER_SPEED_PERCENT;
	} else if (speed_percent > (100 + SPEED_PERCENT_SLOP)) {
		encoder_base_ = speed_percent - 100 - (2 * SPEED_PERCENT_SLOP);
	}

	int result = clamp(speed_percent, 1, 100);
    if (result != previous_speed_percent_) {
        previous_speed_percent_ = result;
        return result;
    } else {
        return -1;
    }
}

void Txr::update_speed_LEDs(int speed_percent)
{
    #define MAP_LED(x,n) x == n ? \
    	255 :\
    	map(clamp(distance(x, n), 0, 24), 0, 24, 50, 0)

    int led_1 = MAP_LED(speed_percent, 1);
    int led_2 = MAP_LED(speed_percent, 25);
    int led_3 = MAP_LED(speed_percent, 50);
    int led_4 = MAP_LED(speed_percent, 75);
    int led_5 = MAP_LED(speed_percent, 100);

	#undef MAP_LED

    analogWrite(SPEED_LED1, led_1);
    analogWrite(SPEED_LED2, led_2);
    analogWrite(SPEED_LED3_1, led_3);
    analogWrite(SPEED_LED3_2, led_3);
    analogWrite(SPEED_LED4, led_4);
    analogWrite(SPEED_LED5, led_5);
}

void Txr::set_speed_LEDs_off()
{
    RED_LED_OFF();
    BSP_turn_off_speed_LED(1);
    BSP_turn_off_speed_LED(2);
    GREEN_LED_OFF();
}

void Txr::log_value(char key, long value)
{
    char buffer[32];
    sprintf(buffer, "%c=%ld", key, value);

    serial_api_queue_output(buffer);
}

void Txr::update_position_calibration()
{
    long cur_encoder_count = BSP_get_encoder();

    if (cur_encoder_count != prev_encoder_count_) {
        log_value(SERIAL_ENCODER_GET, cur_encoder_count);
    }

    // since it's 4 counts per detent, let's make it a detent per motor count
    // this is a test, remove if not necessary
    float amount_to_move = cur_encoder_count - prev_encoder_count_;
    amount_to_move /= 4.0;
    amount_to_move *= calibration_multiplier_;
     //(cur_encoder_count - prev_encoder_count_) * calibration_multiplier_;
    cur_pos_ += amount_to_move;
    prev_encoder_count_ = cur_encoder_count;

    if (amount_to_move != 0) {
        PACKET_SEND(PACKET_TARGET_POSITION_SET, target_position_set, cur_pos_);
    }
}

void Txr::update_position_freerun()
{
    int speed_percent = get_speed_percent_if_changed();
    if (speed_percent != -1) {
        PACKET_SEND(PACKET_SPEED_PERCENT_SET, speed_percent_set, speed_percent);
        update_speed_LEDs(speed_percent);   
    }

    update_position();
}

void Txr::update_max_speed_using_encoder()
{
    long cur_encoder_count = BSP_get_encoder();

    if (cur_encoder_count != prev_encoder_count_) {
        log_value(SERIAL_ENCODER_GET, cur_encoder_count);
    }

    float amount_to_move = cur_encoder_count - prev_encoder_count_;
    amount_to_move /= 4.0;
    amount_to_move *= MAX_SPEED_ENCODER_FACTOR;

    long cur_max_speed = settings_get_max_speed();

    prev_encoder_count_ = cur_encoder_count;

    if (amount_to_move != 0) {
        long clamped = clamp(cur_max_speed + amount_to_move, 1, 32768);
        settings_set_max_speed((unsigned int)clamped);
        log_value(SERIAL_MAX_SPEED_GET, clamped);
    }
}

void Txr::update_position()
{
    long new_pos = BSP_get_pot();

    // only update the current position if it's not jittering between two values
    if (new_pos != prev_pos_1_ && new_pos != prev_pos_2_) {
        log_value(SERIAL_POT_GET, new_pos);

        prev_pos_1_ = prev_pos_2_;
        prev_pos_2_ = new_pos;
        cur_pos_ = map(new_pos, MIN_POT_VAL, MAX_POT_VAL,
                          calibration_pos_1_, calibration_pos_2_);

        PACKET_SEND(PACKET_TARGET_POSITION_SET, target_position_set, cur_pos_);
    }
}

void Txr::update_position_z_mode()
{
    update_position();
}

void Txr::update_position_playback()
{
    int speed_percent = get_speed_percent_if_changed();
    if (speed_percent != -1) {
        PACKET_SEND(PACKET_SPEED_PERCENT_SET, speed_percent_set, speed_percent);
        update_speed_LEDs(speed_percent);   
    }

    if (playback_target_pos_ != cur_pos_) {
        cur_pos_ = playback_target_pos_;
        PACKET_SEND(PACKET_TARGET_POSITION_SET, target_position_set, cur_pos_);
    }
}

void Txr::update_calibration_multiplier(int setting)
{
    switch (setting) {
        case PLAYBACK_MODE: {
            calibration_multiplier_ = 40;
        } break;
        case Z_MODE: {
            calibration_multiplier_ = 80;
        } break;
        case FREE_MODE:
        default: {
            calibration_multiplier_ = 8;
        } break;
    }
}

QP::QState Txr::initial(Txr *const me, QP::QEvt const *const e)
{
    me->calibration_multiplier_ = 1;
    me->cur_pos_ = 0;
    me->prev_encoder_count_ = 0;
    PACKET_SEND(PACKET_TARGET_POSITION_SET, target_position_set, 0);
    me->z_mode_saved_speed_ = 50;
    me->z_mode_saved_acceleration_ = 100;
    me->prev_pos_1_ = -1;
    me->prev_pos_2_ = -1;
    me->subscribe(ENC_DOWN_SIG);
    me->subscribe(ENC_UP_SIG);
    me->subscribe(PLAY_BACK_MODE_SIG);
    me->subscribe(FREE_RUN_MODE_SIG);
    me->subscribe(Z_MODE_SIG);
    me->subscribe(POSITION_BUTTON_SIG);
    me->subscribe(UPDATE_PARAMS_SIG);
    me->send_timeout_.postEvery(me, SEND_ENCODER_TOUT);
    me->flush_settings_timeout_.postEvery(me, FLUSH_SETTINGS_TOUT);
    me->alive_timeout_.postEvery(me, ALIVE_DURATION_TOUT);
    me->speed_and_accel_timeout_.postEvery(me, SEND_SPEED_AND_ACCEL_TOUT);
    me->calibration_pos_1_ = settings_get_calibration_position_1();
    me->calibration_pos_2_ = settings_get_calibration_position_2();

    if (!settings_get_start_in_calibration_mode()) {
        for (int i = 0; i < NUM_POSITION_BUTTONS; i++) {
            me->saved_positions_[i] = settings_get_saved_position(i);
        }

        if (FREESWITCH_ON()) {
            return Q_TRAN(&free_run_mode);
        } else if (ZSWITCH_ON()) {
            return Q_TRAN(&z_mode);
        } else {
            return Q_TRAN(&play_back_mode);
        }
    }
    return Q_TRAN(&uncalibrated);
}

QP::QState Txr::on(Txr *const me, QP::QEvt const *const e)
{
    QP::QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            status = Q_HANDLED();
        } break;
        case Q_EXIT_SIG: {
            status = Q_HANDLED();
        } break;
        case ALIVE_SIG: {
            if (radio_is_alive()) {
                me->set_LED_status(GREEN_LED, LED_OFF);
            } else {
                me->set_LED_status(GREEN_LED, LED_ON);
            }
            status = Q_HANDLED();
        } break;
        case SPEED_AND_ACCEL_TIMEOUT_SIG:{
            PACKET_SEND(PACKET_MAX_SPEED_SET, max_speed_set,
                settings_get_max_speed());
            PACKET_SEND(PACKET_ACCEL_SET, accel_set,
                settings_get_max_accel());
            status = Q_HANDLED();
        } break;
        case FLUSH_SETTINGS_TIMEOUT_SIG: {
            settings_flush_debounced_values();
            status = Q_HANDLED();
        } break;
        case UPDATE_PARAMS_SIG: {
            int channel = settings_get_channel();
            radio_set_channel(channel, false);
            status = Q_HANDLED();
        } break;
        default: {
            status = Q_SUPER(&QP::QHsm::top);
        } break;
    }
    return status;
}

QP::QState Txr::uncalibrated(Txr *const me, QP::QEvt const *const e)
{
    QP::QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->set_LED_status(ENC_RED_LED, LED_ON);
            me->set_LED_status(ENC_GREEN_LED, LED_OFF);
            me->prev_encoder_count_ = BSP_get_encoder();
            me->update_calibration_multiplier(BSP_get_mode());
            status = Q_HANDLED();
        } break;
        case Q_EXIT_SIG: {
            status = Q_HANDLED();
        } break;
        case SEND_TIMEOUT_SIG: {
            me->update_position_calibration();
            status = Q_HANDLED();
        } break;
        case ENC_DOWN_SIG: {
            // if this is first time button press, just save the position
            if ((me->enc_pushes_)++ == 0) {
                me->calibration_pos_1_ = me->cur_pos_;
                settings_set_calibration_position_1(me->calibration_pos_1_);
            }
            // if this is second time, determine whether swapping is necessary
            // to map higher calibrated position with higher motor position
            else {
                me->calibration_pos_2_ = me->cur_pos_;
                settings_set_calibration_position_2(me->calibration_pos_2_);
            }
            status = Q_TRAN(&flashing);
        } break;
        case PLAY_BACK_MODE_SIG: {
            me->update_calibration_multiplier(PLAYBACK_MODE);
            status = Q_HANDLED();
        } break;
        case Z_MODE_SIG: {
            me->update_calibration_multiplier(Z_MODE);
            status = Q_HANDLED();
        } break;
        case FREE_RUN_MODE_SIG: {
            me->update_calibration_multiplier(FREE_MODE);
            status = Q_HANDLED();
        } break;
        default: {
            status = Q_SUPER(&on);
        } break;
    }
    return status;
}

QP::QState Txr::calibrated(Txr *const me, QP::QEvt const *const e)
{
    QP::QState status;

    switch (e->sig) {
    case Q_ENTRY_SIG: {
        me->set_LED_status(ENC_RED_LED, LED_OFF);
        me->set_LED_status(ENC_GREEN_LED, LED_ON);
        status = Q_HANDLED();
    } break;
    case Q_EXIT_SIG: {
        status = Q_HANDLED();
    } break;
    case ENC_DOWN_SIG: {
        // reenter calibration if held down for 2 seconds
        me->calibration_timeout_.postIn(me, ENTER_CALIBRATION_TOUT);
        status = Q_HANDLED();
    } break;
    case ENC_UP_SIG: {
        // if they released the button before the time is up, stop waiting for
        // the timeout
        me->calibration_timeout_.disarm();
        status = Q_HANDLED();
    } break;
    case CALIBRATION_SIG: {
        // we've held for 2 seconds, go to calibration
        me->enc_pushes_ = 0;
        status = Q_TRAN(&flashing);
    } break;
    case PLAY_BACK_MODE_SIG: {
        status = Q_TRAN(&play_back_mode);
    } break;
    case Z_MODE_SIG: {
        status = Q_TRAN(&z_mode);
    } break;
    case FREE_RUN_MODE_SIG: {
        status = Q_TRAN(&free_run_mode);
    } break;
    default: {
        status = Q_SUPER(&on);
    } break;
    }
    return status;
}

QP::QState Txr::flashing(Txr *const me, QP::QEvt const *const e)
{
    static char led_count = 0;
    QP::QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->set_LED_status(ENC_RED_LED, LED_ON);
            // me->set_LED_status(ENC_GREEN_LED, LED_TOGGLE);
            me->flash_timeout_.postEvery(me, FLASH_RATE_TOUT);
            me->calibration_timeout_.postIn(me, FLASH_DURATION_TOUT);
            led_count = 0;
            /*if (FREESWITCH_ON()) {
             *  me->update_position_freerun();
             * }
             * else if (ZSWITCH_ON()) {
             *  me->update_position_z_mode();
             * }
             * else {
             *  me->update_position_playback();
             * }*/
            status = Q_HANDLED();
        } break;
        case Q_EXIT_SIG: {
            me->flash_timeout_.disarm();
            me->set_LED_status(ENC_RED_LED, LED_ON);
            // me->set_LED_status(ENC_GREEN_LED, LED_OFF);
            status = Q_HANDLED();
        } break;
        case CALIBRATION_SIG: {
            // if they've pressed button 2 times calibration should be complete
            if (me->enc_pushes_ >= 2) {
                if (FREESWITCH_ON()) {
                    status = Q_TRAN(&free_run_mode);
                } else if (ZSWITCH_ON()) {
                    status = Q_TRAN(&z_mode);
                } else {
                    status = Q_TRAN(&play_back_mode);
                }
            } else {
                status = Q_TRAN(&uncalibrated);
            }
        } break;
        case FLASH_RATE_SIG: {
        	static int red_on = 0;
            analogWrite(SPEED_LED3_1, red_on ? 0xff : 0x00);
            red_on = !red_on;
            status = Q_HANDLED();
        } break;
        case ENC_DOWN_SIG: {
            // here to swallow the encoder press while flashing; else an exception
            // occurs
            status = Q_HANDLED();
        } break;
        default: {
            status = Q_SUPER(&uncalibrated);
        } break;
    }
    return status;
}

QP::QState Txr::free_run_mode(Txr *const me, QP::QEvt const *const e)
{
    QP::QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->set_LED_status(ENC_GREEN_LED, LED_ON);
            me->set_LED_status(ENC_RED_LED, LED_OFF);

            me->init_speed_percent(50);

            status = Q_HANDLED();
        } break;
        case Q_EXIT_SIG: {
            status = Q_HANDLED();
        } break;
        case SEND_TIMEOUT_SIG: {
            me->update_position_freerun();
            status = Q_HANDLED();
        } break;
        case POSITION_BUTTON_SIG: {
            // only save position if finished flashing from previous save
            if (me->flash_timeout_.ctr() == 0) {
                me->prev_position_button_pressed_ =
                    ((PositionButtonEvt *)e)->ButtonNum;
                Q_REQUIRE(me->prev_position_button_pressed_ < NUM_POSITION_BUTTONS);
                me->saved_positions_[me->prev_position_button_pressed_] = me->cur_pos_;
                settings_set_saved_position(me->prev_position_button_pressed_,
                                            (int)me->cur_pos_);
                BSP_turn_on_speed_LED(me->prev_position_button_pressed_);
                me->flash_timeout_.postIn(me, FLASH_RATE_TOUT);
            }

            status = Q_HANDLED();
        } break;
        case FLASH_RATE_SIG: {
            // turn off flashed LED
            BSP_turn_off_speed_LED(me->prev_position_button_pressed_);
            status = Q_HANDLED();
        } break;
        default: {
            status = Q_SUPER(&calibrated);
        } break;
    }
    return status;
}

QP::QState Txr::play_back_mode(Txr *const me, QP::QEvt const *const e)
{
    QP::QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->playback_target_pos_ = me->cur_pos_;
            me->set_LED_status(ENC_GREEN_LED, LED_ON);
            me->set_LED_status(ENC_RED_LED, LED_OFF);
            PACKET_SEND(PACKET_TARGET_POSITION_SET, target_position_set, me->cur_pos_);
            me->init_speed_percent(50);
            status = Q_HANDLED();
        } break;
        case Q_EXIT_SIG: {
            me->set_speed_LEDs_off();
            status = Q_HANDLED();
        } break;
        case SEND_TIMEOUT_SIG: {
            me->update_position_playback();
            status = Q_HANDLED();
        } break;
        case POSITION_BUTTON_SIG: {
            int button_num = ((PositionButtonEvt *)e)->ButtonNum;
            Q_REQUIRE(button_num < NUM_POSITION_BUTTONS);
            me->playback_target_pos_ = me->saved_positions_[button_num];
            status = Q_HANDLED();
        } break;
        default: {
            status = Q_SUPER(&calibrated);
        } break;
    }
    return status;
}

QP::QState Txr::z_mode(Txr *const me, QP::QEvt const *const e)
{
    QP::QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->set_LED_status(ENC_GREEN_LED, LED_ON);
            me->set_LED_status(ENC_RED_LED, LED_ON);

            me->prev_encoder_count_ = BSP_get_encoder();

            status = Q_HANDLED();
        } break;
        case Q_EXIT_SIG: {
            me->set_speed_LEDs_off();
            status = Q_HANDLED();
        } break;
        case SEND_TIMEOUT_SIG: {
            me->update_max_speed_using_encoder();
            me->update_position_z_mode();
            status = Q_HANDLED();
        } break;
        case POSITION_BUTTON_SIG: {
            int button_index = ((PositionButtonEvt *)e)->ButtonNum;
            Q_REQUIRE(button_index < NUM_POSITION_BUTTONS);

            me->log_value(SERIAL_PRESET_INDEX_GET, button_index);
            settings_set_preset_index(button_index);
            radio_set_channel(settings_get_channel(), false);
            PACKET_SEND(PACKET_MAX_SPEED_SET, max_speed_set,
                settings_get_max_speed());
            PACKET_SEND(PACKET_ACCEL_SET, accel_set, settings_get_max_accel());


            status = Q_HANDLED();
        } break;
        default: {
            status = Q_SUPER(&calibrated);
        } break;
    }
    return status;
}
