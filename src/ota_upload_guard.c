/*
 * OTA Upload Guard – defence-in-depth callback that rejects any MCUmgr
 * firmware upload targeting the primary slot (slot0).
 *
 * The real fix is positioning slot1_partition at an external-flash
 * offset > CONFIG_FLASH_LOAD_OFFSET so the MCUmgr range-check macro
 * no longer misidentifies it as the running partition.  This guard is
 * kept as a safety net in case the macro is ever wrong again.
 *
 * We deliberately avoid #include <…/img_mgmt.h> because its transitive
 * dependency on bootutil/image.h is not available in the application
 * build.  Instead we replicate just the struct layouts we need.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>

LOG_MODULE_REGISTER(ota_upload_guard, LOG_LEVEL_INF);

/*
 * Minimal mirror of struct img_mgmt_upload_action (from img_mgmt.h)
 * – only the fields we inspect.  Layout must match the Zephyr source.
 */
struct ota_guard_upload_action {
	unsigned long long size;
	int write_bytes;
	int area_id;
	/* remaining fields omitted */
};

struct ota_guard_upload_check {
	struct ota_guard_upload_action *action;
	/* req pointer follows – not used */
};

static enum mgmt_cb_return ota_upload_chunk_guard(uint32_t event,
						  enum mgmt_cb_return prev_status,
						  int32_t *rc,
						  uint16_t *group,
						  bool *abort_more,
						  void *data,
						  size_t data_size)
{
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(data_size);

	if (event != MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK || data == NULL) {
		return MGMT_CB_OK;
	}

	struct ota_guard_upload_check *upload =
		(struct ota_guard_upload_check *)data;

	if (upload->action == NULL) {
		return MGMT_CB_OK;
	}

	const int slot0_id = FIXED_PARTITION_ID(slot0_partition);

	if (upload->action->area_id == slot0_id) {
		LOG_ERR("BLOCKED DFU chunk targeting slot0 (area_id=%d)",
			upload->action->area_id);

		if (rc != NULL) {
			*rc = MGMT_ERR_EACCESSDENIED;
		}
		if (abort_more != NULL) {
			*abort_more = true;
		}
		return MGMT_CB_ERROR_RC;
	}

	return MGMT_CB_OK;
}

static struct mgmt_callback ota_upload_guard_callback = {
	.callback = ota_upload_chunk_guard,
	.event_id = MGMT_EVT_OP_IMG_MGMT_DFU_CHUNK,
};

static int ota_upload_guard_init(void)
{
	mgmt_callback_register(&ota_upload_guard_callback);
	LOG_INF("OTA upload guard active (slot0 writes blocked)");
	return 0;
}

SYS_INIT(ota_upload_guard_init, APPLICATION, 99);
