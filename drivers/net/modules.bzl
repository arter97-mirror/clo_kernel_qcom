load(":drivers/net/ethernet/realtek/modules.bzl", register_r8169 = "register_modules")
load(":drivers/net/ethernet/stmicro/stmmac/modules.bzl", register_stmmac_eth = "register_modules")
load(":drivers/net/mdio_fe/modules.bzl", register_mdio_fe = "register_modules")
load(":drivers/net/pcs/modules.bzl", register_pcs = "register_modules")
load(":drivers/net/phy/aquantia/modules.bzl", register_phy_aquantia = "register_modules")
load(":drivers/net/phy/motorcomm/modules.bzl", register_phy_motorcomm = "register_modules")
load(":drivers/net/phy/qcom/modules.bzl", register_phy_qcom = "register_modules")
load(":drivers/net/phy/realtek/modules.bzl", register_phy_realtek = "register_modules")

def register_modules(registry):
    register_pcs(registry)
    register_r8169(registry)
    register_stmmac_eth(registry)
    register_phy_aquantia(registry)
    register_mdio_fe(registry)
    register_phy_motorcomm(registry)
    register_phy_qcom(registry)
    register_phy_realtek(registry)
    registry.register(
        name = "drivers/net/phy/dp83867",
        out = "dp83867.ko",
        config = "CONFIG_DP83867_PHY",
        srcs = [
            # do not sort
            "drivers/net/phy/dp83867.c",
            "include/dt-bindings/net/ti-dp83867.h",
        ],
    )
    registry.register(
        name = "net/core/failover",
        out = "failover.ko",
        config = "CONFIG_FAILOVER",
        srcs = [
            # do not sort
            "net/core/failover.c",
        ],
    )

    registry.register(
        name = "drivers/net/net_failover",
        out = "net_failover.ko",
        config = "CONFIG_NET_FAILOVER",
        srcs = [
            # do not sort
            "drivers/net/net_failover.c",
        ],
        deps = [
            # do not sort
            "net/core/failover",
        ],
    )

    registry.register(
        name = "drivers/net/virtio_net",
        out = "virtio_net.ko",
        config = "CONFIG_VIRTIO_NET",
        srcs = [
            # do not sort
            "drivers/net/virtio_net_fixed.c",
        ],
        deps = [
            # do not sort
            "drivers/net/net_failover",
        ],
    )
