// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace VideoCommon {

// Erases both the lookup map entry and the slot it points to. Erasing only the map entry (as
// RemoveImage/RemoveFramebuffers used to) leaks a slot in the pool on every cache eviction.
template <class Slots, class Lookup>
auto EraseCachedSlot(Slots& slots, Lookup& lookup, typename Lookup::iterator entry) {
    slots.erase(entry->second);
    return lookup.erase(entry);
}

} // namespace VideoCommon
