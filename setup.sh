#!/bin/bash

read -p "Enter flash mode then press [Enter] to begin" -n1 -s
echo ""
echo "Flashing initial ROM data to ESP8266..."

esptool.py --port /dev/ttyUSB0 write_flash -fm qio 0x00000 init_data/0x00000.bin 0x10000 init_data/0x10000.bin

read -p "Enter flash mode then press [Enter] to begin" -n1 -s
echo ""
echo "Flashing WiFi-Bridge application to ESP8266..."

make flash

echo "Done!"
