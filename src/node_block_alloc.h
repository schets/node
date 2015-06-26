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

#ifndef SRC_NODE_BLOCK_ALLOC_H_
#define SRC_NODE_BLOCK_ALLOC_H_

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
union chunk {
    T data;
    chunk *next;
};

template<class T>
struct slab {
    chunk<T> *data;
    slab *next;
};

//Slab managers don't make a lot of sense for this class
//Since there aren't very clear semantics for data sharing
//And very consistent patterns in freeing. It's possible, but comes
//With a cost and is rarely applicable

/**
 * This class maintains a series of free lists for objects of constant size
 */
template<class T>
class BlockAllocator {

    //!A pointer to the current value at the top of the list
    chunk<T> *first_open;

    //!Pointer to start of stack
    slab<T> *head;

    //!size of each slab
    const size_t slabsize;
    
private:
    /**
     * Adds a new slab to the start of the list
     */
    T * noinline add_slab() {
        void *rdata = malloc(slabsize * sizeof(chunk<T>) + sizeof(slab<T>));
        slab<T> *newslab = static_cast<slab<T> *>(rdata);
        if (unlikely(!newslab))
            return NULL;
        newslab->next = head;
        head = newslab;
        chunk<T> *start = reinterpret_cast<chunk<T> *>(newslab + 1);
        chunk<T> *curc = start;
        for (size_t i = 0; i < slabsize - 1; i++) {
            curc->next = curc + 1;
            curc += 1;
        }
        curc->next = first_open;
        first_open = start->next;
        return &start->data;
    }

    template<bool do_dtor>
    inline void release(void *in) {
        if (unlikely(!in))
            return;
        
        chunk<T> *val = static_cast<chunk<T> *>(in);
        if(do_dtor)
            val->data.~T();
        val->next = first_open;
        first_open = val;
    }

public:

    BlockAllocator(size_t ssize)
        :
        first_open(NULL),
        head(NULL),
        slabsize(ssize) {}

    
    //!Returns a pointer from the top of the stack
    inline T *alloc() {
        if (unlikely(!first_open)) {
            return add_slab();
        }
        T *data = &first_open->data;
        first_open = first_open->next;
        return data;
    }

    //!Returns a pointer to the free list, no destruction
    inline void free(void *in) {
        release<false>(in);
    }

    //!Returns a pointer to the free list and calls the destructor
    inline void destroy(void *in) {
        release<true>(in);
    }

    //!clears all memory, does not call any destructors
    void clear() {
        while (head) {
            slab<T> *del = head;
            head = head->next;
            free(del);
        }
        head = NULL;
        first_open=NULL;
    }

};

} //namespace node

#undef likely
#undef unlikely
#undef noinline

#endif //SRC_NODE_BLOCK_ALLOC_H
