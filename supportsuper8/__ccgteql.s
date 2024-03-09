;
;	Comparison between top of stack and ac
;

	.export __ccgteql
	.export __ccgtequl
	.code

__ccgtequl:
	ldw rr14,216
	incw rr14
	incw rr14
	lde r12,@rr14
	cp r12,r0
	jr ult,true
	jr nbyte
__ccgteql:
	ldw rr14,216
	incw rr14
	incw rr14	; point to data
	ldei r12,@rr14
	cp r12,r0
	jr lt,true
nbyte:
	jr nz,false
	ldei r12,@rr14
	cp r12,r1
	jr ult,true
	jr nz,false
	ldei r12,@rr14
	cp r12,r2
	jr ult,true
	jr nz,false
	lde r12,@rr14
	cp r12,r3
	jr ult,true
false:
	clr r3
out:
	clr r2
	pop r14		; return address
	pop r15
	add 217,#4
	adc 216,#0
	push r15
	push r14
	xor r3,#1	; invert and get the flags right
	ret
true:
	ld r3,#1
	jr out
