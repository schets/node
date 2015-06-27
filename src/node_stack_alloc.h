// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef SRC_NODE_STACK_ALLOC_H_
#define SRC_NODE_STACK_ALLOC_H_

#include <cstddef>
#include <cstdlib> //malloc does not call constructors

//in benchmarks have seen a ~10-15% allocation speed gain
//anyways though these will help to preserve icache
#if (defined __GNUC__ || defined __clang__)
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#define noinline __attribute__ ((noinline))
#else
#define likely(x) x
#define unlikely(x) x
#define noinline
#endif

namespace node {

template<class T>
struct slab {
    T *data;
    slab *next;
    slab *prev;
};

template<class T> class ManagedStackAllocator;

//Although a StackAllocator could be shared,
//that may be suboptimal since each each stack would share
//space between various datastructures
//However, slabmanager allows each datastructure to share unused slabs with
//and allows some persistence of slabs (with trimming as well)
//Slabmanager is seperate from StackAllocator

//Of course, a plain StackAllocator is provided as well!

/**
 * This class manages the allocation, deallocation, and ownership of
 * memory blocks used by the stack allocators
 */
template<class T>
class SlabManager {

    const static size_t cache_bound = 64;
    //this is rarely called, so not inlining reduces icache pressure
    //In addition, both GCC and clang add a lot of extra push/pop
    //instructions when this gets inlined
    /**
     * Creates a new slab pointer along with the datablob
     * such that the data pointer starts on a 64 byte boundary
     */ 
    slab<T> * noinline alloc_slab() {
        void *data = malloc(slabsize * sizeof(T) + sizeof(*head) + cache_bound);
        if (unlikely(data == NULL)) {
            return NULL;
        }
        slab<T> *retslab = reinterpret_cast<slab<T> *>(data);
        //treat the address as number for easier arithmetic

        size_t slab_end = reinterpret_cast<size_t>(retslab + 1);
        if(!(slab_end % cache_bound))
            retslab->data = reinterpret_cast<T *>(retslab + 1);
        else if (slab_end < cache_bound) //suuuuper unlikely but possible?
            retslab->data = reinterpret_cast<T *>(cache_bound);
        else {
            size_t incr = slab_end + (cache_bound - (slab_end % cache_bound));
            retslab->data = reinterpret_cast<T *>(incr);
        }
        retslab->next = retslab->prev = NULL;
        return retslab;
    }

    void free_slab(slab<T> *inslab) {
        free(inslab);
    }

protected:
    
    /**
     * Returns a new slab to an allocator
     */ 
    inline slab<T> *get_slab() {
        if(unlikely(!head)) {
            return alloc_slab();
        }
        else {
            slab<T> *rethead = head;
            head = head->next;
            rethead->next = rethead->prev = NULL;
            return rethead;
        }
    }

    /**
     * Places a new slab at the head of the slab list.
     * Slab is placed first since it is more likely to be in the cache
     */ 
    inline void return_slab(slab<T> *retslab) {
        if(likely(retslab != NULL)) {
            retslab->next = head;
            head=retslab;
        }
    }

public:
    
    SlabManager(size_t slabsize_)
        :
        head(NULL) {
        slabsize = slabsize_ > 0 ? slabsize_ : 1;
    }
    
    /**
     * Frees slabs until the manager only holds n_keep slabs
     */
    void trim_to(size_t n_keep = 0) {
        slab<T> *start = head;
        size_t curnum = 0;
        while(start && curnum < n_keep) {
            ++curnum;
            start = start->next;
        }
        if(start) {
            slab<T> *del_start = start->next;
            while (del_start) {
                slab<T> *tofree = del_start;
                del_start = del_start->next;
                free_slab(tofree);
            }
        }
    }

private: 
    
    //!Holds a pointer to the head of the slab list
    slab<T> *head;

    //!The number of elements held in each slab
    size_t slabsize;

    friend class ManagedStackAllocator<T>;
};

/**
 * This class allocates memory by popping and pushing elements
 * from a stack. The stack consists of a linked list of slabs
 * form which values are allocated. This class does not manage
 * ownership of slabs and instead delegates that funcitonality to an
 * external SlabManager
 */
template<class T>
class ManagedStackAllocator {

    //!A pointer to the current value at the top of the stack
    T *curpos;

    //Pointer to the end of the current slab
    T *slabend;

    //!Object to control slabs in use
    SlabManager<T>& manager;

    //!Current top of the stack (in terms of allocations)
    slab<T> *stackhead;

    //!Pointer to start of stack
    slab<T> *start;
    
private:
    /**
     * Adds a new slab to the end of the list
     */
    T * noinline inc_slab() {
        slab<T> *next = manager.get_slab();
        if (unlikely(next == NULL))
            return NULL;

        next->prev = stackhead;
        if(stackhead) 
            stackhead->next = next;

        stackhead = next;
        curpos = stackhead->data;
        slabend = curpos + manager.slabsize;
        return curpos++;
    }

    //!Returns a slab to the manager
    void noinline dec_slab() {
        if(likely(stackhead != start)) {
            slab<T> *oldhead = stackhead;
            stackhead = stackhead->prev;
            stackhead->next = NULL;
            manager.return_slab(oldhead);
            slabend = stackhead->data + manager.slabsize;
            curpos = slabend-1;
        }
    }
    /**
     * Calls the destructor on every element between [start, end)
     */
    void dtor_slab(T *start, T* end) {
        while(start < end) {
            start->~T();
            start++;
        }
    }
    
public:

    ManagedStackAllocator(SlabManager<T>& man)
        :
        curpos(NULL),
        slabend(NULL),
        manager(man),
        stackhead(NULL),
        start(NULL)
         {}

    ~ManagedStackAllocator() {
        release_mem();
    }
    
    //!Returns a pointer from the top of the stack
    T *alloc() {
        if (unlikely(curpos == slabend)) 
            return inc_slab();
        return curpos++;
    }

    //!Removes a pointer from the top of the stack, returns slab if
    //!not first
    void pop() {
        if (unlikely(curpos == stackhead->data)) 
            dec_slab();
        else 
            --curpos;
    }

    //!Releases all blocks, calls all destructors
    void release_mem() {
        while (start) {
            slab<T> *del = start;
            start = start->next;
            manager.return_slab(del);
        }
        curpos = NULL;
        slabend = NULL;
        start = NULL;
        stackhead = NULL;
    }

    //!Releases all blocks, calls all destructors
    void delete_mem() {

        size_t slabsize = manager.slabsize;
        while (start != stackhead) {
            slab<T> *del = start;
            dtor_slab(del->data, del->data + slabsize);
            start = start->next;
            manager.return_slab(del);
        }
        if (stackhead)
            dtor_slab(stackhead->data, curpos);

        curpos = NULL;
        slabend = NULL;
        start = NULL;
        stackhead = NULL;
    }
};

} //namespace node

#endif //SRC_NODE_STACK_ALLOC_H
