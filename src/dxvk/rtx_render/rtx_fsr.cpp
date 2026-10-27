/*
* Fork AMD: FSR 3 upscaler — implementation via l'API FFX.
*
* Integration FSR 3.1 Upscale pour le runtime dxvk-remix (fork AMD) :
*  - amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.x, MIT, GPUOpen) est chargee
*    dynamiquement (LoadLibrary + ffx_api_loader.h) — aucun .lib a linker,
*    aucune dependance de build. La DLL doit etre posee a cote de d3d9.dll
*    (dossier remix-sdk de l'instance Minecraft).
*  - Modele d'integration : rtx_xess.cpp (DxvkXeSS). Meme cycle de vie
*    (RtxPass), memes entrees (couleur HDR, profondeur, motion vectors,
*    exposition 1x1), memes barrieres.
*  - Conventions verifiees dans le runtime :
*      * couleur d'entree lineaire HDR (le tonemapping a lieu apres l'upscale)
*        -> FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE (cf. rtx_dlss.cpp mIsHDR).
*      * profondeur NON inversee (mInverseDepth = false dans rtx_dlss.cpp).
*      * motion vectors en pixels absolus a la resolution de rendu
*        -> motionVectorScale = {1, 1} (cf. rtx_dlss.cpp).
*      * le monde Remix est deja en metres -> viewSpaceToMetersFactor = 1.
*/

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "rtx.h"
#include "rtx_context.h"
#include "rtx_options.h"
#include "dxvk_device.h"
#include "rtx_fsr.h"
#include "rtx_camera.h"
#include "rtx_auto_exposure.h"
#include "../../util/util_global_time.h"
#include "../util/util_string.h"
#include "../util/log/log.h"

// API FFX vendoree (FidelityFX SDK 1.1.4, MIT)
#include "fsr/inc/ffx_api/ffx_api.h"
#include "fsr/inc/ffx_api/ffx_api_types.h"
#include "fsr/inc/ffx_api/ffx_upscale.h"
#include "fsr/inc/ffx_api/ffx_api_loader.h"
#include "fsr/inc/ffx_api/vk/ffx_api_vk.h"

namespace dxvk {

  namespace {

    // Table de fonctions ffx_api + module, charges une seule fois par process.
    ffxFunctions s_ffx = {};
    HMODULE s_ffxModule = nullptr;
    bool s_ffxLoadAttempted = false;

    // Callback de messages du runtime FFX -> Logger Remix.
    void ffxMessageCallback(uint32_t type, const wchar_t* message) {
      if (message == nullptr) {
        return;
      }

      char buffer[512];
      const int len = WideCharToMultiByte(CP_UTF8, 0, message, -1, buffer, sizeof(buffer) - 1, nullptr, nullptr);
      if (len <= 0) {
        return;
      }
      buffer[len - 1] = '\0';

      if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        Logger::err(str::format("FSR: ", buffer));
      } else if (type == FFX_API_MESSAGE_TYPE_WARNING) {
        Logger::warn(str::format("FSR: ", buffer));
      }
    }

    // Enveloppe une image Remix en FfxApiResource (l'API FFX travaille sur des
    // VkImage entiers, le backend cree ses propres vues).
    FfxApiResource wrapFfxResource(const Rc<DxvkImageView>& view, uint32_t state, bool isDepth) {
      FfxApiResource res = {};
      if (view == nullptr || view->image() == nullptr) {
        return res;
      }

      const DxvkImageCreateInfo& info = view->image()->info();

      res.resource = reinterpret_cast<void*>(view->image()->handle());
      res.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
      res.description.format = ffxApiGetSurfaceFormatVK(info.format);
      res.description.width = info.extent.width;
      res.description.height = info.extent.height;
      res.description.depth = 1;
      res.description.mipCount = 1;
      res.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
      res.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY
        | (isDepth ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : 0u);
      res.state = state;
      return res;
    }

  }

  // ---------------------------------------------------------------------------
  // Chargement dynamique de la DLL
  // ---------------------------------------------------------------------------

  bool DxvkFsr::loadLibrary() {
    if (s_ffxModule != nullptr) {
      return true;
    }
    if (s_ffxLoadAttempted) {
      return false;
    }
    s_ffxLoadAttempted = true;

    // 1) Dossier du module courant (d3d9.dll -> le dossier remix-sdk du jeu)
    HMODULE selfModule = nullptr;
    if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&loadLibrary),
          &selfModule) && selfModule != nullptr) {
      wchar_t path[MAX_PATH];
      if (GetModuleFileNameW(selfModule, path, MAX_PATH) > 0) {
        const std::wstring dir(path);
        const size_t lastSlash = dir.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos) {
          const std::wstring candidate = dir.substr(0, lastSlash + 1) + L"amd_fidelityfx_vk.dll";
          s_ffxModule = LoadLibraryW(candidate.c_str());
        }
      }
    }

    // 2) Recherche standard Windows (dossier courant, PATH...)
    if (s_ffxModule == nullptr) {
      s_ffxModule = LoadLibraryW(L"amd_fidelityfx_vk.dll");
    }

    // 3) Dossier remix-sdk relatif au repertoire de travail de l'instance
    if (s_ffxModule == nullptr) {
      s_ffxModule = LoadLibraryW(L"remix-sdk\\amd_fidelityfx_vk.dll");
    }

    if (s_ffxModule == nullptr) {
      Logger::warn("FSR: amd_fidelityfx_vk.dll introuvable — posez la DLL a cote de d3d9.dll (remix-sdk) depuis le FidelityFX SDK 1.1.4");
      return false;
    }

    ffxLoadFunctions(&s_ffx, s_ffxModule);
    if (s_ffx.CreateContext == nullptr || s_ffx.DestroyContext == nullptr
        || s_ffx.Query == nullptr || s_ffx.Dispatch == nullptr) {
      Logger::warn("FSR: amd_fidelityfx_vk.dll chargee mais les fonctions ffx_api sont absentes (DLL invalide ?)");
      FreeLibrary(s_ffxModule);
      s_ffxModule = nullptr;
      return false;
    }

    Logger::info("FSR: amd_fidelityfx_vk.dll chargee (API FFX)");
    return true;
  }

  bool DxvkFsr::isLibraryAvailable() {
    return loadLibrary();
  }

  // ---------------------------------------------------------------------------
  // Cycle de vie (RtxPass)
  // ---------------------------------------------------------------------------

  DxvkFsr::DxvkFsr(DxvkDevice* device)
    : RtxPass(device),
      CommonDeviceObject(device),
      m_initialized(false),
      m_recreate(true),
      m_useExposureTexture(true),
      m_ffxContext(nullptr),
      m_inputSize{ 0, 0 },
      m_fsrOutputSize{ 0, 0 },
      m_actualPreset(FsrPreset::Balanced) {
  }

  DxvkFsr::~DxvkFsr() {
    destroyFsrContext();
  }

  bool DxvkFsr::isEnabled() const {
    return RtxOptions::isFSREnabled();
  }

  bool DxvkFsr::onActivation(Rc<DxvkContext>& ctx) {
    if (!loadLibrary()) {
      Logger::warn("FSR: activation echouee — amd_fidelityfx_vk.dll introuvable");
      return false;
    }

    m_recreate = true;
    Logger::info("FSR 3 (API FFX) active");
    return true;
  }

  void DxvkFsr::onDeactivation() {
    destroyFsrContext();
    m_initialized = false;
  }

  // ---------------------------------------------------------------------------
  // Creation / destruction du contexte FFX
  // ---------------------------------------------------------------------------

  void DxvkFsr::createFsrContext(const VkExtent3D& targetExtent) {
    if (!loadLibrary() || device() == nullptr) {
      return;
    }

    // Backend Vulkan : device + physicalDevice + proc addr (le backend FFX
    // resolve lui-meme vkCmdBindPipeline etc. et enregistre dans la command
    // buffer fournie au dispatch).
    ffxCreateBackendVKDesc backendDesc = {};
    backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backendDesc.vkDevice = device()->handle();
    backendDesc.vkPhysicalDevice = device()->adapter()->handle();
    backendDesc.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale desc = {};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.header.pNext = &backendDesc.header;
    // Couleur d'entree en HDR lineaire (tonemapping apres l'upscale).
    desc.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE;
    // Sans texture d'exposition fournie, on laisse FSR la calculer.
    if (!m_useExposureTexture) {
      desc.flags |= FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
    }
    desc.maxRenderSize = { targetExtent.width, targetExtent.height };
    desc.maxUpscaleSize = { targetExtent.width, targetExtent.height };
    desc.fpMessage = &ffxMessageCallback;

    const uint32_t ret = s_ffx.CreateContext(&m_ffxContext, &desc.header, nullptr);
    if (ret != FFX_API_RETURN_OK || m_ffxContext == nullptr) {
      Logger::err(str::format("FSR: echec de creation du contexte (code ", ret, ")"));
      m_ffxContext = nullptr;
      return;
    }

    Logger::info(str::format("FSR: contexte cree (cible ",
      targetExtent.width, "x", targetExtent.height, ", entree ",
      m_inputSize.width, "x", m_inputSize.height, ")"));
  }

  void DxvkFsr::destroyFsrContext() {
    if (m_ffxContext != nullptr) {
      if (s_ffx.DestroyContext != nullptr) {
        s_ffx.DestroyContext(&m_ffxContext, nullptr);
      }
      m_ffxContext = nullptr;
    }
  }

  void DxvkFsr::initialize(Rc<DxvkContext> renderContext, const VkExtent3D& targetExtent) {
    if (!isActive()) {
      return;
    }

    const FsrPreset preset = RtxOptions::fsrPreset();
    const bool useExposure = device()->getCommon()->metaAutoExposure().enabled();

    if (m_initialized
        && m_fsrOutputSize.width == targetExtent.width
        && m_fsrOutputSize.height == targetExtent.height
        && m_actualPreset == preset
        && m_useExposureTexture == useExposure) {
      return; // Rien n'a change
    }

    destroyFsrContext();

    m_actualPreset = preset;
    m_useExposureTexture = useExposure;
    m_fsrOutputSize.width = targetExtent.width;
    m_fsrOutputSize.height = targetExtent.height;

    createFsrContext(targetExtent);
    m_initialized = (m_ffxContext != nullptr);
  }

  // ---------------------------------------------------------------------------
  // Taille de rendu par preset (appele par setDownscaleExtent)
  // ---------------------------------------------------------------------------

  void DxvkFsr::setSetting(const uint32_t displaySize[2], const FsrPreset preset, uint32_t outRenderSize[2]) {
    // Ratios par dimension FSR 3.1 :
    //   NativeAA 1.0 / Quality 1.5 / Balanced 1.7 / Performance 2.0 / UltraPerf 3.0
    // UltraQuality (1.3x) est une echelle maison du fork (cf. updateUpscalerFromFsrPreset).
    float scale = 1.0f / 1.7f;
    uint32_t qualityMode = FFX_UPSCALE_QUALITY_MODE_BALANCED;

    switch (preset) {
      case FsrPreset::UltraPerformance:
        scale = 1.0f / 3.0f;
        qualityMode = FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE;
        break;
      case FsrPreset::Performance:
        scale = 1.0f / 2.0f;
        qualityMode = FFX_UPSCALE_QUALITY_MODE_PERFORMANCE;
        break;
      case FsrPreset::Quality:
        scale = 1.0f / 1.5f;
        qualityMode = FFX_UPSCALE_QUALITY_MODE_QUALITY;
        break;
      case FsrPreset::UltraQuality:
        scale = 1.0f / 1.3f;
        qualityMode = FFX_UPSCALE_QUALITY_MODE_QUALITY;
        break;
      case FsrPreset::Balanced:
      default:
        scale = 1.0f / 1.7f;
        qualityMode = FFX_UPSCALE_QUALITY_MODE_BALANCED;
        break;
    }

    // Valeur par defaut (utilisee aussi si la DLL n'est pas encore chargee)
    uint32_t renderWidth = std::max(1u, static_cast<uint32_t>(std::roundf(displaySize[0] * scale)));
    uint32_t renderHeight = std::max(1u, static_cast<uint32_t>(std::roundf(displaySize[1] * scale)));

    // Taille de rendu recommandee par le SDK quand il est disponible
    if (preset != FsrPreset::UltraQuality && s_ffxModule != nullptr && s_ffx.Query != nullptr) {
      ffxQueryDescUpscaleGetRenderResolutionFromQualityMode query = {};
      query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
      query.displayWidth = displaySize[0];
      query.displayHeight = displaySize[1];
      query.qualityMode = qualityMode;
      query.pOutRenderWidth = &renderWidth;
      query.pOutRenderHeight = &renderHeight;

      if (s_ffx.Query(nullptr, &query.header) != FFX_API_RETURN_OK) {
        // On garde les valeurs calculees ci-dessus
        Logger::warn("FSR: requete de resolution de rendu echouee, echelle forfaitaire utilisee");
      }
    }

    if (m_actualPreset != preset
        || m_fsrOutputSize.width != displaySize[0]
        || m_fsrOutputSize.height != displaySize[1]
        || m_inputSize.width != renderWidth
        || m_inputSize.height != renderHeight) {
      m_actualPreset = preset;
      m_fsrOutputSize.width = displaySize[0];
      m_fsrOutputSize.height = displaySize[1];
      m_recreate = true; // Forcera la re-creation du contexte au dispatch
    }

    m_inputSize.width = renderWidth;
    m_inputSize.height = renderHeight;
    outRenderSize[0] = renderWidth;
    outRenderSize[1] = renderHeight;
  }

  void DxvkFsr::getInputSize(uint32_t& width, uint32_t& height) const {
    width = m_inputSize.width;
    height = m_inputSize.height;
  }

  // ---------------------------------------------------------------------------
  // Dispatch (modele rtx_xess.cpp)
  // ---------------------------------------------------------------------------

  void DxvkFsr::dispatch(
    Rc<DxvkContext> renderContext,
    DxvkBarrierSet& barriers,
    const Resources::RaytracingOutput& rtOutput,
    bool resetHistory) {

    auto copyFallback = [&]() {
      renderContext->copyImage(
        rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image,
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutput.image(Resources::AccessType::Read),
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { 0, 0, 0 },
        rtOutput.m_compositeOutputExtent);
    };

    if (!isActive()) {
      copyFallback();
      return;
    }

    if (m_recreate || !m_initialized) {
      VkExtent3D targetExtent;
      if (m_fsrOutputSize.width > 0 && m_fsrOutputSize.height > 0) {
        targetExtent = { m_fsrOutputSize.width, m_fsrOutputSize.height, 1 };
      } else {
        targetExtent = {
          rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image->info().extent.width,
          rtOutput.m_finalOutput.resource(Resources::AccessType::Write).image->info().extent.height,
          1
        };
      }
      initialize(renderContext, targetExtent);
      m_recreate = false;
    }

    if (!m_initialized || m_ffxContext == nullptr) {
      copyFallback();
      return;
    }

    // Barriers d'entree/sortie (identiques au chemin XeSS)
    std::array<Rc<DxvkImageView>, 4> inputs = {
      rtOutput.m_compositeOutput.view(Resources::AccessType::Read),
      rtOutput.m_primaryScreenSpaceMotionVector.view,
      rtOutput.m_primaryDepth.view,
      nullptr // emplacement pour la texture d'exposition
    };

    auto& autoExposure = device()->getCommon()->metaAutoExposure();
    const bool hasExposureTexture = m_useExposureTexture
      && autoExposure.enabled()
      && autoExposure.getExposureTexture().image != nullptr;
    if (hasExposureTexture) {
      inputs[3] = autoExposure.getExposureTexture().view;
    }

    std::array<Rc<DxvkImageView>, 1> outputs = {
      rtOutput.m_finalOutput.view(Resources::AccessType::Write)
    };

    for (auto input : inputs) {
      if (input == nullptr) {
        continue;
      }

      barriers.accessImage(
        input->image(),
        input->imageSubresources(),
        input->imageInfo().layout,
        input->imageInfo().stages,
        input->imageInfo().access,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_READ_BIT);
    }

    for (auto output : outputs) {
      barriers.accessImage(
        output->image(),
        output->imageSubresources(),
        output->imageInfo().layout,
        output->imageInfo().stages,
        output->imageInfo().access,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT);
    }

    barriers.recordCommands(renderContext->getCommandList());

    // Camera : jitter + parametres de projection pour FSR
    auto& sceneManager = device()->getCommon()->getSceneManager();
    RtCamera& camera = sceneManager.getCamera();
    float jitterOffset[2];
    camera.getJittering(jitterOffset);
    jitterOffset[0] = std::clamp(jitterOffset[0], -0.5f, 0.5f);
    jitterOffset[1] = std::clamp(jitterOffset[1], -0.5f, 0.5f);

    ffxDispatchDescUpscale desc = {};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = static_cast<void*>(
      renderContext->getCommandList()->getCmdBuffer(DxvkCmdBuffer::ExecBuffer));

    desc.color = wrapFfxResource(
      rtOutput.m_compositeOutput.view(Resources::AccessType::Read),
      FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    desc.depth = wrapFfxResource(
      rtOutput.m_primaryDepth.view,
      FFX_API_RESOURCE_STATE_COMPUTE_READ, true);
    desc.motionVectors = wrapFfxResource(
      rtOutput.m_primaryScreenSpaceMotionVector.view,
      FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    desc.exposure = {};
    if (hasExposureTexture) {
      desc.exposure = wrapFfxResource(
        autoExposure.getExposureTexture().view,
        FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    }
    desc.output = wrapFfxResource(
      rtOutput.m_finalOutput.view(Resources::AccessType::Write),
      FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, false);

    desc.jitterOffset = { jitterOffset[0], jitterOffset[1] };
    // Motion vectors Remix en pixels absolus (cf. rtx_dlss.cpp)
    desc.motionVectorScale = { 1.0f, 1.0f };
    desc.renderSize = { m_inputSize.width, m_inputSize.height };
    desc.upscaleSize = { m_fsrOutputSize.width, m_fsrOutputSize.height };
    desc.enableSharpening = RtxOptions::rcasSharpening();
    desc.sharpness = RtxOptions::rcasSharpness();
    desc.frameTimeDelta = std::max(0.1f, GlobalTime::get().deltaTimeMs());
    desc.preExposure = 1.0f;
    desc.reset = resetHistory;
    desc.cameraNear = camera.getNearPlane();
    desc.cameraFar = camera.getFarPlane();
    desc.cameraFovAngleVertical = camera.getFov();
    // Le monde Remix est deja en metres (sceneScale applique a la scene)
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.flags = 0;

    const uint32_t ret = s_ffx.Dispatch(m_ffxContext, &desc.header);
    if (ret != FFX_API_RETURN_OK) {
      Logger::warn(str::format("FSR: echec du dispatch (code ", ret, ")"));
      copyFallback();
    }

    // Restauration des barrieres de sortie (modele XeSS)
    for (auto output : outputs) {
      barriers.accessImage(
        output->image(),
        output->imageSubresources(),
        VK_IMAGE_LAYOUT_GENERAL,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,
        output->imageInfo().layout,
        output->imageInfo().stages,
        output->imageInfo().access);

      renderContext->getCommandList()->trackResource<DxvkAccess::None>(output);
      renderContext->getCommandList()->trackResource<DxvkAccess::Write>(output->image());
    }

    barriers.recordCommands(renderContext->getCommandList());
  }

}
