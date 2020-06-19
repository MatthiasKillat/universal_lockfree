#pragma once

#include <atomic>
#include <iostream>

class Bar
{
public:
    Bar(int value = 0) : a(value)
    {
    }

    //note that this copy is ok if performed on a local state copy were nothing concurrently changes a
    //which is essentially what the lockfree wrapper does
    Bar(const Bar &other) : a(other.a.load(std::memory_order_relaxed))
    {
    }

    int work(int x = 1)
    {

        int b = a.load(std::memory_order_relaxed);

        //not needed to observe the effect
        //costly no-op on b, not optimized away (hopefully)
        // for (int i = 0; i < 1000; ++i)
        // {
        //     if (i % 2 == 0)
        //     {
        //         b++;
        //     }
        //     else
        //     {
        //         b--;
        //     }
        // }

        a.store(b + x);

        return a;
    }

    void print()
    {
        std::cout << a << std::endl;
    }

    std::atomic<int> a{0};
};

// class Bar0
// {
// public:
//     Bar0(int value = 0) : a(value)
//     {
//     }

//     //note that this copy is ok if performed on a local state copy were nothing concurrently changes a
//     //which is essentially what the lockfree wrapper does
//     Bar0(const Bar0 &other) : a(other.a) {}

//     int work(int x = 1)
//     {

//         a += x;

//         return a;
//     }

//     void print()
//     {
//         std::cout << a << std::endl;
//     }

//     int a{0};
// };