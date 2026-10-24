// The OpenVR functions openvr.h's inline VR_Init, VR_Shutdown and interface getters call, forwarded to the headset's
// own libopenvr_api.so (SteamVR's), loaded when first needed. So the panel builds without an OpenVR library for arm64
// and always talks to the SteamVR it runs under.
#include <dlfcn.h>
#include <stddef.h>

#include "openvr.h"

namespace {

const char *const kLibs[] = {"/opt/steamvr/bin/linuxarm64/libopenvr_api.so", "libopenvr_api.so"};

void *Lib() {
  static void *lib = [] {
    for (const char *path : kLibs)
      if (void *l = dlopen(path, RTLD_NOW | RTLD_LOCAL)) return l;
    return (void *)nullptr;
  }();
  return lib;
}

template <typename F>
F Fn(const char *name) {
  void *l = Lib();
  return l ? (F)dlsym(l, name) : nullptr;
}

}  // namespace

namespace vr {

uint32_t VR_InitInternal2(EVRInitError *peError, EVRApplicationType eApplicationType, const char *pStartupInfo) {
  static auto f = Fn<uint32_t (*)(EVRInitError *, EVRApplicationType, const char *)>("VR_InitInternal2");
  if (!f) {
    if (peError) *peError = VRInitError_Init_InstallationNotFound;
    return 0;
  }
  return f(peError, eApplicationType, pStartupInfo);
}

void VR_ShutdownInternal() {
  static auto f = Fn<void (*)()>("VR_ShutdownInternal");
  if (f) f();
}

bool VR_IsHmdPresent() {
  static auto f = Fn<bool (*)()>("VR_IsHmdPresent");
  return f && f();
}

bool VR_IsRuntimeInstalled() {
  static auto f = Fn<bool (*)()>("VR_IsRuntimeInstalled");
  return f && f();
}

const char *VR_GetVRInitErrorAsSymbol(EVRInitError error) {
  static auto f = Fn<const char *(*)(EVRInitError)>("VR_GetVRInitErrorAsSymbol");
  return f ? f(error) : "VRInitError_Init_InstallationNotFound";
}

const char *VR_GetVRInitErrorAsEnglishDescription(EVRInitError error) {
  static auto f = Fn<const char *(*)(EVRInitError)>("VR_GetVRInitErrorAsEnglishDescription");
  return f ? f(error) : "SteamVR's libopenvr_api.so wasn't found";
}

void *VR_GetGenericInterface(const char *pchInterfaceVersion, EVRInitError *peError) {
  static auto f = Fn<void *(*)(const char *, EVRInitError *)>("VR_GetGenericInterface");
  if (!f) {
    if (peError) *peError = VRInitError_Init_InstallationNotFound;
    return nullptr;
  }
  return f(pchInterfaceVersion, peError);
}

bool VR_IsInterfaceVersionValid(const char *pchInterfaceVersion) {
  static auto f = Fn<bool (*)(const char *)>("VR_IsInterfaceVersionValid");
  return f && f(pchInterfaceVersion);
}

uint32_t VR_GetInitToken() {
  static auto f = Fn<uint32_t (*)()>("VR_GetInitToken");
  return f ? f() : 0;
}

}  // namespace vr
