#include <Lazy.hpp>
#include <Optional.hpp>
#include <Range.hpp>
#include <ScopeGuard.hpp>
#include <Timer.hpp>
#include <doctest/doctest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_name.hpp>

TEST_SUITE("Cosmos::Optional")
{
    TEST_CASE("Empty state")
    {
        Optional<int> opt;
        REQUIRE(opt.isInit() == false);
    }

    TEST_CASE("Value construction")
    {
        Optional<int> opt(42);
        REQUIRE(opt.isInit() == true);
        REQUIRE(*opt == 42);
    }

    TEST_CASE("Emplace")
    {
        Optional<std::string> opt;
        opt.emplace(5, 'x');
        REQUIRE(opt.isInit() == true);
        REQUIRE(*opt == "xxxxx");
    }

    TEST_CASE("Copy constructor")
    {
        Optional<int> opt1(100);
        Optional<int> opt2(opt1);
        REQUIRE(opt2.isInit() == true);
        REQUIRE(*opt2 == 100);
    }

    TEST_CASE("Move constructor")
    {
        Optional<std::string> opt1(std::string("hello"));
        Optional<std::string> opt2(std::move(opt1));
        REQUIRE(opt2.isInit() == true);
    }

    TEST_CASE("Assignment operator")
    {
        Optional<int> opt1;
        Optional<int> opt2(50);
        opt1 = opt2;
        REQUIRE(opt1.isInit() == true);
        REQUIRE(*opt1 == 50);
    }

    TEST_CASE("Reset to empty")
    {
        Optional<int> opt(123);
        REQUIRE(opt.isInit() == true);
        opt = Optional<int>();
        REQUIRE(opt.isInit() == false);
    }

    TEST_CASE("Throw on dereference empty")
    {
        Optional<int> opt;
        REQUIRE_THROWS(*opt);
    }

    TEST_CASE("Comparison - equal")
    {
        Optional<int> opt1(10);
        Optional<int> opt2(10);
        REQUIRE(opt1 == opt2);
    }

    TEST_CASE("Comparison - not equal")
    {
        Optional<int> opt1(10);
        Optional<int> opt2(20);
        REQUIRE(opt1 != opt2);
    }

    TEST_CASE("Comparison - less than")
    {
        Optional<int> opt1(10);
        Optional<int> opt2(20);
        REQUIRE(opt1 < opt2);
    }

    TEST_CASE("Comparison - empty equals empty")
    {
        Optional<int> opt1;
        Optional<int> opt2;
        REQUIRE(opt1 == opt2);
    }

    TEST_CASE("Boolean conversion")
    {
        Optional<int> empty;
        Optional<int> filled(1);
        REQUIRE_FALSE(static_cast<bool>(empty));
        REQUIRE(static_cast<bool>(filled));
    }
}

TEST_SUITE("Cosmos::Lazy")
{
    TEST_CASE("Deferred initialization")
    {
        int constructionCount = 0;
        auto factory          = [&constructionCount]() -> std::shared_ptr<int> {
            constructionCount++;
            return std::make_shared<int>(42);
        };

        Lazy<std::shared_ptr<int>> lazy(factory);

        REQUIRE(constructionCount == 0);
        REQUIRE(lazy.IsValueCreated() == false);

        auto &value = lazy.Value();

        REQUIRE(constructionCount == 1);
        REQUIRE(lazy.IsValueCreated() == true);
        REQUIRE(*value == 42);

        auto &value2 = lazy.Value();
        REQUIRE(constructionCount == 1);
        (void)value2;
    }

    TEST_CASE("Lazy returns reference")
    {
        auto factory = []() -> int { return 100; };

        Lazy<int> lazy(factory);
        REQUIRE(lazy.IsValueCreated() == false);

        int &value = lazy.Value();
        REQUIRE(lazy.IsValueCreated() == true);
        REQUIRE(value == 100);

        value       = 200;
        int &value2 = lazy.Value();
        REQUIRE(value2 == 200);
    }
}

TEST_SUITE("Cosmos::ScopeGuard")
{
    TEST_CASE("MakeGuard creates scope guard")
    {
        auto guard = MakeGuard([]() {});
        (void)guard;
    }

    TEST_CASE("Dismiss works")
    {
        auto guard = MakeGuard([]() {});
        guard.Dismiss();
    }
}

TEST_SUITE("Cosmos::Range")
{
    TEST_CASE("Range with positive step")
    {
        int sum = 0;
        for (int i : Range(0, 10, 2)) {
            sum += i;
        }
        REQUIRE(sum == 20);
    }

    TEST_CASE("Range with default step")
    {
        int sum = 0;
        for (int i : Range(0, 5)) {
            sum += i;
        }
        REQUIRE(sum == 10);
    }

    TEST_CASE("Range with single argument")
    {
        int count = 0;
        for (int i : Range(5)) {
            (void)i;
            count++;
        }
        REQUIRE(count == 5);
    }

    TEST_CASE("Range with negative step")
    {
        int sum = 0;
        for (int i : Range(10, 0, -2)) {
            sum += i;
        }
        REQUIRE(sum == 30);
    }

    TEST_CASE("Range size")
    {
        REQUIRE(Range(0, 10, 2).size() == 5);
        REQUIRE(Range(0, 5).size() == 5);
        REQUIRE(Range(5).size() == 5);
    }

    TEST_CASE("Range operator access")
    {
        REQUIRE(Range(0, 10, 2)[0] == 0);
        REQUIRE(Range(0, 10, 2)[1] == 2);
        REQUIRE(Range(0, 10, 2)[4] == 8);
    }
}

TEST_SUITE("Cosmos::Timer")
{
    TEST_CASE("Timer default construction")
    {
        Timer timer;
        int64_t elapsed = timer.elapsed();
        CHECK(elapsed >= 0);
    }

    TEST_CASE("Timer reset")
    {
        Timer timer;
        timer.elapsed<Timer::millisecond_t>();
        timer.reset();
        int64_t elapsed = timer.elapsed();
        CHECK(elapsed == 0);
    }

    TEST_CASE("Timer getStartTimestamp")
    {
        Timer timer;
        std::time_t ts = timer.getStartTimestamp();
        CHECK(ts > 0);
    }

    TEST_CASE("Timer getCurrentTimestamp")
    {
        std::time_t ts = Timer::getCurrentTimestamp();
        CHECK(ts > 0);
    }

    TEST_CASE("Timer getCurrentLocalTime")
    {
        Timer::Date date = Timer::getCurrentLocalTime();
        CHECK(date.year > 0);
        CHECK(date.month >= 1);
        CHECK(date.month <= 12);
    }

    TEST_CASE("Timer getCurrentTimePoint")
    {
        size_t tp = Timer::getCurrentTimePoint<Timer::millisecond_t>();
        CHECK(tp > 0);
    }

    TEST_CASE("Timer Date to_str")
    {
        Timer::Date date;
        date.year        = 2024;
        date.month       = 1;
        date.day         = 15;
        date.weekday     = 1;
        date.hour        = 10;
        date.minute      = 30;
        date.second      = 45;
        date.millisecond = 123;

        std::string str = date.to_str();
        CHECK(str.empty() == false);
    }
}

TEST_SUITE("Cosmos::type_name")
{
    TEST_CASE("type_name for int")
    {
        std::string name = type_name<int>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for const int")
    {
        std::string name = type_name<const int>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for int reference")
    {
        std::string name = type_name<int &>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for const reference")
    {
        std::string name = type_name<const int &>();
        CHECK(name.empty() == false);
    }

    TEST_CASE("type_name for volatile")
    {
        std::string name = type_name<volatile int>();
        CHECK(name.empty() == false);
    }
}
