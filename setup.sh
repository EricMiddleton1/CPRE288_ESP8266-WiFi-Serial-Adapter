#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

SDK_BIN=$ESP_OPEN_SDK/ESP8266_NONOS_SDK*/bin


echo -e "${YELLOW}Enter flash mode then press [Enter] to begin${NC}"
read -n1 -s
echo ""
echo "Flashing initial ROM data to ESP8266..."


#Based on https://blog.vinczejanos.info/2016/11/14/unbrick-esp8266-blinking-blue-led/
esptool.py --port /dev/ttyUSB0 write_flash \
0x00000 $SDK_BIN/boot_v1.6.bin \
0x01000 $SDK_BIN/at/512+512/user1.1024.new.2.bin \
0x7E000 $SDK_BIN/blank.bin \
0x3FC000 $SDK_BIN/esp_init_data_default.bin


if [ "$?" -ne "0" ]; then
	echo -e "${RED}[Error] Flashing initial ROM data failed. Did you enter flash mode?${NC}"
	exit $?
fi


echo -e "${YELLOW}Enter flash mode then press [Enter] to begin${NC}"
read -n1 -s
echo ""
echo "Flashing WiFi-Bridge application to ESP8266..."

make flash
if [ "$?" -ne "0" ]; then
	error=$?
	echo -e "${RED}[Error] Flashing application failed. Did you enter flash mode?${NC}"
	echo -e "${YELLOW}Try flashing the application again using 'make flash'${NC}"
	exit $error
fi

echo -e "${GREEN}Done!${NC}"
