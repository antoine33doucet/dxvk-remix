// Fork AMD start: implementation FSR 3.1 (upscaling) pour dxvk-remix
// ---------------------------------------------------------------------------
// Modele : rtx_xess.cpp (integration XeSS). Les appels FidelityFX suivent
// l'API documentee du SDK FidelityFX (ffx_fsr3.h) :
//   ffxGetInterfaceVK / ffxFsr3UpscaleCreateContext / ffxFsr3UpscaleDispatch
//   ffxFsr3UpscaleGetJitterOffset / ffxFsr3UpscaleDestroyContext
//
// [TODO-FORK] marque les points a valider au premier build MSVC + Vulkan SDK.
// ---------------------------------------------------------------------------

#include "rtx_fsr.h"

#include "rtx_context.h"
#include "rtx_resources.h"

#include "../dxvk_device.h"
#include "../dxvk_context.h"
#include "../dxvk_image.h"
#include "../dxvk_sampler.h"

// Backend Vulkan de FidelityFX
#include <FidelityFX/host/ffx_vk.h>

namespace dxvk {

  DxvkFsr::DxvkFsr(DxvkDevice* device)
  : RtxPass(device), CommonDeviceObject(device) {
  }

  DxvkFsr::~DxvkFsr() {
    destroy();
  }

  bool DxvkFsr::createBackend() {
    // [TODO-FORK] : verifier la taille requise via ffxGetInterfaceVK
    // (retourne le scratch buffer size necessaire) avant de copier dans
    // m_scratchMemory ; prevoir un heap dynamique si > taille fixee.
    size_t scratchSize = 0;
    m_backendInterface = ffxGetInterfaceVK(
      &scratchSize /* [TODO-FORK] signature exacte selon version SDK : voir ffx_vk.h ] */,
      nullptr /* vkInstance */,
      nullptr /* vkPhysicalDevice */,
      nullptr /* vkDevice  */,
      nullptr /* vkDeviceProcAddr */,
      VK_API_VERSION_1_3,
      nullptr /* memory allocation callbacks */);

    if (m_backendInterface.fpInterfaceDestroy == nullptr) {
      Logger::err("DxvkFsr: failed to create FidelityFX Vulkan backend");
      return false;
    }

    // [TODO-FORK] : FfxDeviceCapabilities — verifier fp16 support RDNA 3
    m_deviceSupportsFP16 = true;
    return true;
  }

  bool DxvkFsr::createUpscaleContext(
    const VkExtent2D&   renderSize,
    const VkExtent2D&   outputSize,
    VkFormat            colorFormat,
    VkFormat            outputFormat) {

    FfxFsr3UpscaleContextDescription fsrDesc = {};
    fsrDesc.flags                = 0; // FFX_FSR3UPSCALE_ENABLE_* : voir spec
    fsrDesc.backendInterface     = m_backendInterface;
    // [TODO-FORK] : fsrDesc.shadersResource — pointer vers le blob de shaders
    // FSR fourni par le SDK (fichier ffx_fsr3_shaders_vk.h inclus directement,
    // ou chargement des binaires .spirv du dossier shaders/ du SDK).
    // XeSS fait l'analogue avec son module embarque (xess/inc/...).

    fsrDesc.maxRenderSize.width  = renderSize.width;
    fsrDesc.maxRenderSize.height = renderSize.height;
    fsrDesc.maxUpscaleSize.width = outputSize.width;
    fsrDesc.maxUpscaleSize.height= outputSize.height;

    // [TODO-FORK] : formats FfxSurfaceFormat correspondants (helper dxvk_format)
    fsrDesc.colorFormat          = FfxSurfaceFormat((uint32_t) -1); // a mapper
    fsrDesc.outputFormat         = FfxSurfaceFormat((uint32_t) -1); // a mapper
    fsrDesc.flags               |= FFX_FSR3UPSCALE_ENABLE_HIGH_RES_MOTION_VECTORS
                                   ? 0 : 0; // optionnel, cf. doc

    FfxErrorCode rc = ffxFsr3UpscaleCreateContext(&m_fsrContext, &fsrDesc);
    if (rc != FFX_OK) {
      Logger::err(str::format("DxvkFsr: ffxFsr3UpscaleCreateContext failed: ", (int) rc));
      return false;
    }

    m_fsrContextCreated = true;
    return true;
  }

  void DxvkFsr::setup(
    const VkExtent2D&  renderSize,
    const VkExtent2D&  outputSize,
    VkFormat           colorFormat,
    VkFormat           outputFormat,
    uint32_t           frameCount) {

    if (!createBackend()) {
      return;
    }
    if (!createUpscaleContext(renderSize, outputSize, colorFormat, outputFormat)) {
      return;
    }
    m_initialized = true;

    Logger::info(str::format("DxvkFsr: FSR 3.1 upscaling initialised (",
      renderSize.width, "x", renderSize.height, " -> ",
      outputSize.width, "x", outputSize.height, ")"));
  }

  void DxvkFsr::dispatch(
    Rc<DxvkContext>     ctx,
    const Rc<DxvkImage>& colorTarget,
    const Rc<DxvkImage>& depthTarget,
    const Rc<DxvkImage>& motionVectors,
    const Rc<DxvkImage>& exposureTarget,
    const Rc<DxvkImage>& outputTarget,
    const Vector2i&      renderSize,
    const Vector2i&      outputSize,
    float                cameraNear,
    float                cameraFar,
    bool                 resetHistory) {

    if (!m_initialized) {
      return;
    }

    // Recuperation du command buffer courant, cf. motif XeSS dans dispatch()
    VkCommandBuffer cmdList = ctx->getCommandList()->handle(); // [TODO-FORK] : valider l'API exacte

    FfxFsr3DispatchDescription dispatchDesc = {};
    dispatchDesc.commandList = cmdList;

    dispatchDesc.renderSize.width  = renderSize.x;
    dispatchDesc.renderSize.height = renderSize.y;
    dispatchDesc.outputSize.width  = outputSize.x;
    dispatchDesc.outputSize.height = outputSize.y;

    // Jitter : sequence propre a FSR (phase count selon resolutions)
    const float phaseCount = ffxFsr3UpscaleGetJitterPhaseCount(
      renderSize.width, outputSize.width);
    const int32_t renderIndex = m_device->getCurrentFrameId();
    const Vector2f jitter = DxvkFsr::getJitterOffset(renderIndex, phaseCount);
    dispatchDesc.jitterOffset.x = jitter.x;
    dispatchDesc.jitterOffset.y = jitter.y;

    // Motion vectors : le runtime Remix genere deja des MV en espace ecran
    // (utilises par DLSS/XeSS) — echelle = resolution des MV, cf. XeSS.
    dispatchDesc.motionVectorScale.x = (float) renderSize.x;
    dispatchDesc.motionVectorScale.y = (float) renderSize.y;
    dispatchDesc.frameTimeDelta      = 1.0f / 60.0f; // [TODO-FORK] : vrai deltatime
    dispatchDesc.cameraNear          = cameraNear;
    dispatchDesc.cameraFar           = cameraFar;
    dispatchDesc.reset               = resetHistory;

    // [TODO-FORK] : encapsuler les VkImage en FfxResource via ffxGetResourceVK
    // (pattern : ffxGetResourceVK(image->handle(), description, name, state))
    dispatchDesc.color         = ffxGetResourceVK(nullptr, {}, "InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.depth         = ffxGetResourceVK(nullptr, {}, "InputDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.motionVectors = ffxGetResourceVK(nullptr, {}, "InputMotionVectors", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.exposure      = ffxGetResourceVK(nullptr, {}, "InputExposure", FFX_RESOURCE_STATE_COMPUTE_READ);
    dispatchDesc.output        = ffxGetResourceVK(nullptr, {}, "OutputUpscaledColor", FFX_RESOURCE_STATE_COMPUTE_WRITE);

    FfxErrorCode rc = ffxFsr3UpscaleDispatch(&m_fsrContext, &dispatchDesc);
    if (rc != FFX_OK) {
      Logger::err(str::format("DxvkFsr: dispatch failed: ", (int) rc));
    }
  }

  Vector2f DxvkFsr::getJitterOffset(int32_t renderIndex, float jitterPhaseCount) {
    double x, y;
    ffxFsr3UpscaleGetJitterOffset(&x, &y, renderIndex, jitterPhaseCount);
    return Vector2f((float) x, (float) y);
  }

  void DxvkFsr::destroy() {
    if (m_fsrContextCreated) {
      ffxFsr3UpscaleDestroyContext(&m_fsrContext);
      m_fsrContextCreated = false;
    }
    if (m_backendInterface.fpInterfaceDestroy != nullptr) {
      m_backendInterface.fpInterfaceDestroy(&m_backendInterface); // [TODO-FORK] : valider
    }
    m_initialized = false;
  }

} // namespace dxvk

// Fork AMD end
