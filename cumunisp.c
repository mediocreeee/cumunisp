#include "mpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// If we are compiling on Windows compile these functions
#ifdef _WIN32
/* #include <string.h> */

static char buffer[2048];

char *readline(char *prompt) {
  fputs(prompt, stdout);
  fgets(buffer, 2048, stdin);
  char *cpy = malloc(strlen(buffer) + 1);
  strcpy(cpy, buffer);
  cpy[strlen(cpy) - 1] = '\0';
  return cpy;
}

void add_history(char *unused) {}

#else
#include <editline/readline.h>
#endif

// Forward Declarations
mpc_parser_t *Number;
mpc_parser_t *Symbol;
mpc_parser_t *String;
mpc_parser_t *Comment;
mpc_parser_t *Sexpr;
mpc_parser_t *Qexpr;
mpc_parser_t *Expr;
mpc_parser_t *Cumunisp;

struct lval;
struct lenv;
typedef struct lval lval;
typedef struct lenv lenv;

// Enumeration of possible lval types
enum {
  LVAL_NUM,
  LVAL_SYM,
  LVAL_SEXPR,
  LVAL_QEXPR,
  LVAL_ERR,
  LVAL_FUN,
  LVAL_STR
};

typedef lval *(*lbuiltin)(lenv *, lval *);

// lval Struct
struct lval {
  int type;

  // Basic
  double num;
  char *err;
  char *sym;
  char *str;

  // Function
  lbuiltin builtin;
  lenv *env;
  lval *formals;
  lval *body;

  // Count and Pointer to a list of "lval"
  int count;
  lval **cell;
};

// lval fun constructor
lval *lval_builtin(lbuiltin func) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_FUN;
  v->builtin = func;
  return v;
}

// Create a pointer to a new Number type lval
lval *lval_num(double x) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_NUM;
  v->num = x;
  return v;
}

// A pointer to a new empty Qexpr lval
lval *lval_qexpr(void) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_QEXPR;
  v->count = 0;
  v->cell = NULL;
  return v;
}

// Create a pointer to a new Error type lval
lval *lval_err(char *fmt, ...) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_ERR;

  // Create a va list and initialize it
  va_list va;
  va_start(va, fmt);

  // Allocate 512 bytes of space
  v->err = malloc(512);

  // printf the error string with a maximum of 511 characters
  vsnprintf(v->err, 511, fmt, va);

  // Reallocate to number of bytes actually used
  v->err = realloc(v->err, strlen(v->err) + 1);

  // Cleanup va list
  va_end(va);

  return v;
}

// Create a pointer to a new Symbol type lval
lval *lval_sym(char *s) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_SYM;
  v->sym = malloc(strlen(s) + 1);
  strcpy(v->sym, s);
  return v;
}

// A point to a new empty Sexpr lval
lval *lval_sexpr(void) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_SEXPR;
  v->count = 0;
  v->cell = NULL;
  return v;
}

lval *lval_str(char *s) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_STR;
  v->str = malloc(strlen(s) + 1);
  strcpy(v->str, s);
  return v;
}

// Function that calls free() for every malloc to prevent memory leaks
void lval_del(lval *v) {
  switch (v->type) {
  // Do nothing for number type
  case LVAL_NUM:
    break;

    // DO nothing for fun type
  case LVAL_FUN:
    break;

  // For Err or Sym free the string data
  case LVAL_ERR:
    free(v->err);
    break;
  case LVAL_SYM:
    free(v->sym);
    break;
  // Free string memory for string type
  case LVAL_STR:
    free(v->str);
    break;

    // If Qexpr or Sexpr then delete all elems inside
  case LVAL_QEXPR:
  case LVAL_SEXPR:
    for (int i = 0; i < v->count; i++) {
      lval_del(v->cell[i]);
    }
    // Then also free the memory allocated to conatin the pointers
    free(v->cell);
    break;
  }

  free(v);
}
lval *lval_read_num(mpc_ast_t *t) {
  errno = 0;
  double x = strtod(t->contents, NULL);
  return errno != ERANGE ? lval_num(x) : lval_err("Invalid number!");
}

lval *lval_add(lval *v, lval *x) {
  v->count++;
  v->cell = realloc(v->cell, sizeof(lval *) * v->count);
  v->cell[v->count - 1] = x;
  return v;
}

lval *lval_read_str(mpc_ast_t *t) {
  // Cut off the final quote character
  t->contents[strlen(t->contents) - 1] = '\0';
  // Copy the string missing out the first quote character
  char *unescaped = malloc(strlen(t->contents + 1) + 1);
  strcpy(unescaped, t->contents + 1);
  // Pass through the unescape function
  unescaped = mpcf_unescape(unescaped);
  // Construct a new lval using the string
  lval *str = lval_str(unescaped);
  // Free the string and return
  free(unescaped);
  return str;
}

lval *lval_read(mpc_ast_t *t) {

  // If Symbol or Number return conversion to that type
  if (strstr(t->tag, "number")) {
    return lval_read_num(t);
  }
  if (strstr(t->tag, "string")) {
    return lval_read_str(t);
  }
  if (strstr(t->tag, "symbol")) {
    return lval_sym(t->contents);
  }

  // If root (>) or sexpr then crete empty list
  lval *x = NULL;
  if (strstr(t->tag, ">")) {
    x = lval_sexpr();
  }
  if (strstr(t->tag, "sexpr")) {
    x = lval_sexpr();
  }

  if (strstr(t->tag, "qexpr")) {
    x = lval_qexpr();
  }

  // Fill this list with any valid wxpression contained within
  for (int i = 0; i < t->children_num; i++) {
    if (strcmp(t->children[i]->contents, "(") == 0) {
      continue;
    }
    if (strcmp(t->children[i]->contents, ")") == 0) {
      continue;
    }
    if (strcmp(t->children[i]->contents, "{") == 0) {
      continue;
    }
    if (strcmp(t->children[i]->contents, "}") == 0) {
      continue;
    }
    if (strcmp(t->children[i]->tag, "regex") == 0) {
      continue;
    }
    if (strstr(t->children[i]->tag, "comment")) {
      continue;
    }
    x = lval_add(x, lval_read(t->children[i]));
  }
  return x;
}

void lval_print(lval *v);

// Function to loop over all the sub-expresession and prints them
void lval_expr_print(lval *v, char open, char close) {
  putchar(open);
  for (int i = 0; i < v->count; i++) {
    // Print Value contained within
    lval_print(v->cell[i]);

    // Don't print trailing space if last element
    if (i != (v->count - 1)) {
      putchar(' ');
    }
  }
  putchar(close);
}

// Print string
void lval_print_str(lval *v) {
  // Make a copy of the string
  char *escaped = malloc(strlen(v->str) + 1);
  strcpy(escaped, v->str);
  // Pass it through the escape function
  escaped = mpcf_escape(escaped);
  // Print it between " characters
  printf("\"%s\"", escaped);
  // Free the copied string
  free(escaped);
}

// Print an "lval"
void lval_print(lval *v) {
  // In the case the type is a number print it
  switch (v->type) {
  case LVAL_NUM:
    printf("%g", v->num);
    break;

    // lval fun type
  case LVAL_FUN:
    if (v->builtin) {
      printf("<builtin>");
    } else {
      printf("(\\ )");
      lval_print(v->formals);
      putchar(' ');
      lval_print(v->body);
      putchar(')');
    }
    break;

    // In the case the type is an error
  case LVAL_ERR:
    printf("Error: %s", v->err);
    break;
  case LVAL_SYM:
    printf("%s", v->sym);
    break;
  case LVAL_STR:
    lval_print_str(v);
    break;
  case LVAL_SEXPR:
    lval_expr_print(v, '(', ')');
    break;
  case LVAL_QEXPR:
    lval_expr_print(v, '{', '}');
    break;
  }
}

void lval_print_expr(lval *v, char open, char close) {
  putchar(open);
  for (int i = 0; i < v->count; i++) {
    lval_print(v->cell[i]);
    if (i != (v->count - 1)) {
      putchar(' ');
    }
  }
  putchar(close);
}

lenv *lenv_copy(lenv *e);

// Copying the environment
lval *lval_copy(lval *v) {
  lval *x = malloc(sizeof(lval));
  x->type = v->type;

  switch (v->type) {
  // Copy Functions and Numbers directly
  case LVAL_FUN:
    if (v->builtin) {

      x->builtin = v->builtin;
    } else {
      x->builtin = NULL;
      x->env = lenv_copy(v->env);
      x->formals = lval_copy(v->formals);
      x->body = lval_copy(v->body);
    }
    break;
  case LVAL_NUM:
    x->num = v->num;
    break;

  // Copy Strings using malloc and strcpy
  case LVAL_ERR:
    x->err = malloc(sizeof(strlen(v->err) + 1));
    strcpy(x->err, v->err);
    break;
  case LVAL_SYM:
    x->sym = malloc(sizeof(strlen(v->sym) + 1));
    strcpy(x->sym, v->sym);
    break;
  case LVAL_STR:
    x->str = malloc(strlen(v->str) + 1);
    strcpy(x->str, v->str);
    break;

  // Copy Lists by copying each sub-expression
  case LVAL_SEXPR:
  case LVAL_QEXPR:
    x->count = v->count;
    x->cell = malloc(sizeof(lval *) * x->count);
    for (int i = 0; i < x->count; i++) {
      x->cell[i] = lval_copy(v->cell[i]);
    }
    break;
  }
  return x;
}

// Environment
struct lenv {
  lenv *par;
  int count;
  char **syms;
  lval **vals;
};

// New environment
lenv *lenv_new(void) {
  lenv *e = malloc(sizeof(lenv));
  e->par = NULL;
  e->count = 0;
  e->syms = NULL;
  e->vals = NULL;
  return e;
}
lval *lval_lambda(lval *formals, lval *body) {
  lval *v = malloc(sizeof(lval));
  v->type = LVAL_FUN;
  v->builtin = NULL;
  v->env = lenv_new();
  v->formals = formals;
  v->body = body;
  return v;
}

// Deletion for environment
void lenv_del(lenv *e) {
  for (int i = 0; i < e->count; i++) {
    free(e->syms[i]);
    lval_del(e->vals[i]);
  }

  free(e->syms);
  free(e->vals);
  free(e);
}

lenv *lenv_copy(lenv *e) {
  lenv *n = malloc(sizeof(lenv));
  n->par = e->par;
  n->count = e->count;
  n->syms = malloc(sizeof(char *) * n->count);
  n->vals = malloc(sizeof(lval *) * n->count);
  for (int i = 0; i < e->count; i++) {
    n->syms[i] = malloc(strlen(e->syms[i]) + 1);
    strcpy(n->syms[i], e->syms[i]);
    n->vals[i] = lval_copy(e->vals[i]);
  }
  return n;
}

// Get from environment
lval *lenv_get(lenv *e, lval *k) {
  // Iterate over all items in environment
  for (int i = 0; i < e->count; i++) {
    // Check if the stored string matches the symbel string
    // If it does, return a copy of the value
    if (strcmp(e->syms[i], k->sym) == 0) {
      return lval_copy(e->vals[i]);
    }
  }
  if (e->par) {
    return lenv_get(e->par, k);
  } else {
    // If no symbol found return error
    return lval_err("Unbound Symbol '%s'", k->sym);
  }
}

// Put into environment
void lenv_put(lenv *e, lval *k, lval *v) {
  // Iterate over all items in environment
  // This is to see if variable already exists
  for (int i = 0; i < e->count; i++) {
    // If variable is found delete itema at that position
    // And replace with variable supplied by user
    if (strcmp(e->syms[i], k->sym) == 0) {
      lval_del(e->vals[i]);
      e->vals[i] = lval_copy(v);
      return;
    }
  }

  // If no existing entry found allocate space for new entry
  e->count++;
  e->vals = realloc(e->vals, sizeof(lval *) * e->count);
  e->syms = realloc(e->syms, sizeof(lval *) * e->count);

  // Copy contents of lval and symbol string into new location
  e->vals[e->count - 1] = lval_copy(v);
  e->syms[e->count - 1] = malloc(strlen(k->sym) + 1);
  strcpy(e->syms[e->count - 1], k->sym);
}

void lenf_def(lenv *e, lval *k, lval *v) {
  while (e->par) {
    e = e->par;
  }
  lenv_put(e, k, v);
}

// Print an "lval" followed by a newline
void lval_println(lval *v) {
  lval_print(v);
  putchar('\n');
}

lval *lval_pop(lval *v, int i) {
  // Find the item at "i"
  lval *x = v->cell[i];

  // Shift memory after the item at "i" over the top
  memmove(&v->cell[i], &v->cell[i + 1], sizeof(lval *) * (v->count - i - 1));

  // Decrease the count of items in the list
  v->count--;

  // Reallocate the memory user
  v->cell = realloc(v->cell, sizeof(lval *) * v->count);
  return x;
}
lval *lval_take(lval *v, int i) {
  lval *x = lval_pop(v, i);
  lval_del(v);
  return x;
}

lval *lval_eval_sexpr(lenv *e, lval *v);

lval *lval_eval(lenv *e, lval *v) {
  if (v->type == LVAL_SYM) {
    lval *x = lenv_get(e, v);
    lval_del(v);
    return x;
  }
  // Evaluate Sexpressions
  if (v->type == LVAL_SEXPR) {
    return lval_eval_sexpr(e, v);
  }
  return v;
}

char *ltype_name(int t) {
  switch (t) {
  case LVAL_FUN:
    return "Function";
  case LVAL_NUM:
    return "Number";
  case LVAL_ERR:
    return "Error";
  case LVAL_STR:
    return "String";
  case LVAL_SYM:
    return "Symbol";
  case LVAL_SEXPR:
    return "S-Expression";
  case LVAL_QEXPR:
    return "Q-Expression";
  default:
    return "Unknown";
  }
}

void lenv_def(lenv *e, lval *k, lval *v) {
  while (e->par) {
    e = e->par;
  }
  lenv_put(e, k, v);
}

// Macroses
#define LASSERT(args, cond, fmt, ...)                                          \
  if (!(cond)) {                                                               \
    lval *err = lval_err(fmt, ##__VA_ARGS__);                                  \
    lval_del(args);                                                            \
    return err;                                                                \
  }

#define LASSERT_TYPE(func, args, index, expect)                                \
  LASSERT(args, args->cell[index]->type == expect,                             \
          "Function '%s' passed incorrect type for argunment %i. Got: %s, "    \
          "Expected: %s!",                                                     \
          func, index, ltype_name(args->cell[index]->type),                    \
          ltype_name(expect));

#define LASSERT_NUM(func, args, num)                                           \
  LASSERT(args, args->count == num,                                            \
          "Function '%s', passed incorrect number of arguments. Got: %i, "     \
          "Expected: %i!",                                                     \
          func, args->count, num);

#define LASSERT_NOT_EMPTY(func, args, index)                                   \
  LASSERT(args, args->cell[index]->count != 0,                                 \
          "Function '%s' passed {} for argument %i!" func, index);

lval *builtin_lambda(lenv *e, lval *a) {
  LASSERT_NUM("\\", a, 2);
  LASSERT_TYPE("\\", a, 0, LVAL_QEXPR);
  LASSERT_TYPE("\\", a, 1, LVAL_QEXPR);

  for (int i = 0; i < a->cell[0]->count; i++) {
    LASSERT(a, (a->cell[0]->cell[i]->type == LVAL_SYM),
            "Cannot define non-symbol. Got: %s, Expected: %s!",
            ltype_name(a->cell[0]->cell[i]->type), ltype_name(LVAL_SYM));
  }

  lval *formals = lval_pop(a, 0);
  lval *body = lval_pop(a, 0);
  lval_del(a);

  return lval_lambda(formals, body);
}

lval *builtin_op(lenv *e, lval *a, char *op) {
  // Ensure all args are numbers
  for (int i = 0; i < a->count; i++) {
    LASSERT_TYPE(op, a, i, LVAL_NUM);
  }
  // Pop the first element
  lval *x = lval_pop(a, 0);

  // If no args and sub then perform unary negation
  if ((strcmp(op, "-") == 0) && a->count == 0) {
    x->num = -x->num;
  }

  // While there are still elems remaining
  while (a->count > 0) {
    // Pop the next elem
    lval *y = lval_pop(a, 0);

    // Perform operation
    if (strcmp(op, "+") == 0 || strcmp(op, "add") == 0) {
      x->num += y->num;
    }
    if (strcmp(op, "-") == 0 || strcmp(op, "sub") == 0) {
      x->num -= y->num;
    }
    if (strcmp(op, "*") == 0 || strcmp(op, "mul") == 0) {
      x->num *= y->num;
    }
    if (strcmp(op, "/") == 0 || strcmp(op, "div") == 0) {
      if (y->num == 0) {
        lval_del(x);
        lval_del(y);
        x = lval_err("Division by zero!");
        break;
      }
      x->num /= y->num;
    }
    if (strcmp(op, "%") == 0 || strcmp(op, "rem") == 0) {
      x->num = fmod(x->num, y->num);
    }
    if (strcmp(op, "^") == 0 || strcmp(op, "pow") == 0) {
      x->num = pow(x->num, y->num);
    }
    if (strcmp(op, "max") == 0) {
      x->num = fmax(x->num, y->num);
    }
    if (strcmp(op, "min") == 0) {
      x->num = fmin(x->num, y->num);
    }

    // Delete elem now finished with
    lval_del(y);
  }
  // Delete input expression and return result
  lval_del(a);
  return x;
}

lval *builtin_head(lenv *e, lval *a) {
  // Check Error Conditions
  LASSERT_NUM("head", a, 1);
  LASSERT_TYPE("head", a, 0, LVAL_QEXPR);
  LASSERT_NOT_EMPTY("head", a, 0);

  // Otherwise take first arg
  lval *v = lval_take(a, 0);

  // Delete all elems that are not head and return
  while (v->count > 1) {
    lval_del(lval_pop(v, 1));
  }
  return v;
}

lval *builtin_tail(lenv *e, lval *a) {
  // Check Error Conditions
  LASSERT_NUM("tail", a, 1);
  LASSERT_TYPE("tail", a, 0, LVAL_QEXPR);
  LASSERT_NOT_EMPTY("tail", a, 0);

  // Otherwise take first arg
  lval *v = lval_take(a, 0);

  // Delete firs elem and return
  lval_del(lval_pop(v, 0));
  return v;
}

lval *builtin_eval(lenv *e, lval *a) {
  LASSERT_NUM("eval", a, 1);
  LASSERT_TYPE("eval", a, 0, LVAL_QEXPR);

  lval *x = lval_take(a, 0);
  x->type = LVAL_SEXPR;
  return lval_eval(e, x);
}

lval *lval_join(lval *x, lval *y) {
  // For each cell in 'y' add it to 'x'
  while (y->count) {
    x = lval_add(x, lval_pop(y, 0));
  }
  // Then delete the empty 'y' and return 'x'
  lval_del(y);
  return x;
}

lval *builtin_join(lenv *e, lval *a) {
  for (int i = 0; i < a->count; i++) {
    LASSERT_TYPE("join", a, i, LVAL_QEXPR);
  }

  lval *x = lval_pop(a, 0);
  while (a->count) {
    x = lval_join(x, lval_pop(a, 0));
  }

  lval_del(a);
  return x;
}

lval *builtin_list(lenv *e, lval *a) {
  a->type = LVAL_QEXPR;
  return a;
}

lval *builtin_cons(lenv *e, lval *a) {
  LASSERT_NUM("cons", a, 2);
  LASSERT_TYPE("cons", a, 1, LVAL_QEXPR);

  // Pop the qexpr
  lval *y = lval_pop(a, 1);
  // Append cell[0] to the qexpr
  lval *list = lval_join(y, a);

  return list;
}

lval *builtin_len(lenv *e, lval *a) {
  LASSERT_NUM("len", a, 1);
  LASSERT_TYPE("len", a, 0, LVAL_QEXPR);

  // Just return cell[0] count
  return lval_num(a->cell[0]->count);
}

lval *builtin_init(lenv *e, lval *a) {
  // Check Error Conditions
  LASSERT_NUM("init", a, 1);
  LASSERT_TYPE("init", a, 0, LVAL_QEXPR);
  LASSERT_NOT_EMPTY("init", a, 0);

  // Take first argument
  lval *v = lval_take(a, 0);

  // Delete all elems that are not init and return
  while (v->count > 1) {
    lval_del(lval_pop(v, 0));
  }

  return v;
}
lval *builtin_var(lenv *e, lval *a, char *func) {
  LASSERT_TYPE(func, a, 0, LVAL_QEXPR);

  lval *syms = a->cell[0];
  for (int i = 0; i < syms->count; i++) {
    LASSERT(a, (syms->cell[i]->type == LVAL_SYM),
            "Function '%s' cannot define non-symbol! "
            " Got: %s, Expected: %s",
            func, ltype_name(syms->cell[i]->type), ltype_name(LVAL_SYM));
  }

  LASSERT(a, (syms->count == a->count - 1),
          "Function '%s', passed too many arguments for symbols. "
          "Got: %i, Expected: %i",
          func, syms->count, a->count - 1);

  for (int i = 0; i < syms->count; i++) {
    if (strcmp(func, "def") == 0) {
      lenv_def(e, syms->cell[i], a->cell[i + 1]);
    }
    if (strcmp(func, "=") == 0) {
      lenv_put(e, syms->cell[i], a->cell[i + 1]);
    }
  }

  lval_del(a);
  return lval_sexpr();
}

lval *builtin_def(lenv *e, lval *a) { return builtin_var(e, a, "def"); }
lval *builtin_put(lenv *e, lval *a) { return builtin_var(e, a, "="); }

lval *builtin_ord(lenv *e, lval *a, char *op) {
  LASSERT_NUM(op, a, 2);
  LASSERT_TYPE(op, a, 0, LVAL_NUM);
  LASSERT_TYPE(op, a, 1, LVAL_NUM);

  int r;
  if (strcmp(op, ">") == 0) {
    r = (a->cell[0]->num > a->cell[1]->num);
  }
  if (strcmp(op, ">=") == 0) {
    r = (a->cell[0]->num >= a->cell[1]->num);
  }
  if (strcmp(op, "<") == 0) {
    r = (a->cell[0]->num < a->cell[1]->num);
  }
  if (strcmp(op, "<=") == 0) {
    r = (a->cell[0]->num <= a->cell[1]->num);
  }

  lval_del(a);
  return lval_num(r);
}

int lval_eq(lval *x, lval *y) {
  // Different Types are always unequal
  if (x->type != y->type) {
    return 0;
  };

  // Compare Based upon type
  switch (x->type) {
  // Compare Number value
  case LVAL_NUM:
    return (x->num == y->num);

  // Compare String Values
  case LVAL_ERR:
    return (strcmp(x->err, y->err) == 0);
  case LVAL_SYM:
    return (strcmp(x->sym, y->sym) == 0);
  case LVAL_STR:
    return (strcmp(x->str, y->str) == 0);

  // If builtin compare, otherwise compare formals and body
  case LVAL_FUN:
    if (x->builtin || y->builtin) {
      return x->builtin == y->builtin;
    } else {
      return lval_eq(x->formals, y->formals) && lval_eq(x->body, y->body);
    }

  // If list compare every element
  case LVAL_QEXPR:
  case LVAL_SEXPR:
    if (x->count != y->count) {
      return 0;
    }
    for (int i = 0; i < x->count; i++) {
      // If any element not equal then whole list not equal
      if (!lval_eq(x->cell[i], y->cell[i])) {
        return 0;
      }
    }
    // Otherwise lists are equal
    return 1;
    break;
  }
  return 0;
}

lval *builtin_cmp(lenv *e, lval *a, char *op) {
  LASSERT_NUM(op, a, 2)
  int r;
  if (strcmp(op, "==") == 0) {
    r = lval_eq(a->cell[0], a->cell[1]);
  }
  if (strcmp(op, "!=") == 0) {
    r = !lval_eq(a->cell[0], a->cell[1]);
  }
  lval_del(a);
  return lval_num(r);
}

lval *builtin_if(lenv *e, lval *a) {
  LASSERT_NUM("if", a, 3);
  LASSERT_TYPE("if", a, 0, LVAL_NUM);
  LASSERT_TYPE("if", a, 1, LVAL_QEXPR);
  LASSERT_TYPE("if", a, 2, LVAL_QEXPR);

  // Mark both expressions as evaluable
  lval *x;
  a->cell[1]->type = LVAL_SEXPR;
  a->cell[2]->type = LVAL_SEXPR;

  if (a->cell[0]->num) {
    // If condition is true evaluate first expression
    x = lval_eval(e, lval_pop(a, 1));
  } else {
    // Otherwise ovaluate second expression
    x = lval_eval(e, lval_pop(a, 2));
  }

  // Delete argument list and return
  lval_del(a);
  return x;
}

lval *builtin_load(lenv *e, lval *a) {
  LASSERT_NUM("load", a, 1);
  LASSERT_TYPE("load", a, 0, LVAL_STR);

  // Parse File given by string name
  mpc_result_t r;
  if (mpc_parse_contents(a->cell[0]->str, Cumunisp, &r)) {

    // Read contents
    lval *expr = lval_read(r.output);
    mpc_ast_delete(r.output);

    /* Evaluate each Expression */
    while (expr->count) {
      lval *x = lval_eval(e, lval_pop(expr, 0));
      // If Evaluation leads to error print it
      if (x->type == LVAL_ERR) {
        lval_println(x);
      }
      lval_del(x);
    }

    // Delete expressions and arguments
    lval_del(expr);
    lval_del(a);

    // Return empty list
    return lval_sexpr();

  } else {
    // Get Parse Error as String
    char *err_msg = mpc_err_string(r.error);
    mpc_err_delete(r.error);

    // Create new error message using it
    lval *err = lval_err("Could not load Library %s", err_msg);
    free(err_msg);
    lval_del(a);

    // Cleanup and return error
    return err;
  }
}

lval *builtin_print(lenv *e, lval *a) {
  // Print each argument followed by a space
  for (int i = 0; i < a->count; i++) {
    lval_print(a->cell[i]);
    putchar(' ');
  }

  // Print a newline and delete arguments
  putchar('\n');
  lval_del(a);

  return lval_sexpr();
}

lval *builtin_err(lenv *e, lval *a) {
  LASSERT_NUM("error", a, 1);
  LASSERT_TYPE("error", a, 0, LVAL_STR);

  // Construct Error from first argument
  lval *err = lval_err(a->cell[0]->str);

  // Delete arguments and return
  lval_del(a);
  return err;
}

lval *builtin_add(lenv *e, lval *a) { return builtin_op(e, a, "+"); }
lval *builtin_sub(lenv *e, lval *a) { return builtin_op(e, a, "-"); }
lval *builtin_mul(lenv *e, lval *a) { return builtin_op(e, a, "*"); }
lval *builtin_div(lenv *e, lval *a) { return builtin_op(e, a, "/"); }
lval *builtin_rem(lenv *e, lval *a) { return builtin_op(e, a, "%"); }
lval *builtin_pow(lenv *e, lval *a) { return builtin_op(e, a, "^"); }
lval *builtin_min(lenv *e, lval *a) { return builtin_op(e, a, "min"); }
lval *builtin_max(lenv *e, lval *a) { return builtin_op(e, a, "max"); }
lval *builtin_gt(lenv *e, lval *a) { return builtin_ord(e, a, ">"); };
lval *builtin_ge(lenv *e, lval *a) { return builtin_ord(e, a, ">="); };
lval *builtin_lt(lenv *e, lval *a) { return builtin_ord(e, a, "<"); };
lval *builtin_le(lenv *e, lval *a) { return builtin_ord(e, a, "<="); };
lval *builtin_eq(lenv *e, lval *a) { return builtin_cmp(e, a, "=="); };
lval *builtin_ne(lenv *e, lval *a) { return builtin_cmp(e, a, "!="); };

// Add builitins functions
void lenv_add_builtin(lenv *e, char *name, lbuiltin func) {
  lval *k = lval_sym(name);
  lval *v = lval_builtin(func);
  lenv_put(e, k, v);
  lval_del(k);
  lval_del(v);
}

void lenv_add_builtins(lenv *e) {
  // Variable Functions
  lenv_add_builtin(e, "\\", builtin_lambda);
  lenv_add_builtin(e, "def", builtin_def);
  lenv_add_builtin(e, "=", builtin_put);

  // List Functions
  lenv_add_builtin(e, "list", builtin_list);
  lenv_add_builtin(e, "head", builtin_head);
  lenv_add_builtin(e, "tail", builtin_tail);
  lenv_add_builtin(e, "eval", builtin_eval);
  lenv_add_builtin(e, "join", builtin_join);
  lenv_add_builtin(e, "cons", builtin_cons);
  lenv_add_builtin(e, "init", builtin_init);
  lenv_add_builtin(e, "len", builtin_len);

  // Mathematical Functions
  lenv_add_builtin(e, "+", builtin_add);
  lenv_add_builtin(e, "add", builtin_add);
  lenv_add_builtin(e, "-", builtin_sub);
  lenv_add_builtin(e, "sub", builtin_sub);
  lenv_add_builtin(e, "*", builtin_mul);
  lenv_add_builtin(e, "mul", builtin_mul);
  lenv_add_builtin(e, "/", builtin_div);
  lenv_add_builtin(e, "div", builtin_div);
  lenv_add_builtin(e, "%", builtin_rem);
  lenv_add_builtin(e, "rem", builtin_rem);
  lenv_add_builtin(e, "^", builtin_pow);
  lenv_add_builtin(e, "pow", builtin_pow);
  lenv_add_builtin(e, "min", builtin_min);
  lenv_add_builtin(e, "max", builtin_max);

  // Comparison Functions
  lenv_add_builtin(e, "==", builtin_eq);
  lenv_add_builtin(e, "!=", builtin_ne);
  lenv_add_builtin(e, ">", builtin_gt);
  lenv_add_builtin(e, ">=", builtin_ge);
  lenv_add_builtin(e, "<", builtin_lt);
  lenv_add_builtin(e, "<=", builtin_le);
  lenv_add_builtin(e, "if", builtin_if);

  // String Functions
  lenv_add_builtin(e, "load", builtin_load);
  lenv_add_builtin(e, "err", builtin_err);
  lenv_add_builtin(e, "print", builtin_print);
}

lval *lval_call(lenv *e, lval *f, lval *a) {
  if (f->builtin) {
    return f->builtin(e, a);
  }

  int given = a->count;
  int total = f->formals->count;

  while (a->count) {
    if (f->formals->count == 0) {
      lval_del(a);
      return lval_err("Function passed too many arguments! "
                      "Got: %i, Expected: %i",
                      given, total);
    }

    lval *sym = lval_pop(f->formals, 0);

    if (strcmp(sym->sym, "&") == 0) {
      if (f->formals->count != 1) {
        lval_del(a);
        return lval_err("Function format invalid! "
                        "Symbol '&' not followed by single symbol");
      }

      lval *nsym = lval_pop(f->formals, 0);
      lenv_put(f->env, nsym, builtin_list(e, a));
      lval_del(sym);
      lval_del(nsym);
      break;
    }

    lval *val = lval_pop(a, 0);
    lenv_put(f->env, sym, val);
    lval_del(sym);
    lval_del(val);
  }

  lval_del(a);

  if (f->formals->count > 0 && strcmp(f->formals->cell[0]->sym, "&") == 0) {

    if (f->formals->count != 2) {
      return lval_err("Function format invalid. "
                      "Symbol '&' not followed by single symbol.");
    }

    lval_del(lval_pop(f->formals, 0));

    lval *sym = lval_pop(f->formals, 0);
    lval *val = lval_qexpr();
    lenv_put(f->env, sym, val);
    lval_del(sym);
    lval_del(val);
  }

  if (f->formals->count == 0) {
    f->env->par = e;
    return builtin_eval(f->env, lval_add(lval_sexpr(), lval_copy(f->body)));
  } else {
    return lval_copy(f);
  }
}

lval *lval_eval_sexpr(lenv *e, lval *v) {

  for (int i = 0; i < v->count; i++) {
    v->cell[i] = lval_eval(e, v->cell[i]);
  }
  for (int i = 0; i < v->count; i++) {
    if (v->cell[i]->type == LVAL_ERR) {
      return lval_take(v, i);
    }
  }

  if (v->count == 0) {
    return v;
  }
  if (v->count == 1) {
    return lval_eval(e, lval_take(v, 0));
  }

  lval *f = lval_pop(v, 0);
  if (f->type != LVAL_FUN) {
    lval *err = lval_err("S-Expression starts with incorrect type. "
                         "Got %s, Expected %s.",
                         ltype_name(f->type), ltype_name(LVAL_FUN));
    lval_del(f);
    lval_del(v);
    return err;
  }

  lval *result = lval_call(e, f, v);
  lval_del(f);
  return result;
}

int main(int argc, char **argv) {
  // Create some Parsers
  Number = mpc_new("number");
  Symbol = mpc_new("symbol");
  String = mpc_new("string");
  Comment = mpc_new("comment");
  Sexpr = mpc_new("sexpr");
  Qexpr = mpc_new("qexpr");
  Expr = mpc_new("expr");
  Cumunisp = mpc_new("cumunisp");

  // Define Parsers
  mpca_lang(MPCA_LANG_DEFAULT,
            "                                                     \
      number   : /-?[0-9]+(\\.[0-9]*)?/ ;                             \
      symbol   : /[a-zA-Z0-9_+\\-*\\/\\\\=<>!&%^]+/ ;  \
      string   : /\"(\\\\.|[^\"])*\"/ ; \
      comment  : /;[^\\r\\n]*/ ; \
      sexpr    : '(' <expr>* ')' ; \
      qexpr    : '{' <expr>* '}' ; \
      expr     : <number> | <symbol> | <string> \
               | <comment>| <sexpr> | <qexpr>;                           \
      cumunisp    : /^/ <expr>* /$/ ;             \
    ",
            Number, Symbol, String, Comment, Sexpr, Qexpr, Expr, Cumunisp);

  lenv *e = lenv_new();
  lenv_add_builtins(e);

  // Interactive prompt
  if (argc == 1) {

    // Print Version and Exit Information
    puts("Cumunisp Version 0.0.0.0.3");
    puts("Press Ctrl+c to Exit\n");

    while (1) {
      // Output prompt
      char *input = readline("cumunisp> ");
      // Add input to history
      add_history(input);

      // Attempt to Parse the user Input
      mpc_result_t r;
      if (mpc_parse("<stdin>", input, Cumunisp, &r)) {

        lval *x = lval_eval(e, lval_read(r.output));
        lval_println(x);
        lval_del(x);

        mpc_ast_delete(r.output);
      } else {
        // Otherwise print the Error
        mpc_err_print(r.error);
        mpc_err_delete(r.error);
      }

      // Free retrieved input
      free(input);
    }
  }

  // Supplied with list of files
  if (argc >= 2) {

    // loop over each supplied filename (starting from 1)
    for (int i = 1; i < argc; i++) {

      // Argument list with a single argument, the filename
      lval *args = lval_add(lval_sexpr(), lval_str(argv[i]));

      // Pass to builtin load and get the result
      lval *x = builtin_load(e, args);

      // If the result is an error be sure to print it
      if (x->type == LVAL_ERR) {
        lval_println(x);
      }
      lval_del(x);
    }
  }
  lenv_del(e);
  // Undefine and delete Parsers
  mpc_cleanup(8, Number, Symbol, String, Comment, Sexpr, Qexpr, Expr, Cumunisp);

  return 0;
}
