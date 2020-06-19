#pragma once

#include <utility>
#include <iostream>
#include <map>
#include <mutex>
#include <atomic>

//simple allocator
class DefaultAllocator
{
public:
    template <typename T, typename... Args>
    static T *allocate(Args &&... args)
    {
        s_numAllocations.fetch_add(1);
        return new T(std::forward<Args>(args)...);
    }

    template <typename T>
    static void free(T *p)
    {
        delete p;
        s_numAllocations.fetch_sub(1);
    }

    static void print()
    {
        std::cout << "DefaultAllocator allocations " << s_numAllocations << std::endl;
    }

private:
    static std::atomic<uint64_t> s_numAllocations;
};

std::atomic<uint64_t> DefaultAllocator::s_numAllocations{0};

class MonitoredAllocator
{
public:
    template <typename T, typename... Args>
    static T *allocate(Args &&... args)
    {
        auto p = new T(std::forward<Args>(args)...);
        //std::cout << "allocated " << sizeof(T) << " bytes at " << p << std::endl;
        std::lock_guard<std::mutex> g(s_mutex);

        auto iter = s_allocations.find(p);

        if (iter == s_allocations.end())
        {
            s_allocations[p] = 1;
        }
        else
        {
            iter->second++;
        }
        s_numAllocations++;
        return p;
    }

    template <typename T>
    static void free(T *p)
    {

        std::lock_guard<std::mutex> g(s_mutex);
        auto iter = s_allocations.find(p);
        if (iter != s_allocations.end())
        {
            if (iter->second >= 1)
            {
                delete p;
                //std::cout << "deallocated " << sizeof(T) << " bytes at " << p << std::endl;
                iter->second--;
                s_numAllocations--;

                if (iter->second > 0)
                {
                    std::cout << "free error: " << p << " missing deallocation" << std::endl;
                    s_errors++;
                }
            }
            else if (iter->second == 0)
            {
                std::cout << "free error: " << p << " not allocated or double free" << std::endl;
                s_errors++;
            }
        }
        else
        {
            std::cout << "free error: " << p << " not allocated" << std::endl;
            s_errors++;
        }
    }

    static void
    print()
    {
        std::lock_guard<std::mutex> g(s_mutex);
        std::cout << "MonitoredAllocator free errors " << s_errors << std::endl;
        std::cout << "MonitoredAllocator current allocations " << s_numAllocations << std::endl;
        for (auto &pair : s_allocations)
        {
            if (pair.second > 0)
            {
                std::cout << pair.first << std::endl;
            }
        }
    }

    static size_t errors()
    {
        return s_errors;
    }

    static std::mutex s_mutex; //todo: only for testing, must be removed together with the map to make it lock_free
    static std::map<void *, uint64_t> s_allocations;
    static uint64_t s_numAllocations;
    static size_t s_errors;
};

std::mutex MonitoredAllocator::s_mutex;
std::map<void *, uint64_t> MonitoredAllocator::s_allocations{};
uint64_t MonitoredAllocator::s_numAllocations{0};
size_t MonitoredAllocator::s_errors{0};
