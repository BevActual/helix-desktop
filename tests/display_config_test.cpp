#include "system/display_config.h"

#include <cstdio>
#include <nlohmann/json.hpp>
#include <print>
#include <string>

namespace {

  bool fail(const char* message) {
    std::println(stderr, "display_config_test: {}", message);
    return false;
  }

  bool expect(bool cond, const char* message) { return cond ? true : fail(message); }

  const char* kOutputsReply = R"({
    "Ok": {
      "Outputs": {
        "HDMI-A-1": {
          "name": "HDMI-A-1",
          "make": "LG",
          "model": "TV",
          "serial": "123",
          "modes": [
            {"width": 3840, "height": 2160, "refresh_rate": 119880, "is_preferred": true},
            {"width": 3840, "height": 2160, "refresh_rate": 60000, "is_preferred": false},
            {"width": 1920, "height": 1080, "refresh_rate": 60000, "is_preferred": false}
          ],
          "current_mode": 0,
          "vrr_supported": false,
          "vrr_enabled": false,
          "logical": {
            "x": 1552,
            "y": 0,
            "width": 3840,
            "height": 2160,
            "scale": 1.0,
            "transform": "Normal"
          }
        },
        "eDP-1": {
          "name": "eDP-1",
          "make": "BOE",
          "model": "Panel",
          "serial": "abc",
          "modes": [
            {"width": 2560, "height": 1600, "refresh_rate": 240000, "is_preferred": true},
            {"width": 2560, "height": 1600, "refresh_rate": 60000, "is_preferred": false}
          ],
          "current_mode": 0,
          "vrr_supported": true,
          "vrr_enabled": true,
          "logical": {
            "x": 0,
            "y": 0,
            "width": 1552,
            "height": 970,
            "scale": 1.65,
            "transform": "Normal"
          }
        },
        "DP-2": {
          "name": "DP-2",
          "make": "Unknown",
          "model": "Unknown",
          "modes": [
            {"width": 1920, "height": 1080, "refresh_rate": 60000, "is_preferred": true}
          ],
          "current_mode": null,
          "vrr_supported": false,
          "vrr_enabled": false,
          "logical": null
        }
      }
    }
  })";

} // namespace

int main() {
  bool ok = true;

  const auto outputs = display_config::parseOutputsReply(nlohmann::json::parse(kOutputsReply));
  ok = expect(outputs.size() == 3, "parses three outputs") && ok;
  if (outputs.size() == 3) {
    ok = expect(outputs[0].name == "eDP-1", "sorts enabled outputs by x") && ok;
    ok = expect(outputs[1].name == "HDMI-A-1", "places HDMI after laptop") && ok;
    ok = expect(outputs[2].name == "DP-2" && !outputs[2].enabled, "keeps disabled outputs") && ok;
    ok = expect(outputs[0].current.width == 2560 && outputs[0].current.refreshMilliHz == 240000, "reads current mode")
        && ok;
    ok = expect(outputs[0].scale > 1.6 && outputs[0].scale < 1.7, "reads fractional scale") && ok;
    ok = expect(outputs[0].vrrSupported && outputs[0].vrrEnabled, "reads vrr flags") && ok;
    ok = expect(display_config::displayLabel(outputs[0]) == "BOE Panel", "formats make+model label") && ok;
    ok = expect(display_config::isInternalOutputName("eDP-1"), "eDP is an internal panel") && ok;
    ok = expect(display_config::isInternalOutputName("LVDS-1"), "LVDS is an internal panel") && ok;
    ok = expect(display_config::isInternalOutputName("dsi-1"), "DSI is an internal panel") && ok;
    ok = expect(!display_config::isInternalOutputName("DP-1"), "DisplayPort is external") && ok;
    ok = expect(!display_config::isInternalOutputName("HDMI-A-1"), "HDMI is external") && ok;
    ok = expect(display_config::hasEnabledExternalOutput(outputs), "enabled HDMI counts as external") && ok;
  }

  ok = expect(
           display_config::parseError(nlohmann::json::parse(R"({"Err":"no such output"})")) == "no such output",
           "parses Err string"
       )
      && ok;

  const auto transform = display_config::parseTransform("flipped-90");
  ok = expect(transform == display_config::Transform::Flipped90, "parses CLI transform names") && ok;
  ok = expect(display_config::jsonName(display_config::Transform::Rotate90) == "90", "json transform name for 90")
      && ok;
  ok = expect(display_config::formatRefresh(240000) == "240 Hz", "formats whole-number refresh") && ok;
  ok = expect(display_config::formatRefresh(119880) == "119.880 Hz", "formats fractional refresh") && ok;
  ok = expect(display_config::formatScale(1.65) == "1.65×", "formats fractional scale") && ok;

  if (!outputs.empty()) {
    const auto resolutions = display_config::resolutionChoices(outputs[1]);
    ok = expect(resolutions.size() == 2, "groups HDMI resolutions") && ok;
    const auto refreshes = display_config::refreshChoices(outputs[1], 3840, 2160);
    ok = expect(refreshes.size() == 2, "lists HDMI refresh rates for 4K") && ok;
    const auto best = display_config::bestRefreshMilliHz(outputs[1], 1920, 1080);
    ok = expect(best == 60000, "picks available refresh for 1080p") && ok;

    const auto placed = display_config::snapPosition(outputs[1], outputs[0], display_config::LayoutDirection::Right);
    ok = expect(placed.x == 1552, "places HDMI to the right of the laptop") && ok;

    const auto draggedRight =
        display_config::snapDragPosition(outputs[1], outputs, 1600, 0);
    ok = expect(draggedRight.has_value(), "drag-snap finds a neighbor") && ok;
    ok = expect(
             draggedRight.has_value() && draggedRight->referenceName == "eDP-1"
                 && draggedRight->direction == display_config::LayoutDirection::Right && draggedRight->position.x == 1552
                 && draggedRight->position.y == 0,
             "drag-snap keeps HDMI top-aligned to the right of the laptop"
         )
        && ok;

    const auto draggedBelow = display_config::snapDragPosition(outputs[1], outputs, 0, 2000);
    ok = expect(
             draggedBelow.has_value() && draggedBelow->direction == display_config::LayoutDirection::Bottom
                 && draggedBelow->position.x == 0 && draggedBelow->position.y == 970,
             "drag-snap places HDMI below the laptop"
         )
        && ok;

    const auto draggedDownFromRight =
        display_config::snapDragPosition(outputs[1], outputs, outputs[1].x, outputs[0].logicalHeight + 400);
    ok = expect(
             draggedDownFromRight.has_value() && draggedDownFromRight->direction == display_config::LayoutDirection::Bottom
                 && draggedDownFromRight->position.y == 970 && draggedDownFromRight->position.x <= 0,
             "dragging a right-hand display down places it under the laptop"
         )
        && ok;

    const auto slidAlongRight = display_config::snapDragPosition(outputs[1], outputs, outputs[1].x, 80);
    ok = expect(
             slidAlongRight.has_value() && slidAlongRight->direction == display_config::LayoutDirection::Right
                 && slidAlongRight->position.x == 1552,
             "a small downward drag stays on the right and slides along the shared edge"
         )
        && ok;

    ok = expect(
             display_config::isAdjacentOn(outputs[1], outputs[0], display_config::LayoutDirection::Right),
             "parsed HDMI is already adjacent to the right of the laptop"
         )
        && ok;
    ok = expect(!display_config::outputsOverlap(outputs[0], outputs[1]), "parsed outputs do not overlap") && ok;

    const auto kept = display_config::placeAdjacent(
        outputs[1], outputs[0], display_config::LayoutDirection::Right, outputs[1].x, outputs[1].y
    );
    ok = expect(
             kept.x == 1552 && kept.y == 0,
             "placeAdjacent keeps the current offset instead of recentering"
         )
        && ok;

    auto nudged = outputs[1];
    const auto slid = display_config::placeAdjacent(
        nudged, outputs[0], display_config::LayoutDirection::Right, nudged.x, nudged.y - 40
    );
    ok = expect(slid.x == 1552 && slid.y < 0, "nudge up along the shared edge keeps HDMI adjacent") && ok;
    nudged.x = slid.x;
    nudged.y = slid.y;
    ok = expect(
             display_config::isAdjacentOn(nudged, outputs[0], display_config::LayoutDirection::Right),
             "nudged HDMI stays on the right"
         )
        && ok;
  }

  const auto mode = display_config::modeAction(2560, 1600, 240.0);
  ok = expect(mode["Mode"]["mode"]["Specific"]["width"] == 2560, "mode action width") && ok;
  ok = expect(mode["Mode"]["mode"]["Specific"]["refresh"] == 240.0, "mode action refresh Hz") && ok;

  const auto scale = display_config::scaleAction(1.65);
  ok = expect(scale["Scale"]["scale"]["Specific"] == 1.65, "scale action") && ok;

  const auto off = display_config::powerAction(false);
  ok = expect(off.is_string() && off.get<std::string>() == "Off", "power off is unit variant") && ok;

  const auto request = display_config::outputRequest("eDP-1", display_config::transformAction(display_config::Transform::Rotate90));
  ok = expect(request["Output"]["output"] == "eDP-1", "output request name") && ok;
  ok = expect(request["Output"]["action"]["Transform"]["transform"] == "90", "transform action json") && ok;

  const auto payload = display_config::outputRequestPayload("eDP-1", display_config::powerAction(true));
  ok = expect(payload.ends_with('\n'), "request payload is newline-terminated") && ok;

  const auto vrr = display_config::vrrAction(true);
  ok = expect(vrr["Vrr"]["vrr"]["vrr"] == true && vrr["Vrr"]["vrr"]["on_demand"] == false, "vrr action") && ok;

  return ok ? 0 : 1;
}
