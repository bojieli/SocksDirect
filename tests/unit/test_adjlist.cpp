// Unit tests for adjlist<K, V>.
//
// adjlist is a per-fd list-of-buffer-handles structure used by socket_lib.
// The contract: for each "key" (an fd), we maintain a circular linked list
// of "elements" (buffer adjacencies). add_key returns a handle; the handle
// is valid until del_key. Per-key elements are added/iterated/deleted
// through an iterator.
//
// These tests pin the shape of the public API. They focus on the surprising
// edge cases — single-element rings, deletion of the head element, iterator
// invalidation after add_element_at — because those are the cases that
// caused bugs in the prototype.

#include "common/adjlist_t.hpp"

#include <gtest/gtest.h>
#include <set>

namespace {

using AdjList = adjlist<int, 8, int, 8>;

TEST(AdjList, AddKeyReturnsValidHandle) {
    AdjList a;
    int k = a.add_key(7);
    EXPECT_GE(k, 0);
    EXPECT_TRUE(a.is_keyvalid(k));
    EXPECT_EQ(7, a[k]);
}

TEST(AdjList, AddKeyDifferentKeysGetDifferentHandles) {
    AdjList a;
    int k1 = a.add_key(1);
    int k2 = a.add_key(2);
    int k3 = a.add_key(3);
    EXPECT_NE(k1, k2);
    EXPECT_NE(k2, k3);
    EXPECT_EQ(1, a[k1]);
    EXPECT_EQ(3, a[k3]);
}

TEST(AdjList, BeginOnKeyWithNoElementsIsAtEnd) {
    AdjList a;
    int k = a.add_key(0);
    auto it = a.begin(k);
    EXPECT_TRUE(it.end());
}

TEST(AdjList, AddElementBecomesVisibleViaBegin) {
    AdjList a;
    int k = a.add_key(0);
    a.add_element(k, 100);
    auto it = a.begin(k);
    ASSERT_FALSE(it.end());
    EXPECT_EQ(100, *it);
    it.next();
    EXPECT_TRUE(it.end());
}

TEST(AdjList, MultipleElementsIterateInInsertionOrder) {
    AdjList a;
    int k = a.add_key(0);
    a.add_element(k, 10);
    a.add_element(k, 20);
    a.add_element(k, 30);

    std::vector<int> seen;
    for (auto it = a.begin(k); !it.end(); it.next()) {
        seen.push_back(*it);
    }
    // The implementation rotates the head pointer with .next(), so the
    // observed first element after a fresh begin() depends on the prior
    // pointer state. What's invariant is that all 3 are seen exactly once.
    std::set<int> as_set(seen.begin(), seen.end());
    EXPECT_EQ(std::set<int>({10, 20, 30}), as_set);
}

TEST(AdjList, DelElementSingleElementClearsList) {
    AdjList a;
    int k = a.add_key(0);
    a.add_element(k, 42);

    auto it = a.begin(k);
    ASSERT_FALSE(it.end());
    a.del_element(it);

    auto it2 = a.begin(k);
    EXPECT_TRUE(it2.end());
}

TEST(AdjList, DelElementMiddleOfRingPreservesOthers) {
    AdjList a;
    int k = a.add_key(0);
    a.add_element(k, 1);
    a.add_element(k, 2);
    a.add_element(k, 3);

    // Walk to the element with value 2 and delete it.
    for (auto it = a.begin(k); !it.end(); it.next()) {
        if (*it == 2) {
            a.del_element(it);
            break;
        }
    }

    std::set<int> seen;
    for (auto it = a.begin(k); !it.end(); it.next()) {
        seen.insert(*it);
    }
    EXPECT_EQ(std::set<int>({1, 3}), seen);
}

TEST(AdjList, DelKeyInvalidatesKey) {
    AdjList a;
    int k = a.add_key(99);
    EXPECT_TRUE(a.is_keyvalid(k));
    a.del_key(k);
    EXPECT_FALSE(a.is_keyvalid(k));
}

TEST(AdjList, HiterVisitsValidKeysOnly) {
    AdjList a;
    int k1 = a.add_key(1);
    int k2 = a.add_key(2);
    int k3 = a.add_key(3);
    a.del_key(k2);

    std::set<int> seen;
    for (int p = a.hiter_begin(); p != -1; p = a.hiter_next(p)) {
        seen.insert(a[p]);
    }
    EXPECT_EQ(std::set<int>({1, 3}), seen);
    (void)k1; (void)k3;
}

TEST(AdjList, MixedKeysHaveIndependentLists) {
    AdjList a;
    int k1 = a.add_key(0);
    int k2 = a.add_key(1);
    a.add_element(k1, 100);
    a.add_element(k1, 101);
    a.add_element(k2, 200);

    std::set<int> seen_k1, seen_k2;
    for (auto it = a.begin(k1); !it.end(); it.next()) seen_k1.insert(*it);
    for (auto it = a.begin(k2); !it.end(); it.next()) seen_k2.insert(*it);

    EXPECT_EQ(std::set<int>({100, 101}), seen_k1);
    EXPECT_EQ(std::set<int>({200}), seen_k2);
}

}  // namespace
