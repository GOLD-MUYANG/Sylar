#include "sylar/config.h"
#include "sylar/env.h"
#include "sylar/log.h"
#include "sylar/util.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
namespace sylar
{

sylar::Logger::ptr g_logger = SYLAR_LOG_NAME("system");
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

static std::map<std::string, uint64_t> s_file2modifytime;
static sylar::Mutex s_mutex;

bool Config::LoadFromConfDir(const std::string &path, bool force)
{
    std::string absoulte_path = sylar::EnvMgr::GetInstance()->getAbsolutePath(path);
    struct stat path_stat;
    if (stat(absoulte_path.c_str(), &path_stat) != 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "LoadConfDir path=" << absoulte_path
                                  << " does not exist or is inaccessible errno=" << errno
                                  << " errstr=" << strerror(errno);
        return false;
    }
    if (!S_ISDIR(path_stat.st_mode) || access(absoulte_path.c_str(), R_OK | X_OK) != 0)
    {
        SYLAR_LOG_ERROR(g_logger) << "LoadConfDir path=" << absoulte_path
                                  << " is not a readable directory";
        return false;
    }

    std::vector<std::string> files;
    FSUtil::ListAllFile(files, absoulte_path, ".yml");
    if (files.empty())
    {
        SYLAR_LOG_ERROR(g_logger) << "LoadConfDir path=" << absoulte_path
                                  << " contains no YAML configuration files";
        return false;
    }

    bool success = true;
    for (auto &i : files)
    {
        {
            struct stat st;
            if (lstat(i.c_str(), &st) != 0)
            {
                SYLAR_LOG_ERROR(g_logger) << "LoadConfFile file=" << i
                                          << " stat failed errno=" << errno
                                          << " errstr=" << strerror(errno);
                success = false;
                continue;
            }
            sylar::Mutex::Lock lock(s_mutex);
            if (!force && s_file2modifytime[i] == (uint64_t)st.st_mtime)
            {
                continue;
            }
            s_file2modifytime[i] = st.st_mtime;
        }
        try
        {
            YAML::Node root = YAML::LoadFile(i);
            LoadFromYaml(root);
            SYLAR_LOG_INFO(g_logger) << "LoadConfFile file=" << i << " ok";
        }
        catch (const std::exception &e)
        {
            SYLAR_LOG_ERROR(g_logger) << "LoadConfFile file=" << i << " failed: " << e.what();
            success = false;
        }
        catch (...)
        {
            SYLAR_LOG_ERROR(g_logger) << "LoadConfFile file=" << i << " failed";
            success = false;
        }
    }
    return success;
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
