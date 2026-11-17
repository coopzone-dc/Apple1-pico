; =====================================================================
; External Bus Card Scanner Routine ($C200-$CFFF & $D100-$DFFF)
; Scans the external expansion windows for responding hardware cards
; =====================================================================

* = $0380

SCANNER:
    LDA #$C2         ; Start address high byte ($C200)
    STA PTR+1
    LDA #$00
    STA PTR+0

NEXT_ADDR:
    LDY #$00
    LDA (PTR),Y      ; Read from expansion address (triggers expansion_bus_read)
    CMP #$FF         ; Open-bus pull-up usually reads $FF
    BEQ NOT_FOUND

    STA $00          ; Store active card response in zero page $00

NOT_FOUND:
    INC PTR+0
    BNE NEXT_ADDR
    INC PTR+1
    LDA PTR+1
    CMP #$D0         ; Reached end of $C200-$CFFF window?
    BNE NEXT_ADDR

    BRK

PTR:
    .dw $0000
