; ---------------------------------------------------------------------------
; aci.asm - emulated Apple-1 cassette interface ROM, $C100-$C1FF
;
; Enter with C100R from the Woz Monitor. Shows the '*' prompt and reads one
; line into the monitor's input buffer at $0200 (same keys as the monitor:
; '_' rubout, ESC starts the line again). The line is then handed to the
; emulator by writing to the trap register; the emulator (aci.c) executes it
; against the SD card and the ROM prompts again. A blank line leaves the ACI
; and returns to the Woz Monitor.
;
; Machine code is embedded in aci_rom.h (58 bytes, remainder of the page $FF).
; Assembles at ORG $C100 with any 6502 assembler.
; ---------------------------------------------------------------------------

IN      EQU $0200       ; monitor line buffer (characters stored with bit 7 set)
TRAP    EQU $C0F0       ; write here = "execute the line in IN" (emulator trap)
KBD     EQU $D010       ; PIA keyboard data
KBDCR   EQU $D011       ; PIA keyboard control (bit 7 = key ready)
ECHO    EQU $FFEF       ; Woz Monitor: print character in A
MONESC  EQU $FF1A       ; Woz Monitor: print '\' prompt and read a line

        ORG $C100

START   LDA #$8D        ; new line ...
        JSR ECHO
PROMPT  LDA #$AA        ; ... then the '*' prompt (bit 7 set, as ECHO expects)
        JSR ECHO
        LDY #$01        ; Y = 1, first DEY below makes it 0
BKSP    DEY             ; back up one character
        BMI START       ; backed up past start of line: new line and prompt
NEXTCH  LDA KBDCR
        BPL NEXTCH      ; wait for a key
        LDA KBD
        STA IN,Y        ; store it
        JSR ECHO        ; and echo it
        CMP #$8D        ; carriage return?
        BEQ EXEC        ;   yes: run the line
        CMP #$DF        ; '_' rubout?
        BEQ BKSP        ;   yes: back up
        CMP #$9B        ; ESC?
        BEQ START       ;   yes: new line and prompt
        INY             ; next position in buffer
        BPL NEXTCH      ; room left (Y < $80): keep going
        BMI START       ; line too long: new line and prompt
EXEC    TYA             ; Y = length of the line (0 = blank line)
        BEQ EXIT        ;   blank: leave the ACI
        STA TRAP        ; emulator runs the command line
        JMP PROMPT      ; its output ended with a new line: prompt again
EXIT    JMP MONESC      ; back to the monitor

; ---------------------------------------------------------------------------
; Listing
;   C100: A9 8D     START   LDA #$8D
;   C102: 20 EF FF          JSR ECHO
;   C105: A9 AA     PROMPT  LDA #$AA
;   C107: 20 EF FF          JSR ECHO
;   C10A: A0 01             LDY #$01
;   C10C: 88        BKSP    DEY
;   C10D: 30 F1             BMI START
;   C10F: AD 11 D0  NEXTCH  LDA KBDCR
;   C112: 10 FB             BPL NEXTCH
;   C114: AD 10 D0          LDA KBD
;   C117: 99 00 02          STA IN,Y
;   C11A: 20 EF FF          JSR ECHO
;   C11D: C9 8D             CMP #$8D
;   C11F: F0 0D             BEQ EXEC
;   C121: C9 DF             CMP #$DF
;   C123: F0 E7             BEQ BKSP
;   C125: C9 9B             CMP #$9B
;   C127: F0 D7             BEQ START
;   C129: C8                INY
;   C12A: 10 E3             BPL NEXTCH
;   C12C: 30 D2             BMI START
;   C12E: 98        EXEC    TYA
;   C12F: F0 06             BEQ EXIT
;   C131: 8D F0 C0          STA TRAP
;   C134: 4C 05 C1          JMP PROMPT
;   C137: 4C 1A FF  EXIT    JMP MONESC
; ---------------------------------------------------------------------------
