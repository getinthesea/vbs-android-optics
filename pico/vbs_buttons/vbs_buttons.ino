// VBS Android Optics: the Vector's two rangefinder buttons on a Raspberry Pi Pico W, as a keyboard over USB and
// Bluetooth at once. It types F9 while Bearing is held and F10 while Range is held; the app treats those exactly
// like its on-screen Bearing and Range buttons (both together to lase). Calibrate types F8, which the app treats
// as its Calibrate button.
//
//   USB:        plugged into the phone (OTG cable), which also powers it
//   Bluetooth:  on a battery (VSYS pin 39, GND pin 38; 1.8-5.5 V), paired once in the phone's Bluetooth
//               settings as "VBS Buttons"; it reconnects by itself after that
//
// Wiring: each button between its pin and any GND pin (the Pico's own pull-ups, no resistors):
//   Bearing    GP3 (pin 5)    GND pin 3 (or 8)
//   Range      GP5 (pin 7)    GND pin 8 (or 3)
//   Calibrate  GP7 (pin 10)   GND pin 8 (or 13)
//
// LED: on while a button is held. When neither USB nor Bluetooth is connected it blinks every 2 s.
// Hold Bearing while powering up to forget paired phones (the LED flashes fast), to pair another.
//
// Built with Earle Philhower's Arduino core (rp2040:rp2040, board rpipicow, IP/Bluetooth stack with Bluetooth):
// see build.ps1, which builds and flashes it.
#include <Keyboard.h>
#include <KeyboardBLE.h>
#include <PicoBluetoothBLEHID.h>
#include <BluetoothLock.h>
#include <ble/le_device_db.h>
#include <tusb.h>

const char* NAME = "VBS Buttons";
const uint32_t STABLE_MS = 15; // a change must hold this long (switch bounce)

struct Button {
    uint8_t pin;
    uint8_t key;
    bool pressed;
    bool reading;
    uint32_t changedAt;
};

Button buttons[] = {
    { 3, KEY_F9, false, false, 0 },  // Bearing, GP3
    { 5, KEY_F10, false, false, 0 }, // Range, GP5
    { 7, KEY_F8, false, false, 0 },  // Calibrate, GP7
};

bool bleWasConnected = false;

// Forgets every paired phone, so another can pair
void forgetPhones() {
    BluetoothLock lock;
    for (int i = 0; i < le_device_db_max_count(); i++) {
        le_device_db_remove(i);
    }
}

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    for (Button& b : buttons) {
        pinMode(b.pin, INPUT_PULLUP);
    }
    Keyboard.begin();
    KeyboardBLE.begin(NAME);

    if (digitalRead(buttons[0].pin) == LOW) { // Bearing held at power-up
        forgetPhones();
        for (int i = 0; i < 10; i++) {
            digitalWrite(LED_BUILTIN, i % 2 == 0);
            delay(100);
        }
        digitalWrite(LED_BUILTIN, LOW);
        while (digitalRead(buttons[0].pin) == LOW) {
            delay(10); // wait for it to be let go, so it is not sent as a press
        }
    }
}

void loop() {
    uint32_t now = millis();

    // A fresh Bluetooth connection starts from nothing held, then whatever is held now
    bool bleConnected = PicoBluetoothBLEHID.connected();
    if (bleConnected && !bleWasConnected) {
        KeyboardBLE.releaseAll();
        for (Button& b : buttons) {
            if (b.pressed) {
                KeyboardBLE.press(b.key);
            }
        }
    }
    bleWasConnected = bleConnected;

    bool held = false;
    for (Button& b : buttons) {
        bool down = digitalRead(b.pin) == LOW; // pulled up: pressed reads low
        if (down != b.reading) {
            b.reading = down;
            b.changedAt = now;
        }
        if (b.reading != b.pressed && now - b.changedAt >= STABLE_MS) {
            b.pressed = b.reading;
            if (b.pressed) {
                Keyboard.press(b.key);
                if (bleConnected) {
                    KeyboardBLE.press(b.key);
                }
            } else {
                Keyboard.release(b.key);
                if (bleConnected) {
                    KeyboardBLE.release(b.key);
                }
            }
        }
        held = held || b.pressed;
    }

    bool waiting = !bleConnected && !tud_mounted();
    digitalWrite(LED_BUILTIN, held || (waiting && now % 2000 < 100));
    delay(2);
}
