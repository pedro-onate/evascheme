#include <string.h>
#include <ctype.h>
#include "eva.h"

enum Tag {
  kReferenceTag = 0x0, /* 0b000 */
  kImmediateTag = 0x1, /* 0b001 */
  kBooleanTag   = 0x2, /* 0b010 */
  kIntegerTag   = 0x3, /* 0b011 */
  kSymbolTag    = 0x4, /* 0b100 */
  kCharacter    = 0x5  /* 0b101 */
};

struct SymbolTable {char* symbols[65536]; int id;};
struct Heap {void* buffer; void* ptr;};

#define SCM_TAG_BITS        3
#define SCM_TAG_MASK        0x7
#define SCM_PTR_BITS(ptr)   (intptr_t)(ptr)
#define SCM_GET_TAG(ptr)    SCM_PTR_BITS(ptr) & SCM_TAG_MASK
#define SCM_TAGGED(v, t)    (struct ScmVal*)(SCM_PTR_BITS(v) << SCM_TAG_BITS | t)
#define SCM_UNTAG(t, v)     (t)(SCM_PTR_BITS(v) >> SCM_TAG_BITS)

static struct ScmVal* env;
static struct Heap heap;
static struct SymbolTable symtab;
static struct ScmVal* DEFINE, *LAMBDA, *IF, *BEGIN, *QUOTE;
struct ScmVal*        SCM_NIL, *SCM_FALSE, *SCM_TRUE, *SCM_UNBOUND, *SCM_UNSPECIFIED, *SCM_EOF;

void* (*alloc)(size_t);
static FILE* ostream;

enum ScmType Scm_type(struct ScmVal* exp) {
  if (!exp) {
    return INVALID;
  }
  switch(SCM_GET_TAG(exp)) {
    case kReferenceTag: return exp->type;
    case kImmediateTag: return SCM_UNTAG(int, exp);
    case kBooleanTag:   return BOOLEAN;
    case kIntegerTag:   return INTEGER;
    case kSymbolTag:    return SYMBOL;
    case kCharacter:    return CHARACTER;
    default:            return INVALID;
  }
}

struct ScmVal* Scm_Integer_new(long value) {
  return SCM_TAGGED(value, kIntegerTag);
}

struct ScmVal* Scm_Boolean_new(int value) {
  return SCM_TAGGED(value, kBooleanTag);
}

struct ScmVal* Scm_Symbol_new(char* value) {
  int i;
  for(i = 0; i < symtab.id; i++) {
    if (strcmp(symtab.symbols[i], value) == 0) {
      return SCM_TAGGED(i, kSymbolTag);
    }
  }
  symtab.symbols[symtab.id] = alloc(strlen(value) + 1);
  strcpy(symtab.symbols[symtab.id], value);
  return SCM_TAGGED(symtab.id++, kSymbolTag);
}

struct ScmVal* Scm_Pair_new(struct ScmVal* head, struct ScmVal* tail) {
  struct Pair* cons_cell = alloc(sizeof(struct Pair));
  cons_cell->type = PAIR;
  cons_cell->head = head;
  cons_cell->tail = tail;
  return (struct ScmVal*)cons_cell;
}

struct ScmVal* Scm_Pair_car(struct ScmVal* cons_cell) {
  return ((struct Pair*)cons_cell)->head;
}

struct ScmVal* Scm_Pair_cdr(struct ScmVal* cons_cell) {
  return ((struct Pair*)cons_cell)->tail;
}

void Scm_Pair_set_head(struct ScmVal* pair, struct ScmVal* value) {
  ((struct Pair*)pair)->head = value;
}

void Scm_Pair_set_tail(struct ScmVal* pair, struct ScmVal* value) {
  ((struct Pair*)pair)->tail = value;
}

struct ScmVal* Scm_Procedure_new(struct ScmVal* (*fptr)(struct ScmVal*)) {
  struct Procedure* cfunc = alloc(sizeof(struct Procedure));
  cfunc->type  = PROCEDURE;
  cfunc->fptr  = fptr;
  return (struct ScmVal*)cfunc;
}

struct ScmVal* Scm_Closure_new(struct ScmVal* formals, struct ScmVal* body, struct ScmVal* env) {
  struct Closure* closure = alloc(sizeof(struct Closure));
  closure->type    = CLOSURE;
  closure->formals = formals;
  closure->body    = body;
  closure->env     = env;
  return (struct ScmVal*)closure;
}

static struct ScmVal* bind_args(struct ScmVal* formals, struct ScmVal* args) {
  if (formals == SCM_NIL || args == SCM_NIL) {
    return SCM_NIL;
  } else if (Scm_type(formals) == PAIR) {
    return cons(cons(car(formals), car(args)), 
      bind_args(cdr(formals), cdr(args)));
  } else {
    return cons(cons(formals, args), SCM_NIL);
  }
}

struct ScmVal* Scm_Env_new(struct ScmVal* formals, struct ScmVal* args, struct ScmVal* parent){
  struct ScmVal* bindings;
  if (Scm_type(formals) == SYMBOL) {
    bindings = cons(cons(formals, args), SCM_NIL);  
  } else {
    bindings = bind_args(formals, args);
  }
  return cons(bindings, parent);
}

struct ScmVal* assq(struct ScmVal* lst, struct ScmVal* key){
  if (lst == SCM_NIL) { 
    return SCM_NIL;
  } else if (caar(lst) == key) {
    return car(lst);
  } else {
    return assq(cdr(lst), key);
  }
}

static struct ScmVal* lookup_binding(struct ScmVal* env, struct ScmVal* symbol) {
  struct ScmVal* binding;
  binding  = assq(car(env), symbol);
  if (binding != SCM_NIL) {
    return binding;
  } else if (cdr(env) != SCM_NIL) {
    return lookup_binding(cdr(env), symbol);
  } else {
    return NULL;
  }
}

struct ScmVal* Scm_define_symbol(struct ScmVal* env, struct ScmVal* symbol, struct ScmVal* value) {
  struct ScmVal* binding;
  binding = lookup_binding(env, symbol);
  if (binding) {
    set_cdr(binding, value);
  } else {
    set_car(env, cons(cons(symbol, value), car(env)));
  }
  return symbol;
}

struct ScmVal* Scm_lookup_symbol(struct ScmVal* env, struct ScmVal* symbol) {
  struct ScmVal* binding = lookup_binding(env, symbol);
  return binding ? cdr(binding) : SCM_UNBOUND;
}

static int peekc(FILE* stream) {
  int c;
  c = getc(stream);
  ungetc(c, stream); 
  return c;
}

static struct ScmVal* parse_atom(FILE* stream) {
  char  buf[1028] = "\0";
  char  *pbuf     = buf;
  while(isspace(peekc(stream))) getc(stream);
  if (peekc(stream) == '#') {
    *pbuf++ = getc(stream);
    if (strchr("tf", peekc(stream))) {
      return Scm_Boolean_new(getc(stream) == 't');
    } else {
      goto SYMBOL;
    }
  } else if (isdigit(peekc(stream))) {
    while(isdigit(peekc(stream))) *pbuf++ = getc(stream);
    *pbuf = '\0';
    return Scm_Integer_new(atoi(buf));
  } else {
    SYMBOL: while(!strchr(" \t\n)", peekc(stream))) *pbuf++ = getc(stream);
    *pbuf = '\0';
    return Scm_Symbol_new(buf);
  };
}

static struct ScmVal* parse_list(FILE* stream) {
  struct ScmVal* head, *tail;
  while(isspace(peekc(stream))) getc(stream);
  if (peekc(stream) == ')' && getc(stream)) {
    return SCM_NIL;
  } else if (peekc(stream) == '.' && getc(stream)) {
    tail = parse_atom(stream);
    parse_list(stream);
    return tail;
  } else {
    SYMBOL:;
    head = Scm_parse(stream);
    return cons(head, parse_list(stream));
  }  
}

struct ScmVal* Scm_parse(FILE* stream) {
  if (!stream || peekc(stream) == EOF) {return SCM_EOF;}
  while(isspace(peekc(stream))) getc(stream);
  if (peekc(stream) == '(' && getc(stream)) {
    return parse_list(stream);
  } else if (peekc(stream) == '\'' && getc(stream)) {
    return cons(Scm_Symbol_new("quote"), cons(Scm_parse(stream), SCM_NIL));
  } else {
    return parse_atom(stream);
  }
}

static void print_list(struct ScmVal* exp) {
  struct ScmVal* tail;
  Scm_print(car(exp));
  tail = cdr(exp);
  if (tail != SCM_NIL) {
    if (Scm_type(tail) == PAIR) {
      fprintf(ostream, " "); print_list(tail);
    } else {
      fprintf(ostream, " . "); Scm_print(tail);
    }
  }
}

void Scm_print(struct ScmVal* exp) {
  if (!exp) {
    return;
  }
  switch(Scm_type(exp)) {
    case NIL:         fprintf(ostream, "()");                                      break;
    case INTEGER:     fprintf(ostream, "%ld", SCM_UNTAG(long, exp));               break;
    case BOOLEAN:     fprintf(ostream, "#%c", exp == SCM_TRUE ? 't' : 'f');        break;
    case PAIR:        fprintf(ostream, "("); print_list(exp); fprintf(ostream, ")");         break;
    case SYMBOL:      fprintf(ostream, "%s", symtab.symbols[SCM_UNTAG(int, exp)]); break;
    case CLOSURE:     fprintf(ostream, "#<closure>");                              break;
    case PROCEDURE:   fprintf(ostream, "#<procedure>");                            break;
    case UNSPECIFIED:                                                    break;
    case UNBOUND:     fprintf(ostream, "#<unbound>");                              break;
    case EOFILE:      fprintf(ostream, "#<eof>");                                  break;
    default:          fprintf(ostream, "undefined type");                          break;
  }
}

static struct ScmVal* eval_args(struct ScmVal* args, struct ScmVal* env) {
  if (Scm_type(args) == NIL) {
    return SCM_NIL;
  } else {
    return cons(Scm_eval(car(args), env), eval_args(cdr(args), env));
  }
}

struct ScmVal* Scm_eval(struct ScmVal* exp, struct ScmVal* env) {
  int               type;
  struct ScmVal*    op;
  struct ScmVal*    res;
  struct Procedure* cfunc;
  struct Closure*   closure;

  EVAL:;
  type = Scm_type(exp);
  if (type == PAIR){
    op = car(exp);
    if (op == DEFINE) {
      return Scm_define_symbol(env, cadr(exp), Scm_eval(caddr(exp), env));
    } else if (op == IF) {
      if (Scm_eval(cadr(exp), env) != SCM_FALSE) {
        exp = caddr(exp);
        goto EVAL;
      } else if (cdddr(exp) != SCM_NIL){
        exp = cadddr(exp);
        goto EVAL;
      } else {
        return SCM_UNSPECIFIED;
      }
    } else if (op == QUOTE) {
      return cadr(exp);
    } else if (op == LAMBDA) {
      return Scm_Closure_new(cadr(exp), cddr(exp), env);
    } else if (op == BEGIN) {
      exp = cdr(exp);
      EVAL_SEQ:;
      while(cdr(exp) != SCM_NIL) {
        Scm_eval(car(exp), env);
        exp = cdr(exp);
      }
      exp = car(exp);
      goto EVAL;
    } else {
      op = Scm_eval(op, env);
      type = Scm_type(op);
      if (type == PROCEDURE) {
        cfunc = (struct Procedure*)op;
        return cfunc->fptr(eval_args(cdr(exp), env));
      } else if (type == CLOSURE) {
        closure = (struct Closure*)op;
        env = Scm_Env_new(closure->formals, eval_args(cdr(exp), env), closure->env);
        exp = closure->body;
        goto EVAL_SEQ;
      }
    }
    return NULL;
  } else if(type == SYMBOL) {
    return Scm_lookup_symbol(env, exp);
  } else {
    return exp;
  }
}

static struct ScmVal* procedure_add(struct ScmVal* args) {
  long a = SCM_UNTAG(long, car(args));
  long b = SCM_UNTAG(long, cadr(args));
  return Scm_Integer_new(a + b);
}

static struct ScmVal* procedure_sub(struct ScmVal* args) {
  long a = SCM_UNTAG(long, car(args));
  long b = SCM_UNTAG(long, cadr(args));
  return Scm_Integer_new(a - b);
}

static struct ScmVal* procedure_mult(struct ScmVal* args) {
  long a = SCM_UNTAG(long, car(args));
  long b = SCM_UNTAG(long, cadr(args));
  return Scm_Integer_new(a * b);
}

static struct ScmVal* procedure_eq(struct ScmVal* args) {
  return Scm_Boolean_new(car(args) == cadr(args)); 
}

static struct ScmVal* procedure_car(struct ScmVal* args) {
  return car(car(args));
}

static struct ScmVal* procedure_cdr(struct ScmVal* args) {
  return cdr(car(args));
}

static struct ScmVal* procedure_eval(struct ScmVal* args) {
  return Scm_eval(car(args), env);
}

static struct ScmVal* procedure_read(struct ScmVal* args) {
  return Scm_parse(stdin);
}

static struct ScmVal* procedure_quit(struct ScmVal* args) {
  exit(0);
  return SCM_EOF;
}

#define ALIGN_N(p, n)     (p) % (n) == 0 ? (p) : (p) + ((n) - (p) % (n))
#define DEFAULT_ALIGNMENT 8

static void* bump_allocator(size_t sz) {
  uintptr_t block;
  block    = ALIGN_N((uintptr_t)heap.ptr, 8);
  heap.ptr = (void*)(block + sz);
  return (void*)block;
}

void Scm_init(size_t heap_size) {
  heap.buffer     = malloc(heap_size);
  heap.ptr        = heap.buffer;

  alloc           = bump_allocator;
  DEFINE          = Scm_Symbol_new("define");
  LAMBDA          = Scm_Symbol_new("lambda");
  IF              = Scm_Symbol_new("if");
  BEGIN           = Scm_Symbol_new("begin");
  QUOTE           = Scm_Symbol_new("quote");
  SCM_NIL         = SCM_TAGGED(NIL, kImmediateTag);
  SCM_FALSE       = Scm_Boolean_new(0);
  SCM_TRUE        = Scm_Boolean_new(1);
  SCM_UNBOUND     = SCM_TAGGED(UNBOUND, kImmediateTag);
  SCM_UNSPECIFIED = SCM_TAGGED(UNSPECIFIED, kImmediateTag);
  SCM_EOF         = SCM_TAGGED(EOFILE, kImmediateTag);
  env             = Scm_Env_new(SCM_NIL, SCM_NIL, SCM_NIL);

  Scm_define_symbol(env, Scm_Symbol_new("+"), Scm_Procedure_new(procedure_add));
  Scm_define_symbol(env, Scm_Symbol_new("-"), Scm_Procedure_new(procedure_sub));
  Scm_define_symbol(env, Scm_Symbol_new("*"), Scm_Procedure_new(procedure_mult));
  Scm_define_symbol(env, Scm_Symbol_new("="), Scm_Procedure_new(procedure_eq));
  Scm_define_symbol(env, Scm_Symbol_new("car"), Scm_Procedure_new(procedure_car));
  Scm_define_symbol(env, Scm_Symbol_new("cdr"), Scm_Procedure_new(procedure_cdr));
  Scm_define_symbol(env, Scm_Symbol_new("eval"), Scm_Procedure_new(procedure_eval));
  Scm_define_symbol(env, Scm_Symbol_new("read"), Scm_Procedure_new(procedure_read));
  Scm_define_symbol(env, Scm_Symbol_new("quit"), Scm_Procedure_new(procedure_quit));
}

/*
int main() {
  fprintf(ostream, ".------------------.\n");
  fprintf(ostream, "|  EvaScheme v0.1  |\n");
  fprintf(ostream, "'------------------'\n\n");

  Scm_init(2 << 29);

  while (true) {
    fprintf(ostream, "eva> ");
    Scm_print(Scm_eval(Scm_parse(stdin), env));
    fprintf(ostream, "\n"); 
  }
}
*/

/*
** server.c -- a stream socket server demo
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <signal.h>

#define PORT "3490"  // the port users will be connecting to

#define BACKLOG 10     // how many pending connections queue will hold

void sigchld_handler(int s)
{
    while(waitpid(-1, NULL, WNOHANG) > 0);
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    }

    return &(((struct sockaddr_in6*)sa)->sin6_addr);
}

int main(void)
{
    int sockfd, new_fd;  // listen on sock_fd, new connection on new_fd
    struct addrinfo hints, *servinfo, *p;
    struct sockaddr_storage their_addr; // connector's address information
    socklen_t sin_size;
    struct sigaction sa;
    int yes=1;
    char s[INET6_ADDRSTRLEN];
    int rv;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // use my IP

    if ((rv = getaddrinfo(NULL, PORT, &hints, &servinfo)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    // loop through all the results and bind to the first we can
    for(p = servinfo; p != NULL; p = p->ai_next) {
        if ((sockfd = socket(p->ai_family, p->ai_socktype,
                p->ai_protocol)) == -1) {
            perror("server: socket");
            continue;
        }

        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes,
                sizeof(int)) == -1) {
            perror("setsockopt");
            exit(1);
        }

        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            perror("server: bind");
            continue;
        }

        break;
    }

    if (p == NULL)  {
        fprintf(stderr, "server: failed to bind\n");
        return 2;
    }

    freeaddrinfo(servinfo); // all done with this structure

    if (listen(sockfd, BACKLOG) == -1) {
        perror("listen");
        exit(1);
    }

    sa.sa_handler = sigchld_handler; // reap all dead processes
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    printf("server: waiting for connections...\n");

    while(1) {  // main accept() loop
        sin_size = sizeof their_addr;
        new_fd = accept(sockfd, (struct sockaddr *)&their_addr, &sin_size);
        if (new_fd == -1) {
            perror("accept");
            continue;
        }

        inet_ntop(their_addr.ss_family,
            get_in_addr((struct sockaddr *)&their_addr),
            s, sizeof s);
        printf("server: got connection from %s\n", s);

        if (!fork()) { // this is the child process
          close(sockfd); // child doesn't need the listener
          /*if (send(new_fd, "Hello, world!", 13, 0) == -1)
              perror("send");
          close(new_fd);
          exit(0);*/
          FILE* iostream = fdopen(new_fd, "r+");
          ostream = iostream;

          fprintf(ostream, ".------------------.\n");
          fprintf(ostream, "|  EvaScheme v0.1  |\n");
          fprintf(ostream, "'------------------'\n\n");

          Scm_init(2 << 29);

          while (true) {
            fprintf(ostream, "eva> ");
            Scm_print(Scm_eval(Scm_parse(iostream), env));
            fprintf(ostream, "\n"); 
          }
        }
        close(new_fd);  // parent doesn't need this
    }

    return 0;
}