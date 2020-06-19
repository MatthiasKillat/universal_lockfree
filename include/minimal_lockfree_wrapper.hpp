#pragma once
#include "allocator.hpp"

#include <atomic>
#include <functional>
#include <set>
#include <vector>
#include <algorithm>
#include <list>
#include <string>
#include <thread>

#include <mutex>

#include "assert.h"

//reduce the lockfree wrapper to a minimal set, to find reason for the deletion anomaly

//using Allocator = DefaultAllocator;
using Allocator = MonitoredAllocator;

template <typename T>
class LockFree
{
private:
    enum Status
    {
        FREE,             //the hazard pointer can be acquired and its pointer set
        USED,             //the hazard pointer is in use and protecting what ptr points to
        RELEASED,         //the hazard pointer was released but its protected ptr not cleaned up (which might not be possible if there are other users)
        DELETE_CANDIDATE, //the hazard pointer was released and one instance of its protected ptr can be deleted
        READY_TO_DELETE   //the hazard pointer was released and this specific ptr instance can be deleted
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
            case DELETE_CANDIDATE:
                return "DELETE_CANDIDATE";
            case READY_TO_DELETE:
                return "READY_TO_DELETE";
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
        std::atomic_flag deletionInProgress{false};
        const uint64_t id; //unique and does not change (todo: not needed, can use the this pointer instead later)

        std::mutex mutex;
    };

public:
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

        //std::this_thread::sleep_for(std::chrono::seconds(1));

        //printHazards();

        while (hp)
        {
            if (hp->status.load() == USED)
            {
                hp->status.store(RELEASED); //if there are active users this will cause problems (release their resource)
            }
            hp = hp->next;
        }

        //printHazards();

        deleteScan();

        //printHazards();

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

    T *operator->()
    {
        return currentObject();
    }

    //todo: variant without return value, does not need to compute result internally
    template <typename Function, typename... Params>
    decltype(auto) invoke(Function &&f, Params &&... params)
    {
        auto hp = acquireHazardPointer();
        T *protectedObject = hp->ptr.load(); //supposed to point to current object, but can have changed concurrently (CAS will fail then)
        do
        {
            T *copy = allocate(*protectedObject); //local copy

            auto result = std::invoke(f, copy, std::forward<Params>(params)...);

            //in the successful case of the CAS: hp protected expected
            if (currentObjectHazardPointer->ptr.compare_exchange_strong(protectedObject, copy))
            {
                releaseHazardPointer(*hp);
                return result;
            }

            //our update failed, the copy is useless now
            deallocate(copy); //could optimize and use in place construction in memory instead
            //releaseHazardPointer(*hp);

            //we recycle the hazard pointer and load the new object state
            protectedObject = protectCurrentObject(hp);

        } while (true);
    }

private:
    std::atomic_bool canCreateHazardPointer{true};
    HazardPointer *currentObjectHazardPointer;

    std::atomic<uint64_t> numHazardPointers{1};
    std::atomic<uint64_t> numUsedHazardPointers{1};
    std::atomic<uint64_t> numReleasedHazardPointers{0};
    std::atomic<HazardPointer *> hazardPointers{nullptr};

    T *currentObject()
    {
        return currentObjectHazardPointer->ptr.load();
    }

    T *protectCurrentObject(HazardPointer *hp)
    {
        T *ptr;
        do
        {
            ptr = currentObject();
            hp->ptr.store(ptr);
        } while (hp->ptr.load() != currentObject()); //need to check
        return ptr;
    }

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
                    protectCurrentObject(hp);
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
                protectCurrentObject(hp);
                break;
            }

        } while (true);

        numUsedHazardPointers.fetch_add(1);
        return hp;
    }

    void releaseHazardPointer(HazardPointer &hp)
    {
        if (hp.updateStatus(USED, RELEASED))
        {
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
    }

    void deleteScan()
    {
        //deleteMutex.lock();
        std::list<HazardPointer *> deleteCandidates;
        std::set<T *> usedPointers;

        {
            //we can iterate over the hazard pointer list without problems (there may be added new ones in front,
            //but they are just not considered for deletion and at least as new as currentObject
            auto hp = hazardPointers.load();
            while (hp)
            {
                uint32_t status = hp->status.load(); //this can be outdated but it does not matter

                if (status == RELEASED || status == DELETE_CANDIDATE || status == READY_TO_DELETE)
                {
                    deleteCandidates.push_back(hp);
                }
                else if (status == USED) //note that it does not matter if the status became RELEASED inbetween (we just cannot free it in this scan)
                {
                    usedPointers.insert(hp->ptr.load());
                }
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
                hp->updateStatus(RELEASED, DELETE_CANDIDATE);
                deletableHazardPointers.push_back(hp);
            }
        }

        std::set<T *> deleteSet;
        for (auto hp : deletableHazardPointers)
        {
            auto ptr = hp->ptr.load();
            if (deleteSet.find(ptr) == deleteSet.end())
            {
                //first occurence, delete later
                if (hp->updateStatus(DELETE_CANDIDATE, READY_TO_DELETE))
                {
                    deleteSet.insert(ptr);
                }
            }
            else
            {
                //further occurences, may be marked as free
                hp->updateStatus(DELETE_CANDIDATE, FREE); //todo: can we just use a store?
            }
        }
        //deleteMutex.unlock();

        tryDelete();
    }

    //std::mutex deleteMutex; //for testing
#if 1
    void tryDelete()
    {
        //deleteMutex.lock();
        auto hp = hazardPointers.load();
        while (hp)
        {

            //hp->mutex.lock();
            uint32_t status = hp->status.load();
            while (status == READY_TO_DELETE)
            {
                //not ideal, we may leak this hp if the setting thread dies (todo: find a better solution)
                if (hp->deletionInProgress.test_and_set())
                {
                    break;
                }

                //exclusive access to this hp
                //check whether it is still ready to delete
                do
                {
                    if (hp->status.compare_exchange_strong(status, READY_TO_DELETE))
                    {
                        deallocate(hp->ptr.load());
                        hp->ptr.store(nullptr);
                        hp->updateStatus(READY_TO_DELETE, FREE);
                        break;
                    }
                } while (status == READY_TO_DELETE);

                hp->deletionInProgress.clear();
            }
            //hp->mutex.unlock();

            hp = hp->next;
        }
        //deleteMutex.unlock();
    }

#else
    void tryDelete()
    {
        //deleteMutex.lock();
        auto hp = hazardPointers.load();
        while (hp)
        {
            hp->mutex.lock();
            uint32_t status = hp->status.load();

            if (status == READY_TO_DELETE)
            {
                deallocate(hp->ptr.load());
                hp->status.store(FREE);
            }

            hp->mutex.unlock();

            hp = hp->next;
        }
        //deleteMutex.unlock();
    }
#endif

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