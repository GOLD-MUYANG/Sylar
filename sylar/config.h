#ifndef __SYLAR_CONFIG_H__
#define __SYLAR_CONFIG_H__
#include "sylar/log.h"
#include <algorithm>
#include <boost/lexical_cast.hpp>
#include <cctype>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <yaml-cpp/node/node.h>
#include <yaml-cpp/node/parse.h>
#include <yaml-cpp/yaml.h>
namespace sylar
{

class ConfigVarBase
{
public:
    typedef std::shared_ptr<ConfigVarBase> ptr;
    ConfigVarBase(const std::string &name, const std::string &description = "")
        : m_name(name), m_description(description)
    {
        std::transform(m_name.begin(), m_name.end(), m_name.begin(), ::tolower);
    }

    virtual ~ConfigVarBase() {}

    const std::string &getName() const { return m_name; }

    const std::string &getDescription() const { return m_description; }

    virtual std::string toString() = 0;
    virtual bool fromString(const std::string &val) = 0;
    virtual std::string getTypeName() const = 0;

protected:
    std::string m_name;
    std::string m_description;
};

//基础类型的转换
//      F fromType   T toType
template <class F, class T>
class LexicalCast
{
public:
    T operator()(const F &v) const { return boost::lexical_cast<T>(v); }
};

//其他类型的偏特化
template <class T>
class LexicalCast<std::string, std::vector<T>>
{
public:
    std::vector<T> operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        typename std::vector<T> vec;
        std::stringstream ss;
        for (size_t i = 0; i < node.size(); i++)
        {
            ss.str("");
            ss << node[i];
            vec.push_back(LexicalCast<std::string, T>()(ss.str()));
        }
        return vec;
    }
};

template <class T>
class LexicalCast<std::vector<T>, std::string>
{
public:
    std::string operator()(const std::vector<T> &v)
    {
        YAML::Node node;
        for (auto &i : v)
        {
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

template <class T>
class LexicalCast<std::string, std::list<T>>
{
public:
    std::list<T> operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        typename std::list<T> vec;
        std::stringstream ss;
        for (size_t i = 0; i < node.size(); ++i)
        {
            ss.str("");
            ss << node[i];
            vec.push_back(LexicalCast<std::string, T>()(ss.str()));
        }
        return vec;
    }
};

template <class T>
class LexicalCast<std::list<T>, std::string>
{
public:
    std::string operator()(const std::list<T> &v)
    {
        YAML::Node node;
        for (auto &i : v)
        {
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

template <class T>
class LexicalCast<std::string, std::set<T>>
{
public:
    std::set<T> operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        typename std::set<T> vec;
        std::stringstream ss;
        for (size_t i = 0; i < node.size(); ++i)
        {
            ss.str("");
            ss << node[i];
            vec.insert(LexicalCast<std::string, T>()(ss.str()));
        }
        return vec;
    }
};

template <class T>
class LexicalCast<std::set<T>, std::string>
{
public:
    std::string operator()(const std::set<T> &v)
    {
        YAML::Node node;
        for (auto &i : v)
        {
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

template <class T>
class LexicalCast<std::string, std::unordered_set<T>>
{
public:
    std::unordered_set<T> operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        typename std::unordered_set<T> vec;
        std::stringstream ss;
        for (size_t i = 0; i < node.size(); ++i)
        {
            ss.str("");
            ss << node[i];
            vec.insert(LexicalCast<std::string, T>()(ss.str()));
        }
        return vec;
    }
};

template <class T>
class LexicalCast<std::unordered_set<T>, std::string>
{
public:
    std::string operator()(const std::unordered_set<T> &v)
    {
        YAML::Node node;
        for (auto &i : v)
        {
            node.push_back(YAML::Load(LexicalCast<T, std::string>()(i)));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

template <class T>
class LexicalCast<std::string, std::map<std::string, T>>
{
public:
    std::map<std::string, T> operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        typename std::map<std::string, T> vec;
        std::stringstream ss;
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            ss.str("");
            ss << it->second;
            vec.insert(std::make_pair(it->first.Scalar(), LexicalCast<std::string, T>()(ss.str())));
        }
        return vec;
    }
};

template <class T>
class LexicalCast<std::map<std::string, T>, std::string>
{
public:
    std::string operator()(const std::map<std::string, T> &v)
    {
        YAML::Node node;
        for (auto &i : v)
        {
            node[i.first] = YAML::Load(LexicalCast<T, std::string>()(i.second));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

template <class T>
class LexicalCast<std::string, std::unordered_map<std::string, T>>
{
public:
    std::unordered_map<std::string, T> operator()(const std::string &v)
    {
        YAML::Node node = YAML::Load(v);
        typename std::unordered_map<std::string, T> vec;
        std::stringstream ss;
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            ss.str("");
            ss << it->second;
            vec.insert(std::make_pair(it->first.Scalar(), LexicalCast<std::string, T>()(ss.str())));
        }
        return vec;
    }
};

template <class T>
class LexicalCast<std::unordered_map<std::string, T>, std::string>
{
public:
    std::string operator()(const std::unordered_map<std::string, T> &v)
    {
        YAML::Node node;
        for (auto &i : v)
        {
            node[i.first] = YAML::Load(LexicalCast<T, std::string>()(i.second));
        }
        std::stringstream ss;
        ss << node;
        return ss.str();
    }
};

template <class T,
          class FromStr = LexicalCast<std::string, T>,
          class ToStr = LexicalCast<T, std::string>>
class ConfigVar : public ConfigVarBase
{
public:
    typedef std::shared_ptr<ConfigVar> ptr;
    // on change
    // callback,当一个配置更改后，进行通知（传进来原来的值和新值，再做处理）
    typedef std::function<void(const T &old_value, const T &new_val)> on_change_cb;

    ConfigVar(const std::string &name, const T &default_value, const std::string &description = "")
        : ConfigVarBase(name, description), m_val(default_value)
    {
    }

    std::string toString() override
    {
        try
        {
            // return boost::lexical_cast<std::string>(m_val);
            return ToStr()(m_val);
        }
        catch (std::exception &e)
        {
            SYLAR_LOG_ERROR(SYLAR_LOG_ROOT())
                << "ConfigVar::"
                   "toString exception"
                << e.what() << "convert: " << typeid(m_val).name() << "to string";
        }
        return "";
    }
    bool fromString(const std::string &val) override
    {
        try
        {
            // m_val = boost::lexical_cast<T>(val);
            setValue(FromStr()(val));
            return true;
        }
        catch (std::exception &e)
        {
            SYLAR_LOG_ERROR(SYLAR_LOG_ROOT())
                << "ConfigVar::"
                   "fromString "
                   "exception"
                << e.what() << "convert:string to " << typeid(m_val).name();
        }
        return false;
    }
    const T &getValue() const { return m_val; }
    void setValue(const T &v)
    {
        if (v == m_val)
        {
            return;
        }
        for (auto &i : m_cbs)
        {
            i.second(m_val, v);
        }
        m_val = v;
    }
    std::string getTypeName() const override { return typeid(T).name(); }

    void addListener(uint64_t key, on_change_cb cb) { m_cbs[key] = cb; }

    void delListener(uint64_t key) { m_cbs.erase(key); }

    on_change_cb getListener(uint64_t key)
    {
        auto it = m_cbs.find(key);
        return it == m_cbs.end() ? nullptr : it->second;
    }

    void clearListener() { m_cbs.clear(); }

private:
    T m_val;
    // uint64_t 用来表示回调函数的唯一id,hash值
    std::map<uint64_t, on_change_cb> m_cbs;
};

class Config
{
public:
    typedef std::map<std::string, ConfigVarBase::ptr> ConfigVarMap;

    //查找map里面是不是已经有一个加进去的了
    template <class T>
    static typename ConfigVar<T>::ptr Lookup(const std::string &name)
    {
        auto it = GetDatas().find(name);
        if (it == GetDatas().end())
        {
            return nullptr;
        }
        return std::dynamic_pointer_cast<ConfigVar<T>>(it->second);
    }
    //   查询
    //   + 不存在则创建（带默认值和描述）
    template <class T>
    static typename ConfigVar<T>::ptr
    Lookup(const std::string &name, const T &default_value, const std::string &description = "")
    {
        auto it = GetDatas().find(name);
        if (it != GetDatas().end())
        {
            auto tmp = std::dynamic_pointer_cast<ConfigVar<T>>(it->second);
            if (tmp)
            {
                SYLAR_LOG_INFO(SYLAR_LOG_ROOT()) << "Lookup name=" << name << " exists";
                return tmp;
            }
            else
            {
                SYLAR_LOG_ERROR(SYLAR_LOG_ROOT())
                    << "Lookup name=" << name << " exists but type not " << typeid(T).name()
                    << " real_type=" << it->second->getTypeName() << " " << it->second->toString();
                return nullptr;
            }
        }
        if (name.find_first_not_of("abcdefghijklmnopqrstuv"
                                   "wxyz0123456789_.") != std::string::npos)
        {
            SYLAR_LOG_ERROR(SYLAR_LOG_ROOT()) << "Lookup name "
                                                 "invalid "
                                                 "name="
                                              << name;
            throw std::invalid_argument(name);
        }
        typename ConfigVar<T>::ptr v(new ConfigVar<T>(name, default_value, description));
        GetDatas()[name] = v;
        return v;
    }

    static void LoadFromYaml(const YAML::Node &root);

    static ConfigVarBase::ptr LookupBase(const std::string &name);

private:
    static ConfigVarMap &GetDatas()
    {
        static ConfigVarMap s_datas;
        return s_datas;
    }
};
} // namespace sylar
#endif