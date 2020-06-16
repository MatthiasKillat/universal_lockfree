#pragma once
#include "allocator.hpp"

#include <atomic>
#include <functional>
#include <set>
#include <vector>
#include <algorithm>

//potentially useful for read often, write seldom lockfree structures
//or prototyping where performance is not the primary issue (the internal copies are quite inefficient)

//todo: generalize allocation, deal with failures/bounded resources
//todo: memory order
//todo: interface, proxy design, copy/move
//todo: optimization
//todo: deletion race can result in leak?

//todo: transaction proxy (similar to writer, but with explicit writeback)

template <typename T>
class LockFree
{
private:
    enum Status
    {
        FREE,
        USED,
        RELEASED,
        DELETABLE
    };

    struct HazardPointer
    {
        HazardPointer(uint64_t id = 0) : id(id)
        {
        }

        std::atomic<T *> ptr{nullptr}; //the payload, todo: could use type T* to avoid casts, atomic needed? (who chanegs this?)

        HazardPointer *next{nullptr};
        std::atomic<uint32_t> status{FREE};
        const uint64_t id; //unique and does not change (can use the this pointer instead later)
    };

public:
    //as long as this object lives, we have read access to the object state (which may be outdated, however)
    //this means the object state is NOT deleted during the lifetime of the proxy
    template <typename S>
    class ReadOnlyProxy
    {
    public:
        friend class LockFree<T>;
        ~ReadOnlyProxy()
        {
            wrapper->releaseHazardPointer(*hp);
        }

        const S *operator->()
        {
            return object;
        }

    private:
        HazardPointer *hp;
        S *object;
        LockFree<S> *wrapper;

        ReadOnlyProxy(LockFree<S> &wrapper) : wrapper(&wrapper)
        {
            hp = this->wrapper->acquireHazardPointer(); //todo: deal with failure (hard to do, we need the raw pointer in operator->)

            //std::cout << "reader hp " << hp << std::endl;
            object = this->wrapper->ptr(*hp);
        }

        //ReadOnlyProxy(const ReadOnlyProxy &) = default;
        //ReadOnlyProxy(ReadOnlyProxy &&) = default;
    };

    template <typename S>
    class TryWriteProxy
    {
    public:
        friend class LockFree<T>;
        ~TryWriteProxy()
        {
            if (!wrapper->updateObject(object, copy))
            {
                wrapper->free(copy);
                //std::cout << "TryWriteProxy deleted unused copy " << copy << std::endl;
            }

            wrapper->releaseHazardPointer(*hp);
        }

        S *operator->()
        {
            return copy;
        }

    private:
        HazardPointer *hp;
        S *object;
        S *copy;
        LockFree<S> *wrapper;

        TryWriteProxy(LockFree<S> &wrapper) : wrapper(&wrapper)
        {
            hp = this->wrapper->acquireHazardPointer(); //todo: deal with failure
            object = this->wrapper->ptr(*hp);
            copy = this->wrapper->allocate(*object);
            //std::cout << "TryWriteProxy created copy " << copy << std::endl;
        }

        //TryWriteProxy(const TryWriteProxy &) = default;
        //TryWriteProxy(TryWriteProxy &&) = default;
    };

public:
    friend class ReadOnlyProxy<T>;
    friend class TryWriteProxy<T>;

    //todo: more construction variants, emplacement, move etc.
    LockFree(const T &value) : currentObjectHazardPointer(new HazardPointer(numHazardPointers.fetch_add(1)))
    {
        auto initialObject = allocate(value);
        //std::cout << "created initial object " << initialObject << std::endl;
        currentObjectHazardPointer->ptr.store(initialObject); //owned internally, only released in dtor, prevents deletion of current object
        currentObjectHazardPointer->status.store(USED);
        hazardPointers = currentObjectHazardPointer;
    }

    ~LockFree()
    {
        //std::cout << "Dtor" << std::endl;

        auto finalObject = currentObject();

        //todo: technically we must block the generation of new hazards here ...

        auto hp = hazardPointers.load();

        while (hp)
        {
            hp->status.store(RELEASED); //if there are active users this will cause problems (release their resource)
            hp = hp->next;
        }

        releaseHazardPointer(*currentObjectHazardPointer); //triggers a scan and since all were RELEASED

        hp = hazardPointers.load();
        while (hp)
        {
            auto next = hp->next;
            delete hp;
            hp = next;
        }
    }

    LockFree(const LockFree &) = delete;
    LockFree(LockFree &&) = delete;

    //************************************debug, control object state

    T *currentObject()
    {
        return currentObjectHazardPointer->ptr.load();
    }

    bool updateObject(T *newObject)
    {
        auto hp = acquireHazardPointer(); //to protect the current object and be able to delete it later
        T *expectedObject = hp->ptr.load();
        //we cannot have an ABA problem here, ptr will be deleted and possibly recycled only after no one holds ptr anymore
        //in a hazardpointer (and therefore will not try to update with this old value)
        if (currentObjectHazardPointer->ptr.compare_exchange_strong(expectedObject, newObject))
        {
            releaseHazardPointer(*hp);
            return true;
        }
        releaseHazardPointer(*hp);
        return false;
    }

    bool updateObject(T *expectedObject, T *newObject)
    {
        auto hp = acquireHazardPointer(); //to protect the current object and be able to delete it later
        //we cannot have an ABA problem here, ptr will be deleted and possibly recycled only after no one holds ptr anymore
        //in a hazardpointer (and therefore will not try to update with this old value)
        if (currentObjectHazardPointer->ptr.compare_exchange_strong(expectedObject, newObject))
        {
            releaseHazardPointer(*hp);
            return true;
        }
        releaseHazardPointer(*hp);
        return false;
    }

    //***********************proxy interface

    ReadOnlyProxy<T> readOnly()
    {
        return ReadOnlyProxy<T>(*this);
    }

    TryWriteProxy<T> tryWrite()
    {
        return TryWriteProxy<T>(*this);
    }

    TryWriteProxy<T> operator->()
    {
        return tryWrite();
    }

    //todo: implement, communicate success/failure
    template <typename Function, typename... Params>
    decltype(auto) tryInvoke(Function &&f, Params &&... params)
    {
    }

    template <typename Function, typename... Params>
    decltype(auto) invoke(Function &&f, Params &&... params)
    {
        auto &hp = *acquireHazardPointer();

        do
        {
            T *expected = ptr(hp);
            T *copy = allocate(*expected); //local copy, protected against deletion by hp

            //std::cout << "created copy " << copy << std::endl;

            auto result = std::invoke(f, copy, std::forward<Params>(params)...);

            if (updateObject(expected, copy))
            {
                releaseHazardPointer(hp);
                return result;
            }

            //our update failed, the copy is useless now
            free(copy); //could optimize and use in place construction in memory instead
            //std::cout << "deleted unused copy " << copy << std::endl;

            //we recycle the hazard pointer and load the new object state (can optimize by using load of compare exchange)
            hp.ptr.store(currentObject());

        } while (true);
    }

private:
    HazardPointer *currentObjectHazardPointer;

    //for now, only one deleteScan shall be active at a time
    std::atomic_flag scanInProgress{false};

    //hazardpointers are only created and not destroyed until the LockFree object goes out of scope
    //(to make dealing with some ABA issues easy)
    //the list only grows if new hazard pointers are needed, this avoids ABA problems but leads to inefficiencies
    //this is also not the best structure to search in, there is potential for optimization
    //but it shows the general idea

    std::atomic<uint64_t> numHazardPointers{0};           //i.e. list size
    std::atomic<HazardPointer *> hazardPointers{nullptr}; //managed hazard pointers, can be used to protect objects

    //todo: uneeded, remove later
    T *ptr(HazardPointer &hp)
    {
        return hp.ptr.load();
    }

    //get a free hazard pointer or create a new one
    HazardPointer *acquireHazardPointer()
    {
        HazardPointer *hp = hazardPointers.load();

        //try to recycle a free hazard pointer
        while (hp)
        {
            uint32_t expectedStatus = FREE;
            if (hp->status.compare_exchange_strong(expectedStatus, USED))
            {
                hp->ptr = currentObject();
                return hp;
            }
            hp = hp->next;
        }

        //no free hazard pointer, create a new one
        hp = new HazardPointer(numHazardPointers.fetch_add(1));
        hp->status.store(USED);

        auto head = hazardPointers.load();
        do
        {
            hp->next = head;
            if (hazardPointers.compare_exchange_weak(head, hp))
            {
                hp->ptr = currentObject();
                break;
            }

        } while (true);

        return hp;
    }

    //expect that it is a used hazard pointer
    void releaseHazardPointer(HazardPointer &hp)
    {
        //we are the only writer to this hazard pointer (hence no compare exchange needed)
        hp.status.store(RELEASED);
        deleteScan();
    }

    //we need to scan all hazardpointers for possible deletions, we cannot just free the released one due to races (which would lead to duble frees or leaks)
    //this is essentially a case of "helping" other operations to progress
    //todo: this algorithm can be optimized (more efficient/elegant/simpler)
    bool deleteScan()
    {
        if (scanInProgress.test_and_set(std::memory_order_acq_rel))
        {
            return false; //another scan is in progress, we skip the scan
        }

        //todo: we are lockfree, but the problem is: if the scanning thread dies, no one can ever scan again
        //and we leak memory (and probably fast) sinc eno hazard pointer will be freed
        //major todo/question: can we do it in a robust lockfree way (without the scanInProgress flag)
        auto head = hazardPointers.load();
        auto n = numHazardPointers.load();

        std::set<HazardPointer *> deleteCandidates;
        std::set<T *> usedPointers;

        {
            //we can iterate over the hazard pointer list without problems (there may be added new ones in front,
            //but they are just not considered for deletion and at least as new as currentObject
            auto hp = head;
            while (hp)
            {
                auto status = hp->status.load();
                if (status == RELEASED)
                {
                    hp->status.store(DELETABLE); //optimistic assumption: can delete later
                    deleteCandidates.insert(hp);
                }
                else if (status == DELETABLE)
                {
                    deleteCandidates.insert(hp); //should not happen, optimize the algorithm
                }
                else if (status == USED) //note that it does not matter if the status became RELEASED inbetween (we just cannot free it in this scan)
                {
                    usedPointers.insert(hp->ptr.load());
                }
                hp = hp->next;
            }
        }

        //std::cout << "Delete Scan " << std::endl;
        // std::cout << "used pointers ";
        // for (auto used : usedPointers)
        // {
        //     std::cout << used << " ";
        // }
        // std::cout << std::endl;

        // std::cout << "delete candidates ";
        // for (auto hp : deleteCandidates)
        // {
        //     std::cout << hp->ptr.load() << " ";
        // }
        // std::cout << std::endl;

        //now check if we really can delete the candidates(i.e.no USED hazardpointer with the same ptr exists)

        for (auto hp : deleteCandidates)
        {
            auto ptr = hp->ptr.load();
            if (usedPointers.find(ptr) != usedPointers.end())
            {
                hp->status.store(RELEASED); //assumption was wrong: NOT deletable
            }
        }

        std::set<T *> deleteSet;

        for (auto hp : deleteCandidates)
        {
            if (hp->status.load() == DELETABLE)
            {
                hp->status.store(FREE);
                deleteSet.insert(hp->ptr.load()); //avoid duplicates, can be done with unique as well
            }
        }

        //note that when in the meantime other hazardpointers are released, we might not free all resources we could
        //(they will be freed in the next scan)
        //also note that we never free something that is in use (and have no double free)
        for (auto p : deleteSet)
        {
            free(p);
        }

        scanInProgress.clear(std::memory_order_acq_rel);
        return true;
    }

    template <typename... Args>
    T *allocate(Args &&... args)
    {
        return Allocator::allocate<T>(std::forward<Args>(args)...);
    }

    void free(T *p)
    {
        Allocator::free(p);
    }
};