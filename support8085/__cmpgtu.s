		.export __cmpgtu
		.export __cmpgtub
		.setcpu 8080
		.code

		; true if HL > DE
__cmpgtub:
		mvi	h,0
__cmpgtu:
		mov	a,h
		cmp	d
		jc	__false
		jnz	__true
		mov	a,l
		cmp	e
		jz	__false
		jnc	__true
		jmp	__false
