#ifndef __STOP_WORLD_H__
#define __STOP_WORLD_H__

#include "atomic_helpers.h"

#define MAX_THREADS 100
#define MAX_DIRTY_PAGES 10000

namespace stop_the_world {
	static atomic_u32 threadCount;
	static pthread_t threads[MAX_THREADS];
	static HybridMutex threadsLock;

	static atomic_u32 dirtyPageCount;
	static void* dirtyPages[MAX_DIRTY_PAGES];

	typedef void (* SIG_HNDLR_PTR)(int, siginfo_t *, void *);
	typedef void (* PLAIN_HNDLR_PTR)(int);

	template <typename AllocatorT>
	class StopTheWorld {
			AllocatorT *Allocator;
		public:
			void init(AllocatorT _allocator);
			void addThread(pthread_t thd);
			void protectHeap();
			void stop();
			void resume();
			template<typename f> void iterateOverDirtyAtomic(f Callback);
	};

};

#endif // __STOP_WORLD_H__
