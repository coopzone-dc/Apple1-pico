; =====================================================================
; Apple 1 Test Assembly for External Expansion Bus ($C200-$CFFF, $D100-$DFFF)
; Tests PWM-synchronized bus writes and reads via SN74LVC4245A & 74AHCT573
; =====================================================================

* = $0300            ; Origin in internal RAM

START:
    LDX #$00         ; Initialize test byte counter
LOOP:
    LDA TEST_DATA,X  ; Load test pattern byte
    STA $C200,X      ; Write to External Expansion Window 1 ($C200-$C207)
    LDA $C200,X      ; Read back from External Card to verify transceiver
    STA $0200,X      ; Store read-back result in internal RAM ($0200-$0207) for Wozmon inspection
    INX
    CPX #$08         ; Test 8 bytes
    BNE LOOP

    BRK              ; Return to Wozmon (inspect 0200.0207 to verify)

TEST_DATA:
    .byte $11, $22, $44, $88, $FF, $00, $AA, $55
