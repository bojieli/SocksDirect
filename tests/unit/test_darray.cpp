// Unit tests for darray_t.
//
// darray_t is the slot allocator that backs the fd table and the adjacency
// list. The contract worth pinning down (it is not documented anywhere
// else):
//   - add() returns the lowest unused slot index, including reused slots
//     after del().
//   - The container grows by 2x once the live count meets the current
//     capacity. Capacity never shrinks, even after del().
//   - get_highest_possible() is a watermark of the largest index ever
//     allocated, kept tight after del() of the watermark slot but lazily
//     stale otherwise.
//   - iterator_init() / iterator_next() walk only valid slots in index
//     order; the iterator returns -1 at end.
//   - del() of an invalid slot is undefined behavior (same as the
//     production callers; we don't test it).

#include "common/darray.hpp"

#include <gtest/gtest.h>
#include <set>
#include <vector>

namespace {

TEST(Darray, EmptyOnConstruction) {
    darray_t<int, 8> d;
    EXPECT_EQ(0u, d.get_totalsize());
    EXPECT_EQ(-1, d.iterator_init());
}

TEST(Darray, AddReturnsZeroFirst) {
    darray_t<int, 8> d;
    EXPECT_EQ(0u, d.add(42));
    EXPECT_EQ(1u, d.get_totalsize());
    EXPECT_TRUE(d.isvalid(0));
    EXPECT_EQ(42, d[0]);
}

TEST(Darray, AddReturnsConsecutiveIndices) {
    darray_t<int, 16> d;
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(static_cast<unsigned>(i), d.add(i * 100));
    }
    EXPECT_EQ(10u, d.get_totalsize());
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(i * 100, d[i]);
    }
}

TEST(Darray, DelMakesSlotInvalid) {
    darray_t<int, 8> d;
    d.add(1);
    d.add(2);
    d.add(3);
    d.del(1);
    EXPECT_FALSE(d.isvalid(1));
    EXPECT_TRUE(d.isvalid(0));
    EXPECT_TRUE(d.isvalid(2));
    EXPECT_EQ(2u, d.get_totalsize());
}

TEST(Darray, AddReusesDeletedSlotAfterWrapAround) {
    // KNOWN behavior, not necessarily ideal: lowest_available advances
    // monotonically until it wraps. So del() of an earlier slot does NOT
    // immediately make that slot the next allocation target. The slot is
    // only reused after the cursor walks around the whole table.
    //
    // This is something Phase 3 may want to revisit (a simple free-list
    // would give better locality for the FD table). Documented here so
    // future cleanup intentionally changes the behavior.
    darray_t<int, 8> d;
    d.add(10);
    d.add(20);
    d.add(30);
    d.del(1);
    // Next add does NOT reuse slot 1 — it returns slot 3 because the
    // cursor was already past slot 1.
    EXPECT_EQ(3u, d.add(99));
    EXPECT_EQ(99, d[3]);
    // Slot 1 is reclaimed only after the cursor wraps. Force the wrap by
    // filling the rest of the original capacity (8 slots) and then asking
    // for one more — the next allocation must come back around to slot 1.
    d.add(0); d.add(0); d.add(0); d.add(0);  // slots 4,5,6,7
    EXPECT_EQ(1u, d.add(42));
    EXPECT_EQ(42, d[1]);
}

TEST(Darray, GrowsWhenSaturated) {
    constexpr unsigned kInit = 4;
    darray_t<int, kInit> d;
    // Filling to capacity must not crash; the container grows internally.
    for (unsigned i = 0; i < 100; ++i) {
        unsigned idx = d.add(static_cast<int>(i));
        EXPECT_EQ(i, idx);
        EXPECT_TRUE(d.isvalid(idx));
        EXPECT_EQ(static_cast<int>(i), d[idx]);
    }
    EXPECT_EQ(100u, d.get_totalsize());
}

TEST(Darray, HighestPossibleTrackedAfterDel) {
    darray_t<int, 16> d;
    for (int i = 0; i < 5; ++i) d.add(i);
    EXPECT_EQ(4u, d.get_highest_possible());
    d.del(4);
    // Watermark recomputed when we delete the watermark itself.
    EXPECT_EQ(3u, d.get_highest_possible());
    d.del(0);
    // Deleting non-watermark leaves the watermark alone.
    EXPECT_EQ(3u, d.get_highest_possible());
}

TEST(Darray, IteratorVisitsValidSlotsOnly) {
    darray_t<int, 16> d;
    for (int i = 0; i < 8; ++i) d.add(i * 10);
    d.del(2);
    d.del(5);
    d.del(7);

    std::set<int> seen;
    for (int p = d.iterator_init(); p != -1; p = d.iterator_next(p)) {
        seen.insert(d[p]);
    }
    EXPECT_EQ(std::set<int>({0, 10, 30, 40, 60}), seen);
}

TEST(Darray, IteratorOnEmptyReturnsMinusOne) {
    darray_t<int, 4> d;
    EXPECT_EQ(-1, d.iterator_init());
    d.add(1);
    d.del(0);
    EXPECT_EQ(-1, d.iterator_init());
}

TEST(Darray, ChurnPreservesInvariants) {
    // Stress: add/delete in interleaved patterns, then check that all live
    // entries are visible from the iterator and the totalsize is right.
    //
    // We track unique slot IDs in a set rather than a vector — add()
    // reuses freed slots, so an "insertion-order" vector would contain
    // aliased IDs and a del() of one would corrupt the data structure
    // (the prototype's del() decrements an unsigned total_num, which
    // would wrap to a huge number and trigger an infinite resize loop).
    darray_t<int, 8> d;
    std::set<unsigned> live;
    for (int round = 0; round < 2000; ++round) {
        if (live.size() < 50 || (round % 3) != 0) {
            unsigned idx = d.add(round);
            EXPECT_TRUE(d.isvalid(idx));
            // If add reused a slot, the old occupant is gone — our tracking
            // set already reflects that because the slot ID is a duplicate.
            live.insert(idx);
        } else {
            // Delete a deterministic middle slot.
            auto it = live.begin();
            std::advance(it, live.size() / 2);
            unsigned idx = *it;
            d.del(idx);
            live.erase(it);
        }
    }

    std::set<unsigned> got;
    for (int p = d.iterator_init(); p != -1; p = d.iterator_next(p)) {
        got.insert(static_cast<unsigned>(p));
    }
    EXPECT_EQ(live, got);
    EXPECT_EQ(live.size(), d.get_totalsize());
}

TEST(Darray, InitResetsCountersButLeavesValidFlagsStale) {
    // KNOWN BUG in darray::init(): it resizes the std::vector but does not
    // clear the isvalid flags inside surviving slots. The result is that
    // the counter functions look correct, but a subsequent add() spins
    // forever in its scan loop because every slot reads as "in use."
    //
    // We pin the surviving-but-broken behavior here so that whoever fixes
    // the implementation in Phase 1 cleanup is forced to update the test.
    darray_t<int, 4> d;
    for (int i = 0; i < 20; ++i) d.add(i);
    d.init();
    EXPECT_EQ(0u, d.get_totalsize());
    EXPECT_EQ(0u, d.get_highest_possible());
    // The iterator walks from slot 0 to highest_possible, which is now 0,
    // and slot 0 is still flagged valid from before init() — so we DO see
    // it. Bug, but documented.
    EXPECT_EQ(0, d.iterator_init());

    // We deliberately do NOT call add() here — that would hit the
    // infinite-loop bug. The Phase 1 fix should make init() also clear
    // the data vector or shrink it to size INITSIZE with default-init.
}

}  // namespace
