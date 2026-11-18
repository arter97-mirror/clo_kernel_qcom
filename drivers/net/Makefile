# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

# Subdirectories
obj-y += ethernet/stmicro/stmmac/
obj-y += mdio_fe/
obj-y += pcs/
obj-y += phy/aquantia/
obj-y += phy/qcom/

obj-$(CONFIG_DP83867_PHY) += phy/dp83867.o

obj-$(CONFIG_FAILOVER) += failover.o
failover-objs += ../../net/core/failover.o

obj-$(CONFIG_NET_FAILOVER) += net_failover.o

obj-$(CONFIG_VIRTIO_NET) += virtio_net.o
virtio_net-objs += virtio_net_fixed.o

