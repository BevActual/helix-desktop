#pragma once

#include <cstddef>
#include <string>

namespace helix::agent {

  // Display-safe text for anything an agent, app or window can influence: control characters and bidi/zero-width
  // controls removed, invalid UTF-8 (including overlong forms and surrogates) replaced with '?', and shortened to
  // `maxCodepoints` with an ellipsis. Newlines become spaces so one value cannot fake extra lines in a dialog or list.
  std::string sanitizeForDisplay(const std::string& text, std::size_t maxCodepoints);

  // Short name for a verified client identity ("exe:/usr/bin/python3#/home/u/agent.py" becomes "agent.py"). Only ASCII
  // letters, digits and . _ + - survive; anything else becomes '_', so a hostile file name cannot read as a sentence,
  // imitate another app with look-alike letters or hide behind spaces. Names are not unique; the full identity is
  // always shown beside it.
  std::string friendlyClientName(const std::string& client, std::size_t maxChars = 40);

} // namespace helix::agent
