;
;	*TOS - AC
;
	.export __minuseq
	.code


__minuseq:
	pop r12		;	return
	pop r13
	pop r14		;	pointer (TODO: pass ptr in call ?)
	pop r15
	push r13
	push r12

	incw rr14

	lde r12,@rr14
	sub r3,r12
	lde @rr14,r3
	decw rr14

	lde r12,@rr14
	sbc r2,r12
	lde @rr14,r2

	ret
