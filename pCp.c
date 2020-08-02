#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <dirent.h>
#include <semaphore.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <sys/time.h>
#include <errno.h>
#include <signal.h>

#define MAX_PATH_LENGTH 255

long totalBytesCoppied=0;
sem_t printMutex; //1
sem_t mutex; //1
sem_t mutexc; //1
sem_t full; //0
sem_t empty; //bufferSize

int * bufferSourceFdQueue;
int * bufferDestFdQueue;
char ** bufferFileName;
int queueHead=0;
int queueTail=0;
int bufferSize;
char * strcon (char * str1,char * str2,char * str3){
    //Allocate memory in MAX_PATH_LENGTH size to hold result string
    char *pathBuffer = (char*) malloc(MAX_PATH_LENGTH);
    //concatenate three strings using snprintf
    snprintf(pathBuffer, MAX_PATH_LENGTH, "%s%s%s", str1, str2, str3);
    return pathBuffer;
}




int isDone=0;
void * consumerThread(void * vargp){
    int totalItemsLeftInBuffer;

    while (1){
        sem_wait(&mutexc);
        sem_getvalue(&full,&totalItemsLeftInBuffer);
        //If done flag is set and no item is left in the buffer, Terminate
        if(isDone==1 && totalItemsLeftInBuffer==0){
            sem_post(&mutexc);
            sem_post(&mutex);
            return NULL;
        }//If done flag is set and no item is left in the buffer, Terminate

        sem_getvalue(&full,&totalItemsLeftInBuffer);
        sem_wait(&full);
        if(isDone==1 && totalItemsLeftInBuffer==0){
            sem_post(&mutexc);
            return NULL;
        }

        sem_wait(&mutex);
        //this is critical section for reading items from buffer
        int copyDestFd = bufferDestFdQueue[queueTail];
        int copySourceFd = bufferSourceFdQueue[queueTail];
        char * fileName = bufferFileName[queueTail];
        bufferFileName[queueTail]=NULL;
        queueTail = (queueTail+1)%bufferSize;
        sem_post(&mutex);

        sem_post(&empty);
        sem_post(&mutexc);


        //Copying will be done in parallel
        ssize_t n,totalSize=0;
        char buffer[255];
        while ((n = read(copySourceFd,buffer, 255)) > 0){
            totalSize+=n;
            write(copyDestFd,buffer,n);
        }
        totalBytesCoppied += totalSize;
        sem_wait(&printMutex);
        printf("File %s copied with total size of %d bytes. \n",fileName,totalSize);
        free(fileName);
        sem_post(&printMutex);
        close(copyDestFd);
        close(copySourceFd);

    }
}

void produceFiles(char * sourceFolder,char * destFolder){
    DIR * dirr = opendir(sourceFolder);
    if(dirr==NULL) {
        printf("Could not open directory : '%s'\n",sourceFolder);
        exit(2);
    }

    struct dirent *ent;

    struct stat st = {0};
    if (stat(destFolder, &st) == -1) {
        mkdir(destFolder, 0700);
    }
    while ((ent=readdir(dirr)) != NULL){

        if(ent->d_name[0] == '.' && (ent->d_name[1] == '.' || ent->d_name[1] == 0))
            continue;
        char * pathNextSource = strcon(sourceFolder,"/",ent->d_name);
        char * pathNextDest = strcon(destFolder,"/",ent->d_name);

        if(ent->d_type==DT_REG || ent->d_type==DT_FIFO  ){

            mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH;
            int fdSource = open(pathNextSource,O_RDONLY,mode);
            if(fdSource==-1){
                printf("Error openning file (source) %s\n",pathNextSource);
                if(errno==EMFILE){
                    printf("The per-process limit on the number of open file descriptors has been reached\n");
                }
                continue;
            }
            int fdDest = open(pathNextDest,O_RDWR | O_CREAT | O_TRUNC,mode);

            if(fdDest==-1){
                printf("Error openning file (dest) %s\n",pathNextDest);
                if(errno==EMFILE){
                    printf("The per-process limit on the number of open file descriptors has been reached\n");
                }
                close(fdSource);
                continue;
            }
            sem_wait(&empty);
            sem_wait(&mutex);

            bufferFileName[queueHead] = malloc(MAX_PATH_LENGTH* sizeof(char));
            strcpy(bufferFileName[queueHead],ent->d_name);
            bufferSourceFdQueue[queueHead] = fdSource;
            bufferDestFdQueue[queueHead] = fdDest;
            queueHead = (queueHead+1)%bufferSize;

            sem_post(&mutex);
            sem_post(&full);

        }else if(ent->d_type==DT_DIR){
            produceFiles(pathNextSource,pathNextDest);
        }else if( ent->d_type==DT_LNK){

        }
        free(pathNextDest);
        free(pathNextSource);
    }

    closedir(dirr);
}


void * producerThread(void * vargp){
    char ** fileNames = (char ** )vargp;
    char * sourceFolder = fileNames[0];
    char * destFolder = fileNames[1];

    produceFiles(sourceFolder,destFolder);

    isDone=1; //Producer thread is done, setting the done flag
    int a;
    sem_getvalue(&full,&a);
    if(a==0){//If the consumers finished before producer finishes
        //all the waiting consumers should wake up and terminate
        sem_post(&mutex);
        sem_post(&mutexc);
        sem_post(&full);
    }
    return NULL;
}

int numberOfConsumers;
pthread_t * consumerThreads;
void sig_handler(int signo){
    printf("\n\nExiting pCp...\n");
    int i;
    for(i=0;i<bufferSize;i++){
        free(bufferFileName[i]);
    }
    sem_destroy(&mutex);
    sem_destroy(&mutexc);
    sem_destroy(&printMutex);
    sem_destroy(&full);
    sem_destroy(&empty);
    free(bufferSourceFdQueue);
    free(bufferDestFdQueue);
    free(bufferFileName);
    free(consumerThreads);

    exit(0);
}


int main(int argc,char ** argv){
    if(argc!=5){
        printf("Usage example: pCp [NumberOfConsumers] [BufferSize] [SourcePath] [DestPath]");
        return 0;
    }


    if (signal(SIGINT, sig_handler) == SIG_ERR)
        printf("\nCan't catch SIGINT\n");

    if (signal(SIGTERM, sig_handler) == SIG_ERR)
        printf("\nCan't catch SIGTERM\n");

    if (signal(SIGQUIT, sig_handler) == SIG_ERR)
        printf("\nCan't catch SIGQUIT\n");

    struct timeval t0;
    gettimeofday(&t0, NULL);

    numberOfConsumers = atoi(argv[1]);
    bufferSize = atoi(argv[2]);
    bufferSourceFdQueue = malloc(bufferSize* sizeof(int));
    bufferDestFdQueue = malloc(bufferSize* sizeof(int));
    bufferFileName = malloc(bufferSize* sizeof(char*));
    int i;
    for(i=0;i<bufferSize;i++){
        bufferFileName[i]=NULL;
    }
    char * fileName[2];
    fileName[0] = argv[3];
    fileName[1] = argv[4];
    sem_init(&printMutex, 0, 1);
    sem_init(&mutex, 0, 1);
    sem_init(&mutexc, 0, 1);



    sem_init(&full, 0, 0);
    sem_init(&empty, 0, bufferSize);

    pthread_t thread_id;
    pthread_create(&thread_id, NULL, producerThread, fileName);


    consumerThreads = malloc(numberOfConsumers* sizeof(pthread_t));

    for(i=0;i<numberOfConsumers;i++){
        pthread_create(&(consumerThreads[i]), NULL, consumerThread, NULL);
    }


    pthread_join(thread_id, NULL);
    for(i=0;i<numberOfConsumers;i++){
        pthread_join(consumerThreads[i],NULL);
    }

    struct timeval t1;
    gettimeofday(&t1, NULL);

    long elapsed = (t1.tv_sec-t0.tv_sec)*1000 + (t1.tv_usec-t0.tv_usec)/1000;
    printf("Total time %ld miliseconds\n",elapsed);
    printf("Total bytes coppied %ld (%ld KB)\n",totalBytesCoppied,(totalBytesCoppied/1000));


    sem_destroy(&mutex);
    sem_destroy(&mutexc);
    sem_destroy(&printMutex);
    sem_destroy(&full);
    sem_destroy(&empty);
    free(bufferSourceFdQueue);
    free(bufferDestFdQueue);
    free(bufferFileName);
    free(consumerThreads);
    exit(0);
}
