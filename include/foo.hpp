#pragma once

class Foo
{
public:
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