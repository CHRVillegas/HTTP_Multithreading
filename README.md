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

#### Programmer Defined Functions
Starting off, the main function manages the number of threads specified and potential logfile and sets global variables accordingly, as well as initializing the thread_pool[threads] variable to the total number of threads specified using 
pthreadcreate(). The main function will continuously poll() until a connection is recieved and then designate one of the worker threads to complete the task and then return to polling for connections. It is through the use of poll() that once a sigterm or sigkill signal is recieved, the main function exits its listening loop and uses pthread_join to join all threads and then exit the program successfully.

##### handle_connection()
It is within this function that most of the initial parsing and reading from the client are accomplished using mutiple character buffers and pointers as well as a clientRequest struct to hold request information. There is also a pollfd struct which is required to correctly use the **poll()** function.
First all buffers are initialzed to be filled with NULL characters and it is checked to see if there are any saved connfd's that have been requeued and need work. 
Afterwards we poll to see if there is a potential connection, if there is, then it is read from until the entire 
request field is populated, if it is not, then the connfd recieved is requeued.
Next, the connection is once again polled for potential connections, and if there is, then it is read from. If there is no connection, the contents of the request line are saved and the connfd is requeued.
Once the header fields have been aquired, then we continue to get request-Id which has a default value of 0 if one is not specified.
The request is then validated by the **isbadRequest()** function and the **validReq()** function which verify the contents of the request and header field.
The method of the request is then verified
If the GET method is specifiied, then the method field of the clientRequest object is populated and **processGET()** is called.
If the PUT or APPEND method is specified, then the method field of the clientRequest object is populated and the Content-Length header is checked for. If it is not found, then an error is returned to the client and logged, if it is found then the messageLength field of the clientRequest object is populated and **processPUT** or **processAPPEND** is called based on method.

#### processAPPEND()
Within this function, an integer called this_file is used to open the specified fileName found within the clientRequest object rObj. If this__file is -1 and access to the file was not restriced, then a 404 message and log are created and sent. return -1
If access was restricted, then a 403 message and log are created and sent. return -1
If access to the file is permitted, then we call the **readWriteFD()** function and lock the mutex, then we create a 200 log and response. return 1

#### processPUT()
Within this function, an integer called this_file is used to open the specified fileName found within the clientRequest object rObj. If this___file is -1 and access to the file was not restricted, call **creat(rObj,fileName)** and create the file. We then call readWriteFD and send a response and log with code 201. return 1.
If access was resttrucedm then a 403 message and log are created and sent, return -1
If access to the file was permitted, then we call the **readWriteFD()** function and lokc the mutex, then we create a 200 log and response. return 1

#### processGET()
Within this function, an integer called this_file is used to open the specified fileName found within the clientRequest object rObj. If this__file is -1 and access to the file was not restricted, call **creat(rObj,fileName)** and create the file. We then call readWriteFD and send a response and log with code 201. return 1.
If access was resttrucedm then a 403 message and log are created and sent, return -1
If access to the file was permitted, then we call the **readWriteFD()** function and lock the mutex, then we create a 200 log and response. return 1

#### readWriteFD()
This function is where all of the writing for all Methods occurs. Firstly we create a buffer called buf which is malloced to a size of 4096 bytes, and using 3 integers called rd, wr, and bytes, the amount of bytes read and written are monitored. We initialize buf to all null characters.
Inside of a while loop that will continue to loop until we've reached the end of file, we read and add to bytes, and write to the specified file. The file we're writing to is protected by flock. Buf is also reset to all null characters.
Inside of the while loop, FLocks have been implemented to make sure that only one thread can write to a file at a time.
After the while loop exits, the memory allocated to buffer is freed.


##### validReq()
Simply checks if the method in request is valid, returns an integer to indicate validity.

##### isBadRequest() 
The **isBadRequest()** function checks if the request field and the header field of the client request are valid, and returns an integer indicating its validity.

##### servResponse()
The **servResponse()** function takes in int and clientRequest object. A char buffer is specified to hold the response and the sprintf function is used to write the desired response into the char buffer based on the code given. Using send, we then send the message to the client.

##### struct clientRequest()
This was simply a struct to hold all aspects of the client request. It holds 3 integers and 3 char buffers. The 3 integers hold the socket, enumerated method, and message length. The 3 char buffers hold the file name, http type, and full request sent by the client.

##### char status
This function simply takes a status code and returns a pointer to the string of correct response. For example the code 200 will return a pointer to the string "OK"

##### Typdef enum method
This is an enumerated object which simply specifies the types of requests the client can send. Implemented for readability.

##### int jobQueue[1000]
This was an integer array that was used as a queue with 4 monitoring ints. Those ints being in, out, total_requests, and size. Whenever something was added or removed to the queue, in and out were incremented respectively and modulod with the size of the array, ensuring that the item with the highest priority was accessed.

##### clientRequest saveState[1000]
This was an array of clientRequest objects which would be used to save progress with connections if they were swapped with another. It was also monitored using 4 ints. Those ints being saveIn, saveOut, totalSavedm and saveSize. Whenever something was added or removed to the queue, saveIn and saveOut were incremented respectively and modulod with the size of the array, ensuring that the item with the highest priority was accessed.
