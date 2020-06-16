#pragma once

#include <atomic>
#include <iostream>

class Foo
{
public:
    static constexpr int numValues = 20;
    Foo(int value = 0) : value(value)
    {
    }

    Foo(const Foo &) = default;

    int read() const
    {
        return value;
    }

    void write(int newValue)
    {
        value = newValue;
    }

    int inc(int x = 1)
    {
        value += x;

        return value;
    }

    int value{0};
};

class Bar
{
public:
    static constexpr int numValues = 20;
    Bar(int value = 0) : a(value), b(value)
    {
    }

    Bar(const Bar &) = default;

    int inc(int x = 1)
    {
        //should be not easy to optimize somehow but clear enough (under lock, a == b always)
        static int n = 0;
        if (n % 2 == 0)
        {
            a += x;
            b = a;
        }
        else
        {
            b += x;
            a = b;
        }
        n = (n + 1) % 2;
        return a;
    }

    void print()
    {
        std::cout << a << " " << b << std::endl;
    }

    int a{0};
    int b{0};
};