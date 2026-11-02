# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

obj-$(CONFIG_EOM_MSM) += eom-driver.o
eom-driver-objs += phy_core.o eom_driver.o buffer_manager.o
eom-driver-$(CONFIG_ARCH_CANOE) += pcie_eom_canoe_phy.o
eom-driver-$(CONFIG_ARCH_SERAPH) += pcie_eom_seraph_phy.o
eom-driver-$(CONFIG_ARCH_X1E80100) += pcie_eom_hamoa_phy.o

