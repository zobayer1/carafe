#include <carafe/version.hpp>

#include <gtest/gtest.h>

namespace {

TEST(Version, MatchesTheConfiguredString) {
    EXPECT_EQ(carafe::version(), CARAFE_VERSION_STRING);
}

TEST(Version, IsNotEmpty) {
    EXPECT_FALSE(carafe::version().empty());
}

}  // namespace
