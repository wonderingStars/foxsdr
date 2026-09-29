// module_entry.cpp - the mock engine as a loadable engine module: the one
// exported symbol a host looks for (FOXAPI_ENGINE_QUERY_NAME).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "foxsdr_api.h"
#include "mock_engine/mock_engine.hpp"

extern "C" FOXAPI_EXPORT const FoxEngineApi* FOXAPI_CALL foxsdr_engine_query(uint32_t apiMajor,
                                                                 uint32_t apiMinor) {
    return foxsdr::mock::engineApi(apiMajor, apiMinor);
}
