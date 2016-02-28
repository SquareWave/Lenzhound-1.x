#ifndef SERIAL_API_H
#define SERIAL_API_H

#include "motorcontroller.h"

enum {
    SERIAL_API_SRC_CONSOLE,
    SERIAL_API_SRC_RADIO,
    SERIAL_API_SRC_COUNT,
};

const int SERIAL_API_IN_BUFFER_SIZE = 128;
const int SERIAL_API_OUT_BUFFER_SIZE = 128;
const char SERIAL_API_END_OF_RESPONSE = '\n';
const char SERIAL_API_END_OF_COMMAND = '\n';
const char SERIAL_API_ESCAPE = '\\';

#define MAX_RESPONSE_LENGTH_EXCEEDED "ERR 01"
#define MAX_INPUT_LENGTH_EXCEEDED    "ERR 02"
#define UNKNOWN_COMMAND              "ERR 03"
#define MALFORMED_COMMAND            "ERR 04"

struct serial_api_state_t {
    lh::MotorController* motor_controller;
    char in_buffer[SERIAL_API_SRC_COUNT * SERIAL_API_IN_BUFFER_SIZE];
    char out_buffer[SERIAL_API_SRC_COUNT * SERIAL_API_OUT_BUFFER_SIZE];
    int indices[SERIAL_API_SRC_COUNT];
    int escaped[SERIAL_API_SRC_COUNT];
    int out_indices[SERIAL_API_SRC_COUNT];
};

enum {
    SERIAL_API_CMD_ECHO = 'e',
    SERIAL_API_CMD_VERSION = 'v',
    SERIAL_API_CMD_ROLE = 'r',
    SERIAL_API_CMD_SET_VALUE = 's',
    SERIAL_API_CMD_GET_VALUE = 'g',
};

struct serial_api_echo_command_t {
    char type;
    char *input;
    int length;
};

struct serial_api_command_t {
    union {
        char type;
        serial_api_echo_command_t echo;
    };
};

struct serial_api_response_t {
    char *buffer;
    int length;
};

serial_api_response_t serial_api_read_response(serial_api_state_t *state,
                                               int source);
void serial_api_queue_byte(serial_api_state_t *state, int source, char byte);
void serial_api_queue_output(serial_api_state_t *state, int source,
                             char *message, int length);

#endif
