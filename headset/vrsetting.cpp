// vrsetting: read, set or remove one boolean SteamVR setting.
//   vrsetting SECTION KEY [true|false|remove]
#include <openvr.h>
#include <cstdio>
#include <cstring>
int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: vrsetting SECTION KEY [true|false|remove]\n"); return 2; }
	vr::EVRInitError e;
	vr::VR_Init(&e, vr::VRApplication_Utility);
	if (e) { fprintf(stderr, "VR_Init %d\n", e); return 1; }
	vr::EVRSettingsError se = vr::VRSettingsError_None;
	if (argc > 3 && !strcmp(argv[3], "remove")) vr::VRSettings()->RemoveKeyInSection(argv[1], argv[2], &se);
	else if (argc > 3) vr::VRSettings()->SetBool(argv[1], argv[2], !strcmp(argv[3], "true"), &se);
	if (se) fprintf(stderr, "settings error %d\n", se);
	bool b = vr::VRSettings()->GetBool(argv[1], argv[2], &se);
	printf("%s/%s = %s%s\n", argv[1], argv[2], b ? "true" : "false", se ? " (unset)" : "");
	vr::VR_Shutdown();
	return 0;
}
