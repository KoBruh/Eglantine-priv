// SPDX-License-Identifier: GPL-3.0-or-later

#include "dynarmic/backend/block_range_information.h"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

void Check(bool condition) {
  if (!condition) {
    std::cerr << "JIT invalidation regression failed\n";
    std::exit(1);
  }
}

template <typename Address> void Run() {
  using Range = boost::icl::discrete_interval<Address>;
  using Descriptor = Dynarmic::IR::LocationDescriptor;
  Dynarmic::Backend::BlockRangeInformation<Address> cache;
  struct Entry {
    Address first, last;
    Descriptor descriptor;
  };
  std::vector<Entry> reference;
  std::mt19937 random{17};
  for (unsigned i = 0; i < 20'000; ++i) {
    const Address first = random() % 4096;
    const Address last = first + random() % 256;
    if (i % 3 != 0) {
      const Descriptor descriptor{i};
      cache.AddRange(Range::closed(first, last), descriptor);
      reference.push_back({first, last, descriptor});
    } else {
      boost::icl::interval_set<Address> invalidated;
      invalidated.add(Range::closed(first, last));
      const auto actual = cache.InvalidateRanges(invalidated);
      Common::unordered_set<Descriptor> expected;
      std::erase_if(reference, [&](const auto &entry) {
        if (entry.first <= last && first <= entry.last) {
          expected.insert(entry.descriptor);
          return true;
        }
        return false;
      });
      Check(actual == expected);
      Check(cache.InvalidateRanges(invalidated).empty());
    }
  }
  cache.ClearCache();
  boost::icl::interval_set<Address> all;
  all.add(Range::closed(0, std::numeric_limits<Address>::max()));
  Check(cache.InvalidateRanges(all).empty());
  cache.AddRange(Range::closed(0, std::numeric_limits<Address>::max()),
                 Descriptor{1});
  cache.AddRange(Range::closed(0, 0), Descriptor{2});
  cache.AddRange(Range::closed(0, 0), Descriptor{3});
  boost::icl::interval_set<Address> zero;
  zero.add(Range::closed(0, 0));
  Check(cache.InvalidateRanges(zero).size() == 3);
  Check(cache.InvalidateRanges(all).empty());
}

int main() {
  Run<u32>();
  Run<u64>();
  std::cout << "JIT ranges: 40,000 randomized operations, erased metadata and "
               "address boundaries passed\n";
}
