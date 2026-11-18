# SPDX-License-Identifier: GPL-2.0

ccflags-y += -I$(src)

# Subdirectories
obj-y += flash/
obj-y += rgb/

obj-$(CONFIG_LEDS_QTI_FLASH) += leds-qti-flash.o

obj-$(CONFIG_LEDS_TRIGGER_PATTERN) += ledtrig-pattern.o
ledtrig-pattern-objs += trigger/ledtrig-pattern.o

obj-$(CONFIG_LEDS_QPNP_VIBRATOR_LDO) += leds-qpnp-vibrator-ldo.o

obj-$(CONFIG_LEDS_AW2016) += leds-aw2016.o

obj-$(CONFIG_LEDS_AW200xx) += leds-aw200xx.o

obj-$(CONFIG_LEDS_USER_AW21104) += leds-user-aw21104.o

obj-$(CONFIG_LEDS_BYS_AW21104) += leds-bys-aw21104.o

obj-$(CONFIG_LEDS_USER_LP5814) += leds-user-lp5814.o

obj-$(CONFIG_LEDS_ETIRLED_LM3645) += leds-etirled-lm3645.o

