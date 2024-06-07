// SPDX-FileCopyrightText: 2024 Gregory Neverov
// SPDX-License-Identifier: MIT

#include <malloc.h>
#include <memory.h>

#include "./modlvgl.h"
#include "./queue.h"


lvgl_queue_t *lvgl_queue_default;

lvgl_queue_t *lvgl_queue_alloc(size_t size) {
    size_t mem_size = sizeof(lvgl_queue_t) + size * sizeof(lvgl_queue_elem_t *);
    lvgl_queue_t *queue = lv_malloc_zeroed(mem_size);
    lvgl_ptr_init_handle(&queue->base, &lvgl_queue_type, NULL);
    mp_stream_poll_init(&queue->poll);
    queue->size = size;
    return queue;
}

static void lvgl_queue_clear(lvgl_queue_t *queue) {
    while (queue->read_index < queue->write_index) {
        lvgl_queue_elem_t *elem = queue->ring[queue->read_index++ % queue->size];
        elem->del(elem);
    }
}

void lvgl_queue_deinit(lvgl_ptr_t ptr) {
    lvgl_queue_t *queue = ptr;
    lvgl_queue_clear(queue);
}

mp_obj_t lvgl_queue_new(lvgl_ptr_t ptr) {
    lvgl_queue_t *queue = ptr;
    lvgl_obj_queue_t *self = mp_obj_malloc_with_finaliser(lvgl_obj_queue_t, &lvgl_type_queue);
    lvgl_ptr_init_obj(&self->base, &queue->base);
    self->timeout = portMAX_DELAY;
    return MP_OBJ_FROM_PTR(self);
}

void lvgl_queue_send(lvgl_queue_t *queue, lvgl_queue_elem_t *elem) {
    assert(lvgl_is_locked());

    if (queue->reader_closed) {
        elem->del(elem);
        return;
    }

    if ((queue->write_index - queue->read_index) >= queue->size) {
        queue->writer_overflow = 1;
        elem->del(elem);
        return;
    }

    queue->ring[queue->write_index++ % queue->size] = elem;
    mp_stream_poll_signal(&queue->poll, MP_STREAM_POLL_RD, NULL);
}

void lvgl_queue_close(lvgl_queue_t *queue) {
    assert(lvgl_is_locked());
    queue->writer_closed = 1;
    mp_stream_poll_signal(&queue->poll, MP_STREAM_POLL_RD, NULL);
}

lvgl_queue_elem_t *lvgl_queue_receive(lvgl_queue_t *queue) {
    assert(lvgl_is_locked());
    if (queue->read_index == queue->write_index) {
        return NULL;
    }

    lvgl_queue_elem_t *elem = queue->ring[queue->read_index++ % queue->size];
    queue->writer_overflow = 0;
    return elem;
}

// static mp_obj_t lvgl_obj_queue_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
//     mp_arg_check_num(n_args, n_kw, 0, 0, false);

//     lvgl_obj_queue_t *self = m_new_obj_with_finaliser(lvgl_obj_queue_t);
//     self->base.type = type;
//     self->queue = lvgl_queue_alloc(self);
//     return MP_OBJ_FROM_PTR(self);
// }

static mp_uint_t lvgl_obj_queue_close(mp_obj_t self_in, int *errcode) {
    lvgl_queue_t *queue = lvgl_ptr_from_mp(NULL, self_in);
    lvgl_lock();
    lvgl_queue_clear(queue);
    queue->reader_closed = 1;
    lvgl_unlock();
    return 0;
}

static mp_uint_t lvgl_obj_queue_run_nonblock(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    lvgl_queue_t *queue = lvgl_ptr_from_mp(NULL, self_in);
    lvgl_lock();
    bool reader_closed = queue->reader_closed;
    lvgl_queue_elem_t *elem = lvgl_queue_receive(queue);
    bool writer_closed = queue->writer_closed;
    lvgl_unlock();

    if (reader_closed) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    if (!elem) {
        if (writer_closed) {
            return 0;
        }
        else {
            *errcode = MP_EAGAIN;
            return MP_STREAM_ERROR;
        }
    }

    nlr_buf_t nlr;
    volatile unsigned int r = nlr_push(&nlr);
    if (r == 0) {
        elem->run(elem);
        nlr_pop();
    }

    lvgl_lock();
    elem->del(elem);
    lvgl_unlock();

    if (r != 0) {
        nlr_jump(nlr.ret_val);
    }
    return 1;
}

static mp_obj_t lvgl_obj_queue_run(mp_obj_t self_in) {
    lvgl_obj_queue_t *self = MP_OBJ_TO_PTR(self_in);
    int errcode;
    mp_uint_t ret = mp_poll_block(self_in, NULL, 1, &errcode, lvgl_obj_queue_run_nonblock, MP_STREAM_POLL_RD, self->timeout, false);
    return mp_stream_return(ret, errcode);
}
static MP_DEFINE_CONST_FUN_OBJ_1(lvgl_obj_queue_run_obj, lvgl_obj_queue_run);

static mp_uint_t lvgl_obj_queue_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    lvgl_obj_queue_t *self = MP_OBJ_TO_PTR(self_in);
    lvgl_queue_t *queue = (void *)self->base.handle;
    if (queue->reader_closed && (request != MP_STREAM_CLOSE)) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }

    mp_uint_t ret;
    switch (request) {
        case MP_STREAM_TIMEOUT:
            ret = mp_stream_timeout(&self->timeout, arg, errcode);
            break;
        case MP_STREAM_POLL_CTL:
            lvgl_lock();
            ret = mp_stream_poll_ctl(&queue->poll, (void *)arg, errcode);
            lvgl_unlock();
            break;
        case MP_STREAM_CLOSE:
            ret = lvgl_obj_queue_close(self_in, errcode);
            break;
        default:
            *errcode = MP_EINVAL;
            ret = MP_STREAM_ERROR;
            break;
    }
    return ret;
}

static const mp_stream_p_t lvgl_obj_queue_p = {
    .ioctl = lvgl_obj_queue_ioctl,
};

static const mp_rom_map_elem_t lvgl_obj_queue_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___del__),         MP_ROM_PTR(&lvgl_ptr_del_obj) },
    { MP_ROM_QSTR(MP_QSTR_run),             MP_ROM_PTR(&lvgl_obj_queue_run_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),           MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout),      MP_ROM_PTR(&mp_stream_settimeout_obj) },
};
static MP_DEFINE_CONST_DICT(lvgl_obj_queue_locals_dict, lvgl_obj_queue_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    lvgl_type_queue,
    MP_ROM_QSTR_CONST(MP_QSTR_Queue),
    MP_TYPE_FLAG_ITER_IS_STREAM,
    // make_new, lvgl_obj_queue_make_new,
    protocol, &lvgl_obj_queue_p,
    locals_dict, &lvgl_obj_queue_locals_dict 
    );
MP_REGISTER_OBJECT(lvgl_type_queue);

const lvgl_ptr_type_t lvgl_queue_type = {
    &lvgl_type_queue,
    lvgl_queue_new,
    lvgl_queue_deinit,
    NULL,
    NULL,
};
