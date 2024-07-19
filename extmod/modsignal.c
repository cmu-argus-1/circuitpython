// SPDX-FileCopyrightText: 2023 Gregory Neverov
// SPDX-License-Identifier: MIT

#include <signal.h>

#include "FreeRTOS.h"
#include "task.h"

#include "newlib/thread.h"

#include "./modsignal.h"
#include "shared/runtime/interrupt_char.h"
#include "py/mperrno.h"
#include "py/runtime.h"


static mp_obj_t signal_ctx = mp_const_none;

mp_obj_t signal_default_int_handler(mp_obj_t signum_in) {
    mp_sched_keyboard_interrupt();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(signal_default_int_handler_obj, signal_default_int_handler);

static void signal_handler(int signum) {
    if (MP_OBJ_TO_PTR(signal_ctx) == &signal_default_int_handler_obj) {
        if (mp_interrupt_char != -1) {
            mp_sched_keyboard_interrupt();
            mp_task_interrupt();
        }
    }
    #if MICROPY_ENABLE_SCHEDULER
    else {
        mp_sched_schedule(signal_ctx, MP_OBJ_NEW_SMALL_INT(signum));
    }
    #endif
    signal(signum, signal_handler);
}

static mp_obj_t signal_signal(mp_obj_t signum_in, mp_obj_t handler_in) {
    mp_int_t signum = mp_obj_get_int(signum_in);
    if (signum != SIGINT) {
        mp_raise_ValueError(NULL);
    }
    _sig_func_ptr handler = signal_handler;
    mp_obj_t ctx = handler_in;
    if (mp_obj_is_int(handler_in)) {
        handler = (_sig_func_ptr)mp_obj_get_int(handler_in);
        if ((handler != SIG_DFL) && (handler != SIG_IGN)) {
            mp_raise_ValueError(NULL);
        }
    } else if (!mp_obj_is_callable(handler_in)) {
        mp_raise_TypeError(NULL);
    }
    #if !MICROPY_ENABLE_SCHEDULER
    else if (MP_OBJ_TO_PTR(handler_in) != &signal_default_int_handler_obj) {
        mp_raise_msg(&mp_type_RuntimeError, NULL);
    }
    #endif

    mp_obj_t old_ctx = signal_ctx;
    _sig_func_ptr old_handler = signal(signum, handler);
    if (old_handler == SIG_ERR) {
        mp_raise_OSError(errno);
    }

    signal_ctx = ctx;
    if (old_handler == SIG_DFL) {
        return MP_OBJ_NEW_SMALL_INT(SIG_DFL);
    } else if (old_handler == SIG_IGN) {
        return MP_OBJ_NEW_SMALL_INT(SIG_IGN);
    } else if (old_handler == signal_handler) {
        return old_ctx;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(signal_signal_obj, signal_signal);

static mp_obj_t signal_getsignal(mp_obj_t signum_in) {
    mp_int_t signum = mp_obj_get_int(signum_in);
    if (signum != SIGINT) {
        mp_raise_ValueError(NULL);
    }
    return signal_ctx;
}
static MP_DEFINE_CONST_FUN_OBJ_1(signal_getsignal_obj, signal_getsignal);

static const mp_rom_map_elem_t signal_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),        MP_ROM_QSTR(MP_QSTR_signal) },
    { MP_ROM_QSTR(MP_QSTR_getsignal),       MP_ROM_PTR(&signal_getsignal_obj) },
    { MP_ROM_QSTR(MP_QSTR_signal),          MP_ROM_PTR(&signal_signal_obj) },
    { MP_ROM_QSTR(MP_QSTR_default_int_handler), MP_ROM_PTR(&signal_default_int_handler_obj) },
    { MP_ROM_QSTR(MP_QSTR_SIG_DFL),         MP_ROM_INT(SIG_DFL) },
    { MP_ROM_QSTR(MP_QSTR_SIG_IGN),         MP_ROM_INT(SIG_IGN) },
    { MP_ROM_QSTR(MP_QSTR_SIGINT),          MP_ROM_INT(SIGINT) },
};
static MP_DEFINE_CONST_DICT(signal_module_globals, signal_module_globals_table);

const mp_obj_module_t mp_module_signal = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&signal_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_signal, mp_module_signal);

void signal_init(void) {
    signal_signal(MP_OBJ_NEW_SMALL_INT(SIGINT), MP_OBJ_FROM_PTR(&signal_default_int_handler_obj));
}

void signal_deinit(void) {
    signal_signal(MP_OBJ_NEW_SMALL_INT(SIGINT), MP_OBJ_NEW_SMALL_INT(SIG_DFL));
}
