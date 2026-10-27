#pragma once

// Fork AMD start: interface VK_AMD_anti_lag
// ---------------------------------------------------------------------------
// Copie de secours des definitions VK_AMD_anti_lag (registre officiel Khronos).
// Le submodule include/vulkan pinned par dxvk-remix (Vulkan-Headers 1.4.309)
// definit deja tous ces types dans vulkan_core.h ; le garde ci-dessous evite
// toute collision. Sur un SDK plus ancien, ce header fournit l'interface.
// ATTENTION: l'API reelle est vkAntiLagUpdateAMD, appelee au niveau device
// (pas une commande enregistree dans une command buffer).
// ---------------------------------------------------------------------------

#include "../dxvk_include.h"

#ifndef VK_AMD_anti_lag
#define VK_AMD_anti_lag 1

#define VK_AMD_ANTI_LAG_SPEC_VERSION   1
#define VK_AMD_ANTI_LAG_EXTENSION_NAME "VK_AMD_anti_lag"

typedef enum VkAntiLagModeAMD {
    VK_ANTI_LAG_MODE_DRIVER_CONTROL_AMD = 0,
    VK_ANTI_LAG_MODE_ON_AMD             = 1,
    VK_ANTI_LAG_MODE_OFF_AMD            = 2,
    VK_ANTI_LAG_MODE_MAX_ENUM_AMD       = 0x7FFFFFFF
} VkAntiLagModeAMD;

typedef enum VkAntiLagStageAMD {
    VK_ANTI_LAG_STAGE_INPUT_AMD    = 0,
    VK_ANTI_LAG_STAGE_PRESENT_AMD  = 1,
    VK_ANTI_LAG_STAGE_MAX_ENUM_AMD = 0x7FFFFFFF
} VkAntiLagStageAMD;

typedef struct VkAntiLagPresentationInfoAMD {
    VkStructureType      sType;
    void*                pNext;
    VkAntiLagStageAMD    stage;
    uint64_t             frameIndex;
} VkAntiLagPresentationInfoAMD;

typedef struct VkAntiLagDataAMD {
    VkStructureType                        sType;
    const void*                            pNext;
    VkAntiLagModeAMD                       mode;
    uint32_t                               maxFPS;
    const VkAntiLagPresentationInfoAMD*    pPresentationInfo;
} VkAntiLagDataAMD;

typedef void (VKAPI_PTR *PFN_vkAntiLagUpdateAMD)(
    VkDevice                                    device,
    const VkAntiLagDataAMD*                     pData);

#endif // VK_AMD_anti_lag

// Valeurs sType du registre Vulkan (extension VK_AMD_anti_lag, enumOffset 476).
// Un SDK recent les definit comme constantes d'enum dans vulkan_core.h ; le
// garde macro reste sans effet nefaste car les valeurs sont identiques a celles
// du registre. NB: un #ifndef ne peut pas detecter une constante d'enum, ces
// macros s'appliquent donc toujours — c'est voulu et inoffensif ici.
#ifndef VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD
#define VK_STRUCTURE_TYPE_ANTI_LAG_DATA_AMD static_cast<VkStructureType>(1000476001)
#endif
#ifndef VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD
#define VK_STRUCTURE_TYPE_ANTI_LAG_PRESENTATION_INFO_AMD static_cast<VkStructureType>(1000476002)
#endif

// Fork AMD end