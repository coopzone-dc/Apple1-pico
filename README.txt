SD card layout (FAT-formatted card in the Pico's SD slot). Copy the apple1 folder to the
root of the card:

  /apple1/roms/apple1basic.bin   Apple-1 Integer BASIC, loaded into RAM at $E000 at every
                                 power-up. A 4096-byte binary, or a text hex dump in Woz
                                 Monitor format ("E000: 4C B0 E2 ..."). Start it with E000R.
  /apple1/tape/                  "tape" files made with C100R (folders are created on the
                                 first write): FILE.DAT holds the raw bytes, FILE.MAP the ranges.

The BASIC image is not supplied with the project. The folder name comes from the CMake
option APPLE1_ROOT (default "apple1").
