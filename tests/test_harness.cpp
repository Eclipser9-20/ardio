#include "harness.h"

TEST(harness_reports_equality) {
    CHECK_EQ(2 + 2, 4);
    CHECK(true);
}
