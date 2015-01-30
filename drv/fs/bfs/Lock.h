#ifndef LOCK_H
#define LOCK_H
/* Lock - simple semaphores, read/write lock implementation
**
** Initial version by Axel Dörfler, axeld@pinc-software.de
** Roughly based on a Be sample code written by Nathan Schrenk.
**
** This file may be used under the terms of the OpenBeOS License.
*/



#include "Utility.h"
#include "Debug.h"


// Configure here if and when real benaphores should be used
//#define USE_BENAPHORE
	// if defined, benaphores are used for the Semaphore/RecursiveLock classes
#ifdef USER
//#	define FAST_LOCK
	// the ReadWriteLock class uses a second Semaphore to
	// speed up locking - only makes sense if USE_BENAPHORE
	// is defined, too.
#endif


class Semaphore {
	public:
		Semaphore(const char *name)
			:
#ifdef USE_BENAPHORE
			fSemaphore(create_semaphore(name, 0, 0))
#else
			fSemaphore(create_semaphore(name, 1, 0))
#endif
		{
#ifdef USE_BENAPHORE			
			atomic_set( &fCount, 1 );
#endif			
#ifndef USER
			//set_sem_owner(fSemaphore, B_SYSTEM_TEAM);
#endif
		}

		~Semaphore()
		{
			destroy_semaphore(fSemaphore);
		}

		status_t InitCheck()
		{
			if (fSemaphore < B_OK)
				return fSemaphore;
			
			return B_OK;
		}

		status_t Lock()
		{
#ifdef USE_BENAPHORE
			atomic_dec(&fCount);
			if( atomic_read( &fCount ) < 0 )
#endif			
			{
				return lock_semaphore(fSemaphore);
			}
#ifdef USE_BENAPHORE
			return B_OK;
#endif
		}
	
		status_t Unlock()
		{
#ifdef USE_BENAPHORE
			atomic_inc(&fCount);
			if (atomic_read( &fCount ) <= 0)
#endif
				return unlock_semaphore(fSemaphore);
#ifdef USE_BENAPHORE
			return B_OK;
#endif
		}

	private:
		sem_id	fSemaphore;
#ifdef USE_BENAPHORE
		atomic_t fCount;
#endif
};

// a convenience class to lock a Semaphore object

class Locker {
	public:
		Locker(Semaphore &lock)
			: fLock(lock)
		{
			fStatus = lock.Lock();
			ASSERT(fStatus == B_OK);
		}

		~Locker()
		{
			if (fStatus == B_OK)
				fLock.Unlock();
		}

		status_t Status() const
		{
			return fStatus;
		}

	private:
		Semaphore	&fLock;
		status_t	fStatus;
};


//**** Recursive Lock

class RecursiveLock {
	public:
		RecursiveLock(const char *name)
			:
#ifdef USE_BENAPHORE
			fSemaphore(create_semaphore( name, 0, 0)),
#else
			fSemaphore(create_semaphore( name, 1, 0)),
#endif
			fOwner(-1)
		{
#ifdef USE_BENAPHORE			
			atomic_set( &fCount, 1 );
#endif			
#ifndef USER
			//set_sem_owner(fSemaphore, B_SYSTEM_TEAM);
#endif
		}

		status_t LockWithTimeout(bigtime_t timeout)
		{
			pid_t thread = current_thread_id(NULL);
			if (thread == fOwner) {
				fOwnerCount++;
				return B_OK;
			}

			status_t status;
#ifdef USE_BENAPHORE
			atomic_dec(&fCount);
			if (atomic_read(&fCount) >= 0)
				status = B_OK;
			else
#endif
				status = lock_semaphore_timeout(fSemaphore,  timeout);

			if (status == B_OK) {
				fOwner = thread;
				fOwnerCount = 1;
			}

			return status;
		}

		status_t Lock()
		{
			return LockWithTimeout(INFINITE);
		}

		status_t Unlock()
		{
			pid_t thread = current_thread_id(NULL);
			if (thread != fOwner) {
				panic("RecursiveLock unlocked by %ld, owned by %ld\n", (long int)thread, (long int)fOwner);
			}

			if (--fOwnerCount == 0) {
				fOwner = -1;
#ifdef USE_BENAPHORE
				atomic_inc(&fCount);
				if (atomic_read(&fCount) <= 0)
#endif
					return unlock_semaphore(fSemaphore);
			}

			return B_OK;
		}

	private:
		sem_id	fSemaphore;
#ifdef USE_BENAPHORE
		atomic_t	fCount;
#endif
		pid_t	fOwner;
		int32		fOwnerCount;
};

// a convenience class to lock an RecursiveLock object

class RecursiveLocker {
	public:
		RecursiveLocker(RecursiveLock &lock)
			: fLock(lock)
		{
			fStatus = lock.Lock();
			ASSERT(fStatus == B_OK);
		}

		~RecursiveLocker()
		{
			if (fStatus == B_OK)
				fLock.Unlock();
		}

		status_t Status() const
		{
			return fStatus;
		}

	private:
		RecursiveLock	&fLock;
		status_t		fStatus;
};


//**** Many Reader/Single Writer Lock

// This is a "fast" implementation of a single writer/many reader
// locking scheme. It's fast because it uses the benaphore idea
// to do lazy semaphore locking - in most cases it will only have
// to do some simple integer arithmetic.
// The second semaphore (fWriteLock) is needed to prevent the situation
// that a second writer can acquire the lock when there are still readers
// holding it.

#define MAX_READERS 100000

// Note: this code will break if you actually have 100000 readers
// at once. With the current thread/... limits in BeOS you can't
// touch that value, but it might be possible in the future.
// Also, you can only have about 20000 concurrent writers until
// the semaphore count exceeds the int32 bounds

// Timeouts:
// It may be a good idea to have timeouts for the WriteLocked class,
// in case something went wrong - we'll see if this is necessary,
// but it would be a somewhat poor work-around for a deadlock...
// But the only real problem with timeouts could be for things like
// "chkbfs" - because such a tool may need to lock for some more time


// define if you want to have fast locks as the foundation for the
// ReadWriteLock class - the benefit is that acquire_sem() doesn't
// have to be called when there is no one waiting.
// The disadvantage is the use of 2 real semaphores which is quite
// expensive regarding that BeOS only allows for a total of 64k
// semaphores (since every open BFS inode has such a lock).

#ifdef FAST_LOCK
class ReadWriteLock {
	public:
		ReadWriteLock(const char *name)
			:
			fWriteLock(name)
		{
			Initialize(name);
		}

		ReadWriteLock()
			:
			fWriteLock("bfs r/w w-lock")
		{
		}

		~ReadWriteLock()
		{
			destroy_semaphore(fSemaphore);
		}

		status_t Initialize(const char *name = "bfs r/w lock")
		{
			fSemaphore = create_semaphore(name, 0, 0);
			fCount = MAX_READERS;
#ifndef USER
//			set_sem_owner(fSemaphore, B_SYSTEM_TEAM);
#endif
			return fSemaphore;
		}

		status_t InitCheck()
		{
			if (fSemaphore < 0)
				return fSemaphore;
			
			return B_OK;
		}

		status_t Lock()
		{
			if (atomic_add(&fCount, -1) <= 0)
				return lock_semaphore(fSemaphore);
			
			return B_OK;
		}
		
		void Unlock()
		{
			if (atomic_add(&fCount, 1) < 0)
				unlock_semaphore(fSemaphore);
		}
		
		status_t LockWrite()
		{
			if (fWriteLock.Lock() < B_OK)
				return B_ERROR;

			int32 readers = atomic_add(&fCount, -MAX_READERS);
			status_t status = B_OK;

			if (readers < MAX_READERS) {
				// Acquire sem for all readers currently not using a semaphore.
				// But if we are not the only write lock in the queue, just get
				// the one for us
				status = lock_semaphore_timeout(fSemaphore,   INFINITE);
			}
			fWriteLock.Unlock();

			return status;
		}
		
		void UnlockWrite()
		{
			int32 readers = atomic_add(&fCount, MAX_READERS);
			if (readers < 0) {
				// release sem for all readers only when we were the only writer
				unlock_semaphore_ex(fSemaphore, readers <= -MAX_READERS ? 1 : -readers);
			}
		}

	private:
		friend class ReadLocked;
		friend class WriteLocked;

		sem_id		fSemaphore;
		atomic_t	fCount;
		Semaphore	fWriteLock;
};
#else	// FAST_LOCK
class ReadWriteLock {
	public:
		ReadWriteLock(const char *name)
		{
			Initialize(name);
		}

		ReadWriteLock()
		{
		}

		~ReadWriteLock()
		{
			destroy_semaphore(fSemaphore);
		}

		status_t Initialize(const char *name = "bfs r/w lock")
		{
			fSemaphore = create_semaphore(name, MAX_READERS, 0);
#ifndef USER
//			set_sem_owner(fSemaphore, B_SYSTEM_TEAM);
#endif
			return fSemaphore;
		}

		status_t InitCheck()
		{
			if (fSemaphore < B_OK)
				return fSemaphore;
			
			return B_OK;
		}

		status_t Lock()
		{
			return lock_semaphore(fSemaphore);
		}
		
		void Unlock()
		{
			unlock_semaphore(fSemaphore);
		}
		
		status_t LockWrite()
		{
			return lock_semaphore_timeout(fSemaphore, MAX_READERS);
		}
		
		void UnlockWrite()
		{
			unlock_semaphore_ex(fSemaphore, MAX_READERS);
		}

	private:
		friend class ReadLocked;
		friend class WriteLocked;

		sem_id		fSemaphore;
};
#endif	// FAST_LOCK


class ReadLocked {
	public:
		ReadLocked(ReadWriteLock &lock)
			:
			fLock(lock)
		{
			fStatus = lock.Lock();
		}
		
		~ReadLocked()
		{
			if (fStatus == B_OK)
				fLock.Unlock();
		}
	
	private:
		ReadWriteLock	&fLock;
		status_t		fStatus;
};



class WriteLocked {
	public:
		WriteLocked(ReadWriteLock &lock)
			:
			fLock(lock)
		{
			fStatus = lock.LockWrite();
		}

		~WriteLocked()
		{
			if (fStatus == B_OK)
				fLock.UnlockWrite();
		}

		status_t IsLocked()
		{
			return fStatus;
		}

	private:
		ReadWriteLock	&fLock;
		status_t		fStatus;
};



// A simple locking structure that doesn't use a semaphore - it's useful
// if you have to protect critical parts with a short runtime.
// It also allows to nest several locks for the same thread.

class SimpleLock {
	public:
		SimpleLock()
		{
			atomic_set( &fCount, 0 );
			atomic_set( &fHolder, -1 );
		}

		status_t Lock(bigtime_t time = 500)
		{
			int32 thisThread = current_thread_id(NULL);
			int32 current;
			while (1) {
				/*if (fHolder == -1) {
					current = fHolder;
					fHolder = thisThread;
				}*/
				if( ( current = atomic_read( &fHolder ) ) == -1 )
					atomic_set( &fHolder, thisThread );
				if (current == -1)
					break;
				if (current == thisThread)
					break;
					
				snooze(time);
			}

			// ToDo: the lock cannot fail currently! We may want
			// to change this
			atomic_inc(&fCount);
			return B_OK;
		}

		void Unlock()
		{
			atomic_dec(&fCount);
			if (atomic_read(&fCount) == 0)
				atomic_set(&fHolder, -1);
		}

		bool IsLocked() const
		{
			return atomic_read( &fHolder ) == current_thread_id( NULL);
		}

	private:
		atomic_t fHolder;
		atomic_t fCount;
};

// A convenience class to lock the SimpleLock, note the
// different timing compared to the direct call

class SimpleLocker {
	public:
		SimpleLocker(SimpleLock &lock,bigtime_t time = 1000)
			: fLock(lock)
		{
			lock.Lock(time);
		}

		~SimpleLocker()
		{
			fLock.Unlock();
		}

	private:
		SimpleLock	&fLock;
};

#endif	/* LOCK_H */