;
;		True if HL >= DE
;
		.export __cmpgteq
		.export __cmpgteqb

		.setcpu 8080

		.code
;
;	The 8080 doesn't have signed comparisons directly
;
;	The 8085 has K which might be worth using TODO
;
__cmpgteqb:
		mvi	h,0
__cmpgteq:
		mov	a,h
		xra	d
		jp	sign_same
		xra	d		; A is now H
		jp	__false
		jmp	__true
sign_same:
		mov	a,e
		sub	l
		mov	a,d
		sbb	h
		jc	__false
		jmp	__true
