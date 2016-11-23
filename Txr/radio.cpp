#include "settings.h"
#include "serial_api.h"
#include "radio.h"
#include "Arduino.h"
#include <SPI.h>
#include <Mirf.h>
#include <nRF24L01.h>
#include <MirfHardwareSpiDriver.h>
#include "bsp.h"
#include "version.h"
#include "eeprom_assert.h"

#define HEARTBEAT_INTERVAL_MILLIS 2000

radio_state_t radio_state = {0};

bool _is_radio_available()
{
    return !Mirf.isSending();
}

bool _get_radio_packet(void *buffer)
{
    if (!Mirf.isSending() && Mirf.dataReady()) {
        uint8_t *buf = (uint8_t *)buffer;
        Mirf.getData(buf);
        return true;
    } else {
        return false;
    }
}

void _send_radio_packet(void *buffer)
{
    Mirf.setTADDR((uint8_t *)TRANSMIT_ADDRESS);
    Mirf.send((uint8_t *)buffer);
    while (Mirf.isSending()) {
    }
}

void _queue_print_ok(char type)
{
    char buffer[16];
    sprintf(buffer, "OK %c", type);
    serial_api_queue_output(buffer);
}

void _queue_print_i16(char type, int val)
{
    char buffer[16];
    sprintf(buffer, "%c=%d", type, val);
    serial_api_queue_output(buffer);
}

void _queue_print_i32(char type, long val)
{
    char buffer[16];
    sprintf(buffer, "%c=%ld", type, val);
    serial_api_queue_output(buffer);
}

void _queue_print_u16(char type, int val)
{
    char buffer[16];
    sprintf(buffer, "%c=%u", type, val);
    serial_api_queue_output(buffer);
}

void _queue_print_u32(char type, int val)
{
    char buffer[16];
    sprintf(buffer, "%c=%lu", type, val);
    serial_api_queue_output(buffer);
}

void _queue_print_string(char type, char* val)
{
    char buffer[PACKET_STRING_LEN + 4];
    sprintf(buffer, "%c=%.*s", type, PACKET_STRING_LEN, val);
    serial_api_queue_output(buffer);
}

void _send_ok(char key)
{
    radio_packet_t packet = {0};
    packet.ok.type = PACKET_OK;
    packet.ok.key = key;
    // radio_queue_message(packet);
}

char* _incremental_read_packet_string(char type, char* start)
{
    if (radio_state.string_packet_command != type) {
        radio_state.string_packet_buffer_index = 0;
    }
    radio_state.string_packet_command = type;

    int index = 0;
    char* read_ptr = start;
    char* write_ptr = radio_state.string_packet_buffer +
        radio_state.string_packet_buffer_index;
    while (read_ptr - start <= PACKET_STRING_LEN) {
        *write_ptr++ = *read_ptr++;
        if (!*write_ptr) {
            return radio_state.string_packet_buffer;
        }
    }
    return 0;
}

char _map_ok_type(char key)
{
    switch (key){

    case PACKET_MAX_SPEED_SET: return SERIAL_MAX_SPEED_SET;
    case PACKET_ACCEL_SET: return SERIAL_ACCEL_SET;
    case PACKET_CHANNEL_SET: return SERIAL_REMOTE_CHANNEL_SET;
    case PACKET_PROFILE_ID_SET: return SERIAL_ID_SET;
    case PACKET_PROFILE_NAME_SET: return SERIAL_NAME_SET;
    case PACKET_TARGET_POSITION_SET: return SERIAL_TARGET_POSITION_SET;
    case PACKET_SAVE_CONFIG: return SERIAL_SAVE_CONFIG;
    case PACKET_RELOAD_CONFIG: return SERIAL_RELOAD_CONFIG;

    default: return SERIAL_IGNORE;
    }

    return 0;
}

#define PRINT_PACKET_STRING(serial_cmd, name) do {\
    char __buffer[PACKET_STRING_LEN + 4];\
    sprintf(__buffer, "%c=%s",\
            serial_cmd,\
            packet.name.val);\
    serial_api_queue_output(__buffer);\
} while(0)

void _process_packet(radio_packet_t packet)
{
    char type = packet.type; 

    switch (type) {
    case PACKET_VERSION_GET: {
        PACKET_SEND_STRING(PACKET_VERSION_PRINT, version_print, VERSION);
    } break;
    case PACKET_VERSION_PRINT: {
        PRINT_PACKET_STRING(SERIAL_REMOTE_VERSION, version_print);
        _send_ok(type);
    } break;
    case PACKET_ROLE_GET: {
        PACKET_SEND(PACKET_ROLE_PRINT, role_print, ROLE);
    } break;
    case PACKET_ROLE_PRINT: {
        _queue_print_u16(SERIAL_REMOTE_ROLE, packet.role_print.val);
        _send_ok(type);
    } break;
    case PACKET_MAX_SPEED_GET_NO_PRINT: {
        unsigned int max_speed = settings_get_max_speed();
        PACKET_SEND(PACKET_MAX_SPEED_SET, max_speed_set, max_speed);
    } break;
    case PACKET_MAX_SPEED_GET: {
        unsigned int max_speed = settings_get_max_speed();
        PACKET_SEND(PACKET_MAX_SPEED_PRINT, max_speed_print, max_speed);
    } break;
    case PACKET_MAX_SPEED_SET: {
        settings_set_max_speed(packet.max_speed_set.val);
        _send_ok(type);
    } break;
    case PACKET_MAX_SPEED_PRINT: {
        unsigned int max_speed = packet.max_speed_print.val;
        _queue_print_u16(SERIAL_MAX_SPEED_GET, max_speed);
        _send_ok(type);
    } break;
    case PACKET_ACCEL_GET_NO_PRINT: {
        int accel = settings_get_max_accel();
        PACKET_SEND(PACKET_ACCEL_SET, accel_set, accel);
    } break;
    case PACKET_ACCEL_GET: {
        int accel = settings_get_max_accel();
        PACKET_SEND(PACKET_ACCEL_PRINT, accel_print, accel);
    } break;
    case PACKET_ACCEL_SET: {
        settings_set_max_accel(packet.accel_set.val);
        _send_ok(type);
    } break;
    case PACKET_ACCEL_PRINT: {
        int accel = packet.accel_print.val;
        _queue_print_i16(SERIAL_ACCEL_GET, accel);
        _send_ok(type);
    } break;
    case PACKET_CHANNEL_GET: {
        int channel = settings_get_channel();
        PACKET_SEND(PACKET_CHANNEL_PRINT, channel_print, channel);
    } break;
    case PACKET_CHANNEL_SET: {
        settings_set_channel(packet.channel_set.val);
        radio_set_channel(packet.channel_set.val);
        _send_ok(type);
    } break;
    case PACKET_CHANNEL_PRINT: {
        int channel = packet.channel_print.val;
        _queue_print_i16(SERIAL_REMOTE_CHANNEL_GET, channel);
        _send_ok(type);
    } break;
    case PACKET_PROFILE_ID_GET: {
        long id = settings_get_id();
        PACKET_SEND(PACKET_PROFILE_ID_PRINT, profile_id_print, id);
    } break;
    case PACKET_PROFILE_ID_SET: {
        settings_set_id(packet.profile_id_set.val);
        _send_ok(type);
    } break;
    case PACKET_PROFILE_ID_PRINT: {
        _queue_print_u32(SERIAL_ID_GET, packet.profile_id_print.val);
        _send_ok(type);
    } break;
    case PACKET_PROFILE_NAME_GET: {
        char buffer[NAME_MAX_LENGTH];
        settings_get_name(buffer);

        radio_packet_t send_packet = {0};
        send_packet.type = PACKET_PROFILE_NAME_PRINT;
        int index = 0;
        int remaining = strlen(buffer) + 1;

        while (remaining > 0) {
            char* start = buffer + index;
            int length = PACKET_STRING_LEN;
            strncpy(send_packet.profile_name_print.val, start, length);
            radio_queue_message(send_packet);
            index += length;
            remaining -= length;
        }
    } break;
    case PACKET_PROFILE_NAME_SET: {
        char* packet_val = packet.profile_name_set.val;
        char* name = _incremental_read_packet_string(type, packet_val);

        if (name) {
            settings_set_name(name);
            _send_ok(type);
        }
    } break;
    case PACKET_PROFILE_NAME_PRINT: {
        char* packet_val = packet.profile_name_print.val;
        char* name = _incremental_read_packet_string(type, packet_val);

        if (name) {
            _queue_print_string(SERIAL_NAME_GET, name);
            _send_ok(type);
        }
    } break;
    case PACKET_TARGET_POSITION_GET: {
    } break;
    case PACKET_TARGET_POSITION_SET: {
        _send_ok(type);
    } break;
    case PACKET_TARGET_POSITION_PRINT: {
        _queue_print_i32(SERIAL_TARGET_POSITION_GET,
            packet.target_position_print.val);
        _send_ok(type);
    } break;
    case PACKET_SPEED_PERCENT_SET: {
        _send_ok(type);
    } break;
    case PACKET_ACCEL_PERCENT_SET: {
        _send_ok(type);
    } break;
    case PACKET_SAVE_CONFIG: {
        settings_flush_debounced_values();
        _send_ok(type);
    } break;
    case PACKET_RELOAD_CONFIG: {
        PACKET_SEND(PACKET_MAX_SPEED_SET, max_speed_set, settings_get_max_speed());
        PACKET_SEND(PACKET_ACCEL_SET, accel_set, settings_get_max_accel());
        _send_ok(type);
    } break;
    case PACKET_PRESET_INDEX_GET: {
        PACKET_SEND(PACKET_PRESET_INDEX_PRINT,
            preset_index_print, settings_get_preset_index());
    } break;
    case PACKET_PRESET_INDEX_SET: {
        settings_set_preset_index(packet.preset_index_set.val);
        _send_ok(type);
    } break;
    case PACKET_PRESET_INDEX_PRINT: {
        _queue_print_i16(SERIAL_PRESET_INDEX_GET,
            packet.preset_index_print.val);
        _send_ok(type);
    } break;
    case PACKET_START_STATE_GET: {
        PACKET_SEND(PACKET_START_STATE_PRINT,
            start_state_print, settings_get_start_in_calibration_mode());
    } break;
    case PACKET_START_STATE_SET: {
        settings_set_start_in_calibration_mode(packet.start_state_set.val);
        _send_ok(type);
    } break;
    case PACKET_START_STATE_PRINT: {
        _queue_print_i16(SERIAL_START_STATE_GET,
            packet.start_state_print.val);
        _send_ok(type);
    } break;
    case PACKET_OK: {
        char ok_type = _map_ok_type(packet.ok.key);
        _queue_print_ok(ok_type);
    } break;
    }
}

void radio_queue_message(radio_packet_t packet)
{
    // packet.version = RADIO_VERSION;
    radio_state.buffer[radio_state.write_index++] = packet;
    radio_state.write_index %= RADIO_OUT_BUFFER_SIZE;

    // BSP_assert(radio_state.write_index != radio_state.read_index);
}

void radio_init()
{
    Mirf.spi = &MirfHardwareSpi;
    Mirf.init();
    Mirf.setRADDR((uint8_t *)RECEIVE_ADDRESS);
    Mirf.payload = sizeof(radio_packet_t);

    int channel = settings_get_channel();
    radio_set_channel(channel);
}

void radio_run()
{
    radio_packet_t packet = {0};


    if (_get_radio_packet(&packet)) {
        _process_packet(packet);
    }

    if (_is_radio_available() && radio_state.read_index != radio_state.write_index) {
        radio_packet_t *out_packet = &radio_state.buffer[radio_state.read_index++];
        radio_state.read_index %= RADIO_OUT_BUFFER_SIZE;
        _send_radio_packet(out_packet);
    }
}

void radio_set_channel(int channel)
{
    char reg[] = { RF_DEFAULT, 0 };

    if (channel >= 1 && channel <= 82) {
        Mirf.channel = channel;
    }

    Mirf.writeRegister(RF_SETUP, (uint8_t *)reg, 1);
    Mirf.config();
}

bool radio_is_alive()
{
    uint8_t addr[mirf_ADDR_LEN];

    Mirf.readRegister(TX_ADDR, addr, mirf_ADDR_LEN);
    return memcmp(addr, (uint8_t *)TRANSMIT_ADDRESS, mirf_ADDR_LEN) == 0;
}
