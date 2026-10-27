# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

obj-$(CONFIG_MHI_BUS) += mhi.o
mhi-objs += boot.o init.o main.o pm.o
mhi-$(CONFIG_MHI_BUS_MISC) += misc.o
mhi-$(CONFIG_MHI_BUS_DEBUG) += debugfs.o

obj-$(CONFIG_MHI_BUS_PCI_GENERIC) += mhi_pci_generic.o
mhi_pci_generic-y += pci_generic.o

