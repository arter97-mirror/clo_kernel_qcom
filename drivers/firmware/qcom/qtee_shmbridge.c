// SPDX-License-Identifier: GPL-2.0-only
/*
 * QTI TEE shared memory bridge driver
 *
 * Copyright (c) 2021, The Linux Foundation. All rights reserved.
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */

#define pr_fmt(fmt) "shmbridge: [%d]: " fmt, __LINE__

#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/qtee_shmbridge.h>
#include <linux/bitops.h>

#include "qtee_shmbridge_internal.h"
#include "qcom_tzmem.h"

struct qcom_tzmem_pool *shmbridge_pool;

static uint32_t cookie = 1;

static inline uint32_t handle_to_index(uint64_t handle)
{
	return lower_32_bits(handle);
}

static inline uint32_t handle_to_cookie(uint64_t handle)
{
	return upper_32_bits(handle);
}

static inline uint64_t index_to_handle(uint32_t index)
{
	return ((uint64_t)cookie << 32) | (uint64_t)index;
}

struct bridge_list {
	struct list_head head;
	struct mutex lock;
};

struct bridge_list_entry {
	struct list_head list;
	phys_addr_t paddr;
	uint64_t qtee_handle;
	uint32_t index;
	int32_t ref_count;
};

static struct bridge_list bridge_list_head;

static void qtee_shmbridge_entry_add_locked(struct bridge_list_entry *entry)
{
	struct bridge_list_entry *curr_entry;
	uint32_t index = 0;

	list_for_each_entry(curr_entry, &bridge_list_head.head, list) {
		if (curr_entry->index != index) {
			entry->index = index;
			list_add_tail(&entry->list, &curr_entry->list);
			return;
		}
		index++;
	}

	entry->index = index;
	list_add_tail(&entry->list, &bridge_list_head.head);
}

static int qtee_shmbridge_list_add_locked(phys_addr_t paddr, uint64_t qtee_handle)
{
	struct bridge_list_entry *entry;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->qtee_handle = qtee_handle;
	entry->paddr = paddr;
	entry->ref_count = 0;

	qtee_shmbridge_entry_add_locked(entry);
	return 0;
}

static void qtee_shmbridge_list_del_locked(uint32_t index)
{
	struct bridge_list_entry *entry;

	list_for_each_entry(entry, &bridge_list_head.head, list) {
		if (entry->index == index) {
			list_del(&entry->list);
			kfree(entry);
			break;
		}
	}
}

static int32_t qtee_shmbridge_list_dec_refcount_locked(uint64_t handle)
{
	struct bridge_list_entry *entry;
	int32_t ret = -EINVAL;
	uint32_t index = handle_to_index(handle);

	if (handle_to_cookie(handle) != cookie)
		return ret;

	list_for_each_entry(entry, &bridge_list_head.head, list) {
		if (entry->index == index) {
			if (entry->ref_count > 0) {
				/* decrement reference count. */
				entry->ref_count--;
				pr_debug("bridge on handle: %llx exists decrease refcount :%d\n",
					 handle, entry->ref_count);

				if (entry->ref_count == 0) {
					qcom_tzmem_shm_bridge_delete(entry->qtee_handle);
					qtee_shmbridge_list_del_locked(index);
				}
				ret = 0;
			} else {
				pr_err("ref_count should not be negative, handle %llx , refcount: %d\n",
					handle, entry->ref_count);
			}
			break;
		}
	}

	if (ret == -EINVAL)
		pr_err("Not able to find bridge handle %llx in map\n", handle);

	return ret;
}

static int32_t qtee_shmbridge_list_inc_refcount_locked(phys_addr_t paddr, uint64_t *handle)
{
	struct bridge_list_entry *entry;
	int32_t ret = -EINVAL;

	list_for_each_entry(entry, &bridge_list_head.head, list) {
		if (entry->paddr == paddr) {

			entry->ref_count++;
			/* update handle in case we found paddr already exist */
			*handle = index_to_handle(entry->index);
			pr_debug("bridge on paddr %llx exists increase refcount :%d, handle: %llx\n",
				 (uint64_t)paddr, entry->ref_count, *handle);

			ret = 0;
			break;
		}
	}
	if (ret)
		pr_err("Not able to find bridge paddr %llx in map\n", (uint64_t)paddr);
	return ret;
}

static void qtee_shmbridge_delete_list_locked(void)
{
	struct bridge_list_entry *entry, *next;

	list_for_each_entry_safe(entry, next, &bridge_list_head.head, list) {
		list_del(&entry->list);
		kfree(entry);
	}
}

bool qtee_shmbridge_is_enabled(void)
{
	return qcom_tzmem_get_status();
}
EXPORT_SYMBOL(qtee_shmbridge_is_enabled);

/* Check whether a bridge starting from paddr exists */
int32_t qtee_shmbridge_query(phys_addr_t paddr)
{
	return qcom_tzmem_query(paddr);
}
EXPORT_SYMBOL(qtee_shmbridge_query);

/* Register paddr & size as a bridge, return bridge handle */
int32_t qtee_shmbridge_register(phys_addr_t paddr, size_t size, uint32_t *ns_vmid_list,
	uint32_t *ns_vm_perm_list, uint32_t ns_vmid_num, uint32_t tz_perm, uint64_t *handle)
{
	int ret;
	uint64_t qtee_handle;

	if (!handle || !ns_vmid_list || !ns_vm_perm_list) {
		pr_err("invalid input parameters\n");
		return -EINVAL;
	}

	mutex_lock(&bridge_list_head.lock);
	ret = qcom_tzmem_query(paddr);
	if (ret)
		goto bridge_exist;

	ret = qcom_tzmem_shm_bridge_create_with_vmid(paddr, size, ns_vmid_list,
			ns_vm_perm_list, ns_vmid_num, tz_perm, OTHERS, &qtee_handle);
	if (ret)
		goto exit;

	ret = qtee_shmbridge_list_add_locked(paddr, qtee_handle);
	if (ret) {
		qcom_tzmem_shm_bridge_delete(qtee_handle);
		goto exit;
	}

bridge_exist:
	ret = qtee_shmbridge_list_inc_refcount_locked(paddr, handle);
exit:
	mutex_unlock(&bridge_list_head.lock);
	return ret;
}
EXPORT_SYMBOL(qtee_shmbridge_register);

/* Deregister bridge */
int32_t qtee_shmbridge_deregister(uint64_t handle)
{
	mutex_lock(&bridge_list_head.lock);
	qtee_shmbridge_list_dec_refcount_locked(handle);
	mutex_unlock(&bridge_list_head.lock);
	return 0;
}
EXPORT_SYMBOL(qtee_shmbridge_deregister);


/* Sub-allocate from default kernel bridge created by shmb driver */
int32_t qtee_shmbridge_allocate_shm(size_t size, struct qtee_shm *shm)
{
	shm->size = PAGE_ALIGN(size);
	shm->vaddr = qcom_tzmem_alloc(shmbridge_pool, shm->size, GFP_KERNEL);
	if (!shm->vaddr) {
		pr_err("Failed to alloc memory from shmbridge pool, size: %#zx\n", shm->size);
		return -ENOMEM;
	}

	shm->paddr = qcom_tzmem_to_phys(shm->vaddr);

	pr_debug("shm->paddr: 0x%llx, size: %zu\n", (uint64_t)shm->paddr, shm->size);

	return 0;
}
EXPORT_SYMBOL(qtee_shmbridge_allocate_shm);


/* Free buffer that is sub-allocated from default kernel bridge */
void qtee_shmbridge_free_shm(struct qtee_shm *shm)
{
	if (IS_ERR_OR_NULL(shm) || !shm->vaddr)
		return;
	qcom_tzmem_free(shm->vaddr);
}
EXPORT_SYMBOL(qtee_shmbridge_free_shm);

/* cache clean operation for buffer sub-allocated from default bridge */
void qtee_shmbridge_flush_shm_buf(struct qtee_shm *shm)
{
	qcom_tzmem_flush_shm_buf(shm->paddr, shm->size);
}
EXPORT_SYMBOL(qtee_shmbridge_flush_shm_buf);

/* cache invalidation operation for buffer sub-allocated from default bridge */
void qtee_shmbridge_inv_shm_buf(struct qtee_shm *shm)
{
	qcom_tzmem_inv_shm_buf(shm->paddr, shm->size);
}
EXPORT_SYMBOL(qtee_shmbridge_inv_shm_buf);

int qtee_shmbridge_driver_init(void)
{
	struct qcom_tzmem_pool_config pool_config;

	memset(&pool_config, 0, sizeof(pool_config));
	if (IS_ENABLED(CONFIG_QCOM_TZMEM_LOW_MEMORY))
		pool_config.initial_size = SZ_256K - SZ_32K; /* 224K */
	else
		pool_config.initial_size = SZ_512K;
	pool_config.policy = QCOM_TZMEM_POLICY_STATIC;
	pool_config.max_size = pool_config.initial_size;
	pool_config.is_cached = true;

	shmbridge_pool = qcom_tzmem_pool_new(&pool_config);
	if (IS_ERR(shmbridge_pool)) {
		pr_err("Failed to create qcom_tzmem_pool %ld\n", PTR_ERR(shmbridge_pool));
		return PTR_ERR(shmbridge_pool);
	}

	mutex_init(&bridge_list_head.lock);
	INIT_LIST_HEAD(&bridge_list_head.head);

	pr_info("shmbridge pool of size: %#zx is created successfully\n",
		pool_config.initial_size);
	return 0;
}

void qtee_shmbridge_driver_exit(void)
{
	mutex_lock(&bridge_list_head.lock);
	qtee_shmbridge_delete_list_locked();
	mutex_unlock(&bridge_list_head.lock);
	qcom_tzmem_pool_free(shmbridge_pool);
}

int qtee_shmbridge_pm_freeze(void)
{
	return 0;
}

int qtee_shmbridge_pm_restore(void)
{
	mutex_lock(&bridge_list_head.lock);
	if (++cookie == 0)
		cookie++;
	qtee_shmbridge_delete_list_locked();
	mutex_unlock(&bridge_list_head.lock);
	return 0;
}

int qtee_shmbridge_pm_thaw(void)
{
	return 0;
}
