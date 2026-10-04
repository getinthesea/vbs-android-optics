# Runs once when the Pico powers up. On the phone the Pico should be just a keyboard, so its CIRCUITPY drive
# and serial console are switched off, unless Range is held while plugging it in: do that (into the PC) to get
# the drive back and edit the files.
import board
import digitalio
import storage
import usb_cdc

range_button = digitalio.DigitalInOut(board.GP5)
range_button.switch_to_input(pull=digitalio.Pull.UP)
if range_button.value:  # not held
    storage.disable_usb_drive()
    usb_cdc.disable()
range_button.deinit()
