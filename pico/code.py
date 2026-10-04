# VBS Android Optics: the Vector's two rangefinder buttons on a Raspberry Pi Pico (W), as a USB keyboard.
# Plugged into the phone (USB OTG), it types F9 while Bearing is held and F10 while Range is held; the app
# treats those exactly like its on-screen Bearing and Range buttons (both together to lase).
#
# Wiring: each button between its pin and any GND pin (the Pico's own pull-ups, no resistors):
#   Bearing  GP3 (pin 5)   GND pin 3 (or 8)
#   Range    GP5 (pin 7)   GND pin 8 (or 3)
# The LED lights while a button is held.
import time

import board
import digitalio
import usb_hid
from adafruit_hid.keyboard import Keyboard
from adafruit_hid.keycode import Keycode

BUTTONS = [
    (board.GP3, Keycode.F9),   # Bearing
    (board.GP5, Keycode.F10),  # Range
]
STABLE_SAMPLES = 3  # a change must hold this many 5 ms samples (switch bounce)

keyboard = Keyboard(usb_hid.devices)
led = digitalio.DigitalInOut(board.LED)
led.switch_to_output(False)


class Button:
    def __init__(self, pin, key):
        self.io = digitalio.DigitalInOut(pin)
        self.io.switch_to_input(pull=digitalio.Pull.UP)
        self.key = key
        self.pressed = False
        self.count = 0

    def poll(self):
        down = not self.io.value  # pulled up: pressed reads low
        if down == self.pressed:
            self.count = 0
            return
        self.count += 1
        if self.count < STABLE_SAMPLES:
            return
        self.count = 0
        self.pressed = down
        if down:
            keyboard.press(self.key)
        else:
            keyboard.release(self.key)


buttons = [Button(pin, key) for pin, key in BUTTONS]
while True:
    for b in buttons:
        b.poll()
    led.value = any(b.pressed for b in buttons)
    time.sleep(0.005)
