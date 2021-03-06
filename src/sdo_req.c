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

/* SDO Request Abstraction
 *
 * An SDO request represents an on-going or pending SDO transaction.
 *
 * When an SDO is requested, a request object is returned that can be used to
 * monitor the status and/or cancel the request. A request can be made to any
 * node with id between 1 and 127. Multiple requests can be made to the same
 * node at the same time. They will be queued up in FIFO order.
 *
 * There are 127 queues available; one for each possible node.
 *
 * A request can be handled in either a synchronous or asynchronous manner, by
 * either waiting for it to finish using sdo_req_wait() or registering an
 * "on_done" callback.
 */
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include "vector.h"
#include "sys/queue.h"
#include "canopen/sdo.h"
#include "canopen/sdo_async.h"
#include "canopen/sdo_req.h"
#include "sock.h"

#define SDO_REQ_TIMEOUT 1000 /* ms */
#define SDO_REQ_ASYNC_PRIO 1000

#define SDO_BUFFER_INITIAL_SIZE 8

/* Index 0 is unused */
static struct sdo_req_queue sdo_req__queues[128];

struct sdo_req* sdo_req_new(struct sdo_req_info* info)
{
	struct sdo_req* self = malloc(sizeof(*self));
	if (!self)
		return NULL;

	memset(self, 0, sizeof(*self));

	self->ref = 1;
	self->type = info->type;
	self->index = info->index;
	self->subindex = info->subindex;
	self->on_done = info->on_done;
	self->context = info->context;

	if (info->type == SDO_REQ_DOWNLOAD) {
		if (vector_assign(&self->data, info->dl_data,
				  info->dl_size) < 0)
			goto failure;
	} else {
		if (vector_init(&self->data, SDO_BUFFER_INITIAL_SIZE) < 0)
			goto failure;
	}

	return self;

failure:
	free(self);
	return NULL;
}

void sdo_req_free(struct sdo_req* self)
{
	if (self->context && self->context_free_fn)
		self->context_free_fn(self->context);

	vector_destroy(&self->data);
	free(self);
}

ARC_GENERATE(sdo_req, sdo_req_free)

void sdo_req__process_queue(struct mloop_idle* idle);
int sdo_req__have_req(struct mloop_idle* idle);

int sdo_req__queue_init(struct sdo_req_queue* self, const struct sock* sock,
			int nodeid, size_t limit,
			enum sdo_async_quirks_flags quirks)
{
	memset(self, 0, sizeof(*self));

	if (sdo_async_init(&self->sdo_client, sock, nodeid) < 0)
		return -1;

	self->idle = mloop_idle_new(mloop_default());
	if (!self->idle)
		goto failure;

	mloop_idle_set_idle_fn(self->idle, sdo_req__process_queue);
	mloop_idle_set_cond_fn(self->idle, sdo_req__have_req);
	mloop_idle_set_context(self->idle, self, NULL);
	mloop_idle_start(self->idle);

	self->sdo_client.quirks = quirks;

	self->limit = limit;
	self->nodeid = nodeid;

	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&self->mutex, &attr);
	pthread_mutexattr_destroy(&attr);

	TAILQ_INIT(&self->list);

	return 0;

failure:
	sdo_async_destroy(&self->sdo_client);
	return -1;
}

void sdo_req__queue_clear(struct sdo_req_queue* self)
{
	while (!TAILQ_EMPTY(&self->list)) {
		struct sdo_req* req = TAILQ_FIRST(&self->list);
		TAILQ_REMOVE(&self->list, req, links);
		req->status = SDO_REQ_CANCELLED;
		sdo_req_unref(req);
	}
	self->size = 0;
}

void sdo_req__queue_destroy(struct sdo_req_queue* self)
{
	mloop_idle_unref(self->idle);
	sdo_async_destroy(&self->sdo_client);
	sdo_req__queue_clear(self);
	pthread_mutex_destroy(&self->mutex);
}

int sdo_req_queues_init(const struct sock* sock, size_t limit,
			enum sdo_async_quirks_flags quirks)
{
	size_t i;

	for (i = 1; i < 128; ++i)
		if (sdo_req__queue_init(&sdo_req__queues[i], sock, i, limit,
					quirks) < 0)
			goto failure;

	return 0;

failure:
	for (--i; i > 0; --i)
		sdo_req__queue_destroy(&sdo_req__queues[i]);
	return -1;
}

void sdo_req_queues_cleanup()
{
	size_t i;
	for (i = 1; i < 128; ++i)
		sdo_req__queue_destroy(&sdo_req__queues[i]);
}

struct sdo_req_queue* sdo_req_queue_get(int nodeid)
{
	assert(1 <= nodeid && nodeid <= 127);
	return &sdo_req__queues[nodeid];
}

void sdo_req_queue__lock(struct sdo_req_queue* self)
{
	pthread_mutex_lock(&self->mutex);
}

void sdo_req_queue__unlock(struct sdo_req_queue* self)
{
	pthread_mutex_unlock(&self->mutex);
}

void sdo_req_queue_flush(struct sdo_req_queue* self)
{
	sdo_req_queue__lock(self);
	sdo_req__queue_clear(self);
	sdo_async_stop(&self->sdo_client);
	sdo_req_queue__unlock(self);
}

int sdo_req_queue__enqueue(struct sdo_req_queue* self, struct sdo_req* req)
{
	assert(req->parent == NULL);

	int rc = -1;
	sdo_req_queue__lock(self);
	if (self->size >= self->limit)
		goto done;
	else
		++self->size;

	req->parent = self;
	TAILQ_INSERT_TAIL(&self->list, req, links);
	mloop_iterate(mloop_default());

	rc = 0;
done:
	sdo_req_queue__unlock(self);
	return rc;
}

struct sdo_req* sdo_req_queue__dequeue(struct sdo_req_queue* self)
{
	sdo_req_queue__lock(self);

	struct sdo_req* req = TAILQ_FIRST(&self->list);
	if (!req)
		return NULL;

	assert(self->size);
	--self->size;

	TAILQ_REMOVE(&self->list, req, links);

	sdo_req_queue__unlock(self);
	return req;
}

int sdo_req_queue_remove(struct sdo_req_queue* self, struct sdo_req* req)
{
	if (!req->parent)
		return -1;

	sdo_req_queue__lock(self);

	TAILQ_REMOVE(&self->list, req, links);
	req->parent = NULL;

	sdo_req_queue__unlock(self);

	return 0;
}

void sdo_req_wait(struct sdo_req* self)
{
	while (self->status == SDO_REQ_PENDING)
		usleep(100); /* TODO: Use something more sophisticated */
}

void sdo_req__on_done(struct sdo_async* async);

void sdo_req__on_stop(void* ptr)
{
	struct sdo_req* req = ptr;

	if (req->status == SDO_REQ_PENDING)
		req->status = SDO_REQ_CANCELLED;

	sdo_req_unref(req);
}

int sdo_req__have_req(struct mloop_idle* idle)
{
	struct sdo_req_queue* queue = mloop_idle_get_context(idle);
	if (queue->sdo_client.is_running)
	       return 0;

	__sync_synchronize();
	return !TAILQ_EMPTY(&queue->list);
}

void sdo_req__process_queue(struct mloop_idle* idle)
{
	struct sdo_req_queue* queue = mloop_idle_get_context(idle);
	assert(!queue->sdo_client.is_running);

	sdo_req_queue__lock(queue);

	struct sdo_req* req = sdo_req_queue__dequeue(queue);
	if (!req)
		goto done;

	struct sdo_async_info info = {
		.type = req->type,
		.index = req->index,
		.subindex = req->subindex,
		.timeout = SDO_REQ_TIMEOUT,
		.data = req->data.data,
		.size = req->data.index,
		.on_done = sdo_req__on_done,
		.context = req,
		.free_fn = sdo_req__on_stop
	};

	sdo_async_start(&queue->sdo_client, &info);

done:
	sdo_req_queue__unlock(queue);
}

void sdo_req__on_done(struct sdo_async* async)
{
	struct sdo_req_queue* queue = sdo_req_queue__from_async(async);

	struct sdo_req* req = async->context;
	assert(req != NULL);

	assert(async->status != SDO_REQ_PENDING);
	req->status = async->status;
	req->abort_code = async->abort_code;
	req->is_size_indicated = async->is_size_indicated;

	if (req->type == SDO_REQ_UPLOAD)
		if (vector_copy(&req->data, &async->buffer) < 0)
			req->status = SDO_REQ_NOMEM;

	sdo_req_fn on_done = req->on_done;
	if (on_done)
		on_done(req);

	mloop_iterate(mloop_default());
}

int sdo_req_start(struct sdo_req* self, struct sdo_req_queue* queue)
{
	sdo_req_ref(self);

	if (sdo_req_queue__enqueue(queue, self) == 0)
		return 0;

	sdo_req_unref(self);
	return -1;
}

