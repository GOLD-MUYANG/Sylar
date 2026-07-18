#ifndef __SYLAR_LOAD_BALANCE_CANDIDATE_SELECTOR_H__
#define __SYLAR_LOAD_BALANCE_CANDIDATE_SELECTOR_H__

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace sylar
{
namespace load_balance
{

// 公共策略与候选元数据访问入口。
enum class LoadBalanceStrategy
{
    ROUND_ROBIN = 0,
    RANDOM = 1,
    WEIGHTED_ROUND_ROBIN = 2,
    LEAST_CONNECTION = 3,
};

bool ParseLoadBalanceStrategy(const std::string &value, LoadBalanceStrategy *strategy);
const char *LoadBalanceStrategyToString(LoadBalanceStrategy strategy);

template <class T>
struct CandidateAccessors
{
    std::function<std::string(const T &)> key;
    std::function<bool(const T &)> available;
    std::function<uint32_t(const T &)> weight;
    std::function<uint32_t(const T &)> active_requests;
    // 可选：在 selector 内部状态锁持有期间占用候选，用于最少连接避免并发穿透。
    std::function<void(const T &)> on_selected;

    bool valid() const
    {
        return key && available && weight && active_requests;
    }
};

template <class T>
class CandidateSelector
{
public:
    typedef std::shared_ptr<CandidateSelector<T>> ptr;

    virtual ~CandidateSelector() {}

    virtual bool select(const std::string &pool_key,
                        const std::vector<T> &candidates,
                        const std::vector<std::string> &tried_keys,
                        T *selected) = 0;
};

namespace detail
{

inline std::unordered_set<std::string>
BuildTriedKeySet(const std::vector<std::string> &tried_keys)
{
    return std::unordered_set<std::string>(tried_keys.begin(), tried_keys.end());
}

template <class T>
bool IsEligible(const T &candidate,
                const CandidateAccessors<T> &accessors,
                const std::unordered_set<std::string> &tried_keys,
                std::string *key)
{
    if (!accessors.available(candidate))
    {
        return false;
    }
    const std::string candidate_key = accessors.key(candidate);
    if (candidate_key.empty() || tried_keys.find(candidate_key) != tried_keys.end())
    {
        return false;
    }
    if (key)
    {
        *key = candidate_key;
    }
    return true;
}

template <class T>
void CommitSelection(const T &candidate, const CandidateAccessors<T> &accessors, T *selected)
{
    *selected = candidate;
    if (accessors.on_selected)
    {
        accessors.on_selected(candidate);
    }
}

} // namespace detail

// 四种算法各自保护内部状态；访问器也在同一临界区内执行。
template <class T>
class RoundRobinSelector : public CandidateSelector<T>
{
public:
    explicit RoundRobinSelector(const CandidateAccessors<T> &accessors) : m_accessors(accessors) {}

    bool select(const std::string &pool_key,
                const std::vector<T> &candidates,
                const std::vector<std::string> &tried_keys,
                T *selected) override
    {
        if (!selected || !m_accessors.valid() || candidates.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::unordered_set<std::string> tried = detail::BuildTriedKeySet(tried_keys);
        size_t &next_index = m_nextIndexes[pool_key];
        next_index %= candidates.size();
        for (size_t offset = 0; offset < candidates.size(); ++offset)
        {
            const size_t index = (next_index + offset) % candidates.size();
            if (!detail::IsEligible(candidates[index], m_accessors, tried, nullptr))
            {
                continue;
            }
            detail::CommitSelection(candidates[index], m_accessors, selected);
            next_index = (index + 1) % candidates.size();
            return true;
        }
        return false;
    }

private:
    CandidateAccessors<T> m_accessors;
    std::map<std::string, size_t> m_nextIndexes;
    std::mutex m_mutex;
};

template <class T>
class RandomSelector : public CandidateSelector<T>
{
public:
    explicit RandomSelector(const CandidateAccessors<T> &accessors)
        : m_accessors(accessors), m_engine(std::random_device()())
    {
    }

    RandomSelector(const CandidateAccessors<T> &accessors, uint32_t seed)
        : m_accessors(accessors), m_engine(seed)
    {
    }

    bool select(const std::string &,
                const std::vector<T> &candidates,
                const std::vector<std::string> &tried_keys,
                T *selected) override
    {
        if (!selected || !m_accessors.valid() || candidates.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::unordered_set<std::string> tried = detail::BuildTriedKeySet(tried_keys);
        std::vector<size_t> eligible_indices;
        eligible_indices.reserve(candidates.size());
        for (size_t index = 0; index < candidates.size(); ++index)
        {
            if (detail::IsEligible(candidates[index], m_accessors, tried, nullptr))
            {
                eligible_indices.push_back(index);
            }
        }
        if (eligible_indices.empty())
        {
            return false;
        }

        std::uniform_int_distribution<size_t> distribution(0, eligible_indices.size() - 1);
        detail::CommitSelection(candidates[eligible_indices[distribution(m_engine)]], m_accessors,
                                selected);
        return true;
    }

private:
    CandidateAccessors<T> m_accessors;
    std::mt19937 m_engine;
    std::mutex m_mutex;
};

template <class T>
class WeightedRoundRobinSelector : public CandidateSelector<T>
{
public:
    explicit WeightedRoundRobinSelector(const CandidateAccessors<T> &accessors)
        : m_accessors(accessors)
    {
    }

    bool select(const std::string &pool_key,
                const std::vector<T> &candidates,
                const std::vector<std::string> &tried_keys,
                T *selected) override
    {
        if (!selected || !m_accessors.valid() || candidates.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::unordered_set<std::string> tried = detail::BuildTriedKeySet(tried_keys);
        WeightedState &state = m_states[pool_key];
        state.index %= candidates.size();

        for (size_t scanned = 0; scanned < candidates.size(); ++scanned)
        {
            const T &candidate = candidates[state.index];
            if (!detail::IsEligible(candidate, m_accessors, tried, nullptr))
            {
                Advance(state, candidates.size());
                continue;
            }

            const uint32_t weight = m_accessors.weight(candidate) == 0
                                        ? 1
                                        : m_accessors.weight(candidate);
            detail::CommitSelection(candidate, m_accessors, selected);
            ++state.returned;
            if (state.returned >= weight)
            {
                Advance(state, candidates.size());
            }
            return true;
        }
        return false;
    }

private:
    struct WeightedState
    {
        size_t index = 0;
        uint32_t returned = 0;
    };

    static void Advance(WeightedState &state, size_t candidate_count)
    {
        state.index = (state.index + 1) % candidate_count;
        state.returned = 0;
    }

private:
    CandidateAccessors<T> m_accessors;
    std::map<std::string, WeightedState> m_states;
    std::mutex m_mutex;
};

template <class T>
class LeastConnectionSelector : public CandidateSelector<T>
{
public:
    explicit LeastConnectionSelector(const CandidateAccessors<T> &accessors)
        : m_accessors(accessors)
    {
    }

    bool select(const std::string &,
                const std::vector<T> &candidates,
                const std::vector<std::string> &tried_keys,
                T *selected) override
    {
        if (!selected || !m_accessors.valid() || candidates.empty())
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        const std::unordered_set<std::string> tried = detail::BuildTriedKeySet(tried_keys);
        const T *best = nullptr;
        uint32_t minimum_active = 0;
        for (const auto &candidate : candidates)
        {
            if (!detail::IsEligible(candidate, m_accessors, tried, nullptr))
            {
                continue;
            }
            const uint32_t active = m_accessors.active_requests(candidate);
            if (!best || active < minimum_active)
            {
                best = &candidate;
                minimum_active = active;
            }
        }
        if (!best)
        {
            return false;
        }
        detail::CommitSelection(*best, m_accessors, selected);
        return true;
    }

private:
    CandidateAccessors<T> m_accessors;
    std::mutex m_mutex;
};

// Factory 统一拒绝不完整访问器，避免策略切换后调用空 function。
template <class T>
typename CandidateSelector<T>::ptr
CreateCandidateSelector(LoadBalanceStrategy strategy, const CandidateAccessors<T> &accessors)
{
    if (!accessors.valid())
    {
        return typename CandidateSelector<T>::ptr();
    }

    switch (strategy)
    {
    case LoadBalanceStrategy::RANDOM:
        return typename CandidateSelector<T>::ptr(new RandomSelector<T>(accessors));
    case LoadBalanceStrategy::WEIGHTED_ROUND_ROBIN:
        return typename CandidateSelector<T>::ptr(new WeightedRoundRobinSelector<T>(accessors));
    case LoadBalanceStrategy::LEAST_CONNECTION:
        return typename CandidateSelector<T>::ptr(new LeastConnectionSelector<T>(accessors));
    case LoadBalanceStrategy::ROUND_ROBIN:
        return typename CandidateSelector<T>::ptr(new RoundRobinSelector<T>(accessors));
    default:
        return typename CandidateSelector<T>::ptr();
    }
}

} // namespace load_balance
} // namespace sylar

#endif
