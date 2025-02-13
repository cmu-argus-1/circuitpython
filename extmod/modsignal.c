// SPDX-FileCopyrightText: 2023 Gregory Neverov
// SPDX-License-Identifier: MIT

#include <signal.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include "morelib/thread.h"

#include "FreeRTOS.h"
#include "task.h"

#include "./modsignal.h"
#include "shared/runtime/interrupt_char.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/runtime.h"


static SemaphoreHandle_t signal_mutex;

static mp_obj_t signal_handlers[_NSIG];

static mp_obj_t signal_default_int_handler(mp_obj_t signum_in) {
    mp_sched_keyboard_interrupt();
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(signal_default_int_handler_obj, signal_default_int_handler);

static void signal_handler(int signum) {
    xSemaphoreTake(signal_mutex, portMAX_DELAY);
    mp_obj_t handler_obj = signal_handlers[signum];

    if (MP_OBJ_TO_PTR(handler_obj) == &signal_default_int_handler_obj) {
        if (signum == SIGINT) {
            if (mp_interrupt_char != -1) {
                mp_sched_keyboard_interrupt();
            }
        } else if (signum == SIGQUIT) {
            exit(0);
        }
        mp_task_interrupt();
    }
    #if MICROPY_ENABLE_SCHEDULER
    else {
        mp_sched_schedule(handler_obj, MP_OBJ_NEW_SMALL_INT(signum));
    }
    #endif
    xSemaphoreGive(signal_mutex);
    signal(signum, signal_handler);
}

static mp_obj_t signal_alarm(mp_obj_t time_in) {
    unsigned time = mp_obj_get_int(time_in);
    MP_THREAD_GIL_EXIT();
    unsigned result = alarm(time);
    MP_THREAD_GIL_ENTER();
    return mp_obj_new_int(result);
}
MP_DEFINE_CONST_FUN_OBJ_1(signal_alarm_obj, signal_alarm);

static mp_obj_t signal_signal(mp_obj_t signum_in, mp_obj_t handler_in) {
    mp_int_t signum = mp_obj_get_int(signum_in);
    if ((signum < 0) || (signum >= _NSIG)) {
        mp_raise_ValueError(NULL);
    }
    _sig_func_ptr handler = signal_handler;
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

    xSemaphoreTake(signal_mutex, portMAX_DELAY);
    mp_obj_t old_handler_obj = signal_handlers[signum];
    _sig_func_ptr old_handler = signal(signum, handler);
    if (old_handler == SIG_ERR) {
        mp_raise_OSError(errno);
    }
    signal_handlers[signum] = handler_in;
    xSemaphoreGive(signal_mutex);

    if (old_handler == SIG_DFL) {
        return MP_OBJ_NEW_SMALL_INT(SIG_DFL);
    } else if (old_handler == SIG_IGN) {
        return MP_OBJ_NEW_SMALL_INT(SIG_IGN);
    } else if (old_handler == signal_handler) {
        return old_handler_obj;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(signal_signal_obj, signal_signal);

static mp_obj_t signal_getsignal(mp_obj_t signum_in) {
    mp_int_t signum = mp_obj_get_int(signum_in);
    if ((signum < 0) || (signum >= _NSIG)) {
        mp_raise_ValueError(NULL);
    }
    xSemaphoreTake(signal_mutex, portMAX_DELAY);
    mp_obj_t handler_obj = signal_handlers[signum];
    xSemaphoreGive(signal_mutex);
    return handler_obj;
}
static MP_DEFINE_CONST_FUN_OBJ_1(signal_getsignal_obj, signal_getsignal);

static const mp_rom_map_elem_t signal_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),        MP_ROM_QSTR(MP_QSTR_signal) },
    { MP_ROM_QSTR(MP_QSTR_alarm),           MP_ROM_PTR(&signal_alarm_obj) },
    { MP_ROM_QSTR(MP_QSTR_getsignal),       MP_ROM_PTR(&signal_getsignal_obj) },
    { MP_ROM_QSTR(MP_QSTR_signal),          MP_ROM_PTR(&signal_signal_obj) },
    { MP_ROM_QSTR(MP_QSTR_default_int_handler), MP_ROM_PTR(&signal_default_int_handler_obj) },
    { MP_ROM_QSTR(MP_QSTR_SIG_DFL),         MP_ROM_INT(SIG_DFL) },
    { MP_ROM_QSTR(MP_QSTR_SIG_IGN),         MP_ROM_INT(SIG_IGN) },
    { MP_ROM_QSTR(MP_QSTR_SIGALRM),         MP_ROM_INT(SIGALRM) },
    { MP_ROM_QSTR(MP_QSTR_SIGINT),          MP_ROM_INT(SIGINT) },
    { MP_ROM_QSTR(MP_QSTR_SIGQUIT),         MP_ROM_INT(SIGQUIT) },
};
static MP_DEFINE_CONST_DICT(signal_module_globals, signal_module_globals_table);

const mp_obj_module_t mp_module_signal = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&signal_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_signal, mp_module_signal);

void signal_init(void) {
    signal_mutex = xSemaphoreCreateMutex();
    signal_signal(MP_OBJ_NEW_SMALL_INT(SIGINT), MP_OBJ_FROM_PTR(&signal_default_int_handler_obj));
    signal_signal(MP_OBJ_NEW_SMALL_INT(SIGQUIT), MP_OBJ_FROM_PTR(&signal_default_int_handler_obj));
    struct termios termios;
    tcgetattr(0, &termios);
    termios.c_lflag |= ISIG;
    tcsetattr(0, TCSANOW, &termios);
}

void signal_deinit(void) {
    struct termios termios;
    tcgetattr(0, &termios);
    termios.c_lflag &= ~ISIG;
    tcsetattr(0, TCSANOW, &termios);
    signal_signal(MP_OBJ_NEW_SMALL_INT(SIGINT), MP_OBJ_NEW_SMALL_INT(SIG_DFL));
    signal_signal(MP_OBJ_NEW_SMALL_INT(SIGQUIT), MP_OBJ_NEW_SMALL_INT(SIG_DFL));
    vSemaphoreDelete(signal_mutex);
    signal_mutex = NULL;
}

void signal_collect(void) {
    xSemaphoreTake(signal_mutex, portMAX_DELAY);
    gc_collect_root(signal_handlers, _NSIG);
    xSemaphoreGive(signal_mutex);
}
