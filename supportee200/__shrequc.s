;
;	Shift right TOS by non constant B
;
	.setcpu	4
	.export __shrequc

	.code

; Need to do a spot of work for the unsinged shift
__shrequc:
	lda	15
	nab
	bz	nowork
	lda	2(s)
	ldab	(a)
	bz	nowork
	rl
	rrrb	al
	bra	go
	; Might be worth byte swap optimzations ?
next:
	srab
go:
	dcr	b
	bnz	next
done:
	xab
	lda	2(s)
	stbb	(a)
	inr	s
	inr	s	; pull TOS out of the way
	rsr
nowork:
	lda	2(s)
	ldbb	(a)
	inr	s
	inr	s
	rsr
	