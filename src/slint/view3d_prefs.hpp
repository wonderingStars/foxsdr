// view3d_prefs.hpp - WHAT A 3D VIEW REMEMBERS (docs/engine-stage4.md 6.64): the chosen view (2D or 3D) and the camera, and up to four small options (a depth, a shading, a scale...), for each 3D view
// of the window, in ONE interface-half setting (AppConfig::view3d, a string) so a view added later is a name, not a new field.
//
// THE TEXT: `name:key=value,key=value;name:...` - names and keys are lower-case words, values numbers. Reading is lenient (a malformed entry, an unknown key or a bad number is skipped and the
// default stays, every value is clamped to its range), so a setting written by a newer build reads in an older one. The 2D view is the default: a view that is not listed is off.
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <array>
#include <map>
#include <string>

#include "scene3d.hpp"

namespace foxsdr::slintview3d {

// One view's remembered state.
struct ViewState {
    bool on = false;                       // the 3D view is showing (the 2D view is the default)
    float azimuth = -58.0f;                // degrees
    float elevation = 32.0f;               // degrees
    float zoom = 1.0f;
    std::array<int, 4> opt{0, 0, 0, 0};    // a view's own small choices (an index each); what they mean is the view's
    bool operator==(const ViewState& o) const { return on == o.on && azimuth == o.azimuth && elevation == o.elevation && zoom == o.zoom && opt == o.opt; }
};

// The names of the views of lane A (the setting's keys). Any other lower-case name is a view too: lane B's views use theirs the same way.
inline constexpr const char* kViewWaterfall = "waterfall";
inline constexpr const char* kViewRadar = "radar";
inline constexpr const char* kViewTunnel = "tunnel";
inline constexpr const char* kViewSurvey = "survey";
inline constexpr const char* kViewSonde = "sonde";

class ViewPrefs {
public:
    static ViewPrefs parse(const std::string& text);
    std::string serialize() const;                                  // the text, names in order, the same state always the same text
    // The state of a view, or `defaults` when it is not listed. The camera and options are clamped (elevation 4..89, zoom 0.35..5, azimuth -180..180, each option 0..15).
    ViewState get(const std::string& name, const ViewState& defaults) const;
    void set(const std::string& name, const ViewState& s);
    // ENTRIES THAT ARE NOT A 3D VIEW travel in the same text (screen stage S5: the workspace's saved layout, name "workspace", workspace_prefs.hpp): their body is kept VERBATIM, never read as a camera,
    // so a 3D page that saves its own view does not lose them. Only the names in kRawNames are kept this way; the body may hold anything but ';'.
    std::string raw(const std::string& name) const;       // "" when there is none
    void setRaw(const std::string& name, const std::string& body);   // an empty body removes the entry
    bool operator==(const ViewPrefs& o) const { return views_ == o.views_ && raw_ == o.raw_; }

private:
    std::map<std::string, ViewState> views_;
    std::map<std::string, std::string> raw_;
};

// The names of the entries kept verbatim.
inline constexpr const char* kRawWorkspace = "workspace";

// A camera and the state it came from, both ways.
scene3d::Camera cameraOf(const ViewState& s);
void setCamera(ViewState& s, const scene3d::Camera& c);

}  // namespace foxsdr::slintview3d
