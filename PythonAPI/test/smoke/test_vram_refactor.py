# Copyright (c) 2026 Computer Vision Center (CVC) at the Universitat Autonoma de
# Barcelona (UAB).
#
# This work is licensed under the terms of the MIT license.
# For a copy, see <https://opensource.org/licenses/MIT>.

# NOTE: GBuffer streams are not initialized on ue5-dev — all 13 SetDataStream
# calls in SensorFactory.cpp:142-154 are commented out. Calling
# listen_to_gbuffer() on any GBufferTextureID crashes the server.
# F1 gate (IsAnyGBufferClientListening) is therefore tested via main stream.

from . import SyncSmokeTest

import carla
import time
from queue import Queue, Empty

TICK_COUNT = 20


def _spawn_rgb(world):
    bp = world.get_blueprint_library().find('sensor.camera.rgb')
    return world.spawn_actor(bp, carla.Transform(carla.Location(z=5)))


def _spawn_semantic(world):
    bp = world.get_blueprint_library().find('sensor.camera.semantic_segmentation')
    return world.spawn_actor(bp, carla.Transform(carla.Location(z=5)))


class TestVramRefactor(SyncSmokeTest):
    """Regression tests for the VRAM refactor (fix/carla-vram-needed-performance).

    Covers:
    - F1: ShouldCaptureThisFrame gate — delivery resumes after listen() is called
          on a camera that was previously idle (no subscriber).
    - F1: Listener churn (listen→stop→listen) does not latch the gate off.
    - F4: r.CustomDepth=3→1 does not break semantic segmentation cameras.
    """

    def tearDown(self):
        # The packaged server ships only Town10HD_Opt/Mine_01/Town15.
        # The base SmokeTest.tearDown calls load_world('Town03') which fails.
        # Use reload_world() instead; restore sync settings manually first.
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

    def test_capture_gate_activates_on_first_listen(self):
        """F1 gate: camera spawned with no listener must deliver frames once listen() is called."""
        print("TestVramRefactor.test_capture_gate_activates_on_first_listen")
        camera = _spawn_rgb(self.world)
        q = Queue()
        try:
            for _ in range(10):
                self.world.tick()

            camera.listen(q.put)
            for _ in range(TICK_COUNT):
                self.world.tick()
            time.sleep(0.5)

            self.assertGreater(q.qsize(), 0,
                "No frames received after listen() — gate may be permanently "
                "latched off after idle period")
        finally:
            camera.destroy()

    def test_listener_churn(self):
        """F1 churn: listen→stop→listen must resume delivery without latching off."""
        print("TestVramRefactor.test_listener_churn")
        camera = _spawn_rgb(self.world)
        q = Queue()
        try:
            camera.listen(q.put)
            for _ in range(TICK_COUNT):
                self.world.tick()
            time.sleep(0.5)
            self.assertGreater(q.qsize(), 0, "No frames in listen window 1")

            while not q.empty():
                try:
                    q.get_nowait()
                except Empty:
                    break

            camera.stop()
            for _ in range(TICK_COUNT):
                self.world.tick()
            time.sleep(0.5)
            self.assertEqual(q.qsize(), 0,
                "Frames still arrived after stop() — gate did not activate")

            camera.listen(q.put)
            for _ in range(TICK_COUNT):
                self.world.tick()
            time.sleep(0.5)
            self.assertGreater(q.qsize(), 0,
                "No frames after re-listen() — gate latched off after stop()")
        finally:
            camera.destroy()

    def test_semantic_segmentation_after_custom_depth_reduction(self):
        """F4: r.CustomDepth=3→1 must not break semantic segmentation delivery.

        CARLA segmentation uses SetCustomPrimitiveDataVector4, not CustomStencil.
        This confirms the camera produces valid frames after the ini change.
        """
        print("TestVramRefactor.test_semantic_segmentation_after_custom_depth_reduction")
        camera = _spawn_semantic(self.world)
        q = Queue()
        try:
            camera.listen(q.put)
            for _ in range(10):
                self.world.tick()
            time.sleep(0.5)

            self.assertGreater(q.qsize(), 0,
                "Semantic segmentation camera produced no frames — "
                "r.CustomDepth reduction may have broken the sensor path")

            img = q.get_nowait()
            self.assertGreater(img.width, 0, "Received image has zero width")
            self.assertGreater(img.height, 0, "Received image has zero height")
        finally:
            camera.destroy()
