// OpenDisplay Tests — Entry point
// Uses Google Test with RapidCheck for property-based testing.

#include <gtest/gtest.h>
#include <rapidcheck.h>
#include <rapidcheck/gtest.h>

#include "DriverInterface.h"

// Sanity check: verify shared structs are correctly defined
TEST(DriverInterface, MonitorCreateParamsSize) {
    EXPECT_EQ(sizeof(MonitorCreateParams), 12u);
}

TEST(DriverInterface, MonitorCreateResultSize) {
    EXPECT_EQ(sizeof(MonitorCreateResult), 8u);
}

TEST(DriverInterface, MonitorResizeParamsSize) {
    EXPECT_EQ(sizeof(MonitorResizeParams), 12u);
}

// Sanity check: IOCTL codes are distinct
TEST(DriverInterface, IoctlCodesAreDistinct) {
    EXPECT_NE(IOCTL_CREATE_MONITOR, IOCTL_DESTROY_MONITOR);
    EXPECT_NE(IOCTL_CREATE_MONITOR, IOCTL_RESIZE_MONITOR);
    EXPECT_NE(IOCTL_DESTROY_MONITOR, IOCTL_RESIZE_MONITOR);
}

// Example property test: MonitorCreateParams fields are independent
RC_GTEST_PROP(DriverInterface, CreateParamsFieldsIndependent, ()) {
    auto width = *rc::gen::inRange<uint32_t>(640, 2733);
    auto height = *rc::gen::inRange<uint32_t>(480, 2049);
    auto refresh = *rc::gen::inRange<uint32_t>(30, 144);

    MonitorCreateParams params{width, height, refresh};

    RC_ASSERT(params.widthPixels == width);
    RC_ASSERT(params.heightPixels == height);
    RC_ASSERT(params.refreshHz == refresh);
}
