// Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma
// de Barcelona (UAB).
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.

#include "Carla/Settings/CarlaSettingsDelegate.h"
#include "Carla.h"
#include "Carla/Game/CarlaGameInstance.h"
#include "Carla/Settings/CarlaSettings.h"

#include <util/ue-header-guard-begin.h>
#include "Game/CarlaGameInstance.h"
#include "Async/Async.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/StaticMesh.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/HUD.h"
#include "InstancedFoliageActor.h"
#include "Kismet/GameplayStatics.h"
#include "Landscape.h"
#include "Scalability.h"
#include <util/ue-header-guard-end.h>

static constexpr float CARLA_SETTINGS_MAX_SCALE_SIZE = 50.0f;

/// quality settings configuration between runs
EQualityLevel UCarlaSettingsDelegate::AppliedLowPostResetQualityLevel = EQualityLevel::Epic;

UCarlaSettingsDelegate::UCarlaSettingsDelegate()
  : ActorSpawnedDelegate(FOnActorSpawned::FDelegate::CreateUObject(
        this,
        &UCarlaSettingsDelegate::OnActorSpawned)) {}

void UCarlaSettingsDelegate::Reset()
{
  AppliedLowPostResetQualityLevel = EQualityLevel::Epic;
}

void UCarlaSettingsDelegate::RegisterSpawnHandler(UWorld *InWorld)
{
  CheckCarlaSettings(InWorld);
  InWorld->AddOnActorSpawnedHandler(ActorSpawnedDelegate);
}

void UCarlaSettingsDelegate::OnActorSpawned(AActor *InActor)
{
  check(CarlaSettings != nullptr);
  if (IsValid(InActor) &&
      !InActor->IsA<AInstancedFoliageActor>() && // foliage culling is
                                                 // controlled per instance
      !InActor->IsA<ALandscape>() && // dont touch landscapes nor roads
      !InActor->ActorHasTag(UCarlaSettings::CARLA_ROAD_TAG) &&
      !InActor->ActorHasTag(UCarlaSettings::CARLA_SKY_TAG))
  {
    TArray<UPrimitiveComponent *> components;
    InActor->GetComponents(components);
    switch (CarlaSettings->GetQualityLevel())
    {
      case EQualityLevel::Low: {
        // apply settings for this actor for the current quality level
        float dist = CarlaSettings->LowStaticMeshMaxDrawDistance;
        const float maxscale = InActor->GetActorScale().GetMax();
        if (maxscale > CARLA_SETTINGS_MAX_SCALE_SIZE)
        {
          dist *= 100.0f;
        }
        SetActorComponentsDrawDistance(InActor, dist);
        break;
      }
      default: break;
    }
  }
}

void UCarlaSettingsDelegate::ApplyQualityLevelPostRestart()
{
  CheckCarlaSettings(nullptr);
  UWorld *InWorld = CarlaSettings->GetWorld();

  const EQualityLevel QualityLevel = CarlaSettings->GetQualityLevel();

  if (AppliedLowPostResetQualityLevel == QualityLevel)
  {
    return;
  }

  // enable temporal changes of quality (prevent saving last quality settings to file)
  Scalability::ToggleTemporaryQualityLevels(true);

  switch (QualityLevel)
  {
    case EQualityLevel::Low:
    {
      LaunchLowQualityCommands(InWorld);
      SetAllRoads(InWorld, CarlaSettings->LowRoadPieceMeshMaxDrawDistance, CarlaSettings->LowRoadMaterials);
      ApplyPerActorQualitySettings(
          InWorld,
          CarlaSettings->LowLightFadeDistance,
          false,
          true,
          CarlaSettings->LowStaticMeshMaxDrawDistance);
      SetPostProcessEffectsEnabled(InWorld, false);
      break;
    }
    case EQualityLevel::Medium:
    {
      LaunchMediumQualityCommands(InWorld);
      SetAllRoads(InWorld, 0, CarlaSettings->EpicRoadMaterials);
      ApplyPerActorQualitySettings(InWorld, 0.0f, true, false, 0);
      SetPostProcessEffectsEnabled(InWorld, true);
      break;
    }
    case EQualityLevel::High:
    {
      LaunchHighQualityCommands(InWorld);
      SetAllRoads(InWorld, 0, CarlaSettings->EpicRoadMaterials);
      ApplyPerActorQualitySettings(InWorld, 0.0f, true, false, 0);
      SetPostProcessEffectsEnabled(InWorld, true);
      break;
    }
    default:
      UE_LOG(LogCarla, Warning, TEXT("Unknown quality level, falling back to default."));
    case EQualityLevel::Epic:
    {
      LaunchEpicQualityCommands(InWorld);
      SetAllRoads(InWorld, 0, CarlaSettings->EpicRoadMaterials);
      ApplyPerActorQualitySettings(InWorld, 0.0f, true, false, 0);
      SetPostProcessEffectsEnabled(InWorld, true);
      break;
    }
  }
  AppliedLowPostResetQualityLevel = QualityLevel;
}

void UCarlaSettingsDelegate::ApplyQualityLevelPreRestart()
{
  CheckCarlaSettings(nullptr);
  UWorld *InWorld = CarlaSettings->GetWorld();
  if (!IsValid(InWorld))
  {
    return;
  }
  // enable or disable world and hud rendering
  APlayerController *playercontroller = UGameplayStatics::GetPlayerController(InWorld, 0);
  if (playercontroller)
  {
    ULocalPlayer *player = playercontroller->GetLocalPlayer();
    if (player)
      player->ViewportClient->bDisableWorldRendering = CarlaSettings->bDisableRendering;
    // if we already have a hud class:
    AHUD *hud = playercontroller->GetHUD();
    if (hud)
    {
      hud->bShowHUD = !CarlaSettings->bDisableRendering;
    }
  }

}

UWorld *UCarlaSettingsDelegate::GetLocalWorld()
{
  return GEngine->GetWorldFromContextObjectChecked(this);
}

void UCarlaSettingsDelegate::CheckCarlaSettings(UWorld *world)
{
  if (IsValid(CarlaSettings))
  {
    return;
  }
  if (!IsValid(world))
  {
    world = GetLocalWorld();
  }
  check(world != nullptr);
  auto GameInstance  = Cast<UCarlaGameInstance>(world->GetGameInstance());
  check(GameInstance != nullptr);
  CarlaSettings = &GameInstance->GetCarlaSettings();
  check(CarlaSettings != nullptr);
}

void UCarlaSettingsDelegate::LaunchLowQualityCommands(UWorld *world) const
{
  if (!world)
  {
    return;
  }

  // --- UE5 rendering features: disable the expensive ones ---------------
  // Disable Lumen GI, Lumen/RT reflections and HW ray tracing entirely.
  GEngine->Exec(world, TEXT("r.DynamicGlobalIlluminationMethod 0"));
  GEngine->Exec(world, TEXT("r.ReflectionMethod 2"));
  GEngine->Exec(world, TEXT("r.Lumen.DiffuseIndirect.Allow 0"));
  GEngine->Exec(world, TEXT("r.Lumen.Reflections.Allow 0"));
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing 0"));
  // r.RayTracing is ECVF_ReadOnly and is pinned True by DefaultEngine.ini so
  // the Epic tier can enable HW-RT without a restart. Runtime flips are
  // silent no-ops. Use ForceAllRayTracingEffects 0 to suppress every RT
  // effect so this tier behaves as if RT were off.
  GEngine->Exec(world, TEXT("r.RayTracing.ForceAllRayTracingEffects 0"));
  GEngine->Exec(world, TEXT("r.RayTracing.Shadows 0"));
  // Virtual Shadow Maps off (heavy VRAM cost on their cache).
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.Enable 0"));
  // Nanite stays on but with the smallest streaming pool we dare.
  GEngine->Exec(world, TEXT("r.Nanite.Streaming.PoolSize 128"));
  // Use TAA (cheaper than TSR at low tier).
  GEngine->Exec(world, TEXT("r.AntiAliasingMethod 2"));

  // --- Scalability group buckets ---------------------------------------
  GEngine->Exec(world, TEXT("sg.ResolutionQuality 50"));
  GEngine->Exec(world, TEXT("sg.ViewDistanceQuality 0"));
  GEngine->Exec(world, TEXT("sg.AntiAliasingQuality 0"));
  GEngine->Exec(world, TEXT("sg.ShadowQuality 0"));
  GEngine->Exec(world, TEXT("sg.GlobalIlluminationQuality 0"));
  GEngine->Exec(world, TEXT("sg.ReflectionQuality 0"));
  GEngine->Exec(world, TEXT("sg.PostProcessQuality 0"));
  GEngine->Exec(world, TEXT("sg.TextureQuality 0"));
  GEngine->Exec(world, TEXT("sg.EffectsQuality 0"));
  GEngine->Exec(world, TEXT("sg.FoliageQuality 0"));
  GEngine->Exec(world, TEXT("sg.ShadingQuality 0"));

  // --- Streaming + misc legacy CVars kept for parity -------------------
  GEngine->Exec(world, TEXT("r.Streaming.PoolSize 2000"));
  GEngine->Exec(world, TEXT("r.Streaming.LimitPoolSizeToVRAM 1"));
  GEngine->Exec(world, TEXT("r.DefaultFeature.MotionBlur 0"));
  GEngine->Exec(world, TEXT("r.DefaultFeature.Bloom 0"));
  GEngine->Exec(world, TEXT("r.DefaultFeature.AmbientOcclusion 0"));
  GEngine->Exec(world, TEXT("r.DefaultFeature.AmbientOcclusionStaticFraction 0"));
  GEngine->Exec(world, TEXT("r.DefaultFeature.AutoExposure 1"));
  GEngine->Exec(world, TEXT("foliage.DensityScale 0"));
  GEngine->Exec(world, TEXT("grass.DensityScale 0"));
}

void UCarlaSettingsDelegate::LaunchMediumQualityCommands(UWorld *world) const
{
  if (!world)
  {
    return;
  }

  // Medium sits between Low and High in the ladder reshuffle: Lumen SW-RT on
  // (so cars pick up real reflections and GI), VSM enabled with a moderate
  // page pool, and a mid-weight skin cache / TSR history. HW-RT stays off.
  GEngine->Exec(world, TEXT("r.DynamicGlobalIlluminationMethod 1"));
  GEngine->Exec(world, TEXT("r.ReflectionMethod 1"));
  GEngine->Exec(world, TEXT("r.Lumen.DiffuseIndirect.Allow 1"));
  GEngine->Exec(world, TEXT("r.Lumen.Reflections.Allow 1"));
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing 0"));
  // Force every RT effect off even though the RT subsystem is live (see Low
  // comment). Epic's launch commands flip this back to -1 (use per-effect
  // CVars).
  GEngine->Exec(world, TEXT("r.RayTracing.ForceAllRayTracingEffects 0"));
  GEngine->Exec(world, TEXT("r.RayTracing.Shadows 0"));
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.Enable 1"));
  // Mid-sized VSM page pool: 4096 pages (~256 MB) is enough for a single
  // ego + short-range cascades without blowing the budget.
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.MaxPhysicalPages 4096"));
  GEngine->Exec(world, TEXT("r.Shadow.DistanceScale 1.5"));
  // Mid skin cache: 512 MB keeps the common NPC + ego vehicles fully cached
  // for SW-Lumen card tracing without over-committing.
  GEngine->Exec(world, TEXT("r.SkinCache.SceneMemoryLimitInMB 512"));
  // TSR 150% history tames the worst road-paint shimmer without paying the
  // full 200% history cost that High/Epic take.
  GEngine->Exec(world, TEXT("r.TSR.History.ScreenPercentage 150"));
  GEngine->Exec(world, TEXT("r.MaxAnisotropy 4"));
  GEngine->Exec(world, TEXT("r.Nanite.Streaming.PoolSize 256"));
  GEngine->Exec(world, TEXT("r.AntiAliasingMethod 4")); // TSR

  // --- Scalability group buckets ---------------------------------------
  // Medium now maps to the sg.*Quality 2 buckets so it inherits the SW-Lumen
  // + VSM configs from DefaultScalability.ini's High bucket that the old
  // High tier previously used.
  GEngine->Exec(world, TEXT("sg.ResolutionQuality 100"));
  GEngine->Exec(world, TEXT("sg.ViewDistanceQuality 2"));
  GEngine->Exec(world, TEXT("sg.AntiAliasingQuality 2"));
  GEngine->Exec(world, TEXT("sg.ShadowQuality 2"));
  GEngine->Exec(world, TEXT("sg.GlobalIlluminationQuality 2"));
  GEngine->Exec(world, TEXT("sg.ReflectionQuality 2"));
  GEngine->Exec(world, TEXT("sg.PostProcessQuality 2"));
  GEngine->Exec(world, TEXT("sg.TextureQuality 2"));
  GEngine->Exec(world, TEXT("sg.EffectsQuality 2"));
  GEngine->Exec(world, TEXT("sg.FoliageQuality 2"));
  GEngine->Exec(world, TEXT("sg.ShadingQuality 2"));

  GEngine->Exec(world, TEXT("r.Streaming.PoolSize 3000"));
  GEngine->Exec(world, TEXT("r.Streaming.LimitPoolSizeToVRAM 1"));
  GEngine->Exec(world, TEXT("foliage.DensityScale 1"));
  GEngine->Exec(world, TEXT("grass.DensityScale 1"));
}

void UCarlaSettingsDelegate::LaunchHighQualityCommands(UWorld *world) const
{
  if (!world)
  {
    return;
  }

  // High now inherits what used to be Epic's SW-Lumen configuration: maxed
  // Lumen SW reflections + GI, VSM page pool doubled, long directional-light
  // shadow distance, full skin cache residency, wide TSR history. HW-RT
  // stays off so the VRAM envelope matches the previous post-refactor
  // baseline (~9.5 GB windowed with 30 cars).
  GEngine->Exec(world, TEXT("r.DynamicGlobalIlluminationMethod 1"));
  GEngine->Exec(world, TEXT("r.ReflectionMethod 1"));
  GEngine->Exec(world, TEXT("r.Lumen.DiffuseIndirect.Allow 1"));
  GEngine->Exec(world, TEXT("r.Lumen.Reflections.Allow 1"));
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing 0"));
  // Keep RT subsystem initialized but every effect suppressed on High.
  GEngine->Exec(world, TEXT("r.RayTracing.ForceAllRayTracingEffects 0"));
  GEngine->Exec(world, TEXT("r.RayTracing.Shadows 0"));
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.Enable 1"));
  // 8192 VSM pages (~512 MB pool) so distant cascades hold up under the
  // 1080p multi-camera workloads the team runs (was the main reason tree
  // and traffic-light shadows only loaded within a few metres of the ego).
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.MaxPhysicalPages 8192"));
  // 2x directional-light shadow distance keeps far shadows present.
  GEngine->Exec(world, TEXT("r.Shadow.DistanceScale 2"));
  // Full skin cache: large vehicles need ~768 MB to stay resident so Lumen
  // card tracing + (on Epic) HW-RT BVH skinning don't drop them to matte.
  GEngine->Exec(world, TEXT("r.SkinCache.SceneMemoryLimitInMB 768"));
  // Widen the TSR history to reduce high-contrast shimmer on far road
  // paint (crosswalk stripes, lane markings).
  GEngine->Exec(world, TEXT("r.TSR.History.ScreenPercentage 200"));
  // Pin Lumen surface card atlas at the UE5.5 default. Explicit to protect
  // against future default drops on lower-spec presets.
  GEngine->Exec(world, TEXT("r.LumenScene.SurfaceCache.AtlasSize 4096"));
  GEngine->Exec(world, TEXT("r.MaxAnisotropy 8"));
  GEngine->Exec(world, TEXT("r.Nanite.Streaming.PoolSize 384"));
  GEngine->Exec(world, TEXT("r.AntiAliasingMethod 4")); // TSR

  // --- Scalability group buckets ---------------------------------------
  // High tier now pulls the sg.*Quality 3 buckets that used to belong to
  // Epic. The Epic launch commands below layer HW-RT on top of this same
  // SW-Lumen config.
  GEngine->Exec(world, TEXT("sg.ResolutionQuality 100"));
  GEngine->Exec(world, TEXT("sg.ViewDistanceQuality 3"));
  GEngine->Exec(world, TEXT("sg.AntiAliasingQuality 3"));
  GEngine->Exec(world, TEXT("sg.ShadowQuality 3"));
  GEngine->Exec(world, TEXT("sg.GlobalIlluminationQuality 3"));
  GEngine->Exec(world, TEXT("sg.ReflectionQuality 3"));
  GEngine->Exec(world, TEXT("sg.PostProcessQuality 3"));
  GEngine->Exec(world, TEXT("sg.TextureQuality 3"));
  GEngine->Exec(world, TEXT("sg.EffectsQuality 3"));
  GEngine->Exec(world, TEXT("sg.FoliageQuality 3"));
  GEngine->Exec(world, TEXT("sg.ShadingQuality 3"));

  GEngine->Exec(world, TEXT("r.Streaming.PoolSize 4000"));
  GEngine->Exec(world, TEXT("r.Streaming.LimitPoolSizeToVRAM 1"));
  GEngine->Exec(world, TEXT("foliage.DensityScale 1"));
  GEngine->Exec(world, TEXT("grass.DensityScale 1"));
}

void UCarlaSettingsDelegate::SetAllRoads(
    UWorld *world,
    const float max_draw_distance,
    const TArray<FStaticMaterial> &road_pieces_materials) const
{
  if (!IsValid(world))
  {
    return;
  }
  AsyncTask(ENamedThreads::GameThread, [=]() {
    if (!IsValid(world))
    {
      return;
    }
    TArray<AActor *> actors;
    UGameplayStatics::GetAllActorsWithTag(world, UCarlaSettings::CARLA_ROAD_TAG, actors);

    for (int32 i = 0; i < actors.Num(); i++)
    {
      AActor *actor = actors[i];
      if (!IsValid(actor))
      {
        continue;
      }
      TArray<UStaticMeshComponent *> components;
      actor->GetComponents(components);
      for (int32 j = 0; j < components.Num(); j++)
      {
        UStaticMeshComponent *staticmeshcomponent = Cast<UStaticMeshComponent>(components[j]);
        if (staticmeshcomponent)
        {
          staticmeshcomponent->bAllowCullDistanceVolume = (max_draw_distance > 0);
          staticmeshcomponent->bUseAsOccluder = false;
          staticmeshcomponent->LDMaxDrawDistance = max_draw_distance;
          staticmeshcomponent->CastShadow = (max_draw_distance == 0);
          if (road_pieces_materials.Num() > 0)
          {
            TArray<FName> meshslotsnames = staticmeshcomponent->GetMaterialSlotNames();
            for (int32 k = 0; k < meshslotsnames.Num(); k++)
            {
              const FName &slotname = meshslotsnames[k];
              road_pieces_materials.ContainsByPredicate(
              [staticmeshcomponent, slotname](const FStaticMaterial &material)
              {
                if (material.MaterialSlotName.IsEqual(slotname))
                {
                  staticmeshcomponent->SetMaterial(
                  staticmeshcomponent->GetMaterialIndex(slotname),
                  material.MaterialInterface);
                  return true;
                }
                else
                {
                  return false;
                }
              });
            }
          }
        }
      }
    }
  }); // ,DELAY_TIME_TO_SET_ALL_ROADS, false);
}

void UCarlaSettingsDelegate::SetActorComponentsDrawDistance(
    AActor *actor,
    const float max_draw_distance) const
{
  if (!actor)
  {
    return;
  }
  TArray<UPrimitiveComponent *> components;
  actor->GetComponents(components, false);
  float dist = max_draw_distance;
  const float maxscale = actor->GetActorScale().GetMax();
  if (maxscale > CARLA_SETTINGS_MAX_SCALE_SIZE)
  {
    dist *= 100.0f;
  }
  for (int32 j = 0; j < components.Num(); j++)
  {
    UPrimitiveComponent *primitivecomponent = Cast<UPrimitiveComponent>(components[j]);
    if (IsValid(primitivecomponent))
    {
      primitivecomponent->SetCullDistance(dist);
      primitivecomponent->bAllowCullDistanceVolume = dist > 0;
    }
  }
}

void UCarlaSettingsDelegate::SetAllActorsDrawDistance(UWorld *world, const float max_draw_distance) const
{
  /// @TODO: use semantics to grab all actors by type
  /// (vehicles,ground,people,props) and set different distances configured in
  /// the global properties
  if (!IsValid(world))
  {
    return;
  }
  AsyncTask(ENamedThreads::GameThread, [=, this]() {
    if (!IsValid(world))
    {
      return;
    }
    TArray<AActor *> actors;
    // set the lower quality - max draw distance
    UGameplayStatics::GetAllActorsOfClass(world, AActor::StaticClass(), actors);
    for (int32 i = 0; i < actors.Num(); i++)
    {
      AActor *actor = actors[i];
      if (!IsValid(actor) ||
      actor->IsA<AInstancedFoliageActor>() ||   // foliage culling is controlled
                                                // per instance
      actor->IsA<ALandscape>() ||   // dont touch landscapes nor roads
      actor->ActorHasTag(UCarlaSettings::CARLA_ROAD_TAG) ||
      actor->ActorHasTag(UCarlaSettings::CARLA_SKY_TAG))
      {
        continue;
      }
      SetActorComponentsDrawDistance(actor, max_draw_distance);
    }
  });
}

void UCarlaSettingsDelegate::SetPostProcessEffectsEnabled(UWorld *world, const bool enabled) const
{
  TArray<AActor *> actors;
  UGameplayStatics::GetAllActorsOfClass(world, APostProcessVolume::StaticClass(), actors);
  for (int32 i = 0; i < actors.Num(); i++)
  {
    AActor *actor = actors[i];
    if (!IsValid(actor))
    {
      continue;
    }
    APostProcessVolume *postprocessvolume = Cast<APostProcessVolume>(actor);
    if (postprocessvolume)
    {
      postprocessvolume->bEnabled = enabled;
    }
  }
}

void UCarlaSettingsDelegate::LaunchEpicQualityCommands(UWorld *world) const
{
  if (!world)
  {
    return;
  }

  // Epic = High (SW-Lumen maxed) + full Hardware Ray Tracing restored + a
  // handful of UE5.5-specific quality bumps. The RT subsystem itself is
  // started via DefaultEngine.ini (r.RayTracing is ECVF_ReadOnly so runtime
  // toggling is impossible); this function enables the per-effect CVars.
  GEngine->Exec(world, TEXT("r.DynamicGlobalIlluminationMethod 1"));
  GEngine->Exec(world, TEXT("r.ReflectionMethod 1"));
  GEngine->Exec(world, TEXT("r.Lumen.DiffuseIndirect.Allow 1"));
  GEngine->Exec(world, TEXT("r.Lumen.Reflections.Allow 1"));
  // -1 = use per-effect switches below. The lower tiers force this to 0 so
  // restoring tier-Epic behaviour from a lower tier has to flip it back.
  GEngine->Exec(world, TEXT("r.RayTracing.ForceAllRayTracingEffects -1"));
  // Full HW-RT: Lumen hit lighting with skylight + reflection captures for
  // richer car-paint / building-window reflections, and RT shadows for
  // crisp contact shadows on moving actors.
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing 1"));
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing.LightingMode 3"));
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing.HitLighting.Skylight 1"));
  // UE5.5 only: bring reflection captures into HW-RT hit lighting so the
  // fallback path (roughness above the screen-trace threshold) keeps the
  // baked cubemap contribution on polished surfaces.
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing.HitLighting.ReflectionCaptures 1"));
  // Enable a second recursive bounce inside Lumen reflections so mirrors /
  // car paint reflecting other cars pick up their reflections too. Default
  // is 1 (single bounce); only takes effect under HW-RT Hit Lighting.
  GEngine->Exec(world, TEXT("r.Lumen.Reflections.MaxBounces 2"));
  // Sharper shadow resolution for hit-lighting samples inside reflections
  // (default 0 = Lumen surface-cache shadows; 1 = virtual shadow maps).
  GEngine->Exec(world, TEXT("r.Lumen.HardwareRayTracing.HitLighting.ShadowMode 1"));
  GEngine->Exec(world, TEXT("r.RayTracing.Shadows 1"));
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.Enable 1"));
  // VSM page pool matched to High so multi-camera captures don't thrash.
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.MaxPhysicalPages 8192"));
  GEngine->Exec(world, TEXT("r.Shadow.DistanceScale 2"));
  // UE5.5 only: don't cull the far VSM clipmaps, so long-range shadows from
  // distant trees and traffic infrastructure do not pop as the ego moves.
  GEngine->Exec(world, TEXT("r.Shadow.Virtual.UseFarShadowCulling 0"));
  // Full skin cache residency so the HW-RT BVH keeps every vehicle skinned
  // instead of dropping large meshes to flat.
  GEngine->Exec(world, TEXT("r.SkinCache.SceneMemoryLimitInMB 768"));
  GEngine->Exec(world, TEXT("r.TSR.History.ScreenPercentage 200"));
  GEngine->Exec(world, TEXT("r.LumenScene.SurfaceCache.AtlasSize 4096"));
  GEngine->Exec(world, TEXT("r.MaxAnisotropy 8"));
  // UE5.5 MegaLights: RT-accelerated many-light culling + shadowing. Only
  // enabled on Epic because it depends on HW-RT and costs a small tile
  // buffer. Big quality gain at night (headlights / street-lights reflected
  // on car paint, shadowing from local lights).
  GEngine->Exec(world, TEXT("r.MegaLights.EnableForProject 1"));
  GEngine->Exec(world, TEXT("r.Nanite.Streaming.PoolSize 512"));
  GEngine->Exec(world, TEXT("r.AntiAliasingMethod 4")); // TSR

  // --- Scalability group buckets ---------------------------------------
  GEngine->Exec(world, TEXT("sg.ResolutionQuality 100"));
  GEngine->Exec(world, TEXT("sg.ViewDistanceQuality 3"));
  GEngine->Exec(world, TEXT("sg.AntiAliasingQuality 3"));
  GEngine->Exec(world, TEXT("sg.ShadowQuality 3"));
  GEngine->Exec(world, TEXT("sg.GlobalIlluminationQuality 3"));
  GEngine->Exec(world, TEXT("sg.ReflectionQuality 3"));
  GEngine->Exec(world, TEXT("sg.PostProcessQuality 3"));
  GEngine->Exec(world, TEXT("sg.TextureQuality 3"));
  GEngine->Exec(world, TEXT("sg.EffectsQuality 3"));
  GEngine->Exec(world, TEXT("sg.FoliageQuality 3"));
  GEngine->Exec(world, TEXT("sg.ShadingQuality 3"));

  GEngine->Exec(world, TEXT("r.Streaming.PoolSize 4000"));
  GEngine->Exec(world, TEXT("r.Streaming.LimitPoolSizeToVRAM 1"));
  // Cap the streaming temp scratch buffer at 32 MB (default 50). With the
  // pool clamp above absorbing most growth, the scratch only needs to hold
  // a couple of large mip uploads in flight; the smaller cap shaves ~10-20
  // MB off the working set on Epic. Risk: streaming hitches if many large
  // textures need to stage at once - the streamer falls back to round-robin
  // staging instead of bulk-uploading.
  GEngine->Exec(world, TEXT("r.Streaming.MaxTempMemoryAllowed 32"));
  GEngine->Exec(world, TEXT("r.ViewDistanceScale 1"));
  GEngine->Exec(world, TEXT("foliage.DensityScale 1"));
  GEngine->Exec(world, TEXT("grass.DensityScale 1"));
  GEngine->Exec(world, TEXT("r.DefaultFeature.AntiAliasing 1"));
}

void UCarlaSettingsDelegate::SetAllLights(
    UWorld *world,
    const float max_distance_fade,
    const bool cast_shadows,
    const bool hide_non_directional) const
{
  if (!IsValid(world))
  {
    return;
  }
  AsyncTask(ENamedThreads::GameThread, [=]() {
    if (!IsValid(world))
    {
      return;
    }
    TArray<AActor *> actors;
    UGameplayStatics::GetAllActorsOfClass(world, ALight::StaticClass(), actors);
    for (int32 i = 0; i < actors.Num(); i++)
    {
      if (!IsValid(actors[i]))
      {
        continue;
      }
      // tweak directional lights
      ADirectionalLight *directionallight = Cast<ADirectionalLight>(actors[i]);
      if (directionallight)
      {
        directionallight->SetCastShadows(cast_shadows);
        directionallight->SetLightFunctionFadeDistance(max_distance_fade);
        continue;
      }
      // disable any other type of light
      actors[i]->SetActorHiddenInGame(hide_non_directional);
    }
  });

}

void UCarlaSettingsDelegate::ApplyPerActorQualitySettings(
    UWorld *world,
    const float light_fade_distance,
    const bool cast_directional_shadows,
    const bool hide_non_directional_lights,
    const float draw_distance) const
{
  if (!IsValid(world))
  {
    return;
  }
  AsyncTask(ENamedThreads::GameThread, [=, this]() {
    if (!IsValid(world))
    {
      return;
    }
    TArray<AActor *> actors;
    UGameplayStatics::GetAllActorsOfClass(world, AActor::StaticClass(), actors);
    for (int32 i = 0; i < actors.Num(); i++)
    {
      AActor *actor = actors[i];
      if (!IsValid(actor))
      {
        continue;
      }

      if (ADirectionalLight *directional = Cast<ADirectionalLight>(actor))
      {
        directional->SetCastShadows(cast_directional_shadows);
        directional->SetLightFunctionFadeDistance(light_fade_distance);
        continue;
      }
      if (actor->IsA<ALight>())
      {
        actor->SetActorHiddenInGame(hide_non_directional_lights);
        continue;
      }

      if (actor->IsA<AInstancedFoliageActor>() ||
          actor->IsA<ALandscape>() ||
          actor->ActorHasTag(UCarlaSettings::CARLA_ROAD_TAG) ||
          actor->ActorHasTag(UCarlaSettings::CARLA_SKY_TAG))
      {
        continue;
      }

      SetActorComponentsDrawDistance(actor, draw_distance);
    }
  });
}
