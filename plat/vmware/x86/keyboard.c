#include <uk/config.h>
#include <x86/cpu.h>
#include <vmware-x86/keyboard.h>

#define DATA_PORT 0x60
#define STATUS_PORT 0x64
#define OUTPUT_BUFFER_FULL 0x01

static uint8_t shift_pressed = 0;
static uint8_t caps_lock = 0;

static void handle_special_keys(uint8_t scancode, uint8_t key_down) {
    if (scancode == 0x2A || scancode == 0x36) { // Left or Right Shift
        shift_pressed = key_down;
    } else if (scancode == 0x3A && key_down) { // Caps Lock
        caps_lock = !caps_lock;
    }
}

static char translate_scancode_to_char(uint8_t scancode) {
    uint8_t uppercase = shift_pressed ^ caps_lock; // XOR to determine if we need uppercase

    switch (scancode) {
        // Number keys and special characters
        case 0x02: return shift_pressed ? '!' : '1';
        case 0x03: return shift_pressed ? '@' : '2';
        case 0x04: return shift_pressed ? '#' : '3';
        case 0x05: return shift_pressed ? '$' : '4';
        case 0x06: return shift_pressed ? '%' : '5';
        case 0x07: return shift_pressed ? '^' : '6';
        case 0x08: return shift_pressed ? '&' : '7';
        case 0x09: return shift_pressed ? '*' : '8';
        case 0x0A: return shift_pressed ? '(' : '9';
        case 0x0B: return shift_pressed ? ')' : '0';

        // Letter keys
        case 0x1E: return uppercase ? 'A' : 'a';
        case 0x30: return uppercase ? 'B' : 'b';
        case 0x2E: return uppercase ? 'C' : 'c';
        case 0x20: return uppercase ? 'D' : 'd';
        case 0x12: return uppercase ? 'E' : 'e';
        case 0x21: return uppercase ? 'F' : 'f';
        case 0x22: return uppercase ? 'G' : 'g';
        case 0x23: return uppercase ? 'H' : 'h';
        case 0x17: return uppercase ? 'I' : 'i';
        case 0x24: return uppercase ? 'J' : 'j';
        case 0x25: return uppercase ? 'K' : 'k';
        case 0x26: return uppercase ? 'L' : 'l';
        case 0x32: return uppercase ? 'M' : 'm';
        case 0x31: return uppercase ? 'N' : 'n';
        case 0x18: return uppercase ? 'O' : 'o';
        case 0x19: return uppercase ? 'P' : 'p';
        case 0x10: return uppercase ? 'Q' : 'q';
        case 0x13: return uppercase ? 'R' : 'r';
        case 0x1F: return uppercase ? 'S' : 's';
        case 0x14: return uppercase ? 'T' : 't';
        case 0x16: return uppercase ? 'U' : 'u';
        case 0x2F: return uppercase ? 'V' : 'v';
        case 0x11: return uppercase ? 'W' : 'w';
        case 0x2D: return uppercase ? 'X' : 'x';
        case 0x15: return uppercase ? 'Y' : 'y';
        case 0x2C: return uppercase ? 'Z' : 'z';

        // Special characters
        case 0x1A: return shift_pressed ? '{' : '[';
        case 0x1B: return shift_pressed ? '}' : ']';
        case 0x2B: return shift_pressed ? '|' : '\\';
        case 0x27: return shift_pressed ? ':' : ';';
        case 0x28: return shift_pressed ? '"' : '\'';
        case 0x29: return shift_pressed ? '~' : '`';
        case 0x33: return shift_pressed ? '<' : ',';
        case 0x34: return shift_pressed ? '>' : '.';
        case 0x35: return shift_pressed ? '?' : '/';

        // Space and other keys
        case 0x39: return ' ';
        case 0x0E: return '\b';   // Backspace
        case 0x1C: return '\n';   // Enter

        // Function keys and others
        case 0x01: return '\e';   // Escape

        default: return 0; // Return null character for unknown scancodes
    }
}


// Function to read the status register from port 0x64
static uint8_t read_status_register() {
    return inb(STATUS_PORT);
}

// Function to read data from port 0x60
static uint8_t read_data_port() {
    return inb(DATA_PORT);
}

// Function to check if data is available in the output buffer
static int is_data_available() {
    return (read_status_register() & OUTPUT_BUFFER_FULL) != 0;
}

// Main function to handle keyboard input
int _libvmwareplat_keyboard_getc(void) {
    while (!is_data_available()) { }

	uint8_t scancode = read_data_port();

	// Check if it's a key press (not a release)
    uint8_t key_down = (scancode & 0x80) == 0;

    if (!key_down) {
        scancode &= 0x7F; // Mask out the key release bit
    }

    handle_special_keys(scancode, key_down);

	char chr = translate_scancode_to_char(scancode);

	uk_pr_err("keyboard: %s scancode %d char %c\n", key_down ? "DOWN" : "UP", scancode, chr);

	return chr;
}