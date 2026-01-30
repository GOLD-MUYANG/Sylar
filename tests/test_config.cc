#include "sylar/config.h"
#include "sylar/log.h"
#include <iostream>
#include <string>
#include <vector>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/yaml.h>

sylar::ConfigVar<int>::ptr g_int_value_config =
    sylar::Config::Lookup("system.port", (int)8080, "system port");
sylar::ConfigVar<std::vector<int>>::ptr g_int_vec_value_config =
    sylar::Config::Lookup("system.int_vec", std::vector<int>{1, 2}, "system int vec");

sylar::ConfigVar<std::list<int>>::ptr g_int_list_value_config =
    sylar::Config::Lookup("system.int_list", std::list<int>{1, 2}, "system int list");

sylar::ConfigVar<std::set<int>>::ptr g_int_set_value_config =
    sylar::Config::Lookup("system.int_set", std::set<int>{1, 2}, "system int set");

sylar::ConfigVar<std::unordered_set<int>>::ptr g_int_uset_value_config =
    sylar::Config::Lookup("system.int_uset", std::unordered_set<int>{1, 2}, "system int uset");

sylar::ConfigVar<std::map<std::string, int>>::ptr g_str_int_map_value_config =
    sylar::Config::Lookup(
        "system.str_int_map", std::map<std::string, int>{{"k", 2}}, "system str int map");

sylar::ConfigVar<std::unordered_map<std::string, int>>::ptr g_str_int_umap_value_config =
    sylar::Config::Lookup("system.str_int_umap",
                          std::unordered_map<std::string, int>{{"k", 2}},
                          "system str int map");

//这里的level代表的是层级，而不是日志的级别
void print_yaml(const YAML::Node &node, int level)
{

    if (node.IsMap())
    {
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << std::string(level * 4, ' ') << it->first << "-"
                                             << it->second.Type() << "-" << level;
            print_yaml(it->second, level + 1);
        }
    }
    else if (node.IsSequence())
    {
        for (size_t i = 0; i < node.size(); ++i)
        {
            SYLAR_LOG_INFO(SYLAR_LOG_ROOT())
                << std::string(level * 4, ' ') << i << "-" << node[i].Type() << "-" << level;
            print_yaml(node[i], level + 1);
        }
    }
    else if (node.IsScalar())
    {
        SYLAR_LOG_INFO(SYLAR_LOG_ROOT())
            << std::string(level * 4, ' ') << node.Scalar() << "-" << node.Type() << "-" << level;
    }
    else if (node.IsNull())
    {
        SYLAR_LOG_INFO(SYLAR_LOG_ROOT())
            << std::string(level * 4, ' ') << "NULL - " << node.Type() << "-" << level;
    }
}
void test_yaml()
{
    YAML::Node root = YAML::LoadFile("/home/sylar/workspace/sylar/bin/conf/test.yml");
    print_yaml(root, 0);
    SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << root;
}

void test_config()
{
    SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << "before: " << g_int_value_config->getValue();
#define XX(g_var, name, prefix)                                                                    \
    {                                                                                              \
        auto &v = g_var->getValue();                                                               \
        for (auto &i : v)                                                                          \
        {                                                                                          \
            SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << #prefix " " #name ": " << i;                       \
        }                                                                                          \
        SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << #prefix " " #name " yaml: " << g_var->toString();      \
    }

#define XX_M(g_var, name, prefix)                                                                  \
    {                                                                                              \
        auto &v = g_var->getValue();                                                               \
        for (auto &i : v)                                                                          \
        {                                                                                          \
            SYLAR_LOG_INFO(SYLAR_LOG_ROOT())                                                       \
                << #prefix " " #name ": {" << i.first << " - " << i.second << "}";                 \
        }                                                                                          \
        SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << #prefix " " #name " yaml: " << g_var->toString();      \
    }

    XX(g_int_vec_value_config, int_vec, before);
    XX(g_int_list_value_config, int_list, before);
    XX(g_int_set_value_config, int_set, before);
    XX(g_int_uset_value_config, int_uset, before);
    XX_M(g_str_int_map_value_config, str_int_map, before);
    XX_M(g_str_int_umap_value_config, str_int_umap, before);

    YAML::Node root = YAML::LoadFile("/home/sylar/workspace/sylar/bin/conf/test.yml");
    sylar::Config::LoadFromYaml(root);

    XX(g_int_vec_value_config, int_vec, after);
    XX(g_int_list_value_config, int_list, after);
    XX(g_int_set_value_config, int_set, after);
    XX(g_int_uset_value_config, int_uset, after);
    XX_M(g_str_int_map_value_config, str_int_map, after);
    XX_M(g_str_int_umap_value_config, str_int_umap, after);
}

void test_log()
{
    static sylar::Logger::ptr system_log = SYLAR_LOG_NAME("system");
    SYLAR_LOG_INFO(system_log) << "hello sylar system log" << std::endl;
    std::cout << sylar::LoggerMgr::GetInstance()->toYamlString() << std::endl;
    YAML::Node root = YAML::LoadFile("/home/sylar/workspace/sylar/bin/conf/log.yml");
    sylar::Config::LoadFromYaml(root);
    // SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << "LoadFromYaml finished, now check logs config exist?";
    // auto logs_var = sylar::Config::LookupBase("logs");
    // if (logs_var)
    // {
    //     SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << "logs config found: " << logs_var->toString();
    // }
    // else
    // {
    //     SYLAR_LOG_ERROR(SYLAR_LOG_ROOT()) << "logs config NOT registered!";
    // }
    std::cout << "=========================" << std::endl;
    std::cout << sylar::LoggerMgr::GetInstance()->toYamlString() << std::endl;
    SYLAR_LOG_INFO(system_log) << "hello sylar system log" << std::endl;
}

int main(int argc, char **argv)
{
    // SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << g_int_value_config->getValue();
    // SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << g_int_value_config->toString();

    // test_yaml();
    // test_config();
    test_log();
    std::cout << "=========================" << std::endl;
    sylar::Config::Visit(
        [](sylar::ConfigVarBase::ptr var)
        {
            SYLAR_LOG_INFO(SYLAR_LOG_ROOT())
                << "name=" << var->getName() << " description=" << var->getDescription()
                << " typename=" << var->getTypeName() << " value=" << var->toString();
        });
    std::cout << "=========================" << std::endl;
    return 0;
}