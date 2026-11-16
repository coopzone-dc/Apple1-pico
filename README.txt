SD card layout (FAT-formatted card in the Pico's SD slot). Copy the apple1 folder to the
root of the card:

  /apple1/tape/                  Unified folder for all "tape" images (folders are created
                                 on the first write):
                                   FILE.DAT  - raw binary bytes
                                   FILE.MAP  - memory range map (e.g. "0300.03FF")

Boot-time autoloading (for Integer BASIC, Krusader, etc.):
  Add the line AUTO to a file's .MAP (or save from the * prompt with AUTO at the end,
  e.g. "E000.EFFFW BASIC.DAT AUTO") to load that image into RAM automatically at power-up:

    /apple1/tape/BASIC.DAT       4096-byte Apple-1 Integer BASIC binary
    /apple1/tape/BASIC.MAP       Contains:
                                   AUTO
                                   E000.EFFF

  Start BASIC with E000R in the Woz Monitor. Pressing Ctrl-] resets the CPU and PIA while
  leaving all autoloaded images intact in RAM.

The BASIC image is not supplied with the project. The folder name comes from the CMake
option APPLE1_ROOT (default "apple1").
