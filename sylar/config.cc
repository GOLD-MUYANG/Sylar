#include "config.h"
#include "sylar/log.h"
#include "thread.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <utility>
#include <yaml-cpp/node/node.h>
namespace sylar
{

ConfigVarBase::ptr Config::LookupBase(const std::string &name)
{
    RWMutexType::ReadLock lock(GetMutex());
    auto it = GetDatas().find(name);
    if (it == GetDatas().end())
    {
        return nullptr;
    }
    return it->second;
}
// 把层次的结构变成 A.B.C.D 这样的平级结构，用于后续直接拿取
static void ListAllMember(const std::string &prefix,
                          const YAML::Node &node,
                          std::list<std::pair<std::string, const YAML::Node>> &output)
{
    if (prefix.find_first_not_of("abcdefghijklmnopqrstuv"
                                 "wxyz0123456789_.") != std::string::npos)
    {
        SYLAR_LOG_ERROR(SYLAR_LOG_ROOT()) << "Config invalid name: " << prefix << " : " << node;
        return;
    }
    output.push_back(std::make_pair(prefix, node));
    if (node.IsMap())
    {
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            ListAllMember(prefix.empty() ? it->first.Scalar() : prefix + "." + it->first.Scalar(),
                          it->second, output);
        }
    }
}

void Config::LoadFromYaml(const YAML::Node &root)
{
    std::list<std::pair<std::string, const YAML::Node>> all_Nodes;
    ListAllMember("", root, all_Nodes);
    for (auto &i : all_Nodes)
    {
        std::string key = i.first;
        if (key.empty())
        {
            continue;
        }

        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        ConfigVarBase::ptr var = Config::LookupBase(key);
        if (var)
        {
            if (i.second.IsScalar())
            {
                var->fromString(i.second.Scalar());
            }
            else
            {
                std::stringstream ss;
                ss << i.second;
                var->fromString(ss.str());
            }
        }
    }
}

void Config::Visit(std::function<void(ConfigVarBase::ptr)> cb)
{
    RWMutexType::ReadLock lock(GetMutex());
    ConfigVarMap &m = GetDatas();
    for (auto it = m.begin(); it != m.end(); it++)
    {
        std::cout << it->second << std::endl;
        cb(it->second);
    }
}

} // namespace sylar