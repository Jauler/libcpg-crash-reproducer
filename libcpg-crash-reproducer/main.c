#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <err.h>

#include <corosync/cpg.h>

void on_message_delivered (
	cpg_handle_t handle,
	const struct cpg_name *group_name,
	uint32_t nodeid,
	uint32_t pid,
	void *msg,
	size_t msg_len)
{
}

void on_configuration_hange (
	cpg_handle_t handle,
	const struct cpg_name *group_name,
	const struct cpg_address *member_list, size_t member_list_entries,
	const struct cpg_address *left_list, size_t left_list_entries,
	const struct cpg_address *joined_list, size_t joined_list_entries)
{
}

int main(void)
{
	cpg_handle_t handle;
	cpg_callbacks_t callbacks = {
		.cpg_deliver_fn = on_message_delivered,
		.cpg_confchg_fn = on_configuration_hange
	};

	if (cpg_initialize(&handle, &callbacks) != CS_OK)
		errx(EXIT_FAILURE, "Failed to initialize cpg");

	const char * group_name_str = "test-cluster";
	struct cpg_name group_name;
	group_name.length = sizeof(group_name_str);
	strcpy(group_name.value, group_name_str);
	if (cpg_join(handle, &group_name) != CS_OK)
		errx(EXIT_FAILURE, "Failed to join group");

	// Broadcast large message, libcpg modification will force CS_ERR_TRY_AGAIN to
	// occur during this message sending, although it might happen during normal
	// sending, just not deterministically.
	printf("Broadcasting large message until CS_ERR_TRY_AGAIN occurs...\n");
	size_t msg_40mb_size = 40 * 1024 * 1024; // 40MB
	struct iovec msg_40mb = {
		.iov_base = malloc(msg_40mb_size),
		.iov_len = msg_40mb_size
	};
	int mcast_status = cpg_mcast_joined(handle, CPG_TYPE_AGREED, &msg_40mb, 1);
	if (mcast_status != CS_ERR_TRY_AGAIN)
		errx(EXIT_FAILURE, "Expected status CS_ERR_TRY_AGAIN, but got %d\n", mcast_status);

	// After confirming that indeed CS_ERR_TRY_AGAIN occured, send another, even
	// larger message.
	printf("Broadcasting one more large message...\n");
	size_t msg_80MB_size = 80 * 1024 * 1024; // 80MB
	struct iovec msg_80MB = {
		.iov_base = malloc(msg_80MB_size),
		.iov_len = msg_80MB_size
	};
	while (cpg_mcast_joined(handle, CPG_TYPE_AGREED, &msg_80MB, 1) != CS_OK);

	// Start dispatching indefinatley. During one of those dispatch calls
	// assembly_buf will be overflowed, and valgrind will report errors.
	printf("Start dispatching...\n");
	while (1)
		cpg_dispatch(handle, CS_DISPATCH_ONE);

	if (cpg_leave(handle, &group_name) != CS_OK)
		errx(EXIT_FAILURE, "Failed to leave group");

	if (cpg_finalize(handle) != CS_OK)
		errx(EXIT_FAILURE, "Failed to finalize cpg");

	return 0;
}
