# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

# Subdirectories
obj-y += isl97900_led/
obj-y += lkdtm/

obj-$(CONFIG_QSEECOM_PROXY) += qseecom_proxy.o

obj-$(CONFIG_BOOTMARKER_PROXY) += bootmarker_proxy.o

obj-$(CONFIG_QTI_FASTRPC) += frpc-adsprpc.o
frpc-adsprpc-objs += fastrpc.o

obj-$(CONFIG_TPS6286_STEP_DOWN_CONV) += tps6286.o

obj-$(CONFIG_INA234_CURRENT_MONITOR) += ina234b.o

obj-$(CONFIG_SLG4DC_SIGNAL_IC) += slg4dc.o
