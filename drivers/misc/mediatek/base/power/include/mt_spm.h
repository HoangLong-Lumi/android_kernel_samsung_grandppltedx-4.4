/*
 * Copyright (C) 2015 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#ifndef __MT_SPM_H__
#define __MT_SPM_H__

#if defined(CONFIG_ARCH_MT6755) || defined(CONFIG_ARCH_MT6797)

#include "spm_v2/mt_spm.h"

#elif defined(CONFIG_MACH_MT6735) || defined(CONFIG_ARCH_MT6735M) || defined(CONFIG_ARCH_MT6753)

#elif defined(CONFIG_ARCH_MT6580)

#include "spm_v1/mt_spm_mt6580.h"

#endif

#endif /* __MT_SPM_H__ */

