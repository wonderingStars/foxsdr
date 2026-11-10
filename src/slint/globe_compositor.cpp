// globe_compositor.cpp - see globe_compositor.hpp. PLACEHOLDER until the 3D framework (scene3d.*) is merged: draws nothing and says so.
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "globe_compositor.hpp"

namespace foxsdr::globe {

bool GlobeCompositor::compose(const GlobeDrawList&, int, int, std::vector<std::uint8_t>&, std::vector<ScreenLabel>&) { return false; }

}  // namespace foxsdr::globe
