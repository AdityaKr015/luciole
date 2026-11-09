#include "operations/archive_sevenzip_abi.h"
#include "test_check.h"

#include <print>

namespace {
  UInt32 version = (26U << 16U) | 4U, interface = 0;
  unsigned cleared = 0;
  bool wrongType = false, failure = false;
  HRESULT property(PROPID id, PROPVARIANT* output) {
    output->vt = wrongType ? VT_EMPTY : VT_UI4;
    output->ulVal = id == NModulePropID::kInterfaceType ? interface : version;
    return failure ? E_FAIL : S_OK;
  }
  HRESULT clear(PROPVARIANT* value) {
    ++cleared;
    value->vt = VT_EMPTY;
    return S_OK;
  }
} // namespace

int main() {
  using files::sevenzip::compatibleModule;
  try {
    TEST_CHECK(compatibleModule(property, clear) && cleared == 2);
    version = (23U << 16U) | 1U;
    TEST_CHECK(compatibleModule(property, clear));
    version = 22U << 16U;
    TEST_CHECK(!compatibleModule(property, clear));
    version = 27U << 16U;
    TEST_CHECK(!compatibleModule(property, clear));
    version = (26U << 16U) | 4U;
    interface = 1;
    TEST_CHECK(!compatibleModule(property, clear));
    interface = 0;
    wrongType = true;
    TEST_CHECK(!compatibleModule(property, clear));
    wrongType = false;
    failure = true;
    TEST_CHECK(!compatibleModule(property, clear));
    TEST_CHECK(!compatibleModule(nullptr, clear) && !compatibleModule(property, nullptr));
    return 0;
  } catch (const std::exception& error) {
    std::println(stderr, "{}", error.what());
    return 1;
  }
}
