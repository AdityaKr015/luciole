#pragma once

#include "CPP/7zip/ICoder.h"

namespace files::sevenzip {
  // No archive object or C++ callback may cross the boundary before this succeeds.
  inline bool compatibleModule(Func_GetModuleProp module, HRESULT (*clear)(PROPVARIANT*)) {
    if (!module || !clear) {
      return false;
    }
    PROPVARIANT interface{}, version{};
    const bool compatible = module(NModulePropID::kInterfaceType, &interface) == S_OK
        && interface.vt == VT_UI4
        && interface.ulVal == NModuleInterfaceType::k_IUnknown_VirtDestructor_ThisModule
        && module(NModulePropID::kVersion, &version) == S_OK
        && version.vt == VT_UI4
        && version.ulVal >= (23U << 16U)
        && version.ulVal < (27U << 16U);
    clear(&interface);
    clear(&version);
    return compatible;
  }
} // namespace files::sevenzip
