// keybox: a solid-colour SteamVR overlay on the PC, like Reality Mixer's
// "solid color" boxes. With the Frame keyer keying that colour, the box shows
// the headset's passthrough.
//
//   keybox [front|hand] [RRGGBB] [width_m] [seconds]
//     front: fixed in the room, 0.6 m ahead of where you look at start (default)
//     hand:  follows the left controller
#include <openvr.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

int main(int argc, char **argv) {
	const char *mode = argc > 1 ? argv[1] : "front";
	unsigned rgb = argc > 2 ? strtoul(argv[2], nullptr, 16) : 0xff00ff;
	float width = argc > 3 ? (float)atof(argv[3]) : 0.4f;
	int seconds = argc > 4 ? atoi(argv[4]) : 600;

	vr::EVRInitError e;
	vr::VR_Init(&e, vr::VRApplication_Overlay);
	if (e) { printf("VR_Init: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(e)); return 1; }
	vr::IVROverlay *ov = vr::VROverlay();
	vr::VROverlayHandle_t h;
	if (vr::EVROverlayError oe = ov->CreateOverlay("frame.keybox", "Key box", &h)) {
		printf("CreateOverlay: %s\n", ov->GetOverlayErrorNameFromEnum(oe));
		return 1;
	}

	const int n = 16;
	std::vector<unsigned char> px(n * n * 4);
	for (int i = 0; i < n * n; i++) {
		px[i * 4 + 0] = (rgb >> 16) & 255;
		px[i * 4 + 1] = (rgb >> 8) & 255;
		px[i * 4 + 2] = rgb & 255;
		px[i * 4 + 3] = 255;
	}
	ov->SetOverlayRaw(h, px.data(), n, n, 4);
	ov->SetOverlayWidthInMeters(h, width);

	if (!strcmp(mode, "hand")) {
		vr::TrackedDeviceIndex_t dev =
			vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(vr::TrackedControllerRole_LeftHand);
		// Box sits just above the controller, facing up toward you.
		vr::HmdMatrix34_t m = {{{1, 0, 0, 0}, {0, 0, 1, 0.05f}, {0, -1, 0, -0.05f}}};
		ov->SetOverlayTransformTrackedDeviceRelative(h, dev, &m);
	} else {
		vr::TrackedDevicePose_t pose[vr::k_unMaxTrackedDeviceCount];
		vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, pose,
			vr::k_unMaxTrackedDeviceCount);
		// Same orientation as the head, pushed 0.6 m along its forward (-Z) axis.
		vr::HmdMatrix34_t m = pose[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
		for (int r = 0; r < 3; r++) m.m[r][3] -= 0.6f * m.m[r][2];
		ov->SetOverlayTransformAbsolute(h, vr::TrackingUniverseStanding, &m);
	}
	ov->ShowOverlay(h);
	printf("keybox: %s, #%06x, %.2f m wide, %d s\n", mode, rgb, width, seconds);
	fflush(stdout);

	for (int t = 0; t < seconds * 10; t++) {
		vr::VREvent_t ev;
		while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
			if (ev.eventType == vr::VREvent_Quit) t = seconds * 10;
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	ov->DestroyOverlay(h);
	vr::VR_Shutdown();
	return 0;
}
