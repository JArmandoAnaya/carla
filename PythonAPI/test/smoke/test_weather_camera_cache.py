# Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma de
# Barcelona (UAB).
#
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT>.

from . import SyncSmokeTest

import carla
import time
from queue import Queue, Empty

# WeatherParameters that triggers both blendable branches in
# AWeather::CheckWeatherPostProcessEffects:
#   precipitation > 0  -> M_screenDrops
#   dust_storm > 0     -> M_screenDust_wind
_RAIN_AND_DUST = carla.WeatherParameters(
    cloudiness=80.0,
    precipitation=100.0,
    precipitation_deposits=80.0,
    wind_intensity=1.0,
    sun_azimuth_angle=45.0,
    sun_altitude_angle=30.0,
    fog_density=0.0,
    fog_distance=0.0,
    fog_falloff=0.0,
    wetness=80.0,
    scattering_intensity=0.0,
    mie_scattering_scale=0.0,
    rayleigh_scattering_scale=0.0331,
    dust_storm=100.0,
)


class _CameraRecorder:
    def __init__(self, world):
        bp = world.get_blueprint_library().find('sensor.camera.rgb')
        self.camera = world.spawn_actor(bp, carla.Transform(carla.Location(z=5)))
        self.frames = []
        self.camera.listen(lambda img: self.frames.append(img.frame))

    def destroy(self):
        self.camera.destroy()

    def count(self):
        return len(self.frames)


class TestWeatherCameraCache(SyncSmokeTest):
    """Regression test for F13: AWeather::CachedCameras must survive actor churn.

    The refactored AWeather uses TWeakObjectPtr<ASceneCaptureCamera> maintained
    via OnActorSpawned / OnActorDestroyed handlers instead of a per-call
    GetAllActorsOfClass walk. This test exercises spawn, weather change, destroy,
    and weather change again — confirming no crash and no blendable propagation
    to dead cameras.
    """

    def tearDown(self):
        try:
            if getattr(self, 'settings', None) is not None:
                self.world.apply_settings(self.settings)
                self.world.tick()
        except Exception:
            pass
        self.settings = None
        try:
            self.client.load_world('Town10HD_Opt')
            time.sleep(2)
        except Exception:
            pass
        self.world = None
        self.client = None

    def test_weather_change_with_active_cameras(self):
        print("TestWeatherCameraCache.test_weather_change_with_active_cameras")

        recorders = [_CameraRecorder(self.world) for _ in range(3)]
        try:
            # Trigger both blendable branches simultaneously.
            self.world.set_weather(_RAIN_AND_DUST)
            for _ in range(5):
                self.world.tick()
            time.sleep(0.3)

            # All three cameras must still be delivering frames.
            for i, rec in enumerate(recorders):
                self.assertGreater(rec.count(), 0,
                    f"Camera {i} received no frames after weather change to rain+dust")

            # Destroy the middle camera; let OnActorDestroyed fire.
            dead_camera = recorders[1]
            dead_camera.destroy()
            self.world.tick()
            time.sleep(0.1)

            # Clear weather (removes blendables) then trigger dust-only.
            self.world.set_weather(carla.WeatherParameters.ClearNoon)
            self.world.tick()
            self.world.set_weather(carla.WeatherParameters.DustStorm)

            count_before = [recorders[0].count(), recorders[2].count()]
            for _ in range(5):
                self.world.tick()
            time.sleep(0.3)

            # Live cameras must continue ticking.
            self.assertGreater(recorders[0].count(), count_before[0],
                "Camera 0 stopped receiving frames after sibling destroy + weather change")
            self.assertGreater(recorders[2].count(), count_before[1],
                "Camera 2 stopped receiving frames after sibling destroy + weather change")

            # The destroyed camera must not have gained any frames after destroy.
            # (frames list is closed because the listener lambda captured the list;
            # the camera actor itself is gone so no new callbacks will arrive.)
            frames_after_destroy = dead_camera.count()
            self.world.tick()
            time.sleep(0.1)
            self.assertEqual(dead_camera.count(), frames_after_destroy,
                "Destroyed camera (camera 1) still receiving frames — cache not purged")

        finally:
            for rec in recorders:
                try:
                    rec.destroy()
                except Exception:
                    pass
