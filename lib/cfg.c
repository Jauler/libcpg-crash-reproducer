/*
 * Copyright (c) 2002-2005 MontaVista Software, Inc.
 * Copyright (c) 2006-2009 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>

#include <corosync/corotypes.h>
#include <corosync/cfg.h>
#include <corosync/totem/totemip.h>
#include <corosync/mar_gen.h>
#include <corosync/ipc_gen.h>
#include <corosync/ipc_cfg.h>
#include <corosync/coroipcc.h>

/*
 * Data structure for instance data
 */
struct cfg_instance {
	void *ipc_ctx;
	corosync_cfg_callbacks_t callbacks;
	cs_name_t comp_name;
	int comp_registered;
	int finalize;
	pthread_mutex_t response_mutex;
	pthread_mutex_t dispatch_mutex;
};

static void cfg_handle_instance_destructor (void *);

/*
 * All instances in one database
 */
static struct saHandleDatabase cfg_hdb = {
	.handleCount			= 0,
	.handles			= 0,
	.mutex				= PTHREAD_MUTEX_INITIALIZER,
	.handleInstanceDestructor	= cfg_handle_instance_destructor
};

/*
 * Implementation
 */
void cfg_handle_instance_destructor (void *instance)
{
	struct cfg_instance *cfg_instance = instance;

	pthread_mutex_destroy (&cfg_instance->response_mutex);
	pthread_mutex_destroy (&cfg_instance->dispatch_mutex);
}

cs_error_t
corosync_cfg_initialize (
	corosync_cfg_handle_t *cfg_handle,
	const corosync_cfg_callbacks_t *cfg_callbacks)
{
	struct cfg_instance *cfg_instance;
	cs_error_t error = CS_OK;

	error = saHandleCreate (&cfg_hdb, sizeof (struct cfg_instance), cfg_handle);
	if (error != CS_OK) {
		goto error_no_destroy;
	}

	error = saHandleInstanceGet (&cfg_hdb, *cfg_handle, (void *)&cfg_instance);
	if (error != CS_OK) {
		goto error_destroy;
	}

	error = coroipcc_service_connect (IPC_SOCKET_NAME, CFG_SERVICE, &cfg_instance->ipc_ctx);
	if (error != CS_OK) {
		goto error_put_destroy;
	}

	if (cfg_callbacks) {
	memcpy (&cfg_instance->callbacks, cfg_callbacks, sizeof (corosync_cfg_callbacks_t));
	}

	pthread_mutex_init (&cfg_instance->response_mutex, NULL);

	pthread_mutex_init (&cfg_instance->dispatch_mutex, NULL);

	(void)saHandleInstancePut (&cfg_hdb, *cfg_handle);

	return (CS_OK);

error_put_destroy:
	(void)saHandleInstancePut (&cfg_hdb, *cfg_handle);
error_destroy:
	(void)saHandleDestroy (&cfg_hdb, *cfg_handle);
error_no_destroy:
	return (error);
}

cs_error_t
corosync_cfg_fd_get (
	corosync_cfg_handle_t cfg_handle,
	int32_t *selection_fd)
{
	struct cfg_instance *cfg_instance;
	cs_error_t error;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle, (void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	*selection_fd = coroipcc_fd_get (cfg_instance->ipc_ctx);

	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);
	return (CS_OK);
}

cs_error_t
corosync_cfg_dispatch (
	corosync_cfg_handle_t cfg_handle,
	cs_dispatch_flags_t dispatch_flags)
{
	int timeout = -1;
	cs_error_t error;
	int cont = 1; /* always continue do loop except when set to 0 */
	int dispatch_avail;
	struct cfg_instance *cfg_instance;
	struct res_lib_cfg_testshutdown *res_lib_cfg_testshutdown;
	corosync_cfg_callbacks_t callbacks;
	mar_res_header_t *dispatch_data;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle,
		(void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	/*
	 * Timeout instantly for CS_DISPATCH_ALL
	 */
	if (dispatch_flags == CS_DISPATCH_ALL) {
		timeout = 0;
	}

	do {
		pthread_mutex_lock (&cfg_instance->dispatch_mutex);

		dispatch_avail = coroipcc_dispatch_get (
			cfg_instance->ipc_ctx,
			(void **)&dispatch_data,
			timeout);

		/*
		 * Handle has been finalized in another thread
		 */
		if (cfg_instance->finalize == 1) {
			error = CS_OK;
			pthread_mutex_unlock (&cfg_instance->dispatch_mutex);
			goto error_put;
		}

		if (dispatch_avail == 0 && dispatch_flags == CS_DISPATCH_ALL) {
			pthread_mutex_unlock (&cfg_instance->dispatch_mutex);
			break; /* exit do while cont is 1 loop */
		} else
		if (dispatch_avail == 0) {
			pthread_mutex_unlock (&cfg_instance->dispatch_mutex);
			continue; /* next poll */
		}

		/*
		 * Make copy of callbacks, message data, unlock instance, and call callback
		 * A risk of this dispatch method is that the callback routines may
		 * operate at the same time that cfgFinalize has been called in another thread.
		 */
		memcpy (&callbacks, &cfg_instance->callbacks, sizeof (corosync_cfg_callbacks_t));
		pthread_mutex_unlock (&cfg_instance->dispatch_mutex);

		/*
		 * Dispatch incoming response
		 */
		switch (dispatch_data->id) {
		case MESSAGE_RES_CFG_TESTSHUTDOWN:
			if (callbacks.corosync_cfg_shutdown_callback) {
				res_lib_cfg_testshutdown = (struct res_lib_cfg_testshutdown *)dispatch_data;
				callbacks.corosync_cfg_shutdown_callback(cfg_handle, res_lib_cfg_testshutdown->flags);
			}
			break;
		default:
			coroipcc_dispatch_put (cfg_instance->ipc_ctx);
			error = CS_ERR_LIBRARY;
			goto error_nounlock;
			break;
		}
		coroipcc_dispatch_put (cfg_instance->ipc_ctx);

		/*
		 * Determine if more messages should be processed
		 */
		switch (dispatch_flags) {
		case CS_DISPATCH_ONE:
			cont = 0;
			break;
		case CS_DISPATCH_ALL:
			break;
		case CS_DISPATCH_BLOCKING:
			break;
		}
	} while (cont);

error_put:
	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);
error_nounlock:
	return (error);
}

cs_error_t
corosync_cfg_finalize (
	corosync_cfg_handle_t cfg_handle)
{
	struct cfg_instance *cfg_instance;
	cs_error_t error;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle, (void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	pthread_mutex_lock (&cfg_instance->dispatch_mutex);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	/*
	 * Another thread has already started finalizing
	 */
	if (cfg_instance->finalize) {
		pthread_mutex_unlock (&cfg_instance->response_mutex);
		pthread_mutex_unlock (&cfg_instance->dispatch_mutex);
		(void)saHandleInstancePut (&cfg_hdb, cfg_handle);
		return (CS_ERR_BAD_HANDLE);
	}

	cfg_instance->finalize = 1;

	coroipcc_service_disconnect (cfg_instance->ipc_ctx);

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	pthread_mutex_unlock (&cfg_instance->dispatch_mutex);

	pthread_mutex_destroy (&cfg_instance->response_mutex);

	pthread_mutex_destroy (&cfg_instance->dispatch_mutex);

	(void)saHandleDestroy (&cfg_hdb, cfg_handle);

	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

	return (error);
}

cs_error_t
corosync_cfg_ring_status_get (
	corosync_cfg_handle_t cfg_handle,
	char ***interface_names,
	char ***status,
	unsigned int *interface_count)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_ringstatusget req_lib_cfg_ringstatusget;
	struct res_lib_cfg_ringstatusget res_lib_cfg_ringstatusget;
	unsigned int i;
	cs_error_t error;
	struct iovec iov;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle, (void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_ringstatusget.header.size = sizeof (struct req_lib_cfg_ringstatusget);
	req_lib_cfg_ringstatusget.header.id = MESSAGE_REQ_CFG_RINGSTATUSGET;

	iov.iov_base = 	&req_lib_cfg_ringstatusget,
	iov.iov_len = sizeof (struct req_lib_cfg_ringstatusget),

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive(cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_ringstatusget,
		sizeof (struct res_lib_cfg_ringstatusget));

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	*interface_count = res_lib_cfg_ringstatusget.interface_count;
	*interface_names = malloc (sizeof (char *) * *interface_count);
	if (*interface_names == NULL) {
		return (CS_ERR_NO_MEMORY);
	}
	memset (*interface_names, 0, sizeof (char *) * *interface_count);

	*status = malloc (sizeof (char *) * *interface_count);
	if (*status == NULL) {
		error = CS_ERR_NO_MEMORY;
		goto error_free_interface_names;
	}
	memset (*status, 0, sizeof (char *) * *interface_count);

	for (i = 0; i < res_lib_cfg_ringstatusget.interface_count; i++) {
		(*(interface_names))[i] = strdup (res_lib_cfg_ringstatusget.interface_name[i]);
		if ((*(interface_names))[i] == NULL) {
			error = CS_ERR_NO_MEMORY;
			goto error_free_contents;
		}
		(*(status))[i] = strdup (res_lib_cfg_ringstatusget.interface_status[i]);
		if ((*(status))[i] == NULL) {
			error = CS_ERR_NO_MEMORY;
			goto error_free_contents;
		}
	}
	goto no_error;

error_free_contents:
	for (i = 0; i < res_lib_cfg_ringstatusget.interface_count; i++) {
		if ((*(interface_names))[i]) {
			free ((*(interface_names))[i]);
		}
		if ((*(status))[i]) {
			free ((*(status))[i]);
		}
	}

	free (*status);

error_free_interface_names:
	free (*interface_names);

no_error:
	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

	return (error);
}

cs_error_t
corosync_cfg_ring_reenable (
	corosync_cfg_handle_t cfg_handle)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_ringreenable req_lib_cfg_ringreenable;
	struct res_lib_cfg_ringreenable res_lib_cfg_ringreenable;
	cs_error_t error;
	struct iovec iov;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle, (void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_ringreenable.header.size = sizeof (struct req_lib_cfg_ringreenable);
	req_lib_cfg_ringreenable.header.id = MESSAGE_REQ_CFG_RINGREENABLE;

	iov.iov_base = &req_lib_cfg_ringreenable,
	iov.iov_len = sizeof (struct req_lib_cfg_ringreenable);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_ringreenable,
		sizeof (struct res_lib_cfg_ringreenable));

	pthread_mutex_unlock (&cfg_instance->response_mutex);
	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

	return (error);
}

cs_error_t
corosync_cfg_service_load (
	corosync_cfg_handle_t cfg_handle,
	const char *service_name,
	unsigned int service_ver)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_serviceload req_lib_cfg_serviceload;
	struct res_lib_cfg_serviceload res_lib_cfg_serviceload;
	cs_error_t error;
	struct iovec iov;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle, (void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_serviceload.header.size = sizeof (struct req_lib_cfg_serviceload);
	req_lib_cfg_serviceload.header.id = MESSAGE_REQ_CFG_SERVICELOAD;
	memset (&req_lib_cfg_serviceload.service_name, 0,
		sizeof (req_lib_cfg_serviceload.service_name));
	strncpy (req_lib_cfg_serviceload.service_name, service_name,
		sizeof (req_lib_cfg_serviceload.service_name) - 1);
	req_lib_cfg_serviceload.service_ver = service_ver;

	iov.iov_base = &req_lib_cfg_serviceload;
	iov.iov_len = sizeof (req_lib_cfg_serviceload);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_serviceload,
		sizeof (struct res_lib_cfg_serviceload));

	pthread_mutex_unlock (&cfg_instance->response_mutex);
	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

	return (error);
}

cs_error_t
corosync_cfg_service_unload (
	corosync_cfg_handle_t cfg_handle,
	const char *service_name,
	unsigned int service_ver)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_serviceunload req_lib_cfg_serviceunload;
	struct res_lib_cfg_serviceunload res_lib_cfg_serviceunload;
	cs_error_t error;
	struct iovec iov;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle, (void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_serviceunload.header.size = sizeof (struct req_lib_cfg_serviceunload);
	req_lib_cfg_serviceunload.header.id = MESSAGE_REQ_CFG_SERVICEUNLOAD;
	memset (&req_lib_cfg_serviceunload.service_name, 0,
		sizeof (req_lib_cfg_serviceunload.service_name));
	strncpy (req_lib_cfg_serviceunload.service_name, service_name,
		sizeof (req_lib_cfg_serviceunload.service_name) - 1);
	req_lib_cfg_serviceunload.service_ver = service_ver;

	iov.iov_base = &req_lib_cfg_serviceunload;
	iov.iov_len = sizeof (req_lib_cfg_serviceunload);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_serviceunload,
		sizeof (struct res_lib_cfg_serviceunload));

	pthread_mutex_unlock (&cfg_instance->response_mutex);
	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

	return (error);
}
cs_error_t
corosync_cfg_state_track (
	corosync_cfg_handle_t cfg_handle,
	uint8_t track_flags,
	const corosync_cfg_state_notification_t *notification_buffer)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_statetrack req_lib_cfg_statetrack;
	struct res_lib_cfg_statetrack res_lib_cfg_statetrack;
	cs_error_t error;
	struct iovec iov;

	req_lib_cfg_statetrack.header.size = sizeof (struct req_lib_cfg_statetrack);
	req_lib_cfg_statetrack.header.id = MESSAGE_REQ_CFG_STATETRACKSTART;
	req_lib_cfg_statetrack.track_flags = track_flags;
	req_lib_cfg_statetrack.notification_buffer_address = (corosync_cfg_state_notification_t *)notification_buffer;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle,
		(void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	iov.iov_base = &req_lib_cfg_statetrack,
	iov.iov_len = sizeof (struct req_lib_cfg_statetrack),

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_statetrack,
		sizeof (struct res_lib_cfg_statetrack));

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

        return (error == CS_OK ? res_lib_cfg_statetrack.header.error : error);
}

cs_error_t
corosync_cfg_state_track_stop (
	corosync_cfg_handle_t cfg_handle)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_statetrackstop req_lib_cfg_statetrackstop;
	struct res_lib_cfg_statetrackstop res_lib_cfg_statetrackstop;
	cs_error_t error;
	struct iovec iov;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle,
		(void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_statetrackstop.header.size = sizeof (struct req_lib_cfg_statetrackstop);
	req_lib_cfg_statetrackstop.header.id = MESSAGE_REQ_CFG_STATETRACKSTOP;

	iov.iov_base = &req_lib_cfg_statetrackstop,
	iov.iov_len = sizeof (struct req_lib_cfg_statetrackstop),
	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_statetrackstop,
		sizeof (struct res_lib_cfg_statetrackstop));

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

        return (error == CS_OK ? res_lib_cfg_statetrackstop.header.error : error);
}

cs_error_t
corosync_cfg_kill_node (
	corosync_cfg_handle_t cfg_handle,
	unsigned int nodeid,
	const char *reason)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_killnode req_lib_cfg_killnode;
	struct res_lib_cfg_killnode res_lib_cfg_killnode;
	cs_error_t error;
	struct iovec iov;

	if (strlen(reason) >= CS_MAX_NAME_LENGTH)
		return CS_ERR_NAME_TOO_LONG;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle,
		(void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_killnode.header.id = MESSAGE_REQ_CFG_KILLNODE;
	req_lib_cfg_killnode.header.size = sizeof (struct req_lib_cfg_killnode);
	req_lib_cfg_killnode.nodeid = nodeid;
	strcpy((char *)req_lib_cfg_killnode.reason.value, reason);
	req_lib_cfg_killnode.reason.length = strlen(reason)+1;

	iov.iov_base = &req_lib_cfg_killnode;
	iov.iov_len = sizeof (struct req_lib_cfg_killnode);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_killnode,
		sizeof (struct res_lib_cfg_killnode));

	error = res_lib_cfg_killnode.header.error;

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

        return (error == CS_OK ? res_lib_cfg_killnode.header.error : error);
}

cs_error_t
corosync_cfg_try_shutdown (
	corosync_cfg_handle_t cfg_handle,
	corosync_cfg_shutdown_flags_t flags)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_tryshutdown req_lib_cfg_tryshutdown;
	struct res_lib_cfg_tryshutdown res_lib_cfg_tryshutdown;
	cs_error_t error;
	struct iovec iov;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle,
		(void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_tryshutdown.header.id = MESSAGE_REQ_CFG_TRYSHUTDOWN;
	req_lib_cfg_tryshutdown.header.size = sizeof (struct req_lib_cfg_tryshutdown);
	req_lib_cfg_tryshutdown.flags = flags;

	iov.iov_base = &req_lib_cfg_tryshutdown;
	iov.iov_len = sizeof (req_lib_cfg_tryshutdown);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_tryshutdown,
		sizeof (struct res_lib_cfg_tryshutdown));

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	(void)saHandleInstancePut (&cfg_hdb, cfg_handle);

        return (error == CS_OK ? res_lib_cfg_tryshutdown.header.error : error);
}

cs_error_t
corosync_cfg_replyto_shutdown (
	corosync_cfg_handle_t cfg_handle,
	corosync_cfg_shutdown_reply_flags_t response)
{
	struct cfg_instance *cfg_instance;
	struct req_lib_cfg_replytoshutdown req_lib_cfg_replytoshutdown;
	struct res_lib_cfg_replytoshutdown res_lib_cfg_replytoshutdown;
	struct iovec iov;
	cs_error_t error;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle,
		(void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_replytoshutdown.header.id = MESSAGE_REQ_CFG_REPLYTOSHUTDOWN;
	req_lib_cfg_replytoshutdown.header.size = sizeof (struct req_lib_cfg_replytoshutdown);
	req_lib_cfg_replytoshutdown.response = response;

	iov.iov_base = &req_lib_cfg_replytoshutdown;
	iov.iov_len = sizeof (struct req_lib_cfg_replytoshutdown);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive (cfg_instance->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_replytoshutdown,
		sizeof (struct res_lib_cfg_replytoshutdown));

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	return (error);
}

cs_error_t corosync_cfg_get_node_addrs (
	corosync_cfg_handle_t cfg_handle,
	int nodeid,
	size_t max_addrs,
	int *num_addrs,
	corosync_cfg_node_address_t *addrs)
{
	cs_error_t error;
	struct req_lib_cfg_get_node_addrs req_lib_cfg_get_node_addrs;
	struct res_lib_cfg_get_node_addrs *res_lib_cfg_get_node_addrs;
	struct cfg_instance *cfg_instance;
	int addrlen;
	int i;
	struct iovec iov;
	void *return_address;

	error = saHandleInstanceGet (&cfg_hdb, cfg_handle,
		(void *)&cfg_instance);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_get_node_addrs.header.size = sizeof (req_lib_cfg_get_node_addrs);
	req_lib_cfg_get_node_addrs.header.id = MESSAGE_REQ_CFG_GET_NODE_ADDRS;
	req_lib_cfg_get_node_addrs.nodeid = nodeid;

	iov.iov_base = (char *)&req_lib_cfg_get_node_addrs;
	iov.iov_len = sizeof (req_lib_cfg_get_node_addrs);

	pthread_mutex_lock (&cfg_instance->response_mutex);

	error = coroipcc_msg_send_reply_receive_in_buf (cfg_instance->ipc_ctx,
		&iov,
		1,
		&return_address);
	res_lib_cfg_get_node_addrs = return_address;

	pthread_mutex_unlock (&cfg_instance->response_mutex);

	if (error != CS_OK) {
		goto error_exit;
	}

	if (res_lib_cfg_get_node_addrs->family == AF_INET)
		addrlen = sizeof(struct sockaddr_in);
	if (res_lib_cfg_get_node_addrs->family == AF_INET6)
		addrlen = sizeof(struct sockaddr_in6);

	for (i=0; i<max_addrs && i<res_lib_cfg_get_node_addrs->num_addrs; i++) {
		struct sockaddr_in *in;
		struct sockaddr_in6 *in6;

		addrs[i].address_length = addrlen;

		if (res_lib_cfg_get_node_addrs->family == AF_INET) {
			in = (struct sockaddr_in *)addrs[i].address;
			in->sin_family = AF_INET;
			memcpy(&in->sin_addr, &res_lib_cfg_get_node_addrs->addrs[i][0], sizeof(struct in_addr));
		}
		if (res_lib_cfg_get_node_addrs->family == AF_INET6) {
			in6 = (struct sockaddr_in6 *)addrs[i].address;
			in6->sin6_family = AF_INET6;
			memcpy(&in6->sin6_addr, &res_lib_cfg_get_node_addrs->addrs[i][0], sizeof(struct in6_addr));
		}
	}
	*num_addrs = res_lib_cfg_get_node_addrs->num_addrs;
	errno = error = res_lib_cfg_get_node_addrs->header.error;

error_exit:

	return (error);
}

cs_error_t corosync_cfg_local_get (
	corosync_cfg_handle_t handle,
	unsigned int *local_nodeid)
{
	cs_error_t error;
	struct cfg_instance *cfg_inst;
	struct iovec iov;
	struct req_lib_cfg_local_get req_lib_cfg_local_get;
	struct res_lib_cfg_local_get res_lib_cfg_local_get;

	error = saHandleInstanceGet (&cfg_hdb, handle, (void *)&cfg_inst);
	if (error != CS_OK) {
		return (error);
	}

	req_lib_cfg_local_get.header.size = sizeof (mar_req_header_t);
	req_lib_cfg_local_get.header.id = MESSAGE_REQ_CFG_LOCAL_GET;

	iov.iov_base = &req_lib_cfg_local_get;
	iov.iov_len = sizeof (struct req_lib_cfg_local_get);

	pthread_mutex_lock (&cfg_inst->response_mutex);

	error = coroipcc_msg_send_reply_receive (
		cfg_inst->ipc_ctx,
		&iov,
		1,
		&res_lib_cfg_local_get,
		sizeof (struct res_lib_cfg_local_get));

	pthread_mutex_unlock (&cfg_inst->response_mutex);

	if (error != CS_OK) {
		goto error_exit;
	}

	error = res_lib_cfg_local_get.header.error;

	*local_nodeid = res_lib_cfg_local_get.local_nodeid;

error_exit:
	(void)saHandleInstancePut (&cfg_hdb, handle);

	return (error);
}
