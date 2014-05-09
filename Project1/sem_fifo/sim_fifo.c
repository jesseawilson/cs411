//sim_fifo.c
//Group 17

#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc.h>

struct data{
	int threadID;
	pthread_cond_t cond;
};

#define NUMTHREADS 4

pthread_t threads[NUMTHREADS];
unsigned long queue_pos = 0;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
sem_t sem;

sem_t queue[NUMTHREADS + 1];

int q_size = 0;
int q_front = 0;

void _queue_push(sem_t *sem)
{
	//queue[q_size] = sem;
	q_size++;
}

void _queue_pop()
{
	//queue[q_front] = NULL;
	q_front++;
}

void smf_wait() 
{
	sem_t *sp;

	pthread_mutex_lock(&mutex);

	if(sem_trywait(&sem) == 0) 
		pthread_mutex_unlock(&mutex);
	else {
		sp = (sem_t *)malloc(sizeof(sem_t));
		sem_init(sp, 0, 0);
		_queue_push(sp);
		
		pthread_mutex_unlock(&mutex);

		sem_wait(sp);
		sem_destroy(sp);
		free(sp);
	}
}

void smf_sig()
{
	sem_t *sp;
	pthread_mutex_lock(&mutex);
	if(q_size == 0) 
		sem_post(&sem);		
	else {
		//sp = q_front;
		_queue_pop();
		sem_post(sp);
	}
	
	pthread_mutex_unlock(&mutex);
}

void * thread_print(void *arg)
{
	struct data *d = (struct data*)arg;
	printf("Starting thread %d\n", d->threadID);

	smf_wait();
	printf("Thread ID: %d \n", d->threadID);
	
	return NULL;
}	

int main( int argc, const char* argv[] )
{
	struct data d[NUMTHREADS];
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

	sem_init(&sem, 0, NUMTHREADS);
	pthread_mutex_init(&mutex, NULL);

	for(int i = 0; i < NUMTHREADS; ++i) {
		d[i].threadID = i;

		if(pthread_create(&threads[i], &attr, thread_print, (void*)&d[i]) != 0) {
			perror("Cannot create thread");
			exit(EXIT_FAILURE);
		}
	}
	
	for(int i = 0; i < NUMTHREADS; ++i)
		pthread_join(threads[i], NULL);

	pthread_attr_destroy(&attr);
	pthread_mutex_destroy(&mutex);
	sem_destroy(&sem);

	return 0;
}


