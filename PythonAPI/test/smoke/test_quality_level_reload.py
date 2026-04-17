# Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma de
# Barcelona (UAB).
#
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT>.

from . import SyncSmokeTest

import carla
import time
from queue import Queue, Empty

# The packaged server only ships a subset of maps; Town03 (base tearDown) and
# reload_world() are unavailable. Use load_world() with a packaged map instead.
_RELOAD_MAP = 'Town10HD_Opt'

_SYNC_SETTINGS = carla.WorldSettings(
    no_rendering_mode=False,
    synchronous_mode=True,
    fixed_delta_seconds=0.05,
)

_FRAMES_PER_CYCLE = 5
_RELOAD_CYCLES = 3


class TestQualityLevelReload(SyncSmokeTest):
    """Regression test for F7: ApplyPerActorQualitySettings must not deadlock on reload.

    CARLA has no Python runtime toggle for EQualityLevel — ApplyQualityLevelPostRestart
    fires on every map load at whatever quality the server was launched with. This test
    verifies the refactored single-pass actor walk completes without deadlock and that
    camera capture still works afterward.

    Per-tier behavioral differences (draw distance, light settings) are not
    assertable from Python without restarting the server with different flags.
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
            self.client.load_world(_RELOAD_MAP)
            time.sleep(2)
        except Exception:
            pass
        self.world = None
        self.client = None

    def test_reload_world_cycles_apply_quality_settings(self):
        print("TestQualityLevelReload.test_reload_world_cycles_apply_quality_settings")

        for cycle in range(_RELOAD_CYCLES):
            self.world = self.client.load_world(_RELOAD_MAP)
            # UE asset GC workaround (matches test_sync.py:30-33 idiom).
            time.sleep(5)
            self.world.apply_settings(_SYNC_SETTINGS)
            self.world.tick()

            bp = self.world.get_blueprint_library().find('sensor.camera.rgb')
            camera = self.world.spawn_actor(bp, carla.Transform(carla.Location(z=5)))
            q = Queue()
            try:
                camera.listen(q.put)
                for _ in range(_FRAMES_PER_CYCLE):
                    self.world.tick()
                time.sleep(0.5)

                self.assertGreater(
                    q.qsize(), 0,
                    f"Cycle {cycle}: camera received no frames after map load — "
                    "ApplyPerActorQualitySettings may have deadlocked the GameThread",
                )
            finally:
                camera.destroy()
