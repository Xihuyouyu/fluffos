#include "config.h"
#if !defined(NeXT) && !defined(LATTICE)
#include <varargs.h>
#endif
#if defined(SunOS_5) || defined(LATTICE)
#include <stdlib.h>
#endif
#include <stdio.h>
#include <setjmp.h>
#include <string.h>
#include <ctype.h>
#ifndef LATTICE
#include <sys/time.h>
#else
#include <time.h>
#endif
/* sys/types.h is here to enable include of comm.h below */
#include <sys/types.h>
#include <sys/stat.h>
#ifndef LATTICE
#include <memory.h>
#endif
#ifdef LATTICE
#include <socket.h>
#endif

#include "lint.h"
#include "opcodes.h"
#include "exec.h"
#include "interpret.h"
#include "mapping.h"
#include "buffer.h"
#include "object.h"
#include "instrs.h"
#include "patchlevel.h"
#include "comm.h"
#include "switch.h"
#include "efun_protos.h"
#include "efunctions.h"
#include "socket_efuns.h"
#include "eoperators.h"
#include "stralloc.h"
#include "include/origin.h"
#ifdef OPCPROF
#include "opc.h"
#endif
#include "debug.h"
#include "applies.h"

#ifdef OPCPROF
static int opc_eoper[BASE];
#endif

#if defined(RUSAGE) && !defined(LATTICE)	/* Defined in config.h */
#include <sys/resource.h>
#ifdef SunOS_5
#include <sys/rusage.h>
#endif
#ifdef sun
extern int getpagesize();
#endif
#ifndef RUSAGE_SELF
#define RUSAGE_SELF	0
#endif
#endif

static struct type_name_s {
    int code;
    char *name;
}           type_names[] = {
    {
	T_NUMBER, "int"
    },
    {
	T_STRING, "string"
    },
    {
	T_POINTER, "array"
    },
    {
	T_OBJECT, "object"
    },
    {
	T_MAPPING, "mapping"
    },
    {
	T_FUNCTION, "function"
    },
    {
	T_REAL, "float"
    },
    {
	T_BUFFER, "buffer"
    }
};

int master_ob_is_loading;
#ifndef NO_UIDS
extern userid_t *backbone_uid;
#endif
extern int max_cost;
extern int call_origin;

static void push_indexed_lvalue PROT((void));
static struct svalue *find_value PROT((int));
#ifdef TRACE
static void do_trace_call PROT((struct function *));
#endif
static void break_point PROT((void));
static void do_loop_cond PROT((void));
static void do_catch PROT((char *, unsigned short));
#ifdef OPCPROF
static int cmpopc PROT((opc_t *, opc_t *));
#endif
#ifdef DEBUG
int last_instructions PROT((void));
static void clear_vector_refs PROT((struct svalue *, int));
#endif
static float _strtof PROT((char *, char **));
static struct svalue *sapply PROT((char *, struct object *, int));
static char *get_line_number PROT((char *, struct program *));
#ifdef TRACE_CODE
static char *get_arg PROT((int, int));
#endif
#ifdef DEBUG
static void count_inherits PROT((struct program *, struct program *));
static void count_ref_in_vector PROT((struct svalue *, int));
#endif

extern void print_svalue PROT((struct svalue *));
int inter_sscanf PROT((struct svalue *, struct svalue *, struct svalue *, int));
extern struct object *previous_ob;
extern char *last_verb;
extern struct svalue const0, const1, const0u, const0n;

struct program *current_prog;
short int caller_type;

extern int current_time;
extern struct object *current_heart_beat, *current_interactive;

static int tracedepth;

/*
 * Inheritance:
 * An object X can inherit from another object Y. This is done with
 * the statement 'inherit "file";'
 * The inherit statement will clone a copy of that file, call reset
 * in it, and set a pointer to Y from X.
 * Y has to be removed from the linked list of all objects.
 * All variables declared by Y will be copied to X, so that X has access
 * to them.
 *
 * If Y isn't loaded when it is needed, X will be discarded, and Y will be
 * loaded separately. X will then be reloaded again.
 */
extern int d_flag;

extern int current_line, eval_cost;

/*
 * These are the registers used at runtime.
 * The control stack saves registers to be restored when a function
 * will return. That means that control_stack[0] will have almost no
 * interesting values, as it will terminate execution.
 */
char *pc;			/* Program pointer. */
struct svalue *fp;		/* Pointer to first argument. */

struct svalue *sp;

short *break_sp;		/* Points to address to branch to at next
				 * F_BREAK			 */
int function_index_offset;	/* Needed for inheritance */
static int variable_index_offset;	/* Needed for inheritance */

static struct svalue start_of_stack[EVALUATOR_STACK_SIZE];
/* Used to throw an error to a catch */
struct svalue catch_value =
{T_NUMBER};

struct control_stack control_stack[MAX_TRACE];
struct control_stack *csp;	/* Points to last element pushed */

int too_deep_error = 0, max_eval_error = 0;

void get_version P1(char *, buff)
{
    sprintf(buff, "MudOS %s", PATCH_LEVEL);
}

/*
 * Information about assignments of values:
 *
 * There are three types of l-values: Local variables, global variables
 * and vector elements.
 *
 * The local variables are allocated on the stack together with the arguments.
 * the register 'frame_pointer' points to the first argument.
 *
 * The global variables must keep their values between executions, and
 * have space allocated at the creation of the object.
 *
 * Elements in vectors are similar to global variables. There is a reference
 * count to the whole vector, that states when to deallocate the vector.
 * The elements consists of 'struct svalue's, and will thus have to be freed
 * immediately when over written.
 */

/*
 * Push an object pointer on the stack. Note that the reference count is
 * incremented.
 * A destructed object must never be pushed onto the stack.
 */
INLINE
void push_object P1(struct object *, ob)
{
    sp++;
    if (sp == &start_of_stack[EVALUATOR_STACK_SIZE])
	fatal("stack overflow\n");
    if (ob) {
	sp->type = T_OBJECT;
	sp->u.ob = ob;
	add_ref(ob, "push_object");
    } else {
	sp->type = T_NUMBER;
	sp->subtype = T_NULLVALUE;
	sp->u.number = 0;
    }
}

char * type_name P1(int, c) { 
    int j; 
    int limit;

    limit = sizeof(type_names) / sizeof(struct type_name_s);
    for (j = 0; j < limit; j++) { 
	if (c == type_names[j].code) { 
	    return type_names[j].name;
	}
    } 
    return "unknown";
}

/*
 * May current_object shadow object 'ob' ? We rely heavily on the fact that
 * function names are pointers to shared strings, which means that equality
 * can be tested simply through pointer comparison.
 */
#ifndef NO_SHADOWS
int validate_shadowing P1(struct object *, ob)
{
    int i, j;
    struct program *shadow = current_object->prog, *victim = ob->prog;
    struct svalue *ret;

    if (current_object->shadowing)
	error("shadow: Already shadowing.\n");
    if (current_object->shadowed)
	error("shadow: Can't shadow when shadowed.\n");
    if (current_object->super)
	error("shadow: The shadow must not reside inside another object.\n");
    if (ob == master_ob)
	error("shadow: cannot shadow the master object.\n");
    if (ob->shadowing)
	error("shadow: Can't shadow a shadow.\n");
#ifdef OPTIMIZE_FUNCTION_TABLE_SEARCH
    /*
     * Want to iterate over the smaller function table, and use binary search
     * on the larger one for faster operation, ie O(m lg n), where m < n,
     * versus O(m n)
     */
    if ((int) shadow->p.i.num_functions < (int) victim->p.i.num_functions) {
	for (i = 0; i < (int) shadow->p.i.num_functions; i++) {
	    j = lookup_function(victim->p.i.functions, victim->p.i.tree_r,
				shadow->p.i.functions[i].name);
	    if (j != -1 && victim->p.i.functions[j].type & TYPE_MOD_NO_MASK)
		error("Illegal to shadow 'nomask' function \"%s\".\n",
		      victim->p.i.functions[j].name);
	}
    } else {
	for (i = 0; i < (int) victim->p.i.num_functions; i++) {
	    j = lookup_function(shadow->p.i.functions, shadow->p.i.tree_r,
				victim->p.i.functions[i].name);
	    if (j != -1 && victim->p.i.functions[i].type & TYPE_MOD_NO_MASK)
		error("Illegal to shadow 'nomask' function \"%s\".\n",
		      victim->p.i.functions[i].name);
	}
    }
#else
    for (i = 0; i < (int) shadow->p.i.num_functions; i++) {
	for (j = 0; j < (int) victim->p.i.num_functions; j++) {
	    if (shadow->p.i.functions[i].name != victim->p.i.functions[j].name)
		continue;
	    if (victim->p.i.functions[j].type & TYPE_MOD_NO_MASK)
		error("Illegal to shadow 'nomask' function \"%s\".\n",
		      victim->p.i.functions[j].name);
	}
    }
#endif
    push_object(ob);
    ret = apply_master_ob(APPLY_VALID_SHADOW, 1);
    if (!(ob->flags & O_DESTRUCTED) && MASTER_APPROVED(ret)) {
	return 1;
    }
    return 0;
}
#endif

/*
 * Push a number on the value stack.
 */
INLINE void
push_number P1(int, n)
{
    sp++;
    if (sp == &start_of_stack[EVALUATOR_STACK_SIZE])
	fatal("stack overflow\n");
    sp->type = T_NUMBER;
    sp->subtype = 0;
    sp->u.number = n;
}

INLINE void
push_real P1(double, n)
{
    sp++;
    if (sp == &start_of_stack[EVALUATOR_STACK_SIZE])
	fatal("stack overflow\n");
    sp->type = T_REAL;
    sp->u.real = n;
}

/*
 * Push undefined (const0u) onto the value stack.
 */
INLINE
void push_undefined()
{
    sp++;
    if (sp == &start_of_stack[EVALUATOR_STACK_SIZE])
	fatal("stack overflow\n");
    *sp = const0u;
}

/*
 * Push null (const0n) onto the value stack.
 */
INLINE
void push_null()
{
    sp++;
    if (sp == &start_of_stack[EVALUATOR_STACK_SIZE])
	fatal("stack overflow\n");
    *sp = const0n;
}

/*
 * Push a string on the value stack.
 */
INLINE
void push_string P2(char *, p, int, type)
{
    sp++;
    if (sp == &start_of_stack[EVALUATOR_STACK_SIZE])
	fatal("stack overflow\n");
    sp->type = T_STRING;
    sp->subtype = type;
    switch (type) {
    case STRING_MALLOC:
	sp->u.string = string_copy(p);
	break;
    case STRING_SHARED:
	sp->u.string = make_shared_string(p);
	break;
    case STRING_CONSTANT:
	sp->u.string = p;
	break;
    }
}

/*
 * Get address to a valid global variable.
 */
#ifdef DEBUG
static INLINE struct svalue *find_value P1(int, num)
{
    DEBUG_CHECK2(num >= (int) current_object->prog->p.i.num_variables,
		 "Illegal variable access %d(%d).\n",
		 num, current_object->prog->p.i.num_variables);
    return &current_object->variables[num];
}
#else
#define find_value(num) (&current_object->variables[num])
#endif

INLINE void
free_string_svalue P1(struct svalue *, v)
{
    switch (v->subtype) {
	case STRING_MALLOC:
	FREE(v->u.string);
	break;
    case STRING_SHARED:
	free_string(v->u.string);
	break;
    }
}

/*
 * Free the data that an svalue is pointing to. Not the svalue
 * itself.
 * Use the free_svalue() define to call this
 */
#ifdef DEBUG
INLINE void int_free_svalue P2(struct svalue *, v, char *, tag)
#else
INLINE void int_free_svalue P1(struct svalue *, v)
#endif
{
    switch (v->type) {
	case T_STRING:
	switch (v->subtype) {
	    case STRING_MALLOC:
	    FREE(v->u.string);
	    break;
	case STRING_SHARED:
	    free_string(v->u.string);
	    break;
	}
	break;
    case T_OBJECT:
	free_object(v->u.ob, "free_svalue");
	break;
    case T_POINTER:
	free_vector(v->u.vec);
	break;
    case T_BUFFER:
	free_buffer(v->u.buf);
	break;
    case T_MAPPING:
	free_mapping(v->u.map);
	break;
    case T_FUNCTION:
	free_funp(v->u.fp);
	break;
#ifdef DEBUG
    case T_FREED:
	fatal("T_FREED svalue freed.  Previously freed by %s.\n", v->u.string);
	break;
#endif
    }
    IF_DEBUG(v->type = T_FREED);
    IF_DEBUG(v->u.string = tag);
}

/*
 * Free several svalues, and free up the space used by the svalues.
 * The svalues must be sequentially located.
 */
INLINE void free_some_svalues P2(struct svalue *, v, int, num)
{
    if (v) {
	for (; num--;)
	    free_svalue(&v[num], "free_some_svalues");
	FREE(v);
    }
}

INLINE void push_some_svalues P2(struct svalue *, v, int, num)
{
    while (num--) push_svalue(v++);
}

/*
 * Prepend a slash in front of a string.
 */
char *add_slash P1(char *, str)
{
    char *tmp;

    tmp = DXALLOC(strlen(str) + 2, 43, "add_slash");
    strcpy(tmp, "/");
    strcpy(tmp + 1, str);
    return tmp;
}

/*
 * Assign to a svalue.
 * This is done either when element in vector, or when to an identifier
 * (as all identifiers are kept in a vector pointed to by the object).
 */

INLINE void assign_svalue_no_free P2(struct svalue *, to, struct svalue *, from)
{
    DEBUG_CHECK(from == 0, "Attempt to assign_svalue() from a null ptr.\n");
    DEBUG_CHECK(to == 0, "Attempt to assign_svalue() to a null ptr.\n");
    *to = *from;

    switch (from->type) {
    case T_STRING:
	switch (from->subtype) {
	case STRING_MALLOC:	/* No idea to make the string shared */
	    to->u.string = string_copy(from->u.string);
	    break;
	case STRING_CONSTANT:	/* Good idea to make it shared */
	    to->subtype = STRING_SHARED;
	    to->u.string = make_shared_string(from->u.string);
	    break;
	case STRING_SHARED:	/* It already is shared */
	    to->subtype = STRING_SHARED;
	    to->u.string = ref_string(from->u.string);
	    break;
	default:
	    fatal("Bad string type %d\n", from->subtype);
	}
	break;
    case T_OBJECT:
	add_ref(to->u.ob, "asgn to var");
	break;
    case T_POINTER:
	to->u.vec->ref++;
	break;
    case T_MAPPING:
	to->u.map->ref++;
	break;
    case T_FUNCTION:
	to->u.fp->ref++;
	break;
    case T_BUFFER:
	to->u.buf->ref++;
	break;
    default:
	break;
    }
}

INLINE void assign_svalue P2(struct svalue *, dest, struct svalue *, v)
{
    /* First deallocate the previous value. */
    free_svalue(dest, "assign_svalue");
    assign_svalue_no_free(dest, v);
}

/*
 * Copies an array of svalues to another location, which should be
 * free space.
 */
INLINE void copy_some_svalues P3(struct svalue *, dest, struct svalue *, v, int, num)
{
    int index;

    for (index = 0; index < num; index++)
	assign_svalue_no_free(&dest[index], &v[index]);
}

INLINE void transfer_push_some_svalues P2(struct svalue *, v, int, num)
{
    memcpy(sp + 1, v, num * sizeof(struct svalue));
    sp += num;
}

void push_svalue P1(struct svalue *, v)
{
    sp++;
    assign_svalue_no_free(sp, v);
}

/*
 * Pop the top-most value of the stack.
 * Don't do this if it is a value that will be used afterwards, as the
 * data may be sent to FREE(), and destroyed.
 */
INLINE void pop_stack()
{
    DEBUG_CHECK(sp < start_of_stack, "Stack underflow.\n");
    free_svalue(sp--, "pop_stack");
}

/*
 * Compute the address of an array element.
 */
static INLINE void push_indexed_lvalue()
{
    struct svalue *i, *vec, *item;
    int ind, indType;

    i = sp;
    vec = sp - 1;

    if (vec->type == T_MAPPING) {
	struct mapping *m = vec->u.map;

	vec = find_for_insert(m, i, 0);
	pop_stack();
	free_svalue(sp, "push_indexed_lvalue");
	sp->type = T_LVALUE;
	sp->u.lvalue = vec;
	if (!vec) {
	    mapping_too_large();
	}
	return;
    }
    ind = i->u.number;
    indType = i->type;
    pop_stack();
    if ((indType != T_NUMBER) || (ind < 0))
	error("Illegal index\n");
#ifndef NEW_FUNCTIONS
    if (vec->type == T_FUNCTION) {
	if (ind > 1) {
	    error("Function variables may only be indexed with 0 or 1.\n");
	}
	item = ind ? &vec->u.fp->fun : &vec->u.fp->obj;
	if (vec->u.fp->ref == 1) {	/* kludge */
	    static struct svalue quickfix =
	    {T_NUMBER};

	    assign_svalue(&quickfix, item);
	    item = &quickfix;
	}
	free_svalue(sp, "push_indexed_lvalue");
	sp->type = T_LVALUE;
	sp->u.lvalue = item;
	return;
    }
#endif
    if (vec->type == T_STRING) {
	error("LPC strings (in MudOS) are not mutable (maybe later).\n");
	return;
    }
    if (vec->type == T_BUFFER) {
	unsigned char *addr;

	if ((ind >= vec->u.buf->size) || (ind < 0)) {
	    error("Buffer index out of bounds.\n");
	}
	addr = &vec->u.buf->item[ind];
	free_svalue(sp, "push_indexed_lvalue");
	sp->type = T_LVALUE_BYTE;
	sp->u.lvalue_byte = addr;
	return;
    }
    if (vec->type != T_POINTER)
	error("Indexing on illegal type.\n");
    if (ind >= vec->u.vec->size)
	error("Index out of bounds\n");
    item = &vec->u.vec->item[ind];
    if (vec->u.vec->ref == 1) {
	static struct svalue quickfix =
	{T_NUMBER};

	/* marion says: but this is crude too */
	/* marion blushes. */
	assign_svalue(&quickfix, item);
	item = &quickfix;
    }
    /* This will make 'vec' invalid to use */
    free_svalue(sp, "push_indexed_lvalue");
    sp->type = T_LVALUE;
    sp->u.lvalue = item;
}

/*
 * Deallocate 'n' values from the stack.
 */
INLINE void
pop_n_elems P1(int, n)
{
    DEBUG_CHECK1(n < 0, "pop_n_elems: %d elements.\n", n);
    while (n--) {
	pop_stack();
    }
}

/*
 * Deallocate 2 values from the stack.
 */
INLINE void
pop_2_elems()
{
    free_svalue(sp--, "pop_2_elems");
    DEBUG_CHECK(sp < start_of_stack, "Stack underflow.\n");
    free_svalue(sp--, "pop_2_elems");
}

/*
 * Deallocate 3 values from the stack.
 */
INLINE void
pop_3_elems()
{
    free_svalue(sp--, "pop_3_elems");
    free_svalue(sp--, "pop_3_elems");
    DEBUG_CHECK(sp < start_of_stack, "Stack underflow.\n");
    free_svalue(sp--, "pop_3_elems");
}

void bad_arg P2(int, arg, int, instr)
{
    error("Bad argument %d to %s()\n", arg, get_f_name(instr));
}

void bad_argument P4(struct svalue *, val, int, type, int, arg, int, instr)
{
    char *buf;
    int flag = 0;

    buf = (char *) DMALLOC(300, 0,"bad_argument");
    sprintf(buf, "Bad argument %d to %s%s\nExpected: ", arg, 
	    get_f_name(instr), (instr < BASE ? "" : "()"));

    if (type & T_NUMBER) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "number");
    }
    if (type & T_STRING) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "string");
    }
    if (type & T_POINTER) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "array");
    }
    if (type & T_OBJECT) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "object");
    }
    if (type & T_MAPPING) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "mapping");
    }
    if (type & T_FUNCTION) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "function");
    }
    if (type & T_REAL) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "float");
    }
    if (type & T_BUFFER) {
	if (flag)
	    strcat(buf, " or ");
	flag = 1;
	strcat(buf, "buffer");
    }
    strcat(buf, " Got: ");
    svalue_to_string(val, &buf, 300, 0, 0, 0);
    strcat(buf, ".\n");
    error_needs_free(buf);
}

#ifdef NEW_FUNCTIONS
struct function fake_func = { "<function>" };
struct program fake_prog = { "<function>" };
unsigned char fake_program = F_RETURN;

void setup_fake_frame P1(struct funp *, fun) {
  if (csp == &control_stack[MAX_TRACE-1]) {
    too_deep_error = 1;
    error("Too deep recursion.\n");
  }
  if (fun->owner->flags & O_DESTRUCTED)
      error("Owner of function pointer has been destructed.\n");
  csp++;
  csp->caller_type = caller_type;
  csp->funp = &fake_func;
  csp->ob = current_object;
  csp->extern_call = 1;
  csp->prev_ob = previous_ob;
  csp->fp = fp;
  csp->prog = current_prog;
  csp->pc = pc;
  pc = (char *)&fake_program;
  csp->function_index_offset = function_index_offset;
  csp->variable_index_offset = variable_index_offset;
  csp->break_sp = break_sp;
  caller_type = ORIGIN_FUNCTION_POINTER;
  csp->num_local_variables = 0;
  current_prog = &fake_prog;
  previous_ob = current_object;
  current_object = fun->owner;
}

void remove_fake_frame() {
    DEBUG_CHECK(csp == (control_stack - 1),
		"Popped out of the control stack\n");
  current_object = csp->ob;
  current_prog = csp->prog;
  previous_ob = csp->prev_ob;
  caller_type = csp->caller_type;
  pc = csp->pc;
  fp = csp->fp;
  function_index_offset = csp->function_index_offset;
  variable_index_offset = csp->variable_index_offset;
  break_sp = csp->break_sp;
  csp--;
}
#endif

INLINE void
push_control_stack P1(struct function *, funp)
{
    if (csp == &control_stack[MAX_TRACE - 1]) {
	too_deep_error = 1;
	error("Too deep recursion.\n");
    }
    csp++;
    csp->caller_type = caller_type;
    csp->ob = current_object;
    csp->funp = funp;		/* Only used for tracebacks */
    csp->prev_ob = previous_ob;
    csp->fp = fp;
    csp->prog = current_prog;
    /* csp->extern_call = 0; It is set by eval_instruction() */
    csp->pc = pc;
    csp->function_index_offset = function_index_offset;
    csp->variable_index_offset = variable_index_offset;
    csp->break_sp = break_sp;
#ifdef PROFILE_FUNCTIONS
    if (funp) {
	get_cpu_times(&(csp->entry_secs), &(csp->entry_usecs));
	funp->calls++;
    }
#endif
}

/*
 * Pop the control stack one element, and restore registers.
 * extern_call must not be modified here, as it is used imediately after pop.
 */
void pop_control_stack()
{
    DEBUG_CHECK(csp == (control_stack - 1),
		"Popped out of the control stack\n");
#ifdef PROFILE_FUNCTIONS
    if (csp->funp) {
	long secs, usecs, dsecs;

	get_cpu_times((unsigned long *) &secs, (unsigned long *) &usecs);
	dsecs = (((secs - csp->entry_secs) * 1000000)
		 + (usecs - csp->entry_usecs));
	csp->funp->self += dsecs;
	if (csp != control_stack) {
	    struct function *f;

	    if ((f = (csp - 1)->funp)) {
		f->children += dsecs;
	    }
	}
    }
#endif
    current_object = csp->ob;
    current_prog = csp->prog;
    previous_ob = csp->prev_ob;
    caller_type = csp->caller_type;
    pc = csp->pc;
    fp = csp->fp;
    function_index_offset = csp->function_index_offset;
    variable_index_offset = csp->variable_index_offset;
    break_sp = csp->break_sp;
    csp--;
}

/*
 * Push a pointer to a vector on the stack. Note that the reference count
 * is incremented. Newly created vectors normally have a reference count
 * initialized to 1.
 */
INLINE void push_vector P1(struct vector *, v)
{
    v->ref++;
    sp++;
    sp->type = T_POINTER;
    sp->u.vec = v;
}

INLINE void push_refed_vector P1(struct vector *, v)
{
    sp++;
    sp->type = T_POINTER;
    sp->u.vec = v;
}

INLINE void
push_buffer P1(struct buffer *, b)
{
    b->ref++;
    sp++;
    sp->type = T_BUFFER;
    sp->u.buf = b;
}

INLINE void
push_refed_buffer P1(struct buffer *, b)
{
    sp++;
    sp->type = T_BUFFER;
    sp->u.buf = b;
}

/*
 * Push a mapping on the stack.  See push_vector(), above.
 */
INLINE void
push_mapping P1(struct mapping *, m)
{
    m->ref++;
    sp++;
    sp->type = T_MAPPING;
    sp->u.map = m;
}

INLINE void
push_refed_mapping P1(struct mapping *, m)
{
    sp++;
    sp->type = T_MAPPING;
    sp->u.map = m;
}

/*
 * Push a string on the stack that is already malloced.
 */
INLINE void push_malloced_string P1(char *, p)
{
    sp++;
    sp->type = T_STRING;
    sp->u.string = p;
    sp->subtype = STRING_MALLOC;
}

/*
 * Push a string on the stack that is already constant.
 */
INLINE
void push_constant_string P1(char *, p)
{
    sp++;
    sp->type = T_STRING;
    sp->u.string = p;
    sp->subtype = STRING_CONSTANT;
}

#ifdef TRACE
static void do_trace_call P1(struct function *, funp)
{
    do_trace("Call direct ", funp->name, " ");
    if (TRACEHB) {
	if (TRACETST(TRACE_ARGS)) {
	    int i;

	    add_message(" with %d arguments: ", funp->num_arg);
	    for (i = funp->num_arg - 1; i >= 0; i--) {
		print_svalue(&sp[-i]);
		add_message(" ");
	    }
	}
	add_message("\n");
    }
}
#endif

/*
 * Argument is the function to execute. If it is defined by inheritance,
 * then search for the real definition, and return it.
 * There is a number of arguments on the stack. Normalize them and initialize
 * local variables, so that the called function is pleased.
 */
INLINE struct function *
         setup_new_frame P1(struct function *, funp)
{
    function_index_offset = 0;
    variable_index_offset = 0;
    while (funp->flags & NAME_INHERITED) {
	function_index_offset +=
	    current_prog->p.i.inherit[funp->offset].function_index_offset;
	variable_index_offset +=
	    current_prog->p.i.inherit[funp->offset].variable_index_offset;
	current_prog =
	    current_prog->p.i.inherit[funp->offset].prog;
	funp = &current_prog->p.i.functions[funp->function_index_offset];
    }
    /* Remove excessive arguments */
    while (csp->num_local_variables > (int) funp->num_arg) {
	pop_stack();
	csp->num_local_variables--;
    }
    /* Correct number of arguments and local variables */
    while (csp->num_local_variables < (int) (funp->num_arg + funp->num_local)) {
	push_null();
	csp->num_local_variables++;
    }
#ifdef TRACE
    tracedepth++;
    if (TRACEP(TRACE_CALL)) {
	do_trace_call(funp);
    }
#endif
    fp = sp - csp->num_local_variables + 1;
    break_sp = (short *) (sp + 1);
    return funp;
}

static void break_point()
{
    if (sp - fp - csp->num_local_variables + 1 != 0)
	fatal("Bad stack pointer.\n");
}

/* marion
 * maintain a small and inefficient stack of error recovery context
 * data structures.
 * This routine is called in three different ways:
 * push=-1    Pop the stack.
 * push=1 push the stack.
 * push=0 No error occured, so the pushed value does not have to be
 *        restored. The pushed value can simply be popped into the void.
 *
 * The stack is implemented as a linked list of stack-objects, allocated
 * from the heap, and deallocated when popped.
 */

/* push_pop_error_context: Copied directly from Lars version 3.1.1 */
void push_pop_error_context P1(int, push)
{
    extern jmp_buf error_recovery_context;
    extern int error_recovery_context_exists;
    static struct error_context_stack {
	jmp_buf old_error_context;
	int old_exists_flag;
	struct control_stack *save_csp;
	struct object *save_command_giver;
	struct svalue *save_sp;
	struct error_context_stack *next;
    }                  *ecsp = 0, *p;

    if (push == 1) {
	/*
	 * Save some global variables that must be restored separately after
	 * a longjmp. The stack will have to be manually popped all the way.
	 */
	p = (struct error_context_stack *)
	    DXALLOC(sizeof(struct error_context_stack), 44, "push_pop_error_context");
	p->save_sp = sp;
	p->save_csp = csp;
	p->save_command_giver = command_giver;
	memcpy(
		  (char *) p->old_error_context,
		  (char *) error_recovery_context,
		  sizeof error_recovery_context);
	p->old_exists_flag = error_recovery_context_exists;
	p->next = ecsp;
	ecsp = p;
    } else {
	p = ecsp;
	if (p == 0) {
	    fatal("Catch: error context stack underflow.");
	}
	if (push == 0) {
	    DEBUG_CHECK(csp != (p->save_csp - 1), 
			"Catch: Lost track of csp\n");
	} else {
	    /*
	     * push == -1 ! They did a throw() or error. That means that the
	     * control stack must be restored manually here.
	     */
	    csp = p->save_csp;
	    pop_n_elems(sp - p->save_sp);
	    command_giver = p->save_command_giver;
	}
	memcpy((char *) error_recovery_context,
	       (char *) p->old_error_context,
	       sizeof error_recovery_context);
	error_recovery_context_exists = p->old_exists_flag;
	ecsp = p->next;
	FREE((char *) p);
    }
}

/*
 * When a vector is given as argument to an efun, all items have to be
 * checked if there would be a destructed object.
 * A bad problem currently is that a vector can contain another vector, so this
 * should be tested too. But, there is currently no prevention against
 * recursive vectors, which means that this can not be tested. Thus, MudOS
 * may crash if a vector contains a vector that contains a destructed object
 * and this top-most vector is used as an argument to an efun.
 */
/* MudOS won't crash when doing simple operations like assign_svalue
 * on a destructed object. You have to watch out, of course, that you don't
 * apply a function to it.
 * to save space it is preferable that destructed objects are freed soon.
 *   amylaar
 */
void check_for_destr P1(struct vector *, v)
{
    int i = v->size;

    while (i--) {
	if (v->item[i].type != T_OBJECT)
	    continue;
	if (!(v->item[i].u.ob->flags & O_DESTRUCTED))
	    continue;
	free_svalue(&v->item[i], "check_for_destr");
	v->item[i] = const0;
    }
}

/* do_loop_cond() coded by John Garnett, 1993/06/01

   Optimizes these four cases (with 'int i'):

   1) for (expr0; i < integer_variable; expr2) statement;
   2) for (expr0; i < integer_constant; expr2) statement;
   3) while (i < integer_variable) statement;
   4) while (i < integer_constant) statement;
*/

static INLINE void do_loop_cond()
{
    struct svalue *s1, *s2;
    int is_local, arg2;

    s1 = fp + EXTRACT_UCHAR(pc);    /* a from (a < b) */
    pc++;
    if ((is_local = (*pc == F_LOCAL))) {
	char *tpc;

	tpc = pc + 1;
	s2 = fp + EXTRACT_UCHAR(tpc);
    }
    /*
     * we don't want to optimize the case in which 'a' and 'b' from (a < b)
     * are not both of type 'int' but we don't know the type of 'a' until
     * runtime. Here, we notice the type is not 'int' and proceed as if the
     * last opcode was F_LOCAL_NAME instead of F_FOR_COND.
     */
    if ((s1->type != T_NUMBER) || (is_local && (s2->type != T_NUMBER))) {
	sp++;
	assign_svalue_no_free(sp, s1);
	if ((sp->type == T_OBJECT) && (sp->u.ob->flags & O_DESTRUCTED)) {
	    free_svalue(sp, "do_loop_cond");
	    *sp = const0;
	}
	return;
    }
    if (is_local) {		/* b from (a < b) was a variable rather than
				 * a constant */
	pc += 4;		/* skip LV(index) plus the < and the
				 * F_BBRANCH... */
	arg2 = s2->u.number;
    } else {			/* extract the integer constant */
	pc++; /* F_NUMBER */
	((char *) &arg2)[0] = EXTRACT_UCHAR(pc++);
	((char *) &arg2)[1] = EXTRACT_UCHAR(pc++);
	((char *) &arg2)[2] = EXTRACT_UCHAR(pc++);
	((char *) &arg2)[3] = EXTRACT_UCHAR(pc++);
	pc += 2; /* skip the < and the bbranch */
    }
    /* do the branch if (arg1 < arg2) */
    if (s1->u.number < arg2) {
	unsigned short offset;

	((char *) &offset)[0] = pc[0];
	((char *) &offset)[1] = pc[1];
	pc -= offset;
    } else {			/* skip past the offset to the next
				 * instruction */
	pc += 2;
    }
}

#ifdef LPC_TO_C
void
call_program P2(struct program *, prog, int, offset) {
    struct svalue ret;
    
    if (prog->p.i.program_size)
	eval_instruction(prog->p.i.program + offset);
    else {
	DEBUG_CHECK(!offset, "Null function pointer in jump_table.\n");
	ret.type = T_NUMBER;
	(*
	 ( void (*)() ) offset     /* cast to a function pointer */
	 )(&ret);
	*sp++ = ret;
    }
}

void
call_absolute P1(char *, pc) {
    struct svalue ret;
    
    if (prog->p.i.program_size)
	eval_instruction(pc);
    else {
	ret.type = T_NUMBER;
	(*
	 ( void (*)() ) pc     /* cast to a function pointer */
	 )(&ret);
	*sp++ = ret;
    }
}
#endif

/*
 * Evaluate instructions at address 'p'. All program offsets are
 * to current_prog->p.i.program. 'current_prog' must be setup before
 * call of this function.
 *
 * There must not be destructed objects on the stack. The destruct_object()
 * function will automatically remove all occurences. The effect is that
 * all called efuns knows that they won't have destructed objects as
 * arguments.
 */
#ifdef TRACE_CODE
static int previous_instruction[60];
static int stack_size[60];
static char *previous_pc[60];
static int last;
#endif

void
eval_instruction P1(char *, p)
{
    int i, num_arg;
    float real;
    int instruction;
    unsigned short offset;
    unsigned short string_number;
    static func_t *oefun_table = efun_table - BASE;
    IF_DEBUG(struct svalue *expected_stack);

    /* Next F_RETURN at this level will return out of eval_instruction() */
    csp->extern_call = 1;
    pc = p;
    while (1) {
	instruction = EXTRACT_UCHAR(pc++);
	/* These defines can't handle the F_CALL_EXTRA optimization */
#if defined(TRACE_CODE) || defined(TRACE) || defined(OPCPROF)
#  ifdef NEEDS_CALL_EXTRA
	if (instruction == F_CALL_EXTRA)
	    instruction = EXTRACT_UCHAR(pc++) + 0xff;
#  endif
#  ifdef TRACE_CODE
	previous_instruction[last] = instruction;
	previous_pc[last] = pc - 1;
	stack_size[last] = sp - fp - csp->num_local_variables;
	last = (last + 1) % (sizeof previous_instruction / sizeof(int));
#  endif
#  ifdef TRACE
	if (TRACEP(TRACE_EXEC)) {
	    do_trace("Exec ", get_f_name(instruction), "\n");
	}
#  endif
#  ifdef OPCPROF
	if (instruction < BASE)
	    opc_eoper[instruction]++;
	else
	    opc_efun[instruction-BASE].count++;
#  endif
#endif
	if (!--eval_cost) {
	    fprintf(stderr, "eval_cost too big %d\n", max_cost);
	    eval_cost = max_cost;
	    max_eval_error = 1;
	    error("Too long evaluation. Execution aborted.\n");
	}
	/*
	 * Execute current instruction. Note that all functions callable from
	 * LPC must return a value. This does not apply to control
	 * instructions, like F_JUMP.
	 */
	switch (instruction) {
	case I(F_INC):
	    DEBUG_CHECK(sp->type != T_LVALUE,
			"non-lvalue argument to ++\n");
	    if (sp->u.lvalue->type == T_NUMBER) {
		sp->u.lvalue->u.number++;
		sp--;
		break;
	    } else if (sp->u.lvalue->type == T_REAL) {
		sp->u.lvalue->u.real++;
		sp--;
		break;
	    } else {
		error("++ of non-numeric argument\n");
	    }
#ifdef LPC_OPTIMIZE_LOOPS
	case I(F_WHILE_DEC):
	    {
		struct svalue *s;
		unsigned short offset;
		int r;
		
		s = fp + EXTRACT_UCHAR(pc);
		pc += 2;	/* skip the BBRANCH */
		if (s->type == T_NUMBER) {
		    r = s->u.number--;
		} else if (s->type == T_REAL) {
		    r = s->u.real--;
		} else {
		    error("-- of non-numeric argument\n");
		}
		if (r) {
		    ((char *) &offset)[0] = pc[0];
		    ((char *) &offset)[1] = pc[1];
		    pc -= offset;
		} else {
		    pc += 2;
		}
	    }
	    break;
#endif
	case I(F_LOCAL_LVALUE):
	    sp++;
	    sp->type = T_LVALUE;
	    sp->u.lvalue = fp + EXTRACT_UCHAR(pc);
	    pc++;
	    break;
	case I(F_NUMBER):
	    ((char *) &i)[0] = pc[0];
	    ((char *) &i)[1] = pc[1];
	    ((char *) &i)[2] = pc[2];
	    ((char *) &i)[3] = pc[3];
	    pc += 4;
	    push_number(i);
	    break;
	case I(F_REAL):
	    ((char *) &real)[0] = pc[0];
	    ((char *) &real)[1] = pc[1];
	    ((char *) &real)[2] = pc[2];
	    ((char *) &real)[3] = pc[3];
	    pc += 4;
	    push_real(real);
	    break;
	case I(F_BYTE):
	    i = EXTRACT_UCHAR(pc);
	    pc++;
	    push_number(i);
	    break;
	case I(F_NBYTE):
	    i = EXTRACT_UCHAR(pc);
	    pc++;
	    push_number(-i);
	    break;
#ifdef F_JUMP_WHEN_NON_ZERO
	case I(F_JUMP_WHEN_NON_ZERO):
	    if ((i = (sp->type == T_NUMBER)) && (sp->u.number == 0))
		pc += 2;
	    else {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		pc = current_prog->p.i.program + offset;
	    }
	    if (i) {
		sp--;	/* when sp is an integer svalue, its cheaper
			 * to do this */
	    } else {
		pop_stack();
	    }
	    break;
#endif
	case I(F_BRANCH):	/* relative offset */
	    ((char *) &offset)[0] = pc[0];
	    ((char *) &offset)[1] = pc[1];
	    pc += offset;
	    break;
	case I(F_BBRANCH):	/* relative offset */
	    ((char *) &offset)[0] = pc[0];
	    ((char *) &offset)[1] = pc[1];
	    pc -= offset;
	    break;
	case I(F_BRANCH_WHEN_ZERO):	/* relative offset */
	    if ((i = (sp->type == T_NUMBER)) && (sp->u.number == 0)) {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		pc += offset;
		sp--;	/* can use this instead of pop_stack since
			 * its an integer */
		break;
	    }
	    pc += 2;	/* skip over the offset */
	    if (i) {
		sp--;
	    } else {
		pop_stack();
	    }
	    break;
	case I(F_BRANCH_WHEN_NON_ZERO):	/* relative offset */
	    if (((i = (sp->type != T_NUMBER))) || (sp->u.number != 0)) {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		if (i) {
		    pop_stack();
		} else {
		    sp--;
		}
		pc += offset;
		break;
	    }
	    pc += 2;	/* skip over the offset */
	    sp--;		/* sp contains an int   */
	    break;
	case I(F_BBRANCH_WHEN_ZERO):	/* relative backwards offset */
	    if ((i = (sp->type == T_NUMBER)) && (sp->u.number == 0)) {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		pc -= offset;
		sp--;
		break;
	    }
	    pc += 2;
	    if (i) {
		sp--;
	    } else {
		pop_stack();
	    }
	    break;
	case I(F_BBRANCH_WHEN_NON_ZERO):	/* relative backwards offset */
	    if ((i = (sp->type != T_NUMBER)) || (sp->u.number != 0)) {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		if (i) {
		    pop_stack();
		} else {
		    sp--;
		}
		pc -= offset;
		break;
	    }
	    pc += 2;
	    sp--;
	    break;
	case I(F_LOR):
	    /* replaces F_DUP; F_BRANCH_WHEN_NON_ZERO; F_POP */
	    if ((i = (sp->type != T_NUMBER)) || (sp->u.number != 0)) {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		pc += offset;
		break;
	    } else {
		pc += 2;
		sp--;
	    }
	    break;
	case I(F_LAND):
	    /* replaces F_DUP; F_BRANCH_WHEN_ZERO; F_POP */
	    if ((i = (sp->type == T_NUMBER)) && (sp->u.number == 0)) {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		pc += offset;
		break;
	    } else {
		pc += 2;
		if (i) {
		    sp--;
		} else {
		    pop_stack();
		}
	    }
	    break;
#ifdef LPC_OPTIMIZE_LOOPS
	case I(F_LOOP_INCR):	/* this case must be just prior to
				 * F_LOOP_COND */
	    {
		struct svalue *s;
		
		s = fp + EXTRACT_UCHAR(pc);
		pc++;
		if (s->type == T_NUMBER) {
		    s->u.number++;
		} else if (s->type == T_REAL) {
		    s->u.real++;
		} else {
		    error("++ of non-numeric argument\n");
		}
	    }
	    if (*pc == F_LOOP_COND) {
		pc++;
	    } else {
		break;
	    }
	case I(F_LOOP_COND):
	    do_loop_cond();
	    break;
#endif
	case I(F_LOCAL):
	    {
		struct svalue *s;
		
		s = fp + EXTRACT_UCHAR(pc);
		DEBUG_CHECK((fp-s) >= csp->num_local_variables,
			    "Tried to push non-existent local\n");
		sp++;
		pc++;
		
		/*
		 * If variable points to a destructed object, replace it
		 * with 0, otherwise, fetch value of variable.
		 */
		if ((s->type == T_OBJECT) && (s->u.ob->flags & O_DESTRUCTED)) {
		    *sp = const0;
		    assign_svalue(s, &const0);
		} else {
		    assign_svalue_no_free(sp, s);
		}
		break;
	    }
	case I(F_LT):
	    {
		if ((sp - 1)->type == T_NUMBER) {
		    if (sp->type == T_NUMBER) {	/* optimize this case */
			sp--;
			sp->u.number = (sp->u.number < (sp + 1)->u.number);
			break;
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.number < sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type == T_REAL) {
		    if (sp->type == T_NUMBER) {
			i = ((sp - 1)->u.real < sp->u.number);
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.real < sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type != T_STRING) {
		    bad_argument(sp - 1, T_NUMBER | T_STRING | T_REAL, 1, instruction);
		} else if (sp->type != T_STRING) {
		    bad_argument(sp, T_STRING, 2, instruction);
		} else {
		    i = (strcmp((sp - 1)->u.string, sp->u.string) < 0);
		}
	    }
	    pop_n_elems(2);
	    push_number(i);
	    break;
	case I(F_ADD):
	    {
		switch ((sp - 1)->type) {
		case T_BUFFER:
		    if (sp->type != T_BUFFER) {
			error("Bad type argument to +. Had %s and %s.\n",
			      type_name((sp - 1)->type), type_name(sp->type));
		    } else {
			struct buffer *b;
			
			b = allocate_buffer(sp->u.buf->size + (sp - 1)->u.buf->size);
			memcpy(b->item, (sp - 1)->u.buf->item, (sp - 1)->u.buf->size);
			memcpy(b->item + (sp - 1)->u.buf->size, sp->u.buf->item,
			       sp->u.buf->size);
			pop_n_elems(2);
			push_refed_buffer(b);
		    }
		    break;
		case T_REAL:
		    {
			switch (sp->type) {
			case T_NUMBER:
			    (sp - 1)->u.real = sp->u.number + (sp - 1)->u.real;
			    sp--;
			    break;
			case T_REAL:
			    (sp - 1)->u.real = sp->u.real + (sp - 1)->u.real;
			    sp--;
			    break;
			case T_STRING:
			    {
				char buff[100], *res;
				int r, len;
				
				sprintf(buff, "%f", (sp - 1)->u.real);
				len = SVALUE_STRLEN(sp) + (r = strlen(buff)) + 1;
				res = DXALLOC(len, 36, "f_add: 3");
				strcpy(res, buff);
				strcpy(res + r, sp->u.string);
				pop_n_elems(2);
				push_malloced_string(res);
				break;
			    }
			}
			break;
		    }
		case T_STRING:
		    {
			switch (sp->type) {
			case T_STRING:
			    {
				char *res;
				int r = SVALUE_STRLEN(sp - 1);
				int len = r + SVALUE_STRLEN(sp) + 1;
				
				if ((sp - 1)->subtype == STRING_MALLOC) {
				    res = (char *) DREALLOC((sp - 1)->u.string, len, 34, "f_add: 1");
				    if (!res)
					fatal("Out of memory!\n");
				    (void) strcpy(res + r, sp->u.string);
				    free_string_svalue(sp--);
				    sp->u.string = res;
				    break;
				}
				res = (char *) DXALLOC(len, 34, "f_add: 1");
				(void) strcpy(res, (sp - 1)->u.string);
				(void) strcpy(res + r, sp->u.string);
				/*
				 * we know they are strings so this saves
				 * time
				 */
				free_string_svalue(sp - 1);
				free_string_svalue(sp);
				sp -= 2;
				push_malloced_string(res);
				break;
			    }
			case T_NUMBER:
			    {
				char buff[20];
				char *res;
				int r, len;
				
				sprintf(buff, "%d", sp->u.number);
				len = (r = SVALUE_STRLEN(sp - 1)) + strlen(buff) + 1;
				
				if ((sp - 1)->subtype == STRING_MALLOC) {
				    res = (char *) DREALLOC((sp - 1)->u.string, len, 35, "f_add: 2");
				    if (!res)
					fatal("Out of memory!\n");
				    (void) strcpy(res + r, buff);
				    sp--;
				    sp->u.string = res;
				    break;
				}
				res = DXALLOC(len, 35, "f_add: 2");
				strcpy(res, (sp - 1)->u.string);
				strcpy(res + r, buff);
				pop_n_elems(2);
				push_malloced_string(res);
				break;
			    }
			case T_REAL:
			    {
				char buff[25];
				char *res;
				int r, len;
				
				sprintf(buff, "%f", sp->u.real);
				len = (r = SVALUE_STRLEN(sp - 1)) + strlen(buff) + 1;
				
				if ((sp - 1)->subtype == STRING_MALLOC) {
				    res = (char *) DREALLOC((sp - 1)->u.string, len, 35, "f_add: 2");
				    if (!res)
					fatal("Out of memory!\n");
				    (void) strcpy(res + r, buff);
				    sp--;
				    sp->u.string = res;
				    break;
				}
				res = DXALLOC(len, 35, "f_add: 2");
				strcpy(res, (sp - 1)->u.string);
				strcpy(res + r, buff);
				pop_n_elems(2);
				push_malloced_string(res);
				break;
			    }
			default:
			    error("Bad type argument to +. Had %s and %s\n",
				  type_name((sp - 1)->type), type_name(sp->type));
			}
			break;
		    }
		case T_NUMBER:
		    {
			switch (sp->type) {
			case T_NUMBER:
			    (sp - 1)->u.number = sp->u.number + (sp - 1)->u.number;
			    sp--;
			    break;
			case T_REAL:
			    (sp - 1)->type = T_REAL;
			    (sp - 1)->u.real = sp->u.real + (sp - 1)->u.number;
			    sp--;
			    break;
			case T_STRING:
			    {
				char buff[20], *res;
				int r, len;
				
				sprintf(buff, "%d", (sp - 1)->u.number);
				len = SVALUE_STRLEN(sp) + (r = strlen(buff)) + 1;
				res = DXALLOC(len, 36, "f_add: 3");
				strcpy(res, buff);
				strcpy(res + r, sp->u.string);
				pop_n_elems(2);
				push_malloced_string(res);
				break;
			    }
			default:
			    error("Bad type argument to +. Had %s and %s\n",
				  type_name((sp - 1)->type), type_name(sp->type));
			}
			break;
		    }
		case T_POINTER:
		    if (sp->type != T_POINTER) {
			error("Bad type argument to +. Had %s and %s\n",
			      type_name((sp - 1)->type), type_name(sp->type));
		    } else {
			struct vector *vec;
			
			/* add_array now free's the vectors */
			vec = add_array((sp - 1)->u.vec, sp->u.vec);
			sp--;
			sp->u.vec = vec;
			break;
		    }
		case T_MAPPING:
		    if (sp->type == T_MAPPING) {
			struct mapping *map;
			
			map = add_mapping((sp - 1)->u.map, sp->u.map);
			pop_n_elems(2);
			push_refed_mapping(map);
		    } else {
			error("Bad type argument to +. Had %s and %s\n",
			      type_name((sp - 1)->type), type_name(sp->type));
		    }
		    break;
		default:
		    error("Bad type argument to +. Had %s and %s\n",
			  type_name((sp - 1)->type), type_name(sp->type));
		}
		break;
	    }
	case I(F_VOID_ADD_EQ):
	case I(F_ADD_EQ):
	    {
		struct svalue *argp;

		DEBUG_CHECK(sp->type != T_LVALUE,
			    "non-lvalue argument to +=\n");
		argp = sp->u.lvalue;
		sp--;	/* points to RHS */
		switch (argp->type) {
		case T_STRING:
		    {
			char *new_str;
			
			if (sp->type == T_STRING) {
			    char *res;
			    int r = strlen(argp->u.string);
			    int len = r + strlen(sp->u.string) + 1;

			    if (argp->subtype == STRING_MALLOC) {
				res = (char *) DREALLOC(argp->u.string, len, 34, "f_add_eq: 1");
				(void) strcpy(res+r, sp->u.string);
				free_string_svalue(sp);
				argp->u.string = res;
				break;
			    }
			    new_str = (char *) DXALLOC(len, 34, "f_add_eq: 1");
			    (void) strcpy(new_str, argp->u.string);
			    (void) strcpy(new_str + r, sp->u.string);
			    free_string_svalue(sp);	/* free the RHS */
			} else if (sp->type == T_NUMBER) {
			    char buff[20];
			    int r, len;
			    
			    sprintf(buff, "%d", sp->u.number);
			    len = (r = SVALUE_STRLEN(argp)) + strlen(buff) + 1;
			    new_str = DXALLOC(len, 46, "f_add_eq: 2");
			    strcpy(new_str, argp->u.string);
			    strcpy(new_str + r, buff);
			    /* RHS is a number, doesn't need freed */
			} else if (sp->type == T_REAL) {
			    char buff[20];
			    int r, len;
			    
			    sprintf(buff, "%f", sp->u.real);
			    len = (r = SVALUE_STRLEN(argp)) + strlen(buff) + 1;
			    new_str = DXALLOC(len, 46, "f_add_eq: 2");
			    strcpy(new_str, argp->u.string);
			    strcpy(new_str + r, buff);
			    /* RHS is a real, doesn't need freed */
			} else {
			    bad_argument(sp, T_STRING | T_NUMBER | T_REAL, 2, instruction);
			}
			free_string_svalue(argp);	/* free the LHS */
			argp->subtype = STRING_MALLOC;
			argp->u.string = new_str;
		    }
		    break;
		case T_NUMBER:
		    if (sp->type == T_NUMBER) {
			argp->u.number += sp->u.number;
			/* both sides are numbers, no freeing required */
		    } else if (sp->type == T_REAL) {
			argp->u.number += sp->u.real;
			/* both sides are numerics, no freeing required */
		    } else {
			error("Left hand side of += is a number (or zero); right side is not a number.\n");
		    }
		    break;
		case T_REAL:
		    if (sp->type == T_NUMBER) {
			argp->u.real += sp->u.number;
			/* both sides are numerics, no freeing required */
		    }
		    if (sp->type == T_REAL) {
			argp->u.real += sp->u.real;
			/* both sides are numerics, no freeing required */
		    } else {
			error("Left hand side of += is a number (or zero); right side is not a number.\n");
		    }
		    break;
		case T_BUFFER:
		    if (sp->type != T_BUFFER) {
			bad_argument(sp, T_BUFFER, 2, instruction);
		    } else {
			struct buffer *b;
			
			b = allocate_buffer(argp->u.buf->size + sp->u.buf->size);
			memcpy(b->item, argp->u.buf->item, argp->u.buf->size);
			memcpy(b->item + argp->u.buf->size, sp->u.buf->item,
			       sp->u.buf->size);
			free_buffer(sp->u.buf);
			free_buffer(argp->u.buf);
			argp->u.buf = b;
		    }
		    break;
		    case T_POINTER:
		    if (sp->type != T_POINTER)
			bad_argument(sp, T_POINTER, 2, instruction);
		    else {
			struct vector *v;
			
			/* add_array now frees the vectors */
			v = add_array(argp->u.vec, sp->u.vec);
			argp->u.vec = v;
		    }
		    break;
		case T_MAPPING:
		    if (sp->type != T_MAPPING)
			bad_argument(sp, T_MAPPING, 2, instruction);
		    else {
			absorb_mapping(argp->u.map, sp->u.map);
			free_mapping(sp->u.map);	/* free RHS */
			/* LHS not freed because its being reused */
		    }
		    break;
		default:
		    bad_arg(1, instruction);
		}
		
		if (instruction == F_ADD_EQ) {	/* not void add_eq */
		    assign_svalue_no_free(sp, argp);
		} else {
		    /*
		     * but if (void)add_eq then no need to produce an
		     * rvalue
		     */
		    sp--;
		}
	    }
	    break;
	case I(F_AND):
	    f_and();
	    break;
	case I(F_AND_EQ):
	    f_and_eq();
	    break;
	case I(F_FUNCTION_CONSTRUCTOR):
	    f_function_constructor();
	    break;
#ifndef NEW_FUNCTIONS
	case I(F_EVALUATE):
	    f_evaluate();
	    break;
#endif
	case I(F_AGGREGATE):
	    {
		struct vector *v;
		unsigned short num;
		int i;
		
		((char *) &num)[0] = pc[0];
		((char *) &num)[1] = pc[1];
		pc += 2;
		v = allocate_array((int) num);
		/*
		 * transfer svalues in reverse...popping stack as we go
		 */
		for (i = num; i--; sp--)
		    v->item[i] = *sp;
		push_refed_vector(v);
	    }
	    break;
	case I(F_AGGREGATE_ASSOC):
            {
		unsigned short num;
		struct mapping *m;
		
		((char *) &num)[0] = *pc++;
		((char *) &num)[1] = *pc++;
		
		m = load_mapping_from_aggregate(sp -= num, num);
		(++sp)->type = T_MAPPING;
		sp->u.map = m;
		break;
	    }
	case I(F_ASSIGN):
	    if (sp->type == T_LVALUE_BYTE) {
		if ((sp - 1)->type != T_NUMBER) {
		    *sp->u.lvalue_byte = 0;
		} else {
		    *sp->u.lvalue_byte = ((sp - 1)->u.number & 0xff);
		}
	    } else if (sp->type == T_LVALUE) {
		assign_svalue(sp->u.lvalue, sp - 1);
#ifdef DEBUG
	    } else {
		fatal("Bad argument to F_ASSIGN\n");
#endif
	    }
	    sp--;		/* ignore lvalue */
	    /* rvalue is already in the correct place */
	    break;
	case I(F_VOID_ASSIGN):
	    if ((sp - 1)->type == T_STRING) {
		if ((sp - 1)->subtype == STRING_MALLOC ||
		    (sp - 1)->subtype == STRING_SHARED) {
		    /*
		     * avoid unnecessary (and costly)
		     * string_copy()...FREE() or
		     * ref_string()...free_string()
		     */
		    free_svalue(sp->u.lvalue, "F_VOID_ASSIGN");
		    *(sp->u.lvalue) = *(sp - 1);	/* copy string directly */
		    sp -= 2;/* don't free copied string! */
		    break;
		} else {
		    assign_svalue(sp->u.lvalue, sp - 1);
		}
	    } else if ((sp - 1)->type != T_INVALID) {
		if (sp->type == T_LVALUE_BYTE) {
		    if ((sp - 1)->type != T_NUMBER) {
			*sp->u.lvalue_byte = 0;
		    } else {
			*sp->u.lvalue_byte = ((sp - 1)->u.number & 0xff);
		    }
		} else if (sp->type == T_LVALUE) {
		    assign_svalue(sp->u.lvalue, sp - 1);
#ifdef DEBUG
		} else {
		    debug_message("sp->type: %d, (sp-1)->type: %d, (sp-2)->type: %d\n",
				  sp->type, (sp - 1)->type, (sp - 2)->type);
		    fatal("Bad argument to F_VOID_ASSIGN\n");
#endif
		}
	    }
	    pop_n_elems(2);
	    break;
	case I(F_BREAK_POINT):
	    break_point();
	    break;
	case I(F_BREAK):
	    pc = current_prog->p.i.program + *break_sp++;
	    break;
	case I(F_CALL_FUNCTION_BY_ADDRESS):
	    {
		unsigned short func_index;
		struct function *funp;
		
		((char *) &func_index)[0] = pc[0];
		((char *) &func_index)[1] = pc[1];
		pc += 2;
		func_index += function_index_offset;
		/*
		 * Find the function in the function table. As the
		 * function may have been redefined by inheritance, we
		 * must look in the last table, which is pointed to by
		 * current_object.
		 */
		DEBUG_CHECK(func_index >= current_object->prog->p.i.num_functions,
			    "Illegal function index\n");
		
		funp = &current_object->prog->p.i.functions[func_index];
		
		if (funp->flags & NAME_UNDEFINED)
		    error("Undefined function: %s\n", funp->name);
		/* Save all important global stack machine registers */
		push_control_stack(funp);	/* return pc is adjusted
						 * later */
		
		caller_type = ORIGIN_LOCAL;
		/* This assigment must be done after push_control_stack() */
		current_prog = current_object->prog;
		/*
		 * If it is an inherited function, search for the real
		 * definition.
		 */
		csp->num_local_variables = EXTRACT_UCHAR(pc);
		pc++;
		funp = setup_new_frame(funp);
		csp->pc = pc;	/* The corrected return address */
#ifdef LPC_TO_C
		if (current_prog->p.i.program_size) {
#endif
		    pc = current_prog->p.i.program + funp->offset;
		    csp->extern_call = 0;
#ifdef LPC_TO_C
		} else {
		    struct svalue ret =
			{T_NUMBER};
		    DEBUG_CHECK(!(funp->offset),
				"Null function pointer in jump_table.\n");
		    (*
		     (void (*) ()) (funp->offset)
		     ) (&ret);
		    *sp++ = ret;
		}
#endif
	    }
	    break;
	case I(F_COMPL):
	    if (sp->type != T_NUMBER)
		error("Bad argument to ~\n");
	    sp->u.number = ~sp->u.number;
	    break;
	case I(F_CONST0):
	    push_number(0);
	    break;
	case I(F_CONST1):
	    push_number(1);
	    break;
	case I(F_PRE_DEC):
	    DEBUG_CHECK(sp->type != T_LVALUE, 
			"non-lvalue argument to --\n");
	    if (sp->u.lvalue->type == T_NUMBER) {
		sp->u.lvalue->u.number--;
		assign_svalue(sp, sp->u.lvalue);
		break;
	    } else if (sp->u.lvalue->type == T_REAL) {
		sp->u.lvalue->u.real--;
		assign_svalue(sp, sp->u.lvalue);
		break;
	    } else {
		error("-- of non-numeric argument\n");
	    }
	    break;
	case I(F_DEC):
	    DEBUG_CHECK(sp->type != T_LVALUE,
			"non-lvalue argument to --\n");
	    if (sp->u.lvalue->type == T_NUMBER) {
		sp->u.lvalue->u.number--;
	    } else if (sp->u.lvalue->type == T_REAL) {
		sp->u.lvalue->u.real--;
	    } else {
		error("-- of non-numeric argument\n");
	    }
	    sp--;
	    break;
	case I(F_DIVIDE):
	    { 
		switch((sp-1)->type|sp->type){
		    
		case T_NUMBER:
		    {
			if (!(sp--)->u.number) error("Division by zero\n");
			sp->u.number /= (sp+1)->u.number;
			break;
		    }
		    
		case T_REAL:
		    {
			if ((sp--)->u.real == 0.0) error("Division by zero\n");
			sp->u.real /= (sp+1)->u.real;
			break;
		    }
		    
		case T_NUMBER|T_REAL:
		    {
			if ((sp--)->type & T_NUMBER){
			    sp->u.real /= (sp+1)->u.number;
			} else {
			    sp->type = T_REAL;
			    sp->u.real = sp->u.number / (sp+1)->u.real;
			}
			break;
		    }
		    
		default:
		    {
			if (!((sp-1)->type & (T_NUMBER|T_REAL)))
			    bad_argument(sp-1,T_NUMBER|T_REAL,1, instruction);
			if (!(sp->type & (T_NUMBER|T_REAL)))
			    bad_argument(sp, T_NUMBER|T_REAL,2, instruction);
		    }
		}
	    }
	    break;
	case I(F_DIV_EQ):
	    f_div_eq();
	    break;
	case I(F_EQ):
	    f_eq();
	    break;
	case I(F_GE):
	    {
		if ((sp - 1)->type == T_NUMBER) {
		    if (sp->type == T_NUMBER) {
			sp--;
			sp->u.number = (sp->u.number >= (sp+1)->u.number);
			break;
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.number >= sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type == T_REAL) {
		    if (sp->type == T_NUMBER) {
			i = ((sp - 1)->u.real >= sp->u.number);
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.real >= sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type != T_STRING) {
		    bad_argument(sp - 1, T_NUMBER | T_REAL | T_STRING, 1, instruction);
		} else if (sp->type != T_STRING) {
		    bad_argument(sp, T_STRING, 2, instruction);
		} else {
		    i = (strcmp((sp - 1)->u.string, sp->u.string) >= 0);
		}
	    }
	    pop_n_elems(2);
	    push_number(i);
	    break;
	case I(F_GT):
	    {
		if ((sp - 1)->type == T_NUMBER) {
		    if (sp->type == T_NUMBER) {
			sp--;
			sp->u.number = (sp->u.number > (sp+1)->u.number);
			break;
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.number > sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type == T_REAL) {
		    if (sp->type == T_NUMBER) {
			i = ((sp - 1)->u.real > sp->u.number);
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.real > sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type != T_STRING) {
		    bad_argument(sp - 1, T_STRING | T_NUMBER | T_REAL, 1, instruction);
		} else if (sp->type != T_STRING) {
		    bad_argument(sp, T_STRING, 2, instruction);
		} else {
		    i = (strcmp((sp - 1)->u.string, sp->u.string) > 0);
		}
	    }
	    pop_n_elems(2);
	    push_number(i);
	    break;
	case I(F_GLOBAL):
	    {
		struct svalue *s;
		
		s = find_value((int) (EXTRACT_UCHAR(pc) + variable_index_offset));
		sp++;
		pc++;
		
		/*
		 * If variable points to a destructed object, replace it
		 * with 0, otherwise, fetch value of variable.
		 */
		if ((s->type == T_OBJECT) && (s->u.ob->flags & O_DESTRUCTED)) {
		    *sp = const0;
		    assign_svalue(s, &const0);
		} else {
		    assign_svalue_no_free(sp, s);
		}
		break;
	    }
	case I(F_PRE_INC):
	    DEBUG_CHECK(sp->type != T_LVALUE,
			"non-lvalue argument to ++\n");
	    if (sp->u.lvalue->type == T_NUMBER) {
		sp->u.lvalue->u.number++;
		assign_svalue(sp, sp->u.lvalue);
		break;
	    } else if (sp->u.lvalue->type == T_REAL) {
		sp->u.lvalue->u.real++;
		assign_svalue(sp, sp->u.lvalue);
		break;
	    } else {
		error("++ of non-numeric argument\n");
	    }
	    break;
	case I(F_INDEX):
	    if ((sp - 1)->type == T_MAPPING) {
		struct svalue *v;
		struct mapping *m;
		
		v = find_in_mapping((sp - 1)->u.map, sp);
		pop_stack();/* free b from a[b] */
		m = sp->u.map;
		assign_svalue_no_free(sp, v);	/* v will always have a
						 * value */
		free_mapping(m);
	    } else if ((sp - 1)->type == T_BUFFER) {
		struct svalue w;
		int idx;
		
		if (sp->type != T_NUMBER) {
		    error("Indexing a buffer with an illegal type.\n");
		}
		idx = sp->u.number;
		pop_stack();
		w.type = T_NUMBER;
		w.subtype = 0;
		if ((idx > sp->u.buf->size) || (idx < 0)) {
		    error("Buffer index out of bounds.\n");
		}
		w.u.number = sp->u.buf->item[idx];
		assign_svalue_no_free(sp, &w);
	    } else if ((sp - 1)->type == T_STRING) {
		struct svalue w;
		int idx;
		
		if (sp->type != T_NUMBER) {
		    error("Indexing a string with an illegal type.\n");
		}
		idx = sp->u.number;
		pop_stack();
		w.type = T_NUMBER;
		w.subtype = 0;
		if ((idx > SVALUE_STRLEN(sp)) || (idx < 0)) {
		    error("String index out of bounds.\n");
		}
		w.u.number = (unsigned char) sp->u.string[idx];
		assign_svalue(sp, &w);
	    } else {
		push_indexed_lvalue();
		assign_svalue_no_free(sp, sp->u.lvalue);
	    }
	    /*
	     * Fetch value of a variable. It is possible that it is a
	     * variable that points to a destructed object. In that case,
	     * it has to be replaced by 0.
	     */
	    if (sp->type == T_OBJECT && (sp->u.ob->flags & O_DESTRUCTED)) {
		free_svalue(sp, "F_INDEX");
		sp->type = T_NUMBER;
		sp->u.number = 0;
	    }
	    break;
#ifdef F_JUMP_WHEN_ZERO
	case I(F_JUMP_WHEN_ZERO):
	    if ((i = (sp->type == T_NUMBER)) && sp->u.number == 0) {
		((char *) &offset)[0] = pc[0];
		((char *) &offset)[1] = pc[1];
		pc = current_prog->p.i.program + offset;
	    } else {
		pc += 2;
	    }
	    if (i) {
		sp--;	/* cheaper to do this when sp is an integer
			 * svalue */
	    } else {
		pop_stack();
	    }
	    break;
#endif
#ifdef F_JUMP
	case I(F_JUMP):
	    ((char *) &offset)[0] = pc[0];
	    ((char *) &offset)[1] = pc[1];
	    pc = current_prog->p.i.program + offset;
	    break;
#endif
	case I(F_LE):
	    {
		if ((sp - 1)->type == T_NUMBER) {
		    if (sp->type == T_NUMBER) {
			sp--;
			sp->u.number = (sp->u.number <= (sp+1)->u.number);
			break;
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.number <= sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type == T_REAL) {
		    if (sp->type == T_NUMBER) {
			i = ((sp - 1)->u.real <= sp->u.number);
		    } else if (sp->type == T_REAL) {
			i = ((sp - 1)->u.real <= sp->u.real);
		    } else {
			bad_argument(sp, T_NUMBER | T_REAL, 2, instruction);
		    }
		} else if ((sp - 1)->type != T_STRING) {
		    bad_argument(sp, T_NUMBER | T_STRING | T_REAL, 1, instruction);
		} else if (sp->type != T_STRING) {
		    bad_argument(sp, T_STRING, 2, instruction);
		} else {
		    i = (strcmp((sp - 1)->u.string, sp->u.string) <= 0);
		}
	    }
	    pop_n_elems(2);
	    push_number(i);
	    break;
	case I(F_LSH):
	    f_lsh();
	    break;
	case I(F_LSH_EQ):
	    f_lsh_eq();
	    break;
	case I(F_MOD):
	    {
		CHECK_TYPES(sp - 1, T_NUMBER, 1, instruction);
		CHECK_TYPES(sp, T_NUMBER, 2, instruction);
		if (sp->u.number == 0)
		    error("Modulus by zero.\n");
		i = (sp - 1)->u.number % sp->u.number;
		sp--;
		sp->u.number = i;
	    }
	    break;
	case I(F_MOD_EQ):
	    f_mod_eq();
	    break;
	case I(F_MULTIPLY):
	    {
		switch((sp-1)->type|sp->type){
		case T_NUMBER:
		    {
			sp--;
			sp->u.number *= (sp+1)->u.number;
			break;
		    }
		    
		case T_REAL:
		    {
			sp--;
			sp->u.real *= (sp+1)->u.real;
			break;
		    }
		    
		case T_NUMBER|T_REAL:
		    {
			if ((--sp)->type & T_NUMBER){
			    sp->type = T_REAL;
			    sp->u.real = sp->u.number * (sp+1)->u.real;
			}
			else sp->u.real *= (sp+1)->u.number;
			break;
		    }
		    
		case T_MAPPING:
		    {
			struct mapping *m;
			m = compose_mapping((sp-1)->u.map, sp->u.map, 1);
			if ((sp-1)->type == T_MAPPING) fprintf(stderr,"map1\n");
			if (sp->type == T_MAPPING) fprintf(stderr, "map 2\n");
			fflush(stderr);
			pop_n_elems(2);
			(++sp)->type = T_MAPPING;
			sp->u.map = m;
			break;
		    }
		    
		default:
		    {
			if (!((sp-1)->type & (T_NUMBER|T_REAL|T_MAPPING)))
			    bad_argument(sp-1, T_NUMBER|T_REAL|T_MAPPING,1, instruction);
			if (!(sp->type & (T_NUMBER|T_REAL|T_MAPPING)))
			    bad_argument(sp, T_NUMBER|T_REAL|T_MAPPING,2, instruction);
			error("Args to * are not compatible.\n");
		    }
		}
	    }
	    break;
	case I(F_MULT_EQ):
	    f_mult_eq();
	    break;
	case I(F_NE):
	    f_ne();
	    break;
	case I(F_NEGATE):
	    if (sp->type == T_NUMBER)
		sp->u.number = -sp->u.number;
	    else if (sp->type == T_REAL)
		sp->u.real = -sp->u.real;
	    else
		error("Bad argument to unary minus\n");
	    break;
	case I(F_NOT):
	    if (sp->type == T_NUMBER && sp->u.number == 0)
		sp->u.number = 1;
	    else
		assign_svalue(sp, &const0);
	    break;
	case I(F_OR):
	    f_or();
	    break;
	case I(F_OR_EQ):
	    f_or_eq();
	    break;
	case I(F_PARSE_COMMAND):
	    f_parse_command();
	    break;
	case I(F_POP_VALUE):
	    pop_stack();
	    break;
	case I(F_POP_BREAK):
	    i = EXTRACT_UCHAR(pc);
	    break_sp += i;
	    pc++;
	    break;
	case I(F_POST_DEC):
	    DEBUG_CHECK(sp->type != T_LVALUE,
			"non-lvalue argument to --\n");
	    if (sp->u.lvalue->type == T_NUMBER) {
		sp->u.lvalue->u.number--;
		assign_svalue(sp, sp->u.lvalue);
		sp->u.number++;
		break;
	    } else if (sp->u.lvalue->type == T_REAL) {
		sp->u.lvalue->u.real--;
		assign_svalue(sp, sp->u.lvalue);
		sp->u.real++;
		break;
	    } else {
		error("-- of non-numeric argument\n");
	    }
	    break;
	case I(F_POST_INC):
	    DEBUG_CHECK(sp->type != T_LVALUE,
			"non-lvalue argument to ++\n");
	    if (sp->u.lvalue->type == T_NUMBER) {
		sp->u.lvalue->u.number++;
		assign_svalue(sp, sp->u.lvalue);
		sp->u.number--;
		break;
	    } else if (sp->u.lvalue->type == T_REAL) {
		sp->u.lvalue->u.real++;
		assign_svalue(sp, sp->u.lvalue);
		sp->u.real--;
		break;
	    } else {
		error("++ of non-numeric argument\n");
	    }
	    break;
	case I(F_GLOBAL_LVALUE):
	    sp++;
	    sp->type = T_LVALUE;
	    sp->u.lvalue = find_value((int) (EXTRACT_UCHAR(pc) +
					     variable_index_offset));
	    pc++;
	    break;
	case I(F_INDEXED_LVALUE):
	    push_indexed_lvalue();
	    break;
	case I(F_RANGE):
	    f_range();
	    break;
	case I(F_RETURN):
	    {
		struct svalue sv;
		
		sv = *sp--;
		/*
		 * Deallocate frame and return.
		 */
		pop_n_elems(csp->num_local_variables);
		sp++;
		DEBUG_CHECK(sp != fp, "Bad stack at F_RETURN\n");
		*sp = sv;	/* This way, the same ref counts are
				 * maintained */
		pop_control_stack();
#ifdef TRACE
		tracedepth--;
		if (TRACEP(TRACE_RETURN)) {
		    do_trace("Return", "", "");
		    if (TRACEHB) {
			if (TRACETST(TRACE_ARGS)) {
			    add_message(" with value: ");
			    print_svalue(sp);
			}
			add_message("\n");
		    }
		}
#endif
		/* The control stack was popped just before */
		if (csp[1].extern_call)
		    return;
		break;
	    }
	case I(F_RSH):
	    f_rsh();
	    break;
	case I(F_RSH_EQ):
	    f_rsh_eq();
	    break;
	case I(F_SSCANF):
	    f_sscanf();
	    break;
	case I(F_STRING):
	    ((char *) &string_number)[0] = pc[0];
	    ((char *) &string_number)[1] = pc[1];
	    pc += 2;
	    sp++;
	    DEBUG_CHECK1(string_number >= current_prog->p.i.num_strings,
			"string %d out of range in F_STRING!\n",
			 string_number);
	    sp->type = T_STRING;
	    sp->subtype = STRING_SHARED;
	    ref_string(sp->u.string = current_prog->p.i.strings[string_number]);
	    break;
	case I(F_SUBTRACT):
	    {
		i = (sp--)->type;
		switch (i | sp->type) {
		case T_NUMBER:
		    sp->u.number -= (sp+1)->u.number;
		    break;

		case T_REAL:
		    sp->u.real -= (sp+1)->u.real;
		    break;

		case T_NUMBER | T_REAL:
		    if (sp->type & T_REAL) sp->u.real -= (sp+1)->u.number;
		    else {
			sp->type = T_REAL;
			sp->u.real = sp->u.number - (sp+1)->u.real;
		    }
		    break;

		case T_POINTER:
		    {
			struct vector *v, *w;

			v = (sp+1)->u.vec;
			if (v->ref > 1) {
			    v = slice_array(v, 0, v->size - 1);
			    (sp+1)->u.vec->ref--;
			}
			/*
			 * subtract_array already takes care of
			 * destructed objects
			 */
			w = subtract_array(sp->u.vec, v);
			free_vector(v);
			free_vector(sp->u.vec);
			sp->u.vec = w;
			break;
		    }
		    
		default:
		    if (!((sp++)->type & (T_NUMBER|T_REAL|T_POINTER)))
			error("Bad left type to -.\n");
		    else if (!(sp->type & (T_NUMBER|T_REAL|T_POINTER)))
			error("Bad right type to -.\n");
		    else error("Arguments to - do not have compatible types.\n");
		}
		break;
	    }
	case I(F_SUB_EQ):
	    f_sub_eq();
	    break;
	case I(F_SIMUL_EFUN):
	    f_simul_efun();
	    break;
	case I(F_SWITCH):
	    f_switch();
	    break;
	case I(F_XOR):
	    f_xor();
	    break;
	case I(F_XOR_EQ):
	    f_xor_eq();
	    break;
	case I(F_CATCH):
	    {
		unsigned short new_pc_offset;
		
		/*
		 * Compute address of next instruction after the CATCH
		 * statement.
		 */
		((char *) &new_pc_offset)[0] = pc[0];
		((char *) &new_pc_offset)[1] = pc[1];
		new_pc_offset = pc + new_pc_offset - current_prog->p.i.program;
		pc += 2;
		
		do_catch(pc, new_pc_offset);
		
		pc = current_prog->p.i.program + new_pc_offset;
		
		break;
	    }
	case I(F_END_CATCH):
	    {
		pop_stack();/* discard expression value */
		free_svalue(&catch_value, "F_END_CATCH");
		catch_value.type = T_NUMBER;
		catch_value.u.number = 0;
		/* We come here when no longjmp() was executed */
		pop_control_stack();
		push_pop_error_context(0);
		push_number(0);
		return;	/* return to do_catch */
	    }
	case I(F_TIME_EXPRESSION):
	    {
		long sec, usec;
		
		get_usec_clock(&sec, &usec);
		push_number(sec);
		push_number(usec);
		break;
	    }
	case I(F_END_TIME_EXPRESSION):
	    {
		long sec, usec;
		
		get_usec_clock(&sec, &usec);
		usec = (sec - (sp - 2)->u.number) * 1000000 + (usec - (sp - 1)->u.number);
		pop_stack();
		sp -= 2;
		push_number(usec);
		break;
	    }
#ifdef NEEDS_CALL_EXTRA
	case I(F_CALL_EXTRA):
	    instruction = EXTRACT_UCHAR(pc++) + 0xff;
#endif
	default:
	    /* We have an efun.  Execute it
	     * Check the types of the first two args first
	     */
	    if (instrs[instruction].min_arg != instrs[instruction].max_arg)
		num_arg = EXTRACT_UCHAR(pc++);
	    else
		num_arg = instrs[instruction].min_arg;
#ifdef DEBUG
	    if (instruction > NUM_OPCODES) {
		debug_fatal("Undefined instruction %s (%d)\n",
			    get_f_name(instruction), instruction);
	    }
	    if (instruction < BASE) {
		debug_fatal("No case for eoperator %s (%d)\n",
			    get_f_name(instruction), instruction);
	    }
	    expected_stack = sp - num_arg + 1;
#endif
	    if (num_arg > 0) {
		CHECK_TYPES(sp - num_arg + 1, instrs[instruction].type[0], 1, instruction);
		if (num_arg > 1) {
		    CHECK_TYPES(sp - num_arg + 2, instrs[instruction].type[1], 2, instruction);
		}
	    }
	    (*oefun_table[instruction]) (num_arg, instruction);
	    DEBUG_CHECK2(expected_stack != sp,
		 "Bad stack after evaluation. Instruction %d, num arg %d\n",
			 instruction, num_arg);
	}  /* switch (instruction) */
	DEBUG_CHECK2(sp < fp + csp->num_local_variables - 1,
		 "Bad stack after evaluation. Instruction %d, num arg %d\n",
		     instruction, num_arg);
    }   /* while (1) */
}

static void
do_catch P2(char *, pc, unsigned short, new_pc_offset)
{
    extern jmp_buf error_recovery_context;
    extern int error_recovery_context_exists;

    push_control_stack(0);
    /* next two probably not necessary... */
    csp->pc = current_prog->p.i.program + new_pc_offset;
    csp->num_local_variables = (csp - 1)->num_local_variables;	/* marion */
    /*
     * Save some global variables that must be restored separately after a
     * longjmp. The stack will have to be manually popped all the way.
     */
    push_pop_error_context(1);

    /* signal catch OK - print no err msg */
    error_recovery_context_exists = 2;
    if (SETJMP(error_recovery_context)) {
	/*
	 * They did a throw() or error. That means that the control stack
	 * must be restored manually here.
	 */
	push_pop_error_context(-1);
	pop_control_stack();
	sp++;
	*sp = catch_value;
	assign_svalue_no_free(&catch_value, &const1);
	
	/* if it's too deep or max eval, we can't let them catch it */
	if (max_eval_error)
	    error("Can't catch eval cost too big error.\n");
	if (too_deep_error)
	    error("Can't catch too deep recursion error.\n");
    } else {
	assign_svalue(&catch_value, &const1);
	/* note, this will work, since csp->extern_call won't be used */
	eval_instruction(pc);
    }
}

/*
 * Apply a fun 'fun' to the program in object 'ob', with
 * 'num_arg' arguments (already pushed on the stack).
 * If the function is not found, search in the object pointed to by the
 * inherit pointer.
 * If the function name starts with '::', search in the object pointed out
 * through the inherit pointer by the current object. The 'current_object'
 * stores the base object, not the object that has the current function being
 * evaluated. Thus, the variable current_prog will normally be the same as
 * current_object->prog, but not when executing inherited code. Then,
 * it will point to the code of the inherited object. As more than one
 * object can be inherited, the call of function by index number has to
 * be adjusted. The function number 0 in a superclass object must not remain
 * number 0 when it is inherited from a subclass object. The same problem
 * exists for variables. The global variables function_index_offset and
 * variable_index_offset keep track of how much to adjust the index when
 * executing code in the superclass objects.
 *
 * There is a special case when called from the heart beat, as
 * current_prog will be 0. When it is 0, set current_prog
 * to the 'ob->prog' sent as argument.
 *
 * Arguments are always removed from the stack.
 * If the function is not found, return 0 and nothing on the stack.
 * Otherwise, return 1, and a pushed return value on the stack.
 *
 * Note that the object 'ob' can be destructed. This must be handled by
 * the caller of apply().
 *
 * If the function failed to be called, then arguments must be deallocated
 * manually !  (Look towards end of this function.)
 */

#ifdef DEBUG
static char debug_apply_fun[30];/* For debugging */
#endif

#ifdef CACHE_STATS
unsigned int apply_low_call_others = 0;
unsigned int apply_low_cache_hits = 0;
unsigned int apply_low_slots_used = 0;
unsigned int apply_low_collisions = 0;
#endif

typedef struct cache_entry_s {
    int id;
    char *name;
    struct function *pr;
    struct function *pr_inherited;
    struct program *progp;
    struct program *oprogp;
    int function_index_offset;
    int variable_index_offset;
}             cache_entry_t;

int apply_low P3(char *, fun, struct object *, ob, int, num_arg)
{
    /*
     * static memory is initialized to zero by the system or so Jacques says
     * :)
     */
    static cache_entry_t cache[APPLY_CACHE_SIZE];
    cache_entry_t *entry;
    struct function *pr;
    struct program *progp;
    extern int num_error;
    int ix;
    static int cache_mask = APPLY_CACHE_SIZE - 1;
    char *funname;
    int i;
    int local_call_origin = call_origin;
    IF_DEBUG(struct control_stack *save_csp);

    if (!local_call_origin)
	local_call_origin = ORIGIN_DRIVER;
    call_origin = 0;
    ob->time_of_ref = current_time;	/* Used by the swapper */
    /*
     * This object will now be used, and is thus a target for reset later on
     * (when time due).
     */
#ifdef LAZY_RESETS
    try_reset(ob);
    if ((ob->flags & O_DESTRUCTED) && (num_error <= 0)) {
	pop_n_elems(num_arg);
	return 0;
    }
#endif
    ob->flags &= ~O_RESET_STATE;
#ifdef DEBUG
    strncpy(debug_apply_fun, fun, sizeof(debug_apply_fun));
    debug_apply_fun[sizeof debug_apply_fun - 1] = '\0';
#endif
    if (num_error <= 0) {	/* !failure */
	/*
	 * If there is a chain of objects shadowing, start with the first of
	 * these.
	 */
#ifndef NO_SHADOWS
	while (ob->shadowed && ob->shadowed != current_object)
	    ob = ob->shadowed;
#endif
      retry_for_shadow:
	if (ob->flags & O_SWAPPED)
	    load_ob_from_swap(ob);
	progp = ob->prog;
	DEBUG_CHECK(ob->flags & O_DESTRUCTED,"apply() on destructed object\n");
#ifdef CACHE_STATS
	apply_low_call_others++;
#endif
	ix = (progp->p.i.id_number ^ (int) fun ^
	      ((int) fun >> APPLY_CACHE_BITS)) & cache_mask;
	entry = &cache[ix];
	if ((entry->id == progp->p.i.id_number)
	    && (!entry->progp || (entry->oprogp == ob->prog))
	    && !strcmp(entry->name, fun)) {
	    /*
	     * We have found a matching entry in the cache. The pointer to
	     * the function name has to match, not only the contents. This is
	     * because hashing the string in order to get a cache index would
	     * be much more costly than hashing it's pointer. If cache access
	     * would be costly, the cache would be useless.
	     */
#ifdef CACHE_STATS
	    apply_low_cache_hits++;
#endif
	    if (entry->progp 
		&& (!(entry->pr->type & (TYPE_MOD_STATIC | TYPE_MOD_PRIVATE))
		    || current_object == ob || (local_call_origin & (ORIGIN_DRIVER | ORIGIN_CALL_OUT)))) {
		/*
		 * the cache will tell us in which program the function is,
		 * and where
		 */
		push_control_stack(entry->pr);
		caller_type = local_call_origin;
		csp->num_local_variables = num_arg;
		current_prog = entry->progp;
		pr = entry->pr_inherited;
		function_index_offset = entry->function_index_offset;
		variable_index_offset = entry->variable_index_offset;
		/* Remove excessive arguments */
		if ((i = csp->num_local_variables - (int) pr->num_arg) > 0) {
		    pop_n_elems(i);
		    csp->num_local_variables = pr->num_arg;
		}
		/* Correct number of arguments and local variables */
		while (csp->num_local_variables < (int) (pr->num_arg + pr->num_local)) {
		    push_null();
		    csp->num_local_variables++;
		}
#ifdef TRACE
		tracedepth++;
		if (TRACEP(TRACE_CALL)) {
		    do_trace_call(pr);
		}
#endif
		fp = sp - csp->num_local_variables + 1;
		break_sp = (short *) (sp + 1);
#ifdef OLD_PREVIOUS_OBJECT_BEHAVIOUR
		/*
		 * Now, previous_object() is always set, even by
		 * call_other(this_object()). It should not break any
		 * compatibility.
		 */
		if (current_object != ob)
#endif
		    previous_ob = current_object;
		current_object = ob;
		IF_DEBUG(save_csp = csp);
		call_program(current_prog, pr->offset);

		DEBUG_CHECK(save_csp - 1 != csp, 
			    "Bad csp after execution in apply_low.\n");
		/*
		 * Arguments and local variables are now removed. One
		 * resulting value is always returned on the stack.
		 */
		return 1;
	    }			/* when we come here, the cache has told us
				 * that the function isn't defined in the
				 * object */
	} else {
	    /* we have to search the function */
	    if (!entry->progp && entry->id) {
		/*
		 * The old cache entry was for an undefined function, so the
		 * name had to be malloced
		 */
		FREE(entry->name);
	    }
#ifdef CACHE_STATS
	    if (!entry->id) {
		apply_low_slots_used++;
	    } else {
		apply_low_collisions++;
	    }
#endif
	    /*
	     * All functions are shared strings.  Searching the hash table
	     * will typically take less than three string compares.  If the
	     * string isn't in the hash table then the object contains no
	     * function by that name. If the string is in the hash table then
	     * we can search for the string in the object by comparing
	     * pointers rather than using strcmp's (since shared strings are
	     * unique).  The idea for this optimization comes from the
	     * lp-strcmpoptim file on alcazar.
	     */
	    if ((funname = findstring(fun))) {
#ifdef OPTIMIZE_FUNCTION_TABLE_SEARCH
		i = lookup_function(progp->p.i.functions,
				    progp->p.i.tree_r, funname);
		/* check for NAME_COLON_COLON unnecessary b/c those names
		   don't get into the function table. */
		if (i == -1 || 
		    (progp->p.i.functions[i].flags & NAME_UNDEFINED)
		    || ((progp->p.i.functions[i].type & (TYPE_MOD_STATIC | TYPE_MOD_PRIVATE))
			&& current_object != ob && !(local_call_origin & (ORIGIN_DRIVER | ORIGIN_CALL_OUT)))) {
		    ;
		} else {
		    pr = (struct function *) & progp->p.i.functions[i];
#else
		/* comparing pointers okay since both are shared strings */
		for (pr = progp->p.i.functions;
		     pr < progp->p.i.functions + progp->p.i.num_functions;
		     pr++) {
		    if (pr->name == 0 ||
			pr->name != funname) 
			continue;
		    if (pr->flags & NAME_UNDEFINED)
			continue;
		    if ((pr->flags & NAME_COLON_COLON) && *funname != APPLY___INIT_SPECIAL_CHAR)
			continue;
		    /* Static functions may not be called from outside. */
		    if ((pr->type & (TYPE_MOD_STATIC | TYPE_MOD_PRIVATE)) &&
			current_object != ob && !(local_call_origin & (ORIGIN_DRIVER | ORIGIN_CALL_OUT))) {
			continue;
		    }
#endif
		    push_control_stack(pr);
		    caller_type = local_call_origin;
		    /* The searched function is found */
		    entry->id = progp->p.i.id_number;
		    entry->pr = pr;
		    entry->name = pr->name;
		    csp->num_local_variables = num_arg;
		    current_prog = progp;
		    entry->oprogp = current_prog;	/* before
							 * setup_new_frame */
		    pr = setup_new_frame(pr);
		    entry->pr_inherited = pr;
		    entry->progp = current_prog;
		    entry->variable_index_offset = variable_index_offset;
		    entry->function_index_offset = function_index_offset;
#ifdef OLD_PREVIOUS_OBJECT_BEHAVIOUR
		    if (current_object != ob)
#endif
			previous_ob = current_object;
		    current_object = ob;
		    IF_DEBUG(save_csp = csp);
		    call_program(current_prog, pr->offset);

		    DEBUG_CHECK(save_csp - 1 != csp,
				"Bad csp after execution in apply_low\n");
		    /*
		     * Arguments and local variables are now removed. One
		     * resulting value is always returned on the stack.
		     */
		    return 1;
		}
	    }
	    /* We have to mark a function not to be in the object */
	    entry->id = progp->p.i.id_number;
	    entry->name = string_copy(fun);
	    entry->progp = (struct program *) 0;
	}
#ifndef NO_SHADOWS
	if (ob->shadowing) {
	    /*
	     * This is an object shadowing another. The function was not
	     * found, but can maybe be found in the object we are shadowing.
	     */
	    ob = ob->shadowing;
	    goto retry_for_shadow;
	}
#endif
    }				/* !failure */
    /* Failure. Deallocate stack. */
    pop_n_elems(num_arg);
    return 0;
}

/*
 * Arguments are supposed to be
 * pushed (using push_string() etc) before the call. A pointer to a
 * 'struct svalue' will be returned. It will be a null pointer if the called
 * function was not found. Otherwise, it will be a pointer to a static
 * area in apply(), which will be overwritten by the next call to apply.
 * Reference counts will be updated for this value, to ensure that no pointers
 * are deallocated.
 */

static struct svalue *sapply P3(char *, fun, struct object *, ob, int, num_arg)
{
    static struct svalue ret_value = {T_NUMBER};
    IF_DEBUG(struct svalue *expected_sp);

#ifdef TRACE
    if (TRACEP(TRACE_APPLY)) {
	do_trace("Apply", "", "\n");
    }
#endif

    IF_DEBUG(expected_sp = sp - num_arg);
    if (apply_low(fun, ob, num_arg) == 0)
	return 0;
    free_svalue(&ret_value, "sapply");
    ret_value = *sp--;
    DEBUG_CHECK(expected_sp != sp,
		"Corrupt stack pointer.\n");
    return &ret_value;
}

struct svalue *apply P4(char *, fun, struct object *, ob, int, num_arg,
			int, where)
{
    tracedepth = 0;
    call_origin = where;
    return sapply(fun, ob, num_arg);
}

/*
 * this is a "safe" version of apply
 * this allows you to have dangerous driver mudlib dependencies
 * and not have to worry about causing serious bugs when errors occur in the
 * applied function and the driver depends on being able to do something
 * after the apply. (such as the ed exit function, and the net_dead function).
 * note: this function uses setjmp() and thus is fairly expensive when
 * compared to a normal apply().  Use sparingly.
 */

struct svalue *
       safe_apply P4(char *, fun, struct object *, ob, int, num_arg, int, where)
{
    jmp_buf save_error_recovery_context;
    extern jmp_buf error_recovery_context;
    struct svalue *ret;
    struct object *save_command_giver = command_giver;

    debug(32768, ("safe_apply: before sp = %d\n", sp));
    debug(32768, ("safe_apply: before csp = %d\n", csp));
    ret = NULL;
    memcpy((char *) save_error_recovery_context,
	   (char *) error_recovery_context, sizeof(error_recovery_context));
    if (!SETJMP(error_recovery_context)) {
	if (!(ob->flags & O_DESTRUCTED)) {
	    ret = apply(fun, ob, num_arg, where);
	}
    } else {
#if 1
	/*
	 * this shouldn't be needed here (would likely cause problems).
	 * 0.9.17.5 - tru
	 */
	clear_state();
#endif
	ret = NULL;
	fprintf(stderr, "Warning: Error in the '%s' function in '%s'\n",
		fun, ob->name);
	fprintf(stderr,
		"The driver may function improperly if this problem is not fixed.\n");
    }
    debug(32768, ("safe_apply: after sp = %d\n", sp));
    debug(32768, ("safe_apply: after csp = %d\n", csp));
    memcpy((char *) error_recovery_context,
      (char *) save_error_recovery_context, sizeof(error_recovery_context));
    command_giver = save_command_giver;
    return ret;
}

/*
 * Call a function in all objects in a vector.
 */
struct vector *call_all_other P3(struct vector *, v, char *, func, int, numargs)
{
    int idx;
    struct svalue *tmp;
    struct vector *ret;

    tmp = sp;
    ret = allocate_array(v->size);
    for (idx = 0; idx < v->size; idx++) {
	struct object *ob;
	int i;

	ret->item[idx].type = T_NUMBER;
	ret->item[idx].u.number = 0;
	if (v->item[idx].type != T_OBJECT)
	    continue;
	ob = v->item[idx].u.ob;
	if (ob->flags & O_DESTRUCTED)
	    continue;
	for (i = numargs; i--;)
	    push_svalue(tmp - i);
	if (apply_low(func, ob, numargs)) {
	    assign_svalue_no_free(&ret->item[idx], sp);
	    pop_stack();
	}
    }
    pop_n_elems(numargs);
    return ret;
}

/*
 * This function is similar to apply(), except that it will not
 * call the function, only return object name if the function exists,
 * or 0 otherwise.
 */
char *function_exists P2(char *, fun, struct object *, ob)
{
    struct function *pr;
    char *funname;

#ifdef OPTIMIZE_FUNCTION_TABLE_SEARCH
    int i;

#endif

    DEBUG_CHECK(ob->flags & O_DESTRUCTED,
		"function_exists() on destructed object\n");

    if (ob->flags & O_SWAPPED)
	load_ob_from_swap(ob);
    pr = ob->prog->p.i.functions;
    /* all function names are in the shared string table */
    if ((funname = findstring(fun))) {
#ifdef OPTIMIZE_FUNCTION_TABLE_SEARCH
	i = lookup_function(ob->prog->p.i.functions,
			    ob->prog->p.i.tree_r, funname);
	if (i != -1) {
	    struct program *progp;

	    pr = (struct function *) & ob->prog->p.i.functions[i];
	    if ((pr->flags & NAME_UNDEFINED) ||
		((pr->type & TYPE_MOD_STATIC) && current_object != ob))
		return 0;
#else
	for (; pr < ob->prog->p.i.functions + ob->prog->p.i.num_functions; pr++) {
	    struct program *progp;

	    /* okay to compare pointers since both are shared strings */
	    if (funname != pr->name)
		continue;
	    /* Static functions may not be called from outside. */
	    if ((pr->type & TYPE_MOD_STATIC) && current_object != ob)
		continue;
	    if (pr->flags & NAME_UNDEFINED)
		return 0;
#endif
	    for (progp = ob->prog; pr->flags & NAME_INHERITED;) {
		progp = progp->p.i.inherit[pr->offset].prog;
		pr = &progp->p.i.functions[pr->function_index_offset];
	    }
	    return progp->name;
	}
    }
    return 0;
}

#ifndef NO_SHADOWS
/*
  is_static: returns 1 if a function named 'fun' is declared 'static' in 'ob';
  0 otherwise.
*/

int is_static(fun, ob)
    char *fun;
    struct object *ob;
{
    char *funname;

    DEBUG_CHECK(ob->flags & O_DESTRUCTED,
		"is_static() on destructed object\n");
    if (ob->flags & O_SWAPPED)
	load_ob_from_swap(ob);
    /* all function names are in the shared string table */
    if ((funname = findstring(fun))) {
	struct function *pr;

#ifdef OPTIMIZE_FUNCTION_TABLE_SEARCH
	int i;

	i = lookup_function(ob->prog->p.i.functions,
			    ob->prog->p.i.tree_r, funname);
	if (i != -1) {
	    pr = (struct function *) & ob->prog->p.i.functions[i];
#else
	struct function *limit;

	limit = ob->prog->p.i.functions + ob->prog->p.i.num_functions;
	for (pr = ob->prog->p.i.functions; pr < limit; pr++) {
	    /* okay to compare pointers since both are shared strings */
	    if (funname != pr->name)
		continue;
#endif
	    if (pr->flags & NAME_UNDEFINED)
		return 0;
	    if (pr->type & TYPE_MOD_STATIC)
		return 1;
	}
    }
    return 0;
}
#endif

/*
 * Call a specific function address in an object. This is done with no
 * frame set up. It is expected that there are no arguments. Returned
 * values are removed.
 */

void call_function P2(struct program *, progp, struct function *, pr)
{
    if (pr->flags & NAME_UNDEFINED)
	return;
    push_control_stack(pr);
    caller_type = ORIGIN_DRIVER;
    DEBUG_CHECK(csp != control_stack,
		"call_function with bad csp\n");
    csp->num_local_variables = 0;
    current_prog = progp;
    pr = setup_new_frame(pr);
    previous_ob = current_object;
    tracedepth = 0;
    call_program(current_prog, pr->offset);
    pop_stack();
}

/*
 * This can be done much more efficiently, but the fix has
 * low priority.
 */
#define INCLUDE_DEPTH 10

int find_line P4(char *, p, struct program *, progp,
		 char **, ret_file, int *, ret_line )
{
    int offset;
    int i;
    int which;
    int filesave[INCLUDE_DEPTH];
    int linesave[INCLUDE_DEPTH];
    int linenum;
    int file;
    short s;

    *ret_file = "";
    *ret_line = 0;

    if (!progp) return 1;
#ifdef NEW_FUNCTIONS
    if (progp == &fake_prog) return 2;
#endif

#if defined(LPC_TO_C)
    /* currently no line number info for compiled programs */
    if (progp->p.i.program_size == 0) 
	return 3;
#endif
	
    /*
     * Load line numbers from swap if necessary.  Leave them in memory until
     * look_for_objects_to_swap() swaps them back out, since more errors are
     * likely.
     */
    if (!progp->p.i.line_numbers) {
	load_line_numbers(progp);
	if (!progp->p.i.line_numbers)
	    return 4;
    }
    offset = p - progp->p.i.program;
    DEBUG_CHECK2(offset > (int) progp->p.i.program_size,
		 "Illegal offset %d in object %s\n", offset, progp->name);
    which = 0;
    file = -1;
    linenum = 1;
    for (i=1; ; ) {
	if (s = progp->p.i.line_numbers[i++]) {
	    if (offset < s)
		break;
	    linenum++;
	} else {
	    if (s = progp->p.i.line_numbers[i++]) {
		if (which == 9) 
		    return 5;
		filesave[which] = file;
		linesave[which] = linenum;
		file = s - 1;
		linenum = 1;
		which++;
	    } else {
		which--;
		if (which < 0) break; /* end of file */
		file = filesave[which];
		linenum = linesave[which];
	    }
	}
    }
    if (file == -1) {
	*ret_file = 0;
    } else {
	*ret_file = progp->p.i.strings[file];
    }
    *ret_line = linenum;
    return 0;
}

void get_explicit_line_number_info P4(char *, p, struct program *, prog,
				      char **, ret_file, int *, ret_line) {
    find_line(p, prog, ret_file, ret_line);
    if (!(*ret_file))
	*ret_file = prog->name;
}

void get_line_number_info P2(char **, ret_file, int *, ret_line)
{
    find_line(pc, current_prog, ret_file, ret_line);
    if (!(*ret_file))
	*ret_file = current_prog->name;
}

static char* get_line_number P2(char *, p, struct program *, progp)
{
    static char buf[256];
    int i;
    char *file;
    int line;

    i = find_line(p, progp, &file, &line);
    
    switch (i) {
    case 1:
	strcpy(buf, "(no program)");
	return buf;
    case 2:	
	*buf = 0;
	return buf;
    case 3:
	strcpy(buf, "(compiled program)");
	return buf;
    case 4:
	strcpy(buf, "(no line numbers)");
	return buf;
    case 5:
	strcpy(buf, "(includes too deep)");
	return buf;
    }
    if (!file)
	file = progp->name;
    sprintf(buf, "/%s:%d", file, line);
    return buf;
}

/*
 * Write out a trace. If there is a heart_beat(), then return the
 * object that had that heart beat.
 */
char *dump_trace P1(int, how)
{
    struct control_stack *p;
    char *ret = 0;

#if defined(ARGUMENTS_IN_TRACEBACK) || defined(LOCALS_IN_TRACEBACK)
    struct svalue *ptr;
    int i;
#endif

    if (current_prog == 0)
	return 0;
    if (csp < &control_stack[0]) {
	debug_message("No trace.\n");
	return 0;
    }
#ifdef TRACE_CODE
    if (how)
	(void) last_instructions();
#endif
    for (p = &control_stack[0]; p < csp; p++) {
	debug_message("'%15s' in '%20s' ('%20s') %s\n",
		      p[0].funp ? p[0].funp->name : "CATCH",
		      p[1].prog->name, p[1].ob->name,
		      get_line_number(p[1].pc, p[1].prog));
#ifdef ARGUMENTS_IN_TRACEBACK
	if (p[0].funp) {
	    ptr = p[1].fp;
	    debug_message("arguments were (");
	    for (i = 0; i < p[0].funp->num_arg; i++) {
		char *buf;

		if (i) {
		    debug_message(",");
		}
		buf = (char *) DMALLOC(50, 0, "dump_trace:1");
		*buf = 0;
		svalue_to_string(&ptr[i], &buf, 50, 0, 0, 0);
		debug_message("%s", buf);
		FREE(buf);
	    }
	    debug_message(")\n");
	}
#endif
#ifdef LOCALS_IN_TRACEBACK
	if (p[0].funp && p[0].funp->num_local) {
	    ptr = p[1].fp + p[0].funp->num_arg;
	    debug_message("locals were: ");
	    for (i = 0; i < p[0].funp->num_local; i++) {
		char *buf;

		if (i) {
		    debug_message(",");
		}
		buf = (char *) DMALLOC(50, 0, "dump_trace:2");
		*buf = 0;
		svalue_to_string(&ptr[i], &buf, 50, 0, 0, 0);
		debug_message("%s", buf);
		FREE(buf);
	    }	
	    debug_message("\n");
	}
#endif
	if (p->funp && strcmp(p->funp->name, "heart_beat") == 0)
	    ret = p->ob ? p->ob->name : 0;	/* crash unliked gc */
    }
    debug_message("'%15s' in '%20s' ('%20s') %s\n",
		  p[0].funp ? p[0].funp->name : "CATCH",
		  current_prog->name, current_object->name,
		  get_line_number(pc, current_prog));
#ifdef ARGUMENTS_IN_TRACEBACK
    if (p[0].funp) {
	debug_message("arguments were (");
	for (i = 0; i < p[0].funp->num_arg; i++) {
	    char *buf;

	    if (i) {
		debug_message(",");
	    }
	    buf = (char *) DMALLOC(50, 0, "dump_trace:3");
	    *buf = 0;
	    svalue_to_string(&fp[i], &buf, 50, 0, 0, 0);
	    debug_message("%s", buf);
	    FREE(buf);
	}
	debug_message(")\n");
    }
#endif
#ifdef LOCALS_IN_TRACEBACK
    if (p[0].funp && p[0].funp->num_local) {
	ptr = fp + p[0].funp->num_arg;
	debug_message("locals were: ");
	for (i = 0; i < p[0].funp->num_local; i++) {
	    char *buf;

	    if (i) {
		debug_message(",");
	    }
	    buf = (char *) DMALLOC(50, 0, "dump_trace:4");
	    *buf = 0;
	    svalue_to_string(&ptr[i], &buf, 50, 0, 0, 0);
	    debug_message("%s", buf);
	    FREE(buf);
	}
	debug_message("\n");
    }
#endif
    return ret;
}

struct vector *get_svalue_trace()
{
    struct control_stack *p;
    struct vector *v;
    struct mapping *m;
    char *file;
    int line;

#if defined(ARGUMENTS_IN_TRACEBACK) || defined(LOCALS_IN_TRACEBACK)
    struct svalue *ptr;
    int i, n;
#ifdef LOCALS_IN_TRACEBACK
    int n2;
#endif
#endif

    if (current_prog == 0)
	return null_array();
    if (csp < &control_stack[0]) {
	return null_array();
    }
    v = allocate_array((csp - &control_stack[0]) + 1);
    for (p = &control_stack[0]; p < csp; p++) {
	m = allocate_mapping(6);
	add_mapping_string(m, "function", (p[0].funp ? p[0].funp->name : "CATCH"));
	add_mapping_string(m, "program", p[1].prog->name);
	add_mapping_object(m, "object", p[1].ob);
	get_explicit_line_number_info(p[1].pc, p[1].prog, &file, &line);
	add_mapping_string(m, "file", file);
	add_mapping_pair(m, "line", line);
#ifdef ARGUMENTS_IN_TRACEBACK
	if (p[0].funp) {
	    struct vector *v2;

	    n = p[0].funp->num_arg;
	    ptr = p[1].fp;
	    v2 = allocate_array(n);
	    for (i = 0; i < n; i++) {
		assign_svalue_no_free(&v2->item[i], &ptr[i]);
	    }
	    add_mapping_array(m, "arguments", v2);
	    v2->ref--;
	}
#endif
#ifdef LOCALS_IN_TRACEBACK
	if (p[0].funp) {
	    struct vector *v2;

	    n = p[0].funp->num_arg;
	    n2 = p[0].funp->num_local;
	    ptr = p[1].fp;
	    v2 = allocate_array(n2);
	    for (i = 0; i < n2; i++) {
		assign_svalue_no_free(&v2->item[i], &ptr[i + n]);
	    }
	    add_mapping_array(m, "locals", v2);
	    v2->ref--;
	}
#endif
	v->item[(p - &control_stack[0])].type = T_MAPPING;
	v->item[(p - &control_stack[0])].u.map = m;
    }
    m = allocate_mapping(6);
    add_mapping_string(m, "function", (p[0].funp ? p[0].funp->name : "CATCH"));
    add_mapping_string(m, "program", current_prog->name);
    add_mapping_object(m, "object", current_object);
    get_line_number_info(&file, &line);
    add_mapping_string(m, "file", file);
    add_mapping_pair(m, "line", line);
#ifdef ARGUMENTS_IN_TRACEBACK
    if (p[0].funp) {
	struct vector *v2;

	n = p[0].funp->num_arg;
	v2 = allocate_array(n);
	for (i = 0; i < n; i++) {
	    assign_svalue_no_free(&v2->item[i], &fp[i]);
	}
	add_mapping_array(m, "arguments", v2);
	v2->ref--;
    }
#endif
#ifdef LOCALS_IN_TRACEBACK
    if (p[0].funp) {
	struct vector *v2;

	n = p[0].funp->num_arg;
	n2 = p[0].funp->num_local;
	v2 = allocate_array(n2);
	for (i = 0; i < n2; i++) {
	    assign_svalue_no_free(&v2->item[i], &fp[i + n]);
	}
	add_mapping_array(m, "locals", v2);
	v2->ref--;
    }
#endif
    v->item[(csp - &control_stack[0])].type = T_MAPPING;
    v->item[(csp - &control_stack[0])].u.map = m;
    /* return a reference zero vector */
    v->ref--;
    return v;
}

char * get_line_number_if_any()
{
    if (current_prog)
	return get_line_number(pc, current_prog);
    return 0;
}

#define SSCANF_ASSIGN_SVALUE_STRING(S) \
    arg->type = T_STRING; \
    arg->u.string = S; \
    arg->subtype = STRING_MALLOC; \
    arg--; \
    num_arg--

#define SSCANF_ASSIGN_SVALUE(T,U,V) \
    arg->type = T; \
    arg->U = V; \
    arg--; \
    num_arg--

/* arg points to the same place it used to */
int inter_sscanf P4(struct svalue *, arg, struct svalue *, s0, struct svalue *, s1, int, num_arg)
{
    char *fmt;			/* Format description */
    char *in_string;		/* The string to be parsed. */
    int number_of_matches;
    char *cp;
    int skipme = 0;		/* Encountered a '*' ? */

    /*
     * First get the string to be parsed.
     */
    CHECK_TYPES(s0, T_STRING, 1, I(F_SSCANF));
    in_string = s0->u.string;
    if (in_string == 0)
	return 0;

    /*
     * Now get the format description.
     */
    CHECK_TYPES(s1, T_STRING, 2, I(F_SSCANF));
    fmt = s1->u.string;

    /*
     * Loop for every % or substring in the format.
     */
    for (number_of_matches = 0; num_arg >= 0; number_of_matches++) {
	int i, type;

	while (fmt[0]) {
	    if (fmt[0] == '%' && fmt[1] == '%') {
		if (in_string[0] != '%')
		    return number_of_matches;
		fmt += 2;
		in_string++;
		continue;
	    }
	    if (fmt[0] == '%')
		break;
	    if (in_string[0] == '\0' || fmt[0] != in_string[0])
		return number_of_matches;
	    fmt++;
	    in_string++;
	}

	if (fmt[0] == '\0') {
	    /*
	     * We have reached end of the format string. If there are any
	     * chars left in the in_string, then we put them in the last
	     * variable (if any).
	     */
	    if (in_string[0] && num_arg > 0) {
		number_of_matches++;
		SSCANF_ASSIGN_SVALUE_STRING(string_copy(in_string));
	    }
	    break;
	}
	DEBUG_CHECK(fmt[0] != '%',
		    "In sscanf, should be a %% now !\n");
	skipme = 0;
	if (fmt[1] == '*') {
	    skipme++;
	    fmt++;
	}
	/*
	 * Hmm..maybe we should return number_of_matches here instead of
	 * error.
	 */
	if (skipme == 0 && num_arg < 1)
	    error("Too few arguments to sscanf()\n");

	switch (fmt[1]) {
	case 'd':
	    type = T_NUMBER;
	    break;
	case 'f':
	    type = T_REAL;
	    break;
	case 's':
	    type = T_STRING;
	    break;
	default:
	    error("Bad type : '%%%c' in sscanf() format string\n", fmt[1]);
	}

	fmt += 2;
	/*
	 * Parsing a number is the easy case. Just use strtol() to find the
	 * end of the number.
	 */
	if (type == T_NUMBER) {
	    char *tmp = in_string;
	    int tmp_num;

	    tmp_num = (int) strtol(in_string, &in_string, 10);
	    if (tmp == in_string)	/* No match */
		break;

	    if (!skipme) {
		SSCANF_ASSIGN_SVALUE(T_NUMBER, u.number, tmp_num);
	    }
	    continue;
	}
	/*
	 * float case (bobf@metronet.com 2/24/93) too bad we can't use
	 * strtod() -- not all OS's have it
	 */
	if (type == T_REAL) {
	    char *tmp = in_string;
	    float tmp_num;

	    tmp_num = _strtof(in_string, &in_string);
	    if (tmp == in_string)	/* No match */
		break;

	    if (!skipme) {
		SSCANF_ASSIGN_SVALUE(T_REAL, u.real, tmp_num);
	    }
	    continue;
	}
	/*
	 * Now we have the string case.
	 */

	/*
	 * First case: There were no extra characters to match. Then this is
	 * the last match.
	 */
	if (fmt[0] == '\0') {
	    number_of_matches++;
	    if (!skipme) {
		SSCANF_ASSIGN_SVALUE_STRING(string_copy(in_string));
	    }
	    break;
	}
	/*
	 * If the next char in the format string is a '%' then we have to do
	 * some special checks. Only %d, %f and %% are allowed after a %s
	 */
	if (fmt[0] == '%') {
	    switch (fmt[1]) {
	    case 's':
		error("Illegal to have 2 adjacent %%s's in format string in sscanf()\n");
	    case 'd':
		for (i = 0; in_string[i]; i++) {
		    if (isdigit(in_string[i]))
			break;
		}
		break;
	    case 'f':
		for (i = 0; in_string[i]; i++) {
		    if (isdigit(in_string[i]) ||
			(in_string[i] == '.' && isdigit(in_string[i + 1])))
			break;
		}
		break;
	    case '%':
		if ((cp = strchr(in_string, '%')) == NULL)
		    i = strlen(in_string);
		else
		    i = cp - in_string;
		break;
	    default:
		error("Bad type : '%%%c' in sscanf() format string\n", fmt[1]);
	    }

	    if (!skipme) {
		char *match;

		match = DXALLOC(i + 1, 47, "inter_scanf");
		(void) strncpy(match, in_string, i);
		match[i] = '\0';
		SSCANF_ASSIGN_SVALUE_STRING(match);
	    }
	    in_string += i;
	    continue;
	}
	if ((cp = strchr(fmt, '%')) == NULL)
	    cp = fmt + strlen(fmt);

	for (i = 0; in_string[i]; i++) {
	    if (strncmp(in_string + i, fmt, cp - fmt) == 0) {
		char *match;

		/*
		 * Found a match !
		 */
		if (!skipme) {
		    match = DXALLOC(i + 1, 47, "inter_scanf");
		    (void) strncpy(match, in_string, i);
		    match[i] = '\0';
		    SSCANF_ASSIGN_SVALUE_STRING(match);
		}
		in_string += (i + cp - fmt);
		fmt = cp;	/* advance fmt to next % */
		break;
	    }
	}
	if (fmt == cp)		/* If match, then do continue. */
	    continue;

	/*
	 * No match was found. Then we stop here, and return the result so
	 * far !
	 */
	break;
    }
    return number_of_matches;
}

/* test stuff ... -- LA */
/* dump # of times each efun has been used */
#ifdef OPCPROF

static int cmpopc P2(opc_t *, one, opc_t *, two)
{
    return (two->count - one->count);
}

void opcdump P1(char *, tfn)
{
    int i, len, limit;
    char tbuf[SMALL_STRING_SIZE], *fn;
    FILE *fp;

    if ((len = strlen(tfn)) >= (SMALL_STRING_SIZE - 7)) {
	add_message("Path '%s' too long.\n", tfn);
	return;
    }
    strcpy(tbuf, tfn);
    strcpy(tbuf + len, ".efun");
    fn = check_valid_path(tbuf, current_object, "opcprof", 1);
    if (!fn) {
	add_message("Invalid path '%s' for writing.\n", tbuf);
	return;
    }
    fp = fopen(fn, "w");
    if (!fp) {
	add_message("Unable to open %s.\n", fn);
	return;
    }
    add_message("Dumping to %s ... ", fn);
    limit = sizeof(opc_efun) / sizeof(opc_t);
    for (i = 0; i < limit; i++) {
	fprintf(fp, "%-30s: %10d\n", opc_efun[i].name, opc_efun[i].count);
    }
    fclose(fp);

    strcpy(tbuf, tfn);
    strcpy(tbuf + len, ".eoper");
    fn = check_valid_path(tbuf, current_object, "opcprof", 1);
    if (!fn) {
	add_message("Invalid path '%s' for writing.\n", tbuf);
	return;
    }
    fp = fopen(fn, "w");
    if (!fp) {
	add_message("Unable to open %s for writing.\n", fn);
	return;
    }
    for (i = 0; i < BASE; i++) {
	fprintf(fp, "%-30s: %10d\n",
		query_instr_name(i), opc_eoper[i]);
    }
    fclose(fp);
    add_message("done.\n");
}
#endif

/*
 * Reset the virtual stack machine.
 */
int reset_machine P1(int, first)
{
    csp = control_stack - 1;
    if (first)
	sp = &start_of_stack[-1];
    else {
	int ret = sp - &start_of_stack[-1];
	pop_n_elems(ret);
	return ret;
    }
}

#ifdef TRACE_CODE
static char *get_arg P2(int, a, int, b)
{
    static char buff[10];
    char *from, *to;

    from = previous_pc[a];
    to = previous_pc[b];
    if (to - from < 2)
	return "";
    if (to - from == 2) {
	sprintf(buff, "%d", from[1]);
	return buff;
    }
    if (to - from == 3) {
	short arg;

	((char *) &arg)[0] = from[1];
	((char *) &arg)[1] = from[2];
	sprintf(buff, "%d", arg);
	return buff;
    }
    if (to - from == 5) {
	int arg;

	((char *) &arg)[0] = from[1];
	((char *) &arg)[1] = from[2];
	((char *) &arg)[2] = from[3];
	((char *) &arg)[3] = from[4];
	sprintf(buff, "%d", arg);
	return buff;
    }
    return "";
}

int last_instructions()
{
    int i;

    i = last;
    do {
	if (previous_instruction[i] != 0)
	    printf("%6x: %3d %8s %-25s (%d)\n", previous_pc[i],
		   previous_instruction[i],
		   get_arg(i, (i + 1) %
			   (sizeof previous_instruction / sizeof(int))),
		   get_f_name(previous_instruction[i]),
		   stack_size[i] + 1);
	i = (i + 1) % (sizeof previous_instruction / sizeof(int));
    } while (i != last);
    return last;
}

#endif				/* TRACE_CODE */


#ifdef DEBUG

static void count_inherits P2(struct program *, progp, struct program *, search_prog)
{
    int i;

    /* Clones will not add to the ref count of inherited progs */
    if (progp->p.i.extra_ref != 1)
	return;			/* marion */
    for (i = 0; i < (int) progp->p.i.num_inherited; i++) {
	progp->p.i.inherit[i].prog->p.i.extra_ref++;
	if (progp->p.i.inherit[i].prog == search_prog)
	    printf("Found prog, inherited by %s\n", progp->name);
	count_inherits(progp->p.i.inherit[i].prog, search_prog);
    }
}

static void count_ref_in_vector P2(struct svalue *, svp, int, num)
{
    struct svalue *p;

    for (p = svp; p < svp + num; p++) {
	switch (p->type) {
	case T_OBJECT:
	    p->u.ob->extra_ref++;
	    continue;
	case T_POINTER:
	    count_ref_in_vector(&p->u.vec->item[0], p->u.vec->size);
	    p->u.vec->extra_ref++;
	    continue;
	}
    }
}

/*
 * Clear the extra debug ref count for vectors
 */
static void clear_vector_refs P2(struct svalue *, svp, int, num)
{
    struct svalue *p;

    for (p = svp; p < svp + num; p++) {
	switch (p->type) {
	case T_POINTER:
	    clear_vector_refs(&p->u.vec->item[0], p->u.vec->size);
	    p->u.vec->extra_ref = 0;
	    continue;
	}
    }
}

/*
 * Loop through every object and variable in MudOS and check
 * all reference counts. This will surely take some time, and should
 * only be used for debugging.
 */
void check_a_lot_ref_counts P1(struct program *, search_prog)
{
    extern struct object *master_ob;
    struct object *ob;

    /*
     * Pass 1: clear the ref counts.
     */
    for (ob = obj_list; ob; ob = ob->next_all) {
	ob->extra_ref = 0;
	ob->prog->p.i.extra_ref = 0;
	clear_vector_refs(ob->variables, ob->prog->p.i.num_variables);
    }
    clear_vector_refs(start_of_stack, sp - start_of_stack + 1);

    /*
     * Pass 2: Compute the ref counts.
     */

    /*
     * List of all objects.
     */
    for (ob = obj_list; ob; ob = ob->next_all) {
	ob->extra_ref++;
	count_ref_in_vector(ob->variables, ob->prog->p.i.num_variables);
	ob->prog->p.i.extra_ref++;
	if (ob->prog == search_prog)
	    printf("Found program for object %s\n", ob->name);
	/* Clones will not add to the ref count of inherited progs */
	if (ob->prog->p.i.extra_ref == 1)
	    count_inherits(ob->prog, search_prog);
    }

    /*
     * The current stack.
     */
    count_ref_in_vector(start_of_stack, sp - start_of_stack + 1);
    update_ref_counts_for_users();
    count_ref_from_call_outs();
    /* Special objects */
    if (master_ob)
	master_ob->extra_ref++;
    if (simul_efun_ob)
	simul_efun_ob->extra_ref++;

    if (search_prog)
	return;

    /*
     * Pass 3: Check the ref counts.
     */
    for (ob = obj_list; ob; ob = ob->next_all) {
	if (ob->ref != ob->extra_ref)
	    printf("Bad ref count in object %s, is %d - should be %d\n", ob->name,
		   ob->ref, ob->extra_ref);
	if (ob->prog->p.i.ref != ob->prog->p.i.extra_ref) {
	    check_a_lot_ref_counts(ob->prog);
	    printf("Bad ref count in prog %s, is %d - should be %d\n", ob->prog->name,
		   ob->prog->p.i.ref, ob->prog->p.i.extra_ref);
	}
    }
}

#endif				/* DEBUG */

#ifdef TRACE
/* Generate a debug message to the user */
void do_trace P3(char *, msg, char *, fname, char *, post)
{
    char buf[10000];
    char *objname;

    if (!TRACEHB)
	return;
    objname = TRACETST(TRACE_OBJNAME) ? (current_object && current_object->name ? current_object->name : "??") : "";
    sprintf(buf, "*** %d %*s %s %s %s%s", tracedepth, tracedepth, "", msg, objname, fname, post);
    add_message(buf);
}
#endif

int assert_master_ob_loaded P2(char *, fun, char *, arg) {
    /* First we check two special conditions.  If master_ob == -1, main.c
     * hasn't tried to load the master object yet, so we shouldn't.
     * if master_ob_is_loading is set, then the master is in the process
     * of loading.
     */
    if (master_ob_is_loading || (master_ob == (struct object *)-1))
	return -1;
    if (!master_ob || (master_ob->flags & O_DESTRUCTED)) {
        struct object *ob;

        ob = load_object(master_file_name, 0);
	if (!ob) {
	    fprintf(stderr, "%s(%s) failed: Master failed to load.\n", fun, arg);
	    return 0;
	}
    }
    return 1;
}

/* If the master object can't be loaded, we return zero. (struct svalue *)-1
 * means that a secure object is loading.  Default behavior should be that
 * the check succeeds.
 */
struct svalue *apply_master_ob P2(char *, fun, int, num_arg)
{
    int err;

    call_origin = ORIGIN_DRIVER;
    if ((err = assert_master_ob_loaded("apply_master_ob", fun)) != 1) {
	pop_n_elems(num_arg);
	return (struct svalue *)err;
    }
    return sapply(fun, master_ob, num_arg);
}

struct svalue *safe_apply_master_ob P2(char *, fun, int, num_arg)
{
    int err;
    if ((err = assert_master_ob_loaded("safe_apply_master_ob", fun)) != 1) {
	pop_n_elems(num_arg);
	return (struct svalue *)err;
    }
    return safe_apply(fun, master_ob, num_arg, ORIGIN_DRIVER);
}

/*
 * When an object is destructed, all references to it must be removed
 * from the stack.
 */
void remove_object_from_stack P1(struct object *, ob)
{
    struct svalue *svp;

    for (svp = start_of_stack; svp <= sp; svp++) {
	if (svp->type != T_OBJECT)
	    continue;
	if (svp->u.ob != ob)
	    continue;
	free_object(svp->u.ob, "remove_object_from_stack");
	svp->type = T_NUMBER;
	svp->u.number = 0;
    }
}

int strpref P2(char *, p, char *, s)
{
    while (*p)
	if (*p++ != *s++)
	    return 0;
    return 1;
}

static float _strtof P2(char *, nptr, char **, endptr)
{
    register char *s = nptr;
    register float acc;
    register int neg, c, any, div;

    div = 1;
    neg = 0;
    /*
     * Skip white space and pick up leading +/- sign if any.
     */
    do {
	c = *s++;
    } while (isspace(c));
    if (c == '-') {
	neg = 1;
	c = *s++;
    } else if (c == '+')
	c = *s++;

    for (acc = 0, any = 0;; c = *s++) {
	if (isdigit(c))
	    c -= '0';
	else if ((div == 1) && (c == '.')) {
	    div = 10;
	    continue;
	} else
	    break;
	if (div == 1) {
	    acc *= (float) 10;
	    acc += (float) c;
	} else {
	    acc += (float) c / (float) div;
	    div *= 10;
	}
	any = 1;
    }

    if (neg)
	acc = -acc;

    if (endptr != 0)
	*endptr = any ? s - 1 : (char *) nptr;

    return acc;
}