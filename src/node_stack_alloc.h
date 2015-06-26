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
#define likely(x) __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)
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
template<class T>
class SlabManager {

    //this is rarely called, so not inlining reduces icache pressure
    //In addition, both GCC and clang add a lot of extra push/pop
    //instructions when this gets inlined
    slab<T> * noinline alloc_slab() {
        void *data = malloc(slabsize * sizeof(T) + sizeof(slab<T>));
        if (data == NULL) {
            return NULL;
        }
        slab<T> *retslab = reinterpret_cast<slab<T> *>(data);
        retslab->data = reinterpret_cast<T *>(retslab + 1);
        retslab->next = retslab->prev = NULL;
        retslab->slabsize = slabsize;
        return retslab;
    }

    void free_slab(slab<T> *inslab) {
        free(inslab);
    }

protected:
    
    inline slab<T> *get_slab() {
        if(unlikely(!head)) {
            return alloc_slab(slabsize);
        }
        else {
            slab<T> *rethead = head;
            head = head->next;
            rethead->next = rethead->prev = NULL;
            return rethead;
        }
    }

    inline void return_slab(slab<T> *retslab) {
        if(likely(retslab)) {
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
    slab<T> *head;
    size_t slabsize;
    friend class ManagedStackAllocator<T>;
};

template<class T>
class ManagedStackAllocator {

    slab<T> *stackhead;
    slab<T> *curpos;
    slab<T> *slabend;
    SlabManager<T>& manager;
    slab<T> *start;
    
private:
    T * noinline inc_slab() {
        if (stackhead->next) {
            stackhead = stackhead->next;
        }
        else {
            slab<T> *next = manager.get_slab();
            if (unlikely(next == NULL)) {
                return NULL;
            }
            next->prev = stackhead;
            stackhead->next = next;
            stackhead = next;
        }
        curpos = stackhead->data;
        slabend = curpos + manager.slabsize;
        return curpos++;
    }

    void dtor_slab(T *start, T* end) {
        while(start < end) {
            start->~T();
            start++;
        }
    }

    ManagedStackAllocator(SlabManager<T>& man)
        :
        manager(man) {
        start = man.get_slab();
        stackhead = start;
        curpos = stackhead->data;
        slabend = curpos + manager.slabsize;
    }
    
public:

    T *alloc() {
        if (unlikely(curpos == slabend)) {
            return inc_slab();
        }
        return curpos++;
    }

    void pop() {
        if (unlikely(curpos == stackhead->data)) {
            if(likely(stackhead != start)) {
                stackhead = stackhead->prev;
                curpos = stackhead->data;
                slabend = curpos + manager.slabsize;
            }
        }
        else {
            --curpos;
        }
    }

    //!Releases all but first block
    void release_mem() {
        slab<T> *curdel = start->next;
        while (curdel) {
            slab<T> *del = curdel;
            curdel = curdel->next;
            manager.return_slab(del);
        }
        stackhead = start;
        start->next = NULL;
        curpos = start->data;
        slabend = curpos + manager.slabsize;
    }

    //releases all but first block, calls all destructors
    void delete_mem() {
        dtor_slab(stackhead->data, curpos);
        if (stackhead == start) {
            release_mem();
        }
        else {
            size_t slabsize = manager.slabsize;
            dtor_slab(start->data, start->data + slabsize);
            slab<T> *curdel = start->next;
            while (curdel < stackhead) {
                slab<T> *del = curdel;
                dtor_slab(del->data, del->data + slabsize);
                curdel = curdel->next;
                manager.return_slab(del);
            }
            curdel = stackhead->next;
            while (curdel) {
                slab<T> *del = curdel;
                curdel = curdel->next;
                manager.return_slab(del);
            }
        }
        stackhead = start;
        start->next = NULL;
        curpos = start->data;
        slabend = curpos + manager.slabsize;
    }
};

} //namespace node

#endif //SRC_NODE_STACK_ALLOC_H
