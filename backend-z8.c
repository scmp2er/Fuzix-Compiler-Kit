/*
 *	Beginnings of a Z8 code generator
 *
 *	TODO
 *	- register tracking
 *	- using tcm/tm for bitops
 *	- using tcm/tm for < 0 >= 0
 *	- using incw/decw for cmp -1 or 1
 *	- support library
 *	- efficient comparisons
 *	- flag switching on jtrue/false for comparisons
 *	- CCONLY Z80 style
 *	- registers in r4/5 6/7 8/9 10/11
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "compiler.h"
#include "backend.h"

#define BYTE(x)		(((unsigned)(x)) & 0xFF)
#define WORD(x)		(((unsigned)(x)) & 0xFFFF)

#define ARGBASE	2	/* Bytes between arguments and locals if no reg saves */

#define T_NREF		(T_USER)		/* Load of C global/static */
#define T_CALLNAME	(T_USER+1)		/* Function call by name */
#define T_NSTORE	(T_USER+2)		/* Store to a C global/static */
#define T_LREF		(T_USER+3)		/* Ditto for local */
#define T_LSTORE	(T_USER+4)
#define T_LBREF		(T_USER+5)		/* Ditto for labelled strings or local static */
#define T_LBSTORE	(T_USER+6)
#define T_RREF		(T_USER+7)
#define T_RSTORE	(T_USER+8)
#define T_RDEREF	(T_USER+9)		/* *regptr */
#define T_REQ		(T_USER+10)		/* *regptr = */
#define T_RDEREFPLUS	(T_USER+11)		/* *regptr++ */
#define T_REQPLUS	(T_USER+12)		/* *regptr++ =  */
#define T_LSTREF	(T_USER+13)		/* reference via a local ptr to struct field */
#define T_LSTSTORE	(T_USER+14)		/* store ref via a local ptr to struct field */

/*
 *	State for the current function
 */
static unsigned frame_len;	/* Number of bytes of stack frame */
static unsigned sp;		/* Stack pointer offset tracking */
static unsigned argbase;	/* Argument offset in current function */
static unsigned unreachable;	/* Code following an unconditional jump */
static unsigned label_count;	/* Used for internal labels X%u: */

static unsigned r14_sp;		/* R14/15 address relative to SP */
static unsigned r14_valid;	/* R14/15 are a valid local ptr */
static unsigned r2_sp;		/* R2/3 address relative to SP */
static unsigned r2_valid;	/* R2/3 are a valid local ptr */

static uint8_t r_val[16];
static uint8_t r_type[16];	/* For now just unknown and const */
#define RV_UNKNOWN	0
#define RV_CONST	1

static struct node ac_node;	/* What is in AC 0 type = unknown */

/* Minimal tracking on the working register only */

static void invalidate_ac(void)
{
	if (ac_node.op)
		printf(";invalidate ac\n");
	ac_node.op = 0;
}

static void set_ac_node(struct node *n)
{
	register unsigned op = n->op;
	printf(";ac node now O %u T %u V %lu\n",
		op, n->type, n->value);
	/* Whatever was in AC just got stored so we are now a valid lref */
	if (op == T_LSTORE)
		op = T_LREF;
	if (op == T_RSTORE)
		op = T_RREF;
	if (op != T_LREF && op != T_RREF) {
		ac_node.type = 0;
		return;
	}
	memcpy(&ac_node, n, sizeof(ac_node));
	ac_node.op = op;
}

static void flush_all(unsigned f)
{
}

static void gen_symref(struct node *n)
{
	register unsigned op = n->op;
	register unsigned v = n->value;
	if (op == T_NREF || op == T_NSTORE)
		printf("\t.word _%s+%u\n",  namestr(n->snum), v);
	else if (op == T_LBREF || op == T_LBSTORE)
		printf("\t.word T%u+%u\n", n->val2, v);
	else
		error("gsr");
}


#define R_SPL	255
#define R_SPH	254

/* 0xEx is special encoding form. We borrow that internally to indicate
   using the currennt working register byte (usually encoded as 0-3) */
#define R_ACCHAR	0xE3
#define R_ACINT		0xE2		/* Start reg for ptr/int */
#define R_ACPTR		0xE2
#define R_ACLONG	0xE0
#define R_ISAC(x)		(((x) & 0xE0) == 0xE0)
#define R_AC		0xE0

#define REGBASE		2		/* Reg vars 4,6,8,10 numbered 1,2,3,4 */

#define R_REG(x)	(((x) * 2) + REGBASE)
#define R_REG_S(x,s)	(((x) * 2) + REGBASE + (2 - size))

#define R_WORK		12		/* 12/13 are work ptr */
#define R_INDEX		14		/* 14/15 are usually index */

/*
 *	Beginning of hooks for modify trackign etc
 */

static void r_modify(unsigned r, unsigned size)
{
	if (R_ISAC(r))
		r = 4 - size;
	while(size--) {
		if (r == 14 || r == 15)
			r14_valid = 0;
		if (r == 2 || r == 3) {
			r2_valid = 0;
			invalidate_ac();
		}
		r_type[r++] = RV_UNKNOWN; 
	}
}

/* Until we do value track */
static void r_set(unsigned r, unsigned v)
{
	if (r == 14 || r == 15)
		r14_valid = 0;
	if (r == 2 || r == 3) {
		r2_valid = 0;
		invalidate_ac();
	}
	r_val[r] = v & 0xFF;
	r_type[r] = RV_CONST;
}

static unsigned r_do_adjust(unsigned r, int by, unsigned size)
{
	unsigned rb = r;
	unsigned n;
	r += size - 1;
	if (r_type[r] == RV_CONST) {
		n = r_val[r] + (by & 0xFF);
		r_val[r] = n & 0xFF;
		while(--size) {
			--r;
			by >>= 8;
			if (r_type[r] == RV_CONST) {
				n = r_val[r] + (by & 0xFF) + (n >> 8);
				r_val[r] = n;
			} else
				break;
		}
	}
	/* Mark any unknown bytes as unknown */
	if (size)
		r_modify(rb, size);
	return size;
}

static void r_adjust(unsigned r, int by, unsigned size)
{
	if (r == 14 && size == 2 && r14_valid) {
		r14_sp += by;
		r_do_adjust(14, by, 2);
		r14_valid = 1;
	} else if (r == 2 && size == 2 && r2_valid) {
		r2_sp += by;
		r_do_adjust(2, by, 2);
		r2_valid = 1;
	} else
		size = r_do_adjust(r, by , size);
}

static void invalidate_all(void)
{
	invalidate_ac();
	r_modify(0, 16);
}

/*
 *	These will eventually be smart and track values etc but for now
 *	just do dumb codegen
 */

static void r_dec(unsigned r)
{
	printf("\tdec r%u\n", r);
	r_adjust(r, -1, 1);
}

static void r_inc(unsigned r)
{
	printf("\tinc r%u\n", r);
	r_adjust(r, 1, 1);
}

static void rr_decw(unsigned rr)
{
	if (R_ISAC(rr))
		rr = R_ACPTR;
	printf("\tdecw rr%u\n", rr);
	r_adjust(rr, -1, 2);
}

static void RR_decw(unsigned rr)
{
	printf("\tdecw %u\n", rr);
}

static void rr_incw(unsigned rr)
{
	if (R_ISAC(rr))
		rr = R_ACPTR;
	printf("\tincw rr%u\n", rr);
	r_adjust(rr, 1, 2);
}

static void RR_incw(unsigned rr)
{
	printf("\tincw %u\n", rr);
}

static void load_r_name(unsigned r, struct node *n, unsigned off)
{
	const char *c = namestr(n->snum);
	if (R_ISAC(r)) {
		r = 2;
		set_ac_node(n);
	}
	r_modify(r, 2);
	printf("\tld r%u,#>_%s+%u\n", r, c, off);
	printf("\tld r%u,#<_%s+%u\n", r + 1, c, off);
}

static void load_r_label(unsigned r, struct node *n, unsigned off)
{
	if (R_ISAC(r)) {
		r = 2;
		set_ac_node(n);
	}
	r_modify(r, 2);
	printf("\tld r%u,#>T%u+%u\n", r, n->val2, off);
	printf("\tld r%u,#<T%u+%u\n", r + 1, n->val2, off);
}

static void load_r_r(unsigned r1, unsigned r2)
{
	if (R_ISAC(r1))
		r1 = r1 & 0x0F;
	if (R_ISAC(r2))
		r2 = r2 & 0x0F;

	if (r1 < 4)
		invalidate_ac();
	r_modify(r1, 1);
	printf("\tld r%u,r%u\n", r1, r2);
}

static void load_r_R(unsigned r1, unsigned r2)
{
	if (R_ISAC(r1))
		r1 = r1 & 0x0F;
	r_modify(r1,1);
	printf("\tld r%u,%u\n", r1, r2);
}

static void load_R_r(unsigned r1, unsigned r2)
{
	if (R_ISAC(r2))
		r2 = r2 & 0x0F;
	printf("\tld %u,r%u\n", r1, r2);
}

/* Low level ops so we can track them later */
static void op_r_c(unsigned r1, unsigned val, const char *op)
{
	if (r1 < 4)
		invalidate_ac();
	r_modify(r1, 1);
	printf("\t%s r%u,#%u\n", op, r1, val);
}

static void op_r_r(unsigned r1, unsigned r2, const char *op)
{
	if (r1 < 4)
		invalidate_ac();
	r_modify(r1, 1);
	printf("\t%s r%u,r%u\n", op, r1, r2);
}

static void opnoeff_r_r(unsigned r1, unsigned r2, const char *op)
{
	printf("\t%s r%u,r%u\n", op, r1, r2);
}

static void load_r_constb(unsigned r, unsigned char v)
{
	/* Once we do reg tracking we'll be able to deal with dups
	   using ld r1,r2 */
	unsigned i = 0;
	uint8_t *p = r_type;

	if (R_ISAC(r))
		r &= 0x0F;

	if (v == 0) {
		if (r_type[r] != RV_CONST || r_val[r] != 0) {
			printf("\tclr r%u\n", r);
			r_set(r, v);
		}
		return;
	} else if (r_type[r] == RV_CONST && r_val[r] == v - 1) {
		printf("\tinc r%u\n", r);
		r_set(r, v);
		return;
	} else {
		while(i < 16) {
			if (*p++ == RV_CONST && r_val[i] == v) {
				/* Already has the value */
				if (i == r) {
					printf("; r%u already %u\n", r, v);
					return;
				}
				printf("; r%u get %u from %u\n", r, v, i);
				load_r_r(r, i);
				r_set(r, v);
				return;
			}
			i++;
		}
	}
	printf("\tld r%u,#%u\n", r, v);
	r_set(r, v);
}

static void load_rr_const(unsigned r, unsigned v)
{
	if (r == R_ACPTR)
		r = 2;
	load_r_constb(r, v >> 8);
	load_r_constb(r + 1, v);
}

static void load_r_const(unsigned r, unsigned long v, unsigned size)
{
	if (R_ISAC(r))
		r = 4 - size;
	/* Lots to do here for optimizing - using clr, copying bytes
	   between regs */
	if (size == 4) {
		load_r_constb(r++, v >> 24);
		load_r_constb(r++, v >> 16);
	}
	if (size > 1)
		load_r_constb(r++, v >> 8);
	load_r_constb(r++, v);
}

static void add_r_const(unsigned r, unsigned long v, unsigned size)
{
	if (R_ISAC(r))
		r = 3;
	else
		r += size - 1;

	if (r < 4)
		invalidate_ac();

	r_adjust(r - size + 1, v, size);

	/* Eliminate any low bytes that are not changed */
	while(size && (v & 0xFF) == 0x00) {
		r--;
		size--;
		v >>= 8;
	}
	if (size == 0)
		return;

	/* ADD to r%d is 3 bytes so the only useful cases are word inc/dec of
	   1 which are two bytes as incw/decw and four as add/adc */
	if (size == 2) {
		if (v == 2) {
			rr_incw(r - 1);
			rr_incw(r - 1);
			return;
		}
		if ((signed)v == -1) {
			rr_decw(r - 1);
			return;
		}
		if (v == 1) {
			rr_incw(r - 1);
			return;
		}
		if ((signed)v == -2) {
			rr_decw(r - 1);
			rr_decw(r - 1);
			return;
		}
	}
	/* inc rn is 1 byte, dec rn is 2 ! */
	if (size == 1) {
		if (v == 2) {
			r_inc(r);
			r_inc(r);
			return;
		}
		if ((signed)v == -1) {
			r_dec(r);
			return;
		}
		if (v == 1) {
			r_inc(r);
			return;
		}
		/* For -2 dec, dec is 4 bytes whilst add is 3 */
	}
		
	printf("\tadd r%u,#%u\n", r--, (unsigned)v & 0xFF);
	while(--size) {
		v >>= 8;
		printf("\tadc r%u,#%u\n", r--, (unsigned)v & 0xFF);
	}
}

static void add_R_const(unsigned r, unsigned long v, unsigned size)
{
	r += size - 1;
	/* Eliminate any low bytes that are not changed */
	while(size && (v & 0xFF) == 0x00) {
		r--;
		size--;
		v >>= 8;
	}
	if (size == 0)
		return;
	/* TODO: spot cases to use inc/incw/dec/decw */
	printf("\tadd %u,#%u\n", r--, (unsigned)v & 0xFF);
	while(--size) {
		v >>= 8;
		printf("\tadc %u,#%u\n", r--, (unsigned)v & 0xFF);
	}
}

/* Both registers may be the same */
static void add_r_r(unsigned r1, unsigned r2, unsigned size)
{
	if (R_ISAC(r1))
		r1 = 3;
	else
		r1 += size - 1;
	if (R_ISAC(r2))
		r2 = 3;
	else
		r2 += size - 1;

	if (r1 < 4)
		invalidate_ac();

	r_modify(r1, 2);

	printf("\tadd r%u,r%u\n", r1--, r2--);
	while(--size)
		printf("\tadc r%u,r%u\n", r1--, r2--);
}


static void sub_r_r(unsigned r1, unsigned r2, unsigned size)
{
	if (R_ISAC(r1))
		r1 = 3;
	else
		r1 += size - 1;
	if (R_ISAC(r2))
		r2 = 3;
	else
		r2 += size - 1;

	if (r1 < 4)
		invalidate_ac();

	r_modify(r1 - size + 1, size);

	printf("\tsub r%u,r%u\n", r1--, r2--);
	while(--size)
		printf("\tsbc r%u,r%u\n", r1--, r2--);
}

static void sub_r_const(unsigned r, unsigned long v, unsigned size)
{
	if (R_ISAC(r))
		r = 3;
	else
		r += size - 1;

	if (r < 4)
		invalidate_ac();

	/* SUB to r%d is 2 bytes so the only useful cases are word inc/dec of
	   1 which are two bytes as incw/decw and four as add/adc */
	if (size == 2) {
		if (v == 1) {
			rr_decw(r - 1);
			return;
		}
		if ((signed)v == -1) {
			rr_decw(r - 1);
			return;
		}
	}
	if (size == 1) {
		if (v == 1) {
			r_dec(r);
			return;
		}
		if (v == -1) {
			r_inc(r);
			return;
		}
		if (v == -2) {
			r_inc(r);
			r_inc(r);
			return;
		}
	}

	r_adjust(r - size + 1, -v, size);

	/* Eliminate any low bytes that are not changed */
	while(size && (v & 0xFF) == 0x00) {
		r--;
		size--;
		v >>= 8;
	}
	if (size == 0)
		return;

	printf("\tsub r%u,#%u\n", r--, (unsigned)v & 0xFF);
	while(--size) {
		v >>= 8;
		printf("\tsbc r%u,#%u\n", r--, (unsigned)v & 0xFF);
	}
}

/* Same FIXME as cmp_r_const. Will be far better once we have cc tests */
static void cmpne_r_0(unsigned r, unsigned size)
{
	if (R_ISAC(r))
		r = 4 - size;
	else
		load_r_constb(3, 0);
	if (size == 1)
		printf("\tor r3, r%u\n", r);
	else while(--size)
		printf("\tor r3, r%u\n", r++);
	r_modify(3, 1);
	load_r_constb(2,0);
	printf("\tjr z, X%u\n", ++label_count);
	load_r_constb(3, 1);
	printf("X%u:\n", label_count);
}

static void cmpeq_r_0(unsigned r, unsigned size)
{
	/* Will be able to do better for T_BOOL cases and T_BANG */
	cmpne_r_0(r, size);
	op_r_c(3, 1, "xor");
	r_modify(3, 1);
}

static void mono_r(unsigned r, unsigned size, const char *op)
{
	if (R_ISAC(r))
		r = 4 - size;
	if (r < 4)
		invalidate_ac();
	r_modify(r, size);
	while(size--)
		printf("\t%s r%u\n", op, r++);
}

static void djnz_r(unsigned r, unsigned l)
{
	if (R_ISAC(r))
		r = 3;
	r_modify(r, 1);
	printf("\tdjnz r%u, X%u\n", r, l);
}

static unsigned label(void)
{
	invalidate_all();
	unreachable = 0;
	printf("X%u:\n", ++label_count);
	return label_count;
}


/*
 *	Perform a right shift of a register set signed or unsigned.
 *	Doesn't currently know about 8bit at a time shifts for unsigned
 *	in particular. Might want to make long non fast const a helper ?
 */
static void rshift_r(unsigned r, unsigned size, unsigned l, unsigned uns)
{
	unsigned x;

	if (R_ISAC(r))
		r = 4 - size;
	if (r < 4)
		invalidate_ac();
	r_modify(r, size);

	l &= (8 * size) - 1;
	if (l == 0)
		return;
	if (uns) {
		/* These can only occur for 32bit shift */
		if (l == 24) {
			load_r_r(r + 3, r);
			load_r_constb(r, 0);
			load_r_constb(r + 1, 0);
			load_r_constb(r + 2, 0);
			return;
		}
		if (l == 16) {
			load_r_r(r + 2, r);
			load_r_r(r + 3, r + 1);
			load_r_constb(r, 0);
			load_r_constb(r + 1, 0);
			return;
		}
		if (l == 8) {
			if (size == 2) {
				load_r_r(r + 1, r);
				load_r_constb(r, 0);
			} else {
				load_r_r(r + 1, r);
				load_r_r(r + 2, r + 1);
				load_r_r(r + 3, r + 2);
				load_r_constb(r, 0);
			}
			return;
		}
	}
	if (l > 1) {
		load_r_constb(R_INDEX, l);
		x = label();
	}
	if (uns)
		printf("\trcf\n\trrc r%u\n", r++);
	else
		printf("\tsra r%u\n", r++);
	while(--size)
		printf("\trrc r%u\n", r++);
	if (l > 1)
		djnz_r(R_INDEX, x);
}

#define OP_AND	0
#define OP_OR	1
#define OP_XOR	2

static void logic_r_const(unsigned r, unsigned long v, unsigned size, unsigned op)
{
	const char *opn = "and\0or\0\0xor" + op * 4;
	unsigned n;

	if (R_ISAC(r))
		r = 4;
	else
		r += size;

	if (r <= 4)
		invalidate_ac();

	/* R is now 1 after low byte */
	while(size--) {
		r--;
		n = v & 0xFF;
		v >>= 8;
		if (n == 0xFF) {
			if (op == OP_OR)
				load_r_constb(r, 0xFF);
			else if (op == OP_XOR) {
				printf("\tcom r%u\n", r);
				r_modify(r, 1);
			}
			/* and nothing for and FF */
			continue;
		}
		else if (n == 0x00) {
			if (op == OP_AND)
				load_r_constb(r, 0x00);
			/* OR and XOR do nothing */
			continue;
		} else {
			printf("\t%s r%u, #%u\n", opn, r, n);
			/* TODO: can calc this if know, not clear it is useful */
			r_modify(r, 1);
		}
	}
}

static void logic_r_r(unsigned r1, unsigned r2, unsigned size, unsigned op)
{
	const char *opn = "and\0or\0\0xor" + op * 4;

	if (R_ISAC(r1))
		r1 = 4;
	else
		r1 += size;

	if (R_ISAC(r2))
		r2 = 4;
	else
		r2 += size;

	if (r1 <= 4)
		invalidate_ac();

	r_modify(r1, size);

	/* R is now 1 after low byte */
	while(size--)
		printf("\t%s r%u, r%u\n", opn, --r1, --r2);
}

static void load_l_sprel(unsigned r, unsigned off)
{
	load_r_R(r, R_SPH);
	load_r_R(r + 1, R_SPL);
	add_r_const(r, off, 2);

	if (r == R_INDEX) {
		r14_valid = 1;
		r14_sp = off - sp;
		printf(";r14 valid %d\n", r14_sp);
	}
	if (r == 2) {
		r2_valid = 1;
		r2_sp = off - sp;
		printf(";r2 valid %d\n", r2_sp);
	}
}

static void load_l_mod(unsigned r, unsigned rs, unsigned diff, unsigned off)
{
	if (r != rs) {
		load_r_r(r, rs);
		load_r_r(r + 1, rs + 1);
	}
	add_r_const(r, -diff, 2);

	if (r == R_INDEX) {
		r14_valid = 1;
		r14_sp = off - sp;
		printf(";r14 valid %d\n", r14_sp);
	}
	if (r == 2) {
		r2_valid = 1;
		r2_sp = off - sp;
		printf(";r2 valid %d\n", r2_sp);
	}
}

static unsigned load_l_cost(int diff)
{
	unsigned n = abs(diff);
	if (n <= 2)
		return 2 * n;
	return 6;
}

static void load_r_local(unsigned r, unsigned off)
{
	int diff14;
	int diff2;
	unsigned cost2 = 0xFFFF, cost14 = 0xFFFF, costsp;

	if (R_ISAC(r))
		r = 2;

	diff14 = r14_sp - (off - sp);
	diff2 = r2_sp - (off - sp);

	printf("; load r local %d\n", off);
	if (r14_valid)
		printf(";14: local %d (cache %d) diff %d\n", off - sp, r14_sp, diff14);
	if (r2_valid)
		printf(";2: local %d (cache %d) diff %d\n", off - sp, r2_sp, diff2);

	/* Work out the cost for each */
	if (r14_valid) {
		cost14 = load_l_cost(diff14);
		if (r != 14)
			cost14 += 4;
	}
	if (r2_valid) {
		cost2 = load_l_cost(diff2);
		if (r != 2)
			cost2 += 4;
	}
	costsp = load_l_cost(off) + 6;

	printf(";costs r2 %d r4 %d rsp %d\n", cost2, cost14, costsp);

	/* Cheapest is to load from SP */
	if (costsp <= cost2 && costsp <= cost14) {
		load_l_sprel(r, off);
		return;
	}
	if (cost2 < cost14)
		load_l_mod(r, 2, diff2, off);
	else
		load_l_mod(r, 14, diff14, off);
}

static void load_local_helper(unsigned v, unsigned size)
{
	/* Only works for R_AC case */
	/* We add the extra 2 because the call adjusted the stack as well */
	v += sp;
	if (v < 254) {
		load_r_const(R_INDEX + 1, v + 2, 1);
		printf("\tcall __gargr%u\n", size);
		r_modify(14,2);
	} else {
		load_r_const(R_INDEX, v + 2, 2);
		printf("\tcall __gargrr%u\n", size);
		r_modify(14,2);
	}
	r_modify(4 - size, size);
	r_modify(12, 2);
	r14_valid = 1;
	r14_sp = v - sp + size - 1;
}

static void load_work_helper(unsigned v, unsigned size)
{
	/* We add the extra 2 because the call adjusted the stack as well */
	v += sp;
	if (v < 254) {
		load_r_const(R_INDEX + 1, v + 2, 1);
		printf("\tcall __garg12r%u\n", size);
		r_modify(14,2);
	} else {
		load_r_const(R_INDEX, v + 2, 2);
		printf("\tcall __garg12rr%u\n", size);
		r_modify(14,2);
	}
	r_modify(12, 2);
	r14_valid = 1;
	r14_sp = v - sp + size - 1;
}

static void store_local_helper(struct node *r, unsigned v, unsigned size)
{
	/* Only works for R_AC case */
	/* We add the extra 2 because the call adjusted the stack as well */
	v += sp;
	if (v < 254) {
		load_r_const(R_INDEX + 1, v + 2, 1);
		/* Extra helper paths optimize assign with 0 or 1L */
		if (r && r->op == T_CONSTANT) {
			if (r->value == 0)
				printf("\tcall __pargr%u_0\n", size);
			else if (r->value == 1 && size == 4)
				printf("\tcall __pargr%u_0\n", size);
			else
				printf("\tcall __pargr%u\n", size);
		}
		else
			printf("\tcall __pargr%u\n", size);
		r_modify(14,2);
	} else {
		load_r_const(R_INDEX, v + 2, 2);
		printf("\tcall __pargrr%u\n", size);
		r_modify(14,2);
	}
	r14_valid = 1;
	r14_sp = v - sp + size - 1;
}

/* TODO: support the T_DEREF of AC into AC case specially by having a
   loadac%u which is ld r12, r2 ldr 13,r3 fall into load%u */
static void load_r_memr(unsigned val, unsigned rr, unsigned size)
{
	if (R_ISAC(val))
		val = 4 - size;
	if (R_ISAC(rr))
		rr = 2;
	/* Check this on its own as we sometimes play games with AC
	   registers */
	if (val == 4 - size) {
		invalidate_ac();
		/* We use helpers for the usual case when building for
		   small */
		if (rr == R_INDEX && size > 1 && opt < 1){
			printf("\tcall __load%u\n", size);
			r_adjust(R_INDEX, size - 1, 2);
			r_modify(val, size);
			return;
		}
	}
	printf("\tlde r%u, @rr%u\n", val, rr);
	r_modify(val, size);
	while(--size) {
		val++;
		rr_incw(rr);
		printf("\tlde r%u, @rr%u\n", val, rr);
	}
}

static void store_r_memr(unsigned val, unsigned rr, unsigned size)
{
	if (R_ISAC(val))
		val = 4 - size;
	if (R_ISAC(rr))
		rr = 2;
	if (val == 4 - size && rr == R_INDEX && size > 1 && opt < 1) {
		printf("\tcall __store%u\n", size);
		r_adjust(R_INDEX, size - 1, 2);
		return;
	}
	printf("\tlde @rr%u, r%u\n", rr, val);
	while(--size) {
		val++;
		rr_incw(rr);
		printf("\tlde @rr%u, r%u\n", rr, val);
	}
}

/* Store reverse direction. We use it for eq ops for the moment but we
   could also try and spot cases where the ptr is at the other end of
   the value (ditto we could revload). Would need some smarts in T_LREF,
   T_LSTORE and a helper to decide which way to go : TODO */
static void revstore_r_memr(unsigned val, unsigned rr, unsigned size)
{
	if (R_ISAC(val)) {
		val = 3;
		if (rr == R_INDEX && size > 1 && opt < 1) {
			printf(";%x index %x\n", r14_valid, r14_sp);
			printf("\tcall __revstore%u\n", size);
			r_adjust(R_INDEX, - (size - 1), 2);
			printf(";%x index %x\n", r14_valid, r14_sp);
			return;
		}
	} else
		val += size - 1;
	if (R_ISAC(rr))
		rr = 2;
	printf("\tlde @rr%u, r%u\n", rr, val);
	while(--size) {
		val--;
		rr_decw(rr);
		printf("\tlde @rr%u, r%u\n", rr, val);
	}
}

static void push_r(unsigned r)
{
	if (R_ISAC(r))
		r = r & 0x0F;
	printf("\tpush r%u\n", r);
}

static void pop_r(unsigned r)
{
	if (R_ISAC(r)) {
		invalidate_ac();
		r = r & 0x0F;
	}
	r_modify(r, 1);
	printf("\tpop r%u\n", r);
}

static void push_rr(unsigned rr)
{
	if (R_ISAC(rr))
		rr = rr & 0x0F;
	printf("\tpush r%u\n", rr + 1);
	printf("\tpush r%u\n", rr);
}

static void pop_rr(unsigned rr)
{
	if (R_ISAC(rr))
		rr = rr & 0x0F;
	r_modify(rr, 2);
	printf("\tpop r%u\n", rr);
	printf("\tpop r%u\n", rr + 1);
}

static void pop_ac(unsigned size)
{
	unsigned r = 4 - size;
	while(size--)
		pop_r(r++);
}

static void push_ac(unsigned size)
{
	switch(size) {
	case 1:
		push_r(R_ACCHAR);
		break;
	case 2:
		push_rr(R_ACINT);
		break;
	case 4:
		push_rr(R_ACINT);
		push_rr(R_ACLONG);
		break;
	default:
		error("psz");
	}
}

static void pop_op(unsigned r, const char *op, unsigned size)
{
	if (R_ISAC(r)) {
		invalidate_ac();
		r = 4 - size;
	}
	while(size--) {
		pop_r(R_WORK);
		r_modify(r, 1);
		printf("\t%s r%u, r%u\n", op, r++, R_WORK);
	}
}

/* Address is on the stack, value to logic it with is in AC */
static void logic_popeq(unsigned size, const char *op)
{
	unsigned n = size;
	pop_rr(R_INDEX);
	while(n) {
		load_r_memr(R_WORK, R_INDEX, 1);
		rr_incw(R_INDEX);
		r_modify(4 - n, 1);
		op_r_r(4 - n, R_WORK, op);
		n--;
	}
	revstore_r_memr(R_AC, R_INDEX, size);
}

static void ret_op(void)
{
	printf("\tret\n");
	unreachable = 1;
}

/* Do a left shift by some means */
static void lshift_r(unsigned r, unsigned size, unsigned l)
{
	unsigned x;
	if (r == R_AC)
		r = 4 - size;
	if (l >= 8 * size) {
		load_r_const(r, 0, size);
		return;
	}
	if (size == 4) {
		if (l >= 24) {
			load_r_r(r, r + 3);
			load_r_const(r, 0, 3);
			l -= 24;
		} else if (l >= 16) {
			load_r_r(r, r + 2);
			load_r_r(r + 1, r + 3);
			load_r_const(r + 2, 0, 2);
			l -= 16;
		} else if (l >= 8) {
			load_r_r(r, r + 1);
			load_r_r(r + 1, r + 2);
			load_r_r(r + 2, r + 3);
			load_r_const(r + 3, 0, 1);
		}
	} else if (size == 2) {
		if (l >= 8) {
			load_r_r(r, r + 1);
			load_r_const(r + 1, 0, 1);
			l -= 8;
		}
	}
	if (l * size > 8) {
		load_r_const(R_WORK, l, 1);
		x = label();
		add_r_r(r, r, size);
		djnz_r(R_WORK, x);
	} else while(l--)
		add_r_r(r, r, size);
}

static void test_sign(unsigned r, unsigned size)
{
	if (R_ISAC(r))
		r = 4 - size;
	printf("\tcp r%u,#0x80\n", r);
}

/*
 *	Register variable helpers
 */

static void load_ac_reg(unsigned r, unsigned size)
{
	r = R_REG(r);
	load_r_r(R_ACCHAR, r + 1);
	if (size == 2)
		load_r_r(R_ACINT, r);
}

static void load_reg_ac(unsigned r, unsigned size)
{
	r = R_REG(r);
	if (size == 2)
		load_r_r(r, R_ACINT);
	load_r_r(r + 1, R_ACCHAR);
}

/*
 *	Object sizes
 */

static unsigned get_size(unsigned t)
{
	if (PTR(t))
		return 2;
	if (t == CSHORT || t == USHORT)
		return 2;
	if (t == CCHAR || t == UCHAR)
		return 1;
	if (t == CLONG || t == ULONG || t == FLOAT)
		return 4;
	if (t == CLONGLONG || t == ULONGLONG || t == DOUBLE)
		return 8;
	if (t == VOID)
		return 0;
	fprintf(stderr, "type %x\n", t);
	error("gs");
	return 0;
}

/* Load helpers. Try and get a value into r (usually 12/13) without messing up ac. May
   trash r14/r15. Set mm if returned reg must match requested */
static unsigned load_direct(unsigned r, struct node *n, unsigned mm)
{
	unsigned size = get_size(n->type);
	unsigned v = n->value;

	if (size > 2)
		return 0;
	switch(n->op) {
	case T_LOCAL:
		load_r_local(r, v);
		return r;
	case T_NAME:
		load_r_name(r, n, v);
		return r;
	case T_LABEL:
		load_r_label(r, n, v);
		return r;
	case T_LREF:
		/* Size shrink for all the op with local in r12/r13 stuff */
		if (r == R_WORK && opt < 1) {
			load_work_helper(v, size);
			return r;
		}
		load_r_local(R_INDEX, v);
		if (size == 1)
			r++;
		load_r_memr(r, R_INDEX, size);
		return r;
	case T_NREF:
		if (optsize) {
			if (size <= 2 && r == R_WORK) {
				printf("\tcall __nref12_%d\n", size);
				/* Until we track r14 objects other than local */
				r_modify(R_WORK + size - 1, size);
				gen_symref(n);
				return 1;
			}
		}
		load_r_name(R_INDEX, n, v);
		if (size == 1)
			r++;
		load_r_memr(r, R_INDEX, size);
		return r;
	case T_LBREF:
		if (optsize) {
			if (size <= 2 && r == R_WORK) {
				printf("\tcall __nref12_%d\n", size);
				/* Until we track r14 objects other than local */
				r_modify(R_WORK + size - 1, size);
				gen_symref(n);
				return 1;
			}
		}
		load_r_label(R_INDEX, n, v);
		if (size == 1)
			r++;
		load_r_memr(r, R_INDEX, size);
		return r;
	case T_CONSTANT:
		/* Load lower half */
		if (size == 1)
			r++;
		load_r_const(r, v, size);
		return r;
	case T_RREF:
		v = R_REG_S(v, size);
		if (mm == 0 || v  == r)
			return v;
		load_r_r(r + 1, v + 1);
		if (size == 2) {
			load_r_r(r, v);
			return r;
		}
		return r + 1;
	}
	return 0;
}

static unsigned logic_eq_direct_r(struct node *r,unsigned v, unsigned size, unsigned op)
{
	if (r->op == T_CONSTANT) {
		/* Could spot 0000 and FFFF but prob no point */
		load_r_r(R_INDEX, 2);
		load_r_r(R_INDEX + 1, 3);
		load_r_memr(R_AC, R_INDEX, size);
		logic_r_const(R_AC, v, size, op);
		revstore_r_memr(R_AC, R_INDEX, size);
		return 1;
	}
	if (r->op == T_RREF) {
		load_r_r(R_INDEX, 2);
		load_r_r(R_INDEX + 1, 3);
		load_r_memr(R_AC, R_INDEX, size);
		logic_r_r(R_AC, R_REG(r->value), size, op);
		revstore_r_memr(R_AC, R_INDEX, size);
		return 1;
	}
#if 0
	/* Cannot do this until we have a load direct 'but don't touch R_INDEX' */
	r1 = load_direct(R_WORK, r, 0);
	if (r1) {
		logic_r_r(R_AC, r1, size, op);
		return 1;
	}
#endif	
	return 0;
}


/* Tree manipulation */

static void squash_node(struct node *n, struct node *o)
{
	n->value = o->value;
	n->val2 = o->val2;
	n->snum = o->snum;
	free_node(o);
}

static void squash_left(struct node *n, unsigned op)
{
	struct node *l = n->left;
	n->op = op;
	squash_node(n, l);
	n->left = NULL;
}

static void squash_right(struct node *n, unsigned op)
{
	struct node *r = n->right;
	n->op = op;
	squash_node(n, r);
	n->right = NULL;
}

/*
 *	There isn't a lot we can do the easy way except constants, so stick
 *	constants on the right when we can.
 */
static unsigned is_simple(struct node *n)
{
	unsigned op = n->op;

	/* We can load these directly */
	if (op == T_CONSTANT)
		return 1;
	return 0;
}

/* Chance to rewrite the tree from the top rather than none by node
   upwards. We will use this for 8bit ops at some point and for cconly
   propagation */
struct node *gen_rewrite(struct node *n)
{
	return n;
}

/*
 *	Our chance to do tree rewriting. We don't do much for the 8080
 *	at this point, but we do rewrite name references and function calls
 */
struct node *gen_rewrite_node(struct node *n)
{
	struct node *l = n->left;
	struct node *r = n->right;
	unsigned op = n->op;
	unsigned nt = n->type;

	/* TODO
		- rewrite some reg ops
	*/

	/* BUG - these two break utol test */
	/* Structure field references from locals. These end up big on the Z8 so use
	   a helper for the lot */
	if (optsize && 0 && op == T_DEREF && r->op == T_PLUS && r->right->op == T_CONSTANT) {
		/* For now just do lrefs of offsets within 256 bytes */
		if (r->left->op == T_LREF && r->left->value < 256) {
			n->op = T_LSTREF;
			n->value = r->right->value;
			n->val2 = r->left->value;
			n->left = NULL;
			n->right = NULL;
			free_node(r->right);
			free_node(r->left);
			free_node(r);
			return n;
		}
	}
	/* Structure field assign - same idea */
	if (optsize && 0 && op == T_EQ && l->op == T_PLUS && l->right->op == T_CONSTANT) {
		/* Same restrictions */
		if (l->left->op == T_LSTORE && l->left->value < 256) {
			n->op = T_LSTSTORE;
			n->value = l->right->value;
			n->val2 = l->left->value;
			n->left = NULL;
			free_node(l->right);
			free_node(l->left);
			free_node(l);
			return n;
		}
	}
	/* regptr++  The size will always be the true size for ++ and const */
	if (op == T_DEREF && r->op == T_PLUSPLUS && r->left->op == T_REG)
	{
		n->op = T_RDEREFPLUS;
		n->value = r->left->value;
		free_node(r->left);
		free_node(r->right);
		free_node(r);
		n->right = NULL;
		return n;
	}
	/* *regptr++ =  again the size will be const and right */
	if (op == T_EQ && l->op == T_PLUSPLUS && l->left->op == T_REG)
	{
		n->op = T_REQPLUS;
		n->value = l->left->value;
		free_node(l->left);
		free_node(l->right);
		free_node(l);
		n->left = NULL;
		return n;
	}
	/* *(reg + offset). Optimize this specially as it occurs a lot */
	if (op == T_DEREF && r->op == T_PLUS && r->left->op == T_RREF &&
		r->right->op == T_CONSTANT) {
		n->op = T_RDEREF;
		n->right = NULL;
		n->val2 = r->right->value;	/* Offset to add */
		n->value = r->left->value;	/* Register number */
		free_node(r->right);		/* Discard constant */
		free_node(r->left);		/* Discard T_REG */
		free_node(r);			/* Discsrd plus */
		return n;
	}
	/* *regptr */
	if (op == T_DEREF && r->op == T_RREF) {
		n->op = T_RDEREF;
		n->right = NULL;
		n->val2 = 0;
		n->value = r->value;
		free_node(r);
		return n;
	}
	/* *(reg + offset) =  Optimize this specially as it occurs a lot */
	if (op == T_EQ && l->op == T_PLUS && l->left->op == T_RREF &&
		l->right->op == T_CONSTANT) {
		n->op = T_REQ;
		n->val2 = l->right->value;	/* Offset to add */
		n->value = l->left->value;	/* Register number */
		free_node(l->right);		/* Discard constant */
		free_node(l->left);		/* Discard T_REG */
		free_node(l);			/* Discsrd plus */
		n->left = NULL;
		return n;
	}
	/* *regptr = */
	if (op == T_EQ && l->op == T_RREF) {
		n->op = T_REQ;
		n->val2 = 0;
		n->value = l->value;
		n->left = NULL;
		free_node(l);
		return n;
	}
	/* Rewrite references into a load operation */
	else if (op == T_DEREF) {
		if (r->op == T_LOCAL || r->op == T_ARGUMENT) {
			if (r->op == T_ARGUMENT)
				r->value += argbase + frame_len;
			squash_right(n, T_LREF);
			return n;
		}
		if (r->op == T_REG) {
			squash_right(n, T_RREF);
			return n;
		}
		if (r->op == T_NAME) {
			squash_right(n, T_NREF);
			return n;
		}
		if (r->op == T_LABEL) {
			squash_right(n, T_LBREF);
			return n;
		}
	}
	else if (op == T_EQ) {
		if (l->op == T_NAME) {
			squash_left(n, T_NSTORE);
			return n;
		}
		if (l->op == T_LABEL) {
			squash_left(n, T_LBSTORE);
			return n;
		}
		if (l->op == T_LOCAL || l->op == T_ARGUMENT) {
			if (l->op == T_ARGUMENT)
				l->value += argbase + frame_len;
			squash_left(n, T_LSTORE);
			return n;
		}
		if (l->op == T_REG) {
			squash_left(n, T_RSTORE);
			return n;
		}
	}
	/* Eliminate casts for sign, pointer conversion or same */
	else if (op == T_CAST) {
		if (nt == r->type || (nt ^ r->type) == UNSIGNED ||
		 (PTR(nt) && PTR(r->type))) {
			free_node(n);
			return r;
		}
	}
	/* Rewrite function call of a name into a new node so we can
	   turn it easily into call xyz */
	else if (op == T_FUNCCALL && r->op == T_NAME && PTR(r->type) == 1) {
		n->op = T_CALLNAME;
		n->snum = r->snum;
		n->value = r->value;
		free_node(r);
		n->right = NULL;
	}
	/* Commutive operations. We can swap the sides over on these */
	if (op == T_AND || op == T_OR || op == T_HAT || op == T_STAR || op == T_PLUS) {
/*		printf(";left %d right %d\n", is_simple(n->left), is_simple(n->right)); */
		if (is_simple(n->left) > is_simple(n->right)) {
			n->right = l;
			n->left = r;
		}
	}
	return n;
}

/* Export the C symbol */
void gen_export(const char *name)
{
	printf("	.export _%s\n", name);
}

void gen_segment(unsigned segment)
{
	switch(segment) {
	case A_CODE:
		printf("\t.%s\n", codeseg);
		break;
	case A_DATA:
		printf("\t.data\n");
		break;
	case A_BSS:
		printf("\t.bss\n");
		break;
	case A_LITERAL:
		printf("\t.literal\n");
		break;
	default:
		error("gseg");
	}
}

/* Generate the function prologue - may want to defer this until
   gen_frame for the most part */
void gen_prologue(const char *name)
{
	unreachable = 0;
	printf("_%s:\n", name);
	invalidate_all();
}

/* Generate the stack frame */
/* TODO: defer this to statements so we can ld/push initializers */
void gen_frame(unsigned size, unsigned aframe)
{
	unsigned r;
	frame_len = size;
	sp = 0;

	argbase = ARGBASE;
	
	for (r = 1; r <= 4; r++) {
		if (func_flags & F_REG(r)) {
			push_rr(R_REG(r));
			argbase += 2;
		}
	}

	r14_valid = 0;
	r2_valid = 0;

	if (size > 3 && opt < 1 && size < 256) {
		printf("\tld r13,#%u\n", size & 0xFF);
		printf("\tcall __frame\n");
		return;
	}
	if (size > 4) {
		/* Special handling because of the stack and interrupts */
		load_r_R(R_INDEX, R_SPL);
		/* TODO: will need special tracking of course */
		printf("\tsub r%u,#%u\n", R_INDEX, size & 0xFF);
		printf("\tsbc 254,#%u\n", size >> 8);
		load_R_r(R_SPL, R_INDEX);
		return;
	}
	while(size--)
		RR_decw(R_SPH);
}

void gen_exitjp(unsigned size)
{
	if (size == 0)
		ret_op();
	else if (size > 255) {
		load_r_const(R_WORK, size, 2);
		printf("\tjp __cleanup\n");
	} else if (size > 8) {
		load_r_const(R_WORK + 1, size, 1);
		printf("\tjp __cleanupb\n");
	} else
		printf("\tjp __cleanup%u\n", size);
	unreachable = 1;
}

void gen_epilogue(unsigned size, unsigned argsize)
{
	unsigned r;

	if (sp != 0)
		error("sp");
	if (unreachable)
		return;

	add_R_const(R_SPH, size, 2);
	for (r = 4; r >= 1; r--) {
		if (func_flags & F_REG(r)) {
			pop_rr(R_REG(r));
		}
	}
	if (!(func_flags & F_VARARG)) {
		gen_exitjp(argsize);
		return;
	}
	ret_op();
}

void gen_label(const char *tail, unsigned n)
{
	unreachable = 0;
	/* A branch label means the state is unknown so force any
	   existing state and don't assume anything */
	invalidate_all();
	printf("L%u%s:\n", n, tail);
}

/* A return statement. We can sometimes shortcut this if we have
   no cleanup to do */
unsigned gen_exit(const char *tail, unsigned n)
{
	gen_jump(tail, n);
	return 0;
}

/* FIXME: teach assembler to adjust jr and make these use jr */
void gen_jump(const char *tail, unsigned n)
{
	/* Force anything deferred to complete before the jump */
	flush_all(0);
	printf("\tjr L%u%s\n", n, tail);
	unreachable = 1;
}

/* TODO: Will need work when we implement flag flipping and other compare tricks */
void gen_jfalse(const char *tail, unsigned n)
{
	flush_all(1);	/* Must preserve flags */
	printf("\tjr z,L%u%s\n", n, tail);
}

void gen_jtrue(const char *tail, unsigned n)
{
	flush_all(1);	/* Must preserve flags */
	printf("\tjr nz,L%u%s\n", n, tail);
}

static void gen_cleanup(unsigned v, unsigned vararg)
{
	sp -= v;
	if (vararg) {
		if (v >= 4) {
			add_R_const(R_SPH, v, 2);
			return;
		}
		while(v--)
			RR_incw(R_SPH);
	}
}

/*
 *	Helper handlers. We use a tight format for integers but C
 *	style for float as we'll have C coded float support if any
 */

/* True if the helper is to be called C style */
static unsigned c_style(struct node *np)
{
	register struct node *n = np;
	/* Assignment is done asm style */
	if (n->op == T_EQ || n->op == T_LSTREF || n->op == T_LSTSTORE)
		return 0;
	/* Float ops otherwise are C style */
	if (n->type == FLOAT)
		return 1;
	n = n->right;
	if (n && n->type == FLOAT)
		return 1;
	return 0;
}

void gen_helpcall(struct node *n)
{
	/* Check both N and right because we handle casts to/from float in
	   C call format */
	if (c_style(n))
		gen_push(n->right);
	invalidate_ac();
	printf("\tcall __");
	r_modify(0, 4);
	r_modify(12,4);
}

/* Generate a helper that keeps r14/r15 */
void gen_keepcall(struct node *n)
{
	/* Check both N and right because we handle casts to/from float in
	   C call format */
	if (c_style(n))
		gen_push(n->right);
	invalidate_ac();
	printf("\tcall __");
	r2_valid = 0;
}

void gen_helpclean(struct node *n)
{
	unsigned s;

	if (c_style(n)) {
		s = 0;
		if (n->left) {
			s += get_size(n->left->type);
			/* gen_node already accounted for removing this thinking
			   the helper did the work, adjust it back as we didn't */
			sp += s;
		}
		s += get_size(n->right->type);
		/* No helper uses varargs */
		gen_cleanup(s, 0);
		/* C style ops that are ISBOOL didn't set the bool flags */
		if (n->flags & ISBOOL)
			printf("\tor r3,r2\n");
		r_modify(2, 2);
	}
}

void gen_switch(unsigned n, unsigned type)
{
	/* TODO: this probably belongs as a routine in the cpu bits */
	printf("\tld r%u, #>Sw%u\n", R_INDEX, n);
	printf("\tld r%u, #<Sw%u\n", R_INDEX + 1, n);
	r_modify(R_INDEX, 2);
	/* TODO: tracking fixes for R14/15 when tracking added ??*/
	invalidate_all();
	/* Nothing is preserved over a switch but */
	printf("\tjp __switch");
	helper_type(type, 0);
	putchar('\n');
}

void gen_switchdata(unsigned n, unsigned size)
{
	printf("Sw%u:\n\t.word %u\n", n, size);
}

void gen_case_label(unsigned tag, unsigned entry)
{
	unreachable = 0;
	invalidate_all();
	printf("Sw%u_%u:\n", tag, entry);
}

void gen_case_data(unsigned tag, unsigned entry)
{
	printf("\t.word Sw%u_%u\n", tag, entry);
}

void gen_data_label(const char *name, unsigned align)
{
	printf("_%s:\n", name);
}

void gen_space(unsigned value)
{
	printf("\t.ds %u\n", value);
}

void gen_text_data(unsigned n)
{
	printf("\t.word T%u\n", n);
}

/* The label for a literal (currently only strings) */
void gen_literal(unsigned n)
{
	if (n)
		printf("T%u:\n", n);
}

void gen_name(struct node *n)
{
	printf("\t.word _%s+%u\n", namestr(n->snum), WORD(n->value));
}

void gen_value(unsigned type, unsigned long value)
{
	unsigned w = WORD(value);
	if (PTR(type)) {
		printf("\t.word %u\n", w);
		return;
	}
	switch (type) {
	case CCHAR:
	case UCHAR:
		printf("\t.byte %u\n", BYTE(w));
		break;
	case CSHORT:
	case USHORT:
		printf("\t.word %u\n", w);
		break;
	case CLONG:
	case ULONG:
	case FLOAT:
		/* We are big endian */
		printf("\t.word %u\n", (unsigned) ((value >> 16) & 0xFFFF));
		printf("\t.word %u\n", w);
		break;
	default:
		error("unsuported type");
	}
}

void gen_start(void)
{
/*	printf("\t.setcpu %u\n", cpu); */
}

void gen_end(void)
{
	flush_all(0);
}

void gen_tree(struct node *n)
{
	codegen_lr(n);
	printf(";\n");
/*	printf(";SP=%d\n", sp); */
}

static unsigned can_fast_mul(unsigned s, unsigned n)
{
	/* Fairly primitive for now */
	if (n < 2)
		return 1;
	/* For now only support powers of 2 */
	if (n & (n-1))
		return 0;
	return 1;
}

/* Input must be a power of 2 */
static unsigned ilog2w(unsigned n)
{
	unsigned r = 0;
	if (n & 0xFF00) {
		r = 8;
		n >>= 8;
	}
	if (n & 0xF0) {
		r += 4;
		n >>= 4;
	}
	if (n & 0x0C) {
		r += 2;
		n >>= 2;
	}
	if (n & 0x02) {
		r++;
		n >>= 1;
	}
	return r;
}

static unsigned ilog2(unsigned long n)
{
	if (n & 0xFFFF0000)
		return 16 + ilog2w(n >> 16);
	return ilog2w(n);
}

static void gen_fast_mul(unsigned r, unsigned s, unsigned long n)
{
	unsigned l = ilog2(n);
	if (l == 0)
		load_r_const(r, 0, s);
	else
		lshift_r(r, s, l);
}

static unsigned gen_fast_div(unsigned r, unsigned s, unsigned long n)
{
	if (n & (n - 1))
		return 0;
	rshift_r(r, s, ilog2(n), 1);
	return 1;
}

static unsigned gen_fast_udiv(unsigned r, unsigned s, unsigned long n)
{
	if (n == 1)
		return 1;
	/* Move all this into a single rshift helper */
	if (n == 256 && s == 2) {
		load_r_r(r, r + 1);
		load_r_constb(r + 1, 0);
		return 1;
	}
	if (n & (n - 1))
		return 0;
	rshift_r(r, s, ilog2(n), 0);
	return 0;
}

static unsigned gen_fast_remainder(unsigned r, unsigned s, unsigned long n)
{
	if (n == 1) {
		load_r_const(r, 0, s);
		return 1;
	}
	if (n & (n - 1))
		return 0;
	logic_r_const(r, n - 1, s, OP_AND);
	return 1;
}

/*
 *	If possible turn this node into a direct access. We've already checked
 *	that the right hand side is suitable. If this returns 0 it will instead
 *	fall back to doing it stack based.
 *
 *	On entry the working value in in r0-r3, and the other value is
 *	not yet resolved. Typically this is useful for stuff like a constant
 *	load, but we can do a lot of reg/reg ops this way.
 */
unsigned gen_direct(struct node *n)
{
	unsigned size = get_size(n->type);
	struct node *r = n->right;
	unsigned long v;
	unsigned nr = n->flags & NORETURN;
	unsigned u = n->type & UNSIGNED;
	unsigned r1;

	/* We only deal with simple cases for now */
	if (r)
		v = r->value;

	/* We can do a lot of stuff because we have r14/15 as a scratch */
	switch (n->op) {
	case T_CLEANUP:
		gen_cleanup(v, n->val2);
		return 1;
	case T_NSTORE:
		load_r_name(R_INDEX, n, v);
		store_r_memr(R_AC, R_INDEX, size);
		break;
	case T_LBSTORE:
		load_r_label(R_INDEX, n, v);
		store_r_memr(R_AC, R_INDEX, size);
		return 1;
	case T_LSTORE:
		if (opt < 1)
			store_local_helper(r, v + sp, size);
		else {
			load_r_local(R_INDEX, v + sp);
			store_r_memr(R_AC, R_INDEX, size);
			set_ac_node(n);
		}
		return 1;
	case T_RSTORE:
		v = R_REG(n->value);
		load_r_r(v + 1, R_ACCHAR);
		if (size > 1)
			load_r_r(v, R_ACINT);
		return 1;
	case T_EQ:
		/* We may be able to do better with thought */
		if (r->op == T_CONSTANT) {
			load_r_r(R_INDEX, R_ACPTR);
			load_r_r(R_INDEX + 1, R_ACPTR + 1);
			load_r_const(R_AC, v, size);
			store_r_memr(R_AC, R_INDEX, size);
			return 1;
		}
		/* Can we load the right without touching AC, and do we need the result */
		if (nr && (r1 = load_direct(R_WORK, r, 0)) != 0) {
			/* r1 is the data, R_AC the ptr */
			store_r_memr(r1, R_AC, size);
			return 1;
		}
		return 0;
	/* Some of these would benefit from helpers using r1r/r15 r0-r3 or
	   similar. FIXME : will need to rework the repeated_ stuff when we
	   do tracking into loops of rr_dec etc */
	/* TODO: this code magically knows load_direct will not work on a float. FIXME */
	case T_PLUS:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			add_r_const(R_AC, v, size);
			return 1;
		}
		/* Special case array references with a * 2 in them. In particular
		   the useful case of array[register] can be turned into four adds instead of
		   a shuffle around */
		if (r->op == T_STAR) {
			if (r->right->op == T_CONSTANT && r->right->value == 2) {
				r1 = load_direct(R_WORK, r->left, 0);
				if (r1 == 0)
					return 0;
				add_r_r(R_AC, r1, size);
				add_r_r(R_AC, r1, size);
				return 1;
			}
		}
		if ((r1 = load_direct(R_WORK, r, 0)) != 0) {
			add_r_r(R_AC, r1, size);
			return 1;
		}
		return 0;
	case T_MINUS:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			add_r_const(R_AC, -v, size);
			return 1;
		}
		if ((r1 = load_direct(R_WORK, r, 0)) != 0) {
			sub_r_r(R_AC, r1, size);
			return 1;
		}
		return 0;
	case T_STAR:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			if (size <= 2 && can_fast_mul(size, v)) {
				gen_fast_mul(R_AC, size, v);
				return 1;
			}
		}
		return 0;
	case T_SLASH:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			if (n->type & UNSIGNED) {
				if (gen_fast_udiv(R_AC, size, v))
					return 1;
			} else {
				if (gen_fast_div(R_AC, size, v))
					return 1;
			}
		}
		return 0;
	case T_PERCENT:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			if (n->type & UNSIGNED) {
				if (gen_fast_remainder(R_AC, size, v))
					return 1;
			}
		}
		return 0;
	case T_AND:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT) {
			logic_r_const(R_AC, v, size, OP_AND);
			return 1;
		}
		if ((r1 = load_direct(R_WORK, r, 0)) != 0) {
			logic_r_r(R_AC, r1, size, OP_AND);
			return 1;
		}
		return 0;
	case T_OR:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT) {
			logic_r_const(R_AC, v, size, OP_OR);
			return 1;
		}
		if ((r1 = load_direct(R_WORK, r, 0)) != 0) {
			logic_r_r(R_AC, r1, size, OP_OR);
			return 1;
		}
		return 0;
	case T_HAT:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT) {
			logic_r_const(R_AC, v, size, OP_XOR);
			return 1;
		}
		if ((r1 = load_direct(R_WORK, r, 0)) != 0) {
			logic_r_r(R_AC, r1, size, OP_XOR);
			return 1;
		}
		return 0;
	/* TODO: inline reg compares, also look at reg on left compare optimizations later */
	case T_EQEQ:
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			if (v == 0)
				helper(n, "cceqconst0");
			else {
				load_r_const(R_WORK, v , size);
				helper(n, "cceqconst");
			}
			n->flags |= ISBOOL;
			return 1;
		}
		r1 = load_direct(R_WORK, r, 1);
		if (r1) {
			helper(n, "cceqconst");
			n->flags |= ISBOOL;
			return 1;
		}
		return 0;
	/* The const form helpers do the reverse compare so we use the opposite one */
	case T_GTEQ:
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			/* Quick way to do the classic signed >= 0 */
			if (v == 0 && !u) {
				test_sign(R_AC, size);
				load_r_const(R_ACINT, 0, 2);
				op_r_c(3, 0, "adc");
				n->flags |= ISBOOL;
				return 1;
			}
			if (v == 0)
				helper_s(n, "cclteqconst0");
			else {
				load_r_const(R_WORK, v , size);
				helper_s(n, "cclteqconst");
			}
			n->flags |= ISBOOL;
			return 1;
		}
		r1 = load_direct(R_WORK, r, 1);
		if (r1) {
			helper_s(n, "cclteqconst");
			n->flags |= ISBOOL;
			return 1;
		}
		return 0;
	case T_GT:
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			if (v == 0)
				helper_s(n, "ccltconst0");
			else {
				load_r_const(R_WORK, v , size);
				helper_s(n, "ccltconst");
			}
			n->flags |= ISBOOL;
			return 1;
		}
		r1 = load_direct(R_WORK, r, 1);
		if (r1) {
			helper_s(n, "ccltconst");
			n->flags |= ISBOOL;
			return 1;
		}
		return 0;
	case T_LTEQ:
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			if (v == 0)
				helper_s(n, "ccgteqconst0");
			else {
				load_r_const(R_WORK, v , size);
				helper_s(n, "ccgteqconst");
			}
			n->flags |= ISBOOL;
			return 1;
		}
		r1 = load_direct(R_WORK, r, 1);
		if (r1) {
			helper_s(n, "ccgteqconst");
			n->flags |= ISBOOL;
			return 1;
		}
		return 0;
	case T_LT:
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			/* Quick way to do the classic signed < 0 */
			if (v == 0 && !u) {
				/* FIXME: tied to accumulator proper atm */
				test_sign(R_AC, size);
				load_r_const(R_ACINT, 0, 2);
				printf("\tccf\n");
				op_r_c(3, 0, "adc");
				n->flags |= ISBOOL;
				return 1;
			}
			if (v == 0)
				helper_s(n, "ccgtconst0");
			else {
				load_r_const(R_WORK, v , size);
				helper_s(n, "ccgtconst");
			}
			n->flags |= ISBOOL;
			return 1;
		}
		r1 = load_direct(R_WORK, r, 1);
		if (r1) {
			helper_s(n, "ccgtconst");
			n->flags |= ISBOOL;
			return 1;
		}
		return 0;
	case T_BANGEQ:
		if (r->op == T_CONSTANT && n->type != FLOAT) {
			load_r_const(R_WORK, v , size);
			if (v == 0)
				helper(n, "ccneconst0");
			else
				helper(n, "ccneconst");
			n->flags |= ISBOOL;
			return 1;
		}
		r1 = load_direct(R_WORK, r, 1);
		if (r1) {
			helper(n, "ccneconst");
			n->flags |= ISBOOL;
			return 1;
		}
		return 0;
	/* TODO: a lot of these could use direct versions */
	case T_LTLT:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT) {
			lshift_r(R_AC, size, v);
			return 1;
		}
		return 0;
	case T_GTGT:
		if (nr)
			return 1;
		if (r->op == T_CONSTANT) {
			rshift_r(R_AC, size, v, n->type & UNSIGNED);
			return 1;
		}
		return 0;
	/* Shorten post inc/dec if result not needed - in which case it's the same as
	   pre inc/dec */
	/* Options here to improve thing += reg etc */
	case T_PLUSPLUS:
		if (r->type == T_FLOAT)
			return 0;
		if (optsize) {
			/* AC is at this point the address and r is the value */
			load_r_const(R_WORK, v, size);
			helper(n, "plusplus");
			return 1;
		}
		if (!nr) {
			/* r2/3 is the pointer,  */
			/* Move it to be safe from the load */
			load_r_r(R_INDEX, R_ACPTR);
			load_r_r(R_INDEX + 1, R_ACPTR + 1);
			load_r_memr(R_AC, R_INDEX, size);
			/* load_r_memr bumps r_index up */
			push_ac(size);
			add_r_const(R_AC, v, size);
			/* Revstore compensates by doing the store
			   bacwards */
			revstore_r_memr(R_AC, R_INDEX, size);
			pop_ac(size);
			return 1;
		}
		/* Noreturn is like pluseq */
	case T_PLUSEQ:
		if (r->type == FLOAT)
			return 0;
		if (optsize) {
			/* TODO: onls mashes r0/r1 for most sizes */
			/* At this point 2,3 holds the pointer */
			if (load_direct(R_WORK, r, 1)) {
				helper(n, "cpluseq");
				return 1;
			}
		}
		/* FIXME: will need an "and not register" check */
		if (r->op == T_CONSTANT) {
			if ((n->flags & NORETURN)  && size <= 2) {
				load_r_memr(0, R_ACPTR, size);
				add_r_const(0, v, size);
				revstore_r_memr(0, R_ACPTR, size);
				return 1;
			}
			/* Copy the pointer over but keep R14/15 */
			load_r_r(R_WORK, 2);
			load_r_r(R_WORK + 1, 3);
			/* Value into 2/3 */
			load_r_memr(R_AC, R_WORK, size);
			add_r_const(R_AC, v, size);
			revstore_r_memr(R_AC, R_WORK, size);
			return 1;
		}
		if (size <= 2 && load_direct(R_WORK, r, 1)) {
			load_r_memr(0, R_ACPTR, size);
			add_r_r(0, R_WORK, size);
			revstore_r_memr(0, R_ACPTR, size);
			if (size == 2) {
				load_r_r(R_ACINT, 0);
				load_r_r(R_ACINT + 1, 1);
			} else {
				load_r_r(R_ACCHAR, 0);
			}
			return 1;
		}
		return 0;
	case T_MINUSMINUS:
		if (r->type == FLOAT)
			return 0;
		if (optsize) {
			/* AC is at this point the address and r is the value */
			load_r_const(R_WORK, -v, size);
			helper(n, "plusplus");
			return 1;
		}
		if (!nr) {
			/* r2/3 is the pointer,  */
			load_r_r(R_INDEX, R_ACPTR);
			load_r_r(R_INDEX + 1, R_ACPTR + 1);
			load_r_memr(R_AC, R_INDEX, size);
			push_ac(size);
			add_r_const(0, -v, size);
			revstore_r_memr(0, 2, size);
			pop_ac(size);
			return 1;
		}
		/* Noreturn is like minuseq */
	case T_MINUSEQ:
		if (r->type == FLOAT)
			return 0;
		if (optsize) {
			/* At this point 2,3 holds the pointer */
			if (load_direct(R_WORK, r, 1)) {
				helper(n, "cminuseq");
				return 1;
			}
		}
		/* FIXME: will need an "and not register" check */
		if (r->op == T_CONSTANT) {
			if ((n->flags & NORETURN)  && size <= 2) {
				load_r_memr(0, R_ACPTR, size);
				sub_r_const(0, v, size);
				revstore_r_memr(0, R_ACPTR, size);
				return 1;
			}
			/* Copy the pointer over but keep R14/15 */
			load_r_r(R_WORK, 2);
			load_r_r(R_WORK + 1, 3);
			/* Value into 2/3 */
			load_r_memr(R_AC, R_WORK, size);
			sub_r_const(R_AC, v, size);
			revstore_r_memr(R_AC, R_WORK, size);
			return 1;
		}
		if (size <= 2 && load_direct(R_WORK, r, 1)) {
			load_r_memr(0, R_ACPTR, size);
			sub_r_r(0, R_WORK, size);
			revstore_r_memr(0, R_ACPTR, size);
			if (size == 2) {
				load_r_r(R_ACINT, 0);
				load_r_r(R_ACINT + 1, 1);
			} else {
				load_r_r(R_ACCHAR, 0);
			}
			return 1;
		}
		return 0;
	case T_ANDEQ:
		return logic_eq_direct_r(r, v, size, OP_AND);
	case T_OREQ:
		return logic_eq_direct_r(r, v, size, OP_OR);
	case T_HATEQ:
		return logic_eq_direct_r(r, v, size, OP_XOR);
	/* Should do SHLEQ/SHREQ of const or r */
	}
	return 0;
}

/*
 *	Allow the code generator to shortcut the generation of the argument
 *	of a single argument operator (for example to shortcut constant cases
 *	or simple name loads that can be done better directly)
 */
unsigned gen_uni_direct(struct node *n)
{
	return 0;
}

/*
 *	Helpers for stacking things to try and reduce the very
 *	expensive call/return space cost.
 */
static unsigned argstack_helper(struct node *n, unsigned sz)
{
	unsigned v = n->value;
	if (n->op == T_CONSTANT) {
		if (sz <= 2) {
			if (v < 2) {
				r_set(3, v);
				r_set(2, v >> 8);
				r_modify(12, 2);
				printf("\tcall __push%u\n", (unsigned)n->value);
				return 1;
			}
			return 0;
		}
		if (n->value == 0) {
			/* This clears r0/r1 */
			r_set(0, 0);
			r_set(1, 0);
			r_modify(12, 2);
			printf("\tcall __pushl0\n");
			return 1;
		}
		if (!(n->value & 0xFFFF0000UL)) {
			load_r_const(R_AC, n->value, 2);
			r_set(0, 0);
			r_set(1, 0);
			r_modify(12, 2);
			printf("\tcall __pushl0a\n");
			return 1;
		}
		/* is it worth using __pushl for anything evaluated ? */
	}
	/* Push a local argument */
	if (n->op == T_LREF && n->value + sp < 254) {
		load_r_constb(R_INDEX + 1, n->value + sp + 2);
		r_modify(12, 4);
		r_modify(R_AC, sz);
		if (sz == 2)
			printf("\tcall __pushln\n");
		else
			printf("\tcall __pushlnl\n");
		return 1;		
	}
	return 0;
}

/* Given a node see if we can generate a short form for it */
static void argstack(struct node *n)
{
	unsigned sz = get_size(n->type);
	unsigned r = R_AC + 4;

	if (0 && optsize && argstack_helper(n, sz)) {
		sp += sz;
		return;
	}
	/* Generate the node */
	codegen_lr(n);
	/* And stack it */
	/* TODO optsize case for long call __pushl (mods 12/13) */
	sp += sz;
	while(sz--)
		push_r(--r);
}

/* We handle function arguments specially as we both push them to a different
   stack and optimize them. The tree looks like this

                 FUNCCALL|CALLNAME
                  /
	       ARGCOMMA
	        /    \
	      ...    EXPR
	      /
	    ARG
 */
static void gen_fcall(struct node *n)
{
	if (n == NULL)
		return;
	if (n->op == T_ARGCOMMA) {
		/* Recurse down arguments and stack then on the way up */
		gen_fcall(n->left);
		argstack(n->right);
	} else {
		argstack(n);
	}
	/* Final node done */
}

/*
 *	Allow the code generator to short cut any subtrees it can directly
 *	generate.
 */
unsigned gen_shortcut(struct node *n)
{
	unsigned size = get_size(n->type);
	struct node *l = n->left;
	struct node *r = n->right;
	unsigned nr = n->flags & NORETURN;
	unsigned x;

	/* Unreachable code we can shortcut into nothing ..bye.. */
	if (unreachable)
		return 1;

	/* The comma operator discards the result of the left side, then
	   evaluates the right. Avoid pushing/popping and generating stuff
	   that is surplus */
	if (n->op == T_COMMA) {
		l->flags |= NORETURN;
		codegen_lr(l);
		/* Parent determines child node requirements */
		r->flags |= nr;
		codegen_lr(r);
		return 1;
	}
	/* We should never meet an ARGCOMMA as they are handled by
	   gen_fcall */
	if (n->op == T_ARGCOMMA) {
		fprintf(stderr, "argcomma?\n");
	}
	if (n->op == T_FUNCCALL) {
		/* Generate and stack all the arguments */
		gen_fcall(l);
		/* Generate the address of the function */
		codegen_lr(r);
		invalidate_all();
		/* Rather than mess with indirection use a helper */
		printf("\tcall __jmpr2\n");
		return 1;
	}
	if (n->op == T_CALLNAME) {
		gen_fcall(l);
		invalidate_all();
		printf("\tcall _%s+%d\n", namestr(n->snum), (unsigned)n->value);
		return 1;
	}
	/* We don't know if the result has set the condition flags
	 * until we generate the subtree. So generate the tree, then
	 * either do nice things or use the helper */
	if (n->op == T_BOOL) {
		codegen_lr(r);
		if (r->flags & ISBOOL)
			return 1;
		size = get_size(r->type);
		if (size <= 2 && (n->flags & CCONLY)) {
			if (size == 2) {
				printf("\tor r3,r2\n");
				r_modify(3, 1);
			} else
				printf("\tor r3,r3\n");
			return 1;
		}
		/* Too big or value needed */
		helper(n, "bool");
		n->flags |= ISBOOL;
		return 1;
	}
	/* A common pattern is a ++ or -- on a local. That gets really convoluted on the Z8, so
	   handle it specially */
	if (optsize && n->op == T_PLUSPLUS && l->op == T_LREF && l->value + sp + size - 1 < 254) {
		/* Set the pointer to the last byte */
		load_r_const(R_INDEX + 1, l->value + sp + 2 + size - 1, 1);
		if (r->value == 1)
			helper(n, "lplusplus1");
		else if (r->value < 256) {
			load_r_const(R_ACCHAR, r->value, 1);
			helper(n, "lplusplusb");
		} else {
			load_r_const(R_AC, r->value, size);
			helper(n, "lplusplus");
		}
		return 1;
	}
	if (optsize && n->op == T_MINUSMINUS && l->op == T_LREF && l->value + sp + size - 1 < 254) {
		load_r_const(R_INDEX + 1, l->value + sp + 2 + size - 1, 1);
		load_r_const(R_AC, -r->value, size);
		helper(n, "lplusplus");
		return 1;
	}

	/* Assign to struct field with pointer in local */
	if (n->op == T_LSTSTORE) {
		/* Get thje value to assign into the working registers */
		gen_node(n->right);
		/* Now store t */
		if (n->val2 + sp < 254) {
			load_r_const(R_INDEX + 1, n->val2 + sp + 2, 1);
			load_r_const(R_WORK + 1, n->value, 1);
			helper(n, "lststore0");
		} else {
			load_r_const(R_INDEX, n->val2 + sp + 2, 2);
			load_r_const(R_WORK + 1, n->value, 1);
			helper(n, "lststore");
		}
		return 1;
	}
	/* TODO: Do PLUSEQ/MINUSEQ for CONST non FLOAT akin to above */
	if (n->op == T_RSTORE && (n->flags & NORETURN))
		return load_direct(R_REG(n->value), r, 1);

	/* Squash op for optimization */
	if (n->op == T_REQPLUS) {
		/* *reg++ = r */
		codegen_lr(r);	/* AC now holds the value */
		store_r_memr(R_AC, R_REG(n->value), size);	/* Moves the pointer on as a side effect */
		rr_incw(R_REG(n->value));
		return 1;
	}
	if (n->op == T_REQ) {
		/* *reg = r */
		codegen_lr(r);	/* AC now holds the value */
		load_r_r(R_INDEX, R_REG(n->value));
		load_r_r(R_INDEX + 1, R_REG(n->value) + 1);
		add_r_const(R_INDEX, n->val2, 2);
		store_r_memr(R_AC, R_INDEX, size);	/* Moves the pointer on as a side effect */
		return 1;
	}
	if (optsize && (n->op == T_NSTORE || n->op == T_LBSTORE) && r->op == T_CONSTANT) {
		if (r->value == 0) {
			printf("\tcall __nstore_%d_0\n", size);
			set_ac_node(n);
			r_modify(R_WORK,4);
			gen_symref(n);
			return 1;
		}
		if (r->value < 256 && size > 1) {
			load_r_const(R_ACCHAR, r->value, 1);
			printf("\tcall __nstore_%db\n", size);
			set_ac_node(n);
			r_modify(R_WORK,4);
			gen_symref(n);
			return 1;
		}			
	}
	/* Handle operations on registers as they have no address so cannot be resolved as
	   a subtree with an addr in it */
	if (l && l->op == T_REG) {
		unsigned reg = l->value;
		unsigned v = r->value;
		/* Never a long... */
		switch(n->op) {
		case T_EQ:
			if (load_direct(R_REG(reg), r, 1) == 0)
				return 0;
			if (!nr)
				load_ac_reg(reg, size);
			return 1;
		case T_PLUSPLUS:
			/* v is always a const */
			if (!nr)
				load_ac_reg(reg, size);
			add_r_const(R_REG_S(reg, size), v, size);
			return 1;
		case T_MINUSMINUS:
			printf(";reg minusminus size %u reg %u R_REG() = %u\n",
				size, reg, R_REG(reg));
			if (!nr)
				load_ac_reg(reg, size);
			sub_r_const(R_REG_S(reg, size), v, size);
			return 1;
		case T_PLUSEQ:
			/* TODO : reg/const and reg/direct versions */
			/* The value could be anything */
			codegen_lr(r);
			/* AC now holds stuff to add */
			add_r_r(R_REG(reg), R_AC, size);
			if (!nr)
				load_ac_reg(reg, size);
			return 1;
		case T_MINUSEQ:
			/* TODO : reg/const and reg/direct versions */
			/* The value could be anything */
			codegen_lr(r);
			/* AC now holds stuff to add */
			sub_r_r(R_REG(reg), R_AC, size);
			if (!nr)
				load_ac_reg(reg, size);
			return 1;
		case T_STAREQ:
			if (r->op == T_CONSTANT && n->type != FLOAT) {
				if (can_fast_mul(size, r->value)) {
					gen_fast_mul(R_REG(reg), size, r->value);
					if (!nr)
						load_ac_reg(reg, size);
				}
				return 1;
			}
			codegen_lr(r);
			/* Need to do ac * reg */
			push_rr(R_REG(reg));
			helper(n, "mul");
			/* We did reg * ac into ac */
			load_reg_ac(reg, size);	/* Store it back */
			return 1;
		case T_SLASHEQ:
			codegen_lr(r);
			/* Need to do reg / ac */
			push_rr(R_REG(reg));
			helper_s(n, "div");
			load_reg_ac(reg, size);	/* Store it back */
			return 1;
		case T_PERCENTEQ:
			codegen_lr(r);
			/* Need to do reg / ac */
			push_rr(R_REG(reg));
			helper_s(n, "rem");
			load_reg_ac(reg, size);	/* Store it back */
			return 1;
		case T_SHLEQ:
			if (r->op == T_CONSTANT) {
				lshift_r(R_REG(reg), size, v);
				return 1;
			}
			codegen_lr(r);
			/* Do r << ac */
			v = ++label_count;
			/* Only works on AC for now */
			opnoeff_r_r(3, 3, "or");
			printf("\tjr z, X%u\n", v);
			x = label();
			add_r_r(R_REG(reg), R_REG(reg), size);
			djnz_r(R_ACCHAR, x);
			printf("X%u:\n", v);
			if (!nr)
				load_ac_reg(reg, size);
			return 1;
		case T_SHREQ:
			if (r->op == T_CONSTANT) {
				rshift_r(R_REG(reg), size, v, n->type & UNSIGNED);
				return 1;
			}
			codegen_lr(r);
			v = ++label_count;
			/* Only works on AC */
			opnoeff_r_r(3, 3, "or");
			printf("\tjr z, X%u\n", v);
			x = label();
			rshift_r(R_REG(reg), size, 1, n->type & UNSIGNED);
			djnz_r(R_ACCHAR, x);
			if (!nr)
				load_ac_reg(reg, size);
			printf("X%u:\n", v);
			return 1;
		/* TODO: reg/reg versions */
		case T_ANDEQ:
			codegen_lr(r);
			logic_r_r(reg, R_AC, size, OP_AND);
			if (!nr)
				load_ac_reg(reg, size);
			return 1;
		case T_OREQ:
			codegen_lr(r);
			logic_r_r(reg, R_AC, size, OP_OR);
			if (!nr)
				load_ac_reg(reg, size);
			return 1;
		case T_HATEQ:
			codegen_lr(r);
			logic_r_r(reg, R_AC, size, OP_XOR);
			if (!nr)
				load_ac_reg(reg, size);
			return 1;
		default:
			fprintf(stderr, "unfixed regleft on %04X\n", n->op);
		}
	}
	return 0;
}

/* Stack the node which is currently in the working register */
unsigned gen_push(struct node *n)
{
	unsigned size = get_size(n->type);

	/* Our push will put the object on the stack, so account for it */
	sp += size;
	
	push_ac(size);
	return 1;
}

static unsigned gen_cast(struct node *n)
{
	unsigned lt = n->type;
	unsigned rt = n->right->type;
	unsigned ls;
	unsigned rs;

	if (PTR(rt))
		rt = USHORT;
	if (PTR(lt))
		lt = USHORT;

	/* Floats and stuff handled by helper */
	if (!IS_INTARITH(lt) || !IS_INTARITH(rt))
		return 0;

	ls = get_size(lt);
	rs = get_size(rt);

	/* Size shrink is free */
	if (ls <= rs)
		return 1;
	/* Don't do the harder ones */
	if (!(rt & UNSIGNED))
		return 0;
	if (rs == 1)
		load_r_constb(R_ACINT, 0);
	if (ls == 4) {
		load_r_constb(R_AC, 0);
		load_r_constb(R_AC + 1, 0);
	}
	return 1;
}

unsigned gen_node(struct node *n)
{
	unsigned size = get_size(n->type);
	unsigned v;
	unsigned nr = n->flags & NORETURN;
	unsigned x;
	unsigned u = n->type & UNSIGNED;
	/* We adjust sp so track the pre-adjustment one too when we need it */

	v = n->value;

	/* An operation with a left hand node will have the left stacked
	   and the operation will consume it so adjust the stack.

	   The exception to this is comma and the function call nodes
	   as we leave the arguments pushed for the function call */

	if (n->left && n->op != T_ARGCOMMA && n->op != T_CALLNAME && n->op != T_FUNCCALL)
		sp -= get_size(n->left->type);

	switch (n->op) {
	case T_NREF:
		if (optsize) {
			printf("\tcall __nref_%d\n", size);
			/* Until we track r14 objects other than local */
			set_ac_node(n);
			r_modify(R_AC, size);
			r_modify(R_WORK,4);
			gen_symref(n);
			return 1;
		}
		load_r_name(R_INDEX, n, v);
		load_r_memr(R_AC, R_INDEX, size);
		set_ac_node(n);
		return 1;
	case T_LBREF:
		if (optsize) {
			printf("\tcall __nref_%d\n", size);
			/* Until we track r14 objects other than local */
			set_ac_node(n);
			r_modify(R_AC, size);
			r_modify(R_WORK,4);
			gen_symref(n);
			return 1;
		}
		load_r_label(R_INDEX, n, v);
		load_r_memr(R_AC, R_INDEX, size);
		set_ac_node(n);
		return 1;
	case T_LREF:
		/* We are loading something then not using it, and it's local
		   so can go away */
		if (nr)
			return 1;
		/* Is it already loaded ? */
		if (ac_node.op == T_LREF && ac_node.value == v &&
			get_size(ac_node.type) >= size) {
			printf(";avoided load %d\n", v);
			return 1;
		}
		if (opt < 1) {
			load_local_helper(v, size);
			return 1;
		}
		/* effectively SPL/SPH + n */
		load_r_local(R_INDEX, v + sp);
		load_r_memr(R_AC, R_INDEX, size);
		set_ac_node(n);
		return 1;
	case T_RREF:
		load_ac_reg(v, size);
		set_ac_node(n);
		return 1;
	case T_LSTREF:
		if (n->val2 + sp < 254) {
			load_r_const(R_INDEX + 1, n->val2 + sp + 2, 1);
			load_r_const(R_WORK + 1, v, 1);
			helper(n, "lstref0");
		} else {
			load_r_const(R_INDEX, n->val2 + sp + 2, 2);
			load_r_const(R_WORK + 1, v, 1);
			helper(n, "lstref");
		}
		return 1;
	case T_NSTORE:
		if (optsize) {
			printf("\tcall __nstore_%d\n", size);
			set_ac_node(n);
			r_modify(R_WORK,4);
			gen_symref(n);
			return 1;
		}
		load_r_name(R_INDEX, n, v);
		store_r_memr(R_AC, R_INDEX, size);
			set_ac_node(n);
		return 1;
	case T_LBSTORE:
		if (optsize) {
			printf("\tcall __nstore_%d\n", size);
			set_ac_node(n);
			r_modify(R_WORK,4);
			gen_symref(n);
			return 1;
		}
		load_r_label(R_INDEX, n, v);
		store_r_memr(R_AC, R_INDEX, size);
		set_ac_node(n);
		return 1;
	case T_LSTORE:
		if (opt < 1)
			store_local_helper(NULL, v + sp, size);
		else {
			load_r_local(R_INDEX, v + sp);
			store_r_memr(R_AC, R_INDEX, size);
		}
		set_ac_node(n);
		return 1;
	case T_RSTORE:
		load_reg_ac(v, size);
		set_ac_node(n);
		return 1;
		/* Call a function by name */
	case T_CALLNAME:
		invalidate_all();
		printf("\tcall _%s+%u\n", namestr(n->snum), v);
		return 1;
	case T_EQ:
		pop_rr(R_INDEX);
		store_r_memr(R_AC, R_INDEX, size);
		return 1;
	case T_RDEREF:
		/* Our deref actually is a ++ on the reg ptr. We optimize the *x++ as an op */
		if (size > 1) {
			load_r_r(R_INDEX, R_REG(v));
			load_r_r(R_INDEX + 1, R_REG(v) + 1);
			add_r_const(R_INDEX, n->val2, 2);
			load_r_memr(R_AC, R_INDEX, size);
			return 1;
		}
		load_r_memr(R_AC, R_REG(v), size);
		return 1;
	case T_RDEREFPLUS:
		/* This will do size - 1 incs as it reads */
		load_r_memr(R_AC, R_REG(v), size);
		/* And the final postinc */
		rr_incw(R_REG(v));
		return 1;
	case T_DEREF:
		/* Have to deal with overlap */
		load_r_r(R_INDEX, R_ACPTR);
		load_r_r(R_INDEX + 1, R_ACCHAR);
		load_r_memr(R_AC, R_INDEX, size);
		return 1;
	case T_FUNCCALL:
		invalidate_all();
		/* Rather than mess with indirection use a helper */
		printf("\tcall __jmpr2\n");
		return 1;
	case T_LABEL:
		if (nr)
			return 1;
		load_r_label(R_AC, n, v);
		set_ac_node(n);
		return 1;
	case T_CONSTANT:
		if (nr)
			return 1;
		load_r_const(R_AC, n->value, size);
		return 1;
	case T_NAME:
		if (nr)
			return 1;
		load_r_name(R_AC, n, v);
		set_ac_node(n);
		return 1;
	case T_ARGUMENT:
		v += frame_len + argbase;
	case T_LOCAL:
		if (nr)
			return 1;
		load_r_local(R_AC, v + sp);
		set_ac_node(n);
		return 1;
	case T_REG:
		if (nr)
			return 1;
		/* A register has no address.. we need to sort this out */
		error("rega");
		return 1;
	case T_CAST:
		if (nr)
			return 1;
		return gen_cast(n);
	case T_PLUS:
		/* Tricky as big endian on stack */
		/* FIXME: needs a !reg check */
		if (size == 4 && n->type != FLOAT && !optsize) {
			/* Games time */
			pop_r(R_WORK);
			op_r_r(3, R_WORK, "add");
			pop_r(R_WORK);
			op_r_r(2, R_WORK, "adc");
			pop_r(R_WORK);
			op_r_r(1, R_WORK, "adc");
			pop_r(R_WORK);
			op_r_r(0, R_WORK, "adc");
			return 1;
		}
		if (size > 2)
			return 0;
		/* FIXME: will need a "not register" check .. otherwise
		   use the R_WORK version above */
		if (size == 2) {
			/* Pop into r0,r1 which are free as accum is 16bit */
			pop_rr(0);
			add_r_r(R_AC, 0, 2);
			return 1;
		}
		/* size 1 */
		pop_r(0);
		add_r_r(R_AC, 0, 1);
		return 1;
	case T_MINUS:
		/* We are doing stack - ac. This isn't ideal but we've
		   dealt with the simple cases already. We could more
		   in gen_shortcut perhaps if it is still an issue */
		if (size == 4 && n->type != FLOAT && !optsize) {
			/* Lots of joy involved */
			pop_r(R_WORK);
			op_r_r(R_WORK, 3, "sub");
			load_r_r(3, R_WORK);
			pop_r(R_WORK);
			op_r_r(R_WORK, 2, "sbc");
			load_r_r(2, R_WORK);
			pop_r(R_WORK);
			op_r_r(R_WORK, 1, "sbc");
			load_r_r(1, R_WORK);
			pop_r(R_WORK);
			op_r_r(R_WORK, 0, "sbc");
			load_r_r(0, R_WORK);
			return 1;
		}
		if (size > 2)
			return 0;
		if (size == 2) {
			/* Pop into r0,r1 which are free as accum is 16bit */
			pop_rr(0);
			sub_r_r(0, R_ACINT, 2);
			load_r_r(R_ACINT, 0);
			load_r_r(R_ACCHAR, 1);
			return 1;
		}
		/* size 1 */
		pop_r(0);
		sub_r_r(0, R_ACCHAR, 1);
		load_r_r(R_ACCHAR, 0);
		return 1;
	case T_AND:
		pop_op(R_AC, "and", size);
		return 1;
	case T_OR:
		pop_op(R_AC, "or", size);
		return 1;
	case T_HAT:
		pop_op(R_AC, "xor", size);
		return 1;
	/* Shifts */
	/* TODO: Helper these for non const */
	case T_LTLT:
		if (nr)
			return 1;
		/* The value to shift by is in r3, the value is stacked */
		load_r_r(R_WORK, 3);
		pop_ac(size);	/* Recover working reg off stack */
		printf("\tor r%u,r%u\n", R_WORK, R_WORK);
		v = ++label_count;
		printf("\tjr z, X%u\n", v);
		x = label();
		add_r_r(R_AC, R_AC, size);
		djnz_r(R_WORK, x);
		printf("X%u:\n", v);
		return 1;
	case T_GTGT:
		if (nr)
			return 1;
		/* The value to shift by is in r3, the value is stacked */
		load_r_r(R_WORK, 3);
		pop_ac(size);	/* Recover working reg off stack */
		printf("\tor r%u,r%u\n", R_WORK, R_WORK);
		v = ++label_count;
		printf("jr z, X%u\n", v);
		x = label();
		rshift_r(R_AC, size, 1, n->type & UNSIGNED);
		djnz_r(R_WORK, x);
		printf("X%u:\n", v);
		return 1;
	/* Odd mono ops T_TILDE, T_BANG, T_BOOL, T_NEGATE */
	case T_TILDE:
		mono_r(R_AC, size, "com");
		return 1;
	case T_NEGATE:
		mono_r(R_AC, size, "com");
		add_r_const(R_AC, 1, size);
		return 1;
	case T_BOOL:
		if (n->right->flags & ISBOOL)
			return 1;
		/* Until we do cc only */
		cmpne_r_0(R_AC, size);
		n->flags |= ISBOOL;
		return 1;
	case T_BANG:
		/* Until we do cc only. Also if right is bool can do
		   a simple xor */
		if (n->right->flags & ISBOOL)
			op_r_c(3, 1, "xor");
		else
			cmpeq_r_0(R_AC, size);
		n->flags |= ISBOOL;
		return 1;
	/* Comparisons T_EQEQ, T_BANGEQ, T_LT. T_LTEQ, T_GT, T_GTEQ */
	/* Use helpers for now */
#if 0	
	case T_EQEQ:
		if (n->type == T_FLOAT)
			return 0;
		pop_compare(size, "nz");
		n->flags |= ISBOOL;
		return 1;
	case T_BANGEQ:
		if (n->type == T_FLOAT)
			return 0;
		pop_compare(size, "z");
		n->flags |= ISBOOL;
		return 1;
	case T_LT:
		if (n->type == T_FLOAT)
			return 0;
		pop_compare(size, u ? "uge" : "ge");
		n->flags |= ISBOOL;
		return 1;
	case T_LTEQ:
		if (n->type == T_FLOAT)
			return 0;
		pop_compare(size, u ? "ugt" : "gt");
		n->flags |= ISBOOL;
	case T_GT:
		if (n->type == T_FLOAT)
			return 0;
		pop_compare(size, u ? "ule" : "le");
		n->flags |= ISBOOL;
		return 1;
	case T_GTEQ:
		if (n->type == T_FLOAT)
			return 0;
		pop_compare(size, u ? "ult" : "lt");
		n->flags |= ISBOOL;
		return 1;
#endif		
	/* Need some kind of similar to the above helper for relatives, but
	   messier so probably best done as helper call. */		
	case T_ANDEQ:
		/* On entry ptr is in ACINT, stack is value to "and" by */
		logic_popeq(size, "and");
		return 1;
	case T_OREQ:
		logic_popeq(size, "or");
		return 1;
	case T_HATEQ:
		logic_popeq(size, "xor");
		return 1;
	/* No hardware multiply/divide
		T_STAR, T_SLASH, T_PERCENT, T_STARTEQ, T_SLASHEQ, T_PERCENTEQ */
	case T_SHLEQ:
		/* Pointer into r14/r15 */
		v = ++label_count;
		pop_rr(R_INDEX);
		/* Save counter */
		load_r_r(R_WORK, R_ACCHAR);
		opnoeff_r_r(R_WORK, R_WORK, "or");
		printf("\tjr z, X%u\n", v);
		/* Value into ac */
		load_r_memr(R_AC, R_INDEX, size);
		x = label();
		add_r_r(R_AC, R_AC, size);
		djnz_r(R_WORK, x);
		revstore_r_memr(R_AC, R_INDEX, size);
		printf("X%u:\n", v);
		return 1;
	case T_SHREQ:
		/* Pointer into r14/r15 */
		v = ++label_count;
		pop_rr(R_INDEX);
		load_r_r(R_WORK, R_ACCHAR);
		printf("\tor r%u,r%u\n", R_WORK, R_WORK);
		printf("\tjr z, X%u\n", v);
		/* Value into ac */
		load_r_memr(R_AC, R_INDEX, size);
		x = label();
		rshift_r(R_AC, size, 1, n->type & UNSIGNED);
		djnz_r(R_WORK, x);
		revstore_r_memr(R_AC, R_INDEX, size);
		printf("X%u:\n", v);
		return 1;
	/* += and -= we can inline except for long size. Only works for non
	   regvar case as written though */
	case T_PLUSEQ:
		if (n->type == FLOAT || optsize)
			return 0;
		/* Pointer is on stack, value in ac */
		pop_rr(R_INDEX);
		/* Hardcoded for AC for the moment but not hard to
		   fix */
		x = size;
		add_r_const(R_INDEX, size - 1, 2);
		/* Now points to top byte */
		load_r_memr(R_WORK, R_INDEX, 1);
		op_r_r(3, R_WORK, "add");
		invalidate_ac();
		v = 2;
		while(--x) {
			rr_decw(R_INDEX);
			load_r_memr(R_WORK, R_INDEX, 1);
			op_r_r(v-- , R_WORK, "adc");
			invalidate_ac();
		}
		/* Result is now in AC, and index points to start of
		   object */
		store_r_memr(R_AC, R_INDEX, size);			
		return 1;
	case T_MINUSEQ:
		if (n->type == FLOAT || optsize)
			return 0;
		/* Pointer is on stack, value in ac */
		pop_rr(R_INDEX);
		/* Hardcoded for AC for the moment but not hard to
		   fix */
		x = size;
		add_r_const(R_INDEX, size - 1, 2);
		/* Now points to low byte */
		load_r_memr(R_WORK, R_INDEX, 1);
		op_r_r(R_WORK, 3, "sub");
		invalidate_ac();
		load_r_r(3, R_WORK);
		v = 2;
		while(--x) {
			rr_decw(R_INDEX);
			load_r_memr(R_WORK, R_INDEX, 1);
			op_r_r(R_WORK, x, "sbc");
			invalidate_ac();
			load_r_r(v--, R_WORK);
		}
		/* Result is now in AC, and index points to start of
		   object */
		store_r_memr(R_AC, R_INDEX, size);			
		return 1;
	}
	return 0;
}
