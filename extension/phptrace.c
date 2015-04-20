#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_phptrace.h"


/**
 * PHP-Trace Global
 * --------------------
 */
#define PT_FUNC_UNKNOWN         0x00
#define PT_FUNC_NORMAL          0x01
#define PT_FUNC_MEMBER          0x02
#define PT_FUNC_STATIC          0x03

#define PT_FUNC_INCLUDES        0x10
#define PT_FUNC_INCLUDE         0x10
#define PT_FUNC_INCLUDE_ONCE    0x11
#define PT_FUNC_REQUIRE         0x12
#define PT_FUNC_REQUIRE_ONCE    0x13
#define PT_FUNC_EVAL            0x14

typedef struct _phptrace_entry {
    unsigned char internal;
    int type;
    char *class;
    char *function;
    char *filename;
    uint lineno;
} phptrace_entry;

static void (*phptrace_ori_execute_ex)(zend_execute_data *execute_data TSRMLS_DC);
static void (*phptrace_ori_execute_internal)(zend_execute_data *execute_data, zend_fcall_info *fci, int return_value_used TSRMLS_DC);
ZEND_API void phptrace_execute_ex(zend_execute_data *execute_data TSRMLS_DC);
ZEND_API void phptrace_execute_internal(zend_execute_data *execute_data, zend_fcall_info *fci, int return_value_used TSRMLS_DC);


/**
 * PHP Extension Init
 * --------------------
 */

ZEND_DECLARE_MODULE_GLOBALS(phptrace)

/* True global resources - no need for thread safety here */
static int le_phptrace;

/**
 * Every user visible function must have an entry in phptrace_functions[].
 */
const zend_function_entry phptrace_functions[] = {
    PHP_FE_END  /* Must be the last line in phptrace_functions[] */
};

zend_module_entry phptrace_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
    STANDARD_MODULE_HEADER,
#endif
    "phptrace",
    phptrace_functions,
    PHP_MINIT(phptrace),
    PHP_MSHUTDOWN(phptrace),
    PHP_RINIT(phptrace),        /* Replace with NULL if there's nothing to do at request start */
    PHP_RSHUTDOWN(phptrace),    /* Replace with NULL if there's nothing to do at request end */
    PHP_MINFO(phptrace),
#if ZEND_MODULE_API_NO >= 20010901
    PHP_PHPTRACE_VERSION,
#endif
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_PHPTRACE
ZEND_GET_MODULE(phptrace)
#endif

/* PHP_INI */
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("phptrace.enable",    "0", PHP_INI_ALL, OnUpdateLong, enable, zend_phptrace_globals, phptrace_globals)
PHP_INI_END()

/* php_phptrace_init_globals */
static void php_phptrace_init_globals(zend_phptrace_globals *phptrace_globals)
{
    phptrace_globals->enable = 0;
}


/**
 * PHP Extension Function
 * --------------------
 */

PHP_MINIT_FUNCTION(phptrace)
{
    REGISTER_INI_ENTRIES();

    /* Save original executor */
    phptrace_ori_execute_ex = zend_execute_ex;
    phptrace_ori_execute_internal = zend_execute_internal;

    if (PHPTRACE_G(enable)) {
        /* Replace executor */
        zend_execute_ex = phptrace_execute_ex;
        zend_execute_internal = phptrace_execute_internal;
    }

    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(phptrace)
{
    UNREGISTER_INI_ENTRIES();

    if (PHPTRACE_G(enable)) {
        /* TODO ini_set during runtime ? */
        /* Restore original executor */
        zend_execute_ex = phptrace_ori_execute_ex;
        zend_execute_internal = phptrace_ori_execute_internal;
    }

    return SUCCESS;
}

PHP_RINIT_FUNCTION(phptrace)
{
    return SUCCESS;
}

PHP_RSHUTDOWN_FUNCTION(phptrace)
{
    return SUCCESS;
}

PHP_MINFO_FUNCTION(phptrace)
{
    php_info_print_table_start();
    php_info_print_table_header(2, "phptrace support", "enabled");
    php_info_print_table_end();

    /* Remove comments if you have entries in php.ini */
    DISPLAY_INI_ENTRIES();
}


/**
 * PHP-Trace Function
 * --------------------
 */
int phptrace_build_entry(phptrace_entry *entry, zend_execute_data *execute_data, unsigned char internal TSRMLS_DC)
{
    zend_execute_data *ex = execute_data;
    zend_function *zf;

    /* init */
    if (entry == NULL) {
        entry = malloc(sizeof(phptrace_entry));
    }
    memset(entry, 0, sizeof(phptrace_entry));

    /* internal */
    entry->internal = internal;

    /**
     * Current execute_data is the data going to be executed not the entry
     * point, so we switch to previous data. The internal function is a
     * exception because it's no need to execute by op_array.
     */
    if (!entry->internal && ex->prev_execute_data) {
        ex = ex->prev_execute_data;
    }
    zf = ex->function_state.function;

    /* names */
    if (zf->common.function_name) {
        /* class name */
        if (zf->common.scope) {
            if (ex->object) {
                entry->type = PT_FUNC_MEMBER;
            } else {
                entry->type = PT_FUNC_STATIC;
            }
            entry->class = strdup(zf->common.scope->name);
        } else {
            entry->type = PT_FUNC_NORMAL;
        }

        /* function name */
        if (strcmp(zf->common.function_name, "{closure}") == 0) {
            asprintf(&entry->function, "{closure:%s:%d-%d}", zf->op_array.filename, zf->op_array.line_start, zf->op_array.line_end);
        } else if (strcmp(zf->common.function_name, "__lambda_func") == 0) {
            asprintf(&entry->function, "{lambda:%s}", zf->op_array.filename);
        } else {
            entry->function = strdup(zf->common.function_name);
        }
    } else {
        int add_filename = 0;

        /* special user function */
        switch (ex->opline->extended_value) {
            case ZEND_INCLUDE_ONCE:
                entry->type = PT_FUNC_INCLUDE_ONCE;
                entry->function = "include_once";
                add_filename = 1;
                break;
            case ZEND_REQUIRE_ONCE:
                entry->type = PT_FUNC_REQUIRE_ONCE;
                entry->function = "require_once";
                add_filename = 1;
                break;
            case ZEND_INCLUDE:
                entry->type = PT_FUNC_INCLUDE;
                entry->function = "include";
                add_filename = 1;
                break;
            case ZEND_REQUIRE:
                entry->type = PT_FUNC_REQUIRE;
                entry->function = "require";
                add_filename = 1;
                break;
            case ZEND_EVAL:
                entry->type = PT_FUNC_EVAL;
                entry->function = strdup("{eval}");
                break;
            default:
                /* should be function main */
                entry->type = PT_FUNC_NORMAL;
                entry->function = strdup("{main}");
                break;
        }
        if (add_filename) {
            asprintf(
                &entry->function,
                "{%s:%s}",
                entry->function,
                execute_data->function_state.function->op_array.filename /* using current data */
            );
        }
    }

    /* lineno */
    if (ex->opline) {
        entry->lineno = ex->opline->lineno;
    } else if (execute_data->opline) {
        entry->lineno = execute_data->opline->lineno; /* try using current */
    } else {
        entry->lineno = 0;
    }

    /* filename */
    if (ex->op_array) {
        entry->filename = ex->op_array->filename;
    } else if (execute_data->op_array) {
        entry->filename = execute_data->op_array->filename; /* try using current */
    } else {
        entry->filename = NULL;
    }

    /* debug output */
    {
        fprintf(stderr, "[%s:%d] ", entry->filename, entry->lineno);
        if (entry->type == PT_FUNC_NORMAL) {
            fprintf(stderr, "%s()", entry->function);
        } else if (entry->type == PT_FUNC_MEMBER) {
            fprintf(stderr, "%s->%s()", entry->class, entry->function);
        } else if (entry->type == PT_FUNC_STATIC) {
            fprintf(stderr, "%s::%s()", entry->class, entry->function);
        } else if (entry->type & PT_FUNC_INCLUDES) {
            fprintf(stderr, "%s", entry->function);
        } else {
            fprintf(stderr, "unknown ev: %x", ex->opline->extended_value);
        }
        fprintf(stderr, "\n");
    }

    return 0;
}


/**
 * PHP-Trace Executor Replacement
 * --------------------
 */
ZEND_API void phptrace_execute_ex(zend_execute_data *execute_data TSRMLS_DC)
{
    phptrace_entry *entry = NULL;
    phptrace_build_entry(entry, execute_data, 0 TSRMLS_CC);

    /* call original */
    phptrace_ori_execute_ex(execute_data TSRMLS_CC);
}

ZEND_API void phptrace_execute_internal(zend_execute_data *execute_data, zend_fcall_info *fci, int return_value_used TSRMLS_DC)
{
    phptrace_entry *entry = NULL;
    phptrace_build_entry(entry, execute_data, 1 TSRMLS_CC);

    /* call original */
    if (phptrace_ori_execute_internal) {
        phptrace_ori_execute_internal(execute_data, fci, return_value_used TSRMLS_CC);
    } else {
        execute_internal(execute_data, fci, return_value_used TSRMLS_CC);
    }
}
