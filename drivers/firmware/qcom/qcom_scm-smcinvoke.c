// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#include <linux/delay.h>
#include <linux/qtee_shmbridge.h>
#include <linux/firmware/qcom/qcom_scm.h>

#include "qcom_scm.h"
#include "qcom_scm_smcinvoke.h"

static struct si_object *g_gpu_instance;
static struct si_object *g_dcvs_instance;
static DEFINE_MUTEX(g_gpu_mutex);
static DEFINE_MUTEX(g_dcvs_mutex);

/**
 * qcom_scm_gpu_and_dcvs_remap_error() - remap GPU/DCVS service errors
 * @ret: return code from SMCInvoke method
 * @err: service-specific result written by SMCInvoke method
 *
 * Handle transport failures via @ret and remap
 * GPU/DCVS result codes only when invocation itself succeeds.
 *
 * Return: 0 on success or a negative errno value.
 */
static int qcom_scm_gpu_and_dcvs_remap_error(int ret, int err)
{
	if (!ret && !err)
		return 0;

	if (ret)
		return ret;

	if (err == GPU_ERROR_OPERATION_FAILED ||
	    err == GPU_DCVS_ERROR_INVALID_ARG)
		return -EFAULT;

	return -EINVAL;
}

bool qcom_scm_pas_supported(u32 peripheral)
{
	struct si_object *pil_image_service = NULL;
	int ret;

	ret = qcom_scm_pas_pil_service_init(peripheral, &pil_image_service);
	return ret ? false : true;
}
EXPORT_SYMBOL_GPL(qcom_scm_pas_supported);

int qcom_scm_pas_init_image(u32 peripheral, const void *metadata, size_t size,
		struct qcom_scm_pas_metadata *ctx, struct device *dev_32bit)
{
	struct si_object *pil_image_service = NULL;
	struct si_object *smo = NULL;
	struct device *dma_dev = NULL;
	void *mdata_buf = NULL;
	dma_addr_t mdata_phys;
	int ret;

	if (!metadata || size == 0)
		return -EINVAL;

	ret = qcom_scm_pas_pil_service_init(peripheral, &pil_image_service);
	if (ret)
		return ret;

	dma_dev = qcom_scmi_get_dev();
	if (!dma_dev)
		return -ENODEV;

	if (dev_32bit)
		dma_dev = dev_32bit;

	mdata_buf = dma_alloc_coherent(dma_dev, size, &mdata_phys, GFP_KERNEL);
	if (!mdata_buf)
		return -ENOMEM;
	memcpy(mdata_buf, metadata, size);

	ret = qcom_smci_init_smobject(mdata_phys, mdata_buf, size, &smo,
			SI_CORE_MEM_OBJ_SHARE | SI_CORE_MEM_OBJ_UNCACHED);
	if (ret) {
		dev_err(dma_dev,
			"Failed to initialize shared memory object for metadata: %d\n", ret);
		goto out;
	}

	ret = qcom_smci_smo_call(pil_image_service, smo, SMCI_PILIMAGE_OP_VERIFYMETADATA);
	if (ret)
		pr_err("Failed to verify pil image metadata (%d)\n", ret);

	/* Release smo */
	put_si_object(smo);

out:
	/*
	 * If ctx is NULL, release the metadata immediately after use
	 * If ctx is not NULL, store the metadata info for later release
	 */
	if (ret || !ctx) {
		dma_free_coherent(dma_dev, size, mdata_buf, mdata_phys);
	} else {
		ctx->ptr = mdata_buf;
		ctx->phys = mdata_phys;
		ctx->size = size;
	}
	return ret;
}
EXPORT_SYMBOL_GPL(qcom_scm_pas_init_image);

int qcom_scm_pas_shutdown(u32 peripheral)
{
	struct si_object *pil_image_service = NULL;
	struct si_arg args[1] = { 0 };
	int ret, result;

	ret = qcom_scm_pas_pil_service_init(peripheral, &pil_image_service);
	if (ret)
		return ret;

	args[0].type = SI_AT_END;

	/*
	 * Shutdown/teardown the specified peripheral and unlock the memory area
	 * occupied by that region.
	 */
	ret = qcom_smci_call(pil_image_service, SMCI_PILIMAGE_OP_UNLOCKAREA, args,
			     &result);
	ret = qcom_scmi_remap_error(ret, result);
	if (ret)
		return ret;

	/* Release the SMO and PIL sessions in TZ to free the allocated memory only
	 * when unlockArea succeeds, as the device will panic if unlockArea fails.
	 */
	qcom_smci_release_image_service(SMCI_PILOBJECT_UID, peripheral);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_scm_pas_shutdown);

int qcom_scm_pas_shutdown_retry(u32 peripheral)
{
	unsigned int pas_shutdown_retry_delay_ms = 0;
	int ret;

	ret = qcom_scm_pas_shutdown(peripheral);
	/* No need to retry if the first try worked */
	if (!ret)
		return ret;

	pas_shutdown_retry_delay_ms = qcom_scm_pas_get_shutdown_retry_delay_ms();
	if (pas_shutdown_retry_delay_ms > 0) {
		pr_err("PAS Shutdown: First call to shutdown failed with error: %d\n", ret);
		pr_err("PAS Shutdown: Sleeping for: %u\n", pas_shutdown_retry_delay_ms);
		msleep(pas_shutdown_retry_delay_ms);

		pr_err("PAS Shutdown: Attempting to shutdown peripheral again\n");
		return qcom_scm_pas_shutdown(peripheral);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_scm_pas_shutdown_retry);

int qcom_scm_pas_mem_setup(u32 peripheral, phys_addr_t addr, phys_addr_t size)
{
	struct si_object *pil_image_service = NULL;
	int ret, result;

	struct {
		uint64_t m_address;
		uint64_t m_size;
	} __packed buf = {addr, size};

	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &buf, .size = sizeof(buf) },
		},
		{
			.type = SI_AT_END,
		}
	};

	ret = qcom_scm_pas_pil_service_init(peripheral, &pil_image_service);
	if (ret)
		return ret;

	ret = qcom_smci_call(pil_image_service, SMCI_PILIMAGE_OP_SETUPMEMAREA,
			     args, &result);
	ret = qcom_scmi_remap_error(ret, result);
	if (ret)
		pr_err("memory setup failed with result %d: %d\n", result, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_scm_pas_mem_setup);

int qcom_scm_pas_auth_and_reset(u32 peripheral)
{
	struct si_object *pil_image_service = NULL;
	struct si_object *smo = NULL;
	phys_addr_t addr = 0;
	size_t size = 0;
	int ret;

	ret = qcom_scm_pas_pil_service_init(peripheral, &pil_image_service);
	if (ret)
		return ret;

	/* Retrieve Memory Physical Address and Size */
	qcom_smci_get_memory(SMCI_PILOBJECT_UID, peripheral, &addr, &size);
	if (!addr || !size) {
		pr_err("Failed to get memory region address and size\n");
		return -EINVAL;
	}

	/*
	 * Initialize shared object with physical address and size
	 * If return value is 0, smo will not be NULL_SI_OBJECT.
	 */
	ret = qcom_smci_init_smobject(addr, phys_to_virt(addr), size, &smo,
		SI_CORE_MEM_OBJ_LEND);
	if (ret) {
		pr_err("Failed to initialize shared memory object (%d)\n", ret);
		return ret;
	}

	/*
	 * The memory area will be locked after auth reset; SMO cannot be released
	 * while in the memory lock state.
	 */
	ret = qcom_smci_smo_call(pil_image_service, smo, SMCI_PILIMAGE_OP_AUTHRESET);
	if (ret) {
		pr_err("Failed to auth and reset for %d\n", peripheral);

		/* Release smo */
		put_si_object(smo);
		return ret;
	}

	/*
	 * The memory area will be unlocked during PIL shutdown; store the SMO
	 * to be released at that time.
	 * qcom_smci_release_image_service will be called in PIL shutdown to
	 * release SMO.
	 */
	qcom_smci_store_smo(SMCI_PILOBJECT_UID, peripheral, smo);

	return 0;
}
EXPORT_SYMBOL_GPL(qcom_scm_pas_auth_and_reset);

void qcom_scm_pas_store_memoryinfo(u32 peripheral, phys_addr_t addr,
		phys_addr_t size)
{
	qcom_smci_store_memory(SMCI_PILOBJECT_UID, peripheral, addr, size);
}
EXPORT_SYMBOL_GPL(qcom_scm_pas_store_memoryinfo);

static int qcom_smci_get_instance(struct si_object *service, uint32_t instance_id,
				  unsigned long op, struct si_object **instance)
{
	int ret, result;
	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &instance_id, .size = sizeof(instance_id) },
		},
		{
			.type = SI_AT_OO,
		},
		{
			.type = SI_AT_END,
		}
	};

	ret = qcom_smci_call(service, op, args, &result);
	ret = qcom_scmi_remap_error(ret, result);
	if (ret)
		return ret;

	if (result == 0 && args[1].o != NULL_SI_OBJECT) {
		*instance = args[1].o;
		return 0;
	}

	return -EINVAL;
}

static struct si_object *get_gpu_instance(u32 instance_id)
{
	struct si_object *kgsl_service = NULL;
	int ret;

	if (instance_id != 0)
		return ERR_PTR(-EINVAL);

	mutex_lock(&g_gpu_mutex);
	if (!g_gpu_instance) {
		ret = qcom_smci_init_client_service(SMCI_GPUOBJECT_UID, &kgsl_service);
		if (ret) {
			mutex_unlock(&g_gpu_mutex);
			return ERR_PTR(ret);
		}

		ret = qcom_smci_get_instance(kgsl_service, instance_id,
					     SMCI_GPU_OP_INIT_INSTANCE,
					     &g_gpu_instance);
		if (ret) {
			mutex_unlock(&g_gpu_mutex);
			return ERR_PTR(ret);
		}
	}
	mutex_unlock(&g_gpu_mutex);

	return g_gpu_instance;
}

static struct si_object *get_dcvs_instance(u32 instance_id)
{
	struct si_object *dcvs_service = NULL;
	int ret;

	if (instance_id != 0)
		return ERR_PTR(-EINVAL);

	mutex_lock(&g_dcvs_mutex);
	if (!g_dcvs_instance) {
		ret = qcom_smci_init_client_service(SMCI_DCVSOBJECT_UID, &dcvs_service);
		if (ret) {
			mutex_unlock(&g_dcvs_mutex);
			return ERR_PTR(ret);
		}

		ret = qcom_smci_get_instance(dcvs_service, instance_id,
					     SMCI_DCVS_OP_INIT_INSTANCE,
					     &g_dcvs_instance);
		if (ret) {
			mutex_unlock(&g_dcvs_mutex);
			return ERR_PTR(ret);
		}
	}
	mutex_unlock(&g_dcvs_mutex);

	return g_dcvs_instance;
}

int qcom_scm_mem_protect_video_var(u32 cp_start, u32 cp_size,
				   u32 cp_nonpixel_start,
				   u32 cp_nonpixel_size)
{
	struct si_object *video_var_service = NULL;
	int ret = 0, result = 0;

	struct {
		uint32_t m_cp_start;
		uint32_t m_cp_size;
		uint32_t m_nonpixel_start;
		uint32_t m_nonpixel_size;
	} __packed buf = {cp_start, cp_size, cp_nonpixel_start, cp_nonpixel_size};

	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &buf, .size = sizeof(buf) },
		},
		{
			.type = SI_AT_END,
		}
	};

	ret = qcom_smci_init_client_service(SMCI_VIDEOVAROBJECT_UID, &video_var_service);
	if (ret) {
		pr_err("Failed to initialize video var service: %d\n", ret);
		return ret;
	}

	ret = qcom_smci_call(video_var_service, SMCI_SET_VIDEO_VAR, args, &result);
	ret = qcom_scmi_remap_error(ret, result);
	if (ret)
		pr_err("Setting video vars failed with result %d: ret %d\n", result, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_scm_mem_protect_video_var);

int qcom_scm_assign_dump_table_region(bool __always_unused is_assign,
				      phys_addr_t addr, size_t size)
{
	struct si_object *dt_service = NULL;
	struct si_object *smo = NULL;
	int ret;

	ret = qcom_smci_init_client_service(SMCI_DT_UID, &dt_service);
	if (ret)
		return ret;

	ret = qcom_smci_init_smobject(addr, phys_to_virt(addr), size, &smo,
				      SI_CORE_MEM_OBJ_SHARE);
	if (ret) {
		pr_err("Failed to initialize shared memory object (%d)\n", ret);
		return ret;
	}

	ret = qcom_smci_smo_call(dt_service, smo, SMCI_DT_OP_SET);
	if (ret) {
		pr_err("Failed to set dump table (ret = %d).\n", ret);
		put_si_object(smo);
		return ret;
	}

	/*
	 * The memory area will remain locked until system crash, and the smo
	 * is released only when the module exits.
	 */
	qcom_smci_store_client_smo(SMCI_DT_UID, smo);
	return 0;
}
EXPORT_SYMBOL_GPL(qcom_scm_assign_dump_table_region);

int qcom_scm_kgsl_dcvs_tuning(u32 mingap, u32 penalty, u32 numbusy)
{
	struct si_object *dcvs_instance = NULL;
	int ret, result = 0;

	struct tuning_data {
		uint32_t m_req_mingap;
		uint32_t m_req_penality;
		uint32_t m_req_numbusy;
	} __packed buf;

	buf.m_req_mingap = mingap;
	buf.m_req_penality = penalty;
	buf.m_req_numbusy = numbusy;

	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &buf, .size = sizeof(buf) },
		},
		{
			.type = SI_AT_END,
		}
	};

	dcvs_instance = get_dcvs_instance(0);
	if (IS_ERR(dcvs_instance))
		return PTR_ERR(dcvs_instance);

	ret = qcom_smci_call(dcvs_instance, SMCI_DCVS_OP_TUNING, args, &result);
	return qcom_scm_gpu_and_dcvs_remap_error(ret, result);
}
EXPORT_SYMBOL_GPL(qcom_scm_kgsl_dcvs_tuning);

int _dcvs_update(int level, s64 total_time, s64 busy_time, int context_count)
{
	struct si_object *dcvs_instance = NULL;
	int ret, result = 0, scale_up = 0;

	struct dcvs_update {
		uint32_t m_active_pwrlevel;
		uint32_t m_total;
		uint32_t m_inBusy;
		uint32_t m_context_count;
	} __packed buf;

	buf.m_active_pwrlevel = level;
	buf.m_total = total_time;
	buf.m_inBusy = busy_time;
	buf.m_context_count = context_count;

	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &buf, .size = sizeof(buf) },
		},
		{
			.type = SI_AT_OB,
			.b = { .addr = &scale_up, .size = sizeof(scale_up) },
		},
		{
			.type = SI_AT_END,
		}
	};

	dcvs_instance = get_dcvs_instance(0);
	if (IS_ERR(dcvs_instance))
		return PTR_ERR(dcvs_instance);

	ret = qcom_smci_call(dcvs_instance, SMCI_DCVS_OP_UPDATE, args, &result);
	ret = qcom_scm_gpu_and_dcvs_remap_error(ret, result);
	if (ret) {
		pr_err("DCVS update failed (result: %d, ret: %d)\n", result, ret);
		return ret;
	}

	return scale_up;
}

int qcom_scm_dcvs_update_v2(int level, s64 total_time, s64 busy_time)
{
	/* sending an invalid value, for update_v2 we don't use context_count */
	return _dcvs_update(level, total_time, busy_time, 0);
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_update_v2);

int qcom_scm_dcvs_update_ca_v2(int level, s64 total_time, s64 busy_time,
			       int context_count)
{
	return _dcvs_update(level, total_time, busy_time, context_count);
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_update_ca_v2);

/*
 * qcom_scm_dcvs_reset()
 */
int qcom_scm_dcvs_reset(void)
{
	struct si_object *dcvs_instance = NULL;
	struct si_arg args[] = {
		{
			.type = SI_AT_END,
		}
	};
	int ret, result = 0;

	dcvs_instance = get_dcvs_instance(0);
	if (IS_ERR(dcvs_instance))
		return PTR_ERR(dcvs_instance);

	ret = qcom_smci_call(dcvs_instance, SMCI_DCVS_OP_RESET, args, &result);
	return qcom_scm_gpu_and_dcvs_remap_error(ret, result);
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_reset);

int qcom_scm_dcvs_init_ca_v2(phys_addr_t addr, size_t size)
{
	struct si_object *dcvs_instance = NULL;
	int ret, result = 0;
	void *virt_src = NULL;
	struct dcvs_init_ca {
		uint32_t ctxt_aware_target_pwrlevel;
		uint32_t ctxt_aware_busy_penalty;
	} __packed cmd_buf = { 0 };
	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &cmd_buf, .size = sizeof(cmd_buf) },
		},
		{
			.type = SI_AT_END,
		},
	};

	if (size > sizeof(cmd_buf))
		return -EINVAL;

	virt_src = memremap(addr, size, MEMREMAP_WB);
	if (!virt_src)
		return -ENOMEM;

	memcpy(&cmd_buf, virt_src, size);
	memunmap(virt_src);

	dcvs_instance = get_dcvs_instance(0);
	if (IS_ERR(dcvs_instance))
		return PTR_ERR(dcvs_instance);

	ret = qcom_smci_call(dcvs_instance, SMCI_DCVS_OP_INIT_CA, args, &result);
	return qcom_scm_gpu_and_dcvs_remap_error(ret, result);
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_init_ca_v2);

int qcom_scm_dcvs_init_v2(phys_addr_t addr, size_t size, int *version)
{
	struct si_object *dcvs_instance = NULL;
	int ret, result = 0;
	void *virt_src = NULL;
	uint32_t size_val = size;
	struct dcvs_req {
		uint32_t nlevels;
		uint32_t freq[32];
	} __packed cmd_buf = { 0 };
	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &cmd_buf, .size = sizeof(cmd_buf) },
		},
		{
			.type = SI_AT_IB,
			.b = { .addr = &size_val, .size = sizeof(size_val) },
		},
		{
			.type = SI_AT_OB,
			.b = { .addr = version, .size = sizeof(*version) },
		},
		{
			.type = SI_AT_END,
		},
	};

	if (size > sizeof(cmd_buf))
		return -EINVAL;

	virt_src = memremap(addr, size, MEMREMAP_WB);
	if (!virt_src)
		return -ENOMEM;

	memcpy(&cmd_buf, virt_src, size);
	memunmap(virt_src);

	/*
	 * We are re-initializing, so even if the instance exists, we
	 * want to call INIT on it to update the params.
	 * get_dcvs_instance(0) will create it if it doesn't exist.
	 */
	dcvs_instance = get_dcvs_instance(0);
	if (IS_ERR(dcvs_instance))
		return PTR_ERR(dcvs_instance);

	ret = qcom_smci_call(dcvs_instance, SMCI_DCVS_OP_INIT, args, &result);
	return qcom_scm_gpu_and_dcvs_remap_error(ret, result);
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_init_v2);

int qcom_scm_dcvs_update(int level, s64 total_time, s64 busy_time)
{
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_update);

bool qcom_scm_dcvs_core_available(void)
{
	struct si_object *dcvs_service = NULL;

	/*
	 * qcom_scmci_init_client_serive() return 0 if the service is there,
	 * So !0 will be become True.
	 */
	return !qcom_smci_init_client_service(SMCI_DCVSOBJECT_UID, &dcvs_service);
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_core_available);

/*
 * qcom_scm_dcvs_ca_available() - check if context aware DCVS operations are
 *				  available
 */
bool qcom_scm_dcvs_ca_available(void)
{
	return qcom_scm_dcvs_core_available();
}
EXPORT_SYMBOL_GPL(qcom_scm_dcvs_ca_available);

int qcom_scm_kgsl_init_regs(u32 gpu_req)
{
	struct si_object *kgsl_instance = NULL;
	int ret, result = 0;

	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &gpu_req, .size = sizeof(u32) },
		},
		{
			.type = SI_AT_END,
		}
	};

	kgsl_instance = get_gpu_instance(0);
	if (IS_ERR(kgsl_instance))
		return PTR_ERR(kgsl_instance);

	ret = qcom_smci_call(kgsl_instance, SMCI_GPU_OP_REG_SETUP, args, &result);
	return qcom_scm_gpu_and_dcvs_remap_error(ret, result);
}
EXPORT_SYMBOL_GPL(qcom_scm_kgsl_init_regs);

/**
 * _qcom_scm_kgsl_set_smmu_aperture() - configure GPU SMMU aperture values
 * @config1: aperture configuration word 1
 * @config2: aperture configuration word 2
 * @config3: aperture configuration word 3
 * @config4: aperture configuration word 4
 *
 * Build and invoke the secure GPU request payload used for SMMU aperture
 * programming.
 *
 * Return: 0 on success, or a negative errno value on failure.
 */
static int _qcom_scm_kgsl_set_smmu_aperture(u32 config1, u32 config2,
					    u32 config3, u32 config4)
{
	struct si_object *kgsl_instance = NULL;
	int ret, result = 0;
	struct {
		u32 aperture_config1;
		u32 aperture_config2;
		u32 aperture_config3;
		u32 aperture_config4;
	} __packed buf = {
		.aperture_config1 = config1,
		.aperture_config2 = config2,
		.aperture_config3 = config3,
		.aperture_config4 = config4,
	};
	struct si_arg args[] = {
		{
			.type = SI_AT_IB,
			.b = { .addr = &buf, .size = sizeof(buf) },
		},
		{
			.type = SI_AT_END,
		}
	};

	kgsl_instance = get_gpu_instance(0);
	if (IS_ERR(kgsl_instance))
		return PTR_ERR(kgsl_instance);

	ret = qcom_smci_call(kgsl_instance,
			     SMCI_GPU_OP_CONFIG_GPU_SMMU_APERTURE,
			     args, &result);
	if (ret) {
		pr_err("GPU config SMMU aperture failed: %d\n", ret);
		return ret;
	}

	return ret;
}

int qcom_scm_kgsl_set_smmu_aperture(unsigned int num_context_bank)
{
	int ret;

	ret = _qcom_scm_kgsl_set_smmu_aperture(0xffff0000 |
						((QCOM_SCM_CP_APERTURE_REG & 0xff) << 8) |
						(num_context_bank & 0xff),
						0xffffffff, 0xffffffff,
						0xffffffff);
	if (ret) {
		pr_err("GPU set SMMU aperture failed: %d\n", ret);
		return ret;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_scm_kgsl_set_smmu_aperture);

int qcom_scm_kgsl_set_smmu_lpac_aperture(unsigned int num_context_bank)
{
	int ret;

	ret = _qcom_scm_kgsl_set_smmu_aperture(0xffff0000 |
						((QCOM_SCM_CP_LPAC_APERTURE_REG & 0xff) << 8) |
						(num_context_bank & 0xff),
						0xffffffff, 0xffffffff,
						0xffffffff);
	if (ret) {
		pr_err("GPU set SMMU LPAC aperture failed: %d\n", ret);
		return ret;
	}

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_scm_kgsl_set_smmu_lpac_aperture);

int qcom_scm_dcc_fetch_data(void *to, size_t count)
{
	struct si_object *dcc_service = NULL;
	int ret, result;

	struct si_arg args[] = {
		{
			.type = SI_AT_OB,
			.b = { .addr = to, .size = count },
		},
		{
			.type = SI_AT_END,
		}
	};

	ret = qcom_smci_init_client_service(SMCI_DCCSRAM_UID, &dcc_service);
	if (ret)
		return ret;

	ret = qcom_smci_call(dcc_service, SMCI_DCCSRAM_OP_FETCH, args, &result);
	ret = qcom_scmi_remap_error(ret, result);
	if (ret)
		pr_err("dcc fetch data failed with result %d: %d\n", result, ret);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_scm_dcc_fetch_data);
