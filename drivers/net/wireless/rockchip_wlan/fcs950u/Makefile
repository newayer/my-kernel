#
# AP platform
#
# unisoc/qcom/rk/mtk
export COUSTOM_PLATFORM ?= rk

#
# UNISOC WCN chip ID
#
# umw2651/umw2652/umw2653
# uwe5621/uwe5622/uwe5623
export UNISOC_WCN_CHIP_ID ?= uwe5622

#
# UNISOC WCN hardware interface type
#
# sdio/usb/pcie(not supported)
export UNISOC_WCN_HW_TYPE ?= sdio

UNISOC_BSP_INCLUDE := $(srctree)/$(src)/unisocwcn/include
$(info UNISOC_BSP_INCLUDE = $(UNISOC_BSP_INCLUDE))
ifneq ($(UNISOC_BSP_INCLUDE),)
subdir-ccflags-y += -I$(UNISOC_BSP_INCLUDE)
endif

EXTRA_CFLAGS += -Wno-unused-variable
EXTRA_CFLAGS += -D__linux__

obj-y += unisocwcn/
obj-y += unisocwifi/
obj-y += unisocbt/
ifeq ($(UNISOC_WCN_CHIP_ID), umw2652)
obj-y += unisocfm/
endif
