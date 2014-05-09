#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#define BufferSize 10

void *Producer();
void *Consumer();

int BufferIndex = 0;
int counter = 100;
char *BUFFER;

pthread_cond_t Buffer_Not_Full = PTHREAD_COND_INITIALIZER;
pthread_cond_t Buffer_Not_Empty = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mVar = PTHREAD_MUTEX_INITIALIZER;

int main()
{
	pthread_t ptid, ctid;

	BUFFER = (char *) malloc(sizeof(char) * BufferSize);

	pthread_create(&ptid, NULL, Producer, NULL);
	pthread_create(&ctid, NULL, Consumer, NULL);

	pthread_join(ptid, NULL);
	pthread_join(ctid, NULL);

	return 0;
}


void *Producer()
{
	for(int i = 1; i <= counter; i++){
		pthread_mutex_lock(&mVar);
		if(BufferIndex == BufferSize){
			pthread_cond_wait(&Buffer_Not_Full, &mVar);
		}

		//Checking if buffer is empty to allow things to be put into buffer
	
		BUFFER[BufferIndex++] = '@';
		printf("Produce : %d, i = %d\n", BufferIndex, i);
		
		pthread_mutex_unlock(&mVar);
		pthread_cond_signal(&Buffer_Not_Empty);

	}

	return NULL;
}

void *Consumer()
{
	for(int j = 1; j <= counter; j++){
		pthread_mutex_lock(&mVar);
		if(BufferIndex == -1){
			pthread_cond_wait(&Buffer_Not_Empty, &mVar);
		}

		printf("Consume : %d, j = %d \n", BufferIndex--, j);
		pthread_mutex_unlock(&mVar);
		pthread_cond_signal(&Buffer_Not_Full);
	}

	return NULL;
}
