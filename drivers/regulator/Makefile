# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

obj-$(CONFIG_REGULATOR_DEBUG_CONTROL) += debug-regulator.o

obj-$(CONFIG_REGULATOR_PROXY_CONSUMER) += proxy-consumer.o

obj-$(CONFIG_REGULATOR_QCOM_AMOLED) += qcom-amoled-regulator.o

obj-$(CONFIG_REGULATOR_QTI_FIXED_VOLTAGE) += qti-fixed-regulator.o
qti-fixed-regulator-objs += qti_fixed_regulator.o
CFLAGS_qti_fixed_regulator.o += -DQTI_FIXED_REGULATOR

obj-$(CONFIG_REGULATOR_QTI_OCP_NOTIFIER) += qti-ocp-notifier.o

obj-$(CONFIG_REGULATOR_RPMH) += rpmh-regulator.o

obj-$(CONFIG_REGULATOR_RPM_SMD) += rpm-smd-regulator.o

obj-$(CONFIG_REGULATOR_STUB) += stub-regulator.o

obj-$(CONFIG_VIRTIO_REGULATOR) += virtio_regulator.o

obj-$(CONFIG_REGULATOR_REFGEN) += refgen.o

obj-$(CONFIG_REGULATOR_QPNP_LCDB) += qpnp-lcdb-regulator.o

obj-$(CONFIG_REGULATOR_QCOM_PM8008) += qcom-pm8008-regulator.o
