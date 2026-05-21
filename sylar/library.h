#ifndef __SYLAR_LIBRARY_H__
#define __SYLAR_LIBRARY_H__

#include "module.h"
#include <memory>

namespace sylar
{

class Library
{
public:
    static Module::ptr GetModule(const std::string &path);
};

} // namespace sylar

#endif