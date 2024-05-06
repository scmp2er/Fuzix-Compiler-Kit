;
;	Shift right TOS by non constant B
;
	.setcpu	4
	.export __shr

	.code

; Need to do a spot of work for the unsinged shift
__shr:
	lda	15
	nab
	bz	nowork
	lda	2(s)
	bz	nowork
	; Might be worth byte swap optimzations ?
next:
	sra
go:
	dcr	b
	bnz	next
done:
	inr	s
	inr	s	; pull TOS out of the way
	xab
	rsr
nowork:
	ldb	2(s)
	inr	s
	inr	s
	rsr
	