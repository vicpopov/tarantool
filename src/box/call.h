#ifndef INCLUDES_TARANTOOL_MOD_BOX_CALL_H
#define INCLUDES_TARANTOOL_MOD_BOX_CALL_H
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdint.h>

struct box_function_ctx {
	struct port *port;
};

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Get output buffer to write response on a specified request.
 * The buffer is valid until next yield.
 * @param request Request to get buffer from.
 *
 * @retval not NULL Output buffer, valid until yield.
 * @retval     NULL Memory error.
 */
struct obuf *
call_request_obuf(struct call_request *request);

/**
 * Notify iproto thread, that input request data is not needed
 * anymore. Iproto thread, received such message, can reuse
 * request's ibuf for newer requests.
 * @param request Request to discard data.
 */
void
call_request_discard_input(struct call_request *request);

#ifdef __cplusplus
} /* extern "C" */
#endif

void
box_process_call(struct call_request *request);

void
box_process_eval(struct call_request *request);

#endif /* INCLUDES_TARANTOOL_MOD_BOX_CALL_H */
