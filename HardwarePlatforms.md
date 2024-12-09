# Tonex One Controller: An open-source controller and display interface for the IK Multimedia Tonex One guitar pedal
# Hardware Platforms and Wiring Details
Two hardware platforms are recommended. 

Other ESP32-S3 platforms could be supported but would require code changes. They would need to meet the minimum requirements:
- Minimum SPI Flash size: 8 MB with display, 4 MB without
- Minimum PSRAM size: 2 MB
- USB OTG port

## Hardware Platform 1: Waveshare 4.3" B LCD board (Display supported)
This hardware platform uses this Waveshare 4.3" B LCD board.
https://www.waveshare.com/product/esp32-s3-touch-lcd-4.3b.htm?sku=28141

**Important note:** Waveshare have two very similar boards:
- ESP32-S3-4.3: 2 USB-C ports. 5 volt power input. This board is **NOT RECOMMENDED** but can be made to work with a board modification (remove R19)
- ESP32-S3-4.3**B**: 1 USB-C port and a terminal block for 9v power input. This is the recommended board and works without modification

This module provides the microcontroller, power input suitable for 9v DC pedal board use, LCD screen, capacitive touch screen, and dual isolated inputs suitable for momentary foot switches.

### Connections
Note: the controller code relies on the Tonex One pedal being set to Stomp mode. Code is in place to do this automatically, but it seems it may have a bug. Manually enable Stomp mode on your pedal.
- Connect the USB-C port on the Waveshare board to the ToneX One USB-C port
- Optional: connect dual footswitches to the isolated inputs on the Waveshare board. GND to ground. DI0 for footswitch 1. DI1 for footwitch 2. Exact wiring depends on the footswitch but is usually a 6.5mm stereo jack
- Connect 9V DC power supply to the terminals on the Waveshare board. The terminals are screw terminals, so most likely a DC jack to wires will be needed.
- Switch on the power supply
- The Waveshare board USB port will power the Tonex One. Do not connect 9 volts to it!
- Optional: for the Bluetooth Client version of code, switch on a M-Vave Chocolate Midi pedal (https://www.cuvave.com/productinfo/724103.html). After a few seconds it should connect and the Bluetooth icon should change from gray to blue
- Optional: for the Bluetooth Server version of code, the controller will be available as a peripheral for you to connect to via a Bluetooth Midi device. The Bluetooth icon should change from gray to blue when connected.

![wiring_waveshare_display](https://github.com/user-attachments/assets/11cbf6da-e9bc-43a8-8228-f1e5ceb4b65a)





# Hardware Platform 2: Waveshare ESP32-S3 Zero (no Display support)
This hardware platform uses this Waveshare Zero board.
[https://www.waveshare.com/product/esp32-s3-zero.htm](https://www.waveshare.com/product/esp32-s3-zero.htm)

**Ensure its the ESP32-S3FH4R2 with USB-C port. There are some similar boards with no USB-C or slightly different processor**
This module is very low cost (around US$6) and does not support an LCD display. It requires a 5 volt DC power supply.
Caution: do not connect a pedalboard 9v! If you do, you will probably blow up both the PCB and your Tonex One!

### Connections
Note: the controller code relies on the Tonex One pedal being set to Stomp mode. Code is in place to do this automatically, but it seems it may have a bug. Manually enable Stomp mode on your pedal.
- Solder a DC jack to the PCB, as shown below. Note the positive and negative polarity must match your power supply
- Connect the USB-C port on the Waveshare board to the ToneX One USB-C port
- Connect 5 volts DC to the power input jack that you soldered in the first step
- Switch on the power supply
- The Waveshare board USB port will power the Tonex One. Do not connect 9 volts to it!
- Optional: for the Bluetooth Client version of code, switch on a M-Vave Chocolate Midi pedal (https://www.cuvave.com/productinfo/724103.html). After a few seconds it should connect and the Bluetooth icon should change from gray to blue
- Optional: for the Bluetooth Server version of code, the controller will be available as a peripheral for you to connect to via a Bluetooth Midi device. The Bluetooth icon should change from gray to blue when connected.

![wiring_waveshare_zero](https://github.com/user-attachments/assets/da535002-edf8-408a-aef1-a764ca35cb47)

### Wired Footswitches
Wired dual footswitches can optionally be used to select the Next/Previous preset.
The footswitch must be a "momentary" type that is only has its contacts closed when it is pressed.
The common pin of the footswitch must connect to the Controller ground pin, and the other 2 wires connected as shown.
![footswitches_waveshare_display](https://github.com/user-attachments/assets/5548f907-d769-4c65-8694-9b3ac25e7a86)
![footswitches_waveshare_zero](https://github.com/user-attachments/assets/7eda9912-f905-4e34-b8fe-beb595e10608)

### Wired Midi (firmware version V1.0.4.2 or above required)
Wired Midi is supported. A extra PCB is required, an "Adafruit Midi FeatherWing kit", for US$7.<br> 
https://www.adafruit.com/product/4740 <br>
This Midi board supports both 5-pin DIN sockets (included in kit) and 3.5mm jacks (not included in kit.)<br>
The Waveshare Zero can directly connect to this PCB. The Waveshare 4.3B requires another small interface. Details coming soon.
![midi_featherwing](https://github.com/user-attachments/assets/532d7d81-ae7e-485b-8d59-77ff6056e331)

Connect the Midi FeatherWing to the Controller as per the below diagrams (4.3B coming soon.)
![midi_waveshare_zero](https://github.com/user-attachments/assets/f5e34873-5ccb-4041-aa00-5f0a18ad4609)


### Case
With the Zero being a bare PCB, a case of some type is useful to protect it. User "xXGrimTagnBagXx" from Thingiverse has created a very compact case that can be 3D printed:
https://www.thingiverse.com/thing:6758917

Samples:

![image](https://github.com/user-attachments/assets/f2e9d599-2a16-46a2-a6cc-e0513fba060c)  ![image](https://github.com/user-attachments/assets/6b73ba98-ac7b-4451-bd50-5ae10c71226b)



