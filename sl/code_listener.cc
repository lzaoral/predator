#include "code_listener.h"
#include "cl_factory.hh"
#include "cl_private.hh"

#include <cstdio>
#include <cstring>

#include <unistd.h>

static const char *app_name = "<cl uninitialized>";
static bool app_name_allocated = false;

static void cl_no_msg(const char *)
{
}

static void cl_def_msg(const char *msg)
{
    if (app_name)
        fprintf(stderr, "%s: %s\n", app_name, msg);
    else
        fprintf(stderr, "%s\n", msg);
}

static void cl_def_die(const char *msg)
{
    cl_def_msg(msg);
    exit(EXIT_FAILURE);
}

static struct cl_init_data init_data = {
    cl_def_msg,            // .debug
    cl_def_msg,            // .warn
    cl_def_msg,            // .error
    cl_def_msg,            // .error
    cl_def_die             // .die
};

void cl_debug(const char *msg)
{
    init_data.debug(msg);
}

void cl_warn(const char *msg)
{
    init_data.warn(msg);
}

void cl_error(const char *msg)
{
    init_data.error(msg);
}

void cl_note(const char *msg)
{
    init_data.note(msg);
}

void cl_die(const char *msg)
{
    // this call should never return (TODO: annotation?)
    init_data.die(msg);
    abort();
}

void cl_global_init(struct cl_init_data *data)
{
    init_data = *data;
}

void cl_global_init_defaults(const char *name, bool verbose)
{
    if (app_name_allocated)
        free((char *) app_name);

    app_name_allocated = false;

    if (name) {
        app_name = strdup(name);
        if (app_name)
            app_name_allocated = true;
        else
            CL_DIE("strdup failed");
    } else {
        app_name = NULL;
    }

    if (verbose)
        init_data.debug = cl_def_msg;
    else
        init_data.debug = cl_no_msg;

    init_data.warn   = cl_def_msg;
    init_data.error  = cl_def_msg;
    init_data.note   = cl_def_msg;
    init_data.die    = cl_def_die;
}

void cl_global_cleanup(void)
{
    if (app_name_allocated)
        free((char *)app_name);
}

ICodeListener* cl_obtain_from_wrap(struct cl_code_listener *wrap)
{
    return static_cast<ICodeListener *>(wrap->data);
}

// do not throw an exception through the pure C interface
#define CL_WRAP(fnc) do { \
    try { \
        ICodeListener *listener = cl_obtain_from_wrap(self); \
        listener->fnc(); \
    } \
    catch (...) { \
        CL_DIE("uncaught exception in CL_WRAP"); \
    } \
} while (0)

// TODO: merge this together with the previous macro definition
#define CL_WRAP_VA(fnc, ...) do { \
    try { \
        ICodeListener *listener = cl_obtain_from_wrap(self); \
        listener->fnc(__VA_ARGS__); \
    } \
    catch (...) { \
        CL_DIE("uncaught exception in CL_WRAP"); \
    } \
} while (0)

static void cl_wrap_reg_type_db(
            struct cl_code_listener *self,
            cl_get_type_fnc_t       fnc,
            void                    *user_data)
{
    CL_WRAP_VA(reg_type_db, fnc, user_data);
}

static void cl_wrap_file_open(
            struct cl_code_listener *self,
            const char              *file_name)
{
    CL_WRAP_VA(file_open, file_name);
}

static void cl_wrap_file_close(
            struct cl_code_listener *self)
{
    CL_WRAP(file_close);
}

static void cl_wrap_fnc_open(
            struct cl_code_listener *self,
            const struct cl_location*loc,
            const char              *fnc_name,
            enum cl_scope_e         scope)
{
    CL_WRAP_VA(fnc_open, loc, fnc_name, scope);
}

static void cl_wrap_fnc_arg_decl(
            struct cl_code_listener *self,
            int                     arg_id,
            const char              *arg_name)
{
    CL_WRAP_VA(fnc_arg_decl, arg_id, arg_name);
}

static void cl_wrap_fnc_close(
            struct cl_code_listener *self)
{
    CL_WRAP(fnc_close);
}

static void cl_wrap_bb_open(
            struct cl_code_listener *self,
            const char              *bb_name)
{
    CL_WRAP_VA(bb_open, bb_name);
}

static void cl_wrap_insn(
            struct cl_code_listener *self,
            const struct cl_insn    *cli)
{
    CL_WRAP_VA(insn, cli);
}

static void cl_wrap_insn_call_open(
            struct cl_code_listener *self,
            const struct cl_location*loc,
            const struct cl_operand *dst,
            const struct cl_operand *fnc)
{
    CL_WRAP_VA(insn_call_open, loc, dst, fnc);
}

static void cl_wrap_insn_call_arg(
            struct cl_code_listener *self,
            int                     arg_id,
            const struct cl_operand *arg_src)
{
    CL_WRAP_VA(insn_call_arg, arg_id, arg_src);
}

static void cl_wrap_insn_call_close(
            struct cl_code_listener *self)
{
    CL_WRAP(insn_call_close);
}

static void cl_wrap_insn_switch_open(
            struct cl_code_listener *self,
            const struct cl_location*loc,
            const struct cl_operand *src)
{
    CL_WRAP_VA(insn_switch_open, loc, src);
}

static void cl_wrap_insn_switch_case(
            struct cl_code_listener *self,
            const struct cl_location*loc,
            const struct cl_operand *val_lo,
            const struct cl_operand *val_hi,
            const char              *label)
{
    CL_WRAP_VA(insn_switch_case, loc, val_lo, val_hi, label);
}

static void cl_wrap_insn_switch_close(
            struct cl_code_listener *self)
{
    CL_WRAP(insn_switch_close);
}

static void cl_wrap_destroy(struct cl_code_listener *self)
{
    delete static_cast<ICodeListener *>(self->data);
    delete self;
}

struct cl_code_listener* cl_create_listener_wrap(ICodeListener *listener)
{
    struct cl_code_listener *wrap = new cl_code_listener;
    wrap->data              = listener;
    wrap->reg_type_db       = cl_wrap_reg_type_db;
    wrap->file_open         = cl_wrap_file_open;
    wrap->file_close        = cl_wrap_file_close;
    wrap->fnc_open          = cl_wrap_fnc_open;
    wrap->fnc_arg_decl      = cl_wrap_fnc_arg_decl;
    wrap->fnc_close         = cl_wrap_fnc_close;
    wrap->bb_open           = cl_wrap_bb_open;
    wrap->insn              = cl_wrap_insn;
    wrap->insn_call_open    = cl_wrap_insn_call_open;
    wrap->insn_call_arg     = cl_wrap_insn_call_arg;
    wrap->insn_call_close   = cl_wrap_insn_call_close;
    wrap->insn_switch_open  = cl_wrap_insn_switch_open;
    wrap->insn_switch_case  = cl_wrap_insn_switch_case;
    wrap->insn_switch_close = cl_wrap_insn_switch_close;
    wrap->destroy           = cl_wrap_destroy;

    return wrap;
}

struct cl_code_listener* cl_code_listener_create(const char *config_string)
{
    try {
        ClFactory factory;
        ICodeListener *listener = factory.create(config_string);
        if (!listener) {
            CL_MSG_STREAM(cl_error, __FILE__ << ":" << __LINE__ << " error: "
                    << "failed to create cl_code_listener [internal location]");

            return NULL;
        }

        return cl_create_listener_wrap(listener);
    }
    catch (...) {
        CL_DIE("uncaught exception in cl_writer_create");
    }
}

std::ostream& operator<<(std::ostream &str, const LocationWriter &lw) {
    const Location &loc     = lw.loc;
    const Location &last    = lw.last;

    if ((0 < loc.locLine) && !loc.locFile.empty())
        str << loc.locFile;
    else if ((0 < last.locLine) && !last.locFile.empty())
        str << last.locFile;
    else if (!loc.currentFile.empty())
        str << loc.currentFile;
    else if (!last.currentFile.empty())
        str << last.currentFile;
    else
        str << "<unknown file>";

    str << ":";

    if (0 < loc.locLine)
        str << loc.locLine;
    else if (0 < last.locLine)
        str << last.locLine;
    else
        str << "<unknown line>";

    str << ":";

    if ((0 < loc.locLine) && (0 < loc.locColumn))
        str << loc.locColumn << ":";
    else if ((0 < last.locLine) && (0 < last.locColumn))
        str << last.locColumn << ":";

    str << " " << std::flush;
    return str;
}
