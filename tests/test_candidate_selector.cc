#include "sylar/load_balance/candidate_selector.h"

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{

std::atomic<int> g_failures(0);

#define EXPECT_TRUE(expr)                                                                     \
    do                                                                                        \
    {                                                                                         \
        if (!(expr))                                                                          \
        {                                                                                     \
            std::fprintf(stderr, "EXPECT_TRUE failed line=%d\n", __LINE__);                  \
            ++g_failures;                                                                     \
        }                                                                                     \
    } while (0)

#define EXPECT_EQ(lhs, rhs)                                                                   \
    do                                                                                        \
    {                                                                                         \
        auto _lhs = (lhs);                                                                    \
        auto _rhs = (rhs);                                                                    \
        if (!(_lhs == _rhs))                                                                  \
        {                                                                                     \
            std::fprintf(stderr, "EXPECT_EQ failed line=%d\n", __LINE__);                    \
            ++g_failures;                                                                     \
        }                                                                                     \
    } while (0)

struct TestCandidate
{
    std::string key;
    bool available = true;
    uint32_t weight = 1;
    std::shared_ptr<std::atomic<uint32_t>> active;
};

TestCandidate MakeCandidate(const std::string &key,
                            bool available = true,
                            uint32_t weight = 1,
                            uint32_t active = 0)
{
    TestCandidate candidate;
    candidate.key = key;
    candidate.available = available;
    candidate.weight = weight;
    candidate.active.reset(new std::atomic<uint32_t>(active));
    return candidate;
}

sylar::load_balance::CandidateAccessors<TestCandidate> MakeAccessors()
{
    sylar::load_balance::CandidateAccessors<TestCandidate> accessors;
    accessors.key = [](const TestCandidate &candidate) { return candidate.key; };
    accessors.available = [](const TestCandidate &candidate) { return candidate.available; };
    accessors.weight = [](const TestCandidate &candidate) { return candidate.weight; };
    accessors.active_requests =
        [](const TestCandidate &candidate) { return candidate.active->load(); };
    return accessors;
}

std::string SelectKey(sylar::load_balance::CandidateSelector<TestCandidate> &selector,
                      const std::string &pool_key,
                      const std::vector<TestCandidate> &candidates,
                      const std::vector<std::string> &tried_keys = {})
{
    TestCandidate selected;
    return selector.select(pool_key, candidates, tried_keys, &selected) ? selected.key : "";
}

void TestStrategyParsing()
{
    using sylar::load_balance::LoadBalanceStrategy;
    LoadBalanceStrategy strategy = LoadBalanceStrategy::ROUND_ROBIN;
    EXPECT_TRUE(sylar::load_balance::ParseLoadBalanceStrategy("RANDOM", &strategy));
    EXPECT_EQ((int)strategy, (int)LoadBalanceStrategy::RANDOM);
    EXPECT_TRUE(!sylar::load_balance::ParseLoadBalanceStrategy("random", &strategy));
    EXPECT_TRUE(!sylar::load_balance::ParseLoadBalanceStrategy("ROUND_ROBIN", nullptr));
    EXPECT_EQ(std::string(sylar::load_balance::LoadBalanceStrategyToString(
                  LoadBalanceStrategy::LEAST_CONNECTION)),
              std::string("LEAST_CONNECTION"));
}

void TestRoundRobinAndPoolIsolation()
{
    sylar::load_balance::RoundRobinSelector<TestCandidate> selector(MakeAccessors());
    std::vector<TestCandidate> candidates = {MakeCandidate("A"), MakeCandidate("B")};

    EXPECT_EQ(SelectKey(selector, "pool-a", candidates), std::string("A"));
    EXPECT_EQ(SelectKey(selector, "pool-a", candidates), std::string("B"));
    EXPECT_EQ(SelectKey(selector, "pool-b", candidates), std::string("A"));
    EXPECT_EQ(SelectKey(selector, "pool-a", candidates), std::string("A"));
}

void TestCandidateFilteringAndBoundaries()
{
    sylar::load_balance::RoundRobinSelector<TestCandidate> selector(MakeAccessors());
    std::vector<TestCandidate> candidates = {
        MakeCandidate("", true), MakeCandidate("down", false), MakeCandidate("ready", true)};

    EXPECT_EQ(SelectKey(selector, "pool", candidates), std::string("ready"));
    EXPECT_EQ(SelectKey(selector, "pool", candidates, {"ready"}), std::string(""));
    EXPECT_EQ(SelectKey(selector, "pool", {}), std::string(""));
    EXPECT_TRUE(!selector.select("pool", candidates, {}, nullptr));

    sylar::load_balance::CandidateAccessors<TestCandidate> invalid;
    invalid.key = [](const TestCandidate &candidate) { return candidate.key; };
    EXPECT_TRUE(!invalid.valid());
    EXPECT_TRUE(!sylar::load_balance::CreateCandidateSelector<TestCandidate>(
        sylar::load_balance::LoadBalanceStrategy::ROUND_ROBIN, invalid));
    EXPECT_TRUE(!sylar::load_balance::CreateCandidateSelector<TestCandidate>(
        static_cast<sylar::load_balance::LoadBalanceStrategy>(999), MakeAccessors()));
}

void TestRandomSelectorUsesEligibleCandidatesAndSeed()
{
    std::vector<TestCandidate> candidates = {MakeCandidate("down", false), MakeCandidate("A"),
                                             MakeCandidate("B"), MakeCandidate("tried")};
    sylar::load_balance::RandomSelector<TestCandidate> first(MakeAccessors(), 12345);
    sylar::load_balance::RandomSelector<TestCandidate> second(MakeAccessors(), 12345);

    for (size_t i = 0; i < 32; ++i)
    {
        const std::string first_key = SelectKey(first, "pool", candidates, {"tried"});
        const std::string second_key = SelectKey(second, "pool", candidates, {"tried"});
        EXPECT_TRUE(first_key == "A" || first_key == "B");
        EXPECT_EQ(first_key, second_key);
    }
}

void TestWeightedRoundRobinSequenceAndZeroWeight()
{
    sylar::load_balance::WeightedRoundRobinSelector<TestCandidate> selector(MakeAccessors());
    std::vector<TestCandidate> candidates = {MakeCandidate("A", true, 2),
                                             MakeCandidate("B", true, 1)};
    const std::vector<std::string> expected = {"A", "A", "B", "A", "A", "B"};
    for (const auto &key : expected)
    {
        EXPECT_EQ(SelectKey(selector, "weighted", candidates), key);
    }

    sylar::load_balance::WeightedRoundRobinSelector<TestCandidate> zero_selector(MakeAccessors());
    std::vector<TestCandidate> zero_candidates = {MakeCandidate("A", true, 0),
                                                  MakeCandidate("B", true, 1)};
    EXPECT_EQ(SelectKey(zero_selector, "zero", zero_candidates), std::string("A"));
    EXPECT_EQ(SelectKey(zero_selector, "zero", zero_candidates), std::string("B"));
}

void TestLeastConnectionReadsLatestActiveCount()
{
    sylar::load_balance::LeastConnectionSelector<TestCandidate> selector(MakeAccessors());
    std::vector<TestCandidate> candidates = {MakeCandidate("A", true, 1),
                                             MakeCandidate("B", true, 2)};
    EXPECT_EQ(SelectKey(selector, "pool", candidates), std::string("A"));

    candidates[0].active->store(3);
    candidates[1].active->store(0);
    EXPECT_EQ(SelectKey(selector, "pool", candidates), std::string("B"));

    candidates[0].active->store(0);
    EXPECT_EQ(SelectKey(selector, "pool", candidates), std::string("A"));
}

void TestLeastConnectionReservesInsideSelectionLock()
{
    auto accessors = MakeAccessors();
    accessors.on_selected = [](const TestCandidate &candidate) { ++(*candidate.active); };
    sylar::load_balance::LeastConnectionSelector<TestCandidate> selector(accessors);
    std::vector<TestCandidate> candidates = {MakeCandidate("A"), MakeCandidate("B")};

    std::string selected[2];
    std::thread first([&]() { selected[0] = SelectKey(selector, "pool", candidates); });
    std::thread second([&]() { selected[1] = SelectKey(selector, "pool", candidates); });
    first.join();
    second.join();

    EXPECT_TRUE((selected[0] == "A" && selected[1] == "B") ||
                (selected[0] == "B" && selected[1] == "A"));
    EXPECT_EQ(candidates[0].active->load(), (uint32_t)1);
    EXPECT_EQ(candidates[1].active->load(), (uint32_t)1);
}

void TestConcurrentRoundRobinState()
{
    sylar::load_balance::RoundRobinSelector<TestCandidate> selector(MakeAccessors());
    std::vector<TestCandidate> candidates = {MakeCandidate("A"), MakeCandidate("B")};
    std::atomic<uint32_t> selected_a(0);
    std::atomic<uint32_t> selected_b(0);
    std::vector<std::thread> threads;
    for (size_t thread_index = 0; thread_index < 8; ++thread_index)
    {
        threads.push_back(std::thread([&selector, &candidates, &selected_a, &selected_b]() {
            for (size_t i = 0; i < 100; ++i)
            {
                const std::string key = SelectKey(selector, "shared", candidates);
                if (key == "A")
                {
                    ++selected_a;
                }
                else if (key == "B")
                {
                    ++selected_b;
                }
                else
                {
                    ++g_failures;
                }
            }
        }));
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(selected_a.load(), (uint32_t)400);
    EXPECT_EQ(selected_b.load(), (uint32_t)400);
}

void TestConcurrentWeightedState()
{
    sylar::load_balance::WeightedRoundRobinSelector<TestCandidate> selector(MakeAccessors());
    std::vector<TestCandidate> candidates = {MakeCandidate("A", true, 3),
                                             MakeCandidate("B", true, 1)};
    std::atomic<uint32_t> selected_a(0);
    std::atomic<uint32_t> selected_b(0);
    std::vector<std::thread> threads;
    for (size_t thread_index = 0; thread_index < 8; ++thread_index)
    {
        threads.push_back(std::thread([&selector, &candidates, &selected_a, &selected_b]() {
            for (size_t i = 0; i < 100; ++i)
            {
                const std::string key = SelectKey(selector, "shared", candidates);
                if (key == "A")
                {
                    ++selected_a;
                }
                else if (key == "B")
                {
                    ++selected_b;
                }
                else
                {
                    ++g_failures;
                }
            }
        }));
    }
    for (auto &thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(selected_a.load(), (uint32_t)600);
    EXPECT_EQ(selected_b.load(), (uint32_t)200);
}

} // namespace

int main()
{
    TestStrategyParsing();
    TestRoundRobinAndPoolIsolation();
    TestCandidateFilteringAndBoundaries();
    TestRandomSelectorUsesEligibleCandidatesAndSeed();
    TestWeightedRoundRobinSequenceAndZeroWeight();
    TestLeastConnectionReadsLatestActiveCount();
    TestLeastConnectionReservesInsideSelectionLock();
    TestConcurrentRoundRobinState();
    TestConcurrentWeightedState();
    return g_failures == 0 ? 0 : 1;
}
