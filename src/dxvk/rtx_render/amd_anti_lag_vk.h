#pragma once

// Fork AMD start: interface VK_AMD_anti_lag
// ---------------------------------------------------------------------------
// Copie de secours des définitions VK_AMD_anti_lag (spec officielle Khronos,
// publication aout 2024). Les SDK Vulkan >= 1.3.290 definissent deja ces types
// dans vulkan_core.h ; le garde ci-dessous evite toute collision. Sur un SDK
// plus ancien, ce header fournit l'interface minimale.
// Modèle inspiré du header NVIDIA <NvLowLatencyVk.h> embarqué par dxvk-remix.
// ---------------------------------------------------------------------------

#include "../dxvk_include.h"

#ifndef VK_AMD_anti_lag
#define VK_AMD_anti_lag 1

#define VK_AMD_ANTI_LAG_SPEC_VERSION   1
#define VK_AMD_ANTI_LAG_EXTENSION_NAME "VK_AMD_anti_lag"

typedef enum VkAntiLagModeAMD {
    VK_ANTI_LAG_MODE_ON_AMD  = 1,
    VK_ANTI_LAG_MODE_OFF_AMD = 2,
    VK_ANTI_LAG_MODE_MAX_ENUM_AMD = 0x7FFFFFFF
} VkAntiLagModeAMD;

typedef struct VkAntiLagDataAMD {
    VkStructureType    sType;
    const void*        pNext;
    VkAntiLagModeAMD   mode;
    VkBool32           inFrameMarker;
    VkBool32           outFrameMarker;
    VkBool32           drawMarker;
} VkAntiLagDataAMD;

typedef struct VkAntiLagPresentationInfoAMD {
    VkStructureType    sType;
    const void*        pNext;
    VkAntiLagModeAMD   mode;
    uint64_t           frame;
} VkAntiLagPresentationInfoAMD;

typedef void (VKAPI_PTR *PFN_vkCmdSetAntiLagDataAMD)(
    VkCommandBuffer                             commandBuffer,
    const VkAntiLagDataAMD*                     pAntiLagData);

#endif // VK_AMD_anti_lag

// VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD n'existe que dans les SDK recents :
// valeur fixe du registre Vulkan (1001579001-1001579003 pour cette extension).
#ifndef VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD
#define VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD static_cast<VkStructureType>(1001579001)
#endif
#ifndef VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD
#define VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD static_cast<VkStructureType>(1001579002)
#endif

// Fork AMD end
