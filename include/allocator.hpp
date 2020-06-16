#pragma once

#include <utility>
#include <iostream>
#include <map>
#include <mutex>

//simple allocator
class Allocator
{
public:
    template <typename T, typename... Args>
    static T *allocate(Args &&... args)
    {
        auto p = new T(std::forward<Args>(args)...);
        std::cout << "allocated " << sizeof(T) << " bytes at " << p << std::endl;
        std::lock_guard<std::mutex> g(s_mutex);
        s_allocations[p] = sizeof(T);
        return p;
    }

    template <typename T>
    static void free(T *p)
    {
        delete p;
        std::cout << "deallocated " << sizeof(T) << " bytes at " << p << std::endl;

        std::lock_guard<std::mutex> g(s_mutex);
        auto iter = s_allocations.find(p);
        if (iter != s_allocations.end())
        {
            s_allocations.erase(iter);
        }
    }

    static void printAllocations()
    {
        std::cout << "Allocator current allocations" << std::endl;
        std::lock_guard<std::mutex> g(s_mutex);
        for (auto &pair : s_allocations)
        {
            std::cout << pair.first << " " << pair.second << "bytes" << std::endl;
        }
    }

    static std::mutex s_mutex; //todo: only for testing, must be removed together with the map to make it lock_free
    static std::map<void *, size_t> s_allocations;
};

std::mutex Allocator::s_mutex;
std::map<void *, size_t> Allocator::s_allocations{};