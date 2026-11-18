/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */


#include <array>
#include <thread>
#include <vector>
#include "gtest/gtest.h"

#include "sandstone_test_utils.h"

#ifdef __x86_64__

class CodeBufferTestFixture : public ::testing::Test {
public:
    ManagedCodeBuffer  code_buffer;
    const int function_return_value = 0xaced;

    // Copies the following asm into the buffer:
    //    mov eax, 0xaced
    //    ret
    void copy_code_to_buffer(){
        uint8_t code[] = {0xb8, 0xed, 0xac, 0x00, 0x00, 0xc3};  // returns 0xaced
        memcpy(code_buffer.ptr(), code, std::size(code));
    }
};

TEST_F(CodeBufferTestFixture, SuccessfulAllocateReturnsTrue) {
    auto success = code_buffer.allocate(100);
    EXPECT_EQ( success, true ) << "Pointer was " << code_buffer.ptr();
    EXPECT_EQ( code_buffer.is_valid(), true ) << "Pointer was " << code_buffer.ptr();
}

TEST_F(CodeBufferTestFixture, UnsuccessfulAllocateReturnsFalse) {
    auto success = code_buffer.allocate(INTMAX_MAX);  // definitely won't allocate
    EXPECT_EQ( success, false ) << "Pointer was " << code_buffer.ptr();
    EXPECT_EQ( code_buffer.is_valid(), false ) << "Pointer was " << code_buffer.ptr();
}

TEST_F(CodeBufferTestFixture, CanWriteAndExecuteBuffer) {
    code_buffer.allocate(100);
    copy_code_to_buffer();
    code_buffer.set_executable();

    auto funcptr = (int(*)()) code_buffer.ptr();
    auto return_value = funcptr();

    EXPECT_EQ(return_value, function_return_value );
}

TEST_F(CodeBufferTestFixture, CannotExecuteUntilSetExecutable) {
    code_buffer.allocate(100);
    copy_code_to_buffer();

    auto funcptr = (int(*)()) code_buffer.ptr();
    EXPECT_DEATH(funcptr(), "");
}

TEST_F(CodeBufferTestFixture, AllocatingBufferAtParticularAddress) {
    // Address must be page aligned or mmap gives invalid address
    code_buffer.allocate_at((void *) 0x1234560000, 100);
    EXPECT_EQ((size_t) code_buffer.ptr(), 0x1234560000);
}

#endif // __x86_64__

// ======================================================================
// Page size tests -- the single canonical implementation lives in
// sandstone_page.h and reaches this file via sandstone_test_utils.h.
// These lock the contract the framework's ROUND_UP_TO_PAGE users
// (mmap/mprotect alignment, thread-stack guards) rely on.  Runs on
// all architectures.
// ======================================================================
TEST(PageSizeTest, MatchesSysconf)
{
    EXPECT_EQ(sandstone_page_size(), static_cast<unsigned>(sysconf(_SC_PAGESIZE)));
}

TEST(PageSizeTest, IsPowerOfTwo)
{
    unsigned ps = sandstone_page_size();
    EXPECT_NE(ps, 0u);
    EXPECT_EQ(ps & (ps - 1), 0u);
}

TEST(PageSizeTest, RoundUpToPageRoundsUpToNextPage)
{
    unsigned ps = sandstone_page_size();
    EXPECT_EQ(ROUND_UP_TO_PAGE(1), ps);           // sub-page rounds up to one page
    EXPECT_EQ(ROUND_UP_TO_PAGE(ps), ps);         // page-aligned value is unchanged
    EXPECT_EQ(ROUND_UP_TO_PAGE(ps + 1), 2 * ps); // first byte of page 2
}

TEST(PageSizeTest, RoundUpToAlignsToAnyPowerOfTwo)
{
    // ROUND_UP_TO is the general power-of-two alignment macro ROUND_UP_TO_PAGE
    // is built on; the framework also uses it with non-page alignments
    // (alignof in sandstone.cpp, 256 in resource.cpp, 1024U in child_debug.cpp).
    EXPECT_EQ(ROUND_UP_TO(0, 64), 0);            // zero stays zero
    EXPECT_EQ(ROUND_UP_TO(1, 64), 64);           // sub-align rounds up
    EXPECT_EQ(ROUND_UP_TO(64, 64), 64);          // aligned value unchanged
    EXPECT_EQ(ROUND_UP_TO(65, 64), 128);         // first byte past the boundary
    EXPECT_EQ(ROUND_UP_TO(255, 256), 256);
    EXPECT_EQ(ROUND_UP_TO(1024, 1024), 1024);
    EXPECT_EQ(ROUND_UP_TO(1025, 1024), 2048);
    // size_t operands, as at the alignof() call site (sandstone.cpp)
    EXPECT_EQ(ROUND_UP_TO(static_cast<size_t>(16385), static_cast<size_t>(4096)),
              static_cast<size_t>(20480));
}

// ======================================================================
// Weighted Test Picker Tests
// ======================================================================
class WeightedPickerTestFixture : public ::testing::Test {
protected:
    std::map<std::string, int> counts = {};
    WeightedPicker<std::string> * picker;

    void pick_items(int num_items){
        for (int i = 0; i < num_items; i++)
            counts[picker->pick()]++;
    }

    /* 各用例裸 new picker;不 delete 会被 ASan/LSan 记为泄漏 */
    void TearDown() override {
        delete picker;
    }
};

TEST_F(WeightedPickerTestFixture, Entry1_100_percent) {
    picker = new WeightedPicker<std::string>(
            {{"Op1", 0},
             {"Op2", 1}}
    );
    pick_items(1000);

    ASSERT_NEAR(   0, counts["Op1"], 0);
    ASSERT_NEAR(1000, counts["Op2"], 0);
}

TEST_F(WeightedPickerTestFixture, Entry0_100_percent) {
    picker = new WeightedPicker<std::string>(
            {{"Op1", 1},
             {"Op2", 0}}
    );
    pick_items(1000);

    ASSERT_NEAR(1000, counts["Op1"], 0);
    ASSERT_NEAR(   0, counts["Op2"], 0);
}

TEST_F(WeightedPickerTestFixture, Entry_0_1_is_50_50_percent) {
    picker = new WeightedPicker<std::string>(
            {{"Op1", 1},
             {"Op2", 1}}
    );
    pick_items(1000);

    ASSERT_NEAR(500, counts["Op1"], 100);
    ASSERT_NEAR(500, counts["Op2"], 100);
}

TEST_F(WeightedPickerTestFixture, Entry_0_1_is_33_66_percent) {
    picker = new WeightedPicker<std::string>(
            {{"Op1", 1},
             {"Op2", 2}}
    );
    pick_items(1000);

    ASSERT_NEAR(0.333333 * 1000, counts["Op1"], 100);
    ASSERT_NEAR(0.666666 * 1000, counts["Op2"], 100);
}

// ---- sandstone_page_size() (issue #446) ----
// The lazy-init cache must be race-free, must never cache a sysconf failure,
// and must fail loudly (direct stderr diagnostic + abort) on an unusable page size.

TEST(PageSize, ReturnsPositivePowerOfTwoMatchingSysconf) {
    unsigned ps = sandstone_page_size();
    EXPECT_GT(ps, 0u);
    EXPECT_EQ(ps & (ps - 1), 0u);
    EXPECT_EQ(ps, static_cast<unsigned>(sysconf(_SC_PAGESIZE)));
}

TEST(PageSize, ValidPageSizesAccepted) {
    EXPECT_TRUE(sandstone_is_valid_page_size(1));
    EXPECT_TRUE(sandstone_is_valid_page_size(4096));
    EXPECT_TRUE(sandstone_is_valid_page_size(65536));
}

TEST(PageSize, InvalidPageSizesRejected) {
    EXPECT_FALSE(sandstone_is_valid_page_size(0));
    EXPECT_FALSE(sandstone_is_valid_page_size(-1));          // sysconf() failure value
    EXPECT_FALSE(sandstone_is_valid_page_size(4097));         // not a power of two
    EXPECT_FALSE(sandstone_is_valid_page_size(0xFFFFFFFF));  // (unsigned)-1
}

TEST(PageSize, FatalOnUnusablePageSize) {
    EXPECT_DEATH(sandstone_page_size_fatal(-1, "sysconf(_SC_PAGESIZE)"), "unusable page size");
}

TEST(PageSize, ConcurrentCallsAgree) {
    std::array<unsigned, 8> results{};
    std::vector<std::thread> threads;
    for (auto &r : results)
        threads.emplace_back([&r] { r = sandstone_page_size(); });
    for (auto &t : threads)
        t.join();
    for (unsigned r : results) {
        EXPECT_EQ(r, results[0]);
        EXPECT_GT(r, 0u);
    }
}
