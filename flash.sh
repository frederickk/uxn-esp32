BOARD=$1

echo "Compiling boot.rom"
./build.sh

echo "Flashing Firmware"
pio run -e $BOARD -t upload

echo "Flashing ROM"
pio run -e $BOARD -t uploadfs

echo "Done."
