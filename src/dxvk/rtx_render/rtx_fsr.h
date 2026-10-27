/*
* Fork AMD: FSR 3 upscaler — en-tete.
*
* Integration FSR 3.1 Upscale via l'API FFX (amd_fidelityfx_vk.dll, FidelityFX
* SDK 1.1.x, MIT) chargee dynamiquement au cote de d3d9.dll.
* Modele d'integration : DxvkXeSS (rtx_xess.h).
*
* SPDX license des headers vendores : MIT (AMD) — voir
* src/dxvk/fsr/inc/ffx_api/ffx_api.h.
*/
#pragma once

#include "rtx_resources.h"
#include "rtx_options.h"
#include "rtx_common_object.h"

#include "../dxvk_format.h"
#include "../dxvk_include.h"
#include "../util/rc/util_rc.h"
#include "../util/rc/util_rc_ptr.h"

namespace dxvk {

  class DxvkDevice;

  /**
   * \brief FSR 3 upscaler (fork AMD)
   *
   * Upscaler FSR 3.1 branche sur le pipeline Remix a la place de DLSS/XeSS
   * quand rtx.upscalerType = 5 (UpscalerType::FSR).
   * Necessite amd_fidelityfx_vk.dll posee a cote de d3d9.dll (remix-sdk).
   */
  class DxvkFsr : public RtxPass, public CommonDeviceObject {

  public:

    // Constructor with device
    DxvkFsr(DxvkDevice* device);

    ~DxvkFsr();

    void dispatch(
      Rc<DxvkContext> renderContext,
      DxvkBarrierSet& barriers,
      const Resources::RaytracingOutput& rtOutput,
      bool resetHistory);

    // Calcule la taille de rendu optimale pour le preset FSR courant
    // (appele depuis RtxContext::setDownscaleExtent, comme XeSS::setSetting).
    void setSetting(const uint32_t displaySize[2], const FsrPreset preset, uint32_t outRenderSize[2]);

    void getInputSize(uint32_t& width, uint32_t& height) const;

    // True si amd_fidelityfx_vk.dll a pu etre chargee (une tentative par process).
    static bool isLibraryAvailable();

  protected:

    // RtxPass interface
    virtual bool isEnabled() const override;
    virtual bool onActivation(Rc<DxvkContext>& ctx) override;
    virtual void onDeactivation() override;

  private:

    // Chargement dynamique de amd_fidelityfx_vk.dll (une seule fois par process)
    static bool loadLibrary();

    void initialize(Rc<DxvkContext> renderContext, const VkExtent3D& targetExtent);
    void createFsrContext(const VkExtent3D& targetExtent);
    void destroyFsrContext();

    bool m_initialized;
    bool m_recreate;
    bool m_useExposureTexture;

    void* m_ffxContext; // ffxContext (API FFX)

    VkExtent2D m_inputSize;
    VkExtent2D m_fsrOutputSize;
    FsrPreset m_actualPreset;
  };

}
