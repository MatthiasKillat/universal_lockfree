#pragma once
#include "allocator.hpp"

#include <atomic>
#include <functional>
#include <set>
#include <vector>
#include <algorithm>
#include <list>
#include <string>

#include "assert.h"

//potentially useful for read often, write seldom lockfree structures
//or prototyping where performance is not the primary issue (the internal copies are quite inefficient)

//todo: generalize allocation, deal with failures/bounded resources
//todo: memory order
//todo: interface, proxy design, copy/move
//todo: optimization
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
        DELETABLE,
        READY_TO_DELETE,
        DELETED
    };

    struct HazardPointer
    {
        HazardPointer(uint64_t id = 0) : id(id)
        {
        }

        void print()
        {
            std::cout << "HP " << id << " " << this << " ptr " << ptr.load() << " " << statusStr() << std::endl;
        }

        std::string statusStr()
        {
            switch (status)
            {
            case FREE:
                return "FREE";
            case USED:
                return "USED";
            case RELEASED:
                return "RELEASED";
            case DELETABLE:
                return "DELETABLE";
            case DELETED:
                return "DELETED";
            }
            return "";
        }

        bool updateStatus(uint32_t expected, uint32_t desired)
        {
            uint32_t oldStatus = expected;
            do
            {
                if (status.compare_exchange_weak(oldStatus, desired))
                {
                    return true;
                }
            } while (oldStatus == expected);
            return false;
        }

        std::atomic<T *> ptr{nullptr}; //the payload we want to protect todo: can be mase nonatomic in conjunctio with atomic status?
        HazardPointer *next{nullptr};
        std::atomic<uint32_t> status{FREE};
        const uint64_t id; //unique and does not change (todo: not needed, can use the this pointer instead later)
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
            object = hp->ptr.load();
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
                wrapper->deallocate(copy);
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
            object = hp->ptr.load();
            copy = this->wrapper->allocate(*object);
        }

        //TryWriteProxy(const TryWriteProxy &) = default;
        //TryWriteProxy(TryWriteProxy &&) = default;
    };

public:
    friend class ReadOnlyProxy<T>;
    friend class TryWriteProxy<T>;

    template <typename... Args>
    LockFree(Args &&... args) : currentObjectHazardPointer(new HazardPointer(0))
    {
        auto initialObject = allocate(std::forward<Args>(args)...);
        currentObjectHazardPointer->ptr.store(initialObject); //owned internally, only released in dtor, prevents deletion of current object
        currentObjectHazardPointer->status.store(USED);
        hazardPointers = currentObjectHazardPointer;
    }

    ~LockFree()
    {
        std::cout << "Destructor #hazard pointers " << numHazardPointers.load() << std::endl;
        //should maybe also block acquisition (recycling existing ones) but this is bad since we would need to check it always
        // (acquisition will happen much more often compared to creation)
        canCreateHazardPointer.store(false, std::memory_order_release);

        auto hp = hazardPointers.load();

        while (hp)
        {
            if (hp->status.load() == USED)
            {
                hp->status.store(RELEASED); //if there are active users this will cause problems (release their resource)
            }
            hp = hp->next;
        }

        //major todo: we run into double frees here somehow
        //should probably block acquisition here as well
        releaseHazardPointer(*currentObjectHazardPointer); //triggers a scan and since all were RELEASED all get freed

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
        //auto hp = acquireHazardPointer();
        //we cannot have an ABA problem here, ptr will be deleted and possibly recycled only after no one holds ptr anymore
        //in a hazardpointer (and therefore will not try to update with this old value)
        if (currentObjectHazardPointer->ptr.compare_exchange_strong(expectedObject, newObject))
        {
            //assert(hp->ptr.load() == expectedObject);

            //releaseHazardPointer(*hp);
            return true;
        }
        //releaseHazardPointer(*hp);
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

    //todo: variant without return value, does not need to compute result internally
    template <typename Function, typename... Params>
    decltype(auto) invoke(Function &&f, Params &&... params)
    {
        auto hp = acquireHazardPointer();

        do
        {
            T *expected = hp->ptr.load();
            T *copy = allocate(*expected); //local copy, protected against deletion by hp

            auto result = std::invoke(f, copy, std::forward<Params>(params)...);

            if (updateObject(expected, copy))
            {
                releaseHazardPointer(*hp);
                return result;
            }

            //our update failed, the copy is useless now
            deallocate(copy); //could optimize and use in place construction in memory instead

            //we recycle the hazard pointer and load the new object state (can optimize by using load of compare exchange)
            hp->ptr.store(currentObject());

        } while (true);
    }

private:
    std::atomic_bool canCreateHazardPointer{true};
    HazardPointer *currentObjectHazardPointer;

    //hazardpointers are only created and not destroyed until the LockFree object goes out of scope
    //(to make dealing with some ABA issues easier)
    //the list only grows if new hazard pointers are needed, this avoids ABA problems but leads to inefficiencies
    //this is also not the best structure to search in, there is potential for optimization
    //but it shows the general idea

    std::atomic<uint64_t> numHazardPointers{1}; //i.e. list size
    std::atomic<uint64_t> numUsedHazardPointers{1};
    std::atomic<uint64_t> numReleasedHazardPointers{0};
    std::atomic<HazardPointer *> hazardPointers{nullptr}; //managed hazard pointers, can be used to protect objects

    //get a free hazard pointer or create a new one
    HazardPointer *acquireHazardPointer()
    {

        //we spin until a free one becomes available if creation is impossible
        //later we could control the number available hazardpointers like this
        //and hence the number of maximally needed copies

        do
        {
            HazardPointer *hp = hazardPointers.load();
            //try to recycle a free hazard pointer
            while (hp)
            {
                uint32_t expectedStatus = FREE;
                if (hp->status.compare_exchange_strong(expectedStatus, USED))
                {
                    auto ptr = currentObjectHazardPointer->ptr.load();
                    do
                    {
                        hp->ptr.store(ptr);
                        //is it still the same? needed, to ensure the hazard pointer is protected and no deletion in progress (when current object changes)
                        if (currentObjectHazardPointer->ptr.compare_exchange_strong(ptr, ptr))
                        {
                            break;
                        }
                    } while (true);

                    numUsedHazardPointers.fetch_add(1);
                    return hp;
                }
                hp = hp->next;
            }
        } while (!canCreateHazardPointer.load());

        //no free hazard pointer, create a new one
        auto hp = createHazardPointer();
        hp->status.store(USED);

        auto head = hazardPointers.load();
        do
        {
            hp->next = head;
            if (hazardPointers.compare_exchange_weak(head, hp))
            {
                auto ptr = currentObjectHazardPointer->ptr.load();
                do
                {
                    hp->ptr.store(ptr);
                    //is it still the same? needed, to ensure the hazard pointer is protected and no deletion in progress (when current object changes)
                    if (currentObjectHazardPointer->ptr.compare_exchange_strong(ptr, ptr))
                    {
                        break;
                    }
                } while (true);
                break;
            }

        } while (true);

        numUsedHazardPointers.fetch_add(1);
        return hp;
    }

    //expect that it is a used hazard pointer
    void
    releaseHazardPointer(HazardPointer &hp)
    {
        //we are the only writer to this hazard pointer (hence no compare exchange needed)
        hp.status.store(RELEASED);
        auto numReleased = numReleasedHazardPointers.fetch_add(1, std::memory_order_relaxed);
        auto numUsed = numUsedHazardPointers.fetch_sub(1, std::memory_order_relaxed);

        constexpr double deleteScanFactor = 0.3;
        if (numUsed * deleteScanFactor <= numReleased)
        {
            //when enough were released (relative to those used) we atomically reset the released counter and trigger a scan
            do
            {
            } while (!numReleasedHazardPointers.compare_exchange_weak(numReleased, 0));
            deleteScan();
        }
    }

    //todo: can this be done in a less complicated/inefficient way without compromising lockfree robustness?
    void deleteScan()
    {
        std::list<HazardPointer *> deleteCandidates;
        std::set<T *> usedPointers;

        {
            //we can iterate over the hazard pointer list without problems (there may be added new ones in front,
            //but they are just not considered for deletion and at least as new as currentObject
            auto hp = hazardPointers.load();
            while (hp)
            {
                uint32_t status = hp->status.load(); //this can be outdated but it does not matter

                if (status == RELEASED || status == DELETABLE)
                {
                    deleteCandidates.push_back(hp);
                }
                else if (status == USED) //note that it does not matter if the status became RELEASED inbetween (we just cannot free it in this scan)
                {
                    usedPointers.insert(hp->ptr.load());
                }
                // else if (status == DELETED)
                // {
                //     hp->status.store(FREE); //can only be done by deleting thread (can we improve this?)
                // }
                hp = hp->next;
            }
        }

        std::list<HazardPointer *> deletableHazardPointers;

        //now check if we really can delete the candidates(i.e.no USED hazardpointer with the same ptr exists)
        for (auto hp : deleteCandidates)
        {
            auto ptr = hp->ptr.load();
            if (usedPointers.find(ptr) == usedPointers.end())
            {
                if (hp->updateStatus(RELEASED, DELETABLE))
                {
                    deletableHazardPointers.push_back(hp);
                }
                else
                {
                    deletableHazardPointers.push_back(hp);
                }
            }
        }

        std::set<T *> deleteSet;
        for (auto hp : deletableHazardPointers)
        {
            auto ptr = hp->ptr.load();
            if (deleteSet.find(ptr) == deleteSet.end())
            {
                //first occurence, delete later
                deleteSet.insert(ptr);
                hp->updateStatus(DELETABLE, READY_TO_DELETE);
            }
            else
            {
                //further occurences, can be marked as free
                hp->updateStatus(DELETABLE, FREE); //can we just use a store?
            }
        }

        //we also try to delete those others marked as ready to delete
        auto hp = hazardPointers.load();
        while (hp)
        {
            uint32_t expected = READY_TO_DELETE;
            do
            {
                auto ptr = hp->ptr.load();
                //cannot go to FREE directly, ptr may be used again before it is deleted (causing us to delete the wrong pointer)
                if (hp->status.compare_exchange_strong(expected, DELETED))
                {
                    //if we stall here, there is a leak of the hazard pointer and its resource...hard to avoid
                    while (ptr != nullptr)
                    {
                        if (hp->ptr.compare_exchange_strong(ptr, nullptr))
                        {
                            //hp->ptr.store(nullptr);

                            deallocate(ptr); //only one thread can win and delete the pointer
                            hp->status.store(FREE);
                            break;
                        }
                    }
                    break;
                }
            } while (expected == READY_TO_DELETE); //if not, someone else deleted, ok

            hp = hp->next;
        }
    }

    template <typename... Args>
    T *allocate(Args &&... args)
    {
        return Allocator::allocate<T>(std::forward<Args>(args)...);
    }

    void deallocate(T *p)
    {
        Allocator::free(p);
    }

    HazardPointer *createHazardPointer()
    {
        if (canCreateHazardPointer.load())
        {
            constexpr uint32_t MAX_HAZARDS{1000}; //todo: max limit mechanism works not exact right now, but not that important for now
            auto id = numHazardPointers.fetch_add(1);
            if (id > MAX_HAZARDS)
            {
                canCreateHazardPointer.store(false); //created last one
            }
            //can use a pool later
            return new HazardPointer(id);
        }
        return nullptr;
    }

    void printHazards()
    {
        std::cout << "****************" << std::endl;
        auto hp = hazardPointers.load();
        while (hp)
        {
            hp->print();
            hp = hp->next;
        }
        std::cout << "****************" << std::endl;
    }
};