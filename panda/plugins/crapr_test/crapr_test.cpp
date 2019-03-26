/* CRAPR PLUGIN EXAMPLE
 * 
 * Authors:
 *  Hwiwon Lee              develacker@korea.ac.kr
 */

// This needs to be defined before anything is included in order to get
// the PRIx64 macro
#define __STDC_FORMAT_MACROS

#include "panda/plugin.h"
#include "panda/plugin_plugin.h"
#include "panda/common.h"
#include "panda/rr/rr_log.h"

#include <stdint.h>
#include <string.h>
#include <map>
#include <cstdio>
#include <cstdlib>

#include <capstone/capstone.h>
#if defined(TARGET_I386)
#include <capstone/x86.h>
#elif defined(TARGET_ARM)
#include <capstone/arm.h>
#elif defined(TARGET_PPC)
#include <capstone/ppc.h>
#endif

/*
 * These need to be extern "C" so that the ABI is compatible with
 * QEMU/PANDA, which is written in C
 */
extern "C" {

bool init_plugin(void *);
void uninit_plugin(void *);

}

/*
 * Analysis functions can be defined here
 */
static int _after_block_translate(CPUArchState *env, TranslationBlock *tb) 
{
    return 0;
}

static int _before_block_exec(CPUState *env, TranslationBlock *tb) 
{
    return 0;
}

static int _after_block_exec(CPUState* cpu, TranslationBlock *tb, uint8_t exitCode) 
{
    return 0;
}

/*
 * Initialize and define the functions
 */
bool init_plugin(void *self) 
{
    panda_cb pcb;
    //
    // Argument handling
    // More functions can be found in /panda/panda/src/callbacks.c and 
    // https://github.com/panda-re/panda/blob/master/panda/docs/manual.md#argument-handling
    //
    panda_arg_list *args = panda_get_args("crapr_test");
    const char *func_args_str = panda_parse_string_opt(args, "fargs", nullptr, "Hexidecimal, dash delimited arguments for the function to call.");
    printf("[+] func_args : %s\n", func_args_str);

    //
    // Define callback functions
    // More callbacks can be found in panda/plugin.h file and 
    // https://github.com/panda-re/panda/blob/master/panda/docs/manual.md#callback-and-plugin-management
    //
    pcb.after_block_translate = _after_block_translate;
    panda_register_callback(self, PANDA_CB_AFTER_BLOCK_TRANSLATE, pcb);
    pcb.before_block_exec = _before_block_exec;
    panda_register_callback(self, PANDA_CB_BEFORE_BLOCK_EXEC, pcb);
    pcb.after_block_exec = _after_block_exec;
    panda_register_callback(self, PANDA_CB_AFTER_BLOCK_EXEC, pcb);

    return true;
}

/*
 * Clean up
 */
void uninit_plugin(void *self) 
{

}