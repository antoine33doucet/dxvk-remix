#pragma once

// Fork AMD start: integrale FSR 3.1 (upscaling) dans dxvk-remix
// ---------------------------------------------------------------------------
// Modele architectural : rtx_xess.h / rtx_xess.cpp (DxvkXeSS), car XeSS est
// lui aussi un upscaler tiers vendor-neutral embarque dans le runtime.
//
// DEPENDANCE : FidelityFX SDK (AMD, MIT) >= 1.1
//   https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
//   Headers requis : FidelityFX/host/ffx_fsr3.h, ffx_types.h, ffx_vk.h
//   Les shaders FSR precompiles sont fournis par le SDK (repertoire shaders/).
//
// ETAT : squelette d'integration complet a valider sous Windows/MSVC.
// Les zones marquees [TODO-FORK] exigent un passage de compilation + tests.
// ---------------------------------------------------------------------------

#include "rtx_resources.h"
#include "rtx_options.h"
#include "rtx_common_object.h"
#include "../dxvk_format.h"
#include "../dxvk_include.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/ffx_interface.h>

namespace dxvk {

  class DxvkDevice;
  class DxvkContext;

  class DxvkFsr : public RtxPass, public CommonDeviceObject {
  public:
    struct FsrOptions {
      friend class DxvkFsr;
      friend class ImGUI;
      // Le preset expose est RtxOptions::fsrPreset (cle rtx.fsrPreset),
      // declare dans rtx_options.h — voir la pour les valeurs.
    };

    DxvkFsr(DxvkDevice* device);
    virtual ~DxvkFsr();

    void setup(
      const VkExtent2D&            renderSize,
      const VkExtent2D&            outputSize,
      VkFormat                     colorFormat,
      VkFormat                     outputFormat,
      uint32_t                     frameCount);

    void dispatch(
      Rc<DxvkContext>              ctx,
      const Rc<DxvkImage>&         colorTarget,
      const Rc<DxvkImage>&         depthTarget,
      const Rc<DxvkImage>&         motionVectors,
      const Rc<DxvkImage>&         exposureTarget,
      const Rc<DxvkImage>&         outputTarget,
      const Vector2i&              renderSize,
      const Vector2i&              outputSize,
      float                        cameraNear,
      float                        cameraFar,
      bool                         resetHistory);

    // Jitter FSR (sequence propre a FSR, cf. ffxFsr3UpscaleGetJitterOffset)
    static Vector2f getJitterOffset(int32_t renderIndex, float jitterPhaseCount);

    void destroy();

    inline bool isInitialized() const { return m_initialized; }

  private:
    bool createBackend();
    bool createUpscaleContext(
      const VkExtent2D&            renderSize,
      const VkExtent2D&            outputSize,
      VkFormat                     colorFormat,
      VkFormat                     outputFormat);

    bool                          m_initialized = false;
    bool                          m_deviceSupportsFP16 = true;

    // Scratch memory pour le backend VK de FSR (allocation de FfxInterface)
    // [TODO-FORK] : allouer via l'allocateur dxvk, cf. backendInterface.getInterfaceDesc
    char                          m_scratchMemory[1024 * 1024];

    FfxInterface                  m_backendInterface = {};
    FfxFsr3UpscaleContext         m_fsrContext = {};
    bool                          m_fsrContextCreated = false;

    FsrOptions                    m_options;
  };

} // namespace dxvk

// Fork AMD end
