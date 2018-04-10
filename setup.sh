#!/bin/bash

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo -e "${YELLOW}Enter flash mode then press [Enter] to begin${NC}"
read -n1 -s
echo ""
echo "Flashing initial ROM data to ESP8266..."

esptool.py --port /dev/ttyUSB0 write_flash -fm qio 0x00000 init_data/0x00000.bin 0x10000 init_data/0x10000.bin
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
