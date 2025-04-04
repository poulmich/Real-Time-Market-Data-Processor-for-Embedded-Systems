#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <libwebsockets.h>

// Define colors for printing
#define KGRN "\033[0;32;32m"
#define KCYN "\033[0;36m"
#define KRED "\033[0;32;31m"
#define KYEL "\033[1;33m"
#define KBLU "\033[0;32;34m"
#define KCYN_L "\033[1;36m"
#define KBRN "\033[0;33m"
#define RESET "\033[0m"

// Variable that is =1 if the client should keep running, and =0 to close the client
static volatile int keepRunning = 1;

// Variable that is =1 if the client is connected, and =0 if not
static int connection_flag = 0;

// Variable that is =0 if the client should send messages to the server, and =1 otherwise
static int writeable_flag = 0;

//Macros
#define TASKQUEUESIZE 50
#define DATAQUEUESIZE 1024
#define SYMBOLSNUM 4
#define CONSUMERSNUM 2
#define PRODUCERSNUM 1

//struct to hold the incoming packet 
typedef struct{
  char price[50];
  char symbol[50];
  char timestamp[50];
  char volume[50];
} incoming_packet;

typedef struct{
    struct timespec read_time ;
    int index;
    char price[50];
    char symbol[50];
    char timestamp[50];
    char volume[50];
} storing_packet;

//structure for the tasks to store in the queue
typedef struct{
  void (*work)(void * args);
  void * args;
}workingFunction;

//structure that holds the incoming data
typedef struct{
	float price;
	float volume;
}data;

//queue that stores the tasks
typedef struct {
  workingFunction  buf [TASKQUEUESIZE];
  int len;
  long head, tail;
  int full, empty;
  pthread_cond_t notFull,notEmpty;
}TaskQueue;

//queue that stores the incoming data
typedef struct{
	data buf[DATAQUEUESIZE];
	int len;
	long head, tail;
    int full, empty;
}DataQueue;

//structure that stores the candlestick
typedef struct{
  float min_price;
  float max_price;
  float volume_sum;
  float initial_price;
  float final_price;
} candlestick;

//structure that stores the status moving average and the total volume of the last fifteen minuites 
typedef struct{
  float price_average;
  float price_sum;
  float volume_sum;
}average_price_volume_status;

//structure for the files
typedef struct{
    FILE *candlestick;
    FILE *average;
    FILE *transcactions;
}files;

typedef struct{
    int i;
    DataQueue dq;
}function_args;


//global arrays
char symb_arr[SYMBOLSNUM+1][50] = {"APPL", "AMZN", "BINANCE:BTCUSDT", "IC MARKETS:1","timedifferences"};
candlestick candle_arr[SYMBOLSNUM];
files files_arr[SYMBOLSNUM];
FILE * wtd, *dtd;
char dir_arr[SYMBOLSNUM][50] = {"apple", "amazon","bitcoin","icmarkets"};
DataQueue DataQueues[SYMBOLSNUM];
function_args calc_candle_args[SYMBOLSNUM];
function_args calc_av_args[SYMBOLSNUM];
candlestick cdle_status[SYMBOLSNUM];
data cc_dtemp[SYMBOLSNUM];
data ca_dtemp[SYMBOLSNUM];
average_price_volume_status av_pr_vol_stats[SYMBOLSNUM];
workingFunction candlestick_calculator[SYMBOLSNUM];
workingFunction average_calculator[SYMBOLSNUM];
workingFunction write_tr;
int *candle_prod_arg[SYMBOLSNUM];
int *av_prod_arg[SYMBOLSNUM ];
int produce_calc=0;
DataQueue lmpq [SYMBOLSNUM];
int index_arr[4] = {1,2,3,4};
data incoming_data;

// timing vars
unsigned long long write_timestamp_ns;
unsigned long long read_timestamp_ns;
long long data_diff_ns;
unsigned long long api_timestamp_ms;
unsigned long long api_timestamp_ns;
long long data_diff_api_ns;
long double data_diff_api_seconds;
long double data_diff_seconds;

//timing flags
int one_minuite_passed = 0;

//temp vars
struct timeval read_time, write_time;

//thread vars
pthread_mutex_t TaskQueueMutex;
pthread_mutex_t CalculationSignalMutex;
pthread_mutex_t WriteMutex;
pthread_mutex_t DataQueueMutex;

pthread_cond_t writeTranscaction;
pthread_cond_t calculate;
pthread_cond_t calculateAverage;

pthread_t pro[PRODUCERSNUM*2];
pthread_t con[CONSUMERSNUM];
pthread_t tim;
pthread_t wt;
//Task queue
TaskQueue tq;

//random vars
int write_transcaction_ = 0;
int produce_average_calc = 0;
incoming_packet transcaction;
int i = 0;

//file structure initiliazation
void files_init(files files_arr []){
    files_arr[0].transcactions = fopen("apple/transcactions.txt", "w");
    files_arr[1].transcactions = fopen("amazon/transcactions.txt", "w");
    files_arr[2].transcactions = fopen("bitcoin/transcactions.txt", "w");
    files_arr[3].transcactions = fopen("icmarkets/transcactions.txt","w");

    files_arr[0].average = fopen("apple/average.txt", "w");
    files_arr[1].average = fopen("amazon/average.txt", "w");
    files_arr[2].average = fopen("bitcoin/average.txt", "w");
    files_arr[3].average = fopen("icmarkets/average.txt","w");

    files_arr[0].candlestick = fopen("apple/candlestick.txt", "w");
    files_arr[1].candlestick = fopen("amazon/candlestick.txt", "w");
    files_arr[2].candlestick = fopen("bitcoin/candlestick.txt", "w");
    files_arr[3].candlestick = fopen("icmarkets/candlestick.txt","w");
    wtd = fopen("timedifferences/write_time_differences.txt","w");
    dtd = fopen("timedifferences/data_time_differences.txt","w");
}

//directories initialization

dir_init(const char *dir_name){
    if (mkdir(dir_name, 0777) == -1) {
        // Check if the error is that the directory already exists
        if (errno == EEXIST) {
            printf("Directory '%s' already exists.\n", dir_name);
        } else {
            // Print any other errors
            perror("Error creating directory");
        }
    } else {
        printf("Directory '%s' created successfully.\n", dir_name);
    }
}

//random vars
int j=0;
int t_index = 0;

//functions for data queue manipulations
void DataQueueInit(DataQueue *dq)
{
  int i=0;
  dq->len = 0;
  dq->empty = 1;
  dq->full = 0;
  dq->head = 0;
  dq->tail = 0;
}
void AddData(DataQueue* dq, data in){
    dq->buf[dq->tail].price = in.price;
    dq->buf[dq->tail].volume = in.volume;
    dq->tail++;
    dq->len++;
    if (dq->tail == DATAQUEUESIZE) dq->tail = 0;
    if (dq->tail == dq->head) dq->full = 1;
    dq->empty = 0;
}
void PopData(DataQueue *dq, data *out) {
    if(dq->len>0){
      out->price = dq->buf[dq->head].price;
      out->volume = dq->buf[dq->head].volume;

    dq->head++;
    dq->len--;
    if (dq->head == DATAQUEUESIZE) dq->head = 0;
    if (dq->head == dq->tail) dq->empty = 1;
    dq->full = 0;
    }
}
void EmptyDataQueue(DataQueue *dq,int prev_msg_len){
    int last_msg_len = dq->len;
    if(dq->tail>dq->head){
      dq->head += prev_msg_len;
    }
    else if(dq->head+prev_msg_len-DATAQUEUESIZE>0){
        dq->head=dq->head+prev_msg_len-DATAQUEUESIZE;
    }
    else if(dq->head+prev_msg_len-DATAQUEUESIZE<0){
        dq->head += prev_msg_len;
    }
    dq->len = last_msg_len-prev_msg_len;
}
void TaskQueueInit(TaskQueue *tq)
{
  int i=0;
  tq->len = 0;
  tq->empty = 1;
  tq->full = 0;
  tq->head = 0;
  tq->tail = 0;
  pthread_cond_init(&tq->notEmpty,NULL);
  pthread_cond_init(&tq->notFull,NULL);
}
void PopTask(TaskQueue *tq, workingFunction *out) {
    if(tq->len>0){
    out->work = tq->buf[tq->head].work;
    out->args = tq->buf[tq->head].args;
    tq->head++;
    tq->len--;
    if (tq->head == TASKQUEUESIZE) tq->head = 0;
    if (tq->head == tq->tail) tq->empty = 1;
    tq->full = 0;
    //pthread_cond_signal(tq->notEmpty);
   }
}
// functions for Task queue manipulation
void AddTask(TaskQueue *tq, workingFunction in) {
    tq->buf[tq->tail].work = in.work;
    tq->buf[tq->tail].args = in.args;
    tq->tail++;
    tq->len++;
    if (tq->tail == TASKQUEUESIZE) tq->tail = 0;
    if (tq->tail == tq->head) tq->full = 1;
    tq->empty = 0;
	pthread_cond_signal(&tq->notEmpty);
}
void submitTask(TaskQueue *tq,workingFunction wf){
    pthread_mutex_lock(&TaskQueueMutex);
    AddTask(tq,wf);
    pthread_mutex_unlock(&TaskQueueMutex);
}

//working functions

void calculate_candlestick(void * args){
    int i = 0;
	function_args cc_args  = *((function_args *) args);
    DataQueue dq  = cc_args.dq;
    int length = dq.len;
    int index = cc_args.i;

    if(length>0){

      PopData(&dq,&cc_dtemp[index]);
      cdle_status[index].min_price = cdle_status[index].max_price = cdle_status[index].initial_price = cdle_status[index].final_price = cc_dtemp[index].price; 
      cdle_status[index].volume_sum = cc_dtemp[index].volume;

    if(DataQueues[index].len>2){
	  for(i=0;i<length-2;i++){
		PopData(&dq,&cc_dtemp[index]);
		if(cc_dtemp[index].price<cdle_status[index].min_price){
		  cdle_status[index].min_price = cc_dtemp[index].price;		
		}
		if(cc_dtemp[index].price>cdle_status[index].max_price){
		  cdle_status[index].max_price = cc_dtemp[index].price;
		}
        cdle_status[index].volume_sum = cc_dtemp[index].volume;
	}

	PopData(&dq,&cc_dtemp[index]);
	cdle_status[index].final_price = cc_dtemp[index].price;
    cdle_status[index].volume_sum += cc_dtemp[index].volume;

   }
 else if(length==2){

    PopData(&dq,&cc_dtemp[index]);
    cdle_status[index].min_price = cc_dtemp[index].price;
    cdle_status[index].volume_sum += cc_dtemp[index].volume;

    if(cc_dtemp[index].price<cdle_status[index].min_price){
		  cdle_status[index].min_price = cc_dtemp[index].price;		
		}
      else if(cc_dtemp[index].price>cdle_status[index].max_price){
         cc_dtemp[index].price<cdle_status[index].min_price;
      }
     cdle_status[index].final_price = cc_dtemp[index].price;
     cdle_status[index].volume_sum += cc_dtemp[index].volume;
    }
    fprintf(files_arr[index].candlestick, "\nCandlestick for %s\nInitial price: %f\nFinal price: %f\nMin price: %f\nMax price: %f\nVolume: %f\n",symb_arr[index], cdle_status[index].initial_price, cdle_status[index].final_price, cdle_status[index].min_price, cdle_status[index].max_price, cdle_status[index].volume_sum);
    fflush(files_arr[index].candlestick);
    }
    else{
        fprintf(files_arr[index].candlestick, "\nCandlestick for %s\nInitial price: 0\nFinal price: 0\nMin price:0\nMax price:0\nVolume: 0\n",symb_arr[index]);
        fflush(files_arr[index].candlestick);
    }
    cdle_status[index].volume_sum =0;
}

void calculate_average(void *args){
	int i=0;
	float average;
	function_args  fun_args = *((function_args*) args);
    int index = fun_args.i;
    if(lmpq[index].len<15){
        PopData(&fun_args.dq,&ca_dtemp[index]);
        AddData(&lmpq[index],ca_dtemp[index]);
    }
    else{
	for(i=lmpq[index].head;i<lmpq[index].tail;i++){
	    av_pr_vol_stats[index].price_sum+=lmpq[index].buf[i].price;
        av_pr_vol_stats[index].volume_sum+=lmpq[index].buf[i].volume;
	}
    av_pr_vol_stats[index].price_average = (float)((float)(av_pr_vol_stats[index].price_sum)/(float)(lmpq[index].len));
    fprintf(files_arr[index].average, "\nMoving price averge and Total volume for %s\nMoving price average:%f\nTotal volume: %f\n",symb_arr[index], av_pr_vol_stats[index].price_average, av_pr_vol_stats[index].volume_sum);
    fflush(files_arr[index].average);
    av_pr_vol_stats[index].price_sum = 0;
    av_pr_vol_stats[index].volume_sum = 0;
    PopData(&lmpq[index],&ca_dtemp[index]);
  }
}
//producer consumer functions

void * TaskProducer(void *args){
    int i=0;
    int msg_len;
	while(keepRunning){
         pthread_mutex_lock(&CalculationSignalMutex);
		 while(!produce_calc){
	  	 pthread_cond_wait(&calculate,&CalculationSignalMutex);		
		}

		produce_calc = 0;

        for(i = 0;i<SYMBOLSNUM;i++){
            
		   candlestick_calculator[i].work = calculate_candlestick;
           memcpy(&calc_candle_args[i].dq , &DataQueues[i], sizeof(DataQueue));
           calc_candle_args[i].i = i;
           candlestick_calculator[i].args = &calc_candle_args[i];

           average_calculator[i].work = &calculate_average;
           memcpy(&calc_av_args[i].dq , &DataQueues[i], sizeof(DataQueue));
           calc_av_args[i].i = i;
           average_calculator[i].args = &calc_av_args[i];

           if(candlestick_calculator[i].work!=NULL){
		      submitTask(&tq,candlestick_calculator[i]);
           }
           if(average_calculator[i].work!=NULL){
		      submitTask(&tq,average_calculator[i]);
           }
              msg_len = DataQueues[i].len;
              EmptyDataQueue(&DataQueues[i], msg_len);
        } 
        pthread_mutex_unlock(&CalculationSignalMutex);
	 }
}
void *TaskConsumer(void *args){
    workingFunction wftemp;
	while(keepRunning){
    pthread_mutex_lock(&TaskQueueMutex);
	while(tq.empty){
		pthread_cond_wait(&tq.notEmpty,&TaskQueueMutex);
	}
    pthread_mutex_unlock(&TaskQueueMutex);
     if(tq.len>0){    
	  PopTask(&tq,&wftemp);
      // pthread_mutex_unlock(&CalculationSignalMutex);
      if(wftemp.work!=NULL&&wftemp.args!=NULL){
	     wftemp.work(wftemp.args);
      }		
	}
  }
}
long long millis() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}
//timer thread
void * timer(void *args) {
    while(keepRunning){
    long long start_time = millis();
    int i=0;
    while (1) {
        if (millis() - start_time >= 5999) {  //60 seconds
            one_minuite_passed = 1;
            pthread_cond_signal(&calculate);
            produce_calc = 1;
            //start_time = millis();  // Reset the start time
        }
        if (millis() - start_time >= 6000) {  //60 seconds
            //printf("%lld",millis() - start_time);
            start_time = millis();  // Reset the start time
        }
    }
    return NULL;
    }
}
void write_transcaction(void *args) {
    pthread_mutex_lock(&WriteMutex);
    if (keepRunning) {
        storing_packet sp = *((storing_packet *) args);

        // Write transaction details to the appropriate file
        fprintf(files_arr[sp.index].transcactions, "%s\n", sp.price);
        fflush(files_arr[sp.index].transcactions);
        fprintf(files_arr[sp.index].transcactions, "%s\n", sp.symbol);
        fflush(files_arr[sp.index].transcactions);
        fprintf(files_arr[sp.index].transcactions, "%s\n", sp.timestamp);
        fflush(files_arr[sp.index].transcactions);
        fprintf(files_arr[sp.index].transcactions, "%s\n", sp.volume);
        fflush(files_arr[sp.index].transcactions);

        // Get the current time for writing time (in nanoseconds)
        struct timespec write_time;
        clock_gettime(CLOCK_REALTIME, &write_time);

        // Calculate time difference (based on read_time stored earlier) in nanoseconds
        write_timestamp_ns = (unsigned long long)(write_time.tv_sec * 1000000000LL + write_time.tv_nsec);
        read_timestamp_ns = (unsigned long long)(sp.read_time.tv_sec * 1000000000LL + sp.read_time.tv_nsec);

         data_diff_ns = (long long)(write_timestamp_ns - read_timestamp_ns); // Time difference in nanoseconds

        // Convert to seconds and nanoseconds for higher precision
        data_diff_seconds = data_diff_ns / 1e9; // Convert to seconds with decimals

        // Print the time difference with high precision
        fprintf(wtd, "%.9Lf\n", data_diff_seconds); // Print to 9 decimal places (nanoseconds precision)
        fflush(wtd);

        // Convert API timestamp (in milliseconds) to nanoseconds
        api_timestamp_ms = strtoll(sp.timestamp, NULL, 10);
        api_timestamp_ns = api_timestamp_ms * 1000000ULL; // Convert milliseconds to nanoseconds

        data_diff_api_ns = (long long)(read_timestamp_ns - api_timestamp_ns); // Difference with API timestamp
        data_diff_api_seconds = data_diff_api_ns / 1e9; // Convert to seconds with decimals

        // Print the difference between read time and API timestamp
        fprintf(dtd, "%.9Lf\n", data_diff_api_seconds); // Print with nanoseconds precision
        fflush(dtd);
    }
    pthread_mutex_unlock(&WriteMutex);
}

/*
void * write_transcaction(void * args){
        while(keepRunning){
                while(!write_transcaction_){
                        pthread_cond_wait(&writeTranscaction,&WriteSignalMutex);
                }
         write_transcaction_ = 0;
         fprintf(files_arr[t_index].transcactions ,"%s\n" , transcaction.price);
         fflush(files_arr[t_index].transcactions);
         fprintf(files_arr[t_index].transcactions,"%s\n" , transcaction.symbol);
         fflush(files_arr[t_index].transcactions);
         fprintf(files_arr[t_index].transcactions,"%s\n" , transcaction.timestamp);
         fflush(files_arr[t_index].transcactions);
         fprintf(files_arr[t_index].transcactions,"%s\n" , transcaction.volume);
         fflush(files_arr[t_index].transcactions);
         clock_gettime(CLOCK_MONOTONIC, &write_time);
         fprintf(wtd,"%ld.%09ld \n", write_time.tv_sec, write_time.tv_usec);
         fflush(wtd);
        }
}
*/

// Function to handle the change of the keepRunning boolean
void intHandler(int dummy)
{
    keepRunning = 0;
}

// The JSON paths/labels that we are interested in
static const char *const tok[] = {

    "data[].s",
    "data[].p",
    "data[].t",
    "data[].v",
};

// Callback function for the LEJP JSON Parser
static signed char cb(struct lejp_ctx *ctx, char reason) {
        if (reason & LEJP_FLAG_CB_IS_VALUE && (ctx->path_match > 0)){
        switch(i){
                case 0:{
                    strcpy(transcaction.price,ctx->buf);
                    incoming_data.price = strtof(transcaction.price,NULL);
                    i++;
                    break;
                }
                case 1:{
                        strcpy(transcaction.symbol,ctx->buf);
                        i++;
                        break;
                }
                case 2:{
                         strcpy(transcaction.timestamp,ctx->buf);
                    i++;
                    break;
                }
                case 3:{
                        strcpy(transcaction.volume,ctx->buf);
                        incoming_data.volume = strtof(transcaction.volume,NULL);
						i=0;
                        
                        for(j=0;j<SYMBOLSNUM;j++){
                                if(strcmp(&transcaction.symbol[0], symb_arr[j]) == 0){

                                    pthread_mutex_lock(&DataQueueMutex);
                                    AddData(&DataQueues[j],incoming_data);
                                    pthread_mutex_unlock(&DataQueueMutex);
                                    /*
                                    write_transcaction_=1;
                                    t_index = j;
                                	pthread_cond_signal(&writeTranscaction);
                                    pthread_mutex_lock(&DataQueueMutex);
                                    AddData(&DataQueues[j],incoming_data);
                                    pthread_mutex_unlock(&DataQueueMutex);
                                    api_timestamp = strtol(transcaction.timestamp, NULL, 10);
                                    api_sec = api_timestamp / 1000;      // Seconds part
                                    api_usec = (api_timestamp % 1000) * 1000; // Microseconds part
                                    
                                    // Calculate the difference between current time and API time
                                    clock_gettime(CLOCK_MONOTONIC, &read_time);
                                    data_sec_diff = read_time.tv_sec - api_sec;
                                    data_usec_diff = read_time.tv_usec - api_usec;
                                    if (data_usec_diff < 0) {
                                            data_sec_diff -= 1;
                                            data_usec_diff += 1000000;
                                    }
                                    fprintf(dtd,"%ld.%ld\n",data_sec_diff, data_usec_diff);
                                    fflush(dtd);
                                    break;
                                    */

                                   storing_packet * temp = malloc(sizeof(storing_packet));
                                   write_tr.work = write_transcaction;
                                   clock_gettime(CLOCK_REALTIME, &read_time);
                                   memcpy(&temp->read_time, &read_time,sizeof(struct timeval));
                                   strcpy(temp->price,transcaction.price);
                                   strcpy(temp->symbol,transcaction.symbol);
                                   strcpy(temp->timestamp,transcaction.timestamp);
                                   strcpy(temp->volume,transcaction.volume); 
                                   temp->index = j;
                                   write_tr.args = temp;  
                                   AddTask(&tq,write_tr);
								}
							}   
                      break;
                   }
            }
    }

        /* working code
    if (reason & LEJP_FLAG_CB_IS_VALUE && (ctx->path_match > 0)) {

        while (q->full) {
            pthread_cond_wait(q->notFull, q->mut);
        }
        if(q->len>=4){
          pthread_cond_signal(q->getData);
          pthread_mutex_unlock(q->mut);
       }
        queueAdd(q, ctx->buf);
        pthread_cond_signal(q->notEmpty);


         previous code
        if (reason == LEJPCB_COMPLETE) {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        unsigned long long millisecondsSinceEpoch =
            (unsigned long long)(tv.tv_sec) * 1000 +
            (unsigned long long)(tv.tv_usec) / 1000;
        fprintf(out_fp, "%llu\n\n", millisecondsSinceEpoch);
    }
  }*/
    return 0;
}


// Function used to "write" to the socket, so to send messages to the server
// @args:
// ws_in        -> the websocket struct
// str          -> the message to write/send
// str_size_in  -> the length of the message
static int websocket_write_back(struct lws *wsi_in, char *str, int str_size_in)
{
    if (str == NULL || wsi_in == NULL)
        return -1;
    int m;
    int n;
    int len;
    char *out = NULL;

    if (str_size_in < 1)
        len = strlen(str);
    else
        len = str_size_in;

    out = (char *)malloc(sizeof(char) * (LWS_SEND_BUFFER_PRE_PADDING + len + LWS_SEND_BUFFER_POST_PADDING));
    //* setup the buffer*/
    memcpy(out + LWS_SEND_BUFFER_PRE_PADDING, str, len);
    //* write out*/
    n = lws_write(wsi_in, out + LWS_SEND_BUFFER_PRE_PADDING, len, LWS_WRITE_TEXT);

    printf(KBLU "[websocket_write_back] %s\n" RESET, str);
    //* free the buffer*/
    free(out);
    return n;
}

// The websocket callback function
static int ws_service_callback(
    struct lws *wsi,
    enum lws_callback_reasons reason, void *user,
    void *in, size_t len){

    // Switch-Case structure to check the reason for the callback
    switch (reason){

    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        printf(KYEL "[Main Service] Connect with server success.\n" RESET);

        // Call the on writable callback, to send the subscribe messages to the server
        lws_callback_on_writable(wsi);
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        printf(KRED "[Main Service] Connect with server error: %s.\n" RESET, (char *)in);
        // Set the flag to 0, to show that the connection was lost
        connection_flag = 0;
        break;

    case LWS_CALLBACK_CLOSED:
        printf(KYEL "[Main Service] LWS_CALLBACK_CLOSED\n" RESET);
        // Set the flag to 0, to show that the connection was lost
        connection_flag = 0;
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:;
        // Incoming messages are handled here

        // UNCOMMENT for printing the message on the terminal
        // printf(KCYN_L"[Main Service] Client received:%s\n"RESET, (char *)in);

        // Print that messages are being received
        printf(KCYN_L "\r[Main Service] Client receiving messages" RESET);
        fflush(stdout);

        // Initialize a LEJP JSON parser, and pass it the incoming message
        char *msg = (char *)in;

        struct lejp_ctx ctx;
        lejp_construct(&ctx, cb, NULL, tok, LWS_ARRAY_SIZE(tok));
        int m = lejp_parse(&ctx, (uint8_t *)msg, strlen(msg));
        if (m < 0 && m != LEJP_CONTINUE)
        {
            lwsl_err("parse failed %d\n", m);
        }

        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:

        // When writeable, send the server the desired trade symbols to subscribe to, if not already subscribed
        printf(KYEL "\n[Main Service] On writeable is called.\n" RESET);

        if (!writeable_flag)
        {
            char symb_arr[4][50] = {"APPL\0", "AMZN\0", "BINANCE:BTCUSDT\0", "IC MARKETS:1\0"};
            char str[100];
            for (int i = 0; i < 4; i++)
            {
                sprintf(str, "{\"type\":\"subscribe\",\"symbol\":\"%s\"}", symb_arr[i]);
                int len = strlen(str);
                websocket_write_back(wsi, str, len);
            }

            // Set the flag to 1, to show that the subscribe request have been sent
            writeable_flag = 1;
        }
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:

        // If the client is closed for some reason, set the connection and writeable flags to 0,
        // so a connection can be re-established
        printf(KYEL "\n[Main Service] Client closed.\n" RESET);
        connection_flag = 0;
        writeable_flag = 0;

        break;
    default:
        break;
    }
    return 0;
}

// Protocol to be used with the websocket callback
static struct lws_protocols protocols[] =
    {
        {
            "trade_protocol",
            ws_service_callback,
        },
        {NULL, NULL, 0, 0} /* terminator */
};

// Main function
int main(void)
{
	int i=0;
    // Set intHandle to handle the SIGINT signal
    // (Used for terminating the client)
    signal(SIGINT, intHandler);

    // Open the output file
     for(i=0;i<SYMBOLSNUM+1;i++){
    	DataQueueInit(&DataQueues[i]);
        DataQueueInit(&lmpq[i]);
        dir_init(dir_arr[i]);
	}
    files_init(files_arr);
    TaskQueueInit(&tq);

    pthread_mutex_init(&TaskQueueMutex,NULL);
    pthread_mutex_init(&CalculationSignalMutex,NULL);
    pthread_mutex_init(&WriteMutex,NULL);
    pthread_cond_init(&writeTranscaction,NULL);
    pthread_cond_init(&calculate,NULL);
    pthread_cond_init(&calculateAverage,NULL);
    
     if (pthread_create(&tim, NULL, &timer, NULL)) {
            perror("Failed to create the thread");
     }
/*
     if (pthread_create(&wt, NULL, &write_transcaction, NULL)){
            perror("Failed to create the thread");
     }*/
    
	if (pthread_create(&pro[i], NULL, &TaskProducer, NULL) != 0){
            perror("Failed to create the thread");
         }

	for(i=0;i<CONSUMERSNUM;i++){
		 if (pthread_create(&con[i], NULL, &TaskConsumer, NULL) != 0) {
            perror("Failed to create the thread");
        }
	}
    // Set the LWS and its context
    struct lws_context *context = NULL;
    struct lws_context_creation_info info;
    struct lws *wsi = NULL;
    struct lws_protocols protocol;

    memset(&info, 0, sizeof info);


    // Set the context of the websocket
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    // Set the Finnhub url
    char *api_key = "cppdju9r01qi7uaiggh0cppdju9r01qi7uaigghg"; // PLACE YOUR API KEY HERE!
    if (strlen(api_key) == 0)
    {
        printf(" API KEY NOT PROVIDED!\n");
        return -1;
    }

    // Create the websocket context
    context = lws_create_context(&info);
    printf(KGRN "[Main] context created.\n" RESET);

    if (context == NULL)
    {
        printf(KRED "[Main] context is NULL.\n" RESET);
        return -1;
    }

    // Set up variables for the url
    char inputURL[300];

    sprintf(inputURL, "wss://ws.finnhub.io/?token=%s", api_key);
    const char *urlProtocol, *urlTempPath;
    char urlPath[300];

    struct lws_client_connect_info clientConnectionInfo;
    memset(&clientConnectionInfo, 0, sizeof(clientConnectionInfo));

    // Set the context for the client connection
    clientConnectionInfo.context = context;

    // Parse the url
    if (lws_parse_uri(inputURL, &urlProtocol, &clientConnectionInfo.address,
                      &clientConnectionInfo.port, &urlTempPath))
    {
        printf("Couldn't parse URL\n");
    }

    urlPath[0] = '/';
    strncpy(urlPath + 1, urlTempPath, sizeof(urlPath) - 2);
    urlPath[sizeof(urlPath) - 1] = '\0';

    // While a kill signal is not sent (ctrl+c), keep running
    while (keepRunning){
        // If the websocket is not connected, connect
        if (!connection_flag || !wsi)
        {
            // Set the client information

            connection_flag = 1;
            clientConnectionInfo.port = 443;
            clientConnectionInfo.path = urlPath;
            clientConnectionInfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;

            clientConnectionInfo.host = clientConnectionInfo.address;
            clientConnectionInfo.origin = clientConnectionInfo.address;
            clientConnectionInfo.ietf_version_or_minus_one = -1;
            clientConnectionInfo.protocol = protocols[0].name;

            printf(KGRN "Connecting to %s://%s:%d%s \n\n" RESET, urlProtocol,
                   clientConnectionInfo.address, clientConnectionInfo.port, urlPath);

            wsi = lws_client_connect_via_info(&clientConnectionInfo);
            if (wsi == NULL)
            {
                printf(KRED "[Main] wsi create error.\n" RESET);
                return -1;
            }

            printf(KGRN "[Main] wsi creation success.\n" RESET);
        }

        // Service websocket activity
        lws_service(context, 0);
    }

    printf(KRED "\n[Main] Closing client\n" RESET);
    lws_context_destroy(context);
    
    for(i=0;i<SYMBOLSNUM;i++){
      fclose(files_arr[i].transcactions);
      fclose(files_arr[i].average);
      fclose(files_arr[i].candlestick);
    }
} 
