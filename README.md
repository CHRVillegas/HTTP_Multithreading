## Overview
The server can be started using the command (After compilation):
	./httpserver -t [threads] and [-l logfile] [port] to specify the number of threads, logfile, and port to open.
	
### Design
The thread safety of my system is guaranteed by a few different design implimentations
I have multiple global variables that can be accessed by all threads whenever they need to complete differnt actions. Through the use of an integer queue called jobQueue, potential job connections are pushed and pulled to queue up
jobs for the threads to complete. They are guaranteed mutual exclusion by a mutex called "mutex" which is locked and unlocked whenever something is added to the job queue. There are also two semaphores called empty_sem and full_sem, which are initialized to 1000 and 0 respectively.
There are also two more mutexes, one called fileLock and fileUnlock. This is for calls to flock() which is not an atmoic function. Any time flock() is trying to lock something it's accompanied by locks and unlocks on fileLock. Whenever there is a call to flock() where a file is being unlocked then it is accompanied by locks and unlocks on fileUnlock.
Whenever the dispatcher is adding something to the queue it calls sem_wait(&empty_sem) which will cause it to wait until the the jobqueue is empty. It then locks and adds the job to the queue, unlocking after and sem_post(&full_sem) to signal that there is a job available.
Inside of the worker thread, it waits until the queue is populated with sem_wait(&full_sem) and then locks, and removes an item from the queue to operate on. It then handles the connection and returns to a stable state.
All critical sections for this program are when anything is added or removed to the jobQueue(). There are also locks around logging events and server response events.
Along with the above mentioned semaphores, within the writing functions of the server, flocks have been implemented 
to allow for atomic writing and reading of the various files that will be accessed by the server. Get requests place simple shared flocks around files they're accessing while PUT and APPEND requests will place exclusive locks around files they're accessing while also writing to a tempfile to assure they recieve the full request before continuing to write.
There is also an array of clientRequest structs which holds threads to be saved in case of one thread blocking while there are other requests available.
The amount of threads available and total threads are saved with the totalThreads and threadusage integer variables.
