/* Copyright (c) 2014-2016, Marel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef SDO_SRV_H_
#define SDO_SRV_H_

#include "canopen/sdo.h"
#include "sock.h"
#include "vector.h"
#include "canopen/sdo_req_enums.h"

struct sdo_srv;
struct can_frame;

enum sdo_srv_comm_state {
	SDO_SRV_COMM_INIT_REQ = 0,
	SDO_SRV_COMM_DL_SEG_REQ,
	SDO_SRV_COMM_UL_SEG_REQ
};

typedef int (*sdo_srv_fn)(struct sdo_srv* srv);

struct sdo_srv {
	struct sock sock;
	unsigned int nodeid;
	enum sdo_req_type req_type;
	enum sdo_srv_comm_state comm_state;
	struct vector buffer;
	size_t pos;
	sdo_srv_fn on_init;
	sdo_srv_fn on_done;
	int index, subindex;
	int is_toggled;
	enum sdo_req_status status;
	enum sdo_abort_code abort_code;
};

int sdo_srv_init(struct sdo_srv* self, const struct sock* sock, int nodeid,
		 sdo_srv_fn on_init, sdo_srv_fn on_done);

int sdo_srv_feed(struct sdo_srv* self, const struct can_frame* cf);
void sdo_srv_destroy(struct sdo_srv* self);

int sdo_srv_abort(struct sdo_srv* self, enum sdo_abort_code code);

#endif /* SDO_SRV_H_ */
