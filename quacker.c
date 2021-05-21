/*
  Author: Michael O'Connell
  Title: InstaQuack! a simple multi-threaded server in C
  Class: CIS 415 - Operating Systems at the University of Oregon, Fall 2020
  Date: 12/5/2020
*/


//========================== Preprocessor Directives ==========================
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include "string_parser.c"
//=============================================================================

//============================ Structs and Macros =============================
#define URL_SIZE 500
#define CAP_SIZE 500
#define MAX_NAME 500
#define NUM_PROXIES 20
#define NUM_CLEANUP 1

typedef struct topic_entry 
{
    int entry_num;
    struct timeval time_stamp;
    int pub_id; // publisher thread id
    char photo_url[URL_SIZE];
    char photo_caption[CAP_SIZE];
} topic_entry;

typedef struct topic_queue
{
	char name[MAX_NAME];
	int topic_id;
	topic_entry* buffer;
	int head;
	int tail;
	int max_entry;
	int entry_cnt;
	int overall_entry_cnt; // never resets to 0, counts all entry topics throughout the lifetime of a topic queue
	pthread_mutex_t lock;
} topic_queue;

typedef struct thread
{
	pthread_t thread_id;
	int free_flag;
	char filename[MAX_NAME];
} thread;

typedef struct job_overflow
{
	char** jobs; // filename for each of the outstanding jobs
	int head;
	int tail;
	int jobs_cnt;
	int max_jobs;
	pthread_mutex_t lock;
} job_overflow;

/* -- Global flags and counts to be used throughout all threads -- */
/* Flag to indicate we've reached the end of input.txt. Allows
   threads to know when to stop waiting for a job.
*/
int end_of_command_file_flag = 0;
/* Used to ensure cleanup thread doesn't exit before all our threads have exited.
   Holds the count of every thread that has finished (incremented right before 
   a thread exits).
*/
int publisher_and_subscriber_done_count = 0;
// Used for the same purpose as above. Holds the count of all threads we're using
int total_publisher_and_subscriber_count = 0;

/* -- Global mutexes -- */
// Used to ensure all threads start at the same time
pthread_mutex_t main_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t main_cond = PTHREAD_COND_INITIALIZER;

/* -- Job overflow queues -- */
job_overflow publisher_job_overflow;
job_overflow subscriber_job_overflow;

/* -- Registry and registry vars -- */
topic_queue* registry;
/* number of topics that will be used in registry
   used as a global variable because it needs to be accessed
   by threads and won't be finalized until we reach the end of
   the command file.
*/
int num_topics = 0;

/* -- Cleanup thread vars --*/
// time elapsed before dequeueing, used in cleanup thread
int delta = 0;

//=============================================================================

//================================= Functions =================================

/* Helper method to free dynamically allocated members of topic queues contained 
   in the registry.
*/
void free_registry(int num_topic_queues)
{
	for(int i = 0; i < num_topic_queues; i++)
	{
		free(registry[i].buffer);
	}
	free(registry);
}

/* Helper method that dynamically allocates and and initalizes members for a 
   overflow queue.
*/
void allocate_and_init_job_overflow(job_overflow* job_overflow_queue, int size)
{
	job_overflow_queue->jobs = (char**) malloc(sizeof(char*) * size);
	for(int i = 0; i < size; i++)
	{
		job_overflow_queue->jobs[i] = (char*) malloc(sizeof(char) * MAX_NAME);
	}
	job_overflow_queue->max_jobs = size;
	job_overflow_queue->jobs_cnt = 0;
	job_overflow_queue->head = 0;
	job_overflow_queue->tail = 0;
	pthread_mutex_init(&job_overflow_queue->lock, NULL);
}

/* Helper method to free dynamicaly allocated members in an overflow queue */
void free_job_overflow(job_overflow* job_overflow_queue, int size)
{
	for(int i = 0; i < size; i++)
	{
		free(job_overflow_queue->jobs[i]);
	}
	free(job_overflow_queue->jobs);
}

/* Method used to enqueue into a job overflow queue. 

   Queue is a circular ring buffer which isn't necessary for the implementation in the current 
   iteration in the program because the main command file is read before starting any threads 
   and each job overflow queue must hold ALL the outstanding jobs at once. The current iteration 
   just dynamically  resizes as necessary. Having a circular ring buffer as the implementation is 
   for flexibility for possible future iterations of the program where the threads are spawned as 
   commands are read in the command file.

   Takes in a reference to a job_overflow_queue and a job file name to push into the overflow queue.
   Uses a mutex for exclusive access allowing multi-threaded use. Moves job filename into the queue
   at "head" and then increments "head" job count. Increments "head" to 0 if we've reached the end of 
   the allocated space for the circular ring buffer.

   Returns 1 if successful, 0 if not.

*/
int enqueue_job_overflow(job_overflow* job_overflow_queue, char* job_filename)
{
	pthread_mutex_lock(&job_overflow_queue->lock);
	if(job_overflow_queue->jobs_cnt == job_overflow_queue->max_jobs)
	{
		printf("Job overflow queue is full\n");
		pthread_mutex_unlock(&job_overflow_queue->lock);
		return 0;
	}

	// push job 
	strcpy(job_overflow_queue->jobs[job_overflow_queue->head], job_filename);
	job_overflow_queue->jobs_cnt++;

	/* Increment head pointer so it always points to the 
       position after the newest element in the queue 
    */
    if(job_overflow_queue->head < job_overflow_queue->max_jobs - 1)
    {
        job_overflow_queue->head++;
    }
    else
    {
        job_overflow_queue->head = 0;
    }

	// release lock
	pthread_mutex_unlock(&job_overflow_queue->lock);
	return 1;
}

/* Method used to dequeue from a job overflow queue. 

   See discussion on why queue is a circular ring buffer in enqueue_job_overflow().

   Takes in a reference to a job_overflow_queue and a reference to a empty job file name to remove from the overflow queue.
   Uses a mutex for exclusive access allowing multi-threaded use. Moves job filename from tail into the empty job filename
   and increments tail and decrements the job count.
   
   Returns 1 if successful, 0 if not. You also have the side-effect of having job_filename filled by whatever filename was in
   tail previously.
*/
int dequeue_job_overflow(job_overflow* job_overflow_queue, char** job_filename)
{
	pthread_mutex_lock(&job_overflow_queue->lock);

	if(job_overflow_queue->jobs_cnt == 0)
	{
		printf("Attempt to dequeue from job overflow failed. Job overflow queue is empty\n");
		pthread_mutex_unlock(&job_overflow_queue->lock);
		return 0;
	}

	// copy filename in job_overflow queue
	strcpy(*job_filename, job_overflow_queue->jobs[job_overflow_queue->tail]);

	// dequeue file from the overflow queue
	// update tail
	if(job_overflow_queue->tail < job_overflow_queue->max_jobs - 1)
	{
		job_overflow_queue->tail++;
	}
	else
	{
		job_overflow_queue->tail = 0;
	}

	// update count
	job_overflow_queue->jobs_cnt--;

	// unlock
	pthread_mutex_unlock(&job_overflow_queue->lock);

	return 1;
}

/* Helper method used to get the correct registry index using topic_id.
   
   Matches topic_id against the topic_id's (topic queue member) in the topic queue's contained within the registry. 
   
   If a match is found, return the index of the registry with that topic id. If not, return -1.
*/
int get_registry_index(int topic_id)
{
	for(int i = 0; i < num_topics; i++)
	{
		if(topic_id == registry[i].topic_id)
		{
			return i;
		}
	}

	return -1;
}

/* Used to enqueue entry into the correct topic queue located in the registry. 

   Finds the correct registry index (registry_index) containing the desired topic_queue (the one that has the same ID as
   topic_id) using get_registry_index(). Uses the topic queue's mutex for exclusive access allowing multi-threaded use.
   Updates entry's timestamp (using the current time) and entryNum (using topic queue's overall entry count). Note that using
   the topic queue's overall entry count for entryNum allows for a unique, monotonically increasing entryNum for each entry
   that is representative of WHEN it was entered relative to other entries in the same topic queue. Pushes entry into the queue
   at the head position and increments head. If head reaches the end of the queue, set to 0.

   Note that topic_queues are circular ring buffers, which is why head can reset to 0. 

   Returns 1 if successful, 0 if not (either couldn't find registry address or queue is full).
*/
int enqueue(int topic_id, topic_entry entry) 
{
	// get registry index based off topic id
	int registry_index = get_registry_index(topic_id);
	if(registry_index == -1)
	{
		printf("enqueue() error: Couldn't find topic queue in registry for topic id: %d\n", topic_id);
		return 0;
	}

	//Aquire the lock if it's available. Otherwise, wait until it is.
	pthread_mutex_lock(&registry[registry_index].lock);

	/* Check whether topic_queue's buffer is full */
    if(registry[registry_index].entry_cnt == registry[registry_index].max_entry)
    {
        printf("Publisher is pushing: Queue: %s - Error Queue is full. Thread ID: %ld\n", 
		        registry[registry_index].name, pthread_self());
		pthread_mutex_unlock(&registry[registry_index].lock);
		return 0;
    }

    /* update topic entry's timestamp and entry_num */
    struct timeval time;
    struct timezone time_zone; 
    if(gettimeofday(&time, &time_zone) == -1)
    {
        printf("Error. gettimeofday() failed. entry's time_stamp will be affected.\n");
    }
    entry.time_stamp = time;
    entry.entry_num = registry[registry_index].overall_entry_cnt + 1;

    /* Push topic entry onto registry's target queue */
    registry[registry_index].buffer[registry[registry_index].head] = entry;
    registry[registry_index].entry_cnt++;
	registry[registry_index].overall_entry_cnt++;
	
    /* Increment head pointer so it always points to the 
       position after the newest element in the queue 
    */
    if(registry[registry_index].head < registry[registry_index].max_entry - 1)
    {
        registry[registry_index].head++;
    }
    else
    {
        registry[registry_index].head = 0;
    }

    // FOR TESTING
    printf("Enqueue successful by thread ID: %ld. URL: %s, caption: %s, entry_num: %d time stamp: %ld\n\n", 
            pthread_self(), entry.photo_url, entry.photo_caption, entry.entry_num, entry.time_stamp.tv_sec);

    printf("Queue info: name: %s, head: %d, tail: %d, max_entry: %d, entry_cnt: %d\n\n",
            registry[registry_index].name, registry[registry_index].head, registry[registry_index].tail,
            registry[registry_index].max_entry, registry[registry_index].entry_cnt);

    //Release the lock.
	pthread_mutex_unlock(&registry[registry_index].lock);
	return 1;
}

/* Helper method used in get_entry() that copies the source_entry to the destination_entry */
void copy_entry(topic_entry* destination_entry, topic_entry* source_entry)
{
    destination_entry->entry_num = source_entry->entry_num;
    strcpy(destination_entry->photo_caption, source_entry->photo_caption);
    strcpy(destination_entry->photo_url, source_entry->photo_url);
    destination_entry->pub_id = source_entry->pub_id;
    destination_entry->time_stamp = source_entry->time_stamp;
}

/* Used to "get" an entry (copies the contents of a topic entry in the topic queue to entry) with an entry_num of last_entry + 1 
   from the topic_queue with the associated topic_id. DOES NOT REMOVE ENTRIES.

   Finds the correct registry index of the desired topic_queue using the topic_id, checks whether the topic queue 
   is empty, then goes through the topic queue, starting at the oldest entry (the beginning of the queue, at tail)
   and ending at the newest entry (the end of the queue, head), looking for an entry with an entry_num of last_entry + 1. 
   If there is an entry found with an entry_num of last_entry + 1, then the method is finished and we copy the entry and return 1. 
   If theres's an entry with an entry_num greater than last_entry + 1, we copy that entry and return the entry_num. This case 
   occurs because last_entry + 1 was dequeued by our cleanup thread before we were able to call get_entry (remember, there are 
   multiple subscriber threads calling this method running concurrently with a cleanup thread calling dequeue()). If we don't find
   last_entry + 1, it means all the entry_nums we came across were less than last_entry + 1 (i.e. last_entry + 1 isn't in the queue yet) 
   and we return 0. Uses the topic queue's mutex for exclusive access for multi-threaded use.

   Note all the cases regarding newest and oldest entry are because the topic_queue is a circular ring buffer. See comments below for 
   more of a description.

   Returns 1 if last_entry + 1 is found, 0 if it wasn't found OR if the topic queue was empty, and an entry_num greater than
   last_entry + 1 if an entry_num greater than last_entry + 1 was found. The side effect is entry will have the content of the entry within
   the topic queue with an entry_num of last_entry + 1.
*/
int get_entry(int topic_id, int last_entry, topic_entry* entry)
{
	int registry_index = get_registry_index(topic_id);
	if(registry_index == -1)
	{
		printf("get_entry() error: Couldn't find topic queue in registry for topic id: %d\n", topic_id);
		return 0;
	}
    //Aquire the lock if it's available. Otherwise, wait until it is.
	pthread_mutex_lock(&registry[registry_index].lock);

    /* Case 1: topic queue is empty */
    if(registry[registry_index].entry_cnt == 0)
    {
        printf("Get entry(): Topic queue is empty.\n");
        pthread_mutex_unlock(&registry[registry_index].lock);
        return 0;
    }

    /* Scan queue entries to see if entry is in queue, 
       starting with oldest entry until we find lastEntry + 1 entry, 
       then copy entry into empty topic_entry (entry) passed in and return 1.
    */
    int oldest_entry = registry[registry_index].tail;
    int newest_entry = registry[registry_index].head;
    if(oldest_entry < newest_entry)
    {
        for(int i = oldest_entry; i < newest_entry; i++)
        {
            /* Case 3.2: Entry encontered has an entry_num greater than last_entry + 1.
                         This occurs because entry was dequeud by cleanup thread.
            */
            if(registry[registry_index].buffer[i].entry_num > last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return entry->entry_num;
            }
            // Case 2: last_entry + 1 is in the queue
            else if(registry[registry_index].buffer[i].entry_num == last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return 1;
            }
        }
    }
    /* If tail > head, then we need to go to the end of the queue and wrap around
       to the beginning ending at head. This is because we're dealing with a circular
       ring buffer.
    */
    else if(oldest_entry > newest_entry)
    {
        for(int i = oldest_entry; i < registry[registry_index].max_entry; i++)
        {
            // Case 3.2 - see comment above
            if(registry[registry_index].buffer[i].entry_num > last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return entry->entry_num;
            }
            // Case 2
            else if(registry[registry_index].buffer[i].entry_num == last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return 1;
            }
        }
        for(int i = 0; i < newest_entry; i++)
        {
            // Case 3.2 - see comment above
            if(registry[registry_index].buffer[i].entry_num > last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return entry->entry_num;
            }
            // Case 2
            else if(registry[registry_index].buffer[i].entry_num == last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return 1;
            }
        }
    }
    // queue is full 
    else if(oldest_entry == newest_entry)
    {
        for(int i = 0; i < registry[registry_index].max_entry; i++)
        {
            // Case 3.2
            if(registry[registry_index].buffer[i].entry_num > last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return entry->entry_num;
            }
            // Case 2
            else if(registry[registry_index].buffer[i].entry_num == last_entry + 1)
            {
                copy_entry(entry, &registry[registry_index].buffer[i]);
                pthread_mutex_unlock(&registry[registry_index].lock);
                return 1;
            }
        }
    }

    /* Case 3.1: topic queue not empty and last_entry is not in the queue and
                 all entries in the queue are less than last_entry + 1. Entry
                 has yet to be put in the queue.
    */
   	pthread_mutex_unlock(&registry[registry_index].lock);
    return 0;

}

/* Helper method used by dequeue() that compares the age of the entry at the tail of the topic queue at 
   registry index "pos" to the global delta value assigned by the program's command file. If the entry's
   age is greater than or equal to delta, we remove it. 

   It only looks at the tail because that is the oldest entry, dequeue() calls this method multiple times, so
   if multiple entries need to be removed, they will be in succession. There won't be cases where an entry is removed that
   isn't at tail, because entries are pushed into the queue according to time. In other words, we can't have an entry
   in the queue older than the entry at tail. For example, we have entries 1, 2, 3 pushed into the queue. There ages will be
   decreasing from the order they are pushed in. Now, when dequeue calls this method multiple times, it will start with the oldest
   entry and end with the newest (which is the order they are pushed in). Therefore, compare_age_and_pop() will look at the first entry and
   decide whether it should be dequeued, then the second, and then the third. Now, if compare_age_and_pop() doesn't pop the first
   entry (the oldest), you're guarenteed it won't pop the second or third, even though dequeue will call the method multiple times.
   Because of this, compare_age_and_pop() only looks at the tail of the topic queue at registry[pos]. If compare_age_and_pop() pops
   the first entry, tail is incremented and the next call it will look at the second, and so on. Hopefully this rationale makes
   sense for why compare_age_and_pop only pops at a topic_queues tail.
 */
int compare_age_and_pop(int pos, int current_time, int inserted_time)
{
    int age = current_time - inserted_time;
    int prev;

    // Dequeue
    if(age >= delta)
    {
        prev = registry[pos].tail;

        /* Pop from registry[pos] - 
        Increment tail pointer so it always points to the 
        position after the newest element in the queue 
        */
        if(registry[pos].tail < registry[pos].max_entry - 1)
        {
            registry[pos].tail++;
        }
        else
        {
            registry[pos].tail = 0;
        }

        registry[pos].entry_cnt--;

        printf("dequeue() successful by thread id: %ld Age: %d, Entry num: %d, time_stamp: %ld, pub_id: %d, photo url: %s, photo caption: %s\n\n",
                pthread_self(), age, registry[pos].buffer[prev].entry_num, registry[pos].buffer[prev].time_stamp.tv_sec, registry[pos].buffer[prev].pub_id,
                registry[pos].buffer[prev].photo_url, registry[pos].buffer[prev].photo_caption);
        printf("Queue information. Name: %s, head: %d, tail: %d, max entry: %d, entry count: %d\n\n", registry[pos].name, registry[pos].head,
                registry[pos].tail, registry[pos].max_entry, registry[pos].entry_cnt);
        
        return 1;
    }

    return 0;
}

/* Used to dequeue all entries in all topic queues in the registry with an age older than delta (global variable).

   Goes through each topic queue in the registry, gains mutual exclusive access, checks if it's empty, then goes from its
   oldest (tail) to newest (head) entry, gets the inserted time of the entry, and compares the age of the entry to delta (implemented in
   compare_age_and_pop()). If age >= delta, the entry is popped. Dequeue() only pops from the tail because entries' ages only decrease from
   tail to head within a topic queue Therefore if an entry is popped, the next entry (going from tail to head) could be popped, but if an entry 
   is not popped, the next entry is guarenteed not to be popped. In other words, if multiple entries need to be removed, the ordering
   of how they are enqueued guarentees they will be removed at the tail in successsion (the oldest entry will be the tail, be removed, then the 
   next oldest will be the tail, be removed, and so on).

   Returns 1 if successful, 0 if not.
*/
int dequeue()
{
	struct timeval time;
	struct timezone time_zone;

	for(int i = 0; i < num_topics; i++)
	{ 
		//Aquire the lock if it's available. Otherwise, wait until it is.
		pthread_mutex_lock(&registry[i].lock); 

		if(gettimeofday(&time, &time_zone) == -1)
		{
			printf("Error. gettimeofday() failed in dequeue().\n");
		}
		int time_inserted = 0;
		int current_time = time.tv_sec;

	    // Check if registry[i] is empty. If so, don't need to do anything. 
		if(registry[i].entry_cnt == 0)
		{
			pthread_mutex_unlock(&registry[i].lock);
			return 0;
		}

		int oldest_entry = registry[i].tail;
		int newest_entry = registry[i].head;

		if(oldest_entry < newest_entry)
    	{
        	for(int j = oldest_entry; j < newest_entry; j++)
        	{
            	time_inserted = registry[i].buffer[j].time_stamp.tv_sec;
			    compare_age_and_pop(i, current_time, time_inserted);
        	}
    	}
		/* If tail > head, then we need to go to the end of the queue and wrap around
		   to the beginning ending at head. This is because we're dealing with a circular
		   ring buffer.
		*/
		else if(oldest_entry > newest_entry)
		{
			for(int j = oldest_entry; j < registry[i].max_entry; j++)
			{
				time_inserted = registry[i].buffer[j].time_stamp.tv_sec;
			    compare_age_and_pop(i, current_time, time_inserted);
			}
			for(int j = 0; j < newest_entry; j++)
			{
				time_inserted = registry[i].buffer[j].time_stamp.tv_sec;
			    compare_age_and_pop(i, current_time, time_inserted);
			}
		}
	    // queue is full (assuming we are putting in maximum number of entries)
    	else if(oldest_entry == newest_entry)
    	{
        	for(int j = 0; j < registry[i].max_entry; j++)
        	{
            	time_inserted = registry[i].buffer[j].time_stamp.tv_sec;
			    compare_age_and_pop(i, current_time, time_inserted);
			}
    	}

		pthread_mutex_unlock(&registry[i].lock);
		sched_yield();
	}
    return 1;
}

/* Used for publisher threads to execute. Executes commands from publisher command file (pubn.txt, where n is > 1),
   and picks up new "jobs" (publisher command files) once its finished with its primary job (first publisher command file
   passed in). Exits when its finished executing its primary job (or whatever job it's on), no jobs remain in the overflow
   queue, and the global end_of_command_file (main command file) flag has been set, ensuring no other jobs will be allocated
   for or sent to the publisher overflow queue.

   Thread is halted initially to wait for all other threads to be created and further outstanding publisher jobs (i.e. the 
   publisher thread pool is entirely tied up) to be sent to the publisher/subscriber overflow queue. Once all publisher/subscriber 
   threads have been created, they will all be signaled to start at the same time in main(). The publisher thread takes in a 
   thread* argument (pointer to thread struct) consisting of free_flag indicating its busy status, its thread ID, and its inital job
   filename. The first job is executed by reading through the command file and executing "put" (enqueue()) or "sleep" (usleep()).
   Once it has finished executing its job, the thread will check for additional jobs to pick up in the publisher overflow queue. The
   thread will exit according to the rules specified in the paragraph above. Note that the thread will continue to enqueue() until
   it is successful. Also note the thread uses methods found in "string_parser.h" for the file parsing.
*/
void* publisher(void *thread_args) 
{
	/* initalize thread arguments */
	// Cast args to the correct type in order to use throughout function
	thread* args = thread_args;
	args->thread_id = pthread_self();
	args->free_flag = 0;

	/* getline() vars */
    char* command_string = NULL;
    size_t bufsize = 0;

	// condition to keep thread alive
	int publisher_done = 0;
	int end_of_publisher_command_file = 0;
	int check_for_jobs = 0;

	// once our thread is free and picking up new jobs, this will hold the new filename
	char* new_job_filename = (char*) malloc(sizeof(char) *MAX_NAME);

	// lock so all all threads can start together
	pthread_mutex_lock(&main_lock);
	pthread_cond_wait(&main_cond, &main_lock);
	pthread_mutex_unlock(&main_lock);

	printf("Proxy thread %ld- type: Publisher DEBUG - Filename: %s\n\n", args->thread_id, args->filename);

	// start publisher thread
	while(!publisher_done)
	{
		end_of_publisher_command_file = 0;
		
		/* open command file */
		FILE* fp_in = fopen(args->filename, "r");

		while(!end_of_publisher_command_file)
		{
			check_for_jobs = 0;

			if(!fp_in)
			{
				printf("Error opening input file: %s\n", args->filename);
				check_for_jobs = 1;
			}
			else
			{
				getline(&command_string, &bufsize, fp_in);
				// remove newline character from command (if it's in the line)
				if(command_string[strlen(command_string) - 1] == '\n')
				{
					command_string[strlen(command_string) - 1] = '\0';
				}

				// stop command is always at the end of the publisher file				
				if(strcmp(command_string, "stop") == 0)
				{
					check_for_jobs = 1;
					// close previous command file
					fclose(fp_in);
				}
			}
			
			// if end of file see if more jobs are availible
			if(check_for_jobs)
			{
				args->free_flag = 1;

				// while we're free, lets wait check and see if a new job comes up to take on
				while(args->free_flag)
				{
					// if there are more jobs, start them up
					if(publisher_job_overflow.jobs_cnt > 0)
					{
						// attempt to pick up a new job (dequeue from publisher_job_overflow)
						if(dequeue_job_overflow(&publisher_job_overflow, &new_job_filename))
						{
							strcpy(args->filename, new_job_filename);
							printf("Publisher with thread id: %ld picked up job: %s\n", args->thread_id, args->filename);
							args->free_flag = 0;
							end_of_publisher_command_file = 1;
						}
					}
					else
					{
						// no jobs and the end of the file, let the thread exit
						if(end_of_command_file_flag)
						{
							args->free_flag = 0;
							end_of_publisher_command_file = 1;
							publisher_done = 1;
							break;
						}
					}
				}
			}
			else
			{
				/* parse publisher command file input.
				   
				   see command_line and str_filler() usage/comments in main() for an 
				   explanation on how to use it.
				 */
				command_line command_and_args;
				command_line command;
				command_and_args = str_filler(command_string, "\"");
				command = str_filler(command_and_args.command_list[0], " ");
				char* publisher_command = command.command_list[0];
				
				// enqueue a topic_entry to registry
				if(strcmp(publisher_command, "put") == 0)
				{
					// use parsed command file input to intialize a topic entry to push to registry 
					int topic_id = atoi(command.command_list[1]);
					char* photo_url = command_and_args.command_list[1];
					char* photo_caption = command_and_args.command_list[3];

					topic_entry entry;
					strcpy(entry.photo_url, photo_url);
					strcpy(entry.photo_caption, photo_caption);
					entry.pub_id = topic_id;

					int result = 0;

					result = enqueue(topic_id, entry);

					// continue to push until we're successful
					while(result == 0)
					{
						result = enqueue(topic_id, entry);
						// see if other threads will dequeue
						sched_yield();
						sleep(4);
					}
				}
				else if(strcmp(publisher_command, "sleep") == 0)
				{
					int sleep_amt = atoi(command.command_list[1]);
					// before sleeping yield to other threads so they can get work done
					sched_yield();
					// input is in milliseconds, usleep takes micro seconds and 1 milli = 1000 micro
					usleep((sleep_amt * 1000));
				}

				free_command_line(&command);
				free_command_line(&command_and_args);
			}

		}
	}

	printf("Publisher thread: %ld is exiting...\n\n", args->thread_id);
	publisher_and_subscriber_done_count++;
	free(new_job_filename);
	free(command_string);
}

/* Used for subscriber threads to execute. Executes commands from subscriber command file (subn.txt, where n is > 1),
   and picks up new "jobs" (subscriber command files) once its finished with its primary job (first subscriber command file
   passed in). Exits when its finished executing its primary job (or whatever job it's on), no jobs remain in the overflow
   queue, and the global end_of_command_file (main command file) flag has been set, ensuring no other jobs will be allocated
   for or sent to the subscriber overflow queue.

   Thread is halted initially to wait for all other threads to be created and further outstanding subscriber jobs (i.e. the 
   subscriber thread pool is entirely tied up) to be sent to the publisher/subscriber overflow queue. Once all publisher/subscriber 
   threads have been created, they will all be signaled to start at the same time in main(). The subscriber thread takes in a 
   thread* argument (pointer to thread struct) consisting of free_flag indicating its busy status, its thread ID, and its inital job
   filename. The first job is executed by reading through the command file and executing "get" (getEntry()) or "sleep" (usleep()).
   Once it has finished executing its job, the thread will check for additional jobs to pick up in the subscriber overflow queue. The
   thread will exit according to the rules specified in the paragraph above. A subscriber thread will write to an HTML file called
   "subscriber_<thread_id>_<job_number>.html" after each successful get_entry() call, creating and updating a table consisting
   of all the findings from a set of get_entry() calls on a specific registry topic queue.
*/
void* subscriber(void* thread_args)
{
	/* initalize thread arguments */
	// Cast args to the correct type in order to use throughout function
	thread* args = thread_args;
	args->thread_id = pthread_self();
	args->free_flag = 0;

	/* getline() vars */
    char* command_string = NULL;
    size_t bufsize = 0;

	// condition to keep thread alive
	int subscriber_done = 0;
	int end_of_subscriber_command_file = 0;
	int check_for_jobs = 0;

	// keeping track of jobs thread has done, so we can have a unique html file for each with the same thread id
	int jobs = 0;

	// once our thread is free and picking up new jobs, this will hold the new filename
	char* new_job_filename = (char*) malloc(sizeof(char) *MAX_NAME);

	// lock so all all threads can start together
	pthread_mutex_lock(&main_lock);
	pthread_cond_wait(&main_cond, &main_lock);
	pthread_mutex_unlock(&main_lock);

	printf("Proxy thread %ld- type: Subscriber DEBUG - Filename: %s\n\n", args->thread_id, args->filename);

	// start subscriber thread
	while(!subscriber_done)
	{
		end_of_subscriber_command_file = 0;
		
		/* open command file */
		FILE* fp_in = fopen(args->filename, "r");

		// construct an html filename for subscriber output
		char string_sub_thread_id[MAX_NAME];
		sprintf(string_sub_thread_id, "%ld", args->thread_id);
		char sub_html_filename[MAX_NAME] = "subscriber_";
		strcat(sub_html_filename, string_sub_thread_id);
		strcat(sub_html_filename, "_");
		char jobs_string[MAX_NAME];
		sprintf(jobs_string, "%d", jobs);
		strcat(sub_html_filename, jobs_string);
		strcat(sub_html_filename, ".html");
		
		// create the html file
		FILE* sub_fp;
		sub_fp = fopen(sub_html_filename, "w");
		// if our attempt to create doesn't work, no point in processing our command file
		if(!sub_fp)
		{
			printf("Error creating constructed html file: %s\n", sub_html_filename);
			fclose(fp_in);
			end_of_subscriber_command_file = 1;
		}
		// insert the beginning of the html file
		else
		{
			fprintf(sub_fp, "<!DOCTYPE html>\n<html>\n<head>\n<title>HTML_SUBSCRIBER_FILENAME</title>\n\n<style>\ntable, th, td {\n  border: 1px solid black;\n  border-collapse: collapse;\n}\nth, td {\n  padding: 5px;\n}\nth {\n  text-align: left;\n}\n</style>\n\n</head>\n<body>\n\n<h1>Subscriber: %s </h1>\n\n", 
			        sub_html_filename);
		}

		while(!end_of_subscriber_command_file)
		{
			check_for_jobs = 0;

			if(!fp_in)
			{
				printf("Error opening input file: %s\n", args->filename);
				fclose(sub_fp);
				check_for_jobs = 1;
			}
			else
			{
				getline(&command_string, &bufsize, fp_in);
				// remove newline character from command (if it's in the line)
				if(command_string[strlen(command_string) - 1] == '\n')
				{
					command_string[strlen(command_string) - 1] = '\0';
				}

				// stop command is always at the end of the publisher file				
				if(strcmp(command_string, "stop") == 0)
				{
					// finish html file
					fprintf(sub_fp, "</body>\n");
					fprintf(sub_fp, "</html>\n");
					
					check_for_jobs = 1;
					
					// close previous command file
					fclose(fp_in);
					fclose(sub_fp);
				}
			}
			
			// if end of file see if more jobs are availible
			if(check_for_jobs)
			{
				args->free_flag = 1;

				// while we're free, lets wait check and see if a new job comes up to take on
				while(args->free_flag)
				{
					// if there are more jobs, start them up
					if(subscriber_job_overflow.jobs_cnt > 0)
					{
						// attempt to pick up a new job (dequeue from subscriber_job_overflow)
						if(dequeue_job_overflow(&subscriber_job_overflow, &new_job_filename))
						{
							strcpy(args->filename, new_job_filename);
							printf("Subscriber with thread id: %ld picked up job: %s\n\n", args->thread_id, args->filename);
							jobs++;
							args->free_flag = 0;
							end_of_subscriber_command_file = 1;
						}
					}
					else
					{
						// no jobs and the end of the file, let the thread exit
						if(end_of_command_file_flag)
						{
							args->free_flag = 0;
							end_of_subscriber_command_file = 1;
							subscriber_done = 1;
							break;
						}
					}
				}
			}
			else
			{
				/* parse subscriber command file input.
				   
				   see command_line and str_filler() usage/comments in main() for an 
				   explanation on how to use it.
				*/
				command_line command_and_args;
				command_line command;
				command_and_args = str_filler(command_string, "\"");
				command = str_filler(command_and_args.command_list[0], " ");
				char* subscriber_command = command.command_list[0];
				
				// get new topic_entrys from registry
				if(strcmp(subscriber_command, "get") == 0)
				{
					// use parsed command file input to intialize a topic entry to push to registry 
					int topic_id = atoi(command.command_list[1]);

					int registry_index = get_registry_index(topic_id);
					if(registry_index == -1)
					{
						printf("Subscriber error: Couldn't find topic queue in registry for topic id: %d\n", topic_id);
						free_command_line(&command);
						free_command_line(&command_and_args);						
						continue;
					}

					/* get_entry() vars */
					topic_entry entry;
					int last_entry = 0;
					int done = 0;
					int result = 0;
					int num_tries = 3;
					int max_entry_num = registry[registry_index].overall_entry_cnt;
					// flag to add a header to html table containing topics
					int add_topic_entry_header = 1;

					/* Get all new topic entries in topic queue.
					   If queue is empty or last_entry isn't found, try for num_tries more times.
					   If queue is still empty or last_entry STILL isn't found, abandon the effort.
					*/
					while(!done)
					{
						// got all the new entries
						if(last_entry >= max_entry_num)
						{
							break;
						}

						result = get_entry(topic_id, last_entry, &entry);

						/* Either queue is empty or last_entry has yet to be put in the queue.
						   Try again for some amount of few times (num_tries), then stop.
						*/
						if(result == 0)
						{
							for(int i = 0; i < num_tries; i++)
							{
								result = get_entry(topic_id, last_entry, &entry);
								// queue no longer empty or our last_entry + 1 was found, move on
								if(result > 0)
								{
									break;
								}
								// tried num_tries and either queue was always empty or last_entry + 1 wasn't found
								else if(i == num_tries - 1)
								{
									done = 1;
									break;
								}

								/* Give other threads a chance to add to the topic queue */
								sched_yield();
								usleep(500);
							}
						}
						// last_entry + 1 was found
						if(result == 1)
						{
							last_entry++;
						}

						// last_entry + 1 was dequeud, move on to new oldest entry
						if(result > 1)
						{
							last_entry = result;
						}

						// modify HTML file
						if(result >= 1)
						{
							if(add_topic_entry_header)
							{
								// add on to HTML file before getting new topic entries
								fprintf(sub_fp, "<h2>Topic Name: %s</h2>\n\n", registry[registry_index].name);
								fprintf(sub_fp, "<table style=\"width:100%%\" align=\"middle\">\n");
								fprintf(sub_fp, "  <tr>\n");
								fprintf(sub_fp, "    <th>CAPTION</th>\n");
								fprintf(sub_fp, "    <th>PHOTO-URL</th>\n");
								fprintf(sub_fp, "  </tr>\n");
								add_topic_entry_header = 0;
							}
							fprintf(sub_fp, "  <tr>\n");
							fprintf(sub_fp, "    <td>%s</td>\n", entry.photo_caption);
							fprintf(sub_fp, "    <td>%s</td>\n", entry.photo_url);
							fprintf(sub_fp, "  </tr>\n");

							printf("get_entry() successful: %ld, entry num: %d, caption: %s, url: %s, pubID: %d, timestamp: %ld\n\n", 
							        args->thread_id, entry.entry_num, entry.photo_caption, entry.photo_url, entry.pub_id, entry.time_stamp.tv_sec);

						}
					}
					// finish table in html file
					fprintf(sub_fp, "</table>\n");

				}
				else if(strcmp(subscriber_command, "sleep") == 0)
				{
					int sleep_amt = atoi(command.command_list[1]);
					// before sleeping yield to other threads so they can get work done
					sched_yield();
					// input is in milliseconds, usleep takes micro seconds and 1 milli = 1000 micro
					usleep((sleep_amt * 1000));
				}

				free_command_line(&command);
				free_command_line(&command_and_args);
			}

		}
	}

	printf("Subscriber thread: %ld is exiting...\n\n", args->thread_id);
	publisher_and_subscriber_done_count++;
	free(new_job_filename);
	free(command_string);
}

/* Used for cleanup threads to execute. The thread periodically (every "delta" seconds) calls dequeue() to remove
   topic entries that have aged >= delte. 

   Thread is halted initially to wait for all other threads to be created and further outstanding publisher/subscriber jobs
   to be sent to the publisher/subscriber overflow queue. Once all publisher/subscriber  threads have been created, they will all 
   be signaled to start at the same time in main(). The subscriber thread takes in a thread* argument (pointer to thread struct) 
   consisting of free_flag indicating its busy status, its thread ID, and its inital job filename. In the case of cleanup(), it does
   not execute commands from a command file. The thread exits once all subscriber/publisher threads have also finished (kept in
   the global variable, publisher_and_subscriber_done_count).
*/
void* cleanup(void* thread_args)
{
	// initialize thread arguments
	thread* args = thread_args;
	args->free_flag = 0;
	args->thread_id = pthread_self();

	// Have all threads wait so they can start at the same time (after recieving signal from main() via broadcast)
	pthread_mutex_lock(&main_lock);
	pthread_cond_wait(&main_cond, &main_lock);
	pthread_mutex_unlock(&main_lock);
	printf("Proxy thread %ld- type: Cleanup\n\n", args->thread_id);

    int success;

	// sleep then perform cleanup on all our topic queues (if necessary)
	sched_yield();
	sleep(delta);
	
	// keep thread alive until our thread pool is finished
	while(publisher_and_subscriber_done_count < total_publisher_and_subscriber_count)
	{
		dequeue();
		sched_yield();
		sleep(delta);
	}

	printf("Cleanup thread: %ld is exiting...\n\n", args->thread_id);

}

//=============================================================================


int main(int argc, char* argv[]) 
{
	/* verify and open input file */
    FILE* fp_in;
    if(argc != 2)
    {
        fprintf(stderr, "Incorrect program input. Correct usage: ./server input.txt\n");
        exit(EXIT_FAILURE);
    }
    fp_in = fopen(argv[1], "r");
    if(!fp_in)
    {
        fprintf(stderr, "Error opening input file: %s\n", argv[1]);
        exit(EXIT_FAILURE); 
    }

	/* allocate registry */
	int allocated_registry_size = 10;
	registry = (topic_queue*) malloc(sizeof(topic_queue) * allocated_registry_size);

	/* initalize job overflow */
	// publisher
	int allocated_publisher_overflow_size = 10;
	allocate_and_init_job_overflow(&publisher_job_overflow, allocated_publisher_overflow_size);
	// subscriber
	int allocated_subscriber_overflow_size = 10;
	allocate_and_init_job_overflow(&subscriber_job_overflow, allocated_subscriber_overflow_size);

	/* thread pools */
	thread* publisher_thread_pool = (thread*) malloc(sizeof(thread) * (NUM_PROXIES / 2));
	int num_publishers = 0;
	thread* subscriber_thread_pool = (thread*) malloc(sizeof(thread) * (NUM_PROXIES / 2));
	int num_subscribers = 0;
	thread cleanup_thread;

	/* getline() vars */
    char* command_string = NULL;
    size_t bufsize = 0;

	/* str_filler() vars */
	command_line command_and_args;
	command_line command;
	command_line command_queue_length;

	// queue length will be extracted from "create" command in command input (input.txt)
	int queue_length;

	// start up cleanup thread
	pthread_create(&cleanup_thread.thread_id, NULL, cleanup, &cleanup_thread);

	while(!end_of_command_file_flag)
	{
        getline(&command_string, &bufsize, fp_in);
		// remove newline character from command (if it's in the line)
		if(command_string[strlen(command_string) - 1] == '\n')
		{
			command_string[strlen(command_string) - 1] = '\0';
		}
		
		// if end of file, set condition to exit loop
        if(feof(fp_in))
        {
			end_of_command_file_flag = 1;
            continue;
        }

		/* returns a command_line struct, which has a member "command_list"
		   that is a list of strings delimited by "\""".

		   e.g. 
		   str_filler("create topic 1 "Mountains" 6") 
		   returns ["create topic 1 ", "Mountains", " 6"]
		*/
		command_and_args = str_filler(command_string, "\"");
		/* command is always first in input, so we'll isolate the command by
		   removing the " " (space) character for later string comparisons.
		*/
		command = str_filler(command_and_args.command_list[0], " ");

		if(strcmp(command.command_list[0], "create") == 0)
		{
			// extract queue length from "create" command input
			command_queue_length = str_filler(command_and_args.command_list[2], " ");
			queue_length = atoi(command_queue_length.command_list[0]);
			
			/* allocate/initialize topic_queue's members based on "create" command input */
			registry[num_topics].max_entry = queue_length;
			strcpy(registry[num_topics].name, command_and_args.command_list[1]);
			registry[num_topics].topic_id = atoi(command.command_list[2]);
			registry[num_topics].buffer = (topic_entry*) malloc(sizeof(topic_entry) * queue_length);
			
			// finish initalizing all other topic_queue's members not dependent on "create" command input
			registry[num_topics].head = 0;
			registry[num_topics].tail = 0;
			registry[num_topics].entry_cnt = 0;
			registry[num_topics].overall_entry_cnt = 0;
			pthread_mutex_init(&registry[num_topics].lock, NULL);

			num_topics++;

			// if there should be more topics than we allocated for, double our allocation
			while(allocated_registry_size <= num_topics)
			{
				allocated_registry_size = allocated_registry_size * 2;
				registry = (topic_queue*) realloc(registry, (sizeof(topic_queue) * allocated_registry_size));
				if(registry == NULL)
				{
					printf("reallocation for registry failed.\n");
					free_registry(num_topics);
                    exit(EXIT_FAILURE);
				}
			}

			free_command_line(&command_queue_length);
		}
		else if(strcmp(command.command_list[0], "add") == 0 && strcmp(command.command_list[1], "publisher") == 0)
		{
			char* job_filename = command_and_args.command_list[1];
			/* we can only create NUM_PROXIES / 2 threads, if we have more "add publisher" commands than that, 
			   they will have to wait in the job overflow queue.
			*/
			if(num_publishers < (NUM_PROXIES / 2))
			{
				strcpy(publisher_thread_pool[num_publishers].filename, job_filename);
				// create publisher thread, passed in publisher command file
				pthread_create(&publisher_thread_pool[num_publishers].thread_id, NULL, publisher, &publisher_thread_pool[num_publishers]);
				num_publishers++;
			}
			else
			{
				int success;
				success = enqueue_job_overflow(&publisher_job_overflow, job_filename);
				/* double the allocation (necessary since we're not starting any threads until the command file has been processed, so none
				   of the jobs can be popped until the threads start) and try again.
				*/
				while(!success)
				{
					int old_allocated_publisher_overflow_size = allocated_publisher_overflow_size;
					allocated_publisher_overflow_size = allocated_publisher_overflow_size * 2;
					publisher_job_overflow.jobs = (char**) realloc(&publisher_job_overflow.jobs, (sizeof(char**) *allocated_publisher_overflow_size));
					if(publisher_job_overflow.jobs == NULL)
					{
						printf("Reallocation for publisher overflow failed.\n");
						free_registry(num_topics);
						exit(EXIT_FAILURE);
					}
					// allocate new memory block
					for(int i = old_allocated_publisher_overflow_size; i < allocated_publisher_overflow_size; i++)
					{
						publisher_job_overflow.jobs[i] = (char*) malloc(sizeof(char) * MAX_NAME);
					}
					publisher_job_overflow.max_jobs = allocated_publisher_overflow_size;
					success = enqueue_job_overflow(&publisher_job_overflow, job_filename);
				}
			}
		}
		else if(strcmp(command.command_list[0], "add") == 0 && strcmp(command.command_list[1], "subscriber") == 0)
		{
			char* job_filename = command_and_args.command_list[1];
			/* we can only create NUM_PROXIES / 2 threads, if we have more "add subscriber" commands than that, 
			   they will have to wait in the job overflow queue.
			*/
			if(num_subscribers < (NUM_PROXIES / 2))
			{
				strcpy(subscriber_thread_pool[num_subscribers].filename, job_filename);
				// create subscriber thread, passed in subscriber command file
				pthread_create(&subscriber_thread_pool[num_subscribers].thread_id, NULL, subscriber, &subscriber_thread_pool[num_subscribers]);
				num_subscribers++;
			}
			else
			{
				int success;
				success = enqueue_job_overflow(&subscriber_job_overflow, job_filename);
				/* double the allocation (necessary since we're not starting any threads until the command file has been processed, so none
				   of the jobs can be popped until the threads start) and try again.
				*/
				while(!success)
				{
					int old_allocated_subscriber_overflow_size = allocated_subscriber_overflow_size;
					allocated_subscriber_overflow_size = allocated_subscriber_overflow_size * 2;
					subscriber_job_overflow.jobs = (char**) realloc(&subscriber_job_overflow.jobs, (sizeof(char**) *allocated_subscriber_overflow_size));
					if(subscriber_job_overflow.jobs == NULL)
					{
						printf("Reallocation for subscriber overflow failed.\n");
						free_registry(num_topics);
						exit(EXIT_FAILURE);
					}
					// allocate new memory block
					for(int i = old_allocated_subscriber_overflow_size; i < allocated_subscriber_overflow_size; i++)
					{
						subscriber_job_overflow.jobs[i] = (char*) malloc(sizeof(char) * MAX_NAME);
					}
					subscriber_job_overflow.max_jobs = allocated_subscriber_overflow_size;
					success = enqueue_job_overflow(&subscriber_job_overflow, job_filename);
				}
			}
		}
		else if(strcmp(command.command_list[0], "delta") == 0)
		{
			delta = atoi(command.command_list[1]);
		}


		free_command_line(&command_and_args);
		free_command_line(&command);
	}

	// used by cleanup thread to gauge when all threads have finished
	total_publisher_and_subscriber_count = num_publishers + num_subscribers;

	// start all threads together
	printf("Waiting for 5 seconds to ensure all threads are created.\n");
	sleep(5);
	printf("Sending signal (via pthread_cond_broadcast()) to all threads\n");
	pthread_mutex_lock(&main_lock);
	pthread_cond_broadcast(&main_cond);
	pthread_mutex_unlock(&main_lock);

	// join the thread-pools
	for(int i = 0; i < num_publishers; i++)
	{
		pthread_join(publisher_thread_pool[i].thread_id, NULL);
	}
	for(int i = 0; i < num_subscribers; i++)
	{
		pthread_join(subscriber_thread_pool[i].thread_id, NULL);
	}
	pthread_join(cleanup_thread.thread_id, NULL);

	/* free dynamically allocated memory */
	free(command_string);
	free_registry(num_topics);
	free(publisher_thread_pool);
	free(subscriber_thread_pool);
	free_job_overflow(&publisher_job_overflow, allocated_publisher_overflow_size);
	free_job_overflow(&subscriber_job_overflow, allocated_subscriber_overflow_size);

	/* close input file */
	fclose(fp_in);

    return 0;
}