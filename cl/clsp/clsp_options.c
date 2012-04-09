/*
 * Copyright (C) 2012 Jan Pokorny <pokorny_jan@seznam.cz>
 *
 * This file is part of predator.
 *
 * predator is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * predator is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with predator.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "clsp.h"

#include <ctype.h>   /* isdigit */
#include <assert.h>  /* assert */

#define USE_INT3_AS_BRK
#include "trap.h"

#include "clsp_options.h"

/* see options_gather */
#define EXIT_OK    -1
#define EXIT_BAD   ec_opt
#define CONTINUE   0

/* defaults */
#define DEF_FD_CL      STDERR_FILENO
#define DEF_FD_SPARSE  STDERR_FILENO
#define DEF_FD_DEBUG   STDOUT_FILENO

#define DEF_FD_VAL(what)  DEF_FD_##what
#define DEF_FD_STR(what)  STRINGIFY(DEF_FD_##what)

#define DEF_CLR_CL      CLR_DARKGRAY
#define DEF_CLR_SPARSE  CLR_RED
#define DEF_CLR_DEBUG   CLR_LIGHTGRAY

#define DEF_CLR_VAL_(name,code)  clr_##name
#define DEF_CLR_VAL(what)        APPLY(DEF_CLR_VAL_, DEF_CLR_##what)

#define DEF_CLR_STR_(name,code)  code #name CLR_TERMINATE
#define DEF_CLR_STR(what)        APPLY(DEF_CLR_STR_, DEF_CLR_##what)

/* "is binary option" X option prefix product */
#define OPT_SHORT      false,"-"
#define OPT_SHORT_BIN  true ,"-"
#define OPT_LONG       false,"--"
#define OPT_LONG_BIN   true ,"--"
#define OPT_CL         false,"-cl-"
#define OPT_CL_BIN     true ,"-cl-"

#define ISBIN(what)            APPLY(ISBIN_,OPT_##what)
#define PREFIX(what)           APPLY(PREFIX_,OPT_##what)
#define ISBIN_(isbin,prefix)   isbin
#define PREFIX_(isbin,prefix)  prefix


static const char *d_str[d_last] = {
#define X(name,desc)  [d_##name] = desc,
    DLIST(X)
#undef X
};


/* command-line arguments used internally are exchanged for this */
static const char *const empty = "";


/** general helpers *******************************************************/


/**
    Positive number converter (from string).

    @todo  More checks.
 */
static inline int
get_positive_num(const char *what, const char *value)
{
    if (!isdigit(value[0]))
        DIE( ECODE(OPT,"option %s: not a numeric value: %s",what,value) );
    int ret = strtol(value, NULL, 10);
    if (ret < 0)
        DIE( ECODE(OPT,"option %s: must be positive number",what) );
    return ret;
}

/**
    File descriptor specification converter (from string to int).

    @note Single @c 'D' characters stands for special "deferred" stream
          available as per @c accept_deferred argument.
 */
static inline int
get_fd(const char *what, const char *value, bool accept_deferred)
{
    if (accept_deferred && value[0] == 'D' && value[1] == '\0')
        return opts_fd_deferred;
    return get_positive_num(what, value);
}

/**
    Color specification converter (from string to enum color).
 */
static inline int
get_clr(const char *what, const char *value)
{
#define X(name, code) \
    else if (STREQ(value, clr_str[clr_##name])) return clr_##name;

    if (!value)
        return clr_undef;
    CLRLIST(X)
    else
        DIE( ECODE(OPT,"option %s: must be enumerated color or empty",what) );
#undef X
}


/** output ****************************************************************/


/**
    Version printer.
 */
static void
print_version()
{
    PUT(out, "%s", GIT_SHA1);
}

/**
    Help printer.

    @param[in] cmd   Name of the program (like in argv[0]).
 */
static void
print_help(const char *cmd)
{
#define _(...)       PUT(out, __VA_ARGS__);
#define __           PUT(out, "");
#define ___          ""
#define L(lo, cmt)   PUT(out, "  %-28s%s", PREFIX(LONG) lo, cmt);
#define S(so, cmt)   PUT(out, "  %-28s%s", PREFIX(SHORT) so, cmt);
#define I(ign, cmt)  PUT(out, "  %-28s%s", ign, cmt);
#define B(so, lo, cmt) \
    PUT(out, "  %-28s%s", PREFIX(SHORT) so ", " PREFIX(LONG) lo, cmt);
#define C(co, cmt)   PUT(out, "  %-28s%s", PREFIX(CL) co, cmt);
#define V(v, cmt)    PUT(out, "%8d                      %s", v, cmt);
#define X(stmt)      stmt
#define O(cmt)       PUT(out, "[" cmt "]");
    char buf[1024], *ptr = buf;
    int i,j;
    NORETWRN(setvbuf(STREAM(out), NULL, _IOFBF, 0));  /* flush block later */
    _("Sparse-based Code Listener frontend, version %s"               ,GIT_SHA1)
    __                                                                         ;
    _("usage: %s (INT-OPTS|CL-OPTS|CL-PLUGIN[:ARGS]|SPARSE-OPTS)* file ...",cmd)
    __                                                                         ;
#ifndef HAS_CL
    _("As no Code Listener plugin was built-in (no one to serve as a base one" )
    _("at hand), at least one such has to be provided in the form of a shared" )
    _("library containing the symbols of the interface (plugins targeted for"  )
    _("GCC should be compatible);  see `%s' below.", PREFIX(CL) "plugin"       )
    __                                                                         ;
#endif
    _("This Code Listener front-end defines a few internal options:"           )
    B("h", "help"          , "Prints this help text"                           )
    L("version"            , "Prints the version information"                  )
    B("f", "fork"          , "Do fork (only to show sparse exit status {0,1})" )
    O("specification of file descriptors, use `FD>file' redirection for FD > 2")
    L("fd-cl=FD"           , "for cl messages; def.: "           DEF_FD_STR(CL))
    I(___                  , "(fatal errors are always produced on stderr)"    )
    L("fd-sparse=FD"       , "for sparse, D=deferred; def.: "DEF_FD_STR(SPARSE))
    L("fd-debug=FD"        , "for debugging messages; def.: " DEF_FD_STR(DEBUG))
    O("specification of colors (see below), only used for terminal output"     )
    L("clr-cl[=COLOR]"     , "for cl messages; def.: "          DEF_CLR_STR(CL))
    L("clr-sparse[=COLOR]" , "for sparse; def.: "           DEF_CLR_STR(SPARSE))
    L("clr-debug[=COLOR]"  , "for debugging messages; def.: "DEF_CLR_STR(DEBUG))
    X(for (i = clr_first   ;                       i < (clr_last-1)/8 + 1; i++))
    X(for (j=0, *ptr++='\n'; j < ((i+1)*8/clr_last ? i*8 % clr_last : 8);  j++))
    X(ptr += snprintf(ptr,sizeof(buf)+buf-ptr,"%s%-10s%s",CLR_PRINTARG(i*8+j));)
    X(*ptr = '\0'          ;                         ptr = buf; _("%s", ++ptr);)
    B("d", "debug[=MASK]"  , "Debug, selectively if MASK specified (see below)")
    X(for (i = d_first     ;                                   i < d_last; i++))
    V(DVALUE(i)            ,                                           d_str[i])
    __                                                                         ;
    _("From the options affecting the other end of Code Listener interface,"   )
    _("one particularly important is a way to load other listeners as plugins:")
    C("plugin=FILE[:ARGS]" , "Path to a shared library containg symbols of"    )
    I(___                  , "Code Listener (for instance, GCC plugins can be" )
#ifdef HAS_CL
    I(___                  , "used directly), passing it optional ARGS"        )
#else
    I(___                  , "used directly), passing it optional ARGS;"       )
    I(___                  , "the first one is a base one and must be provided")
#endif
    __                                                                         ;
#ifdef HAS_CL
    _("and specifically these options are for a base (built-in) Code Listener:")
#else
    _("and specifically these options are for a base (provided) Code Listener:")
#endif
    C("default-output"     , "Use Code Listener's built-ins to print messages" )
    C("pprint[=FILE]"      , "Dump pretty-printed linearized code"             )
    C("pprint-types"       , "Add type information to pretty-printed code"     )
    C("pprint-switch-to-if", "Unfold `switch' into series of `if' statements " )
    C("gen-cfg[=MAIN_FILE]", "Generate control flow graphs (as per MAIN_FILE)" )
    C("gen-type[=FILE]"    , "Generate type graphs (to FILE if specified)"     )
    C("debug-location"     , "Output location as first step throughout the run")
    C("debug-level[=LEVEL]", "Debug (according to LEVEL if specified)"         )
    __                                                                         ;
    _("For `sparse-opts-args' (including the specification of the target[s])," )
    _("see sparse documentation;  generally, there is some level of"           )
    _("compatibility with GCC and unrecognized options are ignored anyway."    )
    _("To name a few notable notable ones (referring to current version):"     )
    S("v"                  , "Report more defects, more likely false positives")
    S("m64"                , "Suppose 64bit architecture (32bit by default)"   )
    S("DNAME[=VALUE]"      , "Define macro NAME (holding value VALUE if spec.)")
    S("W[no[-]]WARNING"    , "Request/not to report WARNING-related issues;"   )
    I(___                  , "`sparse-all' covers all available warnings"      )
    __                                                                         ;
    _("Return values:")    ;            for (int i = ec_first; i < ec_last; i++)
    V(ECVALUE(i)           ,                                          ec_str[i])
    NORETWRN(fflush(STREAM(out)));
    /* about to exit -- no change in buffering */
#undef O
#undef X
#undef V
#undef C
#undef B
#undef I
#undef S
#undef L
#undef ___
#undef __
#undef _
}


/** options processing ****************************************************/


/* convenient shortcuts (expects using "opts" for "struct options *") */
#define INTERNALS(what)  (opts->internals.what)
#define CL(what)         (opts->cl.what)
#define SPARSE(what)     (opts->sparse.what)


/**
    The first/initializing phase of gathering options.
 */
static void
options_initialize(struct options *opts)
{
    opts->finalized = false;

    INTERNALS(fork) = false;
    INTERNALS(fd) = (struct oi_fd) {
        .cl     = DEF_FD_VAL(CL),
        .sparse = DEF_FD_VAL(SPARSE),
        .debug  = DEF_FD_VAL(DEBUG)
    };
    INTERNALS(clr) = (struct oi_clr) {
        .cl     = DEF_CLR_VAL(CL),
        .sparse = DEF_CLR_VAL(SPARSE),
        .debug  = DEF_CLR_VAL(DEBUG)
    };
    INTERNALS(debug) = 0;

    CL(listeners.cnt)  = 0;
    CL(listeners.arr)  = NULL;
    CL(default_output) = false;
    CL(pprint.enable)  = false;
    CL(gencfg.enable)  = false;
    CL(gentype.enable) = false;
    CL(debug) = (struct oc_debug) { .location=false, .level=0 };
}


#define PREFIXEQ(argv, i, type, opt)                                          \
    (strncmp(argv[i],PREFIX(type) opt,CONST_STRLEN(PREFIX(type) opt))         \
        ? NULL                                                                \
        : (((ISBIN(type) && argv[i][CONST_STRLEN(PREFIX(type) opt)] != '\0')  \
            ? PUT(err, "option %s: binary option with argument (or clash?)",  \
                  argv[i])                                                    \
            : 0)                                                              \
            , &argv[i][CONST_STRLEN(PREFIX(type) opt)]))
#define VALUE_(argv, i, str, testnextchar)                                    \
    (*str != '\0'                                                             \
        ? (((*str != '=' || *++str != '\0')) ? str : (str = NULL))            \
        : (argv[i+1] && testnextchar(argv[i+1][0]))                           \
            ? (argv[i++] = empty, str = argv[i])                              \
            : (argv[i] = empty, str = NULL))
#define NONOPT(x)  x != '-'
#define VALUE(argv, i, str)  VALUE_(argv, i, str, NONOPT)
#define ISNUM(x)   isdigit(x)
#define NUMVAL(argv, i, str)  VALUE_(argv, i, str, ISNUM)


static inline int
options_proceed_internal(struct options *opts,

{
    int ret = 1;  /* optimistic default */
    const char *value;

    if (PREFIXEQ(argv,i,SHORT_BIN,"h")
      || PREFIXEQ(argv,i,LONG_BIN,"help")) {

        print_help(argv[0]);
        ret = -1;

    } else if ((value = PREFIXEQ(argv,i,LONG_BIN,"version"))) {

        print_version();
        ret = -1;

    } else if ((value = PREFIXEQ(argv,i,SHORT_BIN,"f"))
      || (value = PREFIXEQ(argv,i,LONG_BIN,"fork"))) {

        /* do not collide with "-fstrict-aliasing" etc. */
        if (*value == '\0')
           INTERNALS(fork) = true;
        else
            ret = 2;  /* return back as unconsumed */

    } else if ((value = PREFIXEQ(argv,i,LONG,"fd-fd"))
      || (value = PREFIXEQ(argv,i,LONG,"fd-sparse"))
      || (value = PREFIXEQ(argv,i,LONG,"fd-debug"))) {

        const char *arg = argv[i];  /* preserve across VALUE */
        if (VALUE(argv,i,value)) {
            /* exploiting the difference of initial chars */
            int *to_set;
            switch (arg[CONST_STRLEN(PREFIX(LONG))+3]) {
                case 'c': to_set = &INTERNALS(fd.cl);     break;
                case 's': to_set = &INTERNALS(fd.sparse); break;
                case 'd': to_set = &INTERNALS(fd.debug);  break;
                default: DIE( ECODE(OPT,"unexpected case") );
            }
            *to_set = get_fd(arg, value,
                             (to_set == &INTERNALS(fd.sparse)));
        } else {
            DIE( ECODE(OPT,"option %s: omitted value",arg) );
        }

    } else if ((value = PREFIXEQ(argv,i,LONG,"clr-cl"))
      || (value = PREFIXEQ(argv,i,LONG,"clr-sparse"))
      || (value = PREFIXEQ(argv,i,LONG,"clr-debug"))) {

        const char *arg = argv[i];  /* preserve across VALUE */
        /* exploiting the difference of initial chars */
        enum color *to_set;
        switch (arg[CONST_STRLEN(PREFIX(LONG))+4]) {
            case 'c': to_set = &INTERNALS(clr.cl);     break;
            case 's': to_set = &INTERNALS(clr.sparse); break;
            case 'd': to_set = &INTERNALS(clr.debug);  break;
            default: DIE( ECODE(OPT,"unexpected case") );
        }
        *to_set = get_clr(arg, VALUE(argv,i,value));

    } else if ((value = PREFIXEQ(argv,i,SHORT,"d"))
      || (value = PREFIXEQ(argv,i,LONG,"debug"))) {

        if (!NUMVAL(argv,i,value))
            INTERNALS(debug) = ~0;
        else
            INTERNALS(debug) = get_positive_num("debug", value);

    } else {
        ret = 0;
    }

    return ret;
}

static inline int
options_proceed_cl(struct options *opts,
{
    int ret = 1;  /* optimistic default */
    const char *value;

    if ((value = PREFIXEQ(argv,i,CL,"plugin"))) {

        const char *arg = argv[i];  /* preserve across VALUE */
        if (VALUE(argv,i,value))
            *(MEM_ARR_APPEND(CL(listeners.arr), CL(listeners.cnt)))
                = value;
        else
            DIE( ECODE(OPT,"option %s: omitted value",arg) );

    } else if (PREFIXEQ(argv,i,CL_BIN,"default-output")) {

        CL(default_output) = true;

    } else if ((value = PREFIXEQ(argv,i,CL,"pprint"))) {

        CL(pprint.enable)       = true;
        CL(pprint.file)         = VALUE(argv,i,value);
        CL(pprint.types)        = false;
        CL(pprint.switch_to_if) = false;

    } else if (PREFIXEQ(argv,i,CL_BIN,"pprint-types")) {

        if (!CL(pprint.enable))
            PUT(err, "option %s: cannot be used before %s",
                PREFIX(CL) "pprint-types", PREFIX(CL) "pprint");
        else
            CL(pprint.types) = true;

    } else if (PREFIXEQ(argv,i,CL_BIN,"pprint-switch-to-if")) {

        if (!CL(pprint.enable))
            PUT(err, "option %s: cannot be used before %s",
                PREFIX(CL) "pprint-switch-to-if",
                PREFIX(CL) "pprint");
        else
            CL(pprint.switch_to_if) = true;

    } else if ((value = PREFIXEQ(argv,i,CL,"gen-cfg"))) {

        CL(gencfg.enable) = true;
        CL(gencfg.file)   = VALUE(argv,i,value);

    } else if ((value = PREFIXEQ(argv,i,CL,"gen-type"))) {

        CL(gentype.enable) = true;
        CL(gentype.file)   = VALUE(argv,i,value);

    } else if (PREFIXEQ(argv,i,CL_BIN,"debug-location")) {

        CL(debug.location) = true;

    } else if ((value = PREFIXEQ(argv,i,CL,"debug-level"))) {

        if (!NUMVAL(argv,i,value))
            CL(debug.level) = ~0;
        else
            CL(debug.level) = get_positive_num("debug-level", value);

    }
    /* TODO: remove? */
    /*} else if ((value = PREFIXEQ(argv,i,CL,"cl-args"))) {
        OPTS(peer_args) = VALUE(value)
            ? value
            : "";
      }*/
    else {

        /* nothing we recognise */
        ret = 0;

    }

    return ret;
}

/**
    The main phase of gathering options.

    We only handle known options/arguments and leave argv untouched
    behind us (our options should be guaranteed not to collide with
    sparse, though).
 */
static int
options_proceed(struct options *opts, int argc, const char *argv[])
{
    bool consume_options = true, consumed;
    int i = 1, kept = 1;
    const char *value;

    while (i < argc) {
        assert(empty != argv[i]);

        if (consume_options) {
            consumed = true;

            ret = options_proceed_internal(opts);
            ret = ret ? ret : options_proceed_cl(opts);

            if (ret) {
                switch (ret) {
                    case -1: return 0; /* help and the like, bail out */
                    case 2:  consumed = false; break;
                }
            } else if (PREFIXEQ(argv,i,CL,"" /* prefix only */)) {
                PUT(err, "option %s: this alone does not make sense", argv[i]);
            } else if ((value = PREFIXEQ(argv,i,LONG,"" /* "--" sep. */))) {
                if (*value == '\0')
                    consume_options = false;
                else
                    consumed = false;
            } else {
                /* unhandled opt/arg (probably for sparse) continue below */
                consumed = false;
            }

            if (consumed) {
                /* current item consumed (maybe more previous, not our deal) */
                argv[i++] = empty;
                continue;
            }
        }

        /* probably sparse options/argument (may be forced with "--") */ 
        argv[kept++] = argv[i];
        argv[i++] = empty;
    }

    return kept;
}

/**
    The last/finalizing phase of gathering options.

    @todo Check code listener if !HAS_CL, ...
 */
static void
options_finalize(struct options *opts, int argc, char *argv[])
{
#ifndef HAS_CL
    if (0 == CL(listeners.cnt))
        DIE( ECODE(OPT,"no Code Listener specified") );
#endif

    if (opts_fd_undef != INTERNALS(fd.cl)
      && CL(default_output)) {
        PUT(err, "option %s: does not make sense with %s",
                 PREFIX(LONG) "cl-fd",
                 PREFIX(CL) "default-output");
        INTERNALS(fd.cl) = opts_fd_undef;
    }

    if (opts_fd_undef != INTERNALS(fd.debug)
      && 0 == INTERNALS(debug)) {
        PUT(err, "option %s: does not make sense without %s",
                 PREFIX(LONG) "debug-fd",
                 PREFIX(LONG) "debug");
        INTERNALS(fd.cl) = opts_fd_undef;
    }

    SPARSE(argc) = argc;
    SPARSE(argv) = argv;

    opts->finalized = true;
}

/* see clsp_options.h */
int
options_gather(struct options **opts, int argc, char *argv[])
{
    assert(opts);
    assert(argv != NULL);

    int ret;
    struct options *new_opts;

    new_opts = malloc(sizeof(**opts));
    if (!new_opts)
        DIE( ERRNOCODE(OPT,"malloc") );

    options_initialize(new_opts);
    ret = options_proceed(new_opts, argc, (const char **) argv);

    switch (ret) {
        case 0:
            ret = EXIT_OK;
            break;
        case 1:
            if (1 < argc)
                PUT(err, "missing arguments (while some options specified)");
            else
                print_help(argv[0]);
            ret = EXIT_BAD;
            break;
        default:
            options_finalize(new_opts, ret, argv);
            ret = CONTINUE;
            break;
    }

    *opts = new_opts;
    return ret;
}

void
options_dispose(struct options *opts)
{
    CL(listeners.cnt) = 0;
    free(CL(listeners.arr));

    opts->finalized = false;
}

/* see clsp_options.h */
void
options_dump(const struct options *opts)
{
#define GET_YN(b)  (b) ? 'Y' : 'N'

    assert(opts && opts->finalized);

    char buf[256], *ptr = buf;

    PUT(debug, "------------\noptions dump\n------------");

    PUT(debug, "internals");
    PUT(debug, "\tfork:\t%c", GET_YN(INTERNALS(fork)));
    PUT(debug, "\tfd:\t{cl=%d, sparse=%d, debug=%d}",
               INTERNALS(fd.cl),
               INTERNALS(fd.sparse),
               INTERNALS(fd.debug));
    ptr += snprintf(ptr, sizeof(buf)+buf-ptr, "%scl: %s",
                    STREAMCLRBEGIN(debug), STREAMCLREND(debug));
    ptr += snprintf(ptr, sizeof(buf)+buf-ptr, "%s%s%s",
                    CLR_PRINTARG(INTERNALS(clr.cl)));
    ptr += snprintf(ptr, sizeof(buf)+buf-ptr, "%s, sparse: %s",
                    STREAMCLRBEGIN(debug), STREAMCLREND(debug));
    ptr += snprintf(ptr, sizeof(buf)+buf-ptr, "%s%s%s",
                    CLR_PRINTARG(INTERNALS(clr.sparse)));
    ptr += snprintf(ptr, sizeof(buf)+buf-ptr, "%s, debug: %s",
                    STREAMCLRBEGIN(debug), STREAMCLREND(debug));
    ptr += snprintf(ptr, sizeof(buf)+buf-ptr, "%s%s%s",
                    CLR_PRINTARG(INTERNALS(clr.debug)));
    ptr += snprintf(ptr, sizeof(buf)+buf-ptr, "%s", STREAMCLRBEGIN(debug));
    PUT(debug, "\tclr:\t{%s}", buf);
    PUT(debug, "\tdebug:\t%d", INTERNALS(debug));
    PUT(debug, "");


    PUT(debug, "cl");
    PUT(debug, "\tlisteners:\t%zu", CL(listeners.cnt));
    for (size_t i = 0; i < CL(listeners.cnt); i++)
        PUT(debug, "\t\t%s", CL(listeners.arr[i]));
    PUT(debug, "\tdefault_output:\t%c", GET_YN(CL(default_output)));

    if (CL(pprint.enable))
        PUT(debug, "\tpprint:\t{types=%c, switch_to_if=%c, file=%s}",
                   GET_YN(CL(pprint.types)),
                   GET_YN(CL(pprint.switch_to_if)),
                   CL(pprint.file));
    else
        PUT(debug, "\tpprint:\tN/A");

    if (CL(gencfg.enable))
        PUT(debug, "\tgencfg:\t{file=%s}",
                   CL(gencfg.file));
    else
        PUT(debug, "\tgencfg:\tN/A");

    if (CL(gentype.enable))
        PUT(debug, "\tgentype:\t{file=%s}",
                   CL(gentype.file));
    else
        PUT(debug, "\tgentype:\tN/A");

    PUT(debug, "\tdebug:\t{location=%c, level=%d}",
               GET_YN(CL(debug.location)),
               CL(debug.level));
    PUT(debug, "");


    PUT(debug, "sparse");
    PUT(debug, "\targc:\t%d", SPARSE(argc));
    PUT(debug, "\targv:\t%s",SPARSE(argv[0]));
    for (int i = 1; i < SPARSE(argc); i++)
        PUT(debug, "\t\t%s", SPARSE(argv[i]));


    PUT(debug, "------------");
}

#ifdef TEST
const char *const GIT_SHA1 = "someversion";
struct globals globals;

int
main(int argc, char *argv[])
{
    int ret;
    struct options *opts;

    STREAM(out) = stdout;
    STREAM(err) = stderr;

    ret = options_gather(&opts, argc, argv);

    if (ret)
        return (0 > ret) ? EXIT_SUCCESS : ret;

    options_dump(opts);

    return ret;
}
#endif
