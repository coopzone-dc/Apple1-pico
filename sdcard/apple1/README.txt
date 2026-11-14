SD card layout (FAT-formatted card in the Pico's SD slot)

  roms/apple1basic.bin   Apple-1 Integer BASIC, loaded into RAM at $E000 at every power-up.
                         Either a 4096-byte binary, or a text hex dump in Woz Monitor
                         format ("E000: 4C B0 E2 ..."). Start it with E000R.
  tape/                  "tape" files made with C100R (created automatically on the first
                         write): FILE.DAT holds the raw bytes, FILE.MAP the ranges.

Copy the contents of this folder to the root of the card. The BASIC image is not
supplied with the project.
