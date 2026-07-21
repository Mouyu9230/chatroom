#include <iostream>
#include <queue>
#include <thread>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <fstream>
#include <atomic>

using namespace std;


mutex cout_mtx;


typedef struct thread_pool{
    vector<thread> threads;
    queue<int> task;

    condition_variable cv;
    mutex mtx;

    bool stop_me;
    atomic<int> finished=0;

}thread_pool;

void shutdown(thread_pool* pool);
long long int cal(int x);
void worker(thread_pool* pool);
void submit_task(thread_pool* pool,int t);
void get_task(thread_pool* pool,atomic<int>& submitted);
void pool_init(thread_pool* pool);


int main(){

    atomic<int> submit_num;

    thread_pool pool;
    pool_init(&pool);

    get_task(&pool,submit_num);
    while(submit_num!=pool.finished);
    shutdown(&pool);

}


void pool_init(thread_pool* pool){

    pool->stop_me=false;
    

    for(int i=0;i<10;i++){
        pool->threads.emplace_back(worker,pool);
    }
}

void get_task(thread_pool* pool,atomic<int>& submitted){

    ifstream ifs("test_data.txt");
    if(!ifs.is_open()){
        cout<<"file open failed\n";
        return; 
    }

    int task;
    while(ifs>>task){
        submitted++;
        submit_task(pool,task);

    }

     
}

void submit_task(thread_pool* pool,int t){

    unique_lock<mutex> lock(pool->mtx);
    pool->task.push(t);
    pool->cv.notify_one();

}

void worker(thread_pool* pool){
    while (true){
        int n;

        {
            unique_lock<mutex> lock(pool->mtx);

            pool->cv.wait(lock,[pool](){
                return pool->stop_me||!pool->task.empty();
            });

            if (pool->stop_me&&pool->task.empty())
                return;

            n=pool->task.front();
            pool->task.pop();
            pool->finished++;
            
        }

        lock_guard<mutex> lock(cout_mtx);
        cout<<"thread "<<this_thread::get_id()<<" done task :";


        cout<<n<<"!="<<cal(n)<<endl;

        
    }

    
}

long long int cal(int x){
    long long int ans=1;
    for(int i=1;i<=x;i++){
        ans*=i;
    }
    return ans;
}

void shutdown(thread_pool* pool) {
    
        unique_lock<mutex> lock(pool->mtx);
        pool->stop_me=true;
    

    pool->cv.notify_all();

    for(auto& t:pool->threads)
        t.join();
}

