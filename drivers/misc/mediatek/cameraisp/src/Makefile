#
# Copyright (C) 2015 MediaTek Inc.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#

#include $(srctree)/drivers/misc/mediatek/Makefile.custom

obj-y := cameraisp_dummy.o

ifeq ($(CONFIG_ARCH_MT6580),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

ifeq ($(CONFIG_ARCH_MT6735M),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

ifeq ($(CONFIG_MACH_MT6735),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

ifeq ($(CONFIG_ARCH_MT6755),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

ifeq ($(CONFIG_ARCH_MT6753),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

#ifeq ($(CONFIG_ARCH_MT6795),y)
#  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
#endif

ifeq ($(CONFIG_ARCH_MT8127),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

ifeq ($(CONFIG_ARCH_MT8163),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

ifeq ($(CONFIG_ARCH_MT8173),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif

ifeq ($(CONFIG_ARCH_MT6797),y)
  obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
endif


#this moment do not have other folder
#obj-y += $(subst ",,$(CONFIG_MTK_PLATFORM))/
