def register_modules(registry):
    registry.register(
        name = "drivers/soc/qcom/memshare/heap_mem_ext_v01",
        out = "heap_mem_ext_v01.ko",
        config = "CONFIG_MEM_SHARE_QMI_SERVICE",
        srcs = [
            # do not sort
            "drivers/soc/qcom/memshare/heap_mem_ext_v01.c",
            "drivers/soc/qcom/memshare/heap_mem_ext_v01.h",
        ],
        deps = [
            # do not sort
            "drivers/soc/qcom/qmi_helpers",
        ],
    )

    registry.register(
        name = "drivers/soc/qcom/memshare/msm_memshare",
        out = "msm_memshare.ko",
        config = "CONFIG_MEM_SHARE_QMI_SERVICE",
        srcs = [
            # do not sort
            "drivers/soc/qcom/memshare/heap_mem_ext_v01.h",
            "drivers/soc/qcom/memshare/msm_memshare.c",
            "drivers/soc/qcom/memshare/msm_memshare.h",
        ],
        deps = [
            # do not sort
            "drivers/soc/qcom/memshare/heap_mem_ext_v01",
            "drivers/soc/qcom/qmi_helpers",
            "drivers/remoteproc/rproc_qcom_common",
            "drivers/rpmsg/qcom_smd",
            "drivers/rpmsg/qcom_glink_smem",
            "drivers/rpmsg/qcom_glink",
            "kernel/trace/qcom_ipc_logging",
            "drivers/soc/qcom/minidump",
            "drivers/soc/qcom/smem",
            "drivers/soc/qcom/debug_symbol",
            "drivers/dma-buf/heaps/qcom_dma_heaps",
            "drivers/iommu/msm_dma_iommu_mapping",
            "drivers/soc/qcom/mem_buf/mem_buf_dev",
            "drivers/soc/qcom/secure_buffer",
            "drivers/firmware/qcom/qcom-scm",
            "drivers/virt/gunyah/gh_rm_drv",
            "drivers/virt/gunyah/gh_msgq",
            "drivers/virt/gunyah/gh_dbl",
            "arch/arm64/gunyah/gh_arm_drv",
            "drivers/soc/qcom/qcom_ramdump",
        ],
    )
