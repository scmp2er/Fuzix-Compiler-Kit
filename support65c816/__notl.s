	.65c816
	.a16
	.i16

	.export __notl

__notl:
	ldx #0
	stx @hireg
	ora @hireg
	beq true
	lda #0
	rts
true:
	inc a		; was 0 now 1
	rts
