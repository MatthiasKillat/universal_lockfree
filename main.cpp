#include <iostream>
#include <chrono>

#include "lockfree_wrapper.hpp"
#include "foo.hpp"
#include "allocator.hpp"

int main(int argc, char **argv)
{
#if 1
    {
        Foo foo(73);

        LockFree<Foo> lf(foo);

        {
            auto reader = lf.readOnly();
            auto value = reader->read();

            std::cout << "read value " << value << std::endl;

            Foo *newFoo = Allocator::allocate<Foo>(42);
            std::cout << "externally created " << newFoo << std::endl;
            lf.updateObject(newFoo);
            std::cout << "currentObject " << lf.currentObject() << std::endl;

            value = reader->read();
            std::cout << "read value " << value << std::endl;
        }

        auto value = lf.readOnly()->read();
        std::cout << "read value " << value << std::endl;

        {
            auto writer = lf.tryWrite();
            writer->inc(); //may fail and we do not get a notification ... bad
            writer->inc(3);
        }

        value = lf.readOnly()->read();
        std::cout << "read value " << value << std::endl;

        lf->inc(); //may fail ...

        value = lf.readOnly()->read();
        std::cout << "read value " << value << std::endl;

        auto result = lf.invoke(&Foo::inc, 37); //ugly syntax ... but a wrapper does not suffice, we need a CAS loop
        std::cout << "result " << result << std::endl;

        value = lf.readOnly()->read();
        std::cout << "read value " << value << std::endl;

        //simulate concurrent writing

        auto writer1 = lf.tryWrite();
        {
            auto writer2 = lf.tryWrite();
            writer2->write(-73);
        }
        writer1->write(-42); //fails, writer2 was faster

        value = lf.readOnly()->read();
        std::cout << "read value " << value << std::endl;
    }

    //check if there are undeleted objects
    Allocator::print();
#endif
    return 0;
}
