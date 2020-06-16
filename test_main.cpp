#include <iostream>
#include <thread>
#include <chrono>

#include "lockfree_wrapper.hpp"
#include "foo.hpp"
#include "allocator.hpp"

template <typename TestObject>
void work(TestObject &object, int a = 1, int iterations = 1000000)
{
    for (int i = 0; i < iterations; ++i)
    {
        object.inc(a);
    }
}

template <typename TestObject>
void test(TestObject &object, int iterations = 1000000, int n = 2)
{
    std::vector<std::thread> threads;
    threads.reserve(2 * n);

    for (int i = 0; i < n; ++i)
    {
        threads.emplace_back(work<TestObject>, std::ref(object), 1, iterations);
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
        object.invoke(&Bar::inc, a);
    }
}

template <typename TestObject>
void testLockfree(TestObject &object, int iterations = 1000000, int n = 2)
{
    std::vector<std::thread> threads;
    threads.reserve(2 * n);

    for (int i = 0; i < n; ++i)
    {
        threads.emplace_back(workLockfree<TestObject>, std::ref(object), 1, iterations);
        threads.emplace_back(workLockfree<TestObject>, std::ref(object), -1, iterations);
    }

    for (auto &thread : threads)
    {
        thread.join();
    }
}

//todo: better test to see lockfree property
int main(int argc, char **argv)
{
    Bar bar;
    test(bar, 1 << 20, 4);
    std::cout << "Bar" << std::endl;
    bar.print();

    LockFree<Bar> lfBar;
    testLockfree(lfBar, 10000, 1);
    std::cout << "Lockfree Bar" << std::endl;
    lfBar->print();

    std::cout << "free errors " << Allocator::errors() << std::endl;
    return 0;
}
