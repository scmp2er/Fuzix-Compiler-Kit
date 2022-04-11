extern void header(void);
extern void newline(void);
extern void initmac(void);
extern void output_label_prefix(void);
extern void output_label_terminator (void);
extern void gen_comment(void);
extern void trailer(void);
extern void code_segment_gtext(void);
extern void data_segment_gdata(void);
extern void ppubext(SYMBOL *scptr);
extern void fpubext(SYMBOL *scptr);
extern void output_number(int num);
extern void gen_get_memory(SYMBOL *sym);
extern int gen_get_locale(SYMBOL *sym);
extern void gen_put_memory(SYMBOL *sym);
extern int gen_indirected(SYMBOL *sym);
extern void gen_put_indirect(char typeobj);
extern void gen_get_indirect(char typeobj, int reg);
extern void gen_swap(void);
extern void gen_immediate(void);
extern void gen_push(int reg);
extern void gen_pop(void);
extern void gen_swap_stack(void);
extern void gen_call(unsigned sname);
extern void gen_icall(char * sname);
extern void declare_entry_point(unsigned symbol_name);
extern void gen_ret(void);
extern void callstk(void);
extern void gen_jump(int label);
extern void gen_test_jump(int label, int ft);
extern void gen_def_byte(void);
extern void gen_def_storage(void);
extern void gen_def_word(void);
extern int gen_defer_modify_stack(int newstkp);
extern int gen_modify_stack(int newstkp);
extern void gen_multiply_by_two(void);
extern void gen_divide_by_two(void);
extern void gen_jump_case(void);
extern void gen_add(LVALUE *lval, LVALUE *lval2);
extern void gen_sub(void);
extern void gen_mult(void);
extern void gen_div(void);
extern void gen_udiv(void);
extern void gen_mod(void);
extern void gen_umod(void);
extern void gen_or(void);
extern void gen_xor(void);
extern void gen_and(void);
extern void gen_arithm_shift_right(void);
extern void gen_logical_shift_right(void);
extern void gen_arithm_shift_left(void);
extern void gen_twos_complement(void);
extern void gen_logical_negation(void);
extern void gen_complement(void);
extern void gen_convert_primary_reg_value_to_bool(void);
extern void gen_increment_primary_reg(LVALUE *lval);
extern void gen_decrement_primary_reg(LVALUE *lval);
extern void gen_equal(void);
extern void gen_not_equal(void);
extern void gen_less_than(void);
extern void gen_less_or_equal(void);
extern void gen_greater_than(void);
extern void gen_greater_or_equal(void);
extern void gen_unsigned_less_than(void);
extern void gen_unsigned_less_or_equal(void);
extern void gen_usigned_greater_than(void);
extern void gen_unsigned_greater_or_equal(void);
extern void gen_prologue(void);
extern void gen_epilogue(void);
extern int gen_register(int vp, int size, int typ);
extern void gen_statement_end(void);
extern void gen_code(void);
extern void gen_data(void);
extern char *inclib(void);
extern void gnargs(int d);
extern int assemble(char *s);
extern void gen_immediate2(void);
extern void add_offset(int val);
extern void gen_multiply(int type, int size);
extern void error(char *ptr);
extern void doerror(char *ptr);
extern int nosign(LVALUE *is);
extern void expression(int comma);
extern int hier1(LVALUE *lval);
extern int hier1a(LVALUE *lval);
extern int hier1b(LVALUE *lval);
extern int hier1c(LVALUE *lval);
extern int hier2(LVALUE *lval);
extern int hier3(LVALUE *lval);
extern int hier4(LVALUE *lval);
extern int hier5(LVALUE *lval);
extern int hier6(LVALUE *lval);
extern int hier7(LVALUE *lval);
extern int hier8(LVALUE *lval);
extern int hier9(LVALUE *lval);
extern int hier10(LVALUE *lval);
extern int hier11(LVALUE *lval);
extern void newfunc(void);
extern void newfunc_typed(int storage, unsigned n, int type);
extern void getarg(int t);
extern int doAnsiArguments(void);
extern void doLocalAnsiArgument(int type);
extern void oflush(void);
extern int getlabel(void);
extern void print_label(int label);
extern void glabel(unsigned lab);
extern void generate_label(int nlab);
extern int output_byte(char c);
extern void output_string(char *ptr);
extern void output_label_name(unsigned name);
extern void output_name(unsigned name);
extern void print_tab(void);
extern void output_line(char *ptr);
extern void output_with_tab(char *ptr);
extern void output_decimal(int number);
extern void store(LVALUE *lval);
extern int rvalue(LVALUE *lval, int reg);
extern void test(int label, int ft);
extern void scale_const(int type, int otag, int *size);
extern void create_initials(void);
extern void add_symbol_initials(unsigned symbol_name, char type);
extern int find_symbol_initials(unsigned symbol_name);
extern void add_data_initials(unsigned symbol_name, int type, int value, TAG_SYMBOL *tag);
extern int get_size(unsigned symbol_name);
extern int get_item_at(unsigned symbol_name, int position, TAG_SYMBOL *tag);
extern int openin(char *p);
extern int openout(void);
extern void outfname(char *s);
extern void fixname(char *s);
extern int checkname(char *s);
extern void do_kill(void);
extern int igetc(int unit);
extern void pl(char *str);
extern void writee(char *str);
extern void need_semicolon(void);
extern void junk(void);
extern int endst(void);
extern void needbrack(unsigned tok);
extern int match(unsigned tok);
extern void next_token(void);
extern unsigned tokbyte(void);
extern int get_type(void);
extern void compile(void);
extern void usage(void);
extern void parse(void);
extern int do_declarations(int stclass, TAG_SYMBOL *mtag, int is_struct);
extern void dumplits(void);
extern void dumpglbs(void);
extern void dump_struct(SYMBOL *symbol, int position);
extern void errorsummary(void);
extern int filename_typeof(char *s);
extern int fix_include_name (void);
extern void doinclude(void);
extern void doasm(void);
extern void dodefine(void);
extern void doundef(void);
extern void preprocess(void);
extern void doifdef(int ifdef);
extern int ifline(void);
extern void noiferr(void);
extern int cpp(void);
extern int keepch(char c);
extern void defmac(char *s);
extern void addmac(void);
extern int remove_one_line_comment(char c);
extern void delmac(int mp);
extern int putmac(char c);
extern int findmac(char *sname);
extern void toggle(char name, int onoff);
extern int primary(LVALUE *lval);
extern int dbltest(LVALUE *val1, LVALUE *val2);
extern void result(LVALUE *lval, LVALUE *lval2);
extern int constant(int val[]);
extern int number(int val[]);
extern int quoted_char(int *value);
extern int quoted_string(int *len, unsigned *position);
extern int spechar(void);
extern void callfunction(unsigned name);
extern void needlval(void);
extern int statement(int func);
extern int statement_declare(void);
extern int do_local_declares(int stclass);
extern void do_statement(void);
extern void do_compound(int func);
extern void doif(void);
extern void dowhile(void);
extern void dodo(void);
extern void dofor(void);
extern void doswitch(void);
extern void docase(void);
extern void dodefault(void);
extern void doreturn(void);
extern void dobreak(void);
extern void docont(void);
extern void dumpsw(WHILE *ws);
extern int find_tag(unsigned sname);
extern SYMBOL *find_member(TAG_SYMBOL *tag, unsigned sname);
extern void add_member(unsigned sname, char identity, char type, int offset, int storage_class);
extern int define_struct(unsigned sname, int storage, int is_struct);
extern int declare_global(int type, int storage, TAG_SYMBOL *mtag, int otag, int is_struct);
extern int initials(unsigned symbol_name, int type, int identity, int dim, int otag);
extern void struct_init(TAG_SYMBOL *tag, unsigned symbol_name);
extern int init(unsigned symbol_name, int type, int identity, int *dim, TAG_SYMBOL *tag);
extern void declare_local(int typ, int stclass, int otag);
extern int needsub(void);
extern int find_global (unsigned sname);
extern int find_locale (unsigned sname);
extern int add_global (unsigned sname, int identity, int type, int offset, int storage);
extern int add_local (unsigned sname, int identity, int type, int offset, int storage_class);
extern unsigned symname(void);
extern void illname(void);
extern void multidef(unsigned symbol_name);
extern void notvoid(int type);
extern void addwhile(WHILE *ptr);
extern void delwhile(void);
extern WHILE *readwhile(void);
extern WHILE *findwhile(void);
extern WHILE *readswitch(void);
extern void addcase(int val);
extern void defer_output(void);
extern void end_defer(void);
extern void defer_init(void);
