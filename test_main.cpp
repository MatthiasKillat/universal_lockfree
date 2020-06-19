#include <iostream>
#include <thread>
#include <chrono>

#include "lockfree_wrapper.hpp"
#include "allocator.hpp"
#include "bar.hpp"

template <typename TestObject>
void work(TestObject &object, int a = 1, int iterations = 1000000)
{
    for (int i = 0; i < iterations; ++i)
    {
        object.work(a);
    }
}

template <typename TestObject>
void test(TestObject &object, int iterations = 1000000, int n = 2, int m = 2)
{
    std::vector<std::thread> threads;
    threads.reserve(n + m);

    for (int i = 0; i < n; ++i)
    {
        threads.emplace_back(work<TestObject>, std::ref(object), 1, iterations);
    }

    for (int i = 0; i < m; ++i)
    {
        threads.emplace_back(work<TestObject>, std::ref(object), -1, iterations);
    }

    for (auto &thread : threads)
    {
        thread.join();
    }
}

template <typename TestObject>
void workLockfree(TestObject &object, int a = 1, int iterations = 1000000)
{
    for (int i = 0; i < iterations; ++i)
    {
        object.invoke(&Bar::work, a);
    }
}

template <typename TestObject>
void testLockfree(TestObject &object, int iterations = 1000000, int n = 2, int m = 2)
{
    std::vector<std::thread> threads;
    threads.reserve(n + m);

    for (int i = 0; i < n; ++i)
    {
        threads.emplace_back(workLockfree<TestObject>, std::ref(object), 1, iterations);
    }

    for (int i = 0; i < m; ++i)
    {
        threads.emplace_back(workLockfree<TestObject>, std::ref(object), -1, iterations);
    }

    for (auto &thread : threads)
    {
        thread.join();
    }
}

int main(int argc, char **argv)
{
    {
        Bar bar;
        test(bar, 1 << 20, 2, 3);
        std::cout << "Bar" << std::endl;
        bar.print();
    }

    {
        LockFree<Bar> lfBar;
        testLockfree(lfBar, 10000, 3, 5);
        std::cout << "Lockfree Bar" << std::endl;
        lfBar->print();
    }

    //todo: there still is a race /missing deletion as the MonitoredAllocator shows
    Allocator::print();
    return 0;
}
